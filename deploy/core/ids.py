#!/usr/bin/python3 -I
"""fmnext-ids: host intrusion detection for ForkMesh Next nodes (docs/IDS.md).

Runs as root (``fmnext-ids.service``) on the three core nodes and the
runner; Python stdlib only, plus ``cryptography`` for the rare Ed25519
signature (Debian's python3-cryptography). About 20-30 MB RSS; the scans
stat files and read /proc, and hash only what changed.

Detectors (each yields an event with a severity: info | alert | lockdown):

* **ssh**: journald lines of sshd. A login accepted with a key other than the
  fleet key (``ssh.fleet_fingerprints``) or without a key is a lockdown; a
  login from outside ``ssh.allow`` (when set) is an alert; an interactive
  session (a shell, or anything on a pty) is an alert unless it comes from an
  allowlisted address. The fleet tools' sessions are commands without a pty
  (``next_common.Ssh``: BatchMode, no -t), sftp, or a -N tunnel: info.
  sshd must log at ``LogLevel VERBOSE`` (next_fleet's base step sets it) so
  the "Starting session" lines exist.
* **integrity**: a baseline of sha256 + mode + owner of the code, units and
  config (``integrity.lockdown`` / ``integrity.alert`` roots, the current
  release tree, every account's authorized_keys). Changes are accepted only
  under an unexpired, HMAC-signed *expected change* manifest pushed by the
  operator tools (``expect``); anything else is an incident. /etc/passwd,
  group, shadow and gshadow are diffed: a new uid 0, a new login shell or a
  privileged group member is a lockdown.
* **processes** (/proc every 5 s): a service user's process outside its own
  unit, a shell or unexpected executable under a service user, an executable
  deleted from or running out of /tmp, /dev/shm, /var/tmp or memfd, a ptrace
  of a protected process (the app holds DATA_KEY), and on the runner a job
  user's process outside a job scope.
* **listeners**: a TCP listener, or a UDP socket below the ephemeral range,
  on a port outside the expected set.
* **egress**: kernel log lines of the nftables rule that logs new outbound
  connections of the service users to unusual ports (deploy/core/render.py).
* **kernel**: a module loaded after start-up, a tainting module; a reboot
  outside a deploy window (info).
* **peers** (core): every node writes its IDS heartbeat to rqlite; a peer
  whose app heart-beats while its IDS stays silent is voted into quarantine
  (a strict majority of the core nodes, each vote signed).

Response (the ``Responder`` state machine; normal -> lockdown, sticky):

* every incident is logged to the journal and recorded in rqlite
  ``security_incidents`` (local rqlite, else a peer's over WireGuard; the
  runner posts it, signed, to ``/api/runners/security-incident``); unsealed
  core apps page the admins (Pings/APNs + Mailtrap, forkmesh_host.ids_alerts);
  optional direct mail (local Postfix; Mailtrap in a lockdown) and a
  signed webhook to the desktop;
* **lockdown** on a core: ``seal-restart`` over the unseal socket (DATA_KEY
  leaves memory and the process), the marker file (units carry
  ``ConditionPathExists=!`` on it, so nothing restarts, not even after a
  reboot), a signed ``self`` row in ``cluster_quarantine``, the nftables
  lockdown table (inbound only SSH, from ``ssh.allow`` when set; outbound
  only root), stop the daemon/Git gateway, the app, Caddy and certsync, and
  stop rqlite only when the healthy peers keep quorum without it. On the
  runner: report (which revokes the runner), kill every job scope, stop the
  daemon, block egress.
* peers enforce a quarantine they can verify: they drop the node's
  WireGuard traffic and, when quorum-safe, remove it as an rqlite voter.

Only ``tools/next_ids.py release nN --execute`` (an operator-signed release
row, then a desktop unseal) ends a lockdown.

CLI (root): ``run`` | ``keygen`` | ``expect`` (manifest on stdin) |
``status`` | ``test-fire`` | ``incident-status ID`` | ``release`` (row on
stdin) | ``rescan``. Everything but ``run`` and ``keygen`` talks to the
running daemon over ``/run/fmnext-ids/control.sock`` (0600 root).
"""

from __future__ import annotations

import argparse
import base64
import email.message
import email.utils
import fnmatch
import hashlib
import hmac
import ipaddress
import json
import os
import pwd
import re
import select
import smtplib
import socket
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
try:
    import ids_records  # installed next to this file
except ImportError:  # pragma: no cover - source tree layout (tests)
    sys.path.insert(0, os.path.join(HERE, "..", "..", "app", "host",
                                    "forkmesh_host"))
    import ids_records

CONFIG_PATH = "/etc/forkmesh-next/ids.json"
SEVERITY_RANK = {"info": 0, "alert": 1, "lockdown": 2}
DEDUPE_SECONDS = 600
MAX_EXPECT_WINDOW_MS = 60 * 60 * 1000
EXPECT_SKEW_MS = 5 * 60 * 1000
EXPECT_ALLOW = ("users", "modules", "listeners", "reboot", "ptrace",
                "processes")
SHELLS = ("sh", "bash", "dash", "zsh", "ksh", "csh", "tcsh", "fish", "ash",
          "busybox", "nc", "ncat", "netcat", "socat", "telnet", "perl",
          "php", "ruby", "lua", "node", "python", "python2", "python3")
LOGIN_SHELL_EXCLUDE = ("nologin", "false", "sync", "halt", "shutdown")
PRIVILEGED_GROUPS = ("root", "sudo", "wheel", "adm", "shadow", "disk",
                     "admin", "staff", "systemd-journal", "docker", "lxd")
SYSTEMD_EXECUTORS = ("/usr/lib/systemd/systemd-executor",
                     "/lib/systemd/systemd-executor")
SUSPICIOUS_EXE_DIRS = ("/tmp/", "/dev/shm/", "/var/tmp/", "/run/user/",
                       "memfd:")
GIT_HOOKS = ("pre-receive", "update", "post-receive", "post-update",
             "reference-transaction", "proc-receive", "push-to-checkout")
FORKMESH_HOOK_MARKERS = (b"# Managed-By: ForkMesh", b"FORKMESH_PUSH_INPUT")
CONTEXT_ENV = ("SSH_CONNECTION", "SSH_CLIENT", "SSH_ORIGINAL_COMMAND",
               "REMOTE_ADDR", "REMOTE_PORT", "HTTP_X_FORWARDED_FOR",
               "HTTP_X_REAL_IP", "HTTP_USER_AGENT", "GIT_PROTOCOL",
               "GIT_DIR", "GIT_QUARANTINE_PATH", "SUDO_USER", "LOGNAME")
CONTEXT_PARENTS = 6
CONTEXT_CHILDREN = 5
CONTEXT_CONNECTIONS = 10
CONTEXT_BYTES = 3400
TCP_STATES = {"01": "established", "02": "syn-sent", "03": "syn-recv",
              "04": "fin-wait1", "05": "fin-wait2", "06": "time-wait",
              "08": "close-wait", "0A": "listen"}
MAIL_URL = "https://send.api.mailtrap.io/api/send"


def now_ms():
    return time.time_ns() // 1_000_000


def log(message, **fields):
    record = {"ids": message}
    record.update(fields)
    print(json.dumps(record, sort_keys=True, default=str), flush=True)


# --------------------------------------------------------------------------
# configuration and credentials


def load_config(path=CONFIG_PATH):
    with open(path, "r", encoding="utf-8") as handle:
        cfg = json.load(handle)
    cfg.setdefault("role", "core")
    cfg.setdefault("state_dir", "/var/lib/fmnext-ids")
    cfg.setdefault("run_dir", "/run/fmnext-ids")
    cfg.setdefault("severity", {})
    cfg.setdefault("ssh", {})
    cfg.setdefault("integrity", {})
    cfg.setdefault("processes", {})
    cfg.setdefault("listeners", {})
    cfg.setdefault("lockdown", {})
    cfg.setdefault("heartbeat", {})
    cfg.setdefault("modules", {})
    if not ids_records.NODE_RE.fullmatch(str(cfg.get("node") or "")):
        raise SystemExit("ids.json: node must look like n1 or r1")
    return cfg


def read_credential(name, environ=None):
    """A systemd credential's value, or '' (never logged)."""
    directory = (environ or os.environ).get("CREDENTIALS_DIRECTORY", "")
    if not directory or not name or "/" in name:
        return ""
    try:
        with open(os.path.join(directory, name), "r",
                  encoding="utf-8") as handle:
            return handle.read().strip()
    except OSError:
        return ""


class Event(dict):
    """detector, severity, summary, detail, key (dedupe)."""

    def __init__(self, detector, severity, summary, detail=None, key=None):
        super().__init__(detector=detector, severity=severity,
                         summary=summary[:300], detail=detail or {},
                         key=key or "%s:%s" % (detector, summary[:120]))


def _in_cidrs(ip, cidrs):
    try:
        address = ipaddress.ip_address(ip)
    except ValueError:
        return False
    for cidr in cidrs or ():
        try:
            if address in ipaddress.ip_network(cidr, strict=False):
                return True
        except ValueError:
            continue
    return False


# --------------------------------------------------------------------------
# detector: SSH logins (journald)

ACCEPTED_RE = re.compile(
    r"^Accepted (?P<method>\S+) for (?P<user>\S+) from (?P<ip>\S+) port "
    r"(?P<port>\d+)(?: [^:\s]+)?(?::\s+(?P<keytype>\S+)\s+(?P<fp>\S+))?")
SESSION_RE = re.compile(
    r"^Starting session: (?P<kind>.+?) for (?P<user>\S+) from (?P<ip>\S+) "
    r"port (?P<port>\d+)(?: id \d+)?")
SSHD_IDENTIFIERS = ("sshd", "sshd-session", "sshd-auth")


class SshMonitor:
    def __init__(self, cfg, clock=time.monotonic):
        ssh = cfg.get("ssh") or {}
        self.fleet = set(ssh.get("fleet_fingerprints") or ())
        self.allow = list(ssh.get("allow") or ())
        self.foreign = ssh.get("foreign_ip_severity", "alert")
        self.clock = clock
        self.pending = {}

    def feed(self, entry):
        if entry.get("SYSLOG_IDENTIFIER") not in SSHD_IDENTIFIERS:
            return []
        text = str(entry.get("MESSAGE") or "")
        match = ACCEPTED_RE.match(text)
        if match:
            return self._accepted(match.groupdict())
        match = SESSION_RE.match(text)
        if match:
            return self._session(match.groupdict())
        return []

    def _accepted(self, g):
        events = []
        detail = {"user": g["user"], "ip": g["ip"], "port": int(g["port"]),
                  "method": g["method"], "key": g.get("fp") or ""}
        allowed_ip = bool(self.allow) and _in_cidrs(g["ip"], self.allow)
        if g["method"] != "publickey":
            events.append(Event("ssh_non_key_login", "lockdown",
                                "SSH login without a key (%s) for %s from %s"
                                % (g["method"], g["user"], g["ip"]), detail))
        elif self.fleet and g.get("fp") not in self.fleet:
            events.append(Event("ssh_unknown_key", "lockdown",
                                "SSH login with a key that is not the fleet "
                                "key, %s from %s" % (g["user"], g["ip"]),
                                detail))
        if g["user"] != "root":
            events.append(Event("ssh_login_user", "alert",
                                "SSH login as %s (the fleet uses root) from %s"
                                % (g["user"], g["ip"]), detail))
        if self.allow and not allowed_ip:
            events.append(Event("ssh_foreign_ip", self.foreign,
                                "SSH login from %s, outside the allowlist"
                                % g["ip"], detail))
        self.pending[(g["ip"], g["port"])] = dict(detail, t=self.clock(),
                                                  allowed=allowed_ip,
                                                  sessions=0)
        return events

    def _session(self, g):
        login = self.pending.get((g["ip"], g["port"]))
        if login is None:
            return []
        login["sessions"] += 1
        kind = g["kind"]
        interactive = kind.startswith("shell") or " on " in kind
        detail = {k: v for k, v in login.items() if k not in ("t",)}
        detail["session"] = kind
        if login["sessions"] > 1 and not interactive:
            return []
        if interactive:
            severity = "info" if login["allowed"] else "alert"
            return [Event("ssh_interactive_login", severity,
                          "Interactive SSH session (%s) for %s from %s"
                          % (kind, g["user"], g["ip"]), detail,
                          key="ssh_interactive:%s:%s" % (g["ip"], g["port"]))]
        return [Event("ssh_tool_session", "info",
                      "SSH %s for %s from %s" % (kind, g["user"], g["ip"]),
                      detail, key="ssh_tool:%s:%s" % (g["ip"], g["port"]))]

    def tick(self):
        """Drop logins older than 30 s (a -N tunnel never starts a
        session: nothing to report beyond the accept itself)."""
        now = self.clock()
        for key in [k for k, v in self.pending.items() if now - v["t"] > 30]:
            self.pending.pop(key, None)
        return []


