import json
import re

import workflow_integrations as integrations
import workflows as engine

FORMAT = "forkmesh-workflow"
VERSION = 1
MAX_DOCUMENT_BYTES = 1024 * 1024
X_SCALE = 1.25
N8N = "n8n-nodes-base."
N8N_LANGCHAIN = "@n8n/n8n-nodes-langchain."
EXPORT_TYPES = {
    "core.manual": ("manualTrigger", 1),
    "core.webhook": ("webhook", 2),
    "core.schedule": ("scheduleTrigger", 1.2),
    "core.http": ("httpRequest", 4.2),
    "core.set": ("set", 3.4),
    "core.if": ("if", 2),
    "core.filter": ("filter", 2),
    "core.merge": ("merge", 3),
    "core.limit": ("limit", 1),
    "core.split": ("splitOut", 1),
    "core.noop": ("noOp", 1),
}
IMPORT_TYPES = {
    "manualTrigger": "core.manual",
    "start": "core.manual",
    "webhook": "core.webhook",
    "scheduleTrigger": "core.schedule",
    "cron": "core.schedule",
    "interval": "core.schedule",
    "httpRequest": "core.http",
    "set": "core.set",
    "if": "core.if",
    "filter": "core.filter",
    "merge": "core.merge",
    "limit": "core.limit",
    "splitOut": "core.split",
    "itemLists": "core.split",
    "noOp": "core.noop",
}
SKIPPED_TYPES = {"stickyNote"}
N8N_APP_ALIASES = {
    "microsoftTeams": "teams",
    "googleSheets": "googleSheets",
    "googleCalendar": "googleCalendar",
    "googleDrive": "googleDrive",
    "gmail": "gmail",
    "microsoftOutlook": "microsoft365",
    "microsoftToDo": "microsoft365",
    "mondayCom": "monday",
    "zohoCrm": "zohoCrm",
    "helpScout": "helpscout",
    "wooCommerce": "woocommerce",
    "lemonSqueezy": "lemonsqueezy",
    "quickbooks": "quickbooks",
    "twitter": "x",
    "facebookGraphApi": "facebookPages",
    "youTube": "youtube",
    "wordpress": "wordpress",
    "openAi": "openai",
    "lmChatOpenAi": "openai",
    "lmChatAnthropic": "anthropic",
    "anthropic": "anthropic",
    "lmChatGoogleGemini": "gemini",
    "googleGemini": "gemini",
    "lmChatMistralCloud": "mistral",
    "mistralAi": "mistral",
    "lmChatGroq": "groq",
    "perplexity": "perplexity",
    "deepL": "deepl",
    "openWeatherMap": "openweathermap",
    "homeAssistant": "homeAssistant",
    "supabase": "supabase",
}
OPERATORS_TO_N8N = {
    "equals": ("string", "equals"),
    "notEquals": ("string", "notEquals"),
    "contains": ("string", "contains"),
    "notContains": ("string", "notContains"),
    "startsWith": ("string", "startsWith"),
    "endsWith": ("string", "endsWith"),
    "gt": ("number", "gt"),
    "gte": ("number", "gte"),
    "lt": ("number", "lt"),
    "lte": ("number", "lte"),
    "isEmpty": ("string", "empty"),
    "isNotEmpty": ("string", "notEmpty"),
    "isTrue": ("boolean", "true"),
    "isFalse": ("boolean", "false"),
}
OPERATORS_FROM_N8N = {
    "equals": "equals", "equal": "equals", "notEquals": "notEquals",
    "notEqual": "notEquals", "contains": "contains",
    "notContains": "notContains", "startsWith": "startsWith",
    "notStartsWith": "notEquals", "endsWith": "endsWith",
    "gt": "gt", "larger": "gt", "gte": "gte", "largerEqual": "gte",
    "lt": "lt", "smaller": "lt", "lte": "lte", "smallerEqual": "lte",
    "empty": "isEmpty", "isEmpty": "isEmpty", "notEmpty": "isNotEmpty",
    "isNotEmpty": "isNotEmpty", "true": "isTrue", "false": "isFalse",
    "exists": "isNotEmpty", "notExists": "isEmpty",
}
LEGACY_NODE_RE = re.compile(r"\$\(\s*(['\"])(.+?)\1\s*\)\.(?:item|first\(\))\.json")
SCHEDULE_UNITS = {"seconds": 1 / 60, "minutes": 1, "hours": 60, "days": 1440,
                  "weeks": 10080}


