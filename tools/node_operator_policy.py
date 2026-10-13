#!/usr/bin/env python3
"""Node-local operator policy: what one node stores, serves, and removes.

A ForkMesh node operator decides for their own machine which repositories it
accepts, how large they may grow, which content it refuses, and how it answers
an abuse report or a legal request. Nothing here is a network-wide rule: a
repository this node refuses or quarantines keeps being served by every other
mirror that still carries it, and the gateway reports it as unavailable here so
the router sends readers elsewhere.

The policy file is data only. Enforcement happens at three points:

* ``pre-receive`` runs as the gateway-owned Git hook. Git has already staged
  the incoming objects in its object quarantine, so a refused push is
  discarded by Git itself and the repository's existing objects and refs are
  never touched. The same hook enforces protected branches: deletion, force
  updates, stale expected old values, and commits not signed by an allowed
  SSH signer are refused, and admitted updates to protected refs are audited.
* ``enforce`` scans repositories already stored on the node (for example after
  an upstream fetch or a tightened policy) and quarantines violators: they stop
  being served and accepting pushes, but their bytes stay for appeal/restore.
* The gateway consults the quarantine markers on every repository lookup.

The optional ``secrets`` section runs ``secret_gate.py`` (shipped beside this
module) at both points: a push that adds a blocked probable credential is
refused before the node stores it, and ``enforce`` reports credentials that
arrived by sync with containment, rotation, notification, and residual-risk
steps.

Every state change is appended to a hash-chained audit log, destructive or
availability-reducing changes require a preview and ``--confirm``, and applied
policies are archived so they can be rolled back. Evidence records hold only
metadata (object ids, paths, sizes, rules) -- never the refused content.
"""

from __future__ import annotations

import argparse
import copy
import fcntl
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time
from typing import Any, Iterable, Mapping


SCHEMA_VERSION = 1
MAX_POLICY_BYTES = 256 * 1024
MAX_RECORDED_FINDINGS = 50
GIT_TIMEOUT_SECONDS = 600

NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
CASE_ID_RE = re.compile(r"^case-[0-9]{14}-[0-9a-f]{8}$")
ZERO_OID_RE = re.compile(r"^0+$")
REF_PATTERN_RE = re.compile(r"^refs/[A-Za-z0-9._/*?\[\]!+-]{1,240}$")

# Protected branches. A rule names a ref glob (a bare name means
# refs/heads/<name>) and, optionally, the repositories it covers. Deletion and
# force updates are refused by default; signed commits are opt-in because they
# need an allowed signers file.
MAX_BRANCH_RULES = 200
BRANCH_RULE_FIELDS = (
    "pattern", "repositories", "blockDeletion", "blockForcePush",
    "requireSignedCommits",
)
BRANCH_RULE_DEFAULTS = (
    ("blockDeletion", True),
    ("blockForcePush", True),
    ("requireSignedCommits", False),
)
MAX_ALLOWED_SIGNERS_BYTES = 256 * 1024
MAX_SIGNATURE_COMMITS = 10000
# git log %G? status letters; only "G" (good signature from an allowed signer)
# is admitted.
SIGNATURE_STATES = {
    "N": "is not signed",
    "U": "is signed by a key that is not in this node's allowed signers",
    "B": "has a bad signature",
    "E": "has a signature this node cannot check (only SSH signatures are "
         "verified)",
    "X": "has an expired signature",
    "Y": "is signed by an expired key",
    "R": "is signed by a revoked key",
}
# Copy-ready recovery steps printed under a refused push. {branch} is the short
# branch name; the pusher's remote is assumed to be called origin.
RECOVERY_STEPS: dict[str, list[str]] = {
    "stale-ref": [
        "# someone else updated {branch} first; replay your work on theirs",
        "git fetch origin",
        "git rebase origin/{branch}",
        "git push origin HEAD:{branch}",
    ],
    "protected-branch-force-update": [
        "# {branch} is protected: keep its history and add to it",
        "git fetch origin",
        "git rebase origin/{branch}",
        "git push origin HEAD:{branch}",
        "# or publish the rewritten history on its own branch",
        "git push origin HEAD:refs/heads/rewrite/{branch}",
    ],
    "protected-branch-deletion": [
        "# {branch} is protected and was left unchanged; nothing was lost",
        "# only the node operator can lift the rule in branches.rules",
    ],
    "commit-signature": [
        "# sign every new commit with an SSH key the node operator trusts",
        "git config gpg.format ssh",
        "git config user.signingkey ~/.ssh/id_ed25519.pub",
        "git rebase --exec 'git commit --amend --no-edit -S' origin/{branch}",
        "git push origin HEAD:{branch}",
    ],
    "signature-scan-limit": [
        "# push the history in smaller steps",
        "git push origin <older-commit>:{branch}",
    ],
    "secret": [
        "# this node discarded the push; nothing was stored here",
        "# drop the value from every commit that adds it, then push again",
        "git rebase -i origin/{branch}",
        "git push origin HEAD:{branch}",
        "# if you pushed these commits anywhere else, rotate the credential now",
        "# fake test data: use an allowlisted path, the ignore marker, or a",
        "# signed override (tools/secret_gate.py sign-override)",
    ],
}

GiB = 1024 ** 3
MiB = 1024 ** 2

# Defaults are deliberately conservative for the machine they describe. A home
# node has a small disk and an owner who is not staffed to review abuse, so it
# accepts less and never stores other people's encrypted private replicas. A
# commercial host is expected to set its own figures; its defaults are still
# finite so an unedited profile cannot fill a disk.
PROFILES: dict[str, dict[str, Any]] = {
    "home": {
        "limits": {
            "maxRepositoryBytes": 1 * GiB,
            "maxBlobBytes": 50 * MiB,
            "maxPushBytes": 512 * MiB,
            "maxRepositories": 50,
            "storageBudgetBytes": 20 * GiB,
        },
        "seeding": {"privateReplicas": False},
        "violationAction": "quarantine",
        "retention": {"evidenceDays": 90},
    },
    "community": {
        "limits": {
            "maxRepositoryBytes": 4 * GiB,
            "maxBlobBytes": 100 * MiB,
            "maxPushBytes": 2 * GiB,
            "maxRepositories": 1000,
            "storageBudgetBytes": 500 * GiB,
        },
        "seeding": {"privateReplicas": False},
        "violationAction": "quarantine",
        "retention": {"evidenceDays": 180},
    },
    "commercial": {
        "limits": {
            "maxRepositoryBytes": 20 * GiB,
            "maxBlobBytes": 400 * MiB,
            "maxPushBytes": 8 * GiB,
            "maxRepositories": 10000,
            "storageBudgetBytes": 5000 * GiB,
        },
        "seeding": {"privateReplicas": True},
        "violationAction": "quarantine",
        "retention": {"evidenceDays": 365},
    },
}

LIMIT_FIELDS = (
    "maxRepositoryBytes",
    "maxBlobBytes",
    "maxPushBytes",
    "maxRepositories",
    "storageBudgetBytes",
)
VIOLATION_ACTIONS = ("quarantine", "report")
REPORT_CATEGORIES = (
    "copyright", "malware", "illegal", "privacy", "spam", "other")
CASE_DECISIONS = ("uphold", "reverse", "dismiss")

# Short operator checklists attached to every case. They are process reminders,
# not legal advice; docs/operations/node-operator-policy.md links the sources.
RUNBOOKS: dict[str, list[str]] = {
    "us": [
        "Copyright: confirm the notice names the work, the location, contact "
        "details, a good-faith statement and a signature (17 U.S.C. 512(c)(3)).",
        "Quarantine the repository promptly and notify the owner.",
        "A valid counter-notice allows restore after 10-14 business days "
        "unless the claimant reports filing suit; record both dates.",
        "Child sexual abuse material: do not retain or copy content; report "
        "to NCMEC CyberTipline and preserve only what the report requires.",
    ],
    "eu": [
        "Record the notice and acknowledge receipt (DSA Art. 16).",
        "Send the owner a statement of reasons for any restriction "
        "(DSA Art. 17).",
        "Offer the owner an appeal and decide it without undue delay "
        "(DSA Art. 20); keep the decision in the case history.",
        "Illegal content threatening life or safety: notify the competent "
        "authority (DSA Art. 18).",
    ],
    "uk": [
        "Assess the report against the illegal-content duties of the Online "
        "Safety Act 2023 and record the judgement.",
        "Quarantine promptly where content is judged illegal; give the owner "
        "a route to complain.",
        "Child sexual abuse material: do not retain content; report to the "
        "IWF / NCA as required.",
    ],
    "other": [
        "Record who asked, under which law, and what was requested.",
        "Quarantine rather than delete until the request is verified.",
        "Seek local legal advice before permanent removal or disclosure.",
    ],
}


class PolicyError(ValueError):
    """A policy file, state directory, or command argument is invalid."""


