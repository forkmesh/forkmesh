#!/usr/bin/env python3
"""Render every file a ForkMesh Next node needs, from deploy/fleet.toml.

    deploy/core/render.py --fleet deploy/fleet.toml --node n1 --out DIR
    deploy/core/render.py --fleet deploy/fleet.toml --runner r1 --out DIR
    deploy/core/render.py ... --node n4 --replacement   # joins, no bootstrap

The output directory mirrors the target filesystem (``DIR/etc/...``) and
holds ``MANIFEST.json``: every file with its mode, and the systemd
credentials each unit loads. **No secret is ever rendered**: secrets are
referenced by credential name only (``LoadCredentialEncrypted=NAME:...``)
and pushed separately by ``tools/next_fleet.py secrets``. The renderer never
reads secrets.env, so it cannot leak it. With ``[app] vault = true`` the app
unit loads only the bootstrap credentials and starts sealed; the other
secrets are sealed into the database by ``tools/next_vault.py`` (docs/VAULT.md).

WireGuard public keys come from the fleet file, or else from the operator
state file (``<config_dir>/fleet-state.json``, written by
``next_fleet.py install`` after it generates each node's key on the node).
"""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import ipaddress
import json
import os
import re
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATES = HERE / "templates"
RUNNER_TEMPLATES = HERE.parent / "runner" / "templates"
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import caddy_routes  # noqa: E402

PLACEHOLDER = re.compile(r"@@([A-Z0-9_]+)@@")
NODE_NAME = re.compile(r"^n[0-9]{1,2}$")
RUNNER_NAME = re.compile(r"^[rb][0-9]{1,2}$")
APP_NAME = re.compile(r"^a([0-9]{1,2})$")
BURST_NAME = re.compile(r"^b([0-9]{1,2})$")
APP_WG_OFFSET = 20
CORE_WG_PREFIX = "10.77.0."
APP_LABEL = "fmnext-auto-%d"
BURST_LABEL = "fmnext-burst-%d"
AUTOSCALED_MAX = 9
WG_KEY = re.compile(r"^[A-Za-z0-9+/]{42}[AEIMQUYcgkosw048]=$")
IDS_KEY = re.compile(r"^[A-Za-z0-9_-]{43}$")
SSH_FINGERPRINT = re.compile(r"^SHA256:[A-Za-z0-9+/]{43}$")
LOGREAD_PUBLIC_KEY = re.compile(r"^ssh-ed25519 (AAAAC3NzaC1lZDI1NTE5[A-Za-z0-9+/]{48}) fmnext-logread$")
LOGREAD_PROGRAM = "/opt/forkmesh-next/bin/fmnext-logread"
LOGREAD_AUTHORIZED_KEY = "/etc/forkmesh-next/logread.authorized_key"
ACCESS_LOG_QUERY_KEYS = ("token", "access_token", "refresh_token", "sig",
                         "signature", "key", "api_key", "code", "password",
                         "secret", "auth", "session")
CLOUDFLARE_RANGES = ("173.245.48.0/20", "103.21.244.0/22", "103.22.200.0/22",
                     "103.31.4.0/22", "141.101.64.0/18", "108.162.192.0/18",
                     "190.93.240.0/20", "188.114.96.0/20", "197.234.240.0/22",
                     "198.41.128.0/17", "162.158.0.0/15", "104.16.0.0/13",
                     "104.24.0.0/14", "172.64.0.0/13", "131.0.72.0/22",
                     "2400:cb00::/32", "2606:4700::/32", "2803:f800::/32",
                     "2405:b500::/32", "2405:8100::/32", "2a06:98c0::/29",
                     "2c0f:f248::/32")
ACCESS_LOG_CLOUDFLARE_FIELDS = (("cf_ip", "Cf-Connecting-Ip"),
                                ("cf_country", "Cf-Ipcountry"),
                                ("cf_region", "Cf-Region"),
                                ("cf_city", "Cf-Ipcity"))
ACCESS_LOG_RELAY_FIELDS = (("edge_ip", "{re.edge_client.1}"),
                           ("edge_country", "{re.edge_client.2}"),
                           ("edge_where",
                            "{http.request.header.X-Forkmesh-Edge-Where}"))
MANAGED = "# Managed by deploy/core/render.py for node %s. Do not edit on the node.\n"
DAEMON_OVERRIDES_PATH = "/etc/forkmesh-next/mirror-node.overrides.json"


class RenderError(ValueError):
    pass


# --------------------------------------------------------------------------
# fleet


def expand(path: str) -> Path:
    return Path(os.path.expanduser(path))


def operator_config_dir():
    configured = os.environ.get("FMNEXT_CONFIG_DIR")
    if configured:
        return Path(configured)
    tools = Path(__file__).resolve().parents[2] / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    try:
        import forkmesh_paths
    except ImportError:
        return Path.home() / "projects" / "forkmesh" / "config" / "fleet"
    return forkmesh_paths.resolve().fleet_config


def instances_file(path=None):
    if path:
        return Path(path)
    return operator_config_dir() / "fleet-instances.json"


def load_instances(path=None):
    target = instances_file(path)
    if not target.is_file():
        raise RenderError("no %s: the fleet's public addresses live only in "
                          "that operator file (docs/deploy/README.md)" % target)
    data = json.loads(target.read_text("utf-8"))
    if not isinstance(data, dict):
        raise RenderError("%s is not a JSON object keyed by label" % target)
    return data


def fill_public_addresses(items, instances=None, instances_path=None):
    missing = [item for item in items if not item.get("public_ipv4")]
    if not missing:
        return items
    if instances is None:
        instances = load_instances(instances_path)
    source = instances_path or instances_file()
    for item in missing:
        label = str(item.get("label") or "")
        info = instances.get(label) if label else None
        ip = str((info or {}).get("ip") or "")
        if not ip or ip == "0.0.0.0":
            raise RenderError("%s (%s): no public_ipv4 in the fleet file and no "
                              "\"ip\" for that label in %s" % (
                                  item.get("name"), label or "no label", source))
        item["public_ipv4"] = str(ipaddress.IPv4Address(ip))
        if info.get("ipv6") and not item.get("public_ipv6"):
            item["public_ipv6"] = str(info["ipv6"])
    return items


def load_fleet(path, state_path=None, state=None, instances=None,
               instances_path=None):
    with open(path, "rb") as handle:
        fleet = tomllib.load(handle)
    fill_public_addresses(list(fleet.get("nodes") or [])
                          + list(fleet.get("runners") or []),
                          instances, instances_path)
    domain = fleet.get("domain", "")
    if domain != "next.forkmesh.com":
        raise RenderError("domain must be next.forkmesh.com (got %r)" % domain)
    for key in ("app_host", "world_host"):
        if not str(fleet.get(key, "")).endswith("." + domain):
            raise RenderError("%s must be under %s" % (key, domain))
    if not str(fleet.get("instance_label_prefix", "")).startswith("fmnext-"):
        raise RenderError("instance_label_prefix must start with fmnext-")
    nodes = fleet.get("nodes") or []
    names = [n.get("name") for n in nodes]
    if len(set(names)) != len(names):
        raise RenderError("duplicate node names")
    wg_ips = []
    for node in nodes:
        if not NODE_NAME.match(str(node.get("name"))):
            raise RenderError("node name must look like n1: %r" % node.get("name"))
        if node.get("role", "core") != "core":
            raise RenderError("[[nodes]] are core nodes; runners go in [[runners]]")
        ipaddress.IPv4Address(node["public_ipv4"])
        check_public_ipv6(node)
        wg_ips.append(str(ipaddress.IPv4Address(node["wg_ip"])))
        node.setdefault("rqlite_id", node["name"])
        if not isinstance(node.get("rqlite_voter", True), bool):
            raise RenderError("rqlite_voter must be true or false: %r" % node["name"])
    if nodes and not any(node.get("rqlite_voter", True) for node in nodes):
        raise RenderError("at least one core must be an rqlite voter")
    for runner in fleet.get("runners") or []:
        if not RUNNER_NAME.match(str(runner.get("name"))):
            raise RenderError("runner name must look like r1: %r" % runner.get("name"))
        ipaddress.IPv4Address(runner["public_ipv4"])
        check_public_ipv6(runner)
        if runner.get("wg_ip"):
            wg_ips.append(str(ipaddress.IPv4Address(runner["wg_ip"])))
    if len(set(wg_ips)) != len(wg_ips):
        raise RenderError("duplicate WireGuard addresses")
    retired = (fleet.get("hosted") or {}).get("retired_cores") or []
    if not isinstance(retired, list) or not all(
            NODE_NAME.match(str(name)) for name in retired):
        raise RenderError("hosted.retired_cores must list names like n1: %r" % (retired,))
    if "acme_issuer" in fleet:
        raise RenderError("acme_issuer is gone: certificate issuance is "
                          "leaderless (deploy/core/certsync.py); remove it")
    check_production(fleet)
    dns = fleet.get("dns") or {}
    if dns.get("reconciler", "dry") not in ("on", "dry", "off"):
        raise RenderError("dns.reconciler must be on, dry or off")
    if state is None:
        state = {}
        if state_path is None:
            state_path = operator_config_dir() / "fleet-state.json"
        if state_path and Path(state_path).is_file():
            state = json.loads(Path(state_path).read_text("utf-8"))
    merge_autoscaled(fleet, state, wg_ips)
    for node in nodes + fleet["app_nodes"]:
        key = node.get("wg_pubkey", "")
        if not WG_KEY.match(key or ""):
            key = ((state.get("wg_pubkeys") or {}).get(node["name"]) or "")
        node["wg_pubkey"] = key
    for item in nodes + fleet["app_nodes"] + list(fleet.get("runners") or []):
        key = item.get("ids_pubkey", "")
        if not IDS_KEY.match(key or ""):
            key = ((state.get("ids_pubkeys") or {}).get(item["name"]) or "")
        item["ids_pubkey"] = key if IDS_KEY.match(key or "") else ""
    ids_settings(fleet)
    logs_settings(fleet)
    return fleet


def merge_autoscaled(fleet, state, wg_ips=()):
    fleet["app_nodes"] = []
    taken = set(wg_ips)
    runners = fleet.setdefault("runners", [])
    for name, item in sorted((state.get("autoscaled") or {}).items()):
        item = dict(item or {})
        kind = item.get("kind")
        app = APP_NAME.match(str(name))
        burst = BURST_NAME.match(str(name))
        if kind == "app" and app:
            number = int(app.group(1))
            if not 1 <= number <= AUTOSCALED_MAX:
                raise RenderError("autoscaled %s: number out of range" % name)
            wg_ip = CORE_WG_PREFIX + str(APP_WG_OFFSET + number)
            node = {"name": name, "role": "app", "autoscaled": True,
                    "label": APP_LABEL % number,
                    "instance_id": str(item.get("instance_id") or ""),
                    "public_ipv4": str(ipaddress.IPv4Address(
                        item["public_ipv4"])),
                    "wg_ip": wg_ip, "rqlite_id": name}
            if item.get("public_ipv6"):
                node["public_ipv6"] = item["public_ipv6"]
            check_public_ipv6(node)
            if wg_ip in taken:
                raise RenderError("duplicate WireGuard addresses")
            taken.add(wg_ip)
            fleet["app_nodes"].append(node)
        elif kind == "burst" and burst:
            number = int(burst.group(1))
            if not 1 <= number <= AUTOSCALED_MAX:
                raise RenderError("autoscaled %s: number out of range" % name)
            if any(r["name"] == name for r in runners):
                raise RenderError("duplicate runner %s" % name)
            runners.append({
                "name": name, "burst": True, "autoscaled": True,
                "label": BURST_LABEL % number,
                "instance_id": str(item.get("instance_id") or ""),
                "public_ipv4": str(ipaddress.IPv4Address(
                    item["public_ipv4"])),
                "node_name": "fmnext-%s" % name})
        else:
            raise RenderError("autoscaled entry %r is not an app node (aN) "
                              "or a burst runner (bN)" % name)
    return fleet


AUTOSCALE_CERTSYNC_CREDENTIAL = "AUTOSCALE_CERTSYNC_SECRET"
FLEET_PEERS_REPLICAS = "/run/fmnext-fleet-peers/replicas.json"


def autoscale_enabled(fleet):
    value = (fleet.get("autoscale") or {}).get("enabled", False)
    if not isinstance(value, bool):
        raise RenderError("autoscale.enabled must be true or false")
    return value


def is_app_node(node):
    return (node or {}).get("role") == "app"


