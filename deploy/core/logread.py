#!/usr/bin/python3 -I
import ctypes
import grp
import json
import os
import pwd
import re
import selectors
import signal
import subprocess
import sys
import time

PROTOCOL = 1
JOURNALCTL = "/usr/bin/journalctl"
CONFIG = "/etc/forkmesh-next/ids.json"
FIELDS = ("MESSAGE", "PRIORITY", "SYSLOG_IDENTIFIER", "_SYSTEMD_UNIT",
          "_TRANSPORT")
CURSOR_RE = re.compile(r"^[A-Za-z0-9=;_-]{1,256}$")
NODE_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
DEFAULT_LINES = 200
MAX_LINES = 5000
MAX_MESSAGE = 4000
IDLE_SECONDS = 60
VITALS_SECONDS = 15
PROC = "/proc"
MEMINFO_FIELDS = {"MemTotal": "mem_total", "MemAvailable": "mem_available",
                  "SwapTotal": "swap_total", "SwapFree": "swap_free"}
READER_USER = "nobody"
JOURNAL_GROUP = "systemd-journal"
PR_SET_NO_NEW_PRIVS = 38

SECRET_RULES = (
    ("github-token", r"\bgh[pousr]_[A-Za-z0-9_]{36,}\b", 0),
    ("github-fine-grained-token", r"\bgithub_pat_[A-Za-z0-9_]{22,}\b", 0),
    ("aws-access-key", r"\bAKIA[0-9A-Z]{16,}\b", 0),
    ("aws-temporary-access-key", r"\bASIA[0-9A-Z]{16,}\b", 0),
    ("slack-token", r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b", 0),
    ("openai-api-key", r"\bsk-proj-[A-Za-z0-9_-]{20,}\b|\bsk-[A-Za-z0-9]{48,}\b",
     0),
    ("anthropic-api-key", r"\bsk-ant-[A-Za-z0-9_-]{20,}\b", 0),
    ("stripe-secret-key", r"\bsk_(live|test)_[A-Za-z0-9]{24,}\b", 0),
    ("stripe-restricted-key", r"\brk_(live|test)_[A-Za-z0-9]{24,}\b", 0),
    ("google-api-key", r"\bAIza[A-Za-z0-9_-]{35,}\b", 0),
    ("google-oauth-token", r"\bya29\.[A-Za-z0-9_-]{20,}\b", 0),
    ("google-oauth-client-secret", r"\bGOCSPX-[A-Za-z0-9_-]{28,}\b", 0),
    ("sendgrid-api-key", r"\bSG\.[A-Za-z0-9_-]{22,}\.[A-Za-z0-9_-]{43,}\b", 0),
    ("twilio-api-key", r"\bSK[a-f0-9]{32,}\b", 0),
    ("npm-access-token", r"\bnpm_[A-Za-z0-9]{36,}\b", 0),
    ("vault-token", r"\bhv[sbr]\.[A-Za-z0-9]{24,}\b", 0),
    ("cloudflare-api-token",
     r"(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)"
     r'''\s*=\s*['"]?([A-Za-z0-9_-]{40,})['"]?''', 1),
    ("private-key-material",
     r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY"
     r"(?:-----| BLOCK-----)", 0),
    ("generic-secret-assignment",
     r"(?i)(?:password|passwd|api[_\-]?(?:key|secret|token)|"
     r"auth[_\-]?token|secret[_\-]?key|access[_\-]?token|"
     r'''private[_\-]?key)\s*[:=]\s*['"]([^'"\s]{20,})['"]''', 1),
)
_COMPILED_RULES = tuple((rule_id, re.compile(pattern), group)
                        for rule_id, pattern, group in SECRET_RULES)
_CREDENTIAL_WORDS = (
    r"authorization|proxy-authorization|cookie|set-cookie|bearer|password|"
    r"passwd|secret|token|api[_-]?key|api[_-]?token|api[_-]?secret|"
    r"access[_-]?token|refresh[_-]?token|auth[_-]?token|private[_-]?key|"
    r"data[_-]?key|signature|sig")
_SCRUBS = (
    (re.compile(r'(?i)("(?:%s|x-[a-z0-9-]*(?:token|secret|signature|key|auth)'
                r'[a-z0-9-]*)"\s*:\s*)(\[[^\]]*\]|"(?:[^"\\]|\\.)*")'
                % _CREDENTIAL_WORDS), r'\1"<redacted>"'),
    (re.compile(r"(?i)\bBearer\s+[A-Za-z0-9._~+/=-]{8,}"), "Bearer <redacted>"),
    (re.compile(r"(?i)\bBasic\s+[A-Za-z0-9+/=]{12,}"), "Basic <redacted>"),
    (re.compile(r"(?i)([?&;](?:token|access_token|refresh_token|sig|signature|"
                r"key|api_key|code|password|secret|auth|session)=)[^&\s\"'#]+"),
     r"\1<redacted>"),
    (re.compile(r"(?i)\b(%s)(\s*[:=]\s*)(?!<redacted>)[^\s,;&\"'}\]]+"
                % _CREDENTIAL_WORDS), r"\1\2<redacted>"),
    (re.compile(r"(?i)(://[^/\s:@]+:)[^@\s/]+@"), r"\1<redacted>@"),
    (re.compile(r"\b(?=[A-Za-z0-9_-]*[A-Z])(?=[A-Za-z0-9_-]*[a-z])"
                r"(?=[A-Za-z0-9_-]*[0-9])[A-Za-z0-9_-]{40,}\b"),
     "<redacted-token>"),
)


def redact(text):
    text = str(text or "")
    for rule_id, regex, group in _COMPILED_RULES:
        if rule_id == "private-key-material":
            if regex.search(text):
                return "<redacted: private key material>"
            continue
        if group:
            text = regex.sub(lambda m, g=group, r=rule_id: m.group(0).replace(
                m.group(g), "<redacted:%s>" % r), text)
        else:
            text = regex.sub("<redacted:%s>" % rule_id, text)
    for regex, replacement in _SCRUBS:
        text = regex.sub(replacement, text)
    return text


def parse_request(text):
    words = str(text or "").split()
    if not words or words == ["follow"]:
        return {"mode": "follow", "lines": DEFAULT_LINES}
    if words == ["hello"]:
        return {"mode": "hello"}
    if len(words) == 3 and words[0] == "follow":
        if words[1] == "after" and CURSOR_RE.match(words[2]):
            return {"mode": "follow", "cursor": words[2]}
        if words[1] == "lines" and words[2].isdigit():
            return {"mode": "follow",
                    "lines": max(0, min(int(words[2]), MAX_LINES))}
    raise ValueError("unsupported request")


def journal_command(request, journalctl=JOURNALCTL):
    command = [journalctl, "--follow", "--output=json", "--all", "--no-pager",
               "--quiet", "--output-fields=" + ",".join(FIELDS)]
    if request.get("cursor"):
        command.append("--after-cursor=" + request["cursor"])
    else:
        command.append("--lines=%d" % int(request.get("lines", DEFAULT_LINES)))
    return command


def message_text(value):
    if isinstance(value, list):
        try:
            return bytes(int(b) & 0xFF for b in value).decode("utf-8",
                                                            "replace")
        except (TypeError, ValueError):
            return ""
    if value is None:
        return ""
    return str(value)


def transform(raw):
    try:
        entry = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(entry, dict) or not entry.get("__CURSOR"):
        return None
    message = redact(message_text(entry.get("MESSAGE")).rstrip())
    if len(message) > MAX_MESSAGE:
        message = message[:MAX_MESSAGE - 3] + "..."
    out = {"__CURSOR": str(entry["__CURSOR"]),
           "__REALTIME_TIMESTAMP": str(entry.get("__REALTIME_TIMESTAMP") or ""),
           "MESSAGE": message}
    for field in ("PRIORITY", "SYSLOG_IDENTIFIER", "_SYSTEMD_UNIT",
                  "_TRANSPORT"):
        value = entry.get(field)
        if isinstance(value, str) and value:
            out[field] = value[:200]
    return out


def identity(config_path=CONFIG):
    try:
        with open(config_path, encoding="utf-8") as handle:
            config = json.load(handle)
    except (OSError, ValueError):
        config = {}
    node = str(config.get("node") or "")
    role = str(config.get("role") or "")
    return {"node": node if NODE_RE.match(node) else "",
            "role": role if role in ("core", "runner") else ""}


def drop_privileges():
    if os.geteuid() != 0:
        return
    user = pwd.getpwnam(READER_USER)
    journal = grp.getgrnam(JOURNAL_GROUP).gr_gid
    os.setgroups([journal])
    os.setresgid(user.pw_gid, user.pw_gid, user.pw_gid)
    os.setresuid(user.pw_uid, user.pw_uid, user.pw_uid)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "prctl(PR_SET_NO_NEW_PRIVS)")
    if 0 in os.getresuid() or 0 in os.getresgid():
        raise OSError("still root after dropping privileges")


