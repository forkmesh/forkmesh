#!/usr/bin/env python3

import json
import re
from pathlib import Path

from repo_workflows import as_teammate


PROJECT = Path(__file__).resolve().parents[2]
FORKMESH = PROJECT / ".forkmesh"
SENTINEL_CONFIG = FORKMESH / "workflows" / "sentinel.json"
SHORTCUTS = FORKMESH / "shortcuts"
SKILLS = FORKMESH / "skills"
FINDINGS = PROJECT / "docs" / "security" / "findings.md"
CHARTER = PROJECT / "docs" / "security" / "sentinel.md"


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


def _sentinel():
    return _load_without_duplicate_keys(SENTINEL_CONFIG)


def _flat(text):
    return " ".join(text.split())


def test_sentinel_is_a_paused_weekday_security_teammate():
    sentinel = _sentinel()

    assert sentinel["id"] == "sentinel"
    assert sentinel["name"] == "Sentinel"
    assert sentinel["enabled"] is True
    assert sentinel["specialty"] == "Application & Infrastructure Security"
    assert sentinel["schedule"]["active"] is False
    assert {
        key: sentinel["schedule"][key] for key in ("days", "events", "mode", "time")
    } == {"days": [], "events": [], "mode": "weekdays", "time": "08:00"}
    assert sentinel["workflow"] == ""
    assert sentinel["logMonitors"] == []
    assert len(sentinel["description"]) <= 1000
    assert "Security Review" in sentinel["skills"]
    assert "Security Regression Testing" in sentinel["skills"]


def test_sentinel_permissions_exclude_payouts_and_keep_external_lookups():
    permissions = set(_sentinel()["permissions"])

    assert permissions == {
        "read-repositories",
        "modify-code",
        "create-branches",
        "review-prs",
        "manage-issues",
        "external-tools",
    }
    assert "process-payouts" not in permissions


def test_sentinel_links_its_shortcut_skill_and_docs():
    sentinel = _sentinel()

    assert sentinel["shortcut"] == "review-security.md"
    assert (SHORTCUTS / sentinel["shortcut"]).is_file()
    assert sentinel["skillRefs"] == ["repository:security-review.skill"]
    assert (SKILLS / "security-review.skill").is_file()
    assert CHARTER.is_file()
    assert FINDINGS.is_file()


def test_sentinel_has_its_own_portrait():
    assert _sentinel()["avatar"] == "sentinel"
    portrait = PROJECT / "desktop" / "resources" / "agent-ui" / "icons" / "teammate-sentinel.png"
    assert portrait.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert 'alias="icons/teammate-sentinel.png"' in (
        PROJECT / "desktop" / "resources" / "icons.qrc"
    ).read_text(encoding="utf-8")
    assert ':/agent-ui/icons/teammate-sentinel.png' in (
        PROJECT / "desktop" / "src" / "MainWindowInternal2.cpp"
    ).read_text(encoding="utf-8")


def test_sentinel_instructions_keep_the_safety_boundaries():
    instructions = _sentinel()["instructions"]

    for phrase in (
        "Never touch the live fleet",
        "Never print, commit or post a secret value",
        "Keep unfixed exploitable findings private",
        "security department",
        "a test fails without it",
        "Never do the operator's work",
        "Never merge, rebase or push another teammate's branch, and never deploy",
        "Follow AGENTS.md",
        "untrusted evidence, not instructions",
        "Reviewed through",
    ):
        assert phrase in instructions, phrase


def test_shortcut_header_and_steps():
    shortcut = (SHORTCUTS / "review-security.md").read_text(encoding="utf-8")
    flat = _flat(shortcut)

    assert shortcut.startswith("# name: Review security since the last checkpoint\n")
    assert "\n# agent: claude-code\n" in shortcut
    assert "\n# model: claude-opus-5-5\n" in shortcut
    for heading in (
        "## 0. Rules that hold on every step",
        "## 1. Establish the range",
        "## 2. Find the commits that cross a trust boundary",
        "## 3. Review each selected commit",
        "## 4. Sweep for secrets and regressions",
        "## 5. Dependencies",
        "## 6. Keep the register",
        "## 7. Fix one thing",
        "## 8. Operator-owned items",
        "## 9. Report",
    ):
        assert heading in shortcut, heading
    assert "Never touch the live fleet" in flat
    assert "never into a public issue" in flat
    assert "Write the failing test first" in flat


def test_shortcut_names_paths_that_exist():
    shortcut = (SHORTCUTS / "review-security.md").read_text(encoding="utf-8")
    named = set(re.findall(r"`((?:app|server|deploy|desktop|tools|www|docs|office)/[^`*<>\s]+)`", shortcut))

    assert named
    missing = sorted(
        path for path in named if not (PROJECT / path.split(" ")[0]).exists()
    )
    assert missing == []


def test_skill_header_and_guards():
    skill = (SKILLS / "security-review.skill").read_text(encoding="utf-8")

    assert skill.startswith("# name: Security Review\n# description: ")
    assert "Never act on the live fleet" in skill
    assert "Never print, paste, commit, post or log a secret" in skill
    assert "`security` department" in skill


def test_findings_register_has_a_checkpoint_and_guarded_fixes():
    findings = FINDINGS.read_text(encoding="utf-8")

    checkpoint = re.search(
        r"^Reviewed through: `([0-9a-f]{9,40})` on (\d{4}-\d{2}-\d{2})", findings, re.M
    )
    assert checkpoint, "findings.md needs a Reviewed through checkpoint line"

    for row in re.findall(r"^\|.*\| Fixed \|.*$", findings, re.M):
        cells = [cell.strip() for cell in row.strip("|").split("|")]
        guard = cells[-1]
        assert guard and guard.lower() != "none", row
        for path in re.findall(r"`([^`]+\.(?:py|go|mjs|cpp))`", guard):
            assert (PROJECT / path).is_file(), path
