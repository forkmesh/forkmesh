import io
import json
import os
import subprocess
import threading
from pathlib import Path

import pytest

import next_add_core
import next_common as common
import next_dns
import next_fleet
from fakes import FakeHttp, FakeSsh
from next_common import GuardError, Runner, ToolError
from test_next_dns import V4, V6, Zone, a, aaaa

REPO = Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "deploy" / "fleet.example.toml"
REV = "abcdefabcdef"
N4_IP = "203.0.113.14"
N4_V6 = "2001:19f0:ffff::14"
N4_ID = "cccccccc-0000-0000-0000-000000000004"
PROD = ("app.forkmesh.com", "world.forkmesh.com")
NEXT = ("app.next.forkmesh.com", "world.next.forkmesh.com")


def config():
    return Path(os.environ["FMNEXT_CONFIG_DIR"])


def fleet_text():
    text = EXAMPLE.read_text("utf-8")
    text = text.replace("\n[dns]\n", '\n[production]\napp_host = "app.forkmesh.com"\n'
                        'world_host = "world.forkmesh.com"\n\n[dns]\n', 1)
    for name, ip in V4.items():
        text = text.replace('public_ipv4 = "%s"\n' % ip, 'public_ipv4 = "%s"\n'
                            'public_ipv6 = "%s"\n' % (ip, V6[name]), 1)
    return text


def write_fleet(tmp_path, with_n4=False, voter=False):
    text = fleet_text()
    if with_n4:
        text = next_fleet.upsert_core_block(text, "n4", n4_block(voter))
    path = tmp_path / "fleet.toml"
    path.write_text(text, "utf-8")
    return path


def record_instance(label, info):
    path = config() / "fleet-instances.json"
    data = common.load_json(path, {}) or {}
    data[label] = info
    common.save_json(path, data)


def n4_block(voter=False, pubkey=""):
    record_instance("fmnext-core-4", {"id": N4_ID, "ip": N4_IP, "ipv6": N4_V6})
    return next_fleet.core_block(4, "fmnext-core-4", {"id": N4_ID, "ip": N4_IP,
                                                      "ipv6": N4_V6},
                                 pubkey, voter=voter)


def seed_config(cores=3):
    instances = {"fmnext-core-%d" % n: {"id": "aaaaaaaa-0000-0000-0000-00000000000%d" % n,
                                        "ip": V4["n%d" % n], "plan": "vc2-1c-1gb",
                                        "region": "ewr"} for n in range(1, cores + 1)}
    instances["fmnext-run-1"] = {"id": "aaaaaaaa-0000-0000-0000-000000000011",
                                 "ip": "203.0.113.21", "plan": "vc2-2c-4gb",
                                 "region": "ewr"}
    for label, info in instances.items():
        record_instance(label, info)
    common.save_json(config() / "deploy-manifest.json", {"current": REV})
    return instances


def make_job(tmp_path, execute=True, fleet_path=None, **state):
    out = io.StringIO()
    data = {"name": "n4", "number": 4, "label": "fmnext-core-4", "region": "ewr",
            "plan": "vc2-1c-1gb", "voter": False, "rev": REV,
            "voters_before": ["n1", "n2", "n3"], "peers": ["n1", "n2", "n3"]}
    data.update(state)
    path = next_add_core.state_path("n4")
    job = next_add_core.Job(next_add_core.State(path, data),
                            Runner(execute=execute, out=out),
                            fleet_file=fleet_path or write_fleet(tmp_path), out=out)
    return job, out


def events(out):
    return [json.loads(line[len(next_add_core.EVENT):])
            for line in out.getvalue().splitlines()
            if line.startswith(next_add_core.EVENT)]


class FakeBackend:
    def __init__(self, fail_at=None, fail_times=1):
        self.calls = []
        self.fail_at = fail_at
        self.fail_times = fail_times
        self.contained = 0
        self.in_dns = False
        self.healthy = False

    def __getattr__(self, name):
        if not name.startswith("step_"):
            raise AttributeError(name)
        step = name[len("step_"):]

        def action(job):
            self.calls.append(step)
            if step == self.fail_at and self.fail_times > 0:
                self.fail_times -= 1
                raise ToolError("boom at %s" % step)
            if step == "health":
                self.healthy = True
            if step == "dns":
                assert self.healthy
                self.in_dns = True
        return action

    def contain(self, job):
        self.contained += 1
        self.in_dns = False
        return ["%s held out of DNS" % job.name]


@pytest.mark.parametrize("voters,voter", [(1, False), (2, True), (3, False),
                                          (4, True), (5, False)])
def test_voter_choice_keeps_an_odd_number_of_voters(voters, voter):
    chosen, reason = next_add_core.choose_voter(voters)
    assert chosen is voter
    assert (voters + (1 if chosen else 0)) % 2 == 1
    assert str(voters) in reason


def test_voter_choice_needs_a_cluster():
    with pytest.raises(ToolError):
        next_add_core.choose_voter(0)


def test_name_picking_takes_the_next_number_after_every_known_core():
    used = next_add_core.core_numbers(["n1", "n2", "fmnext-core-3", "r1",
                                       "fmnext-run-7", "a2", "bogus"])
    assert used == {1, 2, 3}
    assert next_add_core.pick_number(used, 10) == 4
    assert next_add_core.pick_number({1, 3}, 10) == 4
    assert next_add_core.pick_number(set(), 10) == 1
    with pytest.raises(GuardError):
        next_add_core.pick_number({10}, 10)


def test_default_placement_follows_the_existing_cores():
    instances = seed_config()
    assert next_add_core.default_placement(instances) == ("ewr", "vc2-1c-1gb")
    instances["fmnext-core-2"]["region"] = "sjc"
    assert next_add_core.default_placement(instances)[0] == "ewr"
    assert next_add_core.default_placement({}) == ("", "")


