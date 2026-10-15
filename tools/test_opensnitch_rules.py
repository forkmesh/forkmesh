from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import opensnitch_rules as rules_tool


def path_regexps(rules):
    found = {}
    for item in rules:
        for op in item["operator"]["list"]:
            if op["operand"] in ("process.path", "dest.ip"):
                found[item["name"]] = re.compile(op["data"])
    return found


def test_rules_allow_forkmesh_programs_for_one_user(tmp_path):
    (tmp_path / "projects" / "forkmesh").mkdir(parents=True)
    rules = rules_tool.build_rules(1234, tmp_path)
    names = [item["name"] for item in rules]
    assert names == ["000-forkmesh-programs", "000-forkmesh-dev-trees", "000-forkmesh-loopback"]
    for item in rules:
        assert item["action"] == "allow" and item["duration"] == "always"
        assert item["precedence"] is True and item["enabled"] is True
        ops = item["operator"]["list"]
        assert ops[0] == {"type": "simple", "operand": "user.id", "sensitive": False,
                          "data": "1234", "list": None}
        assert json.loads(item["operator"]["data"]) == ops
    regexps = path_regexps(rules)
    programs = regexps["000-forkmesh-programs"]
    for path in ("/home/tester/.cache/build/forkmesh-chat-transport-tests",
                 "/home/tester/.cache/debug/ForkMeshCorePackageTests.xctest",
                 "/tmp/.mount_ForkMeX1/usr/bin/forkmesh",
                 "/home/tester/.local/bin/forkmesh",
                 "/home/tester/projects/forkmesh/src/desktop/build/ForkMesh"):
        assert programs.search(path), path
    for path in ("/usr/bin/curl", "/usr/bin/ssh", "/home/tester/forkmesh/evil/python3"):
        assert not programs.search(path), path
    trees = regexps["000-forkmesh-dev-trees"]
    assert trees.search(f"{tmp_path}/projects/forkmesh/src/.worktrees/x/build/chat_transport_copy")
    assert trees.search(f"{tmp_path}/.cache/fm824/swift/Tests.xctest")
    assert not trees.search(f"{tmp_path}/.cache/other/tool")
    loopback = regexps["000-forkmesh-loopback"]
    assert loopback.search("127.0.0.1") and loopback.search("127.0.0.2") and loopback.search("::1")
    assert not loopback.search("10.0.0.1") and not loopback.search("1127.0.0.1")


def test_options_drop_optional_rules(tmp_path):
    rules = rules_tool.build_rules(1, tmp_path, loopback=False)
    assert [item["name"] for item in rules] == ["000-forkmesh-programs"]


def test_install_writes_into_the_rules_folder(tmp_path, monkeypatch):
    home = tmp_path / "home"
    (home / "projects" / "forkmesh").mkdir(parents=True)
    rules_dir = tmp_path / "rules"
    rules_dir.mkdir()
    monkeypatch.setattr(rules_tool, "target_user", lambda name: type(
        "pw", (), {"pw_uid": 42, "pw_dir": str(home)})())
    assert rules_tool.main(["install", "--rules-dir", str(rules_dir)]) == 0
    written = sorted(p.name for p in rules_dir.iterdir())
    assert written == ["000-forkmesh-dev-trees.json", "000-forkmesh-loopback.json",
                       "000-forkmesh-programs.json"]
    assert (rules_dir / "000-forkmesh-programs.json").stat().st_mode & 0o777 == 0o600
    assert rules_tool.main(["install", "--rules-dir", str(tmp_path / "missing")]) == 2
