#!/usr/bin/env python3
"""Contracts for the token-in-memory Cloudflare bootstrapper."""

import base64
import json
from pathlib import Path
import re
import sys
import tomllib
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import cloudflare_bootstrap as bootstrap_module  # noqa: E402

ORIGINAL_POST_DEPLOY_ROUTER_IDENTITY_CHECK = (
    bootstrap_module._post_deploy_router_identity_check
)
ORIGINAL_RELEASE_IDENTITY_CHECK = bootstrap_module._release_identity_check
ORIGINAL_PUBLIC_SNAPSHOT_READINESS_CHECK = (
    bootstrap_module._public_snapshot_readiness_check
)

PUBLIC_KEY = base64.urlsafe_b64encode(b"P" * 32).decode().rstrip("=")
SIGNATURE = base64.urlsafe_b64encode(b"S" * 64).decode().rstrip("=")
ROUTER_SIGNING_SEED = base64.urlsafe_b64encode(bytes(range(32))).decode().rstrip("=")
ROUTER_PUBLIC_KEY = bootstrap_module.derive_ed25519_public_key(ROUTER_SIGNING_SEED)
RELEASE_IDENTITY = {
    "buildRev": "selfhost-0123456789abcdef",
    "appVersion": "1.2.3",
    "deployFingerprint": "a" * 64,
}


def test_snapshot_readiness_window_covers_slow_five_minute_cadence():
    assert ORIGINAL_PUBLIC_SNAPSHOT_READINESS_CHECK.__defaults__ == (40,)


class FakeAPI:
    def __init__(self):
        self.verified = False
        self.calls = []
        self.database_created = True
        self.secret_names = {"DATA_KEY"}
        self.worker_present = True
        self.route_records = None
        self.dns_record_list = None

    def verify_token(self):
        self.verified = True

    def resolve_account(self, requested_id=""):
        self.calls.append(("account", requested_id))
        return {"id": requested_id or "account-1", "name": "Test"}

    def resolve_zone(self, account_id, zone_name):
        self.calls.append(("zone", account_id, zone_name))
        return {"id": "zone-1", "name": zone_name}

    def list_zones(self, account_id):
        self.calls.append(("zones", account_id))
        return [
            {
                "id": "zone-1",
                "name": "example.com",
                "status": "active",
            }
        ]

    def ensure_d1(self, account_id, database_name):
        self.calls.append(("d1", account_id, database_name))
        return (
            "11111111-2222-3333-4444-555555555555",
            self.database_created,
        )

    def ensure_kv_namespace(self, account_id, namespace_name):
        self.calls.append(("kv", account_id, namespace_name))
        return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", True

    def worker_secret_names(self, account_id, worker_name):
        self.calls.append(("secrets", account_id, worker_name))
        return set(self.secret_names)

    def worker_exists(self, account_id, worker_name):
        self.calls.append(("worker", account_id, worker_name))
        return self.worker_present

    def worker_routes(self, zone_id):
        self.calls.append(("routes-read", zone_id))
        if self.route_records is not None:
            return list(self.route_records)
        if not self.worker_present:
            return []
        return [
            {
                "pattern": f"{hostname}/*",
                "script": "alice-forkmesh",
            }
            for hostname in ("mesh.example.com", "forkmesh.example.com")
        ]

    def dns_records(self, zone_id, hostname):
        self.calls.append(("dns-read", zone_id, hostname))
        if self.dns_record_list is not None:
            return list(self.dns_record_list)
        if not self.worker_present:
            return []
        return [{"name": hostname, "type": "AAAA", "proxied": True}]

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
        self.manifests = []
        self.assets_built = False

    def build_assets(self, *, hidden_env_names=()):
        self.assets_built = True
        self.hidden_build_env = tuple(hidden_env_names)

    def build_world_assets(self, *, hidden_env_names=()):
        self.world_assets_built = True
        self.hidden_world_build_env = tuple(hidden_env_names)

    def run(
        self,
        arguments,
        *,
        token,
        account_id,
        stdin_text=None,
        hidden_env_names=(),
    ):
        arguments = list(arguments)
        self.calls.append(
            {
                "arguments": arguments,
                "token": token,
                "account_id": account_id,
                "stdin_text": stdin_text,
                "hidden_env_names": tuple(hidden_env_names),
            }
        )
        config_index = arguments.index("--config") + 1
        config = bootstrap_module.WORKER_DIR / arguments[config_index]
        assert config.exists()
        config_text = config.read_text(encoding="utf-8")
        self.configs.append(config_text)
        if arguments[0] == "deploy":
            match = re.search(r'^directory = "([^"]+)"$', config_text, re.MULTILINE)
            if match:
                manifest = Path(match.group(1)) / bootstrap_module.MIRROR_MANIFEST_ASSET
                if manifest.exists():
                    self.manifests.append(json.loads(manifest.read_text(encoding="utf-8")))


def fake_manifest_signer(*, expect_data_hidden=False):
    def run(command, **kwargs):
        request = json.loads(kwargs["input"])
        assert request["publicKey"] == PUBLIC_KEY
        assert request["algorithm"] == "Ed25519"
        assert "CLOUDFLARE_API_TOKEN" not in kwargs["env"]
        if expect_data_hidden:
            assert "DATA_KEY" not in kwargs["env"]
        return SimpleNamespace(
            stdout=json.dumps({"publicKey": PUBLIC_KEY, "signature": SIGNATURE})
        )

    return bootstrap_module.MirrorManifestSigner(["local-signer"], runner=run)


def existing_router_identity(hostname):
    assert hostname in {"mesh.example.com", "forkmesh.example.com"}
    return ROUTER_PUBLIC_KEY


def router_identity_payload(public_key=ROUTER_PUBLIC_KEY):
    return {
        "ok": True,
        "protocol": "forkmesh-masked-proxy-v1",
        "registration": "forkmesh-https-endpoint-v1",
        "routerPublicKey": public_key,
        "nonCustodial": True,
        "repositoryBytesInD1": False,
    }


@pytest.fixture(autouse=True)
def avoid_live_post_deploy_checks(monkeypatch):
    monkeypatch.setattr(
        bootstrap_module,
        "_post_deploy_router_identity_check",
        lambda _hostname, _public_key: None,
    )
    monkeypatch.setattr(
        bootstrap_module,
        "_release_identity_check",
        lambda _hostname, _identity: None,
    )
    monkeypatch.setattr(
        bootstrap_module,
        "_public_snapshot_readiness_check",
        lambda _hostname, _fingerprint: None,
    )


