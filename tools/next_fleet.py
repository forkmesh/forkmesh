#!/usr/bin/env python3
"""ForkMesh Next fleet: Vultr and AWS instances, node install, secrets, replacement.

    next_fleet.py plans
    next_fleet.py list
    next_fleet.py create-core N   [--execute]      # label fmnext-core-N
    next_fleet.py create-runner N [--execute]      # label fmnext-run-N
    next_fleet.py create nN|rN [--provider vultr|aws] [--region R] [--plan P] [--execute]
    next_fleet.py list  [--provider vultr|aws] [--region R]
    next_fleet.py fleet-toml      [--execute]      # deploy/fleet.toml from fleet-instances.json
    next_fleet.py init-secrets    [--execute]      # fill missing values in secrets.env
    next_fleet.py install nN|rN   [--execute] [--steps base,wireguard,...,ids]
                                               # caddy / caddy-debian: upstream Caddy or back to Debian's
    next_fleet.py roll [nN ...]   [--steps ...] [--mail] [--execute]
    next_fleet.py roll-runners [rN ...] [--idle-wait SECONDS] [--execute]
                                               # new Go daemon on idle runners only
    next_fleet.py secrets nN      [--execute]
    next_fleet.py destroy nN|rN   --yes [--execute] [--force]
    next_fleet.py replace-core nOLD --new nNEW [--execute]
    next_fleet.py enable-ipv6 nN|rN [--reboot] [--execute]
    next_fleet.py add-core [--region R] [--plan P] [--snapshot ID] [--name nN] [--replace nOLD] [--execute]
    next_fleet.py add-core-options [--replace nOLD] [--json]
    next_fleet.py add-core-status  [--name nN] [--json]
    next_fleet.py remove-core nN [--destroy-instance] [--execute]
    next_fleet.py remove-core-plan nN [--destroy-instance] [--json]
    next_fleet.py remove-core-status [--name nN] [--json]
    next_fleet.py delete-refs [nN ...] --repo O/R --from FILE|- [--execute]
                                               # merged branches, every core at once

**Dry-run is the default** for every mutating subcommand; nothing changes
without ``--execute``. Read-only calls (``plans``, ``list``) do call the
Vultr API.

Hard guards (never bypassed, not even by --force):

* an instance is only touched when its label starts with ``fmnext-`` **and**
  its tags include ``forkmesh-next`` -- checked on live API data right before
  every mutation (the same Vultr account holds unrelated production
  instances);
* ``destroy`` needs ``--yes`` and ``--execute``;
* destroying a core node is refused when fewer than 3 healthy rqlite voters
  would remain, unless ``--force``.

Credentials: VULTR_API_KEY from <config>/vultr.env; AWS_ACCESS_KEY_ID,
AWS_SECRET_ACCESS_KEY and AWS_REGION from <config>/aws.env; node secrets from
<config>/secrets.env (pushed through ``systemd-creds encrypt`` on stdin, never
on a command line). With ``[app] vault = true`` only the bootstrap
credentials are pushed and the files of names now in the vault are removed
one by one (tools/next_vault.py seals those into the database). SSH uses ~/projects/forkmesh/ssh/fleet/next_fleet_ed25519
only.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import ipaddress
import json
import re
import secrets as _secrets
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_cloud  # noqa: E402
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

VULTR_API = "https://api.vultr.com/v2"
LABEL_PREFIX = "fmnext-"
TAG = "forkmesh-next"
SSH_KEY_NAME = "fmnext-fleet"
REGION = "ewr"
OS_NAME = "Debian 13 x64 (trixie)"
MIN_RAM_MB = 1024
LABEL_RE = re.compile(r"^fmnext-(core|run|auto|burst|snap)-([0-9]{1,2})$")
KINDS = {"core": "n", "run": "r", "auto": "a", "burst": "b", "snap": "s"}
AUTOSCALED_KINDS = ("auto", "burst")
CORE_WG = "10.77.0.%d"
RUNNER_WG = "10.77.0.%d"
RUNNER_WG_OFFSET = 10
APT_PACKAGES = (
    "ca-certificates curl git python3 python3-venv python3-cryptography openssl "
    "wireguard-tools nftables caddy earlyoom systemd-zram-generator sqlite3 jq "
    "fail2ban python3-systemd python3-pip nodejs npm golang-go"
)
RUNNER_APT_PACKAGES = (
    "ca-certificates curl git python3 python3-cryptography openssl nftables "
    "earlyoom systemd-zram-generator bubblewrap fail2ban python3-systemd"
)
# "daemon" (the Go daemon, role core/runner) runs after the first app deploy:
# the daemon's catalog and lease calls need the App to answer.
INSTALL_STEPS = ("base", "wireguard", "rqlite", "turn", "calls", "ssh-port",
                 "files", "caddy", "secrets", "start")
RUNNER_STEPS = ("base", "ssh-port", "files", "start")
APP_STEPS = ("base", "wireguard", "ids", "rqlite", "turn", "ssh-port", "files",
             "caddy", "secrets", "start")
BURST_STEPS = ("base", "ids", "ssh-port", "files", "secrets", "start", "daemon")
# "ids" generates the node's IDS signing key on the node (docs/IDS.md); run
# it on every node before the files step that first renders the IDS.
ALL_STEPS = INSTALL_STEPS + ("daemon", "ids", "caddy-debian", "logread")
CORE_ONLY_STEPS = ("caddy", "caddy-debian", "turn", "calls")
# Files the Go daemon runs next to itself (desktop MainWindowHostInstall.cpp).
DAEMON_TOOLS = (
    "tools/mirror_gateway.py", "app/src/mirror_replica.py",
    "app/src/public_snapshots.py", "app/src/portable_sessions.py",
    "tools/direct_https_bootstrap.py", "tools/node_operator_policy.py",
    "tools/secret_gate.py",
)
DEFAULT_SECRETS = {
    # name: generator
    "DATA_KEY": lambda: _secrets.token_urlsafe(48),
    "ADMIN_PASS": lambda: _secrets.token_urlsafe(24),
    "FORKMESH_MESH_SECRET": lambda: _secrets.token_urlsafe(48),
    "WWW_EDGE_SECRET": lambda: _secrets.token_urlsafe(48),
    "RQLITE_PASSWORD": lambda: _secrets.token_urlsafe(32),
    # certsync's own secret (only fmnext-certsync loads it; not the app).
    "FORKMESH_CERTSYNC_SECRET": lambda: _secrets.token_urlsafe(48),
    # IDS: the desktop-only master of the per-node expected-change keys
    # (tools/next_ids.py node_key; each node gets only its own).
    "FMNEXT_IDS_MASTER": lambda: _secrets.token_urlsafe(48),
    "OFFICE_TURN_SECRET": lambda: _secrets.token_urlsafe(48),
    "FORKMESH_CALLS_SECRET": lambda: _secrets.token_urlsafe(48),
}
IDS_MASTER = "FMNEXT_IDS_MASTER"
CERTSYNC_SECRET = "FORKMESH_CERTSYNC_SECRET"
JOB_USER = "forkmesh-job"
JOB_HOME = "/var/lib/forkmesh-job"
JOB_TMP = "/var/lib/forkmesh-runner/tmp"
SSH_GENERATOR_SOCKETS = ("sshd-unix-local.socket", "sshd-vsock.socket")
# sshd: key-only root, no agent/X11 forwarding, and only local (-L) TCP
# forwarding, which next_backup.py and next_deploy.py use to reach rqlite
# on 127.0.0.1:4001 (next_common.Ssh.tunnel).
SSHD_SETTINGS = (
    "PasswordAuthentication no", "KbdInteractiveAuthentication no",
    "PermitRootLogin prohibit-password", "MaxAuthTries 3",
    "AllowAgentForwarding no", "AllowTcpForwarding local",
    "X11Forwarding no", "PermitTunnel no",
    # The IDS classifies sessions from "Starting session: ..." (docs/IDS.md).
    "LogLevel VERBOSE",
)
# Vendor leftovers: cloud-init's sshd file says PasswordAuthentication yes
# (overridden by ours today, removed so it can never win), a default sudo
# account, and the tty `less` log reader that escapes to a root shell.
CLOUD_INIT_SSHD = "/etc/ssh/sshd_config.d/50-cloud-init.conf"
VENDOR_USER = "linuxuser"


# --------------------------------------------------------------------------
# guards


def instance_tags(instance) -> list:
    tags = instance.get("tags")
    if tags is None and instance.get("tag"):
        tags = [instance["tag"]]
    return [str(t) for t in (tags or [])]


def guard_instance(instance) -> dict:
    """Raise GuardError unless this is a ForkMesh Next instance."""
    label = str((instance or {}).get("label") or "")
    tags = instance_tags(instance or {})
    if not label.startswith(LABEL_PREFIX):
        raise GuardError("refusing instance %r: label does not start with %s"
                         % (label, LABEL_PREFIX))
    if TAG not in tags:
        raise GuardError("refusing instance %r: not tagged %s (tags: %s)"
                         % (label, TAG, ", ".join(tags) or "none"))
    if not LABEL_RE.match(label):
        raise GuardError("refusing instance %r: label is not fmnext-core-N, "
                         "fmnext-run-N, fmnext-auto-N, fmnext-burst-N or "
                         "fmnext-snap-N" % label)
    return instance


def is_core_like(name) -> bool:
    return str(name or "")[:1] in ("n", "a")


def is_autoscaled_label(label) -> bool:
    match = LABEL_RE.match(str(label or ""))
    return bool(match) and match.group(1) in AUTOSCALED_KINDS


def label_for(kind: str, number: int) -> str:
    if kind not in KINDS or not 1 <= int(number) <= 99:
        raise ToolError("bad instance kind/number")
    return "%s%s-%d" % (LABEL_PREFIX, kind, int(number))


def parse_target(target: str):
    target = target.strip()
    for kind, short in KINDS.items():
        match = re.fullmatch(r"%s([0-9]{1,2})" % short, target) or \
            re.fullmatch(r"fmnext-%s-([0-9]{1,2})" % kind, target)
        if match:
            n = int(match.group(1))
            return kind, n, label_for(kind, n), "%s%d" % (short, n)
    raise GuardError("refusing target %r: expected nN, rN, aN, bN, sN or an "
                     "fmnext-core/run/auto/burst/snap-N label" % target)


# --------------------------------------------------------------------------
# plans


def plan_has_ipv4(plan) -> bool:
    plan_id = str(plan.get("id") or "")
    return not plan_id.endswith("-v6") and "-v6-" not in plan_id


def cheapest_plan(plans, region=REGION, min_ram=MIN_RAM_MB):
    """desktop ControlNode.cpp cheapestVultrPlan, restricted to ``region``:
    >= 1 GiB RAM, IPv4, positive monthly cost; ties go to more RAM, then id."""
    best = None
    for plan in plans:
        try:
            cost = float(plan.get("monthly_cost"))
            ram = float(plan.get("ram"))
        except (TypeError, ValueError):
            continue
        plan_id = str(plan.get("id") or "")
        locations = [str(x).lower() for x in plan.get("locations") or []]
        if (not plan_id or cost <= 0 or ram < min_ram or not plan_has_ipv4(plan)
                or region not in locations):
            continue
        key = (cost, -ram, plan_id)
        if best is None or key < best[0]:
            best = (key, plan)
    return None if best is None else best[1]


# --------------------------------------------------------------------------
# Vultr


class Vultr(next_cloud.Provider):
    name = "vultr"

    def __init__(self, http):
        self.http = http

    def guard(self, instance):
        return guard_instance(instance)

    @classmethod
    def from_config(cls, opener=None):
        key = common.credential("vultr.env", "VULTR_API_KEY")
        return cls(common.JsonHttp(VULTR_API, key, opener))

    def _paged(self, path, field):
        items, cursor = [], ""
        for _ in range(50):
            sep = "&" if "?" in path else "?"
            page = self.http.request("GET", "%s%sper_page=500%s" % (
                path, sep, "&cursor=" + cursor if cursor else ""))
            items += page.get(field) or []
            cursor = ((page.get("meta") or {}).get("links") or {}).get("next") or ""
            if not cursor:
                break
        return items

    def plans(self):
        return self._paged("/plans", "plans")

    def os_list(self):
        return self._paged("/os", "os")

    def ssh_keys(self):
        return self._paged("/ssh-keys", "ssh_keys")

    def fleet_instances(self):
        """Only instances that pass guard_instance (tag filter + label check)."""
        found = []
        for instance in self._paged("/instances?tag=%s" % TAG, "instances"):
            try:
                found.append(guard_instance(instance))
            except GuardError:
                continue
        return found

    def instance(self, instance_id):
        if not re.fullmatch(r"[0-9a-f-]{36}", str(instance_id)):
            raise GuardError("refusing malformed instance id %r" % instance_id)
        return self.http.request("GET", "/instances/%s" % instance_id)["instance"]

    def find_by_label(self, label):
        matches = [i for i in self.fleet_instances() if i.get("label") == label]
        if len(matches) > 1:
            raise GuardError("more than one instance is labelled %s" % label)
        return matches[0] if matches else None

    def create_instance(self, body):
        guard_instance({"label": body.get("label"), "tags": body.get("tags")})
        return self.http.request("POST", "/instances", body)["instance"]

    def enable_ipv6(self, instance_id):
        guard_instance(self.instance(instance_id))
        self.http.request("PATCH", "/instances/%s" % instance_id,
                          {"enable_ipv6": True}, expect=(200, 202, 204))

    def reboot(self, instance_id):
        guard_instance(self.instance(instance_id))
        self.http.request("POST", "/instances/%s/reboot" % instance_id, {},
                          expect=(200, 202, 204))

    def halt(self, instance_id):
        guard_instance(self.instance(instance_id))
        self.http.request("POST", "/instances/halt",
                          {"instance_ids": [instance_id]},
                          expect=(200, 202, 204))

    def start(self, instance_id):
        guard_instance(self.instance(instance_id))
        self.http.request("POST", "/instances/%s/start" % instance_id, {},
                          expect=(200, 202, 204))

    def set_reverse(self, instance_id, ip, name):
        guard_instance(self.instance(instance_id))
        family = "ipv6" if ":" in ip else "ipv4"
        if family == "ipv6":
            import ipaddress
            ip = ipaddress.IPv6Address(ip).exploded
        self.http.request("POST", "/instances/%s/%s/reverse" % (
            instance_id, family), {"ip": ip, "reverse": name},
            expect=(200, 201, 202, 204))

    def delete_instance(self, instance_id):
        # Re-read immediately before deleting: the guard sees live data.
        guard_instance(self.instance(instance_id))
        self.http.request("DELETE", "/instances/%s" % instance_id, expect=(204, 200))

    def create_snapshot(self, instance_id, description):
        instance = guard_instance(self.instance(instance_id))
        if not str(instance.get("label") or "").startswith(LABEL_PREFIX + "snap-"):
            raise GuardError("snapshots are taken only of fmnext-snap-N builders")
        return self.http.request("POST", "/snapshots", {
            "instance_id": instance_id, "description": description[:200]})["snapshot"]

    def snapshot(self, snapshot_id):
        if not re.fullmatch(r"[0-9a-f-]{36}", str(snapshot_id)):
            raise GuardError("refusing malformed snapshot id %r" % snapshot_id)
        return self.http.request("GET", "/snapshots/%s" % snapshot_id)["snapshot"]

    def ensure_ssh_key(self, runner, public_key):
        for key in self.ssh_keys():
            if key.get("name") == SSH_KEY_NAME:
                return key["id"]
        created = runner.step("upload SSH key %s to Vultr" % SSH_KEY_NAME,
                              lambda: self.http.request("POST", "/ssh-keys", {
                                  "name": SSH_KEY_NAME, "ssh_key": public_key}))
        return (created or {}).get("ssh_key", {}).get("id", "<new-key-id>")


def instances_path():
    return common.config_dir() / "fleet-instances.json"


def instance_ipv6(instance):
    value = str((instance or {}).get("v6_main_ip") or "").strip()
    if not value or value == "::":
        return ""
    try:
        address = ipaddress.IPv6Address(value)
    except ValueError:
        return ""
    return str(address) if address.is_global else ""


def record_instance(instance):
    data = common.load_json(instances_path(), {}) or {}
    entry = {
        "id": instance["id"],
        "plan": instance.get("plan", ""),
        "region": instance.get("region", ""),
        "ip": instance.get("main_ip", "") if instance.get("main_ip") not in (
            None, "0.0.0.0") else "",
    }
    ipv6 = instance_ipv6(instance)
    if ipv6:
        entry["ipv6"] = ipv6
    entry["provider"] = next_cloud.check_name(instance.get("provider"))
    data[instance["label"]] = entry
    common.save_json(instances_path(), data)


def forget_instance(label):
    if not is_autoscaled_label(label):
        raise GuardError("only autoscaled instances are forgotten: %r" % label)
    data = common.load_json(instances_path(), {}) or {}
    if data.pop(label, None) is not None:
        common.save_json(instances_path(), data)


def fleet_public_key():
    public_key = Path(str(common.ssh_key_path()) + ".pub")
    key_text = public_key.read_text("utf-8").strip() if public_key.is_file() else ""
    if not key_text:
        raise ToolError("missing %s" % public_key)
    return key_text


def create(kind, number, vultr, runner, os_id=None, plan_id=None,
           region=REGION, snapshot_id=None, enable_ipv6=False, fleet=None,
           ssh_sources=("0.0.0.0/0",), termination_protection=False):
    label = label_for(kind, number)
    existing = vultr.find_by_label(label)
    if existing:
        runner.say("%s already exists (%s, %s): adopting it" % (
            label, existing["id"], existing.get("main_ip") or "no IP yet"))
        record_instance(existing)
        return existing
    if next_cloud.provider_name(vultr) != "vultr":
        if snapshot_id or os_id is not None:
            raise ToolError("--snapshot and --os-id are Vultr-only")
        created = vultr.create_labelled(
            label, runner, plan_id=plan_id, region=None if region == REGION else region,
            fleet=fleet, public_key=fleet_public_key(), ssh_sources=ssh_sources,
            termination_protection=termination_protection)
        if created:
            record_instance(created)
        return created
    plan = plan_id or (cheapest_plan(vultr.plans(), region) or {}).get("id")
    if not plan:
        raise ToolError("no Vultr plan with >= 1 GiB RAM in %s" % region)
    if snapshot_id:
        if not re.fullmatch(r"[0-9a-f-]{36}", str(snapshot_id)):
            raise GuardError("refusing malformed snapshot id %r" % snapshot_id)
    elif os_id is None:
        matches = [o for o in vultr.os_list() if o.get("name") == OS_NAME]
        if not matches:
            raise ToolError("Vultr has no OS named %r" % OS_NAME)
        os_id = matches[0]["id"]
    public_key = Path(str(common.ssh_key_path()) + ".pub")
    key_text = public_key.read_text("utf-8").strip() if public_key.is_file() else ""
    if not key_text:
        raise ToolError("missing %s" % public_key)
    key_id = vultr.ensure_ssh_key(runner, key_text)
    body = {
        "region": region, "plan": plan, "label": label,
        "hostname": label, "tags": [TAG], "sshkey_id": [key_id],
        "enable_ipv6": bool(enable_ipv6), "backups": "disabled",
    }
    if snapshot_id:
        body["snapshot_id"] = snapshot_id
        source = "snapshot %s" % snapshot_id
    else:
        body["os_id"] = os_id
        source = "os %s" % os_id
    created = runner.step("create Vultr instance %s (%s, %s, %s, tag %s)" % (
        label, plan, region, source, TAG), lambda: vultr.create_instance(body))
    if created:
        record_instance(created)
    return created


# --------------------------------------------------------------------------
# fleet.toml from fleet-instances.json


OPTIONAL_SECRETS_RE = re.compile(r"(?ms)^optional_secrets = \[.*?^\]$|^optional_secrets = \[\]$")
VAULT_RES = (re.compile(r"(?m)^vault = (?:true|false)$"),
             re.compile(r"(?m)^vault_peer_unseal = (?:true|false)$"),
             re.compile(r"(?m)^ssh_port = [0-9]+$"))
# The [ids] table (fingerprints, allowlist, operator key) is this fleet's own.
IDS_SECTION_RE = re.compile(r"(?ms)^\[ids\]\n.*?(?=^\[|\Z)")
TURN_SECTION_RE = re.compile(r"(?ms)^\[turn\]\n.*?(?=^\[|\Z)")
CALLS_SECTION_RE = re.compile(r"(?ms)^\[calls\]\n.*?(?=^\[|\Z)")
PRODUCTION_SECTION_RE = re.compile(
    r"(?ms)^\[production\]\n.*?(?=^\[(?!production\.)|\Z)")
HOSTED_SECTION_RE = re.compile(r"(?ms)^\[hosted\]\n.*?(?=^\[|\Z)")
RETIRED_CORES_RE = re.compile(r"(?m)^retired_cores = \[(.*)\]\n?")


def core_block(number, label, info, pubkey="", voter=True):
    name = "n%d" % number
    lines = ["[[nodes]]", 'name = "%s"' % name, 'role = "core"',
             'label = "%s"' % label, 'instance_id = "%s"' % info.get("id", ""),
             'wg_ip = "%s"' % (CORE_WG % number),
        'wg_pubkey = "%s"' % pubkey, 'rqlite_id = "%s"' % name]
    if not voter:
        lines.append("rqlite_voter = false")
    return "\n".join(lines)


NODES_BLOCK_RE = re.compile(r"(?ms)^\[\[nodes\]\]\n.*?(?=^\[|\Z)")


def upsert_core_block(text, name, block):
    blocks = list(NODES_BLOCK_RE.finditer(text))
    for match in blocks:
        if re.search(r'(?m)^name = "%s"$' % re.escape(name), match.group(0)):
            rest = text[match.end():]
            return text[:match.start()] + block + ("\n\n" if rest else "\n") + rest
    if not blocks:
        return text.rstrip() + "\n\n" + block + "\n"
    last = blocks[-1]
    rest = text[last.end():]
    head = text[:last.end()]
    if not rest:
        return head.rstrip() + "\n\n" + block + "\n"
    return head + block + "\n\n" + rest


NODE_LINES_RE = re.compile(r"\[\[nodes\]\]\n(?:[^\n#\[][^\n]*\n?)*\n*")


def remove_core_block(text, name):
    for match in NODES_BLOCK_RE.finditer(text):
        if not re.search(r'(?m)^name = "%s"$' % re.escape(name), match.group(0)):
            continue
        body = NODE_LINES_RE.match(text, match.start())
        end = body.end() if body else match.end()
        head = text[:match.start()]
        rest = text[end:]
        if not rest.strip():
            return head.rstrip() + "\n"
        return head + rest
    return text


def retire_core_block(text, name):
    line = "retired_cores = [%s]\n"
    section = HOSTED_SECTION_RE.search(text)
    if section is None:
        block = "[hosted]\n" + line % ('"%s"' % name) + "\n"
        first = NODES_BLOCK_RE.search(text)
        if first is None:
            return text.rstrip() + "\n\n" + block.rstrip() + "\n"
        return text[:first.start()] + block + text[first.start():]
    body = section.group(0)
    listed = RETIRED_CORES_RE.search(body)
    names = re.findall(r'"([^"]+)"', listed.group(1)) if listed else []
    if name in names:
        return text
    names.append(name)
    entry = line % ", ".join('"%s"' % n for n in names)
    if listed:
        body = body[:listed.start()] + entry + body[listed.end():]
    else:
        body = "[hosted]\n" + entry + body[len("[hosted]\n"):]
    return text[:section.start()] + body + text[section.end():]


def promote_core_block(text, name):
    for match in NODES_BLOCK_RE.finditer(text):
        if not re.search(r'(?m)^name = "%s"$' % re.escape(name), match.group(0)):
            continue
        block = re.sub(r"(?m)^rqlite_voter = false\n?", "", match.group(0))
        return text[:match.start()] + block + text[match.end():]
    return text


def fleet_toml_text(instances, state=None, example=None, current=None):
    """current: the existing fleet.toml text; its [app] optional_secrets
    (which integrations this fleet turned on) and ``vault`` switches survive
    regeneration."""
    state = state or {}
    pubkeys = state.get("wg_pubkeys") or {}
    example = example or (common.REPO / "deploy" / "fleet.example.toml").read_text("utf-8")
    header = example.split("[[nodes]]", 1)[0]
    header = header.split("\n\n", 1)[1] if "\n\n" in header else header
    kept = OPTIONAL_SECRETS_RE.search(current or "")
    if kept and OPTIONAL_SECRETS_RE.search(header):
        header = OPTIONAL_SECRETS_RE.sub(lambda _m: kept.group(0), header, count=1)
    for pattern in VAULT_RES:
        kept_switch = pattern.search(current or "")
        if kept_switch and pattern.search(header):
            header = pattern.sub(lambda _m, k=kept_switch: k.group(0), header, count=1)
    kept_ids = IDS_SECTION_RE.search(current or "")
    if kept_ids and IDS_SECTION_RE.search(header):
        header = IDS_SECTION_RE.sub(lambda _m: kept_ids.group(0), header, count=1)
    kept_turn = TURN_SECTION_RE.search(current or "")
    if kept_turn and TURN_SECTION_RE.search(header):
        header = TURN_SECTION_RE.sub(lambda _m: kept_turn.group(0), header, count=1)
    kept_calls = CALLS_SECTION_RE.search(current or "")
    if kept_calls and CALLS_SECTION_RE.search(header):
        header = CALLS_SECTION_RE.sub(lambda _m: kept_calls.group(0), header, count=1)
    kept_production = PRODUCTION_SECTION_RE.search(current or "")
    if kept_production and not PRODUCTION_SECTION_RE.search(header):
        anchor = re.search(r"(?m)^\[dns\]$", header)
        section = kept_production.group(0).rstrip() + "\n\n"
        header = (header[:anchor.start()] + section + header[anchor.start():]
                  if anchor else header.rstrip() + "\n\n" + section)
    kept_hosted = HOSTED_SECTION_RE.search(current or "")
    if kept_hosted and not HOSTED_SECTION_RE.search(header):
        header = header.rstrip() + "\n\n" + kept_hosted.group(0).rstrip() + "\n"
    header = (
        "# ForkMesh Next fleet. Generated by `tools/next_fleet.py fleet-toml\n"
        "# --execute` from ~/projects/forkmesh/config/fleet/fleet-instances.json\n"
        "# (instance ids) and fleet-state.json (WireGuard public keys). Public\n"
        "# addresses stay in fleet-instances.json; the tools read them by label.\n"
        "# No secrets: every secret is a credential name only (docs/OPERATIONS.md).\n"
        "\n" + header)
    blocks = []
    cores = sorted((int(LABEL_RE.match(l).group(2)), l, v) for l, v in instances.items()
                   if LABEL_RE.match(l) and LABEL_RE.match(l).group(1) == "core")
    runners = sorted((int(LABEL_RE.match(l).group(2)), l, v) for l, v in instances.items()
                     if LABEL_RE.match(l) and LABEL_RE.match(l).group(1) == "run")
    nonvoters = set(state.get("rqlite_nonvoters") or [])
    for number, label, info in cores:
        name = "n%d" % number
        blocks.append(core_block(number, label, info, pubkeys.get(name, ""),
                                 voter=name not in nonvoters))
    for number, label, info in runners:
        blocks.append("\n".join([
            "[[runners]]", 'name = "r%d"' % number, 'label = "%s"' % label,
            'instance_id = "%s"' % info.get("id", "")] + [
            "# Reserved address; runners are not WireGuard peers of the core.",
            'wg_ip = "%s"' % (RUNNER_WG % (RUNNER_WG_OFFSET + number))]))
    return header.rstrip() + "\n\n" + "\n\n".join(blocks) + "\n"


# --------------------------------------------------------------------------
# node install


def shq(value):
    return shlex.quote(str(value))


def base_script(role):
    users = ["forkmesh-node"]
    if role == "core":
        users += ["rqlite", "forkmesh-app", "fmnext-certsync", "fmnext-build",
                  "fmnext-calls"]
    else:
        users += [JOB_USER]
    lines = [
        "export DEBIAN_FRONTEND=noninteractive",
        "timedatectl set-timezone UTC || true",
        "apt-get -o DPkg::Lock::Timeout=900 update -qq",
        "apt-get -o DPkg::Lock::Timeout=900 install -y -qq --no-install-recommends %s" % (
            APT_PACKAGES if role == "core" else RUNNER_APT_PACKAGES),
    ]
    homes = {"forkmesh-node": "/var/lib/forkmesh", JOB_USER: JOB_HOME,
             "fmnext-build": "/var/lib/fmnext-build"}
    for user in users:
        home = homes.get(user, "/nonexistent")
        lines.append("id %s >/dev/null 2>&1 || useradd --system --home-dir %s "
                     "--shell /usr/sbin/nologin %s%s" % (
                         user, home, user,
                         " --create-home" if user in homes else ""))
    lines += [
        "install -d -m 0755 /opt/forkmesh-next /opt/forkmesh-next/bin /opt/forkmesh-next/app /opt/forkmesh-next/app/releases",
        "install -d -m 0755 /etc/forkmesh-next /etc/forkmesh-next/caddy",
        "install -d -m 0700 /etc/forkmesh-next/creds",
        "install -d -o forkmesh-node -g forkmesh-node -m 0700 /var/lib/forkmesh /var/lib/forkmesh/tmp /etc/forkmesh",
        "if [ ! -f /swapfile ]; then fallocate -l 1G /swapfile && chmod 0600 /swapfile && mkswap /swapfile >/dev/null; fi",
        # Key-only SSH, as the desktop's Vultr installer does. The drop-in is
        # checked with sshd -t before the reload; a broken one is removed, so
        # the running sshd and its keys are never lost.
        "if [ -s /root/.ssh/authorized_keys ]; then install -d -m 0755 /etc/ssh/sshd_config.d; "
        "printf '%%s\\n' %s >/etc/ssh/sshd_config.d/10-fmnext.conf; "
        "rm -f %s; "
        "grep -q '^Include /etc/ssh/sshd_config.d/\\*.conf' /etc/ssh/sshd_config || "
        "sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' /etc/ssh/sshd_config; "
        "if sshd -t; then systemctl reload ssh || true; else rm -f /etc/ssh/sshd_config.d/10-fmnext.conf; fi; fi"
        % (" ".join(shq(x) for x in SSHD_SETTINGS), shq(CLOUD_INIT_SSHD)),
        # No console root password (the Vultr console then needs a rescue
        # boot; docs/OPERATIONS.md). passwd -l only where sshd uses PAM,
        # which ignores the lock for key logins; otherwise '*' (no valid
        # password, not "locked") so root's key login keeps working.
        "if sshd -T 2>/dev/null | grep -qi '^usepam yes'; then passwd -l root >/dev/null; "
        "else usermod -p '*' root; fi",
        "if systemctl cat cloud-init-log-reader.service >/dev/null 2>&1; then "
        "systemctl mask --now cloud-init-log-reader.service >/dev/null 2>&1 || true; fi",
        # The vendor's default sudo account: locked, then removed, never
        # while it has processes.
        "if id %s >/dev/null 2>&1; then usermod -L -e 1 %s; "
        "if pgrep -u %s >/dev/null; then echo 'WARNING: %s has processes; locked, not removed' >&2; "
        "else userdel -r %s 2>/dev/null || userdel %s || echo 'WARNING: could not "
        "remove %s' >&2; fi; fi" % ((VENDOR_USER,) * 7),
    ]
    if role == "runner":
        # Jobs run as JOB_USER through the system manager (the daemon runs
        # as root there); forkmesh-node's user manager is no longer used.
        lines += [
            "loginctl disable-linger forkmesh-node 2>/dev/null || true",
            "install -d -o %s -g %s -m 0700 %s %s/.claude %s/.codex" % (
                JOB_USER, JOB_USER, JOB_HOME, JOB_HOME, JOB_HOME),
            "install -d -o root -g root -m 0711 /var/lib/forkmesh-runner "
            "/var/lib/forkmesh-runner/work",
            "install -d -o root -g root -m 1777 %s" % JOB_TMP,
        ]
    if role == "core":
        lines += [
            "install -d -o rqlite -g rqlite -m 0750 /var/lib/forkmesh-next /var/lib/forkmesh-next/rqlite",
            "chmod 0755 /var/lib/forkmesh-next",
            "install -d -o forkmesh-app -g forkmesh-app -m 0700 /var/lib/forkmesh-next/app",
            "install -d -o root -g fmnext-certsync -m 0750 /var/lib/forkmesh-next/certsync /var/lib/forkmesh-next/certsync/export",
            "install -d -o caddy -g caddy -m 0700 /etc/forkmesh-next/tls",
        ]
    return "\n".join(lines)


WIREGUARD_KEY_SCRIPT = """
umask 077
install -d -m 0700 /etc/wireguard
[ -s /etc/wireguard/wg0.key ] || wg genkey >/etc/wireguard/wg0.key
wg pubkey </etc/wireguard/wg0.key
"""


def rqlite_script(version, sha256):
    name = "rqlite-%s-linux-amd64" % version
    return "\n".join([
        "cd /tmp",
        "echo %s  /tmp/%s.tar.gz | sha256sum -c -" % (shq(sha256), name),
        "tar -xzf /tmp/%s.tar.gz -C /tmp" % name,
        "install -m 0755 /tmp/%s/rqlited /usr/local/bin/rqlited" % name,
        "install -m 0755 /tmp/%s/rqlite /usr/local/bin/rqlite" % name,
        "rm -rf /tmp/%s /tmp/%s.tar.gz" % (name, name),
        "rqlited -version",
    ])


def caddy_release(fleet, required=True):
    import render
    settings = render.caddy_settings(fleet)
    if settings is None:
        raise ToolError("deploy/fleet.toml has no [caddy] version and sha256")
    path = common.data_dir() / "dl" / settings["tarball"]
    if not path.is_file():
        if required:
            raise ToolError("%s is missing: download it from the Caddy v%s "
                            "GitHub release" % (path, settings["version"]))
        return settings, path, None
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != settings["sha256"]:
        raise GuardError("local %s sha256 %s != fleet %s" % (
            path.name, digest, settings["sha256"]))
    with tarfile.open(path, "r:gz") as archive:
        member = archive.getmember("caddy")
        if not member.isfile():
            raise GuardError("%s: caddy is not a regular file" % path.name)
        binary = archive.extractfile(member).read()
    return settings, path, hashlib.sha256(binary).hexdigest()


def caddy_script(settings, binary_sha, dropin, host):
    import next_deploy
    import render
    return next_deploy.caddy_functions(host, wait=CADDY_INSTALL_WAIT, max_time=CADDY_INSTALL_MAX_TIME) + """