# --------------------------------------------------------------------------
# detector: kernel lines, account tools (journald)

EGRESS_PREFIX = "fmnext-ids-egress: "
_KV_RE = re.compile(r"\b(DST|DPT|PROTO|UID|SRC)=(\S+)")
ACCOUNT_TOOLS = ("useradd", "usermod", "userdel", "groupadd", "groupmod",
                 "groupdel", "gpasswd", "passwd", "chpasswd", "chage",
                 "sudo", "su")


def uid_name(uid):
    try:
        return pwd.getpwuid(int(uid)).pw_name
    except (KeyError, ValueError, TypeError):
        return str(uid)


def parse_kernel(entry):
    if str(entry.get("_TRANSPORT") or "") != "kernel":
        return []
    text = str(entry.get("MESSAGE") or "")
    if EGRESS_PREFIX in text:
        fields = dict(_KV_RE.findall(text))
        user = uid_name(fields.get("UID")) if fields.get("UID") else "?"
        detail = {"user": user, "dst": fields.get("DST", ""),
                  "port": fields.get("DPT", ""),
                  "proto": fields.get("PROTO", "")}
        return [Event("egress_unexpected", "alert",
                      "%s opened %s to %s port %s" % (
                          user, detail["proto"], detail["dst"],
                          detail["port"]), detail,
                      key="egress:%s:%s:%s" % (user, detail["dst"],
                                               detail["port"]))]
    if "module verification failed" in text or "taints kernel" in text:
        return [Event("kernel_module_taint", "alert",
                      "Kernel: %s" % text[:200], {"message": text[:300]})]
    return []


# su/sudo lines whose invoker is root: pam "by root(uid=0)", su's
# "(to USER) root on TTY", sudo's "root : TTY=... ; COMMAND=...".
_ROOT_INVOKED_RE = re.compile(
    r"\bby root\(uid=0\)|^\(to \S+\) root on |^\s*root\s*:")


def parse_account_tool(entry):
    ident = str(entry.get("SYSLOG_IDENTIFIER") or "")
    if ident not in ACCOUNT_TOOLS:
        return []
    text = str(entry.get("MESSAGE") or "")[:300]
    if ident in ("sudo", "su"):
        # Nothing on these nodes needs sudo or su to GAIN privilege (root
        # logs in by key). Root dropping to a service account (the operator
        # tools' `su -s /bin/sh forkmesh-node -c ...`) is not an escalation,
        # and a "session closed" line only mirrors an open already judged.
        if _ROOT_INVOKED_RE.search(text) or "session closed for user" in text:
            return [Event("privilege_tool_root", "info",
                          "%s: %s" % (ident, text),
                          {"tool": ident, "message": text})]
        return [Event("privilege_tool", "alert", "%s: %s" % (ident, text),
                      {"tool": ident, "message": text},
                      key="privtool:%s:%s" % (ident, text[:80]))]
    return [Event("account_tool", "info", "%s: %s" % (ident, text),
                  {"tool": ident, "message": text})]


# --------------------------------------------------------------------------
# expected changes (signed by the operator tools)


def manifest_mac(key, manifest):
    body = {k: v for k, v in manifest.items() if k != "mac"}
    text = json.dumps(body, sort_keys=True, separators=(",", ":"))
    return hmac.new(key.encode("utf-8"), text.encode("utf-8"),
                    hashlib.sha256).hexdigest()


def verify_manifest(manifest, key, node, now, used_nonces):
    """None when the manifest is authentic and usable, else the reason."""
    if not key:
        return "no_key"
    if not isinstance(manifest, dict) or manifest.get("v") != 1:
        return "bad_format"
    mac = str(manifest.get("mac") or "")
    if not re.fullmatch(r"[0-9a-f]{64}", mac) or not hmac.compare_digest(
            mac, manifest_mac(key, manifest)):
        return "bad_mac"
    if manifest.get("node") != node:
        return "wrong_node"
    nonce = str(manifest.get("nonce") or "")
    if not re.fullmatch(r"[0-9a-f]{16,64}", nonce):
        return "bad_nonce"
    if nonce in used_nonces:
        return "replayed"
    try:
        issued, expires = int(manifest["issued_ms"]), int(manifest["expires_ms"])
    except (KeyError, TypeError, ValueError):
        return "bad_times"
    if abs(now - issued) > EXPECT_SKEW_MS or expires <= now or \
            expires - issued > MAX_EXPECT_WINDOW_MS:
        return "stale"
    files = manifest.get("files") or {}
    rescan = manifest.get("rescan") or []
    allow = manifest.get("allow") or []
    if not isinstance(files, dict) or not isinstance(rescan, list) or \
            not isinstance(allow, list):
        return "bad_format"
    for path in list(files) + list(rescan):
        if not isinstance(path, str) or not path.startswith("/") or \
                ".." in path.split("/"):
            return "bad_path"
    if any(a not in EXPECT_ALLOW for a in allow):
        return "bad_allow"
    return None


def _under(path, root):
    root = root.rstrip("/") or "/"
    return path == root or path.startswith(root + "/")


