import asyncio
import base64
import ipaddress
import json
import random as random_module
import re
from datetime import datetime, timezone
from urllib.parse import quote, urlencode, urlparse

import workflow_actions as actions
import workflow_integrations as integrations
import workflow_script as script

MAX_NODES = 60
PACKAGE = "workflows"
PACK_PREFIX = "workflows-pack-"
EXAMPLES_PACKAGE = "workflows-examples"
FILES_PACKAGE = "workflows-repository-files"
MAX_CONNECTIONS = 120
MAX_NAME = 80
MAX_NODE_NAME = 60
MAX_VALUE_CHARS = 20000
MAX_ITEMS = 100
MAX_RESPONSE_CHARS = 262144
MAX_RUN_ITEMS = 5
MAX_RUN_ITEM_CHARS = 4000
MAX_CREDENTIAL_VALUE = 4000
POSITION_LIMIT = 20000
SCHEDULE_MIN_MINUTES = 5
SCHEDULE_MAX_MINUTES = 10080
NODE_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
CREDENTIAL_ID_RE = re.compile(r"^[0-9a-f]{32}$")
EXPRESSION_RE = re.compile(r"\{\{\s*(.*?)\s*\}\}", re.S)
PLACEHOLDER_RE = re.compile(r"\{([A-Za-z][A-Za-z0-9_]*)\}")
ROOT_RE = re.compile(r"^\$([A-Za-z_]+)")
STEP_RE = re.compile(
    r"\.([A-Za-z_$][\w$-]*)|\[\s*(-?\d+)\s*\]|\[\s*\"([^\"]*)\"\s*\]"
    r"|\[\s*'([^']*)'\s*\]")
BLOCKED_HOST_SUFFIXES = ("forkmesh.com", "localhost", ".local", ".internal")
BODY_METHODS = ("POST", "PUT", "PATCH", "DELETE")
OPERATORS = (
    ("equals", "is equal to"),
    ("notEquals", "is not equal to"),
    ("contains", "contains"),
    ("notContains", "does not contain"),
    ("startsWith", "starts with"),
    ("endsWith", "ends with"),
    ("gt", "is greater than"),
    ("gte", "is greater than or equal to"),
    ("lt", "is less than"),
    ("lte", "is less than or equal to"),
    ("isEmpty", "is empty"),
    ("isNotEmpty", "is not empty"),
    ("isTrue", "is true"),
    ("isFalse", "is false"),
    ("matches", "matches the regular expression"),
    ("notMatches", "does not match the regular expression"),
)
CONDITION_FIELDS = (
    {"key": "left", "label": "Value", "type": "string",
     "placeholder": "{{ $json.status }}"},
    {"key": "operator", "label": "Operator", "type": "select",
     "required": True, "default": "equals",
     "options": [{"value": k, "label": v} for k, v in OPERATORS]},
    {"key": "right", "label": "Compare with", "type": "string",
     "placeholder": "active"},
)

CORE_NODES = {
    "core.manual": {
        "name": "Manual trigger", "group": "Triggers", "icon": "mouse-pointer-click",
        "color": "#64748b", "trigger": True, "outputs": ["main"],
        "description": "Start the workflow with the Run button.",
        "fields": [{"key": "payload", "label": "Test data (JSON)",
                    "type": "json", "placeholder": "{\"name\": \"Ada\"}"}],
    },
    "core.webhook": {
        "name": "Webhook", "group": "Triggers", "icon": "webhook",
        "color": "#7c3aed", "trigger": True, "outputs": ["main"],
        "description": "Start the workflow when its secret URL is called.",
        "fields": [],
    },
    "core.schedule": {
        "name": "Schedule", "group": "Triggers", "icon": "clock",
        "color": "#0891b2", "trigger": True, "outputs": ["main"],
        "description": "Start the workflow every few minutes, hours or days.",
        "fields": [{"key": "every", "label": "Every (minutes)",
                    "type": "number", "required": True, "default": "60",
                    "placeholder": "60"}],
    },
    "core.http": {
        "name": "HTTP request", "group": "Core", "icon": "globe",
        "color": "#2563eb", "outputs": ["main"],
        "description": "Call any HTTPS API.",
        "fields": [
            {"key": "method", "label": "Method", "type": "select",
             "required": True, "default": "GET",
             "options": [{"value": m, "label": m} for m in
                         ("GET", "POST", "PUT", "PATCH", "DELETE")]},
            {"key": "url", "label": "URL", "type": "string", "required": True,
             "placeholder": "https://api.example.com/items"},
            {"key": "headers", "label": "Headers (JSON)", "type": "json",
             "placeholder": "{\"Authorization\": \"Bearer ...\"}"},
            {"key": "query", "label": "Query parameters (JSON)",
             "type": "json"},
            {"key": "bodyType", "label": "Body", "type": "select",
             "default": "json",
             "options": [{"value": "json", "label": "JSON"},
                         {"value": "form", "label": "Form"},
                         {"value": "text", "label": "Text"}]},
            {"key": "body", "label": "Body content", "type": "text",
             "placeholder": "{\"text\": \"{{ $json.message }}\"}"},
        ],
    },
    "core.set": {
        "name": "Set fields", "group": "Core", "icon": "pencil",
        "color": "#16a34a", "outputs": ["main"],
        "description": "Add or change fields on every item.",
        "fields": [
            {"key": "values", "label": "Fields (JSON)", "type": "json",
             "required": True,
             "placeholder": "{\"fullName\": \"{{ $json.first }} {{ $json.last }}\"}"},
            {"key": "keep", "label": "Keep the other fields",
             "type": "boolean", "default": "true"},
        ],
    },
    "core.if": {
        "name": "If", "group": "Flow", "icon": "split", "color": "#ea580c",
        "outputs": ["true", "false"],
        "description": "Send each item down the true or false branch.",
        "fields": list(CONDITION_FIELDS),
    },
    "core.filter": {
        "name": "Filter", "group": "Flow", "icon": "filter",
        "color": "#ea580c", "outputs": ["main"],
        "description": "Keep only the items that match a condition.",
        "fields": list(CONDITION_FIELDS),
    },
    "core.merge": {
        "name": "Merge", "group": "Flow", "icon": "merge", "color": "#ea580c",
        "outputs": ["main"],
        "description": "Combine the items of every incoming branch.",
        "fields": [],
    },
    "core.limit": {
        "name": "Limit", "group": "Flow", "icon": "list-end",
        "color": "#ea580c", "outputs": ["main"],
        "description": "Keep the first items only.",
        "fields": [{"key": "max", "label": "Items to keep", "type": "number",
                    "required": True, "default": "10"}],
    },
    "core.split": {
        "name": "Split out", "group": "Flow", "icon": "list-tree",
        "color": "#ea580c", "outputs": ["main"],
        "description": "Turn a list inside an item into separate items.",
        "fields": [{"key": "field", "label": "List field", "type": "string",
                    "required": True, "placeholder": "data.items"}],
    },
    "core.noop": {
        "name": "No operation", "group": "Flow", "icon": "circle-dot",
        "color": "#64748b", "outputs": ["main"],
        "description": "Pass items through unchanged.",
        "fields": [],
    },
}
CORE_NODES.update(actions.NODES)
NodeError = actions.NodeError
StopRun = actions.StopRun


