#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_cloud  # noqa: E402
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

EVENT = "FMNEXT-ADD-CORE "
BRANCH_PREFIX = "fleet/add-core-"
MAIN_REF = "refs/heads/main"
FLEET_PATH = "deploy/fleet.toml"
KEY_PACKAGES = "wireguard-tools python3-cryptography"
VM_TIMEOUT = 900
SSH_TIMEOUT = 600
JOIN_TIMEOUT = 900
REPOS_TIMEOUT = 1200
HEALTH_TIMEOUT = 300
SYNC_NUDGE_SECONDS = 60
DAEMON_STATUS = "curl -fsS --max-time 10 http://127.0.0.1:8791/v1/status"
DAEMON_SYNC = "curl -fsS --max-time 10 -X POST http://127.0.0.1:8791/v1/control/sync"
RQLITE_READY = ("curl -sS --max-time 5 -K /etc/forkmesh-next/rqlite.curlrc -o /dev/null "
                "-w '%{http_code}' http://127.0.0.1:4001/readyz")

STEPS = (
    ("plan", "Plan: name, region, plan, rqlite role, release", 2),
    ("hold", "Hold the new core out of DNS", 1),
    ("vm", "Create the VM (prefetch the release locally meanwhile)", 120),
    ("fleet", "Add it to deploy/fleet.toml and give it its own DNS name", 10),
    ("keys", "WireGuard and IDS keys on the new core", 40),
    ("mesh", "Existing cores learn the new peer; new core installs", 300),
    ("join", "Start it and join the rqlite cluster", 45),
    ("services", "Mirror daemon and repo prefetch; app release upload", 150),
    ("activate", "Activate the served release and unseal", 60),
    ("repos", "Every served repository synced from its peers", 60),
    ("ids", "Log reader and IDS baseline", 20),
    ("health", "Health checks: readyz, units, IDS, public probes", 20),
    ("dns", "Add it to DNS after the health probes pass", 15),
    ("register", "Node account and owner; monitoring, backups, DNS health loop, autodeploy and roll", 8),
    ("commit", "Commit deploy/fleet.toml to a branch", 2),
)
STEP_IDS = tuple(step for step, _, _ in STEPS)
RECHECK = ("health", "dns")
PARALLEL = {
    "vm": ("create the VM, wait for it to boot and answer SSH",
           "locally: the release tarball at the served rev, the wheelhouse, "
           "the mirror daemon build, the rqlite and Caddy tarballs"),
    "mesh": ("existing cores, one at a time: drain, files+start (WireGuard "
             "peer, mesh peers, IDS keys, node map), health, undrain",
             "new core: base, rqlite binary, files, Caddy, secrets, mail"),
    "services": ("mirror daemon and gateway, then a repository sync from the peers",
                 "release upload and its venv from the wheelhouse"),
}


def estimate_seconds():
    return sum(seconds for _, _, seconds in STEPS)


def choose_voter(voters):
    voters = int(voters)
    if voters < 1:
        raise ToolError("the cluster has no rqlite voter")
    if voters % 2 == 0:
        return True, ("%d voters is even: the new core joins as a voter, making "
                      "%d (odd)" % (voters, voters + 1))
    return False, ("%d voters is odd: a voter would make %d (even), which "
                   "survives no more failures than %d and slows every write, so "
                   "the new core joins as a non-voter (read replica); voters "
                   "stay %d" % (voters, voters + 1, voters, voters))


def core_numbers(names):
    numbers = set()
    for name in names:
        match = re.fullmatch(r"(?:n|fmnext-core-)([0-9]{1,2})", str(name))
        if match:
            numbers.add(int(match.group(1)))
    return numbers


def pick_number(used, limit):
    number = max(set(used) | {0}) + 1
    if number > limit:
        raise GuardError("no free core number: n%d would take a runner's "
                         "WireGuard address (cores stop at n%d)" % (number, limit))
    return number


def aws_placement(instances):
    import next_aws
    cores = [i for label, i in instances.items() if str(label).startswith("fmnext-core-")
             and next_cloud.entry_provider(i) == "aws"]
    regions = Counter(str(i.get("region") or "") for i in cores if i.get("region"))
    plans = Counter(str(i.get("plan") or "") for i in cores if i.get("plan"))
    region = regions.most_common(1)[0][0] if regions else ""
    if not region:
        try:
            region = next_aws.load_credentials()[1]
        except (ToolError, OSError):
            region = next_aws.DEFAULT_REGION
    return region, plans.most_common(1)[0][0] if plans else ""


def default_placement(instances):
    cores = [info for label, info in sorted((instances or {}).items())
             if re.fullmatch(r"fmnext-core-[0-9]{1,2}", label)
             and next_cloud.entry_provider(info) == "vultr"]
    regions = Counter(str(i.get("region") or "") for i in cores if i.get("region"))
    plans = Counter(str(i.get("plan") or "") for i in cores if i.get("plan"))
    region = regions.most_common(1)[0][0] if regions else ""
    plan = plans.most_common(1)[0][0] if plans else ""
    return region, plan


def eligible_plans(plans, min_ram=1024):
    out = []
    for plan in plans or []:
        try:
            cost = float(plan.get("monthly_cost"))
            ram = float(plan.get("ram"))
        except (TypeError, ValueError):
            continue
        plan_id = str(plan.get("id") or "")
        if not plan_id or cost <= 0 or ram < min_ram or plan_id.endswith("-v6") \
                or "-v6-" in plan_id:
            continue
        out.append({"id": plan_id, "ram": int(ram), "monthly_cost": cost,
                    "vcpu_count": int(plan.get("vcpu_count") or 0),
                    "disk": int(plan.get("disk") or 0),
                    "type": str(plan.get("type") or ""),
                    "locations": sorted(str(x).lower() for x in
                                        plan.get("locations") or [])})
    return sorted(out, key=lambda p: (p["monthly_cost"], -p["ram"], p["id"]))


def state_dir():
    return common.config_dir() / "add-core"


def state_path(name):
    if not re.fullmatch(r"n[0-9]{1,2}", str(name)):
        raise GuardError("bad core name %r" % name)
    return state_dir() / ("%s.json" % name)


class State:
    def __init__(self, path, data=None):
        self.path = Path(path)
        self.data = data if data is not None else {}
        self.data.setdefault("steps", {})
        self.lock = threading.Lock()

    @classmethod
    def load(cls, path):
        return cls(path, common.load_json(path, None) or {})

    def save(self):
        if str(self.path) == os.devnull:
            return
        with self.lock:
            self.data["updated_at"] = time.time()
            common.save_json(self.path, self.data)

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, **values):
        self.data.update(values)
        self.save()

    def status(self, step):
        return (self.data["steps"].get(step) or {}).get("status", "pending")

    def done(self, step):
        return self.status(step) == "done"

    def mark(self, step, status, **extra):
        entry = dict(self.data["steps"].get(step) or {})
        entry.update(extra, status=status, at=time.time())
        self.data["steps"][step] = entry
        if status == "failed":
            self.data["failed"] = {"step": step, "error": extra.get("error", "")}
        elif status == "done" and (self.data.get("failed") or {}).get("step") == step:
            self.data.pop("failed", None)
        self.save()

    def items(self, key):
        return list(self.data.get(key) or [])

    def add_item(self, key, value):
        with self.lock:
            items = list(self.data.get(key) or [])
            if value not in items:
                items.append(value)
            self.data[key] = items
        self.save()


