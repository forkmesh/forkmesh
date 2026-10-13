from types import SimpleNamespace
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

import next_autodeploy as auto
import next_common as common
import next_deploy
from next_common import GuardError

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = (REPO / ".forkmesh" / "deploy-nodes.yml").read_text("utf-8")
FLEET_TOML = """domain = "next.example.com"
app_host = "app.next.example.com"
world_host = "world.next.example.com"

[production]
app_host = "app.example.com"
world_host = "world.example.com"

[[nodes]]
name = "n1"
role = "core"
public_ipv4 = "203.0.113.11"

[[nodes]]
name = "n2"
role = "core"
public_ipv4 = "203.0.113.12"

[[nodes]]
name = "r1"
role = "runner"
public_ipv4 = "203.0.113.21"
"""


def git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True,
                          text=True).stdout.strip()


def commit(repo, files, message):
    for name, text in files.items():
        path = repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", message)
    return git(repo, "rev-parse", "HEAD")


@pytest.fixture
def repo(tmp_path):
    path = tmp_path / "src"
    path.mkdir()
    git(path, "init", "-q", "-b", "main")
    git(path, "config", "user.email", "t@example.com")
    git(path, "config", "user.name", "t")
    commit(path, {"app/a.py": "a\n", "world/w.js": "w\n", "www/public/x.css": "x\n",
                  "deploy/fleet.toml": FLEET_TOML, "docs/readme.md": "d\n"}, "base")
    return path


def test_trigger_filter_selects_served_stack_paths_only():
    changed = auto.classify([
        "app/src/entry.py", "app/host/forkmesh_host/serve.py", "world/public/x.js",
        "www/public/styles.css", "deploy/fleet.toml", "deploy/core/render.py",
        "tools/next_deploy.py", "tools/rqlite_migrate.py", "server/main.go",
        "tools/mirror_gateway.py", "marketplace/packages/p/manifest.json",
        "app/tests/test_x.py", "deploy/tests/test_y.py", "app/host/tests_cluster/t.py",
        "app/browser_tests/tests/a.spec.js", "server/main_test.go",
        "www/docs/guide.md", "docs/deploy/README.md", "desktop/src/MainWindow.cpp",
        "native/ios/ForkMesh/App/ForkMeshApp.swift", "tools/source_publisher.py",
        ".forkmesh/ci.yml"])
    assert changed["release"] == [
        "app/host/forkmesh_host/serve.py", "app/src/entry.py", "deploy/core/render.py",
        "deploy/fleet.toml", "marketplace/packages/p/manifest.json",
        "tools/next_deploy.py", "tools/rqlite_migrate.py", "world/public/x.js",
        "www/public/styles.css"]
    assert changed["roll"] == ["deploy/core/render.py", "server/main.go",
                               "tools/mirror_gateway.py"]
    assert auto.classify(["www/docs/a.md", "docs/x.md", "README.md"]) == {
        "release": [], "roll": []}


def _qt_wildcard(pattern):
    return re.compile("^" + "".join("[^/]*" if c == "*" else re.escape(c)
                                    for c in pattern) + "$")


def test_workflow_prefilter_is_generated_and_covers_every_served_file():
    listed = re.findall(r'^      - "([^"]+)"$', WORKFLOW.split("  workflow_dispatch:")[0],
                        re.M)
    assert listed == auto.workflow_path_patterns()
    patterns = [_qt_wildcard(p) for p in listed]
    listing = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True,
                             text=True, check=False)
    if listing.returncode == 0:
        tracked = listing.stdout.split()
    else:
        tracked = [p.relative_to(REPO).as_posix() for p in REPO.rglob("*")
                   if p.is_file()]
    served = [p for p in tracked if auto.release_path(p) or auto.roll_path(p)]
    assert served
    assert [p for p in served if not any(x.match(p) for x in patterns)] == []
    assert not any(x.match("www/docs/guide.md") for x in patterns)
    assert not any(x.match("desktop/src/main.cpp") for x in patterns)


def test_workflow_runs_only_here_and_never_deploys_workers():
    assert WORKFLOW.startswith("name: Deploy nodes\non:\n  push:\n    paths:\n")
    assert "\n  workflow_dispatch:\nruns-on: forkmesh\n" in WORKFLOW
    assert "/workspace/" + auto.SINK_NAME in WORKFLOW
    assert auto.RESULT_MARK in WORKFLOW
    commands = "\n".join(line for line in WORKFLOW.splitlines()
                          if not line.strip().startswith('- "'))
    for word in ("wrangler", "deploy.sh", "vars.", "secrets."):
        assert word not in commands
    deploy = (REPO / ".forkmesh" / "deploy.yml").read_text("utf-8")
    assert deploy.split("\n", 3)[1] == "on: [workflow_dispatch]"
    assert ".forkmesh/deploy-nodes.yml" in deploy


def test_debounce_waits_for_main_to_settle_and_caps_the_wait():
    ready = auto.ready_to_start
    assert not ready(100, None, None, 0)
    assert not ready(100, 90, 95, 80, debounce=60)
    assert not ready(200, 90, 195, 100, debounce=60, settle=10)
    assert ready(200, 90, 150, 100, debounce=60, settle=10)
    assert ready(700, 90, 699, 699, debounce=60, max_wait=600)


