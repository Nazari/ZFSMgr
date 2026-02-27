#!/usr/bin/env python3
"""Use OpenZFS Python bindings (pyzfs/libzfs_core) in a simple way."""

from __future__ import annotations

import json
import sys
from typing import Any


def main() -> int:
    result: dict[str, Any] = {
        "ok": False,
        "module": "libzfs_core",
        "python": sys.version.split()[0],
        "operation": "lzc_exists('tank')",
        "value": None,
        "error": None,
        "hint": None,
    }

    try:
        import libzfs_core
    except Exception as exc:
        result["error"] = f"Cannot import libzfs_core: {exc}"
        print(json.dumps(result, indent=2))
        return 1

    try:
        result["value"] = bool(libzfs_core.lzc_exists("tank"))
        result["ok"] = True
    except Exception as exc:
        result["error"] = str(exc)
        result["hint"] = "Try again as root: sudo .venv/bin/python scripts/use_pylibzfs.py"

    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
