"""Host-owned HTTPS bootstrap contracts."""

import ipaddress
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import direct_https_bootstrap as direct  # noqa: E402


def test_caddy_config_is_managed_and_loopback_only():
    value = direct.caddy_config("mirror35.example.com", "127.0.0.1:8790")
    assert value.startswith(direct.MANAGED_MARKER)
    assert "mirror35.example.com" in value
    assert "reverse_proxy 127.0.0.1:8790" in value
    assert "cloudflared" not in value.lower()
    with pytest.raises(direct.BootstrapError, match="loopback"):
        direct.caddy_config("mirror35.example.com", "0.0.0.0:8790")


def test_caddyfile_refuses_unmanaged_content_without_dedicated_host_opt_in(tmp_path):
    target = tmp_path / "Caddyfile"
    target.write_text("example.com { respond 200 }\n")
    content = direct.caddy_config("mirror35.example.com", "127.0.0.1:8790")
    with pytest.raises(direct.BootstrapError, match="unmanaged"):
        direct.write_managed_caddyfile(target, content, False)
    backup = direct.write_managed_caddyfile(target, content, True)
    assert Path(backup).read_text() == "example.com { respond 200 }\n"
    assert target.read_text() == content
    assert target.stat().st_mode & 0o777 == 0o644


def test_dns_update_replaces_tunnel_cname_with_unproxied_host_address(monkeypatch):
    calls = []

    def fake_request(_token, method, path, payload=None):
        calls.append((method, path, payload))
        if path.startswith("/zones?"):
            return [{"id": "a" * 32}]
        if path.endswith("dns_records?name=mirror35.example.com"):
            return [{
                "id": "b" * 32,
                "type": "CNAME",
                "name": "mirror35.example.com",
                "content": "11111111-2222-3333-4444-555555555555.cfargotunnel.com",
                "proxied": True,
            }]
        return dict(payload or {}, id="b" * 32)

    monkeypatch.setattr(direct, "_request", fake_request)
    result = direct.configure_dns(
        "secret", "example.com", "mirror35.example.com",
        ipaddress.ip_address("8.8.8.8"),
    )
    assert result["replacedTunnelCname"] is True
    update = calls[-1]
    assert update[0] == "PUT"
    assert update[2]["type"] == "A"
    assert update[2]["content"] == "8.8.8.8"
    assert update[2]["proxied"] is False
    assert "secret" not in json.dumps(calls)


@pytest.mark.parametrize("value", ["127.0.0.1", "10.0.0.1", "192.0.2.4"])
def test_non_public_addresses_fail_closed(value):
    with pytest.raises(direct.BootstrapError, match="globally routable"):
        direct.public_address(value)