def secret_gate() -> Any:
    """The shared secret scanner, loaded from beside this file.

    The hook runs as ``python3 -I``, which leaves this directory off
    ``sys.path``, so the module is loaded by path. A policy that enables
    ``secrets`` without the module fails closed.
    """
    module = sys.modules.get("secret_gate")
    if module is not None:
        return module
    path = Path(__file__).resolve().parent / "secret_gate.py"
    if not path.is_file():
        raise PolicyError(
            "secrets requires secret_gate.py beside node_operator_policy.py")
    import importlib.util

    spec = importlib.util.spec_from_file_location("secret_gate", path)
    if spec is None or spec.loader is None:
        raise PolicyError("secret_gate.py cannot be loaded")
    module = importlib.util.module_from_spec(spec)
    sys.modules["secret_gate"] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop("secret_gate", None)
        raise
    return module


# --------------------------------------------------------------------------
# Policy loading


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise PolicyError("policy contains a duplicate key: " + key)
        result[key] = value
    return result


def _expect_keys(value: Any, allowed: Iterable[str], label: str) -> dict:
    if not isinstance(value, dict):
        raise PolicyError(label + " must be an object")
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise PolicyError(label + " has unknown fields: " + ", ".join(unknown))
    return value


def _positive_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise PolicyError(label + " must be a positive integer")
    return value


def _string_list(value: Any, label: str, maximum: int = 1000) -> list[str]:
    if not isinstance(value, list) or len(value) > maximum:
        raise PolicyError(label + " must be an array of strings")
    if not all(isinstance(item, str) and item.strip() for item in value):
        raise PolicyError(label + " must be an array of strings")
    return [item.strip() for item in value]


def _repository_selector(value: str, label: str) -> str:
    selector = value.strip()
    owner, _, name = selector.partition("/")
    if not NODE_RE.fullmatch(owner.lower()) or (name and not REPO_RE.fullmatch(name)):
        raise PolicyError(label + " entries must be owner or owner/repository")
    return owner.lower() + ("/" + name.lower() if name else "")


def normalize_policy(raw: Mapping[str, Any]) -> dict[str, Any]:
    """Validate a policy document and merge it over its profile defaults."""
    _expect_keys(
        raw,
        (
            "schemaVersion", "profile", "limits", "content", "repositories",
            "seeding", "violationAction", "retention", "jurisdiction",
            "branches", "secrets",
        ),
        "policy",
    )
    if raw.get("schemaVersion") != SCHEMA_VERSION:
        raise PolicyError("policy schemaVersion must be 1")
    profile = raw.get("profile")
    if profile not in PROFILES:
        raise PolicyError("profile must be one of: " + ", ".join(PROFILES))
    policy = copy.deepcopy(PROFILES[profile])
    policy["schemaVersion"] = SCHEMA_VERSION
    policy["profile"] = profile

    limits = _expect_keys(raw.get("limits", {}), LIMIT_FIELDS, "limits")
    for field in LIMIT_FIELDS:
        if field in limits:
            policy["limits"][field] = _positive_int(
                limits[field], "limits." + field)

    content = _expect_keys(
        raw.get("content", {}),
        ("blockedExtensions", "blockedPathPatterns", "blockedObjectIds"),
        "content",
    )
    extensions = [
        item.lower() for item in _string_list(
            content.get("blockedExtensions", []), "blockedExtensions")
    ]
    if any(not item.startswith(".") or "/" in item for item in extensions):
        raise PolicyError("blockedExtensions entries must start with '.'")
    patterns = _string_list(
        content.get("blockedPathPatterns", []), "blockedPathPatterns")
    object_ids = [
        item.lower() for item in _string_list(
            content.get("blockedObjectIds", []), "blockedObjectIds", 100000)
    ]
    if not all(OBJECT_ID_RE.fullmatch(item) for item in object_ids):
        raise PolicyError("blockedObjectIds must be full hex object ids")
    policy["content"] = {
        "blockedExtensions": sorted(set(extensions)),
        "blockedPathPatterns": sorted(set(patterns)),
        "blockedObjectIds": sorted(set(object_ids)),
    }

    repositories = _expect_keys(
        raw.get("repositories", {}), ("allow", "block"), "repositories")
    policy["repositories"] = {
        key: sorted({
            _repository_selector(item, "repositories." + key)
            for item in _string_list(
                repositories.get(key, []), "repositories." + key)
        })
        for key in ("allow", "block")
    }

    seeding = _expect_keys(
        raw.get("seeding", {}), ("privateReplicas",), "seeding")
    if "privateReplicas" in seeding:
        if not isinstance(seeding["privateReplicas"], bool):
            raise PolicyError("seeding.privateReplicas must be a boolean")
        policy["seeding"]["privateReplicas"] = seeding["privateReplicas"]

    if "violationAction" in raw:
        if raw["violationAction"] not in VIOLATION_ACTIONS:
            raise PolicyError(
                "violationAction must be one of: " + ", ".join(VIOLATION_ACTIONS))
        policy["violationAction"] = raw["violationAction"]

    retention = _expect_keys(
        raw.get("retention", {}), ("evidenceDays",), "retention")
    if "evidenceDays" in retention:
        days = _positive_int(retention["evidenceDays"], "retention.evidenceDays")
        if days > 3650:
            raise PolicyError("retention.evidenceDays must be at most 3650")
        policy["retention"]["evidenceDays"] = days

    jurisdiction = raw.get("jurisdiction", "other")
    if jurisdiction not in RUNBOOKS:
        raise PolicyError(
            "jurisdiction must be one of: " + ", ".join(RUNBOOKS))
    policy["jurisdiction"] = jurisdiction
    policy["branches"] = _normalize_branches(raw.get("branches", {}))
    if "secrets" in raw:
        # Only present when configured, so existing policies keep their digest.
        gate = secret_gate()
        try:
            policy["secrets"] = gate.normalize_policy(raw["secrets"])
        except gate.GateError as exc:
            raise PolicyError("secrets: %s" % exc)
    return policy


def _normalize_branches(raw: Any) -> dict[str, Any]:
    branches = _expect_keys(
        raw, ("rules", "allowedSignersPath"), "branches")
    raw_rules = branches.get("rules", [])
    if not isinstance(raw_rules, list) or len(raw_rules) > MAX_BRANCH_RULES:
        raise PolicyError(
            "branches.rules must be an array of at most %d rules"
            % MAX_BRANCH_RULES)
    rules = []
    for index, item in enumerate(raw_rules):
        label = "branches.rules[%d]" % index
        rule = _expect_keys(item, BRANCH_RULE_FIELDS, label)
        pattern = rule.get("pattern")
        if not isinstance(pattern, str):
            pattern = ""
        pattern = pattern.strip()
        if not pattern.startswith("refs/"):
            pattern = "refs/heads/" + pattern
        if (
            not REF_PATTERN_RE.fullmatch(pattern)
            or pattern.endswith("/")
            or ".." in pattern
            or "//" in pattern
        ):
            raise PolicyError(
                label + ".pattern must be a branch name, glob, or refs/ "
                "pattern")
        normalized: dict[str, Any] = {"pattern": pattern}
        for field, default in BRANCH_RULE_DEFAULTS:
            value = rule.get(field, default)
            if not isinstance(value, bool):
                raise PolicyError(label + "." + field + " must be a boolean")
            normalized[field] = value
        normalized["repositories"] = sorted({
            _repository_selector(selector, label + ".repositories")
            for selector in _string_list(
                rule.get("repositories", []), label + ".repositories")
        })
        rules.append(normalized)
    signers = branches.get("allowedSignersPath", "")
    if (
        not isinstance(signers, str)
        or "\x00" in signers
        or (signers and not Path(signers).is_absolute())
    ):
        raise PolicyError("branches.allowedSignersPath must be an absolute path")
    if not signers and any(rule["requireSignedCommits"] for rule in rules):
        raise PolicyError(
            "branches.allowedSignersPath is required when a rule sets "
            "requireSignedCommits")
    return {"rules": rules, "allowedSignersPath": signers}


def policy_sha256(policy: Mapping[str, Any]) -> str:
    return hashlib.sha256(_canonical_json(policy)).hexdigest()


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def _read_owner_file(path: Path, label: str, maximum: int) -> bytes:
    if not path.is_absolute():
        raise PolicyError(label + " path must be absolute")
    try:
        info = path.lstat()
    except OSError as exc:
        raise PolicyError(label + " is missing") from exc
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_size > maximum
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise PolicyError(
            label + " must be a small regular file writable only by its owner")
    return path.read_bytes()


def parse_policy_bytes(raw: bytes) -> dict[str, Any]:
    try:
        document = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PolicyError("policy is not valid JSON") from exc
    return normalize_policy(document)


def load_policy(path: Path) -> dict[str, Any]:
    return parse_policy_bytes(
        _read_owner_file(path, "policy", MAX_POLICY_BYTES))