class FormatError(ValueError):
    pass


def _to_n8n_value(value):
    if isinstance(value, str) and "{{" in value:
        return "=" + value
    return value


def _from_n8n_value(value):
    if isinstance(value, str) and value.startswith("="):
        value = value[1:]
        value = LEGACY_NODE_RE.sub(
            lambda m: "$node[" + json.dumps(m.group(2)) + "].json", value)
        value = value.replace("$input.item.json", "$json").replace(
            "$input.first().json", "$json")
    if isinstance(value, (dict, list)):
        return value
    return value


def _json_text(value):
    if value in (None, ""):
        return None
    if isinstance(value, (dict, list)):
        return value
    try:
        return json.loads(str(value))
    except ValueError:
        return None


def _pairs(mapping):
    if not isinstance(mapping, dict):
        return []
    return [{"name": str(k), "value": _to_n8n_value(v)}
            for k, v in mapping.items()]


def _condition_to_n8n(params):
    kind, operation = OPERATORS_TO_N8N.get(
        params.get("operator") or "equals", ("string", "equals"))
    condition = {"id": "c1", "leftValue": _to_n8n_value(params.get("left", "")),
                 "operator": {"type": kind, "operation": operation}}
    if operation not in ("empty", "notEmpty", "true", "false"):
        condition["rightValue"] = _to_n8n_value(params.get("right", ""))
    else:
        condition["operator"]["singleValue"] = True
    return {"conditions": {
        "options": {"caseSensitive": True, "leftValue": "",
                    "typeValidation": "loose"},
        "conditions": [condition], "combinator": "and"}, "options": {}}


def _n8n_parameters(node):
    node_type = node["type"]
    params = node.get("params") or {}
    if node_type == "core.webhook":
        return {"httpMethod": "POST", "path": node["id"], "options": {}}
    if node_type == "core.schedule":
        return {"rule": {"interval": [{
            "field": "minutes",
            "minutesInterval": engine._int(params.get("every"), 60)}]}}
    if node_type == "core.http":
        out = {"method": params.get("method") or "GET",
               "url": _to_n8n_value(params.get("url") or ""), "options": {}}
        headers = _json_text(params.get("headers"))
        if isinstance(headers, dict) and headers:
            out["sendHeaders"] = True
            out["headerParameters"] = {"parameters": _pairs(headers)}
        query = _json_text(params.get("query"))
        if isinstance(query, dict) and query:
            out["sendQuery"] = True
            out["queryParameters"] = {"parameters": _pairs(query)}
        body = params.get("body")
        if body not in (None, ""):
            out["sendBody"] = True
            kind = params.get("bodyType") or "json"
            if kind == "json":
                out["specifyBody"] = "json"
                out["jsonBody"] = _to_n8n_value(
                    body if isinstance(body, str) else json.dumps(body))
            elif kind == "form":
                out["contentType"] = "form-urlencoded"
                out["bodyParameters"] = {"parameters": _pairs(
                    _json_text(body) or {})}
            else:
                out["contentType"] = "raw"
                out["body"] = _to_n8n_value(str(body))
        return out
    if node_type == "core.set":
        values = _json_text(params.get("values")) or {}
        return {"assignments": {"assignments": [
            {"id": "a%d" % index, "name": str(name),
             "value": _to_n8n_value(value),
             "type": "number" if isinstance(value, (int, float))
             and not isinstance(value, bool) else
             "boolean" if isinstance(value, bool) else "string"}
            for index, (name, value) in enumerate(
                values.items() if isinstance(values, dict) else [])]},
            "includeOtherFields": str(params.get("keep", "true")).lower()
            != "false", "options": {}}
    if node_type in ("core.if", "core.filter"):
        return _condition_to_n8n(params)
    if node_type == "core.limit":
        return {"maxItems": engine._int(params.get("max"), 10)}
    if node_type == "core.split":
        return {"fieldToSplitOut": params.get("field") or "", "options": {}}
    return {}


