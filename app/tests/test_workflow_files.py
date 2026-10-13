#!/usr/bin/env python3

import json
import re
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import workflow_files as files  # noqa: E402

FORKMESH = PROJECT / ".forkmesh"


def _action_semantics(path, text):
    nodes, _, _, _ = files.parse_action(path, text)
    trigger = nodes[0]["params"]
    steps = [(n["params"]["name"], n["params"]["run"])
             for n in nodes if n["type"] == "action.step"]
    jobs = [n["params"]["id"] for n in nodes if n["type"] == "action.job"]
    return (trigger["name"], trigger["events"], trigger["paths"],
            trigger["branches"], trigger["runsOn"], trigger["needs"],
            trigger["checkout"], trigger["fetchDepth"], trigger["env"],
            jobs, steps)


def _round_trip(path, now=1):
    text = (PROJECT / path).read_text(encoding="utf-8")
    document, _ = files.parse_file(path, text)
    out, warnings = files.render_file(json.loads(json.dumps(document)), now)
    return text, out, warnings


def _comment_count(text):
    return sum(1 for line in text.split("\n") if line.strip().startswith("#"))


def _parsable_actions():
    found = []
    for path in sorted(FORKMESH.glob("*.yml")):
        relative = path.relative_to(PROJECT).as_posix()
        try:
            files.parse_file(relative, path.read_text(encoding="utf-8"))
        except files.FileError:
            continue
        found.append(relative)
    return found


def test_every_valid_repository_action_keeps_its_meaning_and_comments():
    actions = _parsable_actions()
    assert len(actions) >= 5
    for path in actions:
        text, out, warnings = _round_trip(path)
        assert _action_semantics(path, out) == _action_semantics(path, text)
        assert _comment_count(out) == _comment_count(text), path
        assert warnings == []


def test_simple_actions_round_trip_byte_for_byte():
    for name in ("deploy.yml", "release.yml", "deploy-nodes.yml"):
        path = ".forkmesh/" + name
        if (PROJECT / path).exists():
            text, out, _ = _round_trip(path)
            assert out == text, path


def test_every_shortcut_and_skill_round_trips():
    paths = sorted(FORKMESH.glob("shortcuts/*")) + sorted(
        FORKMESH.glob("skills/*.skill"))
    assert paths
    for path in paths:
        relative = path.relative_to(PROJECT).as_posix()
        text, out, _ = _round_trip(relative)
        collapse = lambda s: re.sub(r"\n[ \t]*\n(?:[ \t]*\n)+", "\n\n",
                                    "\n".join(l.rstrip() for l in
                                              s.strip().split("\n")))
        assert collapse(out) == collapse(text), relative


def test_every_special_agent_keeps_its_data():
    paths = sorted(FORKMESH.glob("teammates/*.json"))
    assert paths
    identical = 0
    for path in paths:
        relative = path.relative_to(PROJECT).as_posix()
        data = json.loads(path.read_text(encoding="utf-8"))
        text, out, warnings = _round_trip(relative,
                                          data.get("updatedAtMs") or 1)
        assert json.loads(out) == data, relative
        assert warnings == []
        identical += out == text
    assert identical >= len(paths) - 1


def test_action_graph_order_follows_connections():
    document, _ = files.new_document("action", ".forkmesh/new.yml")
    nodes = document["nodes"]
    step = next(n for n in nodes if n["type"] == "action.step")
    extra = {"id": "late", "type": "action.step", "name": "x", "x": 9999,
             "y": 0, "params": {"name": "Second", "run": "echo two\n"}}
    loose = {"id": "loose", "type": "action.step", "name": "y", "x": 0,
             "y": 0, "params": {"name": "Loose", "run": "echo loose\n"}}
    document["nodes"] = nodes + [extra, loose]
    document["connections"].append({"from": step["id"], "to": "late"})
    text, warnings = files.render_file(document)
    assert text.index("Say hello") < text.index("Second")
    assert "Loose" not in text
    assert warnings == ["y is not connected and is left out"]
    assert "- name: Second\n        run: |\n          echo two\n" in text


def test_action_rendering_rejects_broken_graphs():
    document, _ = files.new_document("action", ".forkmesh/new.yml")
    document["connections"] = []
    with pytest.raises(files.FileError, match="no_steps"):
        files.render_file(document)
    document, _ = files.new_document("action", ".forkmesh/new.yml")
    trigger = document["nodes"][0]
    trigger["params"]["env"] = "[1, 2]"
    with pytest.raises(files.FileError, match="env_not_json"):
        files.render_file(document)
    document, _ = files.new_document("action", ".forkmesh/new.yml")
    document["nodes"].append(dict(document["nodes"][0], id="again"))
    with pytest.raises(files.FileError, match="only_one_trigger"):
        files.render_file(document)


def test_action_quoting_survives_special_characters():
    document, _ = files.new_document("action", ".forkmesh/q.yml")
    trigger = document["nodes"][0]["params"]
    trigger.update({"name": 'Deploy: "prod" # now', "events": [
        "push", "workflow_dispatch"], "paths": "app/*\nsrc/**/*.py",
        "env": '{"A": "x: y", "B": "1"}', "checkout": "false",
        "fetchDepth": "0"})
    text, _ = files.render_file(document)
    reparsed, _ = files.parse_file(".forkmesh/q.yml", text)
    params = reparsed["nodes"][0]["params"]
    assert params["name"] == 'Deploy: "prod" # now'
    assert params["events"] == ["push", "workflow_dispatch"]
    assert params["paths"] == "app/*\nsrc/**/*.py"
    assert json.loads(params["env"]) == {"A": "x: y", "B": "1"}
    assert params["checkout"] == "false"
    assert params["fetchDepth"] == "0"