def core_and_app_nodes(fleet):
    return list(fleet.get("nodes") or []) + list(fleet.get("app_nodes") or [])


def check_public_ipv6(item):
    value = item.get("public_ipv6")
    if value in (None, ""):
        item.pop("public_ipv6", None)
        return
    try:
        address = ipaddress.IPv6Address(str(value))
    except ValueError:
        raise RenderError("%s: public_ipv6 %r is not an IPv6 address"
                          % (item.get("name"), value)) from None
    if not address.is_global:
        raise RenderError("%s: public_ipv6 %s is not a global address"
                          % (item.get("name"), address))
    item["public_ipv6"] = str(address)


def node_by_name(fleet, name):
    for node in core_and_app_nodes(fleet):
        if node["name"] == name:
            return node
    raise RenderError("no core node named %r" % name)


def runner_by_name(fleet, name):
    for runner in fleet.get("runners") or []:
        if runner["name"] == name:
            return runner
    raise RenderError("no runner named %r" % name)


def fleet_paths(fleet):
    paths = {
        "opt": "/opt/forkmesh-next",
        "releases": "/opt/forkmesh-next/app/releases",
        "current": "/opt/forkmesh-next/app/current",
        "var": "/var/lib/forkmesh-next",
        "etc": "/etc/forkmesh-next",
        "creds": "/etc/forkmesh-next/creds",
        "tls": "/etc/forkmesh-next/tls",
    }
    paths.update(fleet.get("paths") or {})
    for key, value in paths.items():
        if not value.startswith("/") or ".." in value:
            raise RenderError("path %s must be absolute: %r" % (key, value))
    return paths


# --------------------------------------------------------------------------
# templates


def fill(text: str, values: dict) -> str:
    def replace(match):
        key = match.group(1)
        if key not in values:
            raise RenderError("template placeholder @@%s@@ has no value" % key)
        return str(values[key])
    return PLACEHOLDER.sub(replace, text)


def template(name, values, directory=TEMPLATES):
    return fill((directory / name).read_text("utf-8"), values)


def app_secret_names(fleet):
    app = fleet.get("app") or {}
    names = list(app.get("secrets") or [])
    for name in app.get("optional_secrets") or []:
        if name not in names:
            names.append(name)
    for name in names:
        if not re.fullmatch(r"[A-Z][A-Z0-9_]{1,63}", name):
            raise RenderError("secret name %r is not an env-style name" % name)
    return names


# Must match app/host/forkmesh_host/vault_crypto.py (asserted by a test):
# the credentials a vault-mode app still loads from systemd, because rqlite
# and the mesh need them before any key exists.
VAULT_BOOTSTRAP_NAMES = ("RQLITE_PASSWORD", "FORKMESH_MESH_SECRET")
VAULT_KEY_NAME = "DATA_KEY"
UNSEAL_SOCKET = "/run/forkmesh-next/unseal.sock"
SSH_HELPER_SOCKET = "/run/fmnext-ssh-helper.sock"


def vault_enabled(fleet):
    value = (fleet.get("app") or {}).get("vault", False)
    if not isinstance(value, bool):
        raise RenderError("app.vault must be true or false")
    return value


def vault_peer_unseal(fleet):
    """Sealed nodes fetch DATA_KEY from unsealed peers over the mesh
    (default on whenever the vault is on)."""
    value = (fleet.get("app") or {}).get("vault_peer_unseal", True)
    if not isinstance(value, bool):
        raise RenderError("app.vault_peer_unseal must be true or false")
    return vault_enabled(fleet) and value


def d1_log(fleet):
    value = (fleet.get("app") or {}).get("d1_log", True)
    if not isinstance(value, bool):
        raise RenderError("app.d1_log must be true or false")
    return value


def mesh_peers(fleet):
    port = int((fleet.get("app") or {}).get("mesh_port", 8792))
    return ",".join("%s=http://%s:%d" % (n["name"], n["wg_ip"], port)
                    for n in fleet["nodes"])


def app_credential_names(fleet):
    """Names the forkmesh-app unit loads with LoadCredentialEncrypted=.

    Vault off: every app secret. Vault on: only the bootstrap names; the
    rest live encrypted in the host_secrets table (docs/VAULT.md)."""
    names = app_secret_names(fleet)
    if not vault_enabled(fleet):
        return names
    return [n for n in names if n in VAULT_BOOTSTRAP_NAMES] + [
        n for n in VAULT_BOOTSTRAP_NAMES if n not in names]


def vault_secret_names(fleet):
    """App variables tools/next_vault.py seals into host_secrets."""
    return [n for n in app_secret_names(fleet)
            if n != VAULT_KEY_NAME and n not in VAULT_BOOTSTRAP_NAMES]


def shared_hosts(fleet):
    return [fleet["app_host"], fleet["world_host"]]



PRODUCTION_HOSTS = {"app_host": "app.forkmesh.com",
                    "world_host": "world.forkmesh.com"}
LEGACY_PASSTHROUGH = ("/readyz", "/health", "/api/version")
PRODUCTION_VAR_NAME = re.compile(r"^[A-Z][A-Z0-9_]{1,63}$")
SECRETISH = re.compile(r"(SECRET|TOKEN|PASS|SEED|PRIVATE|DSN|_KEY$|^DATA_KEY$)")
PUBLIC_KEY_VARS = frozenset({"TURNSTILE_SITE_KEY"})
EXTRA_PRODUCTION_VARS = frozenset({
    "AP_OUTBOUND",
})


def production(fleet):
    return fleet.get("production") or {}


def wrangler_var_names():
    try:
        with open(REPO / "app" / "wrangler.toml", "rb") as handle:
            return set((tomllib.load(handle).get("vars") or {}).keys())
    except OSError:
        return set()


def check_production(fleet):
    prod = fleet.get("production")
    if prod is None:
        return
    if not isinstance(prod, dict):
        raise RenderError("[production] must be a table")
    for key, expected in PRODUCTION_HOSTS.items():
        if prod.get(key) != expected:
            raise RenderError("production.%s must be %s (got %r)"
                              % (key, expected, prod.get(key)))
    unknown = set(prod) - set(PRODUCTION_HOSTS) - {"vars"}
    if unknown:
        raise RenderError("unknown [production] keys: %s"
                          % ", ".join(sorted(unknown)))
    known = wrangler_var_names()
    for name, value in (prod.get("vars") or {}).items():
        if not PRODUCTION_VAR_NAME.match(str(name)):
            raise RenderError("production.vars: bad name %r" % name)
        if SECRETISH.search(name) and name not in PUBLIC_KEY_VARS:
            raise RenderError("production.vars.%s looks like a secret; secrets "
                              "go through the vault (tools/next_vault.py)" % name)
        if known and name not in known and name not in EXTRA_PRODUCTION_VARS:
            raise RenderError("production.vars.%s is not an app/wrangler.toml "
                              "[vars] name" % name)
        if not isinstance(value, str) or any(c in value for c in "\n\r\0"):
            raise RenderError("production.vars.%s must be a one-line string" % name)


def served_app_host(fleet):
    return production(fleet).get("app_host") or fleet["app_host"]


def served_world_host(fleet):
    return production(fleet).get("world_host") or fleet["world_host"]


def served_hosts(fleet):
    return [served_app_host(fleet), served_world_host(fleet)]


def legacy_hosts(fleet):
    return shared_hosts(fleet) if production(fleet) else []


def tls_hosts(fleet):
    out = []
    for host in served_hosts(fleet) + legacy_hosts(fleet):
        if host not in out:
            out.append(host)
    return out


def production_settings(fleet):
    prod = production(fleet)
    if not prod:
        return ""
    lines = ["", "# Production identity (docs/CUTOVER.md): fleet.toml [production].",
             "FORKMESH_SERVES_PRODUCTION=1"]
    for name, value in sorted((prod.get("vars") or {}).items()):
        lines.append("FORKMESH_VAR_%s=%s" % (name, value))
    return "\n".join(lines)


def core_daemon_names(fleet):
    return ["fmnext-%s" % n["name"] for n in fleet.get("nodes") or []
            if not is_app_node(n)]


def retired_core_names(fleet):
    live = {n["name"] for n in fleet.get("nodes") or []}
    return ["fmnext-%s" % name
            for name in (fleet.get("hosted") or {}).get("retired_cores") or []
            if name not in live]


def core_role_settings(fleet):
    names = ",".join(core_daemon_names(fleet))
    enabled = (fleet.get("hosted") or {}).get("enabled", True)
    hosted = names if enabled else ""
    owners = ",".join(core_daemon_names(fleet) + retired_core_names(fleet)) \
        if enabled else ""
    settings = self_deploy_settings(fleet)
    admin_updates = "1" if settings["enabled"] and settings["adminUpdates"] else ""
    return "\n".join(["", "FORKMESH_VAR_RUNNER_POOL_TARGET_NODES=%s" % names,
                      "FORKMESH_VAR_HOSTED_GIT_CORE_NODES=%s" % hosted,
                      "FORKMESH_VAR_HOSTED_GIT_CORE_OWNERS=%s" % owners,
                      "FORKMESH_VAR_CORE_ADMIN_UPDATES=%s" % admin_updates])


def public_hosts(fleet):
    """Every public hostname a core's App answers to (the host rejects any
    other Host header; app/host/forkmesh_host/asgi.py)."""
    hosts = [h for h in served_hosts(fleet) if h not in shared_hosts(fleet)]
    return hosts + shared_hosts(fleet) + ["%s.%s" % (n["name"], fleet["domain"])
                                          for n in fleet["nodes"]]


# The certsync pair server authenticates with its own secret, which the app
# never loads (only fmnext-certsync.service / fmnext-cert-sync.service).
CERTSYNC_SECRET_NAME = "FORKMESH_CERTSYNC_SECRET"
# The runner's untrusted job user (docs/RUNNERS.md "Job sandbox").
JOB_USER = "forkmesh-job"
JOB_HOME = "/var/lib/forkmesh-job"
RUNNER_WORK_ROOT = "/var/lib/forkmesh-runner/work"
RUNNER_JOB_TMP = "/var/lib/forkmesh-runner/tmp"
RUNNER_JOB_SWAP_MAX = "1G"
# ForkMesh-hosted repositories on the cores (docs/HOSTED_REPOS.md). The fetch
# guard sits a little above the App's per-repository limit (200 MiB,
# HOSTED_REPO_MAX_BYTES in app/wrangler.toml) so a repository at the limit
# still replicates.
HOSTED_ROOT = "/var/lib/forkmesh/hosted"
HOSTED_MAX_BYTES = 256 * 1024 * 1024
# Largest request bodies (must match the App host's limits in
# app/host/forkmesh_host/asgi.py): 16 MB everywhere, 32 MB only for a signed
# pull submission (POST /api/repo/<owner>/<repo>/pulls). The per-node name's
# Git gateway takes pushes up to GIT_BODY_MAX.
APP_BODY_MAX = "16MB"
LARGE_BODY_MAX = "32MB"
LARGE_BODY_PATH = r"^/api/repo/[^/]+/[^/]+/pulls$"
GIT_BODY_MAX = "1GB"
# A Git push through the App (POST /<owner>/<repo>/git-receive-pack) streams to
# a core gateway; ForkMesh-hosted repositories (docs/HOSTED_REPOS.md) need
# more than the App's 16 MB. Matches request_guard.DEFAULT_MAX_GIT_PUSH_BODY.
GIT_PUSH_BODY_MAX = "256MB"
GIT_PUSH_PATH = r"^/[^/]+/[^/]+/git-receive-pack$"
GIT_PUSH_REFS_PATH = r"^/[^/]+/[^/]+/info/refs$"


# --------------------------------------------------------------------------
# intrusion detection (deploy/core/ids.py, docs/IDS.md)

IDS_KEY_CREDENTIAL = "FMNEXT_IDS_KEY"
CADDY_BIN = "/usr/local/bin/caddy"
CADDY_DEBIAN_BIN = "/usr/bin/caddy"
CADDY_BINARY_DROPIN = "/etc/systemd/system/caddy.service.d/forkmesh-next-binary.conf"
CADDY_VERSION = re.compile(r"^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$")
SHA256_HEX = re.compile(r"^[0-9a-f]{64}$")


def caddy_settings(fleet):
    caddy = fleet.get("caddy")
    if not caddy:
        return None
    version = str(caddy.get("version") or "")
    sha256 = str(caddy.get("sha256") or "")
    if not CADDY_VERSION.match(version):
        raise RenderError("caddy.version must look like 2.11.4, not %r" % version)
    if not SHA256_HEX.match(sha256):
        raise RenderError("caddy.sha256 must be 64 lowercase hex characters")
    return {"version": version, "sha256": sha256,
            "tarball": "caddy_%s_linux_amd64.tar.gz" % version}