def to_document(workflow, credential_names=None):
    names = credential_names or {}
    by_id = {node["id"]: node for node in workflow.get("nodes") or []}
    nodes = []
    for node in workflow.get("nodes") or []:
        node_type = node["type"]
        if node_type in EXPORT_TYPES:
            n8n_type, version = EXPORT_TYPES[node_type]
            exported_type = N8N + n8n_type
            parameters = _n8n_parameters(node)
        else:
            exported_type, version = "forkmesh." + node_type, 1
            parameters = {key: _to_n8n_value(value) for key, value in
                          (node.get("params") or {}).items()}
        item = {
            "id": node["id"],
            "name": node["name"],
            "type": exported_type,
            "typeVersion": version,
            "position": [int(round(node.get("x", 0) / X_SCALE)),
                         int(node.get("y", 0))],
            "parameters": parameters,
            "forkmeshType": node_type,
            "forkmeshParams": node.get("params") or {},
        }
        if node.get("disabled"):
            item["disabled"] = True
        if node.get("continueOnFail"):
            item["onError"] = "continueRegularOutput"
        if node.get("credential"):
            item["credentials"] = {node_type[4:]: {
                "id": node["credential"],
                "name": names.get(node["credential"], "")}}
        nodes.append(item)
    connections = {}
    for conn in workflow.get("connections") or []:
        source = by_id.get(conn["from"])
        target = by_id.get(conn["to"])
        if not source or not target:
            continue
        outputs = engine.outputs_for(source["type"])
        index = max(0, outputs.index(conn["output"])
                    if conn["output"] in outputs else 0)
        slots = connections.setdefault(source["name"], {"main": []})["main"]
        while len(slots) <= index:
            slots.append([])
        slots[index].append({"node": target["name"], "type": "main",
                             "index": 0})
    return {
        "name": workflow.get("name") or "Untitled workflow",
        "nodes": nodes,
        "connections": connections,
        "active": False,
        "settings": {"executionOrder": "v1"},
        "pinData": {},
        "meta": {"format": FORMAT, "version": VERSION,
                 "generator": "ForkMesh Workflows",
                 "templateCredsSetupCompleted": False},
        "tags": [],
    }


def _unwrap(document):
    if isinstance(document, list):
        for item in document:
            found = _unwrap(item)
            if found is not None:
                return found
        return None
    if not isinstance(document, dict):
        return None
    if isinstance(document.get("nodes"), list) and isinstance(
            document.get("connections"), (dict, list)):
        return document
    for key in ("workflow", "data", "template"):
        found = _unwrap(document.get(key))
        if found is not None:
            return found
    return None


def parse_document(text):
    if isinstance(text, (dict, list)):
        data = text
    else:
        raw = str(text or "")
        if len(raw.encode("utf-8")) > MAX_DOCUMENT_BYTES:
            raise FormatError("document_too_large")
        try:
            data = json.loads(raw)
        except ValueError:
            raise FormatError("invalid_document")
    document = _unwrap(data)
    if document is None:
        raise FormatError("not_a_workflow")
    return document


def _short_type(full):
    text = str(full or "")
    for prefix in (N8N, N8N_LANGCHAIN, "n8n-nodes-"):
        if text.startswith(prefix):
            return text[len(prefix):]
    return text


def _schedule_minutes(params):
    rule = params.get("rule") if isinstance(params, dict) else None
    intervals = (rule or {}).get("interval") if isinstance(rule, dict) else None
    if isinstance(intervals, list) and intervals:
        first = intervals[0] if isinstance(intervals[0], dict) else {}
        field = first.get("field") or "days"
        amount = engine._int(first.get(field + "Interval"), 1) or 1
        return int(max(1, round(SCHEDULE_UNITS.get(field, 1440) * amount)))
    for key, unit in (("minutesInterval", 1), ("hoursInterval", 60),
                      ("interval", 1)):
        if key in (params or {}):
            factor = 60 if (params.get("unit") == "hours") else unit
            return max(1, engine._int(params.get(key), 1) * factor)
    return 60


def _first_condition(params, warnings, name):
    conditions = params.get("conditions") if isinstance(params, dict) else None
    if isinstance(conditions, dict) and isinstance(
            conditions.get("conditions"), list):
        items = conditions["conditions"]
        if len(items) > 1:
            warnings.append(name + ": only the first condition was kept")
        if items:
            first = items[0] if isinstance(items[0], dict) else {}
            operator = first.get("operator") or {}
            operation = operator.get("operation") if isinstance(
                operator, dict) else str(operator)
            return {"left": _from_n8n_value(first.get("leftValue", "")),
                    "operator": OPERATORS_FROM_N8N.get(operation, "equals"),
                    "right": _from_n8n_value(first.get("rightValue", ""))}
    if isinstance(conditions, dict):
        for kind in ("string", "number", "boolean", "dateTime"):
            items = conditions.get(kind)
            if isinstance(items, list) and items:
                first = items[0] if isinstance(items[0], dict) else {}
                operation = first.get("operation") or "equal"
                if kind == "boolean" and operation in ("equal", "equals"):
                    operation = "true" if first.get("value2") in (
                        True, "true") else "false"
                return {"left": _from_n8n_value(first.get("value1", "")),
                        "operator": OPERATORS_FROM_N8N.get(
                            operation, "equals"),
                        "right": _from_n8n_value(first.get("value2", ""))}
    return {"left": "", "operator": "isNotEmpty", "right": ""}