set -eu
tarball=/tmp/{tarball}
stage=$(mktemp -d /var/lib/fmnext-caddy.XXXXXX)
installed=0
finish() {{
  code=$?
  rm -rf "$stage" "$tarball"
  if [ "$code" -ne 0 ] && [ "$installed" = 1 ]; then echo "fmnext-caddy: restore-needed"; fi
  exit "$code"
}}
trap finish EXIT
echo {sha}'  '"$tarball" | sha256sum -c - >/dev/null
tar -xzf "$tarball" -C "$stage" caddy
echo {binary_sha}'  '"$stage/caddy" | sha256sum -c - >/dev/null
chmod 0755 "$stage/caddy"
"$stage/caddy" version | grep -q '^v{version} ' || {{ echo "fmnext-caddy: the staged binary is not v{version}" >&2; exit 5; }}
mkdir -m 0700 "$stage/home"
if ! HOME="$stage/home" XDG_DATA_HOME="$stage/home/data" XDG_CONFIG_HOME="$stage/home/config" \
    "$stage/caddy" validate --config /etc/caddy/Caddyfile --adapter caddyfile >"$stage/validate.log" 2>&1; then
  tail -n 20 "$stage/validate.log" >&2
  echo "fmnext-caddy: /etc/caddy/Caddyfile does not validate with v{version}; nothing changed" >&2
  exit 6