def test_fast_forward_gate_accepts_descendants_and_refuses_rollbacks(repo):
    g = auto.Git(repo)
    first = git(repo, "rev-parse", "HEAD")
    second = commit(repo, {"app/a.py": "a2\n"}, "second")
    third = commit(repo, {"docs/readme.md": "d2\n"}, "docs")
    live = {"n1": first[:12], "n2": first[:12]}
    gate = auto.fast_forward_gate(live, {}, g, third, third)
    assert gate["base"] == first
    assert auto.classify(g.changed_paths(gate["base"], third))["release"] == ["app/a.py"]
    same = auto.fast_forward_gate({"n1": third[:12]}, {}, g, second, third)
    assert same["base"] == second
    with pytest.raises(GuardError, match="roll production back"):
        auto.fast_forward_gate({"n1": second[:12]}, {}, g, first, third)
    gate = auto.fast_forward_gate({"n1": second[:12]}, {}, g, first, third, mode="rollback")
    assert gate["notes"] and "manual rollback" in gate["notes"][0]
    git(repo, "checkout", "-q", "-b", "side", first)
    side = commit(repo, {"app/a.py": "side\n"}, "side")
    git(repo, "checkout", "-q", "main")
    with pytest.raises(GuardError, match="not on main"):
        auto.fast_forward_gate(live, {}, g, side, third)


def test_fast_forward_gate_matches_the_deployed_rev_by_content(repo):
    g = auto.Git(repo)
    base = git(repo, "rev-parse", "HEAD")
    git(repo, "checkout", "-q", "--orphan", "other-repo")
    other = commit(repo, {"app/a.py": "a\n", "extra/only-there.txt": "x\n"}, "replayed")
    git(repo, "checkout", "-q", "main")
    assert g.signature(other) == g.signature(base)
    tip = commit(repo, {"world/w.js": "w2\n"}, "tip")
    gate = auto.fast_forward_gate({"n1": other[:12]}, {}, g, tip, tip)
    assert gate["base"] == base
    manifest = {"releases": {"feedfacecafe": {"commit": "", "signature": g.signature(base)}}}
    gate = auto.fast_forward_gate({"n1": "feedfacecafe"}, manifest, g, tip, tip)
    assert gate["base"] == base
    with pytest.raises(GuardError, match="matches no commit on main by content"):
        auto.fast_forward_gate({"n1": "0badc0de0bad"}, {}, g, tip, tip)
    gate = auto.fast_forward_gate({"n1": "0badc0de0bad"}, {}, g, tip, tip, mode="manual")
    assert gate["base"] == "" and "unknown" in gate["notes"][0]


def test_deploy_lock_is_exclusive_and_names_its_holder(sandbox):
    first = common.DeployLock(what="next_deploy restart").acquire()
    try:
        with pytest.raises(common.LockBusy) as busy:
            common.DeployLock(what="next_autodeploy auto").acquire(wait=0)
        assert busy.value.holder["what"] == "next_deploy restart"
        assert busy.value.holder["pid"] == os.getpid()
        assert "next_deploy restart" in str(busy.value)
    finally:
        first.release()
    again = common.DeployLock().acquire(wait=0)
    again.release()


def test_a_child_given_the_lock_fd_counts_as_holding_it(sandbox, monkeypatch):
    held = common.DeployLock(what="next_autodeploy auto").acquire()
    try:
        monkeypatch.setenv(common.LOCK_FD_ENV, str(held.fd))
        child = common.DeployLock().acquire(wait=0)
        assert child.inherited and child.fd is None
        child.release()
        monkeypatch.setenv(common.LOCK_FD_ENV, "0")
        with pytest.raises(common.LockBusy):
            common.DeployLock().acquire(wait=0)
    finally:
        held.release()


def test_manual_next_deploy_refuses_while_another_deploy_holds_the_lock(sandbox, capsys):
    held = common.DeployLock(what="next_autodeploy auto 0123456789ab").acquire()
    try:
        code = next_deploy.main(["migrate", "--execute"])
    finally:
        held.release()
    assert code == 1
    assert "next_autodeploy auto 0123456789ab" in capsys.readouterr().err


DOC_INSTANCES = {"fmnext-core-1": {"ip": "203.0.113.11", "ipv6": "2001:19f0:ffff::11"},
                 "fmnext-core-2": {"ip": "203.0.113.12", "ipv6": "2001:19f0:ffff::12"},
                 "fmnext-core-3": {"ip": "203.0.113.13", "ipv6": "2001:19f0:ffff::13"},
                 "fmnext-run-1": {"ip": "203.0.113.21"}}


def test_next_deploy_dry_run_takes_no_lock(sandbox):
    common.save_json(sandbox / "fleet-instances.json", DOC_INSTANCES)
    held = common.DeployLock(what="other").acquire()
    try:
        assert next_deploy.main(["build"], run=lambda *a, **k: None) in (0, 1)
    finally:
        held.release()


def test_record_release_writes_the_manifest(repo, sandbox):
    head = git(repo, "rev-parse", "HEAD")
    entry = next_deploy.record_release(head[:12], repo=repo)
    assert entry["commit"] == head
    assert entry["signature"] == common.served_signature(head, repo)
    manifest = common.load_manifest()
    assert manifest["current"] == head[:12]
    assert manifest["releases"][head[:12]]["by"] == "next_deploy"


def test_kill_switch_defaults_off_and_pause_beats_armed(tmp_path):
    directory = tmp_path / "autodeploy"
    assert auto.switch_state(directory)[0] == "disarmed"
    auto.resume(directory)
    assert auto.switch_state(directory) == ("armed", "")
    auto.pause("incident 42", directory)
    state, reason = auto.switch_state(directory)
    assert state == "paused" and reason.startswith("incident 42")
    assert oct((directory / "paused").stat().st_mode & 0o777) == "0o600"
    auto.resume(directory)
    assert auto.switch_state(directory)[0] == "armed"
    auto.disarm(directory)
    assert auto.switch_state(directory)[0] == "disarmed"


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


