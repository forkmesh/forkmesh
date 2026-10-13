import ast
import hashlib
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tools" / "export_collaboration_log.py"
VERIFIER = ROOT / "tools" / "verify_collaboration.py"
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "app" / "tests"))

import collaboration  # noqa: E402
from collaboration_fixtures import public_key, sign  # noqa: E402


def _load_tool():
    spec = importlib.util.spec_from_file_location("export_collaboration_log", TOOL_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = _load_tool()
AUTHORITY_SEED = hashlib.sha256(b"forkmesh-test-authority:706").digest()


def _worker_issue_event_content():
    source = ROOT / "app" / "src" / "events.py"
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    nodes = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "issue_event_content"
    ]
    assert len(nodes) == 1
    namespace = {}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), "exec"), namespace)
    return namespace["issue_event_content"]


def _signed(actor, number, ev):
    ev = dict(ev, author=public_key(actor))
    content = hashlib.sha256(tool.legacy_issue_event_content(ev).encode()).hexdigest()
    message = "forkmesh-issue-event-v1\n%s\n%d\n%s\n%d\n%s" % (
        ev["type"], number, ev["author"], ev["ts"], content)
    return dict(ev, sig=sign(actor, message.encode()))


def _record(number, status, title, events, priority=0):
    return {
        "schema": "forkmesh-issue-v1",
        "number": number,
        "status": status,
        "title": title,
        "priority": priority,
        "bountyUsd": 12.5,
        "createdAt": 1789600000000 + number,
        "updatedAt": 1789600001000 + number,
        "events": events,
    }


def _checkout(tmp_path):
    issues = tmp_path / "repo" / ".forkmesh" / "issues"
    opened = _signed("alice", 0, {
        "type": "open", "id": "open-1", "title": "Signed", "body": "First body",
        "attachments": [], "ts": 1789600000001,
    })
    records = {
        1: _record(1, "open", "Signed", [
            opened,
            _signed("bob", 1, {"type": "edit", "id": "edit-1", "target": "open-1",
                               "body": "Edited body", "attachments": [],
                               "ts": 1789600000002}),
            _signed("alice", 1, {"type": "priority", "id": "p-1", "priority": 7,
                                 "ts": 1789600000003}),
        ], priority=7),
        2: _record(2, "closed", "Tampered", [
            dict(_signed("bob", 2, {"type": "open", "id": "open-2", "title": "Tampered",
                                    "body": "Original", "attachments": [],
                                    "ts": 1789600000004}), body="Rewritten"),
        ]),
        3: _record(3, "open", "Unsigned", [
            {"type": "open", "id": "open-3", "author": "some-agent", "title": "Unsigned",
             "body": "", "attachments": [], "sig": "", "ts": 1789600000005},
        ]),
        4: _record(4, "open", "x" * 513, [
            _signed("alice", 4, {"type": "open", "id": "open-4", "title": "x" * 513,
                                 "body": "long", "attachments": [], "ts": 1789600000006}),
        ]),
    }
    for number, record in records.items():
        folder = issues / record["status"] / str(number)
        folder.mkdir(parents=True)
        (folder / ("issue-%d.json" % number)).write_text(
            json.dumps(record, indent=4, sort_keys=True) + "\n", encoding="utf-8")
    seed_file = tmp_path / "authority.seed"
    seed_file.write_text(tool._b64url(AUTHORITY_SEED) + "\n", encoding="utf-8")
    return tmp_path / "repo", seed_file


def _export(repo, seed_file):
    completed = subprocess.run(
        [sys.executable, str(TOOL_PATH), "export", "--repository",
         "forkmesh.com/example/disposable", "--signing-key", str(seed_file),
         "--checkout", str(repo)],
        capture_output=True, check=False, cwd=ROOT,
    )
    assert completed.returncode == 0, completed.stderr.decode()
    return json.loads(completed.stdout)


def test_legacy_content_matches_worker_canonicalization():
    worker = _worker_issue_event_content()
    samples = [
        {"type": "open", "title": "t", "body": "b", "attachments": ["a", "c"]},
        {"type": "comment", "body": "hi"},
        {"type": "edit", "body": "new", "attachments": []},
        {"type": "title", "title": "T"},
        {"type": "status", "status": "closed"},
        {"type": "labels", "labels": ["P0", "bug"]},
        {"type": "milestone", "milestone": None},
        {"type": "dates", "startDate": 5, "endDate": "bad"},
        {"type": "priority", "priority": "3"},
        {"type": "progress", "progress": None},
        {"type": "bounty", "bountyUsd": 12.5, "bountyAddress": "addr"},
        {"type": "assignees", "assignees": ["CC"]},
        {"type": "claim", "assignee": "me", "claimState": "assigned"},
        {"type": "reporter", "reporter": "node"},
        {"type": "agent", "agentProvider": "claude-code", "agentSessionId": 180,
         "agentStatus": "queued", "agentCreatePr": True},
        {"type": "reaction", "target": "e1", "reaction": "thumbs-up"},
        {"type": "delete", "target": "e1"},
        {"type": "vote"},
        {"type": "unknown"},
    ]
    for ev in samples:
        assert tool.legacy_issue_event_content(ev) == worker(ev), ev