fi
installed=1
install -m 0755 -o root -g root "$stage/caddy" {bin}
install -d -m 0755 {dropin_dir}
echo {dropin_b64} | base64 -d >"$stage/dropin"
install -m 0644 -o root -g root "$stage/dropin" {dropin_path}
systemctl daemon-reload
systemctl restart caddy
caddy_serving || {{ echo "fmnext-caddy: caddy is not serving on v{version}" >&2; exit 7; }}
exe=$(readlink /proc/$(systemctl show -p MainPID --value caddy)/exe)
[ "$exe" = {bin} ] || {{ echo "fmnext-caddy: caddy runs $exe, not {bin}" >&2; exit 7; }}
echo "fmnext-caddy: $({bin} version | cut -d' ' -f1) serving"
""".format(tarball=settings["tarball"], sha=shq(settings["sha256"]),
           binary_sha=shq(binary_sha), version=settings["version"],
           bin=render.CADDY_BIN, dropin_path=shq(render.CADDY_BINARY_DROPIN),
           dropin_dir=shq(render.CADDY_BINARY_DROPIN.rsplit("/", 1)[0]),
           dropin_b64=shq(base64.b64encode(dropin.encode()).decode()))


def caddy_debian_script(host):
    import next_deploy
    import render
    return next_deploy.caddy_functions(host, wait=CADDY_INSTALL_WAIT, max_time=CADDY_INSTALL_MAX_TIME) + """