class PolicyHandle:
    """Reload a policy file only when its stat identity changes.

    The gateway consults the policy on hot paths (private replica reads,
    repository creation); a stat per call is cheap while a parse is not.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._key: tuple[int, int, int] | None = None
        self._policy: dict[str, Any] | None = None

    def get(self) -> dict[str, Any]:
        info = self.path.stat()
        key = (info.st_ino, info.st_size, info.st_mtime_ns)
        if self._policy is None or key != self._key:
            self._policy = load_policy(self.path)
            self._key = key
        return self._policy


# --------------------------------------------------------------------------
# State directory: audit log, quarantine markers, evidence, cases, history


def _require_state_directory(path: Path) -> Path:
    if not path.is_absolute():
        raise PolicyError("state directory must be absolute")
    try:
        info = path.lstat()
    except OSError as exc:
        raise PolicyError("state directory is missing") from exc
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & (stat.S_IRWXG | stat.S_IRWXO)
    ):
        raise PolicyError(
            "state directory must be a real directory accessible only to its "
            "owner")
    return path


def _private_directory(path: Path) -> Path:
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        raise PolicyError("state path is not a real directory")
    return path


def _atomic_write(path: Path, data: bytes, mode: int = 0o600) -> None:
    _private_directory(path.parent)
    temporary = path.with_name(
        "." + path.name + "." + secrets.token_hex(6) + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, mode)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def _write_json(path: Path, value: Any) -> None:
    _atomic_write(
        path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode())


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _iso(now: float) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))


def _check_identity(owner: str, name: str) -> tuple[str, str]:
    owner = owner.strip().lower()
    name = name.strip()
    if not NODE_RE.fullmatch(owner) or not REPO_RE.fullmatch(name):
        raise PolicyError("owner or repository name is invalid")
    if name in (".", ".."):
        raise PolicyError("owner or repository name is invalid")
    return owner, name


def audit(
    state: Path,
    action: str,
    subject: str,
    detail: Mapping[str, Any],
    *,
    now: float | None = None,
) -> dict[str, Any]:
    """Append one hash-chained record; each names its predecessor's digest."""
    now = time.time() if now is None else now
    log_path = state / "audit.jsonl"
    flags = os.O_RDWR | os.O_CREAT | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(log_path, flags, 0o600)
    with os.fdopen(descriptor, "r+b") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        handle.seek(0)
        previous = "0" * 64
        sequence = 0
        for line in handle:
            if line.strip():
                record = json.loads(line)
                previous = record["sha256"]
                sequence = record["seq"]
        record = {
            "seq": sequence + 1,
            "at": _iso(now),
            "action": action,
            "subject": subject,
            "detail": dict(detail),
            "prev": previous,
        }
        record["sha256"] = hashlib.sha256(_canonical_json(record)).hexdigest()
        handle.seek(0, os.SEEK_END)
        handle.write(_canonical_json(record) + b"\n")
        handle.flush()
        os.fsync(handle.fileno())
    return record


def verify_audit(state: Path) -> dict[str, Any]:
    log_path = state / "audit.jsonl"
    if not log_path.exists():
        return {"ok": True, "records": 0}
    previous = "0" * 64
    count = 0
    for number, line in enumerate(log_path.read_bytes().splitlines(), 1):
        if not line.strip():
            continue
        record = json.loads(line)
        claimed = record.pop("sha256", "")
        if (
            record.get("prev") != previous
            or record.get("seq") != count + 1
            or hashlib.sha256(_canonical_json(record)).hexdigest() != claimed
        ):
            return {"ok": False, "records": count, "brokenAtLine": number}
        previous = claimed
        count += 1
    return {"ok": True, "records": count, "head": previous}


def quarantine_marker_path(state: Path, owner: str, name: str) -> Path:
    owner, name = _check_identity(owner, name)
    return state / "quarantine" / owner / (name.lower() + ".json")


def is_quarantined(state: Path, owner: str, name: str) -> bool:
    """Cheap per-request check used by the gateway; invalid names are refused."""
    try:
        return quarantine_marker_path(state, owner, name).is_file()
    except PolicyError:
        return True


def _new_case_id(now: float) -> str:
    return "case-%s-%s" % (
        time.strftime("%Y%m%d%H%M%S", time.gmtime(now)), secrets.token_hex(4))


def record_evidence(
    state: Path,
    policy: Mapping[str, Any],
    *,
    owner: str,
    name: str,
    source: str,
    findings: list[dict[str, Any]],
    refs: list[dict[str, str]] | None = None,
    case_id: str = "",
    now: float | None = None,
) -> str:
    """Store metadata about a violation; the refused bytes are never kept."""
    now = time.time() if now is None else now
    evidence_id = "evidence-%s-%s" % (
        time.strftime("%Y%m%d%H%M%S", time.gmtime(now)), secrets.token_hex(4))
    _write_json(state / "evidence" / (evidence_id + ".json"), {
        "id": evidence_id,
        "recordedAt": _iso(now),
        "recordedAtEpoch": int(now),
        "owner": owner,
        "repository": name,
        "source": source,
        "caseId": case_id,
        "policySha256": policy_sha256(policy),
        "findings": findings[:MAX_RECORDED_FINDINGS],
        "findingCount": len(findings),
        "refs": refs or [],
        "retainDays": policy["retention"]["evidenceDays"],
        "contentRetained": False,
    })
    return evidence_id


def quarantine_repository(
    state: Path,
    *,
    owner: str,
    name: str,
    reason: str,
    actor: str,
    case_id: str = "",
    evidence_id: str = "",
    now: float | None = None,
) -> dict[str, Any]:
    now = time.time() if now is None else now
    marker = quarantine_marker_path(state, owner, name)
    if marker.exists():
        return _read_json(marker)
    value = {
        "owner": owner.lower(),
        "repository": name,
        "reason": reason,
        "actor": actor,
        "caseId": case_id,
        "evidenceId": evidence_id,
        "quarantinedAt": _iso(now),
    }
    _write_json(marker, value)
    audit(state, "quarantine", owner.lower() + "/" + name, value, now=now)
    return value


def release_repository(
    state: Path, *, owner: str, name: str, reason: str, actor: str,
    now: float | None = None,
) -> bool:
    marker = quarantine_marker_path(state, owner, name)
    if not marker.exists():
        return False
    prior = _read_json(marker)
    marker.unlink()
    audit(state, "release", owner.lower() + "/" + name, {
        "reason": reason, "actor": actor, "previous": prior}, now=now)
    return True


# --------------------------------------------------------------------------
# Repository inspection


def _git_environment() -> dict[str, str]:
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "LANG": "C",
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_OPTIONAL_LOCKS": "0",
    }
    # Inside pre-receive Git points these at its object quarantine; the new
    # objects are only visible through them.
    for key in (
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_QUARANTINE_PATH",
    ):
        if os.environ.get(key):
            environment[key] = os.environ[key]
    return environment


def _git_command(
    git_dir: Path, arguments: list[str], config: Iterable[str]
) -> list[str]:
    return [
        "git", "-c", "core.fsmonitor=", "-c", "safe.directory=*",
        *[token for item in config for token in ("-c", item)],
        "--git-dir", str(git_dir), *arguments,
    ]


def _git(
    git_dir: Path, arguments: list[str], input_bytes: bytes = b"",
    *, config: Iterable[str] = (),
) -> bytes:
    completed = subprocess.run(
        _git_command(git_dir, arguments, config),
        input=input_bytes,
        capture_output=True,
        env=_git_environment(),
        timeout=GIT_TIMEOUT_SECONDS,
        check=False,
    )
    if completed.returncode != 0:
        raise PolicyError(
            "git %s failed: %s" % (
                arguments[0],
                completed.stderr.decode("utf-8", "replace").strip()[:200]))
    return completed.stdout


def _git_succeeds(git_dir: Path, arguments: list[str]) -> bool:
    completed = subprocess.run(
        _git_command(git_dir, arguments, ()),
        capture_output=True,
        env=_git_environment(),
        timeout=GIT_TIMEOUT_SECONDS,
        check=False,
    )
    return completed.returncode == 0


def directory_bytes(path: Path, *, skip_prefix: str = "") -> int:
    total = 0
    for root, directories, files in os.walk(path):
        if skip_prefix:
            directories[:] = [
                item for item in directories if not item.startswith(skip_prefix)]
        for item in files:
            try:
                info = os.lstat(os.path.join(root, item))
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total


def repository_storage_bytes(git_dir: Path) -> int:
    # Git's receive quarantine lives under objects/ as tmp_objdir-incoming-*.
    return directory_bytes(git_dir / "objects", skip_prefix="tmp_objdir")


def stored_repositories(root: Path) -> list[tuple[str, str, Path]]:
    """Every <root>/<owner>/<name>.git bare repository, sorted."""
    found: list[tuple[str, str, Path]] = []
    if not root.is_dir():
        return found
    for owner_dir in sorted(root.iterdir()):
        if owner_dir.is_symlink() or not owner_dir.is_dir():
            continue
        if not NODE_RE.fullmatch(owner_dir.name):
            continue
        for git_dir in sorted(owner_dir.iterdir()):
            name = git_dir.name[:-4] if git_dir.name.endswith(".git") else ""
            if (
                not name
                or not REPO_RE.fullmatch(name)
                or git_dir.is_symlink()
                or not (git_dir / "objects").is_dir()
            ):
                continue
            found.append((owner_dir.name, name, git_dir))
    return found


