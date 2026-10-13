#!/usr/bin/env python3
"""Inspect a D1 SQLite backup and private replicas without decryption keys."""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app" / "src"))

from private_surface_audit import (  # noqa: E402
    MAX_DEFAULT_ROWS,
    audit_database,
    audit_replica_paths,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="SQLite backup restored from a D1 export")
    parser.add_argument(
        "--replica-root", action="append", default=[], type=Path,
        help="directory containing .fm-private ciphertext (repeatable)",
    )
    parser.add_argument("--max-rows", type=int, default=MAX_DEFAULT_ROWS)
    parser.add_argument("--output", type=Path, help="write the JSON report to this path")
    return parser


def main() -> int:
    args = _parser().parse_args()
    if args.max_rows < 1 or not args.database.is_file():
        print("private-surface audit: invalid input", file=sys.stderr)
        return 2
    try:
        uri = args.database.resolve().as_uri() + "?mode=ro&immutable=1"
        connection = sqlite3.connect(uri, uri=True)
        try:
            database = audit_database(connection, max_rows=args.max_rows)
        finally:
            connection.close()
        replica_files = []
        for root in args.replica_root:
            if not root.is_dir():
                print("private-surface audit: invalid replica root", file=sys.stderr)
                return 2
            replica_files.extend(root.rglob("*.fm-private"))
        replicas = audit_replica_paths(replica_files)
    except (OSError, sqlite3.Error):
        print("private-surface audit: inspection failed", file=sys.stderr)
        return 2
    report = {
        **database,
        "replicas": replicas,
    }
    report["ok"] = bool(database["ok"] and not replicas["findings"])
    encoded = json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n"
    try:
        if args.output:
            args.output.write_text(encoded, encoding="utf-8")
        else:
            sys.stdout.write(encoded)
    except OSError:
        print("private-surface audit: could not write report", file=sys.stderr)
        return 2
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
