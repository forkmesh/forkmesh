#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402
import next_add_core  # noqa: E402
from next_add_core import Git, State  # noqa: E402

EVENT = "FMNEXT-REMOVE-CORE "
BRANCH_PREFIX = "fleet/remove-core-"
MAIN_REF = "refs/heads/main"
FLEET_PATH = "deploy/fleet.toml"
DEFAULT_PROVIDER = "vultr"
MIN_CORES = 3
MIN_VOTERS = 3
DRAIN_MARGIN = 15
LEADER_TIMEOUT = 120
REMOVE_TIMEOUT = 120
PROMOTE_TIMEOUT = 300
NAME_RE = re.compile(r"n[0-9]{1,2}")
CORE_LABEL_RE = re.compile(r"fmnext-core-[0-9]{1,2}")
WG_KEY_RE = re.compile(r"[A-Za-z0-9+/]{43}=")
WG_PUBKEY = "wg show wg0 public-key"
WG_PEERS = "wg show wg0 peers"
REMOVE_BODY = "/tmp/fmnext-remove-core.json"
EXECUTE_BODY = "/tmp/fmnext-remove-core-sql.json"
RETIRE_MEMBER_SQL = "DELETE FROM cluster_members WHERE node_id=?"
STOP_UNITS = (
    "forkmesh-app.service", "forkmesh-mirror-node.service", "caddy.service",
    "fmnext-certsync.service", "fmnext-cert-sync.timer", "fmnext-cert-sync.service",
    "fmnext-fleet-peers.service", "fmnext-request-rules.path",
    "fmnext-request-rules.service", "fmnext-turn.service",
    "fmnext-calls.service",
)
RQLITE_UNIT = "rqlite.service"

STEPS = (
    ("preflight", "Preflight: the node, rqlite quorum after removal, leadership", 5),
    ("promote", "Promote a non-voter core so 3 or more voters remain", 120),
    ("leader", "Hand rqlite leadership to another voter if this core leads", 15),
    ("drain", "Drain it from DNS and wait out the record TTL", 75),
    ("stop", "Stop the app, mirror daemon, Caddy and certsync on it", 15),
    ("rqlite", "Stop its rqlite and remove it from the cluster", 30),
    ("fleet", "Take it out of deploy/fleet.toml and fleet-state.json", 2),
    ("peers", "Remaining cores drop its WireGuard peer and firewall allowances", 240),
    ("deregister", "DNS health loop, its own DNS name, mail SPF", 10),
    ("commit", "Commit deploy/fleet.toml to a branch and merge it into main", 2),
    ("destroy", "Delete the cloud instance (irreversible)", 30),
)


def steps_for(destroy):
    return tuple(step for step in STEPS if destroy or step[0] != "destroy")


def step_ids(destroy):
    return tuple(step for step, _, _ in steps_for(destroy))


def estimate_seconds(destroy):
    return sum(seconds for _, _, seconds in steps_for(destroy))


def check_name(name):
    if not NAME_RE.fullmatch(str(name or "")):
        raise GuardError("remove-core takes a core name like n4, not %r" % name)
    return str(name)


def provider_of(entry):
    return str((entry or {}).get("provider") or DEFAULT_PROVIDER).strip().lower()


def provider_client(name, region=None):
    try:
        import next_cloud
    except ImportError:
        next_cloud = None
    if next_cloud is not None:
        return next_cloud.from_config(name, region=region or None)
    if name != DEFAULT_PROVIDER:
        raise ToolError("provider %r is not supported by this checkout (only %s)"
                        % (name, DEFAULT_PROVIDER))
    import next_fleet
    return next_fleet.Vultr.from_config()


def core_order(name):
    digits = re.sub(r"[^0-9]", "", str(name))
    return (int(digits) if digits else 0, str(name))


def promotion_candidate(nodes, target_id, cores_after):
    voters = [str(n.get("id")) for n in nodes if n.get("voter", True)]
    if target_id not in voters:
        return ""
    after = [v for v in voters if v != target_id]
    if len(after) >= MIN_VOTERS and len(after) % 2 == 1:
        return ""
    cores = {str(c) for c in cores_after}
    spare = sorted((str(n.get("id")) for n in nodes
                    if not n.get("voter", True) and n.get("reachable")
                    and str(n.get("id")) in cores and str(n.get("id")) != target_id),
                   key=core_order)
    return spare[0] if spare else ""


def quorum_report(nodes, target_id, cores_after):
    voters = sorted(str(n.get("id")) for n in nodes if n.get("voter", True))
    member = any(str(n.get("id")) == target_id for n in nodes)
    target_voter = target_id in voters
    promote = promotion_candidate(nodes, target_id, cores_after)
    after = sorted([v for v in voters if v != target_id] + ([promote] if promote else []))
    healthy = sorted([str(n.get("id")) for n in nodes
                      if n.get("voter", True) and n.get("reachable")
                      and str(n.get("id")) != target_id] + ([promote] if promote else []))
    leaders = [str(n.get("id")) for n in nodes if n.get("leader")]
    report = {"voters_before": voters, "voters_after": after,
              "healthy_after": healthy, "leader": leaders[0] if leaders else "",
              "member": member, "target_voter": target_voter, "promote": promote,
              "cores_after": sorted(cores_after), "allowed": True, "blocker": "",
              "warnings": []}
    problems = []
    if len(cores_after) < MIN_CORES:
        problems.append("removing %s would leave %d core(s) (%s); the fleet needs at "
                        "least %d" % (target_id, len(cores_after),
                                      ", ".join(sorted(cores_after)) or "none",
                                      MIN_CORES))
    if len(healthy) < MIN_VOTERS:
        problems.append("removing %s would leave %d healthy rqlite voter(s) (%s); "
                        "need %d" % (target_id, len(healthy),
                                     ", ".join(healthy) or "none", MIN_VOTERS))
    elif len(healthy) * 2 <= len(after):
        problems.append("only %d of the %d remaining voters are healthy: not a "
                        "majority" % (len(healthy), len(after)))
    if problems:
        report.update(allowed=False, blocker="; ".join(problems))
    if target_voter and after and len(after) % 2 == 0:
        report["warnings"].append(
            "%d voters remain (even): they survive no more failures than %d" % (
                len(after), len(after) - 1))
    if report["leader"] and report["leader"] == target_id:
        report["warnings"].append("%s is the rqlite leader: leadership is handed "
                                  "to another voter first" % target_id)
    if not leaders:
        report["warnings"].append("no rqlite leader reported")
    return report


