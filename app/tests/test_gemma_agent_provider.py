import ast
import asyncio
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
ISSUE_COMPOSER = (
    ROOT / "public" / "dashboard" / "js" / "07-repo-compose-branch.js"
).read_text(encoding="utf-8")
AGENT_ROUTES = (ROOT / "src" / "agent_routes.py").read_text(encoding="utf-8")
SERVER_AGENTS = (ROOT.parent / "server" / "agents.go").read_text(encoding="utf-8")
sys.path.insert(0, str(ROOT / "src"))
import catalog as catalog_contract  # noqa: E402
import mirrors as mirrors_contract  # noqa: E402


def _entry_namespace(*names, extra=None):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in names)
        or (isinstance(node, ast.Assign)
            and any(getattr(t, "id", "") in names for t in node.targets))
    ]
    namespace = dict(extra or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY_PATH), "exec"),
         namespace)
    return namespace


RELAY = _entry_namespace("ORG_AGENT_PROVIDERS", "ORG_AGENT_MODEL_ALIASES")
GEMMA_MODELS = RELAY["ORG_AGENT_MODEL_ALIASES"]["local-llama"]


def test_relay_offers_gemma_models_the_node_accepts():
    assert "local-llama" in RELAY["ORG_AGENT_PROVIDERS"]
    assert list(GEMMA_MODELS)[0] == "gemma4:e4b"
    for model in GEMMA_MODELS.values():
        assert f'"{model}": true' in SERVER_AGENTS
    assert 'defaultGemmaModel = "gemma4:e4b"' in SERVER_AGENTS


def test_composer_lists_gemma_with_every_relay_model():
    assert '{ value: "local-llama", label: "Gemma (local)" }' in DASHBOARD
    labelled = set(re.findall(
        r'provider: "local-llama", value: "([^"]*)"', DASHBOARD))
    assert labelled == {""} | set(GEMMA_MODELS)
    assert "issueAgentModelOptions(agentProviderInput.value)" in ISSUE_COMPOSER


def test_catalog_keeps_a_signed_gemma_capability():
    record = catalog_contract.safe_catalog_record({
        "owner": "alice", "name": "project", "maintainer": "PUBKEY",
        "visibility": "public",
        "agentProviders": ["codex", "local-llama", "ollama"],
    })
    assert record["agentProviders"] == ["codex", "local-llama"]


def test_owner_targets_route_gemma_to_the_advertising_mirror():
    records = [
        {"owner": "mirror9", "name": "project", "runtimeMode": "headless",
         "agentProviders": ["codex", "local-llama"], "updatedAt": 699990},
        {"owner": "mirror8", "name": "project", "runtimeMode": "headless",
         "agentProviders": ["claude-code"], "updatedAt": 699990},
    ]

    class Clock:
        @staticmethod
        def now():
            return 700000

    async def catalog(_env, _now):
        return [{"data": record, "is_private": False} for record in records]

    ns = _entry_namespace("_org_agent_owner_targets", extra={
        "Date": Clock,
        "_decrypted_public_catalog": catalog,
        "repo_mirror_same_group": lambda _target, _record: True,
        "valid_node_name": lambda value: bool(value),
        "ORG_AGENT_PROVIDERS": RELAY["ORG_AGENT_PROVIDERS"],
        "_org_agent_pool_target": lambda _env, _context, _now: (
            asyncio.sleep(0, result=None)),
        "_org_agent_personal_targets": lambda _env, _context, _now: (
            asyncio.sleep(0, result=[])),
    })
    targets = asyncio.run(ns["_org_agent_owner_targets"](object(), {
        "scope": "owner", "nodeOwner": "mirror9", "repo": "project",
        "actor": "alice", "ownedNodes": ["mirror9", "mirror8"],
    }))
    gemma = [t["node"] for t in targets if "local-llama" in t["providers"]]
    assert gemma == ["mirror9"]


def test_mirror_decision_names_gemma_when_no_node_advertises_it():
    decision = mirrors_contract.agent_provider_target_decision(
        [], {"runtimeMode": "headless"}, "mirror9", "local-llama", 0, 1)
    assert decision["ok"] is False
    assert "Gemma (local)" in decision["message"]


def test_issue_composer_passes_gemma_through_to_the_desktop():
    assert '"claude-code", "claude-api", "openai", "local-llama"' in ENTRY
    assert '"claude-code", "claude-api", "openai", "local-llama"' in AGENT_ROUTES
