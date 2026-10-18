import json
import re

ACTION_PATH_RE = re.compile(r"^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$")
SHORTCUT_PATH_RE = re.compile(r"^\.forkmesh/shortcuts/[A-Za-z0-9._-]{1,96}$")
SKILL_PATH_RE = re.compile(r"^\.forkmesh/skills/[A-Za-z0-9._-]{1,96}\.skill$")
AGENT_PATH_RE = re.compile(r"^\.forkmesh/teammates/([a-z0-9][a-z0-9-]{0,63})\.json$")
AGENT_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
TIME_RE = re.compile(r"^([01]\d|2[0-3]):[0-5]\d$")
MAX_FILE_BYTES = 256 * 1024
MAX_ACTION_STEPS = 64
MAX_ACTION_EVENTS = 8
MAX_DEPTH = 16
COLUMN = 288
ROW = 120
SCRIPT_EXTENSIONS = (".sh", ".bash")
PROMPT_EXTENSIONS = (".md", ".txt", ".prompt")
HEADER_KEYS = ("name", "description", "agent", "model")
STEP_RE = re.compile(r"^#\s*step:\s*(.+?)\s*$")
RUN_SHORTCUT_RE = re.compile(
    r"^\s*bash\s+\.forkmesh/shortcuts/([A-Za-z0-9._-]{1,96})(?:\s|$)")
MAX_STEP_NAME = 80
ACTION_EVENTS = ("push", "pull_request", "release", "workflow_dispatch")
ACTION_TOP_KEYS = ("name", "on", "runs-on", "needs", "env", "checkout",
                   "fetch-depth", "steps", "jobs")
SCHEDULE_MODES = ("on-demand", "continuous", "hourly", "daily", "weekly",
                  "weekdays", "custom", "events")
TASK_TYPES = ("on-demand", "scheduled", "new-issues", "new-pulls",
              "assignments", "mentions", "ui-stalls", "error-logs")
PERMISSIONS = ("read-repositories", "modify-code", "create-branches",
               "review-prs", "manage-issues", "merge", "external-tools",
               "run-unattended", "run-git", "run-tests", "read-forkmesh-web",
               "process-payouts")
NATIVE_WORKFLOWS = ("", "contributor-payouts")
SKILL_SCOPES = ("repository", "app", "organization")
DAY_NAMES = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")


class FileError(ValueError):
    pass


def _opts(values, labels=None):
    return [{"value": v, "label": (labels or {}).get(v, v or "(none)")}
            for v in values]


def _field(key, label, kind="string", **extra):
    field = {"key": key, "label": label, "type": kind}
    field.update(extra)
    return field


NOTES = _field("notes", "Comments (kept as # lines)", "text")