def state_dir():
    return common.config_dir() / "remove-core"


def state_path(name):
    return state_dir() / ("%s.json" % check_name(name))


def remove_instance_entry(label):
    import next_fleet
    if not CORE_LABEL_RE.fullmatch(str(label or "")):
        raise GuardError("refusing to forget %r: not a core instance label" % label)
    path = next_fleet.instances_path()
    data = common.load_json(path, {}) or {}
    if data.pop(label, None) is not None:
        common.save_json(path, data)
        return True
    return False


class Job(next_add_core.Job):
    def emit(self, **event):
        if not self.events:
            return
        with self.emit_lock:
            print(EVENT + json.dumps(event, sort_keys=True), file=self.out, flush=True)

    @property
    def destroy(self):
        return bool(self.state.get("destroy_instance"))


class SshIdentity:
    def __init__(self, ssh, expected):
        self.ssh = ssh
        self.expected = dict(expected or {})

    def verify_target(self, name, ip):
        key = self.expected.get(name, "")
        if not WG_KEY_RE.fullmatch(key or ""):
            raise GuardError("%s has no recorded WireGuard public key; cannot "
                             "confirm over SSH that %s is that node" % (name, ip))
        completed = self.ssh.run(ip, WG_PUBKEY, check=False, timeout=30)
        live = (completed.stdout or b"").decode("utf-8", "replace").strip()
        if completed.returncode != 0 or live != key:
            raise GuardError("%s (%s) answers with WireGuard key %r, not %s's "
                             "recorded key; refusing" % (ip, ip, live[:12], name))
        return {"name": name, "ip": ip}