def _object_format_bytes(git_dir: Path) -> int:
    fmt = _git(git_dir, ["rev-parse", "--show-object-format"]).strip()
    return 32 if fmt == b"sha256" else 20


def _enumerate_objects(
    git_dir: Path, revision_arguments: list[str], input_bytes: bytes = b""
) -> tuple[dict[str, str], list[tuple[str, str, int]]]:
    """Return (first path per object, [(oid, type, size)]) for a rev walk."""
    listing = _git(
        git_dir, ["rev-list", "--objects", *revision_arguments], input_bytes)
    paths: dict[str, str] = {}
    order: list[str] = []
    for line in listing.decode("utf-8", "surrogateescape").split("\n"):
        oid, _, path = line.partition(" ")
        # A path containing a newline continues on the next line; its
        # remainder never looks like an object id, so it is skipped.
        if not OBJECT_ID_RE.fullmatch(oid):
            continue
        if oid not in paths:
            order.append(oid)
            paths[oid] = path
    if not order:
        return paths, []
    checked = _git(
        git_dir,
        ["cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)"],
        ("\n".join(order) + "\n").encode(),
    )
    objects: list[tuple[str, str, int]] = []
    for line in checked.decode().splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2].isdigit():
            objects.append((parts[0], parts[1], int(parts[2])))
    return paths, objects


def _tree_entry_names(git_dir: Path, tree_ids: list[str]) -> list[tuple[str, str]]:
    """Every (tree oid, entry name) in the given trees, parsed from raw objects.

    Scanning entry names of every new tree is complete for extension and
    basename rules: a blob that already exists under an allowed name still
    needs a new tree entry to appear under a refused one.
    """
    if not tree_ids:
        return []
    hash_bytes = _object_format_bytes(git_dir)
    raw = _git(
        git_dir, ["cat-file", "--batch"], ("\n".join(tree_ids) + "\n").encode())
    entries: list[tuple[str, str]] = []
    offset = 0
    while offset < len(raw):
        header_end = raw.index(b"\n", offset)
        header = raw[offset:header_end].split()
        offset = header_end + 1
        if len(header) != 3 or header[1] != b"tree":
            if len(header) == 3 and header[2].isdigit():
                offset += int(header[2]) + 1
            continue
        tree_id = header[0].decode()
        body = raw[offset:offset + int(header[2])]
        offset += int(header[2]) + 1
        position = 0
        while position < len(body):
            space = body.index(b" ", position)
            nul = body.index(b"\0", space)
            name = body[space + 1:nul].decode("utf-8", "surrogateescape")
            entries.append((tree_id, name))
            position = nul + 1 + hash_bytes
    return entries


def _selector_matches(selectors: Iterable[str], owner: str, name: str) -> bool:
    full = owner.lower() + "/" + name.lower()
    return any(item == owner.lower() or item == full for item in selectors)


def repository_admission(
    policy: Mapping[str, Any], owner: str, name: str
) -> list[dict[str, Any]]:
    """Findings that refuse a repository regardless of its content."""
    lists = policy["repositories"]
    findings: list[dict[str, Any]] = []
    if _selector_matches(lists["block"], owner, name):
        findings.append({
            "rule": "repository-blocked",
            "detail": "this node blocks %s/%s" % (owner, name),
        })
    elif lists["allow"] and not _selector_matches(lists["allow"], owner, name):
        findings.append({
            "rule": "repository-not-allowed",
            "detail": "this node only stores allowlisted repositories",
        })
    return findings


def new_repository_admission(
    policy: Mapping[str, Any], root: Path | None, owner: str, name: str
) -> list[dict[str, Any]]:
    """Admission for creating a repository that does not exist yet."""
    findings = repository_admission(policy, owner, name)
    if root is not None:
        existing = [
            item for item in stored_repositories(root)
            if (item[0], item[1].lower()) != (owner.lower(), name.lower())
        ]
        maximum = policy["limits"]["maxRepositories"]
        if len(existing) >= maximum:
            findings.append({
                "rule": "repository-count",
                "detail": "this node stores at most %d repositories" % maximum,
            })
    return findings


def _content_findings(
    policy: Mapping[str, Any],
    git_dir: Path,
    paths: Mapping[str, str],
    objects: list[tuple[str, str, int]],
) -> list[dict[str, Any]]:
    content = policy["content"]
    max_blob = policy["limits"]["maxBlobBytes"]
    blocked_ids = set(content["blockedObjectIds"])
    extensions = content["blockedExtensions"]
    patterns = content["blockedPathPatterns"]
    findings: list[dict[str, Any]] = []
    for oid, kind, size in objects:
        if oid in blocked_ids:
            findings.append({
                "rule": "blocked-object", "oid": oid, "path": paths.get(oid, ""),
                "detail": "object is on this node's blocklist",
            })
        if kind == "blob" and size > max_blob:
            findings.append({
                "rule": "blob-size", "oid": oid, "path": paths.get(oid, ""),
                "bytes": size,
                "detail": "file is larger than %d bytes" % max_blob,
            })
        if kind == "blob" and patterns:
            path = paths.get(oid, "")
            for pattern in patterns:
                if "/" in pattern and path and fnmatch.fnmatchcase(path, pattern):
                    findings.append({
                        "rule": "blocked-path", "oid": oid, "path": path,
                        "detail": "path matches %s" % pattern,
                    })
                    break
    if extensions or any("/" not in item for item in patterns):
        basename_patterns = [item for item in patterns if "/" not in item]
        trees = [oid for oid, kind, _size in objects if kind == "tree"]
        seen: set[str] = set()
        for tree_id, entry in _tree_entry_names(git_dir, trees):
            lowered = entry.lower()
            rule = ""
            if any(lowered.endswith(ext) for ext in extensions):
                rule = "blocked-extension"
            elif any(fnmatch.fnmatchcase(entry, item) for item in basename_patterns):
                rule = "blocked-path"
            if rule and (rule, entry) not in seen:
                seen.add((rule, entry))
                parent = paths.get(tree_id, "")
                findings.append({
                    "rule": rule,
                    "path": (parent + "/" + entry) if parent else entry,
                    "detail": "file name %s is not accepted here" % entry,
                })
    return findings


# --------------------------------------------------------------------------
# Protected branches


def branch_rules_for(
    policy: Mapping[str, Any], owner: str, name: str, ref: str
) -> list[dict[str, Any]]:
    """Every branch rule that covers one ref of one repository."""
    return [
        rule for rule in policy["branches"]["rules"]
        if (not rule["repositories"]
            or _selector_matches(rule["repositories"], owner, name))
        and fnmatch.fnmatchcase(ref, rule["pattern"])
    ]


def protected_updates(
    policy: Mapping[str, Any], owner: str, name: str,
    updates: list[tuple[str, str, str]],
) -> list[dict[str, Any]]:
    """The pushed ref updates covered by at least one branch rule."""
    protected = []
    for old, new, ref in updates:
        rules = branch_rules_for(policy, owner, name, ref)
        if rules:
            protected.append(
                {"ref": ref, "old": old, "new": new, "rules": rules})
    return protected


def _short_oid(value: str) -> str:
    if not value or ZERO_OID_RE.fullmatch(value):
        return "(nothing)"
    return value[:12]


def _current_refs(git_dir: Path) -> dict[str, str]:
    listing = _git(
        git_dir, ["for-each-ref", "--format=%(objectname) %(refname)"])
    refs: dict[str, str] = {}
    for line in listing.decode("utf-8", "surrogateescape").splitlines():
        oid, _, ref = line.partition(" ")
        if OBJECT_ID_RE.fullmatch(oid) and ref:
            refs[ref] = oid
    return refs


def _signature_findings(
    policy: Mapping[str, Any], git_dir: Path, ref: str, old: str, new: str
) -> list[dict[str, Any]]:
    """One finding per commit this update adds that no allowed signer signed.

    An update checks the commits between the old and new tips, so a commit
    that already sits unsigned on another branch cannot be fast-forwarded in.
    A newly created protected ref checks the commits new to the repository.
    """
    signers = Path(policy["branches"]["allowedSignersPath"])
    _read_owner_file(
        signers, "allowed signers file", MAX_ALLOWED_SIGNERS_BYTES)
    exclude = ["--not", "--all"] if ZERO_OID_RE.fullmatch(old) else ["^" + old]
    listing = _git(
        git_dir,
        [
            "log", "--no-show-signature", "--format=%H %G?",
            "-n", str(MAX_SIGNATURE_COMMITS + 1), new, *exclude,
        ],
        config=(
            "gpg.ssh.program=ssh-keygen",
            "gpg.ssh.allowedSignersFile=" + str(signers),
        ),
    )
    lines = listing.decode("utf-8", "replace").splitlines()
    if len(lines) > MAX_SIGNATURE_COMMITS:
        return [{
            "rule": "signature-scan-limit", "ref": ref,
            "detail": "%s would gain more than %d commits to verify"
            % (ref, MAX_SIGNATURE_COMMITS),
        }]
    findings = []
    for line in lines:
        oid, _, state = line.partition(" ")
        if state != "G":
            findings.append({
                "rule": "commit-signature", "ref": ref, "oid": oid,
                "detail": "a new commit on %s %s" % (ref, SIGNATURE_STATES.get(
                    state, "has an unrecognized signature state")),
            })
    return findings