def make_run(root, run_id, commit, ref="refs/heads/main", owner="forkmesh",
             name="forkmesh", workflow=auto.WORKFLOW_PATH, pull=0):
    workspace = root / "sandbox" / ("forkmesh-run-%d-1790000000000" % run_id) / "workspace"
    workspace.mkdir(parents=True)
    meta_dir = root / "runs" / "v2-key" / str(run_id)
    meta_dir.mkdir(parents=True)
    meta = {"id": run_id, "owner": owner, "name": name, "ref": ref, "commit": commit,
            "workflowPath": workflow, "status": "running"}
    if pull:
        meta["pullNumber"] = pull
    (meta_dir / "meta.json").write_text(json.dumps(meta))
    return workspace


def sink_text(workspace):
    return (workspace / auto.SINK_NAME).read_text()


def result_line(workspace):
    lines = [l for l in sink_text(workspace).splitlines() if l.startswith(auto.RESULT_MARK)]
    return lines[-1] if lines else ""


def agent_for(repo, tmp_path, clock, calls, armed=True):
    directory = tmp_path / "switch"
    if armed:
        auto.resume(directory)

    def pipeline(repo_path, target, mode, log, notifier=None, state_dir=None):
        calls.append((target, mode))
        log.say("pipeline ran for %s" % target[:12])
        return {"ok": True, "target": target, "rev": target[:12], "deployed": True,
                "roll": [], "message": "deployed %s" % target[:12]}
    return auto.Agent(repo, tmp_path / "actions", clock=clock, state_dir=directory,
                      pipeline=pipeline, logs=tmp_path / "logs")


def settle(agent, clock, seconds=auto.DEBOUNCE_SECONDS + 1):
    clock.now += seconds
    agent.tick()
    if agent.worker:
        agent.worker.join(5)
    agent.tick()


def test_a_burst_of_pushes_becomes_one_deploy_of_the_latest_commit(repo, tmp_path):
    clock, calls = Clock(), []
    agent = agent_for(repo, tmp_path, clock, calls)
    root = tmp_path / "actions"
    commits, workspaces = [], []
    for index in range(3):
        commits.append(commit(repo, {"app/a.py": "v%d\n" % index}, "push %d" % index))
        workspaces.append(make_run(root, 10 + index, commits[-1]))
        clock.now += 5
        agent.tick()
    assert calls == []
    settle(agent, clock)
    assert calls == [(commits[-1], "auto")]
    for workspace, pushed in zip(workspaces, commits):
        line = result_line(workspace)
        assert line.startswith(auto.RESULT_MARK + " ok deployed " + commits[-1][:12])
        if pushed != commits[-1]:
            assert "covers %s" % pushed[:12] in line
        assert "pipeline ran for %s" % commits[-1][:12] in sink_text(workspace)
    assert agent.pending == {}


def test_other_branches_forks_and_pull_requests_never_deploy(repo, tmp_path):
    clock, calls = Clock(), []
    agent = agent_for(repo, tmp_path, clock, calls)
    root = tmp_path / "actions"
    head = git(repo, "rev-parse", "HEAD")
    branch = make_run(root, 1, head, ref="refs/heads/feature")
    fork = make_run(root, 2, head, owner="someone")
    pull = make_run(root, 3, head, pull=7)
    other = make_run(root, 4, head, workflow=".forkmesh/ci.yml")
    git(repo, "checkout", "-q", "-b", "side")
    side = commit(repo, {"app/a.py": "side\n"}, "side")
    git(repo, "checkout", "-q", "main")
    stray = make_run(root, 5, side)
    agent.tick()
    settle(agent, clock)
    assert calls == []
    assert result_line(branch).startswith(auto.RESULT_MARK + " ok ")
    assert "not main" in result_line(branch)
    assert result_line(fork).startswith(auto.RESULT_MARK + " fail ")
    assert "pull request" in result_line(pull)
    assert not (other / auto.SINK_NAME).exists()
    assert "is not on main" in result_line(stray)


def test_a_run_seen_before_its_workspace_is_cloned_is_attached_later(repo, tmp_path):
    clock, calls = Clock(), []
    agent = agent_for(repo, tmp_path, clock, calls)
    head = commit(repo, {"app/a.py": "late\n"}, "late")
    workspace = make_run(tmp_path / "actions", 1, head)
    workspace.rmdir()
    agent.tick()
    assert 1 not in agent.attached
    workspace.mkdir()
    clock.now += 2
    agent.tick()
    assert agent.attached[1] is not None
    settle(agent, clock)
    assert calls == [(head, "auto")]
    assert result_line(workspace).startswith(auto.RESULT_MARK + " ok deployed ")


def test_a_run_requeued_after_a_desktop_restart_follows_its_new_workspace(repo, tmp_path):
    clock, calls = Clock(), []
    agent = agent_for(repo, tmp_path, clock, calls)
    head = commit(repo, {"app/a.py": "requeued\n"}, "requeued")
    first = make_run(tmp_path / "actions", 603, head)
    agent.tick()
    assert agent.attached[603] is not None
    second = tmp_path / "actions" / "sandbox" / "forkmesh-run-603-1790000099999" / "workspace"
    second.mkdir(parents=True)
    clock.now += 2
    agent.tick()
    assert agent.attached[603] is not None
    settle(agent, clock)
    assert calls == [(head, "auto")]
    assert (second / auto.SINK_NAME).exists()
    assert result_line(second).startswith(auto.RESULT_MARK + " ok deployed ")
    assert result_line(first) == ""
    assert [run["workspace"] for run in auto.running_runs(tmp_path / "actions")] == [second]