def test_eligible_plans_filter_and_sort():
    plans = next_add_core.eligible_plans([
        {"id": "vc2-1c-0.5gb", "ram": 512, "monthly_cost": 2.5, "locations": ["ewr"]},
        {"id": "vc2-1c-1gb", "ram": 1024, "monthly_cost": 5, "vcpu_count": 1,
         "locations": ["EWR", "sjc"]},
        {"id": "vc2-1c-1gb-v6", "ram": 1024, "monthly_cost": 4, "locations": ["ewr"]},
        {"id": "vc2-2c-4gb", "ram": 4096, "monthly_cost": 20, "locations": ["ewr"]},
        {"id": "free", "ram": 4096, "monthly_cost": 0, "locations": ["ewr"]},
    ])
    assert [p["id"] for p in plans] == ["vc2-1c-1gb", "vc2-2c-4gb"]
    assert plans[0]["locations"] == ["ewr", "sjc"] and plans[0]["vcpu_count"] == 1


def test_fleet_toml_insert_keeps_the_rest_of_the_file(tmp_path):
    text = fleet_text()
    block = n4_block()
    updated = next_fleet.upsert_core_block(text, "n4", block)
    assert updated.replace(block + "\n\n", "", 1) == text
    assert updated.index('name = "n3"') < updated.index('name = "n4"') \
        < updated.index("[[runners]]")
    assert next_fleet.upsert_core_block(updated, "n4", block) == updated
    keyed = next_fleet.upsert_core_block(updated, "n4", n4_block(pubkey="K" * 43 + "="))
    assert keyed.count('name = "n4"') == 1 and 'wg_pubkey = "%s="' % ("K" * 43) in keyed
    path = tmp_path / "fleet.toml"
    path.write_text(updated)
    import render
    loaded = render.load_fleet(path, state_path=tmp_path / "none.json")
    n4 = render.node_by_name(loaded, "n4")
    assert n4["rqlite_voter"] is False and n4["wg_ip"] == "10.77.0.4"
    assert n4["public_ipv6"] == N4_V6


def test_fleet_toml_regeneration_keeps_non_voters():
    instances = {"fmnext-core-%d" % n: {"id": "i%d" % n, "ip": "203.0.113.%d" % (10 + n)}
                 for n in range(1, 5)}
    text = next_fleet.fleet_toml_text(instances, {"rqlite_nonvoters": ["n4"]},
                                      example=EXAMPLE.read_text())
    blocks = text.split("[[nodes]]")[1:]
    assert ["rqlite_voter = false" in b for b in blocks] == [False, False, False, True]


def test_render_non_voter_leaves_the_existing_voters_units_alone(tmp_path):
    import render
    before = render.load_fleet(write_fleet(tmp_path), state_path=tmp_path / "none.json")
    after_path = tmp_path / "after.toml"
    after_path.write_text(next_fleet.upsert_core_block(fleet_text(), "n4", n4_block()))
    after = render.load_fleet(after_path, state_path=tmp_path / "none.json")
    unit = "/etc/systemd/system/rqlite.service"
    for name in ("n1", "n2", "n3"):
        assert render.render_core(before, name)[0][unit] == \
            render.render_core(after, name)[0][unit]
    n4 = render.render_core(after, "n4")[0][unit][0]
    assert "-raft-non-voter" in n4 and "-bootstrap-expect" not in n4
    assert "-join 10.77.0.1:4002,10.77.0.2:4002,10.77.0.3:4002 " in n4
    voter_path = tmp_path / "voter.toml"
    voter_path.write_text(next_fleet.upsert_core_block(fleet_text(), "n4",
                                                       n4_block(voter=True)))
    voter = render.load_fleet(voter_path, state_path=tmp_path / "none.json")
    n4 = render.render_core(voter, "n4", replacement=True)[0][unit][0]
    assert "-raft-non-voter" not in n4 and "-bootstrap-expect" not in n4
    assert "-bootstrap-expect 4" in render.render_core(voter, "n1")[0][unit][0]


def test_render_refuses_a_fleet_without_voters(tmp_path):
    import render
    text = fleet_text().replace('rqlite_id = "n1"', 'rqlite_id = "n1"\nrqlite_voter = "no"')
    path = tmp_path / "bad.toml"
    path.write_text(text)
    with pytest.raises(render.RenderError):
        render.load_fleet(path, state_path=tmp_path / "none.json")


@pytest.mark.parametrize("step", next_add_core.STEP_IDS)
def test_resume_after_a_failure_at_each_step(tmp_path, step):
    job, out = make_job(tmp_path)
    backend = FakeBackend(fail_at=step)
    with pytest.raises(ToolError) as error:
        next_add_core.run(job, backend)
    index = next_add_core.STEP_IDS.index(step)
    assert "resume from %s" % step in str(error.value)
    assert backend.calls == list(next_add_core.STEP_IDS[:index + 1])
    assert backend.contained == 1 and backend.in_dns is False
    saved = next_add_core.State.load(job.state.path)
    assert saved.get("failed")["step"] == step
    assert [saved.status(s) for s in next_add_core.STEP_IDS[:index]] == [
        "pending" if s in next_add_core.RECHECK else "done"
        for s in next_add_core.STEP_IDS[:index]]
    assert saved.status(step) == "failed" and not saved.get("finished")
    result = [e for e in events(out) if e["event"] == "result"][-1]
    assert result["status"] == "failed" and result["step"] == step
    assert "n4 held out of DNS" in result["notes"]
    backend.calls = []
    job2, out2 = make_job(tmp_path)
    job2.state = next_add_core.State.load(job.state.path)
    next_add_core.run(job2, backend)
    rechecked = [s for s in next_add_core.STEP_IDS[:index] if s in next_add_core.RECHECK]
    assert backend.calls == rechecked + list(next_add_core.STEP_IDS[index:])
    if step in ("register", "commit"):
        assert rechecked == ["health", "dns"]
    resumed = [e["step"] for e in events(out2) if e.get("resumed")]
    assert resumed == [s for s in next_add_core.STEP_IDS[:index] if s not in rechecked]
    final = next_add_core.State.load(job.state.path)
    assert final.get("finished") and not final.get("failed")
    assert backend.in_dns