class RemoveBackend:
    def __init__(self, ssh=None, cf=None, provider_factory=None, repo=None,
                 sleep=time.sleep, clock=time.monotonic, git=None):
        self.ssh = ssh
        self._cf = cf
        self.provider_factory = provider_factory or provider_client
        self.repo = Path(repo or common.REPO)
        self.sleep = sleep
        self.clock = clock
        self.git = git or Git(self.repo)

    def fleet(self, job):
        return common.load_fleet(job.fleet_file)

    def cf(self, fleet):
        import next_dns
        if self._cf is None:
            self._cf = next_dns.Cloudflare.from_config(
                allowed=next_dns.core_addresses(fleet))
        return self._cf

    def ip(self, job):
        return job.state.get("ip") or ""

    def identity(self, job, fleet=None):
        keys = {}
        for node in (fleet or {}).get("nodes") or []:
            keys[node["name"]] = node.get("wg_pubkey") or ""
        keys[job.name] = job.state.get("wg_pubkey") or keys.get(job.name, "")
        return SshIdentity(self.ssh, keys)

    def verify(self, job):
        if job.execute:
            self.identity(job).verify_target(job.name, self.ip(job))

    def peer_nodes(self, fleet, job):
        wanted = set(job.state.get("peers") or [])
        return [n for n in fleet.get("nodes") or []
                if n["name"] != job.name and (not wanted or n["name"] in wanted)]

    def cluster(self, fleet, name):
        import next_fleet
        return next_fleet.cluster_nodes(fleet, self.ssh, exclude=(name,))

    def plan(self, fleet_file, name, destroy=False):
        import next_fleet
        import render
        name = check_name(name)
        fleet = common.load_fleet(fleet_file)
        try:
            node = render.node_by_name(fleet, name)
        except render.RenderError:
            raise GuardError("%s is not a core in %s" % (name, fleet_file)) from None
        if render.is_app_node(node):
            raise GuardError("%s is an autoscaled app node; next_autoscale.py "
                             "removes those" % name)
        number = int(name[1:])
        label = node.get("label") or next_fleet.label_for("core", number)
        instances = common.load_json(next_fleet.instances_path(), {}) or {}
        entry = instances.get(label) or {}
        cores_after = [n["name"] for n in fleet.get("nodes") or [] if n["name"] != name]
        rqlite_id = str(node.get("rqlite_id") or name)
        plan = {"name": name, "number": number, "label": label,
                "rqlite_id": rqlite_id, "ip": node["public_ipv4"],
                "ipv6": node.get("public_ipv6") or "",
                "wg_pubkey": node.get("wg_pubkey") or "",
                "instance_id": str(node.get("instance_id") or entry.get("id") or ""),
                "provider": provider_of(entry), "region": entry.get("region") or "",
                "plan": entry.get("plan") or "",
                "voter": render.rqlite_voter(node), "peers": sorted(cores_after),
                "configured_voters": sorted(
                    str(n.get("rqlite_id") or n["name"]) for n in fleet.get("nodes") or []
                    if render.rqlite_voter(n)),
                "destroy_instance": bool(destroy), "fleet_file": str(fleet_file)}
        try:
            nodes = self.cluster(fleet, name)
            report = quorum_report(nodes, rqlite_id, cores_after)
        except ToolError as error:
            report = {"allowed": False, "live_error": str(error),
                      "blocker": "cannot read the rqlite cluster (%s); removal "
                                 "needs a live quorum check" % error,
                      "voters_before": plan["configured_voters"],
                      "voters_after": [v for v in plan["configured_voters"]
                                       if v != rqlite_id],
                      "healthy_after": [], "leader": "", "member": True, "promote": "",
                      "target_voter": plan["voter"], "cores_after": sorted(cores_after),
                      "warnings": []}
        plan.update(report)
        plan["is_leader"] = bool(report.get("leader")) and report["leader"] == rqlite_id
        return plan

    def step_preflight(self, job):
        unfinished = [s.get("name") for s in next_add_core.unfinished_states()]
        if unfinished:
            raise GuardError("add-core %s is unfinished; finish or settle it before "
                             "removing a core" % ", ".join(unfinished))
        if job.state.get("peers") is None:
            raise ToolError("the plan has no peer list")
        if not job.execute:
            job.say("[dry-run] %s" % quorum_text(job.state.data))
            return
        fresh = self.plan(job.fleet_file, job.name, job.destroy)
        if not fresh.get("allowed"):
            raise GuardError(fresh.get("blocker") or "removal refused")
        if not WG_KEY_RE.fullmatch(job.state.get("wg_pubkey") or ""):
            raise GuardError("%s has no recorded WireGuard public key; its identity "
                             "cannot be checked over SSH" % job.name)
        self.verify(job)
        job.state.set(leader=fresh.get("leader"), is_leader=fresh.get("is_leader"),
                      healthy_after=fresh.get("healthy_after"),
                      voters_after=fresh.get("voters_after"),
                      promote=fresh.get("promote") or "")
        job.say(quorum_text(fresh))

    def step_promote(self, job):
        import next_dns
        import next_fleet
        import next_ids
        promote = job.state.get("promote") or ""
        if not promote:
            job.say("no non-voter needs promoting: %s" % promote_note(job))
            return
        if not job.execute:
            job.say("[dry-run] %s: drain, rqlite remove it as a non-voter, re-render "
                    "it as a voter (-join the voters), restart rqlite, wait until "
                    "/nodes lists it as a reachable voter, health gate, undrain"
                    % promote)
            return
        self.set_voter_in_files(job, promote)
        fleet = self.fleet(job)
        node = next(n for n in fleet.get("nodes") or [] if n["name"] == promote)
        ip = node["public_ipv4"]
        if self.live_voter(fleet, promote):
            job.state.set(promoted=promote)
            job.say("%s is already an rqlite voter" % promote)
            return
        extra = ["--fleet", str(job.fleet_file)]
        identity = self.identity(job, fleet)
        identity.verify_target(promote, ip)
        if next_dns.main(extra + ["drain", promote, "--execute"]) != 0:
            raise ToolError("%s: drain refused before its promotion" % promote)
        nodes = self.cluster(fleet, promote)
        if any(str(n.get("id")) == promote for n in nodes):
            peer = self.voter_peer(fleet, nodes, promote)
            body = json.dumps({"id": promote}).encode()
            job.runner.step("rqlite remove %s as a non-voter (via %s)" % (
                promote, peer["name"]),
                lambda: self.ssh.run(peer["public_ipv4"], remove_script(),
                                     input_bytes=body, timeout=60))
        next_fleet.install(fleet, promote, self.ssh, job.runner, ("files",),
                           vultr=identity, replacement=True)
        next_ids.push_expectation(fleet, self.ssh, job.runner, promote,
                                  rescan=next_ids.WANTS,
                                  allow=("listeners", "processes"),
                                  reason="next_fleet remove-core promote")
        job.runner.step("%s: restart rqlite to join as a voter" % promote,
                        lambda: self.ssh.run(ip, "systemctl restart %s" % RQLITE_UNIT,
                                             timeout=120))
        if not common.wait_for(lambda: self.live_voter(fleet, promote),
                               PROMOTE_TIMEOUT, interval=3, sleep=self.sleep,
                               clock=self.clock):
            raise ToolError("%s is not a reachable rqlite voter after %d s; it is "
                            "out of DNS" % (promote, PROMOTE_TIMEOUT))
        deadline = self.clock() + next_fleet.ROLL_HEALTH_SECONDS
        while True:
            ok, note = next_fleet.roll_healthy(self.ssh, ip)
            if ok or self.clock() >= deadline:
                break
            self.sleep(3)
        if not ok:
            raise ToolError("%s is a voter but unhealthy (%s); it is out of DNS"
                            % (promote, note))
        next_dns.main(extra + ["undrain", promote, "--execute"])
        job.state.set(promoted=promote)
        job.say("%s is an rqlite voter and back in DNS" % promote)

    def set_voter_in_files(self, job, name):
        import next_fleet
        current = job.fleet_file.read_text("utf-8")
        text = next_fleet.promote_core_block(current, name)
        if text != current:
            job.fleet_file.write_text(text, "utf-8")
            job.say("%s: %s is a voter" % (job.fleet_file, name))
        path = next_add_core.fleet_state_path()
        state = common.load_json(path, {}) or {}
        nonvoters = [n for n in state.get("rqlite_nonvoters") or [] if n != name]
        if nonvoters != list(state.get("rqlite_nonvoters") or []):
            state["rqlite_nonvoters"] = nonvoters
            common.save_json(path, state)
            job.say("%s: %s is no longer a non-voter" % (path, name))

    def live_voter(self, fleet, name):
        try:
            nodes = self.cluster(fleet, name)
        except ToolError:
            return False
        leaders = [n for n in nodes if n.get("leader")]
        return bool(leaders) and any(
            str(n.get("id")) == name and n.get("voter", True) and n.get("reachable")
            for n in nodes)

    def voter_peer(self, fleet, nodes, exclude):
        voters = {str(n.get("id")) for n in nodes
                  if n.get("voter", True) and n.get("reachable")}
        for node in fleet.get("nodes") or []:
            if node["name"] != exclude and str(node.get("rqlite_id") or node["name"]) in voters:
                return node
        raise ToolError("no reachable rqlite voter to send the change through")

    def step_leader(self, job):
        fleet = self.fleet(job)
        target = job.state.get("rqlite_id") or job.name
        if not job.execute:
            job.say("[dry-run] %s" % ("%s leads rqlite: POST /leader on it (step "
                                      "down), wait for another voter to lead" % target
                                      if job.state.get("is_leader") else
                                      "%s does not lead rqlite; nothing to hand over"
                                      % target))
            return
        nodes = self.cluster(fleet, job.name)
        leaders = [str(n.get("id")) for n in nodes if n.get("leader")]
        if leaders and leaders[0] != target:
            job.say("rqlite leader is %s, not %s" % (leaders[0], target))
            return
        self.verify(job)
        job.runner.step("%s: ask rqlite to hand leadership to another voter" % job.name,
                        lambda: self.ssh.run(self.ip(job), common.rqlite_curl(
                            "/leader", "POST"), timeout=60))

        def moved():
            try:
                current = self.cluster(fleet, job.name)
            except ToolError:
                return None
            ids = [str(n.get("id")) for n in current if n.get("leader")]
            return ids[0] if ids and ids[0] != target else None
        leader = common.wait_for(moved, LEADER_TIMEOUT, interval=3, sleep=self.sleep,
                                 clock=self.clock)
        if not leader:
            raise ToolError("%s still leads rqlite after %d s" % (target, LEADER_TIMEOUT))
        job.state.set(leader=leader)
        job.say("rqlite leader is now %s" % leader)

    def step_drain(self, job):
        import next_dns
        fleet = self.fleet(job)
        wait = next_dns.TTL + DRAIN_MARGIN
        if not job.execute:
            job.say("[dry-run] next_dns.py drain %s (refused if it is the last node "
                    "in DNS), then wait %d s (TTL %d s) for resolvers to drop it"
                    % (job.name, wait, next_dns.TTL))
            return
        next_dns.hold(fleet, self.cf(fleet), job.runner, job.name, True)
        if next_dns.node_entry(job.name).get("in_dns"):
            raise ToolError("%s is still in DNS after the drain" % job.name)
        drained = job.state.get("drained_at")
        if not drained:
            drained = time.time()
            job.state.set(drained_at=drained)
        left = wait - (time.time() - drained)
        if left > 0:
            job.say("%s is out of DNS; waiting %d s for the TTL" % (job.name, left))
            self.sleep(left)

    def step_stop(self, job):
        import next_ids
        fleet = self.fleet(job)
        self.verify(job)
        next_ids.push_expectation(fleet, self.ssh, job.runner, job.name,
                                  rescan=next_ids.WANTS,
                                  allow=("listeners", "processes"),
                                  reason="next_fleet remove-core stop")
        job.runner.step("%s: disable and stop %s" % (job.name, ", ".join(STOP_UNITS)),
                        lambda: self.ssh.run(self.ip(job), stop_script(STOP_UNITS),
                                             timeout=180))

    def step_rqlite(self, job):
        import next_ids
        fleet = self.fleet(job)
        target = job.state.get("rqlite_id") or job.name
        if not job.execute:
            job.say("[dry-run] %s: disable and stop %s, then DELETE /remove %s on a "
                    "healthy peer and wait until /nodes no longer lists it" % (
                        job.name, RQLITE_UNIT, target))
            return
        nodes = self.cluster(fleet, job.name)
        report = quorum_report(nodes, target, job.state.get("peers") or [])
        if not report["allowed"]:
            raise GuardError(report["blocker"])
        self.verify(job)
        next_ids.push_expectation(fleet, self.ssh, job.runner, job.name,
                                  rescan=next_ids.WANTS,
                                  allow=("listeners", "processes"),
                                  reason="next_fleet remove-core rqlite")
        job.runner.step("%s: disable and stop %s" % (job.name, RQLITE_UNIT),
                        lambda: self.ssh.run(self.ip(job), stop_script((RQLITE_UNIT,)),
                                             timeout=120))
        if report["member"]:
            peer = self.healthy_peer(fleet, job)
            body = json.dumps({"id": target}).encode()
            job.runner.step("rqlite remove %s (via %s)" % (target, peer["name"]),
                            lambda: self.ssh.run(peer["public_ipv4"], remove_script(),
                                                 input_bytes=body, timeout=60))
        else:
            job.say("%s is no longer in the rqlite cluster" % target)
        retire = self.healthy_peer(fleet, job)
        sql = json.dumps([[RETIRE_MEMBER_SQL, job.name]]).encode()
        job.runner.step("retire %s's cluster_members row (via %s), so /status drops "
                        "its core row now" % (job.name, retire["name"]),
                        lambda: self.ssh.run(retire["public_ipv4"], execute_script(),
                                             input_bytes=sql, timeout=60))

        def gone():
            try:
                current = self.cluster(fleet, job.name)
            except ToolError:
                return None
            if any(str(n.get("id")) == target for n in current):
                return None
            check = quorum_report(current, target, job.state.get("peers") or [])
            return check["healthy_after"] if check["allowed"] and check["leader"] else None
        healthy = common.wait_for(gone, REMOVE_TIMEOUT, interval=3, sleep=self.sleep,
                                  clock=self.clock)
        if not healthy:
            raise ToolError("%s is still listed in rqlite /nodes, or the remaining "
                            "voters are not healthy, after %d s" % (target, REMOVE_TIMEOUT))
        job.say("%s left the rqlite cluster; healthy voters %s" % (
            target, ", ".join(healthy)))

    def healthy_peer(self, fleet, job):
        for node in self.peer_nodes(fleet, job):
            try:
                out = self.ssh.run(node["public_ipv4"], common.rqlite_curl("/readyz"),
                                   check=False, timeout=30)
            except (ToolError, OSError):
                continue
            if out.returncode == 0:
                return node
        raise ToolError("no remaining core answers rqlite /readyz")

    def step_fleet(self, job):
        import next_fleet
        import render
        if not job.execute:
            job.say("[dry-run] %s: remove the %s [[nodes]] block (the rest of the "
                    "file is untouched) and list it in [hosted] retired_cores so its "
                    "repositories stay core-held; fleet-state.json: drop its "
                    "WireGuard key and any rqlite_nonvoters entry"
                    % (job.fleet_file, job.name))
            return
        current = job.fleet_file.read_text("utf-8")
        text = next_fleet.retire_core_block(
            next_fleet.remove_core_block(current, job.name), job.name)
        if text != current:
            with tempfile.TemporaryDirectory(prefix="fmnext-fleet-") as tmp:
                trial = Path(tmp) / "fleet.toml"
                trial.write_text(text, "utf-8")
                left = render.load_fleet(trial, next_add_core.fleet_state_path())
            if job.name in [n["name"] for n in left.get("nodes") or []]:
                raise ToolError("%s is still in the edited fleet file" % job.name)
            job.fleet_file.write_text(text, "utf-8")
            job.say("%s: %s removed" % (job.fleet_file, job.name))
        path = next_add_core.fleet_state_path()
        state = common.load_json(path, {}) or {}
        changed = False
        if job.name in (state.get("wg_pubkeys") or {}):
            state["wg_pubkeys"].pop(job.name)
            changed = True
        nonvoters = [n for n in state.get("rqlite_nonvoters") or [] if n != job.name]
        if nonvoters != list(state.get("rqlite_nonvoters") or []):
            state["rqlite_nonvoters"] = nonvoters
            changed = True
        if changed:
            common.save_json(path, state)
            job.say("%s: %s forgotten" % (path, job.name))

    def step_peers(self, job):
        import next_fleet
        fleet = self.fleet(job)
        done = set(job.state.items("peers_done"))
        key = job.state.get("wg_pubkey") or ""
        for node in self.peer_nodes(fleet, job):
            peer = node["name"]
            if peer in done:
                job.say("%s: already updated" % peer)
                continue
            next_fleet.roll(fleet, [peer], self.ssh, job.runner, steps=("files", "start"),
                            vultr=self.identity(job, fleet), fleet_path=job.fleet_file,
                            sleep=self.sleep, clock=self.clock)
            if not job.execute:
                continue
            listed = self.ssh.run(node["public_ipv4"], WG_PEERS, check=False, timeout=30)
            if key and key in (listed.stdout or b"").decode("utf-8", "replace"):
                raise ToolError("%s still has %s's WireGuard peer after its update"
                                % (peer, job.name))
            job.state.add_item("peers_done", peer)

    def step_deregister(self, job):
        import next_dns
        fleet = self.fleet(job)
        host = "%s.%s" % (job.name, fleet["domain"])
        if not job.execute:
            job.say("[dry-run] %s: forget %s; Cloudflare: delete %s A/AAAA%s" % (
                next_dns.state_path(), job.name, host,
                "; mail: SPF TXT without its addresses" if self.mail_enabled() else ""))
            return
        path = next_dns.state_path()
        with next_dns.locked(path):
            data = common.load_json(path, {}) or {}
            if (data.get("nodes") or {}).pop(job.name, None) is not None:
                common.save_json(path, data)
                job.say("%s: %s forgotten" % (path, job.name))
        cf = self.cf(fleet)
        for record_type in next_dns.RECORD_TYPES:
            for record in cf.records(host, record_type):
                job.runner.step("delete %s %s -> %s" % (record_type, host,
                                                         record.get("content")),
                                lambda r=record: cf.delete(r["id"]))
        if self.mail_enabled():
            import next_mail
            next_mail.dns(fleet, job.runner)
        job.say("backups, autodeploy and roll read deploy/fleet.toml; the desktop's "
                "fleet table lists %s from fleet-instances.json until its instance "
                "is deleted" % job.state.get("label"))

    def mail_enabled(self):
        try:
            import next_mail
            return next_mail.key_path().is_file()
        except (ImportError, ToolError, OSError):
            return False

    def step_commit(self, job):
        import next_fleet
        branch = BRANCH_PREFIX + job.name
        if not job.execute:
            job.say("[dry-run] commit %s without the %s block on %s (base %s) and "
                    "merge it into main" % (FLEET_PATH, job.name, branch, MAIN_REF))
            return
        if job.state.get("replaced_by"):
            job.say("%s is committed together with %s by add-core --replace" % (
                FLEET_PATH, job.state.get("replaced_by")))
            job.state.set(branch="", commit="")
            return
        base_text = self.git.show(self.git.rev(MAIN_REF), FLEET_PATH)
        text = next_fleet.retire_core_block(
            next_fleet.remove_core_block(base_text, job.name), job.name)
        promoted = job.state.get("promoted") or ""
        if promoted:
            text = next_fleet.promote_core_block(text, promoted)
        if text == base_text:
            job.say("%s on main has no %s block; nothing to commit" % (FLEET_PATH,
                                                                       job.name))
            job.state.set(branch="", commit="")
            return
        message = ("Remove core node %s (%s) from deploy/fleet.toml\n\n"
                   "Removed by tools/next_fleet.py remove-core.%s\n"
                   % (job.name, job.state.get("label"),
                      " %s was promoted to an rqlite voter." % promoted
                      if promoted else ""))
        commit, created = self.git.commit_file(branch, MAIN_REF, FLEET_PATH, text, message)
        job.state.set(branch=branch, commit=commit)
        job.say("%s %s on %s" % ("committed" if created else "already committed",
                                 commit[:12], branch))
        next_add_core.land_on_main(self.git, job, branch, commit)

    def step_destroy(self, job):
        import next_fleet
        label = job.state.get("label")
        provider = job.state.get("provider") or DEFAULT_PROVIDER
        if not job.destroy:
            raise GuardError("the instance is deleted only with --destroy-instance")
        if not job.execute:
            job.say("[dry-run] %s: delete %s (%s) after checking its label, tag, id "
                    "and address; forget it in fleet-instances.json" % (
                        provider, label, job.state.get("instance_id") or "id unknown"))
            return
        client = self.provider_factory(provider, job.state.get("region"))
        instance = client.find_by_label(label)
        if instance is None:
            job.say("%s: no instance labelled %s; already deleted" % (provider, label))
            remove_instance_entry(label)
            job.state.set(destroyed=True)
            return
        guard = getattr(client, "guard", None) or next_fleet.guard_instance
        guard(instance)
        recorded = job.state.get("instance_id")
        if recorded and str(instance.get("id")) != str(recorded):
            raise GuardError("%s is instance %s on %s, but %s was recorded; refusing"
                             % (label, instance.get("id"), provider, recorded))
        live_ip = instance.get("main_ip") or instance.get("ip")
        if live_ip and job.state.get("ip") and live_ip != job.state.get("ip"):
            raise GuardError("%s has address %s on %s, not %s; refusing" % (
                label, live_ip, provider, job.state.get("ip")))
        job.runner.step("DESTROY %s instance %s (%s, %s)" % (
            provider, label, instance.get("id"), live_ip),
            lambda: client.delete_instance(instance["id"]))
        remove_instance_entry(label)
        job.state.set(destroyed=True)

    def contain(self, job):
        notes = []
        done = {step for step in step_ids(True) if job.state.done(step)}
        if "drain" in done and "fleet" not in done:
            notes.append("%s is out of DNS and held. Re-run remove-core %s --execute "
                         "to continue from the failed step." % (job.name, job.name))
            if "rqlite" not in done:
                notes.append("To keep %s instead: next_fleet.py install %s --steps "
                             "start --execute, then next_dns.py undrain %s --execute."
                             % (job.name, job.name, job.name))
            else:
                notes.append("%s has left rqlite; finish the removal rather than "
                             "putting it back." % job.name)
        if job.state.get("promote") and "promote" not in done \
                and (job.state.get("failed") or {}).get("step") == "promote":
            notes.append("%s may be out of DNS and out of rqlite while it rejoins as a "
                         "voter. Re-run remove-core %s --execute to finish the "
                         "promotion." % (job.state.get("promote"), job.name))
        if "fleet" in done and "peers" not in done:
            notes.append("deploy/fleet.toml no longer lists %s; some cores may still "
                         "have its WireGuard peer until the peers step finishes."
                         % job.name)
        held = []
        try:
            import next_dns
            data = common.load_json(next_dns.state_path(), {}) or {}
            held = sorted(name for name, item in (data.get("nodes") or {}).items()
                          if name != job.name and not item.get("in_dns"))
        except (ImportError, OSError, ValueError):
            pass
        if held:
            notes.append("out of DNS as well: %s (a peer update stops with the peer "
                         "drained; check it, then next_dns.py undrain <node> --execute)"
                         % ", ".join(held))
        return notes

    def describe(self, job, step):
        name = job.name
        peers = job.state.get("peers") or []
        if step == "preflight":
            return [quorum_text(job.state.data),
                    "refused below %d cores or %d healthy voters, or without a "
                    "healthy majority" % (MIN_CORES, MIN_VOTERS),
                    "state file: %s (a re-run resumes from the failed step)"
                    % state_path(name)]
        if step == "promote":
            promote = job.state.get("promote") or ""
            if not promote:
                return ["nothing to promote: %s" % promote_note(job)]
            return ["%s (a non-voter core) becomes a voter first, so the voters after "
                    "the removal are %s" % (promote, ", ".join(
                        job.state.get("voters_after") or [])),
                    "%s: drain, rqlite remove it as a non-voter, re-render it as a "
                    "voter (-join the voters; fleet.toml drops rqlite_voter = false), "
                    "restart rqlite, wait until it is a reachable voter, health gate, "
                    "undrain" % promote]
        if step == "leader":
            return ["%s: POST /leader (step down), wait for another voter to lead"
                    % name if job.state.get("is_leader") else
                    "%s does not lead rqlite (leader: %s); nothing to hand over"
                    % (name, job.state.get("leader") or "unknown")]
        if step == "drain":
            import next_dns
            return ["next_dns.py drain %s: out of %s; held so the health loop never "
                    "adds it back; wait %d s" % (
                        name, ", ".join(self.served_names(job)) or "the served names",
                        next_dns.TTL + DRAIN_MARGIN)]
        if step == "stop":
            return ["%s: IDS expectation, then systemctl disable --now %s" % (
                name, " ".join(STOP_UNITS))]
        if step == "rqlite":
            return ["%s: systemctl disable --now %s" % (name, RQLITE_UNIT),
                    "a healthy peer: DELETE /remove {\"id\": \"%s\"}; wait until "
                    "/nodes no longer lists it and a leader is up" % (
                        job.state.get("rqlite_id") or name),
                    "a healthy peer: delete its cluster_members row, so its /status "
                    "core row goes at once (the other rows keep their history)"]
        if step == "fleet":
            return ["%s: remove the %s [[nodes]] block; fleet-state.json: drop its "
                    "WireGuard key and rqlite_nonvoters entry" % (job.fleet_file, name)]
        if step == "peers":
            return ["%s: roll --steps files,start: drain, re-render without %s "
                    "(WireGuard peer, nftables allowances, mesh peers, certsync, "
                    "rqlite node map), health gate, undrain; check wg no longer "
                    "lists it" % (peer, name) for peer in peers]
        if step == "deregister":
            return ["dns-health.json: forget %s; Cloudflare: delete %s.%s A/AAAA%s"
                    % (name, name, self.domain(job),
                       "; SPF TXT without its addresses" if self.mail_enabled() else ""),
                    "backups, status, autodeploy and roll read the fleet file"]
        if step == "commit":
            return ["git: commit %s without %s on %s%s from %s (plumbing), then merge "
                    "it into main where main is checked out, after a git merge-tree "
                    "check; refused while that file has other staged or unsaved "
                    "changes, and then the merge command is printed" % (
                        FLEET_PATH, name, BRANCH_PREFIX, name, MAIN_REF)]
        if step == "destroy":
            return ["%s: delete %s (%s) after checking label, tag, id and address; "
                    "IRREVERSIBLE; the only step that uses the provider API key" % (
                        job.state.get("provider"), job.state.get("label"),
                        job.state.get("instance_id") or "id unknown")]
        return []

    def domain(self, job):
        try:
            return self.fleet(job)["domain"]
        except (ToolError, OSError, KeyError):
            return "next.forkmesh.com"

    def served_names(self, job):
        import next_dns
        try:
            return next_dns.served_names(self.fleet(job))
        except (ToolError, OSError, KeyError):
            return []