def test_generated_config_targets_new_d1_without_mutating_template():
    template_path = bootstrap_module.WRANGLER_TEMPLATE
    original = template_path.read_text(encoding="utf-8")
    rendered = bootstrap_module.render_wrangler_config(
        original,
        worker_name="alice-forkmesh",
        database_name="alice-forkmesh",
        database_id="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        namespace_id="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        public_base_url="https://mesh.example.com",
        node_name="alice-node",
        relay_label="Alice relay",
        main_relay_url="https://forkmesh.com",
        world_origin="https://forkmesh-world.example.com",
        build_rev=RELEASE_IDENTITY["buildRev"],
        app_version=RELEASE_IDENTITY["appVersion"],
        deploy_fingerprint=RELEASE_IDENTITY["deployFingerprint"],
        assets_directory="/tmp/forkmesh-assets",
    )

    assert template_path.read_text(encoding="utf-8") == original
    assert rendered.startswith('name = "alice-forkmesh"')
    assert rendered.count("workers_dev = false") == 1
    assert 'database_id = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"' in rendered
    assert 'database_name = "alice-forkmesh"' in rendered
    assert 'binding = "PUBLIC_SNAPSHOTS"' not in rendered
    assert rendered.count("[[d1_databases]]") == 1
    assert 'id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' in rendered
    assert 'PUBLIC_BASE_URL = "https://mesh.example.com"' in rendered
    assert 'API_ORIGIN = "https://mesh.example.com"' in rendered
    assert 'APP_ORIGIN = "https://mesh.example.com"' in rendered
    assert 'WWW_ORIGIN = "https://mesh.example.com"' in rendered
    assert 'WORLD_ORIGIN = "https://forkmesh-world.example.com"' in rendered
    assert (
        'CORS_ALLOWED_ORIGINS = '
        '"https://mesh.example.com,https://forkmesh-world.example.com"'
    ) in rendered
    assert 'SINGLE_WORKER_SITE = "true"' in rendered
    assert (
        'DISCORD_OAUTH_REDIRECT_URI = '
        '"https://mesh.example.com/api/integrations/discord/callback"'
    ) in rendered
    assert 'MAIN_RELAY_URL = "https://forkmesh.com"' in rendered
    assert f'BUILD_REV = "{RELEASE_IDENTITY["buildRev"]}"' in rendered
    assert f'APP_VERSION = "{RELEASE_IDENTITY["appVersion"]}"' in rendered
    assert (
        f'DEPLOY_FINGERPRINT = "{RELEASE_IDENTITY["deployFingerprint"]}"'
        in rendered
    )
    assert 'directory = "/tmp/forkmesh-assets"' in rendered
    assert "tools/build_worker_python.py" in rendered
    assert "tools/build_site_assets.py app" in rendered
    assert "./migrate.sh" not in next(
        line for line in rendered.splitlines() if line.startswith("command =")
    )
    assert "CLOUDFLARE_API_TOKEN" not in rendered
    assert "app.forkmesh.com" not in rendered
    assert "[[routes]]" not in rendered
    assert "/" not in tomllib.loads(rendered)["assets"]["run_worker_first"]


def test_generated_config_has_app_cron_and_no_edge_plumbing():
    source = bootstrap_module.WRANGLER_TEMPLATE.read_text(encoding="utf-8")
    rendered = bootstrap_module.render_wrangler_config(
        source,
        worker_name="alice-forkmesh",
        database_name="alice-main",
        database_id="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        namespace_id="b" * 32,
        public_base_url="https://forkmesh.example.com",
        node_name="alice-node",
        relay_label="Alice relay",
        main_relay_url="https://forkmesh.com",
        world_origin="https://forkmesh-world.example.com",
        build_rev=RELEASE_IDENTITY["buildRev"],
        app_version=RELEASE_IDENTITY["appVersion"],
        deploy_fingerprint=RELEASE_IDENTITY["deployFingerprint"],
        assets_directory="/tmp/assets",
    )

    manifest = tomllib.loads(rendered)
    assert manifest["name"] == "alice-forkmesh"
    assert manifest["triggers"]["crons"] == ["* * * * *"]
    assert "services" not in manifest
    assert {item["binding"] for item in manifest["d1_databases"]} == {"DB"}
    assert "edge-control" not in rendered
    assert "PUBLIC_SNAPSHOTS" not in rendered


def test_d1_rendering_is_binding_named_and_rejects_an_extra_database():
    source = bootstrap_module.WRANGLER_TEMPLATE.read_text(encoding="utf-8")
    rendered = bootstrap_module.render_wrangler_config(
        source,
        worker_name="alice-forkmesh",
        database_name="alice-main",
        database_id="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        namespace_id="b" * 32,
        public_base_url="https://forkmesh.example.com",
        node_name="alice-node",
        relay_label="Alice relay",
        main_relay_url="https://forkmesh.com",
        world_origin="https://forkmesh-world.example.com",
        build_rev=RELEASE_IDENTITY["buildRev"],
        app_version=RELEASE_IDENTITY["appVersion"],
        deploy_fingerprint=RELEASE_IDENTITY["deployFingerprint"],
        assets_directory="/tmp/assets",
    )
    bindings = {
        value["binding"]: value for value in tomllib.loads(rendered)["d1_databases"]
    }
    assert bindings["DB"]["database_name"] == "alice-main"
    assert set(bindings) == {"DB"}

    poisoned = source.replace(
        "[[kv_namespaces]]",
        '[[d1_databases]]\nbinding = "PUBLIC_SNAPSHOTS"\n'
        'database_name = "snapshots"\n'
        'database_id = "cccccccc-dddd-eeee-ffff-000000000000"\n\n'
        "[[kv_namespaces]]",
        1,
    )
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="unknown or duplicate production D1 binding",
    ):
        bootstrap_module.render_wrangler_config(
            poisoned,
            worker_name="alice-app",
            database_name="alice-main",
            database_id="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
            namespace_id="b" * 32,
            public_base_url="https://forkmesh.example.com",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="https://forkmesh.com",
            world_origin="https://forkmesh-world.example.com",
            build_rev=RELEASE_IDENTITY["buildRev"],
            app_version=RELEASE_IDENTITY["appVersion"],
            deploy_fingerprint=RELEASE_IDENTITY["deployFingerprint"],
            assets_directory="/tmp/assets",
        )


def test_router_seed_derivation_matches_rfc8032_and_rejects_mismatched_pair():
    seed = bytes.fromhex(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
    )
    public = bytes.fromhex(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    )

    def encode(value):
        return base64.urlsafe_b64encode(value).decode().rstrip("=")

    assert bootstrap_module.derive_ed25519_public_key(encode(seed)) == encode(public)
    assert (
        bootstrap_module.validate_router_identity_pair(
            ROUTER_PUBLIC_KEY, ROUTER_SIGNING_SEED
        )
        == ROUTER_PUBLIC_KEY
    )
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="does not match MIRROR_ROUTER_SIGNING_SEED",
    ):
        bootstrap_module.validate_router_identity_pair(PUBLIC_KEY, ROUTER_SIGNING_SEED)


