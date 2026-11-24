#!/usr/bin/env python3
"""Create a Runtime-component archive with version/config/provenance/checksums."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def output(command, **kwargs):
    return subprocess.check_output(command, text=True, **kwargs).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-clean', action='store_true')
    args = parser.parse_args()
    cache = (args.build / 'CMakeCache.txt').read_text()
    if not re.search(r'^CMAKE_BUILD_TYPE:[^=]+=Release$', cache, re.M) or not re.search(r'^LB_SANITIZER:[^=]+=none$', cache, re.M):
        parser.error('Only unsanitized Release builds can be packaged')
    revision = output(['git', 'rev-parse', 'HEAD'], cwd=ROOT)
    status = output(['git', 'status', '--porcelain'], cwd=ROOT)
    if args.require_clean and status:
        parser.error('A clean checkout is required; commit all intended source/config/docs first')
    info = json.loads(output([str((args.build / 'load_balancer').resolve()), '--version']))
    configured = json.loads((args.build / 'build-info.json').read_text())
    if info != configured or info['revision'] != revision:
        parser.error('Binary/configure provenance is stale; configure and rebuild before packaging')
    if args.require_clean and info['dirty'] is not False:
        parser.error('Binary was configured from a dirty/unknown tree; configure from the clean commit')
    if args.output.exists():
        parser.error('Choose a new output directory')
    args.output.mkdir(parents=True)
    label = f'load-balancer-{info["version"]}-{revision[:12]}-{info["system"]}-{info["architecture"]}'
    release_eligible = not status and info['dirty'] is False
    if not release_eligible:
        label += '-development'
    with tempfile.TemporaryDirectory(dir=args.output) as temporary:
        stage = Path(temporary) / label
        subprocess.run(['cmake', '--install', str(args.build), '--component', 'Runtime',
                        '--prefix', str(stage)], check=True)
        binary = stage / 'bin/load_balancer'
        dependency_command = ['otool', '-L', str(binary)] if platform.system() == 'Darwin' else ['ldd', str(binary)]
        links = subprocess.run(dependency_command, text=True, capture_output=True)
        provenance = {'build': info, 'source_revision': revision, 'source_dirty': bool(status),
                      'release_eligible': release_eligible,
                      'cmake_version': output(['cmake', '--version']).splitlines()[0],
                      'cache_flags': [line for line in cache.splitlines() if line.startswith((
                          'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:', 'LB_FORCE_MUTEX_SHARED_PTR:'))],
                      'runtime_dependencies': {'command': dependency_command[0], 'stdout': links.stdout,
                                               'stderr': links.stderr, 'exit_code': links.returncode}}
        (stage / 'share/load-balancer/provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        hashes = {str(path.relative_to(stage)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sorted(stage.rglob('*')) if path.is_file()}
        (stage / 'SHA256SUMS.json').write_text(json.dumps(hashes, indent=2) + '\n')
        archive = Path(shutil.make_archive(str(args.output / label), 'gztar', root_dir=temporary, base_dir=label))
    (args.output / 'archive.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__':
    main()
