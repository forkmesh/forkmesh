#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load_changelog_tool():
    spec = importlib.util.spec_from_file_location("changelog", ROOT / "tools" / "changelog.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bundle(source: Path, version: str, build: str) -> dict:
    tool = load_changelog_tool()
    try:
        return tool.export(tool.load(source), "ios")
    except (OSError, ValueError) as exc:
        print(f"No changelog at {source} ({exc}): {version} ({build}) shows its version with no notes.")
        return {"schema": tool.APP_SCHEMA, "entries": []}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Bundle the ForkMesh changelog into the iOS app.")
    parser.add_argument("--changelog", type=Path, default=ROOT / "changelog" / "changelog.json")
    parser.add_argument("--version", default="")
    parser.add_argument("--build", default="")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    result = bundle(args.changelog, args.version, args.build)
    Path(args.out).write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n")
    print(f"Changelog for {args.version} ({args.build}): {len(result['entries'])} entries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
