#!/usr/bin/env python3
"""Contracts for the single-App, token-in-memory Cloudflare bootstrapper."""

import base64
from contextlib import contextmanager
import json
from pathlib import Path
import re
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import cloudflare_bootstrap as bootstrap  # noqa: E402


ROUTER_SEED = base64.urlsafe_b64encode(bytes(range(32))).decode().rstrip("=")
ROUTER_KEY = bootstrap.derive_ed25519_public_key(ROUTER_SEED)
RELEASE = {
    "buildRev": "selfhost-0123456789abcdef",
    "appVersion": "1.2.3",
    "deployFingerprint": "a" * 64,
}


class FakeAPI:
    def __init__(self, *, worker_present=False, routes=None, dns=None):
        self.worker_present = worker_present
        self.routes = list(routes or [])
        self.dns = list(dns or [])
        self.calls = []
        self.database_created = not worker_present
        self.secret_names = set() if not worker_present else {"DATA_KEY"}

    def verify_token(self):
        self.calls.append(("verify",))

    def resolve_account(self, requested_id=""):
        self.calls.append(("account", requested_id))
        return {"id": requested_id or "account-1", "name": "Test"}

    def resolve_zone(self, account_id, zone_name):
        self.calls.append(("zone", account_id, zone_name))
        return {"id": "zone-1", "name": zone_name}

    def list_zones(self, account_id):
        self.calls.append(("zones", account_id))
        return [{"id": "zone-1", "name": "example.com", "status": "active"}]

    def worker_exists(self, account_id, worker_name):
        self.calls.append(("worker", account_id, worker_name))
        return self.worker_present

    def worker_routes(self, zone_id):
        self.calls.append(("routes-read", zone_id))
        return list(self.routes)

    def dns_records(self, zone_id, hostname):
        self.calls.append(("dns-read", zone_id, hostname))
        return list(self.dns)

    def ensure_d1(self, account_id, database_name):
        self.calls.append(("d1", account_id, database_name))
        return "11111111-2222-3333-4444-555555555555", self.database_created

    def ensure_kv_namespace(self, account_id, namespace_name):
        self.calls.append(("kv", account_id, namespace_name))
        return "b" * 32, True

    def worker_secret_names(self, account_id, worker_name):
        self.calls.append(("secrets", account_id, worker_name))
        return set(self.secret_names)

    def ensure_dns(self, zone_id, hostname, *, replace):
        self.calls.append(("dns", zone_id, hostname, replace))
        return {"id": "dns-1", "proxied": True}, True

    def ensure_worker_route(self, zone_id, pattern, worker_name, *, replace):
        self.calls.append(("route", zone_id, pattern, worker_name, replace))
        return {"id": "route-1", "pattern": pattern, "script": worker_name}, True


class FakeRunner:
    def __init__(self):
        self.calls = []
        self.configs = []

    def build_assets(self, *, hidden_env_names=()):
        self.calls.append(("build-app", tuple(hidden_env_names)))

    def build_world_assets(self, *, hidden_env_names=()):
        self.calls.append(("build-world", tuple(hidden_env_names)))

    def run(
        self, arguments, *, token, account_id, stdin_text=None,
        hidden_env_names=(),
    ):
        arguments = list(arguments)
        config_name = arguments[arguments.index("--config") + 1]
        config_path = bootstrap.WORKER_DIR / config_name
        assert config_path.is_file()
        config = config_path.read_text(encoding="utf-8")
        self.configs.append(config)
        self.calls.append({
            "arguments": arguments,
            "token": token,
            "account_id": account_id,
            "stdin_text": stdin_text,
            "hidden": tuple(hidden_env_names),
            "config": config,
        })


def options(tmp_path, **overrides):
    values = dict(
        hostname="mesh.example.com",
        zone_name="example.com",
        worker_name="alice-forkmesh",
        database_name="alice-forkmesh",
        node_name="alice-node",
        relay_label="Alice relay",
        main_relay_url="",
        publish_mirror_manifest=False,
        data_key_backup=tmp_path / "forkmesh.data-key",
        router_identity_backup=tmp_path / "router-identity.json",
    )
    values.update(overrides)
    return bootstrap.BootstrapOptions(**values)


