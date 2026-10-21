#!/usr/bin/env python3
"""Bounded local authorization broker for the ForkMesh SSH Git gateway.

The dedicated broker account is the only local principal that can read the
Worker bearer token.  Two unprivileged clients may connect through one
group-readable Unix socket:

* the OpenSSH lookup account may submit only public-key ``lookup`` requests;
* the Git service account may submit only repository ``authorize`` requests.

Every connection carries one length-prefixed JSON request.  Linux
``SO_PEERCRED`` determines the caller; no claimed uid, URL, header, token,
repository path, or arbitrary command is accepted.  The broker disables
ambient proxies and HTTP redirects and returns only a validated, bounded
authorization result.
"""

from __future__ import annotations

import argparse
import base64
from collections import deque
from dataclasses import dataclass
import grp
import http.client
import ipaddress
import json
import os
from pathlib import Path
import pwd
import re
import socket
import socketserver
import ssl
import stat
import struct
import sys
import threading
import time
from typing import Any, Mapping
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import (
    HTTPRedirectHandler,
    HTTPSHandler,
    ProxyHandler,
    Request,
    build_opener,
)


SCHEMA_VERSION = 1
CONFIG_TYPE = "forkmesh.ssh-authorization-broker"
GRANTS_TYPE = "forkmesh.ssh-local-grants"
DYNAMIC_REPOSITORIES_TYPE = "forkmesh.dynamic-repositories"
LOCAL_MODES = frozenset({"fallback", "local"})
LOCAL_ACCESS_LEVELS = frozenset({"read-only", "read-write", "pull-request"})
MAX_CONFIG_BYTES = 64 * 1024
MAX_GRANTS_BYTES = 256 * 1024
MAX_LOCAL_GRANTS = 200
MAX_GRANT_REPOSITORIES = 200
MAX_DYNAMIC_REPOSITORIES = 1000
MAX_FRAME_BYTES = 32 * 1024
MAX_RESPONSE_BYTES = 64 * 1024
HTTP_TIMEOUT_SECONDS = 8
SOCKET_TIMEOUT_SECONDS = 12
MAX_CONCURRENT_REQUESTS = 16
RATE_WINDOW_SECONDS = 60.0
RATE_REQUESTS_PER_WINDOW = 120
UPSTREAM_UNAVAILABLE = "authorization service is unavailable"
OWNER_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
KEY_ID_RE = re.compile(r"^sk_[A-Za-z0-9_-]{16,80}$")
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{24}$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{32,512}$")
HTTP_USERNAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,62}$")
HTTP_PASSWORD_RE = re.compile(
    r"^fms1\.(sk_[A-Za-z0-9_-]{16,80})\.[0-9]{10,16}\.[0-9a-f]{64}$"
)
PUBLIC_KEY_RE = re.compile(
    r"^(?:ssh-ed25519|sk-ssh-ed25519@openssh\.com|"
    r"ecdsa-sha2-nistp(?:256|384|521)|"
    r"sk-ecdsa-sha2-nistp256@openssh\.com|ssh-rsa) "
    r"[A-Za-z0-9+/]+={0,2}$"
)


class BrokerError(RuntimeError):
    """A fixed, non-sensitive broker failure."""


@dataclass(frozen=True)
class BrokerConfig:
    api_origin: str
    token: str
    socket_path: Path
    socket_gid: int
    lookup_uid: int
    git_uid: int
    # "off" preserves the historical worker-only behavior. "fallback" consults
    # the local grants store only when the Worker is UNREACHABLE (never after a
    # denial, so revocation cannot be bypassed by cutting the uplink). "local"
    # never contacts a Worker at all — the controller-free mirror mode.
    local_mode: str = "off"
    grants_file: Path | None = None
    dynamic_repositories_file: Path | None = None
    # Optional "127.0.0.1:443": reach apiOrigin's own TLS front end on this
    # host instead of through a CDN, still verifying its certificate for the
    # apiOrigin host name. Absent (a gateway on a separate front host), the
    # broker resolves apiOrigin through DNS and verifies TLS as usual, so
    # its configuration never names the origin's address.
    api_connect_address: tuple[str, int] | None = None


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise BrokerError("configuration contains a duplicate field")
        value[key] = item
    return value