def test_steps_run_in_order_and_dns_only_after_health(tmp_path):
    job, out = make_job(tmp_path)
    backend = FakeBackend()
    next_add_core.run(job, backend)
    assert backend.calls == list(next_add_core.STEP_IDS)
    order = backend.calls
    for earlier in ("vm", "keys", "mesh", "join", "services", "activate", "repos",
                    "ids", "health"):
        assert order.index(earlier) < order.index("dns")
    assert order.index("dns") < order.index("commit")
    statuses = [(e["step"], e["status"]) for e in events(out) if e["event"] == "step"]
    assert statuses[:2] == [("plan", "running"), ("plan", "done")]
    result = events(out)[-1]
    assert result == dict(result, event="result", status="ok",
                          summary="n4 is serving, in DNS")


def test_a_failed_health_check_never_reaches_dns(tmp_path):
    job, _ = make_job(tmp_path)
    backend = FakeBackend(fail_at="health", fail_times=99)
    for _ in range(3):
        with pytest.raises(ToolError):
            next_add_core.run(job, backend)
        job.state = next_add_core.State.load(job.state.path)
    assert "dns" not in backend.calls and not backend.in_dns
    assert backend.calls.count("health") == 3 and backend.calls.count("vm") == 1


def test_parallel_runs_branches_together_and_reports_the_first_failure(tmp_path):
    job, out = make_job(tmp_path)
    barrier = threading.Barrier(2, timeout=5)
    seen = []

    def branch(label, fail=False):
        def action():
            barrier.wait()
            seen.append(label)
            if fail:
                raise ToolError("broken")
            return label
        return action
    assert next_add_core.parallel(job, [("a", branch("a")), ("b", branch("b"))]) \
        == ["a", "b"]
    with pytest.raises(ToolError) as error:
        next_add_core.parallel(job, [("a", branch("a")), ("b", branch("b", True))])
    assert str(error.value) == "b: broken"
    assert sorted(seen) == ["a", "a", "b", "b"]
    dry, _ = make_job(tmp_path, execute=False)
    order = []
    next_add_core.parallel(dry, [("a", lambda: order.append("a")),
                                 ("b", lambda: order.append("b"))])
    assert order == ["a", "b"]


def test_mesh_rolls_peers_one_at_a_time_while_the_new_core_installs(tmp_path, monkeypatch):
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    barrier = threading.Barrier(2, timeout=5)
    log = []
    lock = threading.Lock()

    def roll(fleet, names, ssh, runner, steps=(), vultr=None, fleet_path=None,
             sleep=None, clock=None):
        if names == ["n1"]:
            barrier.wait()
        with lock:
            log.append(("roll", tuple(names), tuple(steps), str(fleet_path)))
        return names

    def install(fleet, name, ssh, runner, steps, vultr=None, replacement=False,
                state_path=None):
        barrier.wait()
        with lock:
            log.append(("install", name, tuple(steps), replacement))
    monkeypatch.setattr(next_fleet, "roll", roll)
    monkeypatch.setattr(next_fleet, "install", install)
    backend = next_add_core.FleetBackend(ssh=FakeSsh())
    monkeypatch.setattr(backend, "mail_enabled", lambda: False)
    backend.step_mesh(job)
    rolls = [entry for entry in log if entry[0] == "roll"]
    assert [r[1] for r in rolls] == [("n1",), ("n2",), ("n3",)]
    assert all(r[2] == ("files", "start") and r[3] == str(fleet_path) for r in rolls)
    assert ("install", "n4", ("base", "rqlite", "files", "caddy", "secrets"), False) in log
    assert job.state.items("peers_done") == ["n1", "n2", "n3"]
    log.clear()
    job.state.data["peers_done"] = ["n1", "n2"]
    monkeypatch.setattr(next_fleet, "install", lambda *a, **k: log.append(("install",)))
    monkeypatch.setattr(next_fleet, "roll", lambda fleet, names, *a, **k: log.append(
        ("roll", tuple(names))))
    backend.step_mesh(job)
    assert ("roll", ("n3",)) in log and ("roll", ("n1",)) not in log


def test_services_prefetch_repos_while_the_release_uploads(tmp_path, monkeypatch):
    import next_deploy
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    barrier = threading.Barrier(2, timeout=5)
    calls = []

    def install(fleet, name, ssh, runner, steps, vultr=None, replacement=False,
                state_path=None):
        barrier.wait()
        calls.append(("install", name, tuple(steps)))

    def upload(fleet, rev, tarball, ssh, runner, names=None):
        barrier.wait()
        calls.append(("upload", rev, list(names)))
    monkeypatch.setattr(next_fleet, "install", install)
    monkeypatch.setattr(next_deploy, "upload", upload)
    monkeypatch.setenv("FMNEXT_DIST_DIR", str(tmp_path / "dist"))
    (tmp_path / "dist").mkdir()
    (tmp_path / "dist" / ("fmnext-%s.tar.gz" % REV)).write_bytes(b"release")
    ssh = FakeSsh()
    next_add_core.FleetBackend(ssh=ssh).step_services(job)
    assert ("install", "n4", ("daemon", "calls", "files", "start")) in calls
    assert ("upload", REV, ["n4"]) in calls
    assert any(s == next_add_core.DAEMON_SYNC for _, s, _ in ssh.runs)


def vultr_fake(created_region="ewr"):
    live = {}

    def create(body):
        live.update(body, id=N4_ID, main_ip="0.0.0.0", status="pending",
                    v6_main_ip="::", region=created_region)
        return {"instance": dict(live)}

    def get(body):
        live.update(status="active", main_ip=N4_IP, v6_main_ip=N4_V6)
        return {"instance": dict(live)}
    http = FakeHttp({
        ("GET", "/instances?"): {"instances": [], "meta": {"links": {"next": ""}}},
        ("GET", "/ssh-keys"): {"ssh_keys": [{"id": "key-1", "name": "fmnext-fleet"}]},
        ("GET", "/os"): {"os": [{"id": 2625, "name": "Debian 13 x64 (trixie)"}]},
        ("POST", "/instances"): create,
        ("GET", "/instances/" + N4_ID): get,
    })
    return next_fleet.Vultr(http), http


