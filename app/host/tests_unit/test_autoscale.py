from __future__ import annotations

import asyncio
import copy
import io
import json
import sqlite3
import sys
import types
import urllib.error
from pathlib import Path

import pytest

HOST = Path(__file__).resolve().parents[1]
if str(HOST) not in sys.path:
    sys.path.insert(0, str(HOST))

from forkmesh_host import autoscale_policy as policy  # noqa: E402
from forkmesh_host import autoscaler as scaler  # noqa: E402
from forkmesh_host import fleet_api  # noqa: E402
from forkmesh_host import ids_records  # noqa: E402
from forkmesh_host import vultr_guard  # noqa: E402

MIGRATION = HOST.parents[1] / "app" / "migrations" / "0211_fleet_autoscale.sql"
MINUTE = 60_000
T0 = 1_790_000_000_000
PRIVATE = ids_records.generate_private()
PUBLIC = ids_records.public_key(PRIVATE)
SNAPSHOT = "11111111-2222-3333-4444-555555555555"


def run(coroutine):
    return asyncio.run(coroutine)


def enabled(**app):
    settings = policy.validate({})
    settings["app"].update(enabled=True, dryRun=False, snapshotId=SNAPSHOT)
    settings["app"].update(app)
    settings["runner"].update(enabled=True, dryRun=False, snapshotId=SNAPSHOT)
    return policy.validate(settings)


# ---------------------------------------------------------------- settings


def test_defaults_are_off_and_dry_run():
    settings = policy.validate({})
    assert settings["killSwitch"] is False
    for pool in policy.POOLS:
        assert settings[pool]["enabled"] is False
        assert settings[pool]["dryRun"] is True
        assert settings[pool]["plan"] == "vc2-1c-1gb"
    assert settings["app"]["max"] == 2 and settings["runner"]["max"] == 2
    assert settings["app"]["monthlyCapCents"] + settings["runner"]["monthlyCapCents"] \
        == 2500


@pytest.mark.parametrize("change", [
    {"app": {"max": 5}}, {"app": {"min": 3, "max": 2}},
    {"app": {"cooldownMinutes": 4}}, {"runner": {"monthlyCapCents": 10001}},
    {"app": {"plan": "../x"}}, {"app": {"bogus": 1}}, {"bogus": True},
    {"app": {"scaleUp": {"p95Ms": 0, "loopLagMs": 0, "loadPerCpu": 0, "rps": 0}}},
    {"runner": {"snapshotId": "not-a-snapshot"}}, {"killSwitch": "yes"},
    {"app": {"enabled": 1}},
])
def test_hard_limits_are_enforced(change):
    with pytest.raises(policy.PolicyError):
        policy.validate(change)


def test_settings_signature_round_trip_and_tamper():
    document = policy.settings_document(enabled(), PRIVATE, T0)
    assert policy.verify_settings(document, PUBLIC)["app"]["enabled"] is True
    tampered = copy.deepcopy(document)
    tampered["settings"]["app"]["max"] = 4
    with pytest.raises(policy.PolicyError):
        policy.verify_settings(tampered, PUBLIC)
    other = ids_records.public_key(ids_records.generate_private())
    with pytest.raises(policy.PolicyError):
        policy.verify_settings(document, other)


# ---------------------------------------------------------------- decisions


def fresh_state():
    return policy.empty_pool_state()


def test_app_scales_up_only_after_a_sustained_breach():
    conf = enabled()["app"]
    state = fresh_state()
    hot = {"p95Ms": 900.0, "loopLagMs": 10.0, "loadPerCpu": 0.2, "rps": 3.0}
    assert policy.decide_app(conf, state, hot, 0, 0.7, T0)["action"] == "none"
    assert policy.decide_app(conf, state, hot, 0, 0.7, T0 + 4 * MINUTE)["action"] == "none"
    decision = policy.decide_app(conf, state, hot, 0, 0.7, T0 + 5 * MINUTE)
    assert decision["action"] == "up" and "p95Ms" in decision["reason"]