FILE_NODES = {
    "action.trigger": {
        "kind": "action", "name": "Workflow trigger", "group": "Action",
        "icon": "zap", "color": "#0284c7", "root": True, "single": True, "inputs": False,
        "outputs": ["main"],
        "description": "When the action runs: events, changed paths, where "
                       "it runs and what it waits for.",
        "fields": [
            _field("name", "Action name"),
            _field("events", "Run on", "checks",
                   options=_opts(ACTION_EVENTS, {
                       "push": "Push", "pull_request": "Pull request",
                       "release": "Release",
                       "workflow_dispatch": "Manual run"})),
            _field("paths", "Only when these paths change (one per line)",
                   "list", placeholder="native/ios/*"),
            _field("branches", "Only on these branches (one per line)",
                   "list"),
            _field("runsOn", "Runs on (one label per line)", "list",
                   placeholder="forkmesh"),
            _field("needs", "Waits for these actions (one per line)", "list",
                   placeholder=".forkmesh/ci.yml"),
            _field("checkout", "Check out the repository", "select",
                   options=_opts(("", "true", "false"),
                                 {"": "Default (yes)", "true": "Yes",
                                  "false": "No"})),
            _field("fetchDepth", "Fetch depth", "number"),
            _field("env", "Environment (JSON object)", "json",
                   placeholder='{"CI": "1"}'),
            NOTES,
        ],
    },
    "action.job": {
        "kind": "action", "name": "Job", "group": "Action", "icon": "box",
        "color": "#4f46e5", "outputs": ["main"],
        "description": "A group of steps. ForkMesh runs every job's steps "
                       "in order.",
        "fields": [
            _field("id", "Job id", required=True, placeholder="test"),
            _field("runsOn", "Runs on (one label per line)", "list"),
            _field("needs", "Waits for (one per line)", "list"),
            NOTES,
        ],
    },
    "action.step": {
        "kind": "action", "name": "Run step", "group": "Action",
        "icon": "terminal", "color": "#0f766e", "outputs": ["main"],
        "description": "A shell command. ${{ vars.NAME }} reads an Actions "
                       "variable on the desktop.",
        "fields": [
            _field("name", "Step name"),
            _field("run", "Command", "code", required=True,
                   placeholder="set -eu\nmake test"),
            NOTES,
        ],
    },
    "shortcut.header": {
        "kind": "shortcut", "name": "Shortcut", "group": "Shortcut",
        "icon": "command", "color": "#c2410c", "root": True, "single": True, "inputs": False,
        "outputs": ["main"],
        "description": "A desktop shortcut: a script it runs or a prompt it "
                       "gives an agent.",
        "fields": [
            _field("name", "Name", required=True),
            _field("description", "Description", "text"),
            _field("agent", "Agent", placeholder="codex"),
            _field("model", "Model"),
        ],
    },
    "shortcut.block": {
        "kind": "shortcut", "name": "Block", "group": "Shortcut",
        "icon": "align-left", "color": "#9a3412", "outputs": ["main"],
        "description": "Part of the script or prompt, in order. "
                       "{{appLog}} and {{stallLog}} are filled in when it runs.",
        "fields": [_field("text", "Text", "code", required=True)],
    },
    "shortcut.run": {
        "kind": "shortcut", "name": "Run shortcut", "group": "Shortcut",
        "icon": "corner-down-right", "color": "#2563eb", "outputs": ["main"],
        "description": "A step that runs another shortcut in this folder "
                       "with bash .forkmesh/shortcuts/<file>.",
        "fields": [_field("text", "Text", "code", required=True,
                          placeholder="# step: Deploy\nbash .forkmesh/"
                                      "shortcuts/deploy.sh")],
    },
    "skill.header": {
        "kind": "skill", "name": "Skill", "group": "Skill",
        "icon": "graduation-cap", "color": "#7c3aed", "root": True,
        "single": True, "inputs": False, "outputs": ["main"],
        "description": "Instructions an agent follows when this skill is "
                       "installed.",
        "fields": [
            _field("name", "Name", required=True),
            _field("description", "Description", "text"),
        ],
    },
    "skill.block": {
        "kind": "skill", "name": "Instruction", "group": "Skill",
        "icon": "list-checks", "color": "#6d28d9", "outputs": ["main"],
        "description": "One section of the skill's instructions.",
        "fields": [_field("text", "Text", "code", required=True)],
    },
    "agent.schedule": {
        "kind": "agent", "name": "Schedule", "group": "Triggers",
        "icon": "calendar-clock", "color": "#0891b2", "single": True,
        "inputs": False, "outputs": ["main"],
        "description": "When the special agent runs by itself.",
        "fields": [
            _field("mode", "How often", "select", options=_opts(
                SCHEDULE_MODES, {"on-demand": "Only when asked",
                                 "continuous": "Continuously",
                                 "hourly": "Every hour", "daily": "Every day",
                                 "weekly": "Every week",
                                 "weekdays": "Weekdays",
                                 "custom": "Chosen days",
                                 "events": "On events"}), default="daily"),
            _field("time", "Time (HH:mm)", placeholder="09:00",
                   default="09:00"),
            _field("days", "Days", "checks", options=[
                {"value": str(i + 1), "label": DAY_NAMES[i]}
                for i in range(7)]),
            _field("active", "Schedule is on", "boolean", default="false"),
        ],
    },
    "agent.events": {
        "kind": "agent", "name": "Events", "group": "Triggers",
        "icon": "bell-ring", "color": "#0e7490", "single": True,
        "inputs": False, "outputs": ["main"],
        "description": "Repository events that start a run.",
        "fields": [_field("events", "Run on", "checks", options=_opts(
            TASK_TYPES, {"on-demand": "On-demand work",
                         "scheduled": "Scheduled work",
                         "new-issues": "New issues",
                         "new-pulls": "New pull requests",
                         "assignments": "Assignments",
                         "mentions": "Mentions",
                         "ui-stalls": "UI stall repair",
                         "error-logs": "Error-log repair"}))],
    },
    "agent.monitor": {
        "kind": "agent", "name": "Log monitor", "group": "Triggers",
        "icon": "activity", "color": "#155e75", "inputs": False,
        "outputs": ["main"],
        "description": "Starts a run when an app-log badge passes a "
                       "threshold.",
        "fields": [
            _field("badge", "Badge", required=True, placeholder="STALL"),
            _field("threshold", "Threshold", "number", default="10"),
            _field("taskType", "Task type", "select",
                   options=_opts(TASK_TYPES), default="error-logs"),
            _field("active", "Monitor is on", "boolean", default="true"),
            _field("inclusive", "Include the threshold", "boolean",
                   default="false"),
        ],
    },
    "agent.profile": {
        "kind": "agent", "name": "Special agent", "group": "Agent",
        "icon": "bot", "color": "#db2777", "root": True, "single": True,
        "outputs": ["main"],
        "description": "Who the agent is, what it may do and how it works.",
        "fields": [
            _field("name", "Name", required=True),
            _field("id", "File id", required=True, placeholder="sentinel"),
            _field("specialty", "Specialty", required=True),
            _field("focus", "Focus"),
            _field("avatar", "Portrait", default="sol"),
            _field("description", "Responsibility", "text"),
            _field("instructions", "Operating instructions", "code"),
            _field("skills", "Skill names (one per line)", "list"),
            _field("permissions", "Permissions", "checks",
                   options=_opts(PERMISSIONS)),
            _field("enabled", "Available", "boolean", default="true"),
            _field("selfImproving", "Keeps a learning journal", "boolean",
                   default="false"),
            _field("lowCostModel", "Use the low-cost model", "boolean",
                   default="false"),
            _field("runWindowHours", "Expected run window (hours)", "number",
                   default="0"),
            _field("marketingChannels", "Marketing channels (one per line)",
                   "list"),
        ],
    },
    "agent.shortcut": {
        "kind": "agent", "name": "Playbook", "group": "Uses",
        "icon": "command", "color": "#c2410c", "single": True,
        "outputs": [],
        "description": "The shortcut in .forkmesh/shortcuts that becomes "
                       "the run's assignment.",
        "fields": [_field("file", "Shortcut file", required=True,
                          placeholder="review-security.md")],
    },
    "agent.skill": {
        "kind": "agent", "name": "Skill file", "group": "Uses",
        "icon": "graduation-cap", "color": "#7c3aed", "outputs": [],
        "description": "A skill file added to the agent's instructions.",
        "fields": [
            _field("scope", "Where", "select", options=_opts(
                SKILL_SCOPES, {"repository": "This repository (.forkmesh/skills)",
                               "app": "The desktop app",
                               "organization": "The organization"}),
                   default="repository"),
            _field("file", "File", required=True,
                   placeholder="security-review.skill"),
        ],
    },
    "agent.workflow": {
        "kind": "agent", "name": "Native workflow", "group": "Uses",
        "icon": "workflow", "color": "#16a34a", "single": True,
        "outputs": [],
        "description": "A built-in desktop workflow the agent runs.",
        "fields": [_field("workflow", "Workflow", "select",
                          options=_opts(NATIVE_WORKFLOWS, {
                              "": "None",
                              "contributor-payouts": "Contributor payouts"}))],
    },
    "agent.relay": {
        "kind": "agent", "name": "Relay channel", "group": "Uses",
        "icon": "messages-square", "color": "#2563eb", "single": True,
        "outputs": [],
        "description": "The chat room that gets a summary after each run.",
        "fields": [_field("channel", "Room", required=True,
                          placeholder="#general")],
    },
}

KINDS = {
    "action": {"label": "Action", "plural": "Actions", "folder": ".forkmesh",
               "icon": "zap", "root": "action.trigger",
               "pattern": ACTION_PATH_RE.pattern,
               "description": "Commands that run on push, pull requests, "
                              "releases or by hand (.forkmesh/*.yml)."},
    "shortcut": {"label": "Shortcut", "plural": "Shortcuts",
                 "folder": ".forkmesh/shortcuts", "icon": "command",
                 "root": "shortcut.header",
                 "pattern": SHORTCUT_PATH_RE.pattern,
                 "description": "Scripts and prompts on the desktop's "
                                "Shortcuts panel."},
    "skill": {"label": "Skill", "plural": "Skills",
              "folder": ".forkmesh/skills", "icon": "graduation-cap",
              "root": "skill.header", "pattern": SKILL_PATH_RE.pattern,
              "description": "Instructions special agents can install."},
    "agent": {"label": "Special agent", "plural": "Special agents",
              "folder": ".forkmesh/teammates", "icon": "bot",
              "root": "agent.profile", "pattern": AGENT_PATH_RE.pattern,
              "description": "Agents with a role, schedule, skills and a "
                             "playbook."},
}


def catalog():
    return {"kinds": KINDS,
            "nodes": [dict(spec, type=key) for key, spec in FILE_NODES.items()]}


def kind_for_path(path):
    text = str(path or "")
    if AGENT_PATH_RE.match(text):
        return "agent"
    if SKILL_PATH_RE.match(text):
        return "skill"
    if SHORTCUT_PATH_RE.match(text):
        return "shortcut"
    if ACTION_PATH_RE.match(text):
        return "action"
    return ""