def fleet_key(tmp_path, monkeypatch):
    key = tmp_path / "fleet_key"
    key.write_text("private")
    Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA fmnext-fleet\n")
    monkeypatch.setenv("FMNEXT_SSH_KEY", str(key))


def test_vm_creates_a_tagged_core_and_prefetches_meanwhile(tmp_path, monkeypatch):
    seed_config()
    fleet_key(tmp_path, monkeypatch)
    job, _ = make_job(tmp_path)
    vultr, http = vultr_fake()
    ssh = FakeSsh()
    backend = next_add_core.FleetBackend(vultr=vultr, ssh=ssh, sleep=lambda s: None)
    prefetched = []
    monkeypatch.setattr(backend, "prefetch", lambda job: prefetched.append(job.name))
    backend.step_vm(job)
    posts = [c for c in http.mutations() if c[0] == "POST"]
    assert len(posts) == 1
    body = posts[0][2]
    assert body["label"] == "fmnext-core-4" and body["tags"] == ["forkmesh-next"]
    assert body["region"] == "ewr" and body["plan"] == "vc2-1c-1gb"
    assert body["enable_ipv6"] is True and body["os_id"] == 2625
    assert prefetched == ["n4"]
    assert job.state.get("ip") == N4_IP and job.state.get("ipv6") == N4_V6
    recorded = common.load_json(config() / "fleet-instances.json")
    assert recorded["fmnext-core-4"]["ip"] == N4_IP
    assert ssh.runs and ssh.runs[-1][0] == N4_IP
    assert not any(c[0] == "DELETE" for c in http.calls)


def test_vm_in_the_wrong_region_is_refused(tmp_path, monkeypatch):
    seed_config()
    fleet_key(tmp_path, monkeypatch)
    job, _ = make_job(tmp_path)
    vultr, _ = vultr_fake(created_region="sjc")
    backend = next_add_core.FleetBackend(vultr=vultr, ssh=FakeSsh(), sleep=lambda s: None)
    monkeypatch.setattr(backend, "prefetch", lambda job: None)
    with pytest.raises(GuardError):
        backend.step_vm(job)


def served_zone():
    records = []
    for index, name in enumerate(PROD + NEXT):
        for node in ("n1", "n2", "n3"):
            records.append(a("%s-%s" % (index, node), name, V4[node]))
            records.append(aaaa("%s-%s6" % (index, node), name, V6[node]))
    return Zone(records)


def test_fleet_step_writes_the_block_and_only_the_new_nodes_own_name(tmp_path, monkeypatch):
    instances = seed_config()
    instances["fmnext-core-4"] = {"id": N4_ID, "ip": N4_IP, "ipv6": N4_V6,
                                  "plan": "vc2-1c-1gb", "region": "ewr"}
    common.save_json(config() / "fleet-instances.json", instances)
    fleet_path = write_fleet(tmp_path)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    zone = served_zone()
    backend = next_add_core.FleetBackend(cf=zone.cf, ssh=FakeSsh())
    monkeypatch.setattr(backend, "mail_enabled", lambda: False)
    backend.step_fleet(job)
    assert n4_block() in fleet_path.read_text()
    assert common.load_json(config() / "fleet-state.json")["rqlite_nonvoters"] == ["n4"]
    assert zone.ips("n4.next.forkmesh.com") == [N4_IP]
    assert zone.ips("n4.next.forkmesh.com", "AAAA") == [N4_V6]
    for name in PROD + NEXT:
        assert N4_IP not in zone.ips(name)
    changed = {c[2]["name"] for c in zone.http.mutations() if c[2]}
    assert changed == {"n4.next.forkmesh.com"}
    backend.step_fleet(job)
    assert fleet_path.read_text().count('name = "n4"') == 1


def nodes_reply(n4_voter=False, n4_reachable=True):
    nodes = {n: {"voter": True, "reachable": True, "leader": n == "n1"}
             for n in ("n1", "n2", "n3")}
    nodes["n4"] = {"voter": n4_voter, "reachable": n4_reachable}
    return json.dumps({"nodes": nodes}).encode()


def test_join_waits_for_a_reachable_non_voter(tmp_path, monkeypatch):
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, out = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    polls = {"n": 0}

    def nodes(host, script, stdin):
        polls["n"] += 1
        return nodes_reply(n4_reachable=polls["n"] > 2)
    ssh = FakeSsh([(lambda h, s: s == next_add_core.RQLITE_READY, b"200"),
                   (lambda h, s: "/nodes?nonvoters" in s, nodes)])
    installs = []
    monkeypatch.setattr(next_fleet, "install", lambda fleet, name, ssh, runner, steps,
                        **kw: installs.append((name, steps, kw.get("replacement"))))
    backend = next_add_core.FleetBackend(ssh=ssh, sleep=lambda s: None)
    backend.step_join(job)
    assert installs == [("n4", ("start",), False)]
    assert polls["n"] == 3
    assert "joined rqlite as a non-voter" in out.getvalue()


def test_join_refuses_the_wrong_suffrage(tmp_path, monkeypatch):
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    ssh = FakeSsh([(lambda h, s: s == next_add_core.RQLITE_READY, b"200"),
                   (lambda h, s: "/nodes?nonvoters" in s, nodes_reply(n4_voter=True))])
    monkeypatch.setattr(next_fleet, "install", lambda *a, **k: None)
    with pytest.raises(GuardError):
        next_add_core.FleetBackend(ssh=ssh, sleep=lambda s: None).step_join(job)


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def test_repos_wait_until_every_peer_repository_is_synced(tmp_path, monkeypatch):
    order = []
    monkeypatch.setattr(next_add_core.FleetBackend, "register_account",
                        lambda self, job, fleet: order.append("register"))
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, out = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    peer = {"lastSyncOk": True, "repositories": {"forkmesh/forkmesh": "a",
                                                  "o/r": "b"}}
    calls = {"n": 0}

    def status(host, script, stdin):
        if host != N4_IP:
            return json.dumps(peer).encode()
        order.append("status")
        calls["n"] += 1
        repos = {"forkmesh/forkmesh": "a"} if calls["n"] < 20 else peer["repositories"]
        return json.dumps({"lastSyncOk": True, "repositories": repos}).encode()
    ssh = FakeSsh([(lambda h, s: s == next_add_core.DAEMON_STATUS, status)])
    clock = Clock()
    backend = next_add_core.FleetBackend(ssh=ssh, sleep=clock.sleep, clock=clock)
    backend.step_repos(job)
    nudges = [r for r in ssh.runs if r[1] == next_add_core.DAEMON_SYNC]
    assert nudges and all(r[0] == N4_IP for r in nudges)
    assert order[0] == "register" and order.count("register") == 1
    first_sync = [r[1] for r in ssh.runs].index(next_add_core.DAEMON_SYNC)
    assert all(r[0] != N4_IP or r[1] != next_add_core.DAEMON_STATUS
               for r in ssh.runs[:first_sync])
    assert "serves all 2 repositories" in out.getvalue()