class Expectations:
    def __init__(self, directory, clock=now_ms):
        self.directory = directory
        self.clock = clock
        self.items = []

    def load(self):
        items = []
        now = self.clock()
        try:
            names = sorted(os.listdir(self.directory))
        except OSError:
            names = []
        for name in names:
            path = os.path.join(self.directory, name)
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    item = json.load(handle)
            except (OSError, ValueError):
                continue
            if int(item.get("expires_ms") or 0) <= now:
                try:
                    os.unlink(path)
                except OSError:
                    pass
                continue
            items.append(item)
        self.items = items
        return items

    def add(self, manifest):
        os.makedirs(self.directory, mode=0o700, exist_ok=True)
        path = os.path.join(self.directory, manifest["nonce"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, sort_keys=True)
        self.items.append(manifest)

    def covers(self, path, value):
        """'ok' (expected), 'mismatch' (expected, other content) or None."""
        verdict = None
        for item in self.items:
            files = item.get("files") or {}
            if path in files:
                want = files[path]
                if want == value or (isinstance(want, str) and "*" in want
                                     and value is not None
                                     and fnmatch.fnmatchcase(value, want)):
                    return "ok"
                verdict = "mismatch"
            for root in item.get("rescan") or ():
                if _under(path, root):
                    return "ok"
        return verdict

    def allows(self, kind):
        return any(kind in (item.get("allow") or ()) for item in self.items)


# --------------------------------------------------------------------------
# detector: file integrity


def parse_passwd(text):
    users = {}
    for line in (text or "").splitlines():
        parts = line.split(":")
        if len(parts) >= 7 and parts[0]:
            users[parts[0]] = {"uid": parts[2], "gid": parts[3],
                               "home": parts[5], "shell": parts[6]}
    return users


def parse_group(text):
    groups = {}
    for line in (text or "").splitlines():
        parts = line.split(":")
        if len(parts) >= 4 and parts[0]:
            groups[parts[0]] = {"gid": parts[2], "members": sorted(
                m for m in parts[3].split(",") if m)}
    return groups


def parse_shadow(text):
    return {p[0]: p[1] for p in (l.split(":") for l in (text or "").splitlines())
            if len(p) >= 2 and p[0]}


def _login_shell(shell):
    return bool(shell) and not any(shell.endswith(x)
                                   for x in LOGIN_SHELL_EXCLUDE)


def diff_accounts(old, new):
    """Events for changed /etc/passwd, group, shadow (old/new: {"passwd":
    text, "group": text, "shadow": text}; missing keys = unchanged)."""
    events = []
    if "passwd" in new:
        before, after = parse_passwd(old.get("passwd")), \
            parse_passwd(new.get("passwd"))
        for name, user in after.items():
            prev = before.get(name)
            if user["uid"] == "0" and (prev is None or prev["uid"] != "0"):
                events.append(Event("account_uid0", "lockdown",
                                    "Account %s now has uid 0" % name,
                                    {"user": name}))
            elif _login_shell(user["shell"]) and (
                    prev is None or not _login_shell(prev["shell"])):
                events.append(Event("account_login_shell", "lockdown",
                                    "Account %s got a login shell (%s)" % (
                                        name, user["shell"]), {"user": name}))
            elif prev is None:
                events.append(Event("account_added", "alert",
                                    "New account %s (uid %s)" % (
                                        name, user["uid"]), {"user": name}))
            elif prev != user:
                events.append(Event("account_changed", "alert",
                                    "Account %s changed" % name,
                                    {"user": name}))
        for name in set(before) - set(after):
            events.append(Event("account_removed", "alert",
                                "Account %s removed" % name, {"user": name}))
    if "group" in new:
        before, after = parse_group(old.get("group")), \
            parse_group(new.get("group"))
        for name, group in after.items():
            prev = before.get(name) or {"members": []}
            added = sorted(set(group["members"]) - set(prev["members"]))
            if added and name in PRIVILEGED_GROUPS:
                events.append(Event("group_privileged", "lockdown",
                                    "%s added to the privileged group %s" % (
                                        ",".join(added), name),
                                    {"group": name, "added": added}))
            elif name not in before or added:
                events.append(Event("group_changed", "alert",
                                    "Group %s changed" % name,
                                    {"group": name, "added": added}))
    if "shadow" in new:
        before, after = parse_shadow(old.get("shadow")), \
            parse_shadow(new.get("shadow"))
        root_before, root_after = before.get("root", "*"), \
            after.get("root", "*")
        if root_after != root_before and not root_after.startswith(("!", "*")):
            events.append(Event("root_password_set", "lockdown",
                                "root got a usable password", {}))
        elif before != after:
            events.append(Event("shadow_changed", "alert",
                                "/etc/shadow changed", {}))
    if "gshadow" in new and new.get("gshadow") != old.get("gshadow"):
        events.append(Event("gshadow_changed", "alert",
                            "/etc/gshadow changed", {}))
    return events


ACCOUNT_FILES = {"/etc/passwd": "passwd", "/etc/group": "group",
                 "/etc/shadow": "shadow", "/etc/gshadow": "gshadow"}


class Integrity:
    """Baseline of {path: [sha256 | link:target | dir, mode, uid, gid]}."""

    def __init__(self, cfg, root_prefix=""):
        integrity = cfg.get("integrity") or {}
        self.prefix = root_prefix.rstrip("/")
        self.lockdown_roots = list(integrity.get("lockdown") or ())
        self.alert_roots = list(integrity.get("alert") or ())
        self.trees = list(integrity.get("trees") or ())
        self.hot_roots = list(integrity.get("hot") or ())
        self.exclude = list(integrity.get("exclude") or ())
        self.authorized_keys = bool(integrity.get("authorized_keys", True))
        self.max_files = int(integrity.get("max_files", 60000))
        self._cache = {}
        self._rehash = False

    # paths are always the real (on-node) path; ``prefix`` is for tests.
    def _fs(self, path):
        return self.prefix + path if self.prefix else path

    def _excluded(self, path):
        return any(fnmatch.fnmatchcase(path, pattern)
                   for pattern in self.exclude)

    def severity(self, path, link=False):
        best, level = 0, None
        for root in self.lockdown_roots + self.trees:
            if _under(path, root) and len(root) >= best:
                best, level = len(root), "lockdown"
        for root in self.alert_roots:
            if _under(path, root) and len(root) > best:
                best, level = len(root), "alert"
        if level is None and "/.ssh/authorized_keys" in path:
            level = "lockdown"
        if level == "lockdown" and link and _under(path,
                                                   "/etc/systemd/system"):
            # enable/disable/mask symlinks; unit files stay lockdown.
            level = "alert"
        return level or "alert"

    def authorized_key_paths(self):
        paths = set()
        if not self.authorized_keys:
            return paths
        try:
            with open(self._fs("/etc/passwd"), "r", encoding="utf-8") as h:
                users = parse_passwd(h.read())
        except OSError:
            users = {}
        homes = {u["home"] for u in users.values()} | {"/root"}
        for home in homes:
            if not home.startswith("/") or home in ("/", "/nonexistent"):
                continue
            for name in ("authorized_keys", "authorized_keys2"):
                paths.add(home.rstrip("/") + "/.ssh/" + name)
        return paths

    def _fingerprint(self, path, st):
        if stat.S_ISLNK(st.st_mode):
            try:
                return ["link:" + os.readlink(self._fs(path)), 0o777, st.st_uid,
                        st.st_gid]
            except OSError:
                return None
        if stat.S_ISDIR(st.st_mode):
            return ["dir", stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid]
        if not stat.S_ISREG(st.st_mode):
            return ["special", stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid]
        sig = (st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
        cached = self._cache.get(path)
        if cached and cached[0] == sig and not self._rehash:
            digest = cached[1]
        else:
            h = hashlib.sha256()
            try:
                with open(self._fs(path), "rb") as handle:
                    for chunk in iter(lambda: handle.read(1 << 16), b""):
                        h.update(chunk)
            except OSError:
                return None
            digest = h.hexdigest()
            self._cache[path] = (sig, digest)
        return [digest, stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid]

    def _walk(self, root, out, follow_top=False):
        stack = [root]
        while stack:
            path = stack.pop()
            if self._excluded(path) or len(out) >= self.max_files:
                continue
            try:
                st = os.lstat(self._fs(path))
            except OSError:
                continue
            fp = self._fingerprint(path, st)
            if fp is None:
                continue
            out[path] = fp
            if stat.S_ISDIR(st.st_mode):
                try:
                    names = os.listdir(self._fs(path))
                except OSError:
                    continue
                stack.extend(path.rstrip("/") + "/" + n for n in names)

    def roots(self, group):
        if group == "hot":
            return list(self.hot_roots) + sorted(self.authorized_key_paths())
        roots = [r for r in self.lockdown_roots + self.alert_roots
                 if not any(_under(r, h) for h in self.hot_roots)]
        return roots

    def scan(self, group="all", rehash=False):
        """{path: fingerprint} for the group's roots (``all``, ``hot`` or
        ``normal``); trees add the symlink itself and the tree it points at
        (keyed by real path)."""
        self._rehash = rehash
        out = {}
        groups = ("hot", "normal") if group == "all" else (group,)
        for name in groups:
            for root in self.roots(name):
                self._walk(root, out)
            if name == "normal":
                for tree in self.trees:
                    try:
                        st = os.lstat(self._fs(tree))
                    except OSError:
                        continue
                    fp = self._fingerprint(tree, st)
                    if fp is not None:
                        out[tree] = fp
                    if stat.S_ISLNK(st.st_mode):
                        real = os.path.realpath(self._fs(tree))
                        if self.prefix:
                            real = real[len(self.prefix):] or "/"
                        self._walk(real, out)
        self._rehash = False
        return out

    def in_group(self, path, group, baseline):
        """Whether ``path`` (from a baseline) belongs to a scan group."""
        roots = self.roots("hot" if group == "hot" else "normal")
        if any(_under(path, r) for r in roots):
            return True
        if group != "hot":
            if path in self.trees:
                return True
            for tree in self.trees:
                target = (baseline.get(tree) or [""])[0]
                if isinstance(target, str) and target.startswith("link:"):
                    real = target[5:]
                    if not real.startswith("/"):
                        real = os.path.normpath(os.path.join(
                            os.path.dirname(tree), real))
                    if _under(path, real):
                        return True
        return False

    def read_account_files(self, paths):
        out = {}
        for path in paths:
            name = ACCOUNT_FILES.get(path)
            if not name:
                continue
            try:
                with open(self._fs(path), "r", encoding="utf-8",
                          errors="replace") as handle:
                    out[name] = handle.read()
            except OSError:
                out[name] = ""
        return out


def compare(baseline, current, subset):
    """[(path, old fp or None, new fp or None)] for paths in ``subset``."""
    changes = []
    for path in sorted(set(baseline) | set(current)):
        if not subset(path):
            continue
        old, new = baseline.get(path), current.get(path)
        if old != new:
            changes.append((path, old, new))
    return changes


def change_events(integrity, changes, expectations, account_text_before,
                  account_text_after):
    """Events for unexpected changes; returns (events, accepted paths)."""
    events, accepted = [], []
    account_changed = {}
    for path, old, new in changes:
        value = None if new is None else new[0]
        verdict = expectations.covers(path, value) if expectations else None
        if verdict == "ok":
            accepted.append(path)
            continue
        if path in ACCOUNT_FILES and new is not None and old is not None \
                and old[1:] == new[1:] and verdict is None:
            name = ACCOUNT_FILES[path]
            if name in account_text_after:
                account_changed[name] = account_text_after[name]
            accepted.append(path)
            continue
        link = bool(value) and str(value).startswith("link:") or bool(
            old and str(old[0]).startswith("link:"))
        level = integrity.severity(path, link=link)
        kind = ("added" if old is None else "removed" if new is None
                else "modified")
        if old and new and old[0] == new[0]:
            kind = "permissions"
            if new[1] & 0o6000 and not old[1] & 0o6000:
                level = "lockdown"
        detail = {"path": path, "change": kind,
                  "old": (old or [None])[0] and str(old[0])[:80],
                  "new": (new or [None])[0] and str(new[0])[:80]}
        if verdict == "mismatch":
            detail["expected"] = "different content"
        events.append(Event("integrity_%s" % kind, level,
                            "%s %s%s" % (path, kind, " (not the expected "
                                         "content)" if verdict == "mismatch"
                                         else ""), detail,
                            key="integrity:%s:%s" % (path, value)))
    if account_changed:
        if expectations is not None and expectations.allows("users"):
            accepted.extend(p for p, n in ACCOUNT_FILES.items()
                            if n in account_changed)
        else:
            events += diff_accounts(account_text_before, account_changed)
    return events, accepted


# --------------------------------------------------------------------------
# detector: processes, listeners, modules


def _read(path, limit=65536):
    try:
        with open(path, "rb") as handle:
            return handle.read(limit).decode("utf-8", "replace")
    except OSError:
        return None


def process_info(proc, pid):
    status = _read("%s/%s/status" % (proc, pid))
    if status is None:
        return None
    info = {"pid": int(pid)}
    for line in status.splitlines():
        key, _, value = line.partition(":")
        value = value.strip()
        if key == "Name":
            info["name"] = value
        elif key == "Uid":
            info["uid"] = int(value.split()[1])   # effective uid
            info["ruid"] = int(value.split()[0])
        elif key == "TracerPid":
            info["tracer"] = int(value or 0)
        elif key == "PPid":
            info["ppid"] = int(value or 0)
    try:
        info["exe"] = os.readlink("%s/%s/exe" % (proc, pid))
    except OSError:
        info["exe"] = ""
    units = cgroup_units(_read("%s/%s/cgroup" % (proc, pid)))
    if not units:
        again = _read("%s/%s/status" % (proc, pid))
        if again is None or _status_state(again) in ("Z", "X"):
            info["vanished"] = True
        else:
            units = cgroup_units(_read("%s/%s/cgroup" % (proc, pid)))
    info["unit"] = units[-1] if units else ""
    info["system_unit"] = units[0] if units else ""
    return info


def _status_state(status):
    for line in status.splitlines():
        key, _, value = line.partition(":")
        if key == "State":
            return value.strip()[:1]
    return ""


def cgroup_units(text):
    for line in (text or "").splitlines():
        path = line.split(":", 2)[-1]
        units = [p for p in path.split("/")
                 if p.endswith((".service", ".scope"))]
        if units:
            return units
    return []


SECRET_PATTERNS = (
    (re.compile(r"(://[^/\s:@]+:)[^@\s/]+@"), r"\1***@"),
    (re.compile(r"(?i)(authorization:\s*(?:\w+\s+)?)\S+"), r"\1***"),
    (re.compile(r"(?i)((?:token|password|passwd|secret|api[_-]?key|key)"
                r"[=:])[^\s&]+"), r"\1***"),
    (re.compile(r"(?i)(--(?:token|password|secret|api-key)[ =])\S+"),
     r"\1***"))


def redact(text):
    for pattern, replacement in SECRET_PATTERNS:
        text = pattern.sub(replacement, text)
    return text


def process_argv(proc, pid):
    raw = _read("%s/%s/cmdline" % (proc, pid), 8192) or ""
    return [arg for arg in raw.split("\0") if arg]


def _readlink(path):
    try:
        return os.readlink(path)
    except OSError:
        return ""


def process_environment(proc, pid):
    raw = _read("%s/%s/environ" % (proc, pid), 1 << 17) or ""
    found = {}
    for item in raw.split("\0"):
        key, sep, value = item.partition("=")
        if sep and key in CONTEXT_ENV:
            found[key] = redact(value)[:160]
    return found


def process_started(proc, pid):
    fields = (_read("%s/%s/stat" % (proc, pid), 4096) or "").rpartition(
        ")")[2].split()
    boot = re.search(r"^btime (\d+)$", _read("%s/stat" % proc, 1 << 16) or "",
                     re.M)
    try:
        return int(boot.group(1)) + int(fields[19]) / os.sysconf(
            "SC_CLK_TCK")
    except (AttributeError, IndexError, ValueError, OSError):
        return None


def socket_inodes(proc, pid, limit=1024):
    fd_dir = "%s/%s/fd" % (proc, pid)
    try:
        fds = os.listdir(fd_dir)[:limit]
    except OSError:
        return []
    inodes = []
    for fd in fds:
        target = _readlink(os.path.join(fd_dir, fd))
        if target.startswith("socket:[") and target.endswith("]"):
            inodes.append(target[8:-1])
    return inodes


def connection_table(proc_net):
    rows = {}
    for name in ("tcp", "tcp6", "udp", "udp6"):
        text = _read(os.path.join(proc_net, name), 4 << 20) or ""
        for line in text.splitlines()[1:]:
            parts = line.split()
            if len(parts) < 10 or parts[9] == "0":
                continue
            try:
                local = (_hex_address(parts[1]), _hex_port(parts[1]))
                remote = (_hex_address(parts[2]), _hex_port(parts[2]))
            except (ValueError, IndexError):
                continue
            state = TCP_STATES.get(parts[3], "") if name.startswith(
                "tcp") else ""
            rows[parts[9]] = (name.rstrip("6"), local, remote, state)
    return rows


def _endpoint(address, port):
    return ("[%s]:%d" if ":" in address else "%s:%d") % (address, port)


def _public_peer(row):
    try:
        remote = ipaddress.ip_address(row[2][0])
    except ValueError:
        return False
    return not (remote.is_loopback or remote.is_unspecified)


def read_small_regular(path, limit=1 << 16):
    try:
        info = os.lstat(path)
    except OSError:
        return None
    if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
        return None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    except OSError:
        return None
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            return None
        return os.read(fd, limit)
    except OSError:
        return None
    finally:
        os.close(fd)


def _is_git(exe):
    exe = exe.replace(" (deleted)", "")
    return exe == "/usr/bin/git" or exe.startswith("/usr/lib/git-core/")


def fit_detail(detail, limit=CONTEXT_BYTES):
    def size():
        return len(json.dumps(detail, sort_keys=True, default=str))
    for key in ("children", "connections", "environment", "parents"):
        while size() > limit and detail.get(key):
            value = detail[key]
            if isinstance(value, dict):
                value.pop(sorted(value)[-1])
            else:
                value.pop()
            if not value:
                detail.pop(key)
    for key in ("cmdline", "verdict", "cwd"):
        if size() > limit and isinstance(detail.get(key), str):
            detail[key] = detail[key][:max(40, len(detail[key]) - (
                size() - limit))]
    return detail


class ProcessMonitor:
    def __init__(self, cfg, proc="/proc", user_of=uid_name):
        processes = cfg.get("processes") or {}
        self.proc = proc
        self.user_of = user_of
        self.services = processes.get("service_users") or {}
        self.job_user = processes.get("job_user") or ""
        self.job_units = list(processes.get("job_units") or ())
        self.protected = set(processes.get("protected_units") or ())

    def _pids(self):
        try:
            return [p for p in os.listdir(self.proc) if p.isdigit()]
        except OSError:
            return []

    def scan(self, allow_ptrace=False, allow_processes=False):
        events = []
        infos = {}
        for pid in self._pids():
            info = process_info(self.proc, pid)
            if info is not None and "uid" in info:
                infos[info["pid"]] = info
        for info in infos.values():
            events += self.check(info, infos, allow_ptrace, allow_processes)
        return events

    def check(self, info, infos, allow_ptrace=False, allow_processes=False):
        events = self._check(info, infos, allow_ptrace, allow_processes)
        if events:
            self.annotate(events, info, infos)
        return events

    def annotate(self, events, info, infos):
        context, short, verdict = self.context(info, infos)
        for event in events:
            event["summary"] = ("%s; %s" % (event["summary"], short))[:300]
            event["detail"].update(context, verdict=verdict)
            fit_detail(event["detail"])

    def _ancestors(self, info, infos):
        chain, seen = [], {info["pid"]}
        ppid = info.get("ppid") or 0
        while ppid > 1 and ppid not in seen and len(chain) < CONTEXT_PARENTS:
            seen.add(ppid)
            parent = infos.get(ppid) or process_info(self.proc, str(ppid))
            if parent is None:
                break
            chain.append(parent)
            ppid = parent.get("ppid") or 0
        return chain

    def _describe(self, info, argv=None):
        argv = process_argv(self.proc, info["pid"]) if argv is None else argv
        text = "pid %d %s as %s" % (info["pid"], info.get("exe") or "?",
                                    self.user_of(info.get("uid", "?")))
        if argv:
            text += ": " + redact(" ".join(argv))
        return text[:260]

    def _connections(self, chain):
        tables, found = {}, []
        for item in chain:
            pid = item["pid"]
            inodes = socket_inodes(self.proc, pid)
            if not inodes:
                continue
            netns = _readlink("%s/%d/ns/net" % (self.proc, pid)) or str(pid)
            if netns not in tables:
                tables[netns] = connection_table("%s/%d/net" % (self.proc,
                                                                pid))
            rows = [tables[netns][i] for i in inodes if i in tables[netns]]
            rows.sort(key=lambda r: (not _public_peer(r), r[3] == "listen"))
            for proto, local, remote, state in rows[:4]:
                found.append("pid %d %s %s -> %s%s" % (
                    pid, proto, _endpoint(*local), _endpoint(*remote),
                    " " + state if state else ""))
        return found[:CONTEXT_CONNECTIONS]

    def context(self, info, infos):
        pid = info["pid"]
        argv = process_argv(self.proc, pid)
        cwd = _readlink("%s/%d/cwd" % (self.proc, pid))
        chain = self._ancestors(info, infos)
        context = {}
        if argv:
            context["cmdline"] = redact(" ".join(argv))[:300]
        if cwd:
            context["cwd"] = cwd[:200]
        if info.get("ppid"):
            context["ppid"] = info["ppid"]
        started = process_started(self.proc, pid)
        if started:
            context["started"] = "%s UTC (%.0f s before this scan)" % (
                time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(started)),
                max(0, time.time() - started))
        if chain:
            context["parents"] = [self._describe(p) for p in chain]
        children = sorted((c for c in infos.values()
                           if c.get("ppid") == pid), key=lambda c: c["pid"])
        if children:
            context["children"] = [self._describe(c) for c in
                                   children[:CONTEXT_CHILDREN]]
        environment = {}
        for item in [info] + chain:
            for key, value in process_environment(self.proc,
                                                  item["pid"]).items():
                environment.setdefault(key, value)
        if environment:
            context["environment"] = environment
        connections = self._connections([info] + chain)
        if connections:
            context["connections"] = connections
        short, verdict = self.explain(info, argv, cwd, chain)
        return context, short, verdict

    def _hook(self, info, argv, cwd):
        for arg in argv[1:3]:
            if arg.startswith("-"):
                continue
            if os.path.basename(arg) not in GIT_HOOKS:
                return None
            path = arg if arg.startswith("/") else os.path.normpath(
                os.path.join(cwd or "/", arg))
            text = read_small_regular("%s/%d/root%s" % (self.proc,
                                                        info["pid"], path))
            return path, text
        return None

    def explain(self, info, argv, cwd, chain):
        parents = [(p, process_argv(self.proc, p["pid"])) for p in chain]
        receive = next((p for p, a in parents[:3] if _is_git(
            p.get("exe") or "") and any(x.endswith("receive-pack")
                                        for x in a)), None)
        hook = self._hook(info, argv, cwd)
        if hook and receive:
            path, text = hook
            name = os.path.basename(path)
            if text is not None and any(m in text for m in
                                        FORKMESH_HOOK_MARKERS):
                return ("likely benign: ForkMesh's own %s hook during a "
                        "git push" % name,
                        "Likely benign. This is ForkMesh's own %s hook (%s, "
                        "it carries ForkMesh's marker), run by git "
                        "receive-pack (pid %d) while a push lands. Its "
                        "command line, parent and hook file all match. "
                        "Cross-check the push in the mirror-node journal "
                        "around this time; an attacker could imitate these "
                        "facts." % (name, path, receive["pid"]))
            return ("needs a look: a %s hook ForkMesh did not write ran "
                    "during a git push" % name,
                    "Needs a look. git receive-pack (pid %d) ran the %s hook "
                    "%s, which %s. Read that file and find who installed it."
                    % (receive["pid"], name, path,
                       "could not be read" if text is None else
                       "has no ForkMesh marker"))
        sandbox = next((p for p, _ in parents if os.path.basename(
            (p.get("exe") or "").replace(" (deleted)", "")) == "bwrap"),
            None)
        if sandbox and argv[1:] == ["-c", "exit 0"]:
            return ("likely benign: the Actions sandbox self-test",
                    "Likely benign. The mirror-node daemon checks that "
                    "bubblewrap works by running /bin/sh -c 'exit 0' inside "
                    "it (bwrap pid %d) before it runs Actions jobs."
                    % sandbox["pid"])
        if sandbox:
            return ("probably an Actions job step in its sandbox",
                    "Probably an Actions job step. It runs inside a "
                    "bubblewrap sandbox (bwrap pid %d), which is how the "
                    "daemon runs workflow steps. Check which workflow ran "
                    "at this time." % sandbox["pid"])
        if parents and _is_git(parents[0][0].get("exe") or "") and \
                argv[1:2] == ["-c"]:
            return ("needs a look: git ran a shell command",
                    "Needs a look. git (pid %d: %s) ran a shell command, "
                    "which git does for an ssh command, credential helper, "
                    "alias or hook set in its config or environment. Find "
                    "which setting this is." % (
                        parents[0][0]["pid"],
                        redact(" ".join(parents[0][1]))[:160]))
        if not argv and not cwd:
            return ("unexplained: it exited before the IDS could read it",
                    "Unexplained. The process exited before the IDS could "
                    "read its command line or working directory. Read the "
                    "unit's journal around this time.")
        return ("unexplained: no known ForkMesh source matches",
                "Unexplained. No known ForkMesh source (its own git hooks, "
                "the Actions sandbox) matches this process. Treat it as "
                "suspicious until its parents and command line explain it.")

    def _check(self, info, infos, allow_ptrace=False, allow_processes=False):
        events = []
        user = self.user_of(info["uid"])
        exe = info.get("exe") or ""
        unit = info.get("unit") or ""
        base = os.path.basename(exe.replace(" (deleted)", ""))
        base_name = re.sub(r"[0-9.]+$", "", base)
        detail = {"pid": info["pid"], "user": user, "exe": exe[:200],
                  "unit": unit, "name": info.get("name", "")}
        key = "%s:%s:%s" % (info["pid"], exe, unit)
        tracer = info.get("tracer") or 0
        if tracer:
            tracer_info = infos.get(tracer) or {}
            detail["tracer"] = {"pid": tracer,
                                "exe": (tracer_info.get("exe") or "")[:200]}
            protected = unit in self.protected or user in self.services
            severity = "info" if allow_ptrace else (
                "lockdown" if protected else "alert")
            events.append(Event("ptrace", severity,
                                "Process %d (%s, %s) is being traced by %d"
                                % (info["pid"], user, unit or "?", tracer),
                                detail, key="ptrace:" + key))
        deleted = exe.endswith(" (deleted)")
        if exe.startswith(SUSPICIOUS_EXE_DIRS) or (
                deleted and exe.replace(" (deleted)", "").startswith(
                    SUSPICIOUS_EXE_DIRS)):
            if user != self.job_user and not allow_processes:
                events.append(Event("exe_suspicious_path",
                                    "lockdown" if user in self.services
                                    or info["uid"] == 0 else "alert",
                                    "Process %d (%s) runs %s" % (
                                        info["pid"], user, exe[:120]),
                                    detail, key="exe:" + key))
        vanished = bool(info.get("vanished"))
        if user == self.job_user and self.job_user:
            if not vanished and not any(
                    fnmatch.fnmatchcase(unit, p) for p in self.job_units):
                events.append(Event("job_user_outside_scope", "lockdown",
                                    "Job user process %d outside a job scope "
                                    "(%s)" % (info["pid"], unit or "?"),
                                    detail, key="job:" + key))
            return events
        spec = self.services.get(user)
        if spec is None or allow_processes:
            return events
        units = spec.get("units") or ()
        unknown = vanished and any(p != "-" for p in units)
        owner = info.get("system_unit", unit)
        if units and not unknown and not any(
                fnmatch.fnmatchcase(owner, p) for p in units):
            if owner != unit:
                detail["system_unit"] = owner
            events.append(Event("service_user_outside_unit",
                                spec.get("outside_severity", "lockdown"),
                                "%s process %d (%s) outside its unit (%s)"
                                % (user, info["pid"], base, owner or "?"),
                                detail, key="outside:" + key))
        if base_name in SHELLS and not any(
                fnmatch.fnmatchcase(exe, p) for p in spec.get("exe") or ()):
            events.append(Event("service_user_shell",
                                spec.get("shell_severity", "lockdown"),
                                "%s runs %s (pid %d)" % (user, base,
                                                         info["pid"]),
                                detail, key="shell:" + key))
        elif spec.get("exe") and exe and not any(
                fnmatch.fnmatchcase(exe.replace(" (deleted)", ""), p)
                for p in spec["exe"]) and not self.unit_starting(
                    info, exe, owner, units):
            events.append(Event("service_user_exe",
                                spec.get("exe_severity", "alert"),
                                "%s runs an unexpected program %s (pid %d)"
                                % (user, exe[:120], info["pid"]), detail,
                                key="exe:" + key))
        return events

    @staticmethod
    def unit_starting(info, exe, owner, units):
        name = info.get("name") or ""
        return (exe.replace(" (deleted)", "") in SYSTEMD_EXECUTORS
                and len(name) > 2 and name[0] == "(" and name[-1] == ")"
                and not info.get("vanished")
                and (not units or any(fnmatch.fnmatchcase(owner, p)
                                      for p in units)))


def _hex_port(text):
    return int(text.rsplit(":", 1)[1], 16)


def _hex_address(text):
    raw = bytes.fromhex(text.rsplit(":", 1)[0])
    packed = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
    return str(ipaddress.ip_address(packed))


def listening_rows(proc_net):
    out = []
    for name, state in (("tcp", "0A"), ("tcp6", "0A"),
                        ("udp", "07"), ("udp6", "07")):
        text = _read(os.path.join(proc_net, name), 1 << 20)
        if not text:
            continue
        for line in text.splitlines()[1:]:
            parts = line.split()
            if len(parts) < 8 or parts[3] != state:
                continue
            if name.startswith("udp") and not parts[2].endswith(":0000"):
                continue
            try:
                port, uid = _hex_port(parts[1]), int(parts[7])
            except (ValueError, IndexError):
                continue
            try:
                address = _hex_address(parts[1])
            except ValueError:
                address = ""
            inode = parts[9] if len(parts) > 9 and parts[9] != "0" else ""
            out.append((name.rstrip("6"), port, uid, address, inode))
    return sorted(set(out))


def listening_sockets(proc_net):
    """[(proto, port, uid)] of TCP listeners and unconnected UDP sockets."""
    return sorted({row[:3] for row in listening_rows(proc_net)})


def socket_owners(inodes, proc="/proc"):
    wanted = {"socket:[%s]" % inode: inode for inode in inodes if inode}
    found = {}
    if not wanted:
        return found
    try:
        pids = sorted((p for p in os.listdir(proc) if p.isdigit()), key=int)
    except OSError:
        return found
    for pid in pids:
        fd_dir = "%s/%s/fd" % (proc, pid)
        try:
            fds = os.listdir(fd_dir)
        except OSError:
            continue
        for fd in fds:
            try:
                target = os.readlink("%s/%s" % (fd_dir, fd))
            except OSError:
                continue
            if target in wanted:
                pids_for = found.setdefault(wanted[target], [])
                if int(pid) not in pids_for:
                    pids_for.append(int(pid))
    return found


def _listener_processes(inodes, proc):
    owners = socket_owners(inodes, proc)
    processes = []
    for pid in sorted({pid for pids in owners.values() for pid in pids}):
        info = process_info(proc, pid)
        if info is None:
            continue
        entry = {"pid": pid, "name": info.get("name", ""),
                 "exe": info.get("exe", "")[:200]}
        if info.get("system_unit"):
            entry["unit"] = info["system_unit"]
        if info.get("ppid"):
            entry["ppid"] = info["ppid"]
        processes.append(entry)
    return processes[:4]


def _listener_summary(user, proto, port, addresses, processes):
    text = "%s listens on %s port %d" % (user, proto, port)
    if addresses:
        text += " on %s" % ", ".join(addresses)
        if all(ipaddress.ip_address(a).is_loopback for a in addresses):
            text += " (loopback only)"
    if processes:
        text += " by %s" % ", ".join(
            "%s pid %d%s" % (p["name"] or "?", p["pid"],
                             " in %s" % p["unit"] if p.get("unit") else "")
            for p in processes)
    return text


def _loopback_allowed(rules, user, addresses, processes):
    if not addresses or not processes or not all(
            ipaddress.ip_address(a).is_loopback for a in addresses):
        return False
    for rule in rules:
        if rule.get("user") != user:
            continue
        if all(any(fnmatch.fnmatchcase(p.get("unit", ""), u)
                   for u in rule.get("units") or ())
               and any(fnmatch.fnmatchcase(p.get("exe", ""), e)
                       for e in rule.get("exe") or ())
               for p in processes):
            return True
    return False


def listener_events(sockets, cfg, proc=None):
    listeners = cfg.get("listeners") or {}
    tcp = set(int(p) for p in listeners.get("tcp") or ())
    udp = set(int(p) for p in listeners.get("udp") or ())
    udp_max = int(listeners.get("udp_max_port", 10240))
    groups = {}
    for row in sockets:
        proto, port, uid = row[:3]
        if proto == "tcp" and port not in tcp or \
                proto == "udp" and port < udp_max and port not in udp:
            group = groups.setdefault((proto, port, uid), ([], []))
            if len(row) > 3 and row[3] and row[3] not in group[0]:
                group[0].append(row[3])
            if len(row) > 4 and row[4]:
                group[1].append(row[4])
    events = []
    for (proto, port, uid), (addresses, inodes) in sorted(groups.items()):
        user = uid_name(uid)
        detail = {"user": user, "proto": proto, "port": port}
        if addresses:
            detail["addresses"] = addresses
        processes = _listener_processes(inodes, proc) if proc else []
        if _loopback_allowed(listeners.get("loopback_allow") or (), user,
                             addresses, processes):
            continue
        if processes:
            detail["processes"] = processes
        events.append(Event("listener_unexpected", "alert",
                            _listener_summary(user, proto, port, addresses,
                                              processes),
                            detail,
                            key="listen:%s:%d:%s" % (proto, port, uid)))
    return events


def loaded_modules(path="/proc/modules"):
    text = _read(path, 1 << 20) or ""
    return {line.split()[0] for line in text.splitlines() if line.split()}


# --------------------------------------------------------------------------
# rqlite (core): local first, then the peers over WireGuard


class Rqlite:
    def __init__(self, urls, user, password, opener=None, timeout=5.0):
        self.urls = [u.rstrip("/") for u in urls if u]
        self.user = user
        self.password = password
        self.opener = opener or urllib.request.urlopen
        self.timeout = timeout

    def _request(self, url, path, body=None, method=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(url + path, data=data,
                                         method=method or (
                                             "POST" if data else "GET"))
        request.add_header("content-type", "application/json")
        if self.user:
            token = base64.b64encode(("%s:%s" % (
                self.user, self.password)).encode()).decode()
            request.add_header("authorization", "Basic " + token)
        with self.opener(request, timeout=self.timeout) as response:
            raw = response.read()
        return json.loads(raw) if raw.strip() else {}

    def _any(self, path, body=None, method=None, urls=None):
        error = None
        for url in (urls or self.urls):
            try:
                return url, self._request(url, path, body, method)
            except (OSError, ValueError, urllib.error.URLError) as exc:
                error = exc
        raise OSError("rqlite unreachable: %s" % type(error).__name__)

    def execute(self, statements, local_only=False):
        _, reply = self._any("/db/execute", statements,
                             urls=self.urls[:1] if local_only else None)
        for result in reply.get("results") or []:
            if result.get("error"):
                raise OSError("rqlite: %s" % result["error"][:200])
        return reply.get("results") or []

    def query(self, sql, *params, level="weak"):
        _, reply = self._any("/db/query?associative&level=" + level,
                             [[sql, *params]])
        result = (reply.get("results") or [{}])[0]
        if result.get("error"):
            raise OSError("rqlite: %s" % result["error"][:200])
        return result.get("rows") or []

    def nodes(self):
        _, reply = self._any("/nodes?nonvoters&ver=2", urls=self.urls[:1])
        nodes = reply.get("nodes", reply) if isinstance(reply, dict) else reply
        if isinstance(nodes, dict):
            nodes = [dict(v, id=v.get("id", k)) for k, v in nodes.items()]
        return list(nodes or [])

    def remove(self, node_id):
        return self._any("/remove", {"id": node_id}, method="DELETE",
                         urls=self.urls[:1])


# --------------------------------------------------------------------------
# firewall (nftables table ``inet fmnext_ids``)

TABLE = "fmnext_ids"


def firewall_ruleset(blocked=(), lockdown=False, ssh_allow=(),
                     keep_raft=False, core_wg_ips=(), core_public_ips=(),
                     wg_port=51820, raft_port=4002, dns_uid=None,
                     rqlite_user="rqlite"):
    """The whole IDS table, replaced atomically. Always: drop a quarantined
    peer's traffic (``blocked``). In lockdown: inbound only established
    replies, loopback, SSH (from ``ssh_allow`` when set), WireGuard from the
    cores and (when rqlite stays for quorum) Raft; outbound only root."""
    def elements(values):
        values = sorted(set(values))
        return (" elements = { %s }" % ", ".join(values) + ";") if values \
            else ""
    lines = ["table inet %s {}" % TABLE, "delete table inet %s" % TABLE,
             "table inet %s {" % TABLE,
             "\tset blocked { type ipv4_addr; flags interval;%s }"
             % elements(blocked)]
    if lockdown and ssh_allow:
        lines.append("\tset ssh_allow { type ipv4_addr; flags interval;%s }"
                     % elements(ssh_allow))
    lines += ["\tchain input {",
              "\t\ttype filter hook input priority -10; policy accept;",
              "\t\tip saddr @blocked drop"]
    if lockdown:
        if ssh_allow:
            lines += ["\t\ttcp dport 22 ip saddr != @ssh_allow drop",
                      "\t\tmeta nfproto ipv6 tcp dport 22 drop"]
        lines += ["\t\tct state established,related accept",
                  "\t\tiif \"lo\" accept",
                  "\t\ttcp dport 22 accept",
                  "\t\tip protocol icmp accept",
                  "\t\tmeta l4proto ipv6-icmp accept"]
        if core_public_ips:
            lines.append("\t\tip saddr { %s } udp dport %d accept" % (
                ", ".join(sorted(core_public_ips)), int(wg_port)))
        if keep_raft and core_wg_ips:
            lines.append("\t\tiifname \"wg0\" ip saddr { %s } tcp dport %d "
                         "accept" % (", ".join(sorted(core_wg_ips)),
                                     int(raft_port)))
        lines.append("\t\tcounter drop")
    lines += ["\t}", "\tchain output {",
              "\t\ttype filter hook output priority -10; policy accept;",
              "\t\tip daddr @blocked drop"]
    if lockdown:
        lines += ["\t\toif \"lo\" accept", "\t\tmeta skuid 0 accept"]
        if dns_uid is not None:
            lines.append("\t\tmeta skuid %d meta l4proto { tcp, udp } th "
                         "dport 53 accept" % int(dns_uid))
        if keep_raft and core_wg_ips:
            lines.append("\t\tmeta skuid \"%s\" ip daddr { %s } accept" % (
                rqlite_user, ", ".join(sorted(core_wg_ips))))
        lines.append("\t\tmeta skuid != 0 counter drop")
    lines += ["\t}", "}"]
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# actions (the side effects, injectable for tests)


class Actions:
    def __init__(self, cfg, run=subprocess.run):
        self.cfg = cfg
        self.run = run

    def _cmd(self, argv, stdin=None, timeout=60):
        try:
            done = self.run(argv, input=stdin, capture_output=True,
                            timeout=timeout, check=False)
            return done.returncode == 0, (done.stderr or b"")[-300:].decode(
                "utf-8", "replace")
        except (OSError, subprocess.SubprocessError) as error:
            return False, type(error).__name__

    def seal_app(self):
        """seal-restart over the unseal socket: DATA_KEY and the vault
        values leave the app's memory, then the process exits."""
        path = self.cfg.get("unseal_socket") or ""
        if not path or not os.path.exists(path):
            return False, "no unseal socket"
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(10)
                client.connect(path)
                client.sendall(b"seal-restart\n")
                client.shutdown(socket.SHUT_WR)
                reply = client.recv(65536)
            body = json.loads(reply.decode() or "{}")
            return bool(body.get("sealed")), "sealed" if body.get(
                "sealed") else str(body.get("error"))
        except (OSError, ValueError) as error:
            return False, type(error).__name__

    def firewall(self, text):
        return self._cmd(["nft", "-f", "-"], stdin=text.encode())

    def firewall_present(self):
        ok, _ = self._cmd(["nft", "list", "table", "inet", TABLE])
        return ok

    def stop_units(self, units):
        if not units:
            return True, ""
        return self._cmd(["systemctl", "stop", "--no-block", *units])

    def stop_units_wait(self, units):
        if not units:
            return True, ""
        return self._cmd(["systemctl", "stop", *units], timeout=120)

    def start_units(self, units):
        if not units:
            return True, ""
        return self._cmd(["systemctl", "start", "--no-block", *units])

    def kill_jobs(self):
        results = [self._cmd(["systemctl", "stop", "--no-block",
                              "fmnext-job-*.scope"])]
        user = (self.cfg.get("processes") or {}).get("job_user")
        if user:
            results.append(self._cmd(["pkill", "-KILL", "-u", user]))
        return all(r[0] for r in results), ";".join(r[1] for r in results)


# --------------------------------------------------------------------------
# alert channels


def mask_address(address):
    value = str(address or "").strip().lower()
    local, at, domain = value.rpartition("@")
    return (local[:1] or "?") + "***@" + domain if at else "(no recipient)"


class Alerter:
    """Fan-out of an incident: journal, rqlite (core) or the app API
    (runner), direct mail (Postfix or Mailtrap) and a signed webhook
    (both optional)."""

    def __init__(self, cfg, rqlite=None, opener=None, credential=None,
                 sign_request=None, smtp=None):
        self.cfg = cfg
        self.rqlite = rqlite
        self.opener = opener or urllib.request.urlopen
        self.credential = credential or read_credential
        self.sign_request = sign_request
        self.smtp = smtp or smtplib.SMTP

    def record(self, incident):
        if self.rqlite is None:
            return False, "no rqlite"
        try:
            self.rqlite.execute([[
                "INSERT OR IGNORE INTO security_incidents(id, node_id, "
                "created_ms, severity, detector, summary, detail) "
                "VALUES(?, ?, ?, ?, ?, ?, ?)", incident["id"],
                incident["node"], incident["ts"], incident["severity"],
                incident["detector"], incident["summary"],
                json.dumps(incident["detail"], sort_keys=True,
                           default=str)[:4000]]])
            return True, "recorded"
        except OSError as error:
            return False, str(error)[:120]

    def post_runner(self, incident):
        runner = self.cfg.get("runner") or {}
        if not runner.get("app_url") or self.sign_request is None:
            return False, "no runner api"
        path = "/api/runners/security-incident"
        body = json.dumps({"id": incident["id"],
                           "severity": incident["severity"],
                           "detector": incident["detector"],
                           "summary": incident["summary"],
                           "detail": incident["detail"]},
                          sort_keys=True, default=str).encode()
        try:
            headers = self.sign_request(path, body)
            request = urllib.request.Request(
                runner["app_url"].rstrip("/") + path, data=body,
                method="POST", headers=dict(headers, **{
                    "content-type": "application/json",
                    "user-agent": "fmnext-ids/1"}))
            with self.opener(request, timeout=15) as response:
                reply = json.loads(response.read() or b"{}")
            return bool(reply.get("ok")), "api %s" % reply.get("id", "")
        except (OSError, ValueError, urllib.error.URLError) as error:
            return False, type(error).__name__

    def locked_down(self):
        return os.path.exists(os.path.join(self.cfg.get("state_dir") or "",
                                           "LOCKDOWN"))

    def mail_token(self, mail):
        token = self.credential(mail.get("token_credential", ""))
        path = mail.get("token_file") or ""
        if token or not path:
            return token
        try:
            with open(path, "r", encoding="utf-8") as handle:
                for line in handle:
                    _relay, _, login = line.strip().partition(" ")
                    user, _, secret = login.partition(":")
                    if user == "api" and secret:
                        return secret
        except OSError:
            pass
        return ""

    def mail_smtp(self, mail, sender, to, subject, text):
        host, _, port = str(mail.get("smtp") or "").rpartition(":")
        if host not in ("127.0.0.1", "::1", "localhost") or not port.isdigit():
            return False, "no smtp"
        domain = sender.rpartition("@")[2] or "localhost"
        message = email.message.EmailMessage()
        message["From"] = email.utils.formataddr(("ForkMesh IDS", sender))
        message["To"] = to
        message["Subject"] = subject
        message["Date"] = email.utils.formatdate(usegmt=True)
        message["Message-ID"] = email.utils.make_msgid(domain=domain)
        message["Auto-Submitted"] = "auto-generated"
        message["X-ForkMesh-Kind"] = "security_alert"
        message["Feedback-ID"] = "security_alert:forkmesh"
        message.set_content(text)
        try:
            with self.smtp(host, int(port), timeout=15) as client:
                refused = client.send_message(
                    message, from_addr="no-reply@%s" % domain, to_addrs=[to])
        except (OSError, smtplib.SMTPException) as error:
            return False, "smtp %s" % type(error).__name__
        if refused:
            return False, "smtp refused"
        return True, "smtp %s" % message["Message-ID"]

    def mail_api(self, mail, sender, to, subject, text):
        token = self.mail_token(mail)
        if not token:
            return False, "no mailtrap token"
        payload = {"from": {"email": sender, "name": "ForkMesh IDS"},
                   "to": [{"email": to}], "subject": subject,
                   "text": text, "category": "Security alert"}
        request = urllib.request.Request(
            mail.get("url") or MAIL_URL, data=json.dumps(payload).encode(),
            method="POST", headers={"content-type": "application/json",
                                    "authorization": "Bearer " + token})
        try:
            with self.opener(request, timeout=15) as response:
                return (200 <= response.status < 300,
                        "mail %d" % response.status)
        except urllib.error.HTTPError as error:
            return False, "mail %d" % error.code
        except (OSError, urllib.error.URLError) as error:
            return False, type(error).__name__

    def mail(self, incident):
        mail = self.cfg.get("mail") or {}
        to = mail.get("to") or self.credential(mail.get("to_credential", ""))
        if "@" not in to:
            return False, "not configured"
        sender = mail.get("from") or "founders@forkmesh.com"
        subject = ("[fmnext-ids %s] %s: %s" % (
            incident["severity"], incident["node"], incident["summary"]))[:200]
        text = json.dumps(incident, indent=2, sort_keys=True,
                          default=str)[:6000]
        routes = [("postfix", self.mail_smtp), ("mailtrap", self.mail_api)]
        if self.locked_down():
            routes.reverse()
        note = {"kind": "ids_direct", "to": mask_address(to),
                "subject": subject, "body": text[:400], "from": sender}
        tried = []
        result = False, "not configured"
        for name, send in routes:
            ok, detail = send(mail, sender, to, subject, text)
            if detail in ("no smtp", "no mailtrap token"):
                continue
            tried.append("%s: %s" % (name, detail))
            result = ok, detail
            if ok:
                note["via"] = name
                break
        note["result"] = "accepted" if result[0] else (
            "failed" if tried else "not configured")
        note["tried"] = tried
        note["mailLog"] = self.mail_log(note, incident)
        return result

    def mail_log(self, note, incident):
        data = json.dumps({k: v for k, v in note.items()
                           if k != "mailLog"}, sort_keys=True)[:4000]
        if self.cfg.get("role") == "runner":
            runner = self.cfg.get("runner") or {}
            if not runner.get("app_url") or self.sign_request is None:
                return "no runner api"
            path = "/api/runners/mail-log-note"
            body = data.encode()
            try:
                headers = self.sign_request(path, body)
                request = urllib.request.Request(
                    runner["app_url"].rstrip("/") + path, data=body,
                    method="POST", headers=dict(headers, **{
                        "content-type": "application/json",
                        "user-agent": "fmnext-ids/1"}))
                with self.opener(request, timeout=15) as response:
                    return "api %d" % response.status
            except (OSError, ValueError, urllib.error.URLError) as error:
                log("mail log note not sent", error=type(error).__name__)
                return type(error).__name__
        if self.rqlite is None:
            return "no rqlite"
        try:
            self.rqlite.execute([[
                "INSERT INTO mail_log_node_notes(created_at, node, data) "
                "VALUES(?, ?, ?)", now_ms(), str(incident.get("node") or "")[:64],
                data]])
            return "recorded"
        except OSError as error:
            log("mail log note not recorded", error=str(error)[:120])
            return str(error)[:120]

    def webhook(self, incident, key):
        hook = self.cfg.get("webhook") or {}
        url = hook.get("url") or ""
        if not url.startswith("https://") or not key:
            return False, "not configured"
        body = json.dumps(incident, sort_keys=True, default=str).encode()
        ts = str(now_ms())
        webhook_key = hmac.new(key.encode(), b"fmnext-ids-webhook-v1",
                               hashlib.sha256).digest()
        mac = hmac.new(webhook_key, ts.encode() + b"\n" + body,
                       hashlib.sha256).hexdigest()
        request = urllib.request.Request(url, data=body, method="POST", headers={
            "content-type": "application/json",
            "x-fmnext-ids-signature": "v1:%s:%s" % (ts, mac)})
        try:
            with self.opener(request, timeout=10) as response:
                return 200 <= response.status < 300, "webhook %d" % \
                    response.status
        except (OSError, urllib.error.URLError) as error:
            return False, type(error).__name__

    def send(self, incident, key=""):
        """Fan out one incident; returns {channel: (ok, note)}."""
        log("incident", **{k: incident[k] for k in (
            "id", "node", "severity", "detector", "summary", "detail")})
        results = {"journal": (True, "logged")}
        if self.cfg.get("role") == "runner":
            results["api"] = self.post_runner(incident)
            recorded = results["api"][0]
        else:
            results["rqlite"] = self.record(incident)
            recorded = results["rqlite"][0]
        if incident["severity"] in ("lockdown", "test") or not recorded:
            results["mail"] = self.mail(incident)
        if incident["severity"] != "info":
            results["webhook"] = self.webhook(incident, key)
        return results


def runner_signer(cfg):
    """Sign /api/runners/* requests with the runner daemon's node key
    (the same canonical message as server/runner.go signedNodePost)."""
    runner = cfg.get("runner") or {}
    identity = runner.get("identity") or ""
    node = runner.get("node_name") or ""
    if not identity or not node:
        return None

    def sign(path, body):
        from cryptography.hazmat.primitives import serialization
        with open(identity, "rb") as handle:
            key = serialization.load_pem_private_key(handle.read(), None)
        request_id = ids_records.b64u(os.urandom(18))
        issued = str(now_ms())
        digest = hashlib.sha256(body).hexdigest()
        message = "\n".join(("forkmesh-masked-proxy-v1", node, "POST", path,
                             digest, request_id, issued))
        signature = ids_records.b64u(key.sign(message.encode()))
        return {"x-forkmesh-node": node, "x-forkmesh-request-id": request_id,
                "x-forkmesh-issued-at": issued,
                "x-forkmesh-body-sha256": digest,
                "x-forkmesh-signature": signature}
    return sign


# --------------------------------------------------------------------------
# the response state machine


class Responder:
    """normal -> lockdown (sticky until an operator release)."""

    def __init__(self, cfg, actions, alerter, store, key="", signer=None,
                 rqlite=None, clock=now_ms):
        self.cfg = cfg
        self.node = cfg["node"]
        self.role = cfg.get("role", "core")
        self.actions = actions
        self.alerter = alerter
        self.store = store
        self.key = key
        self.signer = signer          # () -> private key bytes (Ed25519)
        self.rqlite = rqlite
        self.clock = clock
        self.seen = {}
        self.recent = []
        state = store.load()
        self.state = state
        self.lockdown = os.path.exists(self.marker)

    @property
    def marker(self):
        return os.path.join(self.cfg["state_dir"], "LOCKDOWN")

    @property
    def rqlite_marker(self):
        return os.path.join(self.cfg["state_dir"], "RQLITE_STOPPED")

    def severity_of(self, event):
        override = (self.cfg.get("severity") or {}).get(event["detector"])
        return override if override in SEVERITY_RANK else event["severity"]

    def incident(self, event, severity):
        return {"id": ids_records.new_id(), "node": self.node,
                "ts": self.clock(), "severity": severity,
                "detector": event["detector"], "summary": event["summary"],
                "detail": event.get("detail") or {}}

    def handle(self, event):
        """Returns the incident (or None when deduplicated)."""
        severity = self.severity_of(event)
        now = time.monotonic()
        key = event.get("key") or event["summary"]
        last = self.seen.get(key)
        if last is not None and now - last < DEDUPE_SECONDS:
            return None
        self.seen[key] = now
        if len(self.seen) > 5000:
            self.seen = {k: v for k, v in self.seen.items()
                         if now - v < DEDUPE_SECONDS}
        incident = self.incident(event, severity)
        self.recent = (self.recent + [incident])[-50:]
        if severity == "lockdown" and not self.lockdown:
            self.enter_lockdown(incident)
        else:
            incident["delivery"] = self.alerter.send(incident, self.key)
        return incident

    # -- lockdown ---------------------------------------------------------
    def _write_marker(self, incident):
        os.makedirs(self.cfg["state_dir"], mode=0o700, exist_ok=True)
        with open(self.marker, "w", encoding="utf-8") as handle:
            json.dump({"incident": incident["id"], "ts": incident["ts"],
                       "summary": incident["summary"]}, handle)

    def self_quarantine_row(self, incident):
        private = self.signer() if self.signer else None
        if not private:
            return None
        body = ids_records.self_body(self.node, incident["ts"],
                                     incident["id"], incident["summary"])
        return ids_records.make_row(
            "self", self.node, self.node, body,
            ids_records.sign(private, "self", body))

    def firewall_text(self, lockdown=None, blocked=()):
        lock = self.lockdown if lockdown is None else lockdown
        cfg = self.cfg.get("lockdown") or {}
        dns_uid = None
        try:
            dns_uid = pwd.getpwnam("systemd-resolve").pw_uid
        except KeyError:
            pass
        return firewall_ruleset(
            blocked=blocked, lockdown=lock,
            ssh_allow=(self.cfg.get("ssh") or {}).get("allow") or (),
            keep_raft=lock and self.role == "core"
            and not os.path.exists(self.rqlite_marker),
            core_wg_ips=cfg.get("core_wg_ips") or (),
            core_public_ips=cfg.get("core_public_ips") or (),
            wg_port=cfg.get("wg_port", 51820),
            raft_port=cfg.get("raft_port", 4002), dns_uid=dns_uid)

    def enter_lockdown(self, incident):
        """Core: seal the app, marker, signed self-quarantine + incident
        record, firewall, stop the services, rqlite by the quorum rule, then
        the other alert channels. Runner: marker, kill the jobs, report
        (the app revokes the runner), firewall, stop the daemon."""
        self.lockdown = True
        steps = {}
        cfg = self.cfg.get("lockdown") or {}
        if self.role == "core":
            steps["seal"] = self.actions.seal_app()
        self._write_marker(incident)
        steps["marker"] = (True, self.marker)
        delivery = None
        if self.role == "core":
            row = self.self_quarantine_row(incident)
            if row is not None and self.rqlite is not None:
                try:
                    self.rqlite.execute([ids_records.insert_statement(row)])
                    steps["quarantine"] = (True, row["id"])
                except OSError as error:
                    steps["quarantine"] = (False, str(error)[:120])
            else:
                steps["quarantine"] = (False, "no signing key or rqlite")
            # Recorded before rqlite may stop: the peers page the admins.
            steps["record"] = self.alerter.record(incident)
        else:
            steps["jobs"] = self.actions.kill_jobs()
            delivery = self.alerter.send(incident, self.key)
            steps["report"] = delivery.get("api", (False, "?"))
        if self.role == "core":
            # Decided while the local rqlite still answers /nodes; the
            # firewall below then admits Raft only if rqlite stays.
            steps["rqlite"] = self.rqlite_decision()
        steps["firewall"] = self.actions.firewall(
            self.firewall_text(lockdown=True,
                               blocked=self.state.get("blocked") or ()))
        steps["stop"] = self.actions.stop_units(cfg.get("stop_units") or ())
        summary = {k: [bool(v[0]), str(v[1])[:120]] for k, v in steps.items()}
        incident["detail"] = dict(incident["detail"], lockdown=summary)
        log("LOCKDOWN", incident=incident["id"], steps=summary)
        if delivery is None:
            delivery = self.alerter.send(incident, self.key)
            if self.rqlite is not None:
                try:
                    self.rqlite.execute([[
                        "UPDATE security_incidents SET detail=? WHERE id=?",
                        json.dumps(incident["detail"], sort_keys=True,
                                   default=str)[:4000], incident["id"]]])
                except OSError:
                    pass
        incident["delivery"] = delivery
        self.state["lockdown"] = {"incident": incident["id"],
                                  "ts": incident["ts"]}
        self.store.save(self.state)
        return steps

    def rqlite_decision(self):
        """Stop the local rqlite only when the other voters keep quorum
        without it: other reachable voters that are a majority of all the
        voters (and at least two), a leader, and no
        other node already quarantined. Otherwise keep it (it holds only
        ciphertext for secrets; the lockdown firewall lets only the core
        peers reach Raft) so the IDS never takes the cluster down."""
        cfg = self.cfg.get("lockdown") or {}
        unit = cfg.get("rqlite_unit") or "rqlite.service"
        others_quarantined = [n for n in self.state.get("quarantined") or ()
                              if n != self.node]
        try:
            nodes = self.rqlite.nodes() if self.rqlite else []
        except OSError as error:
            return False, "kept: /nodes failed (%s)" % str(error)[:60]
        voters = [n for n in nodes if n.get("voter", True)]
        others = [n for n in voters if str(n.get("id")) != self.node
                  and n.get("reachable")]
        leader = any(n.get("leader") for n in voters)
        needed = max(2, len(voters) // 2 + 1)
        if others_quarantined or len(others) < needed or not leader:
            return False, "kept for quorum (%d other voters reachable%s)" % (
                len(others), ", another node quarantined"
                if others_quarantined else "")
        with open(self.rqlite_marker, "w", encoding="utf-8") as handle:
            handle.write("stopped by fmnext-ids\n")
        ok, note = self.actions.stop_units([unit])
        return ok, "stopped" if ok else note

    def reassert(self, blocked=()):
        """Every 30 s: the IDS table matches the state (nftables reloads
        flush it) and, in lockdown, the stopped units stay stopped."""
        ok, note = self.actions.firewall(self.firewall_text(blocked=blocked))
        if self.lockdown:
            units = list((self.cfg.get("lockdown") or {}).get(
                "stop_units") or ())
            self.actions.stop_units(units)
        return ok, note

    def release(self, row, operator_key):
        """Leave lockdown after a valid operator release row for this node
        (tools/next_ids.py release). Returns (ok, note)."""
        checked = ids_records.verified(row, {}, operator_key)
        if checked is None or checked[0] != "release" or \
                row.get("node_id") != self.node:
            return False, "not a valid operator release for %s" % self.node
        if not self.lockdown:
            return True, "not in lockdown"
        body = checked[1]
        lock_ts = int((self.state.get("lockdown") or {}).get("ts") or 0)
        if body["ts"] < lock_ts:
            return False, "the release predates this lockdown"
        cfg = self.cfg.get("lockdown") or {}
        started = []
        if os.path.exists(self.rqlite_marker):
            os.unlink(self.rqlite_marker)
            started.append(cfg.get("rqlite_unit") or "rqlite.service")
        os.unlink(self.marker)
        self.lockdown = False
        self.state.pop("lockdown", None)
        self.state["released"] = {"id": row.get("id"), "ts": body["ts"]}
        self.store.save(self.state)
        self.actions.firewall(self.firewall_text(
            lockdown=False, blocked=self.state.get("blocked") or ()))
        self.actions.start_units(started + list(cfg.get("stop_units") or ()))
        log("released", release=row.get("id"))
        return True, "released"


class StateStore:
    def __init__(self, path):
        self.path = path

    def load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                return json.load(handle)
        except (OSError, ValueError):
            return {}

    def save(self, state):
        os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(state, handle, sort_keys=True)
        os.replace(tmp, self.path)


# --------------------------------------------------------------------------
# peers (core): heartbeats, votes, quarantine enforcement, voter removal


class PeerWatch:
    def __init__(self, cfg, rqlite, responder, signer, clock=now_ms):
        self.cfg = cfg
        self.node = cfg["node"]
        self.rqlite = rqlite
        self.responder = responder
        self.signer = signer
        self.clock = clock
        hb = cfg.get("heartbeat") or {}
        self.stale_ms = int(hb.get("stale_ms", 180_000))
        self.nodes = list(cfg.get("nodes") or ())
        self.node_keys = dict(cfg.get("node_public_keys") or {})
        self.operator_key = cfg.get("operator_public_key") or ""
        self.unreachable_since = {}

    def beat(self):
        state = "lockdown" if self.responder.lockdown else "normal"
        self.rqlite.execute([[
            "INSERT INTO ids_heartbeats(node_id, beat_ms, state, detail) "
            "VALUES(?, ?, ?, ?) ON CONFLICT(node_id) DO UPDATE SET "
            "beat_ms=excluded.beat_ms, state=excluded.state, "
            "detail=excluded.detail", self.node, self.clock(), state, ""]])

    def silent_peers(self, members, beats, now, seen=()):
        """Peers whose app heart-beats (fresh cluster_members row, running
        longer than the stale window) while their IDS row is stale. Only
        nodes whose IDS has reported before (``seen``, remembered by this
        IDS, so deleting rows does not help): a node that never ran the IDS
        (first rollout, a fresh replacement) is not watched."""
        silent = []
        for node in self.nodes:
            if node == self.node or node not in seen:
                continue
            member = members.get(node) or {}
            app_beat = int(member.get("heartbeat_ms") or 0)
            epoch = int(member.get("epoch") or 0)
            ids_beat = int((beats.get(node) or {}).get("beat_ms") or 0)
            if now - app_beat <= 30_000 and now - epoch > self.stale_ms \
                    and now - ids_beat > self.stale_ms:
                silent.append(node)
        return silent

    def vote(self, target, reason):
        private = self.signer() if self.signer else None
        if not private:
            return None
        body = ids_records.vote_body(self.node, target, self.clock(), reason)
        row = ids_records.make_row("vote", target, self.node, body,
                                   ids_records.sign(private, "vote", body))
        self.rqlite.execute([ids_records.insert_statement(row)])
        return row

    def quarantine(self, rows, sticky):
        state = ids_records.quarantine_state(rows, self.node_keys,
                                             self.operator_key, self.nodes)
        return ids_records.merge_sticky(state, sticky)

    def round(self):
        """One peer round; returns events and the verified state."""
        events = []
        now = self.clock()
        self.beat()
        members = {r["node_id"]: r for r in self.rqlite.query(
            "SELECT node_id, heartbeat_ms, epoch, status FROM "
            "cluster_members")}
        beats = {r["node_id"]: r for r in self.rqlite.query(
            "SELECT node_id, beat_ms, state FROM ids_heartbeats")}
        rows = self.rqlite.query(ids_records.SELECT_SQL)
        state, sticky = self.quarantine(rows, self.responder.state.get(
            "sticky") or {})
        quarantined = sorted(ids_records.quarantined_nodes(state))
        seen = self.responder.state.setdefault("ids_seen", [])
        for node in beats:
            if node in self.nodes and node not in seen:
                seen.append(node)
        voted = self.responder.state.setdefault("voted", {})
        for target in self.silent_peers(members, beats, now, seen):
            if target in quarantined:
                continue
            last = int(voted.get(target) or 0)
            released = int(((state.get(target) or {}).get("release") or {})
                           .get("ts") or 0)
            if last > released and now - last < 3600_000:
                continue
            row = self.vote(target, "ids heartbeat silent for %d s while "
                            "the app runs" % (self.stale_ms // 1000))
            if row:
                voted[target] = now
                rows = list(rows) + [row]
                events.append(Event("peer_ids_silent", "alert",
                                    "The IDS on %s stopped reporting while "
                                    "its app runs; voted to quarantine it"
                                    % target, {"target": target}))
        if any(e["detector"] == "peer_ids_silent" for e in events):
            state, sticky = self.quarantine(rows, self.responder.state.get(
                "sticky") or {})
            quarantined = sorted(ids_records.quarantined_nodes(state))
        self.responder.state["sticky"] = sticky
        self.responder.state["quarantined"] = quarantined
        released = int((self.responder.state.get("released") or {})
                       .get("ts") or 0)
        evidence = (state.get(self.node) or {}).get("evidence") or {}
        fresh = [ts for ts in evidence.values() if int(ts) > released]
        if self.node in quarantined and fresh and not self.responder.lockdown:
            events.append(Event("quarantined_by_peers", "lockdown",
                                "This node is quarantined (%s)" % "; ".join(
                                    state[self.node]["reasons"]),
                                {"reasons": state[self.node]["reasons"]}))
        return events, state

    def blocked_ips(self, quarantined):
        ips = (self.cfg.get("lockdown") or {}).get("core_wg_ips_by_node") or {}
        return sorted(ips[n] for n in quarantined
                      if n in ips and n != self.node)

    def maybe_remove_voter(self, quarantined):
        """Remove a quarantined node as an rqlite voter, from one healthy
        peer (rendezvous-first), when it has been unreachable for 2 minutes
        and at least two reachable voters remain. Never shrinks below two."""
        if not self.cfg.get("remove_voter", True) or self.responder.lockdown:
            return []
        try:
            nodes = self.rqlite.nodes()
        except OSError:
            return []
        voters = [n for n in nodes if n.get("voter", True)]
        events = []
        now = self.clock()
        for target in quarantined:
            if target == self.node:
                continue
            entry = next((n for n in voters if str(n.get("id")) == target),
                         None)
            if entry is None:
                continue
            if entry.get("reachable"):
                self.unreachable_since.pop(target, None)
                continue
            since = self.unreachable_since.setdefault(target, now)
            remaining = [n for n in voters if str(n.get("id")) != target]
            if now - since < 120_000 or len(remaining) < 2 or \
                    not all(n.get("reachable") for n in remaining) or \
                    not any(n.get("leader") for n in remaining):
                continue
            healthy = sorted(str(n.get("id")) for n in remaining
                             if str(n.get("id")) not in quarantined)
            chosen = sorted(healthy, key=lambda n: hashlib.sha256(
                ("ids:remove:%s:%s" % (target, n)).encode()).digest(),
                reverse=True)
            if not chosen or chosen[0] != self.node:
                continue
            try:
                self.rqlite.remove(target)
            except OSError as error:
                log("voter removal failed", target=target,
                    error=str(error)[:120])
                continue
            events.append(Event("voter_removed", "alert",
                                "Removed the quarantined node %s as an rqlite "
                                "voter (%d voters remain)" % (
                                    target, len(remaining)),
                                {"target": target}))
        return events


# --------------------------------------------------------------------------
# the daemon


def journal_command(cursor=None):
    argv = ["journalctl", "--follow", "--output=json", "--no-pager"]
    argv += ["--after-cursor=%s" % cursor] if cursor else ["--lines=0"]
    matches = ["SYSLOG_IDENTIFIER=%s" % i for i in
               SSHD_IDENTIFIERS + ACCOUNT_TOOLS] + ["_TRANSPORT=kernel"]
    for index, match in enumerate(matches):
        if index:
            argv.append("+")
        argv.append(match)
    return argv


class Daemon:
    def __init__(self, cfg, environ=None):
        self.cfg = cfg
        self.node = cfg["node"]
        self.role = cfg.get("role", "core")
        self.key = read_credential(cfg.get("key_credential",
                                           "FMNEXT_IDS_KEY"), environ)
        self.state_dir = cfg["state_dir"]
        os.makedirs(self.state_dir, mode=0o700, exist_ok=True)
        self.store = StateStore(os.path.join(self.state_dir, "state.json"))
        rq = cfg.get("rqlite") or {}
        self.rqlite = None
        if self.role == "core" and rq.get("url"):
            self.rqlite = Rqlite(
                [rq["url"]] + list((rq.get("peers") or {}).values()),
                rq.get("user", ""), read_credential(
                    rq.get("password_credential", "RQLITE_PASSWORD"),
                    environ))
        self.actions = Actions(cfg)
        self.alerter = Alerter(cfg, self.rqlite,
                               sign_request=runner_signer(cfg)
                               if self.role == "runner" else None)
        self.responder = Responder(cfg, self.actions, self.alerter,
                                   self.store, self.key, self.signing_key,
                                   self.rqlite)
        self.expectations = Expectations(os.path.join(self.state_dir,
                                                      "expect"))
        self.integrity = Integrity(cfg)
        self.ssh = SshMonitor(cfg)
        self.processes = ProcessMonitor(cfg)
        self.peers = PeerWatch(cfg, self.rqlite, self.responder,
                               self.signing_key) if self.rqlite else None
        self.baseline = {}
        self.modules = None
        self.expected_modules = frozenset(
            (cfg.get("modules") or {}).get("expected") or ())
        self.started = time.monotonic()
        self.timers = {}
        self.journal = None
        self.control = None
        self.quarantine_state = {}

    # -- keys and baseline -------------------------------------------------
    def signing_key(self):
        path = os.path.join(self.state_dir, "sign.key")
        try:
            return ids_records.load_private(path)
        except (OSError, ids_records.RecordError):
            return None

    @property
    def baseline_path(self):
        return os.path.join(self.state_dir, "baseline.json")

    def _baseline_mac(self, entries):
        text = json.dumps(entries, sort_keys=True, separators=(",", ":"))
        return hmac.new((self.key or "no-key").encode(), text.encode(),
                        hashlib.sha256).hexdigest()

    def load_baseline(self):
        try:
            with open(self.baseline_path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            return None
        entries = data.get("entries") or {}
        if not hmac.compare_digest(str(data.get("mac") or ""),
                                   self._baseline_mac(entries)):
            return False
        return entries

    def save_baseline(self):
        tmp = self.baseline_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump({"entries": self.baseline,
                       "mac": self._baseline_mac(self.baseline)}, handle)
        os.replace(tmp, self.baseline_path)

    def init_baseline(self):
        loaded = self.load_baseline()
        state = self.responder.state
        if loaded is False:
            self.responder.handle(Event(
                "baseline_tampered", "lockdown",
                "The integrity baseline failed its HMAC check"))
            loaded = None
        if loaded is None:
            if state.get("baseline_created_ms"):
                self.responder.handle(Event(
                    "baseline_missing", "lockdown",
                    "The integrity baseline disappeared"))
            self.baseline = self.integrity.scan("all")
            self.save_baseline()
            state["baseline_created_ms"] = now_ms()
            self.store.save(state)
            self.responder.handle(Event(
                "baseline_initialised", "info",
                "Integrity baseline created (%d paths)" % len(self.baseline)))
        else:
            self.baseline = loaded
        if "accounts" not in state:
            state["accounts"] = self.integrity.read_account_files(
                list(ACCOUNT_FILES))
            self.store.save(state)

    # -- scans -------------------------------------------------------------
    def integrity_round(self, group, rehash=False):
        self.expectations.load()
        current = self.integrity.scan(group, rehash=rehash)
        in_group = (lambda p: self.integrity.in_group(p, group, self.baseline)
                    or p in current)
        changes = compare(self.baseline, current, in_group)
        if not changes:
            return []
        account_paths = [p for p, _, _ in changes if p in ACCOUNT_FILES]
        before = self.responder.state.get("accounts") or {}
        after = self.integrity.read_account_files(account_paths)
        events, accepted = change_events(self.integrity, changes,
                                         self.expectations, before, after)
        # The new state is the baseline from now on: an incident is raised
        # once per change, not on every scan.
        for path, _old, new in changes:
            if new is None:
                self.baseline.pop(path, None)
            else:
                self.baseline[path] = new
        if after:
            accounts = dict(before)
            accounts.update(after)
            self.responder.state["accounts"] = accounts
            self.store.save(self.responder.state)
        self.save_baseline()
        if accepted:
            log("accepted expected changes", count=len(accepted),
                sample=accepted[:5])
        return events

    def module_round(self):
        current = loaded_modules()
        if self.modules is None:
            if time.monotonic() - self.started < 120:
                return []
            self.modules = current
            return []
        new = sorted(current - self.modules - self.expected_modules)
        self.modules |= current
        if not new or self.expectations.allows("modules"):
            return []
        return [Event("kernel_module_loaded", "alert",
                      "Kernel module(s) loaded: %s" % ", ".join(new[:10]),
                      {"modules": new[:20]})]

    def reboot_check(self):
        boot = (_read("/proc/sys/kernel/random/boot_id") or "").strip()
        state = self.responder.state
        previous = state.get("boot_id")
        state["boot_id"] = boot
        self.store.save(state)
        self.expectations.load()
        if previous and previous != boot and not \
                self.expectations.allows("reboot"):
            return [Event("reboot", "info",
                          "The node rebooted outside a deploy window")]
        return []

    def peer_round(self):
        if self.peers is None:
            return []
        try:
            events, state = self.peers.round()
        except OSError as error:
            log("peer round failed", error=str(error)[:120])
            return []
        self.quarantine_state = state
        quarantined = set(ids_records.quarantined_nodes(state))
        blocked = self.peers.blocked_ips(quarantined)
        previous = self.responder.state.get("blocked") or []
        if blocked != previous:
            self.responder.state["blocked"] = blocked
            log("peer blocks", blocked=blocked)
        self.responder.reassert(blocked=blocked)
        self.store.save(self.responder.state)
        events += self.peers.maybe_remove_voter(sorted(quarantined))
        return events

    # -- control socket ----------------------------------------------------
    def control_request(self, request):
        command = request.get("command")
        if command == "status":
            return self.status()
        if command == "expect":
            manifest = request.get("manifest") or {}
            used = self.responder.state.setdefault("nonces", {})
            now = now_ms()
            for nonce in [n for n, exp in used.items() if exp < now]:
                used.pop(nonce, None)
            reason = verify_manifest(manifest, self.key, self.node, now,
                                     used)
            if reason:
                self.responder.handle(Event(
                    "expectation_rejected", "alert",
                    "An expected-change manifest was rejected (%s)" % reason,
                    {"reason": reason}))
                return {"ok": False, "error": reason}
            used[manifest["nonce"]] = int(manifest["expires_ms"])
            self.store.save(self.responder.state)
            self.expectations.add(manifest)
            log("expectation accepted", reason=str(manifest.get(
                "reason", ""))[:120], files=len(manifest.get("files") or {}),
                rescan=manifest.get("rescan"), allow=manifest.get("allow"))
            return {"ok": True, "nonce": manifest["nonce"]}
        if command == "rescan":
            events = self.integrity_round("hot") + \
                self.integrity_round("normal")
            for event in events:
                self.responder.handle(event)
            return {"ok": True, "events": len(events)}
        if command == "test-fire":
            incident = self.responder.handle(Event(
                "test_fire", "test",
                "Test alert from %s (harmless; tools/next_ids.py "
                "test-fire)" % self.node, {"nonce": os.urandom(4).hex()},
                key="test:%s" % os.urandom(8).hex()))
            return {"ok": True, "incident": incident["id"],
                    "delivery": incident.get("delivery")}
        if command == "incident-status":
            if self.rqlite is None:
                return {"ok": False, "error": "no rqlite on this node"}
            rows = self.rqlite.query(
                "SELECT id, node_id, severity, delivered_ms, delivery FROM "
                "security_incidents WHERE id=?", str(request.get("id")),
                level="strong")
            return {"ok": True, "incident": rows[0] if rows else None}
        if command == "release":
            ok, note = self.responder.release(
                request.get("row") or {}, self.cfg.get(
                    "operator_public_key") or "")
            if ok:
                self.responder.handle(Event(
                    "released", "info", "Lockdown released by the operator",
                    {"release": (request.get("row") or {}).get("id")}))
            return {"ok": ok, "note": note}
        return {"ok": False, "error": "unknown_command"}

    def status(self):
        state = self.responder.state
        return {"ok": True, "node": self.node, "role": self.role,
                "lockdown": self.responder.lockdown,
                "lockdownIncident": state.get("lockdown"),
                "baselinePaths": len(self.baseline),
                "expectations": len(self.expectations.items),
                "quarantined": state.get("quarantined") or [],
                "blocked": state.get("blocked") or [],
                "keys": {"expect": bool(self.key),
                         "sign": self.signing_key() is not None},
                "recent": [{k: i.get(k) for k in ("id", "ts", "severity",
                                                   "detector", "summary")}
                           for i in self.responder.recent[-15:]]}

    def serve_control(self):
        run_dir = self.cfg.get("run_dir") or "/run/fmnext-ids"
        os.makedirs(run_dir, mode=0o700, exist_ok=True)
        path = os.path.join(run_dir, "control.sock")
        try:
            os.unlink(path)
        except OSError:
            pass
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        old = os.umask(0o177)
        try:
            server.bind(path)
        finally:
            os.umask(old)
        server.listen(4)
        server.setblocking(False)
        self.control = server

    def handle_control(self):
        try:
            conn, _ = self.control.accept()
        except OSError:
            return
        with conn:
            conn.settimeout(10)
            creds = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED,
                                    12)
            uid = int.from_bytes(creds[4:8], sys.byteorder)
            data = b""
            try:
                while not data.endswith(b"\n") and len(data) < 1 << 20:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                if uid not in (0, os.getuid()):   # root (the daemon's own)
                    reply = {"ok": False, "error": "forbidden"}
                else:
                    reply = self.control_request(json.loads(data or b"{}"))
            except (ValueError, OSError) as error:
                reply = {"ok": False, "error": type(error).__name__}
            try:
                conn.sendall(json.dumps(reply, default=str).encode() + b"\n")
            except OSError:
                pass

    # -- loop --------------------------------------------------------------
    def start_journal(self):
        cursor = self.responder.state.get("cursor")
        self.journal = subprocess.Popen(
            journal_command(cursor), stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        self._buffer = b""

    def read_journal(self):
        events = []
        chunk = os.read(self.journal.stdout.fileno(), 1 << 16)
        if not chunk:
            self.journal.wait()
            self.journal = None
            return events
        self._buffer += chunk
        *lines, self._buffer = self._buffer.split(b"\n")
        for line in lines:
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            self.responder.state["cursor"] = entry.get("__CURSOR")
            events += self.ssh.feed(entry)
            events += parse_kernel(entry)
            events += parse_account_tool(entry)
        return events

    def due(self, name, seconds):
        now = time.monotonic()
        if now - self.timers.get(name, -1e9) >= seconds:
            self.timers[name] = now
            return True
        return False

    def tick(self):
        events = []
        integrity = self.cfg.get("integrity") or {}
        if self.due("processes", 5):
            self.expectations.load()
            events += self.processes.scan(
                allow_ptrace=self.expectations.allows("ptrace"),
                allow_processes=self.expectations.allows("processes"))
            events += self.ssh.tick()
        if self.due("hot", integrity.get("hot_interval", 10)):
            events += self.integrity_round("hot")
        if self.due("normal", integrity.get("interval", 60)):
            rehash = self.due("rehash", 3600 * float(
                integrity.get("full_rehash_hours", 6)))
            events += self.integrity_round("normal", rehash=rehash)
            if not self.expectations.allows("listeners"):
                events += listener_events(listening_rows("/proc/net"),
                                          self.cfg, proc="/proc")
            events += self.module_round()
        if self.due("peers", (self.cfg.get("heartbeat") or {}).get(
                "interval", 30)):
            events += self.peer_round()
            if self.peers is None:
                self.responder.reassert(
                    blocked=self.responder.state.get("blocked") or ())
            self.store.save(self.responder.state)
        return events

    def run(self):
        self.serve_control()
        if self.responder.lockdown:
            log("starting in LOCKDOWN (marker present)")
            self.responder.reassert(self.responder.state.get("blocked") or ())
        for event in self.reboot_check():
            self.responder.handle(event)
        self.init_baseline()
        self.start_journal()
        log("started", node=self.node, role=self.role,
            baseline=len(self.baseline), lockdown=self.responder.lockdown,
            expect_key=bool(self.key), sign_key=self.signing_key() is not None)
        while True:
            if self.journal is None:
                self.start_journal()
            fds = [self.control]
            if self.journal is not None:
                fds.append(self.journal.stdout)
            ready, _, _ = select.select(fds, [], [], 1.0)
            events = []
            if self.control in ready:
                self.handle_control()
            if self.journal is not None and self.journal.stdout in ready:
                events += self.read_journal()
            try:
                events += self.tick()
            except Exception as error:  # a scan bug must not stop the IDS
                log("scan failed", error=repr(error)[:200])
            for event in events:
                try:
                    self.responder.handle(event)
                except Exception as error:
                    log("response failed", error=repr(error)[:200])


# --------------------------------------------------------------------------
# CLI


def control(cfg, request, timeout=120):
    path = os.path.join(cfg.get("run_dir") or "/run/fmnext-ids",
                        "control.sock")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(timeout)
            client.connect(path)
            client.sendall(json.dumps(request).encode() + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                chunk = client.recv(65536)
                if not chunk:
                    break
                data += chunk
    except (FileNotFoundError, ConnectionRefusedError):
        return {"ok": False, "error": "not_running"}
    return json.loads(data or b"{}")


def cmd_keygen(cfg):
    path = os.path.join(cfg["state_dir"], "sign.key")
    os.makedirs(cfg["state_dir"], mode=0o700, exist_ok=True)
    if not os.path.exists(path):
        raw = ids_records.generate_private()
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write(ids_records.b64u(raw) + "\n")
    print(ids_records.public_key(ids_records.load_private(path)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="fmnext-ids")
    parser.add_argument("--config", default=CONFIG_PATH)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "keygen", "expect", "status", "test-fire", "release",
                 "rescan"):
        sub.add_parser(name)
    p = sub.add_parser("incident-status")
    p.add_argument("id")
    args = parser.parse_args(argv)
    cfg = load_config(args.config)
    if args.command == "run":
        Daemon(cfg).run()
        return 0
    if args.command == "keygen":
        return cmd_keygen(cfg)
    request = {"command": args.command}
    if args.command == "expect":
        request["manifest"] = json.loads(sys.stdin.read(1 << 20) or "{}")
    elif args.command == "release":
        request["row"] = json.loads(sys.stdin.read(1 << 20) or "{}")
    elif args.command == "incident-status":
        request["id"] = args.id
    reply = control(cfg, request)
    print(json.dumps(reply, sort_keys=True, default=str))
    if reply.get("error") == "not_running":
        return 3
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