def unfinished_states():
    found = []
    directory = state_dir()
    if not directory.is_dir():
        return found
    for path in sorted(directory.glob("n*.json")):
        state = State.load(path)
        if state.get("name") and not state.get("finished"):
            found.append(state)
    return found


def latest_state():
    directory = state_dir()
    if not directory.is_dir():
        return None
    paths = sorted(directory.glob("n*.json"), key=lambda p: p.stat().st_mtime)
    return State.load(paths[-1]) if paths else None


class Job:
    def __init__(self, state, runner, fleet_file=None, out=None, events=True):
        self.state = state
        self.runner = runner
        self.fleet_file = Path(fleet_file) if fleet_file else common.fleet_file()
        self.out = out or sys.stdout
        self.events = events
        self.emit_lock = threading.Lock()

    @property
    def name(self):
        return self.state.get("name")

    @property
    def execute(self):
        return self.runner.execute

    def say(self, message):
        self.runner.say(message)

    def emit(self, **event):
        if not self.events:
            return
        with self.emit_lock:
            print(EVENT + json.dumps(event, sort_keys=True), file=self.out, flush=True)


def parallel(job, branches):
    branches = list(branches)
    if not job.execute or len(branches) < 2:
        return [action() for _, action in branches]
    results = [None] * len(branches)
    errors = []

    def one(index, label, action):
        job.say("[parallel] %s: started" % label)
        try:
            results[index] = action()
        except Exception as error:
            errors.append((index, label, error))
            job.say("[parallel] %s: FAILED: %s" % (label, error))
            return
        job.say("[parallel] %s: done" % label)
    with ThreadPoolExecutor(max_workers=len(branches)) as pool:
        for index, (label, action) in enumerate(branches):
            pool.submit(one, index, label, action)
    if errors:
        _, label, error = sorted(errors, key=lambda item: item[0])[0]
        if isinstance(error, ToolError):
            raise type(error)("%s: %s" % (label, error))
        raise ToolError("%s: %s" % (label, error))
    return results


def run(job, backend, clock=time.monotonic):
    started = clock()
    job.emit(event="start", name=job.name, execute=job.execute,
             steps=[{"id": s, "title": t, "estimate": e} for s, t, e in STEPS])
    for step, title, estimate in STEPS:
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
            notes = []
            try:
                notes = backend.contain(job) or []
            except Exception as contain_error:
                notes = ["containment failed: %s" % contain_error]
            for recheck in RECHECK:
                if job.state.done(recheck):
                    job.state.mark(recheck, "pending")
            for note in notes:
                job.say(note)
            job.emit(event="result", status="failed", name=job.name, step=step,
                     error=message, notes=notes)
            raise ToolError("%s: step %s failed: %s. Re-run the same command "
                            "to resume from %s." % (job.name, step, message, step)) \
                from error
        seconds = round(clock() - begun, 1)
        job.state.mark(step, "done", seconds=seconds)
        job.emit(event="step", step=step, status="done", seconds=seconds)
    total = round(clock() - started, 1)
    job.state.set(finished=True, finished_at=time.time())
    summary = "%s is serving, in DNS" % job.name
    job.emit(event="result", status="ok", name=job.name, seconds=total,
             summary=summary, branch=job.state.get("branch"),
             commit=job.state.get("commit"))
    job.say("%s (%d s)" % (summary, total))
    return total


def describe(job, backend):
    lines = []
    total = 0
    for index, (step, title, estimate) in enumerate(STEPS, 1):
        total += estimate
        lines.append("%2d. %-9s ~%4d s  %s" % (index, step, estimate, title))
        if step in PARALLEL:
            first, second = PARALLEL[step]
            lines.append("      in parallel: [%s] || [%s]" % (first, second))
        for detail in backend.describe(job, step):
            lines.append(("        " if detail.startswith("    ") else "      - ") + detail.strip())
    lines.append("estimated total: about %d min (%d s)" % (
        round(total / 60.0), total))
    return lines


class Git:
    def __init__(self, repo, run=subprocess.run):
        self.repo = Path(repo)
        self._run = run

    def call(self, *args, env=None, input_text=None, check=True):
        completed = self._run(["git", *args], cwd=self.repo, capture_output=True,
                              text=True, input=input_text, check=False,
                              env=env)
        if check and completed.returncode != 0:
            raise ToolError("git %s failed: %s" % (
                " ".join(args[:2]), (completed.stderr or "").strip()[-300:]))
        return completed

    def rev(self, ref):
        completed = self.call("rev-parse", "--verify", "--quiet", ref + "^{commit}",
                              check=False)
        return completed.stdout.strip() if completed.returncode == 0 else ""

    def show(self, ref, path):
        return self.call("show", "%s:%s" % (ref, path)).stdout

    def commit_file(self, branch, base, path, text, message):
        if not re.fullmatch(r"fleet/(?:add|remove)-core-n[0-9]{1,2}", branch):
            raise GuardError("refusing branch %r" % branch)
        ref = "refs/heads/" + branch
        tip = self.rev(ref)
        parent = tip or self.rev(base)
        if not parent:
            raise ToolError("no %s in %s" % (base, self.repo))
        if tip and self.show(tip, path) == text:
            return tip, False
        with tempfile.TemporaryDirectory(prefix="fmnext-fleet-commit-") as tmp:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"))
            self.call("read-tree", parent, env=env)
            blob = self.call("hash-object", "-w", "--stdin", input_text=text,
                             env=env).stdout.strip()
            self.call("update-index", "--add", "--cacheinfo",
                      "100644,%s,%s" % (blob, path), env=env)
            tree = self.call("write-tree", env=env).stdout.strip()
        commit = self.call("commit-tree", tree, "-p", parent, "-m", message
                           ).stdout.strip()
        self.call("update-ref", ref, commit, tip or "0" * 40)
        return commit, True


def fleet_state_path():
    return common.config_dir() / "fleet-state.json"