def _node(node_id, node_type, name, column, row, params):
    return {"id": node_id, "type": node_type, "name": name,
            "x": int(column * COLUMN), "y": int(row * ROW), "params": params,
            "credential": "", "continueOnFail": False, "disabled": False}


def _lines(value):
    if isinstance(value, list):
        items = value
    else:
        items = str(value or "").replace("\r", "").split("\n")
    return [str(item).strip() for item in items if str(item).strip()]


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


class _YNode:
    def __init__(self, kind, line=-1):
        self.kind = kind
        self.keys = []
        self.values = []
        self.lines = []
        self.items = []
        self.item_lines = []
        self.scalar = ""
        self.line = line

    def get(self, key):
        if self.kind != "map":
            return None
        for index, existing in enumerate(self.keys):
            if existing == key:
                return self.values[index]
        return None

    def line_of(self, key):
        for index, existing in enumerate(self.keys):
            if existing == key:
                return self.lines[index]
        return -1

    def strings(self):
        if self.kind == "seq":
            return [item.scalar.strip() for item in self.items
                    if item.kind == "scalar" and item.scalar.strip()]
        if self.kind == "scalar" and self.scalar.strip():
            return [self.scalar.strip()]
        return []


def _strip_comment(line):
    in_single = in_double = False
    index = 0
    while index < len(line):
        char = line[index]
        if char == "\\" and in_double:
            index += 2
            continue
        if char == "'" and not in_double:
            in_single = not in_single
        elif char == '"' and not in_single:
            in_double = not in_double
        elif char == "#" and not in_single and not in_double:
            if index == 0 or line[index - 1] in " \t":
                return line[:index]
        index += 1
    return line


class _YParser:
    def __init__(self, content):
        self.lines = []
        self.consumed = set()
        for raw in content.replace("\r\n", "\n").split("\n"):
            stripped = _strip_comment(raw).rstrip(" ")
            text = stripped.lstrip(" ")
            self.lines.append([len(stripped) - len(text), text, raw])
        self.pos = 0

    def fail(self):
        return FileError("yaml_syntax")

    def skip_blank(self):
        while self.pos < len(self.lines) and self.lines[self.pos][1] == "":
            self.pos += 1

    def parse(self):
        self.skip_blank()
        if self.pos >= len(self.lines):
            raise self.fail()
        if self.lines[self.pos][1] == "---":
            self.pos += 1
            self.skip_blank()
        if self.pos >= len(self.lines):
            raise self.fail()
        node = self.block(self.lines[self.pos][0], 0)
        self.skip_blank()
        if self.pos != len(self.lines):
            raise self.fail()
        return node

    def block(self, indent, depth):
        if depth > MAX_DEPTH:
            raise self.fail()
        self.skip_blank()
        if self.pos >= len(self.lines) or self.lines[self.pos][0] != indent:
            raise self.fail()
        if _is_seq_item(self.lines[self.pos][1]):
            return self.sequence(indent, depth)
        return self.mapping(indent, depth)

    def sequence(self, indent, depth):
        node = _YNode("seq", self.pos)
        while True:
            self.skip_blank()
            if self.pos >= len(self.lines) or self.lines[self.pos][0] != indent \
                    or not _is_seq_item(self.lines[self.pos][1]):
                break
            line = self.lines[self.pos]
            start = self.pos
            rest = line[1][1:].lstrip(" ")
            if rest == "":
                self.pos += 1
                self.skip_blank()
                if self.pos >= len(self.lines) or \
                        self.lines[self.pos][0] <= indent:
                    node.items.append(_YNode("scalar", start))
                    node.item_lines.append(start)
                    continue
                node.items.append(self.block(self.lines[self.pos][0],
                                             depth + 1))
                node.item_lines.append(start)
                continue
            if _split_key(rest) is not None:
                item_indent = indent + (len(line[1]) - len(rest))
                self.lines[self.pos] = [item_indent, rest,
                                        " " * item_indent + rest]
                node.items.append(self.mapping(item_indent, depth + 1))
                node.item_lines.append(start)
                continue
            value = _inline_value(rest)
            value.line = start
            self.pos += 1
            node.items.append(value)
            node.item_lines.append(start)
        if not node.items:
            raise self.fail()
        return node

    def mapping(self, indent, depth):
        if depth > MAX_DEPTH:
            raise self.fail()
        node = _YNode("map", self.pos)
        while True:
            self.skip_blank()
            if self.pos >= len(self.lines) or self.lines[self.pos][0] != indent \
                    or _is_seq_item(self.lines[self.pos][1]):
                break
            split = _split_key(self.lines[self.pos][1])
            if split is None:
                raise self.fail()
            key, rest = split
            if key in node.keys:
                raise self.fail()
            key_line = self.pos
            self.pos += 1
            if rest == "":
                self.skip_blank()
                if self.pos < len(self.lines) and \
                        self.lines[self.pos][0] > indent:
                    value = self.block(self.lines[self.pos][0], depth + 1)
                elif self.pos < len(self.lines) and \
                        self.lines[self.pos][0] == indent and \
                        _is_seq_item(self.lines[self.pos][1]):
                    value = self.sequence(indent, depth + 1)
                else:
                    value = _YNode("scalar", key_line)
            elif rest in ("|", "|-", "|+", ">", ">-", ">+"):
                value = _YNode("scalar", key_line)
                value.scalar = self.block_scalar(indent, rest)
            else:
                value = _inline_value(rest)
                value.line = key_line
            node.keys.append(key)
            node.values.append(value)
            node.lines.append(key_line)
        if not node.keys:
            raise self.fail()
        return node

    def block_scalar(self, key_indent, header):
        body = []
        block_indent = -1
        while self.pos < len(self.lines):
            raw = self.lines[self.pos][2]
            if raw.strip() == "":
                body.append("")
                self.consumed.add(self.pos)
                self.pos += 1
                continue
            indent = len(raw) - len(raw.lstrip(" "))
            if indent <= key_indent:
                break
            if block_indent < 0:
                block_indent = indent
            if indent < block_indent:
                break
            body.append(raw[block_indent:].rstrip("\r"))
            self.consumed.add(self.pos)
            self.pos += 1
        while body and body[-1] == "":
            body.pop()
        if not body:
            return ""
        if header.startswith(">"):
            return " ".join(body)
        text = "\n".join(body)
        return text if header == "|-" else text + "\n"

    def comments_before(self, line_index):
        found = []
        index = line_index - 1
        while index >= 0 and index not in self.consumed:
            raw = self.lines[index][2].strip()
            if raw.startswith("#"):
                found.append(re.sub(r"^#\s?", "", raw))
            elif raw:
                break
            index -= 1
        return "\n".join(reversed(found)).strip("\n")


def _is_seq_item(text):
    return text == "-" or text.startswith("- ")