def test_public_router_identity_reader_requires_exact_https_contract():
    requested = []

    class Response:
        status = 200
        headers = {
            "x-forkmesh-worker": "app",
        }

        def __init__(self, final_url):
            self.final_url = final_url

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def geturl(self):
            return self.final_url

        def read(self, limit):
            assert limit == 8193
            return json.dumps(router_identity_payload()).encode()

    def opener(request, *, timeout):
        assert timeout == 30
        requested.append(request)
        return Response(request.full_url)

    assert (
        bootstrap_module._read_router_public_key("ForkMesh.Example.com", opener=opener)
        == ROUTER_PUBLIC_KEY
    )
    assert requested[0].full_url == ("https://forkmesh.example.com/api/mirrors/https")
    assert requested[0].get_header("Cache-control") == "no-cache"
    assert (
        bootstrap_module._read_router_public_key(
            "forkmesh.example.com",
            opener=opener,
            require_app_worker=True,
        )
        == ROUTER_PUBLIC_KEY
    )

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="redirected unexpectedly",
    ):
        bootstrap_module._read_router_public_key(
            "forkmesh.example.com",
            opener=lambda request, **_kwargs: Response(
                "https://attacker.example/api/mirrors/https"
            ),
        )


def test_post_deploy_router_identity_requires_backend_and_public_app_key():
    ORIGINAL_POST_DEPLOY_ROUTER_IDENTITY_CHECK(
        "forkmesh.example.com",
        ROUTER_PUBLIC_KEY,
        attempts=1,
        backend_reader=lambda hostname: (
            ROUTER_PUBLIC_KEY
            if hostname == "forkmesh.example.com"
            else pytest.fail("unexpected hostname")
        ),
        app_reader=lambda hostname: (
            ROUTER_PUBLIC_KEY
            if hostname == "forkmesh.example.com"
            else pytest.fail("unexpected hostname")
        ),
    )

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="did not expose the selected router identity",
    ):
        ORIGINAL_POST_DEPLOY_ROUTER_IDENTITY_CHECK(
            "forkmesh.example.com",
            ROUTER_PUBLIC_KEY,
            attempts=1,
            backend_reader=lambda _hostname: PUBLIC_KEY,
            app_reader=lambda _hostname: ROUTER_PUBLIC_KEY,
        )


def test_backend_router_identity_proof_requires_python_app_and_ready_pair():
    class Response:
        status = 200

        def __init__(self, payload):
            self.payload = payload

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def geturl(self):
            return "https://forkmesh.example.com/api/mainnode"

        def read(self, limit):
            assert limit == 8193
            return json.dumps(self.payload).encode()

    valid = {
        "ok": True,
        "runtime": "python-workers",
        "worker": "app",
        "routerIdentityReady": True,
        "routerPublicKey": ROUTER_PUBLIC_KEY,
    }
    assert (
        bootstrap_module._read_backend_router_public_key(
            "forkmesh.example.com",
            opener=lambda _request, **_kwargs: Response(valid),
        )
        == ROUTER_PUBLIC_KEY
    )
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="proof is invalid",
    ):
        bootstrap_module._read_backend_router_public_key(
            "forkmesh.example.com",
            opener=lambda _request, **_kwargs: Response({
                **valid,
                "routerIdentityReady": False,
            }),
        )


def test_release_identity_check_requires_exact_python_app_roles(monkeypatch):
    expected = dict(RELEASE_IDENTITY)
    main_payload = {
        "ok": True,
        "runtime": "python-workers",
        "worker": "app",
        "rev": expected["buildRev"],
        "deployFingerprint": expected["deployFingerprint"],
    }
    version_payload = {
        "ok": True,
        "worker": "app",
        "rev": expected["buildRev"],
        "version": expected["appVersion"],
        "deployFingerprint": expected["deployFingerprint"],
    }

    class Response:
        status = 200

        def __init__(self, url, payload, headers=None):
            self.url = url
            self.payload = payload
            self.headers = headers or {}

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def geturl(self):
            return self.url

        def read(self, limit):
            assert limit == 8193
            return json.dumps(self.payload).encode()

    def opener(request, **_kwargs):
        if request.full_url.endswith("/api/mainnode"):
            return Response(
                request.full_url,
                main_payload,
                {"x-forkmesh-worker": "app"},
            )
        return Response(
            request.full_url,
            version_payload,
            {"x-forkmesh-worker": "app"},
        )

    monkeypatch.setattr(bootstrap_module, "urlopen", opener)
    ORIGINAL_RELEASE_IDENTITY_CHECK(
        "forkmesh.example.com", expected, attempts=1
    )

    version_payload["worker"] = "edge"
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="did not agree on the expected release identity",
    ):
        ORIGINAL_RELEASE_IDENTITY_CHECK(
            "forkmesh.example.com", expected, attempts=1
        )


def test_release_identity_uses_installed_product_version_metadata(
    monkeypatch, tmp_path
):
    installed_root = tmp_path / "forkmesh"
    installed_worker = installed_root / "app"
    deploy_targets = installed_worker / "tools" / "deploy_targets.py"
    deploy_targets.parent.mkdir(parents=True)
    deploy_targets.write_text(
        "def fingerprint(target):\n"
        "    assert target == 'app'\n"
        f"    return {'b' * 64!r}\n",
        encoding="utf-8",
    )
    (installed_root / "forkmesh-version.txt").write_text(
        "0.7.19\n", encoding="utf-8"
    )
    monkeypatch.setattr(bootstrap_module, "ROOT", installed_root)
    monkeypatch.setattr(bootstrap_module, "WORKER_DIR", installed_worker)

    assert not (installed_root / "desktop" / "CMakeLists.txt").exists()
    assert bootstrap_module.self_host_release_identity() == {
        "buildRev": "selfhost-" + "b" * 16,
        "appVersion": "0.7.19",
        "deployFingerprint": "b" * 64,
    }
    cmake = (ROOT / "desktop" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "forkmesh-version.txt" in cmake
    assert 'CONTENT "${FORKMESH_VERSION}\\n"' in cmake


def test_new_one_click_install_generates_owner_only_router_recovery(tmp_path):
    api = FakeAPI()
    api.worker_present = False
    runner = FakeRunner()
    output = []
    router_checks = []
    router_backup = tmp_path / "secrets" / "forkmesh.router-identity.json"

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="",
            publish_mirror_manifest=False,
            data_key_backup=tmp_path / "forkmesh.data-key",
            router_identity_backup=router_backup,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=lambda _hostname: pytest.fail(
            "a new install must not read a nonexistent endpoint"
        ),
        router_identity_check=lambda hostname, public_key: router_checks.append(
            (hostname, public_key)
        ),
        output=output.append,
    )

    recovery = json.loads(router_backup.read_text(encoding="utf-8"))
    generated_public = bootstrap_module.validate_router_identity_pair(
        recovery["publicKey"], recovery["signingSeed"]
    )
    app_config = next(
        config for config in runner.configs
        if 'name = "alice-forkmesh"' in config
    )
    seed_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_SIGNING_SEED"
    )
    public_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_PUBLIC_KEY"
    )
    app_deploy = next(
        call for index, call in enumerate(runner.calls)
        if call["arguments"][0] == "deploy"
        and 'name = "alice-forkmesh"' in runner.configs[index]
    )
    assert router_backup.stat().st_mode & 0o777 == 0o600
    assert result["routerIdentity"]["routerIdentitySource"] == "generated-local-pair"
    assert result["routerIdentity"]["routerIdentityBackupCreated"] is True
    assert router_checks == [("forkmesh.example.com", generated_public)]
    assert generated_public not in app_config
    assert seed_call["stdin_text"] == recovery["signingSeed"] + "\n"
    assert seed_call["arguments"][3] == app_deploy["arguments"][2]
    assert public_call["arguments"][3] == app_deploy["arguments"][2]
    assert runner.calls.index(app_deploy) < runner.calls.index(seed_call)
    assert runner.calls.index(seed_call) < runner.calls.index(public_call)
    assert result["workerSecretsSet"][-2:] == [
        bootstrap_module.ROUTER_SIGNING_SEED_ENV,
        bootstrap_module.ROUTER_PUBLIC_KEY_ENV,
    ]
    assert recovery["signingSeed"] not in repr(result)
    assert recovery["signingSeed"] not in "\n".join(output)
    assert all(recovery["signingSeed"] not in config for config in runner.configs)