def test_paused_or_disarmed_agent_answers_without_deploying(repo, tmp_path):
    clock, calls = Clock(), []
    agent = agent_for(repo, tmp_path, clock, calls, armed=False)
    workspace = make_run(tmp_path / "actions", 1, commit(repo, {"app/a.py": "p\n"}, "p"))
    agent.tick()
    assert calls == []
    assert "auto-deploy is disarmed" in result_line(workspace)
    assert result_line(workspace).startswith(auto.RESULT_MARK + " ok ")
    auto.resume(tmp_path / "switch")
    auto.pause("hold", tmp_path / "switch")
    second = make_run(tmp_path / "actions", 2, commit(repo, {"app/a.py": "q\n"}, "q"))
    settle(agent, clock)
    assert calls == []
    assert "auto-deploy is paused (hold" in result_line(second)


def test_a_push_during_a_deploy_waits_for_the_next_one(repo, tmp_path):
    clock, calls = Clock(), []
    gate = __import__("threading").Event()
    agent = agent_for(repo, tmp_path, clock, calls)
    inner = agent.pipeline

    def slow(*args, **kwargs):
        gate.wait(5)
        return inner(*args, **kwargs)
    agent.pipeline = slow
    root = tmp_path / "actions"
    first = commit(repo, {"app/a.py": "1\n"}, "1")
    make_run(root, 1, first)
    agent.tick()
    clock.now += auto.DEBOUNCE_SECONDS + 1
    agent.tick()
    assert agent.worker is not None
    second = commit(repo, {"app/a.py": "2\n"}, "2")
    late = make_run(root, 2, second)
    agent.tick()
    assert "in progress" in sink_text(late)
    gate.set()
    agent.worker.join(5)
    agent.tick()
    assert result_line(late) == ""
    assert list(agent.pending.values()) == [second]
    settle(agent, clock)
    assert calls == [(first, "auto"), (second, "auto")]
    assert "deployed %s" % second[:12] in result_line(late)


def test_run_sink_never_follows_a_planted_file_or_link(tmp_path):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    target = tmp_path / "victim"
    target.write_text("keep\n")
    (workspace / auto.SINK_NAME).symlink_to(target)
    with pytest.raises(OSError):
        auto.RunSink(1, workspace)
    assert target.read_text() == "keep\n"
    link = tmp_path / "linked-workspace"
    link.symlink_to(workspace)
    with pytest.raises(OSError):
        auto.RunSink(2, link)
    (workspace / auto.SINK_NAME).unlink()
    sink = auto.RunSink(3, workspace)
    sink.write("line\n")
    sink.finish(False, "tests\nbroke")
    assert sink_text(workspace) == "line\n%s fail tests broke\n" % auto.RESULT_MARK


class FakeNotifier:
    def __init__(self):
        self.sent = []

    def send(self, title, body, key, email=False):
        self.sent.append((title, key, email))
        return True


def test_failures_ping_and_only_a_repeat_emails(tmp_path):
    notifier = FakeNotifier()
    failed = {"ok": False, "rev": "0123456789ab", "message": "tests broke", "roll": []}
    for _ in range(3):
        auto.record_outcome(failed, notifier, print, state_dir=tmp_path)
    assert [email for _, _, email in notifier.sent] == [False, True, False]
    assert notifier.sent[0][1] == "deploy-failed:0123456789ab"
    auto.record_outcome({"ok": True, "rev": "0123456789ab", "deployed": True,
                         "roll": ["server/main.go"], "target": "0" * 40},
                        notifier, print, state_dir=tmp_path)
    assert notifier.sent[-1][1] == "node-roll:0123456789ab"
    state = auto.load_state(tmp_path)
    assert state["failures"] == 0 and state["roll"]["paths"] == ["server/main.go"]
    auto.record_outcome(failed, notifier, print, state_dir=tmp_path)
    assert notifier.sent[-1][2] is False


def test_fleet_hosts_uses_production_names_and_core_nodes_only():
    hosts = auto.fleet_hosts(FLEET_TOML)
    assert hosts["app_host"] == "app.example.com"
    assert hosts["world_host"] == "world.example.com"
    assert [n["name"] for n in hosts["nodes"]] == ["n1", "n2"]


def test_fleet_hosts_reads_missing_addresses_from_the_instances_file(sandbox):
    text = FLEET_TOML.replace('public_ipv4 = "203.0.113.11"',
                              'label = "fmnext-core-1"', 1)
    hosts = auto.fleet_hosts(text, DOC_INSTANCES)
    assert hosts["nodes"][0] == {"name": "n1", "ip": "203.0.113.11"}
    with pytest.raises(ValueError, match="fleet-instances.json"):
        auto.fleet_hosts(text)
    common.save_json(sandbox / "fleet-instances.json", DOC_INSTANCES)
    assert auto.fleet_hosts(text)["nodes"] == hosts["nodes"]


def test_verify_checks_every_node_rev_and_pages():
    hosts = auto.fleet_hosts(FLEET_TOML)
    seen = []

    def run(command, **kwargs):
        host_path = command[-1]
        seen.append((command[command.index("--resolve") + 1], host_path))
        body = ""
        if host_path.endswith("/api/version"):
            body = json.dumps({"rev": "0123456789ab"})
        if host_path == "https://world.example.com/":
            body = '<meta name="forkmesh-world-deploy" content="0123456789ab">'
        return subprocess.CompletedProcess(command, 0, body + "\n200", "")
    said = []
    assert auto.verify(hosts, "0123456789ab", said.append, run=run, timeout=0)
    assert ("app.example.com:443:203.0.113.12", "https://app.example.com/readyz") in seen
    assert ("n2.next.example.com:443:203.0.113.12",
            "https://n2.next.example.com/readyz") in seen
    said.clear()
    assert not auto.verify(hosts, "ffffffffffff", said.append, run=run, timeout=0)
    assert any("want ffffffffffff" in line for line in said)