def test_app_holds_without_metrics_and_at_max():
    conf = enabled()["app"]
    state = fresh_state()
    decision = policy.decide_app(conf, state, {}, 0, 0.7, T0)
    assert decision["action"] == "none" and "no metrics" in decision["reason"]
    hot = {"p95Ms": 900.0}
    policy.decide_app(conf, state, hot, 2, 0.7, T0)
    decision = policy.decide_app(conf, state, hot, 2, 0.7, T0 + 10 * MINUTE)
    assert decision["action"] == "none" and "maximum" in decision["reason"]


def test_app_cooldown_and_daily_rate_limit():
    conf = enabled()["app"]
    state = fresh_state()
    state["lastActionMs"] = T0
    hot = {"p95Ms": 900.0}
    policy.decide_app(conf, state, hot, 0, 0.7, T0)
    decision = policy.decide_app(conf, state, hot, 0, 0.7, T0 + 6 * MINUTE)
    assert decision["action"] == "none" and "cooldown" in decision["reason"]
    assert policy.decide_app(conf, state, hot, 0, 0.7, T0 + 11 * MINUTE)["action"] == "up"
    state["actions"] = [T0 + i for i in range(policy.MAX_ACTIONS_PER_DAY)]
    decision = policy.decide_app(conf, state, hot, 0, 0.7, T0 + 12 * MINUTE)
    assert decision["action"] == "none" and "24 h" in decision["reason"]


def test_app_spend_cap_blocks_growth_and_drains_the_pool():
    conf = enabled(monthlyCapCents=100)["app"]
    state = fresh_state()
    state["spentCents"] = 99.9
    state["month"] = policy.month_of(T0)
    hot = {"p95Ms": 900.0}
    policy.decide_app(conf, state, hot, 1, 0.7, T0)
    decision = policy.decide_app(conf, state, hot, 1, 0.7, T0 + 10 * MINUTE)
    assert decision["action"] == "none" and "spend cap" in decision["reason"]
    state["spentCents"] = 100.0
    decision = policy.decide_app(conf, state, hot, 1, 0.7, T0 + 11 * MINUTE)
    assert decision["action"] == "down" and "cap" in decision["reason"]


def test_app_scales_down_after_calm_and_never_below_min():
    conf = enabled(min=1)["app"]
    state = fresh_state()
    calm = {"p95Ms": 20.0, "loopLagMs": 1.0, "loadPerCpu": 0.1}
    policy.decide_app(conf, state, calm, 2, 0.7, T0)
    assert policy.decide_app(conf, state, calm, 2, 0.7, T0 + 29 * MINUTE)["action"] == "none"
    assert policy.decide_app(conf, state, calm, 2, 0.7, T0 + 30 * MINUTE)["action"] == "down"
    state = fresh_state()
    policy.decide_app(conf, state, calm, 1, 0.7, T0)
    assert policy.decide_app(conf, state, calm, 1, 0.7, T0 + 60 * MINUTE)["action"] == "none"
    assert policy.decide_app(conf, fresh_state(), {}, 0, 0.7, T0)["action"] == "up"


def test_runner_pool_never_scales_up_automatically_without_burst_credentials():
    conf = enabled()["runner"]
    state = fresh_state()
    queue = {"queued": 5, "oldestWaitS": 600, "alwaysOnFree": False, "busy": {}}
    decision = policy.decide_runner(conf, state, queue, [], 0.7, T0)
    assert decision["action"] == "none" and "credentials" in decision["reason"]


def test_runner_idle_node_is_removed_after_the_idle_window():
    conf = enabled()["runner"]
    state = fresh_state()
    nodes = [{"name": "b1", "state": "serving"}]
    quiet = {"queued": 0, "busy": {"b1": False}}
    assert policy.decide_runner(conf, state, quiet, nodes, 0.7, T0)["action"] == "none"
    decision = policy.decide_runner(conf, state, quiet, nodes, 0.7, T0 + 10 * MINUTE)
    assert decision["action"] == "down" and decision["node"] == "b1"
    busy = {"queued": 0, "busy": {"b1": True}}
    state = fresh_state()
    policy.decide_runner(conf, state, busy, nodes, 0.7, T0)
    assert policy.decide_runner(conf, state, busy, nodes, 0.7,
                                T0 + 60 * MINUTE)["action"] == "none"