def _read_file(
    path: Path,
    *,
    maximum: int,
    owner_uids: frozenset[int],
    private: bool,
) -> bytes:
    descriptor = -1
    try:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        forbidden_mode = 0o077 if private else 0o022
        if (
            not path.is_absolute()
            or not stat.S_ISREG(info.st_mode)
            or info.st_uid not in owner_uids
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) & forbidden_mode
            or info.st_size <= 0
            or info.st_size > maximum
        ):
            raise BrokerError("authorization broker file is unsafe")
        content = bytearray()
        while len(content) <= maximum:
            chunk = os.read(
                descriptor,
                min(64 * 1024, maximum + 1 - len(content)),
            )
            if not chunk:
                break
            content.extend(chunk)
        if len(content) != info.st_size or len(content) > maximum:
            raise BrokerError("authorization broker file changed while reading")
        return bytes(content)
    except BrokerError:
        raise
    except OSError as exc:
        raise BrokerError("authorization broker file is unavailable") from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _https_origin(value: Any) -> str:
    try:
        parsed = urlsplit(str(value or "").strip())
        port = parsed.port
    except ValueError as exc:
        raise BrokerError("authorization origin is invalid") from exc
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path not in {"", "/"}
    ):
        raise BrokerError("authorization origin is invalid")
    suffix = "" if port in {None, 443} else ":" + str(port)
    return "https://" + parsed.hostname.lower() + suffix


def _account(name: Any, label: str) -> pwd.struct_passwd:
    clean = str(name or "")
    if not re.fullmatch(r"[a-z_][a-z0-9_-]{0,62}", clean):
        raise BrokerError(f"{label} account is invalid")
    try:
        record = pwd.getpwnam(clean)
    except KeyError as exc:
        raise BrokerError(f"{label} account is unavailable") from exc
    if record.pw_uid == 0:
        raise BrokerError(f"{label} account is invalid")
    return record


def _is_group_member(user: pwd.struct_passwd, group: grp.struct_group) -> bool:
    return user.pw_gid == group.gr_gid or user.pw_name in set(group.gr_mem)


def load_config(path: Path) -> BrokerConfig:
    raw = _read_file(
        path,
        maximum=MAX_CONFIG_BYTES,
        owner_uids=frozenset({0, os.geteuid()}),
        private=False,
    )
    try:
        source = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except BrokerError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BrokerError("authorization broker configuration is invalid") from exc
    base_fields = {
        "schemaVersion",
        "type",
        "socketPath",
        "socketGroup",
        "lookupUser",
        "gitUser",
    }
    worker_fields = {"apiOrigin", "gatewayTokenFile"}
    allowed_shapes = (
        base_fields | worker_fields,
        base_fields | worker_fields | {"localAuthorization"},
        base_fields | {"localAuthorization"},
    )
    if (
        not isinstance(source, dict)
        or (set(source) - {"apiConnectAddress"}) not in allowed_shapes
        or ("apiConnectAddress" in source and "apiOrigin" not in source)
        or source.get("schemaVersion") != SCHEMA_VERSION
        or source.get("type") != CONFIG_TYPE
    ):
        raise BrokerError("authorization broker configuration is unsupported")
    local_mode, grants_file, dynamic_file = _local_authorization_settings(
        source.get("localAuthorization"),
        worker_configured=worker_fields <= set(source),
    )

    lookup = _account(source.get("lookupUser"), "lookup")
    git = _account(source.get("gitUser"), "Git")
    if lookup.pw_uid == git.pw_uid or os.geteuid() in {lookup.pw_uid, git.pw_uid}:
        raise BrokerError("authorization broker accounts are not isolated")
    try:
        socket_group = grp.getgrnam(str(source.get("socketGroup") or ""))
    except KeyError as exc:
        raise BrokerError("authorization socket group is unavailable") from exc
    if not _is_group_member(lookup, socket_group) or not _is_group_member(
        git, socket_group
    ):
        raise BrokerError("authorization socket group is incomplete")
    if socket_group.gr_gid not in {os.getegid(), *os.getgroups()}:
        raise BrokerError("authorization broker lacks its socket group")

    socket_path = Path(str(source.get("socketPath") or ""))
    if (
        not socket_path.is_absolute()
        or socket_path.name in {"", ".", ".."}
        or "\x00" in str(socket_path)
    ):
        raise BrokerError("authorization socket path is invalid")
    try:
        parent = socket_path.parent.resolve(strict=True)
        parent_info = parent.lstat()
    except OSError as exc:
        raise BrokerError("authorization socket directory is unavailable") from exc
    if (
        not stat.S_ISDIR(parent_info.st_mode)
        or stat.S_ISLNK(parent_info.st_mode)
        or parent_info.st_uid not in {0, os.geteuid()}
        or parent_info.st_gid != socket_group.gr_gid
        or stat.S_IMODE(parent_info.st_mode) & 0o007
        or stat.S_IMODE(parent_info.st_mode) & stat.S_IWGRP
    ):
        raise BrokerError("authorization socket directory is unsafe")

    api_origin = ""
    token = ""
    if "gatewayTokenFile" in source:
        token_path = Path(str(source.get("gatewayTokenFile") or ""))
        token_raw = _read_file(
            token_path,
            maximum=4096,
            owner_uids=frozenset({0, os.geteuid()}),
            private=True,
        )
        try:
            token = token_raw.decode("ascii").strip()
        except UnicodeDecodeError as exc:
            raise BrokerError("authorization token is unavailable") from exc
        if not TOKEN_RE.fullmatch(token):
            raise BrokerError("authorization token is unavailable")
        api_origin = _https_origin(source.get("apiOrigin"))
    api_connect_address = None
    if "apiConnectAddress" in source:
        api_connect_address = loopback_address(source.get("apiConnectAddress"))
        if api_connect_address is None:
            raise BrokerError("authorization connect address is invalid")
    return BrokerConfig(
        api_origin=api_origin,
        token=token,
        socket_path=parent / socket_path.name,
        socket_gid=socket_group.gr_gid,
        lookup_uid=lookup.pw_uid,
        git_uid=git.pw_uid,
        local_mode=local_mode,
        grants_file=grants_file,
        dynamic_repositories_file=dynamic_file,
        api_connect_address=api_connect_address,
    )


