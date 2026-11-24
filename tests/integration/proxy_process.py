#!/usr/bin/env python3
"""HTTP forwarding checks HTTP-01..10 using only owned processes and isolated loopback ports."""

import argparse
import http.client
import http.server
import json
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import threading
import time

from test_backend_process import check, fetch


class OwnedProcess:
    def __init__(self, executable, args, name, logs):
        self.path = logs / f"{name}.stderr.log"
        self.stderr = self.path.open("w")
        self.process = subprocess.Popen([executable, *args], stdout=subprocess.PIPE,
                                        stderr=self.stderr, text=True)
        self.event = None

    def ready(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            check(selector.select(timeout=10), "No startup event within 10 seconds")
            line = self.process.stdout.readline()
        check(line, f"Startup failed: {self.path.read_text()}")
        self.event = json.loads(line)
        check(self.event["event"] == "listening", "Bad startup event")
        return self.event["port"]

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            code = self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise AssertionError("Owned process failed to stop")
        self.stderr.close()
        self.process.stdout.close()
        check(code == 0, f"Process exit {code}: {self.path.read_text()}")


class WireHandler(http.server.BaseHTTPRequestHandler):
    """Origin fixture for framing/header behavior beyond simulator endpoints."""
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "12345")
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        if self.path == "/hang":
            time.sleep(0.8)
        if self.path == "/upgrade":
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.end_headers()
            self.close_connection = True
            return
        if self.path == "/oversized":
            self.send_response(200)
            self.send_header("Content-Length", "1048577")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        if self.path == "/informational":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n")
        body = b"ok" if self.path == "/health" else json.dumps(dict(self.headers)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "close, X-Origin-Secret")
        self.send_header("X-Origin-Secret", "hidden")
        self.send_header("Keep-Alive", "timeout=10")
        self.send_header("Via", "1.0 origin")
        if self.path == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Trailer")
            self.end_headers()
            self.wfile.write(b"5\r\nhello\r\n0\r\nX-Trailer: secret\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # expected after timeout/client-abort tests


def raw_status(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as stream:
        stream.sendall(raw)
        with stream.makefile("rb") as response:
            return int(response.readline(8192).split()[1])


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
    fixture = None
    fixture_thread = None
    checks = []

    def start(executable, flags, name):
        process = OwnedProcess(executable, flags, name, logs)
        owned.append(process)
        return process, process.ready()

    def proxy_at(name, backends, timeout=1000):
        flags = ["--metrics-port", "0", "--port", "0", "--upstream-timeout-ms", str(timeout), "--connect-timeout-ms", "300"]
        for identifier, port in backends:
            flags += ["--backend", f"{identifier},127.0.0.1,{port}"]
        return start(proxy, flags, name)

    try:
        backends = []
        for name in "ABC":
            process, port = start(backend, ["--id", name, "--port", "0", "--test-mode"], name)
            backends.append((name, port, process))
        balancer, port = proxy_at("proxy", [(name, value) for name, value, _ in backends])
        check(balancer.event["eligible"] == 3, "Readiness bootstrap did not admit all backends")

        identities = [json.loads(fetch(port, "/request")[2])["server_id"] for _ in range(6)]
        check(identities == list("ABCABC"), f"HTTP-01: wrong round robin {identities}")
        checks.append("HTTP-01")

        payload = b"proxy\x00binary\xff"
        status, headers, body = fetch(port, "/echo", "POST", payload,
                                     {"Content-Type": "application/octet-stream", "X-Request-ID": "post-once",
                                      "X-Echo-Token": "token"})
        check(status == 200 and body == payload and headers["X-Echo-Token"] == "token", "HTTP-02: POST changed")
        received = 0
        for name, _, process in backends:
            events = [json.loads(line) for line in process.path.read_text().splitlines()]
            received += sum(event.get("event") == "received" and event.get("request_id") == "post-once"
                            for event in events)
        check(received == 1, "HTTP-02: POST was processed more than once")
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connection.request("POST", "/echo", body=iter([b"chunk", b"ed"]), encode_chunked=True)
        response = connection.getresponse()
        check(response.status == 200 and response.read() == b"chunked", "Chunked POST changed")
        connection.close()
        checks.append("HTTP-02")

        check(fetch(port, "/error")[0] == 500, "HTTP-04: business 500 changed")
        after = [json.loads(fetch(port, "/request")[2])["server_id"] for _ in range(6)]
        check(all(after.count(name) == 2 for name in "ABC"), "HTTP-04: business 500 ejected a backend")
        checks.append("HTTP-04")

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        persistent = []
        for _ in range(6):
            connection.request("GET", "/request")
            response = connection.getresponse()
            check(response.status == 200, "Persistent request failed")
            persistent.append(json.loads(response.read())["server_id"])
            check(connection.sock is not None, "HTTP-08: client connection was closed")
        connection.close()
        check(set(persistent[:3]) == set("ABC") and persistent[:3] == persistent[3:], "HTTP-08: connection affinity")
        checks.append("HTTP-08")

        empty, empty_port = proxy_at("empty", [])
        check(empty.event["eligible"] == 0 and fetch(empty_port, "/request")[0] == 503, "HTTP-05: no 503")
        c_port = backends[2][1]
        fetch(c_port, "/admin/ready?value=false", "POST")
        unavailable, unavailable_port = proxy_at("unready", [("C", c_port)])
        check(unavailable.event["eligible"] == 0 and fetch(unavailable_port, "/request")[0] == 503,
              "Unready backend was admitted")
        fetch(c_port, "/admin/ready?value=true", "POST")
        checks.append("HTTP-05")

        check(raw_status(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n") == 413,
              "HTTP-07: oversized body accepted")
        check(raw_status(port, b"GET / HTTP/1.1\r\nHost: x\r\nX-Large: " + b"x" * 17000 + b"\r\n\r\n") == 431,
              "HTTP-07: oversized header accepted")
        check(raw_status(port, b"BAD REQUEST\r\n\r\n") == 400, "Malformed request accepted")
        check(raw_status(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n") == 400,
              "Ambiguous framing accepted")
        checks.append("HTTP-07")

        check(raw_status(port, b"CONNECT example:80 HTTP/1.1\r\nHost: example\r\n\r\n") == 501, "CONNECT accepted")
        check(raw_status(port, b"GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n") == 501,
              "Upgrade accepted")
        check(raw_status(port, b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\n") == 417,
              "Unsupported expectation not rejected promptly")
        checks.append("HTTP-10")

        fixture = http.server.ThreadingHTTPServer(("127.0.0.1", 0), WireHandler)
        fixture.daemon_threads = True
        fixture_thread = threading.Thread(target=lambda: fixture.serve_forever(poll_interval=0.05), daemon=True)
        fixture_thread.start()
        fixture_port = fixture.server_address[1]
        _, wire_port = proxy_at("wire", [("wire", fixture_port)], timeout=200)
        status, headers, body = fetch(wire_port, "/inspect", headers={
            "Connection": "keep-alive, X-Secret", "X-Secret": "hidden", "Proxy-Authorization": "secret",
            "Forwarded": "for=spoof", "X-Forwarded-For": "spoof", "X-Forwarded-Host": "spoof",
            "X-Forwarded-Proto": "https", "X-Real-IP": "spoof", "Via": "1.0 earlier", "TE": "trailers"})
        observed = {key.lower(): value for key, value in json.loads(body).items()}
        check(status == 200 and observed["host"] == f"127.0.0.1:{fixture_port}", "HTTP-03: Host not rewritten")
        check(observed["x-forwarded-for"] == "127.0.0.1" and observed["x-forwarded-proto"] == "http", "Spoofed identity")
        check(all(key not in observed for key in ["x-secret", "proxy-authorization", "forwarded", "x-forwarded-host", "x-real-ip", "te"]),
              "Hop/identity headers leaked upstream")
        check(observed["via"] == "1.0 earlier, 1.1 load-balancer", "Request Via incorrect")
        check(headers["Via"] == "1.0 origin, 1.1 load-balancer", "Response Via incorrect")
        check("X-Origin-Secret" not in headers and "Keep-Alive" not in headers, "Response hop headers leaked")
        status, headers, body = fetch(wire_port, "/chunked")
        check(status == 200 and body == b"hello" and headers["Content-Length"] == "5", "Chunked response framing")
        check("Transfer-Encoding" not in headers and "X-Trailer" not in headers, "Trailers leaked")
        status, headers, body = fetch(wire_port, "/head", "HEAD")
        check(status == 200 and headers["Content-Length"] == "12345" and body == b"", "HEAD framing changed")
        check(fetch(wire_port, "/informational")[0] == 200, "Informational response handling failed")
        check(fetch(wire_port, "/upgrade")[0] == 502, "Upstream upgrade was forwarded")
        check(fetch(wire_port, "/oversized")[0] == 502, "Oversized upstream body accepted")
        checks.append("HTTP-03")

        started = time.monotonic()
        check(fetch(wire_port, "/hang")[0] == 504, "Upstream timeout did not return 504")
        check(time.monotonic() - started < 3, "Upstream deadline did not bound elapsed time")
        with socket.create_connection(("127.0.0.1", wire_port), timeout=5) as stream:
            stream.sendall(b"GET /hang HTTP/1.1\r\nHost: x\r\n\r\n")
        time.sleep(0.4)  # exceeds the configured 200ms upstream deadline
        check(fetch(wire_port, "/inspect")[0] == 200, "HTTP-09: disconnect killed proxy")
        checks.append("HTTP-09")

        a_name, a_port, a_process = backends[0]
        _, doomed_port = proxy_at("refused", [(a_name, a_port)])
        a_process.stop()
        stopped.add(a_process)
        check(fetch(doomed_port, "/request")[0] == 502, "HTTP-06: refused connect did not return 502")
        check(fetch(doomed_port, "/request")[0] == 502, "HTTP-06: proxy crashed after failure")
        checks.append("HTTP-06")
    finally:
        cleanup_errors = []
        for process in reversed(owned):
            if process not in stopped:
                try:
                    process.stop()
                except Exception as error:
                    cleanup_errors.append(str(error))
        if fixture:
            fixture.shutdown()
            fixture.server_close()
            fixture_thread.join(timeout=2)
        (logs / "checks.json").write_text(json.dumps(sorted(checks), indent=2) + "\n")
        if cleanup_errors and sys.exc_info()[0] is None:
            raise AssertionError("; ".join(cleanup_errors))
    check(len(checks) == 10, "Not every HTTP-01..10 check completed")
    print("HTTP-01..10 passed: forwarding, headers, errors, limits, keep-alive, disconnects, upgrades; plus timeouts/framing")


if __name__ == "__main__":
    main()