def test_spend_accrues_per_node_hour_and_resets_each_month():
    state = fresh_state()
    nodes = [{"createdMs": T0, "hourlyCents": 0.7}]
    policy.accrue(state, nodes, T0)
    policy.accrue(state, nodes, T0 + 10 * 3_600_000)
    assert state["spentCents"] == pytest.approx(7.0)
    later = T0 + 40 * 86_400_000
    policy.accrue(state, nodes, later)
    assert state["month"] == policy.month_of(later)
    assert state["spentCents"] < 7.0 * 24 * 31


def test_signal_aggregation():
    signals = policy.aggregate_app_signals([
        {"p95Ms": 100, "loopLagMs": 5, "loadPerCpu": 0.5, "rps": 2},
        {"p95Ms": 300, "loopLagMs": 1, "loadPerCpu": 0.1, "rps": 4}, None])
    assert signals == {"p95Ms": 300.0, "loopLagMs": 5.0, "loadPerCpu": 0.3,
                       "rps": 3.0, "nodesReporting": 2}


# ---------------------------------------------------------------- Vultr guard


class FakeVultr:
    def __init__(self):
        self.instances = {}
        self.calls = []

    def __call__(self, request, timeout=None):
        method, url = request.get_method(), request.full_url
        path = url.split("/v2", 1)[1]
        self.calls.append((method, path))
        body = json.loads(request.data) if request.data else None
        if method == "GET" and path == "/instances" or path.startswith("/instances?"):
            raise AssertionError("the guard must never list instances")
        if method == "POST" and path == "/instances":
            instance_id = "aaaaaaaa-bbbb-cccc-dddd-%012d" % len(self.instances)
            instance = {"id": instance_id, "label": body["label"],
                        "tags": body["tags"], "main_ip": "45.76.0.%d" % len(self.instances)}
            self.instances[instance_id] = instance
            return Reply(202, {"instance": instance})
        if method == "GET" and path.startswith("/instances/"):
            instance = self.instances.get(path.rsplit("/", 1)[1])
            if instance is None:
                raise urllib.error.HTTPError(url, 404, "nf", {}, io.BytesIO(b"{}"))
            return Reply(200, {"instance": instance})
        if method == "DELETE" and path.startswith("/instances/"):
            self.instances.pop(path.rsplit("/", 1)[1], None)
            return Reply(204, None)
        if method == "GET" and path.startswith("/plans"):
            return Reply(200, {"plans": [{"id": "vc2-1c-1gb", "monthly_cost": 5}]})
        raise AssertionError("unexpected Vultr call %s %s" % (method, path))


class Reply:
    def __init__(self, status, body):
        self.status = status
        self._body = b"" if body is None else json.dumps(body).encode()

    def read(self, limit=None):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def record(name="a1", **extra):
    base = {"name": name, "label": vultr_guard.label_for_name(name),
            "created_by": "autoscaler", "instance_id": ""}
    base.update(extra)
    return base


@pytest.mark.parametrize("label", ["fmnext-run-1", "fmnext-core-1", "fmnext-auto-0",
                                   "fmnext-auto-10", "fmnext-app-1", "tradeskie-production",
                                   "Blt", "fmnext-snap-1", "fmnext-burst-1x"])
def test_guard_refuses_every_label_outside_the_allowlist(label):
    fake = FakeVultr()
    guard = vultr_guard.VultrGuard("key", opener=fake)
    with pytest.raises(vultr_guard.GuardRefused):
        guard.create({"name": "a1", "label": label, "created_by": "autoscaler"},
                      "vc2-1c-1gb", "ewr", SNAPSHOT, "#!/bin/sh\n")
    assert fake.calls == []


def test_guard_creates_only_from_a_snapshot_with_the_tag():
    fake = FakeVultr()
    guard = vultr_guard.VultrGuard("key", opener=fake)
    with pytest.raises(vultr_guard.GuardRefused):
        guard.create(record(), "vc2-1c-1gb", "ewr", "", "x")
    with pytest.raises(vultr_guard.GuardRefused):
        guard.create(record(created_by="someone"), "vc2-1c-1gb", "ewr", SNAPSHOT, "x")
    with pytest.raises(vultr_guard.GuardRefused):
        guard.create(record(name="a2", label="fmnext-auto-1"), "vc2-1c-1gb", "ewr",
                     SNAPSHOT, "x")
    instance = guard.create(record(), "vc2-1c-1gb", "ewr", SNAPSHOT, "x")
    assert instance["label"] == "fmnext-auto-1"
    assert fake.instances[instance["id"]]["tags"] == ["forkmesh-next"]


