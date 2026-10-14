import base64
import os
import sqlite3
import subprocess
import sys
from pathlib import Path

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "deploy" / "core"))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "app" / "src"))
import deploy_approvals  # noqa: E402
import next_autodeploy  # noqa: E402
import render  # noqa: E402
import self_deploy  # noqa: E402

NOW = 2_000_000_000_000


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


class Phone:
    def __init__(self):
        self.key = Ed25519PrivateKey.generate()
        self.public = b64(self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    def sign(self, message):
        return b64(self.key.sign(message))


class FakeDB:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (REPO / "app" / "migrations" / "0249_deploy_approvals.sql")
            .read_text().split("INSERT INTO schema_meta")[0])

    def query(self, sql, *args):
        return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    def execute(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor.rowcount


def git(cwd, *args):
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1",
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def repo(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    (work / "app").mkdir()
    (work / "app" / "a.py").write_text("one\n")
    git(work, "add", "-A")
    git(work, "commit", "-q", "-m", "base")
    base = git(work, "rev-parse", "HEAD")
    git(work, "checkout", "-q", "-b", "feature")
    (work / "app" / "a.py").write_text("two\n")
    git(work, "commit", "-qam", "change")
    head = git(work, "rev-parse", "HEAD")
    git(work, "checkout", "-q", "main")
    git(work, "merge", "-q", "--no-ff", "-m", "merge", "feature")
    merged = git(work, "rev-parse", "HEAD")
    git(work, "checkout", "-q", "-b", "side", base)
    (work / "app" / "b.py").write_text("x\n")
    git(work, "add", "-A")
    git(work, "commit", "-qm", "side")
    side = git(work, "rev-parse", "HEAD")
    git(work, "checkout", "-q", "main")
    return {"git_dir": str(work / ".git"), "base": base, "head": head,
            "merged": merged, "side": side}


def approval(db, phone, repo, request_id="mobile_0001", kind="pull",
             base_after=None, created=NOW, sign_with=None, expected_head=None):
    row = {
        "request_id": request_id, "kind": kind, "owner": "forkmesh",
        "repo": "forkmesh", "target": "12",
        "expected_base": repo["base"] if kind == "pull" else "",
        "expected_head": (expected_head or repo["head"]) if kind == "pull" else "",
        "signed_at": created,
    }
    message = deploy_approvals.approval_message(
        row["kind"], row["owner"], row["repo"], row["target"],
        row["expected_base"], row["expected_head"], row["request_id"],
        row["signed_at"])
    db.execute(
        "INSERT INTO deploy_approvals (request_id,kind,owner,repo,target,"
        "expected_base,expected_head,base_before,head,base_after,public_key,"
        "signature,signed_at,actor,status,created_at,updated_at) VALUES "
        "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,'pending',?,?)",
        row["request_id"], row["kind"], row["owner"], row["repo"], row["target"],
        row["expected_base"], row["expected_head"], repo["base"], repo["head"],
        base_after or repo["merged"], phone.public,
        (sign_with or phone).sign(message), row["signed_at"], "jett",
        created, created)
    return db.query("SELECT id FROM deploy_approvals WHERE request_id=?",
                    request_id)[0]["id"]


class Steps:
    def __init__(self):
        self.calls = []
        self.fail = set()
        self.current = {}


def agent_for(name, db, repo, phone, steps, clock, state=None, go=None):
    config = {"node": name, "order": ["n1", "n2", "n3"],
              "repository": "forkmesh/forkmesh", "gitDir": repo["git_dir"],
              "approverKeys": [phone.public], "host": name + ".next.forkmesh.com",
              "stateDir": str(state / name) if state else "/nonexistent/state",
              "go": go or "/nonexistent/go"}
    agent = self_deploy.Agent(config, db, now=lambda: clock[0], log=lambda *_: None)
    agent.current_rev = lambda: steps.current.get(name, repo["base"][:12])

    def build(target, rev, release=True, daemon=False):
        steps.calls.append((name, "build", rev, release, daemon))
        if (name, "build") in steps.fail:
            raise self_deploy.Problem("npm ci failed")

    def roll_daemon(target, rev):
        steps.calls.append((name, "daemon", rev))
        if (name, "daemon") in steps.fail:
            raise self_deploy.Problem("daemon: unhealthy after the roll; rolled back")
        agent.set_daemon_rev(target)

    def migrate(rev):
        steps.calls.append((name, "migrate", rev))

    def activate(rev):
        steps.calls.append((name, "activate", rev))
        if (name, "activate") in steps.fail:
            raise self_deploy.Problem("activate failed: gate")
        steps.current[name] = rev

    agent.build_and_upload = build
    agent.roll_daemon = roll_daemon
    agent.migrate = migrate
    agent.activate = activate
    return agent


def status(db, approval_id):
    return db.query("SELECT status,message FROM deploy_approvals WHERE id=?",
                    approval_id)[0]


def test_app_and_core_sign_the_same_bytes():
    row = {"kind": "pull", "owner": "ForkMesh", "repo": "forkmesh", "target": "7",
           "expected_base": "a" * 40, "expected_head": "b" * 40,
           "request_id": "mobile_x1", "signed_at": 5}
    assert self_deploy.approval_message(row) == deploy_approvals.approval_message(
        "pull", "ForkMesh", "forkmesh", "7", "a" * 40, "b" * 40, "mobile_x1", 5)
    assert self_deploy.approval_message(row).startswith(
        b"forkmesh-deploy-approval-v1\npull\nforkmesh\n")


def test_path_rules_match_the_desktop_auto_deploy():
    paths = ["app/src/entry.py", "app/tests/test_x.py", "server/runner.go",
             "server/runner_test.go", "deploy/core/render.py", "deploy/core/x.md",
             "tools/next_deploy.py", "tools/mirror_gateway.py", "www/public/a.html",
             "www/docs/a.md", "desktop/src/main.cpp", "marketplace/catalog.json"]
    for path in paths:
        assert self_deploy.release_path(path) == next_autodeploy.release_path(path), path
        assert self_deploy.roll_path(path) == next_autodeploy.roll_path(path), path


def test_daemon_files_match_the_desktop_roll():
    import next_fleet
    assert self_deploy.DAEMON_TOOLS == next_fleet.DAEMON_TOOLS
    assert "golang-go" in next_fleet.APT_PACKAGES.split()
    assert self_deploy.daemon_path("server/runner.go")
    assert self_deploy.daemon_path("tools/mirror_gateway.py")
    assert not self_deploy.daemon_path("server/runner_test.go")
    assert not self_deploy.daemon_path("server/README.md")
    assert not self_deploy.daemon_path("deploy/core/render.py")


def test_cores_deploy_one_at_a_time_and_the_first_migrates(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    approval_id = approval(db, phone, repo)
    agents = {n: agent_for(n, db, repo, phone, steps, clock) for n in ("n1", "n2", "n3")}
    assert agents["n2"].tick() == "waiting for n1"
    assert agents["n3"].tick() == "waiting for n1"
    assert agents["n1"].tick() == "in progress"
    assert agents["n3"].tick() == "waiting for n2"
    assert agents["n2"].tick() == "in progress"
    assert agents["n3"].tick() == "deployed"
    rev = repo["merged"][:12]
    assert [c for c in steps.calls if c[1] != "build"] == [
        ("n1", "migrate", rev), ("n1", "activate", rev),
        ("n2", "activate", rev), ("n3", "activate", rev)]
    assert status(db, approval_id)["status"] == "deployed"
    assert agents["n1"].tick() == "idle"


def test_the_first_core_rotates_with_the_approval(repo):
    agent = agent_for("n1", FakeDB(), repo, Phone(), Steps(), [NOW])
    firsts = {agent.sequence({"id": i})[0] for i in range(1, 40)}
    assert firsts == {"n1", "n2", "n3"}
    four = dict(agent.config, order=["n1", "n2", "n3", "n4"])
    wider = self_deploy.Agent(four, FakeDB(), now=lambda: NOW, log=lambda *_: None)
    for i in range(1, 40):
        assert [n for n in wider.sequence({"id": i}) if n != "n4"] == \
            agent.sequence({"id": i})


def members(db, clock, fresh, stale=()):
    db.execute("CREATE TABLE IF NOT EXISTS cluster_members (node_id TEXT PRIMARY KEY,"
               " status TEXT, heartbeat_ms INTEGER)")
    for name in fresh:
        db.execute("INSERT OR REPLACE INTO cluster_members VALUES (?,?,?)",
                   name, "active", clock[0])
    for name in stale:
        db.execute("INSERT OR REPLACE INTO cluster_members VALUES (?,?,?)",
                   name, "active", clock[0] - 10 * 60 * 1000)


def test_a_dead_core_neither_blocks_nor_migrates_for_the_others(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    approval_id = approval(db, phone, repo)
    agents = {n: agent_for(n, db, repo, phone, steps, clock) for n in ("n1", "n2", "n3")}
    sequence = agents["n2"].sequence({"id": approval_id})
    dead, first, last = sequence
    members(db, clock, fresh=(first, last), stale=(dead,))
    assert agents[last].tick() == "waiting for %s" % first
    assert agents[first].tick() == "in progress"
    assert agents[last].tick() == "deployed"
    rev = repo["merged"][:12]
    assert [c for c in steps.calls if c[1] != "build"] == [
        (first, "migrate", rev), (first, "activate", rev), (last, "activate", rev)]
    result = status(db, approval_id)
    assert result["status"] == "deployed" and dead in result["message"]


def test_a_failed_core_stops_the_rest(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    steps.fail.add(("n1", "activate"))
    approval_id = approval(db, phone, repo)
    agents = {n: agent_for(n, db, repo, phone, steps, clock) for n in ("n1", "n2", "n3")}
    agents["n2"].tick()
    assert agents["n1"].tick() == "failed"
    result = status(db, approval_id)
    assert result["status"] == "failed" and "n1: activate failed" in result["message"]
    agents["n2"].tick()
    assert ("n2", "activate", repo["merged"][:12]) not in steps.calls


@pytest.mark.parametrize("change, reason", [
    ({"sign_with": "other"}, "not signed by an enrolled phone key"),
    ({"created": NOW - 3 * 3600 * 1000}, "older than two hours"),
    ({"expected_head": "c" * 40}, "does not match what the phone approved"),
    ({"base_after": "side"}, "not on main"),
])
def test_unapproved_merges_never_deploy(repo, change, reason):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    if change.get("sign_with") == "other":
        change["sign_with"] = Phone()
    if change.get("base_after") == "side":
        change["base_after"] = repo["side"]
    approval_id = approval(db, phone, repo, **change)
    agent = agent_for("n1", db, repo, phone, steps, clock)
    assert reason in agent.tick()
    assert status(db, approval_id)["status"] == "skipped"
    assert steps.calls == []


def test_a_missing_commit_waits_then_gives_up(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    approval_id = approval(db, phone, repo, base_after="d" * 40)
    agent = agent_for("n1", db, repo, phone, steps, clock)
    assert agent.tick() == "waiting for the commit"
    clock[0] += self_deploy.WAIT_FOR_COMMIT_MS + 1
    assert "never reached" in agent.tick()
    assert status(db, approval_id)["status"] == "skipped"


def test_a_newer_merge_supersedes_an_older_pending_one(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    older = approval(db, phone, repo, request_id="mobile_0001",
                     base_after=repo["head"], kind="branch")
    newer = approval(db, phone, repo, request_id="mobile_0002")
    agent = agent_for("n1", db, repo, phone, steps, clock)
    agent.tick()
    assert status(db, older)["status"] == "superseded"
    assert status(db, newer)["status"] == "deploying"


def test_no_rollback_and_nothing_served_changed(repo):
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    approval_id = approval(db, phone, repo)
    agents = {n: agent_for(n, db, repo, phone, steps, clock) for n in ("n1", "n2", "n3")}
    for name in agents:
        steps.current[name] = repo["merged"][:12]
    assert agents["n1"].tick() == "in progress"
    for name in ("n2", "n3"):
        agents[name].tick()
    assert status(db, approval_id)["status"] == "deployed"
    assert steps.calls == []


def test_the_ids_window_is_signed_like_next_ids():
    import next_ids
    manifest = self_deploy.ids_manifest("n1", "k" * 64, "abcdefabcdef", NOW)
    body = {k: v for k, v in manifest.items() if k != "mac"}
    again = next_ids.build_manifest(
        "n1", "k" * 64, files=body["files"], rescan=body["rescan"],
        allow=body["allow"], reason=body["reason"], minutes=30, now_ms=NOW)
    assert set(manifest) == set(again)
    for field in ("files", "rescan", "allow", "issued_ms", "expires_ms", "v", "node"):
        assert manifest[field] == again[field]


def test_render_ships_the_agent_only_when_enabled_with_a_key():
    fleet = {"domain": "next.forkmesh.com", "nodes": [{"name": "n1"}, {"name": "n2"}]}
    added, units, creds = {}, [], {}

    def add(path, content, mode=0o644):
        added[path] = (content, mode)
    paths = {"opt": "/opt/forkmesh-next", "etc": "/etc/forkmesh-next",
             "creds": "/etc/forkmesh-next/creds"}
    render.add_self_deploy_files(fleet, "n1", paths, {"NODE": "n1"}, add, units, creds)
    assert added == {} and units == []
    fleet["self_deploy"] = {"enabled": True, "approver_keys": ["x"]}
    with pytest.raises(render.RenderError):
        render.self_deploy_settings(fleet)
    key = Phone().public
    fleet["self_deploy"] = {"enabled": True, "approver_keys": [key]}
    values = {"NODE": "n1", "OPT": "/opt/forkmesh-next", "ETC": "/etc/forkmesh-next"}
    render.add_self_deploy_files(fleet, "n1", paths, values, add, units, creds)
    config = __import__("json").loads(added["/etc/forkmesh-next/self-deploy.json"][0])
    assert config["approverKeys"] == [key] and config["order"] == ["n1", "n2"]
    assert config["node"] == "n1" and config["host"] == "n1.next.forkmesh.com"
    assert units == ["fmnext-self-deploy.service"]
    assert added["/opt/forkmesh-next/bin/fmnext-self-deploy"][1] == 0o755
    assert "/opt/forkmesh-next/bin/forkmesh_safe_delete.py" in added


def merge_on_main(repo, files):
    work = Path(repo["git_dir"]).parent
    git(work, "checkout", "-q", "-b", "roll-" + str(len(files)), "main")
    for path, text in files.items():
        (work / path).parent.mkdir(parents=True, exist_ok=True)
        (work / path).write_text(text)
    git(work, "add", "-A")
    git(work, "commit", "-qm", "roll change")
    head = git(work, "rev-parse", "HEAD")
    git(work, "checkout", "-q", "main")
    before = git(work, "rev-parse", "HEAD")
    git(work, "merge", "-q", "--no-ff", "-m", "merge roll", head)
    return dict(repo, base=before, head=head, merged=git(work, "rev-parse", "HEAD"))


def go_binary(tmp_path):
    go = tmp_path / "go"
    go.write_text("")
    return str(go)


def test_a_daemon_change_rolls_each_core_in_turn(repo, tmp_path):
    rolled = merge_on_main(repo, {"server/runner.go": "package main\n"})
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    for name in ("n1", "n2", "n3"):
        steps.current[name] = rolled["base"][:12]
    approval_id = approval(db, phone, rolled, kind="branch")
    agents = {n: agent_for(n, db, rolled, phone, steps, clock, state=tmp_path,
                           go=go_binary(tmp_path)) for n in ("n1", "n2", "n3")}
    assert agents["n2"].tick() == "waiting for n1"
    assert agents["n1"].tick() == "in progress"
    assert agents["n3"].tick() == "waiting for n2"
    assert agents["n2"].tick() == "in progress"
    assert agents["n3"].tick() == "deployed"
    rev = rolled["merged"][:12]
    assert [c for c in steps.calls if c[1] == "build"] == [
        (n, "build", rev, False, True) for n in ("n2", "n1", "n3")]
    assert [c for c in steps.calls if c[1] != "build"] == [
        ("n1", "daemon", rev), ("n2", "daemon", rev), ("n3", "daemon", rev)]
    result = status(db, approval_id)
    assert result["status"] == "deployed"
    assert self_deploy.RUNNER_NOTE in result["message"]
    assert (tmp_path / "n1" / "daemon-rev").read_text().strip() == rolled["merged"]


def test_an_app_and_daemon_change_activates_then_rolls(repo, tmp_path):
    rolled = merge_on_main(repo, {"server/a.go": "package main\n",
                                  "app/c.py": "x\n"})
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    for name in ("n1", "n2", "n3"):
        steps.current[name] = rolled["base"][:12]
    approval(db, phone, rolled, kind="branch")
    agent = agent_for("n1", db, rolled, phone, steps, clock, state=tmp_path,
                      go=go_binary(tmp_path))
    agent.tick()
    rev = rolled["merged"][:12]
    assert steps.calls == [("n1", "build", rev, True, True), ("n1", "migrate", rev),
                           ("n1", "activate", rev), ("n1", "daemon", rev)]


def test_a_failed_daemon_roll_stops_the_rest(repo, tmp_path):
    rolled = merge_on_main(repo, {"tools/mirror_gateway.py": "x = 1\n"})
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    for name in ("n1", "n2", "n3"):
        steps.current[name] = rolled["base"][:12]
    steps.fail.add(("n1", "daemon"))
    approval_id = approval(db, phone, rolled, kind="branch")
    agents = {n: agent_for(n, db, rolled, phone, steps, clock, state=tmp_path,
                           go=go_binary(tmp_path)) for n in ("n1", "n2")}
    assert agents["n1"].tick() == "failed"
    assert "rolled back" in status(db, approval_id)["message"]
    agents["n2"].tick()
    assert ("n2", "daemon", rolled["merged"][:12]) not in steps.calls


@pytest.mark.parametrize("files, note", [
    ({"server/runner.go": "package main\n"}, self_deploy.NO_GO_NOTE),
    ({"deploy/core/templates/x.service": "[Unit]\n"}, self_deploy.DESKTOP_ROLL_NOTE),
])
def test_what_a_core_cannot_roll_is_left_to_the_desktop(repo, tmp_path, files, note):
    rolled = merge_on_main(repo, files)
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    steps.current["n1"] = rolled["base"][:12]
    approval(db, phone, rolled, kind="branch")
    agent = agent_for("n1", db, rolled, phone, steps, clock, state=tmp_path)
    agent.tick()
    rows = db.query("SELECT status,message FROM deploy_approval_nodes")
    assert note in rows[0]["message"]
    assert not [c for c in steps.calls if c[1] == "daemon"]


def test_the_daemon_baseline_is_the_last_self_roll(repo, tmp_path):
    rolled = merge_on_main(repo, {"server/runner.go": "package main\n"})
    db, phone, steps, clock = FakeDB(), Phone(), Steps(), [NOW + 1000]
    steps.current["n1"] = rolled["base"][:12]
    agent = agent_for("n1", db, rolled, phone, steps, clock, state=tmp_path,
                      go=go_binary(tmp_path))
    agent.set_daemon_rev(rolled["merged"])
    assert agent.plan(rolled["merged"]) == (False, False, "")
    (tmp_path / "n1" / "daemon-rev").unlink()
    assert agent.plan(rolled["merged"]) == (False, True, "")
    assert (tmp_path / "n1" / "daemon-rev").read_text().strip() == rolled["base"]


class FakeSystem:
    def __init__(self, check=0, states=None):
        self.calls = []
        self.check = check
        self.states = list(states or [])

    def __call__(self, command, **kwargs):
        self.calls.append(command)
        stdout = ""
        code = 0
        if "--check" in command:
            code = self.check
        if command[:2] == ["systemctl", "show"]:
            state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
            stdout = "ActiveState=%s\nNRestarts=%s\n" % state
        return subprocess.CompletedProcess(command, code, stdout, "")


@pytest.fixture
def live(tmp_path, monkeypatch):
    root = tmp_path / "live"
    (root / "tools").mkdir(parents=True)
    (root / "bin").write_text("old binary")
    (root / "tools" / "mirror_gateway.py").write_text("old gateway")
    (root / "mirror-node.json").write_text('{"version": "1.0.0", "node": "n1"}')
    monkeypatch.setattr(self_deploy, "DAEMON_BIN", str(root / "bin"))
    monkeypatch.setattr(self_deploy, "DAEMON_TOOLS_DIR", str(root / "tools"))
    monkeypatch.setattr(self_deploy, "DAEMON_CONFIG", str(root / "mirror-node.json"))
    monkeypatch.setattr(self_deploy, "ACTIVATION_LOCK", str(tmp_path / "lock"))
    monkeypatch.setattr(self_deploy, "IDS", str(tmp_path / "no-ids"))
    stage = tmp_path / "state" / "builds" / "abcdefabcdef" / "daemon"
    (stage / "tools").mkdir(parents=True)
    (stage / "forkmesh-mirror-node").write_text("new binary")
    (stage / "tools" / "mirror_gateway.py").write_text("new gateway")
    (stage / "tools" / "secret_gate.py").write_text("new tool")
    (stage / "VERSION").write_text("2.0.0")
    return root


def roller(tmp_path, system, clock):
    config = {"node": "n1", "order": ["n1"], "repository": "forkmesh/forkmesh",
              "gitDir": "/nonexistent", "approverKeys": [],
              "host": "n1.next.forkmesh.com", "stateDir": str(tmp_path / "state")}
    agent = self_deploy.Agent(config, None, run=system, now=lambda: clock[0])

    def sleep(seconds):
        clock[0] += int(seconds * 1000)
    agent.sleep = sleep
    return agent


def test_a_healthy_daemon_roll_swaps_binary_tools_and_version(tmp_path, live):
    clock = [NOW]
    system = FakeSystem(states=[("active", "0")])
    agent = roller(tmp_path, system, clock)
    agent.roll_daemon("a" * 40, "abcdefabcdef")
    assert (live / "bin").read_text() == "new binary"
    assert (live / "tools" / "mirror_gateway.py").read_text() == "new gateway"
    assert (live / "tools" / "secret_gate.py").read_text() == "new tool"
    assert __import__("json").loads((live / "mirror-node.json").read_text()) == {
        "version": "2.0.0", "node": "n1"}
    assert ["systemctl", "restart", "forkmesh-mirror-node.service"] in system.calls
    assert agent.daemon_rev() == "a" * 40


def test_a_crash_looping_daemon_is_rolled_back(tmp_path, live):
    clock = [NOW]
    system = FakeSystem(states=[("activating", str(n)) for n in range(100)])
    agent = roller(tmp_path, system, clock)
    with pytest.raises(self_deploy.Problem, match="rolled back"):
        agent.roll_daemon("a" * 40, "abcdefabcdef")
    assert (live / "bin").read_text() == "old binary"
    assert (live / "tools" / "mirror_gateway.py").read_text() == "old gateway"
    assert not (live / "tools" / "secret_gate.py").exists()
    assert '"1.0.0"' in (live / "mirror-node.json").read_text()
    assert agent.daemon_rev() == ""


def test_a_binary_that_refuses_the_config_changes_nothing(tmp_path, live):
    clock = [NOW]
    system = FakeSystem(check=1, states=[("active", "0")])
    agent = roller(tmp_path, system, clock)
    with pytest.raises(self_deploy.Problem, match="refused this core's config"):
        agent.roll_daemon("a" * 40, "abcdefabcdef")
    assert (live / "bin").read_text() == "old binary"
    assert sorted(p.name for p in live.iterdir()) == ["bin", "mirror-node.json", "tools"]
    assert not any(c[:2] == ["systemctl", "restart"] for c in system.calls)
