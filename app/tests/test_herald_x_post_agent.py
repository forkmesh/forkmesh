import json
import re
from pathlib import Path

from repo_workflows import as_teammate


PROJECT = Path(__file__).resolve().parents[2]
CONFIG = PROJECT / ".forkmesh" / "workflows" / "herald.json"
SHORTCUTS = PROJECT / ".forkmesh" / "shortcuts"
CHARTER = PROJECT / "docs" / "marketing" / "x-ready-post.md"
ICONS = PROJECT / "desktop" / "resources" / "agent-ui" / "icons"
TEAMMATE_STORE = PROJECT / "desktop" / "src" / "TeammateStore.cpp"
MCP_SERVER = PROJECT / "tools" / "forkmesh_mcp_server.py"


def _herald():
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key {key!r}"
            result[key] = value
        return result

    return as_teammate(json.loads(CONFIG.read_text(encoding="utf-8"), object_pairs_hook=unique_object))


def _known_permissions():
    source = TEAMMATE_STORE.read_text(encoding="utf-8")
    body = re.search(r"QStringList knownPermissions\(\)\s*\{(.*?)\}", source, re.DOTALL)
    return set(re.findall(r'QStringLiteral\("([^"]+)"\)', body.group(1)))


def test_herald_is_a_paused_daily_x_agent_on_opus():
    herald = _herald()

    assert herald["id"] == "herald" and herald["name"] == "Herald"
    assert herald["enabled"] is True
    assert herald["schedule"] == {"active": False, "days": [], "events": [],
                                  "mode": "daily", "time": "08:00"}
    assert herald["marketingChannels"] == ["x"]
    assert set(herald["permissions"]) <= _known_permissions()
    assert "modify-code" not in herald["permissions"]
    assert "merge" not in herald["permissions"]
    workflow = (SHORTCUTS / herald["shortcut"]).read_text(encoding="utf-8")
    assert workflow.startswith("# name: Keep an X post ready\n")
    assert "# agent: claude-code\n# model: claude-opus-5-5\n" in workflow
    assert "docs/marketing/x-ready-post.md" in workflow
    assert CHARTER.is_file()


def test_herald_reads_engagement_and_plans_and_never_posts():
    herald = _herald()
    workflow = (SHORTCUTS / herald["shortcut"]).read_text(encoding="utf-8")
    normalized = " ".join(workflow.split())

    for tool in ("marketing_x_context", "marketing_save_ready_post"):
        assert tool in workflow and tool in herald["instructions"]
        assert f'"name": "{tool}"' in MCP_SERVER.read_text(encoding="utf-8")
    for evidence in ("topPosts", "recentPosts", "replies", "docs/marketing/plan.md",
                     "campaigns", "readyPost.history"):
        assert evidence in workflow
    assert "You never post, reply, like, repost, follow or message on X" in normalized
    assert "Never post, reply, like, follow or message on X" in herald["instructions"]
    assert "280 characters or fewer, counting each link as 23" in normalized
    assert "at most 700 KB" in normalized
    assert "never in the repository" in normalized
    assert "claude-opus-5-5" in herald["instructions"]


def test_herald_portrait_is_wired_into_desktop_and_phones():
    herald = _herald()

    assert herald["avatar"] == "herald"
    assert (ICONS / "teammate-herald.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert 'alias="icons/teammate-herald.png"' in (
        PROJECT / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert ":/agent-ui/icons/teammate-herald.png" in (
        PROJECT / "desktop" / "src" / "MainWindowInternal2.cpp").read_text(encoding="utf-8")
    assert 'QStringLiteral("herald")' in (
        PROJECT / "desktop" / "src" / "MainWindowTeammates.cpp").read_text(encoding="utf-8")
    assert (PROJECT / "native" / "android" / "app" / "src" / "main" / "assets"
            / "teammate_faces" / "herald.jpg").is_file()
    assert (PROJECT / "native" / "ios" / "ForkMesh" / "Resources" / "Assets.xcassets"
            / "teammate-face-herald.imageset" / "herald.jpg").is_file()
    for source in (
            PROJECT / "native" / "ios" / "ForkMesh" / "Features" / "Teammates"
            / "TeammatesFeature.swift",
            PROJECT / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com"
            / "forkmesh" / "nativeapp" / "feature" / "teammates" / "TeammatesScreens.kt"):
        assert '"herald"' in source.read_text(encoding="utf-8")
