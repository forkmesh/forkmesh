#!/usr/bin/env python3
"""Self-contained test for tools/forkmesh_mcp_server.py.

Spins up a throwaway git repo + a fresh Ed25519 identity, drives the server
over its stdio JSON-RPC transport, and checks each tool end-to-end — including
bounded Git-object code access, signed Markdown notes, encrypted #general chat,
that written issues/milestones/projects/PRs land in the native signed tracker
layout (.forkmesh/issues/{open,closed}/<n>/issue-<n>.json,
.forkmesh/projects/<n>/project-<n>.json) and carry signatures that verify
against the identity's public key (proving the byte-exact reproduction of the
Qt client's IssueStore/ProjectStore signing).

The repo is seeded with an existing open issue, a closed issue, and a project
so number allocation is checked across both status folders and past projects.

Run:  python3 tools/test_forkmesh_mcp_server.py
"""

import base64
import hashlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

HERE = Path(__file__).resolve().parent
SERVER = HERE / "forkmesh_mcp_server.py"

_spec = importlib.util.spec_from_file_location("srv", SERVER)
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)

FAILURES = []

MAX_SAFE_INT = 9007199254740991

# One fixture per event type, pinning the exact contentForSigning bytes of
# desktop/src/IssueStore.cpp and ProjectStore.cpp (and the relay Worker's
# issue_event_content). If the server's port drifts from these rules, the
# comparison below fails before any signature check ever runs.
ISSUE_CONTENT_FIXTURES = [
    ({"type": "open", "title": "T", "body": "B",
      "attachments": ["a.png", "b.png"]}, "T\x00B\x00a.png,b.png"),
    ({"type": "comment", "body": "C", "attachments": []}, "C\x00"),
    ({"type": "edit", "body": "E", "attachments": ["x"]}, "E\x00x"),
    ({"type": "title", "title": "New"}, "New"),
    ({"type": "status", "status": "closed"}, "closed"),
    ({"type": "labels", "labels": ["bug", "ui"]}, "bug,ui"),
    ({"type": "milestone", "milestone": "v1"}, "v1"),
    ({"type": "dates", "startDate": 1784347200000, "endDate": 1784433600000},
     "1784347200000\x001784433600000"),
    ({"type": "priority", "priority": 3}, "3"),
    ({"type": "progress", "progress": 40}, "40"),
    ({"type": "bounty", "bountyUsd": 12.5, "bountyAddress": "addr",
      "bountyStatus": "funded"}, "12.50\x00addr\x00funded"),
    ({"type": "assignees", "assignees": ["a", "b"]}, "a,b"),
    ({"type": "reporter", "reporter": "newnewnode"}, "newnewnode"),
    ({"type": "agent", "agentProvider": "claude-code", "agentSessionId": 7,
      "agentStatus": "queued", "agentCreatePr": False},
     "claude-code\x007\x00queued\x00no-pr"),
    ({"type": "agent", "agentProvider": "codex", "agentSessionId": 8,
      "agentStatus": "done", "agentCreatePr": True},
     "codex\x008\x00done\x00pr"),
    ({"type": "delete", "target": "self"}, "self"),
    ({"type": "vote"}, ""),
]

PROJECT_CONTENT_FIXTURES = [
    ({"type": "open", "title": "T", "body": "B"}, "T\x00B"),
    ({"type": "title", "title": "N"}, "N"),
    ({"type": "edit", "body": "E"}, "E"),
    ({"type": "status", "status": "closed"}, "closed"),
    ({"type": "dates", "startDate": 1, "endDate": 2}, "1\x002"),
    ({"type": "milestone", "milestone": "M"}, "M"),
    ({"type": "issues", "issues": [385, 384, 384]}, "384,385"),
    ({"type": "delete", "target": "self"}, "self"),
]


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        FAILURES.append(name)


def rpc(request):
    """Round-trip one JSON-RPC request through serve() with a fresh module state."""
    out = io.StringIO()
    srv.serve(io.StringIO(json.dumps(request) + "\n"), out)
    line = out.getvalue().strip()
    return json.loads(line) if line else None


def unpad(s):
    return s + "=" * (-len(s) % 4)


def _canonical(kind, number, ev, content):
    ch = hashlib.sha256(content.encode()).hexdigest()
    return (f"forkmesh-{kind}-event-v1\n{ev['type']}\n{number}\n"
            f"{ev['author']}\n{ev['ts']}\n{ch}").encode()


def _verify(pub_b64, sig_b64, canonical):
    try:
        Ed25519PublicKey.from_public_bytes(
            base64.urlsafe_b64decode(unpad(pub_b64))).verify(
                base64.urlsafe_b64decode(unpad(sig_b64)), canonical)
        return True
    except Exception:
        return False


def verify_issue_event(pub_b64, number, ev):
    return _verify(pub_b64, ev["sig"],
                   _canonical("issue", number, ev,
                              srv.issue_content_for_signing(ev)))


def verify_project_event(pub_b64, number, ev):
    return _verify(pub_b64, ev["sig"],
                   _canonical("project", number, ev,
                              srv.project_content_for_signing(ev)))


