import io
import json
import re
import subprocess
from pathlib import Path

import pytest

import next_add_core
import next_common as common
import next_dns
import next_fleet
import next_remove_core as rc
from fakes import FakeSsh
from next_common import GuardError, Runner, ToolError
from test_next_add_core import (N4_ID, N4_IP, config, fleet_text, git, n4_block,
                                seed_config, write_fleet)
from test_next_dns import V4, Zone, a, aaaa

KEYS = {name: (letter * 43 + "=") for name, letter in
        (("n1", "A"), ("n2", "B"), ("n3", "C"), ("n4", "D"))}
IPS = dict(V4, n4=N4_IP)


def seed(cores=4, n4_voter=True):
    seed_config()
    common.save_json(config() / "fleet-state.json", {"wg_pubkeys": dict(KEYS)})
    return cores


def fleet_file(tmp_path, cores=4, n4_voter=True):
    seed(cores, n4_voter)
    if cores == 4:
        return write_fleet(tmp_path, with_n4=True, voter=n4_voter)
    return write_fleet(tmp_path)


def nodes_json(reachable=("n1", "n2", "n3", "n4"), leader="n1", members=("n1", "n2", "n3", "n4"),
               nonvoters=()):
    return json.dumps({"nodes": [
        {"id": n, "reachable": n in reachable, "leader": n == leader,
         "voter": n not in nonvoters} for n in members]}).encode()


class Cluster:
    def __init__(self, **kwargs):
        self.view = dict(kwargs)
        self.removed = False
        self.stepped_down = False

    def nodes(self, host, script, data):
        view = dict(self.view)
        if self.removed:
            view["members"] = tuple(m for m in view.get("members", ("n1", "n2", "n3", "n4"))
                                    if m != "n4")
        if self.stepped_down:
            view["leader"] = "n2"
        return nodes_json(**view)

    def remove(self, host, script, data):
        assert json.loads(data) == {"id": "n4"}
        self.removed = True
        return b""

    def leader(self, host, script, data):
        assert host == N4_IP
        self.stepped_down = True
        return b""


def ssh_for(cluster=None, keys=None):
    cluster = cluster or Cluster()
    keys = dict(KEYS if keys is None else keys)
    by_ip = {ip: name for name, ip in IPS.items()}
    return FakeSsh([
        (lambda h, s: "/nodes" in s, cluster.nodes),
        (lambda h, s: "/remove" in s, cluster.remove),
        (lambda h, s: "/leader" in s, cluster.leader),
        (lambda h, s: s == rc.WG_PUBKEY, lambda h, s, i: keys.get(by_ip.get(h), "").encode()),
        (lambda h, s: s == rc.WG_PEERS, lambda h, s, i: b""),
    ])


def make_job(tmp_path, execute=True, fleet_path=None, cluster=None, destroy=False,
             n4_voter=True, **extra):
    fleet_path = fleet_path or fleet_file(tmp_path, n4_voter=n4_voter)
    ssh = ssh_for(cluster)
    backend = rc.RemoveBackend(ssh=ssh, sleep=lambda s: None)
    planned = backend.plan(fleet_path, "n4", destroy)
    planned.update(extra)
    out = io.StringIO()
    path = rc.state_path("n4") if execute else Path("/dev/null")
    job = rc.Job(next_add_core.State(path, dict(planned, steps={})),
                 Runner(execute=execute, out=out), fleet_file=fleet_path, out=out)
    ssh.runs.clear()
    return job, backend, ssh, out


def events(out):
    return [json.loads(line[len(rc.EVENT):]) for line in out.getvalue().splitlines()
            if line.startswith(rc.EVENT)]


def test_quorum_allows_removing_one_of_four_healthy_voters():
    report = rc.quorum_report(json.loads(nodes_json())["nodes"], "n4", ["n1", "n2", "n3"])
    assert report["allowed"] and report["healthy_after"] == ["n1", "n2", "n3"]
    assert report["voters_after"] == ["n1", "n2", "n3"] and report["leader"] == "n1"


def test_quorum_refuses_going_below_three_cores_or_voters():
    nodes = json.loads(nodes_json(members=("n1", "n2", "n3")))["nodes"]
    report = rc.quorum_report(nodes, "n3", ["n1", "n2"])
    assert not report["allowed"]
    assert "2 core(s)" in report["blocker"] and "2 healthy rqlite voter(s)" in report["blocker"]


