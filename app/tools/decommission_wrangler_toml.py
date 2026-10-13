#!/usr/bin/env python3

import argparse
import re
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REMOVED_BLOCKS = ("[[routes]]", "[[queues.consumers]]")
PINNED_VARS = (("FORKMESH_DECOMMISSIONED", "1"), ("FORKMESH_READ_ONLY", "1"))
_HEADER_RE = re.compile(r"^\s*\[")


def decommissioned_toml(text):
    out = []
    skipping = False
    in_triggers = False
    in_vars = False
    seen_table = False
    for line in text.splitlines():
        stripped = line.strip()
        if _HEADER_RE.match(line):
            if not seen_table:
                out.append("workers_dev = false")
                out.append("preview_urls = false")
                seen_table = True
            skipping = stripped in REMOVED_BLOCKS
            in_triggers = stripped == "[triggers]"
            in_vars = stripped == "[vars]"
            if skipping:
                continue
            out.append(line)
            if in_vars:
                for name, value in PINNED_VARS:
                    out.append('%s = "%s"' % (name, value))
            continue
        if skipping:
            continue
        if in_triggers and re.match(r"^\s*crons\s*=", line):
            out.append("crons = []")
            continue
        if in_vars and re.match(
                r"^\s*(%s)\s*=" % "|".join(n for n, _ in PINNED_VARS), line):
            continue
        if not seen_table and re.match(
                r"^\s*(workers_dev|preview_urls)\s*=", line):
            continue
        out.append(line)
    result = "\n".join(out) + "\n"
    _check(tomllib.loads(text), tomllib.loads(result))
    return result


def _check(original, variant):
    if variant.get("routes") or variant.get("route"):
        raise ValueError("decommissioned config still has routes")
    if variant.get("triggers", {}).get("crons") != []:
        raise ValueError("decommissioned config still has cron triggers")
    if variant.get("queues", {}).get("consumers"):
        raise ValueError("decommissioned config still consumes a queue")
    if variant.get("workers_dev") is not False:
        raise ValueError("decommissioned config would enable workers.dev")
    for key in ("name", "main", "d1_databases", "kv_namespaces",
                "durable_objects", "migrations", "compatibility_date",
                "compatibility_flags", "assets", "ai"):
        if original.get(key) != variant.get(key):
            raise ValueError("decommissioned config changed %s" % key)
    if original.get("queues", {}).get("producers") != variant.get(
            "queues", {}).get("producers"):
        raise ValueError("decommissioned config changed queue producers")
    variables = variant.get("vars", {})
    for name, value in PINNED_VARS:
        if variables.get(name) != value:
            raise ValueError("decommissioned config lacks %s" % name)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Write the wrangler config for the decommissioned App Worker deploy.")
    parser.add_argument("--source", default=str(ROOT / "wrangler.toml"))
    parser.add_argument("-o", "--output", default="-")
    args = parser.parse_args(argv)
    text = decommissioned_toml(Path(args.source).read_text(encoding="utf-8"))
    if args.output == "-":
        sys.stdout.write(text)
    else:
        Path(args.output).write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