def _app_key(node_type):
    if node_type.startswith("app."):
        return node_type[4:]
    return ""


def node_spec(node_type):
    if node_type in CORE_NODES:
        return CORE_NODES[node_type]
    app = integrations.INTEGRATIONS.get(_app_key(node_type))
    if not app:
        return None
    return {"outputs": ["main"], "app": app}


def is_trigger(node_type):
    return bool(CORE_NODES.get(node_type, {}).get("trigger"))


def outputs_for(node_type):
    spec = node_spec(node_type)
    return list(spec["outputs"]) if spec else []


def _fields_for(node_type, params):
    if node_type in CORE_NODES:
        return CORE_NODES[node_type]["fields"]
    app = integrations.INTEGRATIONS.get(_app_key(node_type))
    if not app:
        return []
    op = app["operations"].get(str((params or {}).get("operation") or ""))
    return op["fields"] if op else []


def catalog():
    core = [dict(spec, type=key) for key, spec in CORE_NODES.items()]
    apps = []
    for key, app in integrations.INTEGRATIONS.items():
        apps.append({
            "key": key,
            "type": "app." + key,
            "name": app["name"],
            "category": app["category"],
            "color": app["color"],
            "description": app["description"],
            "host": urlparse(app["base"]).hostname
            if app["base"].startswith("https://") else "",
            "credential": [
                {"key": f["key"], "label": f["label"],
                 "secret": bool(f.get("secret")),
                 "placeholder": f.get("placeholder", "")}
                for f in app["auth"]["fields"]],
            "operations": [
                {"key": op_key, "name": op["name"], "method": op["method"],
                 "fields": op["fields"]}
                for op_key, op in app["operations"].items()],
        })
    categories = sorted({app["category"] for app in apps})
    return {"core": core, "integrations": apps, "categories": categories,
            "packs": packs(),
            "operators": [{"value": k, "label": v} for k, v in OPERATORS]}


def pack_id(category):
    return PACK_PREFIX + re.sub(r"[^a-z0-9]+", "-", category.lower()).strip("-")


def packs():
    found = {}
    for key, app in integrations.INTEGRATIONS.items():
        found.setdefault(app["category"], []).append(key)
    return [{"id": pack_id(category), "category": category,
             "integrations": keys} for category, keys in found.items()]


def _clean_text(value, limit):
    text = str(value if value is not None else "").replace("\x00", "")
    return text.strip()[:limit]


def _clean_value(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, (dict, list)):
        encoded = json.dumps(value)
        if len(encoded) > MAX_VALUE_CHARS:
            return None
        return value
    if value is None:
        return ""
    text = str(value).replace("\x00", "")
    if len(text) > MAX_VALUE_CHARS:
        return None
    return text


def _clean_params(node_type, raw):
    params = raw if isinstance(raw, dict) else {}
    clean = {}
    if node_type.startswith("app."):
        app = integrations.INTEGRATIONS[_app_key(node_type)]
        operation = str(params.get("operation") or "")
        if operation not in app["operations"]:
            operation = next(iter(app["operations"]))
        clean["operation"] = operation
    for field in _fields_for(node_type, clean or params):
        if field["key"] not in params:
            continue
        value = _clean_value(params[field["key"]])
        if value is None:
            return None
        clean[field["key"]] = value
    return clean


def _int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def clean_workflow(data):
    if not isinstance(data, dict):
        return None, "invalid_workflow"
    name = _clean_text(data.get("name"), MAX_NAME) or "Untitled workflow"
    raw_nodes = data.get("nodes") or []
    raw_connections = data.get("connections") or []
    if not isinstance(raw_nodes, list) or not isinstance(raw_connections, list):
        return None, "invalid_workflow"
    if len(raw_nodes) > MAX_NODES:
        return None, "too_many_nodes"
    if len(raw_connections) > MAX_CONNECTIONS:
        return None, "too_many_connections"
    nodes = []
    ids = set()
    names = set()
    triggers = 0
    for raw in raw_nodes:
        if not isinstance(raw, dict):
            return None, "invalid_node"
        node_id = str(raw.get("id") or "")
        node_type = str(raw.get("type") or "")
        if not NODE_ID_RE.match(node_id) or node_id in ids:
            return None, "invalid_node"
        if node_spec(node_type) is None:
            return None, "unknown_node_type"
        node_name = _clean_text(raw.get("name"), MAX_NODE_NAME)
        if not node_name or node_name in names:
            return None, "duplicate_node_name"
        params = _clean_params(node_type, raw.get("params"))
        if params is None:
            return None, "value_too_long"
        credential = str(raw.get("credential") or "")
        if credential and not CREDENTIAL_ID_RE.match(credential):
            return None, "invalid_credential"
        if is_trigger(node_type):
            triggers += 1
        ids.add(node_id)
        names.add(node_name)
        nodes.append({
            "id": node_id,
            "type": node_type,
            "name": node_name,
            "x": max(-POSITION_LIMIT, min(POSITION_LIMIT, _int(raw.get("x")))),
            "y": max(-POSITION_LIMIT, min(POSITION_LIMIT, _int(raw.get("y")))),
            "params": params,
            "credential": credential if node_type.startswith("app.") else "",
            "continueOnFail": raw.get("continueOnFail") is True,
            "disabled": raw.get("disabled") is True,
        })
    if triggers > 1:
        return None, "too_many_triggers"
    by_id = {node["id"]: node for node in nodes}
    connections = []
    seen = set()
    for raw in raw_connections:
        if not isinstance(raw, dict):
            return None, "invalid_connection"
        source = str(raw.get("from") or "")
        target = str(raw.get("to") or "")
        output = str(raw.get("output") or "main")
        if source not in by_id or target not in by_id or source == target:
            return None, "invalid_connection"
        if output not in outputs_for(by_id[source]["type"]):
            return None, "invalid_connection"
        if is_trigger(by_id[target]["type"]):
            return None, "invalid_connection"
        key = (source, output, target)
        if key in seen:
            continue
        seen.add(key)
        connections.append({"from": source, "output": output, "to": target})
    if topological_order(nodes, connections) is None:
        return None, "cycle"
    return {"name": name, "nodes": nodes, "connections": connections}, ""