set -u
rm -f {dropin_path}
systemctl daemon-reload
systemctl restart caddy
caddy_serving || {{ echo "fmnext-caddy: Debian's caddy is not serving" >&2; exit 8; }}
rm -f {bin}
echo "fmnext-caddy: back on Debian's $({debian} version | cut -d' ' -f1)"
""".format(dropin_path=shq(render.CADDY_BINARY_DROPIN), bin=render.CADDY_BIN,
           debian=render.CADDY_DEBIAN_BIN)


def install_caddy(fleet, name, ip, ssh, runner):
    import next_deploy
    import next_ids
    import render
    host = next_deploy.node_host(fleet, name)
    settings, tarball, binary_sha = caddy_release(fleet, required=runner.execute)

    def action():
        dropin = render.caddy_binary_dropin(fleet)
        ssh.put(ip, tarball, "/tmp/%s" % tarball.name)
        completed = ssh.run(ip, caddy_script(settings, binary_sha, dropin, host),
                            check=False, timeout=600)
        out = (completed.stdout or b"").decode("utf-8", "replace")
        err = (completed.stderr or b"").decode("utf-8", "replace")
        for line in (out + err).splitlines():
            if line.startswith("fmnext-caddy:"):
                runner.say("%s: %s" % (name, line))
        if completed.returncode == 0:
            return True
        if "fmnext-caddy: restore-needed" in out:
            runner.say("%s: Caddy v%s failed after install; restoring Debian's "
                       "caddy" % (name, settings["version"]))
            next_ids.expect_step(fleet, ssh, runner, name, "caddy-debian")
            restore_caddy_debian(fleet, name, ip, ssh, runner)
        raise ToolError("%s: Caddy v%s was not installed (exit %d): %s" % (
            name, settings["version"], completed.returncode,
            err.strip().splitlines()[-1] if err.strip() else "no detail"))
    return runner.step("%s: Caddy v%s (sha256 %s..., verified here and on the node) "
                       "-> %s, validate /etc/caddy/Caddyfile with it first, then "
                       "the %s drop-in and a restart" % (
                           name, settings["version"], settings["sha256"][:12],
                           render.CADDY_BIN, render.CADDY_BINARY_DROPIN), action)


def restore_caddy_debian(fleet, name, ip, ssh, runner):
    import next_deploy
    host = next_deploy.node_host(fleet, name)

    def action():
        completed = ssh.run(ip, caddy_debian_script(host), check=False, timeout=300)
        text = ((completed.stdout or b"") + (completed.stderr or b"")).decode(
            "utf-8", "replace")
        for line in text.splitlines():
            if line.startswith("fmnext-caddy:"):
                runner.say("%s: %s" % (name, line))
        if completed.returncode != 0:
            raise ToolError("%s: restoring Debian's caddy failed (exit %d)" % (
                name, completed.returncode))
        return True
    return runner.step("%s: remove the Caddy binary drop-in, restart Debian's "
                       "caddy, then remove the upstream binary" % name, action)


ZRAM_START = (
    "if ! grep -q '^/dev/zram0 ' /proc/swaps; then "
    "systemctl reset-failed systemd-zram-setup@zram0.service dev-zram0.swap "
    ">/dev/null 2>&1; systemctl start dev-zram0.swap || true; fi")


RQLITE_UNIT_DIGEST = ("{ cat /etc/systemd/system/rqlite.service "
                      "/etc/systemd/system/rqlite.service.d/*.conf 2>/dev/null || true; } "
                      "| sha256sum")
DAEMON_UNIT_DIGEST = ("{ cat /etc/systemd/system/forkmesh-mirror-node.service "
                      "/etc/systemd/system/forkmesh-mirror-node.service.d/*.conf 2>/dev/null "
                      "|| true; } | sha256sum")
CALLS_UNIT_DIGEST = ("{ cat /etc/systemd/system/fmnext-calls.service "
                     "/etc/systemd/system/fmnext-calls.service.d/*.conf 2>/dev/null "
                     "|| true; } | sha256sum")


def files_script(manifest, archive):
    is_core = manifest.get("role") == "core"
    lines = [
        "stage=$(mktemp -d /tmp/fmnext-files.XXXXXX)",
        "trap 'rm -rf \"$stage\"' EXIT",
        "tar -xzf %s -C \"$stage\"" % shq(archive),
        "rm -f %s" % shq(archive),
    ]
    if is_core:
        lines.append("rqlite_unit_before=$(%s)" % RQLITE_UNIT_DIGEST)
    lines.append("daemon_unit_before=$(%s)" % DAEMON_UNIT_DIGEST)
    calls = "/etc/systemd/system/fmnext-calls.service" in manifest["files"]
    if calls:
        lines.append("calls_unit_before=$(%s)" % CALLS_UNIT_DIGEST)
    for path, mode in sorted(manifest["files"].items()):
        lines.append("install -D -m %s \"$stage\"%s %s" % (
            mode.replace("0o", "0"), shq(path), shq(path)))
    for path, source in sorted((manifest.get("release_routes") or {}).items()):
        lines.append("if [ -s %s ]; then install -m 0644 %s %s; fi" % (
            shq(source), shq(source), shq(path)))
    if is_core:
        lines += [
            # /etc/hosts: replace only our managed block.
            "sed -i '/^# BEGIN forkmesh-next/,/^# END forkmesh-next/d' /etc/hosts",
            "cat /etc/forkmesh-next/hosts.fragment >>/etc/hosts",
            "chmod 0600 /etc/wireguard/wg0.conf",
            # Fill in the node-local private key (never leaves the node).
            "test -s /etc/wireguard/wg0.key",
            "sed -i \"s|^PrivateKey = @FMNEXT_WG0_PRIVATE_KEY@$|PrivateKey = "
            "$(cat /etc/wireguard/wg0.key)|\" /etc/wireguard/wg0.conf",
            "if grep -q '@FMNEXT_WG0_PRIVATE_KEY@' /etc/wireguard/wg0.conf; then "
            "echo 'wg0.conf: private key not filled in' >&2; exit 1; fi",
        ]
        # Leaderless certificates (deploy/core/certsync.py): retire the old
        # fixed-issuer units by name, then write the Caddy tls includes from
        # whatever certificate this node already holds (no network), so
        # Caddy can always load its config.
        retired = manifest.get("retired_units") or []
        if retired:
            lines.append("systemctl disable --now %s >/dev/null 2>&1 || true"
                         % " ".join(shq(u) for u in retired))
            lines += ["rm -f %s" % shq("/etc/systemd/system/" + u)
                      for u in retired]
        lines += [
            "chown -R caddy:caddy /etc/forkmesh-next/tls",
            "/usr/bin/python3 -I /opt/forkmesh-next/bin/fmnext-certsync prepare "
            "--config /etc/forkmesh-next/certsync.json",
        ]
    if manifest.get("job_tmp"):
        lines.append("install -d -o root -g root -m 1777 %s" % shq(manifest["job_tmp"]))
    lines += [
        "install -d -m 0755 /etc/systemd/system-generators",
        "ln -sfn /dev/null /etc/systemd/system-generators/systemd-ssh-generator",
        "systemctl daemon-reload",
        "for unit in %s; do if [ \"$(systemctl show -p LoadState --value \"$unit\")\" = not-found ] "
        "&& systemctl is-active -q \"$unit\"; then systemctl stop \"$unit\" || true; fi; done"
        % " ".join(SSH_GENERATOR_SOCKETS),
        "sysctl --system >/dev/null",
        "systemctl restart systemd-journald",
        ZRAM_START,
        "nft -c -f /etc/nftables.conf",
        FIREWALL_BEFORE_CADDY,
    ]
    if is_core:
        lines.append("if [ \"$rqlite_unit_before\" != \"$(%s)\" ] && "
                     "systemctl is-active -q rqlite.service; then "
                     "systemctl restart rqlite.service; fi" % RQLITE_UNIT_DIGEST)
    lines.append("if [ \"$daemon_unit_before\" != \"$(%s)\" ] && "
                 "systemctl is-active -q forkmesh-mirror-node.service; then "
                 "systemctl restart forkmesh-mirror-node.service; fi" % DAEMON_UNIT_DIGEST)
    if calls:
        lines.append("if [ \"$calls_unit_before\" != \"$(%s)\" ] && "
                     "systemctl is-active -q fmnext-calls.service; then "
                     "systemctl restart fmnext-calls.service; fi" % CALLS_UNIT_DIGEST)
    return "\n".join(lines)


CADDY_INSTALL_WAIT = 180
CADDY_INSTALL_MAX_TIME = 35

RESTART_FAIL2BAN = "systemctl try-restart fail2ban.service || true"

RETIRE_UFW = ("if command -v ufw >/dev/null 2>&1; then ufw --force disable "
              ">/dev/null; systemctl disable --now ufw.service >/dev/null "
              "2>&1 || true; systemctl restart nftables.service; fi")

FIREWALL_BEFORE_CADDY = (
    "if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then "
    "nft -c -f /etc/nftables.conf && systemctl enable nftables.service && "
    "systemctl reload-or-restart nftables.service && " + RETIRE_UFW + "; fi")


IDS_UNIT = "fmnext-ids.service"


def start_script(manifest, start_ids=True):
    units = [u for u in manifest["units"] if u not in (
        "forkmesh-app.service", "forkmesh-mirror-node.service", IDS_UNIT)]
    lines = ["systemctl enable --now %s" % " ".join(units)]
    # Vultr's Debian image ships ufw enabled (only 22/tcp open). Our nftables
    # ruleset replaces it; drop ufw only after that ruleset is loaded, so the
    # box is never left without a firewall.
    retire_ufw = RETIRE_UFW
    if manifest.get("role") != "core":
        lines += ["systemctl enable nftables.service",
                  "systemctl reload-or-restart nftables.service", retire_ufw,
                  RESTART_FAIL2BAN]
    if manifest.get("role") == "core":
        lines += [
            "systemctl enable nftables.service",
            "systemctl reload-or-restart nftables.service wg-quick@wg0.service",
            retire_ufw,
            RESTART_FAIL2BAN,
            "systemctl reload caddy || systemctl restart caddy",
            # The app starts with its first release (tools/next_deploy.py).
            "systemctl enable forkmesh-app.service",
            "if [ -e /opt/forkmesh-next/app/current ]; then systemctl restart forkmesh-app.service; fi",
        ]
    if "fmnext-fleet-peers.service" in manifest.get("units", []):
        lines.append("systemctl try-restart fmnext-certsync.service")
    if manifest.get("turn"):
        lines.append("systemctl try-restart fmnext-turn.service")
    ids = bool(manifest.get("ids")) and start_ids
    if ids:
        lines.append("systemctl try-restart %s" % IDS_UNIT)
    lines.append(logread_authorized_script(bool(manifest.get("logread"))))
    if ids:
        lines.append("systemctl enable --now %s" % IDS_UNIT)
    return "\n".join(lines)


LOGREAD_MARK = " fmnext-logread$"


def logread_authorized_script(enabled):
    import render
    lines = [
        "install -d -m 0700 /root/.ssh",
        "touch /root/.ssh/authorized_keys",
        "keys=/root/.ssh/authorized_keys",
        "next=$(mktemp /root/.ssh/.authorized_keys.fmnext.XXXXXX)",
        "grep -v '%s' \"$keys\" >\"$next\" || true" % LOGREAD_MARK,
    ]
    if enabled:
        lines.append("cat %s >>\"$next\"" % render.LOGREAD_AUTHORIZED_KEY)
    lines += [
        "if ! cmp -s <(grep -v '%s' \"$keys\") <(grep -v '%s' \"$next\"); then "
        "rm -f \"$next\"; echo 'authorized_keys: refusing to change other keys' >&2; "
        "exit 1; fi" % (LOGREAD_MARK, LOGREAD_MARK),
        "if cmp -s \"$keys\" \"$next\"; then rm -f \"$next\"; else "
        "chmod 0600 \"$next\" && mv \"$next\" \"$keys\"; fi",
    ]
    return "\n".join(lines)


def logread_paths(manifest):
    import render
    paths = [render.LOGREAD_PROGRAM, render.LOGREAD_AUTHORIZED_KEY,
             "/etc/forkmesh-next/ids.json"]
    if manifest.get("role") == "core":
        paths.append("/etc/caddy/Caddyfile")
    return paths


def logread_script(manifest, archive):
    is_core = manifest.get("role") == "core"
    lines = [
        "stage=$(mktemp -d /tmp/fmnext-logread.XXXXXX)",
        "trap 'rm -rf \"$stage\"' EXIT",
        "tar -xzf %s -C \"$stage\"" % shq(archive),
        "rm -f %s" % shq(archive),
    ]
    if is_core:
        lines.append("cp -p /etc/caddy/Caddyfile \"$stage/Caddyfile.previous\"")
    for path in logread_paths(manifest):
        mode = manifest["files"][path].replace("0o", "0")
        lines.append("install -D -m %s \"$stage\"%s %s" % (mode, shq(path),
                                                           shq(path)))
    lines += ["systemctl try-restart %s" % IDS_UNIT,
              "sleep 3",
              "systemctl is-active -q %s || { echo '%s is not running: run "
              "the start step first' >&2; exit 1; }" % (IDS_UNIT, IDS_UNIT)]
    if is_core:
        lines.append(
            "if ! systemctl reload caddy; then "
            "install -m 0644 \"$stage/Caddyfile.previous\" /etc/caddy/Caddyfile; "
            "systemctl reload caddy || true; "
            "echo 'caddy refused the new Caddyfile; the previous one is back' >&2; "
            "exit 1; fi")
    lines.append(logread_authorized_script(True))
    return "\n".join(lines)


def logread_ids_config(live_text, fingerprint):
    config = json.loads(live_text)
    ssh_config = config.setdefault("ssh", {})
    prints = list(ssh_config.get("fleet_fingerprints") or [])
    if fingerprint and fingerprint not in prints:
        prints.append(fingerprint)
    ssh_config["fleet_fingerprints"] = prints
    return json.dumps(config, indent=2, sort_keys=True) + "\n"


def logread_guard(fleet, name, ssh, ip):
    import hashlib
    import render
    files, _ = render_node(fleet, name)
    bare, _ = render_node(render.without_logs(fleet), name)
    if "/etc/caddy/Caddyfile" not in files:
        return
    out = ssh.run(ip, "sha256sum /etc/caddy/Caddyfile").stdout.decode()
    live = out.split()[0] if out.split() else ""
    allowed = {hashlib.sha256(files["/etc/caddy/Caddyfile"][0].encode("utf-8"))
               .hexdigest(),
               hashlib.sha256(bare["/etc/caddy/Caddyfile"][0].encode("utf-8"))
               .hexdigest()}
    if live not in allowed:
        raise GuardError(
            "%s: /etc/caddy/Caddyfile on the node is neither this tree's "
            "render nor it without [logs]; another change is pending there. "
            "Roll that first (install %s --steps files,start) or wait for its "
            "owner." % (name, name))


def logread_files(fleet, name, ssh, ip):
    import render
    files, manifest = render_node(fleet, name)
    subset = {p: files[p] for p in logread_paths(manifest)}
    live = ssh.run(ip, "cat /etc/forkmesh-next/ids.json").stdout.decode()
    fingerprint = render.logread_fingerprint(
        render.logs_settings(fleet)["reader_public_key"])
    mode = subset["/etc/forkmesh-next/ids.json"][1]
    subset["/etc/forkmesh-next/ids.json"] = (
        logread_ids_config(live, fingerprint), mode)
    return subset, manifest


def install_logread(fleet, name, ip, ssh, runner):
    import hashlib
    import next_ids
    files, manifest = render_node(fleet, name)
    if not manifest.get("logread"):
        raise ToolError("[logs] reader_public_key is not set in the fleet file "
                        "(tools/next_fleet.py logread-key)")

    def action():
        logread_guard(fleet, name, ssh, ip)
        subset, _ = logread_files(fleet, name, ssh, ip)
        expected = {path: hashlib.sha256(content.encode("utf-8")).hexdigest()
                    for path, (content, _mode) in subset.items()}
        if next_ids.push_expectation(
                fleet, ssh, runner, name, files=expected,
                rescan=next_ids.AUTHORIZED_KEYS,
                allow=("processes", "listeners"),
                reason="next_fleet logread") is False:
            raise ToolError("%s: the IDS did not accept the expectation" % name)
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp, "logread.tar.gz")
            archive.write_bytes(tar_files(subset))
            ssh.put(ip, archive, "/tmp/fmnext-logread.tar.gz")
        ssh.run(ip, logread_script(manifest, "/tmp/fmnext-logread.tar.gz"))
    runner.step("%s: install the log reader, add its key to the live IDS "
                "config%s, then authorize the key (forced command)" % (
                    name, " and the Caddy access log" if manifest.get("role")
                    == "core" else ""), action)


def logread_key(runner):
    key = common.ssh_key_path().with_name("next_logread_ed25519")
    if not key.is_file():
        def generate():
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                            "-C", "fmnext-logread", "-f", str(key)], check=True)
        runner.step("generate %s (the desktop's log reader key)" % key, generate)
    public = key.with_name(key.name + ".pub")
    if public.is_file():
        runner.say("[logs]\nreader_public_key = \"%s\"" % " ".join(
            public.read_text("utf-8").split()[:3]))
    return key


def render_node(fleet, name, replacement=False):
    import render
    if is_core_like(name):
        return render.render_core(fleet, name, replacement)
    return render.render_runner(fleet, name)


def tar_files(files) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for path, (content, mode) in sorted(files.items()):
            data = content.encode("utf-8")
            info = tarfile.TarInfo(path.lstrip("/"))
            info.size = len(data)
            info.mode = mode
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def node_ip(fleet, name):
    import render
    if is_core_like(name):
        return render.node_by_name(fleet, name)["public_ipv4"]
    return render.runner_by_name(fleet, name)["public_ipv4"]


def verify_target_instance(vultr, name, ip):
    """The address we SSH to must belong to a guarded fmnext instance."""
    if hasattr(vultr, "verify_target"):
        return vultr.verify_target(name, ip)
    _, _, label, _ = parse_target(name)
    provider = next_cloud.for_label(label, vultr)
    where = next_cloud.display(next_cloud.recorded_provider(label))
    instance = provider.find_by_label(label) if provider else None
    if instance is None:
        raise GuardError("no %s instance tagged %s on %s" % (label, TAG, where))
    guard_instance(instance)
    if hasattr(provider, "guard"):
        provider.guard(instance)
    if instance.get("main_ip") != ip:
        raise GuardError("%s has IP %s on %s but %s in fleet.toml" % (
            label, instance.get("main_ip"), where, ip))
    return instance


SSH_IDENTITY_SCRIPT = "hostname -f; if test -d /etc/forkmesh-next; then echo fmnext-node; fi"


def verify_target_ssh(ssh, fleet, name, ip):
    if not ssh.pinned(ip):
        return False
    completed = ssh.run_strict(ip, SSH_IDENTITY_SCRIPT)
    lines = (completed.stdout or b"").decode("utf-8", "replace").split()
    if completed.returncode != 0 or not lines:
        raise GuardError("%s (%s): pinned host key check failed: %s" % (
            name, ip, (completed.stderr or b"").decode("utf-8", "replace").strip()[-300:]))
    expected = "%s.%s" % (name, fleet.get("domain", ""))
    if lines[0] not in (expected, name, parse_target(name)[2]):
        raise GuardError("%s answers as %s, not %s" % (ip, lines[0], expected))
    return "fmnext-node" in lines


def verify_target(vultr, ssh, fleet, name, ip):
    if getattr(ssh, "pinned", None) and verify_target_ssh(ssh, fleet, name, ip):
        return None
    return verify_target_instance(vultr, name, ip)


def rqlite_tarball(fleet):
    rq = fleet.get("rqlite") or {}
    version = rq.get("version", "v10.3.6")
    path = common.data_dir() / "dl" / ("rqlite-%s-linux-amd64.tar.gz" % version)
    return version, rq.get("sha256", ""), path


SSH_PORT_CONF = "/etc/ssh/sshd_config.d/05-fmnext-port.conf"
SSH_PORT_STAGE = "/run/fmnext-ssh-port"
SSH_PORT_ROLLBACK_UNIT = "fmnext-ssh-port-rollback"
SSH_PORT_ROLLBACK_SECONDS = 180
SSH_PORT_FILES = (("nftables", "/etc/nftables.conf"),
                  ("ids", "/etc/forkmesh-next/ids.json"),
                  ("fail2ban", "/etc/fail2ban/jail.d/fmnext-sshd.local"),
                  ("port", SSH_PORT_CONF))
NFT_MANAGED = "Managed by deploy/core/render.py"


def ssh_port_files(fleet, name, replacement=False):
    import render
    files, _ = render_node(fleet, name, replacement)
    subset = {path: files[path] for _key, path in SSH_PORT_FILES
              if path in files}
    subset[SSH_PORT_CONF] = ("Port %d\n" % render.ssh_port(fleet), 0o644)
    return subset


def ssh_port_image_script(fleet):
    import render
    port = render.ssh_port(fleet)
    return "\n".join([
        "install -d -m 0755 /etc/ssh/sshd_config.d",
        "printf 'Port %d\\n' >%s" % (port, SSH_PORT_CONF),
        "sshd -t",
        "if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | "
        "grep -q 'Status: active'; then ufw allow %d/tcp >/dev/null; fi" % port])


def ssh_port_script(port, files, archive):
    stage = SSH_PORT_STAGE
    restore = []
    for key, path in SSH_PORT_FILES:
        restore.append("if [ -e %s/%s.bak ]; then cp -p %s/%s.bak %s; else rm -f %s; fi"
                       % (stage, key, stage, key, shq(path), shq(path)))
    rollback = "\n".join(restore + [
        "if [ -e %s/nft ]; then nft -f /etc/nftables.conf || true; fi" % stage,
        "if [ -e %s/ufw ]; then ufw delete allow %d/tcp >/dev/null || true; fi"
        % (stage, port),
        "systemctl reload ssh || true",
        "systemctl try-restart fmnext-ids.service fail2ban.service || true",
        "echo 'fmnext ssh-port: rolled back to the previous sshd and firewall'"])
    lines = [
        "if systemctl is-active -q ssh.socket; then echo 'ssh.socket is "
        "active: sshd would ignore Port; nothing changed' >&2; exit 3; fi",
        "systemctl stop %s.timer >/dev/null 2>&1 || true" % SSH_PORT_ROLLBACK_UNIT,
        "install -d -m 0700 %s %s/new" % (stage, stage),
        "rm -f %s/*.bak %s/nft %s/ufw" % (stage, stage, stage),
        "tar -xzf %s -C %s/new" % (shq(archive), stage),
        "rm -f %s" % shq(archive),
    ]
    for key, path in SSH_PORT_FILES:
        lines.append("if [ -e %s ]; then cp -p %s %s/%s.bak; fi"
                     % (shq(path), shq(path), stage, key))
    lines += [
        "cat >%s/rollback.sh <<'ROLLBACK'\n%s\nROLLBACK" % (stage, rollback),
        "systemd-run --quiet --unit=%s --on-active=%d /bin/bash %s/rollback.sh"
        % (SSH_PORT_ROLLBACK_UNIT, SSH_PORT_ROLLBACK_SECONDS, stage),
    ]
    for path, (_content, mode) in sorted(files.items()):
        lines.append("install -D -m %04o %s/new%s %s" % (mode, stage, path, shq(path)))
    lines += [
        "if ! sshd -t; then bash %s/rollback.sh; exit 4; fi" % stage,
        "ports=$(sshd -T | awk '$1 == \"port\" {print $2}' | sort -u | tr '\\n' ' ')",
        "if [ \"$ports\" != '%d ' ]; then echo \"sshd would listen on $ports"
        "(another Port line in /etc/ssh/sshd_config?)\" >&2; bash %s/rollback.sh; "
        "exit 5; fi" % (port, stage),
        "if grep -qF %s %s/nftables.bak 2>/dev/null && systemctl is-active -q "
        "nftables.service; then nft -c -f /etc/nftables.conf && nft -f "
        "/etc/nftables.conf && touch %s/nft; elif command -v ufw >/dev/null 2>&1 && "
        "ufw status 2>/dev/null | grep -q 'Status: active'; then ufw allow %d/tcp "
        ">/dev/null && touch %s/ufw; fi" % (shq(NFT_MANAGED), stage, stage, port, stage),
        "systemctl reload ssh",
        "systemctl try-restart fmnext-ids.service fail2ban.service || true",
    ]
    return "\n".join(lines)


def ssh_port_disarm_script():
    stage = SSH_PORT_STAGE
    return "\n".join([
        "systemctl stop %s.timer >/dev/null 2>&1 || true" % SSH_PORT_ROLLBACK_UNIT,
        "rm -f %s/*.bak %s/nft %s/ufw %s/rollback.sh" % (stage, stage, stage, stage)])


def install_ssh_port(fleet, name, ip, ssh, runner, replacement=False):
    import render
    port = render.ssh_port(fleet)
    files = ssh_port_files(fleet, name, replacement)

    def action():
        current = ssh.port(ip)
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp, "ssh-port.tar.gz")
            archive.write_bytes(tar_files(files))
            ssh.put(ip, archive, "/tmp/fmnext-ssh-port.tar.gz")
        ssh.run(ip, ssh_port_script(port, files, "/tmp/fmnext-ssh-port.tar.gz"))
        ssh.use_port(ip, port)
        completed = ssh.run_strict(ip, SSH_IDENTITY_SCRIPT)
        lines = (completed.stdout or b"").decode("utf-8", "replace").split()
        if completed.returncode != 0 or not lines or lines[0] not in (
                "%s.%s" % (name, fleet.get("domain", "")), name,
                parse_target(name)[2]):
            ssh.use_port(ip, current)
            raise ToolError(
                "%s: no pinned answer on port %d (%s); the node puts its "
                "previous sshd and firewall back within %d s" % (
                    name, port, (completed.stderr or b"").decode(
                        "utf-8", "replace").strip()[-300:],
                    SSH_PORT_ROLLBACK_SECONDS))
        ssh.run(ip, ssh_port_disarm_script())
    runner.step("%s: move sshd to port %d (firewall, IDS and fail2ban too), "
                "check it over a new connection, roll back by itself in %d s "
                "otherwise" % (name, port, SSH_PORT_ROLLBACK_SECONDS), action)


def install(fleet, name, ssh, runner, steps, vultr=None, replacement=False,
            state_path=None, idle_guard=False):
    ip = node_ip(fleet, name)
    runner.say("install %s (%s): steps %s" % (name, ip, ", ".join(steps)))
    if runner.execute:
        verify_target(vultr, ssh, fleet, name, ip)
    role = "core" if is_core_like(name) else "runner"
    import next_ids
    import render
    if runner.execute and "files" in steps and "ssh-port" not in steps[
            :steps.index("files")] and hasattr(ssh, "port") and ssh.port(
            ip) != render.ssh_port(fleet):
        runner.say("%s answers on SSH port %d, not %d: moving sshd before the "
                   "files step closes the old port" % (
                       name, ssh.port(ip), render.ssh_port(fleet)))
        steps = list(steps)
        steps.insert(steps.index("files"), "ssh-port")
    for step in steps:
        # Tell the node's IDS first, so the step's changes are expected
        # (a no-op until the IDS runs there; docs/IDS.md).
        next_ids.expect_step(fleet, ssh, runner, name, step, replacement)
        if step == "base":
            script = base_script(role)
            runner.step("%s: apt packages, users, dirs, swapfile, key-only sshd" % name,
                        lambda: ssh.run(ip, script))
        elif step == "ssh-port":
            install_ssh_port(fleet, name, ip, ssh, runner, replacement)
        elif step == "wireguard":
            def keygen():
                out = ssh.run(ip, WIREGUARD_KEY_SCRIPT).stdout.decode().strip()
                if not re.fullmatch(r"[A-Za-z0-9+/]{43}=", out):
                    raise ToolError("unexpected WireGuard public key from %s" % name)
                path = state_path or common.config_dir() / "fleet-state.json"
                state = common.load_json(path, {}) or {}
                state.setdefault("wg_pubkeys", {})[name] = out
                common.save_json(path, state)
                import render
                render.node_by_name(fleet, name)["wg_pubkey"] = out
                runner.say("%s WireGuard public key recorded in fleet-state.json" % name)
            runner.step("%s: generate the WireGuard key on the node (private key "
                        "stays there)" % name, keygen)
        elif step == "rqlite":
            version, sha256, tarball = rqlite_tarball(fleet)

            def upload_rqlite():
                digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
                if digest != sha256:
                    raise GuardError("local %s sha256 %s != fleet %s" % (
                        tarball.name, digest, sha256))
                ssh.put(ip, tarball, "/tmp/%s" % tarball.name)
                ssh.run(ip, rqlite_script(version, sha256))
            runner.step("%s: install rqlite %s (sha256 %s..., verified locally and "
                        "on the node)" % (name, version, sha256[:12]), upload_rqlite)
        elif step == "files":
            def upload_files():
                files, manifest = render_node(fleet, name, replacement)
                with tempfile.TemporaryDirectory() as tmp:
                    archive = Path(tmp, "files.tar.gz")
                    archive.write_bytes(tar_files(files))
                    ssh.put(ip, archive, "/tmp/fmnext-files.tar.gz")
                ssh.run(ip, files_script(manifest, "/tmp/fmnext-files.tar.gz"))
            runner.step("%s: render deploy files (no secrets) and install them" % name,
                        upload_files)
        elif step == "secrets":
            push_secrets(fleet, name, ssh, runner)
        elif step == "caddy":
            install_caddy(fleet, name, ip, ssh, runner)
        elif step == "caddy-debian":
            restore_caddy_debian(fleet, name, ip, ssh, runner)
        elif step == "turn":
            if not is_core_like(name):
                raise ToolError("step turn is for core nodes only")
            install_turn(fleet, name, ip, ssh, runner)
        elif step == "calls":
            if not is_core_like(name):
                raise ToolError("step calls is for core nodes only")
            install_calls(fleet, name, ip, ssh, runner)
        elif step == "daemon":
            install_daemon(fleet, name, ip, ssh, runner, idle_guard=idle_guard)
        elif step == "ids":
            install_ids_key(fleet, name, ip, ssh, runner, state_path)
        elif step == "logread":
            install_logread(fleet, name, ip, ssh, runner)
        elif step == "start":
            def start():
                _, manifest = render_node(fleet, name, replacement)
                ssh.run(ip, start_script(manifest))
            runner.step("%s: enable and start units" % name, start)
        else:
            raise ToolError("unknown install step %r" % step)


IDS_KEYGEN_SCRIPT = """
umask 077
install -d -m 0700 /var/lib/fmnext-ids
/usr/bin/python3 -I - <<'PY'
import base64, os
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
path = "/var/lib/fmnext-ids/sign.key"
b64 = lambda b: base64.urlsafe_b64encode(b).decode().rstrip("=")
if not os.path.exists(path):
    raw = Ed25519PrivateKey.generate().private_bytes(
        serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
        serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(b64(raw) + "\\n")
text = open(path).read().strip()
raw = base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))
key = Ed25519PrivateKey.from_private_bytes(raw)
print(b64(key.public_key().public_bytes(serialization.Encoding.Raw,
                                        serialization.PublicFormat.Raw)))