def test_new_install_validates_and_keeps_router_seed_out_of_output(
    monkeypatch, tmp_path
):
    monkeypatch.setenv("MIRROR_ROUTER_PUBLIC_KEY", ROUTER_PUBLIC_KEY)
    monkeypatch.setenv("MIRROR_ROUTER_SIGNING_SEED", ROUTER_SIGNING_SEED)
    api = FakeAPI()
    api.worker_present = False
    runner = FakeRunner()
    output = []

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="",
            secret_env=bootstrap_module.ROUTER_IDENTITY_ENV_NAMES,
            publish_mirror_manifest=False,
            data_key_backup=tmp_path / "forkmesh.data-key",
            router_identity_backup=tmp_path / "forkmesh.router-identity.json",
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=lambda _hostname: pytest.fail(
            "a new install must use its validated local identity"
        ),
        output=output.append,
    )

    app_config = next(
        config for config in runner.configs if 'name = "alice-forkmesh"' in config
    )
    seed_call = next(
        call
        for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_SIGNING_SEED"
    )
    public_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_PUBLIC_KEY"
    )
    assert ROUTER_PUBLIC_KEY not in app_config
    assert seed_call["stdin_text"] == ROUTER_SIGNING_SEED + "\n"
    assert runner.calls.index(seed_call) < runner.calls.index(public_call)
    assert result["routerIdentity"]["routerIdentitySource"] == "local-validated-pair"
    assert ROUTER_SIGNING_SEED not in repr(result)
    assert ROUTER_SIGNING_SEED not in "\n".join(output)
    assert all(ROUTER_SIGNING_SEED not in config for config in runner.configs)


def test_rerun_preserves_public_router_key_without_local_seed(tmp_path):
    api = FakeAPI()
    api.database_created = False
    runner = FakeRunner()
    recovery_file = tmp_path / "forkmesh.data-key"
    recovery_file.write_text("operator-recovery-value\n", encoding="utf-8")
    recovery_file.chmod(0o600)
    reads = []

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="",
            publish_mirror_manifest=False,
            data_key_backup=recovery_file,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=lambda hostname: (
            reads.append(hostname) or ROUTER_PUBLIC_KEY
        ),
        output=lambda _message: None,
    )

    app_config = next(
        config for config in runner.configs if 'name = "alice-forkmesh"' in config
    )
    assert reads == ["forkmesh.example.com"]
    assert ROUTER_PUBLIC_KEY not in app_config
    assert result["routerIdentity"]["routerIdentitySource"] == ("existing-public-endpoint")
    assert not any(
        call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] in bootstrap_module.ROUTER_IDENTITY_ENV_NAMES
        for call in runner.calls
    )


def test_partial_first_install_resumes_from_router_recovery_without_live_route(
    tmp_path,
):
    api = FakeAPI()
    api.worker_present = False
    api.route_records = []
    router_backup = tmp_path / "forkmesh.router-identity.json"
    data_backup = tmp_path / "forkmesh.data-key"

    class FailAfterAppDeploy(FakeRunner):
        def run(self, arguments, **kwargs):
            super().run(arguments, **kwargs)
            if list(arguments)[0] == "deploy" and 'name = "alice-forkmesh"' in (
                self.configs[-1]
            ):
                raise bootstrap_module.BootstrapError("injected deploy interruption")

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="injected deploy interruption",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                publish_mirror_manifest=False,
                data_key_backup=data_backup,
                router_identity_backup=router_backup,
            ),
            token="not-persisted",
            api=api,
            runner=FailAfterAppDeploy(),
            router_identity_reader=lambda _hostname: pytest.fail(
                "first install has no live identity route"
            ),
            output=lambda _message: None,
        )

    recovery = json.loads(router_backup.read_text(encoding="utf-8"))
    api.worker_present = True
    api.database_created = False
    api.secret_names = set()
    api.route_records = [{
        "pattern": "forkmesh.example.com/*",
        "script": "alice-forkmesh",
    }]
    runner = FakeRunner()
    reads = []
    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="",
            publish_mirror_manifest=False,
            data_key_backup=data_backup,
            router_identity_backup=router_backup,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=lambda hostname: (
            reads.append(hostname)
            or (_ for _ in ()).throw(
                bootstrap_module.RouterIdentityNotConfigured("HTTP 503")
            )
        ),
        output=lambda _message: None,
    )

    app_config = next(
        config for config in runner.configs
        if 'name = "alice-forkmesh"' in config
    )
    seed_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_SIGNING_SEED"
    )
    public_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "MIRROR_ROUTER_PUBLIC_KEY"
    )
    assert reads == ["forkmesh.example.com"]
    assert result["routerIdentity"]["routerIdentitySource"] == (
        "partial-install-recovery"
    )
    assert recovery["publicKey"] not in app_config
    assert seed_call["stdin_text"] == recovery["signingSeed"] + "\n"
    assert runner.calls.index(seed_call) < runner.calls.index(public_call)
    assert recovery["signingSeed"] not in repr(result)


def test_route_drift_rejects_public_identity_even_with_replace_route(tmp_path):
    api = FakeAPI()
    api.route_records = [
        {
            "pattern": "forkmesh.example.com/*",
            "script": "another-worker",
        }
    ]
    runner = FakeRunner()
    reads = []

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="unexpected Worker route overlaps the selected App hostname",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                replace_route=True,
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
                router_identity_backup=tmp_path / "forkmesh.router-identity.json",
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=lambda hostname: (
                reads.append(hostname) or ROUTER_PUBLIC_KEY
            ),
            output=lambda _message: None,
        )

    assert reads == []
    assert [call[0] for call in api.calls] == [
        "account", "zone", "worker", "routes-read", "dns-read"
    ]
    assert runner.calls == []