def _safe_int(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= MAX_SAFE_INT


def issue_loads_strict(record, number):
    """Python port of IssueStore's strictIssueObjectValid — the schema the
    desktop's strict loader (and the browser reader) accepts without repair."""
    if record.get("schema") != "forkmesh-issue-v1":
        return False
    if record.get("number") != number:
        return False
    for key in ("createdAt", "startDate", "endDate"):
        if key in record and not _safe_int(record[key]):
            return False
    events = record.get("events")
    if not isinstance(events, list):
        return False
    for ev in events:
        if not isinstance(ev, dict):
            return False
        if not (isinstance(ev.get("type"), str) and ev["type"]):
            return False
        if not (isinstance(ev.get("author"), str) and ev["author"]):
            return False
        if not _safe_int(ev.get("ts")):
            return False
        if not (isinstance(ev.get("sig"), str) and ev["sig"]):
            return False
        atts = ev.get("attachments")
        if atts is not None and not (
                isinstance(atts, list) and all(isinstance(a, str) for a in atts)):
            return False
        if ev["type"] == "open" and not (
                isinstance(ev.get("title"), str) and
                isinstance(ev.get("body"), str) and
                isinstance(ev.get("attachments"), list)):
            return False
    return True


def project_loads_strict(record, number):
    """The shape Project::fromJson / the dashboard reader consume."""
    if record.get("schema") != "forkmesh-project-v1":
        return False
    if record.get("number") != number:
        return False
    if not isinstance(record.get("title"), str):
        return False
    for key in ("createdAt", "updatedAt", "startDate", "endDate"):
        if key in record and not _safe_int(record[key]):
            return False
    issues = record.get("issues")
    if not (isinstance(issues, list) and
            all(isinstance(n, int) for n in issues)):
        return False
    events = record.get("events")
    if not isinstance(events, list) or not events:
        return False
    for ev in events:
        if not (isinstance(ev, dict) and ev.get("type") and ev.get("author")
                and _safe_int(ev.get("ts")) and ev.get("sig")):
            return False
    return True


def seed_issue(repo, number, status, title):
    d = repo / f".forkmesh/issues/{status}/{number}"
    d.mkdir(parents=True)
    rec = {
        "schema": "forkmesh-issue-v1", "number": number, "title": title,
        "status": status, "labels": [], "milestone": "", "startDate": 0,
        "endDate": 0, "priority": 0, "progress": 0, "assignees": [],
        "createdAt": 1, "updatedAt": 1, "author": "seed-key",
        "authorName": "seed", "bountyUsd": 0, "bountyAddress": "",
        "bountyStatus": "", "votes": 0,
        "events": [{"type": "open", "id": f"open-{number}",
                    "author": "seed-key", "authorName": "seed", "ts": 1,
                    "title": title, "body": "seeded", "attachments": [],
                    "sig": "seed-sig"}],
    }
    (d / f"issue-{number}.json").write_text(json.dumps(rec, indent=4) + "\n")


def seed_project(repo, number, title):
    d = repo / f".forkmesh/projects/{number}"
    d.mkdir(parents=True)
    rec = {
        "schema": "forkmesh-project-v1", "number": number, "title": title,
        "body": "seeded", "status": "open", "startDate": 0, "endDate": 0,
        "milestone": "", "issues": [], "createdAt": 1, "updatedAt": 1,
        "author": "seed-key", "authorName": "seed",
        "events": [{"type": "open", "id": f"open-{number}",
                    "author": "seed-key", "authorName": "seed", "ts": 1,
                    "title": title, "body": "seeded", "sig": "seed-sig"}],
    }
    (d / f"project-{number}.json").write_text(json.dumps(rec, indent=4) + "\n")


def work_item_merge_cases():
    existing = [
        {"repository": "o/r", "branch": "a", "worktree": "/w/a", "node": "n1"},
        {"repository": "o/r", "branch": "a", "worktree": "/x/a", "node": "n2"},
    ]
    captured = []

    def fake_update(task_id, fields):
        captured.append((task_id, fields))
        return {"ok": True}

    real_get, real_update = srv.get_task, srv.update_task
    srv.get_task = lambda task_id: {"id": "f" * 32, "workItems": existing}
    srv.update_task = fake_update
    try:
        srv.attach_work_item("f" * 32, "c", "o/r", "/w/c", "N1")
        srv.detach_work_item("f" * 32, "a", "O/r", "n2")
        try:
            srv.detach_work_item("f" * 32, "missing")
            missing = False
        except ValueError:
            missing = True
        try:
            srv.attach_work_item("f" * 32, "")
            blank = False
        except ValueError:
            blank = True
    finally:
        srv.get_task, srv.update_task = real_get, real_update
    return (
        captured == [
            ("f" * 32, {"attachWorkItems": [{
                "repository": "o/r", "branch": "c", "worktree": "/w/c",
                "node": "n1"}]}),
            ("f" * 32, {"detachWorkItems": [{
                "repository": "O/r", "branch": "a", "worktree": "",
                "node": "n2"}]}),
        ]
        and missing and blank
        and "attachWorkItems" in srv.TASK_UPDATE_FIELDS
        and srv.task_update_digest({"workItems": [{"branch": "é"}]}) ==
        hashlib.sha256('{"workItems":[{"branch":"é"}]}'.encode()).hexdigest()
    )


def persistent_reminder_update_cases():
    sent = []
    real_request = srv.task_request
    srv.task_request = lambda method, path, proof, body=None, query=None: (
        sent.append((method, path, proof[0], dict(body or {})))
        or {"ok": True})
    try:
        srv.update_task("e" * 32, {
            "title": "Back up keys", "persistentReminder": True})
        srv.update_task("e" * 32, {"persistentReminder": False})
    finally:
        srv.task_request = real_request
    path = "/api/tasks/" + "e" * 32
    return (
        sent == [
            ("PATCH", path, "forkmesh-org-task-update-v1",
             {"title": "Back up keys"}),
            ("PATCH", path, "forkmesh-org-task-update-v1",
             {"persistentReminder": True}),
            ("PATCH", path, "forkmesh-org-task-update-v1",
             {"persistentReminder": False}),
        ]
        and "persistentReminder" in srv.TASK_CREATE_FIELDS
    )


def check_task_trash_tools(call, pub_b64):
    other_id = "d" * 32
    task_id = "c" * 32
    sent = []
    state = {"trash": True, "error": None}

    class FakeResponse(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

    def fake_urlopen(request, timeout):
        url = srv.urllib.parse.urlsplit(request.full_url)
        params = dict(srv.urllib.parse.parse_qsl(url.query))
        body = json.loads(request.data) if request.data else None
        sent.append({"method": request.get_method(), "path": url.path,
                     "params": params, "body": body,
                     "agent": request.get_header("X-forkmesh-agent")})
        if request.get_method() == "GET":
            payload = {"ok": True, "tasks": [], "hasMore": False}
            if params.get("status") == "deleted":
                if state["trash"]:
                    payload["trash"] = True
                payload["tasks"] = [{
                    "id": task_id, "title": "Old", "deletedAt": 5,
                    "deletedBy": "seller", "deleteReason": "dup",
                    "purgeAt": 6, "canRestore": True, "details": "x"}]
            return FakeResponse(json.dumps(payload).encode())
        if state["error"]:
            code, payload = state["error"]
            raise srv.urllib.error.HTTPError(
                request.full_url, code, "refused", {},
                io.BytesIO(json.dumps(payload).encode()))
        return FakeResponse(json.dumps({"ok": True, "deleted": True}).encode())

    def signed(entry, *parts):
        canonical = "\n".join((parts[0], "seller", *parts[1:],
                               entry["params"]["ts"]))
        return _verify(pub_b64, entry["params"]["sig"], canonical.encode())

    real = srv.urllib.request.urlopen
    srv.urllib.request.urlopen = fake_urlopen
    try:
        text, err = call("list_tasks", {"status": "deleted"})
        listed = json.loads(text) if not err else {}
        check("list_tasks status deleted reads the trash with its fields",
              not err and sent[-1]["params"].get("status") == "deleted" and
              signed(sent[-1], "forkmesh-org-task-list-v1") and
              listed["tasks"][0]["deletedBy"] == "seller" and
              listed["tasks"][0]["purgeAt"] == 6 and
              "details" not in listed["tasks"][0])

        sent.clear()
        text, err = call("delete_task", {"task_id": task_id})
        single = sent[-1]
        check("delete_task checks the relay keeps a trash first",
              not err and sent[0]["method"] == "GET" and
              sent[0]["params"].get("status") == "deleted")
        check("delete_task sends one signed DELETE",
              single["method"] == "DELETE" and
              single["path"] == f"/api/tasks/{task_id}" and
              single["body"] is None and
              signed(single, "forkmesh-org-task-delete-v1", task_id) and
              "restore_task" in json.loads(text)["restore"])

        text, err = call("delete_task", {"task_id": task_id,
                                         "reason": "duplicate of #12"})
        single = sent[-1]
        digest = srv.task_update_digest({"reason": "duplicate of #12"})
        check("delete_task signs the reason it sends",
              not err and single["body"] == {"reason": "duplicate of #12"} and
              signed(single, "forkmesh-org-task-delete-v1", task_id, digest))

        text, err = call("delete_task", {
            "task_ids": [task_id, other_id.upper(), task_id], "reason": "cleanup"})
        batch = sent[-1]
        body = {"ids": [task_id, other_id], "reason": "cleanup"}
        check("delete_task batches ids into one signed request",
              not err and batch["method"] == "POST" and
              batch["path"] == "/api/tasks/delete" and batch["body"] == body and
              signed(batch, "forkmesh-org-task-delete-batch-v1",
                     srv.task_update_digest(body)))

        text, err = call("restore_task", {"task_id": task_id})
        check("restore_task sends its own signed proof",
              not err and sent[-1]["method"] == "POST" and
              sent[-1]["path"] == f"/api/tasks/{task_id}/restore" and
              signed(sent[-1], "forkmesh-org-task-restore-v1", task_id))

        count = len(sent)
        for args, why in (
                ({}, "no id"),
                ({"task_id": "nothex"}, "bad id"),
                ({"task_ids": ["%032x" % n for n in range(51)]}, "51 ids"),
                ({"task_ids": task_id}, "ids not a list"),
                ({"task_id": task_id, "reason": "x" * 501}, "long reason"),
                ({"task_id": task_id, "reason": 5}, "reason not text")):
            _, err = call("delete_task", args)
            check(f"delete_task refuses {why} before any request", err)
        check("refused arguments sent nothing", len(sent) == count)

        state["error"] = (409, {"error": "task_delete_blocked",
                                "blockedBy": ["agent_running", "qa_in_progress"],
                                "forceRequires": "owner",
                                "request": {"user": "seller",
                                            "via": "signed-key",
                                            "node": "seller",
                                            "agent": "claude",
                                            "ip": "203.0.113.9",
                                            "userAgent": "ForkMeshMCP/0.2"}})
        text, err = call("delete_task", {"task_id": task_id})
        check("a blocked delete says which guard refused it",
              err and "task_delete_blocked" in text and
              "an agent job is running on it" in text and
              "waiting for QA review" in text and "owner" in text)
        check("a blocked delete names who asked, from where",
              "user seller" in text and "via signed-key" in text and
              "agent claude" in text and "ip 203.0.113.9" in text and
              "user agent ForkMeshMCP/0.2" in text)
        check("task requests name the calling agent",
              all(item["agent"] for item in sent))
        state["error"] = (403, {"error": "task_delete_batch_refused",
                                "failures": [{"id": other_id,
                                              "error": "forbidden"}],
                                "deleted": []})
        text, err = call("delete_task", {"task_ids": [task_id, other_id]})
        check("a refused batch names each refused id and deleted nothing",
              err and "Tasks API 403" in text and
              f"{other_id}: forbidden" in text and "nothing was deleted" in text)
        state["error"] = (410, {"error": "task_trash_expired"})
        text, err = call("restore_task", {"task_id": task_id})
        check("an expired restore explains the 30-day window",
              err and "Tasks API 410" in text and "30 days" in text)
        state["error"] = None

        state["trash"] = False
        count = len(sent)
        text, err = call("delete_task", {"task_id": task_id})
        check("delete_task refuses a relay without a trash",
              err and "trash" in text and
              [entry["method"] for entry in sent[count:]] == ["GET"])
    finally:
        srv.urllib.request.urlopen = real


def main():
    tmp = Path(tempfile.mkdtemp(prefix="fmmcp-"))
    repo = tmp / "repo"
    repo.mkdir()

    data_home = tmp / "data"
    key_dir = data_home / "ForkMesh/ForkMesh/identity"
    key_dir.mkdir(parents=True)
    priv = Ed25519PrivateKey.generate()
    (key_dir / "ed25519.pem").write_bytes(priv.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    pub_raw = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    pub_b64 = base64.urlsafe_b64encode(pub_raw).decode().rstrip("=")

    os.environ["XDG_DATA_HOME"] = str(data_home)
    os.environ["FORKMESH_REPO"] = str(repo)
    os.environ["FORKMESH_MCP_NOTES_DIR"] = str(tmp / "personal-notes")
    srv.KEY_PATH = key_dir / "ed25519.pem"
    connector_path = data_home / "ForkMesh/ForkMesh/mcp/connector.json"
    srv.CONNECTOR_PATH = connector_path
    srv._key = srv._pub = None
    os.environ.pop("FORKMESH_MCP_TOKEN", None)

    def git(*args):
        return subprocess.run(
            ["git", "-C", str(repo), *args], check=True,
            capture_output=True, text=True)

    def last_commit():
        return subprocess.run(
            ["git", "-C", str(repo), "log", "-1", "--format=%s"],
            capture_output=True, text=True).stdout.strip()

    git("init", "-q", "-b", "main")
    git("config", "user.email", "t@t")
    git("config", "user.name", "tester")
    (repo / "README.md").write_text("hello mesh\n")
    source = repo / "src"
    source.mkdir()
    (source / "auth.py").write_text(
        "def authenticate_ssh(key):\n    return key == 'trusted'\n")
    (source / "binary.dat").write_bytes(b"text\x00binary")
    os.symlink("/etc/passwd", repo / "outside-link")
    seed_issue(repo, 1, "open", "Seeded open issue")
    seed_issue(repo, 2, "closed", "Seeded closed issue")
    seed_project(repo, 1, "Seeded project")
    git("add", "-A")
    git("commit", "-qm", "init")
    initial_commit = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True).stdout.strip()
    # Code tools must not see ignored/untracked working-tree files.
    (repo / "local-secret.env").write_text("TOKEN=do-not-expose\n")

    check("issue contentForSigning matches native fixtures", all(
        srv.issue_content_for_signing(ev) == expected
        for ev, expected in ISSUE_CONTENT_FIXTURES))
    check("project contentForSigning matches native fixtures", all(
        srv.project_content_for_signing(ev) == expected
        for ev, expected in PROJECT_CONTENT_FIXTURES))

    resp = rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18"}})
    check("initialize", resp["result"]["serverInfo"]["name"] == "forkmesh")

    resp = rpc({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    names = {t["name"] for t in resp["result"]["tools"]}
    check("tools/list has scoped code, notes, and chat tools", names == {
        "whoami", "list_repos", "read_file", "search_issues", "create_issue",
        "comment_on_issue", "create_milestone", "update_milestone",
        "create_project", "update_project", "open_pr_from_branch",
        "get_pr_diff", "list_repositories", "list_files", "search_code",
        "create_note", "get_note", "list_notes", "search_notes", "update_note",
        "create_cloud_note", "read_chat_messages", "post_chat_message", "crm_list_organizations",
        "crm_get_pipeline", "crm_list_contacts", "crm_get_contact",
        "crm_list_activity", "crm_update_contact", "crm_find_duplicates",
        "crm_merge_contacts", "crm_check_accounts", "marketing_x_context",
        "marketing_save_ready_post", "list_tasks", "get_task",
        "create_task", "update_task", "complete_task", "reopen_task",
        "attach_work_item", "detach_work_item", "delete_task",
        "restore_task", "create_agent", "show_window", "read_window",
        "close_window", "open_browser_window", "read_browser_page",
        "list_releases", "create_release", "publish_release",
        "record_expense"})
    check("work item attach merges on repository, branch and node",
          work_item_merge_cases())
    check("persistent reminder rides its own signed task PATCH",
          persistent_reminder_update_cases())
    check("task update signs its exact body",
          srv.task_update_digest({"b": 1, "a": "x"}) ==
          srv.task_update_digest({"a": "x", "b": 1}) and
          srv.task_update_digest({"a": "x"}) != srv.task_update_digest({"a": "y"}))
    annotations = {t["name"]: t.get("annotations", {})
                   for t in resp["result"]["tools"]}
    check("tool annotations distinguish reads from writes",
          annotations["read_file"].get("readOnlyHint") is True and
          annotations["create_note"].get("readOnlyHint") is False and
          annotations["post_chat_message"].get("destructiveHint") is False)
    check("only delete_task and crm_merge_contacts are flagged destructive", {
        name for name, value in annotations.items()
        if value.get("destructiveHint")} == {"delete_task", "crm_merge_contacts"})
    schemas = {t["name"]: t for t in resp["result"]["tools"]}
    delete_schema = schemas["delete_task"]["inputSchema"]
    check("delete_task schema takes one id, up to 50 ids, and a reason",
          set(delete_schema["properties"]) == {"task_id", "task_ids", "reason"}
          and delete_schema["properties"]["task_ids"]["maxItems"] == 50
          and delete_schema["properties"]["reason"]["maxLength"] == 500
          and "required" not in delete_schema
          and annotations["delete_task"].get("readOnlyHint") is False
          and annotations["restore_task"].get("readOnlyHint") is False)
    check("delete_task says it is restorable",
          "restorable" in schemas["delete_task"]["description"] and
          "30 days" in schemas["delete_task"]["description"] and
          "30 days" in schemas["restore_task"]["description"])
    check("restore_task needs a task id",
          schemas["restore_task"]["inputSchema"]["required"] == ["task_id"])
    check("list_tasks can list the trash",
          "deleted" in schemas["list_tasks"]["inputSchema"]["properties"]
          ["status"]["enum"])
    check("delete and restore are connector-gated writes",
          {"delete_task", "restore_task"} <= srv.WRITE_TOOLS)
    init = rpc({"jsonrpc": "2.0", "id": 4, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18"}})
    instructions = init["result"]["instructions"]
    check("instructions list soft task deletion truthfully",
          "delete_task" in instructions and "restore_task" in instructions and
          "30 days" in instructions and "soft delete" in instructions and
          "push, deletion," not in instructions and
          "no shell, arbitrary filesystem, credential, code-write, push, or "
          "administration tool is exposed" in instructions)

    def call(name, args):
        r = rpc({"jsonrpc": "2.0", "id": 9, "method": "tools/call",
                 "params": {"name": name, "arguments": args}})
        res = r["result"]
        return res["content"][0]["text"], res.get("isError", False)

    text, err = call("list_repos", {})
    check("list_repos", not err and json.loads(text)[0]["name"] == "repo")
    check("list_repos counts seeded open issue",
          json.loads(text)[0]["openIssues"] == 1)
    text, err = call("list_repositories", {})
    repositories = json.loads(text)
    check("list_repositories hides server paths",
          not err and repositories[0]["headCommit"] == initial_commit and
          "path" not in repositories[0])

    text, err = call("list_files", {})
    root_entries = {entry["path"]: entry for entry in json.loads(text)}
    check("list_files returns the tracked root tree", not err and
          root_entries["src"]["type"] == "directory" and
          root_entries["outside-link"]["type"] == "symlink" and
          "local-secret.env" not in root_entries)
    text, err = call("list_files", {"path": "src", "ref": initial_commit})
    check("list_files reads a directory at a ref", not err and
          {entry["name"] for entry in json.loads(text)} ==
          {"auth.py", "binary.dat"})
    _, err = call("list_files", {"path": "src/auth.py"})
    check("list_files rejects a file path", err)

    text, err = call("read_file", {"path": "README.md"})
    check("read_file", not err and text == "hello mesh\n")
    text, err = call("read_file", {"path": "outside-link"})
    check("read_file reads a symlink object, not its filesystem target",
          not err and text == "/etc/passwd")
    _, err = call("read_file", {"path": "src/binary.dat"})
    check("read_file rejects binary blobs", err)
    _, err = call("read_file", {"path": "local-secret.env"})
    check("read_file cannot expose an untracked credential", err)
    _, err = call("read_file", {"path": ".git/config"})
    check("read_file cannot expose git internals", err)
    _, err = call("read_file", {"path": "../escape"})
    check("read_file blocks traversal", err)
    # Absolute paths must not escape the repo: Path(repo) / "/etc/passwd"
    # collapses to "/etc/passwd", so a ".."-only guard would leak any file
    # (including the node's Ed25519 identity key).
    _, err = call("read_file", {"path": "/etc/passwd"})
    check("read_file blocks absolute path", err)

    text, err = call("search_code", {
        "query": "authenticate_ssh", "ref": initial_commit})
    code_matches = json.loads(text)
    check("search_code returns bounded path/line/text matches", not err and
          code_matches == [{
              "path": "src/auth.py", "line": 1,
              "text": "def authenticate_ssh(key):"}])
    text, err = call("search_code", {"query": "TOKEN=do-not-expose"})
    check("search_code ignores untracked files", not err and json.loads(text) == [])
    _, err = call("search_code", {"query": ""})
    check("search_code rejects an empty query", err)

    text, err = call("create_note", {
        "title": "Decentralized Mirrors",
        "content": "# Mirrors\n\nA local-first design.",
        "tags": ["forkmesh", "Architecture", "forkmesh"],
    })
    personal_note = json.loads(text)
    personal_id = personal_note["id"]
    personal_files = list((tmp / "personal-notes").glob("*.md"))
    check("create_note creates signed personal Markdown", not err and
          len(personal_files) == 1 and personal_note["signatureValid"] and
          personal_note["tags"] == ["forkmesh", "Architecture"] and
          personal_files[0].stat().st_mode & 0o077 == 0 and
          (tmp / "personal-notes").stat().st_mode & 0o077 == 0)
    personal_record = srv.read_note(personal_files[0], "")
    check("personal note signature verifies over exact Markdown",
          personal_record["signatureValid"] and
          personal_record["content"] == "# Mirrors\n\nA local-first design.")

    text, err = call("get_note", {"note_id": personal_id})
    fetched_note = json.loads(text)
    check("get_note returns content by stable id", not err and
          fetched_note["content"].endswith("local-first design.") and
          fetched_note["repo"] is None)
    text, err = call("list_notes", {})
    check("list_notes includes personal notes", not err and
          personal_id in {note["id"] for note in json.loads(text)})
    text, err = call("search_notes", {"query": "LOCAL-FIRST"})
    check("search_notes searches Markdown case-insensitively", not err and
          [note["id"] for note in json.loads(text)] == [personal_id])

    old_signature = personal_note["signature"]
    text, err = call("update_note", {
        "note_id": personal_id,
        "content": "# Mirrors\n\nReplicate signed records.",
    })
    updated_personal = json.loads(text)
    check("update_note versions and re-signs a personal note", not err and
          updated_personal["version"] == 2 and
          updated_personal["signature"] != old_signature and
          srv.read_note(personal_files[0], "")["signatureValid"])

    outside_notes = tmp / "outside-notes"
    outside_notes.mkdir()
    repository_notes_root = repo / ".forkmesh/notes"
    os.symlink(outside_notes, repository_notes_root)
    _, err = call("create_note", {
        "title": "Escaping Note", "content": "must stay in the repo",
        "repo": "repo",
    })
    check("repository note writes reject symlink escapes", err and
          not list(outside_notes.iterdir()))
    repository_notes_root.unlink()

    repository_notes_root.mkdir(parents=True)
    unrelated_draft = repository_notes_root / "unrelated-draft.md"
    unrelated_draft.write_text("uncommitted local draft\n")

    text, err = call("create_note", {
        "title": "Mirror Authentication",
        "content": "SSH access is capability-scoped.",
        "repo": "repo",
        "tags": ["authentication"],
    })
    repository_note = json.loads(text)
    repository_id = repository_note["id"]
    repository_note_path = repo / ".forkmesh/notes/mirror-authentication.md"
    check("repository note uses the portable Markdown layout", not err and
          repository_note_path.is_file() and repository_note["repo"] == "repo")
    check("repository note is committed with focused history",
          last_commit() == "note: Mirror Authentication" and
          git("status", "--short", "--",
              ".forkmesh/notes/unrelated-draft.md").stdout.startswith("??") and
          ".forkmesh/notes/unrelated-draft.md" not in
          git("show", "--format=", "--name-only", "HEAD").stdout)
    text, err = call("list_notes", {"repo": "repo"})
    listed_repo_notes = json.loads(text)
    check("list_notes repository filter excludes personal notes", not err and
          [note["id"] for note in listed_repo_notes] == [repository_id])
    text, err = call("update_note", {
        "note_id": repository_id,
        "content": "SSH access uses signed, scoped proofs.",
    })
    updated_repository = json.loads(text)
    check("repository note updates are signed and committed", not err and
          updated_repository["version"] == 2 and
          srv.read_note(repository_note_path, "repo")["signatureValid"] and
          last_commit() == "note: update Mirror Authentication")
    tampered = dict(srv.read_note(repository_note_path, "repo"))
    tampered["content"] += " tampered"
    check("note signatures detect content tampering", not srv.verify_note(tampered))
    tampered_author = dict(srv.read_note(repository_note_path, "repo"))
    tampered_author["authorName"] = "Mallory"
    check("note signatures cover author metadata",
          not srv.verify_note(tampered_author))

    class FakeWebSocket:
        def __init__(self):
            self.incoming = bytearray()
            self.frames = []

        def settimeout(self, _timeout):
            pass

        def sendall(self, payload):
            if payload.startswith(b"GET "):
                request_text = payload.decode("ascii")
                key = next(
                    line.split(":", 1)[1].strip()
                    for line in request_text.split("\r\n")
                    if line.lower().startswith("sec-websocket-key:"))
                accept = base64.b64encode(hashlib.sha1(
                    (key + srv.WS_GUID).encode("ascii")).digest()).decode()
                history_end = b'{"kind":"forkmesh-history-end","v":1}'
                server_frame = bytes((0x81, len(history_end))) + history_end
                self.incoming.extend(
                    ("HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                     f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii")
                    + server_frame)
            else:
                self.frames.append(payload)

        def recv(self, size):
            value = bytes(self.incoming[:size])
            del self.incoming[:size]
            return value

        def close(self):
            pass

    fake_socket = FakeWebSocket()
    real_create_connection = srv.socket.create_connection
    srv.socket.create_connection = lambda _address, timeout: fake_socket
    try:
        websocket = srv.ChatWebSocket("ws://relay.test/room", 1)
        history_marker = json.loads(websocket.receive_text())
        websocket.send_text("hello mesh")
        websocket.close()
    finally:
        srv.socket.create_connection = real_create_connection
    sent_frame = fake_socket.frames[0]
    sent_length = sent_frame[1] & 0x7f
    sent_mask = sent_frame[2:6]
    sent_body = bytes(
        byte ^ sent_mask[index % 4]
        for index, byte in enumerate(sent_frame[6:6 + sent_length]))
    check("chat WebSocket performs the RFC 6455 handshake and masking",
          history_marker["kind"] == "forkmesh-history-end" and
          sent_frame[0] == 0x81 and sent_frame[1] & 0x80 and
          sent_body == b"hello mesh")

    chat_passphrase = "test-room-passphrase"
    chat_key = srv.derive_chat_key(chat_passphrase)
    chat_plains = [
        {"type": "chat", "id": "m1", "senderId": "alice-key",
         "sender": "alice", "accountKind": "user", "ts": 10,
         "channel": "#general", "text": "before edit"},
        {"type": "chat", "id": "m2", "senderId": "bob-key",
         "sender": "bob", "accountKind": "user", "ts": 11,
         "channel": "#general", "text": "delete me"},
        {"type": "edit", "id": "e1", "senderId": "alice-key",
         "accountKind": "user", "ts": 12, "target": "m1",
         "text": "after edit"},
        {"type": "delete", "id": "d1", "senderId": "bob-key",
         "accountKind": "user", "ts": 13, "target": "m2"},
        {"type": "chat", "id": "private", "senderId": "mallory",
         "sender": "mallory", "ts": 14, "channel": "#other",
         "text": "wrong room"},
    ]
    chat_envelopes = [
        srv.encrypt_chat_plain(plain, chat_key, persist=True)
        for plain in chat_plains]
    real_passphrase = srv.chat_room_passphrase
    real_history = srv.chat_history_envelopes
    real_post = srv.chat_post_envelope
    posted = []
    srv.chat_room_passphrase = lambda: chat_passphrase
    srv.chat_history_envelopes = lambda: chat_envelopes
    srv.chat_post_envelope = posted.append
    try:
        text, err = call("read_chat_messages", {"room": "general", "limit": 10})
        chat_messages = json.loads(text)
        check("read_chat_messages decrypts and folds retained history", not err and
              len(chat_messages) == 1 and
              chat_messages[0]["id"] == "m1" and
              chat_messages[0]["message"] == "after edit" and
              chat_messages[0]["edited"] is True)
        sealed_envelope = srv.encrypt_chat_plain(
            {"type": "sealed", "v": 2, "kid": "SQAm1SbhWWBxNiSt"},
            chat_key, persist=True)
        srv.chat_history_envelopes = lambda: [sealed_envelope]
        text, err = call("read_chat_messages", {
            "room": "general", "limit": 10})
        check("controller connector identifies sealed-only legacy history",
              err and "participant devices" in text)
        srv.chat_history_envelopes = lambda: [sealed_envelope] + chat_envelopes
        text, err = call("read_chat_messages", {
            "room": "general", "limit": 10})
        mixed_messages = json.loads(text)
        check("controller connector skips old seals and returns public history",
              not err and len(mixed_messages) == 1 and
              mixed_messages[0]["message"] == "after edit")
        srv.chat_history_envelopes = lambda: [
            sealed_envelope, chat_envelopes[1], chat_envelopes[3]]
        text, err = call("read_chat_messages", {
            "room": "general", "limit": 10})
        check("empty folded public history is not mistaken for sealed-only",
              not err and json.loads(text) == [])
        srv.chat_history_envelopes = lambda: chat_envelopes
        _, err = call("read_chat_messages", {"room": "private", "limit": 10})
        check("chat reads are restricted to #general", err)

        text, err = call("post_chat_message", {
            "room": "#general", "message": "Architecture review complete."})
        post_result = json.loads(text)
        posted_plain = (
            srv.decrypt_chat_envelope(posted[0], chat_key) if posted else None)
        check("post_chat_message uses the public durable room protocol",
              not err and post_result["room"] == "general" and
              len(posted) == 1 and posted[0].get("persist") is True and
              posted_plain["channel"] == "#general" and
              posted_plain["text"] == "Architecture review complete." and
              posted_plain["senderId"] == pub_b64)
        _, err = call("post_chat_message", {
            "room": "engineering", "message": "not allowed"})
        check("chat posts are restricted to #general", err)
    finally:
        srv.chat_room_passphrase = real_passphrase
        srv.chat_history_envelopes = real_history
        srv.chat_post_envelope = real_post

    text, err = call("create_issue", {
        "title": "First bug", "body": "it broke", "labels": ["bug"],
        "start_date": "2026-07-20", "end_date": "2026-07-24"})
    check("create_issue spans open+closed numbering", not err and "#3" in text)
    ipath = repo / ".forkmesh/issues/open/3/issue-3.json"
    check("issue record at native path", ipath.is_file())
    check("no legacy markdown layout",
          not (repo / "issues").exists() and
          not list(repo.glob(".forkmesh/issues/**/issue.md")))
    rec = json.loads(ipath.read_text())
    check("issue loads without repair (strict native schema)",
          issue_loads_strict(rec, 3))
    check("issue record fields", rec["title"] == "First bug" and
          rec["status"] == "open" and rec["labels"] == ["bug"] and
          rec["startDate"] > 0 and rec["endDate"] > rec["startDate"])
    check("issue has open + dates events",
          [e["type"] for e in rec["events"]] == ["open", "dates"])
    check("every issue event signature verifies",
          all(verify_issue_event(pub_b64, 3, e) for e in rec["events"]))
    check("issue committed with native message",
          last_commit() == "issue #3: First bug")

    text, err = call("create_issue", {
        "title": "Second bug", "body": "also broke",
        "start_date": 1784433600000, "end_date": 1784520000000})
    check("second dated issue is #4", not err and "#4" in text)
    rec4 = json.loads(
        (repo / ".forkmesh/issues/open/4/issue-4.json").read_text())
    check("second issue verifies + strict", issue_loads_strict(rec4, 4) and
          all(verify_issue_event(pub_b64, 4, e) for e in rec4["events"]))

    _, err = call("create_issue", {"title": "Bad", "start_date": "not-a-date"})
    check("create_issue rejects invalid date", err)
    _, err = call("create_issue", {"title": "Bad", "start_date": "2026-07-24",
                                   "end_date": "2026-07-20"})
    check("create_issue rejects end before start", err)
    check("no partial record after date errors",
          not any(d.exists() for d in srv.issue_dir_candidates(repo, 5)))

    real_next = srv.next_issue_number
    calls = {"n": 0}

    def stale_next(r):
        calls["n"] += 1
        return 3 if calls["n"] == 1 else real_next(r)

    srv.next_issue_number = stale_next
    try:
        text, err = call("create_issue", {"title": "After stale read"})
    finally:
        srv.next_issue_number = real_next
    check("stale-number collision retries", not err and "#5" in text)

    real_sign = srv.sign_issue_event
    raced = {"done": False}

    def racing_sign(number, ev):
        if not raced["done"]:
            raced["done"] = True
            d = repo / f".forkmesh/issues/closed/{number}"
            d.mkdir(parents=True)
            (d / f"issue-{number}.json").write_text(json.dumps({
                "schema": "forkmesh-issue-v1", "number": number,
                "title": "raced", "status": "closed", "events": []}) + "\n")
        return real_sign(number, ev)

    srv.sign_issue_event = racing_sign
    try:
        text, err = call("create_issue", {"title": "After race"})
    finally:
        srv.sign_issue_event = real_sign
    check("post-write collision re-reads before commit",
          not err and "#7" in text and
          not (repo / ".forkmesh/issues/open/6").exists() and
          (repo / ".forkmesh/issues/open/7/issue-7.json").is_file())

    text, err = call("search_issues", {"query": "bug"})
    check("search_issues match", not err and
          [r["number"] for r in json.loads(text)] == [3, 4])
    text, _ = call("search_issues", {"query": "", "status": "closed"})
    check("search_issues closed folder",
          2 in [r["number"] for r in json.loads(text)])
    text, _ = call("search_issues", {"query": "nonexistent-xyz"})
    check("search_issues no match", json.loads(text) == [])

    _, err = call("comment_on_issue", {"number": 3, "body": "me too"})
    check("comment_on_issue", not err)
    rec = json.loads(ipath.read_text())
    ev = rec["events"][-1]
    check("comment appended to record", ev["type"] == "comment" and
          ev["body"] == "me too")
    check("comment signature verifies", verify_issue_event(pub_b64, 3, ev))
    check("comment committed", last_commit() == "issue #3: comment")
    _, err = call("comment_on_issue", {"number": 99, "body": "x"})
    check("comment_on_issue unknown issue", err)

    _, err = call("create_milestone", {"title": "Sprint A",
                                       "due": "2026-08-01",
                                       "description": "first sprint"})
    check("create_milestone", not err)
    _, err = call("create_milestone", {"title": "Sprint B"})
    check("second milestone", not err)
    ms = json.loads((repo / ".forkmesh/issues/milestones.json").read_text())
    by_title = {m["title"]: m for m in ms}
    check("milestones.json has both", set(by_title) == {"Sprint A", "Sprint B"})
    check("milestone fields", by_title["Sprint A"]["due"] > 0 and
          by_title["Sprint A"]["status"] == "open" and
          by_title["Sprint A"]["description"] == "first sprint" and
          by_title["Sprint B"]["due"] == 0)
    check("milestones committed", last_commit() == "issues: update milestones")
    text, err = call("create_milestone", {"title": "Sprint A"})
    check("duplicate milestone title rejected", err and "already exists" in text)

    _, err = call("update_milestone", {"title": "Sprint B",
                                       "due": 1785556800000,
                                       "status": "closed"})
    ms = json.loads((repo / ".forkmesh/issues/milestones.json").read_text())
    by_title = {m["title"]: m for m in ms}
    check("update_milestone", not err and
          by_title["Sprint B"]["due"] == 1785556800000 and
          by_title["Sprint B"]["status"] == "closed")
    text, err = call("update_milestone", {"title": "Nope", "due": 1})
    check("update_milestone unknown title", err and "unknown milestone" in text)
    _, err = call("update_milestone", {"title": "Sprint A", "status": "wat"})
    check("update_milestone bad status", err)

    text, err = call("create_project", {
        "title": "Roadmap Q3", "body": "the plan",
        "start_date": "2026-07-20", "end_date": "2026-08-01",
        "milestone": "Sprint A", "issues": [4, 3, 4]})
    check("create_project past existing project", not err and "#2" in text)
    ppath = repo / ".forkmesh/projects/2/project-2.json"
    check("project record at native path", ppath.is_file())
    prec = json.loads(ppath.read_text())
    check("project loads without repair (native schema)",
          project_loads_strict(prec, 2))
    check("project links sorted deduped issues", prec["issues"] == [3, 4])
    check("project fields", prec["title"] == "Roadmap Q3" and
          prec["milestone"] == "Sprint A" and prec["status"] == "open" and
          prec["startDate"] > 0 and prec["endDate"] > prec["startDate"])
    check("project events open/dates/milestone/issues",
          [e["type"] for e in prec["events"]] ==
          ["open", "dates", "milestone", "issues"])
    check("every project event signature verifies",
          all(verify_project_event(pub_b64, 2, e) for e in prec["events"]))
    check("project committed with native message",
          last_commit() == "projects: #2 Roadmap Q3")

    text, err = call("create_project", {"title": "Bad", "issues": [999]})
    check("create_project unknown issue link", err and "#999" in text)
    text, err = call("create_project", {"title": "Bad", "milestone": "Nope"})
    check("create_project unknown milestone",
          err and "unknown milestone" in text)
    _, err = call("create_project", {"title": "Bad", "start_date": "junk"})
    check("create_project invalid date", err)
    check("no partial project record after errors",
          not (repo / ".forkmesh/projects/3").exists())

    _, err = call("update_project", {"number": 2, "issues": [3],
                                     "milestone": "Sprint B"})
    check("update_project", not err)
    prec = json.loads(ppath.read_text())
    check("update_project folds into metadata", prec["issues"] == [3] and
          prec["milestone"] == "Sprint B")
    check("update_project appended signed events",
          [e["type"] for e in prec["events"][-2:]] == ["milestone", "issues"]
          and all(verify_project_event(pub_b64, 2, e)
                  for e in prec["events"][-2:]))
    check("update_project committed", last_commit() == "projects: #2 issues")
    _, err = call("update_project", {"number": 2,
                                     "start_date": 1, "end_date": 2})
    prec = json.loads(ppath.read_text())
    check("update_project dates", not err and prec["startDate"] == 1 and
          prec["endDate"] == 2 and last_commit() == "projects: #2 dates")
    _, err = call("update_project", {"number": 42, "issues": [3]})
    check("update_project unknown project", err)
    _, err = call("update_project", {"number": 2})
    check("update_project needs a field", err)
    _, err = call("update_project", {"number": 2, "issues": [999]})
    check("update_project unknown issue link", err)

    git("checkout", "-q", "-b", "feature")
    (repo / "feature.txt").write_text("new\n")
    git("add", "feature.txt")
    git("commit", "-qm", "add feature")
    git("checkout", "-q", "main")
    text, err = call("open_pr_from_branch", {"branch": "feature",
                                             "title": "Add feature"})
    check("open_pr_from_branch", not err and "#1" in text)
    check("pull.md written", (repo / ".forkmesh/pulls/1/pull.md").exists())
    pull_md = (repo / ".forkmesh/pulls/1/pull.md").read_text()
    check("agent PR stores immutable branch pointers",
          "derive: branch" in pull_md and
          "creationBaseOid:" in pull_md and "creationHeadOid:" in pull_md)
    check("agent PR does not duplicate patch payloads",
          not (repo / ".forkmesh/pulls/1/changes.patch").exists() and
          not (repo / ".forkmesh/pulls/1/commits.mbox").exists())
    # The paid-microjob marker. The relay re-reads these two fields from the
    # merged pull and re-verifies the signature against the node that opened
    # it, so a drift here silently stops paying for MCP-opened work.
    fields = dict(
        line.split(": ", 1) for line in pull_md.splitlines()
        if ": " in line and not line.startswith("---"))
    check("agent PR carries the MCP client marker",
          fields.get("client") == "forkmesh-mcp-v1" and
          bool(fields.get("clientSig")))
    check("MCP marker signature covers this exact pull",
          _verify(fields.get("author", ""), fields.get("clientSig", ""),
                  srv.mcp_pull_canonical(
                      1, fields.get("base", ""), fields.get("head", ""),
                      fields.get("author", ""),
                      int(fields.get("ts", "0"))).encode()))
    check("MCP marker signature is bound to the pull it opened",
          not _verify(fields.get("author", ""), fields.get("clientSig", ""),
                      srv.mcp_pull_canonical(
                          2, fields.get("base", ""), fields.get("head", ""),
                          fields.get("author", ""),
                          int(fields.get("ts", "0"))).encode()))
    text, err = call("open_pr_from_branch", {"branch": "feature"})
    check("open_pr idempotent", not err and "already open" in text)
    text, err = call("get_pr_diff", {"number": 1})
    check("get_pr_diff", not err and "feature.txt" in text)
    pull_path = repo / ".forkmesh/pulls/1/pull.md"
    open_metadata = pull_path.read_text()
    pull_path.write_text(open_metadata.replace("status: open", "status: merged"))
    text, err = call("get_pr_diff", {"number": 1})
    check("get_pr_diff refuses non-open pulls",
          err and "diff is no longer retained" in text)
    injected = tmp / "pwned-by-pull-metadata"
    pull_path.write_text(open_metadata.replace(
        "base: main", f"base: --output={injected}"))
    text, err = call("get_pr_diff", {"number": 1})
    check("get_pr_diff refuses option-shaped refs from pull metadata",
          err and not any(tmp.glob("pwned-by-pull-metadata*")))
    pull_path.write_text(open_metadata)
    injected = tmp / "pwned-by-pr-argument"
    text, err = call("open_pr_from_branch", {
        "branch": "feature", "base": f"--output={injected}"})
    check("open_pr_from_branch refuses option-shaped base",
          err and not any(tmp.glob("pwned-by-pr-argument*")))
    text, err = call("open_pr_from_branch", {
        "branch": f"--output={injected}"})
    check("open_pr_from_branch refuses option-shaped branch",
          err and not any(tmp.glob("pwned-by-pr-argument*")))

    # --- Organization CRM over the signed /api/crm API -----------------------
    settings_file = tmp / "config/ForkMesh/ForkMesh.conf"
    settings_file.parent.mkdir(parents=True)
    settings_file.write_text(
        "[account]\nnodeName=Seller\n\n[chat]\nnodeName=wrong\n")
    os.environ["FORKMESH_SETTINGS"] = str(settings_file)
    os.environ.pop("FORKMESH_ACCOUNT", None)
    os.environ.pop("FORKMESH_CRM_ORIGIN", None)
    check("crm_account reads the desktop's account/nodeName",
          srv.crm_account() == "seller")

    crm_contact_a = "a" * 32
    crm_contact_b = "b" * 32
    crm_state = {
        "contacts": {
            crm_contact_a: {
                "id": crm_contact_a, "name": "Ada", "email": "ada@example.test",
                "phone": "555", "company": "Engines", "title": "CTO",
                "flow": "customers", "stage": "lead", "tags": ["inbound"],
                "notes": "Met at a meetup.", "owner": "", "source": ""},
            crm_contact_b: {
                "id": crm_contact_b, "name": "Bob", "email": "bob@example.test",
                "company": "Bakery", "flow": "partnerships",
                "stage": "prospect", "tags": [], "notes": ""},
        },
        "emails": [
            {"id": "e1", "contactId": crm_contact_a, "direction": "inbound",
             "subject": "Pricing?"},
            {"id": "e2", "contactId": crm_contact_b, "direction": "outbound",
             "subject": "Hello"}],
        "mail": [
            {"id": "m1", "from": "Ada <ada@example.test>", "subject": "Re"},
            {"id": "m2", "from": "someone@else.test", "subject": "Other"}],
    }
    crm_requests = []

    class FakeHTTPResponse(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

    def fake_urlopen(request, timeout):
        url = srv.urllib.parse.urlsplit(request.full_url)
        params = dict(srv.urllib.parse.parse_qsl(url.query))
        method = request.get_method()
        body = request.data
        resource = method + "\n" + url.path
        if body is not None:
            resource += "\n" + hashlib.sha256(body).hexdigest()
        proof = ("forkmesh-marketing-request-v1"
                 if url.path.startswith("/api/marketing") else "forkmesh-crm-request-v1")
        canonical = "\n".join((proof, params["node"], resource, params["ts"]))
        crm_requests.append({
            "origin": f"{url.scheme}://{url.netloc}", "method": method,
            "path": url.path, "node": params["node"],
            "signed": _verify(pub_b64, params["sig"], canonical.encode()),
            "body": json.loads(body) if body else None})
        path = url.path
        if path == "/api/marketing":
            payload = {"ok": True, "organizations": [
                {"name": "acme", "installed": True}, {"name": "other", "installed": False}]}
        elif path == "/api/marketing/acme":
            payload = {"ok": True, "settings": {
                "channels": [{"id": "x", "handle": "@forkmesh", "cadenceDays": 2},
                             {"id": "blog"}], "x": {"connected": True}},
                "posts": [
                    {"channel": "x", "url": "https://x.com/forkmesh/status/11111",
                     "title": "Quiet post", "postedAt": 3, "likes": 1, "engagements": 1},
                    {"channel": "x", "url": "https://x.com/forkmesh/status/22222",
                     "title": "Loud post", "postedAt": 1, "likes": 40, "replies": 9,
                     "engagements": 49},
                    {"channel": "reddit", "url": "https://reddit.com/r/a",
                     "title": "Other channel", "engagements": 900}],
                "postsSummary": {"channels": [{"channel": "x", "posts": 2}]},
                "content": [{"channel": "x", "title": "Draft", "body": "b" * 900,
                             "status": "draft"}],
                "metrics": [{"channel": "x", "observedAt": 5, "followers": 120},
                            {"channel": "blog", "observedAt": 6, "views": 9}],
                "campaigns": [{"name": "Launch week", "status": "active",
                               "message": "Own your code"},
                              {"name": "Old", "status": "completed"}],
                "readyPosts": {"x": {"channel": "x", "post": None,
                                     "needsPost": True, "history": []}}}
        elif path == "/api/marketing/acme/ready-post":
            saved = json.loads(body)
            payload = {"ok": True, "readyPost": {"channel": "x", "needsPost": False,
                "post": {"id": "c" * 32, "text": saved["text"],
                         "imageDataUrl": saved["imageDataUrl"], "hasImage": True},
                "history": []}}
        elif path == "/api/crm":
            payload = {"ok": True, "actor": "seller", "organizations": [
                {"name": "acme", "role": "owner", "installed": True}]}
        elif path == "/api/crm/nocrm/contacts":
            raise srv.urllib.error.HTTPError(
                request.full_url, 409, "Conflict", {},
                io.BytesIO(b'{"error":"plugin_not_installed"}'))
        elif path == "/api/crm/acme/settings":
            payload = {"ok": True, "settings": {
                "flows": [
                    {"id": "partnerships", "name": "Partnerships", "stages": [
                        "prospect", "contacted", "discovery", "negotiation",
                        "active", "closed"]},
                    {"id": "customers", "name": "Customers", "stages": [
                        "lead", "qualified", "trial", "customer", "renewal",
                        "inactive"]}],
                "mailBoxes": ["Inbox"], "mailProvider": "zoho",
                "providerName": "Zoho", "mailtrapConfigured": False,
                "fromEmail": "sales@acme.test",
                "zoho": {"connected": True, "lastSyncAt": 5,
                         "accountId": "secret-account"}}}
        elif path == "/api/crm/acme/contacts":
            payload = {"ok": True,
                       "contacts": list(crm_state["contacts"].values())}
        elif path == "/api/crm/acme/contacts/duplicates":
            payload = {"ok": True, "groups": [
                {"contacts": [crm_contact_a, crm_contact_b], "signals": ["name"]}]}
        elif path == "/api/crm/acme/contacts/evidence":
            payload = {"ok": True, "checked": 2, "accounts": 1, "paying": 1,
                       "trial": 0, "ahead": 1, "changed": 2}
        elif path == "/api/crm/acme/contacts/merge":
            merged = json.loads(body)
            gone = crm_state["contacts"].pop(merged["from"])
            kept = crm_state["contacts"][merged["into"]]
            kept["notes"] = "\n\n".join(
                note for note in (kept["notes"], gone["notes"]) if note)
            payload = {"ok": True, "merged": merged["from"], "contact": kept}
        elif path.startswith("/api/crm/acme/contacts/"):
            contact = crm_state["contacts"][path.rsplit("/", 1)[1]]
            if method == "PATCH":
                contact.update(json.loads(body))
            payload = {"ok": True, "contact": contact}
        elif path == "/api/crm/acme/emails":
            payload = {"ok": True, "emails": crm_state["emails"]}
        elif path == "/api/crm/acme/mail":
            payload = {"ok": True, "mail": crm_state["mail"]}
        else:
            raise AssertionError(path)
        return FakeHTTPResponse(json.dumps(payload).encode())

    real_urlopen = srv.urllib.request.urlopen
    srv.urllib.request.urlopen = fake_urlopen
    try:
        text, err = call("crm_list_organizations", {})
        check("crm_list_organizations signs as the account with the identity key",
              not err and json.loads(text)["organizations"][0]["name"] == "acme"
              and crm_requests[-1]["signed"] and crm_requests[-1]["node"] == "seller"
              and crm_requests[-1]["origin"] == "https://app.forkmesh.com")

        text, err = call("crm_get_pipeline", {"organization": "acme"})
        pipeline = json.loads(text) if not err else {}
        customers = next((f for f in pipeline.get("flows", [])
                          if f["id"] == "customers"), {})
        check("crm_get_pipeline counts contacts per flow stage",
              not err and pipeline["contacts"] == 2 and
              customers["stages"][0] == {"stage": "lead", "contacts": 1} and
              pipeline["zoho"] == {"connected": True, "lastSyncAt": 5} and
              "secret-account" not in text and "sales@acme.test" not in text)

        text, err = call("crm_list_contacts", {"organization": "acme",
                                               "query": "engines"})
        listed = json.loads(text) if not err else {}
        check("crm_list_contacts filters and omits email, phone, and notes",
              not err and listed["total"] == 2 and listed["matched"] == 1 and
              listed["contacts"][0]["id"] == crm_contact_a and
              "email" not in listed["contacts"][0] and
              "ada@example.test" not in text and "Met at" not in text)
        text, err = call("crm_list_contacts", {
            "organization": "acme", "flow": "partnerships",
            "include_details": True})
        check("crm_list_contacts include_details returns contact details",
              not err and json.loads(text)["contacts"][0]["email"] ==
              "bob@example.test")

        text, err = call("crm_get_contact", {
            "organization": "acme", "contact_id": crm_contact_a})
        check("crm_get_contact returns notes",
              not err and json.loads(text)["notes"] == "Met at a meetup.")
        _, err = call("crm_get_contact", {
            "organization": "acme", "contact_id": "../../api/admin"})
        check("crm_get_contact rejects a non-id before any request", err)
        _, err = call("crm_list_contacts", {"organization": "Acme/../x"})
        check("crm tools reject a malformed organization", err)
        text, err = call("crm_list_contacts", {"organization": "nocrm"})
        check("crm tools explain an uninstalled CRM package",
              err and "plugin_not_installed" in text and "not installed" in text)

        text, err = call("crm_list_activity", {
            "organization": "acme", "contact_id": crm_contact_a})
        activity = json.loads(text) if not err else {}
        check("crm_list_activity scopes emails and mail to one contact",
              not err and [e["id"] for e in activity["emails"]] == ["e1"] and
              [m["id"] for m in activity["mail"]] == ["m1"])

        before = len(crm_requests)
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "stage": "qualified"})
        check("crm_update_contact refuses a stage change without evidence",
              err and "append_note" in text and len(crm_requests) == before)
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "stage": "trial", "append_note": "Replied."})
        check("crm_update_contact refuses skipping a stage",
              err and "one stage at a time" in text and
              not any(r["method"] == "PATCH" for r in crm_requests))
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "stage": "won", "append_note": "Replied."})
        check("crm_update_contact refuses a stage outside the flow",
              err and "not in flow" in text)
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "flow": "sales", "stage": "lead", "append_note": "Moved."})
        check("crm_update_contact refuses an invented flow",
              err and "unknown flow" in text)

        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "stage": "qualified", "owner": "seller", "source": "meetup",
            "append_note": "Inbound pricing question recorded as email e1."})
        patch = crm_requests[-1]
        updated = json.loads(text) if not err else {}
        check("crm_update_contact advances one stage with a signed PATCH",
              not err and patch["method"] == "PATCH" and patch["signed"] and
              patch["body"]["stage"] == "qualified" and
              patch["body"]["flow"] == "customers" and
              patch["body"]["owner"] == "seller" and
              "email" not in patch["body"] and updated["stage"] == "qualified")
        check("crm_update_contact appends a dated, attributed note",
              updated.get("notes", "").startswith("Met at a meetup.\n\n[") and
              "seller] Inbound pricing question" in updated.get("notes", ""))
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "stage": "inactive", "append_note": "Bounced; company closed."})
        check("crm_update_contact allows closing a contact from any stage",
              not err and json.loads(text)["stage"] == "inactive")
        _, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a})
        check("crm_update_contact refuses an empty update", err)
        text, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "fields": {"other_emails": ""}})
        check("crm_update_contact clears a custom column",
              not err and crm_requests[-1]["body"] == {"fields": {"other_emails": ""}})
        _, err = call("crm_update_contact", {
            "organization": "acme", "contact_id": crm_contact_a,
            "fields": {"Other Emails": "x"}})
        check("crm_update_contact refuses a malformed column key", err)
        text, err = call("crm_find_duplicates", {"organization": "acme"})
        found = json.loads(text) if not err else {}
        check("crm_find_duplicates groups contacts without their addresses",
              not err and found["groups"][0]["signals"] == ["name"] and
              [c["id"] for c in found["groups"][0]["contacts"]] ==
              [crm_contact_a, crm_contact_b] and
              "email" not in found["groups"][0]["contacts"][0])
        sent = len(crm_requests)
        _, err = call("crm_merge_contacts", {
            "organization": "acme", "duplicate_id": crm_contact_b,
            "keep_id": crm_contact_a})
        check("crm_merge_contacts needs evidence", err and len(crm_requests) == sent)
        _, err = call("crm_merge_contacts", {
            "organization": "acme", "duplicate_id": crm_contact_a,
            "keep_id": crm_contact_a, "append_note": "same"})
        check("crm_merge_contacts needs two contacts", err)
        text, err = call("crm_merge_contacts", {
            "organization": "acme", "duplicate_id": crm_contact_b,
            "keep_id": crm_contact_a,
            "append_note": "Same GitHub profile on both records."})
        merged = json.loads(text) if not err else {}
        merge_request = next(r for r in crm_requests[sent:]
                             if r["path"].endswith("/contacts/merge"))
        check("crm_merge_contacts merges with a signed request and notes why",
              not err and merge_request["signed"] and
              merge_request["body"] == {"from": crm_contact_b, "into": crm_contact_a}
              and "Same GitHub profile" in merged.get("notes", "") and
              crm_contact_b not in crm_state["contacts"])
        text, err = call("crm_check_accounts", {"organization": "acme"})
        check("crm_check_accounts posts a signed check and returns counts only",
              not err and crm_requests[-1]["signed"] and
              crm_requests[-1]["method"] == "POST" and
              json.loads(text) == {"organization": "acme", "checked": 2,
                                   "accounts": 1, "paying": 1, "trial": 0,
                                   "ahead": 1, "changed": 2})
        check("crm_merge_contacts is a destructive connector-gated write",
              "crm_merge_contacts" in srv.WRITE_TOOLS and
              "crm_merge_contacts" in srv.DESTRUCTIVE_TOOLS)

        text, err = call("marketing_x_context", {})
        context = json.loads(text) if not err else {}
        check("marketing_x_context signs with the marketing proof and keeps X only",
              not err and crm_requests[-1]["signed"] and
              crm_requests[-1]["path"] == "/api/marketing/acme" and
              [post["title"] for post in context["topPosts"]] == ["Loud post", "Quiet post"] and
              [post["title"] for post in context["recentPosts"]] == ["Quiet post", "Loud post"] and
              context["account"] == {"handle": "@forkmesh", "cadenceDays": 2} and
              len(context["content"][0]["body"]) == 600 and
              context["followerTrend"] == [{"observedAt": 5, "followers": 120}] and
              [item["name"] for item in context["campaigns"]] == ["Launch week"] and
              context["readyPost"]["needsPost"] is True and "Other channel" not in text)
        image = tmp / "ready.png"
        image.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\0" * 32)
        text, err = call("marketing_save_ready_post", {
            "organization": "acme", "text": "Own your code.", "image_path": str(image),
            "why": "Ownership posts get replies", "model": "claude-opus-5-5"})
        saved = json.loads(text) if not err else {}
        check("marketing_save_ready_post PUTs a signed PNG data URL and hides it in the answer",
              not err and crm_requests[-1]["signed"] and
              crm_requests[-1]["method"] == "PUT" and
              crm_requests[-1]["body"]["imageDataUrl"].startswith("data:image/png;base64,") and
              crm_requests[-1]["body"]["model"] == "claude-opus-5-5" and
              saved["post"]["text"] == "Own your code." and "imageDataUrl" not in saved["post"])
        image.write_bytes(b"GIF89a")
        text, err = call("marketing_save_ready_post", {
            "organization": "acme", "text": "x", "image_path": str(image)})
        check("marketing_save_ready_post refuses other image types", err and "PNG" in text)
        check("marketing_save_ready_post is a connector-gated write, not destructive",
              "marketing_save_ready_post" in srv.WRITE_TOOLS and
              "marketing_save_ready_post" not in srv.DESTRUCTIVE_TOOLS)
    finally:
        srv.urllib.request.urlopen = real_urlopen

    os.environ["FORKMESH_CRM_ORIGIN"] = "https://relay.test/prefix"
    _, err = call("crm_list_organizations", {})
    check("crm origin may not carry a path the signature would not cover", err)
    os.environ.pop("FORKMESH_CRM_ORIGIN", None)
    settings_file.write_text("[account]\n")
    _, err = call("crm_list_organizations", {})
    check("crm tools need a signed-in account", err)
    os.environ["FORKMESH_ACCOUNT"] = "seller"

    task_id = "c" * 32
    markdown = ("## Why\nThe cores run **hot**.\n\n| Route | Queries |\n"
                "|---|---:|\n| /api/tasks | 12 |\n\n1. one\n2. two\n\n"
                "```\na < b\n```")
    task_requests = []

    class FakeTaskResponse(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            return False

    def fake_task_urlopen(request, timeout):
        url = srv.urllib.parse.urlsplit(request.full_url)
        params = dict(srv.urllib.parse.parse_qsl(url.query))
        body = json.loads(request.data) if request.data else None
        task_requests.append({
            "method": request.get_method(), "path": url.path,
            "params": params, "raw": request.data, "body": body})
        if url.path == "/api/ledger/expenses":
            return FakeTaskResponse(json.dumps({"ok": True, "created": True,
                "expense": {"id": "anthropic-8ryatvg1-0013",
                            "amountMinor": 21775}}).encode())
        if url.path == "/api/notes":
            return FakeTaskResponse(json.dumps({"ok": True, "note": {
                "id": "d" * 32, "title": body["title"], "version": 1,
                "updatedAt": 7}}).encode())
        return FakeTaskResponse(json.dumps(
            {"ok": True, "task": {"id": task_id, **(body or {})}}).encode())

    real_task_urlopen = srv.urllib.request.urlopen
    srv.urllib.request.urlopen = fake_task_urlopen
    try:
        text, err = call("create_task", {
            "title": "Markdown task", "details": markdown,
            "howToTest": "1. Open\n2. Check"})
        sent = task_requests[-1]
        canonical = "\n".join((
            "forkmesh-org-task-open-v1", "seller", sent["params"]["ts"]))
        check("create_task sends markdown details with their newlines",
              not err and sent["method"] == "POST" and
              sent["path"] == "/api/tasks" and
              sent["body"]["details"] == markdown and
              sent["body"]["howToTest"] == "1. Open\n2. Check" and
              b"\\n" in sent["raw"] and
              _verify(pub_b64, sent["params"]["sig"], canonical.encode()))

        text, err = call("update_task", {
            "task_id": task_id, "details": markdown + "\n\n- re-saved"})
        sent = task_requests[-1]
        digest = hashlib.sha256(json.dumps(
            sent["body"], sort_keys=True, separators=(",", ":"),
            ensure_ascii=False).encode()).hexdigest()
        canonical = "\n".join((
            "forkmesh-org-task-update-v1", "seller", task_id, digest,
            sent["params"]["ts"]))
        check("update_task can re-save details with their newlines",
              not err and sent["method"] == "PATCH" and
              sent["path"] == f"/api/tasks/{task_id}" and
              sent["body"] == {"details": markdown + "\n\n- re-saved"} and
              _verify(pub_b64, sent["params"]["sig"], canonical.encode()))

        text, err = call("complete_task", {
            "task_id": task_id, "completion_note": "Done:\n\n- tests pass"})
        sent = task_requests[-1]
        check("complete_task sends the note with its newlines",
              not err and sent["body"] == {
                  "completionNote": "Done:\n\n- tests pass"})

        text, err = call("create_cloud_note", {
            "title": "Mistral Large 4 — local", "content": markdown})
        sent = task_requests[-1]
        canonical = "\n".join((
            "forkmesh-notes-request-v1", "seller", "POST", "/api/notes",
            hashlib.sha256(sent["raw"]).hexdigest(), sent["params"]["ts"]))
        check("create_cloud_note posts to the Notes API signed over its exact body",
              not err and sent["method"] == "POST" and
              sent["path"] == "/api/notes" and
              sent["body"] == {"title": "Mistral Large 4 — local",
                               "markdown": markdown} and
              sent["params"]["node"] == "seller" and
              json.loads(text)["id"] == "d" * 32 and
              _verify(pub_b64, sent["params"]["sig"], canonical.encode()))

        text, err = call("record_expense", {
            "vendor": "Anthropic", "description": "Anthropic subscription invoice",
            "paid_by": "jett", "amount": 217.75,
            "external_reference": "8RYATVG1-0013", "incurred_on": "2026-10-16"})
        sent = task_requests[-1]
        canonical = "\n".join((
            "forkmesh-ledger-expense-request-v1", "seller", "POST",
            "/api/ledger/expenses", hashlib.sha256(sent["raw"]).hexdigest(),
            sent["params"]["ts"]))
        check("record_expense posts the expense signed over its exact body",
              not err and sent["method"] == "POST" and
              sent["path"] == "/api/ledger/expenses" and
              sent["body"] == {"vendor": "Anthropic",
                               "description": "Anthropic subscription invoice",
                               "paidBy": "jett", "amount": "217.75",
                               "externalReference": "8RYATVG1-0013",
                               "incurredOn": "2026-10-16"} and
              json.loads(text) == {"created": True, "expense": {
                  "id": "anthropic-8ryatvg1-0013", "amountMinor": 21775}} and
              _verify(pub_b64, sent["params"]["sig"], canonical.encode()))

        before = len(task_requests)
        text, err = call("record_expense", {"vendor": "Anthropic"})
        check("record_expense names missing fields before any request",
              err and "description, paid_by, amount" in text and
              len(task_requests) == before)
    finally:
        srv.urllib.request.urlopen = real_task_urlopen

    check_task_trash_tools(call, pub_b64)

    text, err = call("whoami", {})
    me = json.loads(text)
    check("whoami without a connector is open",
          not err and me["access"] == "open" and me["canWrite"] and
          not me["connector"] and me["node"] == pub_b64)
    check("whoami lists reachable repos", me["repos"] == ["repo"])

    connector_path.parent.mkdir(parents=True, exist_ok=True)
    token = "fmcp_" + base64.urlsafe_b64encode(b"k" * 32).decode().rstrip("=")
    connector_path.write_text(json.dumps({
        "version": 1, "token": token, "node": pub_b64,
        "label": "test", "created_ms": 1}))

    text, err = call("whoami", {})
    me = json.loads(text)
    check("whoami without the token is read-only",
          me["access"] == "read" and not me["canWrite"] and me["connector"]
          and not me["tokenPresented"])
    resp = rpc({"jsonrpc": "2.0", "id": 3, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18"}})
    check("initialize announces read-only",
          "read-only" in resp["result"]["instructions"])
    text, err = call("comment_on_issue", {"number": 1, "body": "no token"})
    check("write refused without the token",
          err and "connector token" in text and "Settings -> MCP" in text)
    _, err = call("create_note", {"title": "Blocked", "content": "no token"})
    check("note write refused without the token", err)
    _, err = call("create_cloud_note", {"title": "Blocked", "content": "no token"})
    check("cloud note write refused without the token", err)
    _, err = call("record_expense", {
        "vendor": "Anthropic", "description": "x", "paid_by": "jett",
        "amount": "1", "external_reference": "R-1", "incurred_on": "2026-10-16"})
    check("expense write refused without the token", err)
    _, err = call("update_note", {"note_id": personal_id, "content": "no token"})
    check("note update refused without the token", err)
    _, err = call("post_chat_message", {
        "room": "general", "message": "no token"})
    check("chat post refused without the token before network access", err)
    text, err = call("crm_list_contacts", {"organization": "acme"})
    check("CRM reads refuse without the token before network access",
          err and "connector token" in text)
    check("whoami reports the CRM account and tools",
          me["crmAccount"] == "seller" and
          "crm_update_contact" in me["crmTools"])
    text, err = call("show_window", {"title": "Plan", "body": "no token"})
    check("window tools refuse without the token before the desktop channel",
          err and "connector token" in text and "show_window" in text)
    text, err = call("read_browser_page", {"window_id": "w0123456789abcdef"})
    check("window reads refuse without the token too",
          err and "connector token" in text)
    check("whoami withholds windows without the token",
          me["canOpenWindows"] is False and
          set(me["windowTools"]) == srv.WINDOW_TOOLS)
    _, err = call("search_issues", {"query": ""})
    check("reads still work without the token", not err)
    text, err = call("get_note", {"note_id": personal_id})
    check("note reads still work without the token",
          not err and json.loads(text)["id"] == personal_id)

    os.environ["FORKMESH_MCP_TOKEN"] = token + "x"
    _, err = call("comment_on_issue", {"number": 1, "body": "wrong token"})
    check("write refused with a wrong token", err)

    os.environ["FORKMESH_MCP_TOKEN"] = token
    text, err = call("whoami", {})
    me = json.loads(text)
    check("whoami with the token can write",
          me["access"] == "write" and me["canWrite"] and me["tokenPresented"])
    _, err = call("comment_on_issue", {"number": 1, "body": "with token"})
    check("write allowed with the token", not err)

    connector_path.unlink()
    check("revoked connector falls back to open mode",
          srv.access_level() == "open")
    os.environ.pop("FORKMESH_MCP_TOKEN", None)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S): {FAILURES}")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
