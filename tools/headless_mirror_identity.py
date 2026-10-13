#!/usr/bin/env python3
"""Owner-local identity and encrypted-archive helper for headless mirrors.

This executable is the non-GUI counterpart of the Qt mirror identity helper.
It deliberately exposes a small set of typed JSON protocols over stdin/stdout:

* create an owner-only Ed25519 node identity and native age identity;
* sign endpoint manifests and health challenges with the node identity;
* verify masked-routing capabilities against one pinned router public key;
* atomically seal a bare Git snapshot as an age-encrypted tar archive;
* safely materialize that archive into an empty owner-only destination; and
* sign the bounded account-reclaim, endpoint-registration, and public
  catalog-v2/state payloads used by the Worker.

Private keys never enter argv, JSON configuration, stdout, or a child-process
environment.  The age executable receives only the owner-only identity *path*;
the secret identity bytes remain in that file.
"""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from typing import Any, BinaryIO, Callable, Iterable, Iterator, Mapping
from urllib.parse import urlsplit

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey,
        Ed25519PublicKey,
    )
except ImportError as exc:  # pragma: no cover - exercised on minimal hosts
    raise SystemExit(
        "headless mirror identity: the cryptography package is required"
    ) from exc


SCHEMA_VERSION = 1
MAX_REQUEST_BYTES = 128 * 1024
MAX_MANIFEST_BYTES = 64 * 1024
MAX_HELPER_OUTPUT_BYTES = 64 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 250_000
PROCESS_TIMEOUT_SECONDS = 30 * 60
NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{12,80}$")
NONCE_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BASE64URL_RE = re.compile(r"^[A-Za-z0-9_-]+$")
LINK_CODE_RE = re.compile(r"^[0-9]{6}$")
AGE_RECIPIENT_RE = re.compile(r"^age1[023456789acdefghjklmnpqrstuvwxyz]{20,100}$")
AGE_SECRET_RE = re.compile(
    rb"^AGE-SECRET-KEY-1[023456789ACDEFGHJKLMNPQRSTUVWXYZ]{20,180}$"
)
DNS_LABEL_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
AGE_NATIVE_HEADER = b"age-encryption.org/v1\n"
AGE_ARMORED_HEADER = b"-----BEGIN AGE ENCRYPTED FILE-----\n"
PUBLIC_STATE_FIELDS = frozenset(
    {
        "schemaVersion",
        "type",
        "nodeName",
        "nodePublicKey",
        "ageRecipient",
        "ageKeyReference",
        "routerPublicKey",
        "allowedOrigins",
    }
)
# Bounded client classes a node may report for its last served clone / website
# read. Raw User-Agent strings are never published (see mirror_gateway.py).
SERVE_AGENT_CLASSES = frozenset(
    {
        "forkmesh-node",
        "git-client",
        "bot-tool",
        "browser",
        "client",
    }
)
STORAGE_PURPOSES = frozenset(
    {
        "service-discovery",
        "health-metadata",
        "routing-metadata",
        "signed-manifests",
    }
)
PUBLIC_CATALOG_INPUT_FIELDS = frozenset(
    {
        "owner",
        "name",
        "visibility",
        "sizeBytes",
        "description",
        "logoMetadata",
        "cloneUrl",
        "mirrorUrl",
        "solana",
        "channel",
        "hostedSince",
        "lastSync",
        "updatedAt",
        "rootCommit",
        "source",
        "commit",
        "branch",
        "commitSubject",
        "commitAuthorName",
        "commitAt",
        "issueCount",
        "issueMaxNumber",
        "commitCount",
        "mergeCount",
        "branchCount",
        "pullCount",
        "discussionCount",
        "activityWeeks",
        "mergeActivityWeeks",
        "worktreeCount",
        "artifactCount",
        "platform",
        "version",
        "actionsEnabled",
        "actionsState",
        "nodeId",
        "clonesServed",
        "websiteServed",
        "cloneServedAt",
        "cloneServedAgent",
        "websiteServedAt",
        "websiteServedAgent",
        "cpuPercent",
        "memUsedBytes",
        "memTotalBytes",
        "diskUsedBytes",
        "diskTotalBytes",
        "maintainer",
        "stateHash",
    }
)
FORBIDDEN_RESPONSE_FIELD_RE = re.compile(
    r"(?:private.?key|secret|seed|mnemonic|keypair|password|credential|token)",
    re.IGNORECASE,
)
ALLOWED_PUBLIC_KEY_FIELDS = frozenset(
    {
        "publicKey",
        "nodePublicKey",
        "routerPublicKey",
        "ageKeyReference",
        "keyReference",
        "ciphertextSha256",
        "expectedRefsSha256",
        "payloadSha256",
        "messageSha256",
        "stateHash",
    }
)


class HelperError(RuntimeError):
    """A failure whose message never contains request or secret material."""


@dataclass(frozen=True)
class PublicState:
    node_name: str
    node_public_key: str
    age_recipient: str
    age_key_reference: str
    router_public_key: str
    allowed_origins: tuple[str, ...]

    def as_json(self) -> dict[str, Any]:
        return {
            "schemaVersion": SCHEMA_VERSION,
            "type": "forkmesh.headless-mirror-identity",
            "nodeName": self.node_name,
            "nodePublicKey": self.node_public_key,
            "ageRecipient": self.age_recipient,
            "ageKeyReference": self.age_key_reference,
            "routerPublicKey": self.router_public_key,
            "allowedOrigins": list(self.allowed_origins),
        }


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise HelperError("JSON contains a duplicate field")
        result[key] = value
    return result


def _parse_json_bytes(raw: bytes, *, maximum: int) -> dict[str, Any]:
    if not raw or len(raw) > maximum:
        raise HelperError("JSON request size is invalid")
    try:
        value = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HelperError("JSON request is invalid") from exc
    if not isinstance(value, dict):
        raise HelperError("JSON request must be an object")
    return value


def _read_request(stream: BinaryIO = sys.stdin.buffer) -> dict[str, Any]:
    raw = stream.read(MAX_REQUEST_BYTES + 1)
    return _parse_json_bytes(raw, maximum=MAX_REQUEST_BYTES)


def _canonical_json(value: Any, *, ensure_ascii: bool = False) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=ensure_ascii,
    ).encode("utf-8")


def _b64url_encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def _b64url_decode(value: Any, expected_size: int | None = None) -> bytes:
    if not isinstance(value, str) or not BASE64URL_RE.fullmatch(value):
        raise HelperError("base64url value is invalid")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except (ValueError, TypeError) as exc:
        raise HelperError("base64url value is invalid") from exc
    if _b64url_encode(decoded) != value:
        raise HelperError("base64url value is not canonical")
    if expected_size is not None and len(decoded) != expected_size:
        raise HelperError("base64url value has an invalid size")
    return decoded


def _validate_public_key(value: Any) -> str:
    _b64url_decode(value, 32)
    return str(value)


def _validate_signature(value: Any) -> str:
    _b64url_decode(value, 64)
    return str(value)


def _validate_node(value: Any) -> str:
    node = str(value or "").strip().lower()
    if not NODE_RE.fullmatch(node):
        raise HelperError("node name is invalid")
    return node


def _validate_dns_name(value: str) -> str:
    name = value.strip().lower().rstrip(".")
    labels = name.split(".")
    if (
        len(name) > 253
        or len(labels) < 2
        or any(not DNS_LABEL_RE.fullmatch(label) for label in labels)
    ):
        raise HelperError("public origin hostname is invalid")
    return name


def _normalize_origin(value: Any) -> str:
    try:
        parsed = urlsplit(str(value or "").strip())
        port = parsed.port
    except (ValueError, TypeError) as exc:
        raise HelperError("public origin is invalid") from exc
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or port not in (None, 443)
    ):
        raise HelperError("public origin must be a credential-free HTTPS origin")
    hostname = _validate_dns_name(parsed.hostname)
    return "https://" + hostname


def _validate_origins(value: Any) -> tuple[str, ...]:
    if (
        not isinstance(value, list)
        or len(value) > 16
        or any(not isinstance(item, str) for item in value)
    ):
        raise HelperError("allowedOrigins must be a bounded string array")
    origins: list[str] = []
    for item in value:
        origin = _normalize_origin(item)
        if origin not in origins:
            origins.append(origin)
    return tuple(origins)


