#!/usr/bin/env python3
"""Longer owned-process checks; a failing iteration stops, without pass retries."""
import argparse
import json
import os
import signal
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--seeds', type=int, nargs='+', default=[7, 73, 2026, 4096])
    args = parser.parse_args()
    if not 1 <= args.timeout <= 600 or not 1 <= args.rounds <= 10 or any(not 0 <= seed <= 0xffffffff for seed in args.seeds):
        parser.error('Timeout must be 1..600 seconds, rounds 1..10, and seeds uint32')
    if args.output.exists():
        parser.error('Choose a new output directory')
    args.output.mkdir(parents=True)
    manifest = {'build': json.loads(subprocess.check_output([args.build / 'load_balancer', '--version'], text=True)),
                'rounds': args.rounds, 'seeds': args.seeds, 'results': []}
    def run(name, command):
        child = subprocess.Popen(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            text, _ = child.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            # This new session contains only this invocation and its descendants.
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                text, _ = child.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                text, _ = child.communicate(timeout=10)
        code = 124 if timed_out else child.returncode
        (args.output / (name + '.log')).write_text(text)
        manifest['results'].append({'name': name, 'exit_code': code, 'timed_out': timed_out,
                                    'command': list(map(str, command))})
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(name, code, flush=True)
        return code
    for number in range(args.rounds):
        name = f'lifecycle-{number}'
        code = run(name, ['ctest', '--test-dir', str(args.build.resolve()), '-R', '^lifecycle_process$', '--output-on-failure'])
        source = args.build / 'Testing/lifecycle-process'
        if source.exists():
            shutil.copytree(source, args.output / name)
        if code:
            raise SystemExit(code)
        for seed in args.seeds:
            name = f'chaos-{number}-{seed}'
            code = run(name, [sys.executable, str(ROOT / 'tests/integration/chaos_process.py'),
                '--backend', str((args.build / 'test_backend').resolve()),
                '--proxy', str((args.build / 'load_balancer').resolve()),
                '--seed', str(seed), '--logs', str((args.output / name).resolve())])
            if code:
                raise SystemExit(code)


if __name__ == '__main__':
    main()