def test_quorum_refuses_when_the_remaining_voters_are_unhealthy():
    nodes = json.loads(nodes_json(reachable=("n1", "n3", "n4")))["nodes"]
    report = rc.quorum_report(nodes, "n4", ["n1", "n2", "n3"])
    assert not report["allowed"] and "n1, n3" in report["blocker"]
    seven = [{"id": "n%d" % i, "voter": True, "reachable": i in (1, 2, 3, 7)}
             for i in range(1, 8)]
    report = rc.quorum_report(seven, "n7", ["n%d" % i for i in range(1, 7)])
    assert not report["allowed"] and "not a majority" in report["blocker"]


def test_quorum_allows_a_non_voter_and_flags_the_leader():
    nodes = json.loads(nodes_json(nonvoters=("n4",)))["nodes"]
    report = rc.quorum_report(nodes, "n4", ["n1", "n2", "n3"])
    assert report["allowed"] and not report["target_voter"]
    led = rc.quorum_report(json.loads(nodes_json(leader="n4"))["nodes"], "n4",
                           ["n1", "n2", "n3"])
    assert led["allowed"] and any("leader" in w for w in led["warnings"])


def test_remove_core_block_is_the_inverse_of_upsert():
    text = fleet_text()
    added = next_fleet.upsert_core_block(text, "n4", n4_block())
    assert next_fleet.remove_core_block(added, "n4") == text
    without_n2 = next_fleet.remove_core_block(text, "n2")
    assert 'name = "n2"' not in without_n2
    assert 'name = "n1"' in without_n2 and 'name = "n3"' in without_n2
    without_n3 = next_fleet.remove_core_block(text, "n3")
    assert "# Runners are untrusted" in without_n3 and "[[runners]]" in without_n3
    assert next_fleet.remove_core_block(text, "n9") == text


def test_cli_dry_run_prints_the_plan_and_changes_nothing(tmp_path, capsys):
    path = fleet_file(tmp_path)
    before = {p.name: p.read_bytes() for p in config().iterdir()}
    text = path.read_text()
    ssh = ssh_for()
    assert next_fleet.main(["--fleet", str(path), "remove-core", "n4"], ssh=ssh) == 0
    out = capsys.readouterr().out
    for step in rc.step_ids(False):
        assert ". %s" % step in out
    assert "destroy" not in [line.split()[1] for line in out.splitlines()
                             if line[:3].strip().rstrip(".").isdigit()]
    assert "after removing n4: n1, n2, n3 (healthy n1, n2, n3)" in out
    assert "fmnext-core-4 is kept" in out
    assert "dry run: nothing was changed" in out and rc.EVENT not in out
    assert path.read_text() == text
    assert {p.name: p.read_bytes() for p in config().iterdir()} == before
    assert ssh.runs and all("/nodes" in script for _, script, _ in ssh.runs)
    assert ssh.puts == []


def test_cli_dry_run_with_destroy_lists_the_provider_step(tmp_path, capsys):
    path = fleet_file(tmp_path)
    assert next_fleet.main(["--fleet", str(path), "remove-core", "n4",
                            "--destroy-instance"], ssh=ssh_for()) == 0
    out = capsys.readouterr().out
    assert ". destroy" in out and "IRREVERSIBLE" in out and "vultr: delete" in out


def test_cli_dry_run_refuses_a_removal_that_breaks_quorum(tmp_path, capsys):
    path = fleet_file(tmp_path, cores=3)
    ssh = ssh_for(Cluster(members=("n1", "n2", "n3")))
    assert next_fleet.main(["--fleet", str(path), "remove-core", "n3"], ssh=ssh) == 2
    out = capsys.readouterr().out
    assert "REFUSED" in out and "the fleet needs at least 3" in out


def test_execute_refuses_before_touching_anything(tmp_path, monkeypatch, capsys):
    path = fleet_file(tmp_path, cores=3)
    monkeypatch.setattr(common.DeployLock, "acquire", lambda self, wait=0: self)
    monkeypatch.setattr(common.DeployLock, "release", lambda self: None)
    ssh = ssh_for(Cluster(members=("n1", "n2", "n3")))
    assert next_fleet.main(["--fleet", str(path), "remove-core", "n3", "--execute"],
                           ssh=ssh) == 2
    assert "needs at least 3" in capsys.readouterr().err
    assert not rc.state_path("n3").exists()
    assert all("/nodes" in script for _, script, _ in ssh.runs)


def test_unknown_or_app_names_are_refused(tmp_path):
    path = fleet_file(tmp_path)
    backend = rc.RemoveBackend(ssh=ssh_for())
    with pytest.raises(GuardError):
        backend.plan(path, "n7")
    with pytest.raises(GuardError):
        rc.check_name("r1")