class FleetBackend:
    def __init__(self, vultr=None, ssh=None, cf=None, repo=None, sleep=time.sleep,
                 clock=time.monotonic, git=None, prober=None):
        self.prober = prober
        self.vultr = vultr
        self.clouds = {}
        self.ssh = ssh
        self._cf = cf
        self.repo = Path(repo or common.REPO)
        self.sleep = sleep
        self.clock = clock
        self.git = git or Git(self.repo)

    def fleet(self, job):
        return common.load_fleet(job.fleet_file)

    def provider(self, job):
        return next_cloud.check_name(job.state.get("provider"))

    def cloud(self, job, region=None):
        name = self.provider(job)
        if self.vultr is not None and next_cloud.provider_name(self.vultr) == name:
            return self.vultr
        if name == "vultr":
            return self.vultr
        if name not in self.clouds:
            self.clouds[name] = next_cloud.from_config(
                name, region=region or job.state.get("region"))
        return self.clouds[name]

    def cf(self, fleet):
        import next_dns
        if self._cf is None:
            self._cf = next_dns.Cloudflare.from_config(
                allowed=next_dns.core_addresses(fleet))
        else:
            self._cf.allowed = next_dns.core_addresses(fleet)
        return self._cf

    def peers(self, fleet, job):
        return [n["name"] for n in fleet.get("nodes") or [] if n["name"] != job.name]

    def voter(self, job):
        return bool(job.state.get("voter"))

    def ip(self, job):
        return job.state.get("ip") or ""

    def mail_enabled(self):
        import next_mail
        return next_mail.key_path().is_file()

    def configured_voters(self, fleet, exclude=()):
        import render
        return sorted(str(n.get("rqlite_id") or n["name"]) for n in fleet.get("nodes") or []
                      if n["name"] not in exclude and render.rqlite_voter(n))

    def live_voters(self, fleet, exclude=()):
        import next_fleet
        nodes = next_fleet.cluster_nodes(fleet, self.ssh, exclude=exclude)
        return sorted(str(n.get("id")) for n in nodes if n.get("voter", True))

    def served_rev(self, fleet, job):
        import next_deploy
        manifest = common.load_manifest()
        rev = str(manifest.get("current") or "")
        if not job.execute:
            return rev
        live = {}
        for node in fleet.get("nodes") or []:
            if node["name"] == job.name:
                continue
            state = next_deploy.node_state(self.ssh, node["public_ipv4"])
            if state.get("error") or not next_deploy.activated(state, state.get("current")):
                raise ToolError("%s is not settled on one release (%s); wait for "
                                "the running deploy" % (node["name"],
                                                        next_deploy.state_text(state)))
            live[node["name"]] = state["current"]
        revs = sorted(set(live.values()))
        if len(revs) != 1:
            raise ToolError("the cores run different releases (%s); wait for the "
                            "deploy to finish" % ", ".join(
                                "%s %s" % item for item in sorted(live.items())))
        if rev and rev != revs[0]:
            job.say("deploy-manifest.json says %s but every core runs %s; using %s"
                    % (rev, revs[0], revs[0]))
        return revs[0]

    def plan(self, job, region=None, plan=None, snapshot=None, name=None,
             provider=None):
        import next_fleet
        provider = next_cloud.check_name(provider)
        fleet = self.fleet(job)
        instances = common.load_json(next_fleet.instances_path(), {}) or {}
        used = core_numbers([n["name"] for n in fleet.get("nodes") or []])
        used |= core_numbers(instances)
        used |= core_numbers(s.get("name") for s in unfinished_states())
        if self.vultr is not None:
            try:
                used |= core_numbers(i.get("label") for i in self.vultr.fleet_instances())
            except (ToolError, OSError) as error:
                if job.execute:
                    raise
                job.say("Vultr instance list unavailable (%s); numbers from the "
                        "local files only" % error)
        if provider == "aws":
            import next_aws
            if snapshot:
                raise ToolError("--snapshot is Vultr-only")
            default_region, default_plan = aws_placement(instances)
            region = next_aws.check_region(region or default_region)
            plan = plan or default_plan or next_aws.cheapest_type()
            if not next_aws.TYPE_RE.match(plan):
                raise ToolError("bad instance type %r" % plan)
        else:
            default_region, default_plan = default_placement(instances)
            region = (region or default_region or next_fleet.REGION).lower()
            plan = plan or default_plan
            if not re.fullmatch(r"[a-z]{3,4}", region):
                raise ToolError("bad region %r" % region)
            if plan and not re.fullmatch(r"[a-z0-9-]{3,40}", plan):
                raise ToolError("bad plan %r" % plan)
        if provider != "vultr":
            try:
                cloud = self.clouds.get(provider) or next_cloud.from_config(
                    provider, region=region)
                self.clouds[provider] = cloud
                used |= core_numbers(i.get("label") for i in cloud.fleet_instances())
            except (ToolError, OSError) as error:
                if job.execute:
                    raise
                job.say("%s instance list unavailable (%s); numbers from the "
                        "local files only" % (next_cloud.display(provider), error))
        if name:
            number = int(next_fleet.parse_target(name)[1])
            if number in core_numbers([n["name"] for n in fleet.get("nodes") or []]):
                raise GuardError("%s is already in %s" % (name, job.fleet_file))
        else:
            number = pick_number(used, next_fleet.RUNNER_WG_OFFSET)
        voters = self.configured_voters(fleet)
        if job.execute:
            live = self.live_voters(fleet)
            if live != voters:
                raise GuardError("rqlite voters %s do not match %s's %s; settle "
                                 "the cluster first" % (", ".join(live) or "none",
                                                        job.fleet_file,
                                                        ", ".join(voters)))
        voter, reason = choose_voter(len(voters))
        rev = self.served_rev(fleet, job)
        if not rev and job.execute:
            raise ToolError("no served release recorded; deploy the fleet first")
        return {"name": "n%d" % number, "number": number,
                "label": next_fleet.label_for("core", number), "region": region,
                "provider": provider,
                "plan": plan, "snapshot": snapshot or "", "voter": voter,
                "voter_reason": reason, "voters_before": sorted(voters),
                "rev": rev, "peers": self.peers(fleet, job),
                "fleet_file": str(job.fleet_file)}

    def step_plan(self, job):
        for key in ("name", "region", "rev"):
            if not job.state.get(key):
                raise ToolError("the plan has no %s" % key)
        job.say("%s: %s on %s in %s, plan %s; rqlite: %s; release %s" % (
            job.name, job.state.get("label"),
            next_cloud.display(self.provider(job)), job.state.get("region"),
            job.state.get("plan") or "(cheapest)", job.state.get("voter_reason"),
            job.state.get("rev")))

    def step_hold(self, job):
        import next_dns
        next_dns.hold_joining(self.fleet(job), job.runner, job.name)

    def step_vm(self, job):
        parallel(job, [("VM", lambda: self.create_vm(job)),
                       ("prefetch", lambda: self.prefetch(job))])

    def create_vm(self, job):
        import next_fleet
        number = int(job.state.get("number"))
        cloud = self.cloud(job)
        aws = self.provider(job) == "aws"
        created = next_fleet.create(
            "core", number, cloud, job.runner, plan_id=job.state.get("plan") or None,
            region=job.state.get("region"), snapshot_id=job.state.get("snapshot") or None,
            enable_ipv6=True, fleet=self.fleet(job) if aws else None)
        if not job.execute:
            return None
        instance_id = (created or {}).get("id")
        if not instance_id:
            raise ToolError("%s returned no instance id" % next_cloud.display(
                self.provider(job)))
        job.state.set(instance_id=instance_id)

        def live():
            instance = cloud.instance(instance_id)
            next_fleet.guard_instance(instance)
            if aws:
                cloud.guard(instance)
                ready = (instance.get("status") == "running"
                         and instance.get("main_ip") not in (None, "", "0.0.0.0"))
            else:
                ready = (instance.get("status") == "active" and instance.get("main_ip")
                         not in (None, "", "0.0.0.0") and next_fleet.instance_ipv6(instance))
            return instance if ready else None
        instance = common.wait_for(live, VM_TIMEOUT, interval=10, sleep=self.sleep,
                                   clock=self.clock)
        if not instance:
            raise ToolError("%s did not become active with %s in %d s"
                            % (job.state.get("label"),
                               "a public IPv4" if aws else "IPv4 and IPv6", VM_TIMEOUT))
        if instance.get("region") and instance["region"] != job.state.get("region"):
            raise GuardError("%s is in %s, not %s" % (
                instance["label"], instance["region"], job.state.get("region")))
        next_fleet.record_instance(instance)
        ip = instance["main_ip"]
        if job.state.get("ip") != ip:
            self.forget_host_key(ip)
        job.state.set(ip=ip, ipv6=next_fleet.instance_ipv6(instance),
                      plan=instance.get("plan") or job.state.get("plan"))
        if not common.wait_for(lambda: self.ssh.run(ip, "true", check=False,
                                                    timeout=30).returncode == 0,
                               SSH_TIMEOUT, interval=10, sleep=self.sleep,
                               clock=self.clock):
            raise ToolError("%s (%s) never answered SSH" % (job.name, ip))
        job.say("%s: %s is up (%s, IPv6 %s)" % (job.name, instance["label"], ip,
                                                 job.state.get("ipv6")))
        return instance

    def forget_host_key(self, ip):
        known = common.config_dir() / "known_hosts"
        if known.is_file() and ip in known.read_text("utf-8", "replace"):
            subprocess.run(["ssh-keygen", "-R", ip, "-f", str(known)],
                           capture_output=True, check=False)

    def release_tarball(self, job, fleet):
        import next_deploy
        rev = job.state.get("rev")
        tarball = next_deploy.dist_dir() / ("fmnext-%s.tar.gz" % rev)
        if tarball.is_file():
            return tarball
        for node in fleet.get("nodes") or []:
            if node["name"] == job.name:
                continue
            ip = node["public_ipv4"]
            path = "%s/%s.tar.gz" % (next_deploy.RELEASES, rev)
            sha = self.ssh.run(ip, "cat %s.sha256" % path, check=False, timeout=60)
            digest = (sha.stdout or b"").decode().split()[:1]
            if sha.returncode != 0 or not digest:
                continue
            data = self.ssh.run(ip, "cat %s" % path, check=False, timeout=900)
            import hashlib
            if data.returncode != 0 or hashlib.sha256(data.stdout).hexdigest() != digest[0]:
                continue
            tarball.parent.mkdir(parents=True, exist_ok=True)
            partial = tarball.with_name(tarball.name + ".part")
            partial.write_bytes(data.stdout)
            os.replace(partial, tarball)
            next_deploy.sha_path(tarball).write_text(digest[0] + "\n")
            job.say("release %s copied from %s (sha256 %s...)" % (
                rev, node["name"], digest[0][:12]))
            return tarball
        raise ToolError("release %s is neither in %s nor on any core" % (
            rev, next_deploy.dist_dir()))

    def prefetch(self, job):
        import next_deploy
        import next_fleet
        if not job.execute:
            return None
        fleet = self.fleet(job)
        tarball = self.release_tarball(job, fleet)
        wheels = next_deploy.cached_wheelhouse()
        next_deploy.wheelhouse_tarball(wheels)
        version, sha256, rqlite = next_fleet.rqlite_tarball(fleet)
        import hashlib
        if hashlib.sha256(rqlite.read_bytes()).hexdigest() != sha256:
            raise GuardError("%s does not match fleet.toml" % rqlite)
        next_fleet.caddy_release(fleet, required=True)
        dist = next_deploy.dist_dir()
        dist.mkdir(parents=True, exist_ok=True)
        subprocess.run(["go", "build", "-trimpath", "-o", str(dist / "forkmesh-mirror-node"),
                        "./cmd/forkmesh-mirror-node"], cwd=common.REPO / "server",
                       check=True, env=dict(os.environ, CGO_ENABLED="0", GOOS="linux",
                                            GOARCH="amd64"))
        job.say("prefetched: %s, wheelhouse %s, rqlite %s, Caddy, mirror daemon"
                % (tarball.name, wheels.name, version))
        return tarball

    def node_block(self, job):
        import next_fleet
        instances = common.load_json(next_fleet.instances_path(), {}) or {}
        info = instances.get(job.state.get("label"))
        if not info:
            raise ToolError("%s is not in %s" % (job.state.get("label"),
                                                 next_fleet.instances_path()))
        pubkeys = (common.load_json(fleet_state_path(), {}) or {}).get("wg_pubkeys") or {}
        return next_fleet.core_block(int(job.state.get("number")), job.state.get("label"),
                                     info, pubkeys.get(job.name, ""),
                                     voter=self.voter(job))

    def write_fleet(self, job):
        import next_fleet
        import render
        block = self.node_block(job)
        current = job.fleet_file.read_text("utf-8")
        text = next_fleet.upsert_core_block(current, job.name, block)
        with tempfile.TemporaryDirectory(prefix="fmnext-fleet-") as tmp:
            trial = Path(tmp) / "fleet.toml"
            trial.write_text(text, "utf-8")
            render.load_fleet(trial, fleet_state_path())
        if text != current:
            job.fleet_file.write_text(text, "utf-8")
            job.say("%s: %s updated" % (job.name, job.fleet_file))
        return block

    def record_suffrage(self, job):
        path = fleet_state_path()
        state = common.load_json(path, {}) or {}
        nonvoters = [n for n in state.get("rqlite_nonvoters") or [] if n != job.name]
        if not self.voter(job):
            nonvoters.append(job.name)
        if sorted(nonvoters) != sorted(state.get("rqlite_nonvoters") or []):
            state["rqlite_nonvoters"] = sorted(nonvoters)
            common.save_json(path, state)

    def step_fleet(self, job):
        if not job.execute:
            return
        self.record_suffrage(job)
        self.write_fleet(job)
        fleet = self.fleet(job)
        import next_dns
        next_dns.ensure_node_name(fleet, self.cf(fleet), job.runner, job.name)
        if self.mail_enabled() and self.provider(job) != "vultr":
            job.say("%s: no PTR records (reverse DNS is set by Vultr only; on AWS "
                    "it needs an Elastic IP and an AWS request)" % job.name)
        elif self.mail_enabled():
            import next_mail
            node = next(n for n in fleet["nodes"] if n["name"] == job.name)
            for address in (node["public_ipv4"], node.get("public_ipv6")):
                if address:
                    job.runner.step("PTR %s -> %s" % (address, next_mail.mail_host(node)),
                                    lambda a=address: self.vultr.set_reverse(
                                        node["instance_id"], a, next_mail.mail_host(node)))

    def step_keys(self, job):
        import next_fleet
        fleet = self.fleet(job)
        ip = self.ip(job)
        job.runner.step("%s: apt install %s (the key generators)" % (job.name, KEY_PACKAGES),
                        lambda: self.ssh.run(ip, "export DEBIAN_FRONTEND=noninteractive\n"
                                             "apt-get -o DPkg::Lock::Timeout=900 update -qq\n"
                                             "apt-get -o DPkg::Lock::Timeout=900 install -y "
                                             "-qq --no-install-recommends %s" % KEY_PACKAGES,
                                             timeout=900))
        next_fleet.install(fleet, job.name, self.ssh, job.runner, ("wireguard", "ids"),
                           vultr=self.vultr, replacement=self.voter(job))
        if job.execute:
            self.write_fleet(job)

    def step_mesh(self, job):
        parallel(job, [("existing cores", lambda: self.mesh_peers(job)),
                       (job.name, lambda: self.install_new(job))])

    def mesh_peers(self, job):
        import next_fleet
        fleet = self.fleet(job)
        done = set(job.state.items("peers_done"))
        for peer in self.peers(fleet, job):
            if peer in done:
                job.say("%s: already knows %s" % (peer, job.name))
                continue
            next_fleet.roll(fleet, [peer], self.ssh, job.runner, steps=("files", "start"),
                            vultr=self.vultr, fleet_path=job.fleet_file,
                            sleep=self.sleep, clock=self.clock)
            if job.execute:
                job.state.add_item("peers_done", peer)

    def install_new(self, job):
        import next_fleet
        fleet = self.fleet(job)
        next_fleet.install(fleet, job.name, self.ssh, job.runner,
                           ("base", "rqlite", "files", "caddy", "secrets"),
                           vultr=self.vultr, replacement=self.voter(job))
        if self.mail_enabled():
            import next_mail
            next_mail.install(fleet, job.name, self.ssh, job.runner)
            next_mail.dns(fleet, job.runner)

    def step_join(self, job):
        import next_fleet
        fleet = self.fleet(job)
        next_fleet.install(fleet, job.name, self.ssh, job.runner, ("start",),
                           vultr=self.vultr, replacement=self.voter(job))
        role = "voter" if self.voter(job) else "non-voter"
        if not job.execute:
            job.say("[dry-run] wait until %s is a reachable rqlite %s" % (job.name, role))
            return
        node = next(n for n in fleet["nodes"] if n["name"] == job.name)
        before = set(job.state.get("voters_before") or [])

        def joined():
            ready = self.ssh.run(self.ip(job), RQLITE_READY, check=False, timeout=30)
            if (ready.stdout or b"").strip() != b"200":
                return None
            try:
                nodes = next_fleet.cluster_nodes(fleet, self.ssh, exclude=(job.name,))
            except ToolError:
                return None
            mine = [n for n in nodes if str(n.get("id")) == node["rqlite_id"]]
            if not mine or not mine[0].get("reachable"):
                return None
            if bool(mine[0].get("voter", True)) != self.voter(job):
                raise GuardError("%s joined as a %s, not a %s" % (
                    job.name, "voter" if mine[0].get("voter", True) else "non-voter",
                    role))
            voters = set(next_fleet.healthy_voters(nodes))
            if not before <= voters:
                return None
            return sorted(voters)
        voters = common.wait_for(joined, JOIN_TIMEOUT, interval=5, sleep=self.sleep,
                                 clock=self.clock)
        if not voters:
            raise ToolError("%s did not join rqlite as a %s in %d s" % (
                job.name, role, JOIN_TIMEOUT))
        job.say("%s joined rqlite as a %s; healthy voters %s" % (
            job.name, role, ", ".join(voters)))

    def step_services(self, job):
        parallel(job, [("mirror daemon", lambda: self.daemon(job)),
                       ("release upload", lambda: self.upload(job))])

    def daemon(self, job):
        import next_fleet
        fleet = self.fleet(job)
        next_fleet.install(fleet, job.name, self.ssh, job.runner,
                           ("daemon", "files", "start"), vultr=self.vultr,
                           replacement=self.voter(job))
        job.runner.step("%s: ask the mirror daemon to sync every served repository "
                        "from its peers" % job.name,
                        lambda: self.ssh.run(self.ip(job), DAEMON_SYNC, check=False,
                                             timeout=60))

    def upload(self, job):
        import next_deploy
        fleet = self.fleet(job)
        rev = job.state.get("rev")
        tarball = next_deploy.dist_dir() / ("fmnext-%s.tar.gz" % rev)
        if job.execute and not tarball.is_file():
            tarball = self.release_tarball(job, fleet)
        next_deploy.upload(fleet, rev, tarball, self.ssh, job.runner, [job.name])

    def step_activate(self, job):
        import next_deploy
        import next_vault
        fleet = self.fleet(job)
        rev = job.state.get("rev")
        done = next_deploy.restart(fleet, rev, self.ssh, job.runner, [job.name])
        if job.execute and done != [job.name]:
            raise ToolError("%s did not activate %s" % (job.name, rev))
        if next_deploy.vault_on(fleet):
            if next_vault.unseal(fleet, self.ssh, job.runner, [job.name],
                                 if_sealed=True) != 0:
                raise ToolError("%s is still sealed" % job.name)

    def daemon_status(self, ip):
        completed = self.ssh.run(ip, DAEMON_STATUS, check=False, timeout=60)
        if completed.returncode != 0:
            return None
        try:
            return json.loads(completed.stdout or b"{}")
        except ValueError:
            return None

    def peer_repositories(self, fleet, job):
        for node in fleet.get("nodes") or []:
            if node["name"] == job.name:
                continue
            status = self.daemon_status(node["public_ipv4"])
            if status and status.get("lastSyncOk"):
                return node["name"], set((status.get("repositories") or {}).keys())
        raise ToolError("no peer core reports its served repositories")

    def step_repos(self, job):
        if not job.execute:
            job.say("[dry-run] compare %s's synced repositories with a peer's, "
                    "nudging a sync every %d s" % (job.name, SYNC_NUDGE_SECONDS))
            return
        fleet = self.fleet(job)
        peer, wanted = self.peer_repositories(fleet, job)
        ip = self.ip(job)
        last = {"nudge": self.clock(), "missing": sorted(wanted)}

        def synced():
            status = self.daemon_status(ip) or {}
            have = set((status.get("repositories") or {}).keys())
            last["missing"] = sorted(wanted - have)
            if status.get("lastSyncOk") and not last["missing"]:
                return len(have)
            if self.clock() - last["nudge"] >= SYNC_NUDGE_SECONDS:
                self.ssh.run(ip, DAEMON_SYNC, check=False, timeout=60)
                last["nudge"] = self.clock()
            return None
        count = common.wait_for(synced, REPOS_TIMEOUT, interval=5, sleep=self.sleep,
                                clock=self.clock)
        if not count:
            raise ToolError("%s is missing %d of %s's %d repositories after %d s: %s"
                            % (job.name, len(last["missing"]), peer, len(wanted),
                               REPOS_TIMEOUT, ", ".join(last["missing"][:10])))
        job.say("%s serves all %d repositories %s serves" % (job.name, len(wanted), peer))

    def step_ids(self, job):
        import next_fleet
        import next_ids
        fleet = self.fleet(job)
        if (fleet.get("logs") or {}).get("reader_public_key"):
            next_fleet.install(fleet, job.name, self.ssh, job.runner, ("logread",),
                               vultr=self.vultr, replacement=self.voter(job))
        if next_ids.baseline(fleet, self.ssh, job.runner, job.name) != 0:
            raise ToolError("%s: the IDS baseline was refused" % job.name)

    def hosts(self, fleet, job):
        import next_autodeploy
        text = job.fleet_file.read_text("utf-8")
        hosts = next_autodeploy.fleet_hosts(text)
        hosts["nodes"] = [n for n in hosts["nodes"] if n["name"] == job.name]
        return hosts

    def step_health(self, job):
        import next_autodeploy
        import next_fleet
        fleet = self.fleet(job)
        if not job.execute:
            job.say("[dry-run] %s: readyz 200, forkmesh-app and forkmesh-mirror-node "
                    "active, IDS normal; then from here: app readyz, /api/version "
                    "= %s, %s.%s readyz, /dashboard, /login and the World page"
                    % (job.name, job.state.get("rev"), job.name, fleet["domain"]))
            return
        ip = self.ip(job)
        notes = {"inside": "", "outside": []}

        def healthy():
            ok, notes["inside"] = next_fleet.roll_healthy(self.ssh, ip)
            if not ok:
                return False
            notes["outside"] = next_autodeploy.verify_once(
                self.hosts(fleet, job), job.state.get("rev"))
            return not notes["outside"]
        if not common.wait_for(healthy, HEALTH_TIMEOUT, interval=5, sleep=self.sleep,
                               clock=self.clock):
            raise ToolError("%s is not healthy: %s%s" % (
                job.name, notes["inside"], "; " + "; ".join(notes["outside"])
                if notes["outside"] else ""))
        job.say("%s: %s; public probes pass" % (job.name, notes["inside"]))

    def step_dns(self, job):
        import next_dns
        fleet = self.fleet(job)
        if not job.execute:
            job.say("[dry-run] %s: %d probes of https://%s/readyz on its IPv4 and "
                    "IPv6, then add it to %s" % (
                        job.name, next_dns.UNDRAIN_PROBES, next_dns.probe_host(fleet),
                        ", ".join(next_dns.served_names(fleet))))
            return
        if next_dns.node_entry(job.name).get("in_dns"):
            job.say("%s is already in DNS" % job.name)
            return
        extra = {"prober": self.prober} if self.prober else {}
        next_dns.hold(fleet, self.cf(fleet), job.runner, job.name, False,
                      sleep=self.sleep, **extra)
        if not next_dns.node_entry(job.name).get("in_dns"):
            raise ToolError("%s failed a DNS health probe and was not added; it "
                            "stays out of DNS" % job.name)

    def step_register(self, job):
        import next_dns
        fleet = self.fleet(job)
        names = [n["name"] for n in fleet.get("nodes") or []]
        if job.name not in names:
            raise ToolError("%s is not in %s" % (job.name, job.fleet_file))
        entry = next_dns.node_entry(job.name)
        if job.execute and (not entry.get("in_dns") or entry.get("held")):
            raise ToolError("%s is not a released member of the DNS health loop" % job.name)
        self.register_account(job, fleet)
        unit = subprocess.run(["systemctl", "--user", "is-active",
                               "fmnext-dns-health.service"], capture_output=True,
                              text=True, check=False).stdout.strip()
        job.say("DNS health loop (fmnext-dns-health.service: %s) probes %s from %s"
                % (unit or "unknown", ", ".join(names), job.fleet_file))
        job.say("backups (next_backup.py) read any core of the fleet file; "
                "the Network tab lists %s from fleet-instances.json" % job.state.get("label"))
        job.say("autodeploy and roll read deploy/fleet.toml on main: they include "
                "%s once the commit below is merged" % job.name)

    def register_account(self, job, fleet):
        import next_node_owner
        account = next_node_owner.NODE_PREFIX + job.name
        if not job.execute:
            job.say("[dry-run] %s: the node key signs a reserve and a finalize; "
                    "%s creates the node account, then the node is linked to "
                    "the other cores' owner with this desktop's key" % (
                        account, next_node_owner.APP_URL))
            return
        node = dict(name=job.name, public_ipv4=self.ip(job))
        db = next_node_owner.Db(fleet, self.ssh, next_node_owner.keyset())
        next_node_owner.provision_core(db, self.ssh, job.runner, fleet, node,
                                       next_node_owner.APP_URL)

    def step_commit(self, job):
        branch = BRANCH_PREFIX + job.name
        if not job.execute:
            job.say("[dry-run] commit %s with the %s block on %s (base %s), "
                    "not pushed" % (FLEET_PATH, job.name, branch, MAIN_REF))
            return
        block = self.node_block(job)
        base_text = self.git.show(self.git.rev(MAIN_REF), FLEET_PATH)
        import next_fleet
        text = next_fleet.upsert_core_block(base_text, job.name, block)
        message = ("Add core node %s (%s) to deploy/fleet.toml\n\n"
                   "Created by tools/next_fleet.py add-core: rqlite %s, release %s.\n"
                   % (job.name, job.state.get("label"),
                      "voter" if self.voter(job) else "non-voter",
                      job.state.get("rev")))
        commit, created = self.git.commit_file(branch, MAIN_REF, FLEET_PATH, text, message)
        job.state.set(branch=branch, commit=commit)
        job.say("%s %s on %s: merge it into main so autodeploy and roll include %s:\n"
                "  git -C %s merge --autostash --no-ff %s" % (
                    "committed" if created else "already committed", commit[:12],
                    branch, job.name, self.repo, branch))

    def contain(self, job):
        import next_dns
        notes = []
        name = job.name
        if not name:
            return notes
        entry = next_dns.node_entry(name)
        if entry.get("in_dns") and job.execute:
            fleet = self.fleet(job)
            next_dns.hold(fleet, self.cf(fleet), job.runner, name, True)
            notes.append("%s drained: removed from DNS and held" % name)
        elif job.execute:
            try:
                next_dns.hold_joining(self.fleet(job), job.runner, name)
            except ToolError as error:
                notes.append("could not hold %s: %s" % (name, error))
        notes.append("%s is out of DNS and held; the VM %s is kept for the retry "
                     "(next_fleet.py destroy %s --yes --execute removes it)" % (
                         name, job.state.get("instance_id") or "(not created)", name))
        held = []
        data = common.load_json(next_dns.state_path(), {}) or {}
        for other, item in sorted((data.get("nodes") or {}).items()):
            if other != name and not item.get("in_dns"):
                held.append(other)
        if held:
            notes.append("out of DNS as well: %s (a peer update stops with the peer "
                         "drained; check it, then next_dns.py undrain <node> "
                         "--execute)" % ", ".join(held))
        return notes

    def describe(self, job, step):
        import next_fleet
        name = job.name
        voter = self.voter(job)
        role = "voter" if voter else "non-voter"
        peers = job.state.get("peers") or []
        if step == "plan":
            return ["name %s (%s), region %s, plan %s%s" % (
                        name, job.state.get("label"), job.state.get("region"),
                        job.state.get("plan") or "(cheapest >= 1 GiB)",
                        ", snapshot %s" % job.state.get("snapshot")
                        if job.state.get("snapshot") else ", Debian 13 image"),
                    "rqlite: %s (voters now: %s)" % (
                        job.state.get("voter_reason"),
                        ", ".join(job.state.get("voters_before") or []) or "none"),
                    "release: %s (the rev every core serves)" % job.state.get("rev"),
                    "state file: %s (a re-run resumes from the failed step)"
                    % state_path(name)]
        if step == "hold":
            import next_dns
            return ["%s: %s held, joining, out of DNS before it is in the fleet "
                    "file, so the health loop never adds it early" % (
                        next_dns.state_path(), name)]
        if step == "vm" and self.provider(job) == "aws":
            return ["AWS: create %s (tags Name=%s %s=1, %s, %s, Debian 13 AMI, "
                    "IMDSv2 only, encrypted gp3, security group mirroring nftables), "
                    "adopt it if it exists; wait until running, record it in "
                    "fleet-instances.json, wait for SSH" % (
                        job.state.get("label"), job.state.get("label"), next_fleet.TAG,
                        job.state.get("region"), job.state.get("plan"))]
        if step == "vm":
            return ["Vultr: create %s (tag %s, %s, %s, IPv6 on, backups off), adopt "
                    "it if it exists; wait until active, record it in "
                    "fleet-instances.json, wait for SSH" % (
                        job.state.get("label"), next_fleet.TAG,
                        job.state.get("region"), job.state.get("plan"))]
        if step == "fleet":
            lines = ["fleet-state.json: %s" % (
                "voter (no rqlite_nonvoters entry)" if voter
                else "rqlite_nonvoters += %s" % name),
                "%s: insert the [[nodes]] block below (the rest of the file "
                "is untouched)" % job.fleet_file]
            lines += ["    " + line for line in self.preview_block(job).splitlines()]
            lines += ["Cloudflare: %s.%s A/AAAA -> its addresses (its own name for "
                      "its certificate; not the served names)" % (
                          name, self.domain(job))]
            if self.mail_enabled() and self.provider(job) == "vultr":
                lines.append("Vultr PTR for its addresses -> %s.next.forkmesh.com" % name)
            return lines
        if step == "keys":
            return ["%s: apt install %s; install --steps wireguard,ids (private keys "
                    "stay on the node); public keys into fleet-state.json and the "
                    "fleet file" % (name, KEY_PACKAGES)]
        if step == "mesh":
            lines = ["%s: next_fleet.py roll %s --steps files,start: drain, "
                     "files+start, health gate, undrain (one at a time)" % (
                         peer, peer) for peer in peers]
            lines.append("%s: install --steps base,rqlite,files,caddy,secrets%s"
                         % (name, " --replacement" if voter else ""))
            if self.mail_enabled():
                lines.append("%s: next_mail.py install %s; SPF TXT with its "
                             "addresses" % (name, name))
            return lines
        if step == "join":
            return ["%s: install --steps start (wg0, rqlite -join %s%s, Caddy, "
                    "certsync, IDS; the app waits for its release)" % (
                        name, ",".join(peers), "" if voter else " -raft-non-voter"),
                    "wait (<= %d s) for rqlite /readyz on %s and a reachable %s in "
                    "/nodes; voters %s stay healthy" % (
                        JOIN_TIMEOUT, name, role,
                        ", ".join(job.state.get("voters_before") or []))]
        if step == "services":
            return ["%s: install --steps daemon,files,start, then POST "
                    "/v1/control/sync" % name,
                    "%s: next_deploy.py upload %s --rev %s (wheelhouse and venv "
                    "caches reused)" % (name, name, job.state.get("rev"))]
        if step == "activate":
            return ["%s: next_deploy.py restart %s --rev %s (IDS expectation, "
                    "readyz + /api/version + Caddy gate, DATA_KEY on SSH stdin), "
                    "then next_vault.py unseal --if-sealed" % (
                        name, name, job.state.get("rev"))]
        if step == "repos":
            return ["compare /v1/status repositories with a peer's; nudge a sync "
                    "every %d s; <= %d s" % (SYNC_NUDGE_SECONDS, REPOS_TIMEOUT)]
        if step == "ids":
            return ["%s: install --steps logread; next_ids.py baseline %s" % (name, name)]
        if step == "health":
            return ["%s: readyz 200, forkmesh-app and forkmesh-mirror-node active, "
                    "IDS normal" % name,
                    "from here with --resolve: app readyz, /api/version = %s, "
                    "%s.%s/readyz, /dashboard, /login, the World page" % (
                        job.state.get("rev"), name, self.domain(job))]
        if step == "dns":
            import next_dns
            return ["next_dns.py undrain %s: %d probes on IPv4 and IPv6, then add it "
                    "to %s" % (name, next_dns.UNDRAIN_PROBES, ", ".join(
                        self.served_names(job)))]
        if step == "register":
            return ["node account fmnext-%s: the node key signs a reserve and a "
                    "finalize, the app stores its public key (no account = HTTP 401 "
                    "on every catalog, replica and event call)" % name,
                    "owner: linked to the other cores' owner, accepted with this "
                    "desktop's identity key when it is that owner's key",
                    "DNS health loop: %s released (in_dns, not held)" % name,
                    "backups and status: read from the fleet file and "
                    "fleet-instances.json; nothing else to add",
                    "autodeploy and roll: read deploy/fleet.toml on main"]
        if step == "commit":
            return ["git: commit %s on %s%s from %s (plumbing: HEAD, index and "
                    "working tree untouched); print the merge command" % (
                        FLEET_PATH, BRANCH_PREFIX, name, MAIN_REF)]
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

    def preview_block(self, job):
        import next_fleet
        instances = common.load_json(next_fleet.instances_path(), {}) or {}
        info = instances.get(job.state.get("label")) or {
            "id": "<new instance id>", "ip": job.state.get("ip") or "0.0.0.0"}
        pubkeys = (common.load_json(fleet_state_path(), {}) or {}).get("wg_pubkeys") or {}
        return next_fleet.core_block(int(job.state.get("number")),
                                     job.state.get("label"), info,
                                     pubkeys.get(job.name, "<generated on the node>"),
                                     voter=self.voter(job))


