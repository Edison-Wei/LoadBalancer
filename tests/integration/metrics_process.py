#!/usr/bin/env python3
"""Real isolated metrics endpoint: accounting, bounded labels, health and shutdown."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import re
import socket
import struct
import time

from proxy_process import OwnedProcess
from health_process import wait_for
from test_backend_process import check, fetch


def parse(text):
    values = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        match = re.fullmatch(r'([a-zA-Z_:][a-zA-Z_0-9:]*)(\{.*\})? ([0-9.eE+\-]+)', line)
        check(match, f"Invalid sample: {line}")
        name, labels, value = match.groups()
        key = (name, tuple(sorted(re.findall(r'(\w+)="([^"\\]*)"', labels or ''))))
        check(key not in values, f"Duplicate sample: {line}")
        values[key] = float(value)
    return values


def value(samples, name, **labels):
    return samples[(name, tuple(sorted(labels.items())))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', required=True)
    parser.add_argument('--proxy', required=True)
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    owned, sockets, checks = [], [], []

    def start(executable, flags, name):
        process = OwnedProcess(executable, flags, name, args.logs)
        owned.append(process)
        return process, process.ready()

    try:
        endpoints = []
        for name in ('A', 'B'):
            _, port = start(args.backend, ['--id', name, '--port', '0', '--test-mode'], name)
            endpoints.append(port)
        flags = ['--port', '0', '--metrics-port', '0', '--max-inflight', '1',
                 '--upstream-timeout-ms', '350', '--health-interval-ms', '40']
        for name, port in zip(('A', 'B'), endpoints):
            flags += ['--backend', f'{name},127.0.0.1,{port}']
        proxy, port = start(args.proxy, flags, 'proxy')
        monitoring = proxy.event['metrics_port']

        def scrape():
            status, headers, body = fetch(monitoring, '/metrics')
            check(status == 200 and 'version=0.0.4' in headers['Content-Type'], 'Scrape format')
            return body.decode(), parse(body.decode())

        text, initial = scrape()
        check(text.count('# TYPE ') == 10, 'Ten required metric families')
        check(value(initial, 'lb_request_duration_seconds_count', route='proxy') == 0, 'Monitoring counts as traffic')
        check(fetch(port, '/request')[0] == 200, 'First request')
        for _ in range(4):
            scrape()
        status, _, body = fetch(port, '/request')
        check(status == 200 and json.loads(body)['server_id'] == 'B', 'Scrapes must not advance scheduler')
        check(fetch(monitoring, '/missing')[0] == 404, 'Unknown monitoring path')
        check(fetch(monitoring, '/metrics', method='POST')[0] == 405, 'Monitoring method restriction')
        check(fetch(port, '/metrics')[0] == 404, 'Public interface must not expose instrumentation')
        checks.append('OBS-01 isolated listener and scheduler')

        with ThreadPoolExecutor(max_workers=3) as pool:
            pending = pool.submit(fetch, port, '/slow?ms=1000')
            wait_for(lambda: value(scrape()[1], 'lb_inflight_requests') == 1, 'In-flight gauge')
            live = scrape()[1]
            check(value(live, 'lb_active_connections', side='downstream') >= 1, 'Downstream gauge')
            wait_for(lambda: value(scrape()[1], 'lb_active_connections', side='upstream') >= 1, 'Upstream gauge')
            check(fetch(port, '/request')[0] == 503, 'Request cap')
            check(pending.result()[0] == 504, 'Timeout response')
        fetch(port, '/error')
        wait_for(lambda: value(scrape()[1], 'lb_inflight_requests') == 0, 'Gauge release')
        _, counted = scrape()
        check(value(counted, 'lb_requests_total', method='GET', status_class='5xx') == 3, 'Completed 500/503/504')
        check(value(counted, 'lb_overload_rejections_total', reason='request_limit') == 1, 'Overload counter')
        check(sum(v for (n, labels), v in counted.items() if n == 'lb_upstream_errors_total' and ('reason', 'timeout') in labels) == 1, 'Timeout counter')
        check(sum(v for (n, labels), v in counted.items() if n == 'lb_backend_requests_total' and ('result', 'http_error') in labels) == 2, 'HTTP errors separate from transport')
        check(value(counted, 'lb_request_duration_seconds_count', route='proxy') == 6, 'Histogram response count')
        checks.append('OBS-02 response outcomes, latency, resources and overload')

        fetch(endpoints[1], '/admin/ready?value=false', method='POST')
        wait_for(lambda: value(scrape()[1], 'lb_backend_healthy', backend='B') == 0, 'Exclusion metric')
        unhealthy = scrape()[1]
        check(value(unhealthy, 'lb_healthcheck_total', backend='B', result='failure') >= 3, 'Probe failures')
        check(value(unhealthy, 'lb_backend_state_changes_total', backend='B', to_state='unhealthy') == 1, 'Exclusion transition')
        for _ in range(3):
            check(json.loads(fetch(port, '/request')[2])['server_id'] == 'A', 'Excluded backend received request')
        fetch(endpoints[1], '/admin/ready?value=true', method='POST')
        wait_for(lambda: value(scrape()[1], 'lb_backend_healthy', backend='B') == 1, 'Recovery metric')
        check(value(scrape()[1], 'lb_backend_state_changes_total', backend='B', to_state='healthy') == 2, 'Recovery transition')
        checks.append('OBS-03 exclusion and recovery reflected in gauges and counters')

        def traffic():
            for index in range(40):
                check(fetch(port, f'/arbitrary/user/{index}?id={index}')[0] == 404, 'Traffic response')
        with ThreadPoolExecutor(max_workers=2) as pool:
            future = pool.submit(traffic)
            while not future.done():
                _, samples = scrape()
                check(set(samples) == set(initial), 'Cardinality changed with paths')
                count = value(samples, 'lb_request_duration_seconds_count', route='proxy')
                check(value(samples, 'lb_request_duration_seconds_bucket', route='proxy', le='+Inf') == count, 'Histogram count mismatch')
                buckets = sorted((float(dict(labels)['le']), v) for (n, labels), v in samples.items() if n == 'lb_request_duration_seconds_bucket')
                check(all(a[1] <= b[1] for a, b in zip(buckets, buckets[1:])), 'Noncumulative buckets')
            future.result()
        text, final = scrape()
        (args.logs / 'metrics.txt').write_text(text)
        check(value(final, 'lb_request_duration_seconds_count', route='proxy') == 49, 'Concurrent completed counts')
        checks.append('OBS-04 concurrent scrapes and traffic preserve histogram and fixed cardinality')

        # Keep-alive idle waiting must not inflate the response histogram.
        before_sum = value(final, 'lb_request_duration_seconds_sum', route='proxy')
        persistent = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        try:
            persistent.request('GET', '/request')
            check(persistent.getresponse().read(), 'Persistent response')
            time.sleep(1)
            persistent.request('GET', '/request')
            check(persistent.getresponse().read(), 'Second persistent response')
        finally:
            persistent.close()
        after = scrape()[1]
        check(value(after, 'lb_request_duration_seconds_sum', route='proxy') - before_sum < 0.8,
              'Keep-alive idle time leaked into latency')
        before_count = value(after, 'lb_request_duration_seconds_count', route='proxy')
        reset = socket.create_connection(('127.0.0.1', port), timeout=2)
        reset.sendall(b'GET /slow?ms=1000 HTTP/1.1\r\nHost: test\r\n\r\n')
        wait_for(lambda: value(scrape()[1], 'lb_active_connections', side='upstream') >= 1 and
                 value(scrape()[1], 'lb_inflight_requests') == 1, 'Reset request in flight')
        reset.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        reset.close()
        wait_for(lambda: value(scrape()[1], 'lb_inflight_requests') == 0, 'Reset cancellation completes')
        text, final = scrape()
        check(value(final, 'lb_request_duration_seconds_count', route='proxy') == before_count,
              'Aborted response counted as completed')
        check(sum(v for (n, labels), v in final.items() if n == 'lb_backend_requests_total' and
                  ('result', 'cancelled') in labels) == 1, 'Cancellation outcome')
        (args.logs / 'metrics.txt').write_text(text)
        checks.append('OBS-05 idle time excluded and cancelled response not completed')

        limits, limits_port = start(args.proxy, ['--port', '0', '--metrics-port', '0',
            '--max-connections', '3', '--max-inflight', '2', '--max-upstream-connections', '1',
            '--backend', f'A,127.0.0.1,{endpoints[0]}'], 'limits')
        def limits_scrape():
            return parse(fetch(limits.event['metrics_port'], '/metrics')[2].decode())
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(fetch, limits_port, '/slow?ms=200')
            wait_for(lambda: value(limits_scrape(), 'lb_inflight_requests') == 1, 'Upstream cap in flight')
            check(fetch(limits_port, '/request')[0] == 503, 'Upstream admission response')
            check(value(limits_scrape(), 'lb_overload_rejections_total', reason='upstream_limit') == 1,
                  'Upstream admission metric')
            check(pending.result()[0] == 200, 'Admitted request failed')
        wait_for(lambda: value(limits_scrape(), 'lb_active_connections', side='downstream') == 0,
                 'Previous connection closes before cap test')
        held = [socket.create_connection(('127.0.0.1', limits_port), timeout=2) for _ in range(3)]
        sockets.extend(held)
        wait_for(lambda: value(limits_scrape(), 'lb_active_connections', side='downstream') == 3,
                 'Connection cap occupied')
        rejected = socket.create_connection(('127.0.0.1', limits_port), timeout=2)
        try:
            try:
                check(rejected.recv(1) == b'', 'Connection cap did not reject')
            except ConnectionResetError:
                pass
        finally:
            rejected.close()
        check(value(limits_scrape(), 'lb_overload_rejections_total', reason='connection_limit') == 1,
              'Connection admission metric')
        for connection in held:
            connection.close()
            sockets.remove(connection)
        checks.append('OBS-06 distinct upstream and connection admission reasons')

        # Eight slow monitoring clients exhaust only the separate monitoring cap.
        for _ in range(8):
            connection = socket.create_connection(('127.0.0.1', monitoring), timeout=2)
            connection.sendall(b'G')
            sockets.append(connection)
        extra = socket.create_connection(('127.0.0.1', monitoring), timeout=2)
        sockets.append(extra)
        try:
            check(extra.recv(1) == b'', 'Unbounded monitoring admission')
        except ConnectionResetError:
            pass
        check(fetch(port, '/request')[0] == 200, 'Monitoring cap affected business traffic')
        started = time.monotonic()
        proxy.stop()
        check(time.monotonic() - started < 3, 'Shutdown waited for monitoring header timeout')
        owned.remove(proxy)
        checks.append('OBS-07 bounded monitoring clients and cancellation on shutdown')
        (args.logs / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
        print(json.dumps({'checks': checks, 'series': len(final)}))
    finally:
        for connection in sockets:
            connection.close()
        for process in reversed(owned):
            process.stop()


if __name__ == '__main__':
    main()
