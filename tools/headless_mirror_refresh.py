#!/usr/bin/env python3
"""Fail-closed refresh orchestration for a headless ForkMesh mirror.

The identity helper owns all node and age private material.  This process
accepts only a bounded, owner-only, secret-free JSON configuration and invokes
the helper through its typed JSON stdin protocols.

The local refresh is deliberately separate from network publication:

* ``refresh`` fscks the exact bare source, seals a content-addressed age
  archive, validates a staged gateway configuration, and atomically switches
  the gateway configuration to it.
* the service manager restarts the gateway;
* ``register`` validates the active configuration, registers the endpoint, and
  publishes the node-owner catalog-v2 record.
* ``check`` performs the same active-state validation without sealing or
  making a network request.

Operational output contains only a generic event and counts.  Paths, node and
repository identities, subprocess output, HTTP response bodies, and request
payloads are never logged.
"""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
import errno
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, BinaryIO, Callable, Iterable, Iterator, Mapping
from urllib import error as urlerror
from urllib import request as urlrequest
from urllib.parse import quote, urlsplit


SCHEMA_VERSION = 1
CONFIG_TYPE = "forkmesh.headless-mirror-refresh"
MAX_CONFIG_BYTES = 64 * 1024
MAX_JSON_OUTPUT_BYTES = 128 * 1024
MAX_HTTP_RESPONSE_BYTES = 256 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024 * 1024
PROCESS_TIMEOUT_SECONDS = 30 * 60
HTTP_TIMEOUT_SECONDS = 30
MAX_PUBLIC_REPOSITORY_COUNT = 999_999_999_999
MAX_REPOSITORY_METADATA_PATHS = 100_000
MAX_REPOSITORY_METADATA_BYTES = 32 * 1024 * 1024
MAX_ISSUE_RECORD_BYTES = 1024 * 1024
MAX_MERGE_REQUEST_BYTES = 8 * 1024
MAX_PULL_METADATA_BYTES = 256 * 1024
MAX_ACTIONS_CONFIGURATION_BYTES = 1024
MAX_ACTIONS_STATE_BYTES = 4096
MAX_ACTIONS_STATE_LEASE_MS = 15 * 60 * 1000
CATALOG_RENEWAL_STATE_FILE = "catalog-renewal-state.json"
CATALOG_RENEWAL_STATE_TYPE = "forkmesh.catalog-renewal-state"
MAX_CATALOG_RENEWAL_STATE_BYTES = 4096
CATALOG_RENEWAL_INTERVAL_MS = 30 * 60 * 1000
CATALOG_RENEWAL_BACKOFF_FILE = "catalog-renewal-backoff.json"
CATALOG_RENEWAL_BACKOFF_TYPE = "forkmesh.catalog-renewal-backoff"
MAX_CATALOG_RENEWAL_BACKOFF_BYTES = 4096
# The service manager retries failed one-shot renewals after 20 seconds. Keep
# the first failure visible, but make that immediate retry a local no-op and
# let the normal four-minute timer perform the next remote attempt. This caps
# D1-affecting failures without treating an unaccepted catalog as published.
CATALOG_RENEWAL_FAILURE_BACKOFF_MS = 4 * 60 * 1000
CATALOG_SEMANTIC_EXCLUDED_FIELDS = frozenset({
    "lastSync",
    "updatedAt",
    "cpuPercent",
    "memUsedBytes",
    "memTotalBytes",
    "diskUsedBytes",
    "diskTotalBytes",
    "clonesServed",
    "websiteServed",
    "cloneServedAt",
    "websiteServedAt",
    "cloneServedAgent",
    "websiteServedAgent",
    "activityWeeks",
    "mergeActivityWeeks",
})
SERVICE_COUNTERS_FILE = "service-counters.json"
SERVICE_COUNTERS_TYPE = "forkmesh.mirror-service-counters"
MAX_SERVICE_COUNTERS_BYTES = 1024 * 1024
MAX_SERVICE_COUNTER = 999_999_999_999
# Gateway counters row: the three totals fields plus the optional last-served
# stamps (see GatewayServiceCounters in mirror_gateway.py).
SERVICE_COUNTER_FIELDS = frozenset({
    "clonesServed",
    "websiteServed",
    "updatedAt",
    "cloneServedAt",
    "cloneServedAgent",
    "websiteServedAt",
    "websiteServedAgent",
})
SERVICE_COUNTER_AGENT_CLASSES = frozenset({
    "forkmesh-node",
    "git-client",
    "bot-tool",
    "browser",
    "client",
})
MAX_MERGE_QUARANTINE_OBJECTS = 100_000
MAX_MERGE_QUARANTINE_BYTES = 2 * 1024 * 1024 * 1024
MAX_LOCAL_MERGE_JOBS = 10_000
LOCAL_MERGE_JOB_RETENTION_SECONDS = 30 * 24 * 60 * 60
MAX_SAFE_JSON_INTEGER = (1 << 53) - 1
AGE_NATIVE_HEADER = b"age-encryption.org/v1\n"
AGE_ARMORED_HEADER = b"-----BEGIN AGE ENCRYPTED FILE-----\n"
ACTIONS_CONFIGURATION_TYPE = "forkmesh.mirror-actions-catalog-configuration"
ACTIONS_STATE_TYPE = "forkmesh.mirror-actions-state"
ACTIONS_STATE_FILE = "actions-state.json"
ACTIONS_SUMMARY_FILE = "actions-summary.json"
# Must match tools/mirror_gateway.ACTIONS_CONTROL_DIRECTORY: the gateway-owned
# 0700 queue the Actions executor consumes router-signed control requests from.
ACTIONS_CONTROL_DIRECTORY = "actions-control"
HOSTED_REPOSITORIES_FILE = "hosted-repositories.json"
HOSTED_REPOSITORIES_TYPE = "forkmesh.hosted-repositories"
HOSTED_REPOSITORIES_ROOT = Path("/srv/forkmesh-git/imports")
MAX_HOSTED_REPOSITORIES = 100
# Repositories created by an authorized first push. tools/mirror_gateway.py
# writes this sidecar atomically next to the generated gateway configuration
# and touches the refresh path-unit marker, so the same refresh/registration
# cycle an SSH push drives also renders push-created repositories.
HOSTED_PUSHES_FILE = "hosted-pushes.json"
HOSTED_PUSHES_TYPE = "forkmesh.hosted-pushes"
# Must match tools/mirror_gateway.WRITABLE_OWNERS_WILDCARD: the entry a node
# hosting website-created repositories uses in place of a name list.
WRITABLE_OWNERS_WILDCARD = "*"
MAX_HOSTED_PUSHES = 200
MAX_HOSTED_PUSHES_BYTES = 256 * 1024
HOSTED_PUSH_ENTRY_FIELDS = frozenset(
    {"owner", "name", "gitDir", "firstPushedAt", "lastPushedAt"}
)
# Operations a push-created repository is rendered with. Reads follow the
# node's configured public surface; the write operation is added only here and
# only for a sidecar entry that survived every identity and storage check.
PUSHED_REPOSITORY_WRITE_OPERATION = "git-receive-pack"
# Operations no derived repository can inherit: they need a per-repository
# release store, the merge executor, or the Actions summary/control handoff.
DERIVED_REPOSITORY_EXCLUDED_OPERATIONS = frozenset(
    {
        "merge-pull",
        "update-pull-branch",
        "actions-status",
        "actions-control",
        "release-blob",
    }
)
MIN_RECEIVE_MAX_INPUT_BYTES = 1024 * 1024
MAX_RECEIVE_MAX_INPUT_BYTES = 4 * 1024 * 1024 * 1024
SYSTEM_ACTIONS_STATE_PATH = Path(
    "/var/lib/forkmesh-mirror/gateway/actions-state.json"
)
NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
ARCHIVE_NAME_RE = re.compile(r"^archive-[0-9a-f]{64}\.age$")
GIT_OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BASE64URL_RE = re.compile(r"^[A-Za-z0-9_-]+$")
MERGE_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")
MERGE_BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
SOLANA_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
KEY_REFERENCE_RE = re.compile(r"^[A-Za-z0-9._:/@+-]{3,240}$")
SENSITIVE_FIELD_RE = re.compile(
    r"(?:private.?key|secret|token|seed|mnemonic|keypair|password|credential)",
    re.IGNORECASE,
)
PUBLIC_KEY_FIELDS = frozenset(
    {
        "publicKey",
        "nodePublicKey",
        "routerPublicKey",
        "keyReference",
        "ciphertextSha256",
        "expectedRefsSha256",
        "stateHash",
        "payloadSha256",
    }
)
PUBLIC_OPERATIONS = frozenset(
    {
        "git-info-refs",
        "git-upload-pack",
        "tree",
        "blobs",
        "blob",
        "raw",
        "history",
        "commit",
        "compare",
        "branches",
        "search",
        "stats",
        "sizes",
        "release-blob",
        "merge-pull",
        "merge-preview",
        "update-pull-branch",
        "actions-status",
        "actions-control",
    }
)
REQUIRED_ROUTING_OPERATIONS = frozenset(
    {"git-info-refs", "git-upload-pack", "tree", "blob", "raw"}
)


class RefreshError(RuntimeError):
    """A fixed policy failure safe to print without leaking operator data."""


@dataclass(frozen=True)
class CatalogDefaults:
    description: str
    solana: str
    channel: str
    hosted_since: str
    branch: str
    platform: str
    version: str
    actions_enabled: bool
    report_cpu: bool
    report_memory: bool
    report_disk: bool


@dataclass(frozen=True)
class RefreshConfig:
    config_path: Path
    source_repository: Path
    archive_directory: Path
    release_store: Path | None
    gateway_config_path: Path
    identity_state_directory: Path
    identity_helper_path: Path
    mirror_gateway_path: Path
    python_program: Path
    git_program: Path
    manifest_path: Path
    node_owner: str
    repository_name: str
    owner_aliases: tuple[str, ...]
    worker_origin: str
    public_origin: str
    mirror_url: str
    listen_host: str
    listen_port: int
    operations: tuple[str, ...]
    catalog: CatalogDefaults
    # Push hosting is opt-in and defaults to "this node accepts no pushes".
    writable_repository_root: Path | None = None
    writable_owners: tuple[str, ...] = ()
    receive_max_input_bytes: int | None = None

    def helper_command(self, mode: str) -> list[str]:
        return [
            str(self.python_program),
            str(self.identity_helper_path),
            "--state-dir",
            str(self.identity_state_directory),
            mode,
        ]

    def gateway_check_command(self, config_path: Path) -> list[str]:
        return [
            str(self.python_program),
            str(self.mirror_gateway_path),
            "--config",
            str(config_path),
            "--check",
        ]


@dataclass(frozen=True)
class PublicIdentity:
    node_name: str
    node_public_key: str
    router_public_key: str
    allowed_origins: tuple[str, ...]


@dataclass(frozen=True)
class SealMetadata:
    ciphertext_sha256: str
    ciphertext_bytes: int
    key_reference: str
    expected_refs_sha256: str


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RefreshError("JSON contains a duplicate field")
        result[key] = value
    return result


def _parse_json(raw: bytes, *, maximum: int, label: str) -> dict[str, Any]:
    if not raw or len(raw) > maximum:
        raise RefreshError(f"{label} size is invalid")
    try:
        value = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RefreshError(f"{label} is not valid JSON") from exc
    if not isinstance(value, dict):
        raise RefreshError(f"{label} must be a JSON object")
    return value


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def _expect_fields(
    value: Mapping[str, Any],
    expected: Iterable[str],
    *,
    optional: Iterable[str] = (),
    label: str,
) -> None:
    fields = set(value)
    required = set(expected)
    allowed = required | set(optional)
    if not required.issubset(fields) or not fields.issubset(allowed):
        raise RefreshError(f"{label} contains an unknown or missing field")


def _assert_no_secret_fields(value: Any) -> None:
    if isinstance(value, dict):
        for raw_key, item in value.items():
            key = str(raw_key)
            if (
                key not in PUBLIC_KEY_FIELDS
                and SENSITIVE_FIELD_RE.search(key)
            ):
                raise RefreshError("configuration contains a prohibited secret field")
            _assert_no_secret_fields(item)
    elif isinstance(value, list):
        for item in value:
            _assert_no_secret_fields(item)


def _safe_environment() -> dict[str, str]:
    # Keep the child environment allowlisted.  In particular, do not pass
    # cloud credentials, Python import hooks, dynamic-loader options, Git
    # configuration injection, SSH agents, or wallet-related variables.
    environment = {
        "PATH": os.environ.get(
            "PATH",
            "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        ),
        "LANG": "C",
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_PROTOCOL_FROM_USER": "0",
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_NO_REPLACE_OBJECTS": "1",
    }
    # A materialized mirror can be several times larger than its encrypted
    # archive. Small hosts frequently mount /tmp as a bounded tmpfs, so permit
    # an operator to select disk-backed temporary storage without weakening the
    # otherwise allowlisted child environment. Only an existing, normalized,
    # owner-only real directory is trusted; unsafe TMPDIR values are ignored
    # and Python retains its normal fail-closed temporary-directory behavior.
    temporary_raw = os.environ.get("TMPDIR", "")
    if temporary_raw:
        temporary = Path(temporary_raw)
        try:
            info = temporary.lstat()
        except OSError:
            info = None
        if (
            temporary.is_absolute()
            and temporary == Path(os.path.normpath(str(temporary)))
            and info is not None
            and stat.S_ISDIR(info.st_mode)
            and not stat.S_ISLNK(info.st_mode)
            and info.st_uid == os.geteuid()
            and not stat.S_IMODE(info.st_mode)
            & (stat.S_IRWXG | stat.S_IRWXO)
        ):
            environment["TMPDIR"] = str(temporary)
    return environment


def _absolute_path(value: Any, label: str) -> Path:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or len(value.encode("utf-8")) > 4096
    ):
        raise RefreshError(f"{label} path is invalid")
    path = Path(value)
    if not path.is_absolute() or path != Path(os.path.normpath(str(path))):
        raise RefreshError(f"{label} path must be absolute and normalized")
    return path


def _lstat_no_symlink(path: Path, label: str) -> os.stat_result:
    try:
        info = path.lstat()
    except FileNotFoundError as exc:
        raise RefreshError(f"{label} is unavailable") from exc
    if stat.S_ISLNK(info.st_mode):
        raise RefreshError(f"{label} must not be a symbolic link")
    return info


def _reject_symlink_components(
    path: Path,
    label: str,
    *,
    allow_missing_leaf: bool = False,
) -> None:
    if not path.is_absolute():
        raise RefreshError(f"{label} path must be absolute")
    current = Path(path.anchor)
    parts = path.parts[1:]
    for index, part in enumerate(parts):
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if allow_missing_leaf and index == len(parts) - 1:
                return
            raise RefreshError(f"{label} is unavailable")
        if stat.S_ISLNK(info.st_mode):
            raise RefreshError(f"{label} path must not traverse a symbolic link")


def _require_owner_directory(path: Path, label: str) -> Path:
    _reject_symlink_components(path, label)
    info = _lstat_no_symlink(path, label)
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o077
        or stat.S_IMODE(info.st_mode) & 0o700 != 0o700
    ):
        raise RefreshError(f"{label} permissions or ownership are unsafe")
    return path


def _require_secure_parent(path: Path, label: str) -> Path:
    _reject_symlink_components(path.parent, f"{label} parent")
    return _require_owner_directory(path.parent, f"{label} parent")


def _open_bounded_file(
    path: Path,
    *,
    label: str,
    maximum: int,
    owner_only: bool,
) -> tuple[int, os.stat_result]:
    expected = _lstat_no_symlink(path, label)
    mode = stat.S_IMODE(expected.st_mode)
    if (
        not stat.S_ISREG(expected.st_mode)
        or expected.st_nlink != 1
        or expected.st_size <= 0
        or expected.st_size > maximum
        or (owner_only and expected.st_uid != os.geteuid())
        or (not owner_only and expected.st_uid not in {0, os.geteuid()})
        or mode & (stat.S_IWGRP | stat.S_IWOTH)
        or (owner_only and mode & (stat.S_IRWXG | stat.S_IRWXO))
    ):
        raise RefreshError(f"{label} permissions or ownership are unsafe")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise RefreshError(f"{label} cannot be opened safely") from exc
    actual = os.fstat(descriptor)
    if (
        actual.st_dev != expected.st_dev
        or actual.st_ino != expected.st_ino
        or actual.st_size != expected.st_size
    ):
        os.close(descriptor)
        raise RefreshError(f"{label} changed while opening")
    return descriptor, actual


def _read_bounded_file(
    path: Path,
    *,
    label: str,
    maximum: int,
    owner_only: bool,
) -> bytes:
    descriptor, info = _open_bounded_file(
        path,
        label=label,
        maximum=maximum,
        owner_only=owner_only,
    )
    try:
        chunks = bytearray()
        while len(chunks) <= maximum:
            chunk = os.read(descriptor, min(64 * 1024, maximum + 1 - len(chunks)))
            if not chunk:
                break
            chunks.extend(chunk)
        if len(chunks) != info.st_size or len(chunks) > maximum:
            raise RefreshError(f"{label} changed while reading")
        return bytes(chunks)
    finally:
        os.close(descriptor)


def _read_secure_json(
    path: Path,
    *,
    label: str,
    maximum: int,
    owner_only: bool,
) -> dict[str, Any]:
    return _parse_json(
        _read_bounded_file(
            path,
            label=label,
            maximum=maximum,
            owner_only=owner_only,
        ),
        maximum=maximum,
        label=label,
    )


def _actions_state_metadata_allowed(
    path: Path,
    info: os.stat_result,
    *,
    effective_uid: int,
    effective_gid: int,
) -> bool:
    """Accept only same-account 0600 or the fixed root-to-service lease."""

    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_nlink != 1
        or not 0 < info.st_size <= MAX_ACTIONS_STATE_BYTES
    ):
        return False
    mode = stat.S_IMODE(info.st_mode)
    if info.st_uid == effective_uid:
        return mode == 0o600
    return (
        effective_uid != 0
        and path == SYSTEM_ACTIONS_STATE_PATH
        and info.st_uid == 0
        and info.st_gid == effective_gid
        and mode == 0o640
    )