def options(backend, job, vultr=None, provider=None):
    import next_fleet
    provider = next_cloud.check_name(provider)
    instances = common.load_json(next_fleet.instances_path(), {}) or {}
    if provider == "aws":
        import next_aws
        region, plan = aws_placement(instances)
        plan = plan or next_aws.cheapest_type()
    else:
        region, plan = default_placement(instances)
    result = {"estimate_seconds": estimate_seconds(),
              "steps": [{"id": s, "title": t, "estimate": e} for s, t, e in STEPS],
              "region": region or next_fleet.REGION, "plan": plan, "plans": [],
              "regions": [], "provider": provider,
              "providers": list(next_cloud.PROVIDERS)}
    try:
        planned = backend.plan(job, provider=provider)
        result.update(name=planned["name"], voter=planned["voter"],
                      voter_reason=planned["voter_reason"], rev=planned["rev"])
    except ToolError as error:
        result["error"] = str(error)
    unfinished = unfinished_states()
    if unfinished:
        result["unfinished"] = summary(unfinished[0])
    if provider == "aws":
        import next_aws
        result["plans"] = eligible_plans(next_aws.plans())
        result["regions"] = list(next_aws.REGIONS)
        result["price_note"] = next_aws.PRICE_NOTE
        try:
            next_aws.load_credentials()
        except ToolError as error:
            result["plans_error"] = str(error)
    elif vultr is not None:
        try:
            plans = eligible_plans(vultr.plans())
            result["plans"] = plans
            result["regions"] = sorted({loc for p in plans for loc in p["locations"]})
        except (ToolError, OSError) as error:
            result["plans_error"] = str(error)
    return result


