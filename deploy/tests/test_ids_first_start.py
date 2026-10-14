import json
import os
import subprocess
from pathlib import Path

import pytest

import join_agent
import next_fleet
import next_ids
import render
from fakes import FakeSsh
from next_common import Runner

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
FLEET_KEY = "ssh-ed25519 AAAAfleet operator\n"
READER_KEY = 'restrict,command="x" ssh-ed25519 AAAA fmnext-logread\n'

SYSTEMCTL = r'''#!/usr/bin/python3 -I
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__))
state_path = os.path.join(here, "state.json")
with open(state_path) as handle:
    state = json.load(handle)
args = sys.argv[1:]
state["log"].append(" ".join(args))
units = [a for a in args[1:] if not a.startswith("-")]
active = set(state["active"])
verb = args[0]
code = 0
if verb == "is-active":
    code = 0 if units and units[0] in active else 3
else:
    if verb in ("restart", "reload-or-restart"):
        starting = units
    elif verb == "try-restart":
        starting = [u for u in units if u in active]
    elif verb == "start" or (verb == "enable" and "--now" in args):
        starting = [u for u in units if u not in active]
    else:
        starting = []
    if verb == "disable" and "--now" in args:
        active -= set(units)
    for unit in starting:
        if unit == "fmnext-ids.service":
            with open(state["keys"]) as handle:
                keys = handle.read()
            state["ids_starts"].append({"index": len(state["log"]) - 1,
                                        "fresh": unit not in active,
                                        "keys": keys})
        active.add(unit)
state["active"] = sorted(active)
with open(state_path, "w") as handle:
    json.dump(state, handle)
sys.exit(code)
'''


def fleet(tmp_path):
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def run_start(tmp_path, name, ids_running):
    _, manifest = next_fleet.render_node(fleet(tmp_path), name)
    assert manifest["ids"] and manifest["logread"]
    stub = tmp_path / "stub"
    stub.mkdir()
    ssh_dir = tmp_path / "root-ssh"
    ssh_dir.mkdir()
    keys = ssh_dir / "authorized_keys"
    keys.write_text(FLEET_KEY)
    reader = tmp_path / "logread.authorized_key"
    reader.write_text(READER_KEY)
    (stub / "systemctl").write_text(SYSTEMCTL)
    (stub / "ufw").write_text("#!/bin/sh\nexit 0\n")
    for tool in ("systemctl", "ufw"):
        (stub / tool).chmod(0o755)
    (stub / "state.json").write_text(json.dumps({
        "log": [], "ids_starts": [], "keys": str(keys),
        "active": ["fmnext-ids.service"] if ids_running else []}))
    script = next_fleet.start_script(manifest)
    script = script.replace("/root/.ssh", str(ssh_dir)).replace(
        render.LOGREAD_AUTHORIZED_KEY, str(reader)).replace(
        "/opt/forkmesh-next/app/current", str(tmp_path / "no-current"))
    env = {"PATH": "%s:/usr/bin:/bin" % stub, "HOME": str(tmp_path),
           "XDG_DATA_HOME": str(tmp_path / "xdg-data"),
           "XDG_CONFIG_HOME": str(tmp_path / "xdg-config"),
           "XDG_CACHE_HOME": str(tmp_path / "xdg-cache"), "LC_ALL": "C"}
    done = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
                          capture_output=True, env=env, timeout=60)
    assert done.returncode == 0, done.stderr.decode()
    state = json.loads((stub / "state.json").read_text())
    return state, keys.read_text()


@pytest.mark.parametrize("name", ["n1", "r1"])
def test_a_fresh_node_starts_its_ids_only_after_every_change_of_the_step(
        tmp_path, name):
    state, keys = run_start(tmp_path, name, ids_running=False)
    assert keys == FLEET_KEY + READER_KEY
    assert len(state["ids_starts"]) == 1
    first = state["ids_starts"][0]
    assert first["fresh"]
    assert first["keys"] == keys
    assert first["index"] == len(state["log"]) - 1
    assert state["log"][-1] == "enable --now fmnext-ids.service"
    before = state["log"][:first["index"]]
    assert any(line.startswith("enable --now ") for line in before)
    if name == "n1":
        assert "enable forkmesh-app.service" in before


@pytest.mark.parametrize("name", ["n1", "r1"])
def test_a_running_ids_reloads_its_config_before_the_reader_key_is_written(
        tmp_path, name):
    state, keys = run_start(tmp_path, name, ids_running=True)
    assert keys == FLEET_KEY + READER_KEY
    assert len(state["ids_starts"]) == 1
    restart = state["ids_starts"][0]
    assert not restart["fresh"]
    assert restart["keys"] == FLEET_KEY
    assert state["log"][restart["index"]] == "try-restart fmnext-ids.service"


@pytest.mark.parametrize("name", ["n1", "r1"])
def test_the_first_bulk_start_never_includes_the_ids(tmp_path, name):
    _, manifest = next_fleet.render_node(fleet(tmp_path), name)
    assert "fmnext-ids.service" in manifest["units"]
    script = next_fleet.start_script(manifest)
    first = script.splitlines()[0]
    assert first.startswith("systemctl enable --now ")
    assert "fmnext-ids" not in first
    lines = [line for line in script.splitlines() if "fmnext-ids" in line]
    assert lines == ["systemctl try-restart fmnext-ids.service",
                     "systemctl enable --now fmnext-ids.service"]


@pytest.mark.parametrize("name", ["n1", "r1"])
def test_join_bundles_leave_the_ids_start_to_the_join_agent(tmp_path, name):
    _, manifest = next_fleet.render_node(fleet(tmp_path), name)
    assert "fmnext-ids" not in next_fleet.start_script(manifest, start_ids=False)
    stripped = join_agent.start_without_ids(next_fleet.start_script(manifest))
    assert "fmnext-ids" not in stripped
    assert "systemctl enable --now\n" not in stripped + "\n"


def test_fleet_api_renders_join_bundles_without_the_ids_start():
    source = (REPO / "app" / "host" / "forkmesh_host" / "fleet_api.py").read_text()
    assert "next_fleet.start_script(manifest, start_ids=False)" in source


def test_logread_step_never_starts_a_stopped_ids(tmp_path):
    _, manifest = render.render_core(fleet(tmp_path), "n1")
    script = next_fleet.logread_script(manifest, "/tmp/x.tar.gz")
    assert "systemctl restart fmnext-ids" not in script
    assert "systemctl start fmnext-ids" not in script
    assert "enable --now fmnext-ids" not in script
    check = script.index("systemctl is-active -q fmnext-ids.service ||")
    assert script.index("systemctl try-restart fmnext-ids.service") < check
    assert check < script.index("cat " + render.LOGREAD_AUTHORIZED_KEY)


def test_start_expectation_is_skipped_quietly_on_a_fresh_node(tmp_path, sandbox,
                                                              monkeypatch):
    config = Path(os.environ["FMNEXT_CONFIG_DIR"])
    (config / "secrets.env").write_text("FMNEXT_IDS_MASTER=%s\n" % ("m" * 64))
    said = []
    runner = Runner(execute=True)
    monkeypatch.setattr(runner, "say", said.append)
    ssh = FakeSsh([(lambda h, s: "fmnext-ids expect" in s,
                    b'{"ok": false, "error": "not_running"}')])
    assert next_ids.expect_step(fleet(tmp_path), ssh, runner, "n1",
                                "start") is None
    assert any("not running" in line for line in said)
