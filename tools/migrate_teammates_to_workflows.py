#!/usr/bin/env python3
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app" / "src"))

import repo_workflows  # noqa: E402
import workflow_files  # noqa: E402


def migrate(repo, remove_legacy, dry_run):
    legacy = repo / repo_workflows.LEGACY_FOLDER
    target = repo / repo_workflows.FOLDER
    moved = []
    problems = []
    for source in sorted(legacy.glob("*.json")):
        destination = target / source.name
        try:
            data = json.loads(source.read_text(encoding="utf-8"))
            if isinstance(data, dict) and isinstance(data.get("nodes"), list):
                workflow = data
            else:
                workflow = repo_workflows.teammate_to_workflow(data)
        except (OSError, ValueError) as error:
            problems.append(f"{source.name}: {error}")
            continue
        if workflow.get("id") != source.stem:
            problems.append(f"{source.name}: its id is {workflow.get('id')!r}")
            continue
        if destination.exists():
            problems.append(f"{source.name}: {destination} already exists")
            continue
        moved.append(source.name)
        if dry_run:
            continue
        target.mkdir(parents=True, exist_ok=True)
        destination.write_text(workflow_files.qt_json(workflow) + "\n",
                               encoding="utf-8")
        if remove_legacy and source.is_file():
            source.unlink()
    return moved, problems


def main():
    parser = argparse.ArgumentParser(
        description="Turn .forkmesh/teammates special agents into "
                    ".forkmesh/workflows files.")
    parser.add_argument("--repo", default=".", help="repository checkout")
    parser.add_argument("--remove-legacy", action="store_true",
                        help="remove each teammate file once its workflow "
                             "file is written")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    repo = Path(args.repo).resolve()
    moved, problems = migrate(repo, args.remove_legacy, args.dry_run)
    for name in moved:
        print(("would move " if args.dry_run else "moved ") + name)
    for problem in problems:
        print("skipped " + problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