def test_a_probe_retries_a_dropped_connection_but_not_an_answer():
    codes = ["000", "000", "503"]
    slept = []

    def run(command, **kwargs):
        if not codes:
            raise AssertionError("an HTTP answer is not retried")
        return subprocess.CompletedProcess(command, 0, "busy\n" + codes.pop(0), "")
    assert auto.curl("app.example.com", "203.0.113.11", "/readyz", run=run,
                     sleep=slept.append) == (503, "busy")
    assert len(slept) == 2

    def down(command, **kwargs):
        raise subprocess.TimeoutExpired(command, 15)
    assert auto.curl("app.example.com", "203.0.113.11", "/readyz", run=down,
                     attempts=3, sleep=slept.append) == (0, "")


def test_ff_refusal_stops_before_tests_build_or_deploy(repo, tmp_path, sandbox):
    first = git(repo, "rev-parse", "HEAD")
    newer = commit(repo, {"app/a.py": "new\n"}, "new")
    git(repo, "reset", "-q", "--hard", first)
    git(repo, "update-ref", "refs/heads/keep", newer)
    target = commit(repo, {"world/w.js": "w9\n"}, "diverged")
    launched = []

    def run(command, **kwargs):
        if command[0] == "curl":
            body = json.dumps({"rev": newer[:12]})
            return subprocess.CompletedProcess(command, 0, body + "\n200", "")
        return subprocess.run(command, **kwargs)

    def popen(command, **kwargs):
        launched.append(command)
        raise AssertionError("nothing may be launched")
    log = auto.Log(echo=False)
    result = auto.run_pipeline(repo, target, "auto", log, run=run, popen=popen,
                               state_dir=tmp_path / "state",
                               layout=__import__("forkmesh_paths").resolve())
    assert not result["ok"]
    assert "not an ancestor" in result["message"]
    assert launched == []
    assert common.DeployLock().acquire(wait=0).release() is None


def test_deploy_commit_resolves_main_to_the_checkout_branch(repo):
    head = git(repo, "rev-parse", "HEAD")
    commit(repo, {"app/a.py": "b\n"}, "side")
    git(repo, "branch", "-f", "side")
    git(repo, "reset", "-q", "--hard", head)
    assert auto.deploy_commit(auto.Git(repo), "main") == head
    assert auto.deploy_commit(auto.Git(repo), head[:12]) == head
    git(repo, "checkout", "-q", "side")
    git(repo, "branch", "-D", "main")
    with pytest.raises(GuardError, match="no main branch"):
        auto.deploy_commit(auto.Git(repo), "main")
    with pytest.raises(GuardError, match="not a commit id"):
        auto.deploy_commit(auto.Git(repo), "HEAD")


def test_website_step_compares_the_www_tree_with_the_live_header():
    class FakeGit:
        def __init__(self, trees):
            self.trees = trees

        def call(self, *args, check=False):
            assert args[0] == "rev-parse"
            return SimpleNamespace(stdout=self.trees[args[1]] + "\n", returncode=0)
    base = {"abc123:www": "tree0001", "abc123:marketplace": "tree0101"}
    wanted = auto.website_fingerprint(FakeGit(base), "abc123")
    assert len(wanted) == 64 and wanted != auto.website_fingerprint(
        FakeGit({**base, "abc123:www": "tree0002"}), "abc123")
    assert wanted != auto.website_fingerprint(
        FakeGit({**base, "abc123:marketplace": "tree0102"}), "abc123"), (
        "a package page such as /design redeploys forkmesh.com")

    def fake_curl(argv, **_kwargs):
        assert argv[-1] == auto.WEBSITE_URL
        return SimpleNamespace(stdout="HTTP/2 200\r\nX-ForkMesh-Deploy-Fingerprint: %s\r\n"
                               % wanted, returncode=0)
    assert auto.live_website_fingerprint(run=fake_curl) == wanted
    assert "www/worker.js" in auto.RELEASE_FILES
    assert auto.release_path("www/wrangler.production.toml")


def test_node22_bin_prefers_the_newest_nvm_node(tmp_path):
    for version in ("v20.1.0", "v22.1.0", "v22.23.1"):
        bin_dir = tmp_path / ".nvm" / "versions" / "node" / version / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "npx").write_text("")
    assert auto.node22_bin(tmp_path).parent.name == "v22.23.1"
    assert auto.node22_bin(tmp_path / "none") is None


def test_new_failures_against_the_deployed_base_show_their_output(tmp_path, monkeypatch):
    log = auto.Log(echo=False)
    pipeline = auto.Pipeline(tmp_path, log, state_dir=tmp_path / "state",
                             layout=SimpleNamespace(cache=tmp_path / "cache"))
    auto.save_state({"baselines": {"base": ["app/tests/host/test_old.py::test_known"]}},
                    tmp_path / "state")
    outputs = {
        "app/tests/host/test_old.py": ["E   known breakage",
                                       "FAILED app/tests/host/test_old.py::test_known"],
        "app/tests/host/test_rekey.py": ["E   AssertionError: code_challenges missing",
                                         "FAILED app/tests/host/test_rekey.py::test_columns"],
        "app/tests/host/test_fine.py": ["1 passed in 0.01s"],
    }
    monkeypatch.setattr(pipeline, "archive", lambda commit, work: None)
    monkeypatch.setattr(pipeline, "venv", lambda src: tmp_path / "venv")
    monkeypatch.setattr(pipeline, "prestage", lambda work, venv: None)
    monkeypatch.setattr(pipeline, "test_files", lambda src: sorted(outputs))
    monkeypatch.setattr(pipeline, "pytest_quiet", lambda work, venv, path: (
        path, 1 if "FAILED" in outputs[path][-1] else 0,
        auto.failing_ids(outputs[path]), outputs[path]))
    with pytest.raises(auto.ToolError, match="test_rekey.py::test_columns"):
        pipeline.test_gate("target", "base")
    text = "\n".join(log.lines)
    assert "code_challenges missing" in text
    assert "known breakage" not in text


