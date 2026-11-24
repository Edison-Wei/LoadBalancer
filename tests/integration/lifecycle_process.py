#!/usr/bin/env python3
"""Worker/load, admission, independent deadlines, cancellation, and shutdown gates."""

import argparse
import asyncio
import concurrent.futures
import http.client
import http.server
import json
from pathlib import Path
import resource
import select
import signal
import socket
import struct
import sys
import threading
import time

from health_process import events, wait_for
from proxy_process import OwnedProcess
from test_backend_process import check, fetch


async def async_get(port, path="/request", gate=None, ready=None):
    reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 20)
    try:
        if ready:
            ready()
        if gate:
            await gate.wait()
        writer.write(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        await writer.drain()
        header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 20)
        lines = header.decode().split("\r\n")
        status = int(lines[0].split()[1])
        fields = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
        body = await asyncio.wait_for(reader.readexactly(int(fields.get("Content-Length", "0"))), 20)
        return status, body
    finally:
        writer.close()
        await writer.wait_closed()


async def load(port, count, path="/request", process=None):
    started = time.monotonic()
    gate, connected = asyncio.Event(), asyncio.Event()
    ready_count = 0
    baseline = sum(event["event"] == "connection_accepted" for event in events(process)) if process else 0
    def ready():
        nonlocal ready_count
        ready_count += 1
        if ready_count == count:
            connected.set()
    tasks = [asyncio.create_task(async_get(port, path, gate, ready)) for _ in range(count)]
    try:
        await asyncio.wait_for(connected.wait(), 20)
        if process:
            await asyncio.to_thread(wait_for, lambda: sum(event["event"] == "connection_accepted"
                for event in events(process)) >= baseline + count, "Proxy did not accept the full held connection set")
        gate.set()
        result = await asyncio.gather(*tasks)
    finally:
        gate.set()
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
    check(all(status == 200 for status, _ in result), "Unexpected overload/error in admitted load test")
    return {"clients": count, "elapsed_seconds": time.monotonic() - started,
            "identities": [json.loads(body)["server_id"] for _, body in result]}


