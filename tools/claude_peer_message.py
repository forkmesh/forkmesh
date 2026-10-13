#!/usr/bin/env python3
import hashlib
import json
import os
import re
import socket
import struct
import sys
import uuid
from pathlib import Path

PEER_PROTOCOL = 1
ENVELOPE_TAG = "cross-session-message"
SENDER_NAME = "ForkMesh (phone)"
STEER_LEAD = (
    "Your user typed this in the ForkMesh app on their phone, on this session's own page, as their next "
    "message to you in this conversation. ForkMesh can only reach a terminal session through cross-session "
    "messaging, so it arrives that way, but it was not sent by another session."
)
RECORD_NAME = re.compile(r"^(\d+)\.json$")
TOKEN_SHAPE = re.compile(r"^[0-9a-f]{32}$")
ADDRESS_SHAPE = re.compile(r"^[A-Za-z0-9%:_/.\\-]{1,300}$")
MODE_FIELD = re.compile(r'"permissionMode"\s*:\s*"([A-Za-z]+)"')
MAX_RECORD_BYTES = 64 * 1024
MAX_KEY_BYTES = 4096
MODE_TAIL_BYTES = 512 * 1024


def config_dirs(home, data_root, extra=()):
    dirs = []
    seen = set()

    def add(path):
        if not path:
            return
        clean = os.path.normpath(path)
        if clean in seen:
            return
        seen.add(clean)
        dirs.append(clean)

    if home:
        add(os.path.join(home, ".claude"))
    if data_root:
        accounts = Path(data_root) / "agent-accounts" / "claude"
        if accounts.is_dir():
            for child in sorted(accounts.iterdir()):
                if child.is_dir():
                    add(str(child))
    for path in extra:
        add(path)
    return dirs


def proc_start(pid):
    try:
        stat = Path("/proc/%d/stat" % pid).read_bytes()
    except OSError:
        return ""
    close = stat.rfind(b")")
    if close < 0:
        return ""
    fields = stat[close + 2:].split(b" ")
    return fields[19].decode() if len(fields) > 19 else ""


def is_socket(path):
    try:
        import stat as stat_module
        return stat_module.S_ISSOCK(os.lstat(path).st_mode)
    except OSError:
        return False


def process_alive(pid):
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def _read_small(path, limit):
    try:
        if os.path.getsize(path) > limit:
            return b""
        return Path(path).read_bytes()
    except OSError:
        return b""


def read_sessions(config_dir):
    out = []
    folder = Path(config_dir) / "sessions"
    if not folder.is_dir():
        return out
    for entry in sorted(folder.iterdir(), key=lambda p: p.name):
        match = RECORD_NAME.match(entry.name)
        if not match:
            continue
        try:
            record = json.loads(_read_small(entry, MAX_RECORD_BYTES) or b"{}")
        except ValueError:
            continue
        if not isinstance(record, dict):
            continue
        pid = record.get("pid")
        if not isinstance(pid, int) or pid <= 0 or pid != int(match.group(1)):
            continue
        if record.get("peerProtocol") != PEER_PROTOCOL:
            continue
        sock = record.get("messagingSocketPath") or ""
        if not isinstance(sock, str) or not os.path.isabs(sock):
            continue
        if not process_alive(pid):
            continue
        recorded = record.get("procStart") or ""
        actual = proc_start(pid)
        if recorded and actual and str(recorded) != actual:
            continue
        if not is_socket(sock):
            continue
        out.append({
            "pid": pid,
            "sessionId": record.get("sessionId") or "",
            "cwd": record.get("cwd") or "",
            "name": record.get("name") or "",
            "status": record.get("status") or "",
            "entrypoint": record.get("entrypoint") or "",
            "socketPath": sock,
            "configDir": os.path.normpath(config_dir),
        })
    return out


def live_sessions(dirs):
    out = []
    seen = set()
    for folder in dirs:
        for session in read_sessions(folder):
            if session["pid"] in seen:
                continue
            seen.add(session["pid"])
            out.append(session)
    return out


def key_path(config_dir, pid, socket_path):
    digest = hashlib.sha256(socket_path.encode("utf-8")).hexdigest()
    return os.path.join(config_dir, "sessions", "%d.%s.key" % (pid, digest))


def peer_token(path):
    try:
        key = json.loads(_read_small(path, MAX_KEY_BYTES) or b"{}")
    except ValueError:
        return ""
    token = key.get("peerToken") if isinstance(key, dict) else None
    return token if isinstance(token, str) and TOKEN_SHAPE.match(token) else ""


