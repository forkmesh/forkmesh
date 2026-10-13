#!/usr/bin/env python3
"""Provision a dedicated mirror's DNS and host-owned HTTPS listener.

The script deliberately does not install or launch a connector.  Cloudflare is
used only as the authoritative DNS API: the resulting A/AAAA record is
unproxied and points at the mirror host itself.  Caddy terminates TLS on the
host and forwards requests to the loopback-only ForkMesh gateway.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


API_ROOT = "https://api.cloudflare.com/client/v4"
MANAGED_MARKER = "# Managed by ForkMesh direct HTTPS bootstrap v1"
DNS_NAME_RE = re.compile(
    r"(?=.{1,253}\Z)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+"
    r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?\Z"
)


class BootstrapError(RuntimeError):
    pass


def normalize_dns_name(value: Any, label: str) -> str:
    name = str(value or "").strip().lower().rstrip(".")
    if not DNS_NAME_RE.fullmatch(name):
        raise BootstrapError(f"{label} is not a valid DNS name")
    return name


def public_address(value: Any) -> ipaddress.IPv4Address | ipaddress.IPv6Address:
    try:
        address = ipaddress.ip_address(str(value or "").strip())
    except ValueError as error:
        raise BootstrapError("address is not an IP address") from error
    if not address.is_global:
        raise BootstrapError("address must be globally routable")
    return address


def _request(token: str, method: str, path: str, payload: Any = None) -> Any:
    body = None
    headers = {
        "Accept": "application/json",
        "Authorization": "Bearer " + token,
        "User-Agent": "ForkMesh-Direct-HTTPS-Bootstrap/1",
    }
    if payload is not None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = Request(API_ROOT + path, data=body, headers=headers, method=method)
    try:
        with urlopen(request, timeout=30) as response:
            raw = response.read(2 * 1024 * 1024 + 1)
    except HTTPError as error:
        raw = error.read(4096)
        raise BootstrapError(
            f"DNS API returned HTTP {error.code}: "
            + raw.decode("utf-8", "replace")[:240]
        ) from error
    except (OSError, URLError) as error:
        raise BootstrapError("DNS API request failed") from error
    if len(raw) > 2 * 1024 * 1024:
        raise BootstrapError("DNS API response is too large")
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BootstrapError("DNS API returned invalid JSON") from error
    if not isinstance(value, dict) or value.get("success") is not True:
        raise BootstrapError("DNS API rejected the request")
    return value.get("result")


def configure_dns(token: str, zone: str, hostname: str, address: Any) -> dict[str, Any]:
    if hostname == zone or not hostname.endswith("." + zone):
        raise BootstrapError("hostname must be below the selected DNS zone")
    zones = _request(token, "GET", "/zones?" + urlencode({"name": zone}))
    if not isinstance(zones, list) or len(zones) != 1:
        raise BootstrapError("DNS zone was not found uniquely")
    zone_id = str((zones[0] or {}).get("id") or "")
    if not re.fullmatch(r"[0-9a-f]{32}", zone_id):
        raise BootstrapError("DNS zone identity is invalid")
    records = _request(
        token,
        "GET",
        f"/zones/{zone_id}/dns_records?" + urlencode({"name": hostname}),
    )
    if not isinstance(records, list) or len(records) > 1:
        raise BootstrapError("mirror hostname must have at most one DNS record")
    record_type = "A" if address.version == 4 else "AAAA"
    desired = {
        "type": record_type,
        "name": hostname,
        "content": str(address),
        "proxied": False,
        "ttl": 60,
        "comment": "ForkMesh independent mirror; host-owned HTTPS",
    }
    previous: dict[str, Any] = {}
    if records:
        previous = records[0] if isinstance(records[0], dict) else {}
        record_id = str(previous.get("id") or "")
        if not re.fullmatch(r"[0-9a-f]{32}", record_id):
            raise BootstrapError("existing DNS record identity is invalid")
        result = _request(
            token, "PUT", f"/zones/{zone_id}/dns_records/{record_id}", desired
        )
    else:
        result = _request(token, "POST", f"/zones/{zone_id}/dns_records", desired)
    if not isinstance(result, dict):
        raise BootstrapError("DNS API returned no record")
    if (
        str(result.get("type") or "") != record_type
        or str(result.get("name") or "").rstrip(".").lower() != hostname
        or str(result.get("content") or "") != str(address)
        or result.get("proxied") is not False
    ):
        raise BootstrapError("DNS API did not persist the direct record")
    old_target = str(previous.get("content") or "").strip().lower()
    return {
        "recordId": str(result.get("id") or ""),
        "recordType": record_type,
        "address": str(address),
        "replacedTunnelCname": old_target.endswith(".cfargotunnel.com"),
    }


def caddy_config(hostname: str, upstream: str) -> str:
    if upstream != "127.0.0.1:8790":
        raise BootstrapError("gateway upstream must be the ForkMesh loopback listener")
    return (
        MANAGED_MARKER
        + "\n{\n\tadmin off\n}\n\n"
        + hostname
        + " {\n"
        + "\tencode zstd gzip\n"
        + "\treverse_proxy "
        + upstream
        + "\n"
        + "\theader {\n"
        + "\t\t-Server\n"
        + "\t\tStrict-Transport-Security \"max-age=31536000; includeSubDomains\"\n"
        + "\t}\n"
        + "}\n"
    )


def write_managed_caddyfile(path: Path, content: str, adopt: bool) -> str:
    if not path.is_absolute():
        raise BootstrapError("Caddyfile path must be absolute")
    path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    backup = ""
    if path.exists() or path.is_symlink():
        if path.is_symlink() or not path.is_file():
            raise BootstrapError("Caddyfile must be a regular file")
        current = path.read_text(encoding="utf-8")
        if not current.startswith(MANAGED_MARKER):
            if not adopt:
                raise BootstrapError("refusing to replace an unmanaged Caddyfile")
            backup_path = path.with_name(path.name + ".before-forkmesh")
            if not backup_path.exists():
                os.replace(path, backup_path)
                backup = str(backup_path)
    descriptor, temporary = tempfile.mkstemp(prefix=".forkmesh-caddy-", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    return backup


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hostname", required=True)
    parser.add_argument("--zone", required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--caddyfile", default="/etc/caddy/Caddyfile")
    parser.add_argument("--gateway-upstream", default="127.0.0.1:8790")
    parser.add_argument("--adopt-dedicated-host", action="store_true")
    parser.add_argument("--skip-dns", action="store_true")
    args = parser.parse_args()
    hostname = normalize_dns_name(args.hostname, "hostname")
    zone = normalize_dns_name(args.zone, "zone")
    address = public_address(args.address)
    dns = {
        "recordType": "A" if address.version == 4 else "AAAA",
        "address": str(address),
        "skipped": True,
    }
    if not args.skip_dns:
        token = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
        if not token:
            raise BootstrapError("CLOUDFLARE_API_TOKEN is required for DNS update")
        dns = configure_dns(token, zone, hostname, address)
    backup = write_managed_caddyfile(
        Path(args.caddyfile),
        caddy_config(hostname, args.gateway_upstream),
        args.adopt_dedicated_host,
    )
    print(json.dumps({"ok": True, "hostname": hostname, "dns": dns, "backup": backup}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BootstrapError as error:
        print("direct-https-bootstrap:", error, file=os.sys.stderr)
        raise SystemExit(2)
