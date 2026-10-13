#!/usr/bin/env python3
"""Mirror-node daily security scan runner with local/offline retention.

The runner obtains a short-lived repository+commit lease from the relay, scans
that exact checkout with ``tools/security_scan.py``, verifies HEAD did not move,
retains only public-safe artifacts, and publishes idempotently with the scoped
lease token. The normal ForkMesh session token is read from
``FORKMESH_SESSION_TOKEN`` and is never written to disk or passed to the scanner.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlparse
from urllib.request import Request, urlopen

import security_scan
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)


DAY_SECONDS = 24 * 60 * 60
LOCK_STALE_SECONDS = 2 * 60 * 60
MAX_RETAINED_COMMITS = 30
MAX_RESPONSE_BYTES = 256 * 1024
REPOSITORY_RE = re.compile(
    r"^([A-Za-z0-9._:-]{1,80})/([A-Za-z0-9._:-]{1,80})$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


class NativeScanError(RuntimeError):
    pass


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def _git_head(checkout):
    try:
        result = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise NativeScanError("checkout_unavailable") from exc
    commit = result.stdout.strip().lower()
    if not COMMIT_RE.fullmatch(commit):
        raise NativeScanError("commit_unavailable")
    return commit


def _safe_state_dir(value):
    path = Path(value).expanduser().resolve()
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass
    return path


def _atomic_json(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + str(os.getpid()))
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode()
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _load_json(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


@contextmanager
def local_lease(state_dir, now=None):
    """Single-process lease with bounded stale recovery."""
    now = int(time.time() if now is None else now)
    lock = state_dir / "runner.lock"
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        saved = _load_json(lock)
        if now - int(saved.get("createdAt") or now) <= LOCK_STALE_SECONDS:
            raise NativeScanError("local_lease_held")
        try:
            lock.unlink()
        except OSError as exc:
            raise NativeScanError("local_lease_held") from exc
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(descriptor, json.dumps({
            "createdAt": now, "pid": os.getpid(),
        }, separators=(",", ":")).encode())
        os.close(descriptor)
        descriptor = -1
        yield
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            lock.unlink()
        except FileNotFoundError:
            pass


def _relay_origin(value):
    raw = str(value or "").strip().rstrip("/")
    parsed = urlparse(raw)
    if (
        parsed.scheme.lower() != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
    ):
        raise NativeScanError("invalid_relay_origin")
    return raw


def _request_json(
    url, method, payload, authorization, *, opener=urlopen, timeout=20,
):
    body = (
        json.dumps(payload, separators=(",", ":")).encode()
        if payload is not None else None)
    headers = {
        "accept": "application/json",
        "cache-control": "no-store",
        "authorization": "Bearer " + authorization,
    }
    if body is not None:
        headers["content-type"] = "application/json"
    request = Request(url, data=body, headers=headers, method=method)
    try:
        with opener(request, timeout=timeout) as response:
            raw = response.read(MAX_RESPONSE_BYTES + 1)
            if len(raw) > MAX_RESPONSE_BYTES:
                raise NativeScanError("relay_response_too_large")
            value = json.loads(raw.decode()) if raw else {}
            return int(response.status), value
    except HTTPError as exc:
        raw = exc.read(MAX_RESPONSE_BYTES + 1)
        try:
            value = json.loads(raw.decode()) if raw else {}
        except ValueError:
            value = {}
        return int(exc.code), value
    except (OSError, URLError) as exc:
        raise NativeScanError("relay_offline") from exc


def _artifact_paths(state_dir, repository, commit):
    owner, repo = repository.split("/", 1)
    root = state_dir / "artifacts" / (owner + "__" + repo) / commit
    return {
        "root": root,
        "rich": root / "security-scan.json",
        "clipboard": root / "security-clipboard.json",
        "summary": root / "security-scan.md",
    }


def create_artifacts(checkout, repository, commit, state_dir, *, offline):
    if _git_head(checkout) != commit:
        raise NativeScanError("commit_changed_before_scan")
    rich = security_scan.build_report(
        checkout, repository_name=repository, offline=offline)
    if str(rich["repository"]["commit"]).lower() != commit:
        raise NativeScanError("scanner_commit_mismatch")
    clipboard = security_scan.clipboard_report(rich)
    security_scan.validate_public_report(rich)
    paths = _artifact_paths(state_dir, repository, commit)
    paths["root"].mkdir(mode=0o700, parents=True, exist_ok=True)
    _atomic_json(paths["rich"], rich)
    _atomic_json(paths["clipboard"], clipboard)
    summary = security_scan.markdown_summary(rich).encode()
    descriptor = os.open(
        paths["summary"], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(summary)
    if _git_head(checkout) != commit:
        raise NativeScanError("commit_changed_during_scan")
    return rich, clipboard, paths


def _retain_bounded(state_dir, repository):
    owner, repo = repository.split("/", 1)
    parent = state_dir / "artifacts" / (owner + "__" + repo)
    try:
        directories = sorted(
            (item for item in parent.iterdir() if item.is_dir()),
            key=lambda item: item.stat().st_mtime,
            reverse=True,
        )
    except OSError:
        return
    for directory in directories[MAX_RETAINED_COMMITS:]:
        for item in directory.iterdir():
            if item.is_file():
                item.unlink()
        directory.rmdir()


def _backoff_seconds(attempt):
    return min(6 * 60 * 60, 60 * (2 ** min(8, max(0, attempt - 1))))


def run_once(
    *, checkout, repository, relay_url, state_dir, session_token,
    offline=False, opener=urlopen, now=None,
):
    match = REPOSITORY_RE.fullmatch(repository)
    if not match:
        raise NativeScanError("invalid_repository")
    checkout = Path(checkout).resolve()
    state_dir = _safe_state_dir(state_dir)
    now = int(time.time() if now is None else now)
    state_path = state_dir / "status.json"
    state = _load_json(state_path)
    attempt = int(state.get("attemptCount") or 0) + 1

    with local_lease(state_dir, now=now):
        if (
            state.get("status") == "offline_artifact_pending"
            and now < int(state.get("nextAttemptAt") or 0)
        ):
            return state
        commit = _git_head(checkout)
        due = (
            state.get("lastCompletedCommit") != commit
            or now - int(state.get("lastCompletedAt") or 0) >= DAY_SECONDS)
        if not due:
            return state
        lease = None
        if not offline:
            if not session_token:
                raise NativeScanError("session_token_required")
            owner = quote(match.group(1), safe="")
            repo = quote(match.group(2), safe="")
            lease_url = (
                _relay_origin(relay_url) + "/api/repo/" + owner + "/" + repo
                + "/security-scans/lease")
            try:
                status, lease = _request_json(
                    lease_url, "POST", {"commit": commit}, session_token,
                    opener=opener)
            except NativeScanError:
                offline = True
            else:
                if status == 200 and lease.get("reason") == "not_due":
                    state.update({
                        "status": "not_due",
                        "lastCheckedAt": now,
                        "nextAttemptAt": int(lease.get("nextDueAt") or 0) // 1000,
                    })
                    _atomic_json(state_path, state)
                    return state
                if status == 409 and lease.get("error") == "lease_held":
                    state.update({
                        "status": "lease_held",
                        "lastCheckedAt": now,
                        "nextAttemptAt": now + max(
                            60, int(lease.get("retryAfterMs") or 0) // 1000),
                    })
                    _atomic_json(state_path, state)
                    return state
                if status != 201 or not lease.get("leased"):
                    raise NativeScanError("lease_rejected")
        rich, clipboard, paths = create_artifacts(
            checkout, repository, commit, state_dir, offline=offline)
        _retain_bounded(state_dir, repository)
        if offline:
            state.update({
                "status": "offline_artifact_pending",
                "commit": commit,
                "artifact": str(paths["rich"]),
                "attemptCount": attempt,
                "lastCheckedAt": now,
                "nextAttemptAt": now + _backoff_seconds(attempt),
                "privateDiagnosticsStored": False,
            })
            _atomic_json(state_path, state)
            return state
        token = str(lease.get("leaseToken") or "")
        ingest_url = str(lease.get("ingestUrl") or "")
        if (
            not re.fullmatch(r"[A-Za-z0-9_-]{43}", token)
            or not ingest_url.startswith(_relay_origin(relay_url) + "/api/")
            or str(lease.get("commit") or "") != commit
        ):
            raise NativeScanError("invalid_lease_response")
        security_scan.publish_scan_ingest(
            rich, clipboard, url=ingest_url, token=token, opener=opener)
        state = {
            "status": "completed",
            "commit": commit,
            "lastCompletedCommit": commit,
            "lastCompletedAt": now,
            "lastCheckedAt": now,
            "nextAttemptAt": now + DAY_SECONDS,
            "attemptCount": 0,
            "artifact": str(paths["rich"]),
            "publicStatus": rich["status"],
            "privateDiagnosticsStored": False,
        }
        _atomic_json(state_path, state)
        return state


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkout", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--relay-url", default="https://app.forkmesh.com")
    parser.add_argument(
        "--state-dir",
        default=str(forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT).tool_state
                    / "scans"))
    parser.add_argument(
        "--offline", action="store_true",
        help="scan and retain a redacted artifact without contacting the relay")
    args = parser.parse_args(argv)
    try:
        state = run_once(
            checkout=args.checkout,
            repository=args.repository,
            relay_url=args.relay_url,
            state_dir=args.state_dir,
            session_token=os.environ.get("FORKMESH_SESSION_TOKEN", ""),
            offline=args.offline,
        )
    except NativeScanError as exc:
        print(json.dumps({
            "ok": False,
            "status": str(exc),
            "at": utc_now(),
            "privateDiagnosticsStored": False,
        }, sort_keys=True))
        return 2
    print(json.dumps({"ok": True, **state}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