def permission_class(argv, transcript_path):
    if transcript_path:
        try:
            with open(transcript_path, "rb") as handle:
                handle.seek(0, os.SEEK_END)
                size = handle.tell()
                handle.seek(max(0, size - MODE_TAIL_BYTES))
                tail = handle.read().decode("utf-8", "replace")
            modes = MODE_FIELD.findall(tail)
            if modes:
                return "bypass" if modes[-1] == "bypassPermissions" else "prompting"
        except OSError:
            pass
    args = list(argv[1:])
    for i, arg in enumerate(args):
        if arg in ("--dangerously-skip-permissions", "--permission-mode=bypassPermissions"):
            return "bypass"
        if arg == "--permission-mode" and i + 1 < len(args) and args[i + 1] == "bypassPermissions":
            return "bypass"
    return "prompting"


def _word_char(ch):
    return ch.isascii() and (ch.isalnum() or ch in "_-")


def escape_body(text):
    out = []
    for i, ch in enumerate(text):
        out.append(ch)
        if ch != "<":
            continue
        if i + 1 < len(text) and text[i + 1] == "\\":
            continue
        for nxt in text[i + 1:]:
            if _word_char(nxt) or nxt in "<>":
                break
            if nxt == "/" or ord(nxt) > 0x7F:
                out.append("\\")
                break
    return "".join(out)


def _attribute_value(value):
    import unicodedata
    kept = []
    for ch in value or "":
        if ch in '"<>':
            continue
        if unicodedata.category(ch) in ("Cc", "Cf", "Cs", "Zl", "Zp"):
            continue
        kept.append(ch)
    out = "".join(kept).strip()
    return out[:64]


def envelope(message):
    attributes = ""
    sender = message.get("from") or ""
    if ADDRESS_SHAPE.match(sender):
        attributes += ' from="%s"' % sender
    name = _attribute_value(message.get("fromName"))
    if name:
        attributes += ' from-name="%s"' % name
    if message.get("fromMode") in ("bypass", "prompting"):
        attributes += ' from-mode="%s"' % message["fromMode"]
    return "<%s%s>\n%s\n</%s>" % (ENVELOPE_TAG, attributes, escape_body(message.get("text", "")), ENVELOPE_TAG)


def json_string(value):
    out = ['"']
    simple = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
    for ch in value:
        code = ord(ch)
        if ch in simple:
            out.append(simple[ch])
        elif code < 0x20 or 0xD800 <= code <= 0xDFFF:
            out.append("\\u%04x" % code)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out).encode("utf-8")


def auth_line(token):
    return b'{"type":"auth","token":' + json_string(token) + b"}"


def user_line(message):
    line = (b'{"msgV":1,"msg_id":' + json_string(message.get("msgId", ""))
            + b',"type":"user","message":{"role":"user","content":' + json_string(envelope(message))
            + b'},"priority":' + json_string(message.get("priority") or "next"))
    if message.get("from"):
        line += b',"from":' + json_string(message["from"])
    return line + b"}"


def frame(token, message):
    data = b""
    if token:
        data += auth_line(token) + b"\n"
    return data + user_line(message) + b"\n"


def parse_frame(data):
    lines = [line for line in data.split(b"\n") if line.strip()]
    parsed = [json.loads(line) for line in lines]
    token = ""
    if parsed and parsed[0].get("type") == "auth":
        token = parsed.pop(0).get("token", "")
    return token, parsed