def test_guard_destroys_only_recorded_and_reverified_instances():
    fake = FakeVultr()
    guard = vultr_guard.VultrGuard("key", opener=fake)
    instance = guard.create(record(), "vc2-1c-1gb", "ewr", SNAPSHOT, "x")
    mine = record(instance_id=instance["id"])
    fake.instances[instance["id"]]["label"] = "tradeskie-production"
    with pytest.raises(vultr_guard.GuardRefused):
        guard.destroy(mine)
    fake.instances[instance["id"]]["label"] = "fmnext-auto-1"
    fake.instances[instance["id"]]["tags"] = []
    with pytest.raises(vultr_guard.GuardRefused):
        guard.destroy(mine)
    fake.instances[instance["id"]]["tags"] = ["forkmesh-next"]
    with pytest.raises(vultr_guard.GuardRefused):
        guard.destroy(record(instance_id=instance["id"], created_by="desktop"))
    with pytest.raises(vultr_guard.GuardRefused):
        guard.destroy(record(name="a2", instance_id=instance["id"]))
    assert ("DELETE", "/instances/" + instance["id"]) not in fake.calls
    assert guard.destroy(mine) is True
    assert guard.destroy(mine) is False
    assert not any(m == "GET" and p.startswith("/instances?") for m, p in fake.calls)


def test_guard_never_touches_the_cores_or_r1_even_by_id():
    fake = FakeVultr()
    fake.instances["de4a830b-2880-47d1-a303-0ad08ba44746"] = {
        "id": "de4a830b-2880-47d1-a303-0ad08ba44746", "label": "fmnext-core-1",
        "tags": ["forkmesh-next"]}
    guard = vultr_guard.VultrGuard("key", opener=fake)
    for label in ("fmnext-core-1", "fmnext-run-1"):
        with pytest.raises(vultr_guard.GuardRefused):
            guard.destroy({"name": "a1", "label": label, "created_by": "autoscaler",
                           "instance_id": "de4a830b-2880-47d1-a303-0ad08ba44746"})
    with pytest.raises(vultr_guard.GuardRefused):
        guard.destroy(record(instance_id="de4a830b-2880-47d1-a303-0ad08ba44746"))
    assert "de4a830b-2880-47d1-a303-0ad08ba44746" in fake.instances


def test_guard_redacts_the_key_from_errors():
    def failing(request, timeout=None):
        raise urllib.error.HTTPError(request.full_url, 400, "bad", {},
                                     io.BytesIO(b"bad key SECRETKEY here"))
    guard = vultr_guard.VultrGuard("SECRETKEY", opener=failing)
    with pytest.raises(RuntimeError) as caught:
        guard.plan_hourly_cents("vc2-1c-1gb")
    assert "SECRETKEY" not in str(caught.value)


# ---------------------------------------------------------------- tokens


def test_join_tokens_are_bound_single_purpose_and_expire():
    token = scaler.mint_token("k" * 40, "a1", "fmnext-auto-1", T0 + MINUTE)
    assert scaler.check_token("k" * 40, token, T0)["n"] == "a1"
    assert scaler.check_token("other" * 8, token, T0) is None
    assert scaler.check_token("k" * 40, token, T0 + 2 * MINUTE) is None
    body, mac = token.split(".")
    assert scaler.check_token("k" * 40, body + "." + "0" * 64, T0) is None
    assert scaler.check_token("k" * 40, "garbage", T0) is None


# ---------------------------------------------------------------- cluster flow