def stop_script(units):
    names = " ".join(units)
    return ("for unit in %s; do systemctl disable --now \"$unit\" >/dev/null 2>&1 "
            "|| true; done\n"
            "for unit in %s; do if systemctl is-active --quiet \"$unit\"; then "
            "echo \"still active: $unit\" >&2; exit 1; fi; done" % (names, names))


def execute_script():
    return "cat >%s\n%s\nstatus=$?\nrm -f %s\nexit $status" % (
        EXECUTE_BODY, common.rqlite_curl("/db/execute", "POST", EXECUTE_BODY),
        EXECUTE_BODY)


def remove_script():
    return "cat >%s\n%s\nstatus=$?\nrm -f %s\nexit $status" % (
        REMOVE_BODY, common.rqlite_curl("/remove", "DELETE", REMOVE_BODY), REMOVE_BODY)


def promote_note(job):
    if not job.state.get("voter", True):
        return "%s is a non-voter" % job.name
    after = job.state.get("voters_after") or []
    if len(after) >= MIN_VOTERS and len(after) % 2 == 1:
        return "the voters left stay odd and at least %d" % MIN_VOTERS
    return "no reachable non-voter core to promote (add a core first)"


def quorum_text(plan):
    if plan.get("live_error"):
        return "rqlite: cannot read the cluster (%s)" % plan["live_error"]
    lines = "rqlite: voters %s; after removing %s: %s (healthy %s); leader %s" % (
        ", ".join(plan.get("voters_before") or []) or "none", plan.get("name"),
        ", ".join(plan.get("voters_after") or []) or "none",
        ", ".join(plan.get("healthy_after") or []) or "none",
        plan.get("leader") or "unknown")
    if not plan.get("voter", True):
        lines += "; %s is a non-voter" % plan.get("name")
    if plan.get("promote"):
        lines += "; %s is promoted from non-voter to voter first" % plan["promote"]
    return lines