PY
"""


def install_ids_key(fleet, name, ip, ssh, runner, state_path=None):
    """Generate the node's IDS signing key on the node (the private key never
    leaves it) and record the public key in fleet-state.json; the files step
    renders it into every core's app.env and ids.json."""
    def keygen():
        out = ssh.run(ip, IDS_KEYGEN_SCRIPT).stdout.decode().strip()
        public = out.splitlines()[-1] if out else ""
        if not re.fullmatch(r"[A-Za-z0-9_-]{43}", public):
            raise ToolError("unexpected IDS public key from %s" % name)
        path = state_path or common.config_dir() / "fleet-state.json"
        state = common.load_json(path, {}) or {}
        state.setdefault("ids_pubkeys", {})[name] = public
        common.save_json(path, state)
        for item in list(fleet.get("nodes") or []) + list(
                fleet.get("app_nodes") or []) + list(
                fleet.get("runners") or []):
            if item["name"] == name:
                item["ids_pubkey"] = public
        runner.say("%s IDS public key recorded in fleet-state.json; re-render "
                   "every node (install nN --steps files,start)" % name)
    runner.step("%s: generate the IDS signing key on the node (private key "
                "stays there)" % name, keygen)


def daemon_script(fleet, name, version, idle_guard=False):
    import render
    role = "core" if name.startswith("n") else "runner"
    guard = [RUNNER_BUSY_SCRIPT] if idle_guard and role == "runner" else []
    app_host = render.served_app_host(fleet)
    # --init always wants a public origin; a runner serves nothing, so its
    # (unresolvable) per-node name is only a placeholder in the gateway file,
    # and the merge below drops mirrorUrl from a runner's config.
    origin = "https://%s.%s" % (name, fleet["domain"])
    # The daemon's own user runs --check: root on a runner (runner.jobUid
    # needs it), forkmesh-node on a core.
    check = ("/usr/local/bin/forkmesh-mirror-node" if role == "runner" else
             "runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node")
    init = [
        "runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node --init",
        "--config /etc/forkmesh/mirror-node.json.init --state-dir /var/lib/forkmesh",
        "--node %s --owner forkmesh --repository forkmesh" % shq("fmnext-" + name),
        "--upstream %s" % shq("https://%s/forkmesh/forkmesh" % app_host),
        "--catalog-url %s" % shq("https://%s/api/repositories" % app_host),
        "--disable-cloudflared --version %s" % shq(version),
        '--router-public-key "$(cat /tmp/fmnext-router-key)"',
    ]
    init.append("--public-origin %s --mirror-url %s" % (shq(origin), shq(origin)))
    return "\n".join(guard + [
        "stage=/tmp/fmnext-daemon",
        "install -m 0755 $stage/forkmesh-mirror-node /usr/local/bin/forkmesh-mirror-node",
        "install -d -m 0755 /usr/local/share/forkmesh/tools",
        "for f in $stage/tools/*; do install -m 0644 \"$f\" /usr/local/share/forkmesh/tools/; done",
        "if [ ! -s /etc/forkmesh/mirror-node.json ]; then",
        "  " + " \\\n    ".join(init) + " >/dev/null",
        "  mv /etc/forkmesh/mirror-node.json.init /etc/forkmesh/mirror-node.json",
        "fi",
        "python3 - <<'PY'",
        "import json, os",
        "key = open('/tmp/fmnext-router-key').read().strip()",
        "origin = %r" % ("https://" + app_host),
        "assets_root = %r" % (
            render.fleet_paths(fleet)["current"] + "/app/dist" if role == "core" else ""),
        "gw = json.load(open('/etc/forkmesh/mirror-node.json')).get('gatewayConfig')",
        "if gw and os.path.exists(gw):",
        "    doc = json.load(open(gw))",
        "    changed = []",
        "    if key and doc.get('routerPublicKey') != key:",
        "        doc['routerPublicKey'] = key",
        "        changed.append('router public key re-pinned from the vault value')",
        "    dash = doc.get('dashboard')",
        "    if isinstance(dash, dict) and dash.get('replicaPath') and dash.get('canonicalOrigin') != origin:",
        "        dash['canonicalOrigin'] = origin",
        "        changed.append('dashboard replica source set to ' + origin)",
        "    if isinstance(dash, dict) and assets_root and dash.get('assetsRoot') != assets_root:",
        "        dash['assetsRoot'] = assets_root",
        "        changed.append('dashboard pages served from ' + assets_root)",
        "    receive = %r" % bool(
            role == "core" and (fleet.get("app") or {}).get("flagship_receive_pack")),
        "    for entry in doc.get('repositories') or []:",
        "        if (entry.get('owner'), entry.get('name')) != ('forkmesh', 'forkmesh'):",
        "            continue",
        "        ops = set(entry.get('operations') or [])",
        "        if receive and 'git-receive-pack' not in ops:",
        "            entry['operations'] = sorted(ops | {'git-receive-pack'})",
        "            changed.append('forkmesh/forkmesh accepts signed pushes')",
        "        elif not receive and 'git-receive-pack' in ops:",
        "            entry['operations'] = sorted(ops - {'git-receive-pack'})",
        "            changed.append('forkmesh/forkmesh is read-only')",
        "    if changed:",
        "        st = os.stat(gw)",
        "        open(gw + '.new', 'w').write(json.dumps(doc, indent=2) + '\\n')",
        "        os.chmod(gw + '.new', st.st_mode & 0o7777)",
        "        os.chown(gw + '.new', st.st_uid, st.st_gid)",
        "        os.replace(gw + '.new', gw)",
        "        print('; '.join(changed))",
        "PY",
        "rm -f /tmp/fmnext-router-key",
        # Merge the rendered overrides (role, agent jobs/Actions off on core;
        # runner settings on a runner). An older daemon without "role"
        # rejects unknown fields: fall back to the config without it and warn.
        "python3 - <<'PY'",
        "import json",
        "cfg = json.load(open('/etc/forkmesh/mirror-node.json'))",
        "cfg.update(json.load(open('/etc/forkmesh-next/mirror-node.overrides.json')))",
        "cfg['version'] = %r" % version,
        # --init always writes one repository into the gateway document, and
        # a runner daemon refuses to start with any (it keeps no repositories).
        "if cfg.get('role') == 'runner':",
        "    import pwd",
        "    cfg.setdefault('runner', {})['jobUid'] = pwd.getpwnam(%r).pw_uid" % JOB_USER,
        "    if cfg['runner']['jobUid'] == 0:",
        "        raise SystemExit('%s must not be root')" % JOB_USER,
        "    cfg.pop('mirrorUrl', None)",
        "    gw = cfg['gatewayConfig']",
        "    doc = json.load(open(gw))",
        "    if doc.get('repositories'):",
        "        doc['repositories'] = []",
        "        open(gw + '.new', 'w').write(json.dumps(doc, indent=2) + '\\n')",
        "        import os, shutil",
        "        shutil.copymode(gw, gw + '.new')",
        "        st = os.stat(gw)",
        "        os.chown(gw + '.new', st.st_uid, st.st_gid)",
        "        os.replace(gw + '.new', gw)",
        "open('/etc/forkmesh/mirror-node.json.new', 'w').write(json.dumps(cfg, indent=2) + '\\n')",
        "cfg.pop('role', None)",
        "cfg.pop('hosted', None)",
        "open('/etc/forkmesh/mirror-node.json.norole', 'w').write(json.dumps(cfg, indent=2) + '\\n')",
        "PY",
        "chown forkmesh-node:forkmesh-node /etc/forkmesh/mirror-node.json.new /etc/forkmesh/mirror-node.json.norole",
        "if %s --config /etc/forkmesh/mirror-node.json.new --check; then" % check,
        "  mv /etc/forkmesh/mirror-node.json.new /etc/forkmesh/mirror-node.json; rm -f /etc/forkmesh/mirror-node.json.norole",
        "else",
        "  echo 'WARNING: this daemon does not know \"role\" yet; installed without it' >&2",
        "  mv /etc/forkmesh/mirror-node.json.norole /etc/forkmesh/mirror-node.json; rm -f /etc/forkmesh/mirror-node.json.new",
        "fi",
        "rm -rf $stage",
        # A runner's root daemon uses the system manager; drop the old
        # forkmesh-node user-manager runtime dir.
        "if [ -f /etc/forkmesh/node.env ]; then sed -i '/^XDG_RUNTIME_DIR=/d' /etc/forkmesh/node.env; fi",
        # The node key, mirror-node.json (connectorToken) and the daemon's
        # state are owner-only: a job user or any other local uid reads none.
        "chmod 0700 /etc/forkmesh /var/lib/forkmesh",
        "chmod -R go-rwx /etc/forkmesh /var/lib/forkmesh",
        "systemctl daemon-reload",
        "systemctl enable forkmesh-mirror-node.service",
        "systemctl restart forkmesh-mirror-node.service",
    ])


def install_daemon(fleet, name, ip, ssh, runner, idle_guard=False):
    import next_deploy
    dist = next_deploy.dist_dir()
    binary = dist / "forkmesh-mirror-node"

    def action():
        dist.mkdir(parents=True, exist_ok=True)
        subprocess.run(["go", "build", "-trimpath", "-o", str(binary),
                        "./cmd/forkmesh-mirror-node"], cwd=common.REPO / "server",
                       check=True, env=dict(common.os.environ, CGO_ENABLED="0",
                                            GOOS="linux", GOARCH="amd64"))
        values = common.read_env_file(secrets_path())
        router_key = values.get("MIRROR_ROUTER_PUBLIC_KEY", "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{43}", router_key):
            raise ToolError("MIRROR_ROUTER_PUBLIC_KEY missing from secrets.env")
        ssh.run(ip, "rm -rf /tmp/fmnext-daemon && install -d -m 0700 /tmp/fmnext-daemon/tools")
        ssh.put(ip, binary, "/tmp/fmnext-daemon/forkmesh-mirror-node")
        ssh.put(ip, [common.REPO / tool for tool in DAEMON_TOOLS],
                "/tmp/fmnext-daemon/tools/")
        ssh.run(ip, "umask 077; cat >/tmp/fmnext-router-key",
                input_bytes=router_key.encode())
        version = common.release_version()
        ssh.run(ip, daemon_script(fleet, name, version, idle_guard=idle_guard))
    runner.step("%s: build server/cmd/forkmesh-mirror-node, install it with its "
                "tools, init its identity, merge the %s overrides" % (
                    name, "core" if name.startswith("n") else "runner"), action)


ROLL_STEPS = ("daemon", "turn", "calls", "files", "start")
ROLL_HEALTH_SECONDS = 90
ROLL_READD_SECONDS = 600
ROLL_DRAIN_MARGIN_SECONDS = 30


ROLL_LOCK_WAIT_SECONDS = 30 * 60


def roll_healthy(ssh, ip):
    import next_ids
    completed = ssh.run(ip, "curl -s -o /dev/null -w '%{http_code}' "
                        "--max-time 5 http://127.0.0.1:8080/readyz; echo; "
                        "systemctl is-active forkmesh-mirror-node forkmesh-app rqlite",
                        check=False, timeout=60)
    lines = (completed.stdout or b"").decode("utf-8", "replace").split()
    if not lines or lines[0] != "200" or any(x != "active" for x in lines[1:]):
        return False, "readyz/units: %s" % " ".join(lines)
    ids = next_ids.ids_command(ssh, ip, "status", timeout=60)
    if ids.get("lockdown") or ids.get("error"):
        return False, "IDS: %s" % (ids.get("error") or "lockdown")
    return True, "readyz 200, units active, IDS normal"


def in_dns(name):
    import next_dns
    state = common.load_json(next_dns.state_path(), {}) or {}
    return bool(((state.get("nodes") or {}).get(name) or {}).get("in_dns"))


def roll(fleet, names, ssh, runner, steps=ROLL_STEPS, mail=False, vultr=None,
         fleet_path=None, sleep=time.sleep, clock=time.monotonic):
    import next_dns
    extra = ["--fleet", str(fleet_path)] if fleet_path else []
    done = []
    if runner.execute and "turn" in steps:
        turn_payloads(fleet, common.read_env_file(secrets_path()))
    if runner.execute and "calls" in steps:
        calls_payloads(fleet, common.read_env_file(secrets_path()))
    for name in names:
        ip = node_ip(fleet, name)
        started = clock()
        if runner.execute:
            verify_target(vultr, ssh, fleet, name, ip)
            if next_dns.main(extra + ["drain", name, "--execute"]) != 0:
                raise ToolError("%s: drain refused; rolled %s" % (
                    name, ", ".join(done) or "nothing"))
            drained = next_dns.TTL + ROLL_DRAIN_MARGIN_SECONDS
            runner.say("%s: out of DNS; waiting %d s (TTL %d s + %d s) for "
                       "resolvers to forget it before it restarts" % (
                           name, drained, next_dns.TTL,
                           ROLL_DRAIN_MARGIN_SECONDS))
            sleep(drained)
        else:
            runner.say("[dry-run] %s: drain from DNS, wait out its TTL" % name)
        install(fleet, name, ssh, runner, steps, vultr=vultr)
        if mail:
            import next_mail
            next_mail.install(fleet, name, ssh, runner)
        if not runner.execute:
            runner.say("[dry-run] %s: wait for readyz, units and IDS, then "
                       "undrain" % name)
            continue
        deadline = clock() + ROLL_HEALTH_SECONDS
        while True:
            ok, note = roll_healthy(ssh, ip)
            if ok or clock() >= deadline:
                break
            sleep(3)
        runner.say("%s: %s" % (name, note))
        if not ok:
            raise ToolError("%s is unhealthy after its roll (%s); left out of "
                            "DNS; rolled %s" % (name, note,
                                                ", ".join(done) or "nothing"))
        next_dns.main(extra + ["undrain", name, "--execute"])
        deadline = clock() + ROLL_READD_SECONDS
        while not in_dns(name) and clock() < deadline:
            sleep(5)
        if not in_dns(name):
            raise ToolError("%s is healthy but not back in DNS after %d s; "
                            "stopped before the next node" % (
                                name, ROLL_READD_SECONDS))
        done.append(name)
        runner.say("%s: rolled and back in DNS in %d s" % (
            name, clock() - started))
    return done


RUNNER_IDLE_WAIT_SECONDS = 15 * 60
RUNNER_IDLE_POLL_SECONDS = 15
RUNNER_BUSY_EXIT = 75
RUNNERS_BUSY_EXIT = 3
RUNNER_BUSY_MARK = "fmnext-runner-busy"
RUNNER_JOBS_SCRIPT = ("systemctl list-units --no-legend --plain --state=active "
                      "'fmnext-job-*' | grep -c . || true")