def test_repos_time_out_with_the_missing_names(tmp_path, monkeypatch):
    monkeypatch.setattr(next_add_core.FleetBackend, "register_account",
                        lambda self, job, fleet: None)
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)

    def status(host, script, stdin):
        repos = {"o/one": "1", "o/two": "2"} if host != N4_IP else {"o/one": "1"}
        return json.dumps({"lastSyncOk": True, "repositories": repos}).encode()
    clock = Clock()
    backend = next_add_core.FleetBackend(ssh=FakeSsh([(
        lambda h, s: s == next_add_core.DAEMON_STATUS, status)]),
        sleep=clock.sleep, clock=clock)
    with pytest.raises(ToolError) as error:
        backend.step_repos(job)
    assert "o/two" in str(error.value)


def test_health_retries_then_passes_and_fails_closed(tmp_path, monkeypatch):
    import next_autodeploy
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    answers = iter([(False, "readyz/units: 503"), (True, "readyz 200, units active, "
                                                   "IDS normal")] + [(True, "ok")] * 999)
    monkeypatch.setattr(next_fleet, "roll_healthy", lambda ssh, ip: next(answers))
    seen = []

    def verify_once(hosts, rev):
        seen.append(([n["name"] for n in hosts["nodes"]], hosts["app_host"], rev))
        return []
    monkeypatch.setattr(next_autodeploy, "verify_once", verify_once)
    clock = Clock()
    backend = next_add_core.FleetBackend(ssh=FakeSsh(), sleep=clock.sleep, clock=clock)
    backend.step_health(job)
    assert seen == [(["n4"], "app.forkmesh.com", REV)]
    monkeypatch.setattr(next_autodeploy, "verify_once",
                        lambda hosts, rev: ["n4: /login HTTP 502"])
    with pytest.raises(ToolError) as error:
        backend.step_health(job)
    assert "/login HTTP 502" in str(error.value)


def test_dns_adds_the_new_core_only_after_its_probes_pass(tmp_path):
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    fleet = common.load_fleet(fleet_path)
    next_dns.hold_joining(fleet, Runner(execute=True, out=io.StringIO()), "n4")
    zone = served_zone()
    failing = next_add_core.FleetBackend(cf=zone.cf, sleep=lambda s: None,
                                         prober=lambda ip, host: ip != N4_IP)
    with pytest.raises(ToolError):
        failing.step_dns(job)
    for name in PROD + NEXT:
        assert N4_IP not in zone.ips(name)
    assert next_dns.node_entry("n4")["held"] is False
    assert next_dns.node_entry("n4")["in_dns"] is False
    probed = []
    passing = next_add_core.FleetBackend(cf=zone.cf, sleep=lambda s: None,
                                         prober=lambda ip, host: probed.append(ip) or True)
    passing.step_dns(job)
    assert probed.count(N4_IP) == next_dns.UNDRAIN_PROBES
    for name in PROD + NEXT:
        assert zone.ips(name) == sorted(list(V4.values()) + [N4_IP])
        assert N4_V6 in zone.ips(name, "AAAA")
    entry = next_dns.node_entry("n4")
    assert entry["in_dns"] and not entry["held"] and "joining" not in entry


def test_contain_drains_a_new_core_that_reached_dns(tmp_path):
    fleet_path = write_fleet(tmp_path, with_n4=True)
    job, _ = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP, instance_id=N4_ID)
    zone = served_zone()
    fleet = common.load_fleet(fleet_path)
    next_dns.hold_joining(fleet, Runner(execute=True, out=io.StringIO()), "n4")
    backend = next_add_core.FleetBackend(cf=zone.cf, sleep=lambda s: None,
                                         prober=lambda ip, host: True)
    backend.step_dns(job)
    assert N4_IP in zone.ips("app.forkmesh.com")
    notes = backend.contain(job)
    assert N4_IP not in zone.ips("app.forkmesh.com")
    assert zone.ips("app.forkmesh.com") == sorted(V4.values())
    entry = next_dns.node_entry("n4")
    assert entry["held"] and not entry["in_dns"]
    assert any("drained" in note for note in notes)
    assert any(N4_ID in note for note in notes)


def test_contain_before_dns_keeps_it_held(tmp_path):
    fleet_path = write_fleet(tmp_path)
    job, _ = make_job(tmp_path, fleet_path=fleet_path)
    backend = next_add_core.FleetBackend(cf=served_zone().cf)
    backend.contain(job)
    entry = next_dns.node_entry("n4")
    assert entry["held"] and not entry["in_dns"] and entry["joining"]


def test_health_loop_never_adds_a_joining_core(tmp_path):
    fleet_path = write_fleet(tmp_path)
    fleet = common.load_fleet(fleet_path)
    zone = served_zone()
    runner = Runner(execute=True, out=io.StringIO())
    path = config() / "dns-health.json"
    next_dns.health_round(fleet, zone.cf, runner, lambda ip, host: True, now=0, path=path)
    next_dns.hold_joining(fleet, runner, "n4")
    for now in range(30, 60 * 30, 30):
        next_dns.health_round(fleet, zone.cf, runner, lambda ip, host: True, now=now,
                              path=path)
    assert next_dns.node_entry("n4")["joining"]
    joined = common.load_fleet(write_fleet(tmp_path, with_n4=True))
    for now in range(1800, 3600, 30):
        next_dns.health_round(joined, zone.cf, runner, lambda ip, host: True, now=now,
                              path=path)
    assert N4_IP not in zone.ips("app.forkmesh.com")
    assert next_dns.node_entry("n4")["held"]


