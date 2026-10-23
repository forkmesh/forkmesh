import json
import re

import workflow_desktop as desktop

PATH_RE = re.compile(r"^\.forkmesh/workflows/([a-z0-9][a-z0-9-]{0,63})\.json$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
NODE_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
TIME_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")
FILE_KIND = "forkmesh.workflow"
FOLDER = ".forkmesh/workflows"
LEGACY_FOLDER = ".forkmesh/teammates"
MANUAL = "workflow.manual"
SCHEDULE = "workflow.schedule"
MONITOR = "workflow.monitor"
AGENT = "agent.run"
SHORTCUT = "workflow.shortcut"
OUTPUT = "workflow.output"
NOTE = "workflow.note"
TRIGGERS = (MANUAL, SCHEDULE, MONITOR)
RAILS = ("top", "left", "right", "bottom")
COLUMN = 288
ROW = 120
MAX_NODES = 60
MAX_CONNECTIONS = 120
MAX_MONITORS = 16
MAX_NODE_NAME = 60
SCHEDULE_MODES = ("on-demand", "continuous", "hourly", "daily", "weekly",
                  "weekdays", "custom", "events")
TASK_TYPES = ("on-demand", "scheduled", "new-issues", "new-pulls",
              "assignments", "mentions", "ui-stalls", "error-logs")
PERMISSIONS = ("read-repositories", "modify-code", "create-branches",
               "review-prs", "manage-issues", "merge", "external-tools",
               "run-unattended", "run-git", "run-tests", "read-forkmesh-web",
               "process-payouts")
NATIVE_WORKFLOWS = ("", "contributor-payouts")
RUNTIME_KEYS = ("version", "id", "name", "enabled", "schedule", "logMonitors",
                "createdAtMs", "updatedAtMs", "lastRunAtMs", "lastSessionId",
                "lastTrigger")
DAY_NAMES = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")


class WorkflowFileError(ValueError):
    pass


def _opts(values, labels=None):
    return [{"value": v, "label": (labels or {}).get(v, v or "(none)")}
            for v in values]


def _field(key, label, kind="string", **extra):
    field = {"key": key, "label": label, "type": kind}
    field.update(extra)
    return field


def _spec(name, group, icon, color, description, fields, trigger=False,
          single=False):
    spec = {"kind": "workflow", "name": name, "group": group, "icon": icon,
            "color": color, "outputs": ["main"], "description": description,
            "fields": fields, "runsOn": "desktop"}
    if trigger:
        spec["inputs"] = False
    if single:
        spec["single"] = True
    return spec


NODES = {
    MANUAL: _spec(
        "Run by hand", "Triggers", "mouse-pointer-click", "#64748b",
        "Start the workflow with its Run button, a pinned rail button or a "
        "phone.", [], trigger=True, single=True),
    SCHEDULE: _spec(
        "Schedule", "Triggers", "calendar-clock", "#0891b2",
        "When the workflow runs by itself: on a cadence, continuously, or on "
        "repository events.", [
            _field("mode", "How often", "select", options=_opts(
                SCHEDULE_MODES, {"on-demand": "Only when asked",
                                 "continuous": "Continuously",
                                 "hourly": "Every hour", "daily": "Every day",
                                 "weekly": "Every week",
                                 "weekdays": "Weekdays",
                                 "custom": "Chosen days",
                                 "events": "On events"}),
                   default="on-demand"),
            _field("time", "Time (HH:mm)", placeholder="09:00",
                   default="09:00"),
            _field("days", "Days", "checks", options=[
                {"value": str(i + 1), "label": DAY_NAMES[i]}
                for i in range(7)]),
            _field("events", "Run on events", "checks",
                   options=_opts(TASK_TYPES)),
            _field("active", "Schedule is on", "boolean", default="false"),
        ], trigger=True, single=True),
    MONITOR: _spec(
        "Log monitor", "Triggers", "activity", "#155e75",
        "Starts a run when an app-log badge passes a threshold.", [
            _field("badge", "Badge", required=True, placeholder="STALL"),
            _field("threshold", "Threshold", "number", default="10"),
            _field("taskType", "Task type", "select",
                   options=_opts(TASK_TYPES), default="error-logs"),
            _field("active", "Monitor is on", "boolean", default="true"),
            _field("inclusive", "Include the threshold", "boolean",
                   default="false"),
        ], trigger=True),
    AGENT: _spec(
        "Special agent", "Agents", "bot", "#db2777",
        "Runs a coding agent with a role, instructions, skills, a playbook, "
        "a learning journal and only the permissions you tick. Its reply is "
        "the step's output.", [
            _field("avatar", "Portrait", default="sol"),
            _field("specialty", "Specialty", required=True),
            _field("focus", "Focus"),
            _field("description", "Responsibility", "text"),
            _field("instructions", "Operating instructions", "code"),
            _field("shortcut", "Playbook shortcut",
                   placeholder="review-security.md"),
            _field("skills", "Skill names (one per line)", "list"),
            _field("skillRefs", "Skill files (scope:file, one per line)",
                   "list", placeholder="repository:security-review.skill"),
            _field("permissions", "Permissions", "checks",
                   options=_opts(PERMISSIONS)),
            _field("selfImproving", "Keeps a learning journal", "boolean",
                   default="true"),
            _field("improvementNotes",
                   "Learning journal (one lesson per line)", "list"),
            _field("lowCostModel", "Use the low-cost model", "boolean",
                   default="false"),
            _field("runWindowHours", "Expected run window (hours)", "number",
                   default="0"),
            _field("relayChannel", "Relay channel", placeholder="#team"),
            _field("marketingChannels", "Marketing channels (one per line)",
                   "list"),
            _field("workflow", "Native workflow", "select",
                   options=_opts(NATIVE_WORKFLOWS, {
                       "": "None",
                       "contributor-payouts": "Contributor payouts"})),
        ]),
    SHORTCUT: _spec(
        "Run shortcut", "Shortcuts", "command", "#2563eb",
        "Runs a script or prompt from .forkmesh/shortcuts. Scripts get the "
        "earlier steps' output in FORKMESH_WORKFLOW_INPUT.", [
            _field("file", "Shortcut file", required=True,
                   placeholder="deploy-new-version.sh"),
            _field("wait", "Wait for it to finish", "boolean",
                   default="true"),
            _field("allowEdits", "Prompt shortcuts may edit files", "boolean",
                   default="false"),
        ]),
    OUTPUT: _spec(
        "Save output", "Output", "file-text", "#16a34a",
        "Writes the run so far as Markdown. Every run keeps its own output "
        "file on the desktop; a path also saves a copy in the repository.", [
            _field("title", "Heading"),
            _field("path", "Also save to (repository path)",
                   placeholder="docs/reports/latest.md"),
        ]),
    NOTE: _spec(
        "Note", "Output", "sticky-note", "#a16207",
        "A note on the canvas. It passes its input on unchanged.",
        [_field("text", "Note", "text")]),
}
for _type, _desktop in desktop.NODES.items():
    NODES[_type] = _spec(_desktop["name"], "Desktop", _desktop["icon"],
                         _desktop["color"], _desktop["description"], [])

KIND = {"label": "Workflow", "plural": "Desktop workflows",
        "folder": FOLDER, "icon": "workflow", "root": "",
        "pattern": PATH_RE.pattern,
        "description": "Workflows the desktop runs: special agents, "
                       "shortcuts, desktop steps and saved output "
                       "(.forkmesh/workflows/*.json)."}


def _lines(value):
    if isinstance(value, list):
        items = value
    else:
        items = str(value or "").replace("\r", "").split("\n")
    return [str(item).strip() for item in items if str(item).strip()]


def _bool(value, default=False):
    if isinstance(value, bool):
        return value
    text = str(value if value is not None else "").strip().lower()
    if not text:
        return default
    return text in ("true", "1", "yes", "on")


def _int(value, default=0):
    try:
        return int(float(str(value).strip()))
    except (TypeError, ValueError):
        return default


def safe_repository_path(path):
    text = str(path or "").strip().replace("\\", "/")
    if not text or text.startswith(("/", "~")) or ":" in text or "\x00" in text:
        return ""
    parts = [part for part in text.split("/") if part not in ("", ".")]
    if not parts or ".." in parts or parts[0] in (".git", ".forkmesh") or \
            len(text) > 240:
        return ""
    return "/".join(parts)


def _node(node_id, node_type, name, column, row, params):
    return {"id": node_id, "type": node_type, "name": name,
            "x": column * COLUMN, "y": row * ROW, "params": params}


def _edge(source, target):
    return {"from": source, "output": "main", "to": target}


def _schedule_params(schedule):
    schedule = schedule if isinstance(schedule, dict) else {}
    mode = str(schedule.get("mode") or "on-demand")
    days = []
    for day in schedule.get("days") or []:
        number = _int(day, 0)
        if 1 <= number <= 7 and str(number) not in days:
            days.append(str(number))
    return {"mode": mode if mode in SCHEDULE_MODES else "on-demand",
            "time": str(schedule.get("time") or "09:00"),
            "days": sorted(days),
            "events": [str(e) for e in schedule.get("events") or []],
            "active": bool(schedule.get("active"))}


def _monitor_params(monitor):
    return {"badge": str(monitor.get("badge") or ""),
            "threshold": max(0, _int(monitor.get("threshold"), 0)),
            "taskType": str(monitor.get("taskType") or "error-logs"),
            "active": bool(monitor.get("active", True)),
            "inclusive": bool(monitor.get("inclusive"))}


def teammate_to_workflow(data):
    if not isinstance(data, dict):
        raise WorkflowFileError("invalid_json_file")
    agent_id = str(data.get("id") or "")
    if not ID_RE.match(agent_id):
        raise WorkflowFileError("invalid_agent_id")
    name = str(data.get("name") or "").strip()
    if not name or not str(data.get("specialty") or "").strip():
        raise WorkflowFileError("agent_needs_name_and_specialty")
    params = {k: v for k, v in data.items() if k not in RUNTIME_KEYS}
    nodes = [_node("agent", AGENT, name[:MAX_NODE_NAME - 4], 1, 0, params),
             _node("schedule", SCHEDULE, "Schedule", 0, 0,
                   _schedule_params(data.get("schedule")))]
    connections = [_edge("schedule", "agent")]
    row = 1
    for monitor in data.get("logMonitors") or []:
        if not isinstance(monitor, dict) or not monitor.get("badge"):
            continue
        node_id = "monitor%d" % row
        nodes.append(_node(node_id, MONITOR, "Log monitor %d" % row, 0, row,
                           _monitor_params(monitor)))
        connections.append(_edge(node_id, "agent"))
        row += 1
    nodes.append(_node("output", OUTPUT, "Save the report", 2, 0,
                       {"title": "", "path": ""}))
    connections.append(_edge("agent", "output"))
    return {"kind": FILE_KIND, "version": 1, "id": agent_id, "name": name,
            "description": str(data.get("description") or ""),
            "icon": str(data.get("avatar") or "sol"),
            "enabled": data.get("enabled", True) is not False,
            "createdAtMs": int(data.get("createdAtMs") or 0),
            "updatedAtMs": int(data.get("updatedAtMs") or 0),
            "nodes": nodes, "connections": connections}


def _order(nodes, connections):
    ids = [n["id"] for n in nodes]
    incoming = {i: 0 for i in ids}
    outgoing = {}
    for conn in connections:
        if conn["from"] in incoming and conn["to"] in incoming:
            outgoing.setdefault(conn["from"], []).append(conn["to"])
            incoming[conn["to"]] += 1
    ready = [i for i in ids if incoming[i] == 0]
    order = []
    while ready:
        current = ready.pop(0)
        order.append(current)
        for target in outgoing.get(current, []):
            incoming[target] -= 1
            if incoming[target] == 0:
                ready.append(target)
                ready.sort(key=ids.index)
    return order if len(order) == len(ids) else None


def primary_agent(workflow):
    nodes = workflow.get("nodes") or []
    order = _order(nodes, workflow.get("connections") or []) or \
        [n["id"] for n in nodes]
    by_id = {n["id"]: n for n in nodes}
    for node_id in order:
        if by_id[node_id].get("type") == AGENT:
            return by_id[node_id]
    return None


def workflow_to_teammate(workflow):
    agent = primary_agent(workflow)
    if agent is None:
        return None
    data = dict(agent.get("params") or {})
    data["version"] = 2
    data["id"] = workflow.get("id")
    data["name"] = workflow.get("name") or agent.get("name")
    data.setdefault("avatar", workflow.get("icon") or "sol")
    if not data.get("description"):
        data["description"] = workflow.get("description") or ""
    data["enabled"] = workflow.get("enabled", True) is not False and \
        not agent.get("disabled")
    schedule = next((n for n in workflow.get("nodes") or []
                     if n.get("type") == SCHEDULE), None)
    if schedule is not None:
        params = schedule.get("params") or {}
        data["schedule"] = {
            "mode": params.get("mode") or "on-demand",
            "time": params.get("time") or "09:00",
            "days": [_int(d) for d in params.get("days") or []],
            "events": list(params.get("events") or []),
            "active": _bool(params.get("active")) and
            not schedule.get("disabled")}
    else:
        data["schedule"] = {"mode": "on-demand", "time": "09:00", "days": [],
                            "events": [], "active": False}
    data["logMonitors"] = [
        dict(n.get("params") or {}) for n in workflow.get("nodes") or []
        if n.get("type") == MONITOR]
    data["createdAtMs"] = int(workflow.get("createdAtMs") or 0)
    data["updatedAtMs"] = int(workflow.get("updatedAtMs") or 0)
    for key, default in (("lastRunAtMs", 0), ("lastSessionId", 0),
                         ("lastTrigger", "")):
        data.setdefault(key, default)
    return data


def load_teammate(path):
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    if isinstance(data, dict) and isinstance(data.get("nodes"), list):
        return workflow_to_teammate(data)
    return data


def parse(path, text, max_bytes):
    if len(text.encode("utf-8")) > max_bytes:
        raise WorkflowFileError("file_too_large")
    try:
        data = json.loads(text)
    except ValueError:
        raise WorkflowFileError("invalid_json_file")
    if not isinstance(data, dict):
        raise WorkflowFileError("invalid_json_file")
    if not isinstance(data.get("nodes"), list) and data.get("specialty"):
        data = teammate_to_workflow(data)
    match = PATH_RE.match(path)
    file_id = match.group(1) if match else ""
    warnings = []
    if data.get("id") and data.get("id") != file_id:
        warnings.append("The file name and the workflow id differ; saving "
                        "uses the file name")
    nodes = []
    for raw in data.get("nodes") or []:
        if not isinstance(raw, dict):
            continue
        params = raw.get("params") if isinstance(raw.get("params"),
                                                 dict) else {}
        nodes.append({"id": str(raw.get("id") or ""),
                      "type": str(raw.get("type") or ""),
                      "name": str(raw.get("name") or ""),
                      "x": _int(raw.get("x")), "y": _int(raw.get("y")),
                      "params": params})
        if raw.get("type") not in NODES:
            warnings.append(str(raw.get("name") or raw.get("id")) +
                            " is a step this editor does not know")
    connections = [{"from": str(c.get("from") or ""), "output": "main",
                    "to": str(c.get("to") or "")}
                   for c in data.get("connections") or []
                   if isinstance(c, dict)]
    if not nodes:
        nodes.append(_node("run", MANUAL, "Run by hand", 0, 0, {}))
    meta = {"original": data}
    return nodes, connections, meta, warnings


def render(path, nodes, connections, meta, now_ms, dump):
    match = PATH_RE.match(path or "")
    if not match:
        raise WorkflowFileError("unsupported_path")
    data = dict((meta or {}).get("original") or {})
    original_nodes = {n.get("id"): n for n in data.get("nodes") or []
                      if isinstance(n, dict)}
    if len(nodes) > MAX_NODES:
        raise WorkflowFileError("too_many_nodes")
    names = set()
    counts = {}
    out_nodes = []
    for node in nodes:
        node_type = node["type"]
        if node_type not in NODES:
            raise WorkflowFileError("unknown_node_type")
        name = str(node.get("name") or NODES[node_type]["name"]).strip()
        name = name[:MAX_NODE_NAME]
        if not name or name in names:
            raise WorkflowFileError("duplicate_node_name")
        names.add(name)
        counts[node_type] = counts.get(node_type, 0) + 1
        previous = original_nodes.get(node["id"]) or {}
        params = dict(previous.get("params") or {}) \
            if previous.get("type") == node_type else {}
        params.update(node.get("params") or {})
        params = _clean_params(node_type, params)
        entry = {"id": node["id"], "type": node_type, "name": name,
                 "x": _int(node.get("x")), "y": _int(node.get("y")),
                 "params": params}
        for flag in ("disabled", "continueOnFail"):
            if previous.get(flag) is True:
                entry[flag] = True
        out_nodes.append(entry)
    if counts.get(SCHEDULE, 0) > 1:
        raise WorkflowFileError("too_many_schedules")
    if counts.get(MANUAL, 0) > 1:
        raise WorkflowFileError("too_many_triggers")
    if counts.get(MONITOR, 0) > MAX_MONITORS:
        raise WorkflowFileError("too_many_monitors")
    by_id = {n["id"]: n for n in out_nodes}
    warnings = []
    out_connections = []
    seen = set()
    for conn in connections:
        source, target = conn["from"], conn["to"]
        if source not in by_id or target not in by_id or source == target:
            continue
        if by_id[target]["type"] in TRIGGERS:
            warnings.append("A connection into " + by_id[target]["name"] +
                            " is left out: triggers start a workflow")
            continue
        if (source, target) in seen:
            continue
        seen.add((source, target))
        out_connections.append(_edge(source, target))
    if len(out_connections) > MAX_CONNECTIONS:
        raise WorkflowFileError("too_many_connections")
    if _order(out_nodes, out_connections) is None:
        raise WorkflowFileError("cycle")
    workflow_id = match.group(1)
    data.update({"kind": FILE_KIND, "version": 1, "id": workflow_id,
                 "nodes": out_nodes, "connections": out_connections})
    agent = primary_agent(data)
    if not str(data.get("name") or "").strip():
        data["name"] = agent["name"] if agent else \
            workflow_id.replace("-", " ").title()
    if agent is not None:
        data["icon"] = str(agent["params"].get("avatar") or
                           data.get("icon") or "sol")
    data.setdefault("description", "")
    data.setdefault("icon", "workflow")
    data.setdefault("enabled", True)
    if not data.get("createdAtMs"):
        data["createdAtMs"] = int(now_ms)
    data["updatedAtMs"] = int(now_ms)
    return dump(data) + "\n", warnings


def _clean_params(node_type, params):
    if node_type == SCHEDULE:
        mode = str(params.get("mode") or "on-demand")
        if mode not in SCHEDULE_MODES:
            raise WorkflowFileError("invalid_schedule_mode")
        time_value = str(params.get("time") or "09:00").strip()
        if not TIME_RE.match(time_value):
            raise WorkflowFileError("invalid_schedule_time")
        days = sorted({str(_int(d, 0)) for d in _checks(params.get("days"))})
        if any(not 1 <= int(d) <= 7 for d in days):
            raise WorkflowFileError("invalid_schedule_days")
        events = _checks(params.get("events"))
        if any(e not in TASK_TYPES for e in events):
            raise WorkflowFileError("invalid_event")
        return {"mode": mode, "time": time_value, "days": days,
                "events": [e for e in TASK_TYPES if e in events],
                "active": _bool(params.get("active"))}
    if node_type == MONITOR:
        badge = str(params.get("badge") or "").strip()
        if not badge:
            raise WorkflowFileError("monitor_needs_badge")
        task_type = str(params.get("taskType") or "error-logs")
        if task_type not in TASK_TYPES:
            raise WorkflowFileError("invalid_event")
        return {"badge": badge[:40],
                "threshold": max(0, _int(params.get("threshold"), 0)),
                "taskType": task_type,
                "active": _bool(params.get("active"), True),
                "inclusive": _bool(params.get("inclusive"))}
    if node_type == AGENT:
        out = dict(params)
        if not str(out.get("specialty") or "").strip():
            raise WorkflowFileError("agent_needs_specialty")
        permissions = _checks(out.get("permissions"))
        if any(p not in PERMISSIONS for p in permissions):
            raise WorkflowFileError("unknown_permission")
        out["permissions"] = [p for p in PERMISSIONS if p in permissions]
        for key in ("skills", "skillRefs", "improvementNotes",
                    "marketingChannels"):
            if key in out:
                out[key] = _lines(out[key])
        for key, default in (("selfImproving", True), ("lowCostModel", False)):
            if key in out:
                out[key] = _bool(out[key], default)
        if "runWindowHours" in out:
            hours = _int(out["runWindowHours"], 0)
            if not 0 <= hours <= 720:
                raise WorkflowFileError("invalid_run_window")
            out["runWindowHours"] = hours
        if str(out.get("workflow") or "") not in NATIVE_WORKFLOWS:
            raise WorkflowFileError("invalid_native_workflow")
        return out
    if node_type == SHORTCUT:
        file_name = str(params.get("file") or "").strip()
        if not file_name or "/" in file_name or "\\" in file_name or \
                file_name.startswith("."):
            raise WorkflowFileError("shortcut_needs_file")
        return {"file": file_name, "wait": _bool(params.get("wait"), True),
                "allowEdits": _bool(params.get("allowEdits"))}
    if node_type == OUTPUT:
        path = str(params.get("path") or "").strip()
        if path and not safe_repository_path(path):
            raise WorkflowFileError("invalid_output_path")
        return {"title": str(params.get("title") or "")[:120],
                "path": safe_repository_path(path) if path else ""}
    if node_type == NOTE:
        return {"text": str(params.get("text") or "")[:4000]}
    return {}


def _checks(value):
    if isinstance(value, list):
        return [str(item) for item in value]
    text = str(value or "").strip()
    if not text:
        return []
    if text.startswith("["):
        try:
            parsed = json.loads(text)
            if isinstance(parsed, list):
                return [str(item) for item in parsed]
        except ValueError:
            pass
    return [part.strip() for part in text.split(",") if part.strip()]


def new_text(path, dump):
    match = PATH_RE.match(path or "")
    if not match:
        raise WorkflowFileError("unsupported_path")
    workflow_id = match.group(1)
    data = {"kind": FILE_KIND, "version": 1, "id": workflow_id,
            "name": workflow_id.replace("-", " ").title(),
            "description": "", "icon": "workflow", "enabled": True,
            "nodes": [_node("run", MANUAL, "Run by hand", 0, 0, {}),
                      _node("output", OUTPUT, "Save the report", 1, 0,
                            {"title": "", "path": ""})],
            "connections": [_edge("run", "output")]}
    return dump(data) + "\n"


def as_teammate(data):
    if isinstance(data, dict) and isinstance(data.get("nodes"), list):
        return workflow_to_teammate(data)
    return data