def test_plan_json_for_the_desktop(tmp_path, capsys):
    path = fleet_file(tmp_path)
    assert next_fleet.main(["--fleet", str(path), "remove-core-plan", "n4", "--json"],
                           ssh=ssh_for(Cluster(leader="n4"))) == 0
    plan = json.loads(capsys.readouterr().out)
    assert plan["allowed"] is True and plan["is_leader"] is True
    assert plan["provider"] == "vultr" and plan["instance_id"] == N4_ID
    assert [s["id"] for s in plan["steps"]] == list(rc.step_ids(False))
    assert plan["voters_after"] == ["n1", "n2", "n3"] and plan["resume"] is False
    assert any("POST /leader" in line for line in plan["description"])
    assert next_fleet.main(["--fleet", str(path), "remove-core-plan", "n4", "--json",
                            "--destroy-instance"], ssh=FakeSsh(returncode=255)) == 0
    blocked = json.loads(capsys.readouterr().out)
    assert blocked["allowed"] is False and "cannot read the rqlite cluster" in blocked["blocker"]
    assert blocked["steps"][-1]["id"] == "destroy"


def test_provider_comes_from_the_instance_record(tmp_path):
    path = fleet_file(tmp_path)
    data = common.load_json(config() / "fleet-instances.json", {})
    data["fmnext-core-4"]["provider"] = "aws"
    common.save_json(config() / "fleet-instances.json", data)
    plan = rc.RemoveBackend(ssh=ssh_for()).plan(path, "n4")
    assert plan["provider"] == "aws"
    try:
        import next_cloud  # noqa: F401
    except ImportError:
        with pytest.raises(ToolError):
            rc.provider_client("aws")


def test_identity_check_reads_the_wireguard_key_over_ssh():
    identity = rc.SshIdentity(ssh_for(), {"n4": KEYS["n4"], "n1": KEYS["n1"]})
    identity.verify_target("n4", N4_IP)
    assert next_fleet.verify_target_instance(identity, "n1", V4["n1"]) == {
        "name": "n1", "ip": V4["n1"]}
    with pytest.raises(GuardError):
        identity.verify_target("n1", V4["n2"])
    with pytest.raises(GuardError):
        rc.SshIdentity(ssh_for(), {}).verify_target("n4", N4_IP)


def test_preflight_execute_rechecks_quorum(tmp_path):
    job, backend, ssh, out = make_job(tmp_path)
    backend.step_preflight(job)
    assert job.state.get("leader") == "n1" and "healthy n1, n2, n3" in out.getvalue()
    backend.ssh = ssh_for(Cluster(reachable=("n1", "n4")))
    with pytest.raises(GuardError):
        backend.step_preflight(job)


def test_preflight_refuses_during_an_unfinished_add_core(tmp_path):
    job, backend, _, _ = make_job(tmp_path)
    common.save_json(next_add_core.state_path("n5"), {"name": "n5", "steps": {}})
    with pytest.raises(GuardError):
        backend.step_preflight(job)


def test_leader_step_hands_leadership_over_first(tmp_path):
    cluster = Cluster(leader="n4")
    job, backend, ssh, out = make_job(tmp_path, cluster=cluster)
    backend.ssh = ssh_for(cluster)
    backend.step_leader(job)
    posts = [(h, s) for h, s, _ in backend.ssh.runs if "/leader" in s]
    assert posts and posts[0][0] == N4_IP and "-X POST" in posts[0][1]
    assert job.state.get("leader") == "n2"


def test_leader_step_does_nothing_when_another_core_leads(tmp_path):
    job, backend, ssh, _ = make_job(tmp_path)
    backend.step_leader(job)
    assert not [s for _, s, _ in ssh.runs if "/leader" in s]


def test_stop_step_disables_the_services_on_the_node_only(tmp_path):
    job, backend, ssh, _ = make_job(tmp_path)
    backend.step_stop(job)
    stops = [(h, s) for h, s, _ in ssh.runs if "disable --now" in s]
    assert len(stops) == 1 and stops[0][0] == N4_IP
    for unit in ("forkmesh-app.service", "forkmesh-mirror-node.service", "caddy.service"):
        assert unit in stops[0][1]
    assert "rqlite.service" not in stops[0][1]