def release_identity():
    return dict(RELEASE)


def no_live_checks(**kwargs):
    kwargs.setdefault("health_check", lambda _host: None)
    kwargs.setdefault("readiness_check", lambda _host, _key: None)
    kwargs.setdefault("snapshot_readiness_check", lambda _host, _fp: None)
    kwargs.setdefault("router_identity_check", lambda _host, _key: None)
    kwargs.setdefault("release_identity_check", lambda _host, _release: None)
    kwargs.setdefault("release_identity_resolver", release_identity)
    kwargs.setdefault("output", lambda _message: None)
    return kwargs


@contextmanager
def staged_empty_assets(tmp_path):
    directory = tmp_path / "staged-assets"
    directory.mkdir(exist_ok=True)
    yield directory


def test_rendered_config_has_one_app_db_cron_and_no_official_routes():
    source = bootstrap.WRANGLER_TEMPLATE.read_text(encoding="utf-8")
    rendered = bootstrap.render_wrangler_config(
        source,
        worker_name="alice-forkmesh",
        database_name="alice-forkmesh",
        database_id="11111111-2222-3333-4444-555555555555",
        namespace_id="b" * 32,
        public_base_url="https://mesh.example.com",
        node_name="alice-node",
        relay_label="Alice relay",
        main_relay_url="https://app.forkmesh.com",
        world_origin="https://forkmesh-world.example.com",
        build_rev=RELEASE["buildRev"],
        app_version=RELEASE["appVersion"],
        deploy_fingerprint=RELEASE["deployFingerprint"],
        assets_directory="/tmp/forkmesh-assets",
    )
    manifest = __import__("tomllib").loads(rendered)
    assert manifest["name"] == "alice-forkmesh"
    assert manifest["workers_dev"] is False
    assert [item["binding"] for item in manifest["d1_databases"]] == ["DB"]
    assert manifest["triggers"]["crons"] == ["* * * * *"]
    assert "routes" not in manifest
    assert "PUBLIC_SNAPSHOTS" not in rendered
    assert "edge-control" not in rendered
    assert "CLOUDFLARE_API_TOKEN" not in rendered
    assert source == bootstrap.WRANGLER_TEMPLATE.read_text(encoding="utf-8")


def test_router_identity_pair_and_public_reader_require_exact_app_contract():
    assert bootstrap.validate_router_identity_pair(ROUTER_KEY, ROUTER_SEED) == ROUTER_KEY
    with pytest.raises(bootstrap.BootstrapError, match="does not match"):
        bootstrap.validate_router_identity_pair("A" * 43, ROUTER_SEED)

    class Response:
        status = 200
        headers = {"x-forkmesh-worker": "app"}

        def __init__(self, url):
            self.url = url

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def geturl(self):
            return self.url

        def read(self, limit):
            assert limit == 8193
            return json.dumps({
                "ok": True,
                "protocol": "forkmesh-masked-proxy-v1",
                "registration": "forkmesh-https-endpoint-v1",
                "routerPublicKey": ROUTER_KEY,
                "nonCustodial": True,
                "repositoryBytesInD1": False,
            }).encode()

    def opener(request, *, timeout):
        assert timeout == 30
        return Response(request.full_url)

    assert bootstrap._read_router_public_key(
        "mesh.example.com", opener=opener, require_app_worker=True
    ) == ROUTER_KEY
    Response.headers = {"x-forkmesh-worker": "app", "x-forkmesh-edge-control": "active"}
    # The reader pins App ownership; release/snapshot checks separately reject
    # the legacy marker during final takeover verification.
    assert bootstrap._read_router_public_key(
        "mesh.example.com", opener=opener, require_app_worker=True
    ) == ROUTER_KEY


def test_post_deploy_router_identity_requires_both_app_endpoints():
    bootstrap._post_deploy_router_identity_check(
        "mesh.example.com",
        ROUTER_KEY,
        attempts=1,
        backend_reader=lambda _host: ROUTER_KEY,
        app_reader=lambda _host: ROUTER_KEY,
    )
    with pytest.raises(bootstrap.BootstrapError, match="App endpoints"):
        bootstrap._post_deploy_router_identity_check(
            "mesh.example.com",
            ROUTER_KEY,
            attempts=1,
            backend_reader=lambda _host: ROUTER_KEY,
            app_reader=lambda _host: "B" * 43,
        )