def test_same_script_overlapping_api_route_is_rejected_before_identity_read(
    tmp_path,
):
    api = FakeAPI()
    api.route_records = [
        {
            "pattern": "forkmesh.example.com/*",
            "script": "alice-forkmesh",
        },
        {
            "pattern": "forkmesh.example.com/api/mirrors/https*",
            "script": "alice-forkmesh-edge",
        },
        {
            "pattern": "forkmesh.example.com/api/*",
            "script": "alice-forkmesh",
        },
    ]
    reads = []
    runner = FakeRunner()

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="unexpected Worker route overlaps the selected App hostname",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                replace_route=True,
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=lambda hostname: (
                reads.append(hostname) or ROUTER_PUBLIC_KEY
            ),
            output=lambda _message: None,
        )

    assert reads == []
    assert runner.calls == []


def test_unproxied_dns_never_authorizes_public_router_identity(tmp_path):
    api = FakeAPI()
    api.dns_record_list = [{
        "id": "dns-1",
        "name": "forkmesh.example.com",
        "type": "A",
        "proxied": False,
    }]
    reads = []
    runner = FakeRunner()

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="lacks exclusively proxied Cloudflare DNS",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                replace_dns=True,
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=lambda hostname: (
                reads.append(hostname) or ROUTER_PUBLIC_KEY
            ),
            output=lambda _message: None,
        )

    assert reads == []
    assert runner.calls == []


def test_partial_install_without_proxied_dns_uses_only_pending_recovery(tmp_path):
    api = FakeAPI()
    api.database_created = False
    api.secret_names = {"DATA_KEY"}
    api.route_records = []
    api.dns_record_list = []
    router_backup = tmp_path / "forkmesh.router-identity.json"
    bootstrap_module._write_router_identity_backup(
        router_backup, ROUTER_PUBLIC_KEY, ROUTER_SIGNING_SEED
    )
    data_backup = tmp_path / "forkmesh.data-key"
    data_backup.write_text("operator-recovery-value\n", encoding="utf-8")
    data_backup.chmod(0o600)
    runner = FakeRunner()

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="",
            publish_mirror_manifest=False,
            data_key_backup=data_backup,
            router_identity_backup=router_backup,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=lambda _hostname: pytest.fail(
            "unproxied DNS must never authorize a public identity read"
        ),
        output=lambda _message: None,
    )

    assert result["routerIdentity"]["routerIdentitySource"] == (
        "partial-install-recovery"
    )
    secret_names = [
        call["arguments"][-1] for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
    ]
    assert secret_names[-2:] == [
        bootstrap_module.ROUTER_SIGNING_SEED_ENV,
        bootstrap_module.ROUTER_PUBLIC_KEY_ENV,
    ]


def test_unproxied_partial_install_cannot_override_existing_public_secret(
    tmp_path,
):
    api = FakeAPI()
    api.secret_names = {"DATA_KEY", bootstrap_module.ROUTER_PUBLIC_KEY_ENV}
    api.route_records = []
    api.dns_record_list = []
    router_backup = tmp_path / "forkmesh.router-identity.json"
    bootstrap_module._write_router_identity_backup(
        router_backup, ROUTER_PUBLIC_KEY, ROUTER_SIGNING_SEED
    )
    runner = FakeRunner()

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="cannot prove the existing public router identity",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
                router_identity_backup=router_backup,
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=lambda _hostname: pytest.fail(
                "unproxied DNS must never authorize a public identity read"
            ),
            output=lambda _message: None,
        )

    assert runner.calls == []


def test_unrecoverable_rerun_identity_fails_before_mutation_even_with_local_pair(
    monkeypatch, tmp_path
):
    monkeypatch.setenv("MIRROR_ROUTER_PUBLIC_KEY", ROUTER_PUBLIC_KEY)
    monkeypatch.setenv("MIRROR_ROUTER_SIGNING_SEED", ROUTER_SIGNING_SEED)
    api = FakeAPI()
    runner = FakeRunner()

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="existing router identity is unrecoverable",
    ) as raised:
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="",
                secret_env=bootstrap_module.ROUTER_IDENTITY_ENV_NAMES,
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=lambda _hostname: (_ for _ in ()).throw(
                bootstrap_module.BootstrapError("HTTP 503")
            ),
            output=lambda _message: None,
        )

    assert ROUTER_SIGNING_SEED not in str(raised.value)
    assert [call[0] for call in api.calls] == [
        "account", "zone", "worker", "routes-read", "dns-read"
    ]
    assert runner.calls == []
    assert not (tmp_path / "forkmesh.data-key").exists()


def test_self_host_asset_stage_is_operational_and_excludes_official_content():
    bootstrap_module.WranglerRunner().build_assets()
    with bootstrap_module.staged_public_assets(None) as staged:
        for relative in (
            "api-client.js",
            "dashboard/index.html",
            "dashboard/repo.html",
            "index.html",
            "install.sh",
            "uninstall.sh",
            "login.html",
            "signup.html",
        ):
            assert (staged / relative).is_file(), relative
        assert not (staged / "docs").exists()
        assert not (staged / "blog/one-app-one-mesh/index.html").exists()


def test_self_host_asset_copy_rewrites_official_app_and_world_links():
    bootstrap_module.WranglerRunner().build_assets()
    with bootstrap_module.staged_public_assets(None) as staged:
        with bootstrap_module.self_host_asset_copy(
            staged,
            "https://forkmesh.example.com",
            "https://forkmesh-world.example.com",
        ) as copied:
            homepage = (copied / "index.html").read_text(encoding="utf-8")
            header = (copied / "site-header.js").read_text(encoding="utf-8")
            installer = (copied / "install.sh").read_text(encoding="utf-8")
            combined = homepage + header + installer
            assert "https://app.forkmesh.com" not in combined
            assert "https://world.forkmesh.com" not in combined
            assert "https://forkmesh.com/install.sh" not in combined
            assert "https://forkmesh.example.com" in combined
            assert "https://forkmesh-world.example.com" in combined