class SqliteRqlite:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT);"
                              "CREATE TABLE security_incidents (id TEXT PRIMARY KEY, "
                              "node_id TEXT, created_ms INTEGER, severity TEXT, "
                              "detector TEXT, summary TEXT, detail TEXT);"
                              "CREATE TABLE runner_nodes (name TEXT PRIMARY KEY, "
                              "public_key TEXT, registered_by TEXT, scope TEXT, "
                              "allowed_owners TEXT, status TEXT, created_at INTEGER, "
                              "updated_at INTEGER, last_seen_at INTEGER DEFAULT 0);"
                              "CREATE TABLE runner_job_leases (runner TEXT, "
                              "lease_id TEXT, expires_at INTEGER);"
                              "CREATE TABLE org_agent_jobs (id INTEGER PRIMARY KEY, "
                              "status TEXT, target_node TEXT, created_at INTEGER);")
        self.db.executescript(MIGRATION.read_text())
        self.url = "http://rqlite"

    async def execute(self, statements, transaction=True):
        out = []
        for statement in statements:
            cursor = self.db.execute(statement[0], statement[1:])
            out.append({"rows_affected": cursor.rowcount})
        self.db.commit()
        return out

    async def query(self, sql, *params, level="none"):
        return [dict(r) for r in self.db.execute(sql, params).fetchall()]


class Membership:
    def __init__(self, rqlite, node_id="n1"):
        self.rqlite = rqlite
        self.node_id = node_id
        self.hosting = True
        self.replica = False
        self.active = ("n1", "n2", "n3")


def make_runtime(fake_vultr, rqlite, node="n1"):
    env = types.SimpleNamespace(DATA_KEY="d" * 64, VULTR_API_KEY="key",
                                FORKMESH_IDS_OPERATOR_PUBKEY=PUBLIC)
    runtime = types.SimpleNamespace(env=env, sealed=False)
    membership = Membership(rqlite, node)
    runtime.durable = types.SimpleNamespace(membership=membership)
    guard = lambda: vultr_guard.VultrGuard("key", opener=fake_vultr)  # noqa: E731
    return runtime, membership, guard


def build(fake_vultr, rqlite, node="n1"):
    runtime, membership, guard = make_runtime(fake_vultr, rqlite, node)
    clock = [T0]
    auto = scaler.Autoscaler(runtime, membership, clock=lambda: clock[0],
                             guard_factory=guard, app_host="app.forkmesh.com")
    removed = []

    async def rqlite_remove(name):
        removed.append(name)
        return True
    auto.rqlite_remove = rqlite_remove
    api = fleet_api.FleetApi(runtime, guard_factory=guard, clock=lambda: clock[0],
                             bundle_builder=lambda record, env: {
                                 "name": record["name"], "files": {},
                                 "release": {"rev": "abcdefabcdef"}})
    return auto, api, clock, removed


def save_settings(api, settings, at=T0):
    document = policy.settings_document(settings, PRIVATE, at)
    return run(api.handle("POST", "/api/fleet/autoscale/settings", {},
                          json.dumps(document).encode(), "127.0.0.1"))