def topological_order(nodes, connections):
    incoming = {node["id"]: 0 for node in nodes}
    edges = {node["id"]: [] for node in nodes}
    for conn in connections:
        edges[conn["from"]].append(conn["to"])
        incoming[conn["to"]] += 1
    ready = [node["id"] for node in nodes if incoming[node["id"]] == 0]
    order = []
    while ready:
        current = ready.pop(0)
        order.append(current)
        for target in edges[current]:
            incoming[target] -= 1
            if incoming[target] == 0:
                ready.append(target)
    if len(order) != len(nodes):
        return None
    return order


def trigger_node(workflow):
    for node in workflow.get("nodes") or []:
        if is_trigger(node["type"]):
            return node
    return None


def schedule_minutes(workflow):
    node = trigger_node(workflow)
    if not node or node["type"] != "core.schedule" or node.get("disabled"):
        return 0
    minutes = _int(node["params"].get("every"), 0)
    if minutes <= 0:
        return 0
    return max(SCHEDULE_MIN_MINUTES, min(SCHEDULE_MAX_MINUTES, minutes))


def credential_values(integration_key, raw):
    app = integrations.INTEGRATIONS.get(integration_key)
    if not app or not isinstance(raw, dict):
        return None, "unknown_integration"
    values = {}
    for field in app["auth"]["fields"]:
        value = str(raw.get(field["key"]) or "").strip()
        if len(value) > MAX_CREDENTIAL_VALUE or "\x00" in value:
            return None, "value_too_long"
        if not value and not field.get("optional"):
            return None, "missing_credential_field"
        if value and field.get("url") and not safe_url(value):
            return None, "invalid_credential_url"
        values[field["key"]] = value
    return values, ""


def credential_hint(integration_key, values):
    app = integrations.INTEGRATIONS.get(integration_key) or {"auth": {"fields": []}}
    hints = {}
    for field in app["auth"]["fields"]:
        value = str((values or {}).get(field["key"]) or "")
        if not value:
            continue
        hints[field["key"]] = ("••••" + value[-4:]) if field.get(
            "secret") and len(value) > 8 else ("••••" if field.get(
                "secret") else value[:80])
    return hints


def safe_url(value):
    raw = str(value or "").strip()
    if not raw or len(raw) > 4096 or any(ch in raw for ch in "\r\n\t \\"):
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if parsed.scheme.lower() != "https" or not host:
        return ""
    if parsed.username is not None or parsed.password is not None:
        return ""
    for suffix in BLOCKED_HOST_SUFFIXES:
        if suffix.startswith("."):
            if host.endswith(suffix):
                return ""
        elif host == suffix or host.endswith("." + suffix):
            return ""
    try:
        if not ipaddress.ip_address(host.strip("[]")).is_global:
            return ""
    except ValueError:
        if "." not in host:
            return ""
    if port is not None and not 1 <= int(port) <= 65535:
        return ""
    return raw


def _step(value, key):
    if isinstance(value, dict):
        return value.get(key)
    if isinstance(value, list) and isinstance(key, int):
        if -len(value) <= key < len(value):
            return value[key]
    return None


def evaluate_expression(expr, context):
    text = str(expr or "").strip()
    match = ROOT_RE.match(text)
    if not match:
        raise NodeError("Expressions start with $json, $node, $trigger, "
                        "$now, $today, $item, $vars or $workflow: " + text[:80])
    root = match.group(1)
    if root not in context:
        raise NodeError("Unknown expression variable $" + root)
    value = context[root]
    if callable(value):
        value = value()
    position = match.end()
    while position < len(text):
        step = STEP_RE.match(text, position)
        if not step:
            raise NodeError("Cannot read expression: " + text[:80])
        name, index, dq, sq = step.groups()
        if name is not None:
            value = _step(value, name)
        elif index is not None:
            value = _step(value, int(index))
        else:
            value = _step(value, dq if dq is not None else sq)
        position = step.end()
    return value


def _stringify(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list)):
        return json.dumps(value)
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def resolve(value, context):
    if isinstance(value, dict):
        return {key: resolve(item, context) for key, item in value.items()}
    if isinstance(value, list):
        return [resolve(item, context) for item in value]
    if not isinstance(value, str) or "{{" not in value:
        return value
    whole = EXPRESSION_RE.fullmatch(value.strip())
    if whole:
        return evaluate_expression(whole.group(1), context)
    return EXPRESSION_RE.sub(
        lambda m: _stringify(evaluate_expression(m.group(1), context)), value)


def coerce(field, value):
    kind = field.get("type", "string")
    empty = value is None or (isinstance(value, str) and not value.strip())
    if empty:
        if field.get("required"):
            raise NodeError(field["label"] + " is required")
        return None
    if kind == "number":
        if isinstance(value, bool):
            raise NodeError(field["label"] + " must be a number")
        try:
            number = float(value)
        except (TypeError, ValueError):
            raise NodeError(field["label"] + " must be a number")
        return int(number) if number.is_integer() else number
    if kind == "boolean":
        if isinstance(value, bool):
            return value
        return str(value).strip().lower() in ("true", "1", "yes", "on")
    if kind == "json":
        if isinstance(value, (dict, list)):
            return value
        try:
            return json.loads(str(value))
        except ValueError:
            raise NodeError(field["label"] + " is not valid JSON")
    if kind == "value":
        return value
    if kind == "select":
        allowed = [option["value"] for option in field.get("options") or []]
        text = _stringify(value)
        if allowed and text not in allowed:
            raise NodeError(field["label"] + " must be one of "
                            + ", ".join(allowed))
        return text
    return _stringify(value)


