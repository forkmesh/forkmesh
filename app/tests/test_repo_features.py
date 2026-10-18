import ast
import asyncio
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")


def load(*names, **extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in names]
    assert {node.name for node in selected} == set(names)
    defaults = next(
        ast.literal_eval(node.value) for node in tree.body
        if isinstance(node, ast.Assign)
        and getattr(node.targets[0], "id", "") == "REPO_FEATURE_DEFAULTS")
    namespace = dict(extra, json=json, REPO_FEATURE_DEFAULTS=defaults)
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    return namespace


def runtime(rows, aliases=None):
    async def blind_index(env, value):
        return "bi:" + value

    async def d1_first(env, sql, *args):
        assert "FROM repo_feature_settings WHERE repo_bi=?" in sql
        data = rows.get(args[0])
        return {"data": data} if data is not None else None

    async def org_repo_node(env, owner, repo):
        return (aliases or {}).get((owner, repo), "")

    def json_response(data, status=200):
        return {"data": data, "status": status}

    return load("_repo_features_bi", "_repo_features_get", "_repo_feature_off",
                blind_index=blind_index, d1_first=d1_first,
                _org_repo_node=org_repo_node, json_response=json_response)


def test_features_default_on_and_follow_the_org_alias():
    ns = runtime({"bi:repo-feature-settings:forkmesh/forkmesh":
                  '{"issues": false, "pulls": false, "discussions": false}',
                  "bi:repo-feature-settings:ada/tools": "not json"},
                 aliases={("acme", "forkmesh"): "forkmesh"})
    off = asyncio.run(ns["_repo_features_get"](None, "Acme", "ForkMesh"))
    assert off == {"issues": False, "pulls": False, "discussions": False,
                   "actions": True, "protectDefaultBranch": False}
    assert asyncio.run(ns["_repo_features_get"](None, "ada", "tools")) == {
        "issues": True, "pulls": True, "discussions": True, "actions": True,
        "protectDefaultBranch": False}
    refused = asyncio.run(ns["_repo_feature_off"](None, "acme", "forkmesh", "pulls"))
    assert refused == {"data": {"error": "feature_disabled", "feature": "pulls"},
                       "status": 403}
    assert asyncio.run(ns["_repo_feature_off"](None, "ada", "tools", "issues")) is None


def test_submissions_are_refused_while_a_feature_is_off():
    for handler, feature in (("issues_handler", "issues"),
                             ("pulls_handler", "pulls"),
                             ("discussions_handler", "discussions")):
        body = ENTRY_TEXT.split("async def %s(env, request, owner, repo):" % handler, 1)[1]
        post = body.split('    if method == "POST":\n', 1)[1]
        assert post.startswith(
            '        disabled = await _repo_feature_off(env, owner, repo, "%s")\n'
            "        if disabled is not None:\n            return disabled\n" % feature)


def test_the_settings_save_and_the_about_card_carry_the_switches():
    about = ENTRY_TEXT.split("async def repo_about_handler(env, request, owner, repo):", 1)[1]
    about = about.split("\nasync def ", 1)[0]
    assert 'feature_data = data.get("features")' in about
    assert "INSERT INTO repo_feature_settings" in about
    assert 'result["features"] = features' in about
    public = ENTRY_TEXT.split("async def _repo_about_public(", 1)[1].split("\nasync def ", 1)[0]
    assert "await _repo_features_get(env, owner, repo)" in public
    assert "_read_only_mirrors.about_fields(" in public
    purge = ENTRY_TEXT.split("async def _delete_repo_named_state(", 1)[1]
    assert '("repo-feature-settings:", "repo_feature_settings")' in purge.split("\nasync def ", 1)[0]
    js = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert 'data-repo-feature="issues"' in js and "applyRepoFeatures(body.features)" in js
    assert "applyRepoFeatures(saved?.features)" in js


def test_the_migration_creates_the_table():
    up = (ROOT / "migrations" / "0270_repo_feature_settings.sql").read_text()
    down = (ROOT / "migrations" / "down" / "0270_repo_feature_settings.sql").read_text()
    assert "CREATE TABLE IF NOT EXISTS repo_feature_settings" in up
    assert "DROP TABLE IF EXISTS repo_feature_settings" in down
