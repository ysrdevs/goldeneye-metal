#!/usr/bin/env python3
"""Execute a command as the leader of a new Unix process group."""

from __future__ import annotations

import os
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: process_group_exec.py COMMAND [ARG ...]", file=sys.stderr)
        return 2
    try:
        os.setsid()
        os.execvpe(sys.argv[1], sys.argv[1:], os.environ)
    except OSError as error:
        print(f"process-group-exec: {error}", file=sys.stderr)
        return 126


if __name__ == "__main__":
    raise SystemExit(main())
