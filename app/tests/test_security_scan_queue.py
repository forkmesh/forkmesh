#!/usr/bin/env python3
"""Native mirror-node security scan scheduling contracts."""

import ast
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
ENTRY = ROOT / "src" / "entry.py"
WORKFLOW = PROJECT / ".github" / "workflows" / "daily-security-scan.yml"
RUNNER_PATH = PROJECT / "tools" / "native_security_scan_runner.py"

sys.path.insert(0, str(PROJECT / "tools"))
spec = importlib.util.spec_from_file_location(
    "forkmesh_native_security_scan_runner", RUNNER_PATH)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class _Request:
    method = "GET"


def _retired_queue_handler():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "repository_security_scan_queue_handler"
    )

    def json_response(data, status=200, cache_control=None,
                      extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cacheControl": cache_control,
            "headers": extra_headers or {},
        }

    namespace = {
        "method_name": lambda request: request.method,
        "json_response": json_response,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace["repository_security_scan_queue_handler"]


def _git_repo(path):
    path.mkdir()
    subprocess.run(["git", "init", "-q", str(path)], check=True)
    subprocess.run(
        ["git", "-C", str(path), "config", "user.email", "test@example.invalid"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(path), "config", "user.name", "Test"],
        check=True,
    )
    (path / "README.md").write_text("public test fixture\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(path), "add", "README.md"], check=True)
    subprocess.run(
        ["git", "-C", str(path), "commit", "-qm", "fixture"], check=True)
    return subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()


def test_central_queue_and_scheduled_workflow_are_retired():
    response = asyncio.run(_retired_queue_handler()(None, _Request()))
    assert response["status"] == 410
    assert response["data"]["nativePath"].endswith(
        "/security-scans/lease")
    assert not WORKFLOW.exists()
    assert "workflow" not in response["data"]["error"]


def test_local_lease_prevents_duplicate_runs_and_recovers_stale_file(tmp_path):
    state = tmp_path / "state"
    state.mkdir()
    with runner.local_lease(state, now=1_000):
        with pytest.raises(runner.NativeScanError, match="local_lease_held"):
            with runner.local_lease(state, now=1_001):
                pass

    lock = state / "runner.lock"
    lock.write_text(json.dumps({"createdAt": 1}), encoding="utf-8")
    with runner.local_lease(
            state, now=runner.LOCK_STALE_SECONDS + 2):
        assert lock.exists()
    assert not lock.exists()


def test_offline_run_is_commit_pinned_redacted_bounded_and_idempotent(
        tmp_path):
    checkout = tmp_path / "checkout"
    commit = _git_repo(checkout)
    state_dir = tmp_path / "state"

    first = runner.run_once(
        checkout=checkout,
        repository="alice/widget",
        relay_url="https://forkmesh.test",
        state_dir=state_dir,
        session_token="",
        offline=True,
        now=10_000,
    )
    assert first["status"] == "offline_artifact_pending"
    assert first["commit"] == commit
    artifact = Path(first["artifact"])
    report = json.loads(artifact.read_text(encoding="utf-8"))
    assert report["repository"]["commit"] == commit
    assert report["visibility"]["redacted"] is True
    assert report["visibility"]["containsSourceExcerpts"] is False
    assert report["visibility"]["containsSecretValues"] is False
    assert first["privateDiagnosticsStored"] is False
    assert os.stat(artifact).st_mode & 0o777 == 0o600

    second = runner.run_once(
        checkout=checkout,
        repository="alice/widget",
        relay_url="https://forkmesh.test",
        state_dir=state_dir,
        session_token="",
        offline=True,
        now=10_001,
    )
    assert second["artifact"] == first["artifact"]
    assert second["attemptCount"] == first["attemptCount"]


def test_runner_backoff_is_bounded():
    delays = [runner._backoff_seconds(attempt) for attempt in range(1, 20)]
    assert delays[:4] == [60, 120, 240, 480]
    assert delays == sorted(delays)
    assert delays[-1] <= 6 * 60 * 60
