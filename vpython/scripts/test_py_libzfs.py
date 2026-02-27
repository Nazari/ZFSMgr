#!/usr/bin/env python3
"""Smoke test for py-libzfs on a host with OpenZFS."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from typing import Any


def build_result() -> dict[str, Any]:
    result: dict[str, Any] = {
        "ok": False,
        "python": sys.version.split()[0],
        "python_executable": sys.executable,
        "platform": platform.platform(),
        "libzfs_version": None,
        "pool_count": 0,
        "pools": [],
        "error": None,
    }

    try:
        import libzfs  # type: ignore
    except Exception as exc:  # pragma: no cover - runtime env dependent
        result["error"] = f"Cannot import libzfs: {exc}"
        return result

    result["libzfs_version"] = getattr(libzfs, "__version__", "unknown")

    try:
        with libzfs.ZFS() as zfs:
            pools = []
            for pool in zfs.pools:
                pools.append(pool.name)
            result["pools"] = pools
            result["pool_count"] = len(pools)
            result["ok"] = True
            return result
    except Exception as exc:  # pragma: no cover - runtime env dependent
        result["error"] = f"libzfs.ZFS() failed: {exc}"
        return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate py-libzfs setup.")
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print machine-readable JSON output.",
    )
    args = parser.parse_args()

    result = build_result()

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"Python: {result['python']} ({result['python_executable']})")
        print(f"Platform: {result['platform']}")
        print(f"libzfs version: {result['libzfs_version']}")
        print(f"Pools: {result['pool_count']}")
        if result["pools"]:
            for name in result["pools"]:
                print(f" - {name}")
        if result["error"]:
            print(f"Error: {result['error']}")
        print("RESULT: OK" if result["ok"] else "RESULT: FAIL")

    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