def branch_findings(
    policy: Mapping[str, Any], git_dir: Path, owner: str, name: str,
    updates: list[tuple[str, str, str]],
) -> list[dict[str, Any]]:
    """Findings for pushed updates to protected refs; empty admits them.

    Git re-checks every expected old value under its ref lock after this hook,
    so a push that loses a race still fails without moving the ref. Checking
    here first turns a stale write into an explanation with recovery steps.
    """
    protected = protected_updates(policy, owner, name, updates)
    if not protected:
        return []
    current = _current_refs(git_dir)
    findings: list[dict[str, Any]] = []
    for item in protected:
        ref, old, new, rules = item["ref"], item["old"], item["new"], item["rules"]
        actual = current.get(ref, "")
        creating = bool(ZERO_OID_RE.fullmatch(old))
        if (creating and actual) or (not creating and actual != old):
            findings.append({
                "rule": "stale-ref", "ref": ref,
                "detail": "%s is at %s, not the %s this push expected"
                % (ref, _short_oid(actual), _short_oid(old)),
            })
            continue
        if ZERO_OID_RE.fullmatch(new):
            if any(rule["blockDeletion"] for rule in rules):
                findings.append({
                    "rule": "protected-branch-deletion", "ref": ref,
                    "detail": "%s is protected and cannot be deleted" % ref,
                })
            continue
        if (
            not creating
            and any(rule["blockForcePush"] for rule in rules)
            and not _git_succeeds(
                git_dir, ["merge-base", "--is-ancestor", old, new])
        ):
            findings.append({
                "rule": "protected-branch-force-update", "ref": ref,
                "detail": "%s is protected and %s does not contain its tip %s"
                % (ref, _short_oid(new), _short_oid(old)),
            })
            continue
        if any(rule["requireSignedCommits"] for rule in rules):
            findings.extend(
                _signature_findings(policy, git_dir, ref, old, new))
    return findings