def _pairs_to_object(container):
    out = {}
    if isinstance(container, dict):
        for pair in container.get("parameters") or []:
            if isinstance(pair, dict) and pair.get("name"):
                out[str(pair["name"])] = _from_n8n_value(pair.get("value", ""))
    return out


def _set_values(params):
    values = {}
    assignments = params.get("assignments")
    if isinstance(assignments, dict):
        for item in assignments.get("assignments") or []:
            if isinstance(item, dict) and item.get("name"):
                values[str(item["name"])] = _from_n8n_value(item.get("value"))
    fields = params.get("fields")
    if isinstance(fields, dict):
        for item in fields.get("values") or []:
            if isinstance(item, dict) and item.get("name"):
                for key in ("stringValue", "numberValue", "booleanValue",
                            "value"):
                    if key in item:
                        values[str(item["name"])] = _from_n8n_value(item[key])
                        break
    legacy = params.get("values")
    if isinstance(legacy, dict):
        for group in legacy.values():
            for item in group if isinstance(group, list) else []:
                if isinstance(item, dict) and item.get("name"):
                    values[str(item["name"])] = _from_n8n_value(
                        item.get("value"))
    return values


def _core_params(core_type, params, warnings, name):
    params = params if isinstance(params, dict) else {}
    if core_type == "core.schedule":
        return {"every": str(max(engine.SCHEDULE_MIN_MINUTES,
                                 _schedule_minutes(params)))}
    if core_type == "core.http":
        out = {"method": str(params.get("method") or params.get(
            "requestMethod") or "GET").upper(),
               "url": _from_n8n_value(params.get("url") or "")}
        headers = _pairs_to_object(params.get("headerParameters"))
        if headers:
            out["headers"] = json.dumps(headers)
        query = _pairs_to_object(params.get("queryParameters"))
        if query:
            out["query"] = json.dumps(query)
        if params.get("jsonBody") not in (None, ""):
            out["bodyType"] = "json"
            out["body"] = _from_n8n_value(params["jsonBody"])
        elif params.get("bodyParameters"):
            out["bodyType"] = "form" if params.get(
                "contentType") == "form-urlencoded" else "json"
            out["body"] = json.dumps(_pairs_to_object(params["bodyParameters"]))
        elif params.get("body") not in (None, ""):
            out["bodyType"] = "text"
            out["body"] = _from_n8n_value(params["body"])
        if params.get("authentication") not in (None, "", "none"):
            warnings.append(name + ": set its authentication headers again")
        return out
    if core_type == "core.set":
        keep = params.get("includeOtherFields", params.get("keepOnlySet"))
        if "keepOnlySet" in params and "includeOtherFields" not in params:
            keep = not params.get("keepOnlySet")
        return {"values": json.dumps(_set_values(params)),
                "keep": "false" if keep is False else "true"}
    if core_type in ("core.if", "core.filter"):
        return _first_condition(params, warnings, name)
    if core_type == "core.limit":
        return {"max": str(engine._int(params.get("maxItems"), 1) or 1)}
    if core_type == "core.split":
        return {"field": str(params.get("fieldToSplitOut") or "items")}
    return {}


def _app_for(short):
    if short in N8N_APP_ALIASES:
        return N8N_APP_ALIASES[short] or None
    base = re.sub(r"(Trigger|Tool|V\d+)$", "", short)
    lowered = base.lower()
    for key in integrations.INTEGRATIONS:
        if key.lower() == lowered:
            return key
    return None


def _app_params(app_key, params):
    app = integrations.INTEGRATIONS[app_key]
    params = params if isinstance(params, dict) else {}
    operation = str(params.get("operation") or "")
    if operation not in app["operations"]:
        operation = next(iter(app["operations"]))
    out = {"operation": operation}
    keys = {f["key"] for f in app["operations"][operation]["fields"]}
    for key, value in params.items():
        if key in keys:
            out[key] = _from_n8n_value(value)
    return out