def summary(state):
    if state is None:
        return None
    keys = ("name", "label", "provider", "region", "plan", "voter", "voter_reason", "rev", "ip",
            "ipv6", "instance_id", "branch", "commit", "finished", "failed",
            "started_at", "updated_at", "log")
    out = {key: state.get(key) for key in keys if state.get(key) is not None}
    out["steps"] = {step: state.status(step) for step in STEP_IDS}
    out["state_file"] = str(state.path)
    return out


def start_state(backend, job_factory, args):
    unfinished = unfinished_states()
    if args.name:
        path = state_path(args.name)
        existing = State.load(path) if path.is_file() else None
    else:
        existing = unfinished[0] if unfinished else None
        if len(unfinished) > 1:
            raise ToolError("more than one unfinished add-core (%s); pass --name" % (
                ", ".join(s.get("name") for s in unfinished)))
    if existing is not None and existing.get("name") and not existing.get("finished"):
        for key, value in (("region", args.region), ("plan", args.plan),
                           ("snapshot", args.snapshot),
                           ("provider", getattr(args, "provider", None))):
            if value and value != (existing.get(key) or (
                    next_cloud.DEFAULT_PROVIDER if key == "provider" else None)):
                raise ToolError("%s is already planned with %s %s; re-run without "
                                "--%s to resume it" % (existing.get("name"), key,
                                                       existing.get(key), key))
        return existing, True
    if existing is not None and existing.get("finished"):
        raise ToolError("%s was already added (%s)" % (existing.get("name"),
                                                        existing.path))
    probe = job_factory(State(Path(os.devnull), {}))
    planned = backend.plan(probe, region=args.region, plan=args.plan,
                           snapshot=args.snapshot, name=args.name,
                           provider=getattr(args, "provider", None))
    path = state_path(planned["name"]) if probe.execute else Path(os.devnull)
    state = State(path, dict(planned, started_at=time.time(), steps={}))
    return state, False


