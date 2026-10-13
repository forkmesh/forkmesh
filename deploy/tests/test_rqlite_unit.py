"""The rendered rqlite.service flags, run for real on loopback.

Uses $RQLITED, ~/projects/forkmesh/data/fleet/bin/rqlited or PATH; skips without
it. Three nodes start with exactly the flags of the rendered units (only
addresses, ports and paths are mapped to loopback/temp) and the auth file
tools/next_fleet.py pushes, and must elect a leader; then a replacement
node joins with the --replacement render and the old one is removed, as
replace-core does.
"""

import json
import os
import shlex
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.request
from base64 import b64encode
from pathlib import Path

import pytest

import next_fleet
import render

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"


def rqlited():
    import forkmesh_paths
    return forkmesh_paths.resolve().fleet_binary("rqlited", "RQLITED")


RQLITED = rqlited()
pytestmark = pytest.mark.skipif(RQLITED is None, reason="rqlited not available")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exec_args(unit_text):
    text = unit_text.replace("\\\n", " ")
    line = next(l for l in text.splitlines() if l.startswith("ExecStart="))
    return shlex.split(line[len("ExecStart="):])


def start(unit_text, mapping, tmp_path, name):
    args = exec_args(unit_text)
    mapped = []
    for arg in args:
        for old, new in mapping.items():
            arg = arg.replace(old, new)
        mapped.append(arg)
    mapped[0] = RQLITED
    log = open(tmp_path / ("%s.log" % name), "wb")
    return subprocess.Popen(mapped, stdout=log, stderr=subprocess.STDOUT,
                            env=dict(os.environ, TZ="UTC"))


def get(port, path, auth):
    request = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), headers={
        "Authorization": "Basic " + b64encode(auth.encode()).decode()})
    with urllib.request.urlopen(request, timeout=3) as response:
        return json.loads(response.read())


def wait_nodes(port, auth, predicate, timeout=90):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = next_fleet.parse_nodes(get(port, "/nodes?nonvoters", auth))
            if predicate(last):
                return last
        except OSError:
            pass
        time.sleep(0.5)
    raise AssertionError("cluster did not converge: %r" % last)


def test_rendered_units_form_a_cluster_and_replace_a_node(tmp_path):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    fleet["nodes"].append({"name": "n4", "role": "core", "public_ipv4": "203.0.113.14",
                           "wg_ip": "10.77.0.4", "wg_pubkey": "", "rqlite_id": "n4"})
    auth_file = tmp_path / "rqlite-auth.json"
    auth_file.write_bytes(next_fleet.rqlite_auth_json("fmnext", "pw-test"))
    ports = {name: (free_port(), free_port()) for name in ("n1", "n2", "n3", "n4")}
    mapping = {"%d": str(tmp_path)}
    for number, name in enumerate(("n1", "n2", "n3", "n4"), start=1):
        http_port, raft_port = ports[name]
        mapping["10.77.0.%d:4002" % number] = "127.0.0.1:%d" % raft_port
    processes = []
    try:
        for name in ("n1", "n2", "n3"):
            files, _ = render.render_core(fleet, name)
            unit = files["/etc/systemd/system/rqlite.service"][0]
            # n4 is added to the fleet only for the replacement below; the
            # original cluster is the three-node render.
            unit = unit.replace(",10.77.0.4:4002", "").replace(
                "-bootstrap-expect 4", "-bootstrap-expect 3")
            node_map = dict(mapping)
            node_map["127.0.0.1:4001"] = "127.0.0.1:%d" % ports[name][0]
            node_map["/var/lib/forkmesh-next/rqlite"] = str(tmp_path / name)
            processes.append(start(unit, node_map, tmp_path, name))
        auth = "fmnext:pw-test"
        nodes = wait_nodes(ports["n1"][0], auth, lambda ns: len(ns) == 3 and all(
            n.get("reachable") for n in ns) and any(n.get("leader") for n in ns))
        assert sorted(next_fleet.healthy_voters(nodes)) == ["n1", "n2", "n3"]

        # replace-core: n4 joins (no bootstrap), then n1 is removed.
        files, _ = render.render_core(fleet, "n4", replacement=True)
        unit = files["/etc/systemd/system/rqlite.service"][0]
        assert "-bootstrap-expect" not in unit
        node_map = dict(mapping)
        node_map["127.0.0.1:4001"] = "127.0.0.1:%d" % ports["n4"][0]
        node_map["/var/lib/forkmesh-next/rqlite"] = str(tmp_path / "n4")
        processes.append(start(unit, node_map, tmp_path, "n4"))
        wait_nodes(ports["n2"][0], auth,
                   lambda ns: len(next_fleet.healthy_voters(ns)) == 4)
        request = urllib.request.Request(
            "http://127.0.0.1:%d/remove" % ports["n2"][0], method="DELETE",
            data=b'{"id":"n1"}', headers={
                "Content-Type": "application/json",
                "Authorization": "Basic " + b64encode(auth.encode()).decode()})
        urllib.request.urlopen(request, timeout=5).close()
        nodes = wait_nodes(ports["n2"][0], auth, lambda ns: len(ns) == 3)
        assert sorted(n["id"] for n in nodes) == ["n2", "n3", "n4"]
        # Anonymous callers get nothing but the bootstrap notify.
        with pytest.raises(urllib.error.HTTPError) as error:
            urllib.request.urlopen("http://127.0.0.1:%d/db/query?q=select+1"
                                   % ports["n2"][0], timeout=3)
        assert error.value.code == 401
    finally:
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
