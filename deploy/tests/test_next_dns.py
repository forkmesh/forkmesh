import json
"""tools/next_dns.py: name guard, dry-run default, health rules."""

from pathlib import Path

import pytest

import next_dns
from fakes import FakeHttp
from next_common import GuardError, Runner

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
ZONE = "0123456789abcdef0123456789abcdef"


def fleet(tmp_path):
    import render
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


@pytest.mark.parametrize("name", [
    "next.forkmesh.com", "app.next.forkmesh.com", "world.next.forkmesh.com",
    "n1.next.forkmesh.com", "APP.NEXT.FORKMESH.COM", "app.next.forkmesh.com.",
])
def test_guard_accepts_next_names(name):
    next_dns.guard_name(name)


@pytest.mark.parametrize("name", [
    "forkmesh.com", "www.forkmesh.com", "api.forkmesh.com", "xapp.forkmesh.com",
    "app.forkmesh.com.evil.example", "app.world.forkmesh.com", "*.forkmesh.com",
    "mirror35.forkmesh.com", "evilnext.forkmesh.com", "next.forkmesh.com.evil.example",
    "app-next.forkmesh.com", "xnext.forkmesh.com", "*.next.forkmesh.com",
    "a..next.forkmesh.com", "", "tradeskie-production.forkmesh.com",
])
def test_guard_refuses_everything_else(name):
    with pytest.raises(GuardError):
        next_dns.guard_name(name)


class Zone:
    """A fake Cloudflare zone holding A records."""

    def __init__(self, records):
        self.records = {r["id"]: dict(r) for r in records}
        self.counter = 0
        self.http = FakeHttp({
            ("GET", "/zones/%s/dns_records?" % ZONE): self.list,
            ("GET", "/zones/%s/dns_records/" % ZONE): self.get,
            ("POST", "/zones/%s/dns_records" % ZONE): self.create,
            ("PUT", "/zones/%s/dns_records/" % ZONE): self.put,
            ("DELETE", "/zones/%s/dns_records/" % ZONE): self.delete,
        })
        self.cf = next_dns.Cloudflare(self.http, ZONE)

    def _id(self):
        return self.http.calls[-1][1].rsplit("/", 1)[1]

    def list(self, body):
        query = self.http.calls[-1][1].split("?", 1)[1]
        params = dict(p.split("=") for p in query.split("&"))
        return {"result": [r for r in self.records.values()
                           if r["name"] == params["name"]
                           and r.get("type", "A") == params["type"]]}

    def get(self, body):
        return {"result": self.records[self._id()]}

    def create(self, body):
        self.counter += 1
        record = dict(body, id="new%d" % self.counter)
        self.records[record["id"]] = record
        return {"result": record}

    def put(self, body):
        self.records[self._id()].update(body)
        return {"result": self.records[self._id()]}

    def delete(self, body):
        self.records.pop(self._id())
        return {"result": {}}

    def ips(self, name, record_type="A"):
        return sorted(r["content"] for r in self.records.values()
                      if r["name"] == name and r.get("type", "A") == record_type)


def a(record_id, name, ip, ttl=60, proxied=False):
    return {"id": record_id, "type": "A", "name": name, "content": ip, "ttl": ttl,
            "proxied": proxied}


def test_ensure_is_dry_run_by_default(tmp_path):
    zone = Zone([])
    runner = Runner(execute=False)
    changes = next_dns.ensure(fleet(tmp_path), zone.cf, runner)
    assert zone.http.mutations() == []
    assert len(changes) == 3 + 3 + 3  # app, world (3 each) + n1..n3


def test_ensure_creates_updates_and_prunes_only_next_records(tmp_path):
    zone = Zone([
        a("p1", "app.forkmesh.com", "192.0.2.1"),       # production: untouched
        a("x1", "app.next.forkmesh.com", "192.0.2.99"),  # stale: deleted
        a("x2", "world.next.forkmesh.com", "203.0.113.11", ttl=300, proxied=True),
    ])
    next_dns.ensure(fleet(tmp_path), zone.cf, Runner(execute=True))
    assert zone.ips("app.next.forkmesh.com") == ["203.0.113.11", "203.0.113.12",
                                                 "203.0.113.13"]
    assert zone.ips("world.next.forkmesh.com") == ["203.0.113.11", "203.0.113.12",
                                                   "203.0.113.13"]
    assert zone.ips("n2.next.forkmesh.com") == ["203.0.113.12"]
    assert zone.records["x2"]["ttl"] == 60 and zone.records["x2"]["proxied"] is False
    assert zone.records["p1"] == a("p1", "app.forkmesh.com", "192.0.2.1")
    for method, path, body in zone.http.mutations():
        if body:
            next_dns.guard_name(body["name"])
            assert body["ttl"] == 60 and body["proxied"] is False


