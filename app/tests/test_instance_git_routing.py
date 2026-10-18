import ast
import asyncio
import json
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
INSTALLER = (ROOT.parent / "www" / "public" / "install-instance.sh").read_text(
    encoding="utf-8")
NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REQUIRED_OPERATIONS = {"git-info-refs", "git-upload-pack"}


def load(names, extra):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra)
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(ENTRY_PATH), "exec"), namespace)
    return namespace


BASE = {
    "clean_string": lambda value, limit: str(value or "")[:limit],
    "MAX_NODE_NAME": 63,
    "valid_node_name": lambda value: bool(NODE_RE.fullmatch(value or "")),
    "urlparse": urlparse,
    "re": re,
    "json": json,
}


def quorum_namespace(rows):
    async def d1_all(_env, _sql, *_args):
        return rows

    extra = dict(BASE, d1_all=d1_all,
                 Date=SimpleNamespace(now=lambda: 1_000_000),
                 https_routing=SimpleNamespace(ENDPOINT_STALE_MS=600_000),
                 HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS=REQUIRED_OPERATIONS)
    return load({"_https_mirror_flagship_quorum", "_instance_solo_flagship_node",
                 "_instance_flagship_node"}, extra)


def row(node, digest):
    return {"node_name": node, "checked_at": 999_000,
            "forkmesh_refs_sha256": digest,
            "forkmesh_operations_json": json.dumps(sorted(REQUIRED_OPERATIONS))}


def test_one_node_never_forms_a_quorum_on_forkmesh_com():
    ns = quorum_namespace([row("fmnext-n1", "a" * 64)])
    result = asyncio.run(ns["_https_mirror_flagship_quorum"](SimpleNamespace()))
    assert result["pins"] == frozenset()
    assert result["nodeDigests"] == {"fmnext-n1": "a" * 64}


def test_an_instance_trusts_its_own_named_flagship_node_alone():
    rows = [row("forkmesh", "a" * 64), row("stranger", "b" * 64)]
    ns = quorum_namespace(rows)
    env = SimpleNamespace(INSTANCE_FLAGSHIP_NODE="forkmesh")
    result = asyncio.run(ns["_https_mirror_flagship_quorum"](env))
    assert result["pins"] == frozenset({"a" * 64})
    assert result["nodes"] == frozenset({"forkmesh"})


def test_two_agreeing_mirrors_still_form_a_quorum():
    ns = quorum_namespace([row("n1", "c" * 64), row("n2", "c" * 64)])
    result = asyncio.run(ns["_https_mirror_flagship_quorum"](SimpleNamespace()))
    assert result["pins"] == frozenset({"c" * 64})


def test_flagship_node_defaults_to_forkmesh_but_solo_trust_needs_the_setting():
    ns = quorum_namespace([])
    assert ns["_instance_flagship_node"](SimpleNamespace()) == "forkmesh"
    assert ns["_instance_solo_flagship_node"](SimpleNamespace()) == ""
    env = SimpleNamespace(INSTANCE_FLAGSHIP_NODE="Bad Name!")
    assert ns["_instance_flagship_node"](env) == "forkmesh"
    assert ns["_instance_solo_flagship_node"](env) == ""


def test_a_mirror_on_the_apps_own_origin_is_proxied_not_redirected():
    ns = load({"_mirror_origin_is_this_app"}, BASE)
    same = ns["_mirror_origin_is_this_app"]
    request = SimpleNamespace(url="https://code.example.com/acme/forkmesh.git/info/refs")
    env = SimpleNamespace(APP_ORIGIN="https://code.example.com")
    assert same(env, request, "https://code.example.com")
    assert same(SimpleNamespace(), request, "https://CODE.example.com")
    assert not same(env, request, "https://n1.next.forkmesh.com")
    other = SimpleNamespace(url="https://app.forkmesh.com/forkmesh/forkmesh.git/info/refs")
    assert not same(SimpleNamespace(APP_ORIGIN="https://app.forkmesh.com"), other,
                    "https://mirror7.example.org")
    proxy = ENTRY.split("async def _https_mirror_proxy(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert "and not _mirror_origin_is_this_app(env, request, mirror_origin)" in proxy


def test_the_installer_names_the_flagship_node_and_aliases_the_owner():
    assert 'echo "INSTANCE_FLAGSHIP_NODE=$NODE_NAME"' in INSTALLER
    merge = INSTALLER.split('"$CFG_ownerName" "$(upstream_page)" <<\'PY\'', 1)[1].split(
        "\nPY\n", 1)[0]
    assert 'alias["owner"] = owner' in merge
    assert '("forkmesh", "forkmesh")' in merge
    assert 'doc["publicOrigin"] = origin' in merge
    reinit = INSTALLER.split("setup_git_node() {", 1)[1].split("\n}\n", 1)[0]
    assert 'manifest = json.load(open(gateway["manifestPath"]))' in reinit
    assert 'mv "$DAEMON_CONFIG" "$DAEMON_CONFIG.before-$(date +%s)"' in reinit