def test_release_identity_rejects_legacy_edge_marker(monkeypatch):
    main = {
        "ok": True, "runtime": "python-workers", "worker": "app",
        "rev": RELEASE["buildRev"],
        "deployFingerprint": RELEASE["deployFingerprint"],
    }
    version = {
        "ok": True, "worker": "app", "rev": RELEASE["buildRev"],
        "version": RELEASE["appVersion"],
        "deployFingerprint": RELEASE["deployFingerprint"],
    }

    class Response:
        status = 200

        def __init__(self, url, payload, legacy=False):
            self.url = url
            self.payload = payload
            self.headers = {"x-forkmesh-worker": "app"}
            if legacy:
                self.headers["x-forkmesh-edge-control"] = "active"

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def geturl(self):
            return self.url

        def read(self, _limit):
            return json.dumps(self.payload).encode()

    legacy = False

    def opener(request, **_kwargs):
        payload = main if request.full_url.endswith("/api/mainnode") else version
        return Response(request.full_url, payload, legacy=legacy)

    monkeypatch.setattr(bootstrap, "urlopen", opener)
    bootstrap._release_identity_check("mesh.example.com", RELEASE, attempts=1)
    legacy = True
    with pytest.raises(bootstrap.BootstrapError, match="App endpoints"):
        bootstrap._release_identity_check("mesh.example.com", RELEASE, attempts=1)


def test_dry_run_is_read_only_and_describes_one_app(tmp_path):
    api = FakeAPI()
    result = bootstrap.bootstrap(
        options(tmp_path, dry_run=True),
        token="token-in-memory",
        api=api,
        runner=FakeRunner(),
        **no_live_checks(),
    )
    assert result["dryRun"] is True
    assert result["workerName"] == "alice-forkmesh"
    assert "edgeControl" not in result
    assert result["routerIdentity"]["routerIdentitySource"] == "generated-on-apply"
    assert not any(call[0] in {"d1", "kv", "dns", "route"} for call in api.calls)


def test_new_install_deploys_one_app_one_db_one_route_and_hides_secrets(tmp_path):
    api = FakeAPI()
    runner = FakeRunner()

    @contextmanager
    def stage(_manifest):
        with staged_empty_assets(tmp_path) as directory:
            yield directory

    result = bootstrap.bootstrap(
        options(tmp_path),
        token="token-must-not-persist",
        api=api,
        runner=runner,
        asset_stager=stage,
        **no_live_checks(),
    )
    assert result["ok"] is True and result["dryRun"] is False
    assert "edgeControl" not in result
    assert result["routerIdentity"]["routerIdentitySource"] == "generated-local-pair"
    assert [call for call in api.calls if call[0] == "d1"] == [
        ("d1", "account-1", "alice-forkmesh")
    ]
    assert [call for call in api.calls if call[0] == "route"] == [
        ("route", "zone-1", "mesh.example.com/*", "alice-forkmesh", False)
    ]
    deploys = [
        call for call in runner.calls
        if isinstance(call, dict) and call["arguments"][0] == "deploy"
    ]
    assert len(deploys) == 1
    assert 'name = "alice-forkmesh"' in deploys[0]["config"]
    assert all("edge-control" not in config for config in runner.configs)
    assert all("token-must-not-persist" not in config for config in runner.configs)
    migrations = [
        call for call in runner.calls
        if isinstance(call, dict) and call["arguments"][:3] == [
            "d1", "migrations", "apply",
        ]
    ]
    assert len(migrations) == 1
    secret_calls = [
        call for call in runner.calls
        if isinstance(call, dict) and call["arguments"][:2] == ["secret", "put"]
    ]
    assert [call["arguments"][-1] for call in secret_calls][-2:] == [
        "MIRROR_ROUTER_SIGNING_SEED", "MIRROR_ROUTER_PUBLIC_KEY",
    ]
    output = json.dumps(result, sort_keys=True)
    recovery = json.loads((tmp_path / "router-identity.json").read_text())
    assert recovery["signingSeed"] not in output