def test_shortcut_and_skill_documents():
    document, _ = files.new_document("shortcut", ".forkmesh/shortcuts/go.sh")
    text, _ = files.render_file(document)
    assert text.startswith("#!/usr/bin/env bash\n# name: New shortcut\n")
    document["nodes"][0]["params"]["agent"] = "codex"
    document["nodes"].append({"id": "b9", "type": "shortcut.block",
                              "name": "x", "x": 5000, "y": 0,
                              "params": {"text": "echo last"}})
    document["connections"].append({"from": document["nodes"][-2]["id"],
                                    "to": "b9"})
    text, _ = files.render_file(document)
    assert "# agent: codex\n" in text
    assert text.rstrip().endswith("echo last")
    prompt, _ = files.new_document("shortcut", ".forkmesh/shortcuts/ask.md")
    text, _ = files.render_file(prompt)
    assert text.startswith("# name: New shortcut\n# description:")
    assert "\n\nDescribe the task" in text
    skill, _ = files.new_document("skill", ".forkmesh/skills/x.skill")
    text, _ = files.render_file(skill)
    assert "# agent:" not in text and text.startswith("# name: New skill\n")


def test_special_agent_graph_controls_triggers_and_uses():
    text = (FORKMESH / "teammates" / "hearth.json").read_text(
        encoding="utf-8")
    document, _ = files.parse_file(".forkmesh/teammates/hearth.json", text)
    types = [n["type"] for n in document["nodes"]]
    assert types.count("agent.profile") == 1
    assert "agent.schedule" in types and "agent.shortcut" in types
    profile = next(n for n in document["nodes"]
                   if n["type"] == "agent.profile")
    schedule = next(n for n in document["nodes"]
                    if n["type"] == "agent.schedule")
    schedule["params"].update({"mode": "weekdays", "time": "08:30",
                               "active": "true"})
    profile["params"]["permissions"] = ["read-repositories", "run-tests"]
    document["nodes"].append({"id": "m1", "type": "agent.monitor",
                              "name": "m", "x": 0, "y": 0, "params": {
                                  "badge": "ERROR", "threshold": "5",
                                  "taskType": "error-logs", "active": "true",
                                  "inclusive": "false"}})
    document["connections"].append({"from": "m1", "to": profile["id"]})
    document["nodes"].append({"id": "s9", "type": "agent.skill", "name": "s",
                              "x": 0, "y": 9999, "params": {
                                  "scope": "repository",
                                  "file": "security-review.skill"}})
    document["connections"] = [c for c in document["connections"]
                               if c["to"] != "shortcut"]
    out, warnings = files.render_file(document, 123)
    data = json.loads(out)
    assert data["schedule"] == {"active": True, "days": [], "events": [],
                                "mode": "weekdays", "time": "08:30"}
    assert data["permissions"] == ["read-repositories", "run-tests"]
    assert data["logMonitors"] == [{"active": True, "badge": "ERROR",
                                    "inclusive": False,
                                    "taskType": "error-logs",
                                    "threshold": 5}]
    assert data["shortcut"] == ""
    assert data["skillRefs"] == ["repository:community-engagement.skill"]
    assert data["updatedAtMs"] == 123
    assert data["improvementNotes"] == json.loads(text)["improvementNotes"]
    assert any("Playbook" in w for w in warnings)
    assert any("Skill file" in w or "s is not connected" in w
               for w in warnings)


def test_special_agent_validation_matches_the_desktop():
    document, _ = files.new_document("agent", ".forkmesh/teammates/ops.json")
    profile = document["nodes"][0]
    assert profile["params"]["id"] == "ops"
    text, _ = files.render_file(document, 5)
    data = json.loads(text)
    assert data["id"] == "ops" and data["version"] == 2
    assert data["createdAtMs"] == 5
    for key, value, code in (
            ("permissions", ["root"], "unknown_permission"),
            ("id", "Other", "invalid_agent_id"),
            ("id", "other", "agent_id_mismatch"),
            ("runWindowHours", "721", "invalid_run_window"),
            ("specialty", "", "agent_needs_name_and_specialty")):
        broken = json.loads(json.dumps(document))
        broken["nodes"][0]["params"][key] = value
        with pytest.raises(files.FileError, match=code):
            files.render_file(broken)
    broken = json.loads(json.dumps(document))
    schedule = next(n for n in broken["nodes"]
                    if n["type"] == "agent.schedule")
    schedule["params"]["time"] = "25:00"
    with pytest.raises(files.FileError, match="invalid_schedule_time"):
        files.render_file(broken)


def test_paths_and_kinds():
    assert files.kind_for_path(".forkmesh/ci.yml") == "action"
    assert files.kind_for_path(".forkmesh/shortcuts/a.md") == "shortcut"
    assert files.kind_for_path(".forkmesh/skills/a.skill") == "skill"
    assert files.kind_for_path(".forkmesh/teammates/a-b.json") == "agent"
    assert files.kind_for_path(".forkmesh/teammates/A.json") == ""
    assert files.kind_for_path("../etc/passwd") == ""
    assert files.kind_for_path(".forkmesh/sub/dir/x.yml") == ""
    with pytest.raises(files.FileError):
        files.parse_file("README.md", "x")
    catalog = files.catalog()
    assert set(catalog["kinds"]) == {"action", "shortcut", "skill", "agent"}
    for node in catalog["nodes"]:
        assert node["kind"] in catalog["kinds"]
        for field in node["fields"]:
            assert field["type"] in ("string", "text", "code", "number",
                                     "boolean", "select", "list", "checks",
                                     "json")