def run(job, backend, clock=time.monotonic):
    started = clock()
    steps = steps_for(job.destroy)
    job.emit(event="start", name=job.name, execute=job.execute,
             destroy_instance=job.destroy,
             steps=[{"id": s, "title": t, "estimate": e} for s, t, e in steps])
    for step, title, estimate in steps:
        if job.state.done(step):
            job.emit(event="step", step=step, status="done", resumed=True)
            job.say("== %s: already done" % step)
            continue
        job.say("== %s: %s (about %d s)" % (step, title, estimate))
        job.emit(event="step", step=step, status="running", title=title,
                 estimate=estimate)
        job.state.mark(step, "running")
        begun = clock()
        try:
            getattr(backend, "step_" + step)(job)
        except Exception as error:
            message = str(error) or error.__class__.__name__
            job.state.mark(step, "failed", error=message,
                           seconds=round(clock() - begun, 1))
            job.emit(event="step", step=step, status="failed", error=message)
            try:
                notes = backend.contain(job) or []
            except Exception as contain_error:
                notes = ["containment failed: %s" % contain_error]
            for note in notes:
                job.say(note)
            job.emit(event="result", status="failed", name=job.name, step=step,
                     error=message, notes=notes)
            raise ToolError("%s: step %s failed: %s. Re-run the same command to "
                            "resume from %s." % (job.name, step, message, step)) \
                from error
        seconds = round(clock() - begun, 1)
        job.state.mark(step, "done", seconds=seconds)
        job.emit(event="step", step=step, status="done", seconds=seconds)
    total = round(clock() - started, 1)
    job.state.set(finished=True, finished_at=time.time())
    summary = "%s is out of the fleet%s" % (
        job.name, "; its instance is deleted" if job.state.get("destroyed")
        else "; its instance is kept")
    job.emit(event="result", status="ok", name=job.name, seconds=total,
             summary=summary, branch=job.state.get("branch"),
             commit=job.state.get("commit"),
             merged=job.state.get("merged") or "",
             merge_error=job.state.get("merge_error") or "",
             destroyed=bool(job.state.get("destroyed")))
    job.say("%s (%d s)" % (summary, total))
    return total


