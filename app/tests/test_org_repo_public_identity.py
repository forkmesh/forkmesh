import ast
import asyncio
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote, unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import repo_short_urls  # noqa: E402

SOURCES = {
    name: (ROOT / "src" / (name + ".py")).read_text(encoding="utf-8")
    for name in ("entry", "catalog_routes", "fediverse_routes")
}
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,38}$")


def _load(module, *names, **namespace):
    tree = ast.parse(SOURCES[module])
    wanted = [node for node in tree.body
              if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
              and node.name in names]
    assert len(wanted) == len(names)
    scope = dict(namespace)
    exec(compile(ast.Module(body=wanted, type_ignores=[]), module, "exec"),
         scope)
    return scope


def _run(coro):
    return asyncio.run(coro)


async def _noop(*_args, **_kwargs):
    return None


def _links(rows):
    calls = []

    async def d1_all(_env, sql, *args):
        calls.append((sql, args))
        if "FROM org_repos r JOIN orgs o" in sql and "LIMIT 2" in sql:
            node, repo = args
            return [{"name": org} for org, linked_node, linked_repo in rows
                    if linked_node == node and linked_repo.lower() == repo][:2]
        return []
    return d1_all, calls


def _identity_scope(rows, now=1000):
    d1_all, calls = _links(rows)
    scope = _load(
        "entry", "_repo_organization", "_repo_public_key",
        valid_node_name=lambda value: bool(NAME_RE.fullmatch(value or "")),
        Date=SimpleNamespace(now=lambda: now), _ORG_ALIAS_MEMO={},
        ORG_ALIAS_MEMO_TTL_MS=30000, ORG_ALIAS_MEMO_MAX=512,
        ensure_schema=_noop, d1_all=d1_all)
    return scope, calls


def test_member_address_resolves_to_the_only_linked_organization():
    scope, calls = _identity_scope([("acme", "alice", "project")])
    resolve = scope["_repo_organization"]
    assert _run(resolve(None, "Alice", "Project")) == "acme"
    assert _run(resolve(None, "alice", "project")) == "acme"
    assert len(calls) == 1
    assert _run(resolve(None, "alice", "other")) == ""
    assert _run(scope["_repo_public_key"](None, "alice/Project")) == \
        "acme/Project"
    assert _run(scope["_repo_public_key"](None, "bob/project")) == \
        "bob/project"


def test_two_organizations_keep_the_member():
    scope, _ = _identity_scope([("acme", "alice", "project"),
                                ("beta", "alice", "project")])
    assert _run(scope["_repo_organization"](None, "alice", "project")) == ""


def _public_owner_scope(rows):
    scope, _ = _identity_scope(rows)

    async def org_repo_node(_env, org, repo):
        return next((node for name, node, linked in rows
                     if name == org and linked == repo.lower()), "")
    pattern = re.compile(r"^/api/repo/([^/]+)/([^/]+)/forks$")
    public = _load(
        "entry", "_public_repo_owner", urlparse=urlparse,
        safe_segment=lambda value: value or "",
        _org_repo_node=org_repo_node,
        _repo_organization=scope["_repo_organization"])
    return public["_public_repo_owner"], pattern


def test_fork_list_and_new_forks_use_the_organization_from_either_address():
    owner_of, pattern = _public_owner_scope([("acme", "alice", "project")])
    for path in ("/api/repo/alice/project/forks",
                 "/api/repo/acme/project/forks"):
        request = SimpleNamespace(url="https://forge.test" + path)
        assert _run(owner_of(None, request, "alice", "project",
                             pattern)) == "acme"
    request = SimpleNamespace(url="https://forge.test/api/repo/bob/x/forks")
    assert _run(owner_of(None, request, "bob", "x", pattern)) == "bob"


def test_catalog_lists_the_organization_and_not_the_member():
    async def d1_all(_env, sql, *args):
        if "FROM users WHERE" in sql:
            return [{"user_bi": "bi-alice", "username": "alice"},
                    {"user_bi": "bi-bob", "username": "bob"}]
        if "FROM org_repos r" in sql:
            assert "lower(r.repo)=?" in sql
            pairs = list(zip(args[::2], args[1::2]))
            assert ("alice", "project") in pairs
            return [{"node_owner": "alice", "repo": "project",
                     "name": "acme"}]
        return []
    scope = _load(
        "entry", "_catalog_logical_owners", d1_all=d1_all,
        clean_string=lambda value, _limit: str(value or ""),
        MAX_NODE_NAME=40, MAX_REPO_SEGMENT=100,
        valid_node_name=lambda value: bool(NAME_RE.fullmatch(value or "")))
    linked = {"owner": "alice", "name": "Project"}
    plain = {"owner": "bob", "name": "tool"}
    _run(scope["_catalog_logical_owners"](
        None, [(linked, "bi-alice"), (plain, "bi-bob")]))
    assert linked["ownerKind"] == "organization"
    assert linked["logicalOwner"] == "acme"
    assert linked["logicalOwners"] == [
        {"kind": "organization", "owner": "acme"}]
    assert linked["owner"] == "alice"
    assert plain["logicalOwners"] == [{"kind": "user", "owner": "bob"}]


