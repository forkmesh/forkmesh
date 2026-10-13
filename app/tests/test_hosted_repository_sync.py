import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
from urllib import request as urlrequest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "hosted_repository_sync.py"
SPEC = importlib.util.spec_from_file_location(
    "forkmesh_hosted_repository_sync_test",
    SCRIPT,
)
sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync)


PROVIDER_URLS = {
    "codeberg": "https://codeberg.org/m33/",
    "github": "https://github.com/spidertest/",
    "gitlab": "https://gitlab.com/group/subgroup/",
}


def _import(name, suffix, provider="codeberg"):
    return {
        "id": "ext_" + suffix * 24,
        "provider": provider,
        "isPrivate": False,
        "status": "external_repository",
        "name": name,
        "originalUrl": PROVIDER_URLS[provider] + name,
        "createdAt": 1785000000000,
        "metadata": {"defaultBranch": "main", "description": name},
    }


def test_prepare_keeps_healthy_imports_when_one_provider_clone_fails(
    tmp_path, monkeypatch
):
    gateway = tmp_path / "gateway"
    gateway.mkdir()
    config = SimpleNamespace(
        node_owner="mirror2",
        gateway_config_path=gateway / "mirror-gateway.json",
        worker_origin="https://app.forkmesh.com",
    )
    monkeypatch.setattr(sync, "SOURCE_ROOT", tmp_path / "imports")
    monkeypatch.setattr(
        sync,
        "_fetch_imports",
        lambda _url: [_import("healthy", "a"), _import("unavailable", "b")],
    )

    def prepare_repository(_config, item):
        if item["name"] == "unavailable":
            raise sync.SyncError("hosted repository operation failed")
        return True

    monkeypatch.setattr(sync, "_prepare_repository", prepare_repository)
    monkeypatch.setattr(sync, "_refs_hash", lambda *_args: "c" * 64)

    result = sync.prepare(config)

    assert result == {
        "ok": True,
        "changed": True,
        "repositoryCount": 1,
        "removedCount": 0,
        "failedCount": 1,
        "deferredCount": 0,
    }
    sidecar = json.loads(
        (gateway / sync.SIDECAR_FILE).read_text(encoding="utf-8")
    )
    assert [item["name"] for item in sidecar["repositories"]] == ["healthy"]