def test_existing_single_app_rerun_preserves_public_router_identity(tmp_path):
    route = {"pattern": "mesh.example.com/*", "script": "alice-forkmesh"}
    dns = {"name": "mesh.example.com", "type": "AAAA", "proxied": True}
    api = FakeAPI(worker_present=True, routes=[route], dns=[dns])
    (tmp_path / "forkmesh.data-key").write_text("operator-recovery\n")
    (tmp_path / "forkmesh.data-key").chmod(0o600)
    runner = FakeRunner()

    @contextmanager
    def stage(_manifest):
        with staged_empty_assets(tmp_path) as directory:
            yield directory

    result = bootstrap.bootstrap(
        options(tmp_path, router_identity_backup=None),
        token="token",
        api=api,
        runner=runner,
        asset_stager=stage,
        router_identity_reader=lambda _host: ROUTER_KEY,
        **no_live_checks(),
    )
    assert result["routerIdentity"]["routerIdentitySource"] == "existing-public-endpoint"
    assert "MIRROR_ROUTER_PUBLIC_KEY" not in result["workerSecretsSet"]


@pytest.mark.parametrize("script", ["legacy-edge", "alice-forkmesh"])
def test_legacy_or_exact_overlapping_route_fails_before_mutation(tmp_path, script):
    routes = [
        {"pattern": "mesh.example.com/*", "script": "alice-forkmesh"},
        {"pattern": "mesh.example.com/api/version*", "script": script},
    ]
    dns = [{"name": "mesh.example.com", "type": "A", "proxied": True}]
    api = FakeAPI(worker_present=True, routes=routes, dns=dns)
    with pytest.raises(bootstrap.BootstrapError, match="legacy or unexpected"):
        bootstrap.bootstrap(
            options(tmp_path), token="token", api=api, runner=FakeRunner(),
            router_identity_reader=lambda _host: pytest.fail("must not read identity"),
            **no_live_checks(),
        )
    assert not any(call[0] in {"d1", "kv", "dns", "route"} for call in api.calls)


def test_optional_world_stays_distinct_and_app_trust_is_published_after_world(tmp_path):
    api = FakeAPI()
    runner = FakeRunner()

    @contextmanager
    def stage(_manifest):
        with staged_empty_assets(tmp_path) as directory:
            yield directory

    result = bootstrap.bootstrap(
        options(tmp_path, deploy_world=True),
        token="token",
        api=api,
        runner=runner,
        asset_stager=stage,
        world_health_check=lambda _host: None,
        **no_live_checks(),
    )
    assert result["world"]["enabled"] is True
    deploy_configs = [
        call["config"] for call in runner.calls
        if isinstance(call, dict) and call["arguments"][0] == "deploy"
    ]
    assert len(deploy_configs) == 2
    assert 'name = "alice-forkmesh-world"' in deploy_configs[0]
    assert 'WORLD_ORIGIN = "https://forkmesh-world.example.com"' in deploy_configs[1]


def test_token_only_discovery_is_read_only_and_fails_closed_when_ambiguous():
    api = FakeAPI()
    discovered = bootstrap.discover_deployment_defaults(token="token", api=api)
    assert discovered["hostname"] == "forkmesh.example.com"
    assert not any(call[0] in {"d1", "kv", "dns", "route"} for call in api.calls)

    class Ambiguous(FakeAPI):
        def list_zones(self, account_id):
            return [
                {"id": "one", "name": "one.example", "status": "active"},
                {"id": "two", "name": "two.example", "status": "active"},
            ]

    with pytest.raises(bootstrap.BootstrapError, match="multiple or no active zones"):
        bootstrap.discover_deployment_defaults(token="token", api=Ambiguous())


def test_bootstrap_docs_describe_single_app_main_db_and_external_legacy_cutover():
    docs = (
        ROOT / "www/docs/operations/automation-and-cloudflare-bootstrap.md"
    ).read_text(encoding="utf-8")
    assert "Workers KV Storage edit" in docs
    assert "generates `DATA_KEY`" in docs
    assert "one complete App Worker" in docs
    assert "one broad `hostname/*` App route" in docs
    assert "legacy exact control route" in docs
    assert "sibling edge-control Worker" not in docs