def test_catalog_keeps_the_member_first_for_two_organizations():
    async def d1_all(_env, sql, *args):
        if "FROM users WHERE" in sql:
            return [{"user_bi": "bi-alice", "username": "alice"}]
        if "FROM org_repos r" in sql:
            return [{"node_owner": "alice", "repo": "project", "name": org}
                    for org in ("acme", "beta")]
        return []
    scope = _load(
        "entry", "_catalog_logical_owners", d1_all=d1_all,
        clean_string=lambda value, _limit: str(value or ""),
        MAX_NODE_NAME=40, MAX_REPO_SEGMENT=100,
        valid_node_name=lambda value: bool(NAME_RE.fullmatch(value or "")))
    linked = {"owner": "alice", "name": "Project"}
    _run(scope["_catalog_logical_owners"](None, [(linked, "bi-alice")]))
    assert linked["ownerKind"] == "user"
    assert linked["logicalOwner"] == "alice"
    assert linked["logicalOwners"] == [
        {"kind": "user", "owner": "alice"},
        {"kind": "organization", "owner": "acme"},
        {"kind": "organization", "owner": "beta"}]


def test_catalog_projection_leaves_signed_fields_alone():
    scope = _load(
        "catalog_routes", "_catalog_organization_projection",
        _ssh_alias_repository_url=lambda _env, org, _owner, name:
            "ssh://git@forge.test/%s/%s.git" % (org, name))
    hosted = {"owner": "alice", "name": "project", "ownerKind":
              "organization", "logicalOwner": "acme",
              "channel": "#alice-project", "signature": "", "stateSig": ""}
    signed = dict(hosted, name="signed", channel="#alice-signed",
                  stateSig="sig")
    scope["_catalog_organization_projection"](
        None, [hosted, signed], {"configured": True})
    assert hosted["channel"] == "#acme-project"
    assert hosted["sshUrl"] == "ssh://git@forge.test/acme/project.git"
    assert signed["channel"] == "#alice-signed"
    assert hosted["owner"] == signed["owner"] == "alice"


def test_old_fork_edges_are_keyed_on_the_organization():
    writes = []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    async def blind_index(_env, value):
        return "bi:" + value.lower()

    async def public_key(_env, key):
        return "acme/project" if key == "alice/project" else key
    scope = _load(
        "entry", "_index_repo_fork_edge", d1_run=d1_run,
        blind_index=blind_index, _repo_public_key=public_key,
        clean_string=lambda value, _limit: str(value or ""),
        MAX_REPO_SEGMENT=100, Date=SimpleNamespace(now=lambda: 5))
    record = {"forkedFrom": "alice/project", "owner": "bob",
              "name": "project", "visibility": "public"}
    _run(scope["_index_repo_fork_edge"](
        None, record, {}, "bi-bob", "bi:bob/project"))
    insert = next(args for sql, args in writes if sql.startswith("INSERT"))
    assert insert[0] == "bi:acme/project"
    assert record["forkedFrom"] == "alice/project"


def test_member_page_redirects_to_the_organization_without_short_addresses():
    scope, _ = _identity_scope([("acme", "alice", "project")])

    async def setting(_env):
        return {}

    async def private(_env, owner, name):
        return name == "secret"
    paths = _load(
        "entry", "_repo_canonical_path", "_repo_organization_path",
        _repo_short_setting_row=setting, repo_short_urls=repo_short_urls,
        _repo_organization=scope["_repo_organization"],
        _repo_is_private=private, unquote=unquote, quote=quote)
    canonical = paths["_repo_canonical_path"]
    assert _run(canonical(None, "/alice/project/issues/3", "tab=1")) == \
        "/acme/project/issues/3?tab=1"
    assert _run(canonical(None, "/acme/project")) is None
    assert _run(canonical(None, "/bob/project")) is None
