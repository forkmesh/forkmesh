#!/usr/bin/env python3
"""Contracts for the configured marketing and documentation teammates."""

import json
import re
from pathlib import Path

from repo_workflows import as_teammate


PROJECT = Path(__file__).resolve().parents[2]
TEAMMATES = PROJECT / ".forkmesh" / "workflows"
SHORTCUTS = PROJECT / ".forkmesh" / "shortcuts"


def _load_without_duplicate_keys(path):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate JSON key {key!r} in {path}"
            result[key] = value
        return result

    return as_teammate(json.loads(
        path.read_text(encoding="utf-8"),
        object_pairs_hook=unique_object,
    ))


SKILLS = PROJECT / ".forkmesh" / "skills"
ICONS = PROJECT / "desktop" / "resources" / "agent-ui" / "icons"


def _handle(teammate):
    name = teammate["name"].strip()
    return teammate["id"] if " " in name else name


def test_blogger_defines_a_weekly_evidence_backed_story_workflow():
    blogger = _load_without_duplicate_keys(TEAMMATES / "blogger.json")
    workflow = (SHORTCUTS / blogger["shortcut"]).read_text(encoding="utf-8")

    assert blogger["id"] == "blogger"
    assert blogger["name"] == "Blogger"
    assert blogger["enabled"] is True
    assert blogger["schedule"]["active"] is False
    assert {
        key: blogger["schedule"][key]
        for key in ("days", "events", "mode", "time")
    } == {
        "days": [1],
        "events": [],
        "mode": "weekly",
        "time": "11:00",
    }
    assert blogger["provider"] == "claude-code"
    assert blogger["model"] == "claude-opus-5-5"
    assert "haiku" not in blogger["model"]
    assert blogger["shortcut"] == "write-changelog-blog-post.md"
    assert "# agent: claude-code" in workflow
    assert "# model: claude-opus-5-5" in workflow
    assert "codex exec" in workflow
    assert "Security Claim Review" in blogger["skills"]
    assert "Visual Storytelling" in blogger["skills"]
    assert "modify-code" in blogger["permissions"]
    assert "merge" not in blogger["permissions"]
    assert "an older post or image is historical evidence" in (
        blogger["instructions"]
    )
    assert "keep generated artwork free of baked-in technical claims" in (
        blogger["instructions"]
    )
    assert "@Marketing" in blogger["instructions"]
    assert blogger["improvementNotes"]


def test_blogger_portrait_is_wired_into_the_desktop():
    blogger = _load_without_duplicate_keys(TEAMMATES / "blogger.json")
    portrait = ICONS / "teammate-blogger.png"

    assert blogger["avatar"] == "blogger"
    assert portrait.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert 'alias="icons/teammate-blogger.png"' in (
        PROJECT / "desktop" / "resources" / "icons.qrc"
    ).read_text(encoding="utf-8")
    assert ":/agent-ui/icons/teammate-blogger.png" in (
        PROJECT / "desktop" / "src" / "MainWindowInternal2.cpp"
    ).read_text(encoding="utf-8")
    assert 'QStringLiteral("blogger")' in (
        PROJECT / "desktop" / "src" / "MainWindowTeammates.cpp"
    ).read_text(encoding="utf-8")


def test_marketing_is_the_marketing_manager():
    marketing = _load_without_duplicate_keys(TEAMMATES / "marketing.json")
    workflow = (SHORTCUTS / marketing["shortcut"]).read_text(encoding="utf-8")
    normalized_workflow = " ".join(workflow.split())
    instructions = marketing["instructions"]

    assert marketing["enabled"] is True
    assert isinstance(marketing["schedule"]["active"], bool)
    assert {
        key: marketing["schedule"][key]
        for key in ("days", "events", "mode", "time")
    } == {
        "days": [1],
        "events": [],
        "mode": "weekly",
        "time": "09:30",
    }
    assert marketing["specialty"] == "Marketing Management"
    assert marketing["shortcut"] == "run-the-marketing-program.md"
    assert marketing["shortcut"] != "write-changelog-blog-post.md"
    for ref in marketing["skillRefs"]:
        scope, name = ref.split(":", 1)
        assert scope == "repository"
        assert (SKILLS / name).is_file()
    assert "repository:marketing-management.skill" in marketing["skillRefs"]
    assert "modify-code" in marketing["permissions"]
    assert "merge" not in marketing["permissions"]
    assert "process-payouts" not in marketing["permissions"]
    for skill in (
        "Marketing Strategy",
        "Growth & Acquisition",
        "Campaign Management",
        "Launch Planning",
        "Marketing Analytics",
        "Security Claim Review",
        "Marketing Compliance",
    ):
        assert skill in marketing["skills"]
    for marker in (
        "north-star metric",
        "AARRR funnel",
        "impact, confidence and ease",
        "UTM-tagged links",
        "/admin/acquisition",
        "never spend money",
        "never write, buy, suppress or vote on reviews",
        "opt-in consent",
    ):
        assert marker in instructions
    for marker in (
        "`/blog/*` and `/docs/*` visits are not captured",
        "ATTRIBUTION_ALLOWED_*",
        "docs/marketing/plan.md",
        "a decision for the operator",
    ):
        assert marker in normalized_workflow


def test_marketing_hands_off_to_teammates_that_exist():
    marketing = _load_without_duplicate_keys(TEAMMATES / "marketing.json")
    workflow = (SHORTCUTS / marketing["shortcut"]).read_text(encoding="utf-8")
    handles = {
        _handle(_load_without_duplicate_keys(path))
        for path in TEAMMATES.glob("*.json")
    }
    mentioned = set(re.findall(r"@([A-Za-z0-9_-]+)", workflow))
    mentioned |= set(re.findall(r"@([A-Za-z0-9_-]+)", marketing["instructions"]))
    assert {"Blogger", "Reddit", "Hearth", "Sales", "y-combinator-news-agent"} <= (
        mentioned
    )
    assert mentioned - {"Name"} <= handles


def test_sage_defines_a_separate_weekly_documentation_stewardship_lane():
    sage = _load_without_duplicate_keys(TEAMMATES / "code-explainer.json")
    workflow_path = SHORTCUTS / sage["shortcut"]
    workflow = workflow_path.read_text(encoding="utf-8")
    normalized_workflow = " ".join(workflow.split())

    assert sage["enabled"] is True
    assert isinstance(sage["schedule"]["active"], bool)
    assert {
        key: sage["schedule"][key]
        for key in ("days", "events", "mode", "time")
    } == {
        "days": [4],
        "events": [],
        "mode": "weekly",
        "time": "10:00",
    }
    assert sage["shortcut"] == "refresh-current-documentation.md"
    assert "modify-code" in sage["permissions"]
    assert "open-prs" not in sage["permissions"]
    assert "Documentation Stewardship" in sage["skills"]
    assert "For a question or mention" in sage["instructions"]
    assert "For the scheduled documentation run" in sage["instructions"]

    for marker in (
        "# agent: claude-code",
        "# model: claude-opus-5",
        "Read the canonical documents for the area before editing",
        "Preserve historical records",
        "Add or extend a regression check",
        "never merge or deploy it yourself",
    ):
        assert marker in normalized_workflow