def _split_key(text):
    if not text or text[0] in "[{\t":
        return None
    if text[0] in "\"'":
        quote = text[0]
        end = text.find(quote, 1)
        if end < 0:
            return None
        after = text[end + 1:]
        if after != ":" and not after.startswith(": "):
            return None
        return text[1:end], after[1:].lstrip(" ")
    for index, char in enumerate(text):
        if char != ":":
            continue
        if index + 1 == len(text) or text[index + 1] == " ":
            key = text[:index].strip()
            if not key:
                return None
            return key, text[index + 1:].lstrip(" ")
    return None


def _inline_value(text):
    text = text.strip()
    if text.startswith("["):
        if not text.endswith("]"):
            raise FileError("yaml_syntax")
        node = _YNode("seq")
        inner = text[1:-1].strip()
        if inner:
            for part in _split_flow(inner):
                item = _YNode("scalar")
                item.scalar = _scalar(part.strip())
                node.items.append(item)
                node.item_lines.append(-1)
        return node
    if text.startswith("{"):
        if text == "{}":
            return _YNode("scalar")
        raise FileError("yaml_syntax")
    node = _YNode("scalar")
    node.scalar = _scalar(text)
    return node


def _split_flow(inner):
    parts = []
    current = []
    quote = ""
    for char in inner:
        if quote:
            if char == quote:
                quote = ""
            current.append(char)
        elif char in "\"'":
            quote = char
            current.append(char)
        elif char == ",":
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    parts.append("".join(current))
    return parts


def _scalar(text):
    if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        out = []
        body = text[1:-1]
        index = 0
        while index < len(body):
            char = body[index]
            if char != "\\" or index + 1 == len(body):
                out.append(char)
                index += 1
                continue
            nxt = body[index + 1]
            mapping = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "/": "/"}
            if nxt not in mapping:
                raise FileError("yaml_syntax")
            out.append(mapping[nxt])
            index += 2
        return "".join(out)
    if text and text[0] in "\"'&*!%@":
        raise FileError("yaml_syntax")
    if text in ("~", "null"):
        return ""
    return text


PLAIN_RE = re.compile(r"^[A-Za-z0-9_./*$][A-Za-z0-9_./*$ @+=()-]*$")


def _quote(value):
    text = str(value)
    if text and PLAIN_RE.match(text) and not text.endswith(" ") and \
            text not in ("null", "~", "true", "false", "yes", "no", "on",
                         "off") and not text[0].isspace():
        return text
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace(
        "\n", "\\n").replace("\t", "\\t") + '"'


def _flow(items):
    return "[" + ", ".join(_quote(item) for item in items) + "]"


def _comment_lines(text, pad):
    out = []
    for line in str(text or "").replace("\r", "").split("\n"):
        out.append(pad + ("# " + line if line.strip() else "#"))
    return out if str(text or "").strip() else []


def parse_action(path, text):
    if len(text.encode("utf-8")) > MAX_FILE_BYTES:
        raise FileError("file_too_large")
    parser = _YParser(text)
    root = parser.parse()
    if root.kind != "map":
        raise FileError("not_a_mapping")
    warnings = []
    for key in root.keys:
        if key not in ACTION_TOP_KEYS:
            warnings.append("The '" + key + "' setting is not used by "
                            "ForkMesh Actions and is removed when you save")
    events = []
    paths = []
    branches = []
    on_node = root.get("on")
    on_map = False
    if on_node is not None and on_node.kind == "map":
        on_map = True
        events = list(on_node.keys)
        push = on_node.get("push")
        if push is not None and push.kind == "map":
            paths = (push.get("paths") or _YNode("seq")).strings()
            branches = (push.get("branches") or _YNode("seq")).strings()
    elif on_node is not None:
        events = on_node.strings()
    env = {}
    env_node = root.get("env")
    if env_node is not None and env_node.kind == "map":
        for index, key in enumerate(env_node.keys):
            if env_node.values[index].kind == "scalar":
                env[key] = env_node.values[index].scalar
    checkout = root.get("checkout")
    depth = root.get("fetch-depth")
    name_node = root.get("name")
    key_comments = {key: parser.comments_before(root.lines[i])
                    for i, key in enumerate(root.keys)}
    head = key_comments.pop(root.keys[0], "") if root.keys else ""
    trigger = {
        "name": name_node.scalar.strip() if name_node is not None else "",
        "events": events,
        "paths": "\n".join(paths),
        "branches": "\n".join(branches),
        "runsOn": "\n".join((root.get("runs-on") or _YNode("seq")).strings()),
        "needs": "\n".join((root.get("needs") or _YNode("seq")).strings()),
        "checkout": "" if checkout is None else (
            "false" if checkout.scalar.strip().lower() in (
                "false", "no", "off", "0") else "true"),
        "fetchDepth": depth.scalar.strip() if depth is not None else "",
        "env": json.dumps(env, indent=2) if env else "",
        "notes": head,
    }
    nodes = [_node("trigger", "action.trigger",
                   trigger["name"] or path.rsplit("/", 1)[-1], 0, 0, trigger)]
    counter = [0]

    def add_steps(steps_node, column, row, prefix_comments=""):
        added = []
        if steps_node is None or steps_node.kind != "seq":
            return added
        for index, item in enumerate(steps_node.items):
            if item.kind != "map":
                continue
            run_node = item.get("run")
            uses = item.get("uses")
            if run_node is None or not run_node.scalar.strip():
                if uses is not None:
                    warnings.append("A step that uses '" + uses.scalar
                                    + "' is ignored by ForkMesh Actions and "
                                    "is removed when you save")
                continue
            extra = [k for k in item.keys if k not in ("name", "run")]
            if extra:
                warnings.append("Step settings " + ", ".join(extra)
                                + " are not used by ForkMesh Actions and are "
                                "removed when you save")
            counter[0] += 1
            name_value = item.get("name")
            notes = parser.comments_before(steps_node.item_lines[index])
            if index == 0 and prefix_comments:
                notes = (prefix_comments + "\n" + notes).strip("\n")
            params = {"name": name_value.scalar.strip()
                      if name_value is not None else "",
                      "run": run_node.scalar, "notes": notes}
            label = params["name"] or run_node.scalar.strip().split(
                "\n", 1)[0][:50]
            added.append(_node("s%d" % counter[0], "action.step",
                               label[:60] or "Step %d" % counter[0],
                               column + len(added), row, params))
        return added

    chain = list(nodes)
    top = add_steps(root.get("steps"), 1, 0,
                    key_comments.pop("steps", ""))
    nodes.extend(top)
    chain.extend(top)
    jobs = root.get("jobs")
    job_comments = key_comments.pop("jobs", "")
    row = 1 if top else 0
    if jobs is not None and jobs.kind == "map":
        for index, job_id in enumerate(jobs.keys):
            job = jobs.values[index]
            notes = parser.comments_before(jobs.lines[index])
            if index == 0 and job_comments:
                notes = (job_comments + "\n" + notes).strip("\n")
            params = {"id": job_id, "runsOn": "", "needs": "",
                      "notes": notes}
            steps_node = None
            steps_comments = ""
            if job.kind == "map":
                params["runsOn"] = "\n".join(
                    (job.get("runs-on") or _YNode("seq")).strings())
                params["needs"] = "\n".join(
                    (job.get("needs") or _YNode("seq")).strings())
                steps_node = job.get("steps")
                if job.line_of("steps") >= 0:
                    steps_comments = parser.comments_before(
                        job.line_of("steps"))
            job_node = _node("j%d" % (index + 1), "action.job",
                             "Job " + job_id, 1, row, params)
            nodes.append(job_node)
            chain.append(job_node)
            steps = add_steps(steps_node, 2, row, steps_comments)
            nodes.extend(steps)
            chain.extend(steps)
            row += 1
    connections = [{"from": chain[i]["id"], "output": "main",
                    "to": chain[i + 1]["id"]} for i in range(len(chain) - 1)]
    trailing = parser.comments_before(len(parser.lines))
    meta = {"onMap": on_map, "keyComments": {k: v for k, v in
                                             key_comments.items() if v},
            "trailing": trailing}
    return nodes, connections, meta, warnings