def caddy_binary_dropin(fleet):
    settings = caddy_settings(fleet)
    if settings is None:
        raise RenderError("deploy/fleet.toml has no [caddy] version")
    return (
        "# Managed by tools/next_fleet.py (step caddy): Caddy %s from the\n"
        "# upstream release instead of Debian's %s (docs/OPERATIONS.md).\n"
        "[Service]\n"
        "ExecStart=\n"
        "ExecStart=%s run --environ --config /etc/caddy/Caddyfile\n"
        "ExecReload=\n"
        "ExecReload=%s reload --config /etc/caddy/Caddyfile --force\n"
        % (settings["version"], CADDY_DEBIAN_BIN, CADDY_BIN, CADDY_BIN))


IDS_MAIL_CREDENTIALS = ("FMNEXT_IDS_MAIL_TOKEN", "FMNEXT_IDS_ALERT_EMAIL")
IDS_STATE_DIR = "/var/lib/fmnext-ids"
IDS_SEVERITIES = ("info", "alert", "lockdown")
# Units the lockdown stops; each gets a drop-in that keeps it from starting
# (even after a reboot) while the lockdown marker exists.
IDS_CORE_STOP_UNITS = ("forkmesh-mirror-node.service", "forkmesh-app.service",
                       "caddy.service", "fmnext-certsync.service",
                       "fmnext-cert-sync.timer", "fmnext-cert-sync.service")
IDS_RUNNER_STOP_UNITS = ("forkmesh-mirror-node.service",)
IDS_LOCKDOWN_CONDITION = IDS_STATE_DIR + "/LOCKDOWN"
IDS_RQLITE_CONDITION = IDS_STATE_DIR + "/RQLITE_STOPPED"
# Watched on every node (docs/IDS.md "What is detected").
IDS_LOCKDOWN_PATHS = (
    "/usr/local/bin", "/usr/local/sbin", "/usr/local/share/forkmesh",
    "/opt/forkmesh-next/bin", "/etc/systemd/system", "/etc/ssh/sshd_config",
    "/etc/ssh/sshd_config.d", "/etc/sudoers", "/etc/sudoers.d",
    "/etc/ld.so.preload", "/etc/pam.d", "/etc/nftables.conf",
    "/etc/passwd", "/etc/group", "/etc/shadow", "/etc/gshadow")
IDS_ALERT_PATHS = (
    "/etc/forkmesh-next", "/etc/forkmesh/mirror-node.json",
    "/etc/forkmesh/node.env", "/etc/crontab", "/etc/cron.d",
    "/var/spool/cron/crontabs", "/etc/sysctl.d", "/etc/hosts",
    "/etc/ld.so.conf.d", "/etc/wireguard/wg0.conf")
IDS_HOT_PATHS = (
    "/etc/passwd", "/etc/group", "/etc/shadow", "/etc/gshadow", "/etc/sudoers",
    "/etc/sudoers.d", "/etc/ssh/sshd_config", "/etc/ssh/sshd_config.d",
    "/etc/ld.so.preload")
IDS_EXPECTED_MODULES = ("inet_diag", "tcp_diag", "dm_mod")
IDS_EXCLUDE = ("/etc/forkmesh-next/tls", "/etc/forkmesh-next/tls/*",
               "/etc/forkmesh-next/caddy/tls-*.caddy",
               "/etc/forkmesh-next/caddy/acme-http.caddy",
               "/etc/forkmesh-next/caddy/request-rules.caddy",
               "/etc/forkmesh-next/caddy/.request-rules.caddy.tmp",
               "*/__pycache__", "*/__pycache__/*")


def ids_core_stop_units(fleet):
    return IDS_CORE_STOP_UNITS + ((TURN_UNIT,) if turn_enabled(fleet) else ())


def ids_settings(fleet):
    """The validated ``[ids]`` table (defaults filled in)."""
    ids = dict(fleet.get("ids") or {})
    ids.setdefault("enabled", bool(fleet.get("ids")))
    if not isinstance(ids["enabled"], bool):
        raise RenderError("ids.enabled must be true or false")
    for item in ids.get("ssh_fleet_fingerprints") or []:
        if not SSH_FINGERPRINT.match(str(item)):
            raise RenderError("ids.ssh_fleet_fingerprints: %r is not a "
                              "SHA256:... key fingerprint" % item)
    for cidr in ids.get("ssh_allow") or []:
        try:
            network = ipaddress.ip_network(str(cidr), strict=False)
        except ValueError:
            raise RenderError("ids.ssh_allow: %r is not an address or CIDR"
                              % cidr) from None
        if network.version != 4:
            raise RenderError("ids.ssh_allow: IPv4 only (%r)" % cidr)
    key = ids.get("operator_public_key", "")
    if key and not IDS_KEY.match(key):
        raise RenderError("ids.operator_public_key must be the 43-character "
                          "base64url key from tools/next_ids.py keygen")
    for name, level in (ids.get("severity") or {}).items():
        if level not in IDS_SEVERITIES:
            raise RenderError("ids.severity.%s must be one of %s" % (
                name, ", ".join(IDS_SEVERITIES)))
    if ids.get("ssh_foreign_ip_severity", "alert") not in IDS_SEVERITIES:
        raise RenderError("ids.ssh_foreign_ip_severity must be one of %s"
                          % ", ".join(IDS_SEVERITIES))
    url = ids.get("webhook_url", "")
    if url and not str(url).startswith("https://"):
        raise RenderError("ids.webhook_url must be https://")
    return ids


def logs_settings(fleet):
    logs = dict(fleet.get("logs") or {})
    key = str(logs.get("reader_public_key", "") or "").strip()
    if key and not LOGREAD_PUBLIC_KEY.match(key):
        raise RenderError("logs.reader_public_key must be the one-line "
                          "ssh-ed25519 public key ending in fmnext-logread "
                          "(tools/next_fleet.py logread-key)")
    logs["reader_public_key"] = key
    access = logs.get("access_log", False)
    if not isinstance(access, bool):
        raise RenderError("logs.access_log must be true or false")
    logs["access_log"] = access
    return logs


def logread_fingerprint(public_key):
    match = LOGREAD_PUBLIC_KEY.match(public_key or "")
    if not match:
        return ""
    digest = hashlib.sha256(base64.b64decode(match.group(1))).digest()
    return "SHA256:" + base64.b64encode(digest).decode().rstrip("=")


def logread_authorized_line(public_key):
    return ('restrict,command="/usr/bin/python3 -I %s" %s\n'
            % (LOGREAD_PROGRAM, public_key))


def without_logs(fleet):
    stripped = copy.deepcopy(fleet)
    stripped.pop("logs", None)
    return stripped


def add_logread_files(fleet, add):
    key = logs_settings(fleet)["reader_public_key"]
    if not key:
        return False
    add(LOGREAD_PROGRAM, (HERE / "logread.py").read_text("utf-8"), 0o755)
    add(LOGREAD_AUTHORIZED_KEY, logread_authorized_line(key), 0o600)
    return True


def access_log_snippet():
    out = ["(access_log) {",
           "\t@access_log_quiet path /readyz /health",
           "\tlog_skip @access_log_quiet",
           "\tlog_append user_agent {http.request.header.User-Agent}",
           "\tlog_append expected_degraded "
           "{http.response.header.X-Forkmesh-Expected-Degraded}",
           "\t# The visitor Cloudflare saw, read before client_headers drops",
           "\t# the Cf-* headers and only when the peer is Cloudflare.",
           "\t@access_log_cloudflare remote_ip %s" % " ".join(CLOUDFLARE_RANGES),
           "\tvars @access_log_cloudflare {"]
    out += ["\t\t%s {http.request.header.%s}" % field
            for field in ACCESS_LOG_CLOUDFLARE_FIELDS]
    out += ["\t}",
            "\t# The visitor the forkmesh.com Worker relays for, as its",
            "\t# x-forkmesh-edge-client header names it (the App checks the",
            "\t# signature; this is only for the log).",
            "\t@access_log_relayed {",
            "\t\tremote_ip %s" % " ".join(CLOUDFLARE_RANGES),
            "\t\theader_regexp edge_client X-Forkmesh-Edge-Client "
            "^v1,([0-9A-Fa-f.:]+),([A-Z]*),",
            "\t}",
            "\tvars @access_log_relayed {"]
    out += ["\t\t%s %s" % field for field in ACCESS_LOG_RELAY_FIELDS]
    out += ["\t}"]
    out += ["\tlog_append %s {vars.%s}" % (name, name)
            for name, _ in ACCESS_LOG_CLOUDFLARE_FIELDS + ACCESS_LOG_RELAY_FIELDS]
    out += ["\tlog {",
            "\t\toutput stderr",
            "\t\tformat filter {",
            "\t\t\trequest>headers delete",
            "\t\t\trequest>tls delete",
            "\t\t\trequest>remote_port delete",
            "\t\t\tresp_headers delete",
            "\t\t\tuser_id delete",
            "\t\t\trequest>uri query {"]
    out += ["\t\t\t\treplace %s REDACTED" % name
            for name in ACCESS_LOG_QUERY_KEYS]
    out += ["\t\t\t}", "\t\t}", "\t}", "}", ""]
    return out


def ids_enabled(fleet):
    return ids_settings(fleet)["enabled"]


def ids_credential_names(fleet, role):
    ids = ids_settings(fleet)
    names = [IDS_KEY_CREDENTIAL]
    if role == "core":
        names.append("RQLITE_PASSWORD")
    if ids.get("direct_mail"):
        names += list(IDS_MAIL_CREDENTIALS)
    return names


def ids_env_settings(fleet):
    """app.env lines: the IDS public keys the app verifies quarantine rows
    with (forkmesh_host.quarantine)."""
    if not ids_enabled(fleet):
        return "# IDS off ([ids] enabled = false)."
    keys = ",".join("%s=%s" % (n["name"], n["ids_pubkey"])
                    for n in fleet["nodes"] if n.get("ids_pubkey"))
    return ("# Intrusion detection (docs/IDS.md): the core IDS public keys and\n"
            "# the operator release key; quarantine rows are verified with them.\n"
            "FORKMESH_IDS_PUBKEYS=%s\nFORKMESH_IDS_OPERATOR_PUBKEY=%s" % (
                keys, ids_settings(fleet).get("operator_public_key", "")))


