"""Cloudflare Tunnel one-click provisioning without API-token persistence."""

import base64
import json
from pathlib import Path
import stat
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import cloudflare_bootstrap as worker_bootstrap  # noqa: E402
import cloudflare_tunnel_bootstrap as tunnel_bootstrap  # noqa: E402


PUBLIC_KEY = base64.urlsafe_b64encode(b"P" * 32).decode().rstrip("=")
SIGNATURE = base64.urlsafe_b64encode(b"S" * 64).decode().rstrip("=")


class FakeTunnelAPI:
    def __init__(self, *, existing=False):
        self.existing = existing
        self.calls = []

    def verify_token(self):
        self.calls.append(("verify",))

    def resolve_account(self, requested_id=""):
        self.calls.append(("account", requested_id))
        return {"id": requested_id or "account-1"}

    def resolve_zone(self, account_id, zone_name):
        self.calls.append(("zone", account_id, zone_name))
        return {"id": "zone-1", "name": zone_name}

    def find_tunnel(self, account_id, tunnel_name):
        self.calls.append(("find", account_id, tunnel_name))
        if self.existing:
            return {
                "id": "11111111-2222-3333-4444-555555555555",
                "name": tunnel_name,
                "config_src": "cloudflare",
            }
        return None

    def ensure_tunnel(self, account_id, tunnel_name):
        self.calls.append(("tunnel", account_id, tunnel_name))
        return (
            {
                "id": "11111111-2222-3333-4444-555555555555",
                "name": tunnel_name,
                "config_src": "cloudflare",
            },
            not self.existing,
        )

    def ensure_configuration(
        self, account_id, tunnel_id, hostname, origin_host, origin_port
    ):
        self.calls.append(
            (
                "config",
                account_id,
                tunnel_id,
                hostname,
                origin_host,
                origin_port,
            )
        )
        return True

    def ensure_dns(self, zone_id, hostname, tunnel_id, *, replace):
        self.calls.append(("dns", zone_id, hostname, tunnel_id, replace))
        return {"id": "dns-1", "proxied": True}, True

    def connector_token(self, account_id, tunnel_id):
        self.calls.append(("connector-token", account_id, tunnel_id))
        return "eyJ" + "x" * 80


def fake_manifest_signer():
    def runner(command, **kwargs):
        request = json.loads(kwargs["input"])
        assert request["publicKey"] == PUBLIC_KEY
        assert "CLOUDFLARE_API_TOKEN" not in kwargs["env"]
        return SimpleNamespace(
            stdout=json.dumps(
                {"publicKey": PUBLIC_KEY, "signature": SIGNATURE}
            )
        )

    return worker_bootstrap.MirrorManifestSigner(["sign"], runner=runner)


def options(tmp_path, **overrides):
    values = dict(
        hostname="mirror.example.com",
        zone_name="example.com",
        tunnel_name="forkmesh-mirror-a",
        node_name="mirror-a",
        origin_host="127.0.0.1",
        origin_port=8790,
        account_id="account-1",
    )
    values.update(overrides)
    return tunnel_bootstrap.TunnelOptions(**values)


def test_provision_configures_remote_ingress_and_proxied_cname_without_token_output(
    tmp_path,
):
    api = FakeTunnelAPI()
    cloudflare_token = "cloudflare-api-token-must-not-be-written"
    output = []
    result = tunnel_bootstrap.provision_tunnel(
        options(tmp_path),
        token=cloudflare_token,
        api=api,
        output=output.append,
    )

    assert result["ok"] is True
    assert result["apiTokenPersisted"] is False
    assert result["connectorToken"]["stored"] is False
    assert cloudflare_token not in repr(result)
    assert cloudflare_token not in "\n".join(output)
    assert (
        "config",
        "account-1",
        "11111111-2222-3333-4444-555555555555",
        "mirror.example.com",
        "127.0.0.1",
        8790,
    ) in api.calls
    assert (
        "dns",
        "zone-1",
        "mirror.example.com",
        "11111111-2222-3333-4444-555555555555",
        False,
    ) in api.calls
    assert not any(call[0] == "connector-token" for call in api.calls)


def test_dry_run_resolves_only_and_does_not_mutate_or_retrieve_connector_token(
    tmp_path,
):
    api = FakeTunnelAPI(existing=True)
    result = tunnel_bootstrap.provision_tunnel(
        options(tmp_path, dry_run=True),
        token="memory-only",
        api=api,
        output=lambda _message: None,
    )
    assert result["dryRun"] is True
    assert result["tunnelExists"] is True
    assert [call[0] for call in api.calls] == [
        "verify",
        "account",
        "zone",
        "find",
    ]


def test_explicit_connector_token_file_is_mode_0600_and_summary_is_redacted(
    tmp_path,
):
    api = FakeTunnelAPI()
    token_path = tmp_path / "secrets" / "tunnel-token"
    result = tunnel_bootstrap.provision_tunnel(
        options(tmp_path, tunnel_token_file=token_path),
        token="api-token",
        api=api,
        output=lambda _message: None,
    )
    connector = api.connector_token("account-1", result["tunnelId"])
    assert token_path.read_text().strip() == connector
    assert stat.S_IMODE(token_path.stat().st_mode) == 0o600
    assert connector not in repr(result)
    assert result["connectorToken"] == {
        "stored": True,
        "path": str(token_path.resolve()),
        "passedInProcessEnvironment": False,
    }


