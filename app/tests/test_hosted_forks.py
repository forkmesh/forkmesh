"""Website forks of hosted repositories (docs/HOSTED_REPOS.md, "Forks").

A Fork click on a public hosted repository creates a hosted repository the
caller owns, marked forkedFrom the parent and indexed on its forks list. The
replica exchange names the parent and the fork network so every core builds
the fork from its own copy and a shared object pool
(server/hosted_forks.go). Runs against the real SQLite world of
test_hosted_core.
"""

import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tests") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests"))

from test_hosted_core import (  # noqa: E402
    CORES, NOW, SESSION_TOKEN, Env, Request, World, _async, create,
    hosted_repos, install, signed_replicas,
)

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
NETWORK_JS = (
    ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
).read_text(encoding="utf-8")
CONTENT_JS = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
HELPERS_JS = (
    ROOT / "public" / "dashboard" / "js" / "02-helpers.js"
).read_text(encoding="utf-8")
ACCOUNT_JS = (
    ROOT / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")


class ForkEnv(Env):
    HOSTED_MAX_FORKS_PER_ACCOUNT = "2"


def _world(**accounts):
    world = World()
    module = install(world)
    users = {
        "alice": {"name": "alice", "status": "active", "kind": "user",
                  "email_verified": True},
        "bob": {"name": "bob", "status": "active", "kind": "user",
                "email_verified": True},
        "carol": {"name": "carol", "status": "active", "kind": "user",
                  "email_verified": True},
    }
    users.update(accounts)
    world.edges = []
    world.private = set()

    async def _account_session_record(_env, _request, _data=None):
        rec = users.get(world.account)
        return ("bi:" + world.account, rec) if rec else (None, None)

    async def _account_owns_node(_env, actor, owner):
        return bool(actor) and actor == world.account and owner == actor

    async def _repo_is_private(_env, owner, repo):
        return (owner + "/" + repo).lower() in world.private

    async def _index_repo_fork_edge(_env, record, prior, account_bi, repo_bi):
        world.edges.append((record["forkedFrom"], record["owner"],
                            record["name"], account_bi, repo_bi))

    module._account_session_record = _account_session_record
    module._account_owns_node = _account_owns_node
    module._account_kind = lambda rec: (rec or {}).get("kind", "node")
    module._repo_is_private = _repo_is_private
    module._index_repo_fork_edge = _index_repo_fork_edge
    module.bounded_json_request = lambda request: _async(dict(request.body))
    for name in ("_web_fork_create", "_web_fork_eligible_nodes"):
        module.__dict__.pop(name, None)
    return world, module


def _fork(module, world, account, owner, repo, body=None, env=None):
    world.account = account
    payload = {"sessionToken": SESSION_TOKEN}
    payload.update(body or {})
    return asyncio.run(module.hosted_fork_create(
        env or ForkEnv(), Request(payload), owner, owner, repo))


def _parent(world, module, name="app"):
    world.account = "alice"
    response = create(module, {"name": name, "readme": True})
    assert response.status == 201, response.data
    return world.hosted("alice", name)["id"]


def test_a_fork_is_a_hosted_repository_the_caller_owns():
    world, module = _world()
    parent_id = _parent(world, module)
    response = _fork(module, world, "bob", "alice", "app")
    assert response.status == 201, response.data
    fork = response.data["fork"]
    assert fork["owner"] == "bob" and fork["name"] == "app"
    assert fork["forkedFrom"] == "alice/app"
    assert fork["url"] == "/bob/app" and fork["status"] == "completed"
    assert response.data["queued"] is False
    hosted = world.hosted("bob", "app")
    assert hosted["visibility"] == "public"
    assert hosted["creator_bi"] == "bi:bob"
    assert hosted["data"]["fork"] == {"of": parent_id, "network": parent_id}
    assert hosted["data"]["init"] is None
    record, private = world.catalog("bob", "app")
    assert record["forkedFrom"] == "alice/app" and not private
    assert record["source"] == "hosted-web"
    assert world.edges == [("alice/app", "bob", "app", "bi:bob",
                            "bi:bob/app")]
    assert sorted(node for node, repo, topic in world.doorbells
                  if repo == "bob/app") == sorted(CORES)


def test_forking_again_links_to_the_existing_fork():
    world, module = _world()
    _parent(world, module)
    assert _fork(module, world, "bob", "alice", "app").status == 201
    again = _fork(module, world, "bob", "alice", "app",
                  {"name": "other-name"})
    assert again.status == 200
    assert again.data["existing"] is True
    assert again.data["fork"]["url"] == "/bob/app"
    assert world.hosted("bob", "other-name") is None


def test_every_core_is_told_to_build_the_fork_from_the_network():
    world, module = _world()
    parent_id = _parent(world, module)
    _fork(module, world, "bob", "alice", "app")
    fork_id = world.hosted("bob", "app")["id"]
    _fork(module, world, "carol", "bob", "app")
    second_id = world.hosted("carol", "app")["id"]
    assert world.hosted("carol", "app")["data"]["fork"] == {
        "of": fork_id, "network": parent_id}
    for index, node in enumerate(CORES):
        items = {item["id"]: item for item in signed_replicas(
            module, node, {},
            request_id="request_0123456789%02d" % index).data["repositories"]}
        assert "forkOf" not in items[parent_id]
        assert (items[fork_id]["forkOf"], items[fork_id]["network"]) == (
            parent_id, parent_id)
        assert (items[second_id]["forkOf"], items[second_id]["network"]) == (
            fork_id, parent_id)
        assert "init" not in items[fork_id]


def test_a_fork_of_a_parent_made_private_is_not_built_from_it():
    world, module = _world()
    parent_id = _parent(world, module)
    _fork(module, world, "bob", "alice", "app")
    fork_id = world.hosted("bob", "app")["id"]
    world.db.execute(
        "UPDATE hosted_repos SET visibility='private' WHERE id=?", (parent_id,))
    items = {item["id"]: item for item in signed_replicas(
        module, CORES[0], {},
        request_id="request_private_000").data["repositories"]}
    assert "forkOf" not in items[fork_id] and "network" not in items[fork_id]


def test_only_verified_active_users_fork_public_hosted_repositories():
    world, module = _world(bob={"name": "bob", "status": "active",
                                "kind": "user", "email_verified": False})
    _parent(world, module)
    unverified = _fork(module, world, "bob", "alice", "app")
    assert unverified.status == 403
    assert unverified.data["error"] == "email_verification_required"

    world, module = _world()
    _parent(world, module)
    world.private.add("alice/app")
    private = _fork(module, world, "bob", "alice", "app")
    assert private.status == 403 and private.data["error"] == "fork_unavailable"

    world, module = _world()
    _parent(world, module)
    own = _fork(module, world, "alice", "alice", "app")
    assert own.status == 409

    world, module = _world()
    world.account = "bob"
    # Not a hosted repository: left to the managed-mirror fork path with its
    # request body unread.
    assert asyncio.run(module.hosted_fork_create(
        ForkEnv(), Request({}), "alice", "alice", "missing")) is None


CLONE_URL = "https://app.forkmesh.test/forkmesh/forkmesh"


def _node_repo(world, owner="forkmesh", name="forkmesh", clone_url=CLONE_URL,
               visibility="public", source="local-node"):
    record = {"owner": owner, "name": name, "visibility": visibility,
              "cloneUrl": clone_url, "source": source,
              "description": "Published from a node"}
    world.db.execute(
        "INSERT INTO repositories VALUES (?,?,?,?)",
        ("bi:%s/%s" % (owner, name), "bi:" + owner, json.dumps(record),
         0 if visibility == "public" else 1))


def test_a_node_published_repository_forks_onto_the_cores():
    world, module = _world()
    _node_repo(world)
    response = _fork(module, world, "bob", "forkmesh", "forkmesh")
    assert response.status == 201, response.data
    assert response.data["fork"]["forkedFrom"] == "forkmesh/forkmesh"
    assert response.data["fork"]["url"] == "/bob/forkmesh"
    hosted = world.hosted("bob", "forkmesh")
    network = hosted_repos.upstream_network("forkmesh", "forkmesh")
    assert hosted["data"]["fork"] == {
        "upstream": {"owner": "forkmesh", "name": "forkmesh",
                     "cloneUrl": CLONE_URL},
        "network": network}
    assert hosted["data"]["init"] is None
    record, private = world.catalog("bob", "forkmesh")
    assert record["forkedFrom"] == "forkmesh/forkmesh" and not private
    assert world.edges == [("forkmesh/forkmesh", "bob", "forkmesh",
                            "bi:bob", "bi:bob/forkmesh")]
    again = _fork(module, world, "bob", "forkmesh", "forkmesh",
                  {"name": "other"})
    assert again.status == 200 and again.data["existing"] is True
    assert world.hosted("bob", "other") is None


def test_every_core_is_told_where_the_node_published_parent_lives():
    world, module = _world()
    _node_repo(world)
    _fork(module, world, "bob", "forkmesh", "forkmesh")
    fork_id = world.hosted("bob", "forkmesh")["id"]
    _fork(module, world, "carol", "bob", "forkmesh")
    second_id = world.hosted("carol", "forkmesh")["id"]
    network = hosted_repos.upstream_network("forkmesh", "forkmesh")
    for index, node in enumerate(CORES):
        items = {item["id"]: item for item in signed_replicas(
            module, node, {},
            request_id="request_upstream_%02d" % index).data["repositories"]}
        assert items[fork_id]["forkUpstream"] == {
            "owner": "forkmesh", "name": "forkmesh", "cloneUrl": CLONE_URL}
        assert items[fork_id]["network"] == network
        assert "forkOf" not in items[fork_id] and "init" not in items[fork_id]
        assert (items[second_id]["forkOf"], items[second_id]["network"]) == (
            fork_id, network)


def test_a_node_published_parent_made_private_is_not_named():
    world, module = _world()
    _node_repo(world)
    _fork(module, world, "bob", "forkmesh", "forkmesh")
    fork_id = world.hosted("bob", "forkmesh")["id"]
    world.private.add("forkmesh/forkmesh")
    items = {item["id"]: item for item in signed_replicas(
        module, CORES[0], {},
        request_id="request_upstream_private").data["repositories"]}
    assert "forkUpstream" not in items[fork_id]
    assert "network" not in items[fork_id]


def test_only_public_node_repositories_with_a_clean_clone_url_fork():
    for clone_url in ("http://app.forkmesh.test/forkmesh/forkmesh",
                      "https://user:pw@app.forkmesh.test/forkmesh/forkmesh",
                      "https://app.forkmesh.test/forkmesh/forkmesh?x=1",
                      "https://app.forkmesh.test/forkmesh/forkmesh#main",
                      "https://app.forkmesh.test/../forkmesh",
                      "https://127.0.0.1/forkmesh/forkmesh",
                      "https://[::1]/forkmesh/forkmesh",
                      "https://localhost/forkmesh/forkmesh", ""):
        world, module = _world()
        _node_repo(world, clone_url=clone_url)
        world.account = "bob"
        assert asyncio.run(module.hosted_fork_create(
            ForkEnv(), Request({}), "forkmesh", "forkmesh",
            "forkmesh")) is None, clone_url
    world, module = _world()
    _node_repo(world, visibility="private")
    assert asyncio.run(module.hosted_fork_create(
        ForkEnv(), Request({}), "forkmesh", "forkmesh", "forkmesh")) is None
    world, module = _world()
    _node_repo(world)
    world.private.add("forkmesh/forkmesh")
    assert asyncio.run(module.hosted_fork_create(
        ForkEnv(), Request({}), "forkmesh", "forkmesh", "forkmesh")) is None
    world, module = _world(forkmesh={"name": "forkmesh", "status": "active",
                                     "kind": "user", "email_verified": True})
    _node_repo(world)
    own = _fork(module, world, "forkmesh", "forkmesh", "forkmesh")
    assert own.status == 409 and own.data["error"] == "fork_of_own_repository"


def test_an_account_with_a_managed_mirror_keeps_the_mirror_fork():
    world, module = _world()
    _node_repo(world)
    calls = []

    async def _web_fork_eligible_nodes(_env, actor, _account):
        return ["bob-mirror"] if actor == "bob" else []

    async def _web_fork_create(_env, _request, owner, repo, data=None):
        calls.append((owner, repo, dict(data or {})))
        return "managed"

    module._web_fork_eligible_nodes = _web_fork_eligible_nodes
    module._web_fork_create = _web_fork_create
    assert _fork(module, world, "bob", "forkmesh", "forkmesh") == "managed"
    assert calls == [("forkmesh", "forkmesh",
                      {"sessionToken": SESSION_TOKEN})]
    assert world.hosted("bob", "forkmesh") is None
    chosen = _fork(module, world, "carol", "forkmesh", "forkmesh",
                   {"targetNode": "carol-mirror"})
    assert chosen == "managed" and calls[-1][2]["targetNode"] == "carol-mirror"
    assert _fork(module, world, "carol", "forkmesh", "forkmesh").status == 201


def test_upstream_forks_count_against_the_fork_quota():
    world, module = _world()
    _node_repo(world, name="one")
    _node_repo(world, name="two")
    _node_repo(world, name="three")
    assert _fork(module, world, "bob", "forkmesh", "one").status == 201
    assert _fork(module, world, "bob", "forkmesh", "two").status == 201
    over = _fork(module, world, "bob", "forkmesh", "three")
    assert over.status == 429 and over.data["error"] == "fork_quota_exceeded"


def test_fork_clone_url_and_network_are_stable():
    assert hosted_repos.fork_clone_url(
        "https://App.ForkMesh.test:8443/forkmesh/forkmesh.git/") == (
        "https://app.forkmesh.test:8443/forkmesh/forkmesh.git")
    network = hosted_repos.upstream_network("ForkMesh", "ForkMesh")
    assert network == hosted_repos.upstream_network("forkmesh", "forkmesh")
    assert hosted_repos.ID_RE.fullmatch(network)


def test_fork_quota_and_name_collisions():
    world, module = _world()
    for name in ("one", "two", "three"):
        _parent(world, module, name)
    assert _fork(module, world, "bob", "alice", "one").status == 201
    assert _fork(module, world, "bob", "alice", "two").status == 201
    over = _fork(module, world, "bob", "alice", "three")
    assert over.status == 429 and over.data["error"] == "fork_quota_exceeded"

    # Forks never use up the ordinary repository quota.
    world.account = "bob"
    assert create(module, {"name": "mine", "readme": True}).status == 201

    world, module = _world()
    _parent(world, module)
    world.account = "bob"
    assert create(module, {"name": "app", "readme": True}).status == 201
    taken = _fork(module, world, "bob", "alice", "app")
    assert taken.status == 409 and taken.data["error"] == "fork_name_taken"
    named = _fork(module, world, "bob", "alice", "app", {"name": "app-fork"})
    assert named.status == 201
    assert named.data["fork"]["url"] == "/bob/app-fork"


def test_the_forks_route_prefers_the_hosted_fork():
    handler = ENTRY.split("async def repo_forks_handler(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "await hosted_fork_create(" in handler
    assert handler.index("hosted_fork_create(") < handler.index(
        "_web_fork_create(")
    assert 'hosted_fork_create = _hosted_git.export("hosted_fork_create")' \
        in ENTRY


def test_the_page_offers_fork_and_contribute():
    assert "data-repo-fork-create" in NETWORK_JS
    assert "Fork this repository" in NETWORK_JS
    assert "data.existing" in CONTENT_JS
    assert "data-repo-contribute" in HELPERS_JS
    assert "Open pull request" in HELPERS_JS
    assert "Fork the repository" in ACCOUNT_JS