def test_full_app_scale_up_join_and_scale_down():
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, removed = build(fake, rqlite)
    save_settings(api, enabled())
    run(auto.store.put_metrics("n1", {"p95Ms": 900.0}))
    run(auto.tick(T0))
    clock[0] = T0 + 6 * MINUTE
    run(auto.store.put_metrics("n1", {"p95Ms": 900.0}))
    report = run(auto.tick(clock[0]))
    assert report["app"]["action"] == "up"
    node = run(auto.store.node("a1"))
    assert node["state"] == "booting" and node["label"] == "fmnext-auto-1"
    instance = fake.instances[node["instance_id"]]
    assert instance["tags"] == ["forkmesh-next"]
    text = __import__("base64").b64decode(
        [c for c in fake.calls if c[0] == "POST"] and _last_body(fake) or "").decode()
    assert "fmnext-join" in text
    token = text.split("--token ")[1].split()[0]
    with pytest.raises(fleet_api.ApiError) as wrong_ip:
        run(api.join({"token": token, "idsPublicKey": "q" * 43,
                      "wgPublicKey": "8cB/cHmWnp+prJ0MsLNmhFmsCexBYyod2MMPdxwh1nE="},
                     "203.0.113.9"))
    assert wrong_ip.value.code == "source_mismatch"
    bundle = run(api.join({"token": token, "idsPublicKey": "q" * 43,
                           "wgPublicKey": "8cB/cHmWnp+prJ0MsLNmhFmsCexBYyod2MMPdxwh1nE="},
                          instance["main_ip"]))
    assert bundle["ok"] and bundle["name"] == "a1"
    with pytest.raises(fleet_api.ApiError):
        run(api.join({"token": token, "idsPublicKey": "q" * 43,
                      "wgPublicKey": "8cB/cHmWnp+prJ0MsLNmhFmsCexBYyod2MMPdxwh1nE="},
                     instance["main_ip"]))
    run(api.joined({"token": token}, instance["main_ip"]))
    run(auto.store.put_metrics("a1", {"p95Ms": 20.0}))
    clock[0] += MINUTE
    run(auto.tick(clock[0]))
    assert run(auto.store.node("a1"))["state"] == "serving"
    command = policy.signed_document(policy.COMMAND_KIND, {
        "ts": clock[0], "nonce": "ab" * 8, "pool": "app", "direction": "down",
        "node": "a1", "reason": "test"}, PRIVATE)
    run(api.handle("POST", "/api/fleet/autoscale/command", {},
                   json.dumps(command).encode(), "127.0.0.1"))
    with pytest.raises(fleet_api.ApiError):
        run(api.handle("POST", "/api/fleet/autoscale/command", {},
                       json.dumps(command).encode(), "127.0.0.1"))
    clock[0] += MINUTE
    run(auto.tick(clock[0]))
    assert run(auto.store.node("a1"))["state"] == "draining"
    clock[0] += scaler.DRAIN_MS
    run(auto.tick(clock[0]))
    clock[0] += MINUTE
    run(auto.tick(clock[0]))
    assert run(auto.store.node("a1")) is None
    assert removed == ["a1"]
    assert instance["id"] not in fake.instances
    pings = rqlite.db.execute("SELECT summary FROM security_incidents").fetchall()
    assert any("creating a1" in p[0] for p in pings)
    assert any("destroyed" in p[0] for p in pings)
    status = run(api.status())
    assert status["verified"] and status["pools"]["app"]["nodes"] == []
    assert status["pools"]["app"]["spentCents"] > 0


def _last_body(fake):
    return fake.last_user_data


def test_kill_switch_and_dry_run_take_no_action():
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, _ = build(fake, rqlite)
    settings = enabled(min=1)
    settings["killSwitch"] = True
    save_settings(api, settings)
    run(auto.tick(T0))
    assert fake.calls == [] and run(auto.store.nodes()) == []
    settings["killSwitch"] = False
    settings["app"]["dryRun"] = True
    save_settings(api, settings, T0 + 1)
    report = run(auto.tick(T0 + MINUTE))
    assert report["app"]["action"] == "up"
    assert fake.calls == [] and run(auto.store.nodes()) == []


def test_unsigned_or_replayed_settings_are_refused():
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, _ = build(fake, rqlite)
    save_settings(api, enabled(), T0)
    with pytest.raises(fleet_api.ApiError):
        save_settings(api, enabled(), T0)
    forged = policy.settings_document(enabled(min=2), ids_records.generate_private(),
                                      T0 + 5)
    with pytest.raises(fleet_api.ApiError):
        run(api.handle("POST", "/api/fleet/autoscale/settings", {},
                       json.dumps(forged).encode(), "127.0.0.1"))
    rqlite.db.execute("UPDATE autoscale_settings SET doc=replace(doc, "
                      "'\"max\": 2', '\"max\": 4')")
    settings, verified, error = run(auto.settings())
    assert not verified and "signature" in error
    run(auto.tick(T0 + MINUTE))
    assert fake.calls == []


def test_only_one_core_holds_the_lease():
    rqlite = SqliteRqlite()
    first = scaler.Store(rqlite)
    assert run(first.acquire_lease("n1", T0))
    assert not run(first.acquire_lease("n2", T0 + 1000))
    assert run(first.acquire_lease("n1", T0 + 2000))
    assert run(first.acquire_lease("n2", T0 + 2000 + scaler.LEASE_MS + 1))
    run(first.release_lease("n2"))
    assert run(first.acquire_lease("n3", T0 + 2000 + scaler.LEASE_MS + 2))


