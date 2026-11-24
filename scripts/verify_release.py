#!/usr/bin/env python3
"""Extract and verify an owned release archive, including a real proxy process test."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/integration'))
from proxy_process import OwnedProcess
from test_backend_process import check, fetch


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('archive', type=Path)
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        with tarfile.open(args.archive) as archive:
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                check(not path.is_absolute() and '..' not in path.parts and
                      (member.isfile() or member.isdir()), 'Unsafe archive entry')
            archive.extractall(temporary, filter='data')
        roots = list(Path(temporary).iterdir())
        check(len(roots) == 1, 'Archive needs one root directory')
        stage = roots[0]
        hashes = json.loads((stage / 'SHA256SUMS.json').read_text())
        files = {str(path.relative_to(stage)) for path in stage.rglob('*') if path.is_file()}
        check(files == set(hashes) | {'SHA256SUMS.json'}, 'Archive manifest membership mismatch')
        for name, digest in hashes.items():
            check(hashlib.sha256((stage / name).read_bytes()).hexdigest() == digest, 'Checksum mismatch: ' + name)
        info = json.loads(subprocess.check_output([stage / 'bin/load_balancer', '--version'], text=True))
        check(info == json.loads((stage / 'share/load-balancer/build-info.json').read_text()), 'Packaged build info mismatch')
        subprocess.run([stage / 'bin/load_balancer', '--smoke'], check=True, capture_output=True, timeout=10)
        owned = []
        try:
            backend = OwnedProcess(str(stage / 'bin/test_backend'), ['--port', '0'], 'packaged-backend', args.logs)
            owned.append(backend)
            port = backend.ready()
            config = args.logs / 'verify.args'
            config.write_text(f'--port 0\n--metrics-port 0\n--backend A,127.0.0.1,{port}\n')
            proxy = OwnedProcess(sys.executable, [str(stage / 'bin/run-proxy.py'), '--config', str(config)],
                                 'packaged-proxy', args.logs)
            owned.append(proxy)
            proxy_port = proxy.ready()
            status, _, body = fetch(proxy_port, '/request')
            check(status == 200 and json.loads(body)['server_id'] == 'A', 'Packaged proxy forwarding failed')
            check(fetch(proxy.event['metrics_port'], '/metrics')[0] == 200, 'Packaged metrics failed')
        finally:
            for child in reversed(owned):
                child.stop()
        (args.logs / 'verification.json').write_text(json.dumps({'build': info, 'files_verified': len(hashes),
            'checks': ['checksums', 'version', 'smoke', 'argument-file launcher', 'HTTP forwarding', 'private metrics', 'clean shutdown']}, indent=2) + '\n')
        print('Release archive checksums and packaged runtime checks passed.')


if __name__ == '__main__':
    main()