def _env_object(value):
    if isinstance(value, dict):
        return value
    text = str(value or "").strip()
    if not text:
        return {}
    try:
        parsed = json.loads(text)
    except ValueError:
        raise FileError("env_not_json")
    if not isinstance(parsed, dict):
        raise FileError("env_not_json")
    return parsed


def _run_lines(run, pad):
    text = str(run or "").replace("\r\n", "\n")
    header = "|" if text.endswith("\n") else "|-"
    body = text.rstrip("\n").split("\n")
    content = [line for line in body if line.strip()]
    if content:
        first = len(content[0]) - len(content[0].lstrip(" "))
        least = min(len(line) - len(line.lstrip(" ")) for line in content)
        if first > least:
            raise FileError("run_first_line_indented")
    out = [header]
    for line in body:
        out.append(pad + line if line.strip() else "")
    return out


def render_action(nodes, chain, meta):
    trigger = next(n for n in chain if n["type"] == "action.trigger")
    t = trigger.get("params") or {}
    key_comments = (meta or {}).get("keyComments") or {}
    out = []
    out.extend(_comment_lines(t.get("notes"), ""))

    def keyed(key, lines):
        out.extend(_comment_lines(key_comments.get(key), ""))
        out.extend(lines)

    if str(t.get("name") or "").strip():
        keyed("name", ["name: " + _quote(str(t["name"]).strip())])
    events = [e for e in _checks(t.get("events")) if e]
    if len(events) > MAX_ACTION_EVENTS:
        raise FileError("too_many_events")
    paths = _lines(t.get("paths"))
    branches = _lines(t.get("branches"))
    if events:
        if paths or branches or (meta or {}).get("onMap"):
            lines = ["on:"]
            for event in events:
                if event == "push" and (paths or branches):
                    lines.append("  push:")
                    if paths:
                        lines.append("    paths:")
                        lines.extend("      - " + json.dumps(p)
                                     for p in paths)
                    if branches:
                        lines.append("    branches:")
                        lines.extend("      - " + json.dumps(b)
                                     for b in branches)
                else:
                    lines.append("  " + _quote(event) + ":")
            keyed("on", lines)
        else:
            keyed("on", ["on: " + _flow(events)])
    runs_on = _lines(t.get("runsOn"))
    if runs_on:
        keyed("runs-on", ["runs-on: " + (_quote(runs_on[0])
                                         if len(runs_on) == 1
                                         else _flow(runs_on))])
    needs = _lines(t.get("needs"))
    if needs:
        keyed("needs", ["needs: " + _flow(needs)])
    if t.get("checkout") in ("true", "false"):
        keyed("checkout", ["checkout: " + t["checkout"]])
    if str(t.get("fetchDepth") or "").strip():
        depth = _int(t["fetchDepth"], -1)
        if depth < 0:
            raise FileError("invalid_fetch_depth")
        keyed("fetch-depth", ["fetch-depth: " + str(depth)])
    variables = _env_object(t.get("env"))
    if variables:
        keyed("env", ["env:"] + ["  " + _quote(k) + ": " + _quote(
            "" if v is None else (json.dumps(v) if isinstance(
                v, (dict, list, bool)) else str(v)))
            for k, v in variables.items()])
    groups = []
    current = {"job": None, "steps": []}
    for node in chain:
        if node["type"] == "action.job":
            groups.append(current)
            current = {"job": node, "steps": []}
        elif node["type"] == "action.step":
            current["steps"].append(node)
    groups.append(current)
    total = sum(len(g["steps"]) for g in groups)
    if total == 0:
        raise FileError("no_steps")
    if total > MAX_ACTION_STEPS:
        raise FileError("too_many_steps")

    def step_lines(step, pad):
        p = step.get("params") or {}
        if not str(p.get("run") or "").strip():
            raise FileError("empty_step")
        lines = _comment_lines(p.get("notes"), pad)
        name = str(p.get("name") or "").strip()
        if name:
            lines.append(pad + "- name: " + _quote(name))
            run_pad = pad + "  "
            run = _run_lines(p.get("run"), run_pad + "  ")
            lines.append(run_pad + "run: " + run[0])
        else:
            run = _run_lines(p.get("run"), pad + "    ")
            lines.append(pad + "- run: " + run[0])
        lines.extend(run[1:])
        return lines

    if groups[0]["job"] is None and groups[0]["steps"]:
        out.extend(_comment_lines(key_comments.get("steps"), ""))
        out.append("steps:")
        for step in groups[0]["steps"]:
            out.extend(step_lines(step, "  "))
    jobs = [g for g in groups if g["job"] is not None]
    if jobs:
        out.append("jobs:")
        seen = set()
        for group in jobs:
            p = group["job"].get("params") or {}
            job_id = str(p.get("id") or "").strip()
            if not job_id or job_id in seen or not re.match(
                    r"^[A-Za-z0-9_.-]{1,64}$", job_id):
                raise FileError("invalid_job_id")
            seen.add(job_id)
            out.extend(_comment_lines(p.get("notes"), "  "))
            out.append("  " + job_id + ":")
            job_runs_on = _lines(p.get("runsOn"))
            if job_runs_on:
                out.append("    runs-on: " + (_quote(job_runs_on[0])
                                              if len(job_runs_on) == 1
                                              else _flow(job_runs_on)))
            job_needs = _lines(p.get("needs"))
            if job_needs:
                out.append("    needs: " + _flow(job_needs))
            if group["steps"]:
                out.append("    steps:")
                for step in group["steps"]:
                    out.extend(step_lines(step, "      "))
    out.extend(_comment_lines((meta or {}).get("trailing"), ""))
    return "\n".join(out) + "\n"