def test_export_verifies_offline_and_reports_legacy_status(tmp_path):
    repo, seed_file = _checkout(tmp_path)
    summary = _export(repo, seed_file)
    authority = tool._b64url(tool.public_key(AUTHORITY_SEED))
    assert summary["issues"] == 3
    assert summary["legacySignatures"] == {"valid": 1, "invalid": 1, "unavailable": 1}
    assert summary["skipped"] == [{"issue": 4, "reason": "invalid_field_value"}]
    assert summary["maintainer"] == authority

    out = repo / ".forkmesh" / "collaboration"
    policy = json.loads((out / "policy.json").read_text(encoding="utf-8"))
    assert policy["maintainers"] == [authority]
    assert policy["repository"] == collaboration.repository_id(
        "forkmesh.com/example/disposable")

    completed = subprocess.run(
        [sys.executable, str(VERIFIER), "--require-complete",
         str(out / "policy.json"), str(out / "issues.ndjson")],
        capture_output=True, check=False, cwd=ROOT,
    )
    assert completed.returncode == 0, completed.stderr.decode()
    verified = json.loads(completed.stdout)
    assert verified["telemetry"]["accepted"] == 3
    assert verified["telemetry"]["rejected"] == verified["telemetry"]["pending"] == 0

    events = [json.loads(line) for line in (out / "issues.ndjson").read_bytes().splitlines()]
    by_title = {event["payload"]["fields"]["title"]: event for event in events}
    signed = by_title["Signed"]["payload"]
    assert signed["fields"] == {
        "title": "Signed", "body": "Edited body", "status": "open", "priority": 7}
    assert signed["legacy"]["signatureStatus"] == "valid"
    record_bytes = (repo / ".forkmesh/issues/open/1/issue-1.json").read_bytes()
    assert signed["legacy"]["digest"] == "sha256:" + hashlib.sha256(record_bytes).hexdigest()
    assert by_title["Tampered"]["payload"]["legacy"]["signatureStatus"] == "invalid"
    assert by_title["Tampered"]["payload"]["fields"]["status"] == "closed"
    assert by_title["Unsigned"]["payload"]["legacy"]["signatureStatus"] == "unavailable"
    assert {event["author"] for event in events} == {authority}


def test_export_is_deterministic_and_tampering_fails_verification(tmp_path):
    repo, seed_file = _checkout(tmp_path)
    _export(repo, seed_file)
    log_path = repo / ".forkmesh" / "collaboration" / "issues.ndjson"
    first = log_path.read_bytes()
    _export(repo, seed_file)
    assert log_path.read_bytes() == first

    line = first.splitlines()[0]
    event = json.loads(line)
    event["payload"]["fields"]["body"] = "forged"
    log_path.write_bytes(
        first.replace(line, collaboration.canonical_json(event), 1))
    completed = subprocess.run(
        [sys.executable, str(VERIFIER), "--policy",
         str(log_path.with_name("policy.json")), "--require-complete", str(log_path)],
        capture_output=True, check=False, cwd=ROOT,
    )
    assert completed.returncode == 3
    assert json.loads(completed.stdout)["telemetry"]["rejectionReasons"]


def test_verifier_requires_the_policy_exactly_once(tmp_path):
    repo, seed_file = _checkout(tmp_path)
    _export(repo, seed_file)
    out = repo / ".forkmesh" / "collaboration"
    for argv in (
        [str(out / "issues.ndjson")],
        ["--policy", str(out / "policy.json"), str(out / "policy.json"),
         str(out / "issues.ndjson")],
    ):
        completed = subprocess.run(
            [sys.executable, str(VERIFIER), *argv],
            capture_output=True, check=False, cwd=ROOT,
        )
        assert completed.returncode == 2
        assert b"give the policy once" in completed.stderr


def _signed_v2(actor, number, ev, repo_key):
    ev = dict(ev, author=public_key(actor))
    content = hashlib.sha256(tool.legacy_issue_event_content(ev).encode()).hexdigest()
    message = "forkmesh-issue-event-v2\n%s\n%s\n%d\n%s\n%d\n%s" % (
        repo_key, ev["type"], number, ev["author"], ev["ts"], content)
    return dict(ev, sig=sign(actor, message.encode()))


def test_signing_repository_is_owner_and_name_of_the_identity():
    assert tool.signing_repository("forkmesh.com/Example/Disposable") == "example/disposable"
    assert tool.signing_repository("example/disposable") == "example/disposable"
    assert tool.signing_repository("disposable") == ""
    assert tool.signing_repository("") == ""


def test_v2_signed_events_are_valid_only_for_their_repository():
    key = tool.signing_repository("forkmesh.com/example/disposable")
    record = _record(5, "open", "V2", [
        _signed_v2("alice", 0, {"type": "open", "id": "open-5", "title": "V2",
                                "body": "", "attachments": [], "ts": 1789600000007}, key),
        _signed_v2("bob", 5, {"type": "priority", "id": "p-5", "priority": 3,
                              "ts": 1789600000008}, key),
    ])
    assert tool.legacy_signature_status(record, key) == "valid"
    assert tool.legacy_signature_status(record, "example/other") == "invalid"
    assert tool.legacy_signature_status(record) == "invalid"
