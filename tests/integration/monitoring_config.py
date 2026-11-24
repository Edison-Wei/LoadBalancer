#!/usr/bin/env python3
"""Validate rendered Compose networking without requiring daemon access."""
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    result = subprocess.run(['docker', 'compose', 'config', '--format', 'json'],
                            cwd=root, check=True, capture_output=True, text=True)
    config = json.loads(result.stdout)
    networks = config['networks']
    services = config['services']
    expected = {'load-balancer': 8080, 'prometheus': 9090, 'grafana': 3000}
    for name, service in services.items():
        published = service.get('ports', [])
        attachments = service.get('networks', {})
        if name in expected:
            assert len(published) == 1, (name, published)
            port = published[0]
            assert port['host_ip'] == '127.0.0.1', (name, port)
            assert int(port['published']) == expected[name], (name, port)
            assert int(port['target']) == expected[name], (name, port)
            assert any(not networks[network].get('internal', False)
                       for network in attachments), f'{name}: published ports on internal-only networks'
        else:
            assert not published, f'{name}: origin must not publish a port'
            assert all(networks[network].get('internal', False)
                       for network in attachments), f'{name}: origin must remain internal-only'
        assert 'monitoring' in attachments, name
        assert service.get('network_mode') != 'host', name
        assert all(int(port['target']) != 9091 for port in published), name
    assert networks['monitoring']['internal'] is True
    print('Compose networking passed: loopback ports have a normal bridge; origins and metrics remain unpublished.')


if __name__ == '__main__':
    main()
