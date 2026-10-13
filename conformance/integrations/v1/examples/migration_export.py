#!/usr/bin/env python3
"""Migration: write a resumable export manifest of public repositories.

Contract resource: GET /api/repositories. Re-running against the same output
directory is idempotent: unchanged repositories keep their entries, and the
summary reports what was added, updated, and removed since the last run.

    python3 migration_export.py --base-url https://app.forkmesh.com --output ./export
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex

import _client

fm = _client.load()

MANIFEST_SCHEMA = "forkmesh-export-v1"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default=fm.DEFAULT_BASE_URLS["hosted"])
    parser.add_argument("--output", required=True)
    parser.add_argument("--owner", default="", help="only export this owner")
    args = parser.parse_args(argv)

    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "forkmesh-export.json"
    previous = {}
    if manifest_path.is_file():
        prior = json.loads(manifest_path.read_text(encoding="utf-8"))
        if prior.get("schema") == MANIFEST_SCHEMA:
            previous = {item["repository"]: item for item in prior["repositories"]}

    snapshot = fm.catalog_snapshot(fm.Client(args.base_url).get_json("/api/repositories"))
    entries = []
    counts = {"added": 0, "updated": 0, "unchanged": 0}
    for subject in sorted(snapshot):
        data = snapshot[subject]["data"]
        if args.owner and data["owner"].lower() != args.owner.lower():
            continue
        if data.get("visibility", "public") != "public":
            continue
        digest = hashlib.sha256(fm.canonical_json(data)).hexdigest()
        prior = previous.get(subject)
        counts["unchanged" if prior and prior["digest"] == digest
               else "updated" if prior else "added"] += 1
        directory = "%s__%s.git" % (data["owner"], data["name"])
        entries.append({
            "repository": subject,
            "digest": digest,
            "record": data,
            "command": "git clone --mirror %s %s" % (
                shlex.quote(str(data.get("cloneUrl") or "")), shlex.quote(directory)),
        })
    removed = sorted(set(previous) - {entry["repository"] for entry in entries})
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "source": fm.redact_url(args.base_url),
        "repositories": entries,
    }
    temporary = manifest_path.with_name(manifest_path.name + ".tmp")
    temporary.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n",
                         encoding="utf-8")
    os.replace(temporary, manifest_path)
    fm.emit(dict(counts, schema=fm.CLI_SCHEMA, command="example.migration",
                 manifest=str(manifest_path), removed=removed,
                 repositories=len(entries)))
    return fm.EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