def test_bootstrap_keeps_tokens_out_of_files_and_summary(monkeypatch):
    api = FakeAPI()
    runner = FakeRunner()
    token = "cloudflare-test-token-that-must-not-be-written"
    local_secret = "local-worker-secret-value-that-must-not-enter-config"
    monkeypatch.setenv("DATA_KEY", local_secret)
    before = set(bootstrap_module.WORKER_DIR.glob(".forkmesh-bootstrap-*.toml"))
    output = []

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="mesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="https://forkmesh.com",
            account_id="account-1",
            secret_env=("DATA_KEY",),
            mirror_public_key=PUBLIC_KEY,
            manifest_signer_command=("unused-when-injected",),
        ),
        token=token,
        api=api,
        runner=runner,
        manifest_signer=fake_manifest_signer(expect_data_hidden=True),
        health_check=lambda hostname: output.append(f"health:{hostname}"),
        readiness_check=lambda hostname, key: output.append(
            f"readiness:{hostname}:{len(key)}"
        ),
        announce=lambda hostname: (
            output.append(f"announce:{hostname}") or "pending"
        ),
        router_identity_reader=existing_router_identity,
        output=output.append,
    )

    assert api.verified
    assert result["ok"] is True
    # The launch-time join ping (adhoc #97) runs only after the instance
    # passed its health check, and its result rides the redacted summary.
    assert output.index("health:mesh.example.com") < output.index(
        "announce:mesh.example.com")
    assert any(message.startswith("readiness:mesh.example.com:") for message in output)
    assert result["joinRequest"] == {"announced": True, "status": "pending"}
    assert result["workerSecretsSet"] == ["DATA_KEY"]
    assert result["repositoryMetadataNamespaceId"] == (
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    )
    assert result["functionalReadinessVerified"] is True
    assert result["mirrorManifest"]["path"] == "/forkmesh-mirror.json"
    assert result["mirrorManifest"]["repositoryBytesInD1"] is False
    assert token not in repr(result)
    assert local_secret not in repr(result)
    assert token not in "\n".join(output)
    assert local_secret not in "\n".join(output)
    assert all(token not in config for config in runner.configs)
    assert all(local_secret not in config for config in runner.configs)
    assert runner.assets_built is True
    assert runner.hidden_build_env == ("DATA_KEY",)
    assert all(call["hidden_env_names"] == ("DATA_KEY",) for call in runner.calls)
    assert runner.manifests[0]["endpoint"]["transport"] == "direct-https"
    assert runner.manifests[0]["endpoint"]["mainProxyMode"] == "masked"
    assert runner.manifests[0]["storage"]["repositoryBytesInD1"] is False
    assert runner.manifests[0]["signature"]["value"] == SIGNATURE
    data_key_call = next(
        call for call in runner.calls
        if call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "DATA_KEY"
    )
    assert data_key_call["stdin_text"] == local_secret + "\n"
    assert set(bootstrap_module.WORKER_DIR.glob(".forkmesh-bootstrap-*.toml")) == before
    assert ("dns", "zone-1", "mesh.example.com", False) in api.calls
    assert any(call[0] == "kv" for call in api.calls)
    assert (
        "route",
        "zone-1",
        "mesh.example.com/*",
        "alice-forkmesh",
        False,
    ) in api.calls
    assert result["workerName"] == "alice-forkmesh"
    assert result["routerIdentity"]["routerIdentitySource"] == (
        "existing-public-endpoint"
    )
    assert "edgeControl" not in result
    assert not any(
        call[0] == "route" and call[2] != "mesh.example.com/*"
        for call in api.calls
    )
    deploys = [
        call for call in runner.calls if call["arguments"][0] == "deploy"
    ]
    assert len(deploys) == 1
    app_deploy = deploys[0]
    assert 'name = "alice-forkmesh"' in runner.configs[
        runner.calls.index(app_deploy)
    ]
    app_manifest = tomllib.loads(runner.configs[runner.calls.index(app_deploy)])
    for config_key, identity_key in (
        ("BUILD_REV", "buildRev"),
        ("APP_VERSION", "appVersion"),
        ("DEPLOY_FINGERPRINT", "deployFingerprint"),
    ):
        assert app_manifest["vars"][config_key] == result["releaseIdentity"][
            identity_key
        ]
    assert runner.calls.index(app_deploy) < runner.calls.index(data_key_call)


def test_dry_run_performs_only_read_only_resolution():
    api = FakeAPI()
    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="mesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="https://forkmesh.com",
            dry_run=True,
        ),
        token="not-persisted",
        api=api,
        runner=FakeRunner(),
        router_identity_reader=existing_router_identity,
        output=lambda message: None,
    )
    assert result["dryRun"] is True
    assert result["workerName"] == "alice-forkmesh"
    assert result["routerIdentity"]["routerIdentitySource"] == (
        "existing-public-endpoint"
    )
    assert "edgeControl" not in result
    assert [call[0] for call in api.calls] == [
        "account", "zone", "worker", "routes-read", "dns-read"
    ]


def test_proxied_dns_proof_ignores_unproxyable_txt_records():
    api = FakeAPI()
    api.dns_record_list = [
        {
            "name": "mesh.example.com",
            "type": "TXT",
            "proxied": False,
        },
        {
            "name": "mesh.example.com",
            "type": "A",
            "proxied": True,
        },
    ]
    reads = []

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="mesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="",
            dry_run=True,
        ),
        token="not-persisted",
        api=api,
        runner=FakeRunner(),
        router_identity_reader=lambda hostname: (
            reads.append(hostname) or ROUTER_PUBLIC_KEY
        ),
        output=lambda _message: None,
    )

    assert result["dryRun"] is True
    assert reads == ["mesh.example.com"]


def test_new_one_click_dry_run_does_not_generate_or_write_router_seed(tmp_path):
    api = FakeAPI()
    api.worker_present = False
    runner = FakeRunner()
    router_backup = tmp_path / "forkmesh.router-identity.json"

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="",
            dry_run=True,
            router_identity_backup=router_backup,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        router_identity_reader=lambda _hostname: pytest.fail(
            "new dry-run must not probe a nonexistent public endpoint"
        ),
        output=lambda _message: None,
    )

    assert result["routerIdentity"]["routerIdentitySource"] == "generated-on-apply"
    assert result["routerIdentity"]["routerIdentityBackupCreated"] is False
    assert result["secrets"] == []
    assert [call[0] for call in api.calls] == [
        "account", "zone", "worker", "routes-read", "dns-read"
    ]
    assert runner.calls == []
    assert not router_backup.exists()


def test_dry_run_describes_optional_world_without_mutation():
    api = FakeAPI()
    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="https://app.forkmesh.com",
            dry_run=True,
            deploy_world=True,
        ),
        token="not-persisted",
        api=api,
        runner=FakeRunner(),
        router_identity_reader=existing_router_identity,
        output=lambda message: None,
    )

    assert result["world"] == {
        "enabled": True,
        "hostname": "forkmesh-world.example.com",
        "workerName": "alice-forkmesh-world",
    }
    assert [call[0] for call in api.calls] == [
        "account", "zone", "worker", "routes-read", "dns-read"
    ]


def test_optional_world_deploy_uses_custom_app_origin_and_no_official_routes(
    tmp_path,
):
    api = FakeAPI()
    runner = FakeRunner()
    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="https://app.forkmesh.com",
            publish_mirror_manifest=False,
            data_key_backup=tmp_path / "forkmesh.data-key",
            deploy_world=True,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda hostname: None,
        readiness_check=lambda hostname, key: None,
        world_health_check=lambda hostname: None,
        announce=lambda hostname: "",
        router_identity_reader=existing_router_identity,
        output=lambda message: None,
    )

    assert runner.world_assets_built is True
    app_config = next(
        config for config in runner.configs
        if 'name = "alice-forkmesh"' in config
    )
    assert (
        'CORS_ALLOWED_ORIGINS = '
        '"https://forkmesh.example.com,https://forkmesh-world.example.com"'
    ) in app_config
    world_config = next(
        config for config in runner.configs
        if 'name = "alice-forkmesh-world"' in config
    )
    assert 'name = "alice-forkmesh-world"' in world_config
    assert 'APP_ORIGIN = "https://forkmesh.example.com"' in world_config
    world_manifest = tomllib.loads(world_config)
    assert world_manifest["services"] == [
        {"binding": "APP", "service": "alice-forkmesh"}
    ]
    assert world_config.count("workers_dev = false") == 1
    assert "world.forkmesh.com" not in world_config
    assert "[[routes]]" not in world_config
    world_worker_routes = world_manifest["assets"]["run_worker_first"]
    for route in ("/", "/world", "/world/", "/api", "/api/*"):
        assert route in world_worker_routes
    for route in (
        "/api-client.js",
        "/dashboard-chat.js",
        "/assets/*",
        "/favicon/*",
    ):
        assert route in world_worker_routes
    for route in ("!/assets/bot-avatars/*", "!/assets/world/*"):
        assert route in world_worker_routes
    assert "/world/world.js" not in world_worker_routes
    assert "/world/world.css" not in world_worker_routes
    assert (
        "route",
        "zone-1",
        "forkmesh-world.example.com/*",
        "alice-forkmesh-world",
        False,
    ) in api.calls
    assert result["world"]["enabled"] is True
    assert result["world"]["healthVerified"] is True


