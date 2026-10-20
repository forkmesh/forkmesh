"""Website forks of hosted repositories (docs/HOSTED_REPOS.md, "Forks").

A Fork click on a public hosted repository creates a hosted repository the
caller owns, marked forkedFrom the parent and indexed on its forks list. The
replica exchange names the parent and the fork network so every core builds
the fork from its own copy and a shared object pool
(server/hosted_forks.go). Runs against the real SQLite world of
test_hosted_core.
"""

import asyncio
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tests") not in sys.path:
    sys.path.insert(0, str(ROOT / "tests"))

from test_hosted_core import (  # noqa: E402
    CORES, NOW, SESSION_TOKEN, Env, Request, World, _async, create, install,
    signed_replicas,
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
