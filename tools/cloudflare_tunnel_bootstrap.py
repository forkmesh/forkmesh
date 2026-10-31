#!/usr/bin/env python3
"""One-click Cloudflare Tunnel exposure for the local mirror gateway.

This is intentionally separate from ``cloudflare_bootstrap.py``: the existing
tool still deploys the ForkMesh Worker, D1 database, migrations, and Worker
route unchanged.  This tool creates/reuses a remotely managed Tunnel, configures
one proxied CNAME and one loopback HTTP ingress, optionally writes a public
signed mirror manifest, and can launch both local processes.

The Cloudflare API token is accepted only through the environment, a hidden
terminal prompt, or stdin.  It is never placed in an argument, output file,
summary, gateway configuration, or child process environment.
"""

from __future__ import annotations

import argparse
import base64
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from cloudflare_bootstrap import (  # noqa: E402
    BootstrapError,
    CloudflareAPI,
    MirrorManifestSigner,
    _read_token,
    normalize_hostname,
    validate_ed25519_public_key,
    validate_resource_name,
)


GATEWAY_SCRIPT = TOOLS / "mirror_gateway.py"


def unsigned_tunnel_manifest(
    *,
    hostname: str,
    node_name: str,
    public_key: str,
    tunnel_id: str,
    generated_at: str | None = None,
) -> dict[str, Any]:
    """Build a truthful public manifest for a Cloudflare Tunnel origin."""

    hostname = normalize_hostname(hostname)
    node_name = validate_resource_name(node_name, "node name")
    public_key = validate_ed25519_public_key(public_key)
    if not tunnel_id or not all(
        character.isalnum() or character == "-" for character in tunnel_id
    ):
        raise BootstrapError("Cloudflare Tunnel id is invalid")
    origin = f"https://{hostname}"
    return {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": generated_at
        or datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "node": {"name": node_name, "publicKey": public_key},
        "endpoint": {
            "origin": origin,
            "healthUrl": origin + "/health",
            "manifestUrl": origin + "/forkmesh-mirror.json",
            "repositoryUrlTemplate": (
                origin
                + "/v1/repositories/{owner}/{repository}/{operation}"
            ),
            "transport": "direct-https",
            "mainProxyMode": "masked",
        },
        "dns": {
            "recordName": hostname,
            "recordType": "CNAME",
            "proxied": True,
            "target": f"{tunnel_id}.cfargotunnel.com",
        },
        "edge": {
            "provider": "cloudflare",
            "kind": "cloudflare-tunnel",
            "tunnelId": tunnel_id,
            "originExposure": "loopback-only",
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


@dataclass(frozen=True)
class TunnelOptions:
    hostname: str
    zone_name: str
    tunnel_name: str
    node_name: str
    origin_host: str = "127.0.0.1"
    origin_port: int = 8790
    account_id: str = ""
    replace_dns: bool = False
    dry_run: bool = False
    tunnel_token_file: Path | None = None
    launch: bool = False
    gateway_config: Path | None = None
    cloudflared_binary: str = "cloudflared"
    mirror_public_key: str = ""
    manifest_signer_command: tuple[str, ...] = ()
    manifest_output: Path | None = None


class CloudflareTunnelAPI:
    """Idempotent Tunnel/DNS adapter over the shared redacting API client."""

    def __init__(self, client: CloudflareAPI) -> None:
        self.client = client

    def verify_token(self) -> None:
        self.client.verify_token()

    def resolve_account(self, requested_id: str = "") -> dict[str, Any]:
        return self.client.resolve_account(requested_id)

    def resolve_zone(self, account_id: str, zone_name: str) -> dict[str, Any]:
        return self.client.resolve_zone(account_id, zone_name)

    def find_tunnel(
        self, account_id: str, tunnel_name: str
    ) -> dict[str, Any] | None:
        result = self.client.request(
            "GET",
            f"/accounts/{account_id}/cfd_tunnel",
            query={"name": tunnel_name, "is_deleted": "false", "per_page": 100},
        )
        matches = [
            item
            for item in (result if isinstance(result, list) else [])
            if isinstance(item, dict)
            and item.get("name") == tunnel_name
            and not item.get("deleted_at")
        ]
        if len(matches) > 1:
            raise BootstrapError(
                f"multiple active Cloudflare Tunnels are named {tunnel_name!r}"
            )
        return matches[0] if matches else None

    def ensure_tunnel(
        self, account_id: str, tunnel_name: str
    ) -> tuple[dict[str, Any], bool]:
        existing = self.find_tunnel(account_id, tunnel_name)
        if existing is not None:
            if existing.get("config_src") not in (None, "cloudflare"):
                raise BootstrapError(
                    "existing Tunnel is locally managed; refusing to replace its "
                    "configuration"
                )
            return existing, False
        # Cloudflare requires a 32-byte base64 tunnel secret when creating the
        # remote tunnel. It remains in process memory for this one API call and
        # is never returned, stored, or logged.
        tunnel_secret = base64.b64encode(secrets.token_bytes(32)).decode("ascii")
        created = self.client.request(
            "POST",
            f"/accounts/{account_id}/cfd_tunnel",
            body={
                "name": tunnel_name,
                "config_src": "cloudflare",
                "tunnel_secret": tunnel_secret,
            },
        )
        if not isinstance(created, dict) or not created.get("id"):
            raise BootstrapError("Cloudflare created a Tunnel without returning an id")
        return created, True

    @staticmethod
    def desired_configuration(
        hostname: str, origin_host: str, origin_port: int
    ) -> dict[str, Any]:
        return {
            "config": {
                "ingress": [
                    {
                        "hostname": hostname,
                        "service": f"http://{origin_host}:{origin_port}",
                        "originRequest": {
                            "httpHostHeader": hostname,
                            "connectTimeout": 10,
                            "noTLSVerify": False,
                        },
                    },
                    {"service": "http_status:404"},
                ],
                "originRequest": {},
            }
        }

    def ensure_configuration(
        self,
        account_id: str,
        tunnel_id: str,
        hostname: str,
        origin_host: str,
        origin_port: int,
    ) -> bool:
        desired = self.desired_configuration(
            hostname, origin_host, origin_port
        )
        current = self.client.request(
            "GET",
            f"/accounts/{account_id}/cfd_tunnel/{tunnel_id}/configurations",
        )
        current_config = current.get("config") if isinstance(current, dict) else None
        if current_config == desired["config"]:
            return False
        self.client.request(
            "PUT",
            f"/accounts/{account_id}/cfd_tunnel/{tunnel_id}/configurations",
            body=desired,
        )
        return True

    def ensure_dns(
        self,
        zone_id: str,
        hostname: str,
        tunnel_id: str,
        *,
        replace: bool,
    ) -> tuple[dict[str, Any], bool]:
        target = f"{tunnel_id}.cfargotunnel.com"
        result = self.client.request(
            "GET",
            f"/zones/{zone_id}/dns_records",
            query={"name": hostname, "per_page": 100},
        )
        records = [
            item
            for item in (result if isinstance(result, list) else [])
            if isinstance(item, dict)
        ]
        matching = next(
            (
                item
                for item in records
                if item.get("type") == "CNAME"
                and str(item.get("content") or "").rstrip(".").lower()
                == target.lower()
                and item.get("proxied") is True
            ),
            None,
        )
        if matching is not None:
            return matching, False
        body = {
            "type": "CNAME",
            "name": hostname,
            "content": target,
            "ttl": 1,
            "proxied": True,
            "comment": "ForkMesh independent mirror gateway via Cloudflare Tunnel",
        }
        if records:
            if not replace:
                raise BootstrapError(
                    f"DNS records already exist for {hostname}; review them or "
                    "rerun with --replace-dns"
                )
            record_id = str(records[0].get("id") or "")
            if not record_id:
                raise BootstrapError("existing DNS record has no id")
            updated = self.client.request(
                "PUT", f"/zones/{zone_id}/dns_records/{record_id}", body=body
            )
            return updated, True
        created = self.client.request(
            "POST", f"/zones/{zone_id}/dns_records", body=body
        )
        return created, True

    def connector_token(self, account_id: str, tunnel_id: str) -> str:
        value = self.client.request(
            "GET",
            f"/accounts/{account_id}/cfd_tunnel/{tunnel_id}/token",
        )
        if not isinstance(value, str) or len(value) < 40:
            raise BootstrapError("Cloudflare returned an invalid Tunnel token")
        return value


def _write_connector_token(path: Path, token: str) -> None:
    """Write an explicitly requested connector credential with mode 0600."""

    resolved = path.expanduser().resolve()
    resolved.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if resolved.exists():
        try:
            if resolved.read_text(encoding="utf-8").strip() == token:
                os.chmod(resolved, 0o600)
                return
        except OSError as exc:
            raise BootstrapError("existing Tunnel token file is not readable") from exc
        raise BootstrapError(
            "Tunnel token file already exists with different contents; rotate or "
            "remove it explicitly before replacing it"
        )
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(resolved, flags, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(token + "\n")
    except Exception:
        try:
            resolved.unlink(missing_ok=True)
        finally:
            raise
    os.chmod(resolved, 0o600)


def _write_public_manifest(path: Path, manifest: dict[str, Any]) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        prefix=".forkmesh-manifest-",
        suffix=".json",
        dir=path.parent,
        delete=False,
    ) as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
        staged = Path(handle.name)
    try:
        staged.chmod(0o644)
        os.replace(staged, path)
    finally:
        staged.unlink(missing_ok=True)


def _launch_environment() -> dict[str, str]:
    return {
        key: value
        for key, value in os.environ.items()
        if key
        not in {
            "CLOUDFLARE_API_TOKEN",
            "CF_API_TOKEN",
            "CLOUDFLARE_TOKEN",
            "CF_TOKEN",
        }
    }


def launch_services(
    *,
    gateway_config: Path,
    connector_token: str,
    cloudflared_binary: str,
    popen: Callable[..., subprocess.Popen[Any]] = subprocess.Popen,
) -> int:
    """Run the loopback gateway and cloudflared without token command arguments."""

    binary = shutil.which(cloudflared_binary)
    if binary is None:
        raise BootstrapError(
            f"{cloudflared_binary!r} was not found; install cloudflared or use "
            "--tunnel-token-file for a service deployment"
        )
    gateway_environment = _launch_environment()
    tunnel_environment = dict(gateway_environment)
    tunnel_environment["TUNNEL_TOKEN"] = connector_token
    gateway = popen(
        [
            sys.executable,
            str(GATEWAY_SCRIPT),
            "--config",
            str(gateway_config.resolve()),
        ],
        env=gateway_environment,
    )
    tunnel: subprocess.Popen[Any] | None = None
    try:
        # Fail fast if gateway validation/startup exits before cloudflared.
        time.sleep(0.2)
        if gateway.poll() is not None:
            return int(gateway.returncode or 1)
        tunnel = popen(
            [binary, "tunnel", "run"],
            env=tunnel_environment,
        )
        while True:
            gateway_code = gateway.poll()
            tunnel_code = tunnel.poll()
            if gateway_code is not None:
                return int(gateway_code)
            if tunnel_code is not None:
                return int(tunnel_code)
            time.sleep(0.5)
    except KeyboardInterrupt:
        return 130
    finally:
        for process in (tunnel, gateway):
            if process is not None and process.poll() is None:
                process.terminate()
        for process in (tunnel, gateway):
            if process is None or process.poll() is not None:
                continue
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


def provision_tunnel(
    options: TunnelOptions,
    *,
    token: str,
    api: Any | None = None,
    manifest_signer: MirrorManifestSigner | None = None,
    output: Callable[[str], None] = print,
    launch: Callable[..., int] = launch_services,
) -> dict[str, Any]:
    hostname = normalize_hostname(options.hostname)
    zone_name = normalize_hostname(options.zone_name)
    if hostname != zone_name and not hostname.endswith("." + zone_name):
        raise BootstrapError(f"{hostname} is not inside Cloudflare zone {zone_name}")
    tunnel_name = validate_resource_name(options.tunnel_name, "tunnel name")
    node_name = validate_resource_name(options.node_name, "node name")
    if options.origin_host not in ("127.0.0.1", "::1", "localhost"):
        raise BootstrapError("Tunnel origin must be loopback")
    if not 1 <= int(options.origin_port) <= 65535:
        raise BootstrapError("Tunnel origin port is outside 1..65535")
    if not token.strip():
        raise BootstrapError("Cloudflare API token is empty")
    if options.launch and options.gateway_config is None:
        raise BootstrapError("--launch requires --gateway-config")
    if options.launch and options.tunnel_token_file is not None:
        raise BootstrapError(
            "--launch and --tunnel-token-file are alternative credential modes"
        )
    if options.manifest_output is not None:
        validate_ed25519_public_key(options.mirror_public_key)
        if not options.manifest_signer_command and manifest_signer is None:
            raise BootstrapError(
                "--manifest-output requires --manifest-signer-command"
            )

    if api is None:
        api = CloudflareTunnelAPI(CloudflareAPI(token))
    api.verify_token()
    account = api.resolve_account(options.account_id)
    account_id = str(account.get("id") or "")
    if not account_id:
        raise BootstrapError("Cloudflare account has no id")
    zone = api.resolve_zone(account_id, zone_name)
    zone_id = str(zone.get("id") or "")
    if not zone_id:
        raise BootstrapError("Cloudflare zone has no id")

    existing = api.find_tunnel(account_id, tunnel_name)
    if options.dry_run:
        output(
            f"Dry run: would create/reuse Tunnel {tunnel_name}, route proxied "
            f"{hostname} to http://{options.origin_host}:{options.origin_port}, "
            "and keep the Cloudflare API token in memory only."
        )
        return {
            "ok": True,
            "dryRun": True,
            "hostname": hostname,
            "tunnelName": tunnel_name,
            "tunnelExists": existing is not None,
            "origin": f"http://{options.origin_host}:{options.origin_port}",
            "apiTokenPersisted": False,
        }

    tunnel, tunnel_created = api.ensure_tunnel(account_id, tunnel_name)
    tunnel_id = str(tunnel.get("id") or "")
    if not tunnel_id:
        raise BootstrapError("Cloudflare Tunnel has no id")
    configuration_changed = api.ensure_configuration(
        account_id,
        tunnel_id,
        hostname,
        options.origin_host,
        options.origin_port,
    )
    dns, dns_changed = api.ensure_dns(
        zone_id,
        hostname,
        tunnel_id,
        replace=options.replace_dns,
    )

    manifest: dict[str, Any] | None = None
    if options.manifest_output is not None:
        signer = manifest_signer or MirrorManifestSigner(
            options.manifest_signer_command
        )
        manifest = signer.sign(
            unsigned_tunnel_manifest(
                hostname=hostname,
                node_name=node_name,
                public_key=options.mirror_public_key,
                tunnel_id=tunnel_id,
            )
        )
        _write_public_manifest(options.manifest_output, manifest)

    connector_token: str | None = None
    token_file = options.tunnel_token_file
    if options.launch or token_file is not None:
        connector_token = api.connector_token(account_id, tunnel_id)
    if token_file is not None and connector_token is not None:
        _write_connector_token(token_file, connector_token)

    summary = {
        "ok": True,
        "dryRun": False,
        "hostname": hostname,
        "url": f"https://{hostname}",
        "accountId": account_id,
        "zoneId": zone_id,
        "tunnelId": tunnel_id,
        "tunnelName": tunnel_name,
        "tunnelCreated": tunnel_created,
        "configurationChanged": configuration_changed,
        "dnsRecordId": str((dns or {}).get("id") or ""),
        "dnsChanged": dns_changed,
        "origin": f"http://{options.origin_host}:{options.origin_port}",
        "apiTokenPersisted": False,
        "connectorToken": {
            "stored": token_file is not None,
            "path": str(token_file.expanduser().resolve())
            if token_file is not None
            else None,
            "passedInProcessEnvironment": options.launch,
        },
        "manifest": {
            "written": manifest is not None,
            "schema": "docs/mirror-gateway-manifest.schema.json",
            "path": str(options.manifest_output.expanduser().resolve())
            if options.manifest_output is not None
            else None,
            "payloadSha256": (
                manifest["signature"]["payloadSha256"]
                if manifest is not None
                else None
            ),
        },
    }
    output(
        f"Cloudflare Tunnel ready for https://{hostname}. "
        "The Cloudflare API token was not persisted."
    )
    if not options.launch and token_file is None:
        output(
            "Connector not started: rerun with --launch or explicitly choose "
            "--tunnel-token-file for a cloudflared service."
        )
    if options.launch:
        assert connector_token is not None
        summary["launchExitCode"] = launch(
            gateway_config=options.gateway_config,
            connector_token=connector_token,
            cloudflared_binary=options.cloudflared_binary,
        )
    return summary


def _derived_tunnel_name(hostname: str) -> str:
    normalized = normalize_hostname(hostname)
    digest = __import__("hashlib").sha256(normalized.encode("ascii")).hexdigest()
    return "forkmesh-mirror-" + digest[:12]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Create/reuse a Cloudflare Tunnel and proxied DNS for the local "
            "ForkMesh mirror gateway without storing the Cloudflare API token."
        )
    )
    parser.add_argument("--hostname", required=True)
    parser.add_argument("--zone", dest="zone_name", required=True)
    parser.add_argument(
        "--account-id", default=os.environ.get("CLOUDFLARE_ACCOUNT_ID", "")
    )
    parser.add_argument("--tunnel-name", default="")
    parser.add_argument("--node-name", default="")
    parser.add_argument("--origin-host", default="127.0.0.1")
    parser.add_argument("--origin-port", type=int, default=8790)
    parser.add_argument("--replace-dns", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--tunnel-token-file",
        type=Path,
        help=(
            "explicitly persist the Cloudflare connector credential mode 0600; "
            "the Cloudflare API token is still never stored"
        ),
    )
    parser.add_argument(
        "--launch",
        action="store_true",
        help="run the gateway and cloudflared, passing the connector token by env",
    )
    parser.add_argument("--gateway-config", type=Path)
    parser.add_argument("--cloudflared-binary", default="cloudflared")
    parser.add_argument(
        "--mirror-public-key",
        default=os.environ.get("FORKMESH_NODE_PUBLIC_KEY", ""),
    )
    parser.add_argument(
        "--manifest-signer-command",
        default=os.environ.get("FORKMESH_MIRROR_MANIFEST_SIGNER", ""),
    )
    parser.add_argument("--manifest-output", type=Path)
    parser.add_argument("--json-output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    token = _read_token()
    try:
        derived = _derived_tunnel_name(args.hostname)
        result = provision_tunnel(
            TunnelOptions(
                hostname=args.hostname,
                zone_name=args.zone_name,
                tunnel_name=args.tunnel_name or derived,
                node_name=args.node_name or args.tunnel_name or derived,
                origin_host=args.origin_host,
                origin_port=args.origin_port,
                account_id=args.account_id,
                replace_dns=args.replace_dns,
                dry_run=args.dry_run,
                tunnel_token_file=args.tunnel_token_file,
                launch=args.launch,
                gateway_config=args.gateway_config,
                cloudflared_binary=args.cloudflared_binary,
                mirror_public_key=args.mirror_public_key,
                manifest_signer_command=tuple(
                    shlex.split(args.manifest_signer_command)
                ),
                manifest_output=args.manifest_output,
            ),
            token=token,
        )
        if args.json_output:
            args.json_output.parent.mkdir(parents=True, exist_ok=True)
            args.json_output.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        return int(result.get("launchExitCode") or 0)
    except (BootstrapError, OSError, ValueError) as exc:
        safe = str(exc).replace(token, "<redacted>") if token else str(exc)
        print(f"tunnel bootstrap failed: {safe}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