RUNNER_BUSY_SCRIPT = ("if [ -n \"$(systemctl list-units --no-legend --plain --state=active "
                      "'fmnext-job-*')\" ]; then echo %s >&2; exit %d; fi" % (
                          RUNNER_BUSY_MARK, RUNNER_BUSY_EXIT))


class RunnersBusy(ToolError):
    pass


def runner_names(fleet):
    return [r["name"] for r in fleet.get("runners") or []]


def runner_jobs(ssh, ip):
    completed = ssh.run(ip, RUNNER_JOBS_SCRIPT, timeout=60)
    text = (completed.stdout or b"").decode("utf-8", "replace").strip()
    return int(text) if text.isdigit() else 0


def runner_healthy(ssh, ip):
    completed = ssh.run(ip, "systemctl is-active forkmesh-mirror-node", check=False,
                        timeout=60)
    state = (completed.stdout or b"").decode("utf-8", "replace").strip()
    return state == "active", "forkmesh-mirror-node %s" % (state or "unknown")


def roll_runners(fleet, names, ssh, runner, vultr=None,
                 idle_wait=RUNNER_IDLE_WAIT_SECONDS, sleep=time.sleep,
                 clock=time.monotonic):
    done, busy = [], []
    for name in names:
        ip = node_ip(fleet, name)
        if not runner.execute:
            runner.say("[dry-run] %s: wait up to %d s until no agent job runs" % (
                name, idle_wait))
            install(fleet, name, ssh, runner, ("daemon",), vultr=vultr)
            continue
        verify_target(vultr, ssh, fleet, name, ip)
        deadline = clock() + idle_wait
        jobs = runner_jobs(ssh, ip)
        if jobs:
            runner.say("%s: %d agent job(s) running; waiting up to %d s" % (
                name, jobs, idle_wait))
        while jobs and clock() < deadline:
            sleep(RUNNER_IDLE_POLL_SECONDS)
            jobs = runner_jobs(ssh, ip)
        if jobs:
            runner.say("%s: still %d agent job(s) running; not rolled" % (name, jobs))
            busy.append(name)
            continue
        try:
            install(fleet, name, ssh, runner, ("daemon",), vultr=vultr, idle_guard=True)
        except ToolError as error:
            if RUNNER_BUSY_MARK not in str(error):
                raise
            runner.say("%s: a job started during the upload; not rolled" % name)
            busy.append(name)
            continue
        deadline = clock() + ROLL_HEALTH_SECONDS
        while True:
            ok, note = runner_healthy(ssh, ip)
            if ok or clock() >= deadline:
                break
            sleep(3)
        runner.say("%s: %s" % (name, note))
        if not ok:
            raise ToolError("%s is unhealthy after its roll (%s); rolled %s" % (
                name, note, ", ".join(done) or "nothing"))
        done.append(name)
    if busy:
        raise RunnersBusy("busy with agent jobs: %s; rolled %s" % (
            ", ".join(busy), ", ".join(done) or "nothing"))
    return done


# --------------------------------------------------------------------------
# secrets


def secrets_path():
    return common.config_dir() / "secrets.env"


def required_secret_names(fleet):
    import render
    names = render.app_secret_names(fleet)
    extras = ["FORKMESH_MESH_SECRET", "RQLITE_PASSWORD", CERTSYNC_SECRET]
    if render.ids_enabled(fleet):
        extras.append(IDS_MASTER)
    if render.turn_enabled(fleet):
        extras.append(render.TURN_SECRET_CREDENTIAL)
    if render.calls_enabled(fleet):
        extras.append(render.CALLS_SECRET_CREDENTIAL)
    for extra in extras:
        if extra not in names:
            names.append(extra)
    return names


def ids_payloads(fleet, name, values):
    """The IDS credential of one node or runner: its own expected-change
    key (derived from the desktop-only master)."""
    import render
    import next_ids
    if not render.ids_enabled(fleet):
        return {}
    master = values.get(IDS_MASTER, "")
    if not master:
        raise ToolError("%s is missing from secrets.env (run init-secrets "
                        "--execute)" % IDS_MASTER)
    return {render.IDS_KEY_CREDENTIAL: next_ids.node_key(
        master, name).encode()}


def router_keypair():
    """(seed, public) base64url, as entry.py _https_mirror_router_seed wants."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    seed = _secrets.token_bytes(32)
    public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    encode = lambda b: base64.urlsafe_b64encode(b).decode().rstrip("=")  # noqa: E731
    return encode(seed), encode(public)


def init_secrets(fleet, runner):
    path = secrets_path()
    current = common.read_env_file(path) if path.is_file() else {}
    missing = [n for n in required_secret_names(fleet) if not current.get(n)]
    # DATA_KEY lives in config/DATA_KEY (0400) once the vault is on and never
    # in secrets.env: generating one here would leave a second, unrelated key
    # next to the real one (a re-key or restore could then pick the wrong one).
    if "DATA_KEY" in missing and (common.config_dir() / "DATA_KEY").is_file():
        missing.remove("DATA_KEY")
    if "MIRROR_ROUTER_PUBLIC_KEY" in missing and current.get("MIRROR_ROUTER_SIGNING_SEED"):
        raise ToolError("MIRROR_ROUTER_PUBLIC_KEY is missing but the seed exists; "
                        "derive it by hand rather than rotating the seed")
    if not missing:
        runner.say("secrets.env already has every required secret")
        return []

    def write():
        values = dict(current)
        if "MIRROR_ROUTER_SIGNING_SEED" in missing or "MIRROR_ROUTER_PUBLIC_KEY" in missing:
            values["MIRROR_ROUTER_SIGNING_SEED"], values["MIRROR_ROUTER_PUBLIC_KEY"] = \
                router_keypair()
        for name in missing:
            if not values.get(name):
                generator = DEFAULT_SECRETS.get(name)
                if generator is None:
                    raise ToolError("%s has no generator; add it to %s by hand"
                                    % (name, path))
                values[name] = generator()
        text = "".join("%s=%s\n" % (k, v) for k, v in sorted(values.items()))
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = common.os.open(path, common.os.O_WRONLY | common.os.O_CREAT
                            | common.os.O_TRUNC, 0o600)
        with common.os.fdopen(fd, "w") as handle:
            handle.write(text)
    runner.step("generate %s in %s (values are never printed)" % (
        ", ".join(missing), path), write)
    return missing


def rqlite_auth_json(user, password) -> bytes:
    """rqlite -auth file. The anonymous "notify" permission is required for
    -bootstrap-expect: bootstrap notifications are sent without the -join-as
    credentials, and without it the cluster never forms (checked against
    rqlited v10.3.6). Raft and the HTTP API stay closed to anonymous users
    for everything else; Raft is reachable only over wg0 from core peers."""
    return json.dumps([
        {"username": user, "password": password, "perms": ["all"]},
        {"username": "*", "perms": ["notify"]},
    ]).encode()


def credential_payloads(fleet, name, values):
    """{credential name: bytes} for node ``name``; built only in memory."""
    import render
    if not is_core_like(name):
        return {}
    payloads = {}
    # Vault on: only the bootstrap credentials; the rest is sealed into the
    # database by tools/next_vault.py and DATA_KEY never reaches the node.
    for secret in render.app_credential_names(fleet):
        if not values.get(secret):
            raise ToolError("%s is missing from secrets.env" % secret)
        payloads[secret] = values[secret].encode()
    if not values.get("RQLITE_PASSWORD"):
        raise ToolError("RQLITE_PASSWORD is missing from secrets.env")
    user = (fleet.get("rqlite") or {}).get("user", "fmnext")
    payloads["rqlite-auth.json"] = rqlite_auth_json(user, values["RQLITE_PASSWORD"])
    payloads["FORKMESH_MESH_SECRET"] = values["FORKMESH_MESH_SECRET"].encode()
    if not values.get(CERTSYNC_SECRET):
        raise ToolError("%s is missing from secrets.env (run init-secrets "
                        "--execute)" % CERTSYNC_SECRET)
    if values[CERTSYNC_SECRET] == values["FORKMESH_MESH_SECRET"]:
        raise ToolError("%s must differ from FORKMESH_MESH_SECRET" % CERTSYNC_SECRET)
    payloads[CERTSYNC_SECRET] = values[CERTSYNC_SECRET].encode()
    if render.AUTOSCALE_CERTSYNC_CREDENTIAL in node_local_credential_names(
            fleet, name):
        replica = values.get(render.AUTOSCALE_CERTSYNC_CREDENTIAL, "")
        if len(replica) < 32 or replica == values[CERTSYNC_SECRET]:
            raise ToolError("%s is missing from secrets.env or equals the core "
                            "certsync secret (next_autoscale.py init-secrets)"
                            % render.AUTOSCALE_CERTSYNC_CREDENTIAL)
        payloads[render.AUTOSCALE_CERTSYNC_CREDENTIAL] = replica.encode()
    payloads.update(turn_payloads(fleet, values))
    if render.CALLS_SECRET_CREDENTIAL in node_local_credential_names(fleet, name):
        payloads.update(calls_payloads(fleet, values))
    payloads.update(ids_payloads(fleet, name, values))
    return payloads


ENCRYPT_SCRIPT = """
umask 077
name=%s
install -d -m 0700 /etc/forkmesh-next/creds
systemd-creds encrypt --with-key=host --name="$name" - "/etc/forkmesh-next/creds/$name.cred.new"
mv "/etc/forkmesh-next/creds/$name.cred.new" "/etc/forkmesh-next/creds/$name.cred"
"""

CURLRC_SCRIPT = """
umask 077
cat >/etc/forkmesh-next/rqlite.curlrc.new
mv /etc/forkmesh-next/rqlite.curlrc.new /etc/forkmesh-next/rqlite.curlrc
"""


CRED_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]{1,63}$")

REMOVE_CREDS_SCRIPT = """
unit=/etc/systemd/system/forkmesh-app.service
if ! grep -qx 'Environment=FORKMESH_VAULT=1' "$unit"; then
  echo "the installed forkmesh-app unit is not in vault mode; run the files step first" >&2
  exit 3
fi
for name in %s; do
  f="/etc/forkmesh-next/creds/$name.cred"
  if [ -f "$f" ] && [ ! -L "$f" ]; then rm -f -- "$f" && echo "removed $name.cred"; fi
done
"""


def node_local_credential_names(fleet, name):
    import render
    names = set()
    if render.autoscale_enabled(fleet) and name.startswith("n"):
        names.add(render.AUTOSCALE_CERTSYNC_CREDENTIAL)
    if render.turn_enabled(fleet) and is_core_like(name):
        names.add(render.TURN_SECRET_CREDENTIAL)
    if render.calls_enabled(fleet) and name.startswith("n"):
        names.add(render.CALLS_SECRET_CREDENTIAL)
    return names


def stale_credential_names(fleet, name=""):
    """Vault on: credential files of app variables that now live in the
    vault (and DATA_KEY, which must not be on the node at all)."""
    import render
    if not render.vault_enabled(fleet):
        return []
    keep = node_local_credential_names(fleet, name)
    names = [item for item in
             [render.VAULT_KEY_NAME] + render.vault_secret_names(fleet)
             if item not in keep]
    for item in names:
        if not CRED_NAME_RE.match(item) or item in render.VAULT_BOOTSTRAP_NAMES:
            raise GuardError("refusing to remove credential %r" % item)
    return names


def turn_payloads(fleet, values):
    import render
    if not render.turn_enabled(fleet):
        return {}
    secret = values.get(render.TURN_SECRET_CREDENTIAL, "")
    if len(secret) < 32 or not re.fullmatch(r"[A-Za-z0-9_-]+", secret):
        raise ToolError("%s is missing from secrets.env or is not a 32+ "
                        "character URL-safe value (run init-secrets --execute)"
                        % render.TURN_SECRET_CREDENTIAL)
    others = {k: v for k, v in values.items()
              if k != render.TURN_SECRET_CREDENTIAL}
    if secret in others.values():
        raise ToolError("%s must not reuse another secret's value"
                        % render.TURN_SECRET_CREDENTIAL)
    return {render.TURN_SECRET_CREDENTIAL: secret.encode()}


TURN_PACKAGE_SCRIPT = """
set -eu
export DEBIAN_FRONTEND=noninteractive
systemctl mask coturn.service >/dev/null
status=$(dpkg-query -W -f='${Status}' coturn 2>/dev/null || true)
if [ "$status" != "install ok installed" ]; then
  apt-get -o DPkg::Lock::Timeout=900 update -qq
  apt-get -o DPkg::Lock::Timeout=900 install -y -qq --no-install-recommends coturn
fi
systemctl stop coturn.service >/dev/null 2>&1 || true
id %s >/dev/null
test -x %s
"""


def install_turn(fleet, name, ip, ssh, runner):
    import render
    if not render.turn_enabled(fleet):
        return runner.step("%s: [turn] is off in the fleet file; nothing to "
                           "install" % name, lambda: None)

    def action():
        values = common.read_env_file(secrets_path())
        payloads = turn_payloads(fleet, values)
        ssh.run(ip, TURN_PACKAGE_SCRIPT % (shq(render.TURN_USER),
                                           shq(render.TURN_BIN)), timeout=900)
        for credential_name, payload in sorted(payloads.items()):
            ssh.run(ip, ENCRYPT_SCRIPT % shq(credential_name),
                    input_bytes=payload)
    return runner.step("%s: coturn from Debian (its own coturn.service masked; "
                       "%s runs it) and systemd-creds encrypt %s (value on "
                       "stdin only)" % (name, render.TURN_UNIT,
                                        render.TURN_SECRET_CREDENTIAL), action)


def calls_payloads(fleet, values):
    import render
    if not render.calls_enabled(fleet):
        return {}
    name = render.CALLS_SECRET_CREDENTIAL
    secret = values.get(name, "")
    if len(secret) < 32 or not re.fullmatch(r"[A-Za-z0-9_-]+", secret):
        raise ToolError("%s is missing from secrets.env or is not a 32+ "
                        "character URL-safe value (run init-secrets --execute)"
                        % name)
    if secret in {v for k, v in values.items() if k != name}:
        raise ToolError("%s must not reuse another secret's value" % name)
    return {name: secret.encode()}


CALLS_STAGE = "/tmp/fmnext-calls"
CALLS_STAGE_SCRIPT = ("install -d -o root -g root -m 0700 %s && rm -f %s/forkmesh-calls"
                      % (CALLS_STAGE, CALLS_STAGE))
CALLS_INSTALL_SCRIPT = """
set -eu
umask 077
user=%(user)s
bin=%(bin)s
unit=%(unit)s
name=%(name)s
creds=%(creds)s
stage=%(stage)s
running="/run/credentials/$unit/$name"
id "$user" >/dev/null 2>&1 || useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin "$user"
secret=$(cat)
restart=0
test -s "$stage/forkmesh-calls"
if ! cmp -s "$stage/forkmesh-calls" "$bin"; then
  install -m 0755 "$stage/forkmesh-calls" "$bin"
  restart=1
fi
rm -f "$stage/forkmesh-calls"
rmdir "$stage" 2>/dev/null || true
old=$(systemd-creds decrypt --name="$name" "$creds/$name.cred" - 2>/dev/null || true)
if [ "$old" != "$secret" ]; then
  install -d -m 0700 "$creds"
  printf '%%s' "$secret" | systemd-creds encrypt --with-key=host --name="$name" - "$creds/$name.cred.new"
  mv "$creds/$name.cred.new" "$creds/$name.cred"
fi
if systemctl is-active -q "$unit"; then
  if [ -r "$running" ] && [ "$(cat "$running")" != "$secret" ]; then restart=1; fi
  if [ "$restart" = 1 ]; then systemctl restart "$unit"; fi