def test_import_sync_never_touches_push_created_repositories(
    tmp_path, monkeypatch
):
    """Provider imports and push-created repositories share no state.

    The import lifecycle owns hosted-repositories.json under SOURCE_ROOT and
    publishes a node-signed catalog record. Push-created repositories live
    under the gateway's separate writableRepositoryRoot, are recorded in
    hosted-pushes.json by the gateway process, and have their catalog record
    published by the device that pushed them. Neither sweep may reach the
    other's storage or state.
    """
    gateway = tmp_path / "gateway"
    gateway.mkdir()
    imports_root = tmp_path / "imports"
    writable_root = tmp_path / "writable"
    config = SimpleNamespace(
        node_owner="mirror2",
        gateway_config_path=gateway / "mirror-gateway.json",
        worker_origin="https://app.forkmesh.com",
    )
    monkeypatch.setattr(sync, "SOURCE_ROOT", imports_root)

    pushed = writable_root / "mirror2" / "phone.git"
    pushed.mkdir(parents=True)
    (pushed / "HEAD").write_text("ref: refs/heads/main\n", encoding="utf-8")
    pushes = gateway / sync.refresh.HOSTED_PUSHES_FILE
    pushes.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": sync.refresh.HOSTED_PUSHES_TYPE,
                "repositories": [
                    {
                        "owner": "mirror2",
                        "name": "phone",
                        "gitDir": str(pushed),
                        "firstPushedAt": 1,
                        "lastPushedAt": 1,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    pushes.chmod(0o600)
    before = pushes.read_bytes()

    # A stale abandoned import is still swept from the imports root only.
    stale = imports_root / "mirror2" / "gone.git"
    stale.mkdir(parents=True)
    sync._atomic_json(
        stale / sync.MARKER_FILE,
        {"schemaVersion": 1, "sourceUrl": "https://codeberg.org/x/gone"},
    )
    (gateway / sync.SIDECAR_FILE).write_text(
        '{"repositories":[]}\n', encoding="utf-8"
    )
    (gateway / sync.SIDECAR_FILE).chmod(0o600)

    monkeypatch.setattr(sync, "_fetch_imports", lambda _url: [])
    prepared = sync.prepare(config)
    assert prepared["repositoryCount"] == 0
    assert pushes.read_bytes() == before

    cleaned = sync.cleanup(config)
    assert cleaned == {"ok": True, "removed": 1}
    assert not stale.exists()
    assert pushed.is_dir()
    assert pushes.read_bytes() == before


def test_delete_catalog_identifies_the_headless_sync_client(monkeypatch):
    config = SimpleNamespace(
        worker_origin="https://forkmesh.com",
    )
    monkeypatch.setattr(
        sync.refresh,
        "_helper_call",
        lambda *_args: {"signature": "signed-delete"},
    )
    requests = []

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def read(self, _limit):
            return b'{"ok":true,"deleted":true}'

    def open_request(request, timeout):
        assert isinstance(request, urlrequest.Request)
        assert timeout == 30
        requests.append(request)
        return Response()

    monkeypatch.setattr(sync.urlrequest, "urlopen", open_request)

    sync._delete_catalog(config, "mirror2", "example")

    assert len(requests) == 1
    request = requests[0]
    assert request.method == "DELETE"
    assert request.get_header("Accept") == "application/json"
    assert request.get_header("User-agent") == "ForkMesh-hosted-import-sync/1.0"
    assert "owner=mirror2" in request.full_url
    assert "name=example" in request.full_url


def test_register_persists_catalog_deletion_progress(tmp_path, monkeypatch):
    gateway = tmp_path / "gateway"
    gateway.mkdir()
    config = SimpleNamespace(
        gateway_config_path=gateway / "mirror-gateway.json",
        node_owner="mirror2",
    )
    (gateway / sync.SIDECAR_FILE).write_text(
        '{"repositories":[]}\n',
        encoding="utf-8",
    )
    (gateway / sync.SIDECAR_FILE).chmod(0o600)
    pending_path = gateway / sync.PENDING_FILE
    pending_path.write_text(
        json.dumps(
            {
                "repositories": [
                    {"owner": "mirror2", "name": "first"},
                    {"owner": "mirror2", "name": "second"},
                    {"owner": "mirror2", "name": "third"},
                ]
            }
        ),
        encoding="utf-8",
    )
    pending_path.chmod(0o600)
    monkeypatch.setattr(sync.refresh, "_load_public_identity", lambda _config: {})
    deleted = []

    def delete_catalog(_config, _owner, name):
        deleted.append(name)
        if name == "second":
            raise sync.SyncError("hosted repository catalog deletion failed")

    monkeypatch.setattr(sync, "_delete_catalog", delete_catalog)

    try:
        sync.register(config)
    except sync.SyncError:
        pass
    else:
        raise AssertionError("the simulated second deletion must fail")

    assert deleted == ["first", "second"]
    pending = json.loads(pending_path.read_text(encoding="utf-8"))
    assert [item["name"] for item in pending["repositories"]] == [
        "second",
        "third",
    ]


def test_clean_imports_hosts_every_supported_public_provider(
    tmp_path, monkeypatch
):
    """A GitHub or GitLab import is materialized like a Codeberg one.

    Hosting only Codeberg imports left every GitHub import as provider-only
    metadata, so importing a repository never ended that provider dependency
    (issue #679).
    """
    monkeypatch.setattr(sync, "SOURCE_ROOT", tmp_path / "imports")
    config = SimpleNamespace(
        node_owner="mirror37",
        gateway_config_path=tmp_path / "gateway" / "mirror-gateway.json",
        worker_origin="https://app.forkmesh.com",
    )

    cleaned = sync._clean_imports(
        [
            _import("from-github", "a", "github"),
            _import("from-gitlab", "b", "gitlab"),
            _import("from-codeberg", "c", "codeberg"),
        ],
        config,
    )

    assert [(item["name"], item["provider"]) for item in cleaned] == [
        ("from-codeberg", "codeberg"),
        ("from-github", "github"),
        ("from-gitlab", "gitlab"),
    ]
    assert [item["sourceUrl"] for item in cleaned] == [
        "https://codeberg.org/m33/from-codeberg",
        "https://github.com/spidertest/from-github",
        "https://gitlab.com/group/subgroup/from-gitlab",
    ]


def test_clean_imports_rejects_unhostable_provider_entries(
    tmp_path, monkeypatch
):
    monkeypatch.setattr(sync, "SOURCE_ROOT", tmp_path / "imports")
    config = SimpleNamespace(
        node_owner="mirror37",
        gateway_config_path=tmp_path / "gateway" / "mirror-gateway.json",
        worker_origin="https://app.forkmesh.com",
    )
    private = _import("private", "a", "github")
    private["isPrivate"] = True
    archived = _import("archived", "b", "github")
    archived["status"] = "archived"
    impostor = _import("impostor", "c", "github")
    impostor["originalUrl"] = "https://github.com.example.net/x/impostor"
    renamed = _import("renamed", "d", "github")
    renamed["originalUrl"] = "https://github.com/spidertest/other"
    deep = _import("deep", "e", "github")
    deep["originalUrl"] = "https://github.com/spidertest/group/deep"
    unknown = _import("unknown", "f", "github")
    unknown["provider"] = "bitbucket"
    credentialed = _import("credentialed", "0", "github")
    credentialed["originalUrl"] = (
        "https://token@github.com/spidertest/credentialed")

    cleaned = sync._clean_imports(
        [private, archived, impostor, renamed, deep, unknown, credentialed],
        config,
    )

    assert cleaned == []


def test_imports_url_follows_the_node_configuration(monkeypatch):
    """The listing URL is the node's own relay origin, not a fixed host.

    A pinned hostname silently stopped matching when the relay moved to
    app.forkmesh.com, so every hosted-import run failed its own validation
    before requesting anything.
    """
    config = SimpleNamespace(worker_origin="https://app.forkmesh.com")
    url = sync._imports_url(config)
    assert url == "https://app.forkmesh.com/api/repository-imports"

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return None

        def read(self, _limit):
            return b'{"ok":true,"repositories":[{"id":"ext_1"}]}'

    requested = []

    def open_request(request, timeout):
        requested.append(request.full_url)
        assert timeout == 30
        return Response()

    monkeypatch.setattr(sync.urlrequest, "urlopen", open_request)

    assert sync._fetch_imports(url) == [{"id": "ext_1"}]
    assert requested == [url]

    for rejected in (
        "http://app.forkmesh.com/api/repository-imports",
        "https://app.forkmesh.com/api/repositories",
        "https://user:pass@app.forkmesh.com/api/repository-imports",
        "https://app.forkmesh.com/api/repository-imports?owner=x",
    ):
        try:
            sync._fetch_imports(rejected)
        except sync.SyncError:
            continue
        raise AssertionError(f"{rejected} must be refused")
    assert requested == [url]


def test_prepare_defers_new_clones_once_the_imports_root_is_full(
    tmp_path, monkeypatch
):
    """Hosting every public import may not fill the node's disk.

    An already-hosted repository keeps being refreshed; a new one waits for
    capacity and stays an external entry until then.
    """
    gateway = tmp_path / "gateway"
    gateway.mkdir()
    imports_root = tmp_path / "imports"
    config = SimpleNamespace(
        node_owner="mirror37",
        gateway_config_path=gateway / "mirror-gateway.json",
        worker_origin="https://app.forkmesh.com",
    )
    monkeypatch.setattr(sync, "SOURCE_ROOT", imports_root)
    monkeypatch.setattr(sync, "MAX_HOSTED_TOTAL_BYTES", 1024)
    existing = imports_root / "mirror37" / "hosted.git"
    existing.mkdir(parents=True)
    (existing / "HEAD").write_bytes(b"x" * 4096)
    monkeypatch.setattr(
        sync,
        "_fetch_imports",
        lambda _url: [
            _import("hosted", "a", "github"),
            _import("waiting", "b", "github"),
        ],
    )
    prepared = []

    def prepare_repository(_config, item):
        prepared.append(item["name"])
        return True

    monkeypatch.setattr(sync, "_prepare_repository", prepare_repository)
    monkeypatch.setattr(sync, "_refs_hash", lambda *_args: "c" * 64)

    result = sync.prepare(config)

    assert prepared == ["hosted"]
    assert result["repositoryCount"] == 1
    assert result["deferredCount"] == 1
    sidecar = json.loads(
        (gateway / sync.SIDECAR_FILE).read_text(encoding="utf-8")
    )
    assert [item["name"] for item in sidecar["repositories"]] == ["hosted"]