def describe(job, backend):
    lines = []
    total = 0
    for index, (step, title, estimate) in enumerate(steps_for(job.destroy), 1):
        total += estimate
        lines.append("%2d. %-10s ~%4d s  %s" % (index, step, estimate, title))
        for detail in backend.describe(job, step):
            lines.append("      - " + detail)
    if not job.destroy:
        lines.append("    the cloud instance %s is kept (add --destroy-instance to "
                     "delete it)" % job.state.get("label"))
    lines.append("estimated total: about %d min (%d s)" % (round(total / 60.0), total))
    return lines


def summary(state):
    if state is None or not state.get("name"):
        return None
    keys = ("name", "label", "provider", "instance_id", "ip", "voter", "leader",
            "destroy_instance", "destroyed", "branch", "commit", "finished", "failed",
            "started_at", "updated_at", "log")
    out = {key: state.get(key) for key in keys if state.get(key) is not None}
    out["steps"] = {step: state.status(step) for step in step_ids(True)}
    out["state_file"] = str(state.path)
    return out


def latest_state():
    directory = state_dir()
    if not directory.is_dir():
        return None
    paths = sorted(directory.glob("n*.json"), key=lambda p: p.stat().st_mtime)
    return State.load(paths[-1]) if paths else None


def in_fleet(fleet_file, name):
    try:
        fleet = common.load_fleet(fleet_file)
    except (ToolError, OSError, KeyError, ValueError):
        return False
    return name in [n["name"] for n in fleet.get("nodes") or []]