def main_add_core(args, vultr=None, ssh=None, cf=None, backend=None, out=None):
    runner = common.runner_from_args(args)
    if args.execute:
        vultr = vultr or _vultr()
    elif vultr is None:
        try:
            vultr = _vultr()
        except (ToolError, OSError):
            vultr = None
    ssh = ssh or common.Ssh()
    backend = backend or FleetBackend(vultr=vultr, ssh=ssh, cf=cf)
    fleet_file = Path(args.fleet) if args.fleet else common.fleet_file()

    def job_factory(state):
        return Job(state, runner, fleet_file=fleet_file, out=out,
                   events=getattr(args, "events", True))
    lock = common.DeployLock(what="next_fleet add-core")
    if runner.execute:
        import next_fleet
        lock.acquire(wait=next_fleet.ROLL_LOCK_WAIT_SECONDS)
    try:
        state, resumed = start_state(backend, job_factory, args)
        job = job_factory(state)
        if runner.execute:
            if runner.log_path:
                state.data["log"] = str(runner.log_path)
            state.save()
            job.say("%s %s (state %s)" % ("resuming" if resumed else "adding",
                                         job.name, state.path))
            run(job, backend)
            return 0
        job.events = False
        for line in describe(job, backend):
            job.say(line)
        job.say("(dry run: nothing was changed; pass --execute to add %s)" % job.name)
        return 0
    finally:
        if runner.execute:
            lock.release()