def _expect_fields(
    value: Mapping[str, Any],
    expected: Iterable[str],
    *,
    context: str = "request",
) -> None:
    if set(value) != set(expected):
        raise HelperError(f"{context} contains an unknown or missing field")


def _require_protocol(
    request: Mapping[str, Any], type_name: str, fields: Iterable[str]
) -> None:
    _expect_fields(request, {"schemaVersion", "type", *fields})
    if (
        request.get("schemaVersion") != SCHEMA_VERSION
        or request.get("type") != type_name
    ):
        raise HelperError("request protocol is unsupported")


def _safe_environment() -> dict[str, str]:
    # Whitelist rather than trying to enumerate every cloud, package-manager,
    # SSH-agent, CI, and application credential variable a host may carry.
    return {
        "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
        "LANG": "C",
        "LC_ALL": "C",
    }


def _resolve_program(name: str) -> str:
    candidate = shutil.which(name)
    if not candidate:
        raise HelperError("a required local executable is unavailable")
    resolved = Path(candidate).resolve(strict=True)
    info = resolved.stat()
    if (
        not stat.S_ISREG(info.st_mode)
        or not os.access(resolved, os.X_OK)
        or stat.S_IMODE(info.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
        or info.st_uid not in {0, os.geteuid()}
    ):
        raise HelperError("a required local executable is unsafe")
    return str(resolved)


def _check_owner(path: Path, info: os.stat_result) -> None:
    if info.st_uid != os.geteuid():
        raise HelperError("owner-only storage has an unexpected owner")


def _require_owner_directory(
    path: Path, *, create: bool = False, empty: bool = False
) -> Path:
    if not path.is_absolute():
        raise HelperError("owner-only directory must be absolute")
    try:
        info = path.lstat()
    except FileNotFoundError:
        if not create:
            raise HelperError("owner-only directory is unavailable")
        parent = path.parent.resolve(strict=True)
        parent_info = parent.stat()
        _check_owner(parent, parent_info)
        if not stat.S_ISDIR(parent_info.st_mode) or stat.S_IMODE(
            parent_info.st_mode
        ) & (stat.S_IWGRP | stat.S_IWOTH):
            raise HelperError("owner-only directory parent is unsafe")
        path.mkdir(mode=0o700)
        info = path.lstat()
    _check_owner(path, info)
    mode = stat.S_IMODE(info.st_mode)
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or mode & 0o077
        or mode & 0o700 != 0o700
    ):
        raise HelperError("owner-only directory permissions are unsafe")
    if empty and any(path.iterdir()):
        raise HelperError("materialization destination must be empty")
    return path.resolve(strict=True)


def _require_private_file(
    path: Path, *, maximum: int = MAX_REQUEST_BYTES
) -> os.stat_result:
    try:
        info = path.lstat()
    except FileNotFoundError as exc:
        raise HelperError("owner-only identity is unavailable") from exc
    _check_owner(path, info)
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_nlink != 1
        or stat.S_IMODE(info.st_mode) & 0o077
        or info.st_size <= 0
        or info.st_size > maximum
    ):
        raise HelperError("owner-only identity file is unsafe")
    return info


def _read_private_file(path: Path, *, maximum: int) -> bytes:
    expected = _require_private_file(path, maximum=maximum)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        actual = os.fstat(descriptor)
        if (
            actual.st_dev != expected.st_dev
            or actual.st_ino != expected.st_ino
            or actual.st_size != expected.st_size
        ):
            raise HelperError("owner-only identity changed while loading")
        chunks = bytearray()
        while len(chunks) <= maximum:
            chunk = os.read(descriptor, min(64 * 1024, maximum + 1 - len(chunks)))
            if not chunk:
                break
            chunks.extend(chunk)
        if len(chunks) != actual.st_size or len(chunks) > maximum:
            raise HelperError("owner-only identity file is invalid")
        return bytes(chunks)
    finally:
        os.close(descriptor)


def _write_all(descriptor: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise HelperError("owner-only state write failed")
        view = view[written:]


def _atomic_write(path: Path, data: bytes, *, replace: bool) -> None:
    temporary = path.with_name("." + path.name + "." + secrets.token_hex(12) + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, 0o600)
    try:
        _write_all(descriptor, data)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        if replace:
            os.replace(temporary, path)
        else:
            os.link(temporary, path, follow_symlinks=False)
            temporary.unlink()
        os.chmod(path, 0o600, follow_symlinks=False)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


@contextmanager
def _state_lock(state_dir: Path, name: str = ".identity.lock") -> Iterator[None]:
    path = state_dir / name
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        _check_owner(path, info)
        if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) & 0o077:
            raise HelperError("owner-only lock file is unsafe")
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _node_key_path(state_dir: Path) -> Path:
    return state_dir / "node-ed25519.pem"


def _age_key_path(state_dir: Path) -> Path:
    return state_dir / "age-identity.txt"


def _public_state_path(state_dir: Path) -> Path:
    return state_dir / "public.json"


def _node_public_key(private_key: Ed25519PrivateKey) -> str:
    raw = private_key.public_key().public_bytes(
        serialization.Encoding.Raw,
        serialization.PublicFormat.Raw,
    )
    return _b64url_encode(raw)


def _load_node_key(state_dir: Path) -> Ed25519PrivateKey:
    raw = bytearray(_read_private_file(_node_key_path(state_dir), maximum=64 * 1024))
    try:
        key = serialization.load_pem_private_key(bytes(raw), password=None)
    except (TypeError, ValueError) as exc:
        raise HelperError("node identity is invalid") from exc
    finally:
        for index in range(len(raw)):
            raw[index] = 0
    if not isinstance(key, Ed25519PrivateKey):
        raise HelperError("node identity algorithm is invalid")
    return key


