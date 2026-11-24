#!/usr/bin/env python3
"""HC-01..10: active probes, hysteresis, crash/restart, and shutdown on owned ports."""

import argparse
import concurrent.futures
import http.server
import json
from pathlib import Path
import sys
import threading
import time

from proxy_process import OwnedProcess
from test_backend_process import check, fetch


def events(process):
    return [json.loads(line) for line in process.path.read_text().splitlines(keepends=True)
            if line.endswith("\n")]


def wait_for(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError(message)


class ProbeHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/ready":
            with self.server.lock:
                self.server.calls += 1
                sequence = self.server.calls
            if self.server.hang:
                self.server.release.wait(timeout=10)
            status = 503 if self.server.alternate and sequence % 2 == 0 else 200
        else:
            status = 200
        body = b"fixture"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True)
    parser.add_argument("--proxy", required=True)
    parser.add_argument("--logs", required=True)
    args = parser.parse_args()
    backend = str(Path(args.backend).resolve())
    proxy = str(Path(args.proxy).resolve())
    logs = Path(args.logs)
    logs.mkdir(parents=True, exist_ok=True)
    owned = []
    stopped = set()
    fixtures = []
    checks = []
    traffic_stop = threading.Event()
    traffic = []
    traffic_errors = []
    traffic_thread = None

    def start(executable, flags, name):
        process = OwnedProcess(executable, flags, name, logs)
        owned.append(process)
        return process, process.ready()

    def start_backend(name, port=0, suffix=""):
        return start(backend, ["--id", name, "--port", str(port), "--test-mode"], name + suffix)

    def start_proxy(name, endpoints, extra=()):
        flags = ["--metrics-port", "0", "--port", "0", "--upstream-timeout-ms", "400", "--health-interval-ms", "40",
                 "--health-connect-timeout-ms", "80", "--health-response-timeout-ms", "100"]
        for identifier, port in endpoints:
            flags += ["--backend", f"{identifier},127.0.0.1,{port}"]
        return start(proxy, [*flags, *extra], name)

    def transition(process, identifier, state, since):
        return next((event for event in events(process)[since:]
                     if event.get("event") == "health_transition" and event.get("backend") == identifier
                     and event.get("to") == state), None)

    def fixture(alternate=False, hang=False):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ProbeHandler)
        server.daemon_threads = True
        server.alternate, server.hang = alternate, hang
        server.calls = 0
        server.lock = threading.Lock()
        server.release = threading.Event()
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.05), daemon=True)
        thread.start()
        fixtures.append((server, thread))
        return server

    try:
        a, a_port = start_backend("A")
        b, b_port = start_backend("B")
        live = {"A": a, "B": b}
        ports = {"A": a_port, "B": b_port}
        balancer, port = start_proxy("health-proxy", list(ports.items()))
        check(balancer.event["eligible"] == 2, "HC-01: healthy backends not admitted")
        identities = [json.loads(fetch(port, "/request")[2])["server_id"] for _ in range(8)]
        check(identities == list("ABABABAB"), "HC-01: probes consumed scheduler positions")
        checks.append("HC-01")

        def send_traffic():
            while not traffic_stop.is_set():
                try:
                    status, _, _ = fetch(port, "/request")
                    traffic.append(status)
                    if status not in (200, 502, 503, 504):
                        traffic_errors.append(f"Unexpected status {status}")
                except Exception as error:
                    traffic_errors.append(str(error))
                traffic_stop.wait(0.01)

        traffic_thread = threading.Thread(target=send_traffic, daemon=True)
        traffic_thread.start()
        since = len(events(balancer))
        fetch(b_port, "/admin/ready?value=false", "POST")
        wait_for(lambda: transition(balancer, "B", "UNHEALTHY", since), "HC-03: no exclusion")
        failures = [event for event in events(balancer)[since:] if event.get("event") == "health_probe"
                    and event.get("backend") == "B" and event.get("status") == 503]
        check([event["failures"] for event in failures[:3]] == [1, 2, 3], "HC-03: wrong consecutive count")
        check([event["state"] for event in failures[:3]] == ["HEALTHY", "HEALTHY", "UNHEALTHY"],
              "HC-02: exclusion before threshold")
        check(all(json.loads(fetch(port, "/request")[2])["server_id"] == "A" for _ in range(8)),
              "Excluded B still routed from a fresh snapshot")
        checks += ["HC-02", "HC-03", "HC-05"]

        since = len(events(balancer))
        fetch(b_port, "/admin/ready?value=true", "POST")
        wait_for(lambda: transition(balancer, "B", "HEALTHY", since), "HC-04: B did not recover")
        successes = [event for event in events(balancer)[since:] if event.get("event") == "health_probe"
                     and event.get("backend") == "B" and event.get("reason") == "success"]
        check([event["successes"] for event in successes[:2]] == [1, 2], "HC-04: wrong recovery threshold")
        check([event["state"] for event in successes[:2]] == ["UNHEALTHY", "HEALTHY"], "Recovered too early")
        checks.append("HC-04")

        since = len(events(balancer))
        for backend_port in ports.values():
            fetch(backend_port, "/admin/ready?value=false", "POST")
        for identifier in ports:
            wait_for(lambda identifier=identifier: transition(balancer, identifier, "UNHEALTHY", since),
                     "HC-08: no all-backend outage")
        check(all(fetch(port, "/request")[0] == 503 for _ in range(5)), "HC-08: outage did not return 503")
        checks.append("HC-08")
        since = len(events(balancer))
        for backend_port in ports.values():
            fetch(backend_port, "/admin/ready?value=true", "POST")
        for identifier in ports:
            wait_for(lambda identifier=identifier: transition(balancer, identifier, "HEALTHY", since),
                     "No recovery from all-backend outage")

        # Kill exactly the process that logged receipt of this selected slow request.
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            pending = executor.submit(fetch, port, "/slow?ms=5000", "GET", None,
                                      {"X-Request-ID": "selected-before-crash"})
            def selected_backend():
                for identifier, process in live.items():
                    if any(event.get("event") == "received" and
                           event.get("request_id") == "selected-before-crash" for event in events(process)):
                        return identifier
                return None
            selected = wait_for(selected_backend, "Slow request was never received", timeout=3)
            crashed = live[selected]
            since = len(events(balancer))
            crashed.process.kill()  # owned process only; intentional SIGKILL fault
            crashed.process.wait(timeout=5)
            crashed.stderr.close()
            crashed.process.stdout.close()
            stopped.add(crashed)
            check(pending.result(timeout=5)[0] in (502, 504), "HC-09: mid-flight crash not bounded")
        wait_for(lambda: transition(balancer, selected, "UNHEALTHY", since), "HC-09: crash not excluded")
        survivor = "B" if selected == "A" else "A"
        check(all(json.loads(fetch(port, "/request")[2])["server_id"] == survivor for _ in range(8)),
              "Crash failover did not use survivor")
        checks.append("HC-09")
        since = len(events(balancer))
        recovered, recovered_port = start_backend(selected, ports[selected], "-restarted")
        live[selected] = recovered
        check(recovered_port == ports[selected], "Recovery changed configured port")
        wait_for(lambda: transition(balancer, selected, "HEALTHY", since), "HC-10: restart did not rejoin")
        recovered_ids = [json.loads(fetch(port, "/request")[2])["server_id"] for _ in range(8)]
        check(set(recovered_ids) == {"A", "B"}, "HC-10: recovered backend receives no traffic")
        checks.append("HC-10")

        alternating = fixture(alternate=True)
        flapping, flapping_port = start_proxy("flapping", [("flap", alternating.server_address[1])],
                                             ["--health-path", "/ready", "--health-interval-ms", "10"])
        wait_for(lambda: len([event for event in events(flapping) if event.get("event") == "health_probe"]) >= 8,
                 "HC-07: no repeated flap probes")
        flap_events = events(flapping)
        check(not any(event.get("to") == "UNHEALTHY" for event in flap_events), "HC-07: flapping caused ejection")
        check(all(event["state"] == "HEALTHY" for event in flap_events if event.get("event") == "health_probe"),
              "HC-07: unexpected flap state")
        check(fetch(flapping_port, "/request")[0] == 200, "Flapping backend unavailable")
        checks.append("HC-07")

        hanging = fixture(hang=True)
        timed, timed_port = start_proxy("timeout", [("hang", hanging.server_address[1])],
            ["--health-path", "/ready", "--health-response-timeout-ms", "40",
             "--health-failure-threshold", "1", "--upstream-timeout-ms", "2000"])
        check(timed.event["eligible"] == 0 and fetch(timed_port, "/request")[0] == 503,
              "HC-06: hanging readiness was admitted")
        timeout_probes = [event for event in events(timed) if event.get("reason") == "timeout"]
        check(timeout_probes and timeout_probes[0]["duration_ms"] < 1000,
              "HC-06: health response deadline was not independent of business timeout")
        wait_for(lambda: hanging.calls >= 2, "No pending health probe for shutdown check")
        timed.stop()
        stopped.add(timed)
        stopped_events = events(timed)
        gate = next(index for index, event in enumerate(stopped_events) if event["event"] == "health_checker_stopped")
        check(not any(event["event"] in ("health_transition", "health_probe") for event in stopped_events[gate + 1:]),
              "A probe published after health shutdown")
        checks.append("HC-06")

        check(not traffic_errors, f"Traffic errors during fault scenarios: {traffic_errors}")
        check(traffic and 200 in traffic, "No background traffic was observed")
        for event in events(balancer):
            if event["event"] in ("health_probe", "health_transition"):
                check(isinstance(event["timestamp_ms"], int) and event["timestamp_ms"] > 0,
                      "Missing transition/probe timestamp")
    finally:
        traffic_stop.set()
        if traffic_thread:
            traffic_thread.join(timeout=8)
        cleanup_errors = []
        for process in reversed(owned):
            if process not in stopped:
                try:
                    process.stop()
                except Exception as error:
                    cleanup_errors.append(str(error))
        for server, thread in fixtures:
            server.release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        (logs / "checks.json").write_text(json.dumps(sorted(checks), indent=2) + "\n")
        (logs / "traffic.json").write_text(json.dumps({"statuses": traffic, "errors": traffic_errors}, indent=2) + "\n")
        if cleanup_errors and sys.exc_info()[0] is None:
            raise AssertionError("; ".join(cleanup_errors))
    check(len(checks) == 10, "Not every HC-01..10 scenario completed")
    print("HC-01..10 passed: thresholds, readiness, timeout, flaps, outage, mid-flight crash, recovery, and shutdown")


if __name__ == "__main__":
    main()