def field_values(fields, params, context):
    values = {}
    for field in fields:
        raw = params.get(field["key"], field.get("default"))
        if field.get("type") == "code":
            values[field["key"]] = coerce(field, raw)
            continue
        if field.get("type") == "json" and isinstance(raw, str) and raw.strip():
            try:
                raw = json.loads(raw)
            except ValueError:
                pass
        values[field["key"]] = coerce(field, resolve(raw, context))
    return values


def _fill(template, values, encode, slashes=()):
    def replace(match):
        key = match.group(1)
        if key not in values:
            return match.group(0)
        text = _stringify(values[key])
        if not encode:
            return text
        return quote(text, safe="/" if key in slashes else "")
    return PLACEHOLDER_RE.sub(replace, template)


def _fill_present(templates, cred):
    out = {}
    for name, template in (templates or {}).items():
        keys = PLACEHOLDER_RE.findall(template)
        if any(not str(cred.get(key) or "") for key in keys):
            continue
        out[name] = _fill(template, cred, False)
    return out


def _render(template, values):
    if isinstance(template, dict):
        out = {}
        for key, item in template.items():
            rendered = _render(item, values)
            if rendered is not None:
                out[key] = rendered
        return out
    if isinstance(template, list):
        return [item for item in (_render(i, values) for i in template)
                if item is not None]
    if isinstance(template, str):
        whole = PLACEHOLDER_RE.fullmatch(template)
        if whole and whole.group(1) in values:
            return values[whole.group(1)]
        return _fill(template, values, False)
    return template


def _set_path(target, dotted, value):
    parts = dotted.split(".")
    for part in parts[:-1]:
        nxt = target.get(part)
        if not isinstance(nxt, dict):
            nxt = {}
            target[part] = nxt
        target = nxt
    target[parts[-1]] = value


def _base_url(app, cred):
    base = app["base"]
    for field in app["auth"]["fields"]:
        token = "{" + field["key"] + "}"
        if token not in base:
            continue
        value = str(cred.get(field["key"]) or "")
        if field.get("url"):
            base = base.replace(token, value.rstrip("/"))
        else:
            base = base.replace(token, quote(value, safe=":"))
    return base


def build_app_request(app_key, params, values, cred):
    app = integrations.INTEGRATIONS[app_key]
    op = app["operations"][params["operation"]]
    path_keys = set(PLACEHOLDER_RE.findall(op["path"]))
    for key in path_keys:
        if values.get(key) in (None, ""):
            raise NodeError("Missing " + key)
    slashes = {f["key"] for f in op["fields"] if f.get("slashes")}
    url = _base_url(app, cred) + _fill(op["path"], values, True, slashes)
    headers = dict(app.get("headers") or {})
    query = dict(op.get("query") or {})
    auth = app["auth"]
    headers.update(_fill_present(auth.get("headers"), cred))
    query.update(_fill_present(auth.get("query"), cred))
    if auth.get("basic"):
        user_t, pass_t = auth["basic"]
        pair = _fill(user_t, cred, False) + ":" + _fill(pass_t, cred, False)
        headers["Authorization"] = "Basic " + base64.b64encode(
            pair.encode("utf-8")).decode("ascii")
    for name, template in (op.get("headers") or {}).items():
        headers[name] = _fill(template, values, False)
    body_fields = {}
    for field in op["fields"]:
        key = field["key"]
        value = values.get(key)
        if key in path_keys or value is None or field.get("in") == "template":
            continue
        where = field.get("in") or (
            "body" if op["method"] in BODY_METHODS else "query")
        if where == "query":
            query[key.split(".")[-1]] = _stringify(value)
        elif where == "query_object":
            if not isinstance(value, dict):
                raise NodeError(field["label"] + " must be a JSON object")
            for name, item in value.items():
                query[str(name)] = _stringify(item)
        elif where == "header":
            headers[field.get("header") or key] = _stringify(value)
        else:
            body_fields[key] = value
    body = None
    if op.get("body") is not None:
        body = _render(op["body"], values)
        if isinstance(body, dict):
            for key, value in body_fields.items():
                if key not in _template_keys(op["body"]):
                    _set_path(body, key, value)
    elif body_fields:
        body = {}
        for key, value in body_fields.items():
            _set_path(body, key, value)
    if auth.get("body") and op["method"] in BODY_METHODS:
        if body is None:
            body = {}
        if isinstance(body, dict):
            for name, template in auth["body"].items():
                body[name] = _fill(template, cred, False)
    encoding = op.get("encoding") or app.get("encoding") or "json"
    text = None
    if body is not None and op["method"] in BODY_METHODS:
        if encoding == "form":
            text = urlencode(_form_pairs(body))
            headers.setdefault("Content-Type",
                               "application/x-www-form-urlencoded")
        else:
            text = json.dumps(body)
            headers.setdefault("Content-Type", "application/json")
    if query:
        url += ("&" if "?" in url else "?") + urlencode(query)
    headers.setdefault("Accept", "application/json")
    return op["method"], url, headers, text


def _template_keys(template):
    found = set()
    if isinstance(template, dict):
        for item in template.values():
            found |= _template_keys(item)
    elif isinstance(template, list):
        for item in template:
            found |= _template_keys(item)
    elif isinstance(template, str):
        found |= set(PLACEHOLDER_RE.findall(template))
    return found


def _form_pairs(body, prefix=""):
    pairs = []
    for key, value in body.items():
        name = prefix + "[" + key + "]" if prefix else key
        if isinstance(value, dict):
            pairs.extend(_form_pairs(value, name))
        elif isinstance(value, list):
            for item in value:
                pairs.append((name, _stringify(item)))
        else:
            pairs.append((name, _stringify(value)))
    return pairs