def test_delete_and_update_recheck_the_live_record_name():
    zone = Zone([a("p1", "www.forkmesh.com", "192.0.2.1")])
    with pytest.raises(GuardError):
        zone.cf.delete("p1")
    with pytest.raises(GuardError):
        zone.cf.update("p1", "app.next.forkmesh.com", "192.0.2.2")
    with pytest.raises(GuardError):
        zone.cf.create("forkmesh.com", "192.0.2.3")
    with pytest.raises(GuardError):
        zone.cf.records("forkmesh.com")
    assert zone.http.mutations() == []
    assert zone.records["p1"]["name"] == "www.forkmesh.com"


def test_sync_never_empties_a_name():
    zone = Zone([a("x1", "app.next.forkmesh.com", "203.0.113.11")])
    with pytest.raises(GuardError):
        next_dns.sync_name(zone.cf, Runner(execute=True), "app.next.forkmesh.com", [])
    assert zone.http.mutations() == []


# --------------------------------------------------------------------------
# health rules


def run_rounds(tmp_path, zone, health, rounds, start=0.0, step=30.0, execute=True):
    """health: {node: [ok, ok, ...]} per round."""
    data = fleet(tmp_path)
    state = tmp_path / "health.json"
    decisions = []
    for index in range(rounds):
        prober = lambda ip, host, i=index: health[
            {"203.0.113.11": "n1", "203.0.113.12": "n2", "203.0.113.13": "n3"}[ip]][i]
        decisions.append(next_dns.health_round(data, zone.cf, Runner(execute=execute),
                                               prober, now=start + index * step,
                                               path=state))
    return decisions


def full_zone():
    records = []
    for name in ("app.next.forkmesh.com", "world.next.forkmesh.com"):
        for n in (1, 2, 3):
            records.append(a("%s-%d" % (name[:3], n), name, "203.0.113.1%d" % n))
    return Zone(records)


def test_node_removed_after_three_failures_then_readded_after_two_minutes(tmp_path):
    zone = full_zone()
    n2 = [False, False, False] + [True] * 30
    decisions = run_rounds(tmp_path, zone, {"n1": [True] * 33, "n2": n2,
                                            "n3": [True] * 33}, rounds=33)
    assert decisions[2] == ("remove", "n2")
    assert decisions[0] is None and decisions[1] is None
    removed_at = decisions.index(("remove", "n2"))
    readded_at = decisions.index(("add", "n2"))
    # healthy from round 3 (t=90); 120 s healthy AND 300 s since the last change
    assert (readded_at - removed_at) * 30 >= 300
    assert zone.ips("app.next.forkmesh.com") == ["203.0.113.11", "203.0.113.12",
                                                 "203.0.113.13"]


def test_at_most_one_change_per_five_minutes(tmp_path):
    zone = full_zone()
    down = [False] * 20
    decisions = run_rounds(tmp_path, zone, {"n1": [True] * 20, "n2": down, "n3": down},
                           rounds=20)
    changes = [(i, d) for i, d in enumerate(decisions) if d]
    assert changes[0] == (2, ("remove", "n2"))
    assert changes[1][1] == ("remove", "n3") and (changes[1][0] - 2) * 30 >= 300


def test_never_removes_the_last_node(tmp_path):
    zone = full_zone()
    n1 = [True] * 12 + [False] * 28
    n2 = [False] * 12 + [True] * 28
    decisions = run_rounds(tmp_path, zone, {"n1": n1, "n2": n2, "n3": [False] * 40},
                           rounds=40)
    changes = [d for d in decisions if d]
    assert changes[:2] == [("remove", "n2"), ("remove", "n3")]
    assert changes.index(("add", "n2")) < changes.index(("remove", "n1"))
    assert zone.ips("app.next.forkmesh.com") == ["203.0.113.12"]
    assert zone.ips("world.next.forkmesh.com") == ["203.0.113.12"]