def test_a_core_new_to_the_health_state_starts_held(tmp_path):
    zone = served_zone()
    runner = Runner(execute=True, out=io.StringIO())
    path = config() / "dns-health.json"
    next_dns.health_round(common.load_fleet(write_fleet(tmp_path)), zone.cf, runner,
                          lambda ip, host: True, now=0, path=path)
    joined = common.load_fleet(write_fleet(tmp_path, with_n4=True))
    for now in range(30, 1800, 30):
        next_dns.health_round(joined, zone.cf, runner, lambda ip, host: True, now=now,
                              path=path)
    entry = next_dns.node_entry("n4")
    assert entry["held"] and not entry["in_dns"]
    assert N4_IP not in zone.ips("app.forkmesh.com")
    assert next_dns.ensure(joined, zone.cf, runner) is not None
    assert N4_IP not in zone.ips("app.forkmesh.com")
    assert zone.ips("n4.next.forkmesh.com") == [N4_IP]


def test_plan_picks_n4_from_the_fleet_and_the_existing_cores(tmp_path):
    seed_config()
    job, _ = make_job(tmp_path, execute=False)
    planned = next_add_core.FleetBackend().plan(job)
    assert planned["name"] == "n4" and planned["label"] == "fmnext-core-4"
    assert (planned["region"], planned["plan"]) == ("ewr", "vc2-1c-1gb")
    assert planned["voter"] is False and planned["voters_before"] == ["n1", "n2", "n3"]
    assert planned["rev"] == REV and planned["peers"] == ["n1", "n2", "n3"]
    other = next_add_core.FleetBackend().plan(job, region="SJC", plan="vc2-2c-4gb")
    assert (other["region"], other["plan"]) == ("sjc", "vc2-2c-4gb")
    vultr, _ = vultr_fake()
    vultr.fleet_instances = lambda: [{"label": "fmnext-core-6"}]
    assert next_add_core.FleetBackend(vultr=vultr).plan(job)["name"] == "n7"
    with pytest.raises(GuardError):
        next_add_core.FleetBackend().plan(job, name="n2")


def test_plan_execute_checks_live_voters_and_the_served_release(tmp_path):
    seed_config()
    job, _ = make_job(tmp_path, execute=True)
    state = json.dumps({"current": REV, "running": REV, "app": "active"}).encode()
    good = FakeSsh([(lambda h, s: "/nodes?nonvoters" in s, json.dumps({"nodes": {
        n: {"voter": True, "reachable": True} for n in ("n1", "n2", "n3")}}).encode()),
                    (lambda h, s: "BUILD_REV" in s, state)])
    assert next_add_core.FleetBackend(ssh=good).plan(job)["rev"] == REV
    two = FakeSsh([(lambda h, s: "/nodes?nonvoters" in s, json.dumps({"nodes": {
        n: {"voter": True, "reachable": True} for n in ("n1", "n2")}}).encode()),
                   (lambda h, s: "BUILD_REV" in s, state)])
    with pytest.raises(GuardError):
        next_add_core.FleetBackend(ssh=two).plan(job)
    mixed = iter([json.dumps({"current": REV, "running": REV}).encode(),
                  json.dumps({"current": "0123456789ab", "running": "0123456789ab"}
                             ).encode()] * 3)
    split = FakeSsh([(lambda h, s: "/nodes?nonvoters" in s, json.dumps({"nodes": {
        n: {"voter": True, "reachable": True} for n in ("n1", "n2", "n3")}}).encode()),
                     (lambda h, s: "BUILD_REV" in s, lambda h, s, i: next(mixed))])
    with pytest.raises(ToolError):
        next_add_core.FleetBackend(ssh=split).plan(job)


def git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True,
                          check=True).stdout.strip()


@pytest.fixture
def repo(tmp_path, monkeypatch):
    for key, value in (("GIT_AUTHOR_NAME", "t"), ("GIT_AUTHOR_EMAIL", "t@example.com"),
                       ("GIT_COMMITTER_NAME", "t"),
                       ("GIT_COMMITTER_EMAIL", "t@example.com")):
        monkeypatch.setenv(key, value)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(tmp_path / "gitconfig"))
    path = tmp_path / "repo"
    (path / "deploy").mkdir(parents=True)
    (path / "deploy" / "fleet.toml").write_text(fleet_text())
    (path / "README").write_text("x\n")
    git(path, "init", "-q", "-b", "main")
    git(path, "add", ".")
    git(path, "commit", "-qm", "base")
    return path


def test_commit_goes_to_a_branch_and_leaves_head_and_the_checkout_alone(tmp_path, repo):
    instances = seed_config()
    instances["fmnext-core-4"] = {"id": N4_ID, "ip": N4_IP, "ipv6": N4_V6}
    common.save_json(config() / "fleet-instances.json", instances)
    fleet_path = repo / "deploy" / "fleet.toml"
    fleet_path.write_text(next_fleet.upsert_core_block(fleet_text(), "n4", n4_block()))
    job, out = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP)
    head = git(repo, "rev-parse", "HEAD")
    backend = next_add_core.FleetBackend(repo=repo)
    backend.step_commit(job)
    commit = job.state.get("commit")
    assert git(repo, "rev-parse", "HEAD") == head
    assert git(repo, "rev-parse", "fleet/add-core-n4") == commit
    assert git(repo, "rev-parse", commit + "^") == head
    assert git(repo, "show", commit + ":deploy/fleet.toml") + "\n" == fleet_path.read_text()
    assert git(repo, "diff", "--name-only", head, commit) == "deploy/fleet.toml"
    assert git(repo, "status", "--porcelain") == "M deploy/fleet.toml"
    assert git(repo, "diff", "--cached", "--name-only") == ""
    assert "merge --autostash --no-ff fleet/add-core-n4" in out.getvalue()
    backend.step_commit(job)
    assert git(repo, "rev-parse", "fleet/add-core-n4") == commit
    plain = subprocess.run(["git", "merge", "--no-ff", "-m", "merge", "fleet/add-core-n4"],
                           cwd=repo, capture_output=True, text=True)
    assert plain.returncode != 0 and git(repo, "rev-parse", "HEAD") == head
    merged = subprocess.run(["git", "merge", "--autostash", "--no-ff", "-m", "merge",
                             "fleet/add-core-n4"], cwd=repo, capture_output=True, text=True)
    assert merged.returncode == 0, merged.stderr
    assert git(repo, "status", "--porcelain") == ""
    assert git(repo, "stash", "list") == ""
    assert git(repo, "rev-parse", "HEAD^2") == commit


