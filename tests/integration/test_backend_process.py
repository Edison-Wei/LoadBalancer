#!/usr/bin/env python3
"""Direct simulator checks: three owned processes, real HTTP, curl, and cleanup."""

import argparse
import concurrent.futures
import http.client
import json
from pathlib import Path
import selectors
import shutil
import socket
import subprocess
import time


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def fetch(port, path, method="GET", body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True)
    parser.add_argument("--logs", required=True)
    args = parser.parse_args()
    backend = str(Path(args.backend).resolve())
    logs = Path(args.logs)
    logs.mkdir(parents=True, exist_ok=True)
    processes = []
    errors = []
    ports = {}
    try:
        for name, flags in [("A", ["--test-mode"]), ("B", []),
                            ("C", ["--test-mode", "--not-ready"])]:
            stderr = (logs / f"{name}.stderr.log").open("w")
            errors.append(stderr)
            process = subprocess.Popen(
                [backend, "--id", name, "--listen", "127.0.0.1", "--port", "0", *flags],
                stdout=subprocess.PIPE, stderr=stderr, text=True,
            )
            processes.append(process)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                check(selector.select(timeout=10), f"{name}: no startup event within 10 seconds")
                line = process.stdout.readline()
            check(line, f"{name}: startup failed: {(logs / f'{name}.stderr.log').read_text()}")
            event = json.loads(line)
            check(event["event"] == "listening" and event["server_id"] == name, "Bad startup identity")
            ports[name] = event["port"]
            (logs / f"{name}.startup.json").write_text(line)

        curl = shutil.which("curl")
        check(curl, "curl is required for the three-backend identity smoke test")
        identities = []
        for name, port in ports.items():
            result = subprocess.run(
                [curl, "--silent", "--show-error", "--fail", "--noproxy", "*", "--max-time", "5",
                 f"http://127.0.0.1:{port}/request"],
                capture_output=True, text=True, timeout=8, check=True,
            )
            identity = json.loads(result.stdout)
            check(identity["server_id"] == name, "curl reached the wrong backend")
            identities.append(identity)
        (logs / "curl-identities.json").write_text(json.dumps(identities, indent=2) + "\n")

        check(fetch(ports["A"], "/health")[0] == 200, "A must start ready")
        check(fetch(ports["C"], "/health")[0] == 503, "C must start unready")
        check(fetch(ports["A"], "/admin/ready?value=false", "POST")[0] == 200, "Toggle failed")
        check(fetch(ports["A"], "/health")[0] == 503, "A readiness did not change")
        check(fetch(ports["B"], "/health")[0] == 200, "A changed B's readiness")
        check(fetch(ports["B"], "/admin/ready?value=false", "POST")[0] == 404, "Admin enabled by default")
        check(fetch(ports["A"], "/admin/ready?value=true", "POST")[0] == 200, "Recovery failed")
        check(fetch(ports["A"], "/health")[0] == 200, "A did not become ready")

        payload = b"binary\x00echo\xff"
        status, headers, body = fetch(ports["A"], "/echo", "POST", payload,
                                     {"Content-Type": "application/octet-stream",
                                      "X-Echo-Token": "token-42", "X-Request-ID": "echo-42"})
        check(status == 200 and body == payload, "Echo body changed")
        check(headers["X-Echo-Token"] == "token-42" and headers["X-Request-ID"] == "echo-42",
              "Echo headers changed")
        check(headers["Content-Type"] == "application/octet-stream", "Content type changed")
        check(fetch(ports["A"], "/error")[0] == 500, "Error endpoint did not return 500")
        check(fetch(ports["A"], "/large?bytes=1048576")[2] == b"x" * 1048576, "Large payload mismatch")
        check(fetch(ports["A"], "/large?bytes=1048577")[0] == 400, "Unbounded large payload")
        check(fetch(ports["A"], "/slow?ms=5001")[0] == 400, "Unbounded delay")

        started = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            delayed = executor.submit(fetch, ports["A"], "/slow?ms=100")
            check(fetch(ports["A"], "/request")[0] == 200, "Parallel request failed")
            check(delayed.result(timeout=8)[0] == 200, "Delayed response failed")
        check(time.monotonic() - started >= 0.1, "Delay was not applied")

        connection = http.client.HTTPConnection("127.0.0.1", ports["A"], timeout=5)
        ids = []
        for _ in range(2):
            connection.request("GET", "/request")
            response = connection.getresponse()
            check(response.status == 200 and response.version == 11, "Keep-alive request failed")
            ids.append(json.loads(response.read())["request_id"])
            check(connection.sock is not None, "Server closed a persistent connection")
        connection.close()
        check(ids[0] != ids[1], "Generated request IDs are not unique")

        for raw, expected in [
            (b"BAD REQUEST\r\n\r\n", 400),
            (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n", 413),
            (b"GET /request HTTP/1.1\r\nHost: localhost\r\nX-Large: " + b"x" * 9000 + b"\r\n\r\n", 431),
        ]:
            with socket.create_connection(("127.0.0.1", ports["A"]), timeout=5) as stream:
                stream.sendall(raw)
                with stream.makefile("rb") as response:
                    status_line = response.readline(8192).rstrip(b"\r\n")
                check(int(status_line.split()[1]) == expected, f"Wrong parser rejection: {status_line!r}")

        rejected = subprocess.run(
            [backend, "--test-mode", "--listen", "0.0.0.0"], capture_output=True, text=True, timeout=5)
        check(rejected.returncode != 0 and "loopback" in rejected.stderr, "Unsafe test mode accepted")
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError("Owned backend did not stop after SIGTERM")
            if process.stdout:
                process.stdout.close()
            # Startup failures must remain failures, including sanitizer aborts.
            if code != 0:
                print(f"Backend exit code {code}; inspect {logs}")
        for stderr in errors:
            stderr.close()

    for name in ports:
        events = [json.loads(line) for line in (logs / f"{name}.stderr.log").read_text().splitlines()]
        check(any(event["event"] == "received" for event in events), "Missing receive log")
        check(any(event["event"] == "completed" for event in events), "Missing completion log")
        if name == "A":
            check(any(event.get("request_id") == "echo-42" and event["event"] == "completed"
                      for event in events), "Missing correlated completion log")
    check(all(process.returncode == 0 for process in processes), "Backend failed during shutdown")
    print("Three backend processes passed curl identity, HTTP behavior, limits, logging, and shutdown checks")


if __name__ == "__main__":
    main()