def ids_config(fleet, name, role, paths):
    ids = ids_settings(fleet)
    rq = fleet.get("rqlite") or {}
    app = fleet.get("app") or {}
    wg = fleet.get("wireguard") or {}
    cores = fleet["nodes"]
    config = {
        "node": name, "role": role,
        "state_dir": IDS_STATE_DIR, "run_dir": "/run/fmnext-ids",
        "key_credential": IDS_KEY_CREDENTIAL,
        "nodes": [n["name"] for n in cores],
        "node_public_keys": {n["name"]: n["ids_pubkey"] for n in cores
                             if n.get("ids_pubkey")},
        "operator_public_key": ids.get("operator_public_key", ""),
        "ssh": {"fleet_fingerprints": list(ids.get("ssh_fleet_fingerprints")
                                           or []) + [
            fp for fp in [logread_fingerprint(
                logs_settings(fleet)["reader_public_key"])] if fp],
                "allow": list(ids.get("ssh_allow") or []),
                "foreign_ip_severity": ids.get("ssh_foreign_ip_severity",
                                               "alert")},
        "severity": dict(ids.get("severity") or {}),
        "integrity": {
            "lockdown": list(IDS_LOCKDOWN_PATHS),
            "alert": list(IDS_ALERT_PATHS),
            "trees": [paths["current"]] if role == "core" else [],
            "hot": list(IDS_HOT_PATHS),
            "exclude": list(IDS_EXCLUDE),
            "authorized_keys": True,
            "interval": 60, "hot_interval": 10, "full_rehash_hours": 6,
        },
        "heartbeat": {"interval": 30,
                      "stale_ms": int(ids.get("heartbeat_stale_s", 180))
                      * 1000},
        "remove_voter": bool(ids.get("remove_voter", True)),
        "modules": {"expected": list(IDS_EXPECTED_MODULES)},
        "webhook": {"url": ids.get("webhook_url", "")},
    }
    if ids.get("direct_mail"):
        config["mail"] = {"token_credential": IDS_MAIL_CREDENTIALS[0],
                          "to_credential": IDS_MAIL_CREDENTIALS[1],
                          "from": ids.get("mail_from", "")}
    if role == "core":
        turn_tcp, turn_udp = (turn_ports(fleet)[:2] if turn_enabled(fleet)
                              else ([], []))
        turn_users = ({TURN_USER: {"units": [TURN_UNIT], "exe": [TURN_BIN]}}
                      if turn_enabled(fleet) else {})
        config.update({
            "processes": {
                "service_users": {
                    "forkmesh-app": {
                        "units": ["forkmesh-app.service"],
                        "exe": ["/usr/bin/python3*"]},
                    "rqlite": {"units": ["rqlite.service"],
                               "exe": ["/usr/local/bin/rqlited"]},
                    "caddy": {"units": ["caddy.service"],
                              "exe": [CADDY_BIN, CADDY_DEBIAN_BIN]},
                    "fmnext-certsync": {
                        "units": ["fmnext-certsync.service",
                                  "fmnext-cert-sync.service"],
                        "exe": ["/usr/bin/python3*", "/usr/bin/openssl"]},
                    "forkmesh-node": {
                        "units": ["forkmesh-mirror-node.service"],
                        "exe": ["/usr/local/bin/forkmesh-mirror-node",
                                "/usr/bin/git", "/usr/lib/git-core/*",
                                "/usr/bin/python3*", "/usr/bin/ssh"],
                        # The daemon holds no vault key; its files are
                        # watched. Alerts, so a lingering user manager or
                        # a git hook never locks a core down by itself.
                        "shell_severity": "alert",
                        "outside_severity": "alert"},
                    **turn_users,
                },
                "protected_units": ["forkmesh-app.service", "rqlite.service",
                                    "caddy.service",
                                    "fmnext-certsync.service",
                                    "fmnext-ids.service"],
            },
            "listeners": {
                "tcp": sorted({22, 25, 80, 443, 53, int(rq.get("http_port", 4001)),
                               int(rq.get("raft_port", 4002)),
                               int(app.get("port", 8080)),
                               int(app.get("gateway_port", 8790)), 8791,
                               int(app.get("mesh_port", 8792)),
                               int(app.get("certsync_port", 8793)),
                               int(app.get("acme_forward_port", 8794)),
                               *turn_tcp}),
                "udp": sorted({53, 68, 123, 323, 443, 546,
                               int(wg.get("port", 51820)), *turn_udp}),
                "udp_max_port": 10240},
            "rqlite": {"url": "http://127.0.0.1:%d" % int(
                rq.get("http_port", 4001)),
                       "user": rq.get("user", "fmnext"),
                       "password_credential": "RQLITE_PASSWORD"},
            "unseal_socket": UNSEAL_SOCKET,
            "lockdown": {
                "stop_units": list(ids_core_stop_units(fleet)),
                "rqlite_unit": "rqlite.service",
                "core_wg_ips": sorted(n["wg_ip"] for n in cores),
                "core_wg_ips_by_node": {n["name"]: n["wg_ip"] for n in cores},
                "core_public_ips": sorted(n["public_ipv4"] for n in cores),
                "wg_port": int(wg.get("port", 51820)),
                "raft_port": int(rq.get("raft_port", 4002)),
            },
        })
    else:
        runner = runner_by_name(fleet, name)
        config.update({
            "processes": {
                "service_users": {
                    # The runner's files owner; the daemon itself is root.
                    "forkmesh-node": {"units": ["-"],
                                      "outside_severity": "alert"}},
                "job_user": JOB_USER,
                # Jobs: fmnext-job-<hex>.scope; the website sign-in PTY:
                # an unnamed systemd-run scope (run-*.scope).
                "job_units": ["fmnext-job-*.scope", "run-*.scope"],
                "protected_units": ["forkmesh-mirror-node.service",
                                    "fmnext-ids.service"],
            },
            "listeners": {"tcp": [22, 53, 8791], "udp": [53, 68, 123, 323, 546],
                          "udp_max_port": 10240,
                          "loopback_allow": [
                              {"user": JOB_USER, "units": ["run-*.scope"],
                               "exe": ["/usr/local/lib/claude/claude"]},
                              {"user": JOB_USER,
                               "units": ["fmnext-job-*.scope"],
                               "exe": [RUNNER_WORK_ROOT + "/*.test",
                                       "/tmp/*.test", "/var/tmp/*.test"]}]},
            "lockdown": {"stop_units": list(IDS_RUNNER_STOP_UNITS)},
            "runner": {"app_url": "https://%s" % served_app_host(fleet),
                       "node_name": runner.get("node_name")
                       or "fmnext-%s" % name,
                       "identity": "/var/lib/forkmesh/identity/ed25519.pem"},
        })
    return json.dumps(config, indent=2, sort_keys=True) + "\n"


def ids_dropin(condition, unit):
    return ("# Managed by deploy/core/render.py (docs/IDS.md). %s never starts\n"
            "# while the IDS holds this node in lockdown; only\n"
            "# tools/next_ids.py release removes the marker.\n"
            "[Unit]\nConditionPathExists=!%s\n" % (unit, condition))


SELF_DEPLOY_KEY = re.compile(r"^[A-Za-z0-9_-]{43}$")


def self_deploy_settings(fleet):
    settings = dict(fleet.get("self_deploy") or {})
    keys = list(settings.get("approver_keys") or [])
    for key in keys:
        if not SELF_DEPLOY_KEY.match(str(key)):
            raise RenderError("self_deploy.approver_keys: %r is not a 43-character "
                              "base64url Ed25519 public key" % key)
    repository = str(settings.get("repository") or "forkmesh/forkmesh")
    if repository.count("/") != 1:
        raise RenderError("self_deploy.repository must be owner/name")
    admin_updates = bool(settings.get("admin_updates"))
    return {
        "enabled": bool(settings.get("enabled")) and (bool(keys) or admin_updates),
        "approverKeys": keys,
        "adminUpdates": admin_updates,
        "repository": repository,
        "gitDir": str(settings.get("git_dir")
                      or "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"),
        "buildMemoryMax": str(settings.get("build_memory_max") or "450M"),
        "buildCPUQuota": str(settings.get("build_cpu_quota") or "60%"),
        "buildUser": "fmnext-build",
        "rollDaemon": bool(settings.get("roll_daemon", True)),
        "daemonBuildMemoryMax": str(settings.get("daemon_build_memory_max")
                                    or "700M"),
        "go": str(settings.get("go") or "/usr/bin/go"),
    }


def add_self_deploy_files(fleet, name, paths, values, add, units,
                          credentials_by_unit):
    settings = self_deploy_settings(fleet)
    if not settings["enabled"]:
        return
    order = [n["name"] for n in fleet.get("nodes") or []]
    config = dict(settings, node=name, order=order,
                  host="%s.%s" % (name, fleet["domain"]))
    names = [IDS_KEY_CREDENTIAL] if ids_enabled(fleet) else []
    unit_values = dict(values, SELF_DEPLOY_CREDENTIALS="\n".join(
        "LoadCredentialEncrypted=%s:%s/%s.cred" % (n, paths["creds"], n)
        for n in names))
    add("/etc/systemd/system/fmnext-self-deploy.service",
        template("fmnext-self-deploy.service", unit_values))
    add(paths["opt"] + "/bin/fmnext-self-deploy",
        (HERE / "self_deploy.py").read_text("utf-8"), 0o755)
    add(paths["opt"] + "/bin/forkmesh_safe_delete.py",
        (REPO / "tools" / "forkmesh_safe_delete.py").read_text("utf-8"), 0o644)
    add(paths["etc"] + "/self-deploy.json",
        json.dumps(config, indent=2, sort_keys=True) + "\n", 0o644)
    units.append("fmnext-self-deploy.service")
    if names:
        credentials_by_unit["fmnext-self-deploy.service"] = names


def add_ids_files(fleet, name, role, paths, values, add, units,
                  credentials_by_unit):
    """The IDS unit, program, config and the lockdown drop-ins."""
    if not ids_enabled(fleet):
        return
    unit = "/etc/systemd/system/"
    names = ids_credential_names(fleet, role)
    values = dict(values, IDS_CREDENTIALS="\n".join(
        "LoadCredentialEncrypted=%s:%s/%s.cred" % (n, paths["creds"], n)
        for n in names))
    add(unit + "fmnext-ids.service", template("fmnext-ids.service", values))
    add(paths["opt"] + "/bin/fmnext-ids",
        (HERE / "ids.py").read_text("utf-8"), 0o755)
    add(paths["opt"] + "/bin/ids_records.py",
        (REPO / "app" / "host" / "forkmesh_host" / "ids_records.py")
        .read_text("utf-8"), 0o644)
    add(paths["etc"] + "/ids.json", ids_config(fleet, name, role, paths),
        0o600)
    stops = ids_core_stop_units(fleet) if role == "core" else IDS_RUNNER_STOP_UNITS
    for stop in stops:
        add(unit + stop + ".d/50-fmnext-ids.conf",
            ids_dropin(IDS_LOCKDOWN_CONDITION, stop))
    if role == "core":
        add(unit + "rqlite.service.d/50-fmnext-ids.conf",
            ids_dropin(IDS_RQLITE_CONDITION, "rqlite.service"))
    units.append("fmnext-ids.service")
    credentials_by_unit["fmnext-ids.service"] = names


def ids_egress_log_rules():
    """Log (never block) new outbound connections of the core service users
    to anything but HTTP(S) outside loopback and the mesh; the IDS reads the
    lines from the kernel log (egress_unexpected)."""
    users = '{ "forkmesh-app", "caddy", "rqlite", "fmnext-certsync" }'
    local = "{ 127.0.0.0/8, 10.77.0.0/24 }"
    local6 = "{ ::1, fe80::/10 }"
    return [
        "\t\t# IDS (docs/IDS.md): log unusual outbound connections of the",
        "\t\t# service users (log only; rate-limited).",
        "\t\tmeta skuid %s ct state new ip daddr != %s tcp flags & (syn | ack) == syn tcp dport != "
        "{ 80, 443 } limit rate 6/minute log prefix \"fmnext-ids-egress: \" "
        "flags skuid" % (users, local),
        "\t\tmeta skuid %s ct state new ip daddr != %s udp dport != "
        "{ 53, 123, 443 } limit rate 6/minute log prefix "
        "\"fmnext-ids-egress: \" flags skuid" % (users, local),
        "\t\tmeta skuid %s ct state new ip6 daddr != %s tcp flags & (syn | ack) == syn tcp dport != "
        "{ 80, 443 } limit rate 6/minute log prefix \"fmnext-ids-egress: \" "
        "flags skuid" % (users, local6),
        "\t\tmeta skuid %s ct state new ip6 daddr != %s udp dport != "
        "{ 53, 123, 443 } limit rate 6/minute log prefix "
        "\"fmnext-ids-egress: \" flags skuid" % (users, local6),
    ]


# --------------------------------------------------------------------------
# office TURN/STUN relay (coturn)


TURN_SECRET_CREDENTIAL = "OFFICE_TURN_SECRET"
TURN_USER = "turnserver"
TURN_BIN = "/usr/bin/turnserver"
TURN_UNIT = "fmnext-turn.service"
TURN_CERT_PATH_UNIT = "fmnext-turn-cert.path"
TURN_CERT_UNIT = "fmnext-turn-cert.service"
TURN_DEFAULTS = {"enabled": False, "realm": "forkmesh.com", "port": 3478,
                 "tls_port": 5349, "relay_min_port": 49160,
                 "relay_max_port": 49999, "user_quota": 12,
                 "total_quota": 1200, "max_bps": 512000,
                 "max_allocate_lifetime": 3600,
                 "fallback_stun": "stun:stun.cloudflare.com:3478"}
TURN_DENIED_PEERS = (
    "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
    "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24",
    "192.88.99.0/24", "192.168.0.0/16", "198.18.0.0/15", "198.51.100.0/24",
    "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4",
    "::1/128", "::ffff:0:0/96", "64:ff9b::/96", "64:ff9b:1::/48", "100::/64",
    "2001::/32", "2001:db8::/32", "2002::/16", "fc00::/7", "fe80::/10",
    "fec0::/10", "ff00::/8")
TURN_HOST_NAME = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
                            r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$")


