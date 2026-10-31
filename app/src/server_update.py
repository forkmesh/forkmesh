import json
import os
import re

INSTANCE_REQUEST_ROOT = "/var/lib/forkmesh-instance/"
INSTANCE_STATUS_ROOT = "/var/lib/forkmesh-instance-update/"
BUILD_INFO_NAME = re.compile(r"^/[A-Za-z0-9._/-]+\.json$")
ACTIONS = ("check", "update", "auto", "follow")
TRIGGERS = ("admin", "merge", "follow")
DAEMON_STATES = ("current", "stale", "unknown", "release")
RUNNING = ("checking", "updating")
MAX_FILE_BYTES = 16 * 1024
OID_RE = re.compile(r"^[0-9a-f]{7,64}$")
VERSION_RE = re.compile(r"^[0-9][0-9.]{0,31}$")
STEP_RE = re.compile(r"^[a-z]{1,32}$")
STATES = ("idle", "checking", "updating", "done", "failed")
FOLLOW_STATES = ("current", "merged", "conflict", "failed")
CORE_KIND = "admin"
CORE_UPSTREAM_KEY = "core_upstream"
CORE_OPEN = ("pending", "deploying")
STEP_LABELS = {
    "system": "Checking the server",
    "packages": "Installing packages",
    "source": "Downloading the source",
    "follow": "Merging the latest ForkMesh",
    "build": "Building the app",
    "configure": "Saving settings",
    "database": "Migrating the database",
    "services": "Restarting the app",
    "https": "Checking HTTPS",
    "admin": "Checking the git node",
    "repo": "Checking the ForkMesh repository",
    "done": "Finishing",
}


def _env(env, name):
    return str(getattr(env, name, "") or "").strip()


def _path_under(value, root):
    value = str(value or "")
    if not value.startswith(root) or ".." in value.split("/"):
        return ""
    return value


def request_path(env):
    return _path_under(_env(env, "INSTANCE_UPDATE_REQUEST"),
                       INSTANCE_REQUEST_ROOT)


def status_path(env):
    return _path_under(_env(env, "INSTANCE_UPDATE_STATUS"),
                       INSTANCE_STATUS_ROOT)


def mode(env):
    if request_path(env) and status_path(env):
        return "instance"
    if _env(env, "CORE_ADMIN_UPDATES") == "1":
        return "core"
    return ""


def read_json(path):
    if not path:
        return {}
    try:
        with open(path, "rb") as handle:
            raw = handle.read(MAX_FILE_BYTES + 1)
    except OSError:
        return {}
    if len(raw) > MAX_FILE_BYTES:
        return {}
    try:
        value = json.loads(raw.decode("utf-8"))
    except ValueError:
        return {}
    return value if isinstance(value, dict) else {}


def _int(value):
    try:
        return max(0, int(value))
    except (TypeError, ValueError):
        return 0


def _text(value, limit=500):
    return " ".join(str(value or "").split())[:limit]


def commit(commit_id="", time_s=0, subject="", version=""):
    commit_id = str(commit_id or "").strip().lower()
    version = str(version or "").strip()
    return {
        "commit": commit_id if OID_RE.fullmatch(commit_id) else "",
        "time": _int(time_s),
        "subject": _text(subject),
        "version": version if VERSION_RE.fullmatch(version) else "",
    }


def running(env):
    path = _env(env, "BUILD_INFO_FILE")
    info = read_json(path if BUILD_INFO_NAME.fullmatch(path) else "")
    current = commit(info.get("commit") or _env(env, "BUILD_REV"),
                     info.get("commitTime"), info.get("subject"),
                     info.get("version") or _env(env, "APP_VERSION"))
    current["builtAt"] = _int(info.get("builtAt"))
    return current


def build_source(env):
    path = _env(env, "BUILD_INFO_FILE")
    info = read_json(path if BUILD_INFO_NAME.fullmatch(path) else "")
    daemon = info.get("daemon") if isinstance(info.get("daemon"), dict) \
        else {}
    state = str(daemon.get("state") or "")
    return {
        "source": _text(info.get("source"), 300),
        "ref": _text(info.get("ref"), 120),
        "daemon": state if state in DAEMON_STATES else "",
        "daemonBuiltCommit": commit(daemon.get("builtCommit"))["commit"],
    }