def test_boot_timeout_destroys_a_node_that_never_joined():
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, removed = build(fake, rqlite)
    save_settings(api, enabled(min=1))
    run(auto.tick(T0))
    node = run(auto.store.node("a1"))
    assert node["state"] == "booting"
    clock[0] = T0 + scaler.BOOT_TIMEOUT_MS + MINUTE
    run(auto.tick(clock[0]))
    clock[0] += MINUTE
    run(auto.tick(clock[0]))
    assert node["instance_id"] not in fake.instances
    assert run(auto.store.node("a1")) is None or run(
        auto.store.node("a1"))["state"] in ("creating", "booting")


def test_status_and_commands_need_a_fresh_operator_signature():
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, _ = build(fake, rqlite)
    stale = policy.signed_document(policy.STATUS_KIND, {"ts": T0 - 5 * MINUTE,
                                                        "nonce": "cd" * 8}, PRIVATE)
    with pytest.raises(fleet_api.ApiError):
        run(api.handle("POST", "/api/fleet/autoscale/status", {},
                       json.dumps(stale).encode(), "127.0.0.1"))
    forged = policy.signed_document(policy.STATUS_KIND, {"ts": T0, "nonce": "ef" * 8},
                                    ids_records.generate_private())
    with pytest.raises(fleet_api.ApiError):
        run(api.handle("POST", "/api/fleet/autoscale/status", {},
                       json.dumps(forged).encode(), "127.0.0.1"))
    good = policy.signed_document(policy.STATUS_KIND, {"ts": T0, "nonce": "0a" * 8},
                                  PRIVATE)
    status, body = run(api.handle("POST", "/api/fleet/autoscale/status", {},
                                  json.dumps(good).encode(), "127.0.0.1"))
    assert status == 200 and body["ok"] and not body["verified"]


def test_ids_node_keys_only_for_autoscaled_names():
    assert len(fleet_api.ids_node_key("s" * 64, "a1")) == 64
    for name in ("n1", "r1", "a0", "b10"):
        with pytest.raises(fleet_api.ApiError):
            fleet_api.ids_node_key("s" * 64, name)


@pytest.fixture(autouse=True)
def capture_user_data(monkeypatch):
    original = vultr_guard.VultrGuard.create

    def create(self, record, plan, region, snapshot_id, user_data):
        opener = self.opener
        if isinstance(opener, FakeVultr):
            opener.last_user_data = __import__("base64").b64encode(
                user_data.encode()).decode()
        return original(self, record, plan, region, snapshot_id, user_data)
    monkeypatch.setattr(vultr_guard.VultrGuard, "create", create)


def test_a_500_prints_an_error_line_and_the_traceback(capsys, monkeypatch):
    fake, rqlite = FakeVultr(), SqliteRqlite()
    auto, api, clock, _ = build(fake, rqlite)

    async def broken():
        raise KeyError("spentCents")
    monkeypatch.setattr(api, "status", broken)
    good = policy.signed_document(policy.STATUS_KIND, {"ts": T0, "nonce": "0b" * 8},
                                  PRIVATE)
    status, data, _ = run(fleet_api.dispatch(
        api, "POST", "/api/fleet/autoscale/status", {},
        json.dumps(good).encode(), "127.0.0.1"))
    assert status == 500 and json.loads(data) == {"ok": False, "error": "internal"}
    lines = capsys.readouterr().out.splitlines()
    assert lines[0] == ("ERROR fleet api: POST /api/fleet/autoscale/status -> 500: "
                        "KeyError('spentCents')")
    assert lines[1] == "Traceback (most recent call last):"
    assert any("in broken" in line for line in lines)
    assert lines[-1] == "KeyError: 'spentCents'"


def test_runner_targets_follow_the_rendered_fleet_not_a_fixed_list():
    from forkmesh_host import autoscaler
    assert autoscaler.default_runner_targets({
        "FORKMESH_VAR_RUNNER_POOL_TARGET_NODES": "fmnext-n2,fmnext-n4"}) == [
        "fmnext-n2", "fmnext-n4"]
    assert autoscaler.default_runner_targets({
        "FORKMESH_MESH_PEERS": "n4=http://10.77.0.4:8792,n2=http://10.77.0.2:8792"}) == [
        "fmnext-n2", "fmnext-n4"]
    assert autoscaler.default_runner_targets({}) == []