def start_state(backend, fleet_file, name, destroy, execute):
    path = state_path(name)
    existing = State.load(path) if path.is_file() else None
    if existing is not None and existing.get("name"):
        if existing.get("finished") and in_fleet(fleet_file, name):
            if execute:
                os.replace(path, path.with_name("%s.%d.json" % (name, time.time())))
            existing = None
        elif existing.get("finished") and (existing.get("destroyed") or not destroy):
            raise ToolError("%s was already removed (%s)%s" % (
                name, path, "" if existing.get("destroyed") else
                "; pass --destroy-instance to delete its instance too"))
    if existing is not None and existing.get("name"):
        if destroy and not existing.get("destroy_instance"):
            existing.data["destroy_instance"] = True
        if not destroy:
            existing.data["destroy_instance"] = False
        if existing.get("finished") and destroy:
            existing.data["finished"] = False
        return existing, True
    planned = backend.plan(fleet_file, name, destroy)
    state = State(path if execute else Path(os.devnull),
                  dict(planned, started_at=time.time(), steps={}))
    return state, False


def main_remove_core(args, ssh=None, cf=None, backend=None, out=None,
                     provider_factory=None):
    runner = common.runner_from_args(args)
    name = check_name(args.target)
    ssh = ssh or common.Ssh()
    backend = backend or RemoveBackend(ssh=ssh, cf=cf, provider_factory=provider_factory)
    fleet_file = Path(args.fleet) if args.fleet else common.fleet_file()
    destroy = bool(getattr(args, "destroy_instance", False))
    lock = common.DeployLock(what="next_fleet remove-core %s" % name)
    if runner.execute:
        import next_fleet
        lock.acquire(wait=next_fleet.ROLL_LOCK_WAIT_SECONDS)
    try:
        state, resumed = start_state(backend, fleet_file, name, destroy, runner.execute)
        job = Job(state, runner, fleet_file=fleet_file, out=out,
                  events=getattr(args, "events", True))
        if runner.execute:
            if not resumed and not state.get("allowed"):
                raise GuardError(state.get("blocker") or "removal refused")
            if runner.log_path:
                state.data["log"] = str(runner.log_path)
            state.save()
            job.say("%s %s (state %s)" % ("resuming the removal of" if resumed
                                         else "removing", name, state.path))
            run(job, backend)
            return 0
        job.events = False
        for line in describe(job, backend):
            job.say(line)
        for warning in state.get("warnings") or []:
            job.say("note: %s" % warning)
        if not resumed and not state.get("allowed"):
            job.say("REFUSED: %s" % state.get("blocker"))
            return 2
        job.say("(dry run: nothing was changed; pass --execute to remove %s)" % name)
        return 0
    finally:
        if runner.execute:
            lock.release()


