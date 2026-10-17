#!/usr/bin/env python3
"""Provision an independent, self-contained ForkMesh App on Cloudflare.

The bootstrapper deliberately keeps the Cloudflare API token in process memory:
it reads the token from the environment, a terminal prompt, or stdin; passes it
to Wrangler through the child environment; and never writes it to a config or
env file.  The generated Wrangler config contains public resource identifiers
only and is removed in a ``finally`` block.

The operation is idempotent.  An existing D1 database, proxied DNS record, and
Worker route are reused when they already match the requested deployment.
"""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import getpass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)


ROOT = Path(__file__).resolve().parents[1]
WORKER_DIR = ROOT / "app"
WORLD_DIR = ROOT / "world"
WRANGLER_TEMPLATE = WORKER_DIR / "wrangler.toml"
API_BASE = "https://api.cloudflare.com/client/v4"
NAME_RE = re.compile(r"^[a-z][a-z0-9-]{0,62}$")
ENV_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]{0,127}$")
RETRYABLE_HTTP = frozenset({408, 409, 425, 429, 500, 502, 503, 504})
MIRROR_MANIFEST_ASSET = "forkmesh-mirror.json"
MIRROR_MANIFEST_PATH = "/" + MIRROR_MANIFEST_ASSET
ROUTER_PUBLIC_KEY_ENV = "MIRROR_ROUTER_PUBLIC_KEY"
ROUTER_SIGNING_SEED_ENV = "MIRROR_ROUTER_SIGNING_SEED"
# The signing seed must be published before the public key. Until the public
# key exists, the App identity endpoint fails closed; putting
# it last prevents a partially staged identity from advertising an unusable
# (or mismatched) public key.
ROUTER_IDENTITY_ENV_NAMES = (ROUTER_SIGNING_SEED_ENV, ROUTER_PUBLIC_KEY_ENV)
ROUTER_IDENTITY_PATH = "/api/mirrors/https"
PUBLIC_SNAPSHOT_PATHS = (
    "/api/repositories",
    "/api/network/overview",
    "/api/repo/forkmesh/forkmesh/mirrors",
    "/api/status",
    "/api/status/world",
)


class BootstrapError(RuntimeError):
    """A safe-to-display bootstrap failure."""


class RouterIdentityNotConfigured(BootstrapError):
    """The owned App route returned the exact fail-closed identity contract."""


def _toml_string(value: str) -> str:
    # JSON string syntax is valid TOML basic-string syntax for these values.
    return json.dumps(value, ensure_ascii=False)


def _canonical_json(value: Any) -> str:
    """ForkMesh JSON sort v1: UTF-8, sorted keys, compact separators."""

    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )


def _decode_base64url(value: str) -> bytes:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise BootstrapError("identity value is not unpadded base64url")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except ValueError as exc:
        raise BootstrapError("identity value is not valid base64url") from exc
    canonical = base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=")
    if not hmac.compare_digest(canonical, value):
        raise BootstrapError("identity value is not canonical unpadded base64url")
    return decoded


def validate_ed25519_public_key(value: str) -> str:
    if len(_decode_base64url(value)) != 32:
        raise BootstrapError("mirror public key must contain 32 Ed25519 bytes")
    return value


def validate_ed25519_signature(value: str) -> str:
    if len(_decode_base64url(value)) != 64:
        raise BootstrapError("mirror signature must contain 64 Ed25519 bytes")
    return value


# RFC 8032 Ed25519 public-key derivation.  Keeping this small implementation in
# the bootstrapper avoids making a deployment-time crypto package a prerequisite
# just to prove that an operator's public router key matches its local seed.
_ED25519_FIELD = 2**255 - 19
_ED25519_D = (
    -121665 * pow(121666, _ED25519_FIELD - 2, _ED25519_FIELD)
) % _ED25519_FIELD
_ED25519_BASE = (
    15112221349535400772501151409588531511454012693041857206046113283949847762202,
    46316835694926478169428394003475163141307993866256225615783033603165251855960,
)


def _ed25519_add(left: tuple[int, int], right: tuple[int, int]) -> tuple[int, int]:
    x1, y1 = left
    x2, y2 = right
    product = (_ED25519_D * x1 * x2 * y1 * y2) % _ED25519_FIELD
    x3 = (
        (x1 * y2 + y1 * x2) * pow(1 + product, _ED25519_FIELD - 2, _ED25519_FIELD)
    ) % _ED25519_FIELD
    y3 = (
        (y1 * y2 + x1 * x2) * pow(1 - product, _ED25519_FIELD - 2, _ED25519_FIELD)
    ) % _ED25519_FIELD
    return x3, y3


def derive_ed25519_public_key(signing_seed: str) -> str:
    """Derive an unpadded base64url Ed25519 public key from a 32-byte seed."""

    seed = _decode_base64url(signing_seed)
    if len(seed) != 32:
        raise BootstrapError("router signing seed must contain 32 Ed25519 bytes")
    hashed = bytearray(hashlib.sha512(seed).digest()[:32])
    hashed[0] &= 248
    hashed[31] &= 63
    hashed[31] |= 64
    scalar = int.from_bytes(hashed, "little")
    point = (0, 1)
    addend = _ED25519_BASE
    while scalar:
        if scalar & 1:
            point = _ed25519_add(point, addend)
        addend = _ed25519_add(addend, addend)
        scalar >>= 1
    x, y = point
    encoded = bytearray(y.to_bytes(32, "little"))
    encoded[31] |= (x & 1) << 7
    return base64.urlsafe_b64encode(encoded).decode("ascii").rstrip("=")


def validate_router_identity_pair(public_key: str, signing_seed: str) -> str:
    """Validate a local router identity without returning its secret seed."""

    public_key = validate_ed25519_public_key(public_key)
    derived = derive_ed25519_public_key(signing_seed)
    if not hmac.compare_digest(public_key, derived):
        raise BootstrapError(
            "MIRROR_ROUTER_PUBLIC_KEY does not match MIRROR_ROUTER_SIGNING_SEED"
        )
    return public_key


def mirror_manifest_payload(manifest: dict[str, Any]) -> bytes:
    """Return the exact bytes covered by the endpoint signature."""

    unsigned = {key: value for key, value in manifest.items() if key != "signature"}
    return _canonical_json(unsigned).encode("utf-8")


def unsigned_mirror_manifest(
    *,
    hostname: str,
    node_name: str,
    public_key: str,
    generated_at: str | None = None,
) -> dict[str, Any]:
    origin = f"https://{hostname}"
    return {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": generated_at
        or datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "node": {
            "name": node_name,
            "publicKey": validate_ed25519_public_key(public_key),
        },
        "endpoint": {
            "origin": origin,
            "healthUrl": origin + "/health",
            "manifestUrl": origin + MIRROR_MANIFEST_PATH,
            "repositoryUrlTemplate": origin + "/{owner}/{repository}",
            "transport": "direct-https",
            "mainProxyMode": "masked",
        },
        "dns": {
            "recordName": hostname,
            "proxied": True,
            "workerRoute": f"{hostname}/*",
        },
        "storage": {
            "repositoryBytesInD1": False,
            "repositoryByteOwner": "independent-mirror-host",
            "d1Purpose": [
                "service-discovery",
                "health-metadata",
                "routing-metadata",
                "signed-manifests",
            ],
        },
    }


