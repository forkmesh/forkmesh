#!/usr/bin/env python3
"""Show whether each GitHub Actions workflow on a branch is passing."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WATCH = ROOT / "app" / "src" / "github_actions_watch.py"


def _watch():
    spec = importlib.util.spec_from_file_location("github_actions_watch", WATCH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _get(watch, path: str, token: str):
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "ForkMesh-GitHub-Actions-Status",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(watch.GITHUB_API + path, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, json.loads(response.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, None
    except (urllib.error.URLError, TimeoutError, ValueError):
        return 0, None


def collect(repo: str, branch: str, token: str = "", fetch=None) -> dict:
    watch = _watch()
    fetch = fetch or (lambda path: _get(watch, path, token))
    status, payload = fetch(watch.runs_path(repo, branch))
    if status != 200 or not isinstance(payload, dict):
        raise RuntimeError(f"GitHub answered {status} for the run list of {repo}")
    verdicts = watch.workflow_verdicts(payload.get("workflow_runs"), branch)
    workflows = []
    for path in sorted(verdicts, key=lambda key: verdicts[key]["name"].lower()):
        verdict = dict(verdicts[path])
        verdict["steps"] = []
        verdict["detail"] = ""
        if verdict["failing"]:
            status, jobs = fetch(watch.jobs_path(repo, verdict["runId"]))
            if status == 200:
                verdict["steps"] = watch.failed_steps(jobs)
                for job_id in watch.failed_job_ids(jobs):
                    status, annotations = fetch(watch.annotations_path(repo, job_id))
                    if status == 200:
                        verdict["detail"] = watch.annotation_detail(annotations)
                    if verdict["detail"]:
                        break
        workflows.append(verdict)
    return {
        "repo": repo,
        "branch": branch,
        "failing": sum(1 for item in workflows if item["failing"]),
        "workflows": workflows,
    }


def render(report: dict) -> str:
    lines = [f"GitHub Actions on {report['repo']} {report['branch']}: "
             f"{report['failing']} of {len(report['workflows'])} workflows failing"]
    for item in report["workflows"]:
        mark = "FAIL" if item["failing"] else "ok  "
        least = "" if item.get("streakComplete", True) else "at least "
        streak = f", {least}{item['streak']} runs in a row" if item.get("streak", 0) > 1 else ""
        lines.append(f"{mark}  {item['name']}  #{item['runNumber']} "
                     f"{item['sha'][:7]} {item['conclusion']}{streak}")
        if item["failing"]:
            for step in item["steps"]:
                lines.append(f"      {step}")
            if item["detail"]:
                lines.append(f"      {item['detail']}")
            lines.append(f"      {item['url']}")
    return "\n".join(lines)


def main(argv=None) -> int:
    watch = _watch()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=watch.DEFAULT_REPO)
    parser.add_argument("--branch", default=watch.DEFAULT_BRANCH)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    token = os.environ.get("GITHUB_TOKEN", "") or os.environ.get("GH_TOKEN", "")
    try:
        report = collect(args.repo, args.branch, token)
    except RuntimeError as error:
        print(f"github-actions-status: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2) if args.json else render(report))
    return 1 if report["failing"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
