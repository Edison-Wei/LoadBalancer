#!/usr/bin/env python3
"""Run the adjacent packaged binary with a trusted, shell-style argument file."""
import argparse
import os
from pathlib import Path
import shlex


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, default=here / 'load_balancer')
    parser.add_argument('--config', type=Path,
                        default=here.parent / 'share/load-balancer/examples/proxy.args')
    args = parser.parse_args()
    values = shlex.split(args.config.read_text(), comments=True)
    os.execv(str(args.binary.resolve()), [str(args.binary.resolve()), *values])


if __name__ == '__main__':
    main()