def test_rqlite_step_stops_it_then_removes_it_through_a_peer(tmp_path):
    cluster = Cluster()
    job, backend, _, out = make_job(tmp_path, cluster=cluster)
    backend.ssh = ssh_for(cluster)
    backend.step_rqlite(job)
    runs = backend.ssh.runs
    stop = next(i for i, (h, s, _) in enumerate(runs) if "rqlite.service" in s)
    remove = next(i for i, (h, s, _) in enumerate(runs) if "/remove" in s)
    assert runs[stop][0] == N4_IP and stop < remove
    assert runs[remove][0] != N4_IP and "-X DELETE" in runs[remove][1]
    assert cluster.removed and "left the rqlite cluster" in out.getvalue()


def test_rqlite_step_refuses_without_quorum(tmp_path):
    job, backend, _, _ = make_job(tmp_path)
    backend.ssh = ssh_for(Cluster(reachable=("n1", "n2", "n4")))
    with pytest.raises(GuardError):
        backend.step_rqlite(job)
    assert not [s for _, s, _ in backend.ssh.runs if "disable" in s or "/remove" in s]


def test_fleet_step_drops_the_block_and_its_state(tmp_path):
    job, backend, _, _ = make_job(tmp_path, n4_voter=False)
    state = common.load_json(config() / "fleet-state.json", {})
    state["rqlite_nonvoters"] = ["n4"]
    common.save_json(config() / "fleet-state.json", state)
    backend.step_fleet(job)
    assert 'name = "n4"' not in job.fleet_file.read_text()
    assert job.fleet_file.read_text() == fleet_text()
    state = common.load_json(config() / "fleet-state.json", {})
    assert "n4" not in state["wg_pubkeys"] and state["rqlite_nonvoters"] == []
    backend.step_fleet(job)
    assert job.fleet_file.read_text() == fleet_text()


def test_peers_step_rolls_every_remaining_core_without_the_node(tmp_path, monkeypatch):
    job, backend, ssh, _ = make_job(tmp_path)
    backend.step_fleet(job)
    rolled = []

    def roll(fleet, names, ssh, runner, steps, vultr=None, fleet_path=None, **kw):
        rolled.append((names, steps, sorted(n["name"] for n in fleet["nodes"])))
        vultr.verify_target(names[0], IPS[names[0]])
        return names
    monkeypatch.setattr(next_fleet, "roll", roll)
    backend.step_peers(job)
    assert [r[0] for r in rolled] == [["n1"], ["n2"], ["n3"]]
    assert all(r[1] == ("files", "start") and r[2] == ["n1", "n2", "n3"] for r in rolled)
    assert job.state.items("peers_done") == ["n1", "n2", "n3"]
    rolled.clear()
    backend.step_peers(job)
    assert rolled == []


def test_peers_step_fails_when_a_peer_keeps_the_wireguard_peer(tmp_path, monkeypatch):
    job, backend, ssh, _ = make_job(tmp_path)
    backend.step_fleet(job)
    monkeypatch.setattr(next_fleet, "roll", lambda *a, **k: [])
    ssh.answers.insert(0, (lambda h, s: s == rc.WG_PEERS, lambda h, s, i: KEYS["n4"].encode()))
    with pytest.raises(ToolError):
        backend.step_peers(job)


def test_deregister_forgets_the_node_and_deletes_only_its_own_name(tmp_path):
    job, backend, _, _ = make_job(tmp_path)
    backend.step_fleet(job)
    common.save_json(next_dns.state_path(), {"nodes": {
        "n1": {"in_dns": True}, "n4": {"in_dns": False, "held": True}}})
    zone = Zone([a("x4", "n4.next.forkmesh.com", N4_IP),
                 aaaa("y4", "n4.next.forkmesh.com", "2001:19f0:ffff::14"),
                 a("x1", "n1.next.forkmesh.com", V4["n1"]),
                 a("p1", "app.next.forkmesh.com", V4["n1"])])
    backend._cf = zone.cf
    backend.step_deregister(job)
    assert sorted(zone.records) == ["p1", "x1"]
    assert "n4" not in common.load_json(next_dns.state_path(), {})["nodes"]


def repo_with_n4(tmp_path, monkeypatch):
    for key, value in (("GIT_AUTHOR_NAME", "t"), ("GIT_AUTHOR_EMAIL", "t@example.com"),
                       ("GIT_COMMITTER_NAME", "t"),
                       ("GIT_COMMITTER_EMAIL", "t@example.com")):
        monkeypatch.setenv(key, value)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(tmp_path / "gitconfig"))
    path = tmp_path / "repo"
    (path / "deploy").mkdir(parents=True)
    (path / "deploy" / "fleet.toml").write_text(
        next_fleet.upsert_core_block(fleet_text(), "n4", n4_block(voter=True)))
    (path / "README").write_text("x\n")
    git(path, "init", "-q", "-b", "main")
    git(path, "add", ".")
    git(path, "commit", "-qm", "base")
    return path