def turn_settings(fleet):
    raw = fleet.get("turn") or {}
    if not isinstance(raw, dict):
        raise RenderError("[turn] must be a table")
    unknown = set(raw) - set(TURN_DEFAULTS) - {"host"}
    if unknown:
        raise RenderError("unknown [turn] keys: %s" % ", ".join(sorted(unknown)))
    settings = dict(TURN_DEFAULTS, **raw)
    if not isinstance(settings["enabled"], bool):
        raise RenderError("turn.enabled must be true or false")
    settings["host"] = str(settings.get("host") or served_app_host(fleet)).lower()
    if not TURN_HOST_NAME.match(settings["host"]):
        raise RenderError("turn.host must be a DNS name")
    if not TURN_HOST_NAME.match(str(settings["realm"])):
        raise RenderError("turn.realm must be a DNS name")
    for key in ("port", "tls_port", "relay_min_port", "relay_max_port",
                "user_quota", "total_quota", "max_bps",
                "max_allocate_lifetime"):
        value = settings[key]
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise RenderError("turn.%s must be a positive integer" % key)
    if not 1024 < settings["port"] < 10240 or not 1024 < settings["tls_port"] < 10240:
        raise RenderError("turn.port and turn.tls_port must be in 1025-10239")
    if not (32768 <= settings["relay_min_port"] < settings["relay_max_port"]
            <= 65535) or settings["relay_max_port"] - settings[
                "relay_min_port"] > 4096:
        raise RenderError("turn relay ports must be a range of at most 4096 "
                          "ports within 32768-65535")
    if settings["user_quota"] > settings["total_quota"]:
        raise RenderError("turn.user_quota must not exceed turn.total_quota")
    fallback = str(settings["fallback_stun"])
    if fallback and not re.fullmatch(r"stuns?:[a-z0-9.-]+(?::[0-9]{1,5})?",
                                     fallback):
        raise RenderError("turn.fallback_stun must be a stun: URL or empty")
    if settings["enabled"]:
        if TURN_SECRET_CREDENTIAL not in app_secret_names(fleet):
            raise RenderError("[turn] enabled needs %s in app.secrets or "
                              "app.optional_secrets (the app signs the TURN "
                              "credentials with it)" % TURN_SECRET_CREDENTIAL)
        clash = {"OFFICE_TURN_URLS", "OFFICE_STUN_URLS"} & set(
            (production(fleet).get("vars") or {}))
        if clash:
            raise RenderError("[turn] sets %s; remove it from production.vars"
                              % ", ".join(sorted(clash)))
    return settings


def turn_enabled(fleet):
    return turn_settings(fleet)["enabled"]


def turn_tls_name(fleet):
    host = turn_settings(fleet)["host"]
    return host if host in tls_hosts(fleet) else ""


def turn_urls(fleet):
    turn = turn_settings(fleet)
    host = turn["host"]
    urls = ["turn:%s:%d?transport=udp" % (host, turn["port"]),
            "turn:%s:%d?transport=tcp" % (host, turn["port"])]
    if turn_tls_name(fleet):
        urls.append("turns:%s:%d?transport=tcp" % (host, turn["tls_port"]))
    stun = ["stun:%s:%d" % (host, turn["port"])]
    if turn["fallback_stun"]:
        stun.append(turn["fallback_stun"])
    return urls, stun


def turn_app_settings(fleet):
    if not turn_enabled(fleet):
        return ""
    turn, stun = turn_urls(fleet)
    return "\n".join(["", "FORKMESH_VAR_OFFICE_TURN_URLS=%s" % ",".join(turn),
                      "FORKMESH_VAR_OFFICE_STUN_URLS=%s" % ",".join(stun)])


def turn_peer_range(cidr):
    network = ipaddress.ip_network(cidr)
    if network.num_addresses == 1:
        return str(network.network_address)
    return "%s-%s" % (network.network_address, network.broadcast_address)


def turn_ports(fleet):
    turn = turn_settings(fleet)
    tcp = [turn["port"]] + ([turn["tls_port"]] if turn_tls_name(fleet) else [])
    return tcp, [turn["port"]], "%d-%d" % (turn["relay_min_port"],
                                           turn["relay_max_port"])


def turnserver_conf(fleet, node, paths):
    turn = turn_settings(fleet)
    lines = [MANAGED.rstrip("\n") % node["name"],
             "listening-port=%d" % turn["port"]]
    addresses = [node["public_ipv4"]] + (
        [node["public_ipv6"]] if node.get("public_ipv6") else [])
    lines += ["listening-ip=%s" % a for a in addresses]
    lines += ["relay-ip=%s" % a for a in addresses]
    tls = turn_tls_name(fleet)
    if tls:
        export = paths["var"] + "/certsync/export/" + tls
        lines += ["tls-listening-port=%d" % turn["tls_port"],
                  "cert=%s.crt" % export, "pkey=%s.key" % export,
                  "cipher-list=ECDHE+AESGCM:ECDHE+CHACHA20",
                  "no-tlsv1", "no-tlsv1_1", "no-dtls"]
    else:
        lines += ["no-tls", "no-dtls"]
    lines += [
        "min-port=%d" % turn["relay_min_port"],
        "max-port=%d" % turn["relay_max_port"],
        "fingerprint",
        "use-auth-secret",
        "realm=%s" % turn["realm"],
        "server-name=%s" % turn["host"],
        "user-quota=%d" % turn["user_quota"],
        "total-quota=%d" % turn["total_quota"],
        "max-bps=%d" % turn["max_bps"],
        "max-allocate-lifetime=%d" % turn["max_allocate_lifetime"],
        "stale-nonce=600",
        "no-cli",
        "no-multicast-peers",
        "no-tcp-relay",
        "no-rfc5780",
        "no-stun-backward-compatibility",
        "response-origin-only-with-rfc5780",
        "no-software-attribute",
        "userdb=/var/lib/fmnext-turn/turndb",
        "pidfile=/run/fmnext-turn/turnserver.pid",
        "log-file=stdout",
        "simple-log",
    ]
    lines += ["denied-peer-ip=%s" % turn_peer_range(c) for c in TURN_DENIED_PEERS]
    return "\n".join(lines) + "\n"


def turn_ip_deny():
    return " ".join(("localhost", "link-local", "multicast") + tuple(
        c for c in TURN_DENIED_PEERS if not c.startswith(("127.", "169.254.",
                                                          "224.", "fe80:",
                                                          "ff00:"))))


def add_turn_files(fleet, node, paths, values, add, units,
                   credentials_by_unit):
    if not turn_enabled(fleet):
        return False
    unit = "/etc/systemd/system/"
    tls = turn_tls_name(fleet)
    values = dict(values, TURN_BIN=TURN_BIN, TURN_USER=TURN_USER,
                  TURN_SECRET=TURN_SECRET_CREDENTIAL,
                  TURN_IP_DENY=turn_ip_deny(),
                  TURN_CERT=(paths["var"] + "/certsync/export/" + tls + ".crt")
                  if tls else "")
    add(paths["etc"] + "/turnserver.conf", turnserver_conf(fleet, node, paths))
    add(paths["opt"] + "/bin/fmnext-turn-config",
        template("fmnext-turn-config", values), 0o755)
    add(unit + TURN_UNIT, template(TURN_UNIT, values))
    units.append(TURN_UNIT)
    credentials_by_unit[TURN_UNIT] = [TURN_SECRET_CREDENTIAL]
    if tls:
        add(unit + TURN_CERT_PATH_UNIT, template(TURN_CERT_PATH_UNIT, values))
        add(unit + TURN_CERT_UNIT, template(TURN_CERT_UNIT, values))
        units.append(TURN_CERT_PATH_UNIT)
    return True


# --------------------------------------------------------------------------
# generated files


WG_KEY_PLACEHOLDER = "@FMNEXT_WG0_PRIVATE_KEY@"


def wireguard_conf(fleet, node, port, prefix):
    lines = [MANAGED % node["name"],
             "# The private key is generated on this node by next_fleet.py",
             "# install (/etc/wireguard/wg0.key, 0600) and never leaves it.",
             "# The installer replaces the placeholder below with that key on",
             "# the node: a PostUp-only key is wiped by `systemctl reload",
             "# wg-quick@wg0`, whose `wg syncconf` applies this file as-is.",
             "[Interface]",
             "Address = %s/%d" % (node["wg_ip"], prefix),
             "ListenPort = %d" % port,
             "PrivateKey = %s" % WG_KEY_PLACEHOLDER,
             ""]
    peers = list(fleet["nodes"])
    if not is_app_node(node):
        peers += list(fleet.get("app_nodes") or [])
    for peer in peers:
        if peer["name"] == node["name"]:
            continue
        block = ["[Peer]",
                 "PublicKey = %s" % (peer.get("wg_pubkey") or "UNKNOWN"),
                 "AllowedIPs = %s/32" % peer["wg_ip"],
                 "Endpoint = %s:%d" % (peer["public_ipv4"], port),
                 "PersistentKeepalive = 25"]
        if peer.get("wg_pubkey"):
            lines += ["# %s" % peer["name"]] + block + [""]
        else:
            lines += ["# %s: public key unknown until next_fleet.py install runs"
                      " there; re-render afterwards." % peer["name"]]
            lines += ["#" + line for line in block] + [""]
    return "\n".join(lines).rstrip() + "\n"


def core_output_rules(fleet):
    """Output rules on a core node (audit M2/M4): only rqlite speaks Raft,
    only the app (and root's tools) reach rqlite's HTTP API, only the app
    reaches the peers' mesh, only root's certsync round reaches the peers'
    certsync. Everything else on the host (the daemon, Caddy, a compromised
    process of another uid) is refused. Replies are unaffected (the rules
    match the destination port of new connections)."""
    rq = fleet.get("rqlite") or {}
    app = fleet.get("app") or {}
    http_port = int(rq.get("http_port", 4001))
    raft_port = int(rq.get("raft_port", 4002))
    mesh_port = int(app.get("mesh_port", 8792))
    certsync_port = int(app.get("certsync_port", 8793))
    wg = "10.77.0.0/24"
    return [
        "\t\t# Raft (plaintext, no password): the rqlite user only.",
        "\t\tip daddr { %s, 127.0.0.0/8 } tcp dport %d meta skuid != \"rqlite\" "
        "counter reject with tcp reset" % (wg, raft_port),
        "\t\t# rqlite HTTP: the app, and root (rqlite.curlrc, the backup and",
        "\t\t# migration SSH tunnels, fmnext-app-ops).",
        "\t\tip daddr { %s, 127.0.0.0/8 } tcp dport %d meta skuid != { \"root\", "
        "\"forkmesh-app\" } counter reject with tcp reset" % (wg, http_port),
        "\t\t# Mesh (Durable Objects, peer unseal): the app only.",
        "\t\tip daddr { %s, 127.0.0.0/8 } tcp dport %d meta skuid != \"forkmesh-app\" "
        "counter reject with tcp reset" % (wg, mesh_port),
        "\t\t# certsync pair server: root's fmnext-cert-sync round only.",
        "\t\tip daddr { %s, 127.0.0.0/8 } tcp dport %d meta skuid != \"root\" "
        "counter reject with tcp reset" % (wg, certsync_port),
        "\t\t# Cloud metadata: nobody but root (cloud-init).",
        "\t\tip daddr 169.254.0.0/16 meta skuid != \"root\" counter reject",
    ]


def runner_output_rules():
    """Output rules on a runner (audit H1/L7): the job user reaches only the
    public internet. The daemon itself runs as root there (it must start
    jobs as the job user); its unit denies the metadata service with
    IPAddressDeny=, and forkmesh-node (the files owner) is refused here."""
    private4 = ("169.254.0.0/16, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, "
                "100.64.0.0/10, 127.0.0.0/8, 10.77.0.0/24, 0.0.0.0/8, "
                "224.0.0.0/4, 240.0.0.0/4")
    private6 = "::1, ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8"
    return [
        "\t\t# Jobs (%s): no metadata service, no private, CGNAT, loopback" % JOB_USER,
        "\t\t# or WireGuard addresses. DNS only to a local stub resolver",
        "\t\t# (systemd-resolved's 127.0.0.53), when the node uses one.",
        "\t\tmeta skuid \"%s\" ip daddr 127.0.0.53 meta l4proto { tcp, udp } "
        "th dport 53 accept" % JOB_USER,
        "\t\tmeta skuid \"%s\" ip daddr { %s } counter reject" % (JOB_USER, private4),
        "\t\tmeta skuid \"%s\" ip6 daddr { %s } counter reject" % (JOB_USER, private6),
        "\t\tmeta skuid \"forkmesh-node\" ip daddr 169.254.0.0/16 counter reject",
    ]