fi
"""


def calls_binary():
    import next_deploy
    return next_deploy.dist_dir() / "forkmesh-calls"


def build_calls(binary):
    binary.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["go", "build", "-trimpath", "-o", str(binary),
                    "./cmd/forkmesh-calls"], cwd=common.REPO / "server",
                   check=True, env=dict(common.os.environ, CGO_ENABLED="0",
                                        GOOS="linux", GOARCH="amd64"))


def install_calls(fleet, name, ip, ssh, runner):
    import render
    if not render.calls_enabled(fleet):
        return runner.step("%s: [calls] is off in the fleet file; nothing to "
                           "install" % name, lambda: None)
    if not name.startswith("n"):
        return runner.step("%s: app replicas run no group-call server" % name,
                           lambda: None)
    binary = calls_binary()

    def action():
        payloads = calls_payloads(fleet, common.read_env_file(secrets_path()))
        build_calls(binary)
        ssh.run(ip, CALLS_STAGE_SCRIPT)
        ssh.put(ip, binary, CALLS_STAGE + "/forkmesh-calls")
        ssh.run(ip, CALLS_INSTALL_SCRIPT % {
            "user": shq(render.CALLS_USER), "bin": shq(render.CALLS_BIN),
            "unit": shq(render.CALLS_UNIT),
            "name": shq(render.CALLS_SECRET_CREDENTIAL),
            "creds": shq(render.fleet_paths(fleet)["creds"]),
            "stage": shq(CALLS_STAGE)},
            input_bytes=payloads[render.CALLS_SECRET_CREDENTIAL])
    return runner.step("%s: build server/cmd/forkmesh-calls, install it as %s, "
                       "systemd-creds encrypt %s (value on stdin only); restart "
                       "%s only when the binary or its secret changed" % (
                           name, render.CALLS_BIN,
                           render.CALLS_SECRET_CREDENTIAL, render.CALLS_UNIT),
                       action)


def push_secrets(fleet, name, ssh, runner):
    ip = node_ip(fleet, name)
    if not is_core_like(name):
        import render
        if not render.ids_enabled(fleet):
            runner.say("%s is a runner: it receives no core secrets" % name)
            return []
        names = sorted(render.ids_credential_names(fleet, "runner"))

        def push_runner():
            values = common.read_env_file(secrets_path())
            for credential_name, payload in sorted(
                    ids_payloads(fleet, name, values).items()):
                ssh.run(ip, ENCRYPT_SCRIPT % shq(credential_name),
                        input_bytes=payload)
        runner.step("%s: runner: only its IDS credentials %s (no core "
                    "secret; values on stdin only)" % (name, ", ".join(names)),
                    push_runner)
        return names
    import render
    names = sorted(set(render.app_credential_names(fleet)) | {
        "rqlite-auth.json", "FORKMESH_MESH_SECRET", CERTSYNC_SECRET} | (
        set(render.ids_credential_names(fleet, "core")) - {"RQLITE_PASSWORD"}
        if render.ids_enabled(fleet) else set()) |
        node_local_credential_names(fleet, name))
    stale = stale_credential_names(fleet, name)

    def push():
        values = common.read_env_file(secrets_path())
        payloads = credential_payloads(fleet, name, values)
        for credential_name, payload in sorted(payloads.items()):
            ssh.run(ip, ENCRYPT_SCRIPT % shq(credential_name), input_bytes=payload)
        user = (fleet.get("rqlite") or {}).get("user", "fmnext")
        curlrc = 'user = "%s:%s"\n' % (user, values["RQLITE_PASSWORD"])
        ssh.run(ip, CURLRC_SCRIPT, input_bytes=curlrc.encode())
    runner.step("%s: systemd-creds encrypt %s (values sent on stdin only)" % (
        name, ", ".join(names)), push)
    if stale:
        # Individual files by exact name, never a recursive delete; refused
        # on the node unless its unit already runs in vault mode.
        runner.step("%s: vault on: remove the credential files %s from "
                    "/etc/forkmesh-next/creds (now in the vault)" % (
                        name, ", ".join("%s.cred" % n for n in stale)),
                    lambda: ssh.run(ip, REMOVE_CREDS_SCRIPT % " ".join(stale)))
    return names


# --------------------------------------------------------------------------
# rqlite quorum


def parse_nodes(payload):
    nodes = payload.get("nodes", payload) if isinstance(payload, dict) else payload
    if isinstance(nodes, dict):
        nodes = [dict(v, id=v.get("id", k)) for k, v in nodes.items()]
    return list(nodes or [])


def cluster_nodes(fleet, ssh, exclude=()):
    """/nodes as seen from the first reachable core not in ``exclude``."""
    for node in fleet.get("nodes") or []:
        if node["name"] in exclude:
            continue
        try:
            out = ssh.run(node["public_ipv4"], common.rqlite_curl("/nodes?nonvoters&timeout=2s"),
                          timeout=30).stdout
            return parse_nodes(json.loads(out))
        except (ToolError, ValueError, subprocess.TimeoutExpired):
            continue
    raise ToolError("no core node answered /nodes")


def healthy_voters(nodes, exclude_ids=()):
    return sorted(n.get("id") for n in nodes
                  if n.get("voter", True) and n.get("reachable")
                  and n.get("id") not in exclude_ids)


def quorum_guard(fleet, ssh, target_name, force, runner):
    import render
    target = render.node_by_name(fleet, target_name)
    try:
        nodes = cluster_nodes(fleet, ssh, exclude=(target_name,))
        remaining = healthy_voters(nodes, (target["rqlite_id"],))
    except ToolError as error:
        if force:
            runner.say("WARNING: cannot verify rqlite quorum (%s); --force given" % error)
            return
        raise GuardError("cannot verify rqlite quorum (%s); refusing (use --force "
                         "only if the cluster is already gone)" % error)
    if len(remaining) < 3 and not force:
        raise GuardError("destroying %s would leave %d healthy rqlite voter(s) (%s); "
                         "need 3. Join a replacement first (replace-core) or pass "
                         "--force." % (target_name, len(remaining), ", ".join(remaining)))
    runner.say("rqlite voters remaining after %s: %s" % (target_name, ", ".join(remaining)))


def destroy(fleet, target, vultr, ssh, runner, yes=False, force=False):
    kind, number, label, name = parse_target(target)
    if not yes:
        raise GuardError("destroy needs --yes")
    vultr = next_cloud.for_label(label, vultr)
    instance = vultr.find_by_label(label)
    if instance is None:
        raise ToolError("no instance labelled %s tagged %s" % (label, TAG))
    guard_instance(instance)
    if hasattr(vultr, "guard"):
        vultr.guard(instance)
    if kind == "core":
        quorum_guard(fleet, ssh, name, force, runner)
    runner.step("DESTROY %s instance %s (%s, %s)" % (
        next_cloud.display(next_cloud.provider_name(vultr)),
        label, instance["id"], instance.get("main_ip")),
        lambda: vultr.delete_instance(instance["id"]))


# --------------------------------------------------------------------------
# replace-core


def replace_core(fleet, old, new, vultr, ssh, runner, catchup_timeout=900,
                 sleep=time.sleep):
    import render
    old_node = render.node_by_name(fleet, old)
    new_node = render.node_by_name(fleet, new)
    if old == new:
        raise ToolError("--new must differ from the node being replaced")
    kind, number, _, _ = parse_target(new)
    runner.say("replace %s with %s: 1) create/adopt fmnext-core-%d  2) install it "
               "as a joining voter  3) wait for catch-up  4) rqlite remove %s  "
               "5) destroy %s" % (old, new, number, old_node["rqlite_id"], old))
    create("core", number, vultr, runner)
    install(fleet, new, ssh, runner, INSTALL_STEPS, vultr=vultr, replacement=True)

    def caught_up():
        try:
            nodes = cluster_nodes(fleet, ssh, exclude=(old,))
        except ToolError:
            return False
        voters = healthy_voters(nodes)
        return new_node["rqlite_id"] in voters and len(voters) >= 4
    if runner.step("%s: wait until %s is a reachable voter (4 healthy voters)"
                   % (new, new_node["rqlite_id"]), lambda: True):
        if not common.wait_for(caught_up, catchup_timeout, sleep=sleep):
            raise ToolError("%s did not catch up in %ds; %s left in place" % (
                new, catchup_timeout, old))

    def remove():
        leader_ip = new_node["public_ipv4"]
        body = json.dumps({"id": old_node["rqlite_id"]})
        ssh.run(leader_ip, "cat >/tmp/fmnext-remove.json\n" + common.rqlite_curl(
            "/remove", "DELETE", "/tmp/fmnext-remove.json") + "\nrm -f /tmp/fmnext-remove.json",
            input_bytes=body.encode())
    runner.step("rqlite remove %s (via %s)" % (old_node["rqlite_id"], new), remove)
    destroy(fleet, old, vultr, ssh, runner, yes=True, force=False)
    runner.say("next: update deploy/fleet.toml, re-render the peers "
               "(install <peer> --steps files,start) and run next_dns.py ensure")


# --------------------------------------------------------------------------
# delete-ref

DELETE_REF_RE = re.compile(r"^refs/heads/[A-Za-z0-9][A-Za-z0-9._/-]{0,150}$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
PROTECTED_BRANCHES = ("refs/heads/main", "refs/heads/master",
                      "refs/heads/forkmesh/pulls")
DELETE_REFS_PASSES = 4


def served_git_dir_lines(owner, repo):
    return [
        "git_dir=$(python3 - <<'PY'",
        "import json",
        "cfg = json.load(open('/etc/forkmesh/mirror-node.json'))",
        "doc = json.load(open(cfg['gatewayConfig']))",
        "dirs = [e['gitDir'] for e in doc.get('repositories') or []",
        "        if (e.get('owner'), e.get('name')) == (%r, %r)]" % (owner, repo),
        "print(dirs[0] if dirs else '')",
        "PY",
        ")",
        "test -n \"$git_dir\" || { echo 'repository not served here' >&2; exit 1; }",
    ]


def delete_ref_trash(ref, expect):
    branch = ref[len("refs/heads/"):]
    kind = "agent-branch" if branch.startswith("agent/") and len(branch) > 6 \
        else "branch"
    token = hashlib.sha256(("%s %s" % (ref, expect)).encode()).hexdigest()[:16]
    return "refs/forkmesh/trash/%s/%s/%s" % (kind, branch, token)


def delete_ref_script(owner, repo, ref, expect):
    return "\n".join(served_git_dir_lines(owner, repo) + [
        "g() { runuser -u forkmesh-node -- git --git-dir=\"$git_dir\" \"$@\"; }",
        "current=$(g rev-parse --verify --quiet %s || true)" % shq(ref),
        "if [ -z \"$current\" ]; then echo 'already absent'; exit 0; fi",
        "if [ \"$current\" != %s ]; then echo \"ref is at $current\" >&2; exit 1; fi"
        % shq(expect),
        "g merge-base --is-ancestor %s refs/heads/main" % shq(expect),
        "g update-ref %s %s" % (shq(delete_ref_trash(ref, expect)), shq(expect)),
        "g update-ref -d %s %s" % (shq(ref), shq(expect)),
        "curl -fsS -X POST 'http://127.0.0.1:8791/v1/control/sync?local=1' >/dev/null || true",
        "echo deleted",
    ])


def delete_ref_repository(repository):
    owner, _, repo = repository.partition("/")
    if (not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", owner)
            or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", repo)):
        raise ToolError("--repo must be <owner>/<name>")
    return owner, repo


def check_delete_ref(ref, expect):
    if (not DELETE_REF_RE.fullmatch(ref) or ref in PROTECTED_BRANCHES
            or ".." in ref or ref.endswith((".lock", "/", "."))):
        raise GuardError("only a non-protected branch may be deleted: %r" % ref)
    if not OID_RE.fullmatch(expect):
        raise ToolError("--expect must be the branch's object id")


def delete_ref(fleet, name, ssh, runner, repository, ref, expect):
    import next_ids
    owner, repo = delete_ref_repository(repository)
    check_delete_ref(ref, expect)
    if not name.startswith("n"):
        raise ToolError("delete-ref is for core nodes only")
    ip = node_ip(fleet, name)
    next_ids.push_expectation(fleet, ssh, runner, name, allow=("processes",),
                              reason="next_fleet delete-ref", minutes=5)
    runner.step("%s: delete %s from %s (only at %s, contained in main)" % (
        name, ref, repository, expect[:12]),
        lambda: runner.say(ssh.run(ip, delete_ref_script(
            owner, repo, ref, expect)).stdout.decode().strip()))


def delete_refs_script(owner, repo):
    return "\n".join(served_git_dir_lines(owner, repo) + [
        "g() { runuser -u forkmesh-node -- git --git-dir=\"$git_dir\" \"$@\" </dev/null; }",
        "pairs=$(mktemp)",
        "trap 'rm -f \"$pairs\"' EXIT",
        "cat >\"$pairs\"",
        "deleted=0; absent=0; kept=0",
        "while read -r ref expect; do",
        "  [ -n \"$ref\" ] || continue",
        "  current=$(g rev-parse --verify --quiet \"$ref\" || true)",
        "  if [ -z \"$current\" ]; then absent=$((absent + 1)); continue; fi",
        "  if [ \"$current\" != \"$expect\" ]; then",
        "    echo \"kept $ref: it is at $current\"; kept=$((kept + 1)); continue",
        "  fi",
        "  if ! g merge-base --is-ancestor \"$expect\" refs/heads/main; then",
        "    echo \"kept $ref: not contained in main\"; kept=$((kept + 1)); continue",
        "  fi",
        "  branch=${ref#refs/heads/}",
        "  case \"$branch\" in agent/?*) kind=agent-branch ;; *) kind=branch ;; esac",
        "  token=$(printf '%s %s' \"$ref\" \"$expect\" | sha256sum | cut -c1-16)",
        "  if ! g update-ref \"refs/forkmesh/trash/$kind/$branch/$token\" \"$expect\"; then",
        "    echo \"kept $ref: could not keep its tip in the trash\"; kept=$((kept + 1)); continue",
        "  fi",
        "  if g update-ref -d \"$ref\" \"$expect\"; then",
        "    deleted=$((deleted + 1))",
        "  else",
        "    echo \"kept $ref: update-ref failed\"; kept=$((kept + 1))",
        "  fi",
        "done <\"$pairs\"",
        "curl -fsS -X POST 'http://127.0.0.1:8791/v1/control/sync?local=1' >/dev/null || true",
        "echo \"deleted $deleted, already absent $absent, kept $kept\"",
        "[ \"$kept\" -eq 0 ]",
    ])


def parse_ref_list(text):
    pairs = []
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) != 2:
            raise ToolError("line %d: expected '<branch> <object id>'" % number)
        ref, expect = fields
        if not ref.startswith("refs/"):
            ref = "refs/heads/" + ref
        check_delete_ref(ref, expect)
        pairs.append((ref, expect))
    if not pairs:
        raise ToolError("no branches to delete")
    return pairs


def delete_refs(fleet, names, ssh, runner, repository, pairs):
    import next_ids
    owner, repo = delete_ref_repository(repository)
    for ref, expect in pairs:
        check_delete_ref(ref, expect)
    cores = [node["name"] for node in common.core_nodes(fleet)]
    names = list(names) or cores
    outside = [name for name in names if name not in cores]
    if outside:
        raise GuardError("delete-refs is for core nodes only: %s" % ", ".join(outside))
    if runner.execute and sorted(names) != sorted(cores):
        raise GuardError("delete-refs must run on every core at once (%s): a core "
                         "that keeps a branch hands it back to the others"
                         % ", ".join(cores))
    for name in names:
        next_ids.push_expectation(fleet, ssh, runner, name, allow=("processes",),
                                  reason="next_fleet delete-refs", minutes=5)
    payload = "".join("%s %s\n" % pair for pair in pairs).encode()
    script = delete_refs_script(owner, repo)

    def on_node(name):
        completed = ssh.run(node_ip(fleet, name), script, input_bytes=payload,
                            check=False)
        out = (completed.stdout or b"").decode("utf-8", "replace").strip()
        runner.say("\n".join("%s: %s" % (name, line) for line in out.splitlines()))
        if completed.returncode != 0:
            tail = (completed.stderr or b"")[-500:].decode("utf-8", "replace").strip()
            raise ToolError("%s: delete-refs left branches in place%s" % (
                name, ": " + tail if tail else ""))
        match = re.search(r"^deleted (\d+),", out, re.M)
        return int(match.group(1)) if match else 0

    for attempt in range(1, DELETE_REFS_PASSES + 1):
        deleted = common.parallel_steps(runner, [
            ("%s: pass %d: delete %d branch(es) from %s (each only at its "
             "expected commit and contained in main)" % (
                 name, attempt, len(pairs), repository),
             lambda name=name: on_node(name))
            for name in names])
        if not runner.execute or not any(deleted):
            return
    raise ToolError("branches kept coming back after %d passes; a core or its "
                    "upstream still serves them" % DELETE_REFS_PASSES)


# --------------------------------------------------------------------------
# enable-ipv6


IPV6_CHECK_SCRIPT = (
    "ip -6 -o addr show scope global | awk '{print $4}' | cut -d/ -f1")


def node_ipv6_addresses(ssh, ip):
    completed = ssh.run(ip, IPV6_CHECK_SCRIPT, check=False, timeout=60)
    found = set()
    for line in (completed.stdout or b"").decode("utf-8", "replace").split():
        try:
            found.add(str(ipaddress.IPv6Address(line.strip())))
        except ValueError:
            continue
    return found


def node_ready(ssh, ip):
    completed = ssh.run(ip, "curl -fsS --max-time 5 -o /dev/null "
                        "http://127.0.0.1:8080/readyz", check=False, timeout=60)
    return completed.returncode == 0


def enable_ipv6(fleet, name, vultr, ssh, runner, reboot=False, timeout=600,
                sleep=time.sleep):
    if next_cloud.recorded_provider(parse_target(name)[2]) != "vultr":
        raise ToolError("enable-ipv6 is Vultr-only")
    ip = node_ip(fleet, name)
    instance = verify_target_instance(vultr, name, ip)
    address = instance_ipv6(instance)
    if address:
        record_instance(dict(instance, label=instance["label"]))
        runner.say("%s: Vultr IPv6 already on (%s, recorded in "
                   "fleet-instances.json)" % (name, address))
        if address in node_ipv6_addresses(ssh, ip):
            runner.say("%s: %s is configured on the node" % (name, address))
            return address
    if not reboot:
        runner.say("%s: %s reboots the node (Vultr applies IPv6 at boot). "
                   "Drain it from DNS first (next_dns.py drain %s --execute), "
                   "then rerun with --reboot --execute" % (
                       name, "the reboot" if address else
                       "enabling IPv6 on Vultr", name))
        return address or None
    import next_ids
    next_ids.push_expectation(fleet, ssh, runner, name,
                              allow=("reboot", "listeners", "processes",
                                     "modules"),
                              reason="next_fleet enable-ipv6 reboot",
                              minutes=20)
    if address:
        runner.step("%s: reboot through the Vultr API" % name,
                    lambda: vultr.reboot(instance["id"]))
    else:
        runner.step("%s: enable IPv6 on Vultr instance %s (Vultr reboots it)"
                    % (name, instance["id"]),
                    lambda: vultr.enable_ipv6(instance["id"]))
    if not runner.execute:
        return address or None

    def assigned():
        return instance_ipv6(vultr.instance(instance["id"]))
    address = common.wait_for(assigned, 120, interval=5, sleep=sleep)
    if not address:
        raise ToolError("%s: Vultr assigned no IPv6 address" % name)
    record_instance(dict(vultr.instance(instance["id"]), label=instance["label"]))
    runner.say("%s: Vultr IPv6 %s (recorded in fleet-instances.json)" % (
        name, address))
    sleep(30)

    def back():
        try:
            return address in node_ipv6_addresses(ssh, ip) and node_ready(ssh, ip)
        except (ToolError, OSError, subprocess.SubprocessError):
            return False
    if not common.wait_for(back, timeout, interval=10, sleep=sleep):
        raise ToolError("%s: not back with %s and /readyz within %ds" % (
            name, address, timeout))
    runner.say("%s: back, %s configured, /readyz ok" % (name, address))
    return address


# --------------------------------------------------------------------------
# CLI


def add_provider_flags(parser, create=False):
    parser.add_argument("--provider", default=next_cloud.DEFAULT_PROVIDER,
                        choices=next_cloud.PROVIDERS)
    parser.add_argument("--region", help="AWS region (default: AWS_REGION in aws.env)"
                        if not create else "Vultr region or AWS region")
    if create:
        parser.add_argument("--ssh-from", action="append", metavar="CIDR",
                            help="AWS: allow SSH only from this IPv4 CIDR "
                                 "(repeatable; default 0.0.0.0/0 like nftables)")
        parser.add_argument("--termination-protection", action="store_true",
                            help="AWS: turn on API termination protection")


def provider_client(provider, args, given, runner):
    if given is not None and next_cloud.provider_name(given) == provider:
        return given
    if provider != "aws":
        return next_cloud.from_config(provider)
    import next_aws
    try:
        return next_aws.Aws.from_config(region=args.region)
    except ToolError as error:
        if getattr(args, "execute", False) or args.command not in (
                "create", "create-core", "create-runner", "plans"):
            raise
        runner.say("note: %s; planning offline (no AWS calls)" % error)
        return next_aws.Aws.offline_plan(args.region)


def main(argv=None, vultr=None, ssh=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet", help="deploy/fleet.toml (default)")
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("plans", "list"):
        p = sub.add_parser(command)
        add_provider_flags(p)
    for kind in ("create-core", "create-runner"):
        p = sub.add_parser(kind)
        p.add_argument("number", type=int)
        p.add_argument("--plan")
        p.add_argument("--os-id", type=int)
        add_provider_flags(p, create=True)
        common.add_execute_flags(p)
    p = sub.add_parser("create", help="create (or adopt) one labelled instance")
    p.add_argument("target", help="nN, rN, aN, bN or an fmnext-<kind>-N label")
    p.add_argument("--plan", help="Vultr plan or AWS instance type "
                   "(default: the cheapest with >= 1 GiB RAM)")
    p.add_argument("--os-id", type=int)
    add_provider_flags(p, create=True)
    common.add_execute_flags(p)
    p = sub.add_parser("fleet-toml")
    p.add_argument("--out", default=str(common.REPO / "deploy" / "fleet.toml"))
    common.add_execute_flags(p)
    p = sub.add_parser("logread-key")
    common.add_execute_flags(p)
    p = sub.add_parser("init-secrets")
    common.add_execute_flags(p)
    p = sub.add_parser("install")
    p.add_argument("target")
    p.add_argument("--steps")
    p.add_argument("--replacement", action="store_true")
    common.add_execute_flags(p)
    p = sub.add_parser("roll")
    p.add_argument("targets", nargs="*")
    p.add_argument("--steps")
    p.add_argument("--mail", action="store_true",
                   help="also reinstall core mail (next_mail.py install)")
    common.add_execute_flags(p)
    p = sub.add_parser("roll-runners")
    p.add_argument("targets", nargs="*")
    p.add_argument("--idle-wait", type=int, default=RUNNER_IDLE_WAIT_SECONDS)
    common.add_execute_flags(p)
    p = sub.add_parser("secrets")
    p.add_argument("target")
    common.add_execute_flags(p)
    p = sub.add_parser("destroy")
    p.add_argument("target")
    p.add_argument("--yes", action="store_true")
    p.add_argument("--force", action="store_true",
                   help="skip the rqlite quorum guard (never the label/tag guard)")
    common.add_execute_flags(p)
    p = sub.add_parser("replace-core")
    p.add_argument("target")
    p.add_argument("--new", required=True)
    common.add_execute_flags(p)
    p = sub.add_parser("add-core", help="create a new core and join it to the "
                       "fleet end to end (resumable)")
    p.add_argument("--provider", choices=next_cloud.PROVIDERS,
                   help="cloud for the new core (default vultr)")
    p.add_argument("--region", help="Vultr or AWS region (default: the existing cores')")
    p.add_argument("--plan", help="Vultr plan (default: the existing cores')")
    p.add_argument("--snapshot", help="boot from this keyless Vultr snapshot id")
    p.add_argument("--name", help="nN to add or resume (default: the next free "
                   "number, or the unfinished run)")
    p.add_argument("--replace", metavar="nOLD", help="once the new core serves, "
                   "take this core out of the fleet in the same run (its instance "
                   "is kept) and commit both changes together")
    p.add_argument("--no-events", dest="events", action="store_false",
                   help="no FMNEXT-ADD-CORE progress lines")
    common.add_execute_flags(p)
    p = sub.add_parser("add-core-options", help="defaults, plans and estimates "
                       "for add-core (read-only)")
    p.add_argument("--provider", choices=next_cloud.PROVIDERS,
                   default=next_cloud.DEFAULT_PROVIDER)
    p.add_argument("--replace", metavar="nOLD")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("add-core-status", help="the last add-core run (read-only)")
    p.add_argument("--name")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("remove-core", help="take a core out of the fleet end to "
                       "end (resumable); the cloud instance only with "
                       "--destroy-instance")
    p.add_argument("target")
    p.add_argument("--destroy-instance", action="store_true",
                   help="also delete the cloud instance (irreversible; the only "
                        "step that needs the provider API key)")
    p.add_argument("--no-events", dest="events", action="store_false",
                   help="no FMNEXT-REMOVE-CORE progress lines")
    common.add_execute_flags(p)
    p = sub.add_parser("remove-core-plan", help="what remove-core would do, and "
                       "whether quorum allows it (read-only)")
    p.add_argument("target")
    p.add_argument("--destroy-instance", action="store_true")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("remove-core-status", help="the last remove-core run "
                       "(read-only)")
    p.add_argument("--name")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("delete-ref")
    p.add_argument("target")
    p.add_argument("--repo", required=True)
    p.add_argument("--ref", required=True)
    p.add_argument("--expect", required=True)
    common.add_execute_flags(p)
    p = sub.add_parser("delete-refs")
    p.add_argument("targets", nargs="*")
    p.add_argument("--repo", required=True)
    p.add_argument("--from", dest="from_file", required=True,
                   help="file of '<branch> <object id>' lines, or - for stdin")
    common.add_execute_flags(p)
    p = sub.add_parser("enable-ipv6")
    p.add_argument("target")
    p.add_argument("--reboot", action="store_true",
                   help="allow the reboot (enabling IPv6 on Vultr reboots "
                        "the instance)")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)

    try:
        if args.command in ("add-core", "add-core-options", "add-core-status"):
            import next_add_core
            if args.command == "add-core":
                return next_add_core.main_add_core(args, vultr=vultr, ssh=ssh)
            if args.command == "add-core-options":
                return next_add_core.main_options(args, vultr=vultr, ssh=ssh)
            return next_add_core.main_status(args)
        if args.command in ("remove-core", "remove-core-plan", "remove-core-status"):
            import next_remove_core
            if args.command == "remove-core":
                return next_remove_core.main_remove_core(args, ssh=ssh)
            if args.command == "remove-core-plan":
                return next_remove_core.main_plan(args, ssh=ssh)
            return next_remove_core.main_status(args)
        runner = common.runner_from_args(args)
        if args.command == "logread-key":
            logread_key(runner)
            return 0
        if args.command == "fleet-toml":
            instances = common.load_json(instances_path(), None)
            if not instances:
                raise ToolError("no %s" % instances_path())
            for label in instances:
                if not LABEL_RE.match(label):
                    raise GuardError("refusing %r in fleet-instances.json" % label)
            out = Path(args.out)
            text = fleet_toml_text(instances, common.load_json(
                common.config_dir() / "fleet-state.json", {}),
                current=out.read_text("utf-8") if out.is_file() else None)
            if runner.execute:
                Path(args.out).write_text(text, "utf-8")
                runner.say("wrote %s" % args.out)
            else:
                runner.say("[dry-run] would write %s:\n%s" % (args.out, text))
            return 0
        provider = next_cloud.check_name(getattr(args, "provider", None))
        if provider != "vultr" and args.command in ("plans", "list", "create-core",
                                                    "create-runner", "create"):
            vultr = provider_client(provider, args, vultr, runner)
        elif args.command in ("plans", "list", "create-core", "create-runner", "create",
                              "destroy", "replace-core", "enable-ipv6") or (
                args.command in ("install", "secrets", "roll") and args.execute):
            vultr = vultr or Vultr.from_config()
        ssh = ssh or common.Ssh()
        if args.command == "plans" and provider == "aws":
            import next_aws
            best = next_aws.cheapest_type()
            print("%s  %s MB RAM  about $%s/month  (%s; on-demand estimate)" % (
                best, next_aws.INSTANCE_TYPES[best]["ram"], next_aws.plan_monthly(best),
                vultr.region))
            return 0
        if args.command == "plans":
            best = cheapest_plan(vultr.plans())
            if not best:
                raise ToolError("no plan with >= 1 GiB RAM in %s" % REGION)
            print("%s  %s MB RAM  $%s/month  (%s)" % (
                best["id"], best.get("ram"), best.get("monthly_cost"), REGION))
            return 0
        if args.command == "list":
            for inst in sorted(vultr.fleet_instances(), key=lambda i: i["label"]):
                print("%-16s %s  %-15s %-8s %s" % (
                    inst["label"], inst["id"], inst.get("main_ip", ""),
                    inst.get("status", ""), inst.get("plan", "")))
            return 0
        if args.command in ("create-core", "create-runner", "create"):
            if args.command == "create":
                kind, number, _, _ = parse_target(args.target)
            else:
                kind = "core" if args.command == "create-core" else "run"
                number = args.number
            fleet = None
            if provider != "vultr":
                try:
                    fleet = common.load_fleet(args.fleet)
                except (ToolError, OSError, ValueError) as error:
                    runner.say("note: %s not loaded (%s); default ports for the "
                               "security group" % (args.fleet or "deploy/fleet.toml",
                                                   error))
            create(kind, number, vultr, runner, os_id=args.os_id, plan_id=args.plan,
                   region=args.region or REGION, fleet=fleet,
                   ssh_sources=tuple(args.ssh_from or ("0.0.0.0/0",)),
                   termination_protection=args.termination_protection)
            if not runner.execute:
                runner.say("(dry run: nothing was changed; pass --execute to apply)")
            return 0
        fleet = common.load_fleet(args.fleet)
        if args.command == "init-secrets":
            init_secrets(fleet, runner)
        elif args.command == "install":
            _, _, _, name = parse_target(args.target)
            default = {"n": INSTALL_STEPS, "a": APP_STEPS, "b": BURST_STEPS}.get(
                name[:1], RUNNER_STEPS)
            steps = tuple(s for s in (args.steps.split(",") if args.steps else default) if s)
            unknown = [s for s in steps if s not in ALL_STEPS]
            if unknown:
                raise ToolError("unknown step(s): %s (known: %s)" % (
                    ", ".join(unknown), ", ".join(ALL_STEPS)))
            core_only = [s for s in steps if s in CORE_ONLY_STEPS]
            if core_only and not is_core_like(name):
                raise ToolError("step(s) %s are for core nodes only" % (
                    ", ".join(core_only)))
            lock = common.DeployLock(what="next_fleet install %s" % name)
            if runner.execute and is_core_like(name):
                lock.acquire(wait=ROLL_LOCK_WAIT_SECONDS)
            try:
                install(fleet, name, ssh, runner, steps, vultr=vultr,
                        replacement=args.replacement)
            finally:
                if runner.execute and is_core_like(name):
                    lock.release()
        elif args.command == "roll":
            cores = [n["name"] for n in fleet.get("nodes") or []]
            names = [parse_target(t)[3] for t in args.targets] or cores
            outside = [n for n in names if n not in cores]
            if outside:
                raise GuardError("roll is for core nodes: %s" % ", ".join(outside))
            steps = tuple(s for s in (args.steps.split(",") if args.steps
                                      else ROLL_STEPS) if s)
            unknown = [s for s in steps if s not in ALL_STEPS]
            if unknown:
                raise ToolError("unknown step(s): %s" % ", ".join(unknown))
            lock = common.DeployLock(what="next_fleet roll %s" % " ".join(names))
            if runner.execute:
                lock.acquire(wait=ROLL_LOCK_WAIT_SECONDS)
            try:
                roll(fleet, names, ssh, runner, steps, mail=args.mail,
                     vultr=vultr, fleet_path=args.fleet)
            finally:
                if runner.execute:
                    lock.release()
        elif args.command == "roll-runners":
            known = runner_names(fleet)
            names = [parse_target(t)[3] for t in args.targets] or known
            outside = [n for n in names if n not in known]
            if outside:
                raise GuardError("roll-runners is for runners: %s" % ", ".join(outside))
            lock = common.DeployLock(what="next_fleet roll-runners %s" % " ".join(names))
            if runner.execute:
                lock.acquire(wait=ROLL_LOCK_WAIT_SECONDS)
            try:
                roll_runners(fleet, names, ssh, runner, vultr=vultr,
                             idle_wait=max(0, args.idle_wait))
            finally:
                if runner.execute:
                    lock.release()
        elif args.command == "secrets":
            _, _, _, name = parse_target(args.target)
            if runner.execute:
                verify_target(vultr, ssh, fleet, name, node_ip(fleet, name))
            push_secrets(fleet, name, ssh, runner)
        elif args.command == "destroy":
            destroy(fleet, args.target, vultr, ssh, runner, yes=args.yes, force=args.force)
        elif args.command == "replace-core":
            _, _, _, old = parse_target(args.target)
            _, _, _, new = parse_target(args.new)
            replace_core(fleet, old, new, vultr, ssh, runner)
        elif args.command == "delete-ref":
            _, _, _, name = parse_target(args.target)
            delete_ref(fleet, name, ssh, runner, args.repo, args.ref, args.expect)
        elif args.command == "delete-refs":
            text = (sys.stdin.read() if args.from_file == "-"
                    else Path(args.from_file).read_text())
            names = [parse_target(t)[3] for t in args.targets]
            delete_refs(fleet, names, ssh, runner, args.repo, parse_ref_list(text))
        elif args.command == "enable-ipv6":
            _, _, _, name = parse_target(args.target)
            enable_ipv6(fleet, name, vultr, ssh, runner, reboot=args.reboot)
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
    except RunnersBusy as error:
        print("next_fleet: %s" % error, file=sys.stderr)
        return RUNNERS_BUSY_EXIT
    except (ToolError, OSError, KeyError) as error:
        print("next_fleet: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