def test_commit_refuses_other_branches(repo):
    with pytest.raises(GuardError):
        next_add_core.Git(repo).commit_file("main", "refs/heads/main",
                                            "deploy/fleet.toml", "x", "m")


def test_cli_dry_run_changes_nothing(tmp_path, capsys):
    seed_config()
    fleet_path = write_fleet(tmp_path)
    before = sorted(p.name for p in config().iterdir())
    text = fleet_path.read_text()
    ssh = FakeSsh()
    assert next_fleet.main(["--fleet", str(fleet_path), "add-core", "--dry-run"],
                           ssh=ssh) == 0
    out = capsys.readouterr().out
    assert "n4 (fmnext-core-4), region ewr, plan vc2-1c-1gb" in out
    assert "joins as a non-voter" in out and "in parallel" in out
    assert "estimated total" in out and "dry run: nothing was changed" in out
    assert next_add_core.EVENT not in out
    assert fleet_path.read_text() == text
    assert sorted(p.name for p in config().iterdir()) == before
    assert ssh.runs == [] and ssh.puts == []


def test_options_and_status_json(tmp_path, capsys):
    seed_config()
    fleet_path = write_fleet(tmp_path)
    vultr, _ = vultr_fake()
    vultr.plans = lambda: [{"id": "vc2-1c-1gb", "ram": 1024, "monthly_cost": 5,
                            "vcpu_count": 1, "locations": ["ewr", "sjc"]}]
    assert next_fleet.main(["--fleet", str(fleet_path), "add-core-options", "--json"],
                           vultr=vultr, ssh=FakeSsh()) == 0
    data = json.loads(capsys.readouterr().out)
    assert data["name"] == "n4" and data["region"] == "ewr" and data["plan"] == "vc2-1c-1gb"
    assert data["plans"][0]["monthly_cost"] == 5 and data["regions"] == ["ewr", "sjc"]
    assert data["voter"] is False and data["estimate_seconds"] > 0
    assert [s["id"] for s in data["steps"]] == list(next_add_core.STEP_IDS)
    assert next_fleet.main(["add-core-status", "--json"]) == 0
    assert json.loads(capsys.readouterr().out) is None
    job, _ = make_job(tmp_path, fleet_path=fleet_path)
    with pytest.raises(ToolError):
        next_add_core.run(job, FakeBackend(fail_at="mesh"))
    assert next_fleet.main(["add-core-status", "--json"]) == 0
    status = json.loads(capsys.readouterr().out)
    assert status["name"] == "n4" and status["failed"]["step"] == "mesh"
    assert status["steps"]["keys"] == "done" and status["steps"]["dns"] == "pending"
    assert next_fleet.main(["--fleet", str(fleet_path), "add-core-options", "--json"],
                           vultr=vultr, ssh=FakeSsh()) == 0
    assert json.loads(capsys.readouterr().out)["unfinished"]["name"] == "n4"


def test_execute_resumes_the_unfinished_run_and_refuses_other_placement(tmp_path, monkeypatch):
    seed_config()
    fleet_path = write_fleet(tmp_path)
    job, _ = make_job(tmp_path, fleet_path=fleet_path)
    job.state.save()
    backend = FakeBackend()
    monkeypatch.setattr(common.DeployLock, "acquire", lambda self, wait=0: self)
    monkeypatch.setattr(common.DeployLock, "release", lambda self: None)
    args = type("Args", (), {"execute": True, "dry_run": False, "fleet": str(fleet_path),
                             "region": "sjc", "plan": None, "snapshot": None,
                             "name": None, "events": True, "command": "add-core"})()
    with pytest.raises(ToolError):
        next_add_core.main_add_core(args, vultr=object(), ssh=FakeSsh(), backend=backend,
                                    out=io.StringIO())
    args.region = None
    assert next_add_core.main_add_core(args, vultr=object(), ssh=FakeSsh(),
                                       backend=backend, out=io.StringIO()) == 0
    assert backend.calls == list(next_add_core.STEP_IDS)
    args.name = "n4"
    with pytest.raises(ToolError) as error:
        next_add_core.main_add_core(args, vultr=object(), ssh=FakeSsh(), backend=backend,
                                    out=io.StringIO())
    assert "already added" in str(error.value)


REPLACE_STEPS = next_add_core.STEP_IDS[:-1] + ("remove", "commit")


def test_replace_removes_the_old_core_after_the_new_one_serves(tmp_path):
    job, out = make_job(tmp_path, replaces="n2")
    backend = FakeBackend()
    next_add_core.run(job, backend)
    assert tuple(backend.calls) == REPLACE_STEPS
    start = events(out)[0]
    assert start["replaces"] == "n2" and [s["id"] for s in start["steps"]] == list(REPLACE_STEPS)
    assert events(out)[-1]["summary"] == ("n4 is serving, in DNS; n2 is out of the fleet, "
                                          "its instance kept")


def test_a_failed_removal_keeps_the_new_core_serving_and_resumes(tmp_path):
    job, out = make_job(tmp_path, replaces="n2")
    backend = FakeBackend(fail_at="remove")
    with pytest.raises(ToolError):
        next_add_core.run(job, backend)
    assert backend.contained == 0 and backend.in_dns
    saved = next_add_core.State.load(job.state.path)
    assert saved.status("health") == "done" and saved.status("dns") == "done"
    result = [e for e in events(out) if e["event"] == "result"][-1]
    assert "n4 stays in DNS" in result["notes"][0]
    backend.calls = []
    job.state = saved
    next_add_core.run(job, backend)
    assert backend.calls == ["remove", "commit"]


