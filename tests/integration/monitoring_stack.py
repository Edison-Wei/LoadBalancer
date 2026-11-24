#!/usr/bin/env python3
"""Verify an already-running Compose stack using real Prometheus/Grafana APIs.

This check sends business traffic but does not start, stop or signal services.
Run after: docker compose up --build --wait
"""
import argparse
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request


def get(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.load(response)


def eventually(action, description, timeout=60):
    deadline = time.monotonic() + timeout
    failure = None
    while time.monotonic() < deadline:
        try:
            result = action()
            if result:
                return result
        except (OSError, ValueError, KeyError, AssertionError) as error:
            failure = error
        time.sleep(0.5)
    raise AssertionError(
        f'{description}: {failure}. Check `docker compose ps` and '
        '`docker compose logs --tail 50 prometheus grafana`. '
        'Healthy containers do not prove host port publication; services with '
        'published ports need a non-internal network attachment.'
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--prometheus', default='http://127.0.0.1:9090')
    parser.add_argument('--grafana', default='http://127.0.0.1:3000')
    parser.add_argument('--proxy', default='http://127.0.0.1:8080')
    parser.add_argument('--logs', type=Path, default=Path('build/monitoring-evidence'))
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)

    def query(expression):
        result = get(args.prometheus + '/api/v1/query?' + urllib.parse.urlencode({'query': expression}))
        assert result['status'] == 'success', result
        return result['data']['result']

    def reachable(url):
        with urllib.request.urlopen(url, timeout=5) as response:
            response.read()
            return response.status == 200

    # Report failed host publication before retrying the scrape query. Docker's
    # container-level health check cannot verify host-to-container reachability.
    eventually(lambda: reachable(args.prometheus + '/-/ready'),
               'Prometheus host port 9090 reachable')
    eventually(lambda: reachable(args.grafana + '/api/health'),
               'Grafana host port 3000 reachable')

    up = eventually(lambda: [row for row in query('up{job="cpp-load-balancer"}') if float(row['value'][1]) == 1], 'Prometheus target up=1')
    identities = [get(args.proxy + '/request')['server_id'] for _ in range(60)]
    assert set(identities) == {'A', 'B', 'C'}, identities
    rps = eventually(lambda: [row for row in query('sum(rate(lb_requests_total{job="cpp-load-balancer"}[1m]))') if float(row['value'][1]) > 0], 'Prometheus records traffic rate')
    health = query('lb_backend_healthy{job="cpp-load-balancer"}')
    assert len(health) == 3 and all(float(row['value'][1]) == 1 for row in health), health
    dashboard = eventually(lambda: get(args.grafana + '/api/dashboards/uid/load-balancer'), 'Provisioned Grafana dashboard')
    datasource = get(args.grafana + '/api/datasources/uid/prometheus')
    assert datasource['url'] == 'http://prometheus:9090', datasource
    panels = dashboard['dashboard']['panels']
    assert len(panels) == 12, len(panels)
    results = {}
    for panel in panels:
        for target in panel['targets']:
            results[panel['title'] + '/' + target['refId']] = query(target['expr'])
    evidence = {'up': up, 'rps': rps, 'health': health, 'identities': identities,
                'dashboard_uid': dashboard['dashboard']['uid'], 'panel_queries': results}
    (args.logs / 'checks.json').write_text(json.dumps(evidence, indent=2) + '\n')
    (args.logs / 'dashboard.json').write_text(json.dumps(dashboard, indent=2) + '\n')
    print('Verified Prometheus up=1, recorded traffic, three backend gauges, and 12 provisioned Grafana panels.')


if __name__ == '__main__':
    main()
