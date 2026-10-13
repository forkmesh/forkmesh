#!/usr/bin/env python3
"""Fixed-input bridge from an SSH Git hook to mirror refresh publication.

``notify`` is invoked by a root-owned post-receive hook as the dedicated Git
account. It drains but never parses hook stdin and atomically creates one fixed
marker.

``run`` is invoked by a systemd path unit as root. It claims that marker,
temporarily pauses periodic lease renewal, runs the existing refresh tool as
the unprivileged mirror account, restarts only the configured mirror gateway
service, verifies a signed loopback health challenge, and finally renews the
publication as the mirror account before resuming the timer. Pausing the timer
closes the restart race where two valid publishers could alternately reject
one another and make a busy mirror flap.

No ref, repository path, SSH command, environment value, or other user input is
placed in a process argument. Configuration is root-owned and secret-free.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
import fcntl
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import stat
import subprocess
import sys
import time
from typing import Any, BinaryIO, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PublicKey,
    )
except ImportError as exc:  # pragma: no cover - minimal-host failure
    raise SystemExit(
        "ForkMesh SSH refresh: the cryptography package is required"
    ) from exc


SCHEMA_VERSION = 1
MAX_CONFIG_BYTES = 64 * 1024
MAX_HOOK_INPUT_BYTES = 4 * 1024 * 1024
MAX_HEALTH_BYTES = 64 * 1024
REFRESH_TIMEOUT_SECONDS = 30 * 60
REFRESH_MAX_ATTEMPTS = 2
REFRESH_RETRY_DELAY_SECONDS = 2.0
PERSISTENT_RETRY_DELAYS_SECONDS = (60, 5 * 60, 15 * 60, 60 * 60)
MAX_PERSISTENT_ATTEMPTS = 32
SERVICE_TIMEOUT_SECONDS = 2 * 60
DEFAULT_HEALTH_TIMEOUT_SECONDS = 180
HEALTH_REQUEST_TIMEOUT_SECONDS = 3.0
HEALTH_RETRY_SECONDS = 0.5
HEALTH_USER_AGENT = "ForkMesh-ssh-refresh-health/1.0"
FIXED_RUNTIME_TMPDIR = "/var/lib/forkmesh-mirror/runtime-tmp"
RENEW_SERVICE = "forkmesh-mirror-renew.service"
RENEW_TIMER = "forkmesh-mirror-renew.timer"
NAME_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,62}$")
SERVICE_RE = re.compile(r"^[A-Za-z0-9@_.:-]{1,200}\.service$")
PUBLIC_HOST_RE = re.compile(
    r"^(?=.{1,253}$)"
    r"(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)*"
    r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$",
    re.IGNORECASE,
)
PUBLIC_KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIGNATURE_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
NONCE_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
FORBIDDEN_FIELD_RE = re.compile(
    r"(?:private.?key|secret|seed|mnemonic|keypair|password|credential|token)",
    re.IGNORECASE,
)
ALLOWED_PUBLIC_FIELDS = frozenset(
    {
        "publicKey",
        "routerPublicKey",
        "keyReference",
        "ciphertextSha256",
        "expectedRefsSha256",
    }
)


class RefreshBridgeError(RuntimeError):
    """A safe fixed-message failure."""


@dataclass(frozen=True)
class BridgeConfig:
    trigger_path: Path
    notify_user: str
    mirror_user: str
    python_program: Path
    refresh_program: Path
    refresh_config_path: Path
    gateway_config_path: Path
    mirror_service: str
    health_timeout_seconds: int = DEFAULT_HEALTH_TIMEOUT_SECONDS


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RefreshBridgeError("configuration contains a duplicate field")
        result[key] = value
    return result


def _assert_no_secret_fields(value: Any) -> None:
    if isinstance(value, dict):
        for raw_key, item in value.items():
            key = str(raw_key)
            if key not in ALLOWED_PUBLIC_FIELDS and FORBIDDEN_FIELD_RE.search(key):
                raise RefreshBridgeError("configuration contains a prohibited field")
            _assert_no_secret_fields(item)
    elif isinstance(value, list):
        for item in value:
            _assert_no_secret_fields(item)


def _read_root_config(path: Path) -> dict[str, Any]:
    if not path.is_absolute():
        raise RefreshBridgeError("configuration path must be absolute")
    descriptor = -1
    try:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != 0
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) & 0o022
            or info.st_size <= 0
            or info.st_size > MAX_CONFIG_BYTES
        ):
            raise RefreshBridgeError("configuration file is unsafe")
        chunks = bytearray()
        while len(chunks) <= MAX_CONFIG_BYTES:
            chunk = os.read(
                descriptor,
                min(64 * 1024, MAX_CONFIG_BYTES + 1 - len(chunks)),
            )
            if not chunk:
                break
            chunks.extend(chunk)
        if len(chunks) != info.st_size or len(chunks) > MAX_CONFIG_BYTES:
            raise RefreshBridgeError("configuration file changed while reading")
        value = json.loads(
            bytes(chunks).decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except RefreshBridgeError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RefreshBridgeError("configuration file is invalid") from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if not isinstance(value, dict):
        raise RefreshBridgeError("configuration must be an object")
    _assert_no_secret_fields(value)
    return value


def _absolute_path(value: Any, label: str) -> Path:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or "\r" in value
        or "\n" in value
    ):
        raise RefreshBridgeError(f"{label} path is invalid")
    path = Path(value)
    if not path.is_absolute():
        raise RefreshBridgeError(f"{label} path must be absolute")
    return path


def _safe_program(path: Path, *, executable: bool = True) -> Path:
    try:
        info = path.lstat()
    except OSError as exc:
        raise RefreshBridgeError("configured executable is unavailable") from exc
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != 0
        or stat.S_IMODE(info.st_mode) & 0o022
        or (executable and not os.access(path, os.X_OK))
    ):
        raise RefreshBridgeError("configured executable is unsafe")
    return path.resolve(strict=True)


def load_config(path: Path) -> BridgeConfig:
    source = _read_root_config(path)
    expected = {
        "schemaVersion",
        "type",
        "triggerPath",
        "notifyUser",
        "mirrorUser",
        "pythonProgram",
        "refreshProgram",
        "refreshConfigPath",
        "gatewayConfigPath",
        "mirrorService",
        "healthTimeoutSeconds",
    }
    if (
        set(source) != expected
        or source.get("schemaVersion") != SCHEMA_VERSION
        or source.get("type") != "forkmesh.ssh-post-receive-refresh"
    ):
        raise RefreshBridgeError("configuration schema is unsupported")
    notify_user = str(source.get("notifyUser") or "")
    mirror_user = str(source.get("mirrorUser") or "")
    if (
        not NAME_RE.fullmatch(notify_user)
        or not NAME_RE.fullmatch(mirror_user)
        or notify_user == mirror_user
        or notify_user == "root"
        or mirror_user == "root"
    ):
        raise RefreshBridgeError("configured service accounts are invalid")
    try:
        notify_record = pwd.getpwnam(notify_user)
        mirror_record = pwd.getpwnam(mirror_user)
    except KeyError as exc:
        raise RefreshBridgeError("configured service account is unavailable") from exc
    if notify_record.pw_uid == 0 or mirror_record.pw_uid == 0:
        raise RefreshBridgeError("configured service account is invalid")
    service = str(source.get("mirrorService") or "")
    if not SERVICE_RE.fullmatch(service):
        raise RefreshBridgeError("configured mirror service is invalid")
    try:
        health_timeout = int(source.get("healthTimeoutSeconds"))
    except (TypeError, ValueError) as exc:
        raise RefreshBridgeError("health timeout is invalid") from exc
    if not 5 <= health_timeout <= 300:
        raise RefreshBridgeError("health timeout is invalid")
    return BridgeConfig(
        trigger_path=_absolute_path(source.get("triggerPath"), "trigger"),
        notify_user=notify_user,
        mirror_user=mirror_user,
        python_program=_safe_program(
            _absolute_path(source.get("pythonProgram"), "Python")
        ),
        refresh_program=_safe_program(
            _absolute_path(source.get("refreshProgram"), "refresh program"),
            executable=False,
        ),
        refresh_config_path=_absolute_path(
            source.get("refreshConfigPath"), "refresh configuration"
        ),
        gateway_config_path=_absolute_path(
            source.get("gatewayConfigPath"), "gateway configuration"
        ),
        mirror_service=service,
        health_timeout_seconds=health_timeout,
    )


def _safe_environment() -> dict[str, str]:
    return {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C",
        "LC_ALL": "C",
        # This is the packaged, service-owned disk-backed materialization
        # directory. Never forward an ambient or config-derived TMPDIR.
        "TMPDIR": FIXED_RUNTIME_TMPDIR,
    }


def _require_trigger_directory(config: BridgeConfig) -> Path:
    parent = config.trigger_path.parent
    try:
        info = parent.lstat()
        notify = pwd.getpwnam(config.notify_user)
    except (OSError, KeyError) as exc:
        raise RefreshBridgeError("refresh trigger directory is unavailable") from exc
    process_groups = {os.getegid(), *os.getgroups()}
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != 0
        or info.st_gid not in process_groups
        or info.st_gid not in {notify.pw_gid, *os.getgroups()}
        or stat.S_IMODE(info.st_mode) & 0o007
        or not stat.S_IMODE(info.st_mode) & stat.S_IWGRP
        or not stat.S_IMODE(info.st_mode) & stat.S_IXGRP
    ):
        raise RefreshBridgeError("refresh trigger directory is unsafe")
    return parent.resolve(strict=True)


def notify(
    config: BridgeConfig,
    *,
    input_stream: BinaryIO = sys.stdin.buffer,
) -> None:
    try:
        current = pwd.getpwuid(os.geteuid()).pw_name
    except KeyError as exc:
        raise RefreshBridgeError("notification account is invalid") from exc
    if current != config.notify_user:
        raise RefreshBridgeError("notification account is not authorized")
    parent = _require_trigger_directory(config)
    hook_input = input_stream.read(MAX_HOOK_INPUT_BYTES + 1)
    if len(hook_input) > MAX_HOOK_INPUT_BYTES:
        raise RefreshBridgeError("post-receive input is too large")
    # Ref names and object IDs are intentionally discarded without parsing.
    del hook_input

    temporary = parent / (".pending-" + secrets.token_hex(12))
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, 0o600)
    try:
        os.write(descriptor, b"forkmesh-refresh-v1\n")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        os.replace(temporary, config.trigger_path)
        directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _processing_path(config: BridgeConfig) -> Path:
    return config.trigger_path.with_name(".processing")


def _retry_path(config: BridgeConfig) -> Path:
    return config.trigger_path.with_name(".retry")


def _state_path(config: BridgeConfig) -> Path:
    return config.trigger_path.with_name("refresh-state.json")


def _safe_marker(path: Path) -> bool:
    """Return whether *path* is one bounded regular coalescing marker."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_nlink != 1
        or info.st_size <= 0
        or info.st_size > 128
    ):
        raise RefreshBridgeError("refresh trigger is unsafe")
    return True


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    """Durably replace one root-service state file without following links."""
    raw = (
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    parent = path.parent
    temporary = parent / (".state-" + secrets.token_hex(12))
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, 0o600)
    try:
        os.write(descriptor, raw)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        os.replace(temporary, path)
        directory_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _read_state(config: BridgeConfig) -> dict[str, Any]:
    path = _state_path(config)
    try:
        info = path.lstat()
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_nlink != 1
            or info.st_size <= 0
            or info.st_size > 4096
        ):
            raise RefreshBridgeError("refresh state is unsafe")
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RefreshBridgeError("refresh state is invalid") from exc
    if not isinstance(value, dict) or value.get("schemaVersion") != 1:
        raise RefreshBridgeError("refresh state is invalid")
    return value


