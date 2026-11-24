#!/usr/bin/env python3
"""Pinned clang-format check/fix for owned C++ only, including untracked source."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--clang-format', default='clang-format-18')
    parser.add_argument('--fix', action='store_true')
    args = parser.parse_args()
    version = subprocess.check_output([args.clang_format, '--version'], text=True)
    if 'version 18.1.8' not in version:
        parser.error('Use clang-format 18.1.8 to match CI')
    files = sorted(path for directory in ['include', 'src', 'apps', 'tests/unit']
                   for path in (ROOT / directory).rglob('*') if path.suffix in {'.cpp', '.hpp', '.h'})
    subprocess.run([args.clang_format, '-i' if args.fix else '--dry-run',
                    *([] if args.fix else ['--Werror']), *map(str, files)], check=True, cwd=ROOT)
    print(f'{len(files)} owned C++ files: formatting {"applied" if args.fix else "passed"}.')


if __name__ == '__main__':
    main()
