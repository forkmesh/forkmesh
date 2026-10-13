import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
_RBAC_SPEC = importlib.util.spec_from_file_location(
    "organization_rbac", ROOT / "src" / "organization_rbac.py")
RBAC = importlib.util.module_from_spec(_RBAC_SPEC)
_RBAC_SPEC.loader.exec_module(RBAC)


def test_world_status_pill_no_longer_copies_an_mcp_prompt():
    assert "data-world-mcp-prompt" not in WORLD
    assert "copyMcpTaskPrompt" not in WORLD
    assert "worldRemoteMcpPrompt" not in WORLD
    assert "mcpPromptOrganization" not in WORLD
    assert "world-prompt-button" not in CSS


def test_members_can_mint_only_task_scoped_tokens():
    handler = ENTRY[
        ENTRY.index("async def org_bot_tokens_handler("):
        ENTRY.index("\n\nasync def bot_session_handler", ENTRY.index(
            "async def org_bot_tokens_handler("))
    ]
    assert (
        '"organization.bots.manage" if method in ("GET", "DELETE")'
        in handler
    )
    assert 'else "organization.bots.link"' in handler
    matrix = RBAC.ORG_AUTHORIZATION_MATRIX
    assert "member" in matrix["organization.bots.link"][1]
    assert matrix["organization.bots.manage"][1] == ("owner", "admin")
    assert 'actor_role == "member"' in handler
    assert '"organization.tasks.read", "organization.tasks.write"' in handler
