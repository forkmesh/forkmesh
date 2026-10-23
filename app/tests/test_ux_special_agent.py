import json
import re
from pathlib import Path

from repo_workflows import as_teammate


PROJECT = Path(__file__).resolve().parents[2]
CONFIG = PROJECT / ".forkmesh" / "workflows" / "ux.json"
SHORTCUTS = PROJECT / ".forkmesh" / "shortcuts"
ICONS = PROJECT / "desktop" / "resources" / "agent-ui" / "icons"


def _agent():
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key {key!r}"
            result[key] = value
        return result

    return as_teammate(json.loads(CONFIG.read_text(encoding="utf-8"), object_pairs_hook=unique_object))


def test_ux_agent_runs_weekdays_and_can_repair_confirmed_issues():
    agent = _agent()

    assert agent["id"] == "ux" and agent["name"] == "UX"
    assert agent["enabled"] is True
    assert agent["specialty"] == "User Experience"
    assert agent["schedule"] == {"active": True, "days": [], "events": [],
                                 "mode": "weekdays", "time": "10:00"}
    assert {"read-repositories", "modify-code", "create-branches", "run-unattended",
            "run-git", "run-tests", "read-forkmesh-web"} <= set(agent["permissions"])
    instructions = agent["instructions"]
    for term in ("Reproduce a concrete problem", "accessibility", "regression coverage",
                 "Never weaken accessibility checks"):
        assert term in instructions


def test_ux_agent_has_a_focused_playbook_and_portrait_on_every_client():
    agent = _agent()
    workflow = (SHORTCUTS / agent["shortcut"]).read_text(encoding="utf-8")

    assert workflow.startswith("# name: Audit and improve user experience\n")
    assert "journeys people actually take" in workflow
    assert "Reproduce the highest-impact problem" in workflow
    assert "do not open a pull request or merge it" in workflow
    assert agent["avatar"] == "ux"
    assert (ICONS / "teammate-ux.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert 'alias="icons/teammate-ux.png"' in (
        PROJECT / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert ":/agent-ui/icons/teammate-ux.png" in (
        PROJECT / "desktop" / "src" / "MainWindowInternal2.cpp").read_text(encoding="utf-8")
    assert 'QStringLiteral("ux")' in (
        PROJECT / "desktop" / "src" / "MainWindowTeammates.cpp").read_text(encoding="utf-8")
    assert (PROJECT / "native" / "android" / "app" / "src" / "main" / "assets" /
            "teammate_faces" / "ux.jpg").is_file()
    assert (PROJECT / "native" / "ios" / "ForkMesh" / "Resources" / "Assets.xcassets" /
            "teammate-face-ux.imageset" / "ux.jpg").is_file()
    for path in (
        PROJECT / "native" / "ios" / "ForkMesh" / "Features" / "Teammates" /
        "TeammatesFeature.swift",
        PROJECT / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" /
        "forkmesh" / "nativeapp" / "feature" / "teammates" / "TeammatesScreens.kt",
    ):
        assert re.search(r'"ux"', path.read_text(encoding="utf-8"))