def loopback_address(value: Any) -> tuple[str, int] | None:
    """Parse ``127.0.0.1:443`` / ``[::1]:443``; only loopback is accepted."""

    raw = str(value or "").strip()
    host, separator, port_text = raw.rpartition(":")
    if not separator or not port_text.isdigit():
        return None
    host = host[1:-1] if host.startswith("[") and host.endswith("]") else host
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return None
    port = int(port_text)
    if not address.is_loopback or not 1 <= port <= 65535:
        return None
    return str(address), port


class _LoopbackHTTPSConnection(http.client.HTTPSConnection):
    """HTTPS to a fixed loopback socket, verified for the URL's host name."""

    connect_address: tuple[str, int] = ("127.0.0.1", 443)

    def connect(self) -> None:
        sock = socket.create_connection(self.connect_address, self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=self.host)


def _loopback_https_handler(address: tuple[str, int]) -> HTTPSHandler:
    connection_class = type(
        "_BoundLoopbackHTTPSConnection",
        (_LoopbackHTTPSConnection,),
        {"connect_address": address},
    )

    class _Handler(HTTPSHandler):
        def https_open(self, req: Request) -> Any:
            return self.do_open(
                connection_class, req, context=ssl.create_default_context()
            )

    return _Handler()


def _local_authorization_settings(
    value: Any,
    *,
    worker_configured: bool,
) -> tuple[str, Path | None, Path | None]:
    if value is None:
        if not worker_configured:
            raise BrokerError(
                "authorization broker configuration is unsupported"
            )
        return "off", None, None
    allowed = {"mode", "grantsFile"}
    if (
        not isinstance(value, dict)
        or set(value) not in (allowed, allowed | {"dynamicRepositoriesFile"})
    ):
        raise BrokerError("local authorization configuration is invalid")
    mode = str(value.get("mode") or "")
    if mode not in LOCAL_MODES:
        raise BrokerError("local authorization configuration is invalid")
    # The mode must be honest about the deployment: a Worker uplink is
    # mandatory for "fallback" and contradictory for "local".
    if worker_configured != (mode == "fallback"):
        raise BrokerError("local authorization configuration is invalid")
    grants_file = Path(str(value.get("grantsFile") or ""))
    if not grants_file.is_absolute() or "\x00" in str(grants_file):
        raise BrokerError("local authorization configuration is invalid")
    dynamic_file: Path | None = None
    if "dynamicRepositoriesFile" in value:
        dynamic_file = Path(str(value.get("dynamicRepositoriesFile") or ""))
        if not dynamic_file.is_absolute() or "\x00" in str(dynamic_file):
            raise BrokerError("local authorization configuration is invalid")
    return mode, grants_file, dynamic_file


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(
        self,
        req: Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Mapping[str, str],
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl
        return None


def _read_exact(connection: socket.socket, size: int) -> bytes:
    content = bytearray()
    while len(content) < size:
        chunk = connection.recv(size - len(content))
        if not chunk:
            raise BrokerError("authorization frame is incomplete")
        content.extend(chunk)
    return bytes(content)


def _receive_frame(connection: socket.socket) -> dict[str, Any]:
    announced = struct.unpack("!I", _read_exact(connection, 4))[0]
    if announced <= 0 or announced > MAX_FRAME_BYTES:
        raise BrokerError("authorization frame size is invalid")
    raw = _read_exact(connection, announced)
    if connection.recv(1):
        raise BrokerError("authorization frame has trailing bytes")
    try:
        value = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except BrokerError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BrokerError("authorization frame is invalid") from exc
    if not isinstance(value, dict):
        raise BrokerError("authorization frame is invalid")
    return value


def _send_frame(connection: socket.socket, value: Mapping[str, Any]) -> None:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    if not raw or len(raw) > MAX_RESPONSE_BYTES:
        raise BrokerError("authorization response is invalid")
    connection.sendall(struct.pack("!I", len(raw)) + raw)


def _peer_uid(connection: socket.socket) -> int:
    if not hasattr(socket, "SO_PEERCRED"):
        raise BrokerError("peer credentials are unavailable")
    try:
        raw = connection.getsockopt(
            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")
        )
        _pid, uid, _gid = struct.unpack("3i", raw)
    except (OSError, struct.error) as exc:
        raise BrokerError("peer credentials are unavailable") from exc
    return uid


def _validate_request(
    value: dict[str, Any], peer_uid: int, config: BrokerConfig
) -> dict[str, str]:
    action = value.get("action")
    if action == "lookup" and peer_uid == config.lookup_uid:
        if set(value) != {"action", "publicKey"}:
            raise BrokerError("authorization request is invalid")
        public_key = str(value.get("publicKey") or "")
        if len(public_key.encode("utf-8")) > 16 * 1024 or not PUBLIC_KEY_RE.fullmatch(
            public_key
        ):
            raise BrokerError("authorization request is invalid")
        return {"action": "lookup", "publicKey": public_key}
    if action == "authorize" and peer_uid == config.git_uid:
        if set(value) != {
            "action",
            "keyId",
            "owner",
            "repository",
            "operation",
        }:
            raise BrokerError("authorization request is invalid")
        key_id = str(value.get("keyId") or "")
        owner = str(value.get("owner") or "")
        repository = str(value.get("repository") or "")
        operation = str(value.get("operation") or "")
        if (
            not KEY_ID_RE.fullmatch(key_id)
            # An empty owner is a short address (git@host:Repository.git);
            # the control plane resolves it or denies.
            or (owner and not OWNER_RE.fullmatch(owner))
            or not REPO_RE.fullmatch(repository)
            or operation not in {"git-upload-pack", "git-receive-pack"}
        ):
            raise BrokerError("authorization request is invalid")
        return {
            "action": "authorize",
            "keyId": key_id,
            "owner": owner,
            "repository": repository,
            "operation": operation,
        }
    raise BrokerError("authorization action is not permitted")


def _validated_upstream_response(
    request_value: Mapping[str, str],
    response_value: Any,
    request_id: str,
) -> dict[str, Any]:
    if not isinstance(response_value, dict):
        raise BrokerError("authorization service response is invalid")
    if response_value.get("requestId") != request_id:
        raise BrokerError("authorization service response is invalid")
    if request_value["action"] == "lookup":
        if (
            set(response_value)
            != {
                "ok",
                "authorized",
                "keyId",
                "requestId",
            }
            or response_value.get("ok") is not True
            or response_value.get("authorized") is not True
        ):
            raise BrokerError("authorization service response is invalid")
        key_id = str(response_value.get("keyId") or "")
        if not KEY_ID_RE.fullmatch(key_id):
            raise BrokerError("authorization service response is invalid")
        return {"authorized": True, "keyId": key_id}

    expected = {
        "ok",
        "authorized",
        "keyId",
        "requestId",
        "operation",
        "owner",
        "repository",
        "repositoryRelativePath",
    }
    key_id = str(response_value.get("keyId") or "")
    owner = str(response_value.get("owner") or "")
    repository = str(response_value.get("repository") or "")
    operation = str(response_value.get("operation") or "")
    relative = str(response_value.get("repositoryRelativePath") or "")
    credential = response_value.get("httpCredential")
    if credential is not None:
        password = (
            str(credential.get("password") or "")
            if isinstance(credential, dict) else ""
        )
        match = HTTP_PASSWORD_RE.fullmatch(password)
        expires_at = (
            credential.get("expiresAt") if isinstance(credential, dict) else None
        )
        if (
            not isinstance(credential, dict)
            or set(credential) != {"username", "password", "expiresAt"}
            or not HTTP_USERNAME_RE.fullmatch(str(credential.get("username") or ""))
            or not match
            or match.group(1) != request_value["keyId"]
            or isinstance(expires_at, bool)
            or not isinstance(expires_at, int)
        ):
            raise BrokerError("authorization service response is invalid")
        expected = expected | {"httpCredential"}
    if (
        set(response_value) != expected
        or response_value.get("ok") is not True
        or response_value.get("authorized") is not True
        or key_id != request_value["keyId"]
        or operation != request_value["operation"]
        or not OWNER_RE.fullmatch(owner)
        or not REPO_RE.fullmatch(repository)
        or repository.lower() != request_value["repository"].lower()
        or relative != owner + "/" + repository + ".git"
    ):
        raise BrokerError("authorization service response is invalid")
    result = {
        "authorized": True,
        "keyId": key_id,
        "operation": operation,
        "owner": owner,
        "repository": repository,
        "repositoryRelativePath": relative,
    }
    if credential is not None:
        result["httpCredential"] = {
            "username": str(credential["username"]),
            "password": str(credential["password"]),
            "expiresAt": int(credential["expiresAt"]),
        }
    return result


def _authorize_upstream(
    config: BrokerConfig,
    value: Mapping[str, str],
    *,
    opener: Any | None = None,
) -> dict[str, Any]:
    request_id = base64.urlsafe_b64encode(os.urandom(18)).decode().rstrip("=")
    if not REQUEST_ID_RE.fullmatch(request_id):
        raise BrokerError("authorization request could not be created")
    payload = dict(value)
    payload["requestId"] = request_id
    body = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    request = Request(
        config.api_origin + "/api/ssh/authorize",
        data=body,
        method="POST",
        headers={
            "accept": "application/json",
            "authorization": "Bearer " + config.token,
            "content-type": "application/json",
            "user-agent": "ForkMesh-ssh-authorization-broker/1.0",
        },
    )
    if opener is None:
        handlers: list[Any] = [_NoRedirect(), ProxyHandler({})]
        if config.api_connect_address is not None:
            handlers.append(_loopback_https_handler(config.api_connect_address))
        opener = build_opener(*handlers)
    direct = opener
    try:
        with direct.open(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            if int(response.status) != 200:
                raise BrokerError("authorization service denied access")
            raw = response.read(MAX_RESPONSE_BYTES + 1)
    except HTTPError as exc:
        raise BrokerError("authorization service denied access") from exc
    except (URLError, OSError, TimeoutError) as exc:
        raise BrokerError(UPSTREAM_UNAVAILABLE) from exc
    if not raw or len(raw) > MAX_RESPONSE_BYTES:
        raise BrokerError("authorization service response is invalid")
    try:
        response_value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BrokerError("authorization service response is invalid") from exc
    return _validated_upstream_response(value, response_value, request_id)


def _parse_bounded_json(raw: bytes, expected_type: str) -> dict[str, Any]:
    try:
        value = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except BrokerError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BrokerError("local authorization store is invalid") from exc
    if (
        not isinstance(value, dict)
        or value.get("schemaVersion") != SCHEMA_VERSION
        or value.get("type") != expected_type
    ):
        raise BrokerError("local authorization store is invalid")
    return value


def _load_local_grants(config: BrokerConfig) -> list[dict[str, Any]]:
    if config.grants_file is None:
        raise BrokerError("authorization denied")
    raw = _read_file(
        config.grants_file,
        maximum=MAX_GRANTS_BYTES,
        owner_uids=frozenset({0, os.geteuid()}),
        private=False,
    )
    value = _parse_bounded_json(raw, GRANTS_TYPE)
    if set(value) != {"schemaVersion", "type", "grants"}:
        raise BrokerError("local authorization store is invalid")
    grants = value.get("grants")
    if not isinstance(grants, list) or len(grants) > MAX_LOCAL_GRANTS:
        raise BrokerError("local authorization store is invalid")
    validated: list[dict[str, Any]] = []
    seen_key_ids: set[str] = set()
    for grant in grants:
        if (
            not isinstance(grant, dict)
            or set(grant) != {"keyId", "publicKey", "account", "repositories"}
        ):
            raise BrokerError("local authorization store is invalid")
        key_id = str(grant.get("keyId") or "")
        public_key = str(grant.get("publicKey") or "")
        account = str(grant.get("account") or "")
        repositories = grant.get("repositories")
        if (
            not KEY_ID_RE.fullmatch(key_id)
            or key_id in seen_key_ids
            or len(public_key.encode("utf-8")) > 16 * 1024
            or not PUBLIC_KEY_RE.fullmatch(public_key)
            or not OWNER_RE.fullmatch(account)
            or not isinstance(repositories, list)
            or len(repositories) > MAX_GRANT_REPOSITORIES
        ):
            raise BrokerError("local authorization store is invalid")
        seen_key_ids.add(key_id)
        entries: dict[tuple[str, str], dict[str, str]] = {}
        for item in repositories:
            if (
                not isinstance(item, dict)
                or set(item) != {"owner", "name", "access"}
            ):
                raise BrokerError("local authorization store is invalid")
            owner = str(item.get("owner") or "")
            name = str(item.get("name") or "")
            access = str(item.get("access") or "")
            if (
                not OWNER_RE.fullmatch(owner)
                or not REPO_RE.fullmatch(name)
                or access not in LOCAL_ACCESS_LEVELS
            ):
                raise BrokerError("local authorization store is invalid")
            entries[(owner, name.lower())] = {"name": name, "access": access}
        validated.append(
            {
                "keyId": key_id,
                "publicKey": public_key,
                "account": account,
                "repositories": entries,
            }
        )
    return validated


def _load_dynamic_repositories(
    config: BrokerConfig,
) -> dict[tuple[str, str], str]:
    """Fork registry entries: (owner, lowercased name) -> canonical name.

    The registry is written by the Git service account when an authorized key
    forks a repository, so accept its uid in addition to the operator's.  A
    missing file simply means no fork has been created yet.
    """

    if config.dynamic_repositories_file is None:
        return {}
    try:
        raw = _read_file(
            config.dynamic_repositories_file,
            maximum=MAX_GRANTS_BYTES,
            owner_uids=frozenset({0, os.geteuid(), config.git_uid}),
            private=False,
        )
    except BrokerError:
        try:
            config.dynamic_repositories_file.lstat()
        except OSError:
            return {}
        raise
    value = _parse_bounded_json(raw, DYNAMIC_REPOSITORIES_TYPE)
    if set(value) != {"schemaVersion", "type", "repositories"}:
        raise BrokerError("local authorization store is invalid")
    repositories = value.get("repositories")
    if (
        not isinstance(repositories, list)
        or len(repositories) > MAX_DYNAMIC_REPOSITORIES
    ):
        raise BrokerError("local authorization store is invalid")
    entries: dict[tuple[str, str], str] = {}
    for item in repositories:
        if not isinstance(item, dict):
            raise BrokerError("local authorization store is invalid")
        owner = str(item.get("owner") or "")
        name = str(item.get("name") or "")
        if not OWNER_RE.fullmatch(owner) or not REPO_RE.fullmatch(name):
            raise BrokerError("local authorization store is invalid")
        entries[(owner, name.lower())] = name
    return entries


def _authorize_local(
    config: BrokerConfig, value: Mapping[str, str]
) -> dict[str, Any]:
    grants = _load_local_grants(config)
    if value["action"] == "lookup":
        for grant in grants:
            if grant["publicKey"] == value["publicKey"]:
                return {"authorized": True, "keyId": grant["keyId"]}
        raise BrokerError("authorization denied")
    grant = next(
        (item for item in grants if item["keyId"] == value["keyId"]), None
    )
    if grant is None:
        raise BrokerError("authorization denied")
    owner = value["owner"]
    requested = value["repository"]
    operation = value["operation"]
    entry = grant["repositories"].get((owner, requested.lower()))
    if entry is not None:
        name = entry["name"]
        access = entry["access"]
    else:
        # A key's own dynamically created forks are implicitly writable: the
        # fork verb only ever creates repositories under the grant's account.
        dynamic = _load_dynamic_repositories(config)
        name = dynamic.get((owner, requested.lower()), "")
        if not name or owner != grant["account"]:
            raise BrokerError("authorization denied")
        access = "read-write"
    if operation == "git-receive-pack" and access == "read-only":
        raise BrokerError("authorization denied")
    result: dict[str, Any] = {
        "authorized": True,
        "keyId": grant["keyId"],
        "operation": operation,
        "owner": owner,
        "repository": name,
        "repositoryRelativePath": owner + "/" + name + ".git",
        "account": grant["account"],
    }
    if operation == "git-receive-pack" and access == "pull-request":
        result["refNamespace"] = "refs/heads/pr/" + grant["account"] + "/"
    return result


def _authorize_request(
    config: BrokerConfig, value: Mapping[str, str]
) -> dict[str, Any]:
    if config.local_mode == "local":
        return _authorize_local(config, value)
    try:
        return _authorize_upstream(config, value)
    except BrokerError as exc:
        # Only Worker UNREACHABILITY may fall through to the local store. An
        # authoritative denial (revoked key, bad response) must stay a denial.
        if config.local_mode == "fallback" and str(exc) == UPSTREAM_UNAVAILABLE:
            return _authorize_local(config, value)
        raise


class AuthorizationServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    allow_reuse_address = False
    request_queue_size = 32

    def __init__(self, config: BrokerConfig):
        self.config = config
        self._slots = threading.BoundedSemaphore(MAX_CONCURRENT_REQUESTS)
        self._rate_lock = threading.Lock()
        self._request_times: dict[int, deque[float]] = {}
        try:
            existing = config.socket_path.lstat()
        except FileNotFoundError:
            existing = None
        if existing is not None:
            if not stat.S_ISSOCK(existing.st_mode) or existing.st_uid != os.geteuid():
                raise BrokerError("authorization socket path is unsafe")
            config.socket_path.unlink()
        previous_umask = os.umask(0o117)
        try:
            super().__init__(str(config.socket_path), AuthorizationHandler)
            os.chown(config.socket_path, os.geteuid(), config.socket_gid)
            os.chmod(config.socket_path, 0o660)
        except Exception:
            try:
                config.socket_path.unlink()
            except FileNotFoundError:
                pass
            raise
        finally:
            os.umask(previous_umask)

    def verify_request(self, request: socket.socket, client_address: Any) -> bool:
        del client_address
        try:
            uid = _peer_uid(request)
        except BrokerError:
            return False
        if uid not in {self.config.lookup_uid, self.config.git_uid}:
            return False
        now = time.monotonic()
        with self._rate_lock:
            events = self._request_times.setdefault(uid, deque())
            while events and events[0] <= now - RATE_WINDOW_SECONDS:
                events.popleft()
            if len(events) >= RATE_REQUESTS_PER_WINDOW:
                return False
            events.append(now)
        return True

    def process_request(self, request: socket.socket, client_address: Any) -> None:
        if not self._slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            raise

    def process_request_thread(
        self, request: socket.socket, client_address: Any
    ) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            try:
                self.config.socket_path.unlink()
            except FileNotFoundError:
                pass


class AuthorizationHandler(socketserver.BaseRequestHandler):
    server: AuthorizationServer

    def handle(self) -> None:
        self.request.settimeout(SOCKET_TIMEOUT_SECONDS)
        try:
            peer_uid = _peer_uid(self.request)
            value = _validate_request(
                _receive_frame(self.request), peer_uid, self.server.config
            )
            result = _authorize_request(self.server.config, value)
            _send_frame(self.request, result)
        except (BrokerError, OSError, TimeoutError):
            try:
                _send_frame(self.request, {"authorized": False})
            except (BrokerError, OSError, TimeoutError):
                pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ForkMesh local SSH authorization broker"
    )
    parser.add_argument("--config", required=True, type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        config = load_config(args.config)
        with AuthorizationServer(config) as server:
            server.serve_forever(poll_interval=0.25)
        return 0
    except BrokerError as exc:
        print(f"ForkMesh SSH authorization broker: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
    except Exception:
        print(
            "ForkMesh SSH authorization broker: unexpected local failure",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