def test_every_node_failing_means_this_box_is_offline(tmp_path):
    zone = full_zone()
    down = [False] * 40
    decisions = run_rounds(tmp_path, zone, {"n1": down, "n2": down, "n3": down},
                           rounds=40)
    assert decisions == [None] * 40
    assert zone.http.mutations() == []
    state = json.loads((tmp_path / "health.json").read_text())
    assert all(e["fails"] == 0 and e["in_dns"] for e in state["nodes"].values())


def test_an_offline_round_does_not_count_toward_a_real_failure(tmp_path):
    zone = full_zone()
    others = [True, True, False, True]
    decisions = run_rounds(tmp_path, zone, {"n1": others, "n2": [False] * 4,
                                            "n3": others}, rounds=4)
    assert decisions == [None, None, None, ("remove", "n2")]


def test_health_dry_run_changes_nothing(tmp_path):
    zone = full_zone()
    down = [False] * 5
    run_rounds(tmp_path, zone, {"n1": [True] * 5, "n2": down, "n3": [True] * 5},
               rounds=5, execute=False)
    assert zone.http.mutations() == []


def test_probe_fails_closed_on_unreachable_or_untrusted(monkeypatch):
    # Loopback with nothing listening: a clean False, no exception.
    assert next_dns.probe("127.0.0.1", "app.next.forkmesh.com", timeout=0.5) is False


# --------------------------------------------------------------------------
# production names (app.forkmesh.com, world.forkmesh.com), A and AAAA


PROD = ("app.forkmesh.com", "world.forkmesh.com")
NEXT = ("app.next.forkmesh.com", "world.next.forkmesh.com")
V4 = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13"}
V6 = {"n1": "2001:19f0:ffff::11", "n2": "2001:19f0:ffff::12",
      "n3": "2001:19f0:ffff::13"}
BY_IP = {**{ip: n for n, ip in V4.items()}, **{ip: n for n, ip in V6.items()}}


def production_fleet(tmp_path, ipv6=True):
    import render
    text = FLEET.read_text("utf-8")
    text = text.replace("\n[dns]\n", '\n[production]\napp_host = "app.forkmesh.com"\n'
                        'world_host = "world.forkmesh.com"\n\n[dns]\n', 1)
    if ipv6:
        for name, ip in V4.items():
            text = text.replace('public_ipv4 = "%s"\n' % ip,
                                'public_ipv4 = "%s"\npublic_ipv6 = "%s"\n'
                                % (ip, V6[name]), 1)
    path = tmp_path / "fleet.toml"
    path.write_text(text, "utf-8")
    return render.load_fleet(path, state_path=tmp_path / "none.json")


def aaaa(record_id, name, ip, ttl=60, proxied=False):
    return dict(a(record_id, name, ip, ttl, proxied), type="AAAA")


def record(record_id, name, record_type, content):
    return {"id": record_id, "type": record_type, "name": name,
            "content": content, "ttl": 300, "proxied": False}


def production_zone(extra=(), with_aaaa=True):
    records = list(extra)
    for name in PROD + NEXT:
        for n in ("n1", "n2", "n3"):
            records.append(a("%s-%s-4" % (name, n), name, V4[n]))
            if with_aaaa:
                records.append(aaaa("%s-%s-6" % (name, n), name, V6[n]))
    return Zone(records)


@pytest.mark.parametrize("name", ["app.forkmesh.com", "WORLD.forkmesh.com.",
                                  "app.next.forkmesh.com"])
@pytest.mark.parametrize("record_type", ["A", "AAAA"])
def test_guard_allows_production_names_a_and_aaaa(name, record_type):
    next_dns.guard_name(name, record_type)


@pytest.mark.parametrize("record_type", ["CNAME", "TXT", "MX", "CAA", "NS", "", None])
@pytest.mark.parametrize("name", PROD + NEXT)
def test_guard_refuses_other_record_types(name, record_type):
    with pytest.raises(GuardError):
        next_dns.guard_name(name, record_type)


