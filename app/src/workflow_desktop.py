import hashlib
import json

DESKTOP = "#0f766e"
RUNS_ON = "desktop"
SEED_KEY = "free-disk-space"
SEED_NAME = "Free disk space"
MAX_RUN_MS = 6 * 3_600_000
MAX_LOG_LINES = 200
MAX_LOG_CHARS = 300
MAX_ERROR_CHARS = 300
STATUSES = ("success", "error", "skipped")
PASSIVE = ("core.noop", "core.comment")
REFUSED = ("This step runs on your desktop. Press Run workflow on the "
           "Workflows page while the ForkMesh desktop app is open.")

STEPS = (
    ("desktop.diskUsage", "usage", "Measure the disk", "hard-drive",
     "Reads the free space and the agent worktrees the cleanup keeps."),
    ("desktop.cleanAgents", "agents", "Remove agent leftovers", "bot",
     "Deletes scratch folders and builds of agents that no longer run."),
    ("desktop.cleanTests", "tests", "Remove test leftovers", "flask-conical",
     "Deletes pytest, Actions sandbox and Qt test folders nothing uses."),
    ("desktop.cleanVersions", "versions", "Prune old versions and Trash",
     "archive", "Removes old Claude versions, backups past the newest "
     "three and ForkMesh cleanup items in the Trash."),
    ("desktop.cleanCaches", "caches", "Clear pip and npm caches", "package",
     "Runs pip3 cache purge and npm cache clean."),
    ("desktop.diskReport", "report", "Report free space", "gauge",
     "Reads the free space after the cleanup and lists big folders it "
     "leaves for you."),
)

NODES = {
    node_type: {
        "name": name, "group": "Desktop", "icon": icon, "color": DESKTOP,
        "outputs": ["main"], "description": description, "fields": [],
        "runsOn": RUNS_ON, "desktopStep": step, "refusal": REFUSED,
    }
    for node_type, step, name, icon, description in STEPS
}


def is_desktop(node_type):
    return node_type in NODES


def has_desktop_steps(workflow):
    return any(is_desktop(node.get("type")) for node in
               (workflow or {}).get("nodes") or [])


def check(nodes, is_trigger):
    if not any(is_desktop(node["type"]) for node in nodes):
        return ""
    for node in nodes:
        if is_desktop(node["type"]) or node["type"] in PASSIVE:
            continue
        if node["type"] == "core.manual":
            continue
        if is_trigger(node["type"]):
            return "desktop_needs_manual"
        return "desktop_steps_only"
    return ""


def seed_id(owner_bi):
    digest = hashlib.sha256(
        ("workflow-seed:" + SEED_KEY + ":" + str(owner_bi)).encode("utf-8"))
    return digest.hexdigest()[:32]


def seed_workflow():
    nodes = [{"id": "run", "type": "core.manual", "name": "Run", "x": 0,
              "y": 0, "params": {}}]
    connections = []
    previous = "run"
    for index, (node_type, step, name, _icon, _text) in enumerate(STEPS):
        nodes.append({"id": step, "type": node_type, "name": name,
                      "x": (index + 1) * 288, "y": 0, "params": {}})
        connections.append({"from": previous, "output": "main", "to": step})
        previous = step
    return {"name": SEED_NAME, "nodes": nodes, "connections": connections}


def human(size):
    size = float(max(0, size))
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    return f"{size:.1f} TB"


def _int(value, low=0, high=2 ** 62):
    try:
        number = int(float(value))
    except (TypeError, ValueError):
        return None
    if number < low or number > high:
        return None
    return number


def _text(value, limit):
    return str(value if value is not None else "").replace("\x00", "")[:limit]


def disk_output(raw):
    raw = raw if isinstance(raw, dict) else {}
    freed = _int(raw.get("freedBytes")) or 0
    free = _int(raw.get("freeBytes"))
    total = _int(raw.get("totalBytes"))
    metrics = [{"label": "Cleaned", "value": human(freed)}]
    text = "Freed " + human(freed)
    if free is not None:
        metrics.append({"label": "Free", "value": human(free)})
        text += " · " + human(free) + " free"
    out = {"text": text, "metrics": metrics, "freedBytes": freed}
    if free is not None:
        out["freeBytes"] = free
    if total is not None and total > 0:
        out["totalBytes"] = total
    return out


def clean_report(workflow, data, summarize, now):
    if not isinstance(data, dict):
        return None, "invalid_run"
    started = _int(data.get("startedAt"), now - MAX_RUN_MS, now + 60_000)
    finished = _int(data.get("finishedAt"), now - MAX_RUN_MS, now + 60_000)
    if started is None or finished is None or finished < started:
        return None, "invalid_run"
    raw_nodes = data.get("nodes")
    if not isinstance(raw_nodes, dict):
        return None, "invalid_run"
    by_id = {node["id"]: node for node in workflow.get("nodes") or []}
    nodes = {}
    failed = ""
    for node_id, raw in raw_nodes.items():
        node = by_id.get(node_id)
        if node is None or not isinstance(raw, dict):
            return None, "invalid_run"
        status = raw.get("status")
        if status not in STATUSES:
            return None, "invalid_run"
        entry = {"status": status}
        if status != "skipped":
            entry["ms"] = _int(raw.get("ms"), 0, MAX_RUN_MS) or 0
        if status == "success":
            outputs = {}
            for name, output in (raw.get("outputs") or {}).items():
                if name != "main" or not isinstance(output, dict):
                    continue
                items = [item for item in output.get("items") or []
                         if isinstance(item, dict)]
                outputs["main"] = {
                    "count": _int(output.get("count"), 0, 10_000) or len(items),
                    "items": summarize(items)}
            entry["outputs"] = outputs or {"main": {"count": 0, "items": []}}
        if status == "error":
            entry["error"] = _text(raw.get("error"), MAX_ERROR_CHARS) \
                or "The desktop step failed."
            if not failed:
                failed = node["name"] + ": " + entry["error"]
        logs = raw.get("logs")
        if isinstance(logs, list) and logs:
            entry["logs"] = [_text(line, MAX_LOG_CHARS)
                             for line in logs[-MAX_LOG_LINES:]]
        nodes[node_id] = entry
    report = {
        "status": "error" if failed else "success",
        "error": failed,
        "requests": 0,
        "nodes": nodes,
        "place": RUNS_ON,
        "output": disk_output(data.get("output")),
    }
    if failed:
        report["output"]["text"] = failed[:MAX_ERROR_CHARS]
    return (report, started, finished), ""


def output_preview(item):
    if not isinstance(item, dict):
        return _text(json.dumps(item, default=str), 120)
    for key in ("output", "text", "message", "result", "status"):
        value = item.get(key)
        if isinstance(value, (str, int, float)) and not isinstance(value, bool):
            return _text(value, 120)
    parts = []
    for key, value in item.items():
        if isinstance(value, (str, int, float, bool)) and not key.startswith("_"):
            parts.append(str(key) + ": " + _text(value, 40))
        if len(parts) == 3:
            break
    return ", ".join(parts)[:120]