def send(socket_path, expect_pid, data, timeout=3.0):
    if not is_socket(socket_path):
        raise OSError("no socket at %s" % socket_path)
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        conn.settimeout(timeout)
        conn.connect(socket_path)
        if hasattr(socket, "SO_PEERCRED"):
            pid, uid, _gid = struct.unpack("3i", conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
            if uid != os.getuid():
                raise OSError("peer owned by another user")
            if expect_pid and pid != expect_pid:
                raise OSError("peer is pid %d, not %d" % (pid, expect_pid))
        conn.sendall(data)
        conn.shutdown(socket.SHUT_WR)
        try:
            while conn.recv(256):
                pass
        except OSError:
            pass
    finally:
        conn.close()


INBOUND_VALUES = ("accept", "hold", "refuse")
INBOUND_RANK = {"accept": 0, "hold": 1, "refuse": 2}


def inbound_setting(settings_json):
    try:
        settings = json.loads(settings_json or b"{}")
    except ValueError:
        return ""
    if not isinstance(settings, dict) or "crossSessionInbound" not in settings:
        return ""
    value = settings["crossSessionInbound"]
    return value if value in INBOUND_VALUES else "invalid"


def inbound_setting_file(path):
    if not path:
        return ""
    return inbound_setting(_read_small(path, 4 * 1024 * 1024))


def flag_inbound_setting(argv):
    value = ""
    args = list(argv)
    for i in range(1, len(args)):
        arg = args[i]
        if arg == "--settings" and i + 1 < len(args):
            value = args[i + 1]
        elif arg.startswith("--settings="):
            value = arg[len("--settings="):]
    value = value.strip()
    if not value:
        return ""
    if value.startswith("{"):
        return inbound_setting(value.encode("utf-8"))
    return inbound_setting_file(value)


def managed_settings_path():
    if sys.platform == "darwin":
        return "/Library/Application Support/ClaudeCode/managed-settings.json"
    return "/etc/claude-code/managed-settings.json"


def git_root(cwd):
    if not cwd or not os.path.isdir(cwd):
        return ""
    folder = os.path.abspath(cwd)
    while True:
        if os.path.exists(os.path.join(folder, ".git")):
            return folder
        parent = os.path.dirname(folder)
        if parent == folder:
            return ""
        folder = parent


def inbound_policy(managed, flag, user, local, project, receiver_class):
    value, decided = "", ""
    if managed == "invalid":
        value, decided = "refuse", "managed-setting"
    elif managed in INBOUND_VALUES:
        value, decided = managed, "managed-setting"
    elif flag in INBOUND_VALUES:
        value, decided = flag, "explicit-setting"
    elif user in INBOUND_VALUES:
        value, decided = user, "explicit-setting"
    for repo in (local, project):
        if repo not in INBOUND_VALUES:
            continue
        if INBOUND_RANK[repo] > INBOUND_RANK[value or "accept"]:
            value, decided = repo, "repo-setting"
        elif repo != "accept" and value and INBOUND_RANK[repo] == INBOUND_RANK[value] and decided != "managed-setting":
            decided = "repo-setting"
    if "invalid" in (flag, user, local, project) and INBOUND_RANK[value or "accept"] < 1:
        value, decided = "hold", "invalid-setting"
    if value:
        return value, decided
    if receiver_class == "bypass":
        return "hold", "no-mode-asserted"
    return "accept", "mode-parity"


def predict_inbound(session, argv, receiver_class, managed_path):
    cwd = session.get("cwd") or ""
    local = project = ""
    if cwd:
        local = inbound_setting_file(os.path.join(cwd, ".claude", "settings.local.json"))
        project = inbound_setting_file(os.path.join(cwd, ".claude", "settings.json"))
        root = git_root(cwd)
        if root and os.path.normpath(root) != os.path.normpath(cwd):
            root_local = inbound_setting_file(os.path.join(root, ".claude", "settings.local.json"))
            if root_local and root_local != "accept" and (
                    not local or local == "accept" or root_local in ("refuse", "invalid")):
                local = root_local
    return inbound_policy(inbound_setting_file(managed_path), flag_inbound_setting(argv),
                          inbound_setting_file(os.path.join(session["configDir"], "settings.json")),
                          local, project, receiver_class)


def steer_body(text):
    return STEER_LEAD + "\n\n" + text


def steer_text(body):
    if not body.startswith(STEER_LEAD):
        return body
    return body[len(STEER_LEAD):].strip()


def deliver(session, text):
    message = {
        "text": steer_body(text),
        "fromName": SENDER_NAME,
        "fromMode": "",
        "msgId": str(uuid.uuid4()),
        "priority": "next",
    }
    token = peer_token(key_path(session["configDir"], session["pid"], session["socketPath"]))
    send(session["socketPath"], session["pid"], frame(token, message))


def main(argv):
    home = os.path.expanduser("~")
    data_root = os.environ.get("FORKMESH_DATA_ROOT") or os.path.join(home, "projects", "forkmesh", "data")
    sessions = live_sessions(config_dirs(home, data_root))
    if len(argv) < 2 or argv[1] == "list":
        for session in sessions:
            print(json.dumps(session))
        return 0
    if argv[1] == "send" and len(argv) >= 4:
        pid = int(argv[2])
        match = [s for s in sessions if s["pid"] == pid]
        if not match:
            print("no live Claude Code session with pid %d" % pid, file=sys.stderr)
            return 1
        try:
            args = [a.decode() for a in Path("/proc/%d/cmdline" % pid).read_bytes().split(b"\0") if a]
        except OSError:
            args = []
        policy, cause = predict_inbound(match[0], args, permission_class(args, ""), managed_settings_path())
        if policy == "refuse":
            print("the session refuses cross-session messages (%s)" % cause, file=sys.stderr)
            return 1
        deliver(match[0], argv[3])
        print("held_for_approval (%s)" % cause if policy == "hold" else "delivered")
        return 0
    print("usage: claude_peer_message.py [list | send <pid> <text>]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