def test_commit_goes_to_a_branch_and_leaves_head_index_and_tree_alone(tmp_path, monkeypatch):
    seed()
    repo = repo_with_n4(tmp_path, monkeypatch)
    fleet_path = repo / "deploy" / "fleet.toml"
    job, _, ssh, out = make_job(tmp_path, fleet_path=fleet_path)
    backend = rc.RemoveBackend(ssh=ssh, repo=repo)
    head = git(repo, "rev-parse", "HEAD")
    tree_before = fleet_path.read_text()
    backend.step_commit(job)
    commit = job.state.get("commit")
    assert git(repo, "rev-parse", "HEAD") == head
    assert git(repo, "rev-parse", "fleet/remove-core-n4") == commit
    assert git(repo, "rev-parse", commit + "^") == head
    assert git(repo, "show", commit + ":deploy/fleet.toml") + "\n" == fleet_text()
    assert git(repo, "diff", "--name-only", head, commit) == "deploy/fleet.toml"
    assert git(repo, "status", "--porcelain") == ""
    assert fleet_path.read_text() == tree_before
    assert "merge --autostash --no-ff fleet/remove-core-n4" in out.getvalue()
    backend.step_commit(job)
    assert git(repo, "rev-parse", "fleet/remove-core-n4") == commit


def test_commit_refuses_other_branches(tmp_path, monkeypatch):
    repo = repo_with_n4(tmp_path, monkeypatch)
    with pytest.raises(GuardError):
        next_add_core.Git(repo).commit_file("fleet/remove-core-main", "refs/heads/main",
                                            "deploy/fleet.toml", "x", "m")


class FakeBackend:
    def __init__(self, fail_at=None):
        self.calls = []
        self.fail_at = fail_at

    def __getattr__(self, name):
        if not name.startswith("step_"):
            raise AttributeError(name)
        step = name[len("step_"):]

        def action(job):
            self.calls.append(step)
            if step == self.fail_at:
                self.fail_at = None
                raise ToolError("boom at %s" % step)
            if step == "destroy":
                job.state.set(destroyed=True)
        return action

    def contain(self, job):
        return ["%s is out of DNS and held" % job.name]


def test_run_resumes_from_the_failed_step_and_emits_events(tmp_path):
    job, _, _, out = make_job(tmp_path)
    backend = FakeBackend(fail_at="peers")
    with pytest.raises(ToolError) as error:
        rc.run(job, backend)
    assert "resume from peers" in str(error.value)
    assert backend.calls == list(rc.step_ids(False)[:rc.step_ids(False).index("peers") + 1])
    seen = events(out)
    assert seen[0]["event"] == "start" and [s["id"] for s in seen[0]["steps"]] == \
        list(rc.step_ids(False))
    assert seen[-1] == {"event": "result", "status": "failed", "name": "n4", "step": "peers",
                        "error": "boom at peers", "notes": ["n4 is out of DNS and held"]}
    reloaded = rc.Job(next_add_core.State.load(rc.state_path("n4")), job.runner,
                      fleet_file=job.fleet_file, out=out)
    backend.calls.clear()
    rc.run(reloaded, backend)
    assert backend.calls == ["peers", "deregister", "commit"]
    result = events(out)[-1]
    assert result["status"] == "ok" and "instance is kept" in result["summary"]
    assert "destroy" not in backend.calls


class FakeProvider:
    name = "vultr"

    def __init__(self, instance):
        self.instance = instance
        self.deleted = []

    def find_by_label(self, label):
        return self.instance if self.instance and self.instance["label"] == label else None

    def delete_instance(self, instance_id):
        self.deleted.append(instance_id)


def finished_except_destroy(tmp_path, monkeypatch, provider):
    path = fleet_file(tmp_path)
    monkeypatch.setattr(common.DeployLock, "acquire", lambda self, wait=0: self)
    monkeypatch.setattr(common.DeployLock, "release", lambda self: None)
    calls = []

    def factory(name, region=None):
        calls.append((name, region))
        return provider
    backend = rc.RemoveBackend(ssh=ssh_for(), provider_factory=factory, sleep=lambda s: None)
    planned = backend.plan(path, "n4")
    steps = {step: {"status": "done"} for step in rc.step_ids(False)}
    common.save_json(rc.state_path("n4"), dict(planned, steps=steps, finished=True))
    path.write_text(fleet_text())
    return path, backend, calls


