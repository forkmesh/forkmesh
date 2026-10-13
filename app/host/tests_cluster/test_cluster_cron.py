"""Leaderless cron: the per-minute coordinator rotates across the live nodes.

A real 3-node cluster (cluster_harness.py) with a 10 s cron minute
(FORKMESH_HOST_CRON_MINUTE_MS), a 1 s heartbeat and a 3 s liveness window,
every host running its own cron loop. Which node ran which minute comes from
``host_cron_runs`` (migration 0196), written by the node that ran it.
"""

from __future__ import annotations

import json
import time
import urllib.request

import pytest

from cluster_harness import finish_harness, start_harness

from forkmesh_host import cluster as mesh  # noqa: E402

MINUTE_MS = 10_000
HANDOFF_MS = MINUTE_MS * 3 // 4
RUNNER = ("ForkMeshCronRunner", "scheduled-runner-v1")
ENV = {"FORKMESH_HOST_CRON_MINUTE_MS": str(MINUTE_MS),
       "FORKMESH_HOST_HEARTBEAT_MS": "1000",
       "FORKMESH_HOST_STALE_MS": "3000"}


@pytest.fixture(scope="module")
def cron_cluster():
    harness = start_harness(ENV, cron=True)
    yield harness
    finish_harness(harness)


def _rows(harness, sql, *params):
    node = harness.rqlite.leader()
    body = json.dumps([[sql, *params]]).encode()
    request = urllib.request.Request(
        node.http + "/db/query?associative&level=strong", data=body,
        headers={"content-type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read())["results"][0].get("rows") or []


def cron_runs(harness, with_phases=False):
    """{slot: [node, ...]} of the coordinator's minutes (or, with_phases,
    {slot: {node: [phase, ...]}})."""
    runs = {}
    for row in _rows(harness, "SELECT slot, node_id, phases FROM "
                     "host_cron_runs WHERE ns=? AND name=? ORDER BY slot, "
                     "first_ms", *RUNNER):
        slot = int(row["slot"])
        if with_phases:
            runs.setdefault(slot, {})[row["node_id"]] = [
                p for p in str(row["phases"] or "").split(",") if p]
        else:
            runs.setdefault(slot, []).append(row["node_id"])
    return runs


def assert_no_phase_ran_twice(harness):
    """A minute may be handed over mid-tick (start-up, a node dying), but
    no phase of any minute ever runs on two nodes."""
    for slot, by_node in cron_runs(harness, with_phases=True).items():
        seen = {}
        for node, phases in by_node.items():
            for phase in phases:
                assert phase not in seen, "minute %d phase %s ran on %s " \
                    "and %s" % (slot, phase, seen[phase], node)
                seen[phase] = node


def now_slot():
    return mesh.now_ms() // MINUTE_MS


def wait_for(predicate, timeout, what, interval=0.25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise TimeoutError("%s not met in %.0fs (last %r)" % (what, timeout, last))


def test_cron_owner_rotates_every_minute_without_double_runs(cron_cluster):
    harness = cron_cluster
    nodes = sorted(h.node_id for h in harness.hosts)
    start = now_slot()
    # Seven cron minutes.
    wait_for(lambda: now_slot() >= start + 8, 120, "seven cron minutes",
             interval=1)
    runs = cron_runs(harness)
    window = [s for s in range(start + 1, start + 8)]
    missing = [s for s in window if s not in runs]
    doubled = {s: runs[s] for s in window if len(runs.get(s) or ()) > 1}
    print("\nCRON runs %s" % {s: runs.get(s) for s in window})
    assert not missing, "skipped cron minutes: %r" % missing
    assert not doubled, "cron minutes run on two nodes: %r" % doubled
    # Round-robin: each minute ran on rotation_owner(minute), so all three
    # nodes took turns, in the same order every node computes.
    for slot in window:
        assert runs[slot] == [mesh.rotation_owner(*RUNNER, nodes, slot)], slot
    assert {runs[s][0] for s in window} == set(nodes)
    assert_no_phase_ran_twice(harness)
    # Every node sees the same owner right now.
    owners = {h.cluster_state()["cron"]["owner"] for h in harness.hosts}
    assert len(owners) == 1


def test_owner_killed_mid_minute_next_owner_runs_the_slot(cron_cluster):
    harness = cron_cluster
    nodes = sorted(h.node_id for h in harness.hosts)
    # Wait until a fresh cron minute index has just been handed off, then
    # kill the node that now owns the upcoming minute (it has already
    # re-armed the coordinator's alarm).
    def handed_off():
        now = mesh.now_ms()
        into = (now - HANDOFF_MS) % MINUTE_MS
        return mesh.rotation_index(now, MINUTE_MS, HANDOFF_MS) \
            if 300 <= into <= 1500 else None
    index = wait_for(handed_off, 30, "a hand-off instant")
    victim_id = mesh.rotation_owner(*RUNNER, nodes, index)
    victim = harness.host(victim_id)
    victim.kill()
    killed_at = time.monotonic()
    survivors = [h for h in harness.hosts if h is not victim]
    wait_for(lambda: now_slot() >= index + 3, 60, "three more minutes",
             interval=1)
    runs = cron_runs(harness)
    print("\nKILLED %s owning minute %d; runs %s" % (
        victim_id, index, {s: runs.get(s) for s in range(index - 1, index + 3)}))
    for slot in (index, index + 1, index + 2):
        assert slot in runs, "minute %d was skipped" % slot
        assert len(runs[slot]) == 1, "minute %d ran twice: %r" % (
            slot, runs[slot])
        assert runs[slot][0] != victim_id
    assert_no_phase_ran_twice(harness)
    # The survivors now rotate between themselves.
    live = sorted(h.node_id for h in survivors)
    for slot in (index + 1, index + 2):
        assert runs[slot] == [mesh.rotation_owner(*RUNNER, live, slot)]
    # Liveness was derived by each survivor from the stale heartbeat: nobody
    # wrote the dead node's row (it still says what it last said itself).
    row = _rows(harness, "SELECT status, heartbeat_ms FROM cluster_members "
                "WHERE node_id=?", victim_id)[0]
    assert row["status"] == "active"
    for host in survivors:
        state = host.cluster_state()
        assert state["memberStatus"][victim_id] == "stale"
        assert victim_id not in state["live"]
    print("owner kill -> minutes continued, %.1fs" % (
        time.monotonic() - killed_at))
    victim.start()
    victim.wait_http()
    victim.wait_ready()
    wait_for(lambda: all(len(h.cluster_state()["live"]) == 3
                         for h in harness.hosts), 20, "3 live members again")