def instance_status(env):
    doc = read_json(status_path(env))
    state = str(doc.get("state") or "idle")
    step = str(doc.get("step") or "")
    status = {
        "state": state if state in STATES else "idle",
        "action": str(doc.get("action") or "") if doc.get("action") in ACTIONS
        else "",
        "trigger": str(doc.get("trigger") or "") if doc.get("trigger") in
        TRIGGERS else "",
        "startedAt": _int(doc.get("startedAt")),
        "finishedAt": _int(doc.get("finishedAt")),
        "step": step if STEP_RE.fullmatch(step) else "",
        "lastError": _text(doc.get("lastError")),
        "exitCode": _int(doc.get("exitCode")),
        "fromCommit": commit(doc.get("fromCommit"))["commit"],
        "toCommit": commit(doc.get("toCommit"))["commit"],
        "checkedAt": _int(doc.get("checkedAt")),
        "upstream": commit(doc.get("upstreamCommit"), doc.get("upstreamTime"),
                           doc.get("upstreamSubject"),
                           doc.get("upstreamVersion")),
        "requested": "",
        "follow": {
            "state": str(doc.get("followState") or "")
            if doc.get("followState") in FOLLOW_STATES else "",
            "at": _int(doc.get("followAt")),
            "upstream": commit(doc.get("followUpstream"))["commit"],
            "commit": commit(doc.get("followCommit"))["commit"],
            "behind": _int(doc.get("followBehind")),
            "message": _text(doc.get("followMessage"), 300),
            "files": [f for f in _text(doc.get("followFiles")).split()
                      if re.fullmatch(r"[A-Za-z0-9._/@+-]{1,200}", f)][:20],
        },
    }
    path = request_path(env)
    if path:
        try:
            with open(path, "rb") as handle:
                action = handle.read(32).decode("ascii", "ignore").strip()
        except OSError:
            action = ""
        status["requested"] = action if action in ACTIONS else ""
    return status


def busy(status):
    return bool(status.get("requested")) or status.get("state") in RUNNING


def request_instance(env, action):
    path = request_path(env)
    if action not in ACTIONS or not path:
        return False
    tmp = path + ".new"
    try:
        with open(tmp, "w", encoding="ascii") as handle:
            handle.write(action + "\n")
        os.replace(tmp, path)
    except OSError:
        return False
    return True


def follow_path(env):
    path = request_path(env)
    return os.path.join(os.path.dirname(path), "follow") if path else ""


def follow_on(env):
    path = follow_path(env)
    if not path or os.path.islink(path):
        return False
    try:
        with open(path, "rb") as handle:
            return handle.read(8).decode("ascii", "ignore").strip() == "on"
    except OSError:
        return False


def set_follow(env, on):
    path = follow_path(env)
    if not path:
        return False
    try:
        if on:
            tmp = path + ".new"
            with open(tmp, "w", encoding="ascii") as handle:
                handle.write("on\n")
            os.replace(tmp, path)
        elif os.path.lexists(path):
            os.unlink(path)
    except OSError:
        return False
    return True


def core_upstream(doc):
    try:
        value = json.loads(str(doc or "")[:MAX_FILE_BYTES] or "{}")
    except ValueError:
        value = {}
    if not isinstance(value, dict):
        value = {}
    upstream = commit(value.get("commit"), value.get("time"),
                      value.get("subject"), value.get("version"))
    upstream["checkedAt"] = _int(value.get("checkedAt"))
    return upstream


def freshness(current, upstream):
    mine = str((current or {}).get("commit") or "")
    latest = str((upstream or {}).get("commit") or "")
    if not latest:
        return "unknown"
    if not mine:
        return "behind"
    if latest.startswith(mine) or mine.startswith(latest):
        return "current"
    if 0 < _int((upstream or {}).get("time")) < _int((current or {}).get("time")):
        return "current"
    return "behind"


def step_label(step):
    return STEP_LABELS.get(step, step)