def test_markdown_under_roll_prefixes_never_needs_a_roll():
    assert not auto.roll_path("deploy/core/edge_client.md")
    assert not auto.roll_path("server/README.md")
    assert auto.roll_path("deploy/core/edge_client.py")
    assert auto.classify(["deploy/core/edge_client.md"])["roll"] == []


def test_a_roll_lost_by_an_earlier_deploy_is_rolled_by_the_next(repo, tmp_path):
    rolled = git(repo, "rev-parse", "HEAD")
    lost = commit(repo, {"tools/mirror_gateway.py": "g\n", "app/a.py": "a2\n"}, "gateway")
    target = commit(repo, {"app/a.py": "a3\n"}, "app")
    state_dir = tmp_path / "state"
    pipeline = auto.Pipeline(repo, auto.Log(echo=False), state_dir=state_dir,
                             layout=SimpleNamespace(cache=tmp_path / "cache"))
    assert pipeline.roll_paths(lost, target, []) == []
    auto.save_state({"lastRoll": {"rev": rolled[:12], "at": 1}}, state_dir)
    assert pipeline.roll_paths(lost, target, []) == ["tools/mirror_gateway.py"]
    auto.save_state({"lastRoll": {"rev": lost[:12], "commit": lost, "at": 1}}, state_dir)
    assert pipeline.roll_paths(lost, target, []) == []
    auto.save_state({"roll": {"since": lost[:12], "paths": ["server/main.go"]}}, state_dir)
    assert pipeline.roll_paths(lost, target, ["deploy/core/x.py"]) == [
        "deploy/core/x.py", "server/main.go"]


def test_a_newer_last_roll_than_the_deployed_base_adds_nothing(repo, tmp_path):
    base = git(repo, "rev-parse", "HEAD")
    later = commit(repo, {"tools/mirror_gateway.py": "g\n"}, "gateway")
    state_dir = tmp_path / "state"
    auto.save_state({"lastRoll": {"rev": later[:12], "at": 1}}, state_dir)
    pipeline = auto.Pipeline(repo, auto.Log(echo=False), state_dir=state_dir,
                             layout=SimpleNamespace(cache=tmp_path / "cache"))
    assert pipeline.roll_paths(base, later, []) == []


class _RollLock:
    fd = 0

    def child_env(self):
        return {}

    def acquire(self, wait=0, sleep=None):
        pass

    def release(self):
        pass


def _roll_pipeline(tmp_path, monkeypatch, exit_code):
    log = auto.Log(echo=False)
    pipeline = auto.Pipeline(tmp_path, log, state_dir=tmp_path / "state",
                             layout=SimpleNamespace(cache=tmp_path / "cache"))
    commands = []

    def fake_stream(command, say, **kwargs):
        commands.append((command, kwargs.get("cwd")))
        return exit_code
    monkeypatch.setattr(auto, "stream", fake_stream)
    monkeypatch.setattr(pipeline, "worktree", lambda target: tmp_path / "wt")
    monkeypatch.setattr(pipeline, "website", lambda target, result: None)
    monkeypatch.setattr(auto.common, "DeployLock", lambda what: _RollLock())
    monkeypatch.setattr(pipeline, "_execute", lambda target, rev, lock, result: dict(
        result, ok=True, deployed=True, roll=["server/main.go"], message="deployed"))
    return pipeline, commands


def test_daemon_changes_roll_the_cores_after_a_good_deploy(tmp_path, monkeypatch):
    pipeline, commands = _roll_pipeline(tmp_path, monkeypatch, 0)
    result = pipeline.execute("a" * 40)
    assert result["rolled"] is True and result["ok"] is True
    assert commands == [
        ([sys.executable, "tools/next_fleet.py", "roll", "--execute"], tmp_path / "wt"),
        ([sys.executable, "tools/next_fleet.py", "roll-runners", "--idle-wait", "0",
          "--execute"], tmp_path / "wt")]
    assert result["runnersRolled"] is True
    notifier = FakeNotifier()
    state = auto.record_outcome(result, notifier, print, state_dir=tmp_path / "state")
    assert "roll" not in state and state["lastRoll"]["rev"] == "a" * 12
    assert "runnerRoll" not in state and state["lastRunnerRoll"]["rev"] == "a" * 12
    assert notifier.sent[-1] == ("Cores rolled after %s" % ("a" * 12),
                                 "node-rolled:%s" % ("a" * 12), False)


def test_a_failed_roll_keeps_the_pending_roll_and_emails(tmp_path, monkeypatch):
    pipeline, _ = _roll_pipeline(tmp_path, monkeypatch, 3)
    result = pipeline.execute("b" * 40)
    assert "rolled" not in result and result["rollError"].endswith("3")
    notifier = FakeNotifier()
    state = auto.record_outcome(result, notifier, print, state_dir=tmp_path / "state")
    assert state["roll"]["paths"] == ["server/main.go"] and state["roll"]["error"]
    assert notifier.sent[-1][1:] == ("node-roll-failed:%s" % ("b" * 12), True)


def _failing_website(target, result):
    result.update(ok=False, message=result["message"] + "; website deploy failed")