class MirrorManifestSigner:
    """External Ed25519 signer adapter; private key material stays local."""

    def __init__(
        self,
        command: Iterable[str],
        *,
        runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    ) -> None:
        self.command = list(command)
        if not self.command or any(not part for part in self.command):
            raise BootstrapError(
                "a local manifest signer command is required unless "
                "--skip-mirror-manifest is used"
            )
        self._runner = runner

    def sign(
        self,
        manifest: dict[str, Any],
        *,
        hidden_env_names: Iterable[str] = (),
    ) -> dict[str, Any]:
        payload = mirror_manifest_payload(manifest)
        request = {
            "schemaVersion": 1,
            "type": "forkmesh.mirror-endpoint-signing-request",
            "algorithm": "Ed25519",
            "encoding": "base64url-no-padding",
            "canonicalization": "forkmesh-json-sort-v1",
            "publicKey": manifest["node"]["publicKey"],
            "payloadBase64": base64.urlsafe_b64encode(payload).decode().rstrip("="),
            "payloadSha256": hashlib.sha256(payload).hexdigest(),
        }
        env = os.environ.copy()
        for name in {
            "CLOUDFLARE_API_TOKEN",
            "CF_API_TOKEN",
            "CLOUDFLARE_TOKEN",
            "CF_TOKEN",
            *hidden_env_names,
        }:
            env.pop(name, None)
        try:
            completed = self._runner(
                self.command,
                input=_canonical_json(request) + "\n",
                text=True,
                capture_output=True,
                env=env,
                check=True,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            raise BootstrapError(
                "local mirror manifest signer failed; inspect its local logs"
            ) from exc
        try:
            response = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            raise BootstrapError("local mirror manifest signer returned invalid JSON") from exc
        if not isinstance(response, dict):
            raise BootstrapError("local mirror manifest signer returned a non-object")
        if any(
            re.search(r"(?:private|secret|seed|mnemonic|keypair)", str(key), re.I)
            for key in response
        ):
            raise BootstrapError("local signer returned a prohibited private-key field")
        if response.get("publicKey") not in (None, manifest["node"]["publicKey"]):
            raise BootstrapError("local signer used a different node public key")
        signature = validate_ed25519_signature(str(response.get("signature") or ""))
        signed = dict(manifest)
        signed["signature"] = {
            "algorithm": "Ed25519",
            "encoding": "base64url-no-padding",
            "canonicalization": "forkmesh-json-sort-v1",
            "payloadSha256": request["payloadSha256"],
            "value": signature,
        }
        return signed


@contextmanager
def staged_public_assets(
    mirror_manifest: dict[str, Any] | None,
) -> Iterable[Path]:
    """Stage assets without writing the deployment manifest into the checkout."""

    try:
        subprocess.run(
            [sys.executable, "tools/build_site_assets.py", "single"],
            cwd=WORKER_DIR,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        raise BootstrapError(
            f"App asset staging failed with exit code {exc.returncode}"
        ) from exc

    if mirror_manifest is None:
        yield WORKER_DIR / "dist"
        return
    with tempfile.TemporaryDirectory(prefix="forkmesh-bootstrap-assets-") as temp:
        destination = Path(temp) / "public"
        shutil.copytree(
            WORKER_DIR / "dist",
            destination,
            copy_function=shutil.copy2,
        )
        (destination / MIRROR_MANIFEST_ASSET).write_text(
            json.dumps(mirror_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        yield destination


@contextmanager
def self_host_asset_copy(
    source: Path, app_origin: str, world_origin: str
) -> Iterable[Path]:
    with tempfile.TemporaryDirectory(prefix="forkmesh-self-host-assets-") as temp:
        destination = Path(temp) / "public"
        shutil.copytree(source, destination, copy_function=shutil.copy2)
        replacements = (
            ("wss://app.forkmesh.com", "wss" + app_origin[5:]),
            ("https://world.forkmesh.com", world_origin),
            ("https://app.forkmesh.com", app_origin),
            ("https://app.forkmesh.com", app_origin),
            ("https://forkmesh.com", app_origin),
            ("https://forkmesh.com", app_origin),
        )
        for path in destination.rglob("*"):
            if not path.is_file() or path.suffix not in {
                ".css", ".html", ".js", ".json", ".sh", ".webmanifest"
            }:
                continue
            text = path.read_text(encoding="utf-8")
            for old, new in replacements:
                text = text.replace(old, new)
            path.write_text(text, encoding="utf-8")
        yield destination


def normalize_hostname(value: str) -> str:
    host = value.strip().rstrip(".").lower()
    if not host or "://" in host or "/" in host or "*" in host:
        raise BootstrapError("hostname must be a plain DNS name without a URL path")
    labels = host.split(".")
    if len(labels) < 2:
        raise BootstrapError("hostname must contain a DNS suffix")
    try:
        ascii_host = host.encode("idna").decode("ascii")
    except UnicodeError as exc:
        raise BootstrapError("hostname is not valid IDNA") from exc
    if len(ascii_host) > 253 or any(
        not label
        or len(label) > 63
        or label.startswith("-")
        or label.endswith("-")
        or not re.fullmatch(r"[a-z0-9-]+", label)
        for label in ascii_host.split(".")
    ):
        raise BootstrapError("hostname contains an invalid DNS label")
    return ascii_host


def validate_resource_name(value: str, label: str) -> str:
    cleaned = value.strip().lower()
    if not NAME_RE.fullmatch(cleaned):
        raise BootstrapError(
            f"{label} must start with a letter and contain only lowercase "
            "letters, digits, or hyphens (maximum 63 characters)"
        )
    return cleaned


def sibling_worker_name(worker_name: str, suffix: str) -> str:
    candidate = worker_name + suffix
    if len(candidate) <= 63:
        return candidate
    digest = hashlib.sha256(candidate.encode("ascii")).hexdigest()[:10]
    return worker_name[:51].rstrip("-") + "-" + digest


def render_wrangler_config(
    source: str,
    *,
    worker_name: str,
    database_name: str,
    database_id: str,
    namespace_id: str,
    public_base_url: str,
    node_name: str,
    relay_label: str,
    main_relay_url: str,
    world_origin: str,
    build_rev: str,
    app_version: str,
    deploy_fingerprint: str,
    assets_directory: str | None = None,
) -> str:
    """Render a temporary production config without mutating wrangler.toml."""

    lines = source.splitlines()
    output: list[str] = []
    section = ""
    production_d1_bindings: set[str] = set()
    current_d1_binding = ""
    production_kv_seen = False
    vars_seen = False
    vars_values = {
        "NODE_NAME": node_name,
        "PUBLIC_BASE_URL": public_base_url,
        "API_ORIGIN": public_base_url,
        "APP_ORIGIN": public_base_url,
        "WWW_ORIGIN": public_base_url,
        "WORLD_ORIGIN": world_origin,
        "CORS_ALLOWED_ORIGINS": ",".join(
            dict.fromkeys(
                origin for origin in (public_base_url, world_origin) if origin
            )
        ),
        "SINGLE_WORKER_SITE": "true",
        "DISCORD_OAUTH_REDIRECT_URI": (
            public_base_url.rstrip("/")
            + "/api/integrations/discord/callback"
        ),
        "RELAY_LABEL": relay_label,
        "MAIN_RELAY_URL": main_relay_url,
        "BUILD_REV": build_rev,
        "APP_VERSION": app_version,
        "DEPLOY_FINGERPRINT": deploy_fingerprint,
    }
    vars_written: set[str] = set()

    def flush_missing_vars() -> None:
        if section != "vars":
            return
        for key, value in vars_values.items():
            if key not in vars_written:
                output.append(f"{key} = {_toml_string(value)}")
                vars_written.add(key)

    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            flush_missing_vars()
            section = stripped.strip("[]")
            if stripped == "[[routes]]":
                section = "routes"
                continue
            if stripped == "[[d1_databases]]":
                section = "d1_databases"
                current_d1_binding = ""
            elif stripped == "[[kv_namespaces]]":
                section = "kv_namespaces"
                production_kv_seen = True
            elif stripped == "[vars]":
                section = "vars"
                vars_seen = True
            output.append(line)
            continue

        if section == "routes":
            continue

        if index == 0 and re.match(r"^\s*name\s*=", line):
            output.append(f"name = {_toml_string(worker_name)}")
            output.append("workers_dev = false")
            continue
        if not section and re.match(r"^\s*workers_dev\s*=", line):
            continue
        if (
            section == "assets"
            and assets_directory
            and re.match(r"^\s*directory\s*=", line)
        ):
            output.append(f"directory = {_toml_string(assets_directory)}")
            continue
        if section == "assets" and stripped == '"/",':
            continue
        if section == "vars":
            match = re.match(r"^(\s*)([A-Z][A-Z0-9_]*)\s*=", line)
            if match and match.group(2) in vars_values:
                key = match.group(2)
                output.append(f"{match.group(1)}{key} = {_toml_string(vars_values[key])}")
                vars_written.add(key)
                continue
        if section == "d1_databases":
            binding_match = re.match(
                r'^\s*binding\s*=\s*"([A-Z][A-Z0-9_]*)"\s*$', line
            )
            if binding_match:
                current_d1_binding = binding_match.group(1)
                if (
                    current_d1_binding != "DB"
                    or current_d1_binding in production_d1_bindings
                ):
                    raise BootstrapError(
                        "wrangler template has an unknown or duplicate "
                        "production D1 binding"
                    )
                production_d1_bindings.add(current_d1_binding)
                output.append(line)
                continue
            if not current_d1_binding and re.match(
                r"^\s*(database_name|database_id)\s*=", line
            ):
                raise BootstrapError(
                    "wrangler template D1 binding must precede its database fields"
                )
            if re.match(r"^\s*database_name\s*=", line):
                output.append(f'database_name = {_toml_string(database_name)}')
                continue
            if re.match(r"^\s*database_id\s*=", line):
                output.append(f'database_id = {_toml_string(database_id)}')
                continue
        if section == "kv_namespaces" and production_kv_seen:
            if re.match(r"^\s*id\s*=", line):
                output.append(f'id = {_toml_string(namespace_id)}')
                continue
        output.append(line)

    flush_missing_vars()
    if production_d1_bindings != {"DB"}:
        raise BootstrapError("wrangler template must bind the App DB exactly once")
    if not vars_seen:
        raise BootstrapError("wrangler template has no [vars] section")
    if not production_kv_seen:
        raise BootstrapError("wrangler template has no production KV binding")
    rendered = "\n".join(output) + "\n"
    if "CLOUDFLARE_API_TOKEN" in rendered:
        raise BootstrapError("refusing to render an API token into Wrangler config")
    return rendered


def render_world_wrangler_config(
    *, worker_name: str, app_worker_name: str, app_origin: str,
    assets_directory: Path
) -> str:
    return "\n".join(
        (
            f"name = {_toml_string(worker_name)}",
            f"main = {_toml_string(str(WORLD_DIR / 'worker.js'))}",
            'compatibility_date = "2026-09-22"',
            "workers_dev = false",
            "",
            "[assets]",
            f"directory = {_toml_string(str(assets_directory))}",
            'binding = "ASSETS"',
            'html_handling = "none"',
            'not_found_handling = "404-page"',
            "run_worker_first = [",
            '  "/",',
            '  "/api",',
            '  "/api/*",',
            '  "/world",',
            '  "/world/",',
            '  "/chat",',
            '  "/dashboard",',
            '  "/dashboard/*",',
            '  "/forgot-password",',
            '  "/ledger",',
            '  "/leaderboards",',
            '  "/login",',
            '  "/mirror-payouts",',
            '  "/network",',
            '  "/notes/*",',
            '  "/referral",',
            '  "/referrals",',
            '  "/reset-password",',
            '  "/signup",',
            '  "/status",',
            '  "/about",',
            '  "/blog",',
            '  "/blog/*",',
            '  "/careers",',
            '  "/changelog",',
            '  "/desktop",',
            '  "/mobile",',
            '  "/docs",',
            '  "/docs/*",',
            '  "/features",',
            '  "/market",',
            '  "/market/*",',
            '  "!/market/previews/*",',
            '  "/ecosystem",',
            '  "/ecosystem/*",',
            '  "/press",',
            '  "/pricing",',
            '  "/privacy",',
            '  "/security-report",',
            '  "/terms",',
            '  "/account-events.js",',
            '  "/api-client.js",',
            '  "/chat-apps.js",',
            '  "/chat-attachments.js",',
            '  "/chat-crypto.js",',
            '  "/chat-issue-filing.js",',
            '  "/chat-mlkem.js",',
            '  "/chat-moderation.js",',
            '  "/chat-retention.js",',
            '  "/chat-room-transport.js",',
            '  "/chat-seal.js",',
            '  "/dashboard-chat.js",',
            '  "/member-welcome.js",',
            '  "/analytics.js",',
            '  "/analytics-sdk.js",',
            '  "/assets/*",',
            '  "!/assets/bot-avatars/*",',
            '  "!/assets/world/*",',
            '  "/favicon/*",',
            "]",
            "",
            "[vars]",
            f"APP_ORIGIN = {_toml_string(app_origin)}",
            "",
            "[[services]]",
            'binding = "APP"',
            f"service = {_toml_string(app_worker_name)}",
            "",
            "[observability]",
            "enabled = true",
            "head_sampling_rate = 0.05",
            "",
        )
    )


class CloudflareAPI:
    """Small Cloudflare v4 client with bounded retry and token redaction."""

    def __init__(
        self,
        token: str,
        *,
        opener: Callable[..., Any] = urlopen,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        self._token = token
        self._opener = opener
        self._sleep = sleeper

    def __repr__(self) -> str:  # pragma: no cover - defensive logging guard
        return "CloudflareAPI(token=<redacted>)"

    def request(
        self,
        method: str,
        path: str,
        *,
        query: dict[str, Any] | None = None,
        body: dict[str, Any] | None = None,
        attempts: int = 4,
    ) -> Any:
        suffix = ""
        if query:
            suffix = "?" + urlencode(
                [(key, value) for key, value in query.items() if value is not None]
            )
        url = API_BASE + path + suffix
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {
            "Authorization": f"Bearer {self._token}",
            "Accept": "application/json",
            "User-Agent": "forkmesh-cloudflare-bootstrap/1",
        }
        if payload is not None:
            headers["Content-Type"] = "application/json"

        for attempt in range(attempts):
            request = Request(url, data=payload, method=method, headers=headers)
            try:
                with self._opener(request, timeout=30) as response:
                    raw = response.read()
                    decoded = json.loads(raw.decode("utf-8")) if raw else {}
                    if isinstance(decoded, dict) and decoded.get("success") is False:
                        raise BootstrapError(
                            self._safe_api_error(decoded).replace(
                                self._token, "<redacted>"
                            )
                        )
                    return decoded.get("result") if isinstance(decoded, dict) else decoded
            except HTTPError as exc:
                retryable = exc.code in RETRYABLE_HTTP
                raw = exc.read().decode("utf-8", "replace")
                if retryable and attempt + 1 < attempts:
                    retry_after = exc.headers.get("Retry-After") if exc.headers else None
                    try:
                        delay = min(15.0, max(0.0, float(retry_after)))
                    except (TypeError, ValueError):
                        delay = min(8.0, 0.5 * (2**attempt))
                    self._sleep(delay)
                    continue
                message = f"Cloudflare API returned HTTP {exc.code}"
                try:
                    decoded = json.loads(raw)
                    message += ": " + self._safe_api_error(decoded)
                except (json.JSONDecodeError, TypeError):
                    pass
                raise BootstrapError(message.replace(self._token, "<redacted>")) from exc
            except URLError as exc:
                if attempt + 1 < attempts:
                    self._sleep(min(8.0, 0.5 * (2**attempt)))
                    continue
                raise BootstrapError(f"Cloudflare API connection failed: {exc.reason}") from exc
        raise BootstrapError("Cloudflare API retry budget exhausted")

    @staticmethod
    def _safe_api_error(payload: Any) -> str:
        errors = payload.get("errors", []) if isinstance(payload, dict) else []
        messages = [
            str(item.get("message", "")).strip()
            for item in errors
            if isinstance(item, dict) and item.get("message")
        ]
        return "; ".join(messages) or "request was rejected"

    def verify_token(self) -> None:
        try:
            result = self.request("GET", "/user/tokens/verify")
        except BootstrapError as error:
            # Account-owned API tokens are valid for the account/zone calls
            # this client makes, but the user-scoped verify endpoint rejects
            # them with HTTP 401 "Invalid API Token". Defer to the account
            # resolution that always follows, which still fails closed.
            if "HTTP 401" in str(error):
                return
            raise
        if not isinstance(result, dict) or result.get("status") != "active":
            raise BootstrapError("Cloudflare API token is not active")

    def resolve_account(self, requested_id: str = "") -> dict[str, Any]:
        if requested_id:
            result = self.request("GET", f"/accounts/{requested_id}")
            if not isinstance(result, dict):
                raise BootstrapError("Cloudflare account lookup returned no account")
            return result
        result = self.request("GET", "/accounts", query={"per_page": 50})
        accounts = result if isinstance(result, list) else []
        if len(accounts) != 1:
            raise BootstrapError(
                "token can access multiple or no accounts; pass --account-id explicitly"
            )
        return accounts[0]

    def resolve_zone(self, account_id: str, zone_name: str) -> dict[str, Any]:
        result = self.request(
            "GET",
            "/zones",
            query={"name": zone_name, "account.id": account_id, "per_page": 50},
        )
        zones = [
            zone
            for zone in (result if isinstance(result, list) else [])
            if isinstance(zone, dict) and zone.get("name") == zone_name
        ]
        if len(zones) != 1:
            raise BootstrapError(
                f"expected one accessible Cloudflare zone named {zone_name!r}"
            )
        return zones[0]

    def list_zones(self, account_id: str) -> list[dict[str, Any]]:
        """Return active zones visible to the scoped token.

        Automatic setup is intentionally fail-closed: it proceeds only when one
        account and one active zone are unambiguous. Operators with broader
        tokens can still select an account and zone in the advanced fields.
        """

        result = self.request(
            "GET",
            "/zones",
            query={
                "account.id": account_id,
                "status": "active",
                "per_page": 50,
            },
        )
        return [
            zone
            for zone in (result if isinstance(result, list) else [])
            if isinstance(zone, dict)
            and zone.get("id")
            and zone.get("name")
            and str(zone.get("status") or "active") == "active"
        ]

    def ensure_d1(self, account_id: str, database_name: str) -> tuple[str, bool]:
        result = self.request(
            "GET",
            f"/accounts/{account_id}/d1/database",
            query={"name": database_name, "per_page": 100},
        )
        for database in result if isinstance(result, list) else []:
            if isinstance(database, dict) and database.get("name") == database_name:
                database_id = str(database.get("uuid") or database.get("id") or "")
                if database_id:
                    return database_id, False
        created = self.request(
            "POST",
            f"/accounts/{account_id}/d1/database",
            body={"name": database_name},
        )
        database_id = str(
            (created or {}).get("uuid") or (created or {}).get("id") or ""
        )
        if not database_id:
            raise BootstrapError("Cloudflare created D1 without returning its id")
        return database_id, True

    def ensure_kv_namespace(
        self, account_id: str, namespace_name: str
    ) -> tuple[str, bool]:
        path = f"/accounts/{account_id}/storage/kv/namespaces"
        result = self.request("GET", path, query={"per_page": 100})
        for namespace in result if isinstance(result, list) else []:
            if (
                isinstance(namespace, dict)
                and namespace.get("title") == namespace_name
            ):
                namespace_id = str(namespace.get("id") or "")
                if namespace_id:
                    return namespace_id, False
        created = self.request("POST", path, body={"title": namespace_name})
        namespace_id = str((created or {}).get("id") or "")
        if not namespace_id:
            raise BootstrapError(
                "Cloudflare created a KV namespace without returning its id"
            )
        return namespace_id, True

    def worker_secret_names(self, account_id: str, worker_name: str) -> set[str]:
        try:
            result = self.request(
                "GET",
                f"/accounts/{account_id}/workers/scripts/{worker_name}/secrets",
            )
        except BootstrapError as error:
            if "HTTP 404" in str(error):
                return set()
            raise
        return {
            str(item.get("name") or "")
            for item in result if isinstance(item, dict) and item.get("name")
        } if isinstance(result, list) else set()

    def worker_exists(self, account_id: str, worker_name: str) -> bool:
        """Check for a deployed Worker without downloading or changing it."""

        result = self.request("GET", f"/accounts/{account_id}/workers/scripts")
        if not isinstance(result, list):
            raise BootstrapError("Cloudflare Worker lookup returned an invalid list")
        return any(
            isinstance(item, dict) and str(item.get("id") or "") == worker_name
            for item in result
        )

    def worker_routes(self, zone_id: str) -> list[dict[str, Any]]:
        """Return Worker route ownership for identity preflight."""

        result = self.request("GET", f"/zones/{zone_id}/workers/routes")
        if not isinstance(result, list) or any(
            not isinstance(item, dict) for item in result
        ):
            raise BootstrapError("Cloudflare Worker route lookup returned invalid data")
        return result

    def dns_records(self, zone_id: str, hostname: str) -> list[dict[str, Any]]:
        """Return exact-name DNS records for identity ownership preflight."""

        result = self.request(
            "GET",
            f"/zones/{zone_id}/dns_records",
            query={"name": hostname, "per_page": 100},
        )
        if not isinstance(result, list) or any(
            not isinstance(item, dict) for item in result
        ):
            raise BootstrapError("Cloudflare DNS lookup returned invalid data")
        return result

    def ensure_dns(
        self,
        zone_id: str,
        hostname: str,
        *,
        replace: bool,
    ) -> tuple[dict[str, Any], bool]:
        result = self.request(
            "GET",
            f"/zones/{zone_id}/dns_records",
            query={"name": hostname, "per_page": 100},
        )
        records = (
            [item for item in result if isinstance(item, dict)]
            if isinstance(result, list)
            else []
        )
        address_records = [
            item
            for item in records
            if str(item.get("type") or "").upper() in {"A", "AAAA", "CNAME"}
        ]
        if address_records and all(
            item.get("proxied") is True for item in address_records
        ):
            return address_records[0], False
        body = {
            "type": "AAAA",
            "name": hostname,
            # A Workers route only needs an orange-cloud record.  100:: is the
            # IPv6 discard-only prefix and never becomes repository storage.
            "content": "100::",
            "ttl": 1,
            "proxied": True,
            "comment": "ForkMesh relay route; origin traffic is handled by Workers",
        }
        if address_records:
            if len(address_records) != 1 or not replace:
                raise BootstrapError(
                    f"address records already exist for {hostname} but are not "
                    "exclusively proxied; review them or rerun with --replace-dns"
                )
            record_id = str(address_records[0].get("id") or "")
            if not record_id:
                raise BootstrapError("existing DNS record has no id")
            updated = self.request(
                "PUT", f"/zones/{zone_id}/dns_records/{record_id}", body=body
            )
            return updated, True
        created = self.request("POST", f"/zones/{zone_id}/dns_records", body=body)
        return created, True

    def ensure_worker_route(
        self,
        zone_id: str,
        pattern: str,
        worker_name: str,
        *,
        replace: bool,
    ) -> tuple[dict[str, Any], bool]:
        result = self.request("GET", f"/zones/{zone_id}/workers/routes")
        routes = result if isinstance(result, list) else []
        existing = next(
            (
                item
                for item in routes
                if isinstance(item, dict) and item.get("pattern") == pattern
            ),
            None,
        )
        body = {"pattern": pattern, "script": worker_name}
        if existing and existing.get("script") == worker_name:
            return existing, False
        if existing:
            if not replace:
                raise BootstrapError(
                    f"Worker route {pattern} belongs to {existing.get('script')!r}; "
                    "review it or rerun with --replace-route"
                )
            route_id = str(existing.get("id") or "")
            if not route_id:
                raise BootstrapError("existing Worker route has no id")
            updated = self.request(
                "PUT", f"/zones/{zone_id}/workers/routes/{route_id}", body=body
            )
            return updated, True
        created = self.request("POST", f"/zones/{zone_id}/workers/routes", body=body)
        return created, True


class WranglerRunner:
    """Invoke the repository-pinned pywrangler wrapper without a shell token."""

    def __init__(self, worker_dir: Path = WORKER_DIR) -> None:
        self.worker_dir = worker_dir

    def build_assets(self, *, hidden_env_names: Iterable[str] = ()) -> None:
        env = os.environ.copy()
        for name in {
            "CLOUDFLARE_API_TOKEN",
            "CF_API_TOKEN",
            "CLOUDFLARE_TOKEN",
            "CF_TOKEN",
            *hidden_env_names,
        }:
            env.pop(name, None)
        try:
            for arguments in (
                [sys.executable, "tools/build_dashboard_assets.py"],
                [sys.executable, "tools/build_worker_python.py"],
                [
                    sys.executable,
                    str(WORLD_DIR / "tools" / "build_worker_footprint.py"),
                    "--staged",
                ],
            ):
                subprocess.run(
                    arguments,
                    cwd=self.worker_dir,
                    env=env,
                    check=True,
                )
        except subprocess.CalledProcessError as exc:
            raise BootstrapError(
                f"dashboard asset build failed with exit code {exc.returncode}"
            ) from exc

    def build_world_assets(self, *, hidden_env_names: Iterable[str] = ()) -> None:
        env = os.environ.copy()
        for name in {
            "CLOUDFLARE_API_TOKEN",
            "CF_API_TOKEN",
            "CLOUDFLARE_TOKEN",
            "CF_TOKEN",
            *hidden_env_names,
        }:
            env.pop(name, None)
        try:
            subprocess.run(
                [sys.executable, "tools/build_site_assets.py", "world"],
                cwd=self.worker_dir,
                env=env,
                check=True,
            )
        except subprocess.CalledProcessError as exc:
            raise BootstrapError(
                f"World asset build failed with exit code {exc.returncode}"
            ) from exc

    def run(
        self,
        arguments: Iterable[str],
        *,
        token: str,
        account_id: str,
        stdin_text: str | None = None,
        hidden_env_names: Iterable[str] = (),
    ) -> None:
        env = os.environ.copy()
        for name in hidden_env_names:
            env.pop(name, None)
        env["CLOUDFLARE_API_TOKEN"] = token
        env["CLOUDFLARE_ACCOUNT_ID"] = account_id
        command = [
            "bash",
            "-c",
            '. ./pywrangler.sh; pywrangler "$@"',
            "forkmesh-bootstrap",
            *list(arguments),
        ]
        try:
            subprocess.run(
                command,
                cwd=self.worker_dir,
                env=env,
                input=stdin_text,
                text=True,
                check=True,
            )
        except subprocess.CalledProcessError as exc:
            raise BootstrapError(
                f"Wrangler command failed with exit code {exc.returncode}"
            ) from exc


def _self_host_app_version() -> str:
    """Resolve the exact product semver in source and installed layouts."""

    metadata_path = ROOT / "forkmesh-version.txt"
    cmake_path = ROOT / "desktop" / "CMakeLists.txt"
    try:
        if metadata_path.is_file():
            version = metadata_path.read_text(encoding="utf-8").strip()
        else:
            cmake = cmake_path.read_text(encoding="utf-8")
            match = re.search(
                r"^project\(ForkMesh VERSION ([0-9]+(?:\.[0-9]+){2,3})\b",
                cmake,
                re.MULTILINE,
            )
            version = match.group(1) if match is not None else ""
    except OSError as exc:
        raise BootstrapError("could not determine the ForkMesh App version") from exc
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2,3}", version):
        raise BootstrapError("could not determine the ForkMesh App version")
    return version


def self_host_release_identity() -> dict[str, str]:
    """Return the deterministic identity of the consolidated App bundle."""

    try:
        namespace = runpy.run_path(str(WORKER_DIR / "tools" / "deploy_targets.py"))
        fingerprint = str(namespace["fingerprint"]("app"))
    except (OSError, KeyError, TypeError, ValueError) as exc:
        raise BootstrapError("could not calculate the self-host release identity") from exc
    if not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
        raise BootstrapError("self-host deployment fingerprint is invalid")
    return {
        "buildRev": "selfhost-" + fingerprint[:16],
        "appVersion": _self_host_app_version(),
        "deployFingerprint": fingerprint,
    }


@dataclass(frozen=True)
class BootstrapOptions:
    hostname: str
    zone_name: str
    worker_name: str
    database_name: str
    node_name: str
    relay_label: str
    main_relay_url: str
    account_id: str = ""
    replace_dns: bool = False
    replace_route: bool = False
    dry_run: bool = False
    secret_env: tuple[str, ...] = ()
    data_key_backup: Path | None = None
    router_identity_backup: Path | None = None
    deploy_world: bool = False
    world_hostname: str = ""
    world_worker_name: str = ""
    verify_health: bool = True
    publish_mirror_manifest: bool = True
    mirror_public_key: str = ""
    manifest_signer_command: tuple[str, ...] = ()


def _validate_secret_names(names: Iterable[str]) -> tuple[str, ...]:
    result: list[str] = []
    for raw in names:
        name = raw.strip()
        if not ENV_NAME_RE.fullmatch(name):
            raise BootstrapError(f"invalid secret environment variable name: {raw!r}")
        if name in {"CLOUDFLARE_API_TOKEN", "CLOUDFLARE_ACCOUNT_ID"}:
            raise BootstrapError(f"{name} configures deployment and is not a Worker secret")
        if name not in result:
            result.append(name)
    return tuple(result)


def _data_key_backup_path(path: Path) -> Path:
    candidate = Path(os.path.abspath(path.expanduser()))
    try:
        candidate.resolve(strict=False).relative_to(ROOT.resolve())
    except ValueError:
        return candidate
    raise BootstrapError("the DATA_KEY recovery file must be outside the repository")


def _read_data_key_backup(path: Path) -> str:
    path = _data_key_backup_path(path)
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return ""
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise BootstrapError("the DATA_KEY recovery path must be a regular file")
    if stat.S_IMODE(metadata.st_mode) & 0o077:
        raise BootstrapError("the DATA_KEY recovery file permissions must be 0600")
    if hasattr(os, "getuid") and metadata.st_uid != os.getuid():
        raise BootstrapError("the DATA_KEY recovery file must be owned by this user")
    value = path.read_text(encoding="utf-8").strip()
    if not value:
        raise BootstrapError("the DATA_KEY recovery file is empty")
    return value


def _write_data_key_backup(path: Path, value: str) -> bool:
    path = _data_key_backup_path(path)
    existing = _read_data_key_backup(path)
    if existing:
        if not hmac.compare_digest(existing, value):
            raise BootstrapError(
                "the DATA_KEY recovery file does not match the selected key"
            )
        return False
    parent_existed = path.parent.exists()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not parent_existed:
        path.parent.chmod(0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", closefd=False) as stream:
            stream.write(value + "\n")
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)
    return True


def _router_identity_backup_path(path: Path) -> Path:
    candidate = Path(os.path.abspath(path.expanduser()))
    try:
        candidate.resolve(strict=False).relative_to(ROOT.resolve())
    except ValueError:
        return candidate
    raise BootstrapError(
        "the router identity recovery file must be outside the repository"
    )


def _read_router_identity_backup(path: Path) -> tuple[str, str] | None:
    path = _router_identity_backup_path(path)
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise BootstrapError(
            "the router identity recovery path must be a regular file"
        )
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        raise BootstrapError(
            "the router identity recovery file permissions must be 0600"
        )
    if hasattr(os, "getuid") and metadata.st_uid != os.getuid():
        raise BootstrapError(
            "the router identity recovery file must be owned by this user"
        )
    if metadata.st_size <= 0 or metadata.st_size > 4096:
        raise BootstrapError("the router identity recovery file size is invalid")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BootstrapError("the router identity recovery file is invalid") from exc
    if not isinstance(payload, dict) or payload.get("schemaVersion") != 1:
        raise BootstrapError("the router identity recovery file is invalid")
    public_key = str(payload.get("publicKey") or "")
    signing_seed = str(payload.get("signingSeed") or "")
    try:
        validate_router_identity_pair(public_key, signing_seed)
    except BootstrapError as exc:
        raise BootstrapError(
            "the router identity recovery file contains an invalid key pair"
        ) from exc
    return public_key, signing_seed


def _write_router_identity_backup(
    path: Path, public_key: str, signing_seed: str
) -> bool:
    path = _router_identity_backup_path(path)
    public_key = validate_router_identity_pair(public_key, signing_seed)
    existing = _read_router_identity_backup(path)
    if existing is not None:
        if not (
            hmac.compare_digest(existing[0], public_key)
            and hmac.compare_digest(existing[1], signing_seed)
        ):
            raise BootstrapError(
                "the router identity recovery file does not match the selected key"
            )
        return False
    parent_existed = path.parent.exists()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not parent_existed:
        path.parent.chmod(0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    document = {
        "schemaVersion": 1,
        "publicKey": public_key,
        "signingSeed": signing_seed,
    }
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", closefd=False) as stream:
            stream.write(_canonical_json(document) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)
    return True


def _read_router_public_key(
    hostname: str,
    *,
    opener: Callable[..., Any] = urlopen,
    require_app_worker: bool = False,
) -> str:
    """Read and validate the existing public router identity without redirects."""

    url = f"https://{normalize_hostname(hostname)}{ROUTER_IDENTITY_PATH}"
    request = Request(
        url,
        headers={
            "Accept": "application/json",
            "Cache-Control": "no-cache",
            "Pragma": "no-cache",
            "User-Agent": "forkmesh-cloudflare-bootstrap/1",
        },
    )
    status = 0
    worker_marker = ""
    try:
        with opener(request, timeout=30) as response:
            status = int(getattr(response, "status", 0) or 0)
            if status != 200:
                raise BootstrapError(
                    "existing router identity endpoint did not return HTTP 200"
                )
            final_url = str(getattr(response, "geturl", lambda: url)())
            if final_url != url:
                raise BootstrapError(
                    "existing router identity endpoint redirected unexpectedly"
                )
            headers = getattr(response, "headers", {})
            worker_marker = str(headers.get("x-forkmesh-worker") or "")
            raw = response.read(8193)
    except HTTPError as exc:
        if exc.code != 503 or str(exc.geturl()) != url:
            raise BootstrapError(
                "existing router identity could not be recovered over public "
                "HTTPS; no Cloudflare changes were made"
            ) from exc
        status = 503
        raw = exc.read(8193)
    except BootstrapError:
        raise
    except (URLError, TimeoutError, OSError) as exc:
        raise BootstrapError(
            "existing router identity could not be recovered over public HTTPS; "
            "no Cloudflare changes were made"
        ) from exc
    if len(raw) > 8192:
        raise BootstrapError("existing router identity response is too large")
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BootstrapError(
            "existing router identity response is invalid JSON"
        ) from exc
    if not isinstance(payload, dict) or not (
        payload.get("protocol") == "forkmesh-masked-proxy-v1"
        and payload.get("registration") == "forkmesh-https-endpoint-v1"
        and payload.get("nonCustodial") is True
        and payload.get("repositoryBytesInD1") is False
    ):
        raise BootstrapError("existing router identity response is invalid")
    if status == 503:
        if payload.get("ok") is False and not payload.get("routerPublicKey"):
            raise RouterIdentityNotConfigured(
                "owned App route has no configured public router identity"
            )
        raise BootstrapError("existing router identity response is invalid")
    if payload.get("ok") is not True:
        raise BootstrapError("existing router identity response is invalid")
    if require_app_worker and worker_marker != "app":
        raise BootstrapError(
            "deployed router identity was not served by the expected App Worker"
        )
    try:
        return validate_ed25519_public_key(str(payload.get("routerPublicKey") or ""))
    except BootstrapError as exc:
        raise BootstrapError("existing router public key is invalid") from exc


def _read_backend_router_public_key(
    hostname: str,
    *,
    opener: Callable[..., Any] = urlopen,
) -> str:
    """Read the unoverlaid Python App's proof of its matched router key pair."""

    url = f"https://{normalize_hostname(hostname)}/api/mainnode"
    request = Request(
        url,
        headers={
            "Accept": "application/json",
            "Cache-Control": "no-cache",
            "Pragma": "no-cache",
            "User-Agent": "forkmesh-cloudflare-bootstrap/1",
        },
    )
    try:
        with opener(request, timeout=30) as response:
            if int(getattr(response, "status", 0) or 0) != 200:
                raise BootstrapError(
                    "deployed App router identity proof did not return HTTP 200"
                )
            if str(getattr(response, "geturl", lambda: url)()) != url:
                raise BootstrapError(
                    "deployed App router identity proof redirected unexpectedly"
                )
            raw = response.read(8193)
    except BootstrapError:
        raise
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        raise BootstrapError(
            "deployed App router identity proof could not be read"
        ) from exc
    if len(raw) > 8192:
        raise BootstrapError("deployed App router identity proof is too large")
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BootstrapError(
            "deployed App router identity proof is invalid JSON"
        ) from exc
    if not isinstance(payload, dict) or not (
        payload.get("ok") is True
        and payload.get("runtime") == "python-workers"
        and payload.get("worker") == "app"
        and payload.get("routerIdentityReady") is True
    ):
        raise BootstrapError("deployed App router identity proof is invalid")
    try:
        return validate_ed25519_public_key(
            str(payload.get("routerPublicKey") or "")
        )
    except BootstrapError as exc:
        raise BootstrapError(
            "deployed App router identity public key is invalid"
        ) from exc


def _post_deploy_router_identity_check(
    hostname: str,
    expected_public_key: str,
    attempts: int = 6,
    *,
    backend_reader: Callable[[str], str] = _read_backend_router_public_key,
    app_reader: Callable[[str], str] | None = None,
) -> None:
    """Require the App pair and public App identity to agree exactly."""

    expected_public_key = validate_ed25519_public_key(expected_public_key)
    if app_reader is None:
        def app_reader(host: str) -> str:
            return _read_router_public_key(host, require_app_worker=True)
    for attempt in range(attempts):
        try:
            backend_public_key = backend_reader(hostname)
            app_public_key = app_reader(hostname)
            if hmac.compare_digest(
                backend_public_key, expected_public_key
            ) and hmac.compare_digest(app_public_key, expected_public_key):
                return
        except BootstrapError:
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(
        "deployed App endpoints did not expose the selected router identity"
    )


def _health_check(hostname: str, attempts: int = 6) -> None:
    url = f"https://{hostname}/health"
    for attempt in range(attempts):
        try:
            request = Request(
                url,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                },
            )
            with urlopen(request, timeout=30) as response:
                if 200 <= response.status < 300:
                    return
        except (HTTPError, URLError, TimeoutError):
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(f"deployed relay did not pass its health check at {url}")


def _public_snapshot_readiness_check(
    hostname: str, deploy_fingerprint: str, attempts: int = 40
) -> None:
    """Require all public overlays after routing; allow absent flagship 404."""
    if not re.fullmatch(r"[0-9a-f]{64}", deploy_fingerprint):
        raise BootstrapError("public snapshot readiness fingerprint is invalid")
    for attempt in range(attempts):
        ready = True
        for path in PUBLIC_SNAPSHOT_PATHS:
            status = 0
            headers: Any = {}
            requested_url = f"https://{hostname}{path}"
            final_url = ""
            try:
                request = Request(
                    requested_url,
                    headers={
                        "Accept": "application/json",
                        "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                    },
                )
                with urlopen(request, timeout=30) as response:
                    status = response.status
                    headers = response.headers
                    final_url = response.geturl()
            except HTTPError as error:
                status = error.code
                headers = error.headers
                final_url = error.geturl()
            except (URLError, TimeoutError):
                ready = False
                break
            marker_ok = headers.get("x-forkmesh-worker", "") == "app"
            legacy_edge_absent = not headers.get("x-forkmesh-edge-control", "")
            snapshot_state = headers.get("x-forkmesh-public-snapshot", "")
            if (
                path == "/api/repo/forkmesh/forkmesh/mirrors"
                and status == 404
                and final_url == requested_url
                and marker_ok
                and legacy_edge_absent
                and snapshot_state == "missing-fallback"
            ):
                continue
            if not (
                status == 200
                and final_url == requested_url
                and marker_ok
                and legacy_edge_absent
                and snapshot_state == "fresh"
                and hmac.compare_digest(
                    headers.get("x-forkmesh-snapshot-fingerprint", ""),
                    deploy_fingerprint,
                )
            ):
                ready = False
                break
        if ready:
            bypass_ready = True
            for suffix, extra_headers in (
                ("/api/status?fresh=bootstrap-verification", {}),
                ("/api/status", {
                    "Authorization": "Bearer bootstrap-verification",
                }),
            ):
                requested_url = f"https://{hostname}{suffix}"
                try:
                    request = Request(
                        requested_url,
                        headers={
                            "Accept": "application/json",
                            "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                            **extra_headers,
                        },
                    )
                    with urlopen(request, timeout=30) as response:
                        if (
                            response.status != 200
                            or response.geturl() != requested_url
                            or response.headers.get(
                                "x-forkmesh-public-snapshot", "")
                        ):
                            bypass_ready = False
                            break
                except (HTTPError, URLError, TimeoutError):
                    bypass_ready = False
                    break
            if bypass_ready:
                return
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(
        "deployed App public snapshots did not become current and fresh"
    )


def _functional_readiness_check(
    hostname: str, data_key: str, attempts: int = 6
) -> None:
    url = f"https://{hostname}/api/bootstrap/readiness"
    for attempt in range(attempts):
        timestamp = str(int(time.time() * 1000))
        canonical = "forkmesh-bootstrap-readiness-v1\n" + timestamp
        proof = hmac.new(
            data_key.encode(), canonical.encode(), hashlib.sha256
        ).hexdigest()
        try:
            request = Request(
                url,
                headers={
                    "Accept": "application/json",
                    "Authorization": "Bearer " + proof,
                    "X-ForkMesh-Readiness-Timestamp": timestamp,
                    "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                },
            )
            with urlopen(request, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8") or "{}")
                if 200 <= response.status < 300 and payload.get("ok") is True:
                    return
        except (HTTPError, URLError, TimeoutError, ValueError):
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(
        "deployed App did not pass its authenticated database/encryption check"
    )


def _release_identity_check(
    hostname: str, expected: dict[str, str], attempts: int = 6
) -> None:
    main_url = f"https://{hostname}/api/mainnode"
    version_url = f"https://{hostname}/api/version"
    for attempt in range(attempts):
        try:
            payloads: dict[str, dict[str, Any]] = {}
            app_markers: dict[str, str] = {}
            for label, url in (("main", main_url), ("version", version_url)):
                request = Request(
                    url,
                    headers={
                        "Accept": "application/json",
                        "Cache-Control": "no-cache",
                        "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                    },
                )
                with urlopen(request, timeout=30) as response:
                    if response.status != 200 or str(response.geturl()) != url:
                        raise ValueError("unexpected release identity response")
                    raw = response.read(8193)
                    if len(raw) > 8192:
                        raise ValueError("release identity response is too large")
                    payload = json.loads(raw.decode("utf-8"))
                    if not isinstance(payload, dict):
                        raise ValueError("release identity response is not an object")
                    payloads[label] = payload
                    app_markers[label] = str(
                        response.headers.get("x-forkmesh-worker") or ""
                    )
                    if response.headers.get("x-forkmesh-edge-control"):
                        raise ValueError("legacy edge marker is still active")
            main = payloads["main"]
            version = payloads["version"]
            if (
                main.get("ok") is True
                and main.get("runtime") == "python-workers"
                and main.get("worker") == "app"
                and version.get("ok") is True
                and version.get("worker") == "app"
                and app_markers == {"main": "app", "version": "app"}
                and main.get("rev") == expected["buildRev"]
                and version.get("rev") == expected["buildRev"]
                and version.get("version") == expected["appVersion"]
                and main.get("deployFingerprint")
                == expected["deployFingerprint"]
                and version.get("deployFingerprint")
                == expected["deployFingerprint"]
            ):
                return
        except (
            HTTPError,
            URLError,
            TimeoutError,
            OSError,
            UnicodeDecodeError,
            json.JSONDecodeError,
            ValueError,
        ):
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(
        "deployed App endpoints did not agree on the expected release identity"
    )


def _world_health_check(hostname: str, attempts: int = 6) -> None:
    url = f"https://{hostname}/"
    for attempt in range(attempts):
        try:
            request = Request(
                url,
                headers={"User-Agent": "forkmesh-cloudflare-bootstrap/1"},
            )
            with urlopen(request, timeout=30) as response:
                if (
                    200 <= response.status < 300
                    and response.headers.get("x-forkmesh-worker") == "world"
                ):
                    return
        except (HTTPError, URLError, TimeoutError):
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    raise BootstrapError(f"deployed World did not pass its check at {url}")


def _announce_join(hostname: str, attempts: int = 3) -> str:
    """Launch-time join ping (adhoc #97): ask the fresh instance to register.

    POSTing the new instance's own /api/federation/announce makes it send its
    signed registration to its configured main relay right now, so its request
    to join (and the approve prompt in the operator's desktop) appears the
    moment it launches instead of on the first staggered cron. Best effort:
    returns the reported join status ("pending"/"approved") or "" on failure —
    the instance's scheduled task re-registers later either way.
    """
    url = f"https://{hostname}/api/federation/announce"
    for attempt in range(attempts):
        try:
            request = Request(
                url,
                data=b"{}",
                headers={
                    "Accept": "application/json",
                    "Content-Type": "application/json",
                    "User-Agent": "forkmesh-cloudflare-bootstrap/1",
                },
                method="POST",
            )
            with urlopen(request, timeout=30) as response:
                if 200 <= response.status < 300:
                    payload = json.loads(response.read().decode("utf-8") or "{}")
                    return str(payload.get("status") or "pending")
        except (HTTPError, URLError, TimeoutError, ValueError):
            pass
        if attempt + 1 < attempts:
            time.sleep(min(10, 1 + attempt * 2))
    return ""


def bootstrap(
    options: BootstrapOptions,
    *,
    token: str,
    api: CloudflareAPI | None = None,
    runner: WranglerRunner | None = None,
    manifest_signer: MirrorManifestSigner | None = None,
    asset_stager: Callable[
        [dict[str, Any] | None], Any
    ] = staged_public_assets,
    health_check: Callable[[str], None] = _health_check,
    readiness_check: Callable[[str, str], None] = _functional_readiness_check,
    snapshot_readiness_check: Callable[[str, str], None] | None = None,
    world_health_check: Callable[[str], None] = _world_health_check,
    announce: Callable[[str], str] = _announce_join,
    router_identity_reader: Callable[[str], str] = _read_router_public_key,
    router_identity_check: Callable[[str, str], None] | None = None,
    release_identity_resolver: Callable[[], dict[str, str]] = (
        self_host_release_identity
    ),
    release_identity_check: Callable[[str, dict[str, str]], None] | None = None,
    output: Callable[[str], None] = print,
) -> dict[str, Any]:
    """Execute one idempotent relay bootstrap and return a redacted summary."""

    hostname = normalize_hostname(options.hostname)
    zone_name = normalize_hostname(options.zone_name)
    if hostname != zone_name and not hostname.endswith("." + zone_name):
        raise BootstrapError(f"{hostname} is not inside Cloudflare zone {zone_name}")
    worker_name = validate_resource_name(options.worker_name, "worker name")
    database_name = validate_resource_name(options.database_name, "database name")
    node_name = validate_resource_name(options.node_name, "node name")
    world_hostname = normalize_hostname(
        options.world_hostname or f"forkmesh-world.{zone_name}"
    )
    if world_hostname != zone_name and not world_hostname.endswith("." + zone_name):
        raise BootstrapError(
            f"World hostname {world_hostname} is not inside Cloudflare zone {zone_name}"
        )
    default_world_name = sibling_worker_name(worker_name, "-world")
    world_worker_name = validate_resource_name(
        options.world_worker_name or default_world_name, "World worker name"
    )
    if worker_name == world_worker_name:
        raise BootstrapError("App and World worker names must be different")
    secret_names = _validate_secret_names(options.secret_env)
    secret_values: dict[str, str] = {}
    for name in secret_names:
        value = os.environ.get(name, "")
        if not value:
            raise BootstrapError(
                f"--secret-env {name} was requested but {name} is unset or empty"
            )
        secret_values[name] = value
    local_router_public_key = secret_values.get(ROUTER_PUBLIC_KEY_ENV, "")
    local_router_signing_seed = secret_values.get(ROUTER_SIGNING_SEED_ENV, "")
    if bool(local_router_public_key) != bool(local_router_signing_seed):
        raise BootstrapError(
            "a local router identity requires both MIRROR_ROUTER_PUBLIC_KEY "
            "and MIRROR_ROUTER_SIGNING_SEED"
        )
    if local_router_public_key:
        local_router_public_key = validate_router_identity_pair(
            local_router_public_key, local_router_signing_seed
        )
    router_identity_backup = (
        _router_identity_backup_path(options.router_identity_backup)
        if options.router_identity_backup is not None
        else None
    )
    if not token.strip():
        raise BootstrapError("Cloudflare API token is empty")
    if options.main_relay_url and not options.main_relay_url.startswith("https://"):
        raise BootstrapError("--main-relay-url must use https://")
    relay_label = options.relay_label.strip() or node_name
    if len(relay_label) > 80 or any(ord(character) < 32 for character in relay_label):
        raise BootstrapError("relay label must be at most 80 printable characters")

    api = api or CloudflareAPI(token)
    runner = runner or WranglerRunner()
    api.verify_token()
    account = api.resolve_account(options.account_id)
    account_id = str(account.get("id") or "")
    if not account_id:
        raise BootstrapError("Cloudflare account has no id")
    zone = api.resolve_zone(account_id, zone_name)
    zone_id = str(zone.get("id") or "")
    if not zone_id:
        raise BootstrapError("Cloudflare zone has no id")

    existing_worker = api.worker_exists(account_id, worker_name)
    routes = api.worker_routes(zone_id)
    hostname_dns_records = api.dns_records(zone_id, hostname)

    if any(
        str(item.get("name") or "").rstrip(".").lower() != hostname
        for item in hostname_dns_records
    ):
        raise BootstrapError(
            "Cloudflare returned a DNS record outside the requested hostname"
        )
    hostname_address_records = [
        item
        for item in hostname_dns_records
        if str(item.get("type") or "").upper() in {"A", "AAAA", "CNAME"}
    ]
    proxied_dns_present = bool(hostname_address_records) and all(
        item.get("proxied") is True for item in hostname_address_records
    )
    if hostname_address_records and not proxied_dns_present and (
        len(hostname_address_records) != 1 or not options.replace_dns
    ):
        raise BootstrapError(
            "the hostname DNS is not exclusively proxied by Cloudflare; "
            "refusing to mutate Cloudflare state"
        )

    def route_owner(pattern: str) -> tuple[bool, str]:
        matches = [item for item in routes if str(item.get("pattern") or "") == pattern]
        if len(matches) > 1:
            raise BootstrapError("Cloudflare returned duplicate Worker routes")
        if not matches:
            return False, ""
        return True, str(matches[0].get("script") or "")

    def route_host_matches(route: dict[str, Any]) -> bool:
        pattern = re.sub(
            r"^https?://",
            "",
            str(route.get("pattern") or "").strip(),
            flags=re.IGNORECASE,
        )
        host_pattern = pattern.split("/", 1)[0]
        expression = "".join(
            ".*" if part == "*" else re.escape(part)
            for part in re.split(r"(\*)", host_pattern)
        )
        return re.fullmatch(expression, hostname, re.IGNORECASE) is not None

    broad_route_present, broad_route_owner = route_owner(f"{hostname}/*")

    def expected_hostname_route(item: dict[str, Any]) -> bool:
        pattern = re.sub(
            r"^https?://",
            "",
            str(item.get("pattern") or "").strip(),
            flags=re.IGNORECASE,
        ).rstrip("/")
        script = str(item.get("script") or "")
        if item.get("custom_domain") is True:
            return pattern.lower() == hostname and script == worker_name
        return pattern.lower() == f"{hostname}/*" and script == worker_name

    hostname_matching_routes = [item for item in routes if route_host_matches(item)]
    if any(
        not expected_hostname_route(item) for item in hostname_matching_routes
    ):
        raise BootstrapError(
            "a legacy or unexpected Worker route overlaps the selected App "
            "hostname; remove it in a separately authorized control-plane "
            "cutover before running bootstrap"
        )
    custom_app_routes = [
        item
        for item in routes
        if item.get("custom_domain") is True
        and str(item.get("pattern") or "").rstrip("/").lower() == hostname
    ]
    if len(custom_app_routes) > 1:
        raise BootstrapError("Cloudflare returned duplicate Worker custom domains")
    custom_app_route_owned = bool(custom_app_routes) and str(
        custom_app_routes[0].get("script") or ""
    ) == worker_name
    app_host_route_owned = (
        broad_route_present and broad_route_owner == worker_name
    ) or custom_app_route_owned
    recovered_router_identity = (
        _read_router_identity_backup(router_identity_backup)
        if router_identity_backup is not None
        else None
    )
    if recovered_router_identity is not None and local_router_public_key:
        if not (
            hmac.compare_digest(
                recovered_router_identity[0], local_router_public_key
            )
            and hmac.compare_digest(
                recovered_router_identity[1], local_router_signing_seed
            )
        ):
            raise BootstrapError(
                "the local router identity does not match its recovery file"
            )

    router_identity_backup_created = False
    router_identity_source = ""
    stage_router_secrets = False
    if existing_worker:
        live_route_owned = app_host_route_owned and proxied_dns_present
        partial_install = not hostname_matching_routes
        if live_route_owned:
            try:
                existing_router_public_key = router_identity_reader(hostname)
            except RouterIdentityNotConfigured as exc:
                if recovered_router_identity is None:
                    raise BootstrapError(
                        "the existing router identity is unrecoverable; refusing "
                        "to mutate Cloudflare state"
                    ) from exc
                router_public_key, local_router_signing_seed = (
                    recovered_router_identity
                )
                local_router_public_key = router_public_key
                router_identity_source = "partial-install-recovery"
                stage_router_secrets = not options.dry_run
            except BootstrapError as exc:
                raise BootstrapError(
                    "the existing router identity is unrecoverable; refusing to "
                    "mutate Cloudflare state"
                ) from exc
            else:
                for candidate in (
                    local_router_public_key,
                    recovered_router_identity[0]
                    if recovered_router_identity is not None
                    else "",
                ):
                    if candidate and not hmac.compare_digest(
                        candidate, existing_router_public_key
                    ):
                        raise BootstrapError(
                            "the recovered router identity does not match the "
                            "existing public router key; refusing to rotate it "
                            "during bootstrap"
                        )
                router_public_key = existing_router_public_key
                router_identity_source = "existing-public-endpoint"
                if (
                    local_router_public_key
                    and router_identity_backup is not None
                    and recovered_router_identity is None
                    and not options.dry_run
                ):
                    router_identity_backup_created = _write_router_identity_backup(
                        router_identity_backup,
                        local_router_public_key,
                        local_router_signing_seed,
                    )
        elif partial_install:
            if recovered_router_identity is None:
                raise BootstrapError(
                    "the partial first install has no valid local router identity "
                    "recovery file; refusing to mutate Cloudflare state"
                )
            if (
                not proxied_dns_present
                and ROUTER_PUBLIC_KEY_ENV
                in api.worker_secret_names(account_id, worker_name)
            ):
                raise BootstrapError(
                    "the unproxied hostname cannot prove the existing public "
                    "router identity; refusing to replace it from recovery"
                )
            router_public_key, local_router_signing_seed = recovered_router_identity
            local_router_public_key = router_public_key
            router_identity_source = "partial-install-recovery"
            stage_router_secrets = not options.dry_run
        elif not proxied_dns_present and app_host_route_owned:
            raise BootstrapError(
                "the hostname lacks exclusively proxied Cloudflare DNS, so its "
                "public router identity cannot be trusted"
            )
        else:
            raise BootstrapError(
                "the requested App Worker does not own the hostname's current "
                "router routes; refusing to mutate Cloudflare state"
            )
    else:
        if hostname_matching_routes:
            raise BootstrapError(
                "the hostname already has Worker routes not owned by this new "
                "App; refusing to mutate Cloudflare state"
            )
        if not local_router_public_key and recovered_router_identity is not None:
            local_router_public_key, local_router_signing_seed = (
                recovered_router_identity
            )
            router_identity_source = "local-recovery-file"
        if not local_router_public_key:
            if options.dry_run:
                router_public_key = ""
                router_identity_source = "generated-on-apply"
            else:
                if router_identity_backup is None:
                    raise BootstrapError(
                        "a new instance requires an out-of-repository router "
                        "identity recovery path"
                    )
                local_router_signing_seed = base64.urlsafe_b64encode(
                    os.urandom(32)
                ).decode("ascii").rstrip("=")
                local_router_public_key = derive_ed25519_public_key(
                    local_router_signing_seed
                )
                router_public_key = local_router_public_key
                router_identity_source = "generated-local-pair"
        else:
            router_public_key = local_router_public_key
            if router_identity_source != "local-recovery-file":
                router_identity_source = "local-validated-pair"
        if not options.dry_run:
            if router_identity_backup is None:
                raise BootstrapError(
                    "a new instance requires an out-of-repository router "
                    "identity recovery path"
                )
            router_identity_backup_created = _write_router_identity_backup(
                router_identity_backup,
                local_router_public_key,
                local_router_signing_seed,
            )
            if router_identity_backup_created:
                output(
                    "Stored the router identity recovery file at "
                    f"{router_identity_backup} with mode 0600."
                )
            stage_router_secrets = True

    if stage_router_secrets:
        secret_values[ROUTER_SIGNING_SEED_ENV] = local_router_signing_seed
        secret_values[ROUTER_PUBLIC_KEY_ENV] = local_router_public_key
        for name in ROUTER_IDENTITY_ENV_NAMES:
            if name not in secret_names:
                secret_names = (*secret_names, name)

    release_identity = release_identity_resolver()
    if not isinstance(release_identity, dict) or not (
        re.fullmatch(r"[A-Za-z0-9._-]{1,80}", release_identity.get("buildRev", ""))
        and re.fullmatch(
            r"[0-9]+(?:\.[0-9]+){2,3}", release_identity.get("appVersion", "")
        )
        and re.fullmatch(
            r"[0-9a-f]{64}", release_identity.get("deployFingerprint", "")
        )
    ):
        raise BootstrapError("self-host release identity is invalid")

    if options.dry_run:
        output(
            f"Dry run: would deploy {worker_name} with D1 {database_name}, "
            f"proxied DNS {hostname}, and one App route at {hostname}/*."
        )
        return {
            "ok": True,
            "dryRun": True,
            "accountId": account_id,
            "zoneId": zone_id,
            "hostname": hostname,
            "workerName": worker_name,
            "routerIdentity": {
                "routerIdentitySource": router_identity_source,
                "routerIdentityBackupCreated": False,
            },
            "databaseName": database_name,
            "releaseIdentity": dict(release_identity),
            "world": {
                "enabled": options.deploy_world,
                "hostname": world_hostname,
                "workerName": world_worker_name,
            },
            "secrets": list(secret_names),
            "mirrorManifest": {
                "published": options.publish_mirror_manifest,
                "path": MIRROR_MANIFEST_PATH
                if options.publish_mirror_manifest
                else None,
            },
        }

    mirror_manifest: dict[str, Any] | None = None
    if options.publish_mirror_manifest:
        public_key = validate_ed25519_public_key(options.mirror_public_key)
        signer = manifest_signer or MirrorManifestSigner(
            options.manifest_signer_command
        )
        mirror_manifest = signer.sign(
            unsigned_mirror_manifest(
                hostname=hostname,
                node_name=node_name,
                public_key=public_key,
            ),
            hidden_env_names=secret_names,
        )

    database_id, database_created = api.ensure_d1(account_id, database_name)
    remote_secret_names = api.worker_secret_names(account_id, worker_name)
    remote_has_data_key = "DATA_KEY" in remote_secret_names
    backup_path = options.data_key_backup
    backup_data_key = _read_data_key_backup(backup_path) if backup_path else ""
    readiness_data_key = secret_values.get("DATA_KEY", "") or backup_data_key
    generated_data_key = False
    data_key_backup_created = False
    if not readiness_data_key:
        if database_created:
            if backup_path is None:
                raise BootstrapError(
                    "a new instance requires a DATA_KEY recovery path; use "
                    "--data-key-backup outside the repository"
                )
            readiness_data_key = base64.urlsafe_b64encode(os.urandom(32)).decode(
                "ascii"
            ).rstrip("=")
            generated_data_key = True
        elif remote_has_data_key:
            raise BootstrapError(
                "the Worker has DATA_KEY but no local recovery copy is available; "
                "restore the key from the operator secret store with --secret-env "
                "DATA_KEY before redeploying"
            )
        else:
            raise BootstrapError(
                "the existing D1 database has no DATA_KEY Worker secret or local "
                "recovery copy; restore it with --secret-env DATA_KEY"
            )
    if backup_path is not None:
        data_key_backup_created = _write_data_key_backup(
            backup_path, readiness_data_key
        )
        if data_key_backup_created:
            output(
                f"Stored the DATA_KEY recovery copy at "
                f"{_data_key_backup_path(backup_path)} with mode 0600."
            )
    if database_created or not remote_has_data_key:
        secret_values["DATA_KEY"] = readiness_data_key
        if "DATA_KEY" not in secret_names:
            secret_names = (*secret_names, "DATA_KEY")
    else:
        secret_values.pop("DATA_KEY", None)
    secret_publish_names = [
        name for name in secret_values if name not in ROUTER_IDENTITY_ENV_NAMES
    ]
    secret_publish_names.extend(
        name for name in ROUTER_IDENTITY_ENV_NAMES if name in secret_values
    )
    namespace_name = _derived_resource_name(hostname) + "-repository-metadata"
    namespace_id, namespace_created = api.ensure_kv_namespace(
        account_id, namespace_name
    )
    app_origin = f"https://{hostname}"
    world_origin = ""
    world_dns: dict[str, Any] | None = None
    world_dns_changed = False
    world_route: dict[str, Any] | None = None
    world_route_changed = False
    if options.deploy_world:
        runner.build_world_assets(hidden_env_names=secret_names)
        rendered_world = render_world_wrangler_config(
            worker_name=world_worker_name,
            app_worker_name=worker_name,
            app_origin=app_origin,
            assets_directory=(WORLD_DIR / "dist").resolve(),
        )
        world_config_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                prefix=".forkmesh-world-bootstrap-",
                suffix=".toml",
                dir=WORKER_DIR,
                delete=False,
            ) as handle:
                handle.write(rendered_world)
                world_config_path = Path(handle.name)
            world_config_path.chmod(0o600)
            runner.run(
                ["deploy", "--config", world_config_path.name, "--env", ""],
                token=token,
                account_id=account_id,
                hidden_env_names=secret_names,
            )
        finally:
            if world_config_path is not None:
                world_config_path.unlink(missing_ok=True)
        world_dns, world_dns_changed = api.ensure_dns(
            zone_id, world_hostname, replace=options.replace_dns
        )
        world_route, world_route_changed = api.ensure_worker_route(
            zone_id,
            f"{world_hostname}/*",
            world_worker_name,
            replace=options.replace_route,
        )
        if options.verify_health:
            world_health_check(world_hostname)
        # Only after the optional World is deployed, routed, and (when enabled)
        # healthy may the App credential-trust its origin.
        world_origin = f"https://{world_hostname}"

    runner.build_assets(hidden_env_names=secret_names)
    with asset_stager(mirror_manifest) as staged_assets:
        with self_host_asset_copy(
            Path(staged_assets), app_origin, world_origin or app_origin
        ) as assets_directory:
            template = WRANGLER_TEMPLATE.read_text(encoding="utf-8")
            rendered = render_wrangler_config(
                template,
                worker_name=worker_name,
                database_name=database_name,
                database_id=database_id,
                namespace_id=namespace_id,
                public_base_url=app_origin,
                node_name=node_name,
                relay_label=relay_label,
                main_relay_url=options.main_relay_url.rstrip("/"),
                world_origin=world_origin,
                build_rev=release_identity["buildRev"],
                app_version=release_identity["appVersion"],
                deploy_fingerprint=release_identity["deployFingerprint"],
                assets_directory=str(Path(assets_directory).resolve()),
            )
            config_path: Path | None = None
            try:
                with tempfile.NamedTemporaryFile(
                    mode="w",
                    encoding="utf-8",
                    prefix=".forkmesh-bootstrap-",
                    suffix=".toml",
                    dir=WORKER_DIR,
                    delete=False,
                ) as handle:
                    handle.write(rendered)
                    config_path = Path(handle.name)
                config_path.chmod(0o600)
                relative_config = config_path.name

                output(f"Applying ForkMesh migrations to D1 {database_name}...")
                runner.run(
                    [
                        "d1",
                        "migrations",
                        "apply",
                        database_name,
                        "--remote",
                        "--config",
                        relative_config,
                        "--env",
                        "",
                    ],
                    token=token,
                    account_id=account_id,
                    hidden_env_names=secret_names,
                )
                output(f"Deploying Worker {worker_name}...")
                runner.run(
                    ["deploy", "--config", relative_config, "--env", ""],
                    token=token,
                    account_id=account_id,
                    hidden_env_names=secret_names,
                )
                for name in secret_publish_names:
                    value = secret_values[name]
                    output(
                        f"Setting Worker secret {name} from the local environment..."
                    )
                    runner.run(
                        [
                            "secret",
                            "put",
                            "--config",
                            relative_config,
                            "--env",
                            "",
                            name,
                        ],
                        token=token,
                        account_id=account_id,
                        stdin_text=value + "\n",
                        hidden_env_names=secret_names,
                    )
            finally:
                if config_path is not None:
                    config_path.unlink(missing_ok=True)

    dns, dns_changed = api.ensure_dns(
        zone_id, hostname, replace=options.replace_dns
    )
    route, route_changed = api.ensure_worker_route(
        zone_id,
        f"{hostname}/*",
        worker_name,
        replace=options.replace_route,
    )
    (snapshot_readiness_check or _public_snapshot_readiness_check)(
        hostname, release_identity["deployFingerprint"]
    )
    if router_identity_check is None:
        _post_deploy_router_identity_check(hostname, router_public_key)
    else:
        router_identity_check(hostname, router_public_key)
    if options.verify_health:
        health_check(hostname)
        if readiness_data_key:
            readiness_check(hostname, readiness_data_key)
    if release_identity_check is None:
        _release_identity_check(hostname, release_identity)
    else:
        release_identity_check(hostname, release_identity)
    join_status = ""
    if options.main_relay_url.strip():
        # The launched instance pings its main relay as a request to join
        # (adhoc #97); the operator approves it from the desktop's red-dot
        # prompt, which links it into the federation and the World.
        join_status = announce(hostname)
        output(
            f"Join request sent to {options.main_relay_url.rstrip('/')} "
            f"(status: {join_status}). Approve it from the main instance to "
            "link this World into the federation."
            if join_status
            else "Launch join ping did not reach the main relay; the "
                 "instance's scheduled task will register it later."
        )

    summary = {
        "ok": True,
        "dryRun": False,
        "hostname": hostname,
        "url": f"https://{hostname}",
        "accountId": account_id,
        "zoneId": zone_id,
        "workerName": worker_name,
        "databaseName": database_name,
        "databaseId": database_id,
        "databaseCreated": database_created,
        "releaseIdentity": dict(release_identity),
        "repositoryMetadataNamespace": namespace_name,
        "repositoryMetadataNamespaceId": namespace_id,
        "repositoryMetadataNamespaceCreated": namespace_created,
        "dnsRecordId": str((dns or {}).get("id") or ""),
        "dnsChanged": dns_changed,
        "routeId": str((route or {}).get("id") or ""),
        "routeChanged": route_changed,
        "routerIdentity": {
            "routerIdentitySource": router_identity_source,
            "routerIdentityBackupCreated": router_identity_backup_created,
        },
        "mainRelayUrl": options.main_relay_url.rstrip("/"),
        "joinRequest": {
            "announced": bool(join_status),
            "status": join_status,
        },
        "workerSecretsSet": list(secret_publish_names),
        "dataKeyGenerated": generated_data_key,
        "dataKeyBackupCreated": data_key_backup_created,
        "dataKeyRecoveryAvailable": bool(readiness_data_key),
        "functionalReadinessVerified": bool(
            options.verify_health and readiness_data_key
        ),
        "world": {
            "enabled": options.deploy_world,
            "hostname": world_hostname,
            "workerName": world_worker_name,
            "dnsRecordId": str((world_dns or {}).get("id") or ""),
            "dnsChanged": world_dns_changed,
            "routeId": str((world_route or {}).get("id") or ""),
            "routeChanged": world_route_changed,
            "healthVerified": bool(options.deploy_world and options.verify_health),
        },
        "mirrorManifest": {
            "published": mirror_manifest is not None,
            "path": MIRROR_MANIFEST_PATH if mirror_manifest is not None else None,
            "url": (
                f"https://{hostname}{MIRROR_MANIFEST_PATH}"
                if mirror_manifest is not None
                else None
            ),
            "schema": "www/docs/mirror-endpoint.schema.json",
            "payloadSha256": (
                mirror_manifest["signature"]["payloadSha256"]
                if mirror_manifest is not None
                else None
            ),
            "repositoryBytesInD1": False,
        },
    }
    output(
        f"ForkMesh relay ready at https://{hostname}. "
        "The Cloudflare API token was not persisted."
    )
    return summary


def _read_token(stdin: Any = sys.stdin) -> str:
    token = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
    if token:
        return token
    if stdin.isatty():
        return getpass.getpass("Cloudflare API token (not stored): ").strip()
    return stdin.readline().strip()


def _derived_resource_name(hostname: str) -> str:
    digest = hashlib.sha256(normalize_hostname(hostname).encode("ascii")).hexdigest()
    return "forkmesh-" + digest[:12]


def discover_deployment_defaults(
    *,
    token: str,
    api: CloudflareAPI | None = None,
    account_id: str = "",
    zone_name: str = "",
    hostname: str = "",
) -> dict[str, str]:
    """Resolve an unambiguous account/zone and safe idempotent hostnames.

    This is the token-only desktop path. It never mutates Cloudflare state and
    never returns the token. A token spanning multiple accounts or zones must be
    narrowed by the operator rather than guessing a deployment target.
    """

    if not token.strip():
        raise BootstrapError("Cloudflare API token is empty")
    client = api or CloudflareAPI(token)
    client.verify_token()
    account = client.resolve_account(account_id)
    resolved_account_id = str(account.get("id") or "")
    if not resolved_account_id:
        raise BootstrapError("Cloudflare account has no id")

    requested_zone = normalize_hostname(zone_name) if zone_name.strip() else ""
    requested_hostname = normalize_hostname(hostname) if hostname.strip() else ""
    if requested_zone:
        zone = client.resolve_zone(resolved_account_id, requested_zone)
    else:
        zones = client.list_zones(resolved_account_id)
        if requested_hostname:
            zones = [
                candidate
                for candidate in zones
                if requested_hostname == normalize_hostname(str(candidate["name"]))
                or requested_hostname.endswith(
                    "." + normalize_hostname(str(candidate["name"]))
                )
            ]
        if len(zones) != 1:
            raise BootstrapError(
                "token can access multiple or no active zones; select a "
                "Cloudflare zone in Advanced settings"
            )
        zone = zones[0]

    resolved_zone_name = normalize_hostname(str(zone.get("name") or ""))
    resolved_zone_id = str(zone.get("id") or "")
    if not resolved_zone_id:
        raise BootstrapError("Cloudflare zone has no id")
    resolved_hostname = (
        requested_hostname or normalize_hostname(f"forkmesh.{resolved_zone_name}")
    )
    if (
        resolved_hostname != resolved_zone_name
        and not resolved_hostname.endswith("." + resolved_zone_name)
    ):
        raise BootstrapError(
            f"{resolved_hostname} is not inside Cloudflare zone "
            f"{resolved_zone_name}"
        )
    direct_hostname = normalize_hostname(f"mirror.{resolved_zone_name}")
    if direct_hostname == resolved_hostname:
        direct_hostname = normalize_hostname(f"mirror-node.{resolved_zone_name}")
    node_name = _derived_resource_name(resolved_hostname)
    return {
        "accountId": resolved_account_id,
        "zoneId": resolved_zone_id,
        "zoneName": resolved_zone_name,
        "hostname": resolved_hostname,
        "directMirrorHostname": direct_hostname,
        "nodeName": node_name,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Create/reuse D1, deploy ForkMesh, and attach proxied DNS plus a "
            "Worker route without saving the Cloudflare API token."
        )
    )
    parser.add_argument(
        "--hostname",
        default="",
        help=(
            "public relay hostname; omitted with --auto-configure to derive "
            "forkmesh.<the single accessible zone>"
        ),
    )
    parser.add_argument(
        "--zone",
        dest="zone_name",
        default="",
        help="Cloudflare zone containing the hostname",
    )
    parser.add_argument(
        "--auto-configure",
        action="store_true",
        help=(
            "discover one unambiguous account and active zone from the scoped "
            "token, then derive relay, mirror, D1, and Worker names"
        ),
    )
    parser.add_argument(
        "--account-id", default=os.environ.get("CLOUDFLARE_ACCOUNT_ID", "")
    )
    parser.add_argument(
        "--worker-name",
        default="",
        help="defaults to a stable name derived from the public hostname",
    )
    parser.add_argument(
        "--database-name",
        default="",
        help="defaults to the derived Worker name",
    )
    parser.add_argument(
        "--node-name",
        default="",
        help="defaults to the derived Worker name",
    )
    parser.add_argument("--relay-label", default="")
    parser.add_argument(
        "--main-relay-url",
        default="https://app.forkmesh.com",
        help="upstream relay mesh URL; pass an empty string for a main relay",
    )
    parser.add_argument("--replace-dns", action="store_true")
    parser.add_argument("--replace-route", action="store_true")
    parser.add_argument(
        "--with-world",
        action="store_true",
        help="also deploy the optional World Worker for this self-hosted App",
    )
    parser.add_argument(
        "--world-hostname",
        default="",
        help="defaults to forkmesh-world.<zone>",
    )
    parser.add_argument(
        "--world-worker-name",
        default="",
        help="defaults to the App Worker name with a -world suffix",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--secret-env",
        action="append",
        default=[],
        metavar="NAME",
        help=(
            "read NAME from the local environment and pipe it to Wrangler secret "
            "put; may be repeated"
        ),
    )
    parser.add_argument(
        "--data-key-backup",
        type=Path,
        default=(
            Path(os.environ["FORKMESH_DATA_KEY_BACKUP"])
            if os.environ.get("FORKMESH_DATA_KEY_BACKUP")
            else None
        ),
        help=(
            "0600 recovery file for DATA_KEY; defaults outside the repository "
            "under the user's ForkMesh config directory"
        ),
    )
    parser.add_argument(
        "--router-identity-backup",
        type=Path,
        default=(
            Path(os.environ["FORKMESH_ROUTER_IDENTITY_BACKUP"])
            if os.environ.get("FORKMESH_ROUTER_IDENTITY_BACKUP")
            else None
        ),
        help=(
            "0600 recovery file for the self-host router Ed25519 pair; defaults "
            "outside the repository under the user's ForkMesh config directory"
        ),
    )
    parser.add_argument(
        "--mirror-public-key",
        default=os.environ.get("FORKMESH_NODE_PUBLIC_KEY", ""),
        help=(
            "base64url Ed25519 node public key; defaults to "
            "FORKMESH_NODE_PUBLIC_KEY"
        ),
    )
    parser.add_argument(
        "--manifest-signer-command",
        default=os.environ.get("FORKMESH_MIRROR_MANIFEST_SIGNER", ""),
        help=(
            "local signer executable and arguments; receives public signing JSON "
            "on stdin and returns a base64url Ed25519 signature"
        ),
    )
    parser.add_argument(
        "--skip-mirror-manifest",
        action="store_true",
        help="deploy a relay only, without advertising it as a trusted HTTPS mirror",
    )
    parser.add_argument("--skip-health-check", action="store_true")
    parser.add_argument(
        "--json-output",
        type=Path,
        help="write a non-secret deployment summary to this path",
    )
    parser.add_argument(
        "--json-stdout",
        action="store_true",
        help=(
            "emit the non-secret result as a FORKMESH_BOOTSTRAP_RESULT line "
            "for the desktop client"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    token = _read_token()
    try:
        api = CloudflareAPI(token)
        discovered: dict[str, str] | None = None
        if args.auto_configure or not args.hostname or not args.zone_name:
            if not args.auto_configure and (not args.hostname or not args.zone_name):
                raise BootstrapError(
                    "--hostname and --zone are required unless --auto-configure "
                    "is used"
                )
            discovered = discover_deployment_defaults(
                token=token,
                api=api,
                account_id=args.account_id,
                zone_name=args.zone_name,
                hostname=args.hostname,
            )
            hostname = discovered["hostname"]
            zone_name = discovered["zoneName"]
            account_id = discovered["accountId"]
        else:
            hostname = args.hostname
            zone_name = args.zone_name
            account_id = args.account_id
        derived_name = _derived_resource_name(hostname)
        signer_command = tuple(shlex.split(args.manifest_signer_command))
        has_mirror_key = bool(args.mirror_public_key.strip())
        has_mirror_signer = bool(signer_command)
        if has_mirror_key != has_mirror_signer:
            raise BootstrapError(
                "trusted mirror publication needs both --mirror-public-key "
                "and --manifest-signer-command"
            )
        publish_mirror_manifest = (
            not args.skip_mirror_manifest
            and has_mirror_key
            and has_mirror_signer
        )
        secret_env_names = list(args.secret_env)
        if any(os.environ.get(name, "") for name in ROUTER_IDENTITY_ENV_NAMES):
            for name in ROUTER_IDENTITY_ENV_NAMES:
                if name not in secret_env_names:
                    secret_env_names.append(name)
        worker_name = args.worker_name or derived_name
        data_key_backup = args.data_key_backup
        if data_key_backup is None:
            data_key_backup = (
                forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT).secrets
                / f"{worker_name}.data-key"
            )
        router_identity_backup = args.router_identity_backup
        if router_identity_backup is None:
            router_identity_backup = (
                forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT).secrets
                / f"{worker_name}.router-identity.json"
            )
        options = BootstrapOptions(
            hostname=hostname,
            zone_name=zone_name,
            worker_name=worker_name,
            database_name=args.database_name or args.worker_name or derived_name,
            node_name=args.node_name or args.worker_name or derived_name,
            relay_label=args.relay_label,
            main_relay_url=args.main_relay_url,
            account_id=account_id,
            replace_dns=args.replace_dns,
            replace_route=args.replace_route,
            dry_run=args.dry_run,
            secret_env=tuple(secret_env_names),
            data_key_backup=data_key_backup,
            router_identity_backup=router_identity_backup,
            deploy_world=args.with_world,
            world_hostname=args.world_hostname,
            world_worker_name=args.world_worker_name,
            verify_health=not args.skip_health_check,
            publish_mirror_manifest=publish_mirror_manifest,
            mirror_public_key=args.mirror_public_key,
            manifest_signer_command=signer_command,
        )
        result = bootstrap(options, token=token, api=api)
        result["autoConfigured"] = discovered is not None
        result["zoneName"] = zone_name
        result["nodeName"] = options.node_name
        result["directMirrorHostname"] = (
            discovered["directMirrorHostname"]
            if discovered is not None
            else normalize_hostname(f"mirror.{zone_name}")
        )
        if args.json_output:
            args.json_output.parent.mkdir(parents=True, exist_ok=True)
            args.json_output.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        if args.json_stdout:
            print(
                "FORKMESH_BOOTSTRAP_RESULT="
                + base64.urlsafe_b64encode(
                    _canonical_json(result).encode("utf-8")
                )
                .decode("ascii")
                .rstrip("=")
            )
        return 0
    except (BootstrapError, OSError, ValueError) as exc:
        safe = str(exc).replace(token, "<redacted>") if token else str(exc)
        print(f"bootstrap failed: {safe}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