def args_for(path, destroy, execute=True):
    return type("Args", (), {"execute": execute, "dry_run": not execute,
                             "fleet": str(path), "target": "n4", "events": True,
                             "destroy_instance": destroy, "command": "remove-core"})()


def test_the_instance_is_deleted_only_with_the_flag(tmp_path, monkeypatch):
    provider = FakeProvider({"id": N4_ID, "label": "fmnext-core-4", "tags": ["forkmesh-next"],
                             "main_ip": N4_IP})
    path, backend, calls = finished_except_destroy(tmp_path, monkeypatch, provider)
    with pytest.raises(ToolError) as error:
        rc.main_remove_core(args_for(path, False), backend=backend, out=io.StringIO())
    assert "--destroy-instance" in str(error.value)
    assert calls == [] and provider.deleted == []
    out = io.StringIO()
    assert rc.main_remove_core(args_for(path, True), backend=backend, out=out) == 0
    assert [c[0] for c in calls] == ["vultr"] and provider.deleted == [N4_ID]
    assert "fmnext-core-4" not in common.load_json(config() / "fleet-instances.json", {})
    seen = events(out)
    ran = [e["step"] for e in seen if e.get("event") == "step" and not e.get("resumed")
           and e.get("status") == "running"]
    assert ran == ["destroy"] and seen[-1]["destroyed"] is True
    with pytest.raises(ToolError) as error:
        rc.main_remove_core(args_for(path, True), backend=backend, out=io.StringIO())
    assert "already removed" in str(error.value)


def test_destroy_refuses_an_instance_that_does_not_match_the_record(tmp_path, monkeypatch):
    provider = FakeProvider({"id": "ffffffff-0000-0000-0000-000000000009",
                             "label": "fmnext-core-4", "tags": ["forkmesh-next"],
                             "main_ip": N4_IP})
    path, backend, _ = finished_except_destroy(tmp_path, monkeypatch, provider)
    with pytest.raises(ToolError):
        rc.main_remove_core(args_for(path, True), backend=backend, out=io.StringIO())
    assert provider.deleted == []
    assert rc.State.load(rc.state_path("n4")).status("destroy") == "failed"
    untagged = FakeProvider({"id": N4_ID, "label": "fmnext-core-4", "tags": [],
                             "main_ip": N4_IP})
    backend.provider_factory = lambda name, region=None: untagged
    with pytest.raises(ToolError):
        rc.main_remove_core(args_for(path, True), backend=backend, out=io.StringIO())
    assert untagged.deleted == []


def test_status_reports_the_last_run(tmp_path, monkeypatch, capsys):
    provider = FakeProvider(None)
    path, _, _ = finished_except_destroy(tmp_path, monkeypatch, provider)
    assert next_fleet.main(["remove-core-status", "--json"]) == 0
    status = json.loads(capsys.readouterr().out)
    assert status["name"] == "n4" and status["finished"] is True
    assert status["steps"]["commit"] == "done" and status["steps"]["destroy"] == "pending"
    assert next_fleet.main(["--fleet", str(path), "remove-core-plan", "n4", "--json"],
                           ssh=ssh_for()) == 0
    plan = json.loads(capsys.readouterr().out)
    assert plan["resume"] is True and plan["allowed"] is False
    assert "tick the instance box" in plan["blocker"]
    assert next_fleet.main(["--fleet", str(path), "remove-core-plan", "n4", "--json",
                            "--destroy-instance"], ssh=ssh_for()) == 0
    assert json.loads(capsys.readouterr().out)["allowed"] is True


def test_forgetting_an_instance_entry_is_limited_to_core_labels():
    with pytest.raises(GuardError):
        rc.remove_instance_entry("fmnext-run-1")
    with pytest.raises(GuardError):
        rc.remove_instance_entry("mirror35")


def test_scripts_hold_no_recursive_delete():
    for script in (rc.remove_script(), rc.stop_script(rc.STOP_UNITS)):
        assert re.search(r"\brm\s+-[a-zA-Z]*[rR]", script) is None
    assert subprocess.run(["bash", "-n", "-c", rc.stop_script(rc.STOP_UNITS)]).returncode == 0
    assert subprocess.run(["bash", "-n", "-c", rc.remove_script()]).returncode == 0