def nftables_conf(fleet, node_name, core_ports, public_tcp, public_udp,
                  wg_port=None, output_rules=(), autoscaled_set=False):
    core_ips = sorted(n["wg_ip"] for n in fleet["nodes"])
    if NODE_NAME.match(str(node_name)):
        core_ips += sorted(n["wg_ip"] for n in fleet.get("app_nodes") or [])
    lines = [
        "#!/usr/sbin/nft -f",
        MANAGED.rstrip("\n") % node_name,
        "flush ruleset",
        "",
        "table inet filter {",
        *(["\tset autoscaled {", "\t\ttype ipv4_addr", "\t}"]
          if autoscaled_set else []),
        "\tchain input {",
        "\t\ttype filter hook input priority filter; policy drop;",
        "\t\tct state established,related accept",
        "\t\tct state invalid drop",
        "\t\tiif \"lo\" accept",
        "\t\tip protocol icmp icmp type { echo-request, destination-unreachable, time-exceeded } limit rate 20/second accept",
        "\t\tmeta l4proto ipv6-icmp accept",
        "\t\ttcp dport { %s } accept" % ", ".join(str(p) for p in public_tcp),
    ]
    if public_udp:
        lines.append("\t\tudp dport { %s } accept" % ", ".join(str(p) for p in public_udp))
    if core_ports:
        # Mesh ports only from core WireGuard addresses on wg0 (runners, even
        # if they are ever given a WireGuard address, never reach them).
        lines.append("\t\tiifname \"wg0\" ip saddr { %s } tcp dport { %s } accept" % (
            ", ".join(core_ips), ", ".join(str(p) for p in core_ports)))
        if autoscaled_set:
            lines.append("\t\tiifname \"wg0\" ip saddr @autoscaled tcp dport { %s } "
                         "accept" % ", ".join(str(p) for p in core_ports))
    lines += [
        "\t}",
        "\tchain forward {",
        "\t\ttype filter hook forward priority filter; policy drop;",
        "\t}",
        "\tchain output {",
        "\t\ttype filter hook output priority filter; policy accept;",
        *output_rules,
        "\t}",
        "}",
    ]
    return "\n".join(lines) + "\n"


# The private Git mirror for Codemagic (tools/next_git_host.py writes the
# snippet: basic auth, then file_server over GIT_PRIVATE_ROOT). A glob, so a
# node without the file (Caddy only warns) serves no /git/ route at all.
GIT_PRIVATE_ROOT = "/var/lib/forkmesh-next/git"
REQUEST_RULES_GLOB = "request-rules*.caddy"
REQUEST_CHECK_PATH = "/_host/request-policy"
GIT_PRIVATE_GLOB = "git-private*.caddy"


def git_private_import(include_dir):
    return ["\t# Private Git over dumb HTTP, basic auth (tools/next_git_host.py);",
            "\t# first, so /git/* never reaches the App or the gateway.",
            "\timport %s/%s" % (include_dir, GIT_PRIVATE_GLOB)]


def request_rules_import(include_dir):
    return ["\timport %s/%s" % (include_dir, REQUEST_RULES_GLOB)]


def request_rules_paths(paths):
    return {"REQUEST_RULES_EXPORT": paths["var"] + "/app/request-rules.json",
            "REQUEST_RULES_CADDY": paths["etc"] + "/caddy/request-rules.caddy"}


def caddyfile(fleet, node, paths, per_node=True):
    app = fleet.get("app") or {}
    app_port = int(app.get("port", 8080))
    gateway_port = int(app.get("gateway_port", 8790))
    node_host = "%s.%s" % (node["name"], fleet["domain"])
    include_dir = paths["etc"] + "/caddy"
    access_log = logs_settings(fleet)["access_log"]
    out = [MANAGED % node["name"].rstrip()]
    out.append("# No fixed ACME issuer: for %s the"
               % " and ".join(tls_hosts(fleet)))
    out.append("# node that issues/renews is chosen among the live cores by "
               "fmnext-certsync,")
    out.append("# which writes the tls-*.caddy and acme-http.caddy includes "
               "below.")
    # The admin API is a unix socket in caddy's 0700 RuntimeDirectory (never
    # TCP: any local uid could /load a config that serves the TLS keys).
    # `caddy reload` (the unit's ExecReload, certsync's reload) finds it in
    # this file.
    out += ["{", "\tadmin unix//run/caddy/admin.sock"]
    if fleet.get("acme_email"):
        out.append("\temail %s" % fleet["acme_email"])
    limits = ["\t\ttimeouts {", "\t\t\tread_header 10s",
              "\t\t\tread_body 5m", "\t\t\tidle 2m", "\t\t}",
              "\t\tmax_header_size 64KB", "\t}"]
    out += ["\tservers :443 {", "\t\tprotocols h1 h2 h3",
            "\t\t# read_body is per listener, so it must fit a large git push",
            "\t\t# on the per-node name; App bodies are capped at %s/%s below"
            % (APP_BODY_MAX, LARGE_BODY_MAX),
            "\t\t# and by the App host itself. WebSockets are not affected."]
    out += limits
    out += ["\tservers :80 {", "\t\tprotocols h1"] + limits + ["}", ""]
    # Snippets ----------------------------------------------------------
    out += [
        "# Every Cf-* header a client sends is dropped; the App learns the",
        "# visitor from Cf-Connecting-Ip (this connection's peer) or, for",
        "# requests relayed by the next.forkmesh.com Worker, from the signed",
        "# x-forkmesh-edge-client header (deploy/core/edge_client.md).",
        "(client_headers) {",
        "\trequest_header -Cf-*",
        "\trequest_header -True-Client-Ip",
        "\trequest_header -X-Real-Ip",
        "\trequest_header -X-Forwarded-Host",
        "\trequest_header -Forwarded",
        "\trequest_header -X-Forkmesh-Edge-Where",
        "}",
        "",
        "(app_upstream) {",
        "\theader_up Cf-Connecting-Ip {remote_host}",
        "\t# Idle upstream connections close before uvicorn's 15 s keep-alive",
        "\t# does, so a request never races the App closing the connection.",
        "\ttransport http {",
        "\t\tkeepalive 10s",
        "\t}",
        "\tlb_try_duration 10s",
        "\tlb_try_interval 250ms",
        "\tflush_interval -1",
        "}",
        "",
        "(app_proxy) {",
        "\treverse_proxy 127.0.0.1:%d {" % app_port,
        "\t\timport app_upstream",
        "\t}",
        "}",
        "",
        "(request_policy_check) {",
        "\tforward_auth {args[0]} 127.0.0.1:%d {" % app_port,
        "\t\turi %s" % REQUEST_CHECK_PATH,
        "\t\theader_up Host %s" % served_app_host(fleet),
        "\t\timport app_upstream",
        "\t}",
        "}",
        "",
        "# world.next asks the App for App-owned assets under the App's name,",
        "# like world/worker.js did through its service binding.",
        "(app_proxy_app_host) {",
        "\treverse_proxy 127.0.0.1:%d {" % app_port,
        "\t\theader_up Host %s" % served_app_host(fleet),
        "\t\timport app_upstream",
        "\t}",
        "}",
        "",
        "# On every response, proxied, redirected or generated here (defer:",
        "# applied when the response is written, over any upstream value).",
        "(security_headers) {",
        "\theader {",
        "\t\tdefer",
        "\t\t-Server",
        "\t\tStrict-Transport-Security \"max-age=31536000; includeSubDomains\"",
        "\t\tX-Content-Type-Options nosniff",
        "\t}",
        "}",
        "",
        "# Body limits (the App host enforces the same ones).",
        "(app_body_limits) {",
        "\t@large_body {",
        "\t\tmethod POST",
        "\t\tpath_regexp %s" % LARGE_BODY_PATH,
        "\t}",
        "\t@git_push_body {",
        "\t\tmethod POST",
        "\t\tpath_regexp git_push %s" % GIT_PUSH_PATH,
        "\t}",
        "\t@normal_body {",
        "\t\tnot {",
        "\t\t\tmethod POST",
        "\t\t\tpath_regexp %s" % LARGE_BODY_PATH,
        "\t\t}",
        "\t\tnot {",
        "\t\t\tmethod POST",
        "\t\t\tpath_regexp git_push %s" % GIT_PUSH_PATH,
        "\t\t}",
        "\t}",
        "\trequest_body @large_body {",
        "\t\tmax_size %s" % LARGE_BODY_MAX,
        "\t}",
        "\trequest_body @git_push_body {",
        "\t\tmax_size %s" % GIT_PUSH_BODY_MAX,
        "\t}",
        "\trequest_body @normal_body {",
        "\t\tmax_size %s" % APP_BODY_MAX,
        "\t}",
        "}",
        "",
        *(access_log_snippet() if access_log else []),
        "(site_common) {",
        *(["\timport access_log"] if access_log else []),
        "\timport client_headers",
        "\timport security_headers",
        "\timport app_body_limits",
        "\tencode zstd gzip",
        "\t# Host-internal and mesh paths are never public. A handle (not a",
        "\t# bare respond) so it runs before the route handles that follow.",
        "\t@internal path /_host/* /_do/* /_rpc/*",
        "\thandle @internal {",
        "\t\trespond 404",
        "\t}",
        "}",
        "",
    ]
    # Shared names ------------------------------------------------------
    routes = {served_app_host(fleet): "app-routes.caddy",
              served_world_host(fleet): "world-routes.caddy"}
    git_private = git_private_import(include_dir)
    request_rules = request_rules_import(include_dir)
    for host in served_hosts(fleet):
        out.append("%s {" % host)
        out += ["\t# files | acme | internal, chosen by fmnext-certsync",
                "\timport %s/tls-%s.caddy" % (include_dir, host)]
        out += request_rules
        if host == served_app_host(fleet):
            out += git_private
        out += ["\timport site_common",
                "\timport %s/%s" % (include_dir, routes[host]),
                "}", ""]
    targets = {fleet["app_host"]: served_app_host(fleet),
               fleet["world_host"]: served_world_host(fleet)}
    for host in legacy_hosts(fleet):
        out.append("%s {" % host)
        out += ["\t# Next name after the cutover (docs/CUTOVER.md): redirects.",
                "\timport %s/tls-%s.caddy" % (include_dir, host)]
        out += request_rules
        if host == fleet["app_host"]:
            out += git_private
        out += ["\timport client_headers", "\timport security_headers"]
        if host == fleet["app_host"]:
            out += ["\t@readiness path %s" % " ".join(LEGACY_PASSTHROUGH),
                    "\thandle @readiness {", "\t\timport app_proxy_app_host",
                    "\t}"]
        out += ["\thandle {",
                "\t\tredir https://%s{uri} 308" % targets[host],
                "\t}", "}", ""]
    out += [
        "# HTTP-01 for the shared names: whichever A record Let's Encrypt picks,",
        "# fmnext-certsync's forwarder (127.0.0.1:%d) asks the other cores."
        % int(app.get("acme_forward_port", 8794)),
        "import %s/acme-http.caddy" % include_dir,
        "",
    ]
    if not per_node:
        return "\n".join(out) + "\n"
    out += [
        "%s {" % node_host,
        *request_rules,
        *git_private,
        *(["\timport access_log"] if access_log else []),
        "\timport client_headers",
        "\timport security_headers",
        "\t# The /_app relay carries App requests: the App's limits. Git pushes",
        "\t# to the gateway are streamed to disk, up to %s." % GIT_BODY_MAX,
        "\t@app_large_body {",
        "\t\tmethod POST",
        "\t\tpath_regexp ^/_app%s" % LARGE_BODY_PATH.lstrip("^"),
        "\t}",
        "\t@app_body {",
        "\t\tpath /_app/* /readyz",
        "\t\tnot {",
        "\t\t\tmethod POST",
        "\t\t\tpath_regexp ^/_app%s" % LARGE_BODY_PATH.lstrip("^"),
        "\t\t}",
        "\t}",
        "\t@git_body {",
        "\t\tnot path /_app/* /readyz",
        "\t}",
        "\trequest_body @app_large_body {",
        "\t\tmax_size %s" % LARGE_BODY_MAX,
        "\t}",
        "\trequest_body @app_body {",
        "\t\tmax_size %s" % APP_BODY_MAX,
        "\t}",
        "\trequest_body @git_body {",
        "\t\tmax_size %s" % GIT_BODY_MAX,
        "\t}",
        "\t@internal path /_host/* /_do/* /_rpc/*",
        "\thandle @internal {",
        "\t\trespond 404",
        "\t}",
        "\thandle /readyz {",
        "\t\timport app_proxy",
        "\t}",
        "\t# next.forkmesh.com's Worker falls back to https://nN.%s/_app/..."
        % fleet["domain"],
        "\t# when %s does not answer (www/worker.js appFetch)." % fleet["app_host"],
        "\thandle_path /_app/* {",
        "\t\t@relay_internal path /_host/* /_do/* /_rpc/*",
        "\t\thandle @relay_internal {",
        "\t\t\trespond 404",
        "\t\t}",
        "\t\thandle {",
        "\t\t\timport app_proxy_app_host",
        "\t\t}",
        "\t}",
        "\t@git_push_app {",
        "\t\tpath_regexp git_push %s" % GIT_PUSH_PATH,
        "\t}",
        "\t@git_push_refs {",
        "\t\tpath_regexp git_push_refs %s" % GIT_PUSH_REFS_PATH,
        "\t\tquery service=git-receive-pack",
        "\t}",
        "\thandle @git_push_app {",
        "\t\timport app_proxy_app_host",
        "\t}",
        "\thandle @git_push_refs {",
        "\t\timport app_proxy_app_host",
        "\t}",
        "\thandle {",
        "\t\treverse_proxy 127.0.0.1:%d {" % gateway_port,
        "\t\t\tflush_interval -1",
        "\t\t}",
        "\t}",
        "}",
    ]
    return "\n".join(out) + "\n"