def _vultr():
    import next_fleet
    return next_fleet.Vultr.from_config()


def main_options(args, vultr=None, ssh=None, backend=None, out=None):
    out = out or sys.stdout
    runner = common.Runner(execute=False, out=open(os.devnull, "w"))
    if vultr is None:
        try:
            vultr = _vultr()
        except (ToolError, OSError):
            vultr = None
    backend = backend or FleetBackend(vultr=vultr, ssh=ssh or common.Ssh())
    fleet_file = Path(args.fleet) if args.fleet else common.fleet_file()
    job = Job(State(Path(os.devnull), {}), runner, fleet_file=fleet_file, out=out,
              events=False)
    result = options(backend, job, vultr, provider=getattr(args, "provider", None))
    if args.json:
        print(json.dumps(result, sort_keys=True), file=out)
        return 0
    print("next core: %s, region %s, plan %s, about %d min" % (
        result.get("name", "?"), result["region"], result["plan"] or "(cheapest)",
        round(result["estimate_seconds"] / 60.0)), file=out)
    if result.get("voter_reason"):
        print("rqlite: %s" % result["voter_reason"], file=out)
    for plan in result["plans"]:
        if result["region"] in plan["locations"]:
            print("  %-24s %2d vCPU %6d MB  $%.2f/month" % (
                plan["id"], plan["vcpu_count"], plan["ram"], plan["monthly_cost"]),
                file=out)
    return 0


def main_status(args, out=None):
    out = out or sys.stdout
    state = State.load(state_path(args.name)) if args.name else latest_state()
    result = summary(state) if state and state.get("name") else None
    if args.json:
        print(json.dumps(result, sort_keys=True), file=out)
        return 0
    if result is None:
        print("no add-core run recorded in %s" % state_dir(), file=out)
        return 0
    print("%s: %s" % (result["name"], "finished" if result.get("finished") else
                      "failed at %s: %s" % (result["failed"]["step"],
                                            result["failed"]["error"])
                      if result.get("failed") else "in progress"), file=out)
    for step in STEP_IDS:
        print("  %-9s %s" % (step, result["steps"][step]), file=out)
    return 0