def main_plan(args, ssh=None, backend=None, out=None):
    out = out or sys.stdout
    name = check_name(args.target)
    destroy = bool(getattr(args, "destroy_instance", False))
    backend = backend or RemoveBackend(ssh=ssh or common.Ssh())
    fleet_file = Path(args.fleet) if args.fleet else common.fleet_file()
    path = state_path(name)
    existing = State.load(path) if path.is_file() else None
    record = summary(existing) if existing is not None else None
    result = {"name": name, "destroy_instance": destroy,
              "estimate_seconds": estimate_seconds(destroy),
              "steps": [{"id": s, "title": t, "estimate": e}
                        for s, t, e in steps_for(destroy)],
              "state": record}
    in_progress = record is not None and not (record.get("finished")
                                              and in_fleet(fleet_file, name))
    if in_progress:
        data = dict(existing.data)
        data["destroy_instance"] = destroy
        result.update({key: value for key, value in data.items()
                       if key not in ("steps", "failed", "log")})
        result["resume"] = True
        result["allowed"] = not (record.get("finished") and
                                 (record.get("destroyed") or not destroy))
        result["blocker"] = "" if result["allowed"] else (
            "%s is already out of the fleet%s" % (
                name, "" if record.get("destroyed") else
                "; tick the instance box to delete it"))
        job = Job(State(Path(os.devnull), data), common.Runner(execute=False),
                  fleet_file=fleet_file)
    else:
        try:
            planned = backend.plan(fleet_file, name, destroy)
        except ToolError as error:
            result.update(allowed=False, blocker=str(error), error=str(error))
            planned = None
        if planned is not None:
            result.update(planned)
            result["resume"] = False
            job = Job(State(Path(os.devnull), dict(planned)), common.Runner(execute=False),
                      fleet_file=fleet_file)
    if result.get("allowed") is not None and "error" not in result:
        result["description"] = describe(job, backend)
        result["quorum"] = quorum_text(job.state.data)
    if args.json:
        print(json.dumps(result, sort_keys=True), file=out)
        return 0
    for line in result.get("description") or []:
        print(line, file=out)
    print("allowed" if result.get("allowed") else "REFUSED: %s" % result.get("blocker"),
          file=out)
    return 0


def main_status(args, out=None):
    out = out or sys.stdout
    state = State.load(state_path(args.name)) if args.name else latest_state()
    result = summary(state)
    if args.json:
        print(json.dumps(result, sort_keys=True), file=out)
        return 0
    if result is None:
        print("no remove-core run recorded in %s" % state_dir(), file=out)
        return 0
    if result.get("finished"):
        status = "finished"
    elif result.get("failed"):
        status = "failed at %s: %s" % (result["failed"]["step"], result["failed"]["error"])
    else:
        status = "in progress"
    print("%s: %s" % (result["name"], status), file=out)
    for step in step_ids(True):
        print("  %-10s %s" % (step, result["steps"][step]), file=out)
    return 0
