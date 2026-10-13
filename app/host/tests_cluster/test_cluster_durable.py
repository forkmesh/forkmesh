"""Phase 3: Durable Objects across a real 3-node cluster.

The tests run in file order on one module-scoped cluster (cluster_harness.py)
and deliberately break it step by step: kill an object owner's host, kill the
rqlite leader with its host, then take rqlite away from one node. No host-level
role follows the rqlite leader (placement is leaderless).
"""

from __future__ import annotations

import json
import socket
import time
import urllib.error
import urllib.request

import httpx
import pytest

from cluster_harness import APP_ORIGIN, MESH_SECRET, HostNode, vm_rss_kib

websockets_sync = pytest.importorskip("websockets.sync.client")
from websockets.exceptions import ConnectionClosed, InvalidStatus  # noqa

from forkmesh_host import cluster as mesh  # noqa: E402

pytestmark = pytest.mark.filterwarnings(
    "ignore:connect\\(\\) must be used as a context manager")

WORLD = ("ForkMeshWorld", "town-square-v1")
RUNNER = ("ForkMeshCronRunner", "scheduled-runner-v1")
TIMINGS = {}


def _connect(host, path="/api/world/ws", timeout=10):
    """A browser on APP_ORIGIN whose DNS answer is this node."""
    sock = socket.create_connection(("127.0.0.1", host.port), timeout)
    try:
        return websockets_sync.connect(
            APP_ORIGIN.replace("http", "ws", 1) + path, sock=sock,
            origin=APP_ORIGIN, open_timeout=timeout)
    except BaseException:
        sock.close()
        raise


def _recv_type(ws, kind, timeout=10.0):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("no %r frame" % kind)
        frame = json.loads(ws.recv(timeout=remaining))
        if frame.get("type") == kind:
            return frame


def _owner(harness, ns_name, via=None):
    hosts = [via] if via else harness.live_hosts()
    owners = {h.node_id: h.cluster_state(*ns_name)["owner"] for h in hosts}
    return owners