def read_text(path):
    try:
        with open(path, encoding="ascii", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def cpu_ticks(proc=PROC):
    fields = read_text(proc + "/stat").split("\n", 1)[0].split()
    if len(fields) < 5 or fields[0] != "cpu":
        return None
    try:
        ticks = [int(value) for value in fields[1:]]
    except ValueError:
        return None
    idle = ticks[3] + (ticks[4] if len(ticks) > 4 else 0)
    return sum(ticks), idle


class Vitals:
    def __init__(self, proc=PROC, root="/", clock=time.time):
        self.proc = proc
        self.root = root
        self.clock = clock
        self.last_ticks = cpu_ticks(proc)

    def sample(self):
        record = {"logread_vitals": int(self.clock() * 1000),
                  "cpus": os.cpu_count() or 1}
        for line in read_text(self.proc + "/meminfo").splitlines():
            name, _, rest = line.partition(":")
            words = rest.split()
            if name in MEMINFO_FIELDS and words and words[0].isdigit():
                record[MEMINFO_FIELDS[name]] = int(words[0]) * 1024
        load = read_text(self.proc + "/loadavg").split()
        if len(load) >= 4:
            try:
                record["load"] = [float(value) for value in load[:3]]
                running, _, total = load[3].partition("/")
                record["procs_running"] = int(running)
                record["procs_total"] = int(total)
            except ValueError:
                record.pop("load", None)
        files = read_text(self.proc + "/sys/fs/file-nr").split()
        if len(files) >= 3 and all(value.isdigit() for value in files[:3]):
            record["files_open"] = int(files[0]) - int(files[1])
            record["files_max"] = int(files[2])
        uptime = read_text(self.proc + "/uptime").split()
        if uptime:
            try:
                record["uptime"] = int(float(uptime[0]))
            except ValueError:
                pass
        try:
            disk = os.statvfs(self.root)
        except OSError:
            disk = None
        if disk is not None:
            record["disk_total"] = disk.f_blocks * disk.f_frsize
            record["disk_free"] = disk.f_bavail * disk.f_frsize
            record["inodes_total"] = disk.f_files
            record["inodes_free"] = disk.f_favail
        ticks = cpu_ticks(self.proc)
        if ticks is not None and self.last_ticks is not None:
            total = ticks[0] - self.last_ticks[0]
            idle = ticks[1] - self.last_ticks[1]
            if total > 0:
                record["cpu_busy"] = round(100.0 * (total - idle) / total, 1)
        if ticks is not None:
            self.last_ticks = ticks
        return record


def emit(out, record):
    out.write(json.dumps(record, separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8") + b"\n")
    out.flush()


def serve(request_text, out, journalctl=JOURNALCTL, config_path=CONFIG,
          drop=drop_privileges, idle_seconds=IDLE_SECONDS,
          clock=time.time, vitals=None, vitals_seconds=VITALS_SECONDS):
    try:
        request = parse_request(request_text)
    except ValueError:
        emit(out, {"logread": PROTOCOL, "error": "unsupported request"})
        return 2
    header = dict(identity(config_path), logread=PROTOCOL)
    drop()
    if request["mode"] == "hello":
        emit(out, header)
        return 0
    child = subprocess.Popen(journal_command(request, journalctl),
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL)

    def stop():
        if child.poll() is None:
            child.terminate()

    emit(out, dict(header, resumed=bool(request.get("cursor"))))
    if vitals is None:
        vitals = Vitals(clock=clock)
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    pending = b""
    try:
        emit(out, vitals.sample())
        next_vitals = clock() + vitals_seconds
        last_output = clock()
        while True:
            now = clock()
            wait = min(next_vitals, last_output + idle_seconds) - now
            ready = selector.select(timeout=max(0.0, wait))
            now = clock()
            if now >= next_vitals:
                emit(out, vitals.sample())
                next_vitals = now + vitals_seconds
            if not ready:
                if now - last_output >= idle_seconds:
                    emit(out, {"logread_idle": int(now * 1000)})
                    last_output = now
                continue
            last_output = now
            chunk = os.read(child.stdout.fileno(), 65536)
            if not chunk:
                break
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for line in lines:
                record = transform(line)
                if record is not None:
                    emit(out, record)
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        stop()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
    code = child.returncode
    if code not in (0, None, -signal.SIGTERM):
        try:
            emit(out, {"logread_error": "journalctl exited %d" % code})
        except (BrokenPipeError, ConnectionResetError):
            pass
        return 1
    return 0


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    return serve(os.environ.get("SSH_ORIGINAL_COMMAND", ""),
                 sys.stdout.buffer)


if __name__ == "__main__":
    sys.exit(main())