def build_http_request(values):
    method = values.get("method") or "GET"
    url = str(values.get("url") or "")
    headers = values.get("headers") or {}
    if not isinstance(headers, dict):
        raise NodeError("Headers must be a JSON object")
    headers = {str(k): _stringify(v) for k, v in headers.items()}
    query = values.get("query") or {}
    if not isinstance(query, dict):
        raise NodeError("Query parameters must be a JSON object")
    if query:
        url += ("&" if "?" in url else "?") + urlencode(
            {str(k): _stringify(v) for k, v in query.items()})
    text = None
    body = values.get("body")
    if method in BODY_METHODS and body not in (None, ""):
        kind = values.get("bodyType") or "json"
        if kind == "text":
            text = _stringify(body)
            headers.setdefault("Content-Type", "text/plain")
        else:
            if isinstance(body, str):
                try:
                    body = json.loads(body)
                except ValueError:
                    raise NodeError("Body is not valid JSON")
            if kind == "form":
                if not isinstance(body, dict):
                    raise NodeError("A form body must be a JSON object")
                text = urlencode(_form_pairs(body))
                headers.setdefault("Content-Type",
                                   "application/x-www-form-urlencoded")
            else:
                text = json.dumps(body)
                headers.setdefault("Content-Type", "application/json")
    return method, url, headers, text


def parse_response(text):
    body = str(text or "")[:MAX_RESPONSE_CHARS]
    stripped = body.strip()
    if stripped[:1] in ("{", "["):
        try:
            return json.loads(stripped)
        except ValueError:
            pass
    return {"body": body} if stripped else {}


def _texts(blocks):
    return "".join(
        str(block.get("text") or "") for block in blocks or []
        if isinstance(block, dict) and block.get("type", "text") == "text")


def with_text(output, parsed):
    if not output or not isinstance(parsed, dict):
        return parsed
    text = None
    if output == "anthropic":
        text = _texts(parsed.get("content"))
    elif output == "openai":
        choice = _step(parsed.get("choices"), 0) or {}
        text = (_step(choice, "message") or {}).get("content")
    elif output == "gemini":
        candidate = _step(parsed.get("candidates"), 0) or {}
        text = _texts((_step(candidate, "content") or {}).get("parts"))
    elif output == "cohere":
        text = _texts((parsed.get("message") or {}).get("content"))
    if text is None:
        return parsed
    return dict(parsed, text=text)


def items_from(value):
    if isinstance(value, list):
        return [item if isinstance(item, dict) else {"value": item}
                for item in value[:MAX_ITEMS]]
    if isinstance(value, dict):
        return [value]
    return [{"value": value}]


def _number(value):
    if isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def check_condition(left, operator, right):
    if operator == "isEmpty":
        return left in (None, "", [], {})
    if operator == "isNotEmpty":
        return left not in (None, "", [], {})
    if operator == "isTrue":
        return left is True or _stringify(left).lower() == "true"
    if operator == "isFalse":
        return left is False or _stringify(left).lower() in ("false", "")
    if operator in ("gt", "gte", "lt", "lte"):
        a, b = _number(left), _number(right)
        if a is None or b is None:
            return False
        return {"gt": a > b, "gte": a >= b, "lt": a < b, "lte": a <= b}[operator]
    a, b = _stringify(left), _stringify(right)
    if operator == "equals":
        return a == b
    if operator == "notEquals":
        return a != b
    if operator == "contains":
        if isinstance(left, list):
            return any(_stringify(item) == b for item in left)
        return b in a
    if operator == "notContains":
        if isinstance(left, list):
            return not any(_stringify(item) == b for item in left)
        return b not in a
    if operator == "startsWith":
        return a.startswith(b)
    if operator == "endsWith":
        return a.endswith(b)
    if operator in ("matches", "notMatches"):
        found = actions.regex(b, False).compiled.search(a) is not None
        return found if operator == "matches" else not found
    raise NodeError("Unknown operator " + str(operator))


def _summarize_items(items):
    out = []
    for item in items[:MAX_RUN_ITEMS]:
        encoded = json.dumps(item, default=str)
        if len(encoded) > MAX_RUN_ITEM_CHARS:
            out.append({"_truncated": encoded[:MAX_RUN_ITEM_CHARS]})
        else:
            out.append(item)
    return out


def iso_time(now_ms):
    return datetime.fromtimestamp(now_ms / 1000, timezone.utc).isoformat(
        timespec="seconds").replace("+00:00", "Z")


async def _sleep(ms):
    await asyncio.sleep(ms / 1000)