def test_replace_target_must_be_another_core_in_the_fleet(tmp_path):
    fleet = common.load_fleet(write_fleet(tmp_path))
    assert next_add_core.check_replace(fleet, "n2", "n4") == "n2"
    for old, new in (("n9", "n4"), ("r1", "n4"), ("n2;x", "n4"), ("", "n4"), ("n2", "n2")):
        with pytest.raises(GuardError):
            next_add_core.check_replace(fleet, old, new)


def test_replace_commit_adds_the_new_core_and_retires_the_old_one(tmp_path, repo):
    instances = seed_config()
    instances["fmnext-core-4"] = {"id": N4_ID, "ip": N4_IP, "ipv6": N4_V6}
    common.save_json(config() / "fleet-instances.json", instances)
    fleet_path = repo / "deploy" / "fleet.toml"
    job, out = make_job(tmp_path, fleet_path=fleet_path, ip=N4_IP, replaces="n2",
                        promoted="n4")
    head = git(repo, "rev-parse", "HEAD")
    next_add_core.FleetBackend(repo=repo).step_commit(job)
    commit = job.state.get("commit")
    assert git(repo, "rev-parse", "HEAD") == head
    text = git(repo, "show", commit + ":deploy/fleet.toml") + "\n"
    assert 'name = "n2"' not in text and 'name = "n4"' in text
    assert 'retired_cores = ["n2"]' in text
    block = text[text.index('name = "n4"'):]
    assert "rqlite_voter = false" not in block.split("[[", 1)[0]
    assert git(repo, "log", "-1", "--format=%s", commit) == \
        "Replace core node n2 with n4 (fmnext-core-4) in deploy/fleet.toml"


def test_step_remove_runs_remove_core_and_records_the_promotion(tmp_path, monkeypatch):
    import next_remove_core as rc
    job, _ = make_job(tmp_path, replaces="n2")
    seen = {}

    def start_state(backend, fleet_file, name, destroy, execute):
        seen["start"] = (name, destroy, execute)
        return next_add_core.State(tmp_path / "remove-n2.json",
                                   {"name": "n2", "allowed": True, "steps": {}}), False

    def run(removal, backend):
        seen["replaced_by"] = removal.state.get("replaced_by")
        removal.state.set(promoted="n4", finished=True)
    monkeypatch.setattr(rc, "start_state", start_state)
    monkeypatch.setattr(rc, "run", run)
    next_add_core.FleetBackend(ssh=FakeSsh()).step_remove(job)
    assert seen == {"start": ("n2", False, True), "replaced_by": "n4"}
    assert job.state.get("promoted") == "n4"


def test_step_remove_refuses_when_remove_core_would(tmp_path, monkeypatch):
    import next_remove_core as rc
    job, _ = make_job(tmp_path, replaces="n2")
    monkeypatch.setattr(rc, "start_state", lambda *a: (next_add_core.State(
        tmp_path / "r.json", {"name": "n2", "allowed": False,
                              "blocker": "would leave 2 voters"}), False))
    monkeypatch.setattr(rc, "run", lambda *a: pytest.fail("ran a refused removal"))
    with pytest.raises(GuardError) as error:
        next_add_core.FleetBackend(ssh=FakeSsh()).step_remove(job)
    assert "2 voters" in str(error.value)


def test_options_and_resume_with_replace(tmp_path, capsys, monkeypatch):
    seed_config()
    fleet_path = write_fleet(tmp_path)
    vultr, _ = vultr_fake()
    vultr.plans = lambda: []
    assert next_fleet.main(["--fleet", str(fleet_path), "add-core-options", "--replace",
                            "n2", "--json"], vultr=vultr, ssh=FakeSsh()) == 0
    data = json.loads(capsys.readouterr().out)
    assert data["replaces"] == "n2" and "error" not in data
    assert [s["id"] for s in data["steps"]] == list(REPLACE_STEPS)
    assert next_fleet.main(["--fleet", str(fleet_path), "add-core-options", "--replace",
                            "n7", "--json"], vultr=vultr, ssh=FakeSsh()) == 0
    assert "not a core" in json.loads(capsys.readouterr().out)["error"]
    job, _ = make_job(tmp_path, fleet_path=fleet_path, replaces="n2")
    with pytest.raises(ToolError):
        next_add_core.run(job, FakeBackend(fail_at="mesh"))
    assert next_fleet.main(["add-core-status", "--json"]) == 0
    status = json.loads(capsys.readouterr().out)
    assert status["replaces"] == "n2" and status["steps"]["remove"] == "pending"
    assert [s["id"] for s in status["step_list"]] == list(REPLACE_STEPS)
    monkeypatch.setattr(common.DeployLock, "acquire", lambda self, wait=0: self)
    monkeypatch.setattr(common.DeployLock, "release", lambda self: None)
    args = type("Args", (), {"execute": True, "dry_run": False, "fleet": str(fleet_path),
                             "region": None, "plan": None, "snapshot": None,
                             "name": None, "replace": "n3", "events": True,
                             "command": "add-core"})()
    with pytest.raises(ToolError) as error:
        next_add_core.main_add_core(args, vultr=object(), ssh=FakeSsh(),
                                    backend=FakeBackend(), out=io.StringIO())
    assert "to replace n2" in str(error.value)
    args.replace = None
    backend = FakeBackend()
    assert next_add_core.main_add_core(args, vultr=object(), ssh=FakeSsh(),
                                       backend=backend, out=io.StringIO()) == 0
    assert backend.calls[-2:] == ["remove", "commit"]


def test_rqlite_ready_probe_sends_the_root_only_credentials():
    assert "-K /etc/forkmesh-next/rqlite.curlrc" in next_add_core.RQLITE_READY
    assert "/readyz" in next_add_core.RQLITE_READY