def certsync_config(fleet, node, paths):
    """The same shape on every core node: no issuer role (certsync.py)."""
    app = fleet.get("app") or {}
    port = int(app.get("certsync_port", 8793))
    replica = is_app_node(node)
    allowed = sorted(n["wg_ip"] for n in fleet["nodes"]
                     if n["name"] != node["name"])
    if not replica:
        allowed += sorted(n["wg_ip"] for n in fleet.get("app_nodes") or [])
    peers = {n["name"]: "http://%s:%d" % (n["wg_ip"], port)
             for n in fleet["nodes"]}
    if not replica:
        peers[node["name"]] = "http://%s:%d" % (node["wg_ip"], port)
    config = {
        "node": node["name"],
        "names": tls_hosts(fleet),
        "secret_credential": CERTSYNC_SECRET_NAME,
        "listen": "%s:%d" % (node["wg_ip"], port),
        "allowed_peers": [] if replica else allowed,
        "peers": peers,
        "challenge_listen": "127.0.0.1:%d" % int(
            app.get("acme_forward_port", 8794)),
        "challenge_peers": {n["name"]: "%s:80" % n["wg_ip"]
                            for n in fleet["nodes"]},
        "caddy_storage": "/var/lib/caddy/.local/share/caddy/certificates",
        "export_dir": paths["var"] + "/certsync/export",
        "export_group": "fmnext-certsync",
        "tls_dir": paths["tls"],
        "tls_owner": "caddy",
        "include_dir": paths["etc"] + "/caddy",
        "renew_days": 30,
        "reload_command": ["systemctl", "reload", "caddy"],
        # Pulled pairs must chain to a system CA, cover the name and be
        # valid now and for at most max_days more (certsync.openssl_validate).
        "ca_file": None,
        "max_days": 100,
    }
    if replica:
        config["issue"] = False
    elif autoscale_enabled(fleet):
        config["replica_secret_credential"] = AUTOSCALE_CERTSYNC_CREDENTIAL
        config["replica_peers_file"] = FLEET_PEERS_REPLICAS
    return json.dumps(config, indent=2, sort_keys=True) + "\n"


def replica_settings(fleet):
    return ("FORKMESH_CLUSTER_ROLE=replica\nFORKMESH_DNS_RECONCILER=off\n"
            "FORKMESH_DNS_TARGETS=\nFORKMESH_DNS_NAMES=%s"
            % ",".join(shared_hosts(fleet)))


def dns_settings(fleet):
    """app.env lines for the node DNS reconciler (dns_reconciler.py)."""
    mode = (fleet.get("dns") or {}).get("reconciler", "dry")
    targets = ",".join("%s=%s" % (n["name"], n["public_ipv4"])
                       for n in fleet["nodes"])
    return ("FORKMESH_DNS_RECONCILER=%s\nFORKMESH_DNS_TARGETS=%s\n"
            "FORKMESH_DNS_NAMES=%s" % (mode, targets,
                                      ",".join(shared_hosts(fleet))))


FLAGSHIP_REPOSITORY = "forkmesh/forkmesh"


def core_upstreams(fleet, node_name):
    public = "https://%s/%s" % (served_app_host(fleet), FLAGSHIP_REPOSITORY)
    peers = ["https://%s.%s/%s.git" % (n["name"], fleet["domain"],
                                       FLAGSHIP_REPOSITORY)
             for n in fleet.get("nodes") or []
             if n["name"] != node_name and not is_app_node(n)]
    return {FLAGSHIP_REPOSITORY: [public] + peers}, [public]


def daemon_overrides(fleet, role, burst=False, node_name=None):
    """Merged over the daemon's --init config (docs/RUNNERS.md)."""
    if role == "core":
        overrides = {
            "role": "core",
            "catalogUrl": "https://%s/api/repositories" % served_app_host(fleet),
            "disableCloudflared": True,
            "disableAgentJobs": True, "enableActions": False,
            "disableAgentLogin": True,
        }
        if node_name is not None:
            upstreams, event_driven = core_upstreams(fleet, node_name)
            overrides["upstreams"] = upstreams
            overrides["eventDrivenUpstreams"] = event_driven
        hosted = fleet.get("hosted") or {}
        if hosted.get("enabled", True):
            # ForkMesh-hosted repositories (server/hosted.go,
            # docs/HOSTED_REPOS.md): every core keeps a replica under root,
            # inside the daemon's own state directory.
            overrides["hosted"] = {
                "enabled": True,
                "root": hosted.get("root", HOSTED_ROOT),
                "maxRepositoryBytes": int(hosted.get(
                    "max_repository_bytes", HOSTED_MAX_BYTES)),
            }
    else:
        # A runner keeps no repositories (upstreams must be empty); the role
        # forces catalog, tunnel and merge executor off. One job at a time on
        # the 1 GB always-on runner, in its own systemd scope.
        overrides = {
            "role": "runner",
            "upstreams": {},
            "eventDrivenUpstreams": [],
            "disableCloudflared": True,
            "enableActions": True,
            "runner": {
                "appUrl": "https://%s" % served_app_host(fleet),
                "maxConcurrentRuns": 1,
                "jobMemoryMax": "600M",
                "jobMemorySwapMax": RUNNER_JOB_SWAP_MAX,
                "jobCPUQuota": "90%",
                # Traversable by the job user (0711 root), unlike the
                # daemon's own 0700 state dir. jobUid (the uid of
                # forkmesh-job) is added on the node by next_fleet.py.
                "workRoot": RUNNER_WORK_ROOT,
            },
        }
        if burst:
            overrides["runner"]["burst"] = True
    return json.dumps(overrides, indent=2, sort_keys=True) + "\n"


# --------------------------------------------------------------------------
# node renderers


def base_values(fleet, node_name, paths):
    rq = fleet.get("rqlite") or {}
    app = fleet.get("app") or {}
    return {
        "NODE": node_name,
        "OPT": paths["opt"],
        "CURRENT": paths["current"],
        "SSH_HELPER_SOCKET": SSH_HELPER_SOCKET,
        "VAR": paths["var"],
        "ETC": paths["etc"],
        "CREDS": paths["creds"],
        "TLS": paths["tls"],
        "HTTP_PORT": int(rq.get("http_port", 4001)),
        "RAFT_PORT": int(rq.get("raft_port", 4002)),
        "RQLITE_USER": rq.get("user", "fmnext"),
        "GOMEMLIMIT": rq.get("gomemlimit", "150MiB"),
        "APP_PORT": int(app.get("port", 8080)),
        "MESH_PORT": int(app.get("mesh_port", 8792)),
        "APP_HOST": fleet["app_host"],
        "APP_HOSTS": " ".join(dict.fromkeys([served_app_host(fleet),
                                             fleet["app_host"]])),
        "DATA_DIR": paths["var"] + "/rqlite",
    }


RETIRED_CERT_UNITS = [
    "fmnext-cert-export.service", "fmnext-cert-export.timer",
    "fmnext-cert-pull.service", "fmnext-cert-pull.timer",
    "cert-export.service", "cert-export.timer",
    "cert-pull.service", "cert-pull.timer",
]


def rqlite_voter(node):
    return node.get("rqlite_voter", True) is not False