def test_live_record_of_another_type_is_never_changed():
    zone = Zone([record("t1", "app.forkmesh.com", "TXT", "v=spf1 -all"),
                 record("c1", "app.forkmesh.com", "CAA", "0 issue letsencrypt.org"),
                 record("w1", "www.forkmesh.com", "AAAA", "2001:19f0:ffff::99")])
    for record_id in ("t1", "c1", "w1"):
        with pytest.raises(GuardError):
            zone.cf.delete(record_id)
    with pytest.raises(GuardError):
        zone.cf.update("t1", "app.forkmesh.com", "203.0.113.11", "A")
    with pytest.raises(GuardError):
        zone.cf.update("w1", "app.forkmesh.com", "2001:19f0:ffff::11", "AAAA")
    assert zone.http.mutations() == []


def test_content_must_be_a_core_address(tmp_path):
    data = production_fleet(tmp_path)
    zone = Zone([])
    zone.cf.allowed = next_dns.core_addresses(data)
    with pytest.raises(GuardError):
        zone.cf.create("app.forkmesh.com", "192.0.2.50", "A")
    with pytest.raises(GuardError):
        zone.cf.create("app.forkmesh.com", "2001:19f0:ffff::99", "AAAA")
    with pytest.raises(GuardError):
        zone.cf.create("app.forkmesh.com", "fd00::1", "AAAA")
    with pytest.raises(GuardError):
        zone.cf.create("app.forkmesh.com", V6["n1"], "A")
    zone.cf.create("app.forkmesh.com", V6["n1"], "AAAA")
    assert zone.ips("app.forkmesh.com", "AAAA") == [V6["n1"]]


def test_ensure_manages_production_and_next_names_a_and_aaaa(tmp_path):
    data = production_fleet(tmp_path)
    keep = [a("w1", "www.forkmesh.com", "192.0.2.1"),
            record("t1", "app.forkmesh.com", "TXT", "keep"),
            a("x1", "app.forkmesh.com", "192.0.2.99")]
    zone = Zone(keep[:2] + [keep[2]])
    zone.cf.allowed = next_dns.core_addresses(data)
    next_dns.ensure(data, zone.cf, Runner(execute=True))
    for name in PROD + NEXT:
        assert zone.ips(name) == sorted(V4.values())
        assert zone.ips(name, "AAAA") == sorted(V6.values())
    assert zone.ips("n2.next.forkmesh.com", "AAAA") == [V6["n2"]]
    assert zone.records["w1"] == keep[0] and zone.records["t1"] == keep[1]
    assert "x1" not in zone.records
    for method, path, body in zone.http.mutations():
        if body:
            assert body["type"] in ("A", "AAAA")
            assert body["name"] in PROD + NEXT or body["name"].endswith(
                ".next.forkmesh.com")


def test_without_ipv6_aaaa_records_are_left_alone(tmp_path):
    data = production_fleet(tmp_path, ipv6=False)
    zone = production_zone()
    next_dns.ensure(data, zone.cf, Runner(execute=True))
    assert [m for m in zone.http.mutations()
            if m[0] != "POST" or m[2]["type"] != "A"] == []
    for name in PROD + NEXT:
        assert zone.ips(name, "AAAA") == sorted(V6.values())


def prod_rounds(tmp_path, data, zone, health, rounds, start=0.0, step=30.0,
                execute=True, v6_health=None):
    state = tmp_path / "health.json"
    decisions = []
    for index in range(rounds):
        def prober(ip, host, i=index):
            assert host == "app.forkmesh.com"
            return health[BY_IP[ip]][i]
        decisions.append(next_dns.health_round(data, zone.cf, Runner(execute=execute),
                                               prober, now=start + index * step,
                                               path=state))
    return decisions