def _wait(predicate, timeout, interval=0.25, what="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
            if last:
                return last
        except Exception as error:  # noqa: BLE001 - keep polling
            last = error
        time.sleep(interval)
    raise TimeoutError("%s not met in %.0fs (last %r)" % (what, timeout, last))


def _leader_host(harness):
    leader = harness.rqlite.leader()
    return harness.host(leader.name)


def test_placement_agrees_on_every_node(harness):
    for name in (WORLD, ("ForkMeshRoom", "repo:a/b:room:general"),
                 ("ForkMeshNodes", "node-events-v1:alice")):
        owners = set(_owner(harness, name).values())
        assert len(owners) == 1 and None not in owners, (name, owners)
        state = harness.hosts[0].cluster_state(*name)
        assert state["placement"] == "hash"
    # The cron coordinator and the watchdog rotate; every node agrees on
    # this minute's owner (placement "rotate"), whoever holds rqlite's lead.
    for rotating in (RUNNER, ("ForkMeshCronWatchdog", "cron-watchdog-v1")):
        owners = set(_owner(harness, rotating).values())
        assert len(owners) == 1 and None not in owners, rotating
        assert harness.hosts[0].cluster_state(*rotating)["placement"] == \
            "rotate"
    # DiscordGate and the other CronRunner doorbells: rendezvous on the name.
    for hashed in (("ForkMeshDiscordGate", "bot-rate-v1"),
                   ("ForkMeshCronRunner", "snapshot-republish-v1")):
        state = harness.hosts[0].cluster_state(*hashed)
        assert state["placement"] == "hash", hashed
        assert state["owner"] == mesh.rendezvous(
            hashed[0], hashed[1], tuple(state["live"]))
    state = harness.hosts[0].cluster_state()
    cron = state["cron"]
    assert cron["owner"] == mesh.rotation_owner(*RUNNER, state["live"],
                                                cron["index"])
    assert "rqliteLeader" in state and "isLeader" not in state
    ingress = ("ForkMeshCronRunner", "app-ingress-v1:a")
    for host in harness.hosts:
        state = host.cluster_state(*ingress)
        assert state["placement"] == "local"
        assert state["owner"] == host.node_id
    # Rendezvous spreads names over all three nodes.
    spread = {mesh.rendezvous("ForkMeshRoom", "room-%d" % i,
                              ("n1", "n2", "n3")) for i in range(64)}
    assert spread == {"n1", "n2", "n3"}


def test_status_socket_works_through_every_node(harness):
    for host in harness.hosts:
        with _connect(host, "/api/status/ws") as ws:
            assert json.loads(ws.recv(timeout=10)) == {"type": "ready"}
            ws.send(json.dumps({"type": "ping"}))
            assert _recv_type(ws, "pong")["serverTimeMs"] > 0


def test_world_presence_is_shared_across_nodes(harness):
    owner = _owner(harness, WORLD, harness.hosts[0])[
        harness.hosts[0].node_id]
    others = [h for h in harness.hosts if h.node_id != owner]
    node_a, node_b = others
    with _connect(node_a) as guest_a, _connect(node_b) as guest_b:
        welcome_a = _recv_type(guest_a, "welcome")
        welcome_b = _recv_type(guest_b, "welcome")
        # Both sockets live in the owner's single instance.
        peer_ids = {peer["id"] for peer in welcome_b["peers"]}
        assert welcome_a["id"] in peer_ids
        join = _recv_type(guest_a, "join")
        assert join["peer"]["id"] == welcome_b["id"]
        # (b) Message fan-out: a frame sent through node A reaches the
        # socket held open through node B.
        guest_a.send(json.dumps({"type": "presence", "status": "coding"}))
        presence = _recv_type(guest_b, "presence")
        assert presence["peer"]["id"] == welcome_a["id"]
    for host in others:
        assert host.cluster_state()["stats"]["proxied_websockets"] >= 1
    owner_state = harness.host(owner).cluster_state()
    assert "ForkMeshWorld/town-square-v1" in owner_state["instances"]
    for host in others:
        assert "ForkMeshWorld/town-square-v1" not in \
            host.cluster_state()["instances"]


def test_mesh_rejects_unauthenticated_requests(harness):
    host = harness.hosts[0]
    path = mesh.do_path(*WORLD, "/api/world/new-member")
    url = host.mesh + path
    assert httpx.post(url, content=b"{}").status_code == 401
    bad = mesh.sign("wrong-secret-0123456789", "POST", path, b"{}")
    assert httpx.post(url, content=b"{}", headers={
        mesh.AUTH_HEADER: bad}).status_code == 401
    stale = mesh.sign(MESH_SECRET, "POST", path, b"{}",
                      ts=mesh.now_ms() - 60_000)
    assert httpx.post(url, content=b"{}", headers={
        mesh.AUTH_HEADER: stale}).status_code == 401
    # Signed over a different body.
    other = mesh.sign(MESH_SECRET, "POST", path, b"{\"x\":1}")
    assert httpx.post(url, content=b"{}", headers={
        mesh.AUTH_HEADER: other}).status_code == 401
    good = mesh.sign(MESH_SECRET, "POST", path, b"{}")
    first = httpx.post(url, content=b"{}", headers={
        mesh.AUTH_HEADER: good, "x-fm-do-url":
        APP_ORIGIN + "/api/world/new-member"})
    assert first.status_code != 401
    replay = httpx.post(url, content=b"{}", headers={
        mesh.AUTH_HEADER: good})
    assert replay.status_code == 401
    ws_path = mesh.do_path(*WORLD, "/api/world/ws")
    with pytest.raises(InvalidStatus) as caught:
        with websockets_sync.connect("ws://127.0.0.1:%d%s" % (
                host.mesh_port, ws_path), open_timeout=5):
            pass
    assert caught.value.response.status_code == 401
    assert host.cluster_state()["stats"]["mesh_rejected"] >= 5


def _join_world(harness, exclude=(), timeout=60.0):
    """Reconnect loop a real client would run: any live node will do."""
    deadline = time.monotonic() + timeout
    attempt = 0
    while time.monotonic() < deadline:
        hosts = [h for h in harness.live_hosts() if h.node_id not in exclude]
        host = hosts[attempt % len(hosts)]
        attempt += 1
        try:
            ws = _connect(host, timeout=5)
            welcome = _recv_type(ws, "welcome", 5)
            return host, ws, welcome
        except Exception:  # noqa: BLE001 - retry like a browser
            time.sleep(0.5)
    raise TimeoutError("could not rejoin the world")


def test_owner_host_crash_fails_over(harness):
    first = harness.hosts[0]
    owner_id = _owner(harness, WORLD, first)[first.node_id]
    owner = harness.host(owner_id)
    node_a, node_b = [h for h in harness.hosts if h is not owner]
    guest_a = _connect(node_a)
    guest_b = _connect(node_b)
    _recv_type(guest_a, "welcome")
    _recv_type(guest_b, "welcome")

    owner.kill()
    killed_at = time.monotonic()
    codes = []
    for ws in (guest_a, guest_b):
        with pytest.raises(ConnectionClosed) as closed:
            while True:
                ws.recv(timeout=10)
        codes.append(closed.value.rcvd.code if closed.value.rcvd else None)
    assert codes == [1012, 1012]
    TIMINGS["close_seen_s"] = time.monotonic() - killed_at

    host_1, ws_1, welcome_1 = _join_world(harness)
    TIMINGS["world_failover_s"] = time.monotonic() - killed_at
    host_2, ws_2, welcome_2 = _join_world(harness, exclude=(host_1.node_id,))
    with ws_1, ws_2:
        assert welcome_1["id"] in {p["id"] for p in welcome_2["peers"]}
        new_owner = set(_owner(harness, WORLD).values())
        assert len(new_owner) == 1 and owner_id not in new_owner
        assert harness.host(owner_id).alive is False
        states = {h.node_id: h.cluster_state() for h in harness.live_hosts()}
        for state in states.values():
            assert owner_id not in state["active"]
            # Derived by every node from the heartbeat; nobody wrote it.
            assert state["memberStatus"][owner_id] == "stale"
    print("\nFAILOVER world owner %s killed: sockets closed after %.1fs, "
          "rejoined after %.1fs" % (owner_id, TIMINGS["close_seen_s"],
                                    TIMINGS["world_failover_s"]))

    # The node comes back under the same id and rejoins the membership.
    owner.start()
    owner.wait_http()
    owner.wait_ready()
    TIMINGS["rejoin_ready_s"] = time.monotonic() - killed_at
    _wait(lambda: all(len(h.cluster_state()["active"]) == 3
                      for h in harness.live_hosts()), 20,
          what="3 active members after rejoin")


def _first_try_welcome(host):
    with _connect(host, timeout=5) as ws:
        return _recv_type(ws, "welcome", 5)


def test_graceful_owner_restart_hands_rooms_over_without_503(harness):
    _wait(lambda: all(len(h.cluster_state()["active"]) == 3
                      for h in harness.live_hosts()), 30,
          what="3 active members")
    first = harness.hosts[0]
    owner_id = _owner(harness, WORLD, first)[first.node_id]
    owner = harness.host(owner_id)
    peers = [h for h in harness.hosts if h is not owner]
    guest = _connect(peers[0])
    _recv_type(guest, "welcome")

    started = time.monotonic()
    owner.stop()
    TIMINGS["graceful_stop_s"] = time.monotonic() - started
    with pytest.raises(ConnectionClosed) as closed:
        while True:
            guest.recv(timeout=10)
    assert closed.value.rcvd.code == 1012
    for peer in peers:
        state = peer.cluster_state(*WORLD)
        assert owner_id not in state["active"]
        assert state["memberStatus"][owner_id] == "leaving"
        assert state["owner"] != owner_id
        assert _first_try_welcome(peer)["type"] == "welcome"

    owner.start()
    owner.wait_http()
    owner.wait_ready()
    for host in [owner] + peers:
        assert _first_try_welcome(host)["type"] == "welcome"
    _wait(lambda: all(len(h.cluster_state()["active"]) == 3
                      for h in harness.live_hosts()), 20,
          what="3 active members after the graceful restart")
    for host in [owner] + peers:
        assert _first_try_welcome(host)["type"] == "welcome"


def _query_rows(node, sql, *params):
    body = json.dumps([[sql, *params]]).encode()
    request = urllib.request.Request(
        node.http + "/db/query?associative&level=none", data=body,
        headers={"content-type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read())["results"][0].get("rows") or []


def _agreed_owner(harness, name):
    owners = set(_owner(harness, name).values())
    return owners.pop() if len(owners) == 1 else None


def test_cron_runner_survives_losing_the_rqlite_leader(harness):
    # A scheduled() event on any node kicks the rotating CronRunner, which
    # arms its alarm through the mesh on this minute's owner.
    kicker = harness.hosts[0]
    kicked = httpx.post(kicker.http + "/_host/scheduled", timeout=60)
    assert kicked.status_code == 200, kicked.text
    _wait(lambda: sum(h.cluster_state()["stats"]["alarms_fired"]
                      for h in harness.live_hosts()) >= 1,
          30, what="CronRunner alarm fired on its owner")
    rows = _wait(lambda: _query_rows(
        harness.rqlite.leader(),
        "SELECT at_ms FROM do_alarms WHERE ns=? AND name=?", *RUNNER),
        30, what="the coordinator re-armed its next alarm")
    assert int(rows[0]["at_ms"]) > 0

    # Kill the rqlite leader and its host together: nothing at the host
    # level depended on that node, the cron chain continues on the owner.
    leader_host = _leader_host(harness)
    old = leader_host.node_id
    leader_host.kill()
    harness.rqlite.leader().stop()
    killed_at = time.monotonic()
    _wait(lambda: harness.rqlite.leader(timeout=1), 30,
          what="a new rqlite leader")
    fired_before = sum(h.cluster_state()["stats"]["alarms_fired"]
                       for h in harness.live_hosts())

    def rearmed():
        owner = _agreed_owner(harness, RUNNER)
        if owner is None or owner == old:
            return None
        state = harness.host(owner).cluster_state()
        return owner if ("ForkMeshCronRunner/scheduled-runner-v1"
                         in state["instances"]) else None
    owner = _wait(rearmed, 90, what="a live owner re-armed the CronRunner")
    TIMINGS["alarm_rearm_s"] = time.monotonic() - killed_at
    _wait(lambda: sum(h.cluster_state()["stats"]["alarms_fired"]
                      for h in harness.live_hosts()) > fired_before, 100,
          interval=1, what="re-armed CronRunner alarm fired")
    TIMINGS["alarm_fired_s"] = time.monotonic() - killed_at
    _wait(lambda: all(old not in h.cluster_state()["live"]
                      for h in harness.live_hosts()), 30,
          what="every survivor derived that %s is gone" % old)
    for host in harness.live_hosts():
        assert host.readyz()[0] == 200
    print("\nFAILOVER rqlite leader %s killed: CronRunner re-armed on %s "
          "after %.1fs, fired after %.1fs" % (
              old, owner, TIMINGS["alarm_rearm_s"],
              TIMINGS["alarm_fired_s"]))


def test_rss_of_cluster_mode(harness):
    """Extra RSS of cluster mode (membership + mesh listener) vs a
    single-process host on the same database and the same traffic."""
    live = harness.live_hosts()
    solo = HostNode(harness, "solo", live[0].rqlite, cluster=False)
    solo.start()
    try:
        solo.wait_http()
        for host in [solo] + live:
            with _connect(host, "/api/status/ws") as ws:
                ws.recv(timeout=10)
            httpx.get(host.http + "/api/version", timeout=10)
        time.sleep(1)
        solo_rss = vm_rss_kib(solo.proc.pid)
        cluster_rss = [vm_rss_kib(h.proc.pid) for h in live]
    finally:
        solo.stop()
    delta = max(cluster_rss) - solo_rss
    TIMINGS["rss_solo_kib"] = solo_rss
    TIMINGS["rss_cluster_kib"] = cluster_rss
    print("\nRSS solo %d KiB, cluster nodes %s KiB, delta <= %d KiB"
          % (solo_rss, cluster_rss, delta))
    assert delta < 40 * 1024


def test_readyz_fails_without_rqlite(harness):
    live = harness.live_hosts()
    assert len(live) == 2
    victim, peer = live
    with _connect(victim, "/api/status/ws") as ws_victim:
        assert json.loads(ws_victim.recv(timeout=10)) == {"type": "ready"}
        victim.rqlite.stop()
        started = time.monotonic()
        status, body = _wait(lambda: (lambda r: r if r[0] == 503 else None)(
            victim.readyz()), 10, what="readyz 503 on the node without rqlite")
        TIMINGS["readyz_503_s"] = time.monotonic() - started
        assert any("rqlite" in reason for reason in body["reasons"])
        # Only one voter of three is left: the peer loses quorum too, stops
        # hosting (sockets closed with 1012) and reports not ready.
        _wait(lambda: peer.readyz()[0] == 503, 25,
              what="peer not ready without quorum")
        TIMINGS["quorum_loss_s"] = time.monotonic() - started
        _wait(lambda: not victim.cluster_state()["hosting"], 25,
              what="victim stops hosting")
        _wait(lambda: not peer.cluster_state()["hosting"], 25,
              what="peer stops hosting")
        TIMINGS["hosting_stopped_s"] = time.monotonic() - started
        # Hosting stopped: every object instance on the peer was dropped
        # (their sockets closed with 1012).
        assert peer.cluster_state()["instances"] == [] or all(
            "app-ingress" in name or "app-git-ingress" in name
            for name in peer.cluster_state()["instances"])
    print("\nQUORUM readyz 503 after %.1fs on the node without rqlite; peer "
          "without quorum after %.1fs" % (TIMINGS["readyz_503_s"],
                                          TIMINGS["quorum_loss_s"]))
    print("TIMINGS %s" % json.dumps(TIMINGS))