def parse_header_file(kind, path, text):
    if len(text.encode("utf-8")) > MAX_FILE_BYTES:
        raise FileError("file_too_large")
    lines = text.replace("\r\n", "\n").split("\n")
    shebang = ""
    index = 0
    if lines and lines[0].startswith("#!"):
        shebang = lines[0]
        index = 1
    header = {key: "" for key in HEADER_KEYS}
    found = False
    while index < len(lines) and index < 20:
        match = re.match(r"^#\s*(name|description|agent|model):\s?(.*)$",
                         lines[index])
        if not match:
            break
        header[match.group(1)] = match.group(2).strip()
        found = True
        index += 1
    body_lines = lines[index:]
    blank_after = bool(body_lines) and body_lines[0].strip() == "" and found
    body = "\n".join(body_lines).strip("\n")
    blocks = [b for b in re.split(r"\n[ \t]*\n", body) if b.strip()] \
        if body.strip() else []
    root_type = kind + ".header"
    block_type = kind + ".block"
    params = {"name": header["name"], "description": header["description"]}
    if kind == "shortcut":
        params["agent"] = header["agent"]
        params["model"] = header["model"]
    file_name = path.rsplit("/", 1)[-1]
    nodes = [_node("h", root_type, params["name"] or file_name, 0, 0, params)]
    script = is_script(path)
    steps = kind == "shortcut" and script
    label = "Commands" if script else "Section"
    for number, block in enumerate(blocks, 1):
        node_type, name, params = block_type, "", {"text": block}
        if steps:
            name = shortcut_step_name(block)
            target = shortcut_link(block)
            if target:
                node_type = "shortcut.run"
                params["shortcut"] = target
        nodes.append(_node("b%d" % number, node_type,
                           name or "%s %d" % (label, number), number, 0,
                           params))
    connections = [{"from": nodes[i]["id"], "output": "main",
                    "to": nodes[i + 1]["id"]} for i in range(len(nodes) - 1)]
    meta = {"shebang": shebang, "blankAfterHeader": blank_after}
    if text and not text.endswith("\n"):
        meta["finalNewline"] = False
    return nodes, connections, meta, []


def shortcut_step_name(block):
    for line in str(block or "").split("\n"):
        if not line.strip():
            continue
        match = STEP_RE.match(line)
        if not match:
            return ""
        return " ".join(match.group(1).split())[:MAX_STEP_NAME]
    return ""


def shortcut_link(block):
    for line in str(block or "").split("\n"):
        match = RUN_SHORTCUT_RE.match(line)
        if match:
            return match.group(1)
    return ""


def is_script(path):
    name = str(path or "").rsplit("/", 1)[-1].lower()
    if name.endswith(SCRIPT_EXTENSIONS):
        return True
    return "." not in name


def render_header_file(kind, path, chain, meta):
    root = chain[0]
    params = root.get("params") or {}
    out = []
    shebang = (meta or {}).get("shebang") or ""
    if kind == "shortcut" and is_script(path) and not shebang:
        shebang = "#!/usr/bin/env bash"
    if shebang:
        out.append(shebang)
    keys = HEADER_KEYS if kind == "shortcut" else ("name", "description")
    for key in keys:
        value = " ".join(str(params.get(key) or "").split())
        if value:
            out.append("# " + key + ": " + value)
    block_types = (kind + ".block", "shortcut.run")
    blocks = [str((n.get("params") or {}).get("text") or "").strip("\n")
              for n in chain[1:] if n["type"] in block_types]
    blocks = [b for b in blocks if b.strip()]
    script = kind == "shortcut" and is_script(path)
    if blocks:
        if (meta or {}).get("blankAfterHeader", not script):
            out.append("")
        out.append("\n\n".join(blocks))
    end = "" if (meta or {}).get("finalNewline") is False else "\n"
    return "\n".join(out) + end


def _qt_escape(text):
    out = []
    for char in text:
        code = ord(char)
        if char == '"':
            out.append('\\"')
        elif char == "\\":
            out.append("\\\\")
        elif char == "\b":
            out.append("\\b")
        elif char == "\f":
            out.append("\\f")
        elif char == "\n":
            out.append("\\n")
        elif char == "\r":
            out.append("\\r")
        elif char == "\t":
            out.append("\\t")
        elif code < 0x20:
            out.append("\\u%04x" % code)
        else:
            out.append(char)
    return '"' + "".join(out) + '"'


def qt_json(value, indent=0):
    pad = "    " * (indent + 1)
    close = "    " * indent
    if isinstance(value, dict):
        if not value:
            return "{\n" + close + "}"
        return "{\n" + ",\n".join(
            pad + _qt_escape(str(k)) + ": " + qt_json(value[k], indent + 1)
            for k in sorted(value)) + "\n" + close + "}"
    if isinstance(value, list):
        if not value:
            return "[\n" + close + "]"
        return "[\n" + ",\n".join(pad + qt_json(v, indent + 1)
                                  for v in value) + "\n" + close + "]"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if value is None:
        return "null"
    if isinstance(value, str):
        return _qt_escape(value)
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return json.dumps(value)