def test_production_failover_removes_from_both_names_and_readds(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    up = [True] * 40
    n2 = [False, False, False] + [True] * 37
    decisions = prod_rounds(tmp_path, data, zone, {"n1": up, "n2": n2, "n3": up}, 40)
    assert decisions[:2] == [None, None] and decisions[2] == ("remove", "n2")
    removed_at = decisions.index(("remove", "n2"))
    readded_at = decisions.index(("add", "n2"))
    assert (readded_at - removed_at) * 30 >= 300
    for name in PROD + NEXT:
        assert zone.ips(name) == sorted(V4.values())
        assert zone.ips(name, "AAAA") == sorted(V6.values())


def test_production_removal_state_after_three_failures(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    up = [True] * 3
    prod_rounds(tmp_path, data, zone, {"n1": up, "n2": [False] * 3, "n3": up}, 3)
    for name in PROD:
        assert zone.ips(name) == [V4["n1"], V4["n3"]]
        assert zone.ips(name, "AAAA") == [V6["n1"], V6["n3"]]


def test_two_failures_change_nothing(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    flaky = [False, False, True, False, False, True]
    prod_rounds(tmp_path, data, zone, {"n1": [True] * 6, "n2": flaky,
                                       "n3": [True] * 6}, 6)
    assert zone.http.mutations() == []


def test_readd_needs_two_full_minutes_of_health(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    up = [True] * 60
    n2 = [False] * 3 + [True] * 8 + [False] + [True] * 48
    decisions = prod_rounds(tmp_path, data, zone, {"n1": up, "n2": n2, "n3": up}, 60)
    assert decisions[2] == ("remove", "n2")
    readded_at = decisions.index(("add", "n2"))
    assert readded_at * 30 - 12 * 30 >= 120 and readded_at < 20


def test_production_never_removes_the_last_record(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    down = [False] * 40
    decisions = prod_rounds(tmp_path, data, zone, {"n1": [True] * 12 + [False] * 28,
                                                   "n2": [False] * 12 + [True] * 28,
                                                   "n3": down}, 40)
    changes = [(i, d) for i, d in enumerate(decisions) if d]
    assert [d for _i, d in changes] == [("remove", "n2"), ("remove", "n3"),
                                        ("add", "n2"), ("remove", "n1")]
    assert all((b[0] - a[0]) * 30 >= 300 for a, b in zip(changes, changes[1:]))
    for name in PROD + NEXT:
        assert len(zone.ips(name)) == 1 and len(zone.ips(name, "AAAA")) == 1


def test_production_offline_box_changes_no_record(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    down = [False] * 40
    decisions = prod_rounds(tmp_path, data, zone, {"n1": down, "n2": down, "n3": down}, 40)
    assert decisions == [None] * 40 and zone.http.mutations() == []


def test_drain_is_immediate_and_held_until_undrain(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    state = tmp_path / "health.json"
    assert next_dns.hold(data, zone.cf, Runner(execute=False), "n1", True,
                         now=0, path=state) is None
    assert zone.http.mutations() == []
    assert next_dns.hold(data, zone.cf, Runner(execute=True), "n1", True,
                         now=0, path=state) == ("remove", "n1")
    for name in PROD:
        assert zone.ips(name) == [V4["n2"], V4["n3"]]
        assert zone.ips(name, "AAAA") == [V6["n2"], V6["n3"]]
    up = [True] * 30
    for index in range(30):
        next_dns.health_round(data, zone.cf, Runner(execute=True),
                              lambda ip, host: True, now=30 + index * 30, path=state)
    assert zone.ips("app.forkmesh.com") == [V4["n2"], V4["n3"]]
    next_dns.hold(data, zone.cf, Runner(execute=True), "n1", False, now=930, path=state,
                  prober=lambda ip, host: False, sleep=lambda s: None)
    assert zone.ips("app.forkmesh.com") == [V4["n2"], V4["n3"]]
    decision = next_dns.health_round(data, zone.cf, Runner(execute=True),
                                     lambda ip, host: True, now=960, path=state)
    assert decision == ("add", "n1") and up
    assert zone.ips("app.forkmesh.com", "AAAA") == sorted(V6.values())


def test_drain_refuses_the_last_member(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    state = tmp_path / "health.json"
    next_dns.hold(data, zone.cf, Runner(execute=True), "n1", True, now=0, path=state)
    next_dns.hold(data, zone.cf, Runner(execute=True), "n2", True, now=1, path=state)
    with pytest.raises(GuardError):
        next_dns.hold(data, zone.cf, Runner(execute=True), "n3", True, now=2, path=state)
    assert zone.ips("app.forkmesh.com") == [V4["n3"]]
    with pytest.raises(GuardError):
        next_dns.hold(data, zone.cf, Runner(execute=True), "n9", True, now=3, path=state)


def test_token_comes_from_its_own_file(tmp_path, monkeypatch):
    import os
    config = Path(os.environ["FMNEXT_CONFIG_DIR"])
    (config / "cloudflare.env").write_text(
        "CLOUDFLARE_API_TOKEN=account-wide\nCLOUDFLARE_ZONE_ID=%s\n" % ZONE)
    with pytest.raises(next_dns.ToolError):
        next_dns.Cloudflare.from_config()
    token = config / "cloudflare-dns.env"
    token.write_text("CLOUDFLARE_API_TOKEN=dns-only\nCLOUDFLARE_ZONE_ID=%s\n" % ZONE)
    token.chmod(0o644)
    with pytest.raises(GuardError):
        next_dns.Cloudflare.from_config()
    token.chmod(0o600)
    cf = next_dns.Cloudflare.from_config()
    assert cf.http._token == "dns-only" and cf.zone == ZONE
    assert "dns-only" not in repr(cf.http)


def test_main_status_lists_a_and_aaaa(tmp_path, capsys):
    data_path = tmp_path / "fleet.toml"
    production_fleet(tmp_path)
    zone = production_zone()
    assert next_dns.main(["--fleet", str(data_path), "status"], cf=zone.cf) == 0
    out = capsys.readouterr().out
    assert "app.forkmesh.com             AAAA 2001:19f0:ffff::11" in out
    assert zone.http.mutations() == []


def test_undrain_of_a_healthy_node_readds_it_at_once(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    state = tmp_path / "health.json"
    next_dns.hold(data, zone.cf, Runner(execute=True), "n1", True, now=0, path=state)
    next_dns.hold(data, zone.cf, Runner(execute=True), "n2", True, now=10, path=state)
    probed = []

    def prober(ip, host):
        probed.append(ip)
        return True
    decision = next_dns.hold(data, zone.cf, Runner(execute=True), "n1", False,
                             now=20, path=state, prober=prober,
                             sleep=lambda s: None)
    assert decision == ("add", "n1")
    assert zone.ips("app.forkmesh.com") == [V4["n1"], V4["n3"]]
    assert probed.count(V4["n1"]) == next_dns.UNDRAIN_PROBES
    saved = json.loads(state.read_text())
    assert saved["last_change"] == 10
    assert next_dns.hold(data, zone.cf, Runner(execute=True), "n2", False,
                         now=25, path=state, prober=prober,
                         sleep=lambda s: None) == ("add", "n2")
    assert zone.ips("app.forkmesh.com") == sorted(V4.values())


def test_undrain_of_a_node_that_fails_a_probe_waits_for_the_loop(tmp_path):
    data = production_fleet(tmp_path)
    zone = production_zone()
    state = tmp_path / "health.json"
    next_dns.hold(data, zone.cf, Runner(execute=True), "n1", True, now=0, path=state)
    answers = iter([True, True, False])
    assert next_dns.hold(data, zone.cf, Runner(execute=True), "n1", False, now=5,
                         path=state, prober=lambda ip, host: next(answers, True),
                         sleep=lambda s: None) is None
    assert zone.ips("app.forkmesh.com") == [V4["n2"], V4["n3"]]


def test_read_only_cores_stay_while_no_core_is_ready(tmp_path):
    zone = full_zone()
    down = ["down"] * 30
    decisions = run_rounds(tmp_path, zone, {"n1": ["read_only"] * 30,
                                            "n2": down, "n3": down}, rounds=30)
    changes = [d for d in decisions if d]
    assert changes == [("remove", "n2"), ("remove", "n3")]
    assert zone.ips("app.next.forkmesh.com") == ["203.0.113.11"]


def test_a_read_only_core_leaves_while_another_core_is_ready(tmp_path):
    zone = full_zone()
    decisions = run_rounds(tmp_path, zone, {"n1": ["ready"] * 4,
                                            "n2": ["read_only"] * 4,
                                            "n3": [True] * 4}, rounds=4)
    assert decisions == [None, None, ("remove", "n2"), None]


def test_a_read_only_core_comes_back_once_the_others_are_gone(tmp_path):
    zone = full_zone()
    n1 = ["read_only"] * 40
    gone = [True] * 4 + ["down"] * 36
    decisions = run_rounds(tmp_path, zone, {"n1": n1, "n2": gone, "n3": gone},
                           rounds=40)
    changes = [d for d in decisions if d]
    assert changes[0] == ("remove", "n1")
    assert ("add", "n1") in changes
    assert zone.ips("app.next.forkmesh.com") == ["203.0.113.11"]


def test_undrain_needs_a_fully_ready_core(tmp_path):
    data = fleet(tmp_path)
    assert next_dns.healthy_now(data, "n1", lambda ip, host: "read_only",
                                sleep=lambda s: None) is False
    assert next_dns.healthy_now(data, "n1", lambda ip, host: "ready",
                                sleep=lambda s: None) is True