def render_core(fleet, node_name, replacement=False):
    node = node_by_name(fleet, node_name)
    replica = is_app_node(node)
    paths = fleet_paths(fleet)
    app = fleet.get("app") or {}
    rq = fleet.get("rqlite") or {}
    wg = fleet.get("wireguard") or {}
    wg_port = int(wg.get("port", 51820))
    values = base_values(fleet, node_name, paths)
    raft_port = values["RAFT_PORT"]
    voters = [n for n in fleet["nodes"] if rqlite_voter(n)]
    peers = [n for n in voters if n["name"] != node_name]
    if replica or not rqlite_voter(node):
        join = ",".join("%s:%d" % (p["wg_ip"], raft_port) for p in peers)
        join_flags = "  -raft-non-voter \\\n  -join %s \\\n" % join
    elif replacement:
        join = ",".join("%s:%d" % (p["wg_ip"], raft_port) for p in peers)
        join_flags = "  -join %s \\\n" % join
    else:
        join = ",".join("%s:%d" % (n["wg_ip"], raft_port) for n in voters)
        join_flags = "  -bootstrap-expect %d \\\n  -join %s \\\n" % (
            len(voters), join)
    if rq.get("fk"):
        join_flags += "  -fk \\\n"
    for key, flag in (("snap_entries", "-raft-snap"), ("snap_wal_size", "-raft-snap-wal-size")):
        if rq.get(key) is not None:
            join_flags += "  %s %d \\\n" % (flag, int(rq[key]))
    read_mode = app.get("read_mode", "http")
    if read_mode not in ("http", "local"):
        raise RenderError("app.read_mode must be http or local")
    secrets = app_credential_names(fleet)
    vault = vault_enabled(fleet)
    credentials = "\n".join(
        "LoadCredentialEncrypted=%s:%s/%s.cred" % (name, paths["creds"], name)
        for name in secrets)
    values.update({
        "WG_IP": node["wg_ip"],
        "RQLITE_ID": node["rqlite_id"],
        "JOIN_FLAGS": join_flags,
        "APP_CREDENTIALS": credentials,
        "READ_MODE": read_mode,
        "D1_LOG": "1" if d1_log(fleet) else "0",
        "RQLITE_NODE_MAP": ",".join("%s=%s" % (n["rqlite_id"], n["name"])
                                    for n in fleet["nodes"] + (
                                        [node] if replica else [])),
        "ROLE": "core",
        "DAEMON_AFTER": "forkmesh-app.service caddy.service",
        "DAEMON_USER": "forkmesh-node",
        "DAEMON_CAPS": "",
        "DAEMON_AMBIENT": "",
        "DAEMON_RW": "/var/lib/forkmesh",
        "PROTECT_HOME": "true",
        "DAEMON_LIMITS": "MemoryHigh=96M\nMemoryMax=128M\nOOMScoreAdjust=200",
        "DAEMON_TMP": "PrivateTmp=true",
        "DNS_SETTINGS": replica_settings(fleet) if replica
        else dns_settings(fleet),
        "IDS_SETTINGS": ids_env_settings(fleet),
        "PUBLIC_HOSTS": ",".join(public_hosts(fleet)),
        "PRODUCTION_SETTINGS": production_settings(fleet) + core_role_settings(fleet),
        "TURN_SETTINGS": turn_app_settings(fleet),
        "MESH_PEERS": mesh_peers(fleet),
        **request_rules_paths(paths),
        "CERTSYNC_SECRET": CERTSYNC_SECRET_NAME,
        "VAULT": "1" if vault else "0",
        "UNSEAL_SOCKET": UNSEAL_SOCKET,
        "VAULT_SETTINGS": (
            "# Vault on (docs/VAULT.md): start sealed; every other secret is\n"
            "# unsealed from host_secrets through the socket below%s.\n"
            "Environment=FORKMESH_VAULT=1\n"
            "Environment=FORKMESH_VAULT_SOCKET=%s\n"
            "Environment=FORKMESH_VAULT_PEER_UNSEAL=%s\n"
            "Environment=FORKMESH_MESH_PEERS=%s" % (
                " or with\n# DATA_KEY from an unsealed core peer over the "
                "WireGuard mesh" if vault_peer_unseal(fleet) else "",
                UNSEAL_SOCKET, "1" if vault_peer_unseal(fleet) else "0",
                mesh_peers(fleet)))
        if vault else "# Vault off: every secret is a credential above.",
    })
    files = {}

    def add(path, content, mode=0o644):
        files[path] = (content, mode)

    unit = "/etc/systemd/system/"
    add(unit + "rqlite.service", template("rqlite.service", values))
    add(unit + "forkmesh-app.service", template("forkmesh-app.service", values))
    add(paths["etc"] + "/app.env", template("app.env", values))
    if not replica:
        add(unit + "forkmesh-mirror-node.service",
            template("forkmesh-mirror-node.service", values))
        add(DAEMON_OVERRIDES_PATH, daemon_overrides(fleet, "core",
                                                      node_name=node_name))
    add(unit + "caddy.service.d/forkmesh-next.conf",
        template("caddy-dropin.conf", values))
    add("/etc/caddy/Caddyfile", caddyfile(fleet, node, paths,
                                          per_node=not replica))
    add(paths["etc"] + "/caddy/app-routes.caddy",
        caddy_routes.generate("app", REPO, root=paths["current"] + "/app/dist"))
    add(paths["etc"] + "/caddy/world-routes.caddy",
        caddy_routes.generate("world", REPO, root=paths["current"] + "/world/dist",
                              app_root=paths["current"] + "/app/dist"))
    add("/etc/sysctl.d/90-forkmesh-next.conf", template("sysctl.conf", values))
    add("/etc/systemd/zram-generator.conf", template("zram-generator.conf", values))
    add(unit + "swapfile.swap", template("swapfile.swap", values))
    add("/etc/default/earlyoom", template("earlyoom", values))
    add("/etc/systemd/journald.conf.d/60-forkmesh-next.conf",
        template("journald.conf", values))
    mesh_ports = sorted({values["HTTP_PORT"], raft_port, values["MESH_PORT"],
                         int(app.get("certsync_port", 8793)), 80})
    # 80 on wg0: the HTTP-01 forwarder of every peer asks this Caddy.
    peers_agent = autoscale_enabled(fleet) and not replica
    public_tcp, public_udp = [22, 80, 443], [443, wg_port]
    if turn_enabled(fleet):
        turn_tcp, turn_udp, relay = turn_ports(fleet)
        public_tcp, public_udp = public_tcp + turn_tcp, public_udp + turn_udp + [relay]
    add("/etc/nftables.conf", nftables_conf(
        fleet, node_name, mesh_ports, public_tcp, public_udp,
        output_rules=core_output_rules(fleet) + (
            ids_egress_log_rules() if ids_enabled(fleet) else []),
        autoscaled_set=peers_agent), 0o755)
    add("/etc/wireguard/wg0.conf",
        wireguard_conf(fleet, node, wg_port, int(wg.get("prefix", 24))), 0o600)
    add(paths["etc"] + "/hosts.fragment", template("hosts.fragment", values))
    add(paths["opt"] + "/bin/fmnext-certsync",
        (HERE / "certsync.py").read_text("utf-8"), 0o755)
    add(paths["etc"] + "/certsync.json", certsync_config(fleet, node, paths))
    add(paths["opt"] + "/bin/fmnext-request-rules",
        (HERE / "request_rules.py").read_text("utf-8"), 0o755)
    for name in ("fmnext-request-rules.path", "fmnext-request-rules.service"):
        add(unit + name, template(name, values))
    if peers_agent:
        add(paths["opt"] + "/bin/fmnext-fleet-peers",
            (HERE / "fleet_peers.py").read_text("utf-8"), 0o755)
        add(paths["etc"] + "/fleet-peers.json", json.dumps({
            "rqlite_url": "http://127.0.0.1:%d" % values["HTTP_PORT"],
            "curlrc": paths["etc"] + "/rqlite.curlrc",
            "wg_port": wg_port, "replicas_file": FLEET_PEERS_REPLICAS},
            indent=2, sort_keys=True) + "\n")
        add(unit + "fmnext-fleet-peers.service", template(
            "fmnext-fleet-peers.service", values))
    add(unit + "fmnext-ssh-helper.socket",
        template("fmnext-ssh-helper.socket", values))
    add(unit + "fmnext-ssh-helper.service",
        template("fmnext-ssh-helper.service", values))
    add("/usr/local/sbin/fmnext-app-ops", template("fmnext-app-ops", values), 0o755)
    add("/usr/local/sbin/fmnext-unseal", template("fmnext-unseal", values), 0o755)
    units = ["rqlite.service", "forkmesh-app.service", "caddy.service",
             "forkmesh-mirror-node.service", "swapfile.swap", "nftables.service",
             "wg-quick@wg0.service", "earlyoom.service",
             "fmnext-ssh-helper.socket"]
    if replica:
        units.remove("forkmesh-mirror-node.service")
    credentials_by_unit = {
        "rqlite.service": ["rqlite-auth.json"],
        "forkmesh-app.service": secrets,
    }
    # Leaderless certificates: the same units on every core node.
    for name in ("fmnext-certsync.service", "fmnext-cert-sync.service",
                 "fmnext-cert-sync.timer"):
        text = template(name, values)
        if peers_agent and name == "fmnext-certsync.service":
            line = "LoadCredentialEncrypted=%s:%s/%s.cred" % (
                CERTSYNC_SECRET_NAME, paths["creds"], CERTSYNC_SECRET_NAME)
            text = text.replace(line, line + "\nLoadCredentialEncrypted=%s:%s/%s.cred"
                                % (AUTOSCALE_CERTSYNC_CREDENTIAL, paths["creds"],
                                   AUTOSCALE_CERTSYNC_CREDENTIAL), 1)
        add(unit + name, text)
    units += ["fmnext-certsync.service", "fmnext-cert-sync.timer",
              "fmnext-request-rules.path", "fmnext-request-rules.service"]
    credentials_by_unit["fmnext-certsync.service"] = [CERTSYNC_SECRET_NAME]
    credentials_by_unit["fmnext-cert-sync.service"] = [CERTSYNC_SECRET_NAME]
    if peers_agent:
        units.append("fmnext-fleet-peers.service")
        credentials_by_unit["fmnext-certsync.service"].append(
            AUTOSCALE_CERTSYNC_CREDENTIAL)
    turn = add_turn_files(fleet, node, paths, values, add, units,
                          credentials_by_unit)
    add_ids_files(fleet, node_name, "core", paths, values, add, units,
                  credentials_by_unit)
    if not replica:
        add_self_deploy_files(fleet, node_name, paths, values, add, units,
                              credentials_by_unit)
    logread = add_logread_files(fleet, add)
    manifest = {
        "role": "core",
        "turn": turn,
        "logread": logread,
        "node": node_name,
        "public_ipv4": node["public_ipv4"],
        "public_ipv6": node.get("public_ipv6", ""),
        "wg_ip": node["wg_ip"],
        # The fixed-issuer certificate units of the first rollout, removed by
        # exact name on every node by next_fleet.py's files step.
        "retired_units": RETIRED_CERT_UNITS,
        "replacement": bool(replacement),
        "shared_hosts": shared_hosts(fleet),
        "tls_hosts": tls_hosts(fleet),
        "served_hosts": served_hosts(fleet),
        "units": units,
        "credentials": credentials_by_unit,
        "credentials_dir": paths["creds"],
        "vault": vault,
        "vault_peer_unseal": vault_peer_unseal(fleet),
        "ids": ids_enabled(fleet),
        "files": {path: oct(mode) for path, (_, mode) in sorted(files.items())},
        "release_routes": {
            paths["etc"] + "/caddy/%s-routes.caddy" % site:
            paths["current"] + "/caddy/%s-routes.caddy" % site
            for site in ("app", "world")},
    }
    if replica:
        manifest["replica"] = True
    return files, manifest


def render_runner(fleet, runner_name):
    runner = runner_by_name(fleet, runner_name)
    paths = fleet_paths(fleet)
    values = base_values(fleet, runner_name, paths)
    values.update({
        "ROLE": "runner",
        "DAEMON_AFTER": "",
        # Root, so jobs run as the separate, unprivileged job user through
        # the system manager (systemd-run --uid, runner.jobUid). The daemon
        # itself never runs git in a job's tree (server/runner.go).
        "DAEMON_USER": "root",
        # chown the job's dirs to the job user, clean up what it wrote, and
        # signal its processes; nothing else.
        "DAEMON_CAPS": "CAP_CHOWN CAP_DAC_OVERRIDE CAP_FOWNER CAP_KILL CAP_SETUID CAP_SETGID",
        "DAEMON_AMBIENT": "CAP_SETUID CAP_SETGID",
        "DAEMON_RW": "/var/lib/forkmesh %s %s" % (
            RUNNER_WORK_ROOT.rsplit("/", 1)[0], JOB_HOME),
        "PROTECT_HOME": "true",
        "DAEMON_TMP": "PrivateTmp=false\nBindPaths=%s:/tmp %s:/var/tmp" % (
            RUNNER_JOB_TMP, RUNNER_JOB_TMP),
        "DAEMON_LIMITS": (
            "# Jobs run in their own transient units of the system manager\n"
            "# (MemoryMax=jobMemoryMax), so these limits cover the daemon only.\n"
            "MemoryHigh=150M\nMemoryMax=250M"),
    })
    files = {}

    def add(path, content, mode=0o644):
        files[path] = (content, mode)

    unit = "/etc/systemd/system/"
    add(unit + "forkmesh-mirror-node.service",
        template("forkmesh-mirror-node.service", values))
    add(DAEMON_OVERRIDES_PATH, daemon_overrides(
        fleet, "runner", burst=bool(runner.get("burst"))))
    add("/etc/sysctl.d/90-forkmesh-next.conf", template("sysctl.conf", values))
    add("/etc/systemd/zram-generator.conf", template("zram-generator.conf", values))
    add(unit + "swapfile.swap", template("swapfile.swap", values))
    add("/etc/default/earlyoom", template("earlyoom", values))
    add("/etc/systemd/journald.conf.d/60-forkmesh-next.conf",
        template("journald.conf", values))
    add("/etc/nftables.conf", nftables_conf(
        fleet, runner_name, [], [22], [], output_rules=runner_output_rules()),
        0o755)
    add("/etc/forkmesh-next/runner.md",
        (RUNNER_TEMPLATES / "runner.md").read_text("utf-8"))
    units = ["forkmesh-mirror-node.service", "swapfile.swap",
             "nftables.service", "earlyoom.service"]
    credentials_by_unit = {}
    logread = add_logread_files(fleet, add)
    add_ids_files(fleet, runner_name, "runner", paths, values, add, units,
                  credentials_by_unit)
    manifest = {
        "role": "runner",
        "burst": bool(runner.get("burst")),
        "logread": logread,
        "node": runner_name,
        "public_ipv4": runner["public_ipv4"],
        "units": units,
        "credentials": credentials_by_unit,
        "ids": ids_enabled(fleet),
        "credentials_dir": paths["creds"],
        "job_user": JOB_USER,
        "job_home": JOB_HOME,
        "work_root": RUNNER_WORK_ROOT,
        "job_tmp": RUNNER_JOB_TMP,
        "files": {path: oct(mode) for path, (_, mode) in sorted(files.items())},
    }
    return files, manifest


def write_tree(files, manifest, out: Path):
    out = Path(out)
    for path, (content, mode) in files.items():
        target = out / path.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        os.chmod(target, mode)
    (out / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", "utf-8")
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet", default=str(REPO / "deploy" / "fleet.toml"))
    parser.add_argument("--state", help="fleet-state.json (wg public keys)")
    parser.add_argument("--instances",
                        help="fleet-instances.json (public addresses by label)")
    who = parser.add_mutually_exclusive_group(required=True)
    who.add_argument("--node")
    who.add_argument("--runner")
    parser.add_argument("--replacement", action="store_true",
                        help="join an existing cluster instead of bootstrapping")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    try:
        fleet = load_fleet(args.fleet, args.state,
                           instances_path=args.instances)
        if args.node:
            files, manifest = render_core(fleet, args.node, args.replacement)
        else:
            files, manifest = render_runner(fleet, args.runner)
        write_tree(files, manifest, Path(args.out))
    except (RenderError, KeyError, OSError, ValueError) as error:
        print("render: %s" % error, file=sys.stderr)
        return 1
    print("render: %d files for %s in %s" % (
        len(files), args.node or args.runner, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