def parse_agent(path, text):
    if len(text.encode("utf-8")) > MAX_FILE_BYTES:
        raise FileError("file_too_large")
    try:
        data = json.loads(text)
    except ValueError:
        raise FileError("invalid_json_file")
    if not isinstance(data, dict):
        raise FileError("invalid_json_file")
    match = AGENT_PATH_RE.match(path)
    file_id = match.group(1) if match else ""
    profile = {
        "name": str(data.get("name") or ""),
        "id": str(data.get("id") or file_id),
        "specialty": str(data.get("specialty") or ""),
        "focus": str(data.get("focus") or ""),
        "avatar": str(data.get("avatar") or "sol"),
        "description": str(data.get("description") or ""),
        "instructions": str(data.get("instructions") or ""),
        "skills": "\n".join(str(s) for s in data.get("skills") or []),
        "permissions": [str(p) for p in data.get("permissions") or []],
        "enabled": "true" if data.get("enabled", True) else "false",
        "selfImproving": "true" if data.get("selfImproving") else "false",
        "lowCostModel": "true" if data.get("lowCostModel") else "false",
        "runWindowHours": str(int(data.get("runWindowHours") or 0)),
        "marketingChannels": "\n".join(
            str(c) for c in data.get("marketingChannels") or []),
    }
    nodes = [_node("profile", "agent.profile",
                   profile["name"] or file_id or "Special agent", 1, 0,
                   profile)]
    connections = []
    schedule = data.get("schedule") if isinstance(
        data.get("schedule"), dict) else {}
    nodes.append(_node("schedule", "agent.schedule", "Schedule", 0, -1, {
        "mode": str(schedule.get("mode") or "on-demand"),
        "time": str(schedule.get("time") or "09:00"),
        "days": [str(d) for d in schedule.get("days") or []],
        "active": "true" if schedule.get("active") else "false"}))
    connections.append({"from": "schedule", "output": "main", "to": "profile"})
    events = [str(e) for e in schedule.get("events") or []]
    if events:
        nodes.append(_node("events", "agent.events", "Events", 0, 0,
                           {"events": events}))
        connections.append({"from": "events", "output": "main",
                            "to": "profile"})
    for number, monitor in enumerate(data.get("logMonitors") or [], 1):
        if not isinstance(monitor, dict):
            continue
        node_id = "monitor%d" % number
        nodes.append(_node(node_id, "agent.monitor",
                           "Log monitor %d" % number, 0, number, {
                               "badge": str(monitor.get("badge") or ""),
                               "threshold": str(monitor.get("threshold") or 0),
                               "taskType": str(monitor.get("taskType") or
                                               "error-logs"),
                               "active": "true" if monitor.get(
                                   "active", True) else "false",
                               "inclusive": "true" if monitor.get(
                                   "inclusive") else "false"}))
        connections.append({"from": node_id, "output": "main",
                            "to": "profile"})
    row = -1
    if data.get("shortcut"):
        nodes.append(_node("shortcut", "agent.shortcut", "Playbook", 2, row,
                           {"file": str(data["shortcut"])}))
        connections.append({"from": "profile", "output": "main",
                            "to": "shortcut"})
        row += 1
    for number, ref in enumerate(data.get("skillRefs") or [], 1):
        scope, _, file_name = str(ref).partition(":")
        node_id = "skill%d" % number
        nodes.append(_node(node_id, "agent.skill", "Skill file %d" % number,
                           2, row, {"scope": scope or "repository",
                                    "file": file_name}))
        connections.append({"from": "profile", "output": "main",
                            "to": node_id})
        row += 1
    if data.get("workflow"):
        nodes.append(_node("workflow", "agent.workflow", "Native workflow",
                           2, row, {"workflow": str(data["workflow"])}))
        connections.append({"from": "profile", "output": "main",
                            "to": "workflow"})
        row += 1
    if data.get("relayChannel"):
        nodes.append(_node("relay", "agent.relay", "Relay channel", 2, row,
                           {"channel": str(data["relayChannel"])}))
        connections.append({"from": "profile", "output": "main",
                            "to": "relay"})
    meta = {"original": data}
    warnings = []
    if file_id and data.get("id") and data.get("id") != file_id:
        warnings.append("The file name and the agent id differ; the desktop "
                        "reports this file as invalid")
    return nodes, connections, meta, warnings


def _limit(value, limit, code):
    text = str(value or "")
    if len(text) > limit:
        raise FileError(code)
    return text


def render_agent(path, nodes, connections, meta, now_ms):
    by_id = {n["id"]: n for n in nodes}
    profile = next((n for n in nodes if n["type"] == "agent.profile"), None)
    if profile is None:
        raise FileError("missing_profile")
    attached = set()
    for conn in connections:
        if conn["to"] == profile["id"]:
            attached.add(conn["from"])
        if conn["from"] == profile["id"]:
            attached.add(conn["to"])
    warnings = [n["name"] + " is not connected to the agent and is left out"
                for n in nodes if n is not profile and n["id"] not in attached]
    linked = [by_id[i] for i in attached if i in by_id]
    linked.sort(key=lambda n: (n.get("y", 0), n.get("x", 0)))
    p = profile.get("params") or {}
    data = dict((meta or {}).get("original") or {})
    agent_id = str(p.get("id") or "").strip()
    if not AGENT_ID_RE.match(agent_id):
        raise FileError("invalid_agent_id")
    expected = ".forkmesh/teammates/" + agent_id + ".json"
    if path and path != expected:
        raise FileError("agent_id_mismatch")
    name = _limit(str(p.get("name") or "").strip(), 80, "name_too_long")
    specialty = _limit(str(p.get("specialty") or "").strip(), 100,
                       "specialty_too_long")
    if not name or not specialty:
        raise FileError("agent_needs_name_and_specialty")
    permissions = [x for x in _checks(p.get("permissions"))]
    if any(x not in PERMISSIONS for x in permissions):
        raise FileError("unknown_permission")
    hours = _int(p.get("runWindowHours"), 0)
    if not 0 <= hours <= 720:
        raise FileError("invalid_run_window")
    data.update({
        "version": 2,
        "id": agent_id,
        "name": name,
        "specialty": specialty,
        "focus": _limit(str(p.get("focus") or "").strip(), 120,
                        "focus_too_long"),
        "avatar": _limit(str(p.get("avatar") or "sol").strip() or "sol", 80,
                         "avatar_too_long"),
        "description": _limit(str(p.get("description") or ""), 1000,
                              "description_too_long"),
        "instructions": _limit(str(p.get("instructions") or ""), 65536,
                               "instructions_too_long"),
        "skills": _lines(p.get("skills")),
        "permissions": [x for x in PERMISSIONS if x in permissions],
        "enabled": _bool(p.get("enabled"), True),
        "selfImproving": _bool(p.get("selfImproving")),
        "runWindowHours": hours,
    })
    if _bool(p.get("lowCostModel")):
        data["lowCostModel"] = True
    else:
        data.pop("lowCostModel", None)
    channels = _lines(p.get("marketingChannels"))
    if channels:
        data["marketingChannels"] = channels
    else:
        data.pop("marketingChannels", None)
    schedule = {"active": False, "days": [], "events": [],
                "mode": "on-demand", "time": "09:00"}
    original_schedule = data.get("schedule")
    if isinstance(original_schedule, dict):
        schedule["time"] = str(original_schedule.get("time") or "09:00")
    monitors = []
    skill_refs = []
    data["shortcut"] = ""
    data["workflow"] = ""
    data["relayChannel"] = ""
    for node in linked:
        q = node.get("params") or {}
        kind = node["type"]
        if kind == "agent.schedule":
            mode = str(q.get("mode") or "on-demand")
            if mode not in SCHEDULE_MODES:
                raise FileError("invalid_schedule_mode")
            time_value = str(q.get("time") or "09:00").strip()
            if not TIME_RE.match(time_value):
                raise FileError("invalid_schedule_time")
            days = sorted({_int(d, 0) for d in _checks(q.get("days"))})
            if any(not 1 <= d <= 7 for d in days):
                raise FileError("invalid_schedule_days")
            schedule.update({"mode": mode, "time": time_value, "days": days,
                             "active": _bool(q.get("active"))})
        elif kind == "agent.events":
            events = _checks(q.get("events"))
            if any(e not in TASK_TYPES for e in events):
                raise FileError("invalid_event")
            schedule["events"] = [e for e in TASK_TYPES if e in events]
        elif kind == "agent.monitor":
            task_type = str(q.get("taskType") or "error-logs")
            if task_type not in TASK_TYPES:
                raise FileError("invalid_event")
            badge = str(q.get("badge") or "").strip()
            if not badge:
                raise FileError("monitor_needs_badge")
            monitors.append({"active": _bool(q.get("active"), True),
                             "badge": badge[:40],
                             "inclusive": _bool(q.get("inclusive")),
                             "taskType": task_type,
                             "threshold": max(0, _int(q.get("threshold"), 0))})
        elif kind == "agent.shortcut":
            data["shortcut"] = str(q.get("file") or "").strip()
        elif kind == "agent.skill":
            scope = str(q.get("scope") or "repository")
            file_name = str(q.get("file") or "").strip()
            if scope not in SKILL_SCOPES or not file_name or "/" in file_name:
                raise FileError("invalid_skill_ref")
            skill_refs.append(scope + ":" + file_name)
        elif kind == "agent.workflow":
            workflow = str(q.get("workflow") or "")
            if workflow not in NATIVE_WORKFLOWS:
                raise FileError("invalid_native_workflow")
            data["workflow"] = workflow
        elif kind == "agent.relay":
            channel = str(q.get("channel") or "").strip()
            data["relayChannel"] = channel if channel.startswith("#") or \
                not channel else "#" + channel
    if len(monitors) > 16:
        raise FileError("too_many_monitors")
    data["schedule"] = schedule
    data["logMonitors"] = monitors
    data["skillRefs"] = skill_refs
    if not data.get("createdAtMs"):
        data["createdAtMs"] = int(now_ms)
    data["updatedAtMs"] = int(now_ms)
    for key, default in (("activity", []), ("improvementCycles", 0),
                         ("improvementNotes", []), ("lastRunAtMs", 0),
                         ("lastSessionId", 0), ("lastTrigger", "")):
        data.setdefault(key, default)
    return qt_json(data) + "\n", warnings