def test_a_failed_website_deploy_still_rolls_the_cores(tmp_path, monkeypatch):
    pipeline, commands = _roll_pipeline(tmp_path, monkeypatch, 0)
    monkeypatch.setattr(pipeline, "website", _failing_website)
    result = pipeline.execute("d" * 40)
    assert result["ok"] is False and result["rolled"] is True
    assert commands[0][0] == [sys.executable, "tools/next_fleet.py", "roll", "--execute"]
    state = auto.record_outcome(result, FakeNotifier(), print,
                                state_dir=tmp_path / "state")
    assert "roll" not in state and state["lastRoll"]["rev"] == "d" * 12


def test_a_failed_website_deploy_keeps_a_failed_roll_pending(tmp_path, monkeypatch):
    pipeline, _ = _roll_pipeline(tmp_path, monkeypatch, 3)
    monkeypatch.setattr(pipeline, "website", _failing_website)
    result = pipeline.execute("e" * 40)
    state = auto.record_outcome(result, FakeNotifier(), print,
                                state_dir=tmp_path / "state")
    assert state["roll"]["paths"] == ["server/main.go"] and state["roll"]["error"]


def test_manual_roll_file_turns_the_automatic_roll_off(tmp_path, monkeypatch):
    pipeline, commands = _roll_pipeline(tmp_path, monkeypatch, 0)
    (tmp_path / "state").mkdir(parents=True, exist_ok=True)
    (tmp_path / "state" / "manual-roll").write_text("")
    result = pipeline.execute("c" * 40)
    assert commands == [] and "rolled" not in result
    notifier = FakeNotifier()
    auto.record_outcome(result, notifier, print, state_dir=tmp_path / "state")
    assert notifier.sent[-1][1] == "node-roll:%s" % ("c" * 12)


def test_gateway_only_rolls_leave_the_runners_alone():
    assert auto.runner_roll_needed(["server/runner.go"])
    assert not auto.runner_roll_needed(["deploy/core/edge_client.py",
                                        "tools/mirror_gateway.py"])


def _codes(*codes):
    queue = list(codes)
    seen = []

    def fake_stream(command, say, **kwargs):
        seen.append(command[2])
        return queue.pop(0)
    return fake_stream, seen


def test_a_busy_runner_leaves_a_pending_roll_without_failing(tmp_path, monkeypatch):
    pipeline, _ = _roll_pipeline(tmp_path, monkeypatch, 0)
    fake_stream, seen = _codes(0, auto.RUNNERS_BUSY_EXIT)
    monkeypatch.setattr(auto, "stream", fake_stream)
    result = pipeline.execute("d" * 40)
    assert seen == ["roll", "roll-runners"]
    assert result["ok"] is True and result["runnerRollPending"] is True
    notifier = FakeNotifier()
    state = auto.record_outcome(result, notifier, print, state_dir=tmp_path / "state")
    assert state["runnerRoll"]["commit"] == "d" * 40 and not state["runnerRoll"]["error"]
    assert notifier.sent[-1][1] == "node-rolled:%s" % ("d" * 12)


def test_a_failed_core_roll_never_touches_the_runners(tmp_path, monkeypatch):
    pipeline, _ = _roll_pipeline(tmp_path, monkeypatch, 0)
    fake_stream, seen = _codes(3)
    monkeypatch.setattr(auto, "stream", fake_stream)
    result = pipeline.execute("e" * 40)
    assert seen == ["roll"] and "runnerRollPending" not in result


def test_a_failed_runner_roll_emails_and_stops_retrying(tmp_path, monkeypatch):
    pipeline, _ = _roll_pipeline(tmp_path, monkeypatch, 0)
    fake_stream, _ = _codes(0, 1)
    monkeypatch.setattr(auto, "stream", fake_stream)
    result = pipeline.execute("f" * 40)
    notifier = FakeNotifier()
    state = auto.record_outcome(result, notifier, print, state_dir=tmp_path / "state")
    assert state["runnerRoll"]["error"]
    assert notifier.sent[-1][1:] == ("runner-roll-failed:%s" % ("f" * 12), True)
    assert auto.retry_runner_roll(tmp_path, auto.Log(echo=False),
                                  state_dir=tmp_path / "state") is None


def _pending_runner_roll(tmp_path, monkeypatch, code):
    state_dir = tmp_path / "state"
    state_dir.mkdir()
    (state_dir / "armed").write_text("")
    auto.save_state({"runnerRoll": {"commit": "9" * 40, "rev": "9" * 12, "at": 0,
                                    "error": ""}}, state_dir)
    monkeypatch.setattr(auto.common, "DeployLock", lambda what: _RollLock())
    calls = []

    class FakePipeline(auto.Pipeline):
        def runner_worktree(self, target):
            calls.append(target)
            return tmp_path / "wt"
    monkeypatch.setattr(auto, "stream", lambda command, say, **kwargs: code)
    notifier = FakeNotifier()
    result = auto.retry_runner_roll(
        tmp_path, auto.Log(echo=False), state_dir=state_dir, notifier=notifier,
        layout=SimpleNamespace(cache=tmp_path / "cache"), pipeline_class=FakePipeline)
    return result, auto.load_state(state_dir), notifier, calls


def test_the_retry_rolls_the_runners_once_they_are_idle(tmp_path, monkeypatch):
    result, state, notifier, calls = _pending_runner_roll(tmp_path, monkeypatch, 0)
    assert calls == ["9" * 40] and result["runnersRolled"] is True
    assert "runnerRoll" not in state and state["lastRunnerRoll"]["rev"] == "9" * 12
    assert notifier.sent[-1][1] == "runners-rolled:%s" % ("9" * 12)