def test_app_only_install_does_not_trust_dormant_world_origin(tmp_path):
    api = FakeAPI()
    runner = FakeRunner()

    bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="",
            main_relay_url="",
            publish_mirror_manifest=False,
            data_key_backup=tmp_path / "forkmesh.data-key",
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda _hostname: None,
        readiness_check=lambda _hostname, _key: None,
        router_identity_reader=existing_router_identity,
        output=lambda _message: None,
    )

    app_configs = [
        config for config in runner.configs
        if tomllib.loads(config).get("name")
        in {"alice-forkmesh", "alice-forkmesh-edge"}
    ]
    assert len(app_configs) >= 2
    for config in app_configs:
        assert 'WORLD_ORIGIN = ""' in config
        assert (
            'CORS_ALLOWED_ORIGINS = "https://forkmesh.example.com"'
            in config
        )
        assert "forkmesh-world.example.com" not in config
        assert config.count("workers_dev = false") == 1


def test_failed_world_route_never_publishes_world_trust_to_app(tmp_path):
    class FailWorldRouteAPI(FakeAPI):
        def ensure_worker_route(
            self, zone_id, pattern, worker_name, *, replace
        ):
            if pattern == "forkmesh-world.example.com/*":
                raise bootstrap_module.BootstrapError("injected World route failure")
            return super().ensure_worker_route(
                zone_id, pattern, worker_name, replace=replace
            )

    api = FailWorldRouteAPI()
    runner = FakeRunner()

    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="injected World route failure",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="",
                main_relay_url="",
                publish_mirror_manifest=False,
                data_key_backup=tmp_path / "forkmesh.data-key",
                deploy_world=True,
            ),
            token="not-persisted",
            api=api,
            runner=runner,
            router_identity_reader=existing_router_identity,
            output=lambda _message: None,
        )

    deployed_names = [
        tomllib.loads(config).get("name")
        for call, config in zip(runner.calls, runner.configs)
        if call["arguments"][0] == "deploy"
    ]
    assert deployed_names == ["alice-forkmesh-world"]


def test_new_instance_generates_recovery_copy_and_verifies_data_key(tmp_path):
    api = FakeAPI()
    runner = FakeRunner()
    readiness = []
    recovery_file = tmp_path / "secrets" / "forkmesh.data-key"
    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="https://app.forkmesh.com",
            publish_mirror_manifest=False,
            data_key_backup=recovery_file,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda hostname: None,
        readiness_check=lambda hostname, key: readiness.append((hostname, key)),
        announce=lambda hostname: "",
        router_identity_reader=existing_router_identity,
        output=lambda message: None,
    )

    data_key_calls = [
        call for call in runner.calls
        if call["arguments"][0:3] == ["secret", "put", "--config"]
        and call["arguments"][-1] == "DATA_KEY"
    ]
    assert len(data_key_calls) == 1
    generated = data_key_calls[0]["stdin_text"].strip()
    assert len(generated) >= 40
    assert readiness == [("forkmesh.example.com", generated)]
    assert result["dataKeyGenerated"] is True
    assert result["dataKeyBackupCreated"] is True
    assert result["dataKeyRecoveryAvailable"] is True
    assert result["functionalReadinessVerified"] is True
    assert generated not in repr(result)
    assert recovery_file.read_text(encoding="utf-8").strip() == generated
    assert recovery_file.stat().st_mode & 0o777 == 0o600


def test_existing_remote_data_key_requires_local_recovery_copy():
    api = FakeAPI()
    api.database_created = False
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="no local recovery copy",
    ):
        bootstrap_module.bootstrap(
            bootstrap_module.BootstrapOptions(
                hostname="forkmesh.example.com",
                zone_name="example.com",
                worker_name="alice-forkmesh",
                database_name="alice-forkmesh",
                node_name="alice-node",
                relay_label="Alice relay",
                main_relay_url="https://app.forkmesh.com",
                publish_mirror_manifest=False,
            ),
            token="not-persisted",
            api=api,
            runner=FakeRunner(),
            router_identity_reader=existing_router_identity,
            output=lambda message: None,
        )


def test_existing_remote_data_key_is_proved_without_replacing_it(tmp_path):
    api = FakeAPI()
    api.database_created = False
    runner = FakeRunner()
    recovery_file = tmp_path / "forkmesh.data-key"
    recovery_file.write_text("operator-recovery-value\n", encoding="utf-8")
    recovery_file.chmod(0o600)
    readiness = []

    result = bootstrap_module.bootstrap(
        bootstrap_module.BootstrapOptions(
            hostname="forkmesh.example.com",
            zone_name="example.com",
            worker_name="alice-forkmesh",
            database_name="alice-forkmesh",
            node_name="alice-node",
            relay_label="Alice relay",
            main_relay_url="https://app.forkmesh.com",
            publish_mirror_manifest=False,
            data_key_backup=recovery_file,
        ),
        token="not-persisted",
        api=api,
        runner=runner,
        health_check=lambda hostname: None,
        readiness_check=lambda hostname, key: readiness.append((hostname, key)),
        announce=lambda hostname: "",
        router_identity_reader=existing_router_identity,
        output=lambda message: None,
    )

    assert readiness == [("forkmesh.example.com", "operator-recovery-value")]
    assert result["dataKeyGenerated"] is False
    assert result["dataKeyBackupCreated"] is False
    assert result["functionalReadinessVerified"] is True
    assert not any(
        call["arguments"][0:2] == ["secret", "put"]
        and call["arguments"][-1] == "DATA_KEY"
        for call in runner.calls
    )


def test_cli_without_local_manifest_identity_defaults_to_no_trusted_manifest():
    parser = bootstrap_module.build_parser()
    args = parser.parse_args(["--auto-configure"])
    assert not args.skip_mirror_manifest
    assert args.mirror_public_key == ""
    assert args.manifest_signer_command == ""