def _chain(nodes, connections, root_type):
    root = next((n for n in nodes if n["type"] == root_type), None)
    if root is None:
        raise FileError("missing_root")
    by_id = {n["id"]: n for n in nodes}
    outgoing = {}
    for conn in connections:
        outgoing.setdefault(conn["from"], []).append(conn["to"])
    order = []
    seen = set()
    stack = [root["id"]]
    while stack:
        current = stack.pop()
        if current in seen or current not in by_id:
            continue
        seen.add(current)
        order.append(by_id[current])
        targets = sorted((t for t in outgoing.get(current, [])
                          if t in by_id and t not in seen),
                         key=lambda t: (by_id[t].get("x", 0),
                                        by_id[t].get("y", 0)))
        stack.extend(reversed(targets))
    warnings = [n["name"] + " is not connected and is left out"
                for n in nodes if n["id"] not in seen]
    return order, warnings


def parse_file(path, text):
    kind = kind_for_path(path)
    if not kind:
        raise FileError("unsupported_path")
    text = str(text or "")
    if kind == "action":
        nodes, connections, meta, warnings = parse_action(path, text)
    elif kind == "agent":
        nodes, connections, meta, warnings = parse_agent(path, text)
    else:
        nodes, connections, meta, warnings = parse_header_file(
            kind, path, text)
    root = nodes[0]
    name = str((root.get("params") or {}).get("name") or "") or \
        path.rsplit("/", 1)[-1]
    return {"kind": kind, "path": path, "name": name, "nodes": nodes,
            "connections": connections, "meta": meta}, warnings


def clean_document(document):
    if not isinstance(document, dict):
        raise FileError("invalid_document")
    path = str(document.get("path") or "")
    kind = kind_for_path(path)
    if not kind or kind != document.get("kind"):
        raise FileError("unsupported_path")
    nodes = []
    ids = set()
    for raw in document.get("nodes") or []:
        if not isinstance(raw, dict):
            raise FileError("invalid_node")
        node_type = str(raw.get("type") or "")
        spec = FILE_NODES.get(node_type)
        node_id = str(raw.get("id") or "")
        if not spec or spec["kind"] != kind or not re.match(
                r"^[A-Za-z0-9_-]{1,32}$", node_id) or node_id in ids:
            raise FileError("invalid_node")
        ids.add(node_id)
        params = raw.get("params") if isinstance(raw.get("params"),
                                                 dict) else {}
        keys = {f["key"] for f in spec["fields"]}
        nodes.append({"id": node_id, "type": node_type,
                      "name": str(raw.get("name") or spec["name"])[:80],
                      "x": _int(raw.get("x")), "y": _int(raw.get("y")),
                      "params": {k: v for k, v in params.items()
                                 if k in keys}})
    if len(nodes) > 200:
        raise FileError("too_many_nodes")
    for node_type, spec in FILE_NODES.items():
        if spec.get("single") and sum(
                1 for n in nodes if n["type"] == node_type) > 1:
            raise FileError("only_one_" + node_type.split(".")[1])
    connections = []
    for raw in document.get("connections") or []:
        if not isinstance(raw, dict):
            continue
        source, target = str(raw.get("from") or ""), str(raw.get("to") or "")
        if source in ids and target in ids and source != target:
            connections.append({"from": source, "output": "main",
                                "to": target})
    meta = document.get("meta") if isinstance(document.get("meta"),
                                              dict) else {}
    return kind, path, nodes, connections, meta


def render_file(document, now_ms=0):
    kind, path, nodes, connections, meta = clean_document(document)
    if kind == "agent":
        text, warnings = render_agent(path, nodes, connections, meta, now_ms)
    else:
        chain, warnings = _chain(nodes, connections, KINDS[kind]["root"])
        if kind == "action":
            text = render_action(nodes, chain, meta)
        else:
            text = render_header_file(kind, path, chain, meta)
    if len(text.encode("utf-8")) > MAX_FILE_BYTES:
        raise FileError("file_too_large")
    return text, warnings


def new_document(kind, path):
    if kind_for_path(path) != kind:
        raise FileError("unsupported_path")
    if kind == "action":
        text = ("name: New action\non: [workflow_dispatch]\njobs:\n"
                "  main:\n    steps:\n      - name: Say hello\n"
                "        run: |\n          echo \"Hello from ForkMesh\"\n")
    elif kind == "shortcut":
        text = ("#!/usr/bin/env bash\n# name: New shortcut\n"
                "# description: What this shortcut does\n"
                "set -euo pipefail\necho \"Hello\"\n") if is_script(path) \
            else ("# name: New shortcut\n# description: What this prompt "
                  "asks for\n\nDescribe the task for the agent here.\n")
    elif kind == "skill":
        text = ("# name: New skill\n# description: What the skill teaches\n"
                "\nWhen this skill is installed:\n\n- Follow these steps.\n")
    else:
        agent_id = AGENT_PATH_RE.match(path).group(1)
        text = qt_json({"id": agent_id, "name": agent_id.replace("-", " ")
                        .title(), "specialty": "Special agent",
                        "version": 2}) + "\n"
    return parse_file(path, text)