def test_the_retry_keeps_waiting_while_a_runner_is_busy(tmp_path, monkeypatch):
    result, state, notifier, _ = _pending_runner_roll(
        tmp_path, monkeypatch, auto.RUNNERS_BUSY_EXIT)
    assert result["runnerRollPending"] is True
    assert state["runnerRoll"]["commit"] == "9" * 40 and state["runnerRoll"]["at"] > 0
    assert notifier.sent == []


def test_a_failed_next_deploy_step_names_its_cause(tmp_path):
    output = ["[execute] pick the first core node\n",
              "next_deploy: SSH tunnel to n1 exited 255: Permission denied. Fix: key\n",
              "[execute] World deploy notice: failed abc\n"]

    def popen(command, **kwargs):
        return SimpleNamespace(stdout=iter(output), wait=lambda: 1, kill=lambda: None)
    log = auto.Log(echo=False)
    pipeline = auto.Pipeline(tmp_path, log, popen=popen, layout=SimpleNamespace())
    lock = SimpleNamespace(child_env=lambda: {}, fd=0)
    with pytest.raises(auto.ToolError) as caught:
        pipeline.tool(tmp_path, lock, ["migrate", "--execute"], 60)
    assert str(caught.value) == ("next_deploy migrate exited 1: SSH tunnel to n1 exited "
                                 "255: Permission denied. Fix: key")
    output[:] = ["building\n"]
    pipeline.popen = lambda command, **kwargs: SimpleNamespace(
        stdout=iter(output), wait=lambda: 2, kill=lambda: None)
    with pytest.raises(auto.ToolError, match=r"^next_deploy build exited 2$"):
        pipeline.tool(tmp_path, lock, ["build"], 60)


def test_installed_agent_resolves_node_addresses_from_its_own_folder(tmp_path, monkeypatch):
    layout = SimpleNamespace(node_deploy_bin=tmp_path / "bin", systemd_units=tmp_path / "units")
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    monkeypatch.setattr(common, "autodeploy_dir", lambda: tmp_path / "autodeploy")
    calls = []
    assert auto.install(layout=layout, run=lambda *a, **k: calls.append(a), enable=False) == 0
    for name in auto.INSTALLED_FILES + auto.INSTALLED_DEPLOY_FILES:
        assert (layout.node_deploy_bin / name).is_file(), name
    instances = tmp_path / "instances.json"
    instances.write_text(json.dumps({"fmnext-core-1": {"ip": "203.0.113.11"}}))
    fleet = ('domain = "example.test"\napp_host = "app.example.test"\n'
             '[[nodes]]\nname = "n1"\nlabel = "fmnext-core-1"\nrole = "core"\n')
    script = ("import sys, json; sys.path.insert(0, sys.argv[1]); import next_autodeploy as a; "
              "print(json.dumps(a.fleet_hosts(sys.argv[2], json.load(open(sys.argv[3])))))")
    done = subprocess.run([sys.executable, "-I", "-c", script, str(layout.node_deploy_bin),
                           fleet, str(instances)], capture_output=True, text=True, check=False)
    assert done.returncode == 0, done.stderr
    assert json.loads(done.stdout)["nodes"] == [{"name": "n1", "ip": "203.0.113.11"}]


def offline_hosts():
    return {"app_host": "app.example.com", "domain": "next.example.com",
            "world_host": "world.example.com",
            "nodes": [{"name": "n%d" % i, "ip": "203.0.113.1%d" % i} for i in (1, 2, 3)]}


def test_gate_one_waits_for_this_machine_to_come_back_online():
    online = []
    now = [0.0]

    def run(command, **kwargs):
        if not online:
            return subprocess.CompletedProcess(command, 7, "\n000", "")
        return subprocess.CompletedProcess(command, 0, '{"rev": "abc1234"}\n200', "")

    def sleep(seconds):
        now[0] += seconds
        if now[0] >= 600:
            online.append(True)
    said = []
    revs = auto.live_revs(offline_hosts(), run=run, say=said.append, sleep=sleep,
                          clock=lambda: now[0])
    assert revs == {"n1": "abc1234", "n2": "abc1234", "n3": "abc1234"}
    assert any("looks offline" in line for line in said)
    assert said[-1] == "gate 1: the network is back"


def test_gate_one_gives_up_offline_with_a_cause_that_names_this_machine():
    now = [0.0]

    def run(command, **kwargs):
        return subprocess.CompletedProcess(command, 7, "\n000", "")

    def sleep(seconds):
        now[0] += seconds
    with pytest.raises(GuardError, match="this machine looks offline"):
        auto.live_revs(offline_hosts(), run=run, say=lambda line: None, sleep=sleep,
                       clock=lambda: now[0])
    assert now[0] >= auto.OFFLINE_WAIT_SECONDS


def test_gate_one_fails_at_once_when_only_one_node_is_down():
    slept = []

    def run(command, **kwargs):
        if "203.0.113.11" in command[command.index("--resolve") + 1]:
            return subprocess.CompletedProcess(command, 7, "\n000", "")
        return subprocess.CompletedProcess(command, 0, '{"rev": "abc1234"}\n200', "")
    with pytest.raises(GuardError, match="n1 did not answer /api/version"):
        auto.live_revs(offline_hosts(), run=run, say=lambda line: None, sleep=slept.append,
                       clock=lambda: 0.0)
    assert all(seconds == auto.CURL_RETRY_SECONDS for seconds in slept)


def test_the_busy_exit_code_matches_next_fleet():
    import next_fleet
    assert auto.RUNNERS_BUSY_EXIT == next_fleet.RUNNERS_BUSY_EXIT
