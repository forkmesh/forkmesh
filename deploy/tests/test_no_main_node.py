import json
import re
from pathlib import Path

import pytest

import next_fleet
import render

REPO = Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "deploy" / "fleet.example.toml"
KEYS = {"n1": "k" * 42 + "A=", "n2": "m" * 42 + "E=", "n3": "p" * 42 + "I=",
        "n4": "q" * 42 + "M="}
N1_TRACES = (re.compile(r"10\.77\.0\.1(?![0-9])"), re.compile(r"\bn1\b"),
             re.compile(r"203\.0\.113\.11\b"))


def n4_block(voter):
    lines = ["[[nodes]]", 'name = "n4"', 'role = "core"',
             'public_ipv4 = "203.0.113.14"', 'wg_ip = "10.77.0.4"',
             'rqlite_id = "n4"']
    if not voter:
        lines.append("rqlite_voter = false")
    return "\n".join(lines)


def load(tmp_path, text, name):
    path = tmp_path / ("%s.toml" % name)
    path.write_text(text)
    state = tmp_path / "state.json"
    state.write_text(json.dumps({"wg_pubkeys": KEYS}))
    return render.load_fleet(path, state_path=state)


@pytest.fixture
def fleets(tmp_path):
    base = EXAMPLE.read_text()
    grown = next_fleet.upsert_core_block(base, "n4", n4_block(False))
    shrunk = next_fleet.promote_core_block(next_fleet.remove_core_block(grown, "n1"), "n4")
    return {"base": load(tmp_path, base, "base"), "grown": load(tmp_path, grown, "grown"),
            "shrunk": load(tmp_path, shrunk, "shrunk")}


def files_of(fleet, name):
    files, _ = render.render_core(fleet, name)
    return {path: content for path, (content, _) in files.items()
            if isinstance(content, str)}


def test_a_fleet_without_n1_renders_no_trace_of_it(fleets):
    fleet = fleets["shrunk"]
    assert [n["name"] for n in fleet["nodes"]] == ["n2", "n3", "n4"]
    shipped = {path for path, content in files_of(fleets["base"], "n2").items()
               if content == files_of(fleets["grown"], "n2").get(path)
               and path.startswith(("/opt/", "/usr/"))}
    for name in ("n2", "n3", "n4"):
        for path, content in files_of(fleet, name).items():
            if path in shipped:
                continue
            for trace in N1_TRACES:
                assert not trace.search(content), "%s on %s names n1 (%s)" % (
                    path, name, trace.pattern)
    for runner in fleet.get("runners") or []:
        files, _ = render.render_runner(fleet, runner["name"])
        for path, (content, _) in files.items():
            if isinstance(content, str):
                assert not N1_TRACES[0].search(content), path


def rqlite_flags(fleet, name):
    unit = files_of(fleet, name)["/etc/systemd/system/rqlite.service"]
    return " ".join(line.strip(" \\") for line in unit.splitlines())


def test_every_voter_bootstraps_alike_and_the_join_list_follows_the_fleet(fleets):
    for name in ("n2", "n3", "n4"):
        flags = rqlite_flags(fleets["shrunk"], name)
        assert "-bootstrap-expect 3" in flags
        assert "-join 10.77.0.2:4002,10.77.0.3:4002,10.77.0.4:4002" in flags
    grown = rqlite_flags(fleets["grown"], "n4")
    assert "-raft-non-voter" in grown and "-bootstrap-expect" not in grown
    assert "-join 10.77.0.1:4002,10.77.0.2:4002,10.77.0.3:4002" in grown
    assert rqlite_flags(fleets["grown"], "n1") == rqlite_flags(fleets["base"], "n1")


def test_core_lists_follow_the_fleet(fleets):
    env = files_of(fleets["shrunk"], "n3")["/etc/forkmesh-next/app.env"]
    assert "FORKMESH_VAR_RUNNER_POOL_TARGET_NODES=fmnext-n2,fmnext-n3,fmnext-n4" in env
    assert "FORKMESH_VAR_HOSTED_GIT_CORE_NODES=fmnext-n2,fmnext-n3,fmnext-n4" in env
    assert "FORKMESH_MESH_PEERS=n2=http://10.77.0.2:8792,n3=http://10.77.0.3:8792," \
        "n4=http://10.77.0.4:8792" in env
    grown = files_of(fleets["grown"], "n2")["/etc/forkmesh-next/app.env"]
    assert "fmnext-n1,fmnext-n2,fmnext-n3,fmnext-n4" in grown
    wg = files_of(fleets["grown"], "n1")["/etc/wireguard/wg0.conf"]
    assert "10.77.0.4/32" in wg and KEYS["n4"] in wg


def test_every_core_renders_the_same_shape(fleets):
    fleet = fleets["shrunk"]
    shapes = {}
    for name in ("n2", "n3", "n4"):
        files = files_of(fleet, name)
        shapes[name] = sorted(files)
        upstreams = json.loads(files["/etc/forkmesh-next/mirror-node.overrides.json"])
        others = {n for n in ("n2", "n3", "n4") if n != name}
        text = json.dumps(upstreams)
        assert all(("%s.%s" % (o, fleet["domain"])) in text or
                   ("10.77.0.%s" % o[1:]) in text for o in others), name
    assert shapes["n2"] == shapes["n3"] == shapes["n4"]


def test_hosting_off_in_the_fleet_empties_the_hosted_core_list(fleets):
    fleet = dict(fleets["shrunk"], hosted={"enabled": False})
    env = render.core_role_settings(fleet)
    assert "FORKMESH_VAR_HOSTED_GIT_CORE_NODES=\n" in env + "\n"
    assert "FORKMESH_VAR_RUNNER_POOL_TARGET_NODES=fmnext-n2,fmnext-n3,fmnext-n4" in env