class Execution:
    def __init__(self, workflow, http, credentials, max_requests, deadline_ms,
                 clock, now_ms, workflow_id="", random=None, sleep=None,
                 load_workflow=None, depth=0, counter=None):
        self.workflow = workflow
        self.http = http
        self.credentials = credentials or {}
        self.max_requests = max_requests
        self.deadline_ms = deadline_ms
        self.clock = clock
        self.now_ms = now_ms
        self.workflow_id = workflow_id
        self.random = random or random_module.random
        self.sleep = sleep or _sleep
        self.load_workflow = load_workflow
        self.depth = depth
        self.counter = counter if counter is not None else {"requests": 0}
        self.outputs = {}
        self.trigger_item = {}
        self.by_name = {}
        self.variables = {}
        self.logs = {}
        self.stopped = ""

    @property
    def requests(self):
        return self.counter["requests"]

    def context(self, item, index):
        nodes = {}
        for name, node_id in self.by_name.items():
            main = self._first_output(node_id)
            if main:
                nodes[name] = {"json": main[index] if index < len(main)
                               else main[0]}
        return {
            "json": item,
            "item": {"index": index},
            "node": nodes,
            "trigger": self.trigger_item,
            "now": iso_time(self.now_ms),
            "today": iso_time(self.now_ms)[:10],
            "vars": self.variables,
            "workflow": {"id": self.workflow_id,
                         "name": self.workflow.get("name", "")},
        }

    def values(self, node, item, index):
        params = node.get("params") or {}
        return field_values(_fields_for(node["type"], params), params,
                            self.context(item, index))

    def read_path(self, path, item, index):
        path = str(path or "").strip()
        if not path:
            return None
        return evaluate_expression(path if path.startswith("$") else
                                   "$json." + path, self.context(item, index))

    def _first_output(self, node_id):
        out = self.outputs.get(node_id) or {}
        for items in out.values():
            if items:
                return items
        return []

    async def request(self, method, url, headers, body):
        if not safe_url(url):
            raise NodeError("Only public https:// URLs can be called")
        if self.counter["requests"] >= self.max_requests:
            raise NodeError("This run reached its limit of "
                            + str(self.max_requests) + " requests")
        if self.clock() > self.deadline_ms:
            raise NodeError("This run ran out of time")
        self.counter["requests"] += 1
        status, text = await self.http(method, url, headers, body)
        if not status:
            raise NodeError("No response from " + (urlparse(url).hostname or url))
        parsed = parse_response(text)
        if status >= 400:
            detail = text if isinstance(text, str) else ""
            raise NodeError("HTTP " + str(status) + ": " + detail.strip()[:300])
        return parsed

    async def run_node(self, node, items):
        node_type = node["type"]
        params = node.get("params") or {}
        if node_type in ("core.manual", "core.webhook", "core.schedule"):
            return {"main": [self.trigger_item]}
        if node.get("disabled"):
            first = outputs_for(node_type)[0]
            return {first: items}
        if node_type in ("core.merge", "core.noop", "core.comment"):
            return {"main": items[:MAX_ITEMS]}
        if node_type == "core.limit":
            values = field_values(_fields_for(node_type, params), params,
                                  self.context({}, 0))
            return {"main": items[:max(0, int(values["max"]))]}
        if node_type in actions.NODES:
            return await self.run_action(node, items[:MAX_ITEMS])
        out = {name: [] for name in outputs_for(node_type)}
        for index, item in enumerate(items[:MAX_ITEMS]):
            context = self.context(item, index)
            values = field_values(_fields_for(node_type, params), params,
                                  context)
            if node_type == "core.set":
                extra = values.get("values")
                if not isinstance(extra, dict):
                    raise NodeError("Fields must be a JSON object")
                merged = dict(item) if values.get("keep") is not False else {}
                for key, value in extra.items():
                    _set_path(merged, str(key), value)
                out["main"].append(merged)
            elif node_type in ("core.if", "core.filter"):
                passed = check_condition(values.get("left"),
                                         values.get("operator"),
                                         values.get("right"))
                if node_type == "core.filter":
                    if passed:
                        out["main"].append(item)
                else:
                    out["true" if passed else "false"].append(item)
            elif node_type == "core.split":
                found = evaluate_expression("$json." + values["field"]
                                            if not values["field"].startswith("$")
                                            else values["field"], context)
                if isinstance(found, list):
                    out["main"].extend(items_from(found))
            elif node_type == "core.http":
                method, url, headers, body = build_http_request(values)
                out["main"].extend(items_from(
                    await self.request(method, url, headers, body)))
            else:
                app_key = _app_key(node_type)
                cred = {}
                app = integrations.INTEGRATIONS[app_key]
                if app["auth"]["fields"]:
                    record = self.credentials.get(node.get("credential") or "")
                    if not record or record.get("integration") != app_key:
                        raise NodeError("Choose a " + app["name"]
                                        + " credential for this step")
                    cred = record.get("values") or {}
                method, url, headers, body = build_app_request(
                    app_key, params, values, cred)
                parsed = await self.request(method, url, headers, body)
                output = app["operations"][params["operation"]].get("output")
                out["main"].extend(items_from(with_text(output, parsed)))
            for name in out:
                del out[name][MAX_ITEMS:]
        return out

    async def run_action(self, node, items):
        node_type = node["type"]
        if node_type == "core.code":
            return {"main": self.run_script(node, items)[:MAX_ITEMS]}
        if node_type == "core.wait":
            await self.wait(self.values(node, items[0], 0)["seconds"])
            return {"main": items}
        if node_type == "core.stop":
            values = self.values(node, items[0], 0)
            if values["outcome"] == "error":
                raise NodeError(values.get("message")
                                or "The workflow was stopped")
            raise StopRun({"main": items})
        if node_type == "core.pick":
            return {"main": self.pick(self.values(node, items[0], 0), items)}
        if node_type == "core.sort":
            params = node.get("params") or {}
            values = self.values(node, items[0], 0)
            keys = [actions.sort_key(self.sort_value(params.get("by"), item,
                                                     index))
                    for index, item in enumerate(items)]
            return {"main": actions.sort_items(items, keys, values["order"],
                                               self.random)}
        if node_type == "core.dedupe":
            by = (node.get("params") or {}).get("by")
            seen = set()
            kept = []
            for index, item in enumerate(items):
                key = json.dumps(self.read_path(by, item, index) if str(
                    by or "").strip() else item)
                if key not in seen:
                    seen.add(key)
                    kept.append(item)
            return {"main": kept}
        if node_type == "core.statistics":
            numbers = [self.values(node, item, index).get("value")
                       for index, item in enumerate(items)]
            values = self.values(node, items[0], 0)
            result = actions.statistics(numbers, values["operation"])
            return {"main": [actions.with_field(
                {}, values["field"], actions.number_out(result))]}
        if node_type == "core.combineItems":
            whole = not str((node.get("params") or {}).get("value") or "").strip()
            gathered = [actions.copy_json(item) if whole else
                        self.values(node, item, index).get("value")
                        for index, item in enumerate(items)]
            field = self.values(node, items[0], 0)["field"]
            return {"main": [actions.with_field({}, field, gathered)]}
        out = {name: [] for name in outputs_for(node_type)}
        for index, item in enumerate(items):
            values = self.values(node, item, index)
            if node_type == "core.switch":
                text = _stringify(values.get("value"))
                for case in ("case1", "case2", "case3"):
                    expected = values.get(case)
                    if expected not in (None, "") and expected == text:
                        out[case].append(item)
                        break
                else:
                    out["otherwise"].append(item)
            elif node_type == "core.repeat":
                times = int(max(0, min(actions.MAX_REPEAT, values["times"])))
                for count in range(1, times + 1):
                    out["main"].append(actions.with_field(
                        item, values["field"], count))
            elif node_type == "core.variable":
                self.set_variable(values)
                out["main"].append(item)
            elif node_type == "core.runWorkflow":
                out["main"].extend(await self.run_child(values["workflow"],
                                                        item))
            else:
                out["main"].append(actions.with_field(
                    item, values["field"], self.compute(node_type, values)))
            for name in out:
                del out[name][MAX_ITEMS:]
        return out

    def compute(self, node_type, values):
        text = values.get("value")
        text = "" if text is None else _stringify(text)
        if node_type == "core.textCase":
            return actions.change_case(text, values["case"])
        if node_type == "core.textReplace":
            return actions.replace_text(text, values.get("find") or "",
                                        values.get("replace") or "",
                                        values.get("regex") is True,
                                        values.get("ignoreCase") is True)
        if node_type == "core.textMatch":
            return actions.match_text(text, values["pattern"], values["result"],
                                      values.get("ignoreCase") is True)
        if node_type == "core.textSplit":
            return actions.split_text(text, values["separator"],
                                      values.get("custom") or "",
                                      values.get("skipEmpty") is not False)
        if node_type == "core.textCombine":
            return actions.combine_text(values.get("value"),
                                        values["separator"],
                                        values.get("custom") or "")
        if node_type == "core.textEncode":
            return actions.encode_text(text, values["method"])
        if node_type == "core.hash":
            return actions.hash_text(text, values["algorithm"],
                                     values.get("key") or "",
                                     values["encoding"])
        if node_type == "core.textExtract":
            return actions.extract(text, values["kind"])
        if node_type == "core.count":
            return actions.count(values.get("value"), values["what"])
        if node_type == "core.math":
            second = values.get("b")
            return actions.number_out(actions.calculate(
                actions.to_number(values.get("a"), "Number"),
                values["operator"],
                None if second is None else actions.to_number(
                    second, "Second number")))
        if node_type == "core.round":
            return actions.number_out(actions.round_number(
                actions.to_number(values.get("value"), "Number"),
                values["decimals"], values["mode"]))
        if node_type == "core.formatNumber":
            return actions.format_number(
                actions.to_number(values.get("value"), "Number"),
                values["decimals"], values.get("grouping") is not False)
        if node_type == "core.random":
            return actions.number_out(self.random_number(values))
        if node_type == "core.json":
            return self.json_value(values)
        if node_type == "core.url":
            return actions.url_parts(text)
        if node_type == "core.dateFormat":
            return actions.format_date(values.get("value"), values["format"],
                                       values.get("pattern") or "",
                                       values.get("timeZone") or "")
        if node_type == "core.dateAdjust":
            return actions.adjust_date(values.get("value"), values["operation"],
                                       values.get("amount"), values["unit"],
                                       values.get("timeZone") or "")
        if node_type == "core.dateDiff":
            return actions.date_difference(values.get("start"),
                                           values.get("end"), values["unit"])
        raise NodeError("Unknown step " + node_type)

    def json_value(self, values):
        value = values.get("value")
        if values["operation"] == "stringify":
            if values.get("pretty") is True:
                return json.dumps(value, indent=2)
            return json.dumps(value)
        if value is None:
            raise NodeError("There is no JSON text to read")
        if not isinstance(value, str):
            return value
        try:
            return json.loads(value, parse_constant=_reject_constant)
        except (ValueError, RecursionError):
            raise NodeError("The text is not valid JSON")

    def random_number(self, values):
        low, high = values["min"], values["max"]
        if values.get("whole") is not False:
            low, high = int(-(-low // 1)), int(high // 1)
            if low > high:
                raise NodeError("From must not be larger than To")
            return low + int(self.random() * (high - low + 1) // 1)
        if low > high:
            raise NodeError("From must not be larger than To")
        return low + self.random() * (high - low)

    def set_variable(self, values):
        name = values.get("name") or ""
        if not actions.VARIABLE_RE.match(name):
            raise NodeError("Variable names use letters, digits and _")
        value = actions.copy_json(values.get("value"))
        mode = values["mode"]
        if mode == "set":
            self.variables[name] = value
        elif mode == "append":
            existing = self.variables.get(name)
            items = list(existing) if isinstance(existing, list) else (
                [] if existing is None else [existing])
            items.append(value)
            self.variables[name] = items[-MAX_ITEMS * 10:]
        else:
            existing = self.variables.get(name)
            base = 0.0 if existing is None else actions.to_number(
                existing, "The variable " + name)
            self.variables[name] = actions.number_out(
                base + actions.to_number(value, "Value"))

    def sort_value(self, by, item, index):
        if str(by or "").strip():
            return self.read_path(by, item, index)
        for value in item.values():
            return value
        return None

    def pick(self, values, items):
        which = values["which"]
        if which == "first":
            return items[:1]
        if which == "last":
            return items[-1:]
        if which == "random":
            return [items[int(self.random() * len(items) // 1)]]
        position = int(values.get("index") or 0)
        if which == "index":
            if position < 0:
                position += len(items) + 1
            return [items[position - 1]] if 1 <= position <= len(items) else []
        end = int(values.get("to") or 0)
        return items[max(0, position - 1):max(0, end)]

    async def wait(self, seconds):
        if seconds < 0:
            raise NodeError("Seconds must be 0 or more")
        if seconds > actions.MAX_WAIT_SECONDS:
            raise NodeError("Wait at most " + str(actions.MAX_WAIT_SECONDS)
                            + " seconds")
        ms = int(seconds * 1000)
        if self.clock() + ms > self.deadline_ms:
            raise NodeError("Waiting would run past this run's time limit")
        await self.sleep(ms)

    async def run_child(self, ref, item):
        ref = str(ref or "").strip()
        if self.depth + 1 >= actions.MAX_WORKFLOW_DEPTH:
            raise NodeError("Workflows can run other workflows at most "
                            + str(actions.MAX_WORKFLOW_DEPTH - 1) + " deep")
        loaded = await self.load_workflow(ref) if self.load_workflow else None
        if not loaded:
            raise NodeError("No workflow named " + ref)
        workflow, credentials, child_id = loaded
        child = Execution(workflow, self.http, credentials, self.max_requests,
                          self.deadline_ms, self.clock, self.now_ms, child_id,
                          self.random, self.sleep, self.load_workflow,
                          self.depth + 1, self.counter)
        report = await child.run(item)
        if report["status"] != "success":
            raise NodeError(workflow.get("name", ref) + " failed: "
                            + report["error"])
        return child.final_items(report)

    def final_items(self, report):
        nodes = self.workflow.get("nodes") or []
        connections = self.workflow.get("connections") or []
        if self.stopped:
            ids = [self.by_name[self.stopped]]
        else:
            sources = {conn["from"] for conn in connections}
            order = topological_order(nodes, connections) or []
            ids = [node_id for node_id in order if node_id not in sources]
        found = []
        for node_id in ids:
            if (report["nodes"].get(node_id) or {}).get("status") != "success":
                continue
            for items in (self.outputs.get(node_id) or {}).values():
                found.extend(items)
        return found[:MAX_ITEMS]

    def run_script(self, node, items):
        values = self.values(node, items[0], 0)
        try:
            program = script.parse(values["code"])
        except script.ScriptError as error:
            raise NodeError(str(error))
        interp = script.Interpreter(self.now_ms, self.random, self.clock,
                                    self.deadline_ms)
        script_items = [script.from_json(item) for item in items]
        shared = {
            "$trigger": script.from_json(self.trigger_item),
            "$vars": script.from_json(self.variables),
            "$now": iso_time(self.now_ms),
            "$today": iso_time(self.now_ms)[:10],
            "$workflow": script.from_json({"id": self.workflow_id,
                                           "name": self.workflow.get("name",
                                                                     "")}),
            "$utils": actions.utils_object(interp),
        }
        out = []
        try:
            if values["mode"] == "each":
                for index, item in enumerate(script_items):
                    current = dict(shared, **{
                        "items": script_items,
                        "item": item,
                        "$json": item,
                        "$index": float(index),
                        "$node": script.from_json(
                            self.context(items[index], index)["node"]),
                        "$input": self._script_input(script_items, item),
                    })
                    result = interp.run(program, current)
                    out.extend(script.unwrap_items(result, item))
            else:
                current = dict(shared, **{
                    "items": script_items,
                    "$node": script.from_json(self.context(items[0], 0)["node"]),
                    "$input": self._script_input(script_items, None),
                })
                result = interp.run(program, current)
                out = script.unwrap_items(result, script_items)
            variables = script.to_json(shared["$vars"])
        except script.ScriptError as error:
            raise NodeError(str(error))
        except script.ScriptLimit as error:
            raise NodeError(str(error))
        finally:
            if interp.logs:
                self.logs[node["id"]] = list(interp.logs)
        self.variables = variables if isinstance(variables, dict) else {}
        return out

    def _script_input(self, script_items, current):
        def wrap(item):
            return {"json": item}
        out = {
            "all": script.native("all", lambda i, a: [wrap(item) for item in
                                                      script_items]),
            "first": script.native("first", lambda i, a: wrap(script_items[0])
                                   if script_items else script.UNDEFINED),
            "last": script.native("last", lambda i, a: wrap(script_items[-1])
                                  if script_items else script.UNDEFINED),
        }
        if current is not None:
            out["item"] = wrap(current)
        return out

    async def run(self, trigger_item):
        if self.depth:
            return await self._run(trigger_item)
        token = script.regex_budget()
        try:
            return await self._run(trigger_item)
        finally:
            script.REGEX_BUDGET.reset(token)

    async def _run(self, trigger_item):
        self.trigger_item = trigger_item if isinstance(trigger_item, dict) else {}
        nodes = self.workflow.get("nodes") or []
        connections = self.workflow.get("connections") or []
        by_id = {node["id"]: node for node in nodes}
        self.by_name = {node["name"]: node["id"] for node in nodes}
        order = topological_order(nodes, connections) or []
        start = trigger_node(self.workflow)
        report = {}
        failed = ""
        for node_id in order:
            node = by_id[node_id]
            started = self.clock()
            if failed or self.stopped:
                report[node_id] = {"status": "skipped"}
                continue
            if is_trigger(node["type"]):
                if not start or node_id != start["id"]:
                    report[node_id] = {"status": "skipped"}
                    continue
                items = [self.trigger_item]
            else:
                items = []
                fed = False
                for conn in connections:
                    if conn["to"] != node_id:
                        continue
                    produced = (self.outputs.get(conn["from"]) or {}).get(
                        conn["output"])
                    if produced is not None:
                        fed = True
                        items.extend(produced)
                if not fed or not items:
                    report[node_id] = {"status": "skipped"}
                    continue
            try:
                try:
                    out = await self.run_node(node, items)
                except script.ScriptLimit as limit:
                    raise NodeError(str(limit))
            except StopRun as stop:
                self.outputs[node_id] = stop.out
                report[node_id] = self._report(node_id, stop.out, started, "")
                self.stopped = node["name"]
                continue
            except NodeError as error:
                if node.get("continueOnFail"):
                    out = {name: [] for name in outputs_for(node["type"])}
                    out[outputs_for(node["type"])[0]] = [{"error": str(error)}]
                    self.outputs[node_id] = out
                    report[node_id] = self._report(node_id, out, started,
                                                   str(error))
                    continue
                report[node_id] = {"status": "error", "error": str(error),
                                   "ms": max(0, self.clock() - started)}
                if self.logs.get(node_id):
                    report[node_id]["logs"] = self.logs[node_id]
                failed = node["name"] + ": " + str(error)
                continue
            self.outputs[node_id] = out
            report[node_id] = self._report(node_id, out, started, "")
        result = {
            "status": "error" if failed else "success",
            "error": failed,
            "requests": self.requests,
            "nodes": report,
        }
        if self.stopped:
            result["stopped"] = self.stopped
        return result

    def _report(self, node_id, out, started, error):
        entry = {
            "status": "success",
            "ms": max(0, self.clock() - started),
            "outputs": {name: {"count": len(items),
                               "items": _summarize_items(items)}
                        for name, items in out.items()},
        }
        if error:
            entry["error"] = error
        if self.logs.get(node_id):
            entry["logs"] = self.logs[node_id]
        return entry


def _reject_constant(name):
    raise ValueError(name)


async def execute(workflow, trigger_item, http, credentials, max_requests,
                  deadline_ms, clock, now_ms, workflow_id="", random=None,
                  sleep=None, load_workflow=None):
    execution = Execution(workflow, http, credentials, max_requests,
                          deadline_ms, clock, now_ms, workflow_id, random,
                          sleep, load_workflow)
    return await execution.run(trigger_item)