def from_document(document):
    document = parse_document(document)
    warnings = []
    raw_nodes = [n for n in document.get("nodes") or [] if isinstance(n, dict)]
    nodes = []
    by_name = {}
    used_names = set()
    has_trigger = False
    for index, raw in enumerate(raw_nodes):
        full_type = str(raw.get("type") or "")
        short = _short_type(full_type)
        if short in SKIPPED_TYPES:
            continue
        original = str(raw.get("name") or short or "Step").strip()[:60] or "Step"
        name = original
        counter = 2
        while name in used_names:
            name = (original[:55] + " " + str(counter))
            counter += 1
        used_names.add(name)
        node_type = ""
        params = {}
        own_type = str(raw.get("forkmeshType") or "")
        if own_type and engine.node_spec(own_type) is not None:
            node_type = own_type
            own = raw.get("forkmeshParams")
            params = own if isinstance(own, dict) else {}
        elif full_type.startswith("forkmesh.") and engine.node_spec(
                full_type[9:]) is not None:
            node_type = full_type[9:]
            params = {k: _from_n8n_value(v) for k, v in
                      (raw.get("parameters") or {}).items()}
        elif short in IMPORT_TYPES:
            node_type = IMPORT_TYPES[short]
            params = _core_params(node_type, raw.get("parameters"), warnings,
                                  name)
        else:
            app_key = _app_for(short)
            if app_key:
                node_type = "app." + app_key
                params = _app_params(app_key, raw.get("parameters"))
                warnings.append(name + ": imported as " + integrations.
                                INTEGRATIONS[app_key]["name"]
                                + "; check its action and fields")
            else:
                node_type = "core.noop"
                warnings.append(name + ": " + (short or "unknown step")
                                + " is not available, kept as No operation")
        if engine.is_trigger(node_type):
            if has_trigger:
                warnings.append(name + ": only one trigger is allowed, "
                                "kept as No operation")
                node_type, params = "core.noop", {}
            has_trigger = True
        position = raw.get("position") if isinstance(
            raw.get("position"), list) else [0, 0]
        try:
            x = float(position[0]) * X_SCALE
            y = float(position[1])
        except (IndexError, TypeError, ValueError):
            x, y = 0.0, 0.0
        node = {
            "id": "n%d" % (index + 1),
            "type": node_type,
            "name": name,
            "x": int(round(x / 8.0) * 8),
            "y": int(round(y / 8.0) * 8),
            "params": params,
            "credential": "",
            "continueOnFail": raw.get("continueOnFail") is True
            or raw.get("onError") in ("continueRegularOutput",
                                       "continueErrorOutput"),
            "disabled": raw.get("disabled") is True,
        }
        if raw.get("credentials") and node_type.startswith("app."):
            warnings.append(name + ": choose a credential")
        nodes.append(node)
        by_name[str(raw.get("name") or "")] = node
        by_name.setdefault(name, node)
    connections = []
    raw_connections = document.get("connections") or {}
    if isinstance(raw_connections, dict):
        for source_name, groups in raw_connections.items():
            source = by_name.get(source_name)
            if not source or not isinstance(groups, dict):
                continue
            outputs = engine.outputs_for(source["type"])
            for slot_index, targets in enumerate(groups.get("main") or []):
                output = outputs[slot_index] if slot_index < len(outputs) \
                    else outputs[0]
                for target in targets or []:
                    if not isinstance(target, dict):
                        continue
                    node = by_name.get(str(target.get("node") or ""))
                    if not node or engine.is_trigger(node["type"]) or \
                            node is source:
                        continue
                    connections.append({"from": source["id"],
                                        "output": output, "to": node["id"]})
    workflow, error = engine.clean_workflow({
        "name": str(document.get("name") or "Imported workflow"),
        "nodes": nodes, "connections": connections})
    if error == "cycle":
        kept = []
        for conn in connections:
            trial, trial_error = engine.clean_workflow({
                "name": "x", "nodes": nodes, "connections": kept + [conn]})
            if not trial_error:
                kept.append(conn)
        warnings.append("Connections that loop back were removed")
        workflow, error = engine.clean_workflow({
            "name": str(document.get("name") or "Imported workflow"),
            "nodes": nodes, "connections": kept})
    if error:
        raise FormatError(error)
    return workflow, warnings