def test_cli_auto_forwards_router_identity_names_without_storing_seed(monkeypatch):
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "session-only-token")
    monkeypatch.setenv("MIRROR_ROUTER_PUBLIC_KEY", ROUTER_PUBLIC_KEY)
    monkeypatch.setenv("MIRROR_ROUTER_SIGNING_SEED", ROUTER_SIGNING_SEED)
    discovered = {
        "accountId": "account-1",
        "zoneId": "zone-1",
        "zoneName": "example.com",
        "hostname": "forkmesh.example.com",
        "directMirrorHostname": "mirror.example.com",
        "nodeName": "alice-node",
    }
    captured = {}
    monkeypatch.setattr(
        bootstrap_module,
        "discover_deployment_defaults",
        lambda **_kwargs: discovered,
    )

    def fake_bootstrap(options, **_kwargs):
        captured["options"] = options
        return {"ok": True}

    monkeypatch.setattr(bootstrap_module, "bootstrap", fake_bootstrap)

    assert bootstrap_module.main(["--auto-configure"]) == 0
    options = captured["options"]
    assert options.secret_env == bootstrap_module.ROUTER_IDENTITY_ENV_NAMES
    assert ROUTER_SIGNING_SEED not in repr(options)


def test_cli_one_click_uses_default_router_recovery_without_secret_env(
    monkeypatch, tmp_path
):
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "session-only-token")
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path))
    for name in (
        *bootstrap_module.ROUTER_IDENTITY_ENV_NAMES,
        "FORKMESH_ROUTER_IDENTITY_BACKUP",
    ):
        monkeypatch.delenv(name, raising=False)
    discovered = {
        "accountId": "account-1",
        "zoneId": "zone-1",
        "zoneName": "example.com",
        "hostname": "forkmesh.example.com",
        "directMirrorHostname": "mirror.example.com",
        "nodeName": "alice-node",
    }
    captured = {}
    monkeypatch.setattr(
        bootstrap_module,
        "discover_deployment_defaults",
        lambda **_kwargs: discovered,
    )

    def fake_bootstrap(options, **_kwargs):
        captured["options"] = options
        return {"ok": True}

    monkeypatch.setattr(bootstrap_module, "bootstrap", fake_bootstrap)

    assert bootstrap_module.main(["--auto-configure", "--json-stdout"]) == 0
    options = captured["options"]
    assert options.secret_env == ()
    assert options.router_identity_backup == (
        tmp_path
        / "forkmesh"
        / "secrets"
        / f"{options.worker_name}.router-identity.json"
    )
    assert not options.router_identity_backup.exists()


def test_token_only_discovery_is_read_only_and_derives_distinct_topology():
    api = FakeAPI()
    result = bootstrap_module.discover_deployment_defaults(
        token="session-only-token",
        api=api,
    )

    assert api.verified is True
    assert result == {
        "accountId": "account-1",
        "zoneId": "zone-1",
        "zoneName": "example.com",
        "hostname": "forkmesh.example.com",
        "directMirrorHostname": "mirror.example.com",
        "nodeName": bootstrap_module._derived_resource_name(
            "forkmesh.example.com"
        ),
    }
    assert [call[0] for call in api.calls] == ["account", "zones"]


def test_token_only_discovery_fails_closed_for_multiple_zones():
    api = FakeAPI()
    api.list_zones = lambda account_id: [
        {"id": "zone-1", "name": "one.example", "status": "active"},
        {"id": "zone-2", "name": "two.example", "status": "active"},
    ]
    with pytest.raises(
        bootstrap_module.BootstrapError,
        match="multiple or no active zones",
    ):
        bootstrap_module.discover_deployment_defaults(
            token="session-only-token",
            api=api,
        )


def test_auto_configure_parser_does_not_require_hostname_or_zone():
    args = bootstrap_module.build_parser().parse_args(
        ["--auto-configure", "--json-stdout"]
    )
    assert args.hostname == ""
    assert args.zone_name == ""
    assert args.auto_configure is True
    assert args.json_stdout is True


@pytest.mark.parametrize(
    "hostname",
    ["https://example.com", "*.example.com", "example.com/path", "-bad.example.com"],
)
def test_hostname_validation_rejects_urls_wildcards_paths_and_bad_labels(hostname):
    with pytest.raises(bootstrap_module.BootstrapError):
        bootstrap_module.normalize_hostname(hostname)


def test_secret_env_cannot_turn_cloudflare_token_into_worker_secret():
    with pytest.raises(bootstrap_module.BootstrapError):
        bootstrap_module._validate_secret_names(["CLOUDFLARE_API_TOKEN"])


def test_mirror_manifest_signer_covers_public_key_urls_and_storage_claim():
    unsigned = bootstrap_module.unsigned_mirror_manifest(
        hostname="mesh.example.com",
        node_name="alice-node",
        public_key=PUBLIC_KEY,
        generated_at="2026-07-23T10:00:00Z",
    )
    signed = fake_manifest_signer().sign(unsigned)
    payload = bootstrap_module.mirror_manifest_payload(signed)

    assert signed["node"]["publicKey"] == PUBLIC_KEY
    assert signed["endpoint"]["healthUrl"] == "https://mesh.example.com/health"
    assert (
        signed["endpoint"]["repositoryUrlTemplate"]
        == "https://mesh.example.com/{owner}/{repository}"
    )
    assert signed["dns"] == {
        "recordName": "mesh.example.com",
        "proxied": True,
        "workerRoute": "mesh.example.com/*",
    }
    assert signed["storage"]["repositoryBytesInD1"] is False
    assert b'"signature"' not in payload
    assert signed["signature"]["payloadSha256"] == __import__(
        "hashlib"
    ).sha256(payload).hexdigest()


def test_published_mirror_manifest_schema_is_strict_about_d1_and_https():
    schema = json.loads(
        (ROOT / "www" / "docs" / "mirror-endpoint.schema.json").read_text(encoding="utf-8")
    )
    storage = schema["properties"]["storage"]["properties"]
    endpoint = schema["properties"]["endpoint"]["properties"]
    assert storage["repositoryBytesInD1"]["const"] is False
    assert endpoint["transport"]["const"] == "direct-https"
    assert endpoint["mainProxyMode"]["const"] == "masked"


def test_bootstrap_docs_include_kv_data_key_and_app_origin_requirements():
    docs = (
        ROOT / "www/docs/operations/automation-and-cloudflare-bootstrap.md"
    ).read_text(encoding="utf-8")
    normalized_docs = " ".join(docs.split())
    assert "Workers KV Storage edit" in docs
    assert "generates `DATA_KEY`" in docs
    assert "--data-key-backup" in docs
    assert "mode-`0600` recovery copy" in docs
    assert "authenticated database/encryption probe" in normalized_docs
    assert "--main-relay-url https://app.forkmesh.com" in docs
    assert "MIRROR_ROUTER_PUBLIC_KEY" in docs
    assert "MIRROR_ROUTER_SIGNING_SEED" in docs
    assert "one-click installation needs no" in docs
    assert "router secret in the environment" in docs
    assert "never put in argv" in docs
    assert "signing seed" in docs
    assert "first and the public key last" in docs
    assert "public App identity route" in normalized_docs
    assert "fails before D1, DNS, routes, secrets" in normalized_docs