def _write_state(
    config: BridgeConfig,
    *,
    status: str,
    attempts: int,
    now_ms: int,
    phase: str = "",
    next_retry_at: int = 0,
) -> None:
    if status not in {"published", "retry-pending", "processing"}:
        raise RefreshBridgeError("refresh state is invalid")
    _atomic_json(
        _state_path(config),
        {
            "schemaVersion": 1,
            "status": status,
            "attempts": max(0, min(int(attempts), MAX_PERSISTENT_ATTEMPTS)),
            "phase": phase if phase in {
                "", "refresh", "restart", "health", "register"
            } else "refresh",
            "updatedAt": int(now_ms),
            "nextRetryAt": max(0, int(next_retry_at)),
            # Deliberately generic: command output, repository names, refs,
            # paths, credentials, and exception strings never enter this file.
            "lastError": (
                "publication_phase_failed" if status == "retry-pending" else ""
            ),
        },
    )


def refresh_status(
    config: BridgeConfig,
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> dict[str, Any]:
    """Return a bounded, secret-free local health view for operators."""
    state = _read_state(config)
    pending = _safe_marker(config.trigger_path)
    processing = _safe_marker(_processing_path(config))
    retry = _safe_marker(_retry_path(config))
    status = str(state.get("status") or "idle")
    if processing:
        status = "processing"
    elif pending:
        status = "pending"
    elif retry:
        status = "retry-pending"
    next_retry_at = int(state.get("nextRetryAt") or 0)
    now = int(clock_ms())
    return {
        "ok": status not in {"retry-pending"},
        "status": status,
        "pending": bool(pending),
        "processing": bool(processing),
        "retryPending": bool(retry),
        "attempts": max(
            0, min(int(state.get("attempts") or 0), MAX_PERSISTENT_ATTEMPTS)
        ),
        "phase": str(state.get("phase") or ""),
        "nextRetryAt": next_retry_at,
        "retryDue": bool(retry and (not next_retry_at or next_retry_at <= now)),
        "lastError": str(state.get("lastError") or ""),
    }


def _run_command(
    command: list[str],
    *,
    timeout: float,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    try:
        completed = runner(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=_safe_environment(),
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise RefreshBridgeError("fixed refresh command failed") from exc
    if completed.returncode != 0:
        raise RefreshBridgeError("fixed refresh command failed")


def _run_as_mirror(
    config: BridgeConfig,
    mode: str,
    *,
    timeout: float = REFRESH_TIMEOUT_SECONDS,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    if mode not in {"refresh", "register", "renew"}:
        raise RefreshBridgeError("refresh mode is invalid")
    _run_command(
        [
            "/usr/sbin/runuser",
            "--user",
            config.mirror_user,
            "--",
            str(config.python_program),
            str(config.refresh_program),
            "--config",
            str(config.refresh_config_path),
            mode,
        ],
        timeout=timeout,
        runner=runner,
    )


def _refresh_with_retry(
    config: BridgeConfig,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
    monotonic: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> None:
    """Retry only the idempotent refresh within one original timeout budget."""
    deadline = monotonic() + REFRESH_TIMEOUT_SECONDS
    for attempt in range(REFRESH_MAX_ATTEMPTS):
        remaining = deadline - monotonic()
        if remaining <= 0:
            raise RefreshBridgeError("fixed refresh command failed")
        try:
            _run_as_mirror(
                config,
                "refresh",
                timeout=remaining,
                runner=runner,
            )
            return
        except RefreshBridgeError:
            if attempt + 1 >= REFRESH_MAX_ATTEMPTS:
                raise
            # The fixed pause counts against the existing 30-minute refresh
            # deadline. If it would consume the remaining budget, fail closed
            # instead of launching an effectively unbounded second command.
            if deadline - monotonic() <= REFRESH_RETRY_DELAY_SECONDS:
                raise
            sleeper(REFRESH_RETRY_DELAY_SECONDS)
            if deadline - monotonic() <= 0:
                raise


def _restart_gateway(
    config: BridgeConfig,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    _run_command(
        ["/usr/bin/systemctl", "restart", config.mirror_service],
        timeout=SERVICE_TIMEOUT_SECONDS,
        runner=runner,
    )


def _pause_renewal(
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    """Stop the timer and any queued renewal before changing generations."""
    _run_command(
        ["/usr/bin/systemctl", "stop", RENEW_TIMER, RENEW_SERVICE],
        timeout=SERVICE_TIMEOUT_SECONDS,
        runner=runner,
    )


def _resume_renewal(
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
) -> None:
    """Restore periodic renewal after publication or a failed refresh."""
    _run_command(
        ["/usr/bin/systemctl", "start", RENEW_TIMER],
        timeout=SERVICE_TIMEOUT_SECONDS,
        runner=runner,
    )


def _normalize_public_origin(value: Any) -> str:
    try:
        parsed = urlsplit(str(value or "").strip())
        port = parsed.port
    except ValueError as exc:
        raise RefreshBridgeError("gateway public origin is invalid") from exc
    hostname = str(parsed.hostname or "").lower()
    try:
        ipaddress.ip_address(hostname)
        hostname_is_ip = True
    except ValueError:
        hostname_is_ip = False
    if (
        parsed.scheme != "https"
        or not PUBLIC_HOST_RE.fullmatch(hostname)
        or hostname == "localhost"
        or hostname_is_ip
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in ("", "/")
        or port not in (None, 443)
    ):
        raise RefreshBridgeError("gateway public origin is invalid")
    return "https://" + hostname


def _parse_gateway_identity(path: Path) -> tuple[str, str, str, str]:
    try:
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        try:
            info = os.fstat(descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_nlink != 1
                or stat.S_IMODE(info.st_mode) & 0o077
                or info.st_size <= 0
                or info.st_size > MAX_CONFIG_BYTES
            ):
                raise RefreshBridgeError("gateway configuration is unsafe")
            content = bytearray()
            while len(content) <= MAX_CONFIG_BYTES:
                chunk = os.read(
                    descriptor,
                    min(
                        64 * 1024,
                        MAX_CONFIG_BYTES + 1 - len(content),
                    ),
                )
                if not chunk:
                    break
                content.extend(chunk)
            if len(content) != info.st_size or len(content) > MAX_CONFIG_BYTES:
                raise RefreshBridgeError("gateway configuration changed while reading")
            raw = bytes(content)
        finally:
            os.close(descriptor)
        value = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except RefreshBridgeError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RefreshBridgeError("gateway configuration is invalid") from exc
    node = value.get("node") if isinstance(value, dict) else None
    listen = value.get("listen") if isinstance(value, dict) else None
    public_origin = (
        _normalize_public_origin(value.get("publicOrigin"))
        if isinstance(value, dict)
        else ""
    )
    if (
        not isinstance(node, dict)
        or set(node) != {"name", "publicKey"}
        or not NAME_RE.fullmatch(str(node.get("name") or ""))
        or not PUBLIC_KEY_RE.fullmatch(str(node.get("publicKey") or ""))
        or not isinstance(listen, dict)
        or set(listen) != {"host", "port"}
        or listen.get("host") not in {"127.0.0.1", "::1", "localhost"}
    ):
        raise RefreshBridgeError("gateway health identity is invalid")
    try:
        port = int(listen.get("port"))
    except (TypeError, ValueError) as exc:
        raise RefreshBridgeError("gateway health listener is invalid") from exc
    if not 1 <= port <= 65535:
        raise RefreshBridgeError("gateway health listener is invalid")
    host = "[::1]" if listen["host"] == "::1" else listen["host"]
    return (
        str(node["name"]),
        str(node["publicKey"]),
        f"http://{host}:{port}",
        public_origin,
    )


def _b64url_decode(value: str, size: int) -> bytes:
    if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise RefreshBridgeError("signed gateway health is invalid")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except ValueError as exc:
        raise RefreshBridgeError("signed gateway health is invalid") from exc
    if (
        len(decoded) != size
        or base64.urlsafe_b64encode(decoded).decode().rstrip("=") != value
    ):
        raise RefreshBridgeError("signed gateway health is invalid")
    return decoded


def _signed_health_once(
    node: str,
    public_key: str,
    origin: str,
    *,
    opener: Any,
    clock_ms: Callable[[], int],
    timeout_seconds: float,
) -> bool:
    if timeout_seconds <= 0:
        return False
    nonce = base64.urlsafe_b64encode(os.urandom(18)).decode().rstrip("=")
    issued_at = clock_ms()
    query = urlencode({"nonce": nonce, "issuedAt": str(issued_at)})
    request = Request(
        origin + "/health?" + query,
        method="GET",
        headers={
            "Accept": "application/json",
            "User-Agent": HEALTH_USER_AGENT,
        },
    )
    try:
        with opener.open(request, timeout=timeout_seconds) as response:
            if int(response.status) != 200:
                return False
            raw = response.read(MAX_HEALTH_BYTES + 1)
    except (HTTPError, URLError, OSError, TimeoutError):
        return False
    if not raw or len(raw) > MAX_HEALTH_BYTES:
        return False
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return False
    challenge = value.get("challenge") if isinstance(value, dict) else None
    if (
        value.get("ok") is not True
        or value.get("integrity") != "ok"
        or value.get("node") != node
        or value.get("publicKey") != public_key
        or int(value.get("publicRepositoryCount") or 0) < 1
        or not isinstance(challenge, dict)
        or challenge.get("messageType") != "forkmesh-https-health-v1"
        or challenge.get("nonce") != nonce
        or challenge.get("issuedAt") != issued_at
        or challenge.get("algorithm") != "Ed25519"
        or challenge.get("encoding") != "base64url-no-padding"
        or not SHA256_RE.fullmatch(str(challenge.get("messageSha256") or ""))
        or not SIGNATURE_RE.fullmatch(str(challenge.get("signature") or ""))
    ):
        return False
    message = (f"forkmesh-https-health-v1\n{node}\n{nonce}\n{issued_at}").encode(
        "utf-8"
    )
    if challenge["messageSha256"] != hashlib.sha256(message).hexdigest():
        return False
    try:
        Ed25519PublicKey.from_public_bytes(_b64url_decode(public_key, 32)).verify(
            _b64url_decode(challenge["signature"], 64), message
        )
    except (InvalidSignature, ValueError, RefreshBridgeError):
        return False
    return True


class _NoRedirectHandler(HTTPRedirectHandler):
    """Reject redirects so health is proved only at the configured origin."""

    def redirect_request(
        self,
        req: Any,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> None:
        return None


def _direct_opener() -> Any:
    return build_opener(ProxyHandler({}), _NoRedirectHandler())


def wait_for_signed_health(
    config: BridgeConfig,
    *,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
    monotonic: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
    opener: Any | None = None,
) -> None:
    direct_opener = opener or _direct_opener()
    deadline = monotonic() + config.health_timeout_seconds
    while monotonic() < deadline:
        try:
            node, public_key, loopback_origin, public_origin = (
                _parse_gateway_identity(config.gateway_config_path)
            )
        except RefreshBridgeError:
            # A validated generation is installed by atomic replacement. Treat
            # that tiny replacement window as startup-not-ready and retry.
            remaining = deadline - monotonic()
            if remaining > 0:
                sleeper(min(HEALTH_RETRY_SECONDS, remaining))
            continue
        remaining = deadline - monotonic()
        if remaining <= 0:
            break
        loopback_ready = _signed_health_once(
            node,
            public_key,
            loopback_origin,
            opener=direct_opener,
            clock_ms=clock_ms,
            timeout_seconds=min(HEALTH_REQUEST_TIMEOUT_SECONDS, remaining),
        )
        remaining = deadline - monotonic()
        if loopback_ready and remaining > 0 and _signed_health_once(
            node,
            public_key,
            public_origin,
            opener=direct_opener,
            clock_ms=clock_ms,
            timeout_seconds=min(HEALTH_REQUEST_TIMEOUT_SECONDS, remaining),
        ):
            return
        remaining = deadline - monotonic()
        if remaining > 0:
            sleeper(min(HEALTH_RETRY_SECONDS, remaining))
    raise RefreshBridgeError("signed gateway health did not become ready")


def _claim_trigger(
    config: BridgeConfig,
    *,
    allow_retry: bool = False,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
) -> Path | None:
    processing = _processing_path(config)
    # Recover work left claimed if the oneshot was killed or the machine
    # restarted between claim and completion. systemd serializes this service,
    # so an existing processing marker is never a concurrent worker.
    if _safe_marker(processing):
        return processing
    if _safe_marker(config.trigger_path):
        os.replace(config.trigger_path, processing)
        return processing
    if not allow_retry or not _safe_marker(_retry_path(config)):
        return None
    state = _read_state(config)
    next_retry_at = int(state.get("nextRetryAt") or 0)
    if next_retry_at and next_retry_at > int(clock_ms()):
        return None
    os.replace(_retry_path(config), processing)
    return processing


def _persistent_failure(
    config: BridgeConfig,
    processing: Path | None,
    *,
    phase: str,
    clock_ms: Callable[[], int],
) -> None:
    now = int(clock_ms())
    previous = _read_state(config)
    attempts = min(
        int(previous.get("attempts") or 0) + 1,
        MAX_PERSISTENT_ATTEMPTS,
    )
    delay_index = min(
        max(attempts - 1, 0), len(PERSISTENT_RETRY_DELAYS_SECONDS) - 1
    )
    next_retry_at = now + int(
        PERSISTENT_RETRY_DELAYS_SECONDS[delay_index] * 1000
    )
    retry = _retry_path(config)
    if processing is not None and _safe_marker(processing):
        os.replace(processing, retry)
    elif not _safe_marker(retry):
        _atomic_json(
            retry,
            {"schemaVersion": 1, "type": "forkmesh-refresh-retry"},
        )
    _write_state(
        config,
        status="retry-pending",
        attempts=attempts,
        now_ms=now,
        phase=phase,
        next_retry_at=next_retry_at,
    )


def _publication_succeeded(
    config: BridgeConfig,
    processing: Path | None,
    *,
    clock_ms: Callable[[], int],
) -> None:
    if processing is not None:
        try:
            processing.unlink()
        except FileNotFoundError:
            pass
    try:
        _retry_path(config).unlink()
    except FileNotFoundError:
        pass
    _write_state(
        config,
        status="published",
        attempts=0,
        now_ms=int(clock_ms()),
    )


def _run_locked(
    config: BridgeConfig,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
    health_waiter: Callable[[BridgeConfig], None] = wait_for_signed_health,
    refresh_monotonic: Callable[[], float] = time.monotonic,
    refresh_sleeper: Callable[[float], None] = time.sleep,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
    reconcile: bool = False,
) -> dict[str, Any]:
    if os.geteuid() != 0:
        raise RefreshBridgeError("refresh orchestration requires root")
    processing = _claim_trigger(
        config, allow_retry=reconcile, clock_ms=clock_ms)
    if processing is None and not reconcile:
        return {"ok": True, "event": "ssh_push_refresh_idle"}
    if (
        processing is None
        and reconcile
        and _safe_marker(_retry_path(config))
    ):
        # The periodic reconciliation timer also drives persistent retries, but
        # respects their bounded backoff rather than spinning a failing unit.
        return {"ok": False, "event": "ssh_push_refresh_retry_deferred"}
    phase = "refresh"
    _write_state(
        config,
        status="processing",
        attempts=int(_read_state(config).get("attempts") or 0),
        now_ms=int(clock_ms()),
        phase=phase,
    )
    renewal_paused = False
    try:
        _pause_renewal(runner=runner)
        renewal_paused = True
        _refresh_with_retry(
            config,
            runner=runner,
            monotonic=refresh_monotonic,
            sleeper=refresh_sleeper,
        )
        phase = "restart"
        _restart_gateway(config, runner=runner)
        phase = "health"
        health_waiter(config)
        # Serving is now current and integrity-checked. Do not synchronously
        # renew the catalog lease here: an unavailable control-plane endpoint
        # can block this oneshot for minutes, preventing systemd.path from
        # processing a newer push. The resumed bounded renewal timer performs
        # that publication independently while this critical path becomes idle
        # again within seconds.
        _resume_renewal(runner=runner)
        renewal_paused = False
        _publication_succeeded(config, processing, clock_ms=clock_ms)
        return {"ok": True, "event": "ssh_push_refresh_published"}
    except BaseException:
        if renewal_paused:
            try:
                _resume_renewal(runner=runner)
                renewal_paused = False
            except BaseException:
                # Preserve the original publication error and retained retry
                # marker. Reconciliation retries this fixed resume command.
                pass
        _persistent_failure(
            config, processing, phase=phase, clock_ms=clock_ms)
        raise


def run(
    config: BridgeConfig,
    *,
    runner: Callable[..., subprocess.CompletedProcess[Any]] = subprocess.run,
    health_waiter: Callable[[BridgeConfig], None] = wait_for_signed_health,
    refresh_monotonic: Callable[[], float] = time.monotonic,
    refresh_sleeper: Callable[[float], None] = time.sleep,
    clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
    reconcile: bool = False,
) -> dict[str, Any]:
    """Serialize path-triggered and timer-triggered publication work."""
    if os.geteuid() != 0:
        raise RefreshBridgeError("refresh orchestration requires root")
    lock_path = config.trigger_path.with_name(".orchestrator.lock")
    flags = os.O_RDWR | os.O_CREAT
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(lock_path, flags, 0o600)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise RefreshBridgeError("refresh orchestration lock is unsafe")
        fcntl.flock(descriptor, fcntl.LOCK_EX)
    except RefreshBridgeError:
        try:
            os.close(descriptor)
        except (NameError, OSError):
            pass
        raise
    except OSError as exc:
        try:
            os.close(descriptor)
        except (NameError, OSError):
            pass
        raise RefreshBridgeError(
            "refresh orchestration lock is unavailable") from exc
    try:
        return _run_locked(
            config,
            runner=runner,
            health_waiter=health_waiter,
            refresh_monotonic=refresh_monotonic,
            refresh_sleeper=refresh_sleeper,
            clock_ms=clock_ms,
            reconcile=reconcile,
        )
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fixed post-receive trigger for a ForkMesh mirror"
    )
    parser.add_argument(
        "--config",
        required=True,
        type=Path,
        help="absolute root-owned bridge configuration",
    )
    parser.add_argument("mode", choices=("notify", "run", "reconcile", "status"))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = load_config(args.config)
        if args.mode == "notify":
            notify(config)
            return 0
        if args.mode == "status":
            result = refresh_status(config)
        else:
            result = run(config, reconcile=args.mode == "reconcile")
        print(
            json.dumps(result, sort_keys=True, separators=(",", ":")),
            flush=True,
        )
        return 0 if result.get("ok") is not False else 1
    except RefreshBridgeError as exc:
        print(f"ForkMesh SSH refresh: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("ForkMesh SSH refresh: interrupted", file=sys.stderr)
        return 130
    except Exception:
        print("ForkMesh SSH refresh: unexpected local failure", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