def _read_actions_state_json(path: Path) -> dict[str, Any]:
    """Read the local lease through its exact protected ownership boundary."""

    if (
        not path.is_absolute()
        or path != Path(os.path.normpath(str(path)))
        or path.name != ACTIONS_STATE_FILE
    ):
        raise RefreshError("Actions state lease path is unsafe")
    _reject_symlink_components(path, "Actions state lease")
    parent = path.parent
    parent_descriptor = -1
    descriptor = -1
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        parent_flags = (
            os.O_RDONLY
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        parent_descriptor = os.open(parent, parent_flags)
        parent_info = os.fstat(parent_descriptor)
        if (
            not stat.S_ISDIR(parent_info.st_mode)
            or parent_info.st_uid != os.geteuid()
            or parent_info.st_gid != os.getegid()
            or stat.S_IMODE(parent_info.st_mode) != 0o700
        ):
            raise RefreshError(
                "Actions state lease parent permissions or ownership are unsafe"
            )
        expected = os.stat(
            path.name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        if not _actions_state_metadata_allowed(
            path,
            expected,
            effective_uid=os.geteuid(),
            effective_gid=os.getegid(),
        ):
            raise RefreshError(
                "Actions state lease permissions or ownership are unsafe"
            )
        descriptor = os.open(
            path.name, flags, dir_fd=parent_descriptor)
    except (OSError, RefreshError) as exc:
        if descriptor >= 0:
            os.close(descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)
        if isinstance(exc, RefreshError):
            raise
        raise RefreshError(
            "Actions state lease cannot be opened safely") from exc
    try:
        actual = os.fstat(descriptor)
        if (
            actual.st_dev != expected.st_dev
            or actual.st_ino != expected.st_ino
            or not _actions_state_metadata_allowed(
                path,
                actual,
                effective_uid=os.geteuid(),
                effective_gid=os.getegid(),
            )
        ):
            raise RefreshError(
                "Actions state lease changed while opening")
        chunks = bytearray()
        while len(chunks) <= MAX_ACTIONS_STATE_BYTES:
            chunk = os.read(
                descriptor,
                min(
                    4096,
                    MAX_ACTIONS_STATE_BYTES + 1 - len(chunks),
                ),
            )
            if not chunk:
                break
            chunks.extend(chunk)
        if (
            len(chunks) != actual.st_size
            or len(chunks) > MAX_ACTIONS_STATE_BYTES
        ):
            raise RefreshError(
                "Actions state lease changed while reading")
        return _parse_json(
            bytes(chunks),
            maximum=MAX_ACTIONS_STATE_BYTES,
            label="Actions state lease",
        )
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if parent_descriptor >= 0:
            os.close(parent_descriptor)


def _require_safe_program(path: Path, label: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
        info = resolved.stat()
    except (OSError, RuntimeError) as exc:
        raise RefreshError(f"{label} is unavailable") from exc
    if (
        not stat.S_ISREG(info.st_mode)
        or not os.access(resolved, os.X_OK)
        or info.st_uid not in {0, os.geteuid()}
        or stat.S_IMODE(info.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise RefreshError(f"{label} permissions or ownership are unsafe")
    return resolved


def _require_safe_script(path: Path, label: str) -> Path:
    descriptor, _info = _open_bounded_file(
        path,
        label=label,
        maximum=8 * 1024 * 1024,
        owner_only=False,
    )
    os.close(descriptor)
    return path


def _normalize_origin(value: Any, label: str) -> str:
    try:
        parsed = urlsplit(str(value or "").strip())
        port = parsed.port
    except (TypeError, ValueError) as exc:
        raise RefreshError(f"{label} is invalid") from exc
    hostname = str(parsed.hostname or "").lower()
    if (
        parsed.scheme != "https"
        or not hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or port not in (None, 443)
        or len(hostname) > 253
        or not all(
            re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label_part)
            for label_part in hostname.split(".")
        )
        or len(hostname.split(".")) < 2
    ):
        raise RefreshError(f"{label} must be a credential-free HTTPS origin")
    return "https://" + hostname


def _clean_public_text(value: Any, *, maximum: int, label: str) -> str:
    if not isinstance(value, str):
        raise RefreshError(f"{label} must be a string")
    text = value.strip()
    if len(text) > maximum or any(char in text for char in ("\x00", "\r")):
        raise RefreshError(f"{label} is invalid")
    return text


def _parse_catalog(value: Any) -> CatalogDefaults:
    if value is None:
        value = {}
    if not isinstance(value, dict):
        raise RefreshError("catalog must be an object")
    _expect_fields(
        value,
        set(),
        optional={
            "description",
            "solana",
            "channel",
            "hostedSince",
            "branch",
            "platform",
            "version",
            "actionsEnabled",
            "reportCpu",
            "reportMemory",
            "reportDisk",
        },
        label="catalog",
    )
    description = _clean_public_text(
        value.get("description", ""), maximum=240, label="catalog.description"
    )
    solana = _clean_public_text(
        value.get("solana", ""), maximum=64, label="catalog.solana"
    )
    if solana and not SOLANA_RE.fullmatch(solana):
        raise RefreshError("catalog.solana is invalid")
    channel = _clean_public_text(
        value.get("channel", ""), maximum=120, label="catalog.channel"
    )
    hosted_since = _clean_public_text(
        value.get("hostedSince", ""), maximum=32, label="catalog.hostedSince"
    )
    if hosted_since and not re.fullmatch(r"[1-9][0-9]{0,18}", hosted_since):
        raise RefreshError("catalog.hostedSince is invalid")
    branch = _clean_public_text(
        value.get("branch", ""), maximum=120, label="catalog.branch"
    )
    platform = _clean_public_text(
        value.get("platform", "git"), maximum=16, label="catalog.platform"
    )
    version = _clean_public_text(
        value.get("version", ""), maximum=32, label="catalog.version"
    )
    actions_enabled = value.get("actionsEnabled", False)
    if not isinstance(actions_enabled, bool):
        raise RefreshError("catalog.actionsEnabled must be a boolean")
    for field in ("reportCpu", "reportMemory", "reportDisk"):
        if field in value and not isinstance(value[field], bool):
            raise RefreshError(f"catalog.{field} must be a boolean")
    return CatalogDefaults(
        description=description,
        solana=solana,
        channel=channel,
        hosted_since=hosted_since,
        branch=branch,
        platform=platform,
        version=version,
        actions_enabled=actions_enabled,
        report_cpu=value.get("reportCpu", False),
        report_memory=value.get("reportMemory", False),
        report_disk=value.get("reportDisk", False),
    )


def load_config(path: Path) -> RefreshConfig:
    path = _absolute_path(str(path), "configuration")
    _reject_symlink_components(path, "configuration")
    source = _read_secure_json(
        path,
        label="configuration",
        maximum=MAX_CONFIG_BYTES,
        owner_only=True,
    )
    _assert_no_secret_fields(source)
    _expect_fields(
        source,
        {
            "schemaVersion",
            "type",
            "sourceRepository",
            "archiveDirectory",
            "gatewayConfigPath",
            "identityStateDirectory",
            "identityHelperPath",
            "mirrorGatewayPath",
            "pythonProgram",
            "gitProgram",
            "manifestPath",
            "nodeOwner",
            "repositoryName",
            "ownerAliases",
            "workerOrigin",
            "publicOrigin",
            "listen",
            "operations",
        },
        optional={
            "catalog",
            "releaseStore",
            "mirrorUrl",
            "writableRepositoryRoot",
            "writableOwners",
            "receiveMaxInputBytes",
        },
        label="configuration",
    )
    if (
        source.get("schemaVersion") != SCHEMA_VERSION
        or source.get("type") != CONFIG_TYPE
    ):
        raise RefreshError("configuration protocol is unsupported")

    source_repository = _absolute_path(
        source.get("sourceRepository"), "source repository"
    )
    archive_directory = _absolute_path(
        source.get("archiveDirectory"), "archive directory"
    )
    release_store_value = source.get("releaseStore")
    release_store = (
        _absolute_path(release_store_value, "release store")
        if release_store_value is not None
        else None
    )
    gateway_config_path = _absolute_path(
        source.get("gatewayConfigPath"), "gateway configuration"
    )
    identity_state_directory = _absolute_path(
        source.get("identityStateDirectory"), "identity state directory"
    )
    identity_helper_path = _absolute_path(
        source.get("identityHelperPath"), "identity helper"
    )
    mirror_gateway_path = _absolute_path(
        source.get("mirrorGatewayPath"), "mirror gateway"
    )
    python_program = _absolute_path(source.get("pythonProgram"), "Python program")
    git_program = _absolute_path(source.get("gitProgram"), "Git program")
    manifest_path = _absolute_path(source.get("manifestPath"), "mirror manifest")

    _require_owner_directory(archive_directory, "archive directory")
    if release_store is not None:
        _require_owner_directory(release_store, "release store")
    _require_owner_directory(identity_state_directory, "identity state directory")
    _require_secure_parent(gateway_config_path, "gateway configuration")
    _reject_symlink_components(source_repository, "source repository")
    _reject_symlink_components(identity_helper_path, "identity helper")
    _reject_symlink_components(mirror_gateway_path, "mirror gateway")
    _reject_symlink_components(manifest_path, "mirror manifest")
    python_program = _require_safe_program(python_program, "Python program")
    git_program = _require_safe_program(git_program, "Git program")
    _require_safe_script(identity_helper_path, "identity helper")
    _require_safe_script(mirror_gateway_path, "mirror gateway")
    manifest_descriptor, _ = _open_bounded_file(
        manifest_path,
        label="mirror manifest",
        maximum=128 * 1024,
        owner_only=False,
    )
    os.close(manifest_descriptor)

    node_owner = str(source.get("nodeOwner") or "").strip().lower()
    repository_name = str(source.get("repositoryName") or "").strip()
    if not NODE_RE.fullmatch(node_owner):
        raise RefreshError("nodeOwner is invalid")
    if not REPOSITORY_RE.fullmatch(repository_name):
        raise RefreshError("repositoryName is invalid")
    aliases_value = source.get("ownerAliases")
    if (
        not isinstance(aliases_value, list)
        or not 1 <= len(aliases_value) <= 16
        or any(not isinstance(item, str) for item in aliases_value)
    ):
        raise RefreshError("ownerAliases must be a bounded string array")
    owner_aliases = tuple(str(item).strip().lower() for item in aliases_value)
    if (
        any(not NODE_RE.fullmatch(item) for item in owner_aliases)
        or len(set(owner_aliases)) != len(owner_aliases)
        or node_owner not in owner_aliases
    ):
        raise RefreshError(
            "ownerAliases must be unique valid owners and include nodeOwner"
        )

    listen = source.get("listen")
    if not isinstance(listen, dict):
        raise RefreshError("listen must be an object")
    _expect_fields(listen, {"host", "port"}, label="listen")
    listen_host = str(listen.get("host") or "")
    if listen_host not in {"127.0.0.1", "::1", "localhost"}:
        raise RefreshError("listen.host must be loopback")
    port_value = listen.get("port")
    if isinstance(port_value, bool):
        raise RefreshError("listen.port is invalid")
    try:
        listen_port = int(port_value)
    except (TypeError, ValueError) as exc:
        raise RefreshError("listen.port is invalid") from exc
    if not 1 <= listen_port <= 65535:
        raise RefreshError("listen.port is outside 1..65535")

    operations_value = source.get("operations")
    if (
        not isinstance(operations_value, list)
        or not operations_value
        or len(operations_value) > len(PUBLIC_OPERATIONS)
        or any(not isinstance(item, str) for item in operations_value)
    ):
        raise RefreshError("operations must be a bounded string array")
    operations = tuple(operations_value)
    if (
        len(set(operations)) != len(operations)
        or not set(operations).issubset(PUBLIC_OPERATIONS)
        or not REQUIRED_ROUTING_OPERATIONS.issubset(operations)
    ):
        raise RefreshError("operations do not satisfy the public routing contract")
    if "actions-control" in operations and "actions-status" not in operations:
        # The gateway refuses the same combination: a control request is only
        # useful where the summary that reports its outcome is served.
        raise RefreshError("actions-control requires actions-status")

    mirror_url = (
        _normalize_origin(source.get("mirrorUrl"), "mirrorUrl")
        if "mirrorUrl" in source else ""
    )
    # Push hosting. Absent keys keep the historical read-only node: no
    # writable root is rendered, no sidecar is consulted, and no repository
    # can be given git-receive-pack.
    writable_root_value = source.get("writableRepositoryRoot")
    writable_repository_root = None
    if writable_root_value is not None:
        writable_repository_root = _require_owner_directory(
            _absolute_path(writable_root_value, "writable repository root"),
            "writable repository root",
        )
    owners_value = source.get("writableOwners", [])
    if (
        not isinstance(owners_value, list)
        or len(owners_value) > 64
        or any(not isinstance(item, str) for item in owners_value)
    ):
        raise RefreshError("writableOwners must be a bounded string array")
    writable_owners = tuple(str(item).strip().lower() for item in owners_value)
    # The wildcard accepts every owner the control plane authorized, which is
    # what a node hosting website-created repositories needs (issue #676): it
    # holds repositories for accounts it can never enumerate. Exclusive, so a
    # mixed list can never read as narrower than it is.
    if WRITABLE_OWNERS_WILDCARD in writable_owners:
        if len(writable_owners) != 1:
            raise RefreshError(
                "writableOwners wildcard must be the only entry")
    elif (
        any(not NODE_RE.fullmatch(item) for item in writable_owners)
        or len(set(writable_owners)) != len(writable_owners)
    ):
        raise RefreshError("writableOwners must be unique valid owner names")
    if writable_owners and writable_repository_root is None:
        raise RefreshError("writableOwners requires writableRepositoryRoot")
    receive_max_value = source.get("receiveMaxInputBytes")
    receive_max_input_bytes = None
    if receive_max_value is not None:
        if (
            isinstance(receive_max_value, bool)
            or not isinstance(receive_max_value, int)
            or not (
                MIN_RECEIVE_MAX_INPUT_BYTES
                <= receive_max_value
                <= MAX_RECEIVE_MAX_INPUT_BYTES
            )
        ):
            raise RefreshError(
                "receiveMaxInputBytes is outside the safe range")
        if writable_repository_root is None:
            raise RefreshError(
                "receiveMaxInputBytes requires writableRepositoryRoot")
        receive_max_input_bytes = receive_max_value
    return RefreshConfig(
        config_path=path,
        source_repository=source_repository,
        archive_directory=archive_directory,
        release_store=release_store,
        gateway_config_path=gateway_config_path,
        identity_state_directory=identity_state_directory,
        identity_helper_path=identity_helper_path,
        mirror_gateway_path=mirror_gateway_path,
        python_program=python_program,
        git_program=git_program,
        manifest_path=manifest_path,
        node_owner=node_owner,
        repository_name=repository_name,
        owner_aliases=owner_aliases,
        worker_origin=_normalize_origin(source.get("workerOrigin"), "workerOrigin"),
        public_origin=_normalize_origin(source.get("publicOrigin"), "publicOrigin"),
        mirror_url=mirror_url,
        listen_host=listen_host,
        listen_port=listen_port,
        operations=operations,
        catalog=_parse_catalog(source.get("catalog")),
        writable_repository_root=writable_repository_root,
        writable_owners=writable_owners,
        receive_max_input_bytes=receive_max_input_bytes,
    )


@contextmanager
def _refresh_lock(config: RefreshConfig, *, shared: bool) -> Iterator[None]:
    path = config.identity_state_directory / ".refresh.lock"
    # ``check`` is used inside a ProtectSystem=strict gateway unit.  It must be
    # genuinely read-only: the preceding refresh creates the lock, and a
    # shared flock works on an O_RDONLY descriptor.  Refresh/register retain
    # the writable create path and exclusive lock.
    flags = os.O_RDONLY if shared else os.O_RDWR | os.O_CREAT
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as exc:
        raise RefreshError("refresh lock cannot be opened safely") from exc
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) & 0o077
        ):
            raise RefreshError("refresh lock permissions or ownership are unsafe")
        fcntl.flock(descriptor, fcntl.LOCK_SH if shared else fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _run_bounded(
    command: list[str],
    *,
    input_bytes: bytes | None = None,
    timeout: int = PROCESS_TIMEOUT_SECONDS,
    maximum_output: int = MAX_JSON_OUTPUT_BYTES,
) -> bytes:
    with tempfile.TemporaryFile(mode="w+b") as output:
        try:
            completed = subprocess.run(
                command,
                input=input_bytes,
                stdout=output,
                stderr=subprocess.DEVNULL,
                env=_safe_environment(),
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise RefreshError("required local operation failed") from exc
        if completed.returncode != 0:
            raise RefreshError("required local operation failed")
        size = output.tell()
        if size > maximum_output:
            raise RefreshError("required local operation returned too much data")
        output.seek(0)
        return output.read(maximum_output + 1)


def _helper_call(
    config: RefreshConfig,
    mode: str,
    request: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    input_bytes = (
        _canonical_json(request) + b"\n" if request is not None else None
    )
    raw = _run_bounded(config.helper_command(mode), input_bytes=input_bytes)
    response = _parse_json(
        raw,
        maximum=MAX_JSON_OUTPUT_BYTES,
        label="identity helper response",
    )
    _assert_no_secret_fields(response)
    return response


def _load_public_identity(config: RefreshConfig) -> PublicIdentity:
    response = _helper_call(config, "public-info")
    _expect_fields(
        response,
        {
            "schemaVersion",
            "type",
            "nodeName",
            "nodePublicKey",
            "ageRecipient",
            "ageKeyReference",
            "routerPublicKey",
            "allowedOrigins",
        },
        label="public identity response",
    )
    node = str(response.get("nodeName") or "").strip().lower()
    node_public_key = _validate_public_key(response.get("nodePublicKey"))
    router_public_key = _validate_public_key(response.get("routerPublicKey"))
    origins_value = response.get("allowedOrigins")
    if (
        response.get("schemaVersion") != 1
        or response.get("type") != "forkmesh.headless-mirror-identity"
        or node != config.node_owner
        or not isinstance(origins_value, list)
        or any(not isinstance(item, str) for item in origins_value)
    ):
        raise RefreshError("public identity does not match refresh configuration")
    origins = tuple(
        _normalize_origin(item, "identity allowed origin")
        for item in origins_value
    )
    if config.public_origin not in origins:
        raise RefreshError("public origin is not pinned by the node identity")
    if config.mirror_url and config.mirror_url not in origins:
        raise RefreshError("mirror URL is not pinned by the node identity")
    return PublicIdentity(node, node_public_key, router_public_key, origins)


def _validate_public_key(value: Any) -> str:
    if not isinstance(value, str) or not BASE64URL_RE.fullmatch(value):
        raise RefreshError("public identity key is invalid")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except (TypeError, ValueError) as exc:
        raise RefreshError("public identity key is invalid") from exc
    if (
        len(decoded) != 32
        or base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=") != value
    ):
        raise RefreshError("public identity key is invalid")
    return value


def _git_prefix(config: RefreshConfig) -> list[str]:
    return [
        str(config.git_program),
        "-c",
        "core.alternateRefsCommand=/usr/bin/true",
        "-c",
        "core.hooksPath=" + os.devnull,
        "-c",
        "core.fsmonitor=",
        "-c",
        "credential.helper=",
        "-c",
        "merge.default=text",
        "-c",
        "protocol.ext.allow=never",
        f"--git-dir={config.source_repository}",
    ]


def _require_bare_source(config: RefreshConfig) -> None:
    info = _lstat_no_symlink(config.source_repository, "bare source repository")
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & stat.S_IWOTH
    ):
        raise RefreshError("bare source repository permissions or ownership are unsafe")
    bare = _run_bounded(
        _git_prefix(config) + ["rev-parse", "--is-bare-repository"],
        timeout=60,
    )
    if bare.strip() != b"true":
        raise RefreshError("source repository is not a bare Git repository")


def _fsck_source(config: RefreshConfig) -> None:
    _require_bare_source(config)
    # A power loss may interrupt the narrow interval between installing
    # transaction-owned loose objects and anchoring them. Replay only the
    # owner-only, digest-named merge journals before fsck sees the object store.
    _recover_merge_quarantines(config)
    _run_bounded(
        _git_prefix(config)
        + [
            "fsck",
            "--full",
            "--strict",
            "--no-progress",
            "--no-dangling",
        ]
    )


def _repository_refs_sha256(
    config: RefreshConfig,
    repository: Path,
) -> str:
    raw = _run_bounded(
        [
            str(config.git_program),
            "--no-pager",
            "--git-dir",
            str(repository),
        ]
        + [
            "for-each-ref",
            "--sort=refname",
            "--format=%(objectname) %(refname)",
            "refs/heads/",
            "refs/tags/",
        ],
        maximum_output=16 * 1024 * 1024,
    )
    try:
        canonical = "\n".join(
            line for line in raw.decode("utf-8").splitlines() if line
        )
    except UnicodeDecodeError as exc:
        raise RefreshError("source repository refs are invalid") from exc
    if len(canonical.encode("utf-8")) > 16 * 1024 * 1024:
        raise RefreshError("source repository has too many public refs")
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _source_refs_sha256(config: RefreshConfig) -> str:
    return _repository_refs_sha256(config, config.source_repository)


def _source_storage_bytes(config: RefreshConfig) -> int:
    """Return Git's inexpensive packed + loose object storage estimate."""
    raw = _run_bounded(
        _git_prefix(config) + ["count-objects", "-v"],
        maximum_output=4096,
        timeout=60,
    )
    kibibytes = 0
    for line in raw.decode("ascii", errors="strict").splitlines():
        key, separator, value = line.partition(":")
        if separator and key in {"size", "size-pack", "size-garbage"}:
            try:
                kibibytes += max(0, int(value.strip()))
            except ValueError as exc:
                raise RefreshError("source repository size is invalid") from exc
    return kibibytes * 1024


def _source_branch_commit(config: RefreshConfig) -> str:
    revision = (
        "refs/heads/" + config.catalog.branch
        if config.catalog.branch
        else "HEAD"
    )
    raw = _run_bounded(
        _git_prefix(config)
        + [
            "rev-parse",
            "--verify",
            "--end-of-options",
            revision + "^{commit}",
        ],
        maximum_output=256,
        timeout=60,
    )
    try:
        commit = raw.decode("ascii").strip().lower()
    except UnicodeDecodeError as exc:
        raise RefreshError("source repository commit is invalid") from exc
    if not GIT_OBJECT_ID_RE.fullmatch(commit):
        raise RefreshError("source repository commit is invalid")
    return commit


def _source_commit_identity(config: RefreshConfig) -> dict[str, str]:
    """Subject, author and date of the published head commit.

    Repository content is untrusted: the subject and author are bounded, and any
    control character (a crafted commit could carry newlines or an ANSI escape)
    is dropped before the values reach the signed catalog record. A commit that
    can't be read leaves every field absent rather than publishing a blank.
    """
    try:
        raw = _run_bounded(
            _git_prefix(config)
            + [
                "show",
                "-s",
                "--format=%s%n%an%n%ct",
                _source_revision(config),
                "--",
            ],
            maximum_output=8 * 1024,
            timeout=60,
        )
        lines = raw.decode("utf-8", "replace").split("\n")
    except (RefreshError, UnicodeDecodeError):
        return {}
    if len(lines) < 3:
        return {}

    def sanitized(value: str, maximum: int) -> str:
        text = "".join(
            " " if character < " " or character == "\x7f" else character
            for character in value
        )
        return " ".join(text.split())[:maximum]

    result: dict[str, str] = {}
    subject = sanitized(lines[0], 120)
    if subject:
        result["commitSubject"] = subject
    author = sanitized(lines[1], 64)
    if author:
        result["commitAuthorName"] = author
    try:
        committed_at = int(lines[2].strip())
    except (TypeError, ValueError):
        committed_at = 0
    if 0 < committed_at < 1 << 34:
        result["commitAt"] = str(committed_at * 1000)
    return result


def _source_revision(config: RefreshConfig) -> str:
    return (
        "refs/heads/" + config.catalog.branch
        if config.catalog.branch
        else "HEAD"
    )


def _bounded_repository_count(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError):
        return None
    if number < 0 or number > MAX_PUBLIC_REPOSITORY_COUNT:
        return None
    return number


def _source_commit_count(config: RefreshConfig) -> int | None:
    try:
        raw = _run_bounded(
            _git_prefix(config)
            + ["rev-list", "--count", _source_revision(config), "--"],
            maximum_output=64,
            timeout=60,
        )
        return _bounded_repository_count(raw.decode("ascii").strip())
    except (RefreshError, UnicodeDecodeError):
        return None


def _source_merge_count(config: RefreshConfig) -> int | None:
    try:
        raw = _run_bounded(
            _git_prefix(config)
            + [
                "rev-list",
                "--count",
                "--merges",
                _source_revision(config),
                "--",
            ],
            maximum_output=64,
            timeout=60,
        )
        return _bounded_repository_count(raw.decode("ascii").strip())
    except (RefreshError, UnicodeDecodeError):
        return None


def _source_activity_weeks(
    config: RefreshConfig, *, merges_only: bool = False,
) -> list[int] | None:
    """Return bounded 52-week Git activity without exposing commit content."""
    weeks = 52
    week_seconds = 7 * 24 * 60 * 60
    command = _git_prefix(config) + [
        "log",
        "--since=52 weeks ago",
        "--format=%ct",
    ]
    if merges_only:
        command.append("--merges")
    command.extend([_source_revision(config), "--"])
    try:
        raw = _run_bounded(
            command,
            maximum_output=MAX_REPOSITORY_METADATA_BYTES,
            timeout=60,
        )
    except RefreshError:
        return None

    buckets = [0] * weeks
    now = int(time.time())
    start = now - weeks * week_seconds
    for raw_line in raw.splitlines():
        try:
            timestamp = int(raw_line.decode("ascii").strip())
        except (UnicodeDecodeError, ValueError):
            continue
        index = max(0, min(weeks - 1, (timestamp - start) // week_seconds))
        buckets[index] = min(MAX_PUBLIC_REPOSITORY_COUNT,
                             buckets[index] + 1)
    return buckets


def _source_branch_count(config: RefreshConfig) -> int | None:
    try:
        raw = _run_bounded(
            _git_prefix(config)
            + [
                "for-each-ref",
                "--format=%(refname)",
                "refs/heads/",
            ],
            maximum_output=MAX_REPOSITORY_METADATA_BYTES,
            timeout=60,
        )
    except RefreshError:
        return None
    lines = [line for line in raw.splitlines() if line]
    if len(lines) > MAX_REPOSITORY_METADATA_PATHS:
        return None
    return _bounded_repository_count(len(lines))


def _source_revision_exists(config: RefreshConfig, revision: str) -> bool:
    try:
        _run_bounded(
            _git_prefix(config)
            + [
                "rev-parse",
                "--verify",
                "--quiet",
                "--end-of-options",
                revision + "^{commit}",
            ],
            maximum_output=128,
            timeout=60,
        )
        return True
    except RefreshError:
        return False


def _source_tree_paths(
    config: RefreshConfig,
    revision: str,
    path: str,
) -> tuple[bytes, ...] | None:
    """Return bounded raw paths below one repository metadata directory.

    ``git ls-tree`` exits successfully with no output when the path does not
    exist, which lets a genuine empty count stay distinct from a failed read.
    Paths remain bytes: Git permits non-UTF-8 names, while the metadata records
    selected below have strict ASCII names.
    """
    try:
        raw = _run_bounded(
            _git_prefix(config)
            + [
                "ls-tree",
                "-r",
                "-z",
                "--name-only",
                revision,
                "--",
                path,
            ],
            maximum_output=MAX_REPOSITORY_METADATA_BYTES,
            timeout=60,
        )
    except RefreshError:
        return None
    paths = tuple(item for item in raw.split(b"\0") if item)
    if len(paths) > MAX_REPOSITORY_METADATA_PATHS:
        return None
    return paths


def _numbered_metadata_count(
    paths: tuple[bytes, ...] | None,
    prefix: bytes,
) -> int | None:
    if paths is None:
        return None
    pattern = re.compile(
        rb"^" + re.escape(prefix) + rb"/([1-9][0-9]*)/"
    )
    numbers: set[int] = set()
    for path in paths:
        match = pattern.match(path)
        if match:
            number = _bounded_repository_count(match.group(1))
            if number is None:
                return None
            numbers.add(number)
    return _bounded_repository_count(len(numbers))


def _source_blob_batch(
    config: RefreshConfig,
    revision: str,
    paths: tuple[bytes, ...],
) -> dict[bytes, bytes] | None:
    if not paths:
        return {}
    requests = b"".join(
        revision.encode("ascii") + b":" + path + b"\n" for path in paths
    )
    try:
        raw = _run_bounded(
            _git_prefix(config) + ["cat-file", "--batch"],
            input_bytes=requests,
            maximum_output=MAX_REPOSITORY_METADATA_BYTES,
            timeout=60,
        )
    except (RefreshError, UnicodeEncodeError):
        return None
    result: dict[bytes, bytes] = {}
    position = 0
    for path in paths:
        line_end = raw.find(b"\n", position)
        if line_end < 0:
            return None
        header = raw[position:line_end].split()
        position = line_end + 1
        if len(header) != 3 or header[1] != b"blob":
            return None
        try:
            size = int(header[2])
        except ValueError:
            return None
        if size < 0 or size > MAX_ISSUE_RECORD_BYTES:
            return None
        content_end = position + size
        if content_end >= len(raw) or raw[content_end:content_end + 1] != b"\n":
            return None
        result[path] = raw[position:content_end]
        position = content_end + 1
    if position != len(raw):
        return None
    return result


def _issue_record_is_tombstoned(record: Mapping[str, Any]) -> bool | None:
    events = record.get("events", [])
    if not isinstance(events, list):
        return None
    creator = ""
    for value in events:
        if not isinstance(value, dict):
            return None
        if value.get("type") == "open" and not creator:
            author = value.get("author")
            if isinstance(author, str):
                creator = author
    if not creator:
        return False
    return any(
        value.get("type") == "delete"
        and value.get("target") == "self"
        and value.get("author") == creator
        for value in events
    )


def _source_issue_counts(
    config: RefreshConfig,
) -> tuple[int | None, int | None] | None:
    revision = _source_revision(config)
    paths = _source_tree_paths(
        config, revision, ".forkmesh/issues"
    )
    if paths is None:
        return None
    # Qt derives the allocation high-water mark from numeric directories, not
    # only from a canonical issue-N.json blob. Do the same so a damaged or
    # partially migrated issue directory can never make ForkBot reuse an
    # already occupied number. Recursive ls-tree does not report directories,
    # so any descendant path proves that the corresponding tree exists.
    directory_pattern = re.compile(
        rb"^\.forkmesh/issues/(?:(open|closed)/)?([0-9]+)/"
    )
    directories: dict[str, set[bytes]] = {
        "open": set(),
        "closed": set(),
        "legacy": set(),
    }
    max_number = 0
    for path in paths:
        match = directory_pattern.match(path)
        if not match:
            continue
        state = (
            match.group(1).decode("ascii")
            if match.group(1)
            else "legacy"
        )
        name = match.group(2)
        number = _bounded_repository_count(name)
        if number is None:
            return None
        directories[state].add(name)
        max_number = max(max_number, number)
    bounded_max = _bounded_repository_count(max_number)
    if bounded_max is None:
        return None

    # Match the desktop reader's precedence: an open/<name> tree is examined
    # first, closed/<name> reserves the same legacy name, and only a remaining
    # pre-split root tree is read. A missing or malformed canonical record is
    # conservatively live, just as the Qt issue list does.
    records: list[tuple[str, bytes]] = []
    counted_names = set(directories["open"])
    for name in sorted(directories["open"]):
        records.append(
            (
                "open",
                b".forkmesh/issues/open/"
                + name
                + b"/issue-"
                + name
                + b".json",
            )
        )
    counted_names.update(directories["closed"])
    for name in sorted(directories["legacy"] - counted_names):
        records.append(
            (
                "legacy",
                b".forkmesh/issues/"
                + name
                + b"/issue-"
                + name
                + b".json",
            )
        )

    available_paths = set(paths)
    readable_paths = tuple(
        path for _state, path in records if path in available_paths
    )
    blobs = _source_blob_batch(config, revision, readable_paths)
    if blobs is None:
        return None, bounded_max
    open_count = 0
    for state, path in records:
        if path not in blobs:
            open_count += 1
            continue
        try:
            record = json.loads(blobs[path].decode("utf-8"))
        except Exception:
            # JSON recursion limits and malformed/unreadable issue records must
            # not take down catalog renewal. Treat them as live, matching the
            # desktop's conservative default, without trusting any tombstone.
            open_count += 1
            continue
        if not isinstance(record, dict):
            open_count += 1
            continue
        if state == "open":
            tombstoned = _issue_record_is_tombstoned(record)
            if tombstoned is not True:
                open_count += 1
        elif record.get("status") != "closed":
            open_count += 1
    bounded_open = _bounded_repository_count(open_count)
    if bounded_open is None:
        return None, bounded_max
    return bounded_open, bounded_max


def _source_pull_count(config: RefreshConfig) -> int | None:
    revision = (
        "refs/heads/forkmesh/pulls"
        if _source_revision_exists(config, "refs/heads/forkmesh/pulls")
        else _source_revision(config)
    )
    return _numbered_metadata_count(
        _source_tree_paths(config, revision, ".forkmesh/pulls"),
        b".forkmesh/pulls",
    )


def _source_discussion_count(config: RefreshConfig) -> int | None:
    return _numbered_metadata_count(
        _source_tree_paths(
            config,
            _source_revision(config),
            ".forkmesh/discussions",
        ),
        b".forkmesh/discussions",
    )


def _source_artifact_count(config: RefreshConfig) -> int | None:
    """Count real release blobs in the configured mirror gateway CAS."""
    if config.release_store is None:
        return None
    root = config.release_store / "sha256"
    try:
        root_info = root.lstat()
    except FileNotFoundError:
        return 0
    except OSError:
        return None
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
    ):
        return None
    count = 0
    visited = 0
    try:
        with os.scandir(root) as shards:
            for shard in shards:
                visited += 1
                if visited > MAX_REPOSITORY_METADATA_PATHS:
                    return None
                if (
                    not re.fullmatch(r"[0-9a-f]{2}", shard.name)
                    or not shard.is_dir(follow_symlinks=False)
                ):
                    continue
                with os.scandir(shard.path) as hashes:
                    for digest in hashes:
                        visited += 1
                        if visited > MAX_REPOSITORY_METADATA_PATHS:
                            return None
                        if (
                            not SHA256_RE.fullmatch(digest.name)
                            or not digest.name.startswith(shard.name)
                            or not digest.is_dir(follow_symlinks=False)
                        ):
                            continue
                        data = Path(digest.path) / "data"
                        try:
                            if data.is_file() and not data.is_symlink():
                                count += 1
                        except OSError:
                            return None
    except OSError:
        return None
    return _bounded_repository_count(count)


def _sample_repository_statistics(
    config: RefreshConfig,
) -> dict[str, Any]:
    """Read public, repository-scoped facts from the validated bare source.

    Each fact fails independently. Gateway service counters are content-free,
    repository-scoped totals persisted beside the protected gateway config.
    Their absence remains unknown rather than becoming a misleading zero.
    """
    result: dict[str, Any] = {}

    def include(field: str, value: int | None) -> None:
        if value is not None:
            result[field] = str(value)

    def sample(field: str, sampler: Callable[[], int | None]) -> None:
        try:
            include(field, sampler())
        except Exception:
            # Repository metadata is untrusted input. One malformed or unusually
            # deep fact must remain unknown without suppressing all other
            # independently readable statistics or breaking lease renewal.
            return

    def sample_series(
        field: str, sampler: Callable[[], list[int] | None],
    ) -> None:
        try:
            value = sampler()
            if value is not None:
                result[field] = value
        except Exception:
            return

    sample("commitCount", lambda: _source_commit_count(config))
    sample("mergeCount", lambda: _source_merge_count(config))
    sample_series(
        "activityWeeks", lambda: _source_activity_weeks(config))
    sample_series(
        "mergeActivityWeeks",
        lambda: _source_activity_weeks(config, merges_only=True),
    )
    sample("branchCount", lambda: _source_branch_count(config))
    try:
        issue_counts = _source_issue_counts(config)
    except Exception:
        issue_counts = None
    if issue_counts is not None:
        include("issueCount", issue_counts[0])
        include("issueMaxNumber", issue_counts[1])
    sample("pullCount", lambda: _source_pull_count(config))
    sample("discussionCount", lambda: _source_discussion_count(config))
    sample("artifactCount", lambda: _source_artifact_count(config))
    try:
        result.update(_sample_gateway_service_counters(config))
    except Exception:
        pass
    try:
        result.update(_source_commit_identity(config))
    except Exception:
        # Same independence rule as the counts above: an unreadable commit
        # message must not cost the mirror its lease renewal.
        pass
    return result


def _sample_gateway_service_counters(
    config: RefreshConfig,
) -> dict[str, str]:
    path = config.gateway_config_path.parent / SERVICE_COUNTERS_FILE
    try:
        info = path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_nlink != 1
            or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) != 0o600
            or not 0 < info.st_size <= MAX_SERVICE_COUNTERS_BYTES
        ):
            return {}
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return {}
    if (
        not isinstance(value, dict)
        or set(value) != {"schemaVersion", "type", "repositories"}
        or value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != SERVICE_COUNTERS_TYPE
        or not isinstance(value.get("repositories"), dict)
    ):
        return {}
    key = config.node_owner.lower() + "/" + config.repository_name.lower()
    row = value["repositories"].get(key)
    if (
        not isinstance(row, dict)
        or not {"clonesServed", "websiteServed", "updatedAt"} <= set(row)
        or not set(row) <= SERVICE_COUNTER_FIELDS
    ):
        return {}
    result: dict[str, str] = {}
    for field in ("clonesServed", "websiteServed"):
        raw = row.get(field)
        if isinstance(raw, bool):
            return {}
        try:
            count = int(raw)
        except (TypeError, ValueError, OverflowError):
            return {}
        if not 0 <= count <= MAX_SERVICE_COUNTER:
            return {}
        result[field] = str(count)
    # When the last clone / website read was served, and to which class of
    # client. Optional: an older gateway's counters file carries totals only,
    # and those stay unknown rather than being back-dated to the totals' row.
    for field in ("cloneServedAt", "websiteServedAt"):
        raw = row.get(field)
        if raw is None or isinstance(raw, bool):
            continue
        try:
            stamp = int(raw)
        except (TypeError, ValueError, OverflowError):
            continue
        if 0 < stamp <= MAX_SAFE_JSON_INTEGER:
            result[field] = str(stamp)
    for field in ("cloneServedAgent", "websiteServedAgent"):
        raw = row.get(field)
        if isinstance(raw, str) and raw in SERVICE_COUNTER_AGENT_CLASSES:
            result[field] = raw
    return result


def _validate_seal_response(value: Mapping[str, Any]) -> SealMetadata:
    _expect_fields(
        value,
        {
            "ok",
            "scheme",
            "ciphertextSha256",
            "ciphertextBytes",
            "keyReference",
            "expectedRefsSha256",
        },
        label="repository seal response",
    )
    digest = str(value.get("ciphertextSha256") or "").lower()
    refs = str(value.get("expectedRefsSha256") or "").lower()
    key_reference = str(value.get("keyReference") or "")
    size_value = value.get("ciphertextBytes")
    if isinstance(size_value, bool):
        raise RefreshError("repository seal response is invalid")
    try:
        size = int(size_value)
    except (TypeError, ValueError) as exc:
        raise RefreshError("repository seal response is invalid") from exc
    if (
        value.get("ok") is not True
        or value.get("scheme") != "age-encrypted-tar-v1"
        or not SHA256_RE.fullmatch(digest)
        or not SHA256_RE.fullmatch(refs)
        or not KEY_REFERENCE_RE.fullmatch(key_reference)
        or not 1 <= size <= MAX_ARCHIVE_BYTES
    ):
        raise RefreshError("repository seal response is invalid")
    return SealMetadata(digest, size, key_reference, refs)


def _validate_archive(
    config: RefreshConfig,
    path: Path,
    *,
    digest: str,
    size: int | None = None,
) -> None:
    if (
        path.parent != config.archive_directory
        or not SHA256_RE.fullmatch(digest)
    ):
        raise RefreshError("encrypted archive permissions or metadata are unsafe")
    descriptor, info = _open_bounded_file(
        path,
        label="encrypted archive",
        maximum=MAX_ARCHIVE_BYTES,
        owner_only=True,
    )
    try:
        if (
            info.st_size <= len(AGE_NATIVE_HEADER)
            or (size is not None and info.st_size != size)
        ):
            raise RefreshError(
                "encrypted archive permissions or metadata are unsafe"
            )
        actual = hashlib.sha256()
        header_size = max(len(AGE_NATIVE_HEADER), len(AGE_ARMORED_HEADER))
        header = os.read(descriptor, header_size)
        actual.update(header)
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            actual.update(chunk)
        if not (
            header.startswith(AGE_NATIVE_HEADER)
            or header.startswith(AGE_ARMORED_HEADER)
        ):
            raise RefreshError("encrypted archive is not an age file")
        if not secrets.compare_digest(actual.hexdigest(), digest):
            raise RefreshError("encrypted archive digest does not match")
    except OSError as exc:
        raise RefreshError("encrypted archive cannot be read safely") from exc
    finally:
        os.close(descriptor)


def _writable_owner_allowed(config: "RefreshConfig", owner: str) -> bool:
    """Whether one owner's push-created repositories may be rendered."""
    allowlist = config.writable_owners
    return WRITABLE_OWNERS_WILDCARD in allowlist or owner in allowlist


def _derived_repository_operations(config: RefreshConfig) -> list[str]:
    """Read operations a repository other than the node's own may serve.

    ``config.operations`` can never contain ``git-receive-pack``: the loader
    restricts it to ``PUBLIC_OPERATIONS``, which is read-only by construction.
    A derived repository additionally has no release store, no merge executor
    binding, and no Actions summary, so those operations are dropped here.
    """
    return [
        operation
        for operation in config.operations
        if operation not in DERIVED_REPOSITORY_EXCLUDED_OPERATIONS
    ]


def _pushed_gateway_repositories(
    config: RefreshConfig,
    taken: frozenset[tuple[str, str]],
) -> list[dict[str, Any]]:
    """Render repositories created by an authorized first push.

    ``hosted-pushes.json`` is machine state written by the gateway process, not
    operator state. It is therefore treated as hostile and consumed without
    ever failing the refresh of the node's own mirror: a missing, unsafe,
    oversized, mistyped, or unparseable sidecar renders nothing at all, and one
    unusable entry is skipped rather than aborting the rest. Every surviving
    entry must name an explicitly allowlisted owner and resolve to the exact
    ``<writableRepositoryRoot>/<owner>/<name>.git`` real directory the gateway
    would itself create, so a rewritten sidecar can neither publish a
    repository outside the writable root nor grant a write operation to any
    repository the operator did not opt in.
    """
    root = config.writable_repository_root
    if root is None or not config.writable_owners:
        return []
    path = config.gateway_config_path.parent / HOSTED_PUSHES_FILE
    try:
        source = _read_secure_json(
            path,
            label="hosted pushes",
            maximum=MAX_HOSTED_PUSHES_BYTES,
            owner_only=True,
        )
    except (RefreshError, OSError):
        return []
    items = source.get("repositories") if isinstance(source, dict) else None
    if (
        not isinstance(source, dict)
        or set(source) - {"schemaVersion", "type", "repositories"}
        or source.get("schemaVersion") != SCHEMA_VERSION
        or source.get("type") != HOSTED_PUSHES_TYPE
        or not isinstance(items, list)
        or len(items) > MAX_HOSTED_PUSHES
    ):
        return []
    try:
        writable_root = _require_owner_directory(
            root, "writable repository root"
        ).resolve(strict=True)
    except (RefreshError, OSError):
        return []
    if writable_root != root:
        return []
    operations = sorted(
        set(_derived_repository_operations(config))
        | {PUSHED_REPOSITORY_WRITE_OPERATION}
    )
    seen = set(taken)
    output: list[dict[str, Any]] = []
    for item in items:
        if (
            not isinstance(item, dict)
            or set(item) - HOSTED_PUSH_ENTRY_FIELDS
        ):
            continue
        owner = str(item.get("owner") or "").strip().lower()
        name = str(item.get("name") or "").strip()
        identity = (owner, name.lower())
        if (
            not NODE_RE.fullmatch(owner)
            or not REPOSITORY_RE.fullmatch(name)
            or not _writable_owner_allowed(config, owner)
            or identity in seen
        ):
            continue
        git_dir_value = item.get("gitDir")
        if not isinstance(git_dir_value, str):
            continue
        try:
            git_dir = _absolute_path(git_dir_value, "pushed repository")
        except RefreshError:
            continue
        # The only acceptable location is the exact path the gateway derives
        # for this identity. Anything else is a rewritten sidecar.
        if git_dir != writable_root / owner / (name + ".git"):
            continue
        try:
            _reject_symlink_components(git_dir, "pushed repository")
            info = git_dir.lstat()
            resolved = git_dir.resolve(strict=True)
        except (RefreshError, OSError):
            continue
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
            or resolved != git_dir
            or writable_root not in resolved.parents
            or not (git_dir / "HEAD").is_file()
            or not (git_dir / "objects").is_dir()
            or not (git_dir / "refs").is_dir()
        ):
            continue
        try:
            expected_refs = _repository_refs_sha256(config, git_dir)
        except RefreshError:
            continue
        seen.add(identity)
        output.append(
            {
                "owner": owner,
                "name": name,
                "visibility": "public",
                "enabled": True,
                "gitDir": str(git_dir),
                # A push-enabled repository legitimately moves its refs. The
                # gateway skips the immutable bootstrap pin for exactly these
                # entries (mirror_gateway.GitRepository.check_integrity), so
                # this value is the served generation the signed repository
                # proof advertises, never a freeze. Every other repository
                # keeps the strict pin.
                "integrity": {"expectedRefsSha256": expected_refs},
                "operations": operations,
            }
        )
    output.sort(key=lambda entry: (entry["owner"], entry["name"].casefold()))
    return output


def _hosted_gateway_repositories(
    config: RefreshConfig,
) -> list[dict[str, Any]]:
    path = config.gateway_config_path.parent / HOSTED_REPOSITORIES_FILE
    try:
        path.lstat()
    except FileNotFoundError:
        return []
    source = _read_secure_json(
        path,
        label="hosted repositories",
        maximum=MAX_CONFIG_BYTES,
        owner_only=True,
    )
    _expect_fields(
        source,
        {"schemaVersion", "type", "repositories"},
        label="hosted repositories",
    )
    items = source.get("repositories")
    if (
        source.get("schemaVersion") != SCHEMA_VERSION
        or source.get("type") != HOSTED_REPOSITORIES_TYPE
        or not isinstance(items, list)
        or len(items) > MAX_HOSTED_REPOSITORIES
    ):
        raise RefreshError("hosted repositories configuration is invalid")
    imports_root = HOSTED_REPOSITORIES_ROOT.resolve()
    operations = _derived_repository_operations(config)
    output: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for item in items:
        if not isinstance(item, dict):
            raise RefreshError("hosted repository entry is invalid")
        _expect_fields(
            item,
            {
                "owner",
                "name",
                "sourceRepository",
                "sourceUrl",
                "importId",
                "description",
                "branch",
                "createdAt",
                "publishedStateHash",
            },
            label="hosted repository entry",
        )
        owner = str(item.get("owner") or "").strip().lower()
        name = str(item.get("name") or "").strip()
        identity = (owner, name.lower())
        if (
            owner != config.node_owner
            or not REPOSITORY_RE.fullmatch(name)
            or identity in seen
        ):
            raise RefreshError("hosted repository identity is invalid")
        seen.add(identity)
        repository = _absolute_path(
            item.get("sourceRepository"),
            "hosted source repository",
        )
        _reject_symlink_components(repository, "hosted source repository")
        try:
            info = repository.lstat()
            resolved = repository.resolve()
        except OSError as exc:
            raise RefreshError("hosted source repository is unavailable") from exc
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.geteuid()
            or imports_root not in resolved.parents
        ):
            raise RefreshError("hosted source repository is unsafe")
        output.append(
            {
                "owner": owner,
                "name": name,
                "visibility": "public",
                "enabled": True,
                "gitDir": str(repository),
                "integrity": {
                    "expectedRefsSha256": _repository_refs_sha256(
                        config,
                        repository,
                    ),
                },
                "operations": operations,
            }
        )
    return output


def _prepare_actions_control_directory(config: RefreshConfig) -> None:
    """Create the gateway-owned control queue the rendered path names.

    The gateway refuses a configuration whose ``actionsControlPath`` is not a
    real 0700 directory owned by its own account, so the directory must exist
    before the staged configuration is checked.
    """
    if "actions-control" not in config.operations:
        return
    parent = _require_secure_parent(
        config.gateway_config_path, "gateway configuration")
    path = parent / ACTIONS_CONTROL_DIRECTORY
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    except OSError as exc:
        raise RefreshError(
            "Actions control directory cannot be created") from exc
    _require_owner_directory(path, "Actions control directory")


def _render_gateway_config(
    config: RefreshConfig,
    identity: PublicIdentity,
    metadata: SealMetadata,
    archive_path: Path,
) -> dict[str, Any]:
    helper_base = [
        str(config.python_program),
        str(config.identity_helper_path),
        "--state-dir",
        str(config.identity_state_directory),
    ]
    del archive_path  # retained in the call shape for upgrade compatibility
    repositories = []
    for owner in config.owner_aliases:
        repository = {
            "owner": owner,
            "name": config.repository_name,
            "visibility": "public",
            "enabled": True,
            "gitDir": str(config.source_repository),
            "integrity": {
                "expectedRefsSha256": metadata.expected_refs_sha256
            },
            "operations": list(config.operations),
        }
        if config.release_store is not None:
            repository["releaseStore"] = str(config.release_store)
        repositories.append(repository)
    repositories.extend(_hosted_gateway_repositories(config))
    # Push-created repositories render last and never displace an identity the
    # operator configured or an import already claimed, so a rewritten sidecar
    # cannot take over an existing route or collide the gateway's uniqueness
    # check into a total refresh failure.
    repositories.extend(
        _pushed_gateway_repositories(
            config,
            frozenset(
                (str(item["owner"]), str(item["name"]).lower())
                for item in repositories
            ),
        )
    )
    return {
        "schemaVersion": 1,
        "node": {
            "name": identity.node_name,
            "publicKey": identity.node_public_key,
        },
        "routerPublicKey": identity.router_public_key,
        "publicOrigin": config.public_origin,
        "listen": {
            "host": config.listen_host,
            "port": config.listen_port,
        },
        "manifestPath": str(config.manifest_path),
        "requestVerifierCommand": helper_base + ["capability-verify"],
        "healthSignerCommand": helper_base + ["health-sign"],
        **({
            "mergeExecutorCommand": [
                str(config.python_program),
                str(Path(__file__).resolve()),
                "--config",
                str(config.config_path),
                "merge-execute",
            ],
        } if {"merge-pull", "update-pull-branch"} & set(config.operations)
            else {}),
        **({
            "actionsSummaryPath": str(
                config.gateway_config_path.parent / ACTIONS_SUMMARY_FILE
            ),
        } if "actions-status" in config.operations else {}),
        **({
            "actionsControlPath": str(
                config.gateway_config_path.parent / ACTIONS_CONTROL_DIRECTORY
            ),
        } if "actions-control" in config.operations else {}),
        **({
            "writableRepositoryRoot": str(config.writable_repository_root),
        } if config.writable_repository_root is not None else {}),
        **({
            "writableOwners": list(config.writable_owners),
        } if config.writable_owners else {}),
        **({
            "limits": {
                "receiveMaxInputBytes": config.receive_max_input_bytes,
            },
        } if config.receive_max_input_bytes is not None else {}),
        "repositories": repositories,
    }


def _write_all(descriptor: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise RefreshError("atomic state write failed")
        view = view[written:]


def _write_staged_json(parent: Path, prefix: str, value: Any) -> Path:
    encoded = _canonical_json(value) + b"\n"
    if len(encoded) > MAX_CONFIG_BYTES:
        raise RefreshError("generated gateway configuration is too large")
    path = parent / ("." + prefix + "." + secrets.token_hex(12) + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
        try:
            _write_all(descriptor, encoded)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        return path
    except (OSError, RefreshError) as exc:
        try:
            path.unlink()
        except OSError:
            pass
        raise RefreshError("staged gateway configuration write failed") from exc


def _fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError as exc:
        raise RefreshError("durable state update failed") from exc


def _same_regular_file(
    left: os.stat_result,
    right: os.stat_result,
) -> bool:
    return (
        stat.S_ISREG(left.st_mode)
        and stat.S_ISREG(right.st_mode)
        and left.st_dev == right.st_dev
        and left.st_ino == right.st_ino
        and left.st_uid == right.st_uid
        and left.st_gid == right.st_gid
        and left.st_nlink == right.st_nlink == 1
        and stat.S_IMODE(left.st_mode) == stat.S_IMODE(right.st_mode)
        and left.st_size == right.st_size
        and left.st_mtime_ns == right.st_mtime_ns
    )


def _replace_catalog_actions_enabled(
    config: RefreshConfig,
    enabled: bool,
) -> RefreshConfig:
    """Atomically change only catalog.actionsEnabled in the refresh config."""
    path = config.config_path
    before = _lstat_no_symlink(path, "configuration")
    source = _read_secure_json(
        path,
        label="configuration",
        maximum=MAX_CONFIG_BYTES,
        owner_only=True,
    )
    _assert_no_secret_fields(source)
    # Re-run the complete configuration validator before preserving and
    # rewriting any owner-controlled fields.
    validated = load_config(path)
    after_read = _lstat_no_symlink(path, "configuration")
    if (
        not _same_regular_file(before, after_read)
        or validated.node_owner != config.node_owner
        or validated.repository_name != config.repository_name
        or validated.identity_state_directory
        != config.identity_state_directory
    ):
        raise RefreshError("configuration changed during Actions update")

    catalog = source.get("catalog", {})
    if not isinstance(catalog, dict):
        # load_config() already rejects this, but retain a fixed local failure
        # if the implementation is changed independently later.
        raise RefreshError("catalog must be an object")
    updated = dict(source)
    updated_catalog = dict(catalog)
    updated_catalog["actionsEnabled"] = enabled
    updated["catalog"] = updated_catalog

    staged: Path | None = None
    try:
        staged = _write_staged_json(path.parent, path.name, updated)
        try:
            os.chmod(
                staged,
                stat.S_IMODE(before.st_mode),
                follow_symlinks=False,
            )
            staged_descriptor, staged_info = _open_bounded_file(
                staged,
                label="staged Actions configuration",
                maximum=MAX_CONFIG_BYTES,
                owner_only=True,
            )
            try:
                if (
                    stat.S_IMODE(staged_info.st_mode)
                    != stat.S_IMODE(before.st_mode)
                ):
                    raise RefreshError(
                        "configuration permissions could not be preserved"
                    )
                os.fsync(staged_descriptor)
            finally:
                os.close(staged_descriptor)
            current = _lstat_no_symlink(path, "configuration")
        except OSError as exc:
            raise RefreshError(
                "configuration permissions could not be preserved"
            ) from exc
        if not _same_regular_file(before, current):
            raise RefreshError("configuration changed during Actions update")
        _fsync_directory(path.parent)
        os.replace(staged, path)
        staged = None
        try:
            _fsync_directory(path.parent)
        except RefreshError:
            # The atomic rename already committed a complete owner-only file.
            # As with refresh(), do not misreport that committed state as a
            # rollback-safe failure.
            pass
    except OSError as exc:
        raise RefreshError("Actions configuration update failed") from exc
    finally:
        if staged is not None:
            try:
                staged.unlink()
            except FileNotFoundError:
                pass
    return load_config(path)


def _install_content_addressed_archive(
    config: RefreshConfig,
    staged: Path,
    metadata: SealMetadata,
) -> Path:
    final = config.archive_directory / (
        "archive-" + metadata.ciphertext_sha256 + ".age"
    )
    try:
        existing = final.lstat()
    except FileNotFoundError:
        existing = None
    if existing is not None:
        _validate_archive(
            config,
            final,
            digest=metadata.ciphertext_sha256,
            size=metadata.ciphertext_bytes,
        )
        try:
            staged.unlink()
        except OSError as exc:
            raise RefreshError("staged archive cleanup failed") from exc
        return final
    try:
        os.link(staged, final, follow_symlinks=False)
        os.chmod(final, 0o600, follow_symlinks=False)
        staged.unlink()
        _fsync_directory(config.archive_directory)
    except (FileExistsError, OSError) as exc:
        try:
            staged.unlink()
        except FileNotFoundError:
            pass
        raise RefreshError("content-addressed archive installation failed") from exc
    _validate_archive(
        config,
        final,
        digest=metadata.ciphertext_sha256,
        size=metadata.ciphertext_bytes,
    )
    return final


def _prune_superseded_archives(
    config: RefreshConfig,
    *,
    keep: frozenset[Path],
) -> None:
    """Remove refresh-managed generations that cannot be active anymore."""
    retained = {path.resolve() for path in keep}
    changed = False
    try:
        candidates = tuple(config.archive_directory.iterdir())
    except OSError as exc:
        raise RefreshError("encrypted archive directory cannot be listed") from exc
    for path in candidates:
        if not ARCHIVE_NAME_RE.fullmatch(path.name):
            continue
        try:
            info = path.lstat()
            resolved = path.resolve()
        except OSError as exc:
            raise RefreshError("superseded archive cannot be inspected") from exc
        if resolved in retained:
            continue
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_nlink != 1
            or resolved.parent != config.archive_directory.resolve()
        ):
            raise RefreshError("superseded archive is unsafe")
        try:
            path.unlink()
        except OSError as exc:
            raise RefreshError("superseded archive cleanup failed") from exc
        changed = True
    if changed:
        _fsync_directory(config.archive_directory)


def _invoke_gateway_check(config: RefreshConfig, gateway_config: Path) -> None:
    raw = _run_bounded(config.gateway_check_command(gateway_config))
    result = _parse_json(
        raw,
        maximum=MAX_JSON_OUTPUT_BYTES,
        label="gateway validation response",
    )
    if (
        result.get("ok") is not True
        or result.get("privateKeysLoaded") is not False
        or result.get("privateRepositoryDetailsPublished") is not False
    ):
        raise RefreshError("gateway validation rejected the active mirror")


def _active_metadata(
    config: RefreshConfig,
    identity: PublicIdentity,
) -> tuple[SealMetadata, Path]:
    source = _read_secure_json(
        config.gateway_config_path,
        label="active gateway configuration",
        maximum=MAX_CONFIG_BYTES,
        owner_only=True,
    )
    repositories = source.get("repositories")
    if not isinstance(repositories, list) or not repositories:
        raise RefreshError("active gateway configuration is not refresh-managed")
    first = repositories[0]
    if not isinstance(first, dict):
        raise RefreshError("active gateway configuration is not refresh-managed")
    git_dir_raw = first.get("gitDir")
    integrity = first.get("integrity")
    if not isinstance(git_dir_raw, str) or not isinstance(integrity, dict):
        raise RefreshError("active gateway configuration is not refresh-managed")
    refs = str(integrity.get("expectedRefsSha256") or "").lower()
    git_dir = _absolute_path(git_dir_raw, "active public repository")
    if git_dir != config.source_repository or not SHA256_RE.fullmatch(refs):
        raise RefreshError("active gateway configuration is not refresh-managed")
    metadata = SealMetadata(
        refs, _source_storage_bytes(config), "public-plaintext", refs)
    expected = _render_gateway_config(config, identity, metadata, git_dir)
    if not secrets.compare_digest(_canonical_json(source), _canonical_json(expected)):
        raise RefreshError("active gateway configuration drift was detected")
    _require_bare_source(config)
    return metadata, git_dir


def _validate_active(
    config: RefreshConfig,
) -> tuple[PublicIdentity, SealMetadata, Path]:
    identity = _load_public_identity(config)
    _fsck_source(config)
    metadata, archive_path = _active_metadata(config, identity)
    if not secrets.compare_digest(
        _source_refs_sha256(config),
        metadata.expected_refs_sha256,
    ):
        raise RefreshError("active mirror does not match the exact source refs")
    _invoke_gateway_check(config, config.gateway_config_path)
    return identity, metadata, archive_path


def _validate_active_renewal(
    config: RefreshConfig,
) -> tuple[PublicIdentity, SealMetadata, Path]:
    """Validate the active plaintext generation without a full Git fsck."""
    identity = _load_public_identity(config)
    _require_bare_source(config)
    metadata, archive_path = _active_metadata(config, identity)
    if not secrets.compare_digest(
        _source_refs_sha256(config),
        metadata.expected_refs_sha256,
    ):
        raise RefreshError("active mirror does not match the exact source refs")
    return identity, metadata, archive_path


def refresh(config: RefreshConfig) -> dict[str, Any]:
    with _refresh_lock(config, shared=False):
        identity = _load_public_identity(config)
        _require_bare_source(config)
        staged_gateway: Path | None = None
        for _attempt in range(3):
            before_refs = _source_refs_sha256(config)
            metadata = SealMetadata(
                before_refs,
                _source_storage_bytes(config),
                "public-plaintext",
                before_refs,
            )
            _prepare_actions_control_directory(config)
            staged_gateway = _write_staged_json(
                config.gateway_config_path.parent,
                config.gateway_config_path.name,
                _render_gateway_config(
                    config, identity, metadata, config.source_repository),
            )
            try:
                _invoke_gateway_check(config, staged_gateway)
                if not secrets.compare_digest(
                    before_refs, _source_refs_sha256(config)):
                    staged_gateway.unlink()
                    staged_gateway = None
                    continue
                _fsync_directory(config.gateway_config_path.parent)
                os.replace(staged_gateway, config.gateway_config_path)
                staged_gateway = None
                try:
                    _fsync_directory(config.gateway_config_path.parent)
                except RefreshError:
                    pass
                # Public ciphertext from older releases has no remaining use.
                _prune_superseded_archives(config, keep=frozenset())
                return {
                    "ok": True,
                    "event": "refresh_complete",
                    "aliasCount": len(config.owner_aliases),
                }
            finally:
                if staged_gateway is not None:
                    try:
                        staged_gateway.unlink()
                    except FileNotFoundError:
                        pass
                    staged_gateway = None
        raise RefreshError("source refs kept changing during gateway refresh")


class _NoRedirect(urlrequest.HTTPRedirectHandler):
    def redirect_request(
        self,
        req: urlrequest.Request,
        fp: BinaryIO,
        code: int,
        msg: str,
        headers: Mapping[str, str],
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl
        return None


def _post_json(url: str, value: Mapping[str, Any]) -> tuple[int, dict[str, Any]]:
    encoded = _canonical_json(value)
    if len(encoded) > MAX_CONFIG_BYTES:
        raise RefreshError("publication request is too large")
    request = urlrequest.Request(
        url,
        data=encoded,
        method="POST",
        headers={
            "accept": "application/json",
            "content-type": "application/json",
            "user-agent": "ForkMesh-headless-refresh/1.0",
        },
    )
    opener = urlrequest.build_opener(
        _NoRedirect(),
        urlrequest.ProxyHandler({}),
    )
    try:
        with opener.open(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            status = int(response.status)
            try:
                announced = int(response.headers.get("content-length") or 0)
            except (TypeError, ValueError):
                announced = 0
            if announced > MAX_HTTP_RESPONSE_BYTES:
                raise RefreshError("publication response is too large")
            raw = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
    except urlerror.HTTPError as exc:
        # Do not read or surface the remote body; it may echo a public identity
        # or an operator-supplied field.
        raise RefreshError("publication endpoint rejected the request") from exc
    except (urlerror.URLError, TimeoutError, OSError, http.client.HTTPException) as exc:
        raise RefreshError("publication endpoint is unavailable") from exc
    if len(raw) > MAX_HTTP_RESPONSE_BYTES:
        raise RefreshError("publication response is too large")
    return status, _parse_json(
        raw,
        maximum=MAX_HTTP_RESPONSE_BYTES,
        label="publication response",
    )


def _sign_endpoint(
    config: RefreshConfig,
    identity: PublicIdentity,
) -> dict[str, Any]:
    signing_request = {
        "schemaVersion": 1,
        "type": "forkmesh.https-endpoint-registration-signing",
        "node": config.node_owner,
        "baseUrl": config.public_origin,
    }
    if config.mirror_url:
        signing_request["mirrorUrl"] = config.mirror_url
    response = _helper_call(
        config,
        "sign-endpoint-registration",
        signing_request,
    )
    _expect_fields(
        response,
        {"node", "baseUrl", "publicKey", "issuedAt", "signature"},
        optional={"mirrorUrl"},
        label="endpoint signing response",
    )
    signature = str(response.get("signature") or "")
    if (
        response.get("node") != config.node_owner
        or response.get("baseUrl") != config.public_origin
        or str(response.get("mirrorUrl") or "") != config.mirror_url
        or response.get("publicKey") != identity.node_public_key
        or isinstance(response.get("issuedAt"), bool)
        or not isinstance(response.get("issuedAt"), int)
        or response.get("issuedAt", 0) <= 0
        or not re.fullmatch(r"[A-Za-z0-9_-]{86}", signature)
    ):
        raise RefreshError("endpoint signing response is invalid")
    return response


MAX_PUBLIC_TELEMETRY_BYTES = 1 << 50
MAX_PROC_METRIC_BYTES = 64 * 1024


def _read_proc_metric(path: Path) -> bytes | None:
    """Read one fixed Linux proc metric without following a replacement link."""
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError:
        return None
    try:
        data = os.read(descriptor, MAX_PROC_METRIC_BYTES + 1)
    except OSError:
        return None
    finally:
        os.close(descriptor)
    if not data or len(data) > MAX_PROC_METRIC_BYTES:
        return None
    return data


def _proc_cpu_snapshot(raw: bytes | None) -> tuple[int, int] | None:
    """Return Linux aggregate (idle ticks, total ticks) from /proc/stat."""
    if not raw:
        return None
    try:
        first = raw.decode("ascii", "strict").splitlines()[0].split()
    except (UnicodeDecodeError, IndexError):
        return None
    if not first or first[0] != "cpu" or len(first) < 5:
        return None
    values = []
    for item in first[1:9]:
        if not re.fullmatch(r"[0-9]{1,20}", item):
            return None
        number = int(item)
        if number > (1 << 63) - 1:
            return None
        values.append(number)
    if len(values) < 4:
        return None
    total = sum(values)
    idle = values[3] + (values[4] if len(values) > 4 else 0)
    return (idle, total) if total > 0 and idle <= total else None


def _sample_linux_cpu(
    *,
    read_metric: Callable[[Path], bytes | None] = _read_proc_metric,
    sleeper: Callable[[float], None] = time.sleep,
) -> int | None:
    first = _proc_cpu_snapshot(read_metric(Path("/proc/stat")))
    if first is None:
        return None
    sleeper(0.1)
    second = _proc_cpu_snapshot(read_metric(Path("/proc/stat")))
    if second is None:
        return None
    idle_delta = second[0] - first[0]
    total_delta = second[1] - first[1]
    if total_delta <= 0 or idle_delta < 0 or idle_delta > total_delta:
        return None
    busy_delta = total_delta - idle_delta
    return max(0, min(100, (busy_delta * 100 + total_delta // 2) // total_delta))


def _parse_linux_memory(raw: bytes | None) -> tuple[int, int] | None:
    """Return used/total bytes from bounded MemTotal and MemAvailable values."""
    if not raw:
        return None
    values: dict[str, int] = {}
    try:
        lines = raw.decode("ascii", "strict").splitlines()
    except UnicodeDecodeError:
        return None
    for line in lines:
        match = re.fullmatch(
            r"(MemTotal|MemAvailable):[ \t]+([0-9]{1,20})[ \t]+kB[ \t]*",
            line,
        )
        if not match:
            continue
        kibibytes = int(match.group(2))
        if kibibytes > MAX_PUBLIC_TELEMETRY_BYTES // 1024:
            return None
        values[match.group(1)] = kibibytes * 1024
    total = values.get("MemTotal")
    available = values.get("MemAvailable")
    if (
        total is None
        or available is None
        or total <= 0
        or available < 0
        or available > total
    ):
        return None
    return total - available, total


def _sample_linux_memory(
    *,
    read_metric: Callable[[Path], bytes | None] = _read_proc_metric,
) -> tuple[int, int] | None:
    return _parse_linux_memory(read_metric(Path("/proc/meminfo")))


def _sample_linux_disk(
    path: Path,
    *,
    statvfs: Callable[[Path], Any] = os.statvfs,
) -> tuple[int, int] | None:
    try:
        info = statvfs(path)
        block_size = int(info.f_frsize or info.f_bsize)
        blocks = int(info.f_blocks)
        available_blocks = int(info.f_bavail)
    except (AttributeError, OSError, TypeError, ValueError, OverflowError):
        return None
    if (
        block_size <= 0
        or blocks <= 0
        or available_blocks < 0
        or available_blocks > blocks
    ):
        return None
    total_raw = blocks * block_size
    used_raw = total_raw - available_blocks * block_size
    total = min(total_raw, MAX_PUBLIC_TELEMETRY_BYTES)
    return min(max(0, used_raw), total), total


def _bounded_optional_metric(value: Any, maximum: int) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return min(value, maximum)


def _bounded_optional_usage(value: Any) -> tuple[int, int] | None:
    if not isinstance(value, tuple) or len(value) != 2:
        return None
    used = _bounded_optional_metric(value[0], MAX_PUBLIC_TELEMETRY_BYTES)
    total = _bounded_optional_metric(value[1], MAX_PUBLIC_TELEMETRY_BYTES)
    if used is None or total is None or total <= 0:
        return None
    return min(used, total), total


def _sample_host_telemetry(
    config: RefreshConfig,
    *,
    cpu_sampler: Callable[[], int | None] = _sample_linux_cpu,
    memory_sampler: Callable[[], tuple[int, int] | None] = _sample_linux_memory,
    disk_sampler: Callable[[Path], tuple[int, int] | None] = _sample_linux_disk,
    platform: str = sys.platform,
) -> dict[str, int]:
    """Collect only explicitly enabled public host metrics.

    Sampling is best-effort and content-free. A disabled, unsupported, or
    malformed reading is omitted, which the catalog/World represents as
    unknown rather than zero.
    """
    if not platform.startswith("linux"):
        return {}
    result: dict[str, int] = {}
    if config.catalog.report_cpu:
        try:
            cpu = _bounded_optional_metric(cpu_sampler(), 100)
        except Exception:
            cpu = None
        if cpu is not None:
            result["cpuPercent"] = cpu
    if config.catalog.report_memory:
        try:
            memory = _bounded_optional_usage(memory_sampler())
        except Exception:
            memory = None
        if memory is not None:
            result["memUsedBytes"], result["memTotalBytes"] = memory
    if config.catalog.report_disk:
        try:
            disk = _bounded_optional_usage(
                disk_sampler(config.source_repository))
        except Exception:
            disk = None
        if disk is not None:
            result["diskUsedBytes"], result["diskTotalBytes"] = disk
    return result


def _actions_catalog_state(
    config: RefreshConfig,
    now_ms: int,
) -> tuple[bool, str]:
    """Return the bounded public Actions capability and live state.

    ``catalog.actionsEnabled`` is the owner-controlled upper bound. A local
    executor lease can narrow that capability to disabled or report enabled /
    running, but it can never elevate a statically disabled node. Missing,
    malformed, unsafe, stale, or implausibly future-dated leases fall back to
    the truthful static enabled/disabled state and never interrupt catalog
    renewal.
    """
    fallback = (
        config.catalog.actions_enabled,
        "enabled" if config.catalog.actions_enabled else "disabled",
    )
    if not config.catalog.actions_enabled:
        return fallback
    path = config.gateway_config_path.parent / ACTIONS_STATE_FILE
    try:
        value = _read_actions_state_json(path)
        _expect_fields(
            value,
            {
                "schemaVersion",
                "type",
                "node",
                "state",
                "updatedAt",
                "expiresAt",
            },
            label="Actions state lease",
        )
    except (OSError, RefreshError):
        return fallback

    state = value.get("state")
    updated_at = value.get("updatedAt")
    expires_at = value.get("expiresAt")
    if (
        value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != ACTIONS_STATE_TYPE
        or value.get("node") != config.node_owner
        or state not in {"disabled", "enabled", "running"}
        or isinstance(updated_at, bool)
        or not isinstance(updated_at, int)
        or isinstance(expires_at, bool)
        or not isinstance(expires_at, int)
        or not 0 <= updated_at <= MAX_SAFE_JSON_INTEGER
        or not 0 <= expires_at <= MAX_SAFE_JSON_INTEGER
        or updated_at < now_ms - MAX_ACTIONS_STATE_LEASE_MS
        or updated_at > now_ms + MAX_ACTIONS_STATE_LEASE_MS
        or expires_at <= now_ms
        or expires_at > now_ms + MAX_ACTIONS_STATE_LEASE_MS
        or expires_at <= updated_at
        or expires_at - updated_at > MAX_ACTIONS_STATE_LEASE_MS
    ):
        return fallback
    if state == "disabled":
        # The public catalog contract keeps actionsEnabled/actionsState
        # internally consistent: a live disabled executor advertises no current
        # capability even though the owner configuration permits re-enabling it.
        return False, "disabled"
    return True, state


def _catalog_unsigned(
    config: RefreshConfig,
    metadata: SealMetadata,
    now_ms: int,
) -> dict[str, Any]:
    actions_enabled, actions_state = _actions_catalog_state(config, now_ms)
    record: dict[str, Any] = {
        "owner": config.node_owner,
        "name": config.repository_name,
        "visibility": "public",
        "sizeBytes": metadata.ciphertext_bytes,
        "description": config.catalog.description,
        "cloneUrl": (
            config.worker_origin
            + "/"
            + quote(config.node_owner, safe="")
            + "/"
            + quote(config.repository_name, safe="")
        ),
        "channel": (
            config.catalog.channel
            or "#" + config.node_owner + "-" + config.repository_name
        ),
        "lastSync": str(now_ms),
        "updatedAt": str(now_ms),
        "source": "remote-clone",
        "branch": config.catalog.branch,
        "platform": config.catalog.platform,
        "version": config.catalog.version,
        # The owner configuration caps this capability; the live state comes
        # only from a strict, short owner-local executor lease. Workflow
        # definitions, variables, commands, paths, and logs never enter this
        # signed catalog publication.
        "actionsEnabled": actions_enabled,
        "actionsState": actions_state,
        "nodeId": config.node_owner,
        "stateHash": metadata.expected_refs_sha256,
        "commit": _source_branch_commit(config),
    }
    if config.mirror_url:
        record["mirrorUrl"] = (
            config.mirror_url
            + "/"
            + quote(config.node_owner, safe="")
            + "/"
            + quote(config.repository_name, safe="")
            + ".git"
        )
    if config.catalog.solana:
        record["solana"] = config.catalog.solana
    if config.catalog.hosted_since:
        record["hostedSince"] = config.catalog.hosted_since
    record.update(_sample_repository_statistics(config))
    record.update(_sample_host_telemetry(config))
    return record


def _catalog_report_policy(config: RefreshConfig) -> dict[str, bool]:
    """Return consent bits separately from volatile telemetry samples."""
    return {
        "cpu": bool(config.catalog.report_cpu),
        "memory": bool(config.catalog.report_memory),
        "disk": bool(config.catalog.report_disk),
    }


def _catalog_semantic_fingerprint(
    record: Mapping[str, Any],
    report_policy: Mapping[str, bool],
) -> str:
    if (
        set(report_policy) != {"cpu", "memory", "disk"}
        or any(not isinstance(value, bool) for value in report_policy.values())
    ):
        raise RefreshError("catalog report policy is invalid")
    # Values such as CPU utilization are intentionally coalesced. Their
    # publication consent is not: disabling one reporting class must remove
    # the previously published value on the very next renewal.
    semantic = {
        "record": {
            key: value
            for key, value in record.items()
            if key not in CATALOG_SEMANTIC_EXCLUDED_FIELDS
        },
        "reportPolicy": dict(report_policy),
    }
    return hashlib.sha256(_canonical_json(semantic)).hexdigest()


def _current_time_ms() -> int:
    return time.time_ns() // 1_000_000


def _catalog_renewal_state_path(config: RefreshConfig) -> Path:
    return config.gateway_config_path.parent / CATALOG_RENEWAL_STATE_FILE


def _catalog_renewal_backoff_path(config: RefreshConfig) -> Path:
    return config.gateway_config_path.parent / CATALOG_RENEWAL_BACKOFF_FILE


def _read_catalog_renewal_state(
    config: RefreshConfig,
    now_ms: int,
) -> tuple[str, int] | None:
    path = _catalog_renewal_state_path(config)
    try:
        descriptor, info = _open_bounded_file(
            path,
            label="catalog renewal state",
            maximum=MAX_CATALOG_RENEWAL_STATE_BYTES,
            owner_only=True,
        )
        try:
            if (
                info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o600
            ):
                raise RefreshError(
                    "catalog renewal state permissions or ownership are unsafe"
                )
            raw = bytearray()
            while len(raw) <= MAX_CATALOG_RENEWAL_STATE_BYTES:
                chunk = os.read(
                    descriptor,
                    min(
                        64 * 1024,
                        MAX_CATALOG_RENEWAL_STATE_BYTES + 1 - len(raw),
                    ),
                )
                if not chunk:
                    break
                raw.extend(chunk)
            if (
                len(raw) != info.st_size
                or len(raw) > MAX_CATALOG_RENEWAL_STATE_BYTES
            ):
                raise RefreshError("catalog renewal state changed while reading")
        finally:
            os.close(descriptor)
        value = _parse_json(
            bytes(raw),
            maximum=MAX_CATALOG_RENEWAL_STATE_BYTES,
            label="catalog renewal state",
        )
        _expect_fields(
            value,
            {
                "schemaVersion",
                "type",
                "node",
                "repository",
                "semanticFingerprint",
                "verifiedAt",
            },
            label="catalog renewal state",
        )
        fingerprint = value.get("semanticFingerprint")
        verified_at = value.get("verifiedAt")
        if (
            value.get("schemaVersion") != SCHEMA_VERSION
            or value.get("type") != CATALOG_RENEWAL_STATE_TYPE
            or value.get("node") != config.node_owner
            or value.get("repository") != config.repository_name
            or not isinstance(fingerprint, str)
            or not SHA256_RE.fullmatch(fingerprint)
            or isinstance(verified_at, bool)
            or not isinstance(verified_at, int)
            or not 0 < verified_at <= now_ms
            or verified_at > MAX_SAFE_JSON_INTEGER
        ):
            raise RefreshError("catalog renewal state is invalid")
        return fingerprint, verified_at
    except (OSError, RefreshError):
        # Renewal state is only an optimization. Missing, damaged, stale-mode,
        # or identity-mismatched state must publish rather than suppressing a
        # catalog update.
        return None


def _write_catalog_renewal_state(
    config: RefreshConfig,
    fingerprint: str,
    verified_at: int,
) -> None:
    state = {
        "schemaVersion": SCHEMA_VERSION,
        "type": CATALOG_RENEWAL_STATE_TYPE,
        "node": config.node_owner,
        "repository": config.repository_name,
        "semanticFingerprint": fingerprint,
        "verifiedAt": verified_at,
    }
    if (
        not isinstance(fingerprint, str)
        or not SHA256_RE.fullmatch(fingerprint)
        or isinstance(verified_at, bool)
        or not isinstance(verified_at, int)
        or not 0 < verified_at <= MAX_SAFE_JSON_INTEGER
        or len(_canonical_json(state)) + 1 > MAX_CATALOG_RENEWAL_STATE_BYTES
    ):
        raise RefreshError("catalog renewal state is invalid")

    parent = config.gateway_config_path.parent
    path = _catalog_renewal_state_path(config)
    staged: Path | None = None
    try:
        staged = _write_staged_json(
            parent,
            CATALOG_RENEWAL_STATE_FILE,
            state,
        )
        os.replace(staged, path)
        staged = None
        _fsync_directory(parent)
    except (OSError, RefreshError) as exc:
        if staged is not None:
            try:
                staged.unlink()
            except OSError:
                pass
        raise RefreshError("catalog renewal state write failed") from exc


def _catalog_renewal_is_due(
    config: RefreshConfig,
    fingerprint: str,
    now_ms: int,
) -> bool:
    prior = _read_catalog_renewal_state(config, now_ms)
    if prior is None:
        return True
    prior_fingerprint, verified_at = prior
    return (
        not secrets.compare_digest(prior_fingerprint, fingerprint)
        or now_ms - verified_at >= CATALOG_RENEWAL_INTERVAL_MS
    )


def _read_catalog_renewal_backoff(
    config: RefreshConfig,
    now_ms: int,
) -> tuple[str, int] | None:
    path = _catalog_renewal_backoff_path(config)
    try:
        value = _read_secure_json(
            path,
            label="catalog renewal backoff",
            maximum=MAX_CATALOG_RENEWAL_BACKOFF_BYTES,
            owner_only=True,
        )
        _expect_fields(
            value,
            {
                "schemaVersion",
                "type",
                "node",
                "repository",
                "semanticFingerprint",
                "failedAt",
                "retryAfter",
            },
            label="catalog renewal backoff",
        )
        fingerprint = value.get("semanticFingerprint")
        failed_at = value.get("failedAt")
        retry_after = value.get("retryAfter")
        if (
            value.get("schemaVersion") != SCHEMA_VERSION
            or value.get("type") != CATALOG_RENEWAL_BACKOFF_TYPE
            or value.get("node") != config.node_owner
            or value.get("repository") != config.repository_name
            or not isinstance(fingerprint, str)
            or not SHA256_RE.fullmatch(fingerprint)
            or isinstance(failed_at, bool)
            or not isinstance(failed_at, int)
            or isinstance(retry_after, bool)
            or not isinstance(retry_after, int)
            or not 0 < failed_at <= now_ms
            or retry_after <= failed_at
            or retry_after - failed_at > CATALOG_RENEWAL_FAILURE_BACKOFF_MS
            or retry_after > MAX_SAFE_JSON_INTEGER
        ):
            raise RefreshError("catalog renewal backoff is invalid")
        return fingerprint, retry_after
    except (OSError, RefreshError):
        # A local retry hint can only delay work. Any damaged, unsafe, or
        # clock-inconsistent hint is ignored so it cannot suppress a required
        # semantic publication.
        return None


def _write_catalog_renewal_backoff(
    config: RefreshConfig,
    fingerprint: str,
    failed_at: int,
) -> None:
    retry_after = failed_at + CATALOG_RENEWAL_FAILURE_BACKOFF_MS
    state = {
        "schemaVersion": SCHEMA_VERSION,
        "type": CATALOG_RENEWAL_BACKOFF_TYPE,
        "node": config.node_owner,
        "repository": config.repository_name,
        "semanticFingerprint": fingerprint,
        "failedAt": failed_at,
        "retryAfter": retry_after,
    }
    if (
        not isinstance(fingerprint, str)
        or not SHA256_RE.fullmatch(fingerprint)
        or isinstance(failed_at, bool)
        or not isinstance(failed_at, int)
        or not 0 < failed_at <= MAX_SAFE_JSON_INTEGER
        or retry_after > MAX_SAFE_JSON_INTEGER
        or len(_canonical_json(state)) + 1 > MAX_CATALOG_RENEWAL_BACKOFF_BYTES
    ):
        raise RefreshError("catalog renewal backoff is invalid")
    parent = config.gateway_config_path.parent
    path = _catalog_renewal_backoff_path(config)
    staged: Path | None = None
    try:
        staged = _write_staged_json(
            parent,
            CATALOG_RENEWAL_BACKOFF_FILE,
            state,
        )
        os.replace(staged, path)
        staged = None
        _fsync_directory(parent)
    except (OSError, RefreshError) as exc:
        if staged is not None:
            try:
                staged.unlink()
            except OSError:
                pass
        raise RefreshError("catalog renewal backoff write failed") from exc


def _catalog_renewal_backoff_remaining(
    config: RefreshConfig,
    fingerprint: str,
    now_ms: int,
) -> int:
    prior = _read_catalog_renewal_backoff(config, now_ms)
    if prior is None:
        return 0
    prior_fingerprint, retry_after = prior
    if not secrets.compare_digest(prior_fingerprint, fingerprint):
        return 0
    return max(0, retry_after - now_ms)


def _clear_catalog_renewal_backoff(config: RefreshConfig) -> None:
    path = _catalog_renewal_backoff_path(config)
    try:
        path.unlink()
        _fsync_directory(path.parent)
    except FileNotFoundError:
        return
    except (OSError, RefreshError):
        # An expired or fingerprint-specific hint cannot suppress later work;
        # cleanup is therefore best-effort after a successful remote renewal.
        return


def _record_catalog_renewal_failure(
    config: RefreshConfig,
    fingerprint: str,
) -> None:
    try:
        _write_catalog_renewal_backoff(
            config,
            fingerprint,
            _current_time_ms(),
        )
    except RefreshError:
        # Preserve the original publication/activation failure. A broken local
        # state directory must not turn that failed attempt into success.
        pass


def _deferred_renewal_result(
    config: RefreshConfig,
    remaining_ms: int,
) -> dict[str, Any]:
    return {
        "ok": False,
        "deferred": True,
        "reason": "previous_renewal_failed",
        "retryAfterMs": max(1, int(remaining_ms)),
        "aliasCount": len(config.owner_aliases),
    }


def _sign_catalog(
    config: RefreshConfig,
    identity: PublicIdentity,
    metadata: SealMetadata,
    *,
    unsigned: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    if unsigned is None:
        unsigned = _catalog_unsigned(
            config,
            metadata,
            _current_time_ms(),
        )
    response = _helper_call(
        config,
        "sign-catalog-v2",
        {
            "schemaVersion": 1,
            "type": "forkmesh.catalog-v2-publication-signing",
            "record": unsigned,
        },
    )
    if (
        response.get("owner") != config.node_owner
        or response.get("name") != config.repository_name
        or response.get("visibility") != "public"
        or response.get("maintainer") != identity.node_public_key
        or response.get("stateHash") != metadata.expected_refs_sha256
        or response.get("catalogSigVersion") != 2
        or not re.fullmatch(
            r"[A-Za-z0-9_-]{86}", str(response.get("catalogSig") or "")
        )
        or not re.fullmatch(
            r"[A-Za-z0-9_-]{86}", str(response.get("stateSig") or "")
        )
    ):
        raise RefreshError("catalog signing response is invalid")
    return response


def _publish_registration(
    config: RefreshConfig,
    identity: PublicIdentity,
    metadata: SealMetadata,
    *,
    renewal: bool = False,
    post_json: Callable[
        [str, Mapping[str, Any]], tuple[int, dict[str, Any]]
    ] = _post_json,
) -> dict[str, Any]:
    now_ms = _current_time_ms()
    unsigned = _catalog_unsigned(config, metadata, now_ms)
    fingerprint = _catalog_semantic_fingerprint(
        unsigned,
        _catalog_report_policy(config),
    )
    if renewal:
        remaining_ms = _catalog_renewal_backoff_remaining(
            config, fingerprint, now_ms)
        if remaining_ms:
            return _deferred_renewal_result(config, remaining_ms)

    def publication_failed(message: str) -> None:
        if renewal:
            _record_catalog_renewal_failure(config, fingerprint)
        raise RefreshError(message)

    publish_catalog = (
        not renewal
        or _catalog_renewal_is_due(config, fingerprint, now_ms)
    )
    endpoint = _sign_endpoint(config, identity)
    try:
        endpoint_status, endpoint_response = post_json(
            config.worker_origin + "/api/mirrors/https",
            endpoint,
        )
    except RefreshError:
        if renewal:
            _record_catalog_renewal_failure(config, fingerprint)
        raise
    endpoint_health = endpoint_response.get("health")
    if (
        endpoint_status not in {200, 201}
        or endpoint_response.get("ok") is not True
        or endpoint_response.get("node") != config.node_owner
        or endpoint_response.get("baseUrl") != config.public_origin
        or str(endpoint_response.get("mirrorUrl") or "")
        != config.mirror_url
        or endpoint_health not in {"active", "pending"}
    ):
        publication_failed("endpoint registration was not accepted")

    if publish_catalog:
        catalog = _sign_catalog(
            config,
            identity,
            metadata,
            unsigned=unsigned,
        )
        try:
            catalog_status, catalog_response = post_json(
                config.worker_origin + "/api/repositories",
                catalog,
            )
        except RefreshError:
            if renewal:
                _record_catalog_renewal_failure(config, fingerprint)
            raise
        repository = catalog_response.get("repository")
        if (
            catalog_status not in {200, 201}
            or catalog_response.get("ok") is not True
            or not isinstance(repository, dict)
            or repository.get("owner") != config.node_owner
            or repository.get("name") != config.repository_name
            or repository.get("stateHash") != metadata.expected_refs_sha256
        ):
            publication_failed("catalog publication was not accepted")

        # This checkpoint means exactly one thing: the authoritative service
        # durably accepted this semantic catalog. Route activation is a
        # separate retryable condition, so persist before its follow-up probe.
        # Otherwise a persistent pending endpoint plus systemd's 20-second
        # failure restart republishes an unchanged catalog without bound.
        try:
            _write_catalog_renewal_state(
                config,
                fingerprint,
                _current_time_ms(),
            )
        except RefreshError:
            if renewal:
                _record_catalog_renewal_failure(config, fingerprint)
            raise

        # Always renew once after the durable catalog write. The first request
        # may legitimately return an old fast lease; after a repository update
        # this second request lets the relay observe the new signed catalog
        # generation and challenge the gateway immediately.
        try:
            endpoint_status, endpoint_response = post_json(
                config.worker_origin + "/api/mirrors/https",
                endpoint,
            )
        except RefreshError:
            if renewal:
                _record_catalog_renewal_failure(config, fingerprint)
            raise
        if (
            endpoint_status not in {200, 201}
            or endpoint_response.get("ok") is not True
            or endpoint_response.get("node") != config.node_owner
            or endpoint_response.get("baseUrl") != config.public_origin
            or str(endpoint_response.get("mirrorUrl") or "")
            != config.mirror_url
            or endpoint_response.get("health") != "active"
        ):
            publication_failed(
                "endpoint registration did not activate signed health")
    elif endpoint_health != "active":
        # The first endpoint POST above is already the activation retry. A
        # recent same-fingerprint catalog checkpoint proves that reposting the
        # catalog cannot add information, and a second immediate endpoint POST
        # would only double the failed health write.
        publication_failed(
            "endpoint registration did not activate signed health")

    _clear_catalog_renewal_backoff(config)
    return {
        "ok": True,
        "aliasCount": len(config.owner_aliases),
    }


def register(
    config: RefreshConfig,
    *,
    post_json: Callable[
        [str, Mapping[str, Any]], tuple[int, dict[str, Any]]
    ] = _post_json,
) -> dict[str, Any]:
    with _refresh_lock(config, shared=False):
        identity, metadata, _archive_path = _validate_active(config)
        result = _publish_registration(
            config, identity, metadata, post_json=post_json)
        result["event"] = "registration_complete"
        return result


def renew(
    config: RefreshConfig,
    *,
    post_json: Callable[
        [str, Mapping[str, Any]], tuple[int, dict[str, Any]]
    ] = _post_json,
) -> dict[str, Any]:
    with _refresh_lock(config, shared=False):
        identity, metadata, _archive_path = _validate_active_renewal(config)
        result = _publish_registration(
            config,
            identity,
            metadata,
            renewal=True,
            post_json=post_json,
        )
        result["event"] = (
            "renewal_deferred"
            if result.get("deferred") is True
            else "renewal_complete"
        )
        return result


def configure_actions(
    config: RefreshConfig,
    value: Mapping[str, Any],
    *,
    post_json: Callable[
        [str, Mapping[str, Any]], tuple[int, dict[str, Any]]
    ] = _post_json,
) -> dict[str, Any]:
    """Apply one secret-free Actions toggle and republish signed state."""
    _expect_fields(
        value,
        {"schemaVersion", "type", "actionsEnabled"},
        label="Actions configuration request",
    )
    enabled = value.get("actionsEnabled")
    if (
        value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != ACTIONS_CONFIGURATION_TYPE
        or not isinstance(enabled, bool)
    ):
        raise RefreshError("Actions configuration request is invalid")

    with _refresh_lock(config, shared=False):
        updated = _replace_catalog_actions_enabled(config, enabled)
        identity, metadata, _archive_path = _validate_active_renewal(updated)
        _publish_registration(
            updated,
            identity,
            metadata,
            post_json=post_json,
        )
    return {
        "ok": True,
        "event": "actions_configuration_complete",
        "actionsEnabled": enabled,
    }


def check(config: RefreshConfig) -> dict[str, Any]:
    with _refresh_lock(config, shared=True):
        _validate_active(config)
        return {
            "ok": True,
            "event": "check_complete",
            "aliasCount": len(config.owner_aliases),
            "networkRequests": 0,
        }


# --- Authenticated pull-merge executor --------------------------------------
#
# The edge Worker authenticates the account and signs the complete request.
# This node-side executor still treats every body field as hostile: repository
# paths come only from the owner-controlled refresh configuration, branch names
# come from committed pull metadata, and Git receives fixed argument vectors
# with hooks, credentials, external protocols, and ambient configuration
# disabled.  A request id is recorded as an out-of-band Git ref so retries are
# durable without changing the public refs digest.

MERGE_JOB_TYPE = "forkmesh.pull-merge-job-v1"
MERGE_EXECUTOR_TYPE = "forkmesh.pull-merge-executor-v1"
MERGE_QUARANTINE_TYPE = "forkmesh.pull-merge-quarantine-v1"
MERGE_STAGING_REF_PREFIX = "refs/forkmesh/merge-staging/"
REQUIRED_CHECKS_POLICY_PATH = ".forkmesh/required-checks.json"
PULL_RULES_POLICY_PATH = ".forkmesh/pull-rules.json"
MERGE_COMPLETED_REF_PREFIX = "refs/forkmesh/merge-completed/"
MERGE_FAILURES = frozenset({
    "checks_required",
    "merge_conflict",
    "pull_not_found",
    "pull_not_open",
    "review_required",
    "rules_required",
    "stale_base",
    "stale_head",
    "stale_pull_metadata",
    "unsupported_pull",
})

# Updating a pull request's branch from the base branch is the mirror image of
# merging it: the same exact-OID transaction machinery moves the *head* branch
# and leaves the base branch and the pull-metadata branch untouched.  It gets
# its own job/quarantine namespaces and record types so an idempotency id can
# never be replayed across the two mutations.
UPDATE_JOB_TYPE = "forkmesh.pull-update-job-v1"
UPDATE_EXECUTOR_TYPE = "forkmesh.pull-update-executor-v1"
UPDATE_QUARANTINE_TYPE = "forkmesh.pull-update-quarantine-v1"
UPDATE_STAGING_REF_PREFIX = "refs/forkmesh/update-staging/"
UPDATE_COMPLETED_REF_PREFIX = "refs/forkmesh/update-completed/"
UPDATE_FAILURES = frozenset({
    "already_current",
    "merge_conflict",
    "pull_not_found",
    "pull_not_open",
    "stale_base",
    "stale_head",
    "stale_pull_metadata",
    "unsupported_pull",
})


@dataclass(frozen=True)
class PullOperation:
    """Namespaces and record shapes for one exact-OID pull mutation.

    Everything that differs between merging a pull request and updating its
    branch from the base branch is data, not control flow: the durable record
    and journal types, the owner-only state directories, the hidden staging /
    completion ref prefixes, the permitted failure codes, and which refs the
    transaction creates.  The shared plumbing below is parameterised by this
    record so neither mutation can read, resume, or overwrite the other's state.
    """

    executor_type: str
    job_type: str
    quarantine_type: str
    jobs_directory: str
    quarantine_directory: str
    staging_prefix: str
    completed_prefix: str
    failures: frozenset[str]
    terminal_status: str
    # Journal/record fields naming the objects this transaction publishes, and
    # the matching hidden ref suffixes that keep them reachable until it lands.
    after_fields: tuple[str, ...]
    ref_names: tuple[str, ...]


MERGE_OPERATION = PullOperation(
    executor_type=MERGE_EXECUTOR_TYPE,
    job_type=MERGE_JOB_TYPE,
    quarantine_type=MERGE_QUARANTINE_TYPE,
    jobs_directory="merge-jobs",
    quarantine_directory="merge-quarantine",
    staging_prefix=MERGE_STAGING_REF_PREFIX,
    completed_prefix=MERGE_COMPLETED_REF_PREFIX,
    failures=MERGE_FAILURES,
    terminal_status="merged",
    after_fields=("baseAfter", "pullsAfter"),
    ref_names=("base", "pulls"),
)
UPDATE_OPERATION = PullOperation(
    executor_type=UPDATE_EXECUTOR_TYPE,
    job_type=UPDATE_JOB_TYPE,
    quarantine_type=UPDATE_QUARANTINE_TYPE,
    jobs_directory="update-jobs",
    quarantine_directory="update-quarantine",
    staging_prefix=UPDATE_STAGING_REF_PREFIX,
    completed_prefix=UPDATE_COMPLETED_REF_PREFIX,
    failures=UPDATE_FAILURES,
    terminal_status="updated",
    after_fields=("headAfter",),
    ref_names=("head",),
)


def _valid_merge_branch(value: Any) -> str:
    branch = str(value or "").strip()
    if (
        not MERGE_BRANCH_RE.fullmatch(branch)
        or branch.startswith(("-", ".", "/"))
        or branch.endswith((".", "/", ".lock"))
        or ".." in branch
        or "//" in branch
        or "@{" in branch
        or "\\" in branch
    ):
        return ""
    return branch


def _merge_request(
    config: RefreshConfig,
    value: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    _expect_fields(
        value,
        {
            "schemaVersion",
            "type",
            "action",
            "owner",
            "repository",
            "pullNumber",
            "requestId",
            "expectedBaseOid",
            "expectedHeadOid",
            "expectedPullsOid",
        },
        optional={"overrideReview", "deleteHead"},
        label="merge executor request",
    )
    override_review = value.get("overrideReview", False)
    delete_head = value.get("deleteHead", False)
    if (
        value.get("schemaVersion") != 1
        or value.get("type") != operation.executor_type
        or value.get("action") not in {"execute", "status", "register"}
        or not isinstance(override_review, bool)
        or not isinstance(delete_head, bool)
        # Only merging has a review gate, so only merging may waive one.
        or (override_review and operation is not MERGE_OPERATION)
        or (delete_head and operation is not MERGE_OPERATION)
    ):
        raise RefreshError("merge executor request protocol is unsupported")
    owner = str(value.get("owner") or "").strip().lower()
    repository = str(value.get("repository") or "").strip()
    request_id = str(value.get("requestId") or "").strip()
    try:
        pull_number = int(value.get("pullNumber"))
    except (TypeError, ValueError, OverflowError) as exc:
        raise RefreshError("merge executor request is invalid") from exc
    oids = {
        field: str(value.get(field) or "").strip().lower()
        for field in (
            "expectedBaseOid", "expectedHeadOid", "expectedPullsOid")
    }
    if (
        owner not in config.owner_aliases
        or repository.lower() != config.repository_name.lower()
        or not 1 <= pull_number <= 999_999_999
        or not MERGE_REQUEST_ID_RE.fullmatch(request_id)
        or any(not GIT_OBJECT_ID_RE.fullmatch(oid) for oid in oids.values())
        or len({len(oid) for oid in oids.values()}) != 1
    ):
        raise RefreshError("merge executor request is invalid")
    request = {
        "owner": owner,
        "repository": config.repository_name,
        "pullNumber": pull_number,
        "requestId": request_id,
        **oids,
    }
    # Only added when set: a request that waives the review gate must not share
    # a digest, and so an idempotency record, with the reviewed merge of the
    # same objects, while every ordinary request keeps the digest it has had.
    if override_review:
        request["overrideReview"] = True
    if delete_head:
        request["deleteHead"] = True
    request["requestDigest"] = hashlib.sha256(
        _canonical_json(request)
    ).hexdigest()
    request["action"] = str(value["action"])
    return request


def _merge_request_token(request: Mapping[str, Any]) -> str:
    return hashlib.sha256(
        str(request["requestId"]).encode("ascii")
    ).hexdigest()


def _merge_state_directory(config: RefreshConfig, name: str) -> Path:
    root = _require_owner_directory(
        config.identity_state_directory, "identity state directory")
    path = root / name
    try:
        path.mkdir(mode=0o700)
    except FileExistsError:
        pass
    except OSError as exc:
        raise RefreshError("merge state directory cannot be created") from exc
    return _require_owner_directory(path, "merge state directory")


def _merge_jobs_directory(
    config: RefreshConfig,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> Path:
    return _merge_state_directory(config, operation.jobs_directory)


def _merge_job_path(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> Path:
    return _merge_jobs_directory(config, operation=operation) / (
        _merge_request_token(request) + ".json")


def _validate_merge_job_record(
    record: Mapping[str, Any],
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    expected = {
        "schemaVersion", "type", "requestDigest", "requestId", "status",
        "error", "baseBefore", "head", "pullsBefore",
    } | set(operation.after_fields)
    if (
        set(record) != expected
        or record.get("schemaVersion") != 1
        or record.get("type") != operation.job_type
        or record.get("requestId") != request["requestId"]
        or record.get("requestDigest") != request["requestDigest"]
        or record.get("status") not in {operation.terminal_status, "failed"}
        or (
            record.get("status") == "failed"
            and record.get("error") not in operation.failures
        )
        or any(
            value and not GIT_OBJECT_ID_RE.fullmatch(str(value))
            for value in [
                record.get("baseBefore"), record.get("head"),
                record.get("pullsBefore"),
            ] + [record.get(field) for field in operation.after_fields]
        )
    ):
        raise RefreshError("merge request id was already used")
    return dict(record)


def _merge_git(
    config: RefreshConfig,
    arguments: Iterable[str],
    *,
    input_bytes: bytes | None = None,
    index_file: Path | None = None,
    object_directory: Path | None = None,
    commit_identity: bool = False,
    maximum_output: int = MAX_JSON_OUTPUT_BYTES,
    allowed_codes: frozenset[int] = frozenset({0}),
) -> tuple[int, bytes]:
    environment = _safe_environment()
    if index_file is not None:
        environment["GIT_INDEX_FILE"] = str(index_file)
    if object_directory is not None:
        environment["GIT_OBJECT_DIRECTORY"] = str(object_directory)
    if commit_identity:
        environment.update({
            "GIT_AUTHOR_NAME": "ForkMesh merge node",
            "GIT_AUTHOR_EMAIL": "merge@forkmesh.invalid",
            "GIT_COMMITTER_NAME": "ForkMesh merge node",
            "GIT_COMMITTER_EMAIL": "merge@forkmesh.invalid",
        })
    with tempfile.TemporaryFile(mode="w+b") as output:
        try:
            completed = subprocess.run(
                _git_prefix(config) + list(arguments),
                input=input_bytes,
                stdout=output,
                stderr=subprocess.DEVNULL,
                env=environment,
                timeout=PROCESS_TIMEOUT_SECONDS,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise RefreshError("merge Git operation failed") from exc
        if completed.returncode not in allowed_codes:
            raise RefreshError("merge Git operation failed")
        size = output.tell()
        if size > maximum_output:
            raise RefreshError("merge Git operation returned too much data")
        output.seek(0)
        return completed.returncode, output.read(maximum_output + 1)


def _merge_oid(
    config: RefreshConfig, ref: str, *, object_type: str = "commit"
) -> str:
    _code, raw = _merge_git(
        config,
        ["rev-parse", "--verify", "--quiet", "--end-of-options",
         ref + "^{" + object_type + "}"],
        maximum_output=256,
    )
    oid = raw.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(oid):
        raise RefreshError("merge Git object is invalid")
    return oid


def _merge_job_record(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any] | None:
    path = _merge_job_path(config, request, operation=operation)
    try:
        path.lstat()
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise RefreshError("merge job record cannot be inspected") from exc
    try:
        record = _read_secure_json(
            path,
            label="merge job record",
            maximum=MAX_MERGE_REQUEST_BYTES,
            owner_only=True,
        )
    except RefreshError:
        raise
    return _validate_merge_job_record(record, request, operation=operation)


def _merge_cleanup_job_records(
    config: RefreshConfig,
    *,
    keep: Path | None = None,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    """Apply a hard local bound without ever rewriting a surviving record."""
    directory = _merge_jobs_directory(config, operation=operation)
    now = time.time()
    entries: list[tuple[float, Path]] = []
    for path in directory.iterdir():
        if path == keep or not re.fullmatch(r"[0-9a-f]{64}\.json", path.name):
            continue
        info = _lstat_no_symlink(path, "merge job record")
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) & 0o077
        ):
            raise RefreshError("merge job record permissions are unsafe")
        entries.append((info.st_mtime, path))
    entries.sort(key=lambda item: (item[0], item[1].name))
    expired = [
        path for modified, path in entries
        if now - modified > LOCAL_MERGE_JOB_RETENTION_SECONDS
    ]
    survivors = len(entries) - len(expired) + (1 if keep is not None else 0)
    excess = max(0, survivors - MAX_LOCAL_MERGE_JOBS)
    victims = expired + [
        path for _modified, path in entries
        if path not in expired
    ][:excess]
    changed = False
    for path in dict.fromkeys(victims):
        try:
            path.unlink()
            changed = True
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise RefreshError("merge job retention cleanup failed") from exc
    if changed:
        _fsync_directory(directory)


def _merge_store_record(
    config: RefreshConfig,
    request: Mapping[str, Any],
    record: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    checked = _validate_merge_job_record(record, request, operation=operation)
    existing = _merge_job_record(config, request, operation=operation)
    if existing is not None:
        return existing
    final = _merge_job_path(config, request, operation=operation)
    _merge_cleanup_job_records(config, keep=final, operation=operation)
    staged = _write_staged_json(final.parent, final.name, checked)
    try:
        try:
            os.link(staged, final, follow_symlinks=False)
        except FileExistsError:
            pass
        staged.unlink()
        _fsync_directory(final.parent)
    except OSError as exc:
        try:
            staged.unlink()
        except OSError:
            pass
        raise RefreshError("merge job record write failed") from exc
    stored = _merge_job_record(config, request, operation=operation)
    if stored is None:
        raise RefreshError("merge job record write failed")
    return stored


def _merge_record_value(
    request: Mapping[str, Any],
    *,
    status: str,
    error: str = "",
    base_after: str = "",
    pulls_after: str = "",
    operation: PullOperation = MERGE_OPERATION,
    after: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    published = dict(after or {})
    if operation is MERGE_OPERATION:
        published = {"baseAfter": base_after, "pullsAfter": pulls_after}
    return {
        "schemaVersion": 1,
        "type": operation.job_type,
        "requestDigest": request["requestDigest"],
        "requestId": request["requestId"],
        "status": status,
        "error": error,
        "baseBefore": request["expectedBaseOid"],
        "head": request["expectedHeadOid"],
        "pullsBefore": request["expectedPullsOid"],
        **{field: published.get(field, "") for field in operation.after_fields},
    }


def _merge_store_failure(
    config: RefreshConfig,
    request: Mapping[str, Any],
    error: str,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    if error not in operation.failures:
        raise RefreshError("merge failure code is invalid")
    existing = _merge_job_record(config, request, operation=operation)
    if existing is not None:
        return existing
    record = _merge_record_value(
        request, status="failed", error=error, operation=operation)
    return _merge_store_record(config, request, record, operation=operation)


def _merge_pull_metadata(
    raw: bytes,
    request: Mapping[str, Any],
    default_branch: str,
) -> tuple[bytes, str, str] | None:
    if (
        not raw
        or len(raw) > MAX_PULL_METADATA_BYTES
        or b"\x00" in raw
        or b"\r" in raw
    ):
        return None
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return None
    lines = text.split("\n")
    if not lines or lines[0] != "---":
        return None
    try:
        close = lines.index("---", 1)
    except ValueError:
        return None
    fields: list[tuple[str, str]] = []
    values: dict[str, str] = {}
    for line in lines[1:close]:
        if ": " not in line:
            return None
        key, value = line.split(": ", 1)
        if (
            not re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", key)
            or key in values
            or "\n" in value
        ):
            return None
        fields.append((key, value))
        values[key] = value
    base = _valid_merge_branch(values.get("base"))
    head = _valid_merge_branch(values.get("head"))
    try:
        number = int(values.get("number", "0"))
    except ValueError:
        return None
    if (
        values.get("schema") != "forkmesh-pull-v1"
        or number != request["pullNumber"]
        or values.get("status") != "open"
        or values.get("derive") != "branch"
        or not base
        or not head
        or base == head
        or base != default_branch
        or values.get("creationBaseOid", "").lower()
        != request["expectedBaseOid"]
        or values.get("creationHeadOid", "").lower()
        != request["expectedHeadOid"]
    ):
        return None
    # PullStore's established author signature binds title/base/head and the
    # immutable patch/mbox reconstructed from creationBaseOid/creationHeadOid.
    # It deliberately does not bind owner-applied lifecycle fields. Preserve
    # every signed input and the signature byte-for-byte; change only status and
    # the merge snapshot, exactly like PullStore::setStatus/merge.
    replacements = {
        "status": "merged",
        "mergeBase": request["expectedBaseOid"],
        "mergeHead": request["expectedHeadOid"],
    }
    # A merge-conditional reward estimate stops waiting the moment the pull
    # lands. Leaving it at pending_merge holds the contributor's payment out of
    # the payout queue and keeps the public ledger printing "pending merge" for
    # work that is already in, which is exactly what happened to pull #32. This
    # is the same transition PullReward::advanceForMerge applies on the desktop,
    # applied here because a merge run through a mirror rewrites pull.md itself.
    if values.get("rewardStatus") == "pending_merge" and values.get(
            "rewardCondition", "merged") == "merged":
        replacements["rewardStatus"] = "pending_wallet"
    output_fields = []
    seen = set()
    for key, value in fields:
        if key in replacements:
            value = replacements[key]
            seen.add(key)
        output_fields.append((key, value))
    for key in ("mergeBase", "mergeHead"):
        if key not in seen:
            output_fields.append((key, replacements[key]))
    encoded = (
        "---\n"
        + "\n".join(key + ": " + value for key, value in output_fields)
        + "\n---\n"
        + "\n".join(lines[close + 1:])
    ).encode("utf-8")
    if len(encoded) > MAX_PULL_METADATA_BYTES:
        return None
    return encoded, base, head


def _merge_metadata_blob(
    config: RefreshConfig,
    request: Mapping[str, Any],
) -> tuple[bytes, str] | None:
    path = ".forkmesh/pulls/%d/pull.md" % request["pullNumber"]
    try:
        _code, entry = _merge_git(
            config,
            ["ls-tree", "-z", request["expectedPullsOid"], "--", path],
            maximum_output=1024,
        )
        _code, raw = _merge_git(
            config,
            ["show", request["expectedPullsOid"] + ":" + path],
            maximum_output=MAX_PULL_METADATA_BYTES,
        )
    except RefreshError:
        return None
    match = re.fullmatch(
        rb"(100644|100755) blob ([0-9a-f]{40}|[0-9a-f]{64})\t"
        + re.escape(path.encode("utf-8"))
        + rb"\x00",
        entry,
    )
    if not match:
        return None
    return raw, match.group(1).decode("ascii")


def _merge_front_matter_values(raw: bytes) -> dict[str, str] | None:
    if not raw or len(raw) > MAX_PULL_METADATA_BYTES or b"\x00" in raw:
        return None
    try:
        lines = raw.decode("utf-8").split("\n")
        close = lines.index("---", 1)
    except (UnicodeDecodeError, ValueError):
        return None
    if not lines or lines[0] != "---":
        return None
    values: dict[str, str] = {}
    for line in lines[1:close]:
        if ": " not in line:
            return None
        key, value = line.split(": ", 1)
        if key in values or not re.fullmatch(
                r"[A-Za-z][A-Za-z0-9]*", key):
            return None
        values[key] = value
    return values


def _merge_peer_review_gate(
    config: RefreshConfig,
    request: Mapping[str, Any],
    raw_metadata: bytes,
) -> bool:
    """Require one distinct peer approval and no unresolved peer objection.

    Review files on the owner-published pull metadata ref have already passed
    PullStore/Worker signature validation. This executor still parses only a
    bounded allowlisted projection and ignores any review authored by the PR
    signer.
    """
    metadata = _merge_front_matter_values(raw_metadata)
    pull_author = str((metadata or {}).get("author") or "")
    if not pull_author:
        return False
    prefix = ".forkmesh/pulls/%d/" % request["pullNumber"]
    try:
        _code, listing = _merge_git(
            config,
            ["ls-tree", "-r", "-z", "--name-only",
             request["expectedPullsOid"], "--", prefix],
            maximum_output=256 * 1024,
        )
    except RefreshError:
        return False
    paths = sorted(
        path.decode("utf-8")
        for path in listing.split(b"\x00")
        if path and re.fullmatch(
            rb"\.forkmesh/pulls/[1-9][0-9]*/[0-9]{4,}-review\.md", path)
    )[:1000]
    latest: dict[str, str] = {}
    for path in paths:
        try:
            _code, raw = _merge_git(
                config,
                ["show", request["expectedPullsOid"] + ":" + path],
                maximum_output=MAX_PULL_METADATA_BYTES,
            )
        except RefreshError:
            return False
        values = _merge_front_matter_values(raw)
        if not values or values.get("type") != "review":
            continue
        reviewer = str(values.get("author") or "")
        state = str(values.get("state") or "")
        if not reviewer or reviewer == pull_author or not values.get("sig"):
            continue
        if state in ("approved", "changes_requested"):
            latest[reviewer] = state
        else:
            latest.pop(reviewer, None)
    return (
        any(state == "approved" for state in latest.values())
        and not any(
            state == "changes_requested" for state in latest.values())
    )


def _merge_policy_present(
    config: RefreshConfig, request: Mapping[str, Any], path: str
) -> bool:
    try:
        _code, entry = _merge_git(
            config,
            ["ls-tree", "-z", request["expectedBaseOid"], "--", path],
            maximum_output=4096,
        )
    except RefreshError:
        return True
    return bool(entry)


def _merge_required_checks_policy_present(
    config: RefreshConfig, request: Mapping[str, Any]
) -> bool:
    """Fail closed on a base branch that requires signed status checks.

    The Go mirror node verifies ``forkmesh-check-v1`` attestations against
    ``.forkmesh/required-checks.json`` on the reviewed base commit.  This
    executor has no Ed25519 verifier, so it refuses every merge such a policy
    gates instead of silently becoming the path around it.  An unreadable base
    tree counts as gated.
    """
    try:
        _code, entry = _merge_git(
            config,
            ["ls-tree", "-z", request["expectedBaseOid"], "--",
             REQUIRED_CHECKS_POLICY_PATH],
            maximum_output=4096,
        )
    except RefreshError:
        return True
    return bool(entry)


def _merge_commit_tree(
    config: RefreshConfig,
    tree: str,
    parents: list[str],
    message: str,
    *,
    object_directory: Path,
) -> str:
    arguments = ["commit-tree", tree]
    for parent in parents:
        arguments.extend(["-p", parent])
    _code, raw = _merge_git(
        config,
        arguments,
        input_bytes=(message.strip() + "\n").encode("utf-8"),
        object_directory=object_directory,
        commit_identity=True,
        maximum_output=256,
    )
    oid = raw.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(oid):
        raise RefreshError("merge commit object is invalid")
    return oid


def _merge_code_commit(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    object_directory: Path,
) -> str | None:
    base = request["expectedBaseOid"]
    head = request["expectedHeadOid"]
    code, _raw = _merge_git(
        config,
        ["merge-base", "--is-ancestor", base, head],
        object_directory=object_directory,
        maximum_output=64,
        allowed_codes=frozenset({0, 1}),
    )
    if code == 0:
        return head
    code, _raw = _merge_git(
        config,
        ["merge-base", "--is-ancestor", head, base],
        object_directory=object_directory,
        maximum_output=64,
        allowed_codes=frozenset({0, 1}),
    )
    if code == 0:
        return base
    code, raw = _merge_git(
        config,
        ["merge-tree", "--write-tree", base, head],
        object_directory=object_directory,
        maximum_output=256 * 1024,
        allowed_codes=frozenset({0, 1}),
    )
    if code != 0:
        return None
    first = raw.splitlines()[0].decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(first):
        raise RefreshError("merge tree object is invalid")
    return _merge_commit_tree(
        config,
        first,
        [base, head],
        "Merge pull request #%d" % request["pullNumber"],
        object_directory=object_directory,
    )


def _merge_attributes_safe(
    config: RefreshConfig, base: str, head: str
) -> bool:
    """Reject repository-configured external low-level merge drivers.

    ``merge-tree`` does not run hooks or check out files, but Git attributes
    can select a named driver whose command is read from local repository
    configuration. Online merges support only Git's built-in text, binary, and
    union drivers; a custom driver requires an operator-side review instead.
    """
    info_attributes = config.source_repository / "info" / "attributes"
    try:
        info = info_attributes.lstat()
    except FileNotFoundError:
        info = None
    except OSError:
        return False
    if info is not None and info.st_size:
        return False
    blobs = set()
    for commit in (base, head):
        try:
            _code, raw = _merge_git(
                config,
                ["ls-tree", "-r", "-z", commit],
                maximum_output=MAX_REPOSITORY_METADATA_BYTES,
            )
        except RefreshError:
            return False
        for entry in raw.split(b"\x00"):
            if not entry:
                continue
            match = re.fullmatch(
                rb"[0-9]{6} blob ([0-9a-f]{40}|[0-9a-f]{64})\t(.*)",
                entry,
            )
            if not match:
                continue
            path = match.group(2)
            if not (
                path == b".gitattributes"
                or path.endswith(b"/.gitattributes")
            ):
                continue
            oid = match.group(1).decode("ascii")
            if len(blobs) >= 256:
                return False
            blobs.add(oid)
    allowed = {b"text", b"binary", b"union"}
    total = 0
    for blob in blobs:
        try:
            _code, raw = _merge_git(
                config, ["cat-file", "blob", blob],
                maximum_output=256 * 1024)
        except RefreshError:
            return False
        total += len(raw)
        if total > 1024 * 1024:
            return False
        for match in re.finditer(
                rb"(?:^|[ \t])merge=([^ \t\r\n#]+)", raw, re.MULTILINE):
            if match.group(1).lower() not in allowed:
                return False
    return True


def _merge_metadata_commit(
    config: RefreshConfig,
    request: Mapping[str, Any],
    updated: bytes,
    mode: str,
    *,
    object_directory: Path,
) -> str:
    path = ".forkmesh/pulls/%d/pull.md" % request["pullNumber"]
    # Pull metadata is a committed UTF-8 document, so hash its exact bytes
    # rather than the canonical-JSON encoding used for job records.
    _code, raw = _merge_git(
        config,
        ["hash-object", "-w", "--stdin"],
        input_bytes=updated,
        object_directory=object_directory,
        maximum_output=256,
    )
    blob = raw.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(blob):
        raise RefreshError("pull metadata object is invalid")
    with tempfile.TemporaryDirectory(
        prefix=".merge-index-", dir=config.identity_state_directory
    ) as temporary:
        index = Path(temporary) / "index"
        _merge_git(
            config, ["read-tree", request["expectedPullsOid"]],
            index_file=index, object_directory=object_directory,
            maximum_output=256)
        _merge_git(
            config,
            ["update-index", "--add", "--cacheinfo", mode, blob, path],
            index_file=index,
            object_directory=object_directory,
            maximum_output=256,
        )
        # A merged pull retains metadata and review history, but its patch and
        # commit-series payload are open-PR data. Remove both from the exact
        # metadata tree atomically with the status transition.
        removals = b"".join(
            b"0 " + blob.encode("ascii") + b"\t" +
            (".forkmesh/pulls/%d/%s" %
             (request["pullNumber"], payload)).encode("utf-8") + b"\n"
            for payload in ("changes.patch", "commits.mbox")
        )
        _merge_git(
            config,
            ["update-index", "--index-info"],
            input_bytes=removals,
            index_file=index,
            object_directory=object_directory,
            maximum_output=256,
        )
        _code, tree_raw = _merge_git(
            config, ["write-tree"], index_file=index,
            object_directory=object_directory, maximum_output=256)
    tree = tree_raw.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(tree):
        raise RefreshError("pull metadata tree is invalid")
    # The waiver is recorded where the merge itself is recorded: the ledger
    # commit that closes the pull says the owner merged it unreviewed.
    message = "pull #%d: merged" % request["pullNumber"]
    if request.get("overrideReview"):
        message += " (review gate waived by owner)"
    return _merge_commit_tree(
        config,
        tree,
        [request["expectedPullsOid"]],
        message,
        object_directory=object_directory,
    )


def _merge_quarantine_root(
    config: RefreshConfig,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> Path:
    return _merge_state_directory(config, operation.quarantine_directory)


def _merge_quarantine_path(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> Path:
    return _merge_quarantine_root(
        config, operation=operation) / _merge_request_token(request)


def _merge_staging_refs(
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> tuple[str, ...]:
    prefix = operation.staging_prefix + _merge_request_token(request) + "/"
    return tuple(prefix + name for name in operation.ref_names)


def _merge_completed_refs(
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> tuple[str, ...]:
    prefix = operation.completed_prefix + _merge_request_token(request) + "/"
    return tuple(prefix + name for name in operation.ref_names)


def _merge_ref_pair(
    config: RefreshConfig, refs: tuple[str, ...]
) -> tuple[str, ...] | None:
    values: list[str | None] = []
    for ref in refs:
        try:
            values.append(_merge_oid(config, ref))
        except RefreshError:
            values.append(None)
    if all(value is None for value in values):
        return None
    if any(value is None for value in values):
        raise RefreshError("merge transaction refs are incomplete")
    return tuple(str(value) for value in values)


def _merge_write_owner_file(path: Path, content: bytes) -> None:
    if not content or len(content) > MAX_CONFIG_BYTES:
        raise RefreshError("merge transaction state is invalid")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
        try:
            _write_all(descriptor, content)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError as exc:
        raise RefreshError("merge transaction state write failed") from exc


def _merge_create_quarantine(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> tuple[Path, Path]:
    path = _merge_quarantine_path(config, request, operation=operation)
    try:
        path.mkdir(mode=0o700)
        objects = path / "objects"
        objects.mkdir(mode=0o700)
        info = objects / "info"
        info.mkdir(mode=0o700)
    except OSError as exc:
        raise RefreshError("merge object quarantine cannot be created") from exc
    source_objects = config.source_repository / "objects"
    source_info = _lstat_no_symlink(
        source_objects, "source repository object directory")
    if (
        not stat.S_ISDIR(source_info.st_mode)
        or source_info.st_uid != os.geteuid()
        or stat.S_IMODE(source_info.st_mode) & stat.S_IWOTH
        or "\n" in str(source_objects)
    ):
        raise RefreshError("source repository object directory is unsafe")
    _merge_write_owner_file(
        info / "alternates", (str(source_objects) + "\n").encode("utf-8"))
    _fsync_directory(info)
    _fsync_directory(objects)
    _fsync_directory(path)
    _fsync_directory(path.parent)
    return path, objects


def _merge_remove_quarantine(
    config: RefreshConfig,
    path: Path,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    root = _merge_quarantine_root(config, operation=operation)
    if (
        path.parent != root
        or not re.fullmatch(r"[0-9a-f]{64}", path.name)
    ):
        raise RefreshError("merge object quarantine path is invalid")
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    except OSError as exc:
        raise RefreshError("merge object quarantine cannot be inspected") from exc
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o077
    ):
        raise RefreshError("merge object quarantine permissions are unsafe")
    try:
        shutil.rmtree(path)
        _fsync_directory(root)
    except OSError as exc:
        raise RefreshError("merge object quarantine cleanup failed") from exc


def _merge_quarantine_record(
    value: Mapping[str, Any],
    request: Mapping[str, Any] | None = None,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    expected = {
        "schemaVersion", "type", "requestDigest", "requestId", "pullNumber",
        "expectedBaseOid", "expectedHeadOid", "expectedPullsOid",
        "baseBranch", "headBranch",
    } | set(operation.after_fields)
    if value.get("deleteHead") is True:
        expected.add("deleteHead")
    branches = (
        _valid_merge_branch(value.get("baseBranch")),
        _valid_merge_branch(value.get("headBranch")),
    )
    oids = [
        str(value.get(field) or "")
        for field in (
            "expectedBaseOid", "expectedHeadOid", "expectedPullsOid",
        ) + operation.after_fields
    ]
    try:
        number = int(value.get("pullNumber"))
    except (TypeError, ValueError, OverflowError):
        number = 0
    if (
        set(value) != expected
        or value.get("schemaVersion") != 1
        or value.get("type") != operation.quarantine_type
        or not SHA256_RE.fullmatch(str(value.get("requestDigest") or ""))
        or not MERGE_REQUEST_ID_RE.fullmatch(str(value.get("requestId") or ""))
        or not 1 <= number <= 999_999_999
        or not all(branches)
        or branches[0] == branches[1]
        or any(not GIT_OBJECT_ID_RE.fullmatch(oid) for oid in oids)
        or len({len(oid) for oid in oids}) != 1
        or (request is not None and bool(value.get("deleteHead"))
            != bool(request.get("deleteHead")))
    ):
        raise RefreshError("merge transaction journal is invalid")
    if request is not None and (
        value.get("requestId") != request["requestId"]
        or value.get("requestDigest") != request["requestDigest"]
        or number != request["pullNumber"]
        or any(
            value.get(field) != request[field]
            for field in (
                "expectedBaseOid", "expectedHeadOid", "expectedPullsOid")
        )
    ):
        raise RefreshError("merge request id was already used")
    return dict(value)


def _merge_load_quarantine(
    config: RefreshConfig,
    path: Path,
    request: Mapping[str, Any] | None = None,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    journal = _read_secure_json(
        path / "journal.json",
        label="merge transaction journal",
        maximum=MAX_MERGE_REQUEST_BYTES,
        owner_only=True,
    )
    checked = _merge_quarantine_record(journal, request, operation=operation)
    if _merge_request_token(checked) != path.name:
        raise RefreshError("merge transaction journal identity is invalid")
    return checked


def _merge_write_quarantine(
    path: Path,
    journal: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    checked = _merge_quarantine_record(journal, operation=operation)
    staged = _write_staged_json(path, "journal.json", checked)
    final = path / "journal.json"
    try:
        os.link(staged, final, follow_symlinks=False)
        staged.unlink()
        _fsync_directory(path)
    except OSError as exc:
        try:
            staged.unlink()
        except OSError:
            pass
        raise RefreshError("merge transaction journal write failed") from exc


def _merge_copy_object_cross_device(
    source: Path, destination: Path, size: int
) -> None:
    temporary = destination.parent / (
        ".forkmesh-merge-" + secrets.token_hex(12) + ".tmp")
    source_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    source_flags |= getattr(os, "O_NOFOLLOW", 0)
    output_flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    output_flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    source_fd = -1
    output_fd = -1
    try:
        source_fd = os.open(source, source_flags)
        output_fd = os.open(temporary, output_flags, 0o444)
        copied = 0
        while copied < size:
            chunk = os.read(source_fd, min(1024 * 1024, size - copied))
            if not chunk:
                break
            _write_all(output_fd, chunk)
            copied += len(chunk)
        if copied != size or os.read(source_fd, 1):
            raise RefreshError("merge quarantine object changed while copying")
        os.fsync(output_fd)
        try:
            os.link(temporary, destination, follow_symlinks=False)
        except FileExistsError:
            pass
    except (OSError, RefreshError) as exc:
        if isinstance(exc, RefreshError):
            raise
        raise RefreshError("merge quarantine object installation failed") from exc
    finally:
        if source_fd >= 0:
            os.close(source_fd)
        if output_fd >= 0:
            os.close(output_fd)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _merge_install_quarantine_objects(
    config: RefreshConfig,
    path: Path,
    journal: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    objects = _require_owner_directory(
        path / "objects", "merge quarantine object directory")
    alternates = _read_bounded_file(
        objects / "info" / "alternates",
        label="merge quarantine alternate",
        maximum=4096,
        owner_only=True,
    )
    expected_alternate = (
        str(config.source_repository / "objects") + "\n").encode("utf-8")
    if not secrets.compare_digest(alternates, expected_alternate):
        raise RefreshError("merge quarantine alternate is invalid")
    object_id_length = len(str(journal["expectedBaseOid"]))
    source_objects = config.source_repository / "objects"
    destination_info = _lstat_no_symlink(
        source_objects, "source repository object directory")
    if (
        not stat.S_ISDIR(destination_info.st_mode)
        or destination_info.st_uid != os.geteuid()
        or stat.S_IMODE(destination_info.st_mode) & stat.S_IWOTH
    ):
        raise RefreshError("source repository object directory is unsafe")
    count = 0
    total = 0
    changed_directories: set[Path] = set()
    for shard in objects.iterdir():
        if shard.name == "info":
            continue
        if not re.fullmatch(r"[0-9a-f]{2}", shard.name):
            raise RefreshError("merge quarantine contains unexpected data")
        shard_info = _lstat_no_symlink(
            shard, "merge quarantine object shard")
        if (
            not stat.S_ISDIR(shard_info.st_mode)
            or shard_info.st_uid != os.geteuid()
        ):
            raise RefreshError("merge quarantine object shard is unsafe")
        try:
            os.chmod(shard, 0o700, follow_symlinks=False)
        except OSError as exc:
            raise RefreshError("merge quarantine object shard is unsafe") from exc
        destination_shard = source_objects / shard.name
        try:
            destination_shard.mkdir(mode=0o755)
        except FileExistsError:
            pass
        except OSError as exc:
            raise RefreshError(
                "source repository object shard cannot be created") from exc
        installed_shard = _lstat_no_symlink(
            destination_shard, "source repository object shard")
        if (
            not stat.S_ISDIR(installed_shard.st_mode)
            or installed_shard.st_uid != os.geteuid()
            or stat.S_IMODE(installed_shard.st_mode) & stat.S_IWOTH
        ):
            raise RefreshError("source repository object shard is unsafe")
        for source in shard.iterdir():
            if not re.fullmatch(
                    r"[0-9a-f]{%d}" % (object_id_length - 2), source.name):
                raise RefreshError("merge quarantine object name is invalid")
            info = _lstat_no_symlink(source, "merge quarantine object")
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) & 0o022
                or info.st_size <= 0
            ):
                raise RefreshError("merge quarantine object is unsafe")
            count += 1
            total += info.st_size
            if (
                count > MAX_MERGE_QUARANTINE_OBJECTS
                or total > MAX_MERGE_QUARANTINE_BYTES
            ):
                raise RefreshError("merge quarantine is too large")
            destination = destination_shard / source.name
            try:
                os.link(source, destination, follow_symlinks=False)
            except FileExistsError:
                pass
            except OSError as exc:
                if exc.errno != errno.EXDEV:
                    raise RefreshError(
                        "merge quarantine object installation failed") from exc
                _merge_copy_object_cross_device(
                    source, destination, info.st_size)
            changed_directories.add(destination_shard)
    for directory in changed_directories:
        _fsync_directory(directory)
    if changed_directories:
        _fsync_directory(source_objects)
    for field in operation.after_fields:
        oid = journal[field]
        if _merge_oid(config, str(oid)) != oid:
            raise RefreshError("installed merge object is invalid")


def _merge_journal_after(
    journal: Mapping[str, Any],
    operation: PullOperation = MERGE_OPERATION,
) -> tuple[str, ...]:
    return tuple(str(journal[field]) for field in operation.after_fields)


def _merge_ensure_staging_refs(
    config: RefreshConfig,
    request: Mapping[str, Any],
    journal: Mapping[str, Any],
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    refs = _merge_staging_refs(request, operation=operation)
    expected = _merge_journal_after(journal, operation)
    current = _merge_ref_pair(config, refs)
    if current is not None:
        if current != expected:
            raise RefreshError("merge staging refs do not match the journal")
        return
    lines = [
        "start",
        *(
            "create %s %s" % (ref, oid)
            for ref, oid in zip(refs, expected)
        ),
        "prepare",
        "commit",
        "",
    ]
    try:
        _merge_git(
            config,
            ["update-ref", "--stdin"],
            input_bytes="\n".join(lines).encode("ascii"),
            maximum_output=1024,
        )
    except RefreshError:
        current = _merge_ref_pair(config, refs)
        if current != expected:
            raise


def _merge_recover_quarantine(
    config: RefreshConfig,
    path: Path,
    request: Mapping[str, Any] | None = None,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> dict[str, Any]:
    journal = _merge_load_quarantine(
        config, path, request, operation=operation)
    _merge_install_quarantine_objects(
        config, path, journal, operation=operation)
    derived_request = request or {
        "requestId": journal["requestId"],
    }
    _merge_ensure_staging_refs(
        config, derived_request, journal, operation=operation)
    return journal


def _recover_merge_quarantines(
    config: RefreshConfig,
    *,
    operation: PullOperation = MERGE_OPERATION,
) -> None:
    root = _merge_quarantine_root(config, operation=operation)
    paths = list(root.iterdir())
    if len(paths) > 128:
        raise RefreshError("too many pending merge quarantines")
    for path in paths:
        if (
            not re.fullmatch(r"[0-9a-f]{64}", path.name)
            or stat.S_ISLNK(path.lstat().st_mode)
        ):
            raise RefreshError("merge quarantine entry is invalid")
        journal = path / "journal.json"
        try:
            journal.lstat()
        except FileNotFoundError:
            # Object writes cannot enter the source repository until the
            # durable journal exists, so an incomplete build is safe to drop.
            _merge_remove_quarantine(config, path, operation=operation)
            continue
        _merge_recover_quarantine(config, path, operation=operation)


def _merge_prepare_quarantine(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    base_branch: str,
    head_branch: str,
    updated_metadata: bytes,
    metadata_mode: str,
) -> dict[str, Any] | None:
    path = _merge_quarantine_path(config, request)
    try:
        path.lstat()
    except FileNotFoundError:
        path, objects = _merge_create_quarantine(config, request)
    else:
        return _merge_recover_quarantine(config, path, request)
    journal_written = False
    try:
        base_after = _merge_code_commit(
            config, request, object_directory=objects)
        if base_after is None:
            _merge_remove_quarantine(config, path)
            return None
        pulls_after = _merge_metadata_commit(
            config,
            request,
            updated_metadata,
            metadata_mode,
            object_directory=objects,
        )
        journal = {
            "schemaVersion": 1,
            "type": MERGE_QUARANTINE_TYPE,
            "requestDigest": request["requestDigest"],
            "requestId": request["requestId"],
            "pullNumber": request["pullNumber"],
            "expectedBaseOid": request["expectedBaseOid"],
            "expectedHeadOid": request["expectedHeadOid"],
            "expectedPullsOid": request["expectedPullsOid"],
            "baseBranch": base_branch,
            "headBranch": head_branch,
            "baseAfter": base_after,
            "pullsAfter": pulls_after,
            **({"deleteHead": True} if request.get("deleteHead") else {}),
        }
        _merge_write_quarantine(path, journal)
        journal_written = True
        return _merge_recover_quarantine(config, path, request)
    except Exception:
        if not journal_written:
            _merge_remove_quarantine(config, path)
        raise


def _merge_atomic_update(
    config: RefreshConfig,
    request: Mapping[str, Any],
    journal: Mapping[str, Any],
) -> None:
    staging = _merge_staging_refs(request)
    completed = _merge_completed_refs(request)
    metadata_after, metadata_before, compatibility_before, metadata_staged = (
        _merge_isolated_metadata_commit(
            config, request, str(journal["pullsAfter"])
        )
    )
    metadata_ref = "refs/heads/forkmesh/pull/%d" % request["pullNumber"]
    compatibility_ref = "refs/pr/%d/metadata" % request["pullNumber"]
    metadata_staging_ref = staging[0].rsplit("/", 1)[0] + "/metadata"
    _merge_git(
        config,
        ["update-ref", metadata_staging_ref, metadata_after,
         metadata_staged or ("0" * len(metadata_after))],
        maximum_output=256,
    )
    lines = ["start"]
    if not request.get("deleteHead"):
        lines.append("verify refs/heads/%s %s" % (
            journal["headBranch"], request["expectedHeadOid"]))
    lines.extend([
        "update refs/heads/%s %s %s" % (
            journal["baseBranch"], journal["baseAfter"],
            request["expectedBaseOid"]),
        "update refs/heads/forkmesh/pulls %s %s" % (
            journal["pullsAfter"], request["expectedPullsOid"]),
    ])
    if request.get("deleteHead"):
        lines.append("delete refs/heads/%s %s" % (
            journal["headBranch"], request["expectedHeadOid"]))
    if metadata_before:
        lines.append("delete %s %s" % (metadata_ref, metadata_before))
    lines.append(
        ("update %s %s %s" %
         (compatibility_ref, metadata_after, compatibility_before))
        if compatibility_before else
        ("create %s %s" % (compatibility_ref, metadata_after))
    )
    lines.extend([
        "create %s %s" % (completed[0], journal["baseAfter"]),
        "create %s %s" % (completed[1], journal["pullsAfter"]),
        "delete %s %s" % (staging[0], journal["baseAfter"]),
        "delete %s %s" % (staging[1], journal["pullsAfter"]),
        "delete %s %s" % (metadata_staging_ref, metadata_after),
        "prepare",
        "commit",
        "",
    ])
    _merge_git(
        config,
        ["update-ref", "--stdin"],
        input_bytes="\n".join(lines).encode("ascii"),
        maximum_output=1024,
    )


def _merge_optional_ref_oid(config: RefreshConfig, ref: str) -> str:
    code, raw = _merge_git(
        config,
        ["rev-parse", "--verify", "--quiet", "--end-of-options",
         ref + "^{commit}"],
        maximum_output=256,
        allowed_codes=frozenset({0, 1}),
    )
    if code != 0:
        return ""
    oid = raw.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(oid):
        raise RefreshError("pull metadata ref is invalid")
    return oid


def _merge_isolated_metadata_commit(
    config: RefreshConfig, request: Mapping[str, Any], aggregate_tip: str
) -> tuple[str, str, str, str]:
    """Create a commit containing only one PR's metadata subtree."""
    number = int(request["pullNumber"])
    metadata_ref = "refs/heads/forkmesh/pull/%d" % number
    compatibility_ref = "refs/pr/%d/metadata" % number
    metadata_staging_ref = (
        _merge_staging_refs(request)[0].rsplit("/", 1)[0] + "/metadata"
    )
    before = _merge_optional_ref_oid(config, metadata_ref)
    compatibility_before = _merge_optional_ref_oid(
        config, compatibility_ref)
    staged_before = _merge_optional_ref_oid(config, metadata_staging_ref)
    source = "%s^{tree}:.forkmesh/pulls/%d" % (aggregate_tip, number)
    _code, raw_pull_tree = _merge_git(
        config,
        ["rev-parse", "--verify", "--quiet", "--end-of-options", source],
        maximum_output=256,
    )
    pull_tree = raw_pull_tree.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(pull_tree):
        raise RefreshError("isolated pull metadata tree is invalid")
    tree = pull_tree
    for name in (str(number), "pulls", ".forkmesh"):
        _code, raw_tree = _merge_git(
            config,
            ["mktree"],
            input_bytes=("040000 tree %s\t%s\n" % (tree, name)).encode(
                "ascii"
            ),
            maximum_output=256,
        )
        tree = raw_tree.decode("ascii", "ignore").strip().lower()
        if not GIT_OBJECT_ID_RE.fullmatch(tree):
            raise RefreshError("isolated pull metadata tree is invalid")
    if not GIT_OBJECT_ID_RE.fullmatch(tree):
        raise RefreshError("isolated pull metadata tree is invalid")
    arguments = ["commit-tree", tree]
    parent = staged_before or before or compatibility_before
    if parent:
        arguments.extend(["-p", parent])
    _code, raw_commit = _merge_git(
        config,
        arguments,
        input_bytes=("pull #%d: metadata snapshot\n" % number).encode("utf-8"),
        commit_identity=True,
        maximum_output=256,
    )
    after = raw_commit.decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(after):
        raise RefreshError("isolated pull metadata commit is invalid")
    return after, before, compatibility_before, staged_before


def _merge_finalize_quarantine(
    config: RefreshConfig,
    request: Mapping[str, Any],
    journal: Mapping[str, Any],
) -> dict[str, Any]:
    expected = (str(journal["baseAfter"]), str(journal["pullsAfter"]))
    completed = _merge_ref_pair(config, _merge_completed_refs(request))
    if completed is not None:
        if completed != expected:
            raise RefreshError("merge completion refs do not match the journal")
        record = _merge_record_value(
            request,
            status="merged",
            base_after=expected[0],
            pulls_after=expected[1],
        )
        stored = _merge_store_record(config, request, record)
        _merge_remove_quarantine(
            config, _merge_quarantine_path(config, request))
        return stored
    try:
        _merge_atomic_update(config, request, journal)
    except RefreshError:
        completed = _merge_ref_pair(config, _merge_completed_refs(request))
        if completed is not None:
            if completed != expected:
                raise RefreshError(
                    "merge completion refs do not match the journal")
            record = _merge_record_value(
                request,
                status="merged",
                base_after=expected[0],
                pulls_after=expected[1],
            )
            stored = _merge_store_record(config, request, record)
        else:
            # Exact refs moved after validation. The hidden staging pair remains
            # reachable, so speculative objects cannot become dangling.
            stored = _merge_store_failure(
                config, request, "stale_pull_metadata")
        _merge_remove_quarantine(
            config, _merge_quarantine_path(config, request))
        return stored
    record = _merge_record_value(
        request,
        status="merged",
        base_after=expected[0],
        pulls_after=expected[1],
    )
    stored = _merge_store_record(config, request, record)
    _merge_remove_quarantine(
        config, _merge_quarantine_path(config, request))
    return stored


def _merge_execute_locked(
    config: RefreshConfig, request: Mapping[str, Any]
) -> dict[str, Any]:
    existing = _merge_job_record(config, request)
    if existing is not None:
        return existing
    _recover_merge_quarantines(config)
    quarantine = _merge_quarantine_path(config, request)
    try:
        quarantine.lstat()
    except FileNotFoundError:
        quarantine_exists = False
    else:
        quarantine_exists = True
    if quarantine_exists:
        journal = _merge_recover_quarantine(config, quarantine, request)
        return _merge_finalize_quarantine(config, request, journal)
    # A completion marker without its owner-only journal/record is a consumed
    # idempotency id (for example, after bounded record expiry), never authority
    # to reinterpret an old merge as a new request.
    if _merge_ref_pair(config, _merge_completed_refs(request)) is not None:
        raise RefreshError("merge request id was already used")
    if not config.catalog.branch:
        return _merge_store_failure(
            config, request, "unsupported_pull")
    base_branch = _valid_merge_branch(config.catalog.branch)
    if not base_branch:
        raise RefreshError("configured merge base branch is invalid")
    try:
        current_base = _merge_oid(
            config, "refs/heads/" + base_branch)
        current_pulls = _merge_oid(
            config, "refs/heads/forkmesh/pulls")
    except RefreshError:
        return _merge_store_failure(
            config, request, "stale_pull_metadata")
    if current_base != request["expectedBaseOid"]:
        return _merge_store_failure(config, request, "stale_base")
    if current_pulls != request["expectedPullsOid"]:
        return _merge_store_failure(
            config, request, "stale_pull_metadata")
    metadata = _merge_metadata_blob(config, request)
    if metadata is None:
        return _merge_store_failure(config, request, "pull_not_found")
    raw_metadata, mode = metadata
    parsed = _merge_pull_metadata(raw_metadata, request, base_branch)
    if parsed is None:
        # Deliberately one error for closed, malformed, legacy-patch, and
        # creation-OID-mismatched records: none are safe to merge online.
        return _merge_store_failure(config, request, "unsupported_pull")
    if not request.get("overrideReview") and not _merge_peer_review_gate(
            config, request, raw_metadata):
        return _merge_store_failure(config, request, "review_required")
    if _merge_required_checks_policy_present(config, request):
        return _merge_store_failure(config, request, "checks_required")
    if _merge_policy_present(config, request, PULL_RULES_POLICY_PATH):
        return _merge_store_failure(config, request, "rules_required")
    updated, _base, head_branch = parsed
    try:
        current_head = _merge_oid(
            config, "refs/heads/" + head_branch)
    except RefreshError:
        return _merge_store_failure(config, request, "stale_head")
    if current_head != request["expectedHeadOid"]:
        return _merge_store_failure(config, request, "stale_head")
    if not _merge_attributes_safe(
            config, request["expectedBaseOid"], request["expectedHeadOid"]):
        return _merge_store_failure(config, request, "unsupported_pull")
    journal = _merge_prepare_quarantine(
        config,
        request,
        base_branch=base_branch,
        head_branch=head_branch,
        updated_metadata=updated,
        metadata_mode=mode,
    )
    if journal is None:
        return _merge_store_failure(config, request, "merge_conflict")
    return _merge_finalize_quarantine(config, request, journal)


# --- Authenticated update-from-base executor --------------------------------
#
# "Update branch" merges the base branch into the pull request's head branch so
# the reviewed commits are tested against current base code.  It publishes one
# new commit on one branch: the base branch and the pull-metadata branch are
# verified at their exact expected OIDs and never move, and the pull record is
# not rewritten, so nothing an author signed is touched.


def _update_pull_metadata(
    raw: bytes,
    request: Mapping[str, Any],
    default_branch: str,
) -> tuple[str, str, str] | None:
    """Project the pinned pull record onto the branches this may move."""
    if not raw or len(raw) > MAX_PULL_METADATA_BYTES or b"\r" in raw:
        return None
    values = _merge_front_matter_values(raw)
    if not values:
        return None
    base = _valid_merge_branch(values.get("base"))
    head = _valid_merge_branch(values.get("head"))
    try:
        number = int(values.get("number", "0"))
    except ValueError:
        return None
    creation_head = str(values.get("creationHeadOid", "")).strip().lower()
    if (
        values.get("schema") != "forkmesh-pull-v1"
        or number != request["pullNumber"]
        or values.get("status") != "open"
        or values.get("derive") != "branch"
        or not base
        or not head
        or base == head
        or base != default_branch
        or not GIT_OBJECT_ID_RE.fullmatch(creation_head)
        or len(creation_head) != len(request["expectedHeadOid"])
    ):
        return None
    return base, head, creation_head


def _update_head_commit(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    base_branch: str,
    head_branch: str,
    object_directory: Path,
) -> str | None:
    """Build the head branch's post-update commit, or None on conflict."""
    base = request["expectedBaseOid"]
    head = request["expectedHeadOid"]
    code, _raw = _merge_git(
        config,
        ["merge-base", "--is-ancestor", head, base],
        object_directory=object_directory,
        maximum_output=64,
        allowed_codes=frozenset({0, 1}),
    )
    if code == 0:
        # Every reviewed commit is already contained in the base branch, so the
        # update is an ordinary fast-forward and creates no new object.
        return base
    code, raw = _merge_git(
        config,
        ["merge-tree", "--write-tree", head, base],
        object_directory=object_directory,
        maximum_output=256 * 1024,
        allowed_codes=frozenset({0, 1}),
    )
    if code != 0:
        return None
    first = raw.splitlines()[0].decode("ascii", "ignore").strip().lower()
    if not GIT_OBJECT_ID_RE.fullmatch(first):
        raise RefreshError("merge tree object is invalid")
    # First parent stays the head branch's own history, exactly as a local
    # "git merge <base>" run on the checked-out head branch would record it.
    return _merge_commit_tree(
        config,
        first,
        [head, base],
        "Merge branch '%s' into %s" % (base_branch, head_branch),
        object_directory=object_directory,
    )


def _update_prepare_quarantine(
    config: RefreshConfig,
    request: Mapping[str, Any],
    *,
    base_branch: str,
    head_branch: str,
) -> dict[str, Any] | None:
    path = _merge_quarantine_path(
        config, request, operation=UPDATE_OPERATION)
    try:
        path.lstat()
    except FileNotFoundError:
        path, objects = _merge_create_quarantine(
            config, request, operation=UPDATE_OPERATION)
    else:
        return _merge_recover_quarantine(
            config, path, request, operation=UPDATE_OPERATION)
    journal_written = False
    try:
        head_after = _update_head_commit(
            config,
            request,
            base_branch=base_branch,
            head_branch=head_branch,
            object_directory=objects,
        )
        if head_after is None:
            _merge_remove_quarantine(
                config, path, operation=UPDATE_OPERATION)
            return None
        journal = {
            "schemaVersion": 1,
            "type": UPDATE_QUARANTINE_TYPE,
            "requestDigest": request["requestDigest"],
            "requestId": request["requestId"],
            "pullNumber": request["pullNumber"],
            "expectedBaseOid": request["expectedBaseOid"],
            "expectedHeadOid": request["expectedHeadOid"],
            "expectedPullsOid": request["expectedPullsOid"],
            "baseBranch": base_branch,
            "headBranch": head_branch,
            "headAfter": head_after,
        }
        _merge_write_quarantine(path, journal, operation=UPDATE_OPERATION)
        journal_written = True
        return _merge_recover_quarantine(
            config, path, request, operation=UPDATE_OPERATION)
    except Exception:
        if not journal_written:
            _merge_remove_quarantine(
                config, path, operation=UPDATE_OPERATION)
        raise


def _update_atomic_update(
    config: RefreshConfig,
    request: Mapping[str, Any],
    journal: Mapping[str, Any],
) -> None:
    staging = _merge_staging_refs(request, operation=UPDATE_OPERATION)
    completed = _merge_completed_refs(request, operation=UPDATE_OPERATION)
    lines = [
        "start",
        "verify refs/heads/%s %s" % (
            journal["baseBranch"], request["expectedBaseOid"]),
        "verify refs/heads/forkmesh/pulls %s" % request["expectedPullsOid"],
        "update refs/heads/%s %s %s" % (
            journal["headBranch"], journal["headAfter"],
            request["expectedHeadOid"]),
        "create %s %s" % (completed[0], journal["headAfter"]),
        "delete %s %s" % (staging[0], journal["headAfter"]),
        "prepare",
        "commit",
        "",
    ]
    _merge_git(
        config,
        ["update-ref", "--stdin"],
        input_bytes="\n".join(lines).encode("ascii"),
        maximum_output=1024,
    )


def _update_finalize_quarantine(
    config: RefreshConfig,
    request: Mapping[str, Any],
    journal: Mapping[str, Any],
) -> dict[str, Any]:
    expected = (str(journal["headAfter"]),)

    def landed() -> dict[str, Any]:
        record = _merge_record_value(
            request,
            status=UPDATE_OPERATION.terminal_status,
            operation=UPDATE_OPERATION,
            after={"headAfter": expected[0]},
        )
        return _merge_store_record(
            config, request, record, operation=UPDATE_OPERATION)

    quarantine = _merge_quarantine_path(
        config, request, operation=UPDATE_OPERATION)
    completed = _merge_ref_pair(
        config, _merge_completed_refs(request, operation=UPDATE_OPERATION))
    if completed is not None:
        if completed != expected:
            raise RefreshError(
                "merge completion refs do not match the journal")
        stored = landed()
        _merge_remove_quarantine(
            config, quarantine, operation=UPDATE_OPERATION)
        return stored
    try:
        _update_atomic_update(config, request, journal)
    except RefreshError:
        completed = _merge_ref_pair(
            config,
            _merge_completed_refs(request, operation=UPDATE_OPERATION),
        )
        if completed is not None:
            if completed != expected:
                raise RefreshError(
                    "merge completion refs do not match the journal")
            stored = landed()
        else:
            # An exact ref moved between validation and the transaction. The
            # hidden staging ref still anchors the speculative commit, so no
            # object becomes dangling and the caller can retry with fresh OIDs.
            stored = _merge_store_failure(
                config, request, "stale_head", operation=UPDATE_OPERATION)
        _merge_remove_quarantine(
            config, quarantine, operation=UPDATE_OPERATION)
        return stored
    stored = landed()
    _merge_remove_quarantine(config, quarantine, operation=UPDATE_OPERATION)
    return stored


def _update_execute_locked(
    config: RefreshConfig, request: Mapping[str, Any]
) -> dict[str, Any]:
    existing = _merge_job_record(
        config, request, operation=UPDATE_OPERATION)
    if existing is not None:
        return existing
    _recover_merge_quarantines(config, operation=UPDATE_OPERATION)
    quarantine = _merge_quarantine_path(
        config, request, operation=UPDATE_OPERATION)
    try:
        quarantine.lstat()
    except FileNotFoundError:
        quarantine_exists = False
    else:
        quarantine_exists = True
    if quarantine_exists:
        journal = _merge_recover_quarantine(
            config, quarantine, request, operation=UPDATE_OPERATION)
        return _update_finalize_quarantine(config, request, journal)
    if _merge_ref_pair(
            config,
            _merge_completed_refs(request, operation=UPDATE_OPERATION),
    ) is not None:
        raise RefreshError("merge request id was already used")
    base_branch = _valid_merge_branch(config.catalog.branch)
    if not config.catalog.branch:
        return _merge_store_failure(
            config, request, "unsupported_pull", operation=UPDATE_OPERATION)
    if not base_branch:
        raise RefreshError("configured merge base branch is invalid")
    try:
        current_base = _merge_oid(config, "refs/heads/" + base_branch)
        current_pulls = _merge_oid(config, "refs/heads/forkmesh/pulls")
    except RefreshError:
        return _merge_store_failure(
            config, request, "stale_pull_metadata",
            operation=UPDATE_OPERATION)
    if current_base != request["expectedBaseOid"]:
        return _merge_store_failure(
            config, request, "stale_base", operation=UPDATE_OPERATION)
    if current_pulls != request["expectedPullsOid"]:
        return _merge_store_failure(
            config, request, "stale_pull_metadata",
            operation=UPDATE_OPERATION)
    metadata = _merge_metadata_blob(config, request)
    if metadata is None:
        return _merge_store_failure(
            config, request, "pull_not_found", operation=UPDATE_OPERATION)
    raw_metadata, _mode = metadata
    parsed = _update_pull_metadata(raw_metadata, request, base_branch)
    if parsed is None:
        return _merge_store_failure(
            config, request, "unsupported_pull", operation=UPDATE_OPERATION)
    _base, head_branch, creation_head = parsed
    try:
        current_head = _merge_oid(config, "refs/heads/" + head_branch)
    except RefreshError:
        return _merge_store_failure(
            config, request, "stale_head", operation=UPDATE_OPERATION)
    if current_head != request["expectedHeadOid"]:
        return _merge_store_failure(
            config, request, "stale_head", operation=UPDATE_OPERATION)
    # The branch named by the pinned record must still descend from the commit
    # the pull was opened at; a rebased or recycled branch of the same name is
    # a different change set and is never updated on the author's behalf.
    code, _raw = _merge_git(
        config,
        ["merge-base", "--is-ancestor", creation_head,
         request["expectedHeadOid"]],
        maximum_output=64,
        allowed_codes=frozenset({0, 1}),
    )
    if code != 0:
        return _merge_store_failure(
            config, request, "unsupported_pull", operation=UPDATE_OPERATION)
    code, _raw = _merge_git(
        config,
        ["merge-base", "--is-ancestor", request["expectedBaseOid"],
         request["expectedHeadOid"]],
        maximum_output=64,
        allowed_codes=frozenset({0, 1}),
    )
    if code == 0:
        return _merge_store_failure(
            config, request, "already_current", operation=UPDATE_OPERATION)
    if not _merge_attributes_safe(
            config, request["expectedHeadOid"], request["expectedBaseOid"]):
        return _merge_store_failure(
            config, request, "unsupported_pull", operation=UPDATE_OPERATION)
    journal = _update_prepare_quarantine(
        config,
        request,
        base_branch=base_branch,
        head_branch=head_branch,
    )
    if journal is None:
        return _merge_store_failure(
            config, request, "merge_conflict", operation=UPDATE_OPERATION)
    return _update_finalize_quarantine(config, request, journal)


def _update_public_result(
    config: RefreshConfig,
    request: Mapping[str, Any],
    record: Mapping[str, Any] | None,
) -> dict[str, Any]:
    if record is None:
        return {
            "ok": True,
            "status": "missing",
            "requestId": request["requestId"],
            "generationReady": False,
        }
    if record.get("status") == "failed":
        return {
            "ok": False,
            "status": "failed",
            "requestId": request["requestId"],
            "error": record["error"],
            "generationReady": False,
        }
    return {
        "ok": True,
        "status": UPDATE_OPERATION.terminal_status,
        "requestId": request["requestId"],
        "baseBefore": record["baseBefore"],
        "head": record["head"],
        "pullsBefore": record["pullsBefore"],
        "headAfter": record["headAfter"],
        "generationReady": _merge_generation_ready(config),
    }


def update_executor(
    config: RefreshConfig, value: Mapping[str, Any]
) -> dict[str, Any]:
    if "update-pull-branch" not in config.operations:
        raise RefreshError("pull branch update capability is disabled")
    request = _merge_request(config, value, operation=UPDATE_OPERATION)
    action = request.pop("action")
    if action == "register":
        register(config)
        return {
            "ok": True,
            "status": "registered",
            "requestId": request["requestId"],
        }
    if action == "status":
        with _refresh_lock(config, shared=True):
            record = _merge_job_record(
                config, request, operation=UPDATE_OPERATION)
        return _update_public_result(config, request, record)
    with _refresh_lock(config, shared=False):
        record = _update_execute_locked(config, request)
    if (
        record.get("status") == UPDATE_OPERATION.terminal_status
        and not _merge_generation_ready(config)
    ):
        refresh(config)
    return _update_public_result(config, request, record)


def pull_executor(
    config: RefreshConfig, value: Mapping[str, Any]
) -> dict[str, Any]:
    """Route one stdin executor request to the mutation it declares."""
    declared = value.get("type") if isinstance(value, Mapping) else None
    if declared == UPDATE_EXECUTOR_TYPE:
        return update_executor(config, value)
    return merge_executor(config, value)


def _merge_generation_ready(config: RefreshConfig) -> bool:
    try:
        identity = _load_public_identity(config)
        metadata, _archive = _active_metadata(config, identity)
        return secrets.compare_digest(
            _source_refs_sha256(config),
            metadata.expected_refs_sha256,
        )
    except RefreshError:
        return False


def _merge_public_result(
    config: RefreshConfig,
    request: Mapping[str, Any],
    record: Mapping[str, Any] | None,
) -> dict[str, Any]:
    if record is None:
        return {
            "ok": True,
            "status": "missing",
            "requestId": request["requestId"],
            "generationReady": False,
        }
    if record.get("status") == "failed":
        return {
            "ok": False,
            "status": "failed",
            "requestId": request["requestId"],
            "error": record["error"],
            "generationReady": False,
        }
    return {
        "ok": True,
        "status": "merged",
        "requestId": request["requestId"],
        "baseBefore": record["baseBefore"],
        "head": record["head"],
        "pullsBefore": record["pullsBefore"],
        "baseAfter": record["baseAfter"],
        "pullsAfter": record["pullsAfter"],
        "generationReady": _merge_generation_ready(config),
    }


def merge_executor(
    config: RefreshConfig, value: Mapping[str, Any]
) -> dict[str, Any]:
    if "merge-pull" not in config.operations:
        raise RefreshError("pull merge capability is disabled")
    request = _merge_request(config, value)
    action = request.pop("action")
    if action == "register":
        register(config)
        return {
            "ok": True,
            "status": "registered",
            "requestId": request["requestId"],
        }
    if action == "status":
        with _refresh_lock(config, shared=True):
            record = _merge_job_record(config, request)
        return _merge_public_result(config, request, record)
    with _refresh_lock(config, shared=False):
        record = _merge_execute_locked(config, request)
    if record.get("status") == "merged" and not _merge_generation_ready(config):
        # Resealing is deliberately outside the Git transaction lock. refresh()
        # acquires the same exclusive lock, validates the exact post-merge refs,
        # and atomically swaps only a complete encrypted generation.
        refresh(config)
    return _merge_public_result(config, request, record)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Refresh and publish one encrypted headless ForkMesh mirror "
            "without loading node, wallet, or repository-decryption secrets"
        )
    )
    parser.add_argument(
        "--config",
        required=True,
        type=Path,
        help="absolute owner-only refresh JSON configuration",
    )
    parser.add_argument(
        "mode",
        choices=(
            "refresh",
            "check",
            "register",
            "renew",
            "configure-actions",
            "merge-execute",
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = load_config(args.config)
        if args.mode in {"merge-execute", "configure-actions"}:
            maximum = (
                MAX_MERGE_REQUEST_BYTES
                if args.mode == "merge-execute"
                else MAX_ACTIONS_CONFIGURATION_BYTES
            )
            raw = sys.stdin.buffer.read(maximum + 1)
            request = _parse_json(
                raw,
                maximum=maximum,
                label=(
                    "merge executor request"
                    if args.mode == "merge-execute"
                    else "Actions configuration request"
                ),
            )
            result = (
                pull_executor(config, request)
                if args.mode == "merge-execute"
                else configure_actions(config, request)
            )
        else:
            result = {
                "refresh": refresh,
                "check": check,
                "register": register,
                "renew": renew,
            }[args.mode](config)
        print(_canonical_json(result).decode("utf-8"), flush=True)
        return 0
    except RefreshError as exc:
        print(f"headless mirror refresh: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("headless mirror refresh: interrupted", file=sys.stderr)
        return 130
    except Exception:
        # Unexpected library/OS exceptions can interpolate paths or request
        # values.  Fail closed with a fixed message instead of leaking them.
        print("headless mirror refresh: unexpected local failure", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