class Origin(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/hang":
            self.server.started.set()
            self.server.release.wait(timeout=10)
        body = b"x" * (16 * 1024 * 1024) if self.path == "/large" else b"ok"
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        # Deliberately stop consuming the body so an upstream write cannot finish.
        self.server.started.set()
        self.server.release.wait(timeout=10)


class ConnectGate:
    """Fill only this owned listener's accept queue to produce a real connect stall."""
    def __init__(self):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.02)
        self.port = self.listener.getsockname()[1]
        self.pause = threading.Event()
        self.paused = threading.Event()
        self.stop = threading.Event()
        self.fillers = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            if self.pause.is_set():
                self.paused.set()
                self.stop.wait(0.02)
                continue
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(1)
                try:
                    connection.recv(8192)
                    connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
                except OSError:
                    pass

    def saturate(self):
        self.pause.set()
        check(self.paused.wait(2), "Connect fixture did not pause")
        for _ in range(32):
            connection = socket.socket()
            connection.setblocking(False)
            connection.connect_ex(("127.0.0.1", self.port))
            self.fillers.append(connection)
        time.sleep(0.05)
        writable = set(select.select([], self.fillers, [], 0)[1])
        check(any(connection not in writable for connection in self.fillers), "Local kernel did not produce a connect stall")

    def close(self):
        self.stop.set()
        self.listener.close()
        for connection in self.fillers:
            connection.close()
        self.thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True)
    parser.add_argument("--proxy", required=True)
    parser.add_argument("--logs", required=True)
    args = parser.parse_args()
    backend, proxy = str(Path(args.backend).resolve()), str(Path(args.proxy).resolve())
    logs = Path(args.logs)
    logs.mkdir(parents=True, exist_ok=True)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < 8192:
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(8192, hard), hard))
    check(resource.getrlimit(resource.RLIMIT_NOFILE)[0] >= 4096, "Load test requires 4096 per-process file descriptors")
    owned, stopped, origins, gates, sockets = [], set(), [], [], []
    checks, loads, summaries = [], [], {}

    def start(executable, flags, name):
        process = OwnedProcess(executable, flags, name, logs)
        owned.append(process)
        return process, process.ready()

    def start_proxy(name, endpoints, flags=()):
        values = ["--metrics-port", "0", "--port", "0", "--workers", "4"]
        for identifier, port in endpoints:
            values += ["--backend", f"{identifier},127.0.0.1,{port}"]
        return start(proxy, [*values, *flags], name)

    def finish(process, already_signaled=False):
        if already_signaled:
            check(process.process.wait(timeout=5) == 0, "Proxy failed to exit after signal")
            process.stderr.close()
            process.process.stdout.close()
        else:
            process.stop()
        stopped.add(process)
        summary = next((event for event in reversed(events(process)) if event["event"] == "shutdown_complete"), None)
        check(summary is not None, "No shutdown summary")
        check(all(summary[key] == 0 for key in ["connections", "requests", "upstreams", "queued_requests"]),
              f"Resource leases leaked: {summary}")
        summaries[process.path.stem] = summary
        return summary

    def origin():
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
        server.daemon_threads = True
        server.started, server.release = threading.Event(), threading.Event()
        thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.02), daemon=True)
        thread.start()
        origins.append((server, thread))
        return server

    def received(process, request_id):
        return any(event.get("event") == "received" and event.get("request_id") == request_id
                   for event in events(process))

    def accepted(process, amount):
        return sum(event["event"] == "connection_accepted" for event in events(process)) >= amount

    try:
        a, a_port = start(backend, ["--id", "A", "--port", "0", "--test-mode"], "A")
        b, b_port = start(backend, ["--id", "B", "--port", "0", "--test-mode"], "B")
        endpoints = [("A", a_port), ("B", b_port)]
        for workers in [1, 2, 4, 8]:
            process, port = start_proxy(f"workers-{workers}", endpoints,
                ["--workers", str(workers), "--max-connections", "1600", "--max-inflight", "1200",
                 "--max-upstream-connections", "1200", "--health-interval-ms", "20",
                 "--client-read-timeout-ms", "20000", "--upstream-timeout-ms", "10000",
                 "--upstream-read-timeout-ms", "10000"])
            check(process.event["workers"] == workers, "Worker option ignored")
            counts = [100] + ([10, 500, 1000] if workers == 4 else [])
            for clients in counts:
                result = asyncio.run(load(port, clients, process=process))
                result["workers"] = workers
                check(result["identities"].count("A") == clients // 2, "Concurrent round robin imbalanced")
                check(result["identities"].count("B") == clients // 2, "Concurrent round robin imbalanced")
                loads.append(result)
            if workers == 4:
                async def mutations_and_traffic():
                    mutation_done = asyncio.Event()
                    async def mutate():
                        try:
                            for _ in range(3):
                                for value, target in [("false", "UNHEALTHY"), ("true", "HEALTHY")]:
                                    since = len(events(process))
                                    await asyncio.to_thread(fetch, b_port, f"/admin/ready?value={value}", "POST")
                                    await asyncio.to_thread(wait_for, lambda: any(event.get("event") == "health_transition"
                                        and event.get("backend") == "B" and event.get("to") == target
                                        for event in events(process)[since:]), "No health transition during concurrent load")
                        finally:
                            mutation_done.set()
                    async def traffic():
                        batches = 0
                        while batches < 8 or not mutation_done.is_set():
                            await load(port, 100, "/slow?ms=50")
                            batches += 1
                    await asyncio.gather(mutate(), traffic())
                asyncio.run(mutations_and_traffic())
                transitions = [event.get("to") for event in events(process)
                               if event.get("event") == "health_transition" and event.get("backend") == "B"]
                check("UNHEALTHY" in transitions and transitions.count("HEALTHY") >= 2, "Registry never mutated under traffic")
            summary = finish(process)
            check(summary["workers_joined"] == workers, "Worker pool did not join")
            if workers == 4:
                check(summary["peak_connections"] >= 1000, "Did not hold 1000 active connections simultaneously")
            check(summary["peak_connections"] <= 1600 and summary["peak_requests"] <= 1200
                  and summary["peak_upstreams"] <= 1200, "Configured cap exceeded")
        checks += ["workers-1-2-4-8", "clients-10-100-500-1000", "registry-updates-during-traffic"]

        for kind, flags in [("requests", ["--max-inflight", "1"]),
                             ("upstreams", ["--max-inflight", "8", "--max-upstream-connections", "1"])]:
            process, port = start_proxy(f"cap-{kind}", [("A", a_port)], flags)
            token = f"hold-{kind}"
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                pending = executor.submit(fetch, port, "/slow?ms=500", "GET", None, {"X-Request-ID": token})
                wait_for(lambda: received(a, token), "Holding request did not reach backend")
                check(fetch(port, "/request")[0] == 503, f"{kind}: excess work was not rejected")
                check(not pending.done(), "Overload was queued until capacity returned")
                check(pending.result(timeout=5)[0] == 200, "Admitted request failed")
            check(fetch(port, "/request")[0] == 200, "Capacity was not returned")
            summary = finish(process)
            key = "peak_requests" if kind == "requests" else "peak_upstreams"
            check(summary[key] == 1 and summary["rejected_requests"] >= 1, "Resource cap not recorded")
        checks.append("request-upstream-caps-no-queue")

        process, port = start_proxy("cap-connections", [("A", a_port)], ["--max-connections", "2"])
        idle = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(2)]
        sockets.extend(idle)
        wait_for(lambda: accepted(process, 2), "Idle connections not accepted")
        excess = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            excess.sendall(b"GET /request HTTP/1.1\r\nHost: x\r\n\r\n")
            check(excess.recv(1024) == b"", "Excess connection was retained")
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            excess.close()
        for stream in idle:
            stream.close()
        wait_for(lambda: sum(event["event"] == "connection_closed" for event in events(process)) >= 2,
                 "Idle connection capacity was not returned")
        check(fetch(port, "/request")[0] == 200, "Connection cap never recovered")
        summary = finish(process)
        check(summary["peak_connections"] == 2 and summary["rejected_connections"] >= 1, "Connection bound exceeded")
        checks.append("connection-cap")

        process, port = start_proxy("slowloris", [("A", a_port)],
            ["--client-read-timeout-ms", "100", "--max-inflight", "1"])
        for incomplete in [b"GET /request HTTP/1.1\r\nHost: x",
                           b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\npartial"]:
            before = sum(event["event"] == "client_read_timeout" for event in events(process))
            stream = socket.create_connection(("127.0.0.1", port), timeout=5)
            sockets.append(stream)
            stream.sendall(incomplete)
            wait_for(lambda: sum(event["event"] == "client_read_timeout" for event in events(process)) > before,
                     "Incomplete headers/body escaped ingress deadline")
            stream.close()
            check(fetch(port, "/request")[0] == 200, "Slow client leaked admission capacity")
        finish(process)
        checks.append("slowloris-header-body-deadlines")

        wire = origin()
        wire_endpoint = [("wire", wire.server_address[1])]
        for name, flags, expected in [
            ("read-deadline", ["--upstream-timeout-ms", "5000", "--upstream-read-timeout-ms", "80"], "stage"),
            ("overall-deadline", ["--upstream-timeout-ms", "80", "--upstream-read-timeout-ms", "5000"], "overall")]:
            process, port = start_proxy(name, wire_endpoint, flags)
            check(fetch(port, "/hang")[0] == 504, "Hanging origin escaped its deadline")
            check(any(event.get("status") == 504 and event.get("stage") == "read" and event.get("deadline") == expected
                      for event in events(process)), "Wrong timeout source")
            check(fetch(port, "/health")[0] == 200, "Timeout killed worker")
            finish(process)
        checks += ["upstream-read-deadline", "upstream-overall-deadline"]

        gate = ConnectGate()
        gates.append(gate)
        process, port = start_proxy("connect-deadline", [("gate", gate.port)],
            ["--connect-timeout-ms", "80", "--upstream-timeout-ms", "5000"])
        gate.saturate()
        check(fetch(port, "/request")[0] == 504, "Full owned accept queue escaped connect deadline")
        check(any(event.get("status") == 504 and event.get("stage") == "connect" for event in events(process)),
              "Connect timeout not classified")
        finish(process)
        checks.append("upstream-connect-deadline")

        process, port = start_proxy("write-deadline", wire_endpoint,
            ["--max-body-bytes", "16777216", "--upstream-timeout-ms", "5000",
             "--upstream-write-timeout-ms", "80", "--upstream-read-timeout-ms", "5000"])
        check(fetch(port, "/backpressure", "POST", b"x" * (16 * 1024 * 1024))[0] == 504,
              "Blocked upstream write escaped deadline")
        check(any(event.get("status") == 504 and event.get("stage") == "write" for event in events(process)),
              "Write timeout not classified")
        finish(process)
        checks.append("upstream-write-deadline")

        process, port = start_proxy("client-write", wire_endpoint,
            ["--max-body-bytes", "16777216", "--client-write-timeout-ms", "80",
             "--upstream-read-timeout-ms", "20000", "--upstream-timeout-ms", "30000"])
        stream = socket.socket()
        sockets.append(stream)
        stream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        stream.settimeout(5)
        stream.connect(("127.0.0.1", port))
        stream.sendall(b"GET /large HTTP/1.1\r\nHost: x\r\n\r\n")
        # Isolate downstream backpressure from the cost of receiving/parsing a
        # 16 MiB origin response under sanitizers. A short upstream deadline can
        # otherwise return a tiny 504 before the client write is ever exercised.
        wait_for(lambda: any(event.get("event") == "upstream_complete" and event.get("status") == 200
                             for event in events(process)), "Large origin response did not complete", timeout=30)
        wait_for(lambda: any(event["event"] == "client_write_timeout" for event in events(process)),
                 "Slow receiver escaped client write deadline")
        stream.close()
        check(fetch(port, "/health")[0] == 200, "Client write timeout killed worker")
        finish(process)
        checks.append("client-write-deadline")

        reset_origin = origin()
        process, port = start_proxy("reset", [("wire", reset_origin.server_address[1])],
            ["--upstream-timeout-ms", "5000", "--upstream-read-timeout-ms", "5000", "--max-upstream-connections", "1"])
        stream = socket.create_connection(("127.0.0.1", port), timeout=5)
        stream.sendall(b"GET /hang HTTP/1.1\r\nHost: x\r\n\r\n")
        check(reset_origin.started.wait(3), "Reset request was not in flight")
        stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        stream.close()
        wait_for(lambda: any(event["event"] == "upstream_cancelled" and event.get("reason") == "client_reset"
                             for event in events(process)), "RST did not cancel upstream promptly", timeout=2)
        check(fetch(port, "/health")[0] == 200, "RST cancellation did not release upstream capacity")
        check(finish(process)["reset_cancellations"] >= 1, "Reset not recorded")
        reset_origin.release.set()
        checks.append("rst-cancellation")

        process, port = start_proxy("half-close", [("A", a_port)])
        stream = socket.create_connection(("127.0.0.1", port), timeout=5)
        stream.sendall(b"GET /slow?ms=30 HTTP/1.1\r\nHost: x\r\n\r\n")
        stream.shutdown(socket.SHUT_WR)
        response = http.client.HTTPResponse(stream)
        response.begin()
        check(response.status == 200 and json.loads(response.read())["server_id"] == "A", "FIN half-close canceled a valid response")
        response.close()
        stream.close()
        # Pipeline bytes must not be consumed by disconnect monitoring.
        stream = socket.create_connection(("127.0.0.1", port), timeout=5)
        stream.sendall(b"GET /slow?ms=30 HTTP/1.1\r\nHost: x\r\n\r\nGET /request HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        with stream.makefile("rb") as reader:
            for _ in range(2):
                check(b"200" in reader.readline(), "Pipelined request lost")
                headers = {}
                while (line := reader.readline()) != b"\r\n":
                    check(line, "Incomplete pipelined response")
                    key, value = line.decode().strip().split(": ", 1)
                    headers[key.lower()] = value
                check(json.loads(reader.read(int(headers["content-length"])))['server_id'] == "A", "Pipelined body changed")
        stream.close()
        finish(process)
        checks.append("half-close-and-pipeline-preserved")

        for forced, delay, grace in [(False, 150, 1000), (True, 5000, 80)]:
            name = "forced-shutdown" if forced else "drain-shutdown"
            process, port = start_proxy(name, [("A", a_port)],
                ["--shutdown-grace-ms", str(grace), "--upstream-timeout-ms", "10000", "--upstream-read-timeout-ms", "10000"])
            token = name
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                pending = executor.submit(fetch, port, f"/slow?ms={delay}", "GET", None, {"X-Request-ID": token})
                wait_for(lambda: received(a, token), "Shutdown request was not in flight")
                started = time.monotonic()
                process.process.send_signal(signal.SIGTERM)
                if not forced:
                    check(pending.result(timeout=5)[0] == 200, "Admitted request did not drain")
                else:
                    try:
                        result = pending.result(timeout=5)
                        check(result[0] != 200, "Grace deadline did not cancel long work")
                    except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError):
                        pass
                summary = finish(process, already_signaled=True)
                check(summary["forced"] == forced, "Wrong shutdown path")
                check(time.monotonic() - started < 3, "Shutdown exceeded grace bound and cleanup allowance")
        checks += ["graceful-drain", "forced-grace-deadline"]
    finally:
        cleanup_errors = []
        for stream in sockets:
            stream.close()
        for process in reversed(owned):
            if process not in stopped:
                try:
                    process.stop()
                except Exception as error:
                    cleanup_errors.append(str(error))
        for gate in gates:
            gate.close()
        for server, thread in origins:
            server.release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        (logs / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
        (logs / "loads.json").write_text(json.dumps(loads, indent=2) + "\n")
        (logs / "resources.json").write_text(json.dumps(summaries, indent=2) + "\n")
        if cleanup_errors and sys.exc_info()[0] is None:
            raise AssertionError("; ".join(cleanup_errors))
    print("Lifecycle checks passed: workers/load, bounded admission, stage deadlines, reset/half-close, and graceful shutdown")


if __name__ == "__main__":
    main()