def test_existing_different_connector_token_file_fails_closed(tmp_path):
    token_path = tmp_path / "tunnel-token"
    token_path.write_text("different-credential\n")
    with pytest.raises(worker_bootstrap.BootstrapError, match="different contents"):
        tunnel_bootstrap.provision_tunnel(
            options(tmp_path, tunnel_token_file=token_path),
            token="api-token",
            api=FakeTunnelAPI(),
            output=lambda _message: None,
        )


def test_signed_public_manifest_is_written_without_identity_private_key(tmp_path):
    manifest_path = tmp_path / "public" / "forkmesh-mirror.json"
    result = tunnel_bootstrap.provision_tunnel(
        options(
            tmp_path,
            mirror_public_key=PUBLIC_KEY,
            manifest_signer_command=("unused",),
            manifest_output=manifest_path,
        ),
        token="api-token",
        api=FakeTunnelAPI(),
        manifest_signer=fake_manifest_signer(),
        output=lambda _message: None,
    )
    manifest = json.loads(manifest_path.read_text())
    assert manifest["endpoint"]["origin"] == "https://mirror.example.com"
    assert manifest["endpoint"]["transport"] == "direct-https"
    assert manifest["edge"]["kind"] == "cloudflare-tunnel"
    assert manifest["edge"]["originExposure"] == "loopback-only"
    assert manifest["dns"] == {
        "recordName": "mirror.example.com",
        "recordType": "CNAME",
        "proxied": True,
        "target": "11111111-2222-3333-4444-555555555555.cfargotunnel.com",
    }
    assert "workerRoute" not in manifest["dns"]
    assert manifest["signature"]["value"] == SIGNATURE
    assert result["manifest"]["payloadSha256"] == manifest["signature"][
        "payloadSha256"
    ]
    assert "privateKey" not in manifest_path.read_text()


def test_cloudflare_api_adapter_uses_remote_config_and_proxied_tunnel_dns():
    class Client:
        def __init__(self):
            self.calls = []
            self.responses = [
                [],  # find tunnel
                {"id": "tunnel-id"},  # create
                {"config": {}},  # get config
                {"version": 1},  # put config
                [],  # list dns
                {"id": "dns-id", "proxied": True},  # create dns
            ]

        def request(self, method, path, **kwargs):
            self.calls.append((method, path, kwargs))
            return self.responses.pop(0)

    client = Client()
    api = tunnel_bootstrap.CloudflareTunnelAPI(client)
    tunnel, created = api.ensure_tunnel("account", "mirror-a")
    assert created and tunnel["id"] == "tunnel-id"
    create_body = client.calls[1][2]["body"]
    assert create_body["config_src"] == "cloudflare"
    assert len(base64.b64decode(create_body["tunnel_secret"])) == 32
    assert api.ensure_configuration(
        "account", "tunnel-id", "mirror.example.com", "127.0.0.1", 8790
    )
    ingress = client.calls[3][2]["body"]["config"]["ingress"]
    assert ingress[0] == {
        "hostname": "mirror.example.com",
        "service": "http://127.0.0.1:8790",
        "originRequest": {
            "httpHostHeader": "mirror.example.com",
            "connectTimeout": 10,
            "noTLSVerify": False,
        },
    }
    assert ingress[-1] == {"service": "http_status:404"}
    dns, changed = api.ensure_dns(
        "zone", "mirror.example.com", "tunnel-id", replace=False
    )
    assert changed and dns["proxied"] is True
    dns_body = client.calls[5][2]["body"]
    assert dns_body["type"] == "CNAME"
    assert dns_body["content"] == "tunnel-id.cfargotunnel.com"
    assert dns_body["proxied"] is True


def test_launch_passes_connector_only_in_cloudflared_environment(monkeypatch, tmp_path):
    config = tmp_path / "gateway.json"
    config.write_text("{}")
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "api-secret")
    calls = []

    class Process:
        def __init__(self, name):
            self.name = name
            self.returncode = None
            self.polls = 0

        def poll(self):
            self.polls += 1
            if self.name == "gateway" and self.polls >= 2:
                self.returncode = 0
            return self.returncode

        def terminate(self):
            self.returncode = 0

        def wait(self, timeout=None):
            return self.returncode or 0

        def kill(self):
            self.returncode = -9

    def popen(command, **kwargs):
        calls.append((command, kwargs))
        return Process("gateway" if "mirror_gateway.py" in " ".join(command) else "tunnel")

    monkeypatch.setattr(tunnel_bootstrap.shutil, "which", lambda _name: "/bin/cloudflared")
    monkeypatch.setattr(tunnel_bootstrap.time, "sleep", lambda _seconds: None)
    code = tunnel_bootstrap.launch_services(
        gateway_config=config,
        connector_token="connector-secret",
        cloudflared_binary="cloudflared",
        popen=popen,
    )
    assert code == 0
    assert len(calls) == 2
    assert all("api-secret" not in repr(call) for call in calls)
    assert "connector-secret" not in " ".join(calls[1][0])
    assert calls[1][1]["env"]["TUNNEL_TOKEN"] == "connector-secret"
    assert "TUNNEL_TOKEN" not in calls[0][1]["env"]


def test_tunnel_tool_is_separate_and_worker_d1_bootstrap_contract_remains():
    tunnel_source = (ROOT / "tools" / "cloudflare_tunnel_bootstrap.py").read_text()
    worker_source = (ROOT / "tools" / "cloudflare_bootstrap.py").read_text()
    assert "cfd_tunnel" in tunnel_source
    assert "cfargotunnel.com" in tunnel_source
    assert "ensure_d1" in worker_source
    assert "d1" not in tunnel_bootstrap.TunnelOptions.__dataclass_fields__