def evaluate_push(
    policy: Mapping[str, Any],
    git_dir: Path,
    owner: str,
    name: str,
    updates: list[tuple[str, str, str]],
    *,
    root: Path | None = None,
    state: Path | None = None,
    secret_reports: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """Findings for one staged push; an empty list admits it.

    When the policy scans for secrets, the full scan report (including
    warnings and signed overrides that do not refuse the push) is appended to
    ``secret_reports``.
    """
    findings = repository_admission(policy, owner, name)
    if state is not None and is_quarantined(state, owner, name):
        findings.append({
            "rule": "repository-quarantined",
            "detail": "this repository is quarantined on this node",
        })
    if not findings:
        findings.extend(branch_findings(policy, git_dir, owner, name, updates))
    new_tips = [new for _old, new, _ref in updates if not ZERO_OID_RE.fullmatch(new)]
    if findings or not new_tips:
        return findings
    paths, objects = _enumerate_objects(
        git_dir, ["--stdin", "--not", "--all"],
        ("\n".join(new_tips) + "\n").encode())
    incoming = sum(size for _oid, _kind, size in objects)
    limits = policy["limits"]
    if incoming > limits["maxPushBytes"]:
        findings.append({
            "rule": "push-size", "bytes": incoming,
            "detail": "push adds more than %d bytes" % limits["maxPushBytes"],
        })
    quarantine = os.environ.get("GIT_QUARANTINE_PATH", "")
    staged = directory_bytes(Path(quarantine)) if quarantine else 0
    projected = repository_storage_bytes(git_dir) + staged
    if projected > limits["maxRepositoryBytes"]:
        findings.append({
            "rule": "repository-size", "bytes": projected,
            "detail": "repository would exceed %d bytes"
            % limits["maxRepositoryBytes"],
        })
    if root is not None:
        node_total = sum(
            repository_storage_bytes(item[2])
            for item in stored_repositories(root)) + staged
        if node_total > limits["storageBudgetBytes"]:
            findings.append({
                "rule": "storage-budget", "bytes": node_total,
                "detail": "node storage budget of %d bytes would be exceeded"
                % limits["storageBudgetBytes"],
            })
    findings.extend(_content_findings(policy, git_dir, paths, objects))
    if policy.get("secrets") is not None:
        report = _secret_scan(
            policy, git_dir, owner, name, ["--stdin", "--not", "--all"],
            ("\n".join(new_tips) + "\n").encode(), new_tips, "push")
        findings.extend(_secret_findings(report))
        if secret_reports is not None:
            secret_reports.append(report)
    return findings


def _secret_scan(
    policy: Mapping[str, Any], git_dir: Path, owner: str, name: str,
    revision_arguments: list[str], input_bytes: bytes, tips: list[str],
    boundary: str,
) -> dict[str, Any]:
    gate = secret_gate()
    try:
        return gate.scan_git(
            git_dir, revision_arguments, input_bytes=input_bytes, tips=tips,
            boundary=boundary, organization=policy["secrets"],
            repository=owner + "/" + name)
    except gate.GateError as exc:
        raise PolicyError("secret scan failed: %s" % exc)


def _secret_findings(report: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Blocking secret findings in the policy's finding shape (no values)."""
    return [
        {
            "rule": "secret", "secretRule": item["rule"], "oid": item["blob"],
            "path": item["path"], "line": item["line"],
            "fingerprint": item["fingerprint"],
            "detail": "probable %s (%s) at line %d, fingerprint %s" % (
                item["title"], item["rule"], item["line"],
                item["fingerprint"]),
        }
        for item in report["findings"] if item["outcome"] == "block"
    ]


def evaluate_stored(
    policy: Mapping[str, Any], git_dir: Path, owner: str, name: str
) -> dict[str, Any]:
    """Findings and size for a repository already stored on this node."""
    findings = repository_admission(policy, owner, name)
    size = repository_storage_bytes(git_dir)
    if size > policy["limits"]["maxRepositoryBytes"]:
        findings.append({
            "rule": "repository-size", "bytes": size,
            "detail": "repository is larger than %d bytes"
            % policy["limits"]["maxRepositoryBytes"],
        })
    has_refs = bool(_git(git_dir, ["for-each-ref", "--count=1"]).strip())
    result: dict[str, Any] = {}
    if has_refs:
        paths, objects = _enumerate_objects(git_dir, ["--all"])
        findings.extend(_content_findings(policy, git_dir, paths, objects))
        if policy.get("secrets") is not None:
            # Stored data already reached this node (and possibly others),
            # so detection here is after replication.
            report = _secret_scan(
                policy, git_dir, owner, name, ["--all"], b"", [], "stored")
            findings.extend(_secret_findings(report))
            gate = secret_gate()
            result["secrets"] = gate.telemetry(report)
            if report["counts"]["block"] or report["counts"]["warn"]:
                result["secretResponse"] = gate.response_plan(report)
    return {"owner": owner, "repository": name, "bytes": size,
            "findings": findings, **result}


# --------------------------------------------------------------------------
# Preview, apply, rollback, enforce, retention


def preview(
    candidate: Mapping[str, Any],
    root: Path,
    state: Path | None = None,
    current: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Describe what a policy would do on this node before it is applied."""
    repositories = stored_repositories(root)
    evaluations = []
    total = 0
    newly_unavailable = []
    violating = 0
    for owner, name, git_dir in repositories:
        result = evaluate_stored(candidate, git_dir, owner, name)
        total += result["bytes"]
        quarantined = state is not None and is_quarantined(state, owner, name)
        result["quarantined"] = quarantined
        if result["findings"]:
            violating += 1
            if not quarantined:
                newly_unavailable.append(result)
        evaluations.append(result)
    limits = candidate["limits"]
    budget = limits["storageBudgetBytes"]
    consequences: list[str] = []
    enforcing = candidate["violationAction"] == "quarantine"
    reducing = bool(newly_unavailable and enforcing)
    if newly_unavailable:
        consequences.append(
            "%d repositories on this node violate the policy%s. Other mirrors "
            "that carry them keep serving them; a repository this node alone "
            "seeds becomes unavailable to the network." % (
                len(newly_unavailable),
                " and would stop being served by `enforce --apply`"
                if enforcing else " (report-only: they stay served)"))
    if current is not None:
        tightened = [
            field for field in LIMIT_FIELDS
            if candidate["limits"][field] < current["limits"][field]
        ]
        if tightened:
            reducing = True
            consequences.append(
                "Lower limits (%s) refuse pushes that are accepted today."
                % ", ".join(tightened))
        if current["seeding"]["privateReplicas"] and not candidate["seeding"][
                "privateReplicas"]:
            reducing = True
            consequences.append(
                "Encrypted private replicas stop being served by this node.")
        allow_now = set(current["repositories"]["allow"])
        allow_next = set(candidate["repositories"]["allow"])
        if (
            set(candidate["repositories"]["block"])
            - set(current["repositories"]["block"])
            or (allow_next and (not allow_now or allow_now - allow_next))
        ):
            reducing = True
            consequences.append(
                "Repository allow/block lists narrow what this node accepts.")
        if any(
            set(candidate["content"][key]) - set(current["content"][key])
            for key in (
                "blockedObjectIds", "blockedExtensions", "blockedPathPatterns")
        ):
            reducing = True
            consequences.append("New content rules refuse additional pushes.")
        current_rules = {
            _canonical_json(rule) for rule in current["branches"]["rules"]}
        if any(
            _canonical_json(rule) not in current_rules
            for rule in candidate["branches"]["rules"]
        ):
            reducing = True
            consequences.append(
                "New branch protections refuse ref updates that are accepted "
                "today.")
        if candidate.get("secrets") is not None and _canonical_json(
                candidate["secrets"]) != _canonical_json(current.get("secrets")):
            reducing = True
            consequences.append(
                "Changed secret scanning may refuse pushes that add probable "
                "credentials and report stored ones.")
    if total > budget:
        reducing = True
        consequences.append(
            "Stored data (%d bytes) already exceeds the storage budget; every "
            "push that adds objects will be refused." % total)
    return {
        "policySha256": policy_sha256(candidate),
        "profile": candidate["profile"],
        "repositories": len(repositories),
        "violatingRepositories": violating,
        "newlyUnavailable": [
            {"owner": item["owner"], "repository": item["repository"],
             "rules": sorted({f["rule"] for f in item["findings"]})}
            for item in newly_unavailable
        ],
        "capacity": {
            "storedBytes": total,
            "storageBudgetBytes": budget,
            "headroomBytes": max(0, budget - total),
            "utilization": round(total / budget, 4) if budget else None,
            "repositorySlotsUsed": len(repositories),
            "repositorySlots": limits["maxRepositories"],
            "largestRepositoryBytes": max(
                (item["bytes"] for item in evaluations), default=0),
        },
        "availabilityReducing": reducing,
        "consequences": consequences,
    }


def apply_policy(
    candidate_path: Path,
    target: Path,
    state: Path,
    root: Path,
    *,
    confirm: bool,
    actor: str,
    action: str = "apply-policy",
    now: float | None = None,
) -> dict[str, Any]:
    raw = _read_owner_file(candidate_path, "policy", MAX_POLICY_BYTES)
    candidate = parse_policy_bytes(raw)
    current = load_policy(target) if target.exists() else None
    report = preview(candidate, root, state, current)
    if current is not None and policy_sha256(current) == report["policySha256"]:
        report["applied"] = False
        report["reason"] = "policy is unchanged"
        return report
    if report["availabilityReducing"] and not confirm:
        report["applied"] = False
        report["reason"] = (
            "policy reduces availability; review the consequences and rerun "
            "with --confirm")
        return report
    history = _private_directory(state / "policy-history")
    if target.exists():
        previous_raw = target.read_bytes()
        stamp = time.strftime(
            "%Y%m%d%H%M%S", time.gmtime(time.time() if now is None else now))
        _atomic_write(
            history / ("%s-%s.json" % (
                stamp, hashlib.sha256(previous_raw).hexdigest()[:12])),
            previous_raw)
    _atomic_write(target, raw)
    audit(state, action, str(target), {
        "actor": actor,
        "policySha256": report["policySha256"],
        "previousPolicySha256": policy_sha256(current) if current else "",
        "availabilityReducing": report["availabilityReducing"],
        "consequences": report["consequences"],
    }, now=now)
    report["applied"] = True
    return report


def policy_history(state: Path) -> list[Path]:
    directory = state / "policy-history"
    if not directory.is_dir():
        return []
    return sorted(item for item in directory.iterdir() if item.suffix == ".json")


def enforce(
    policy: Mapping[str, Any],
    root: Path,
    state: Path,
    *,
    apply: bool,
    actor: str,
    now: float | None = None,
) -> dict[str, Any]:
    results = []
    for owner, name, git_dir in stored_repositories(root):
        evaluation = evaluate_stored(policy, git_dir, owner, name)
        response = evaluation.get("secretResponse")
        if not evaluation["findings"] and response is None:
            continue
        if apply and response is not None:
            audit(state, "secret-detected-stored", owner + "/" + name, {
                "policySha256": policy_sha256(policy),
                "secrets": evaluation["secrets"],
            }, now=now)
        already = is_quarantined(state, owner, name)
        action = "none"
        if (
            apply and not already and evaluation["findings"]
            and policy["violationAction"] == "quarantine"
        ):
            evidence_id = record_evidence(
                state, policy, owner=owner, name=name, source="enforce",
                findings=evaluation["findings"], now=now)
            quarantine_repository(
                state, owner=owner, name=name, actor=actor,
                reason="policy violation: " + ", ".join(sorted(
                    {item["rule"] for item in evaluation["findings"]})),
                evidence_id=evidence_id, now=now)
            action = "quarantined"
        elif already:
            action = "already-quarantined"
        entry = {
            "owner": owner, "repository": name, "action": action,
            "rules": sorted({item["rule"] for item in evaluation["findings"]}),
        }
        if response is not None:
            entry["secrets"] = evaluation["secrets"]
            entry["secretResponse"] = response
        results.append(entry)
    return {"applied": apply, "violations": results}


def remove_repository(
    state: Path, root: Path, *, owner: str, name: str, actor: str,
    reason: str, confirm: bool, now: float | None = None,
) -> dict[str, Any]:
    """Permanently delete a quarantined repository from this node only."""
    owner, name = _check_identity(owner, name)
    if not is_quarantined(state, owner, name):
        raise PolicyError("quarantine the repository before removing it")
    git_dir = root / owner / (name + ".git")
    if git_dir.is_symlink() or not git_dir.is_dir() or root.resolve() not in (
            git_dir.resolve().parents):
        raise PolicyError("repository is not stored under this root")
    size = repository_storage_bytes(git_dir)
    if not confirm:
        return {"removed": False, "bytes": size, "reason":
                "removal is irreversible on this node; rerun with --confirm"}
    shutil.rmtree(git_dir)
    audit(state, "remove", owner + "/" + name, {
        "actor": actor, "reason": reason, "bytes": size,
        "irreversible": True}, now=now)
    return {"removed": True, "bytes": size}


def prune_evidence(
    policy: Mapping[str, Any], state: Path, *, apply: bool,
    now: float | None = None,
) -> dict[str, Any]:
    now = time.time() if now is None else now
    held = set()
    for case_path in (state / "cases").glob("case-*.json"):
        case = _read_json(case_path)
        if case.get("legalHold") or case.get("status") in ("open", "appealed"):
            held.add(case["id"])
    cutoff = now - policy["retention"]["evidenceDays"] * 86400
    pruned = []
    for path in sorted((state / "evidence").glob("evidence-*.json")):
        record = _read_json(path)
        if record.get("caseId") in held:
            continue
        if record.get("recordedAtEpoch", now) < cutoff:
            pruned.append(record["id"])
            if apply:
                path.unlink()
    if apply and pruned:
        audit(state, "prune-evidence", "evidence", {"ids": pruned}, now=now)
    return {"applied": apply, "pruned": pruned}


# --------------------------------------------------------------------------
# Reports and appeals


def _case_path(state: Path, case_id: str) -> Path:
    if not CASE_ID_RE.fullmatch(case_id):
        raise PolicyError("case id is invalid")
    return state / "cases" / (case_id + ".json")


def open_case(
    state: Path, policy: Mapping[str, Any], *, owner: str, name: str,
    category: str, reporter: str, details: str, legal_hold: bool = False,
    now: float | None = None,
) -> dict[str, Any]:
    owner, name = _check_identity(owner, name)
    if category not in REPORT_CATEGORIES:
        raise PolicyError("category must be one of: " + ", ".join(
            REPORT_CATEGORIES))
    now = time.time() if now is None else now
    case = {
        "id": _new_case_id(now),
        "openedAt": _iso(now),
        "status": "open",
        "owner": owner,
        "repository": name,
        "category": category,
        "reporter": reporter[:500],
        "details": details[:4000],
        "jurisdiction": policy["jurisdiction"],
        "runbook": RUNBOOKS[policy["jurisdiction"]],
        "legalHold": legal_hold,
        "history": [{"at": _iso(now), "event": "reported"}],
    }
    _write_json(_case_path(state, case["id"]), case)
    audit(state, "report", owner + "/" + name, {
        "caseId": case["id"], "category": category,
        "legalHold": legal_hold}, now=now)
    return case


def _update_case(
    state: Path, case_id: str, event: dict[str, Any], status: str,
    now: float,
) -> dict[str, Any]:
    path = _case_path(state, case_id)
    if not path.is_file():
        raise PolicyError("case was not found")
    case = _read_json(path)
    case["status"] = status
    case["history"].append({"at": _iso(now), **event})
    _write_json(path, case)
    audit(state, "case-" + event["event"], case["owner"] + "/" +
          case["repository"], {"caseId": case_id, **event}, now=now)
    return case


def action_case(
    state: Path, policy: Mapping[str, Any], case_id: str, *, actor: str,
    note: str, now: float | None = None,
) -> dict[str, Any]:
    """Quarantine the reported repository under this case."""
    now = time.time() if now is None else now
    path = _case_path(state, case_id)
    if not path.is_file():
        raise PolicyError("case was not found")
    case = _read_json(path)
    evidence_id = record_evidence(
        state, policy, owner=case["owner"], name=case["repository"],
        source="report", case_id=case_id,
        findings=[{"rule": "report-" + case["category"], "detail": note}],
        now=now)
    quarantine_repository(
        state, owner=case["owner"], name=case["repository"], actor=actor,
        reason="report " + case_id, case_id=case_id,
        evidence_id=evidence_id, now=now)
    return _update_case(state, case_id, {
        "event": "actioned", "actor": actor, "note": note[:2000],
        "evidenceId": evidence_id}, "actioned", now)


def appeal_case(
    state: Path, case_id: str, *, appellant: str, details: str,
    now: float | None = None,
) -> dict[str, Any]:
    now = time.time() if now is None else now
    return _update_case(state, case_id, {
        "event": "appealed", "appellant": appellant[:500],
        "details": details[:4000]}, "appealed", now)


def resolve_case(
    state: Path, case_id: str, *, decision: str, actor: str, note: str,
    now: float | None = None,
) -> dict[str, Any]:
    if decision not in CASE_DECISIONS:
        raise PolicyError("decision must be one of: " + ", ".join(
            CASE_DECISIONS))
    now = time.time() if now is None else now
    case = _read_json(_case_path(state, case_id))
    if decision in ("reverse", "dismiss"):
        marker = quarantine_marker_path(state, case["owner"], case["repository"])
        if marker.is_file() and _read_json(marker).get("caseId") == case_id:
            release_repository(
                state, owner=case["owner"], name=case["repository"],
                actor=actor, reason="case %s %s" % (case_id, decision), now=now)
    status = {"uphold": "upheld", "reverse": "reversed",
              "dismiss": "dismissed"}[decision]
    return _update_case(state, case_id, {
        "event": "resolved", "decision": decision, "actor": actor,
        "note": note[:2000]}, status, now)


def status_summary(state: Path) -> dict[str, Any]:
    """Secret-free counters suitable for local telemetry and dashboards."""
    cases = [_read_json(path) for path in (state / "cases").glob("case-*.json")]
    audit_path = state / "audit.jsonl"
    rejected = 0
    admitted_protected = 0
    secrets = {
        "blockedPushes": 0, "warnedPushes": 0, "warnings": 0,
        "overriddenFindings": 0, "allowlistedFindings": 0,
        "invalidOverrides": 0, "storedDetections": 0,
    }
    if audit_path.exists():
        for line in audit_path.read_bytes().splitlines():
            record = json.loads(line) if line.strip() else {}
            action = record.get("action", "")
            detail = record.get("detail", {})
            if action == "reject-push":
                rejected += 1
                if "secret" in detail.get("rules", []):
                    secrets["blockedPushes"] += 1
            elif action == "admit-push":
                admitted_protected += 1
            elif action == "admit-push-secrets":
                counters = detail.get("secrets", {})
                secrets["warnedPushes"] += 1 if counters.get("warned") else 0
                secrets["warnings"] += counters.get("warned", 0)
                secrets["overriddenFindings"] += counters.get("overridden", 0)
                secrets["allowlistedFindings"] += counters.get("allowlisted", 0)
                secrets["invalidOverrides"] += counters.get(
                    "invalidOverrides", 0)
            elif action == "secret-detected-stored":
                secrets["storedDetections"] += 1
    return {
        "quarantinedRepositories": len(list(
            (state / "quarantine").glob("*/*.json"))),
        "openCases": sum(1 for c in cases if c["status"] == "open"),
        "appealedCases": sum(1 for c in cases if c["status"] == "appealed"),
        "resolvedCases": sum(1 for c in cases if c["status"] in (
            "upheld", "reversed", "dismissed")),
        "rejectedPushes": rejected,
        "admittedProtectedPushes": admitted_protected,
        "secrets": secrets,
        "evidenceRecords": len(list((state / "evidence").glob("*.json"))),
        "audit": verify_audit(state),
    }


# --------------------------------------------------------------------------
# Git hook


def hook_script(python: str, script: Path, policy: Path, state: Path,
                root: Path | None) -> bytes:
    import shlex

    arguments = [
        python, "-I", str(script), "pre-receive",
        "--policy", str(policy), "--state", str(state),
    ]
    if root is not None:
        arguments += ["--root", str(root)]
    return (
        "#!/bin/sh\n# Managed-By: ForkMesh operator policy\n"
        "exec " + " ".join(shlex.quote(item) for item in arguments) + "\n"
    ).encode()


def install_hook(directory: Path, policy: Path, state: Path,
                 root: Path | None) -> Path:
    """Write the gateway-owned hooks directory holding only pre-receive."""
    _private_directory(directory)
    if stat.S_IMODE(directory.stat().st_mode) & (stat.S_IRWXG | stat.S_IRWXO):
        os.chmod(directory, 0o700)
    for stray in directory.iterdir():
        if stray.name != "pre-receive" and not stray.name.startswith("."):
            raise PolicyError("operator policy hooks directory holds other files")
    _atomic_write(
        directory / "pre-receive",
        hook_script(sys.executable, Path(__file__).resolve(), policy, state,
                    root),
        0o700,
    )
    return directory


def _print_recovery(findings: list[dict[str, Any]], stderr: Any) -> None:
    shown: set[str] = set()
    for item in findings:
        rule = item["rule"]
        if rule not in RECOVERY_STEPS or rule in shown:
            continue
        shown.add(rule)
        ref = item.get("ref", "")
        branch = ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else ref
        print("  to recover (%s):" % rule, file=stderr)
        for step in RECOVERY_STEPS[rule]:
            print("    " + step.format(branch=branch), file=stderr)


def _admit_secret_reports(
    state: Path, policy: Mapping[str, Any], owner: str, name: str,
    reports: list[dict[str, Any]], stderr: Any,
) -> None:
    """Show and audit warnings and signed overrides on an admitted push."""
    gate = secret_gate() if reports else None
    for report in reports:
        counters = gate.telemetry(report)
        if not any(counters[key] for key in (
                "warned", "overridden", "allowlisted", "invalidOverrides")):
            continue
        for line in gate.format_report(report, "push"):
            print(line, file=stderr)
        audit(state, "admit-push-secrets", owner + "/" + name, {
            "policySha256": policy_sha256(policy),
            "secrets": counters,
            "overrides": [
                {"signer": item["signer"], "reason": item["reason"],
                 "expiresAt": item["expiresAt"],
                 "fingerprints": item["fingerprints"]}
                for item in report["overrides"]
            ],
            "overriddenFingerprints": sorted(
                item["fingerprint"] for item in report["findings"]
                if item["outcome"] == "overridden"),
        })


def run_pre_receive(
    stdin_text: str, git_dir: Path, policy_path: Path, state: Path,
    root: Path | None, stderr: Any = sys.stderr, repository: str = "",
) -> int:
    """Admit or refuse one push; ``repository`` overrides the path layout.

    The HTTPS gateway stores ``<root>/<owner>/<name>.git`` so the identity is
    read from the path; the SSH gateway's configured paths are arbitrary, so
    it passes ``owner/name`` explicitly.
    """
    updates = []
    for line in stdin_text.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3:
            updates.append((parts[0], parts[1], parts[2]))
    if repository:
        owner, _, name = repository.partition("/")
    else:
        owner = git_dir.parent.name
        name = git_dir.name[:-4] if git_dir.name.endswith(".git") else git_dir.name
    try:
        policy = load_policy(policy_path)
        _require_state_directory(state)
        owner, name = _check_identity(owner, name)
        secret_reports: list[dict[str, Any]] = []
        findings = evaluate_push(
            policy, git_dir, owner, name, updates, root=root, state=state,
            secret_reports=secret_reports)
        if not findings:
            _admit_secret_reports(state, policy, owner, name, secret_reports,
                                  stderr)
            protected = protected_updates(policy, owner, name, updates)
            if protected:
                # Accepted writes to protected refs are audited before Git
                # applies them; a lost ref-lock race can still refuse one.
                audit(state, "admit-push", owner + "/" + name, {
                    "policySha256": policy_sha256(policy),
                    "refs": [
                        {
                            "ref": item["ref"], "old": item["old"],
                            "new": item["new"],
                            "patterns": sorted(
                                {rule["pattern"] for rule in item["rules"]}),
                        }
                        for item in protected
                    ],
                })
    except (PolicyError, OSError, subprocess.SubprocessError, ValueError) as exc:
        # Fail closed: a node that cannot read its own policy (or record an
        # admitted protected write) accepts nothing.
        print("ForkMesh node policy is unavailable: %s" % exc, file=stderr)
        return 1
    if not findings:
        return 0
    refs = [{"ref": ref, "old": old, "new": new} for old, new, ref in updates]
    evidence_id = record_evidence(
        state, policy, owner=owner, name=name, source="push",
        findings=findings, refs=refs)
    audit(state, "reject-push", owner + "/" + name, {
        "evidenceId": evidence_id,
        "rules": sorted({item["rule"] for item in findings}),
        "refs": [item["ref"] for item in refs],
    })
    print("This node's operator policy refused the push "
          "(other ForkMesh nodes may accept it):", file=stderr)
    for item in findings[:10]:
        location = item.get("path") or item.get("oid") or ""
        print("  %s: %s%s" % (
            item["rule"], item["detail"],
            " (" + location + ")" if location else ""), file=stderr)
    if len(findings) > 10:
        print("  ... %d more" % (len(findings) - 10), file=stderr)
    if any(item["rule"] == "secret" for item in findings):
        print("  " + secret_gate().PERMANENCE_WARNING, file=stderr)
    _print_recovery(findings, stderr)
    print("  reference: " + evidence_id, file=stderr)
    return 1


# --------------------------------------------------------------------------
# CLI


def _path(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("path must be absolute")
    return path


def _emit(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def command(name: str, *, policy: bool = False, state: bool = False,
                root: bool = False, **kwargs: Any) -> argparse.ArgumentParser:
        sub = commands.add_parser(name, **kwargs)
        if policy:
            sub.add_argument("--policy", type=_path, required=True)
        if state:
            sub.add_argument("--state", type=_path, required=True)
        if root:
            sub.add_argument("--root", type=_path, required=True)
        return sub

    def identity(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--owner", required=True)
        sub.add_argument("--name", required=True)

    def actor(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--actor", default=os.environ.get("USER", "operator"))

    command("validate", policy=True, help="print the effective policy")
    hook = command("pre-receive", policy=True, state=True,
                   help="Git pre-receive hook mode")
    hook.add_argument("--root", type=_path)
    sub = command("scan", policy=True, help="scan one stored repository")
    sub.add_argument("--repository", type=_path, required=True)
    identity(sub)
    sub = command("preview", policy=True, root=True,
                  help="show a policy's effect and capacity before applying")
    sub.add_argument("--state", type=_path)
    sub.add_argument("--current", type=_path)
    sub = command("apply", policy=True, state=True, root=True,
                  help="install a policy (archives the previous one)")
    sub.add_argument("--target", type=_path, required=True)
    sub.add_argument("--confirm", action="store_true")
    actor(sub)
    sub = command("rollback", state=True, root=True,
                  help="restore the most recent archived policy")
    sub.add_argument("--target", type=_path, required=True)
    sub.add_argument("--to", default="", help="archived file name")
    sub.add_argument("--confirm", action="store_true")
    actor(sub)
    sub = command("history", state=True, help="list archived policies")
    sub = command("enforce", policy=True, state=True, root=True,
                  help="quarantine stored repositories that violate policy")
    sub.add_argument("--apply", action="store_true")
    actor(sub)
    sub = command("quarantine", state=True, help="stop serving a repository")
    identity(sub)
    sub.add_argument("--reason", required=True)
    actor(sub)
    sub = command("release", state=True, help="lift a quarantine")
    identity(sub)
    sub.add_argument("--reason", required=True)
    actor(sub)
    sub = command("remove", state=True, root=True,
                  help="permanently delete a quarantined repository locally")
    identity(sub)
    sub.add_argument("--reason", required=True)
    sub.add_argument("--confirm", action="store_true")
    actor(sub)
    sub = command("report", policy=True, state=True, help="open a case")
    identity(sub)
    sub.add_argument("--category", choices=REPORT_CATEGORIES, required=True)
    sub.add_argument("--reporter", required=True)
    sub.add_argument("--details", required=True)
    sub.add_argument("--legal-hold", action="store_true")
    sub = command("action", policy=True, state=True,
                  help="quarantine the repository named by a case")
    sub.add_argument("--case", required=True)
    sub.add_argument("--note", required=True)
    actor(sub)
    sub = command("appeal", state=True, help="record an owner appeal")
    sub.add_argument("--case", required=True)
    sub.add_argument("--appellant", required=True)
    sub.add_argument("--details", required=True)
    sub = command("resolve", state=True, help="decide a case or appeal")
    sub.add_argument("--case", required=True)
    sub.add_argument("--decision", choices=CASE_DECISIONS, required=True)
    sub.add_argument("--note", required=True)
    actor(sub)
    sub = command("prune-evidence", policy=True, state=True,
                  help="delete evidence past retention (never held cases)")
    sub.add_argument("--apply", action="store_true")
    command("status", state=True, help="counters for telemetry")
    command("verify-audit", state=True, help="check the audit hash chain")
    command("runbook", policy=True, help="print the jurisdiction runbook")

    args = parser.parse_args(argv)
    try:
        if args.command == "pre-receive":
            git_dir = Path(os.environ.get("GIT_DIR", ".")).resolve()
            return run_pre_receive(
                sys.stdin.read(), git_dir, args.policy, args.state, args.root,
                repository=os.environ.get("FORKMESH_POLICY_REPOSITORY", ""))
        state = _require_state_directory(args.state) if getattr(
            args, "state", None) else None
        if args.command == "validate":
            policy = load_policy(args.policy)
            _emit({"policySha256": policy_sha256(policy), "policy": policy})
        elif args.command == "scan":
            _emit(evaluate_stored(load_policy(args.policy), args.repository,
                                  *_check_identity(args.owner, args.name)))
        elif args.command == "preview":
            _emit(preview(load_policy(args.policy), args.root, state,
                          load_policy(args.current) if args.current else None))
        elif args.command == "apply":
            report = apply_policy(args.policy, args.target, state, args.root,
                                  confirm=args.confirm, actor=args.actor)
            _emit(report)
            return 0 if report["applied"] or report.get(
                "reason") == "policy is unchanged" else 3
        elif args.command == "rollback":
            archives = policy_history(state)
            if args.to:
                archives = [item for item in archives if item.name == args.to]
            if not archives:
                raise PolicyError("no archived policy to roll back to")
            report = apply_policy(archives[-1], args.target, state, args.root,
                                  confirm=args.confirm, actor=args.actor,
                                  action="rollback-policy")
            if report["applied"]:
                archives[-1].unlink()
            _emit(report)
            return 0 if report["applied"] else 3
        elif args.command == "history":
            _emit([item.name for item in policy_history(state)])
        elif args.command == "enforce":
            _emit(enforce(load_policy(args.policy), args.root, state,
                          apply=args.apply, actor=args.actor))
        elif args.command == "quarantine":
            _emit(quarantine_repository(
                state, owner=args.owner, name=args.name, reason=args.reason,
                actor=args.actor))
        elif args.command == "release":
            _emit({"released": release_repository(
                state, owner=args.owner, name=args.name, reason=args.reason,
                actor=args.actor)})
        elif args.command == "remove":
            result = remove_repository(
                state, args.root, owner=args.owner, name=args.name,
                actor=args.actor, reason=args.reason, confirm=args.confirm)
            _emit(result)
            return 0 if result["removed"] else 3
        elif args.command == "report":
            _emit(open_case(state, load_policy(args.policy), owner=args.owner,
                            name=args.name, category=args.category,
                            reporter=args.reporter, details=args.details,
                            legal_hold=args.legal_hold))
        elif args.command == "action":
            _emit(action_case(state, load_policy(args.policy), args.case,
                              actor=args.actor, note=args.note))
        elif args.command == "appeal":
            _emit(appeal_case(state, args.case, appellant=args.appellant,
                              details=args.details))
        elif args.command == "resolve":
            _emit(resolve_case(state, args.case, decision=args.decision,
                               actor=args.actor, note=args.note))
        elif args.command == "prune-evidence":
            _emit(prune_evidence(load_policy(args.policy), state,
                                 apply=args.apply))
        elif args.command == "status":
            _emit(status_summary(state))
        elif args.command == "verify-audit":
            result = verify_audit(state)
            _emit(result)
            return 0 if result["ok"] else 4
        elif args.command == "runbook":
            policy = load_policy(args.policy)
            _emit({"jurisdiction": policy["jurisdiction"],
                   "steps": RUNBOOKS[policy["jurisdiction"]]})
    except PolicyError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