def _write_node_key(state_dir: Path) -> Ed25519PrivateKey:
    key = Ed25519PrivateKey.generate()
    encoded = bytearray(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    try:
        _atomic_write(_node_key_path(state_dir), bytes(encoded), replace=False)
    finally:
        for index in range(len(encoded)):
            encoded[index] = 0
    return key


def _run_age_keygen(
    program: str,
    output_path: Path,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> str:
    executable = _resolve_program(program)
    try:
        completed = runner(
            [executable, "-o", str(output_path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=_safe_environment(),
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise HelperError("age identity generation failed") from exc
    if completed.returncode != 0:
        raise HelperError("age identity generation failed")
    os.chmod(output_path, 0o600, follow_symlinks=False)
    return _derive_age_recipient(program, output_path, runner=runner)


def _derive_age_recipient(
    program: str,
    identity_path: Path,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> str:
    _require_private_file(identity_path, maximum=64 * 1024)
    raw = bytearray(_read_private_file(identity_path, maximum=64 * 1024))
    try:
        secret_lines = [
            line.strip()
            for line in bytes(raw).splitlines()
            if line.strip().startswith(b"AGE-SECRET-KEY-")
        ]
        if len(secret_lines) != 1 or not AGE_SECRET_RE.fullmatch(secret_lines[0]):
            raise HelperError("age identity file is invalid")
    finally:
        for index in range(len(raw)):
            raw[index] = 0
    executable = _resolve_program(program)
    try:
        completed = runner(
            [executable, "-y", str(identity_path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=_safe_environment(),
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise HelperError("age identity validation failed") from exc
    output = completed.stdout
    if isinstance(output, bytes):
        recipient = output.decode("ascii", "strict").strip()
    else:
        recipient = str(output or "").strip()
    if completed.returncode != 0 or not AGE_RECIPIENT_RE.fullmatch(recipient):
        raise HelperError("age identity validation failed")
    return recipient


def _write_age_identity(
    state_dir: Path,
    age_keygen_program: str,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> str:
    temporary = state_dir / (".age-identity." + secrets.token_hex(12) + ".tmp")
    try:
        recipient = _run_age_keygen(age_keygen_program, temporary, runner=runner)
        os.link(
            temporary,
            _age_key_path(state_dir),
            follow_symlinks=False,
        )
        os.chmod(_age_key_path(state_dir), 0o600, follow_symlinks=False)
        return recipient
    except FileExistsError as exc:
        raise HelperError("age identity already exists") from exc
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _age_key_reference(recipient: str) -> str:
    digest = hashlib.sha256(recipient.encode("ascii")).hexdigest()
    return "forkmesh-headless-age:" + digest[:32]


def _parse_public_state(value: Mapping[str, Any]) -> PublicState:
    _expect_fields(value, PUBLIC_STATE_FIELDS, context="public identity state")
    if (
        value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != "forkmesh.headless-mirror-identity"
    ):
        raise HelperError("public identity state is unsupported")
    node_name = _validate_node(value.get("nodeName"))
    node_public_key = _validate_public_key(value.get("nodePublicKey"))
    age_recipient = str(value.get("ageRecipient") or "")
    if not AGE_RECIPIENT_RE.fullmatch(age_recipient):
        raise HelperError("public age recipient is invalid")
    age_key_reference = str(value.get("ageKeyReference") or "")
    if age_key_reference != _age_key_reference(age_recipient):
        raise HelperError("public age key reference is invalid")
    router_public_key = _validate_public_key(value.get("routerPublicKey"))
    allowed_origins = _validate_origins(value.get("allowedOrigins"))
    return PublicState(
        node_name,
        node_public_key,
        age_recipient,
        age_key_reference,
        router_public_key,
        allowed_origins,
    )


def _load_public_state(state_dir: Path) -> PublicState:
    raw = _read_private_file(_public_state_path(state_dir), maximum=MAX_REQUEST_BYTES)
    return _parse_public_state(_parse_json_bytes(raw, maximum=MAX_REQUEST_BYTES))


def _write_public_state(state_dir: Path, state: PublicState) -> None:
    encoded = _canonical_json(state.as_json()) + b"\n"
    _atomic_write(_public_state_path(state_dir), encoded, replace=True)


def initialize_identity(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    age_keygen_program: str = "age-keygen",
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.headless-mirror-identity-init",
        {"nodeName", "routerPublicKey", "allowedOrigins"},
    )
    node_name = _validate_node(request.get("nodeName"))
    router_public_key = _validate_public_key(request.get("routerPublicKey"))
    allowed_origins = _validate_origins(request.get("allowedOrigins"))
    state_dir = _require_owner_directory(state_dir, create=True)
    with _state_lock(state_dir):
        public_path = _public_state_path(state_dir)
        if public_path.exists():
            existing = _load_public_state(state_dir)
            if (
                existing.node_name != node_name
                or existing.router_public_key != router_public_key
                or existing.allowed_origins != allowed_origins
            ):
                raise HelperError(
                    "existing identity configuration does not match init request"
                )
            if _node_public_key(_load_node_key(state_dir)) != (
                existing.node_public_key
            ):
                raise HelperError("node identity does not match public state")
            recipient = _derive_age_recipient(
                age_keygen_program, _age_key_path(state_dir), runner=runner
            )
            if recipient != existing.age_recipient:
                raise HelperError("age identity does not match public state")
            return existing.as_json()

        node_path = _node_key_path(state_dir)
        node_key = (
            _load_node_key(state_dir)
            if node_path.exists()
            else _write_node_key(state_dir)
        )
        age_path = _age_key_path(state_dir)
        age_recipient = (
            _derive_age_recipient(age_keygen_program, age_path, runner=runner)
            if age_path.exists()
            else _write_age_identity(state_dir, age_keygen_program, runner=runner)
        )
        state = PublicState(
            node_name=node_name,
            node_public_key=_node_public_key(node_key),
            age_recipient=age_recipient,
            age_key_reference=_age_key_reference(age_recipient),
            router_public_key=router_public_key,
            allowed_origins=allowed_origins,
        )
        _write_public_state(state_dir, state)
        return state.as_json()


def configure_public_state(
    state_dir: Path, request: Mapping[str, Any]
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.headless-mirror-identity-configure",
        {"routerPublicKey", "allowedOrigins"},
    )
    router_public_key = _validate_public_key(request.get("routerPublicKey"))
    allowed_origins = _validate_origins(request.get("allowedOrigins"))
    state_dir = _require_owner_directory(state_dir)
    with _state_lock(state_dir):
        current = _load_public_state(state_dir)
        updated = PublicState(
            node_name=current.node_name,
            node_public_key=current.node_public_key,
            age_recipient=current.age_recipient,
            age_key_reference=current.age_key_reference,
            router_public_key=router_public_key,
            allowed_origins=allowed_origins,
        )
        _write_public_state(state_dir, updated)
        return updated.as_json()


def _load_signing_identity(
    state_dir: Path,
) -> tuple[PublicState, Ed25519PrivateKey]:
    state_dir = _require_owner_directory(state_dir)
    state = _load_public_state(state_dir)
    key = _load_node_key(state_dir)
    if _node_public_key(key) != state.node_public_key:
        raise HelperError("node identity does not match public state")
    return state, key


def _message_request(
    request: Mapping[str, Any],
    *,
    type_name: str,
    include_signature: bool,
) -> tuple[str, bytes]:
    fields = {
        "algorithm",
        "encoding",
        "publicKey",
        "messageBase64",
        "messageSha256",
    }
    if include_signature:
        fields.add("signature")
    _require_protocol(request, type_name, fields)
    if (
        request.get("algorithm") != "Ed25519"
        or request.get("encoding") != "base64url-no-padding"
    ):
        raise HelperError("identity request algorithm is unsupported")
    public_key = _validate_public_key(request.get("publicKey"))
    payload = _b64url_decode(request.get("messageBase64"))
    if not payload or len(payload) > 16 * 1024:
        raise HelperError("identity request message size is invalid")
    if request.get("messageSha256") != hashlib.sha256(payload).hexdigest():
        raise HelperError("identity request message digest is invalid")
    return public_key, payload


def _validate_health_message(payload: bytes, node_name: str) -> None:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise HelperError("health challenge is invalid") from exc
    lines = text.split("\n")
    number_re = re.compile(r"^[1-9][0-9]{0,18}$")
    if (
        len(lines) == 4
        and lines[0] == "forkmesh-https-health-v1"
        and lines[1] == node_name
        and NONCE_RE.fullmatch(lines[2])
        and number_re.fullmatch(lines[3])
    ):
        return
    if (
        len(lines) == 10
        and lines[0] == "forkmesh-https-health-repository-v1"
        and lines[1] == node_name
        and NONCE_RE.fullmatch(lines[2])
        and number_re.fullmatch(lines[3])
        and NODE_RE.fullmatch(lines[4])
        and REPOSITORY_RE.fullmatch(lines[5])
        and lines[6] in {"0", "1"}
        and lines[7] in {"ok", "unavailable"}
        and (lines[6] == "1") == (lines[7] == "ok")
        and SHA256_RE.fullmatch(lines[8])
        and SHA256_RE.fullmatch(lines[9])
    ):
        return
    raise HelperError("health challenge is invalid")


def sign_health_challenge(
    state_dir: Path, request: Mapping[str, Any]
) -> dict[str, Any]:
    state, key = _load_signing_identity(state_dir)
    public_key, payload = _message_request(
        request,
        type_name="forkmesh.health-challenge-signing",
        include_signature=False,
    )
    if public_key != state.node_public_key:
        raise HelperError("health request targets another identity")
    _validate_health_message(payload, state.node_name)
    return {
        "publicKey": state.node_public_key,
        "signature": _b64url_encode(key.sign(payload)),
    }


def _validate_capability_message(payload: bytes, node_name: str) -> None:
    try:
        lines = payload.decode("utf-8").split("\n")
    except UnicodeDecodeError as exc:
        raise HelperError("routing capability is invalid") from exc
    if (
        len(lines) != 7
        or lines[0] != "forkmesh-masked-proxy-v1"
        or lines[1] != node_name
        or lines[2] not in {"GET", "HEAD", "POST"}
        or not lines[3].startswith("/")
        or "\r" in lines[3]
        or not SHA256_RE.fullmatch(lines[4])
        or not REQUEST_ID_RE.fullmatch(lines[5])
        or not re.fullmatch(r"[1-9][0-9]{0,18}", lines[6])
    ):
        raise HelperError("routing capability is invalid")


def verify_router_capability(
    state_dir: Path, request: Mapping[str, Any]
) -> dict[str, Any]:
    state_dir = _require_owner_directory(state_dir)
    state = _load_public_state(state_dir)
    public_key, payload = _message_request(
        request,
        type_name="forkmesh.request-capability-verification",
        include_signature=True,
    )
    if public_key != state.router_public_key:
        raise HelperError("routing capability uses an unpinned key")
    _validate_capability_message(payload, state.node_name)
    signature = _b64url_decode(request.get("signature"), 64)
    try:
        Ed25519PublicKey.from_public_bytes(
            _b64url_decode(state.router_public_key, 32)
        ).verify(signature, payload)
        valid = True
    except ValueError:
        valid = False
    except InvalidSignature:
        valid = False
    return {"valid": valid, "publicKey": state.router_public_key}


def _valid_generated_at(value: Any) -> bool:
    if not isinstance(value, str) or len(value) > 40:
        return False
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return "T" in value


def _validate_manifest_payload(manifest: Mapping[str, Any], state: PublicState) -> None:
    common = {
        "schemaVersion",
        "type",
        "generatedAt",
        "node",
        "endpoint",
        "dns",
        "storage",
    }
    if set(manifest) not in {frozenset(common), frozenset(common | {"edge"})}:
        raise HelperError("mirror manifest has an unsupported shape")
    if (
        manifest.get("schemaVersion") != SCHEMA_VERSION
        or manifest.get("type") != "forkmesh.mirror-endpoint"
        or not _valid_generated_at(manifest.get("generatedAt"))
        or manifest.get("node")
        != {
            "name": state.node_name,
            "publicKey": state.node_public_key,
        }
    ):
        raise HelperError("mirror manifest identity is invalid")
    endpoint = manifest.get("endpoint")
    dns = manifest.get("dns")
    storage = manifest.get("storage")
    if (
        not isinstance(endpoint, dict)
        or not isinstance(dns, dict)
        or not isinstance(storage, dict)
    ):
        raise HelperError("mirror manifest sections are invalid")
    _expect_fields(
        endpoint,
        {
            "origin",
            "healthUrl",
            "manifestUrl",
            "repositoryUrlTemplate",
            "transport",
            "mainProxyMode",
        },
        context="mirror endpoint",
    )
    origin = _normalize_origin(endpoint.get("origin"))
    if not state.allowed_origins or origin not in state.allowed_origins:
        raise HelperError("mirror manifest origin is not configured")
    if (
        endpoint.get("healthUrl") != origin + "/health"
        or endpoint.get("manifestUrl") != origin + "/forkmesh-mirror.json"
        or endpoint.get("transport") != "direct-https"
        or endpoint.get("mainProxyMode") not in {"masked", "direct"}
    ):
        raise HelperError("mirror manifest endpoint is invalid")
    _expect_fields(
        storage,
        {
            "repositoryBytesInD1",
            "repositoryByteOwner",
            "d1Purpose",
        },
        context="mirror storage",
    )
    purposes = storage.get("d1Purpose")
    if (
        storage.get("repositoryBytesInD1") is not False
        or storage.get("repositoryByteOwner") != "independent-mirror-host"
        or not isinstance(purposes, list)
        or not purposes
        or len(purposes) != len(set(purposes))
        or any(item not in STORAGE_PURPOSES for item in purposes)
    ):
        raise HelperError("mirror manifest storage claims are invalid")
    hostname = urlsplit(origin).hostname
    assert hostname is not None
    edge = manifest.get("edge")
    if edge is None:
        # Legacy Worker-route deployment manifest.
        _expect_fields(
            dns,
            {"recordName", "proxied", "workerRoute"},
            context="mirror DNS",
        )
        if (
            dns.get("recordName") != hostname
            or dns.get("proxied") is not True
            or dns.get("workerRoute") != hostname + "/*"
            or endpoint.get("mainProxyMode") != "masked"
            or endpoint.get("repositoryUrlTemplate") != origin + "/{owner}/{repository}"
        ):
            raise HelperError("mirror Worker-route manifest is invalid")
        return
    if not isinstance(edge, dict):
        raise HelperError("mirror edge manifest is invalid")
    if edge.get("kind") == "cloudflare-tunnel":
        _expect_fields(
            edge,
            {"provider", "kind", "tunnelId", "originExposure"},
            context="mirror edge",
        )
        _expect_fields(
            dns,
            {"recordName", "recordType", "proxied", "target"},
            context="mirror DNS",
        )
        tunnel_id = str(edge.get("tunnelId") or "")
        if (
            edge.get("provider") != "cloudflare"
            or edge.get("originExposure") != "loopback-only"
            or not re.fullmatch(r"[A-Za-z0-9-]+", tunnel_id)
            or dns
            != {
                "recordName": hostname,
                "recordType": "CNAME",
                "proxied": True,
                "target": tunnel_id + ".cfargotunnel.com",
            }
        ):
            raise HelperError("Cloudflare Tunnel manifest is invalid")
    elif edge.get("kind") == "cloudflare-proxied-origin":
        # No raw origin address is signed or returned.  The operator must
        # separately enforce a Cloudflare-only origin firewall.
        _expect_fields(
            edge,
            {"provider", "kind", "originExposure"},
            context="mirror edge",
        )
        _expect_fields(
            dns,
            {"recordName", "recordType", "proxied"},
            context="mirror DNS",
        )
        if (
            edge
            != {
                "provider": "cloudflare",
                "kind": "cloudflare-proxied-origin",
                "originExposure": "cloudflare-only-firewall",
            }
            or dns.get("recordName") != hostname
            or dns.get("recordType") not in {"A", "AAAA"}
            or dns.get("proxied") is not True
        ):
            raise HelperError("Cloudflare proxied-origin manifest is invalid")
    elif edge.get("kind") == "public-https-origin":
        _expect_fields(
            edge,
            {"provider", "kind", "originExposure"},
            context="mirror edge",
        )
        _expect_fields(
            dns,
            {"recordName", "recordType", "proxied"},
            context="mirror DNS",
        )
        if (
            edge
            != {
                "provider": "independent",
                "kind": "public-https-origin",
                "originExposure": "public-https",
            }
            or dns.get("recordName") != hostname
            or dns.get("recordType") not in {"A", "AAAA", "A/AAAA"}
            or dns.get("proxied") is not False
            or endpoint.get("mainProxyMode") != "direct"
            or endpoint.get("repositoryUrlTemplate")
            != origin + "/{owner}/{repository}.git"
        ):
            raise HelperError("public HTTPS origin manifest is invalid")
        return
    else:
        raise HelperError("mirror edge kind is unsupported")
    if (
        endpoint.get("mainProxyMode") != "masked"
        or
        endpoint.get("repositoryUrlTemplate")
        != origin + "/v1/repositories/{owner}/{repository}/{operation}"
    ):
        raise HelperError("direct mirror URL template is invalid")


def sign_mirror_manifest(state_dir: Path, request: Mapping[str, Any]) -> dict[str, Any]:
    state, key = _load_signing_identity(state_dir)
    _require_protocol(
        request,
        "forkmesh.mirror-endpoint-signing-request",
        {
            "algorithm",
            "encoding",
            "canonicalization",
            "publicKey",
            "payloadBase64",
            "payloadSha256",
        },
    )
    if (
        request.get("algorithm") != "Ed25519"
        or request.get("encoding") != "base64url-no-padding"
        or request.get("canonicalization") != "forkmesh-json-sort-v1"
        or request.get("publicKey") != state.node_public_key
    ):
        raise HelperError("manifest signing request is unsupported")
    payload = _b64url_decode(request.get("payloadBase64"))
    if not payload or len(payload) > MAX_MANIFEST_BYTES:
        raise HelperError("manifest signing payload size is invalid")
    if request.get("payloadSha256") != hashlib.sha256(payload).hexdigest():
        raise HelperError("manifest signing payload digest is invalid")
    manifest = _parse_json_bytes(payload, maximum=MAX_MANIFEST_BYTES)
    if _canonical_json(manifest) != payload:
        raise HelperError("manifest signing payload is not canonical")
    _validate_manifest_payload(manifest, state)
    return {
        "publicKey": state.node_public_key,
        "signature": _b64url_encode(key.sign(payload)),
    }


def _absolute_path(value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise HelperError(f"{label} path is invalid")
    path = Path(value)
    if not path.is_absolute():
        raise HelperError(f"{label} path must be absolute")
    return path


def _check_ciphertext_file(path: Path, digest: str) -> os.stat_result:
    try:
        info = path.lstat()
    except FileNotFoundError as exc:
        raise HelperError("encrypted repository archive is unavailable") from exc
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_size <= len(AGE_NATIVE_HEADER)
        or info.st_size > MAX_ARCHIVE_BYTES
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise HelperError("encrypted repository archive is unsafe")
    actual = hashlib.sha256()
    with path.open("rb") as stream:
        header = stream.read(max(len(AGE_NATIVE_HEADER), len(AGE_ARMORED_HEADER)))
        actual.update(header)
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            actual.update(chunk)
    if not (
        header.startswith(AGE_NATIVE_HEADER) or header.startswith(AGE_ARMORED_HEADER)
    ):
        raise HelperError("encrypted repository archive is not an age file")
    if not secrets.compare_digest(actual.hexdigest(), digest):
        raise HelperError("encrypted repository archive digest does not match")
    return info


def _archive_member_path(member: tarfile.TarInfo) -> tuple[str, ...]:
    name = member.name.rstrip("/")
    if (
        not name
        or "\x00" in name
        or "\\" in name
        or name.startswith("/")
        or "//" in name
    ):
        raise HelperError("repository archive contains an unsafe path")
    pure = PurePosixPath(name)
    parts = pure.parts
    if (
        not parts
        or any(part in {"", ".", ".."} for part in parts)
        or parts[0] != "repository.git"
        or len(name.encode("utf-8")) > 4096
    ):
        raise HelperError("repository archive contains an unsafe path")
    if member.isdir() or member.isreg():
        return parts
    # This rejects symlinks, hard links, devices, FIFOs, sockets, and unknown
    # extension entries before anything is created on disk.
    raise HelperError("repository archive contains a prohibited entry type")


def _open_directory_at(root_fd: int, parts: Iterable[str]) -> int:
    current = os.dup(root_fd)
    try:
        for part in parts:
            try:
                os.mkdir(part, mode=0o700, dir_fd=current)
            except FileExistsError:
                pass
            flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
            flags |= getattr(os, "O_NOFOLLOW", 0)
            child = os.open(part, flags, dir_fd=current)
            os.close(current)
            current = child
        return current
    except Exception:
        os.close(current)
        raise


def _extract_validated_tar(stream: BinaryIO, destination: Path) -> None:
    try:
        archive = tarfile.open(fileobj=stream, mode="r:")
    except (tarfile.TarError, OSError) as exc:
        raise HelperError("decrypted repository archive is not a tar file") from exc
    with archive:
        members = archive.getmembers()
        if not members or len(members) > MAX_ARCHIVE_ENTRIES:
            raise HelperError("repository archive entry count is invalid")
        validated: list[tuple[tarfile.TarInfo, tuple[str, ...]]] = []
        seen: set[tuple[str, ...]] = set()
        total = 0
        for member in members:
            parts = _archive_member_path(member)
            if parts in seen:
                raise HelperError("repository archive contains duplicate paths")
            seen.add(parts)
            if member.isreg():
                if member.size < 0:
                    raise HelperError("repository archive file size is invalid")
                total += member.size
                if total > MAX_ARCHIVE_BYTES:
                    raise HelperError("repository archive expands beyond its limit")
            validated.append((member, parts))

        staging = destination / (".forkmesh-materialize-" + secrets.token_hex(12))
        staging.mkdir(mode=0o700)
        root_fd = os.open(
            staging,
            os.O_RDONLY
            | os.O_DIRECTORY
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0),
        )
        try:
            for member, parts in validated:
                if member.isdir():
                    directory_fd = _open_directory_at(root_fd, parts)
                    os.close(directory_fd)
                    continue
                parent_fd = _open_directory_at(root_fd, parts[:-1])
                flags = (
                    os.O_WRONLY
                    | os.O_CREAT
                    | os.O_EXCL
                    | getattr(os, "O_CLOEXEC", 0)
                    | getattr(os, "O_NOFOLLOW", 0)
                )
                descriptor = os.open(parts[-1], flags, 0o600, dir_fd=parent_fd)
                try:
                    source = archive.extractfile(member)
                    if source is None:
                        raise HelperError("repository archive file could not be read")
                    remaining = member.size
                    with source:
                        while remaining:
                            chunk = source.read(min(1024 * 1024, remaining))
                            if not chunk:
                                raise HelperError(
                                    "repository archive file is truncated"
                                )
                            _write_all(descriptor, chunk)
                            remaining -= len(chunk)
                        if source.read(1):
                            raise HelperError(
                                "repository archive file size is inconsistent"
                            )
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
                    os.close(parent_fd)
            repository = staging / "repository.git"
            repository_info = repository.lstat()
            if (
                not stat.S_ISDIR(repository_info.st_mode)
                or stat.S_ISLNK(repository_info.st_mode)
                or not (repository / "HEAD").is_file()
                or not (repository / "objects").is_dir()
                or not (repository / "refs").is_dir()
            ):
                raise HelperError("repository archive layout is invalid")
            os.replace(repository, destination / "repository.git")
            staging.rmdir()
        except Exception:
            shutil.rmtree(staging, ignore_errors=True)
            raise
        finally:
            os.close(root_fd)


def materialize_repository(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    age_program: str = "age",
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.repository-archive-materialize",
        {
            "scheme",
            "ciphertextPath",
            "ciphertextSha256",
            "keyReference",
            "destination",
        },
    )
    if request.get("scheme") != "age-encrypted-tar-v1":
        raise HelperError("repository archive scheme is unsupported")
    digest = str(request.get("ciphertextSha256") or "").lower()
    if not SHA256_RE.fullmatch(digest):
        raise HelperError("repository archive digest is invalid")
    state_dir = _require_owner_directory(state_dir)
    state = _load_public_state(state_dir)
    if request.get("keyReference") != state.age_key_reference:
        raise HelperError("repository archive key reference is not configured")
    ciphertext = _absolute_path(request.get("ciphertextPath"), "ciphertext")
    _check_ciphertext_file(ciphertext, digest)
    destination = _require_owner_directory(
        _absolute_path(request.get("destination"), "destination"),
        empty=True,
    )
    identity_path = _age_key_path(state_dir)
    _require_private_file(identity_path, maximum=64 * 1024)
    executable = _resolve_program(age_program)
    try:
        with tempfile.TemporaryFile(
            mode="w+b", prefix=".forkmesh-decrypted-", dir=destination
        ) as plaintext:
            completed = runner(
                [
                    executable,
                    "--decrypt",
                    "--identity",
                    str(identity_path),
                    str(ciphertext),
                ],
                stdin=subprocess.DEVNULL,
                stdout=plaintext,
                stderr=subprocess.DEVNULL,
                env=_safe_environment(),
                timeout=PROCESS_TIMEOUT_SECONDS,
                check=False,
            )
            if completed.returncode != 0:
                raise HelperError("repository archive authentication failed")
            size = plaintext.tell()
            if size <= 0 or size > MAX_ARCHIVE_BYTES:
                raise HelperError("decrypted repository archive size is invalid")
            plaintext.seek(0)
            _extract_validated_tar(plaintext, destination)
    except (OSError, subprocess.SubprocessError) as exc:
        raise HelperError("repository archive materialization failed") from exc
    return {"ok": True, "repositoryPath": "repository.git"}


def _git_environment() -> dict[str, str]:
    environment = _safe_environment()
    environment.update(
        {
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_PROTOCOL_FROM_USER": "0",
            "GIT_OPTIONAL_LOCKS": "0",
        }
    )
    return environment


def _git_prefix(git_program: str, git_dir: Path | None = None) -> list[str]:
    command = [
        git_program,
        "-c",
        "core.alternateRefsCommand=/usr/bin/true",
        "-c",
        "core.hooksPath=" + os.devnull,
        "-c",
        "core.fsmonitor=",
        "-c",
        "credential.helper=",
        "-c",
        "protocol.ext.allow=never",
    ]
    if git_dir is not None:
        command.append(f"--git-dir={git_dir}")
    return command


def _run_git(
    git_program: str,
    arguments: Iterable[str],
    *,
    git_dir: Path | None = None,
    maximum_output: int = 16 * 1024 * 1024,
    timeout: int = PROCESS_TIMEOUT_SECONDS,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> bytes:
    try:
        completed = runner(
            _git_prefix(git_program, git_dir) + list(arguments),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            env=_git_environment(),
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise HelperError("local Git operation failed") from exc
    if completed.returncode != 0 or len(completed.stdout) > maximum_output:
        raise HelperError("local Git operation failed")
    return bytes(completed.stdout)


def _ensure_bare_repository(
    git_program: str,
    repository: Path,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    try:
        info = repository.lstat()
    except FileNotFoundError as exc:
        raise HelperError("bare repository is unavailable") from exc
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        # SSH Git hosts commonly use a repository-sharing group.  The helper
        # snapshots through Git into a fresh owner-only clone before archiving,
        # so group write on the source is an intentional repository permission;
        # world-writable repository storage remains prohibited.
        or stat.S_IMODE(info.st_mode) & stat.S_IWOTH
    ):
        raise HelperError("bare repository storage is unsafe")
    output = _run_git(
        git_program,
        ["rev-parse", "--is-bare-repository"],
        git_dir=repository,
        maximum_output=128,
        runner=runner,
    )
    if output.strip() != b"true":
        raise HelperError("repository is not a bare Git repository")


def _refs_sha256(
    git_program: str,
    repository: Path,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> str:
    output = _run_git(
        git_program,
        [
            "for-each-ref",
            "--sort=refname",
            "--format=%(objectname) %(refname)",
            "refs/heads/",
            "refs/tags/",
        ],
        git_dir=repository,
        runner=runner,
    )
    canonical = "\n".join(
        line for line in output.decode("utf-8", "replace").splitlines() if line
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _validate_snapshot_tree(repository: Path) -> None:
    count = 0
    total = 0
    for root, directories, files in os.walk(repository, followlinks=False):
        root_path = Path(root)
        for name in [*directories, *files]:
            path = root_path / name
            info = path.lstat()
            count += 1
            if count > MAX_ARCHIVE_ENTRIES:
                raise HelperError("bare repository contains too many entries")
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
                if total > MAX_ARCHIVE_BYTES:
                    raise HelperError("bare repository exceeds the archive limit")
            elif not stat.S_ISDIR(info.st_mode):
                raise HelperError("bare repository contains an unsafe entry")


def _sanitized_tar_info(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    info.mode = 0o700 if info.isdir() else 0o600
    info.pax_headers = {}
    return info


def _ciphertext_destination(path: Path) -> tuple[Path, os.stat_result | None]:
    if not path.is_absolute() or path.name in {"", ".", ".."}:
        raise HelperError("ciphertext destination path is invalid")
    parent = _require_owner_directory(path.parent)
    resolved = parent / path.name
    existing: os.stat_result | None = None
    try:
        existing = resolved.lstat()
    except FileNotFoundError:
        pass
    if existing is not None and (
        not stat.S_ISREG(existing.st_mode)
        or stat.S_ISLNK(existing.st_mode)
        or existing.st_uid != os.geteuid()
        or stat.S_IMODE(existing.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise HelperError("existing ciphertext destination is unsafe")
    return resolved, existing


def seal_repository(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    age_program: str = "age",
    git_program: str = "git",
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.repository-archive-seal",
        {"sourceRepository", "ciphertextPath"},
    )
    state_dir = _require_owner_directory(state_dir)
    state = _load_public_state(state_dir)
    source = _absolute_path(request.get("sourceRepository"), "source repository")
    ciphertext, _existing = _ciphertext_destination(
        _absolute_path(request.get("ciphertextPath"), "ciphertext destination")
    )
    git = _resolve_program(git_program)
    age = _resolve_program(age_program)
    _ensure_bare_repository(git, source, runner=runner)
    lock_name = (
        ".seal-"
        + hashlib.sha256(str(ciphertext).encode("utf-8")).hexdigest()[:24]
        + ".lock"
    )
    with _state_lock(state_dir, lock_name):
        with tempfile.TemporaryDirectory(
            prefix=".forkmesh-seal-", dir=state_dir
        ) as temporary_name:
            temporary = Path(temporary_name)
            os.chmod(temporary, 0o700)
            snapshot = temporary / "repository.git"
            _run_git(
                git,
                [
                    "-c",
                    "protocol.allow=never",
                    "-c",
                    "protocol.file.allow=always",
                    "clone",
                    "--mirror",
                    "--no-local",
                    "--",
                    str(source),
                    str(snapshot),
                ],
                runner=runner,
                timeout=PROCESS_TIMEOUT_SECONDS,
            )
            _ensure_bare_repository(git, snapshot, runner=runner)
            _validate_snapshot_tree(snapshot)
            refs_digest = _refs_sha256(git, snapshot, runner=runner)
            with tempfile.TemporaryFile(
                mode="w+b", prefix=".forkmesh-archive-", dir=temporary
            ) as archive_stream:
                with tarfile.open(
                    fileobj=archive_stream,
                    mode="w",
                    format=tarfile.PAX_FORMAT,
                ) as archive:
                    archive.add(
                        snapshot,
                        arcname="repository.git",
                        recursive=True,
                        filter=_sanitized_tar_info,
                    )
                archive_size = archive_stream.tell()
                if archive_size <= 0 or archive_size > MAX_ARCHIVE_BYTES:
                    raise HelperError("repository tar archive size is invalid")
                archive_stream.seek(0)
                staged = ciphertext.parent / (
                    "." + ciphertext.name + "." + secrets.token_hex(12) + ".tmp"
                )
                try:
                    completed = runner(
                        [
                            age,
                            "--encrypt",
                            "--recipient",
                            state.age_recipient,
                            "--output",
                            str(staged),
                            "-",
                        ],
                        stdin=archive_stream,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        env=_safe_environment(),
                        timeout=PROCESS_TIMEOUT_SECONDS,
                        check=False,
                    )
                    if completed.returncode != 0:
                        raise HelperError("repository archive encryption failed")
                    info = staged.lstat()
                    if (
                        not stat.S_ISREG(info.st_mode)
                        or stat.S_ISLNK(info.st_mode)
                        or info.st_size <= len(AGE_NATIVE_HEADER)
                        or info.st_size > MAX_ARCHIVE_BYTES
                    ):
                        raise HelperError("encrypted repository archive is invalid")
                    os.chmod(staged, 0o600, follow_symlinks=False)
                    digest = hashlib.sha256()
                    with staged.open("rb") as encrypted:
                        header = encrypted.read(
                            max(
                                len(AGE_NATIVE_HEADER),
                                len(AGE_ARMORED_HEADER),
                            )
                        )
                        digest.update(header)
                        for chunk in iter(lambda: encrypted.read(1024 * 1024), b""):
                            digest.update(chunk)
                    if not (
                        header.startswith(AGE_NATIVE_HEADER)
                        or header.startswith(AGE_ARMORED_HEADER)
                    ):
                        raise HelperError(
                            "encrypted repository archive is not an age file"
                        )
                    with staged.open("rb") as encrypted:
                        os.fsync(encrypted.fileno())
                    os.replace(staged, ciphertext)
                    os.chmod(ciphertext, 0o600, follow_symlinks=False)
                    directory_fd = os.open(
                        ciphertext.parent, os.O_RDONLY | os.O_DIRECTORY
                    )
                    try:
                        os.fsync(directory_fd)
                    finally:
                        os.close(directory_fd)
                except (OSError, subprocess.SubprocessError) as exc:
                    raise HelperError("repository archive sealing failed") from exc
                finally:
                    try:
                        staged.unlink()
                    except FileNotFoundError:
                        pass
    final_info = ciphertext.stat()
    return {
        "ok": True,
        "scheme": "age-encrypted-tar-v1",
        "ciphertextSha256": digest.hexdigest(),
        "ciphertextBytes": final_info.st_size,
        "keyReference": state.age_key_reference,
        "expectedRefsSha256": refs_digest,
    }


def sign_account_reclaim(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.account-reclaim-signing",
        {"nodeName", "linkCode"},
    )
    state, key = _load_signing_identity(state_dir)
    node = _validate_node(request.get("nodeName"))
    code = str(request.get("linkCode") or "")
    if node != state.node_name or not LINK_CODE_RE.fullmatch(code):
        raise HelperError("account reclaim request is invalid")
    issued_at = str(clock_ms())
    canonical = (
        "forkmesh-reclaim-node-v1\n"
        + node
        + "\n"
        + state.node_public_key
        + "\n"
        + code
        + "\n"
        + issued_at
    ).encode("utf-8")
    return {
        "nodeName": node,
        "pubkey": state.node_public_key,
        "linkCode": code,
        "ts": issued_at,
        "sig": _b64url_encode(key.sign(canonical)),
    }


def sign_endpoint_registration(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> dict[str, Any]:
    fields = set(request)
    expected = {"schemaVersion", "type", "node", "baseUrl"}
    if fields not in {frozenset(expected), frozenset(expected | {"mirrorUrl"})}:
        raise HelperError("request contains an unknown or missing field")
    if (
        request.get("schemaVersion") != SCHEMA_VERSION
        or request.get("type")
        != "forkmesh.https-endpoint-registration-signing"
    ):
        raise HelperError("request protocol is unsupported")
    state, key = _load_signing_identity(state_dir)
    node = _validate_node(request.get("node"))
    origin = _normalize_origin(request.get("baseUrl"))
    mirror_url = (
        _normalize_origin(request.get("mirrorUrl"))
        if "mirrorUrl" in request else ""
    )
    if (
        node != state.node_name
        or not state.allowed_origins
        or origin not in state.allowed_origins
        or (mirror_url and mirror_url not in state.allowed_origins)
    ):
        raise HelperError("endpoint registration is not configured")
    issued_at = clock_ms()
    canonical_fields = [
        "forkmesh-https-endpoint-v1", node, origin,
        state.node_public_key, str(issued_at),
    ]
    if mirror_url:
        canonical_fields = [
            "forkmesh-https-endpoint-v2", node, origin, mirror_url,
            state.node_public_key, str(issued_at),
        ]
    canonical = "\n".join(canonical_fields).encode("utf-8")
    response = {
        "node": node,
        "baseUrl": origin,
        "publicKey": state.node_public_key,
        "issuedAt": issued_at,
        "signature": _b64url_encode(key.sign(canonical)),
    }
    if mirror_url:
        response["mirrorUrl"] = mirror_url
    return response


def _clean_string(value: Any, maximum: int = 240) -> str:
    return value.strip()[:maximum] if isinstance(value, str) else ""


def _clean_integer(value: Any, maximum: int) -> int:
    try:
        number = int(value or 0)
    except (TypeError, ValueError):
        number = 0
    return max(0, min(number, maximum))


def _clean_logo_metadata(value: Any) -> dict[str, Any]:
    value = value if isinstance(value, dict) else {}

    def labels(name: str, limit: int, maximum: int) -> list[str]:
        raw = value.get(name)
        if not isinstance(raw, list):
            return []
        output: list[str] = []
        for item in raw:
            label = _clean_string(item, maximum)
            if label:
                output.append(label)
            if len(output) >= limit:
                break
        return output

    languages: dict[str, int] = {}
    raw_languages = value.get("languages")
    if isinstance(raw_languages, dict):
        for raw_name, raw_bytes in raw_languages.items():
            name = _clean_string(raw_name, 80)
            if not name:
                continue
            languages[name] = _clean_integer(raw_bytes, 1 << 50)
            if len(languages) >= 12:
                break
    return {
        "description": _clean_string(value.get("description", ""), 500),
        "languages": languages,
        "topics": labels("topics", 12, 80),
        "fileStructure": labels("fileStructure", 24, 120),
        "frameworks": labels("frameworks", 12, 80),
        "projectCategory": _clean_string(value.get("projectCategory", ""), 80),
    }


def _clean_activity(value: Any) -> list[int]:
    raw = value if isinstance(value, list) else []
    result = [_clean_integer(item, 1_000_000) for item in raw[-52:]]
    return [0] * (52 - len(result)) + result


def _clean_optional_integer(value: Any, maximum: int) -> int | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and (
        not math.isfinite(value) or not value.is_integer()
    ):
        return None
    if value < 0:
        return None
    return min(int(value), maximum)


def _clean_optional_usage(
    used_value: Any, total_value: Any
) -> tuple[int | None, int | None]:
    maximum = 1 << 50
    used = _clean_optional_integer(used_value, maximum)
    total = _clean_optional_integer(total_value, maximum)
    if used is None or total is None or total <= 0:
        return None, None
    return min(used, total), total


def _normalized_public_catalog(
    source: Mapping[str, Any],
    node_public_key: str,
    *,
    now_ms: int,
) -> dict[str, Any]:
    if set(source) - PUBLIC_CATALOG_INPUT_FIELDS:
        raise HelperError("catalog record contains an unsupported field")
    owner = _clean_string(source.get("owner", ""), 80)
    name = _clean_string(source.get("name", ""), 80)
    segment_re = re.compile(r"^[A-Za-z0-9._:-]+$")
    if (
        not owner
        or not name
        or not segment_re.fullmatch(owner)
        or not segment_re.fullmatch(name)
        or source.get("visibility") != "public"
    ):
        raise HelperError("public catalog identity is invalid")
    supplied_maintainer = _clean_string(source.get("maintainer", ""), 120)
    if supplied_maintainer and supplied_maintainer != node_public_key:
        raise HelperError("catalog record targets another maintainer")
    state_hash = _clean_string(source.get("stateHash", ""), 64).lower()
    if not SHA256_RE.fullmatch(state_hash):
        raise HelperError("catalog state hash is required")
    updated_at = _clean_string(source.get("updatedAt", ""), 32)
    if not re.fullmatch(r"[1-9][0-9]{0,18}", updated_at):
        updated_at = str(now_ms)
    solana = _clean_string(source.get("solana", ""), 64)
    if not re.fullmatch(r"[1-9A-HJ-NP-Za-km-z]{32,44}", solana):
        solana = ""
    mem_used, mem_total = _clean_optional_usage(
        source.get("memUsedBytes"), source.get("memTotalBytes"))
    disk_used, disk_total = _clean_optional_usage(
        source.get("diskUsedBytes"), source.get("diskTotalBytes"))
    actions_fields = {"actionsEnabled", "actionsState"}.intersection(source)
    if actions_fields and actions_fields != {"actionsEnabled", "actionsState"}:
        raise HelperError("catalog Actions capability is incomplete")
    actions_enabled = source.get("actionsEnabled")
    actions_state = source.get("actionsState")
    if actions_fields and (
        not isinstance(actions_enabled, bool)
        or (
            (not actions_enabled and actions_state != "disabled")
            or (actions_enabled and actions_state not in {"enabled", "running"})
        )
    ):
        raise HelperError("catalog Actions capability is invalid")
    record = {
        "owner": owner,
        "name": name,
        "visibility": "public",
        "mirrorEncryption": "",
        "opaqueRepoId": "",
        "keyEpoch": 0,
        "encryptedManifestHash": "",
        "encryptedManifestSig": "",
        "sizeBytes": _clean_integer(source.get("sizeBytes"), 1 << 50),
        "description": _clean_string(source.get("description", ""), 240),
        "logoMetadata": _clean_logo_metadata(source.get("logoMetadata")),
        "cloneUrl": _clean_string(source.get("cloneUrl", ""), 2048),
        "solana": solana,
        "channel": _clean_string(source.get("channel", f"#{owner}-{name}"), 120),
        "hostedSince": _clean_string(source.get("hostedSince", ""), 32),
        "lastSync": _clean_string(source.get("lastSync", ""), 32),
        "updatedAt": updated_at,
        "rootCommit": _clean_string(source.get("rootCommit", ""), 64),
        "source": _clean_string(source.get("source", "local-node"), 40),
        "commit": _clean_string(source.get("commit", ""), 64),
        "branch": _clean_string(source.get("branch", ""), 120),
        "issueCount": _clean_string(source.get("issueCount", ""), 12),
        "issueMaxNumber": _clean_string(source.get("issueMaxNumber", ""), 12),
        "commitCount": _clean_string(source.get("commitCount", ""), 12),
        "branchCount": _clean_string(source.get("branchCount", ""), 12),
        "pullCount": _clean_string(source.get("pullCount", ""), 12),
        "discussionCount": _clean_string(source.get("discussionCount", ""), 12),
        "activityWeeks": _clean_activity(source.get("activityWeeks")),
        "worktreeCount": _clean_string(source.get("worktreeCount", ""), 12),
        "artifactCount": _clean_string(source.get("artifactCount", ""), 12),
        "platform": _clean_string(source.get("platform", ""), 16),
        "version": _clean_string(source.get("version", ""), 32),
        "nodeId": _clean_string(source.get("nodeId", ""), 64),
        "clonesServed": _clean_string(source.get("clonesServed", ""), 12),
        "websiteServed": _clean_string(source.get("websiteServed", ""), 12),
        "maintainer": node_public_key,
        # The legacy field is retained in the Worker-normalized v2 shape but is
        # no longer an independent authorization signature.
        "signature": "",
        "stateHash": state_hash,
        "stateSig": "",
    }
    if "mirrorUrl" in source:
        try:
            parsed_mirror = urlsplit(str(source.get("mirrorUrl") or ""))
            mirror_origin = _normalize_origin(
                parsed_mirror.scheme + "://" + parsed_mirror.netloc)
        except Exception as exc:
            raise HelperError("catalog mirror URL is invalid") from exc
        expected_mirror_url = mirror_origin + "/" + owner + "/" + name + ".git"
        if (
            parsed_mirror.query
            or parsed_mirror.fragment
            or parsed_mirror.path != "/" + owner + "/" + name + ".git"
            or str(source.get("mirrorUrl")) != expected_mirror_url
        ):
            raise HelperError("catalog mirror URL is invalid")
        record["mirrorUrl"] = expected_mirror_url
    # Preserve absence for older catalog-v2 publishers. New headless mirrors
    # send both fields, and both are covered by the catalog signature.
    if actions_fields:
        record["actionsEnabled"] = actions_enabled
        record["actionsState"] = actions_state
    merge_fields = {"mergeCount", "mergeActivityWeeks"}.intersection(source)
    if merge_fields and merge_fields != {"mergeCount", "mergeActivityWeeks"}:
        raise HelperError("catalog merge activity is incomplete")
    if merge_fields:
        record["mergeCount"] = _clean_string(source.get("mergeCount", ""), 12)
        record["mergeActivityWeeks"] = _clean_activity(
            source.get("mergeActivityWeeks"))
    cpu_percent = _clean_optional_integer(source.get("cpuPercent"), 100)
    if cpu_percent is not None:
        record["cpuPercent"] = cpu_percent
    if mem_total is not None:
        record["memUsedBytes"] = mem_used
        record["memTotalBytes"] = mem_total
    if disk_total is not None:
        record["diskUsedBytes"] = disk_used
        record["diskTotalBytes"] = disk_total
    # Subject / author / date of the published head commit. Optional extension
    # fields, kept absent (never empty) exactly as the Worker normalizes them,
    # so an older publisher's catalog-v2 signature still verifies.
    commit_subject = _clean_string(source.get("commitSubject", ""), 120)
    if commit_subject:
        record["commitSubject"] = commit_subject
    commit_author_name = _clean_string(source.get("commitAuthorName", ""), 64)
    if commit_author_name:
        record["commitAuthorName"] = commit_author_name
    commit_at = _clean_string(source.get("commitAt", ""), 16)
    if commit_at:
        record["commitAt"] = commit_at
    # When this node last served a clone / a website read, and the class of
    # client it served. Same optional-extension rule: absent when the gateway
    # never recorded one, so older publishers' signatures still verify.
    for field in ("cloneServedAt", "websiteServedAt"):
        served_at = _clean_string(source.get(field, ""), 16)
        if served_at.isdigit() and int(served_at) > 0:
            record[field] = served_at
    for field in ("cloneServedAgent", "websiteServedAgent"):
        agent = _clean_string(source.get(field, ""), 16)
        if agent in SERVE_AGENT_CLASSES:
            record[field] = agent
    return record


def sign_catalog_v2(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.catalog-v2-publication-signing",
        {"record"},
    )
    if not isinstance(request.get("record"), dict):
        raise HelperError("catalog record must be an object")
    state, key = _load_signing_identity(state_dir)
    record = _normalized_public_catalog(
        request["record"], state.node_public_key, now_ms=clock_ms()
    )
    state_payload = (
        "forkmesh-repostate-v1\n"
        + record["owner"]
        + "\n"
        + record["name"]
        + "\n"
        + record["stateHash"]
        + "\n"
        + record["updatedAt"]
    ).encode("utf-8")
    record["stateSig"] = _b64url_encode(key.sign(state_payload))
    # Match the Worker catalog-v2 verifier exactly. ``signature`` is the
    # retained legacy-v1 field and is not part of the v2 signed record.
    signed_record = dict(record)
    signed_record.pop("signature", None)
    record_hash = hashlib.sha256(
        _canonical_json(signed_record, ensure_ascii=True)
    ).hexdigest()
    catalog_payload = ("forkmesh-catalog-v2\n" + record_hash).encode("ascii")
    record["catalogSigVersion"] = 2
    record["catalogSig"] = _b64url_encode(key.sign(catalog_payload))
    return record


def sign_repository_delete(
    state_dir: Path,
    request: Mapping[str, Any],
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> dict[str, Any]:
    _require_protocol(
        request,
        "forkmesh.repository-delete-signing",
        {"owner", "name", "timestamp"},
    )
    owner = _clean_string(request.get("owner", ""), 80).lower()
    name = _clean_string(request.get("name", ""), 100)
    timestamp = request.get("timestamp")
    if isinstance(timestamp, bool):
        raise HelperError("repository deletion request is invalid")
    try:
        timestamp_ms = int(timestamp)
    except (TypeError, ValueError, OverflowError) as exc:
        raise HelperError("repository deletion request is invalid") from exc
    segment_re = re.compile(r"^[A-Za-z0-9._:-]+$")
    if (
        not owner
        or not name
        or not segment_re.fullmatch(owner)
        or not segment_re.fullmatch(name)
        or abs(clock_ms() - timestamp_ms) > 5 * 60 * 1000
    ):
        raise HelperError("repository deletion request is invalid")
    state, key = _load_signing_identity(state_dir)
    canonical = (
        "forkmesh-catalog-delete-v1\n"
        + owner
        + "\n"
        + name
        + "\n"
        + str(timestamp_ms)
    ).encode("utf-8")
    return {
        "ok": True,
        "owner": owner,
        "name": name,
        "timestamp": timestamp_ms,
        "maintainer": state.node_public_key,
        "signature": _b64url_encode(key.sign(canonical)),
    }


def _assert_public_response(value: Any) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            if (
                key not in ALLOWED_PUBLIC_KEY_FIELDS
                and FORBIDDEN_RESPONSE_FIELD_RE.search(str(key))
            ):
                raise HelperError("helper attempted to emit a prohibited field")
            _assert_public_response(item)
    elif isinstance(value, list):
        for item in value:
            _assert_public_response(item)


def _write_response(
    value: Mapping[str, Any], stream: BinaryIO = sys.stdout.buffer
) -> None:
    _assert_public_response(value)
    encoded = _canonical_json(value) + b"\n"
    if len(encoded) > MAX_HELPER_OUTPUT_BYTES:
        raise HelperError("helper response is too large")
    stream.write(encoded)
    stream.flush()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Owner-local identity helper for a headless ForkMesh mirror"
    )
    parser.add_argument(
        "--state-dir",
        required=True,
        type=Path,
        help="absolute owner-only identity directory (mode 0700)",
    )
    parser.add_argument(
        "mode",
        choices=(
            "init",
            "configure",
            "public-info",
            "manifest-sign",
            "health-sign",
            "capability-verify",
            "seal-repository",
            "materialize",
            "sign-reclaim",
            "sign-endpoint-registration",
            "sign-catalog-v2",
            "sign-repository-delete",
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        state_dir = args.state_dir
        if args.mode == "public-info":
            response = _load_public_state(_require_owner_directory(state_dir)).as_json()
        else:
            request = _read_request()
            handlers: dict[str, Callable[[Path, Mapping[str, Any]], dict[str, Any]]] = {
                "init": initialize_identity,
                "configure": configure_public_state,
                "manifest-sign": sign_mirror_manifest,
                "health-sign": sign_health_challenge,
                "capability-verify": verify_router_capability,
                "seal-repository": seal_repository,
                "materialize": materialize_repository,
                "sign-reclaim": sign_account_reclaim,
                "sign-endpoint-registration": sign_endpoint_registration,
                "sign-catalog-v2": sign_catalog_v2,
                "sign-repository-delete": sign_repository_delete,
            }
            response = handlers[args.mode](state_dir, request)
        _write_response(response)
        return 0
    except HelperError as exc:
        # Messages are fixed policy errors and never interpolate request data,
        # paths, subprocess output, or key material.
        print(f"headless mirror identity: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("headless mirror identity: interrupted", file=sys.stderr)
        return 130
    except Exception:
        # Never let an unexpected library or filesystem exception print a
        # traceback containing local paths or process details in service logs.
        print("headless mirror identity: internal operation failed", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
