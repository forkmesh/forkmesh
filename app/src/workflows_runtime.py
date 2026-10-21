import hashlib
import hmac
import json
import re
from urllib.parse import parse_qsl, urlparse
from hidden_unicode import clean_value as clean_hidden_unicode

import instance_features
import workflow_files as files
import workflow_desktop as desktop
import workflow_formats as formats
import workflow_integrations as integrations
import workflow_templates as templates
import workflows as engine


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PREFIX = "/api/workflows"
REQUEST_PROOF = "forkmesh-workflows-request-v1"
BODY_MAX_BYTES = 1536 * 1024
HOOK_BODY_MAX_BYTES = 64 * 1024
MAX_WORKFLOWS = 50
MAX_CREDENTIALS = 50
RUNS_KEPT = 30
RUNS_PER_HOUR = 120
HOUR_MS = 3_600_000
MINUTE_MS = 60_000
REQUEST_TIMEOUT_SECONDS = 30
LIMITS = {
    "manual": (25, 45_000),
    "webhook": (15, 25_000),
    "schedule": (6, 40_000),
}
SCHEDULED_PER_TICK = 3
HEX32 = r"[0-9a-f]{32}"
WORKFLOW_RE = re.compile(
    r"^/api/workflows/(" + HEX32 + r")(?:/(run|delete|runs|record))?/?$")
RUN_RE = re.compile(
    r"^/api/workflows/(" + HEX32 + r")/runs/(" + HEX32 + r")/?$")
CREDENTIAL_RE = re.compile(
    r"^/api/workflows/credentials/(" + HEX32 + r")/delete/?$")
HOOK_RE = re.compile(
    r"^/api/workflows/hook/(" + HEX32 + r")/([0-9a-f]{64})/?$")
USER_AGENT = "ForkMesh-Workflows/1.0 (+https://forkmesh.com)"
TEMPLATE_RE = re.compile(r"^/api/workflows/templates/([a-z0-9-]{1,64})/?$")
GITHUB_BLOB_RE = re.compile(
    r"^https://github\.com/([^/]+)/([^/]+)/blob/(.+)$")
GIST_RE = re.compile(r"^https://gist\.github\.com/([^/]+)/([0-9a-f]+)/?$")
N8N_TEMPLATE_RE = re.compile(r"^https://n8n\.io/workflows/(\d+)")
IMPORT_TIMEOUT_SECONDS = 15


def _now():
    return int(Date.now())


class _PluginState:
    def __init__(self, env):
        self.env = env

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def account_created_at(self, account_bi):
        row = await d1_first(
            self.env, "SELECT data FROM users WHERE user_bi=?", str(account_bi or ""))
        rec = await decrypt_row(self.env, (row or {}).get("data", "")) if row else None
        return int((rec or {}).get("created_at") or 0)


def _plugin_data():
    try:
        import plugin_data
    except Exception:
        return None
    return plugin_data


async def _installed(env, owner_bi, package):
    module = _plugin_data()
    registry = getattr(module, "REGISTRY", {}) if module else {}
    if package not in registry:
        return True
    try:
        return await module.account_package_installed(
            _PluginState(env), owner_bi, package)
    except Exception:
        return True


async def _package_files(env, owner_bi):
    module = _plugin_data()
    registry = getattr(module, "REGISTRY", {}) if module else {}
    try:
        import plugin_registry
        names = {row.get("id"): row.get("name") for row in
                 getattr(plugin_registry, "MARKET_PACKAGES", ())}
    except Exception:
        names = {}
    found = []
    for package_id in sorted(registry):
        entry = registry[package_id]
        files_list = entry.get("files") or ()
        if not files_list:
            continue
        if entry.get("accountSync"):
            visible = await _installed(env, owner_bi, package_id)
        else:
            visible = bool(entry.get("default"))
        if not visible:
            continue
        for item in files_list:
            found.append({"package": package_id,
                          "packageName": names.get(package_id) or package_id,
                          "kind": item.get("kind"), "path": item.get("path"),
                          "name": item.get("name"), "text": item.get("text")})
    return found


def _error(code, status, extra=None):
    return json_response(dict(extra or {}, ok=False, error=code),
                         status=status, cache_control="no-store")


def _ok(payload, status=200):
    return json_response(dict(payload, ok=True), status=status,
                         cache_control="no-store")


def _not_allowed(allow):
    return json_response({"ok": False, "error": "method_not_allowed"},
                         status=405, extra_headers={"allow": allow})


async def _session_account(env, request, data=None, digest=""):
    try:
        name_bi, rec = await _account_session_record(env, request, data)
        if not name_bi or not rec:
            name_bi, rec = await _plugin_scope_signed_session(
                env, request, digest, REQUEST_PROOF, PREFIX)
    except RuntimeError:
        return ""
    if not name_bi or not rec:
        return ""
    if rec.get("status") != "active" or _account_kind(rec) != "user":
        return ""
    return name_bi


async def _body(request, limit=BODY_MAX_BYTES):
    try:
        raw = await bounded_request_text(request, limit)
        data = clean_hidden_unicode(json.loads(raw or "{}"))
    except Exception:
        return None, ""
    digest = hashlib.sha256(raw.encode("utf-8")).hexdigest()
    if isinstance(data, dict):
        return data, digest
    return ({} if data is None else None), digest


def _hook_path(workflow_id, token):
    return PREFIX + "/hook/" + workflow_id + "/" + token


def _summary(row, record):
    workflow = record or {}
    trigger = engine.trigger_node(workflow)
    return {
        "id": row["workflow_id"],
        "name": workflow.get("name") or "Untitled workflow",
        "active": bool(row.get("active")),
        "trigger": trigger["type"] if trigger else "",
        "nodes": len(workflow.get("nodes") or []),
        "scheduleMinutes": int(row.get("schedule_minutes") or 0),
        "nextRunAt": int(row.get("next_run_at") or 0),
        "lastRunAt": int(row.get("last_run_at") or 0),
        "lastStatus": row.get("last_status") or "",
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
    }


def _detail(row, record):
    workflow = record or {}
    out = _summary(row, workflow)
    out["nodes"] = workflow.get("nodes") or []
    out["connections"] = workflow.get("connections") or []
    out["hookPath"] = _hook_path(row["workflow_id"], workflow.get("hookToken")
                                 or "")
    return out


async def _load(env, workflow_id, owner_bi=None):
    row = await d1_first(
        env, "SELECT * FROM workflows WHERE workflow_id=?", workflow_id)
    if not row:
        return None, None
    if owner_bi is not None and row.get("owner_bi") != owner_bi:
        return None, None
    record = await decrypt_row(env, row.get("data"))
    if not isinstance(record, dict):
        return None, None
    return row, record


async def _seed(env, owner_bi, now):
    workflow_id = desktop.seed_id(owner_bi)
    if await d1_first(env, "SELECT workflow_id FROM workflows WHERE "
                      "workflow_id=?", workflow_id):
        return
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM workflows WHERE owner_bi=?", owner_bi)
    if int((count or {}).get("n") or 0) >= MAX_WORKFLOWS:
        return
    workflow, error = engine.clean_workflow(desktop.seed_workflow())
    if error:
        return
    workflow["hookToken"] = _ap_uuid() + _ap_uuid()
    await d1_run(
        env, "INSERT INTO workflows (workflow_id,owner_bi,active,"
        "schedule_minutes,next_run_at,last_run_at,last_status,created_at,"
        "updated_at,data) VALUES (?,?,0,0,0,0,'',?,?,?) "
        "ON CONFLICT(workflow_id) DO NOTHING",
        workflow_id, owner_bi, now, now, await encrypt_row(env, workflow))


async def _list(env, owner_bi, now=0):
    if now:
        await _seed(env, owner_bi, now)
    rows = await d1_all(
        env, "SELECT * FROM workflows WHERE owner_bi=? "
        "ORDER BY updated_at DESC LIMIT ?", owner_bi, MAX_WORKFLOWS)
    items = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        if isinstance(record, dict):
            items.append(_summary(row, record))
    return items


def _schedule_columns(workflow, active, now, previous=None):
    minutes = engine.schedule_minutes(workflow) if active else 0
    if not minutes:
        return 0, 0
    if previous and int(previous.get("schedule_minutes") or 0) == minutes \
            and int(previous.get("next_run_at") or 0) > now:
        return minutes, int(previous["next_run_at"])
    return minutes, now + minutes * MINUTE_MS


async def _create(env, owner_bi, data, now):
    workflow, error = engine.clean_workflow(data)
    if error:
        return _error(error, 400)
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM workflows WHERE owner_bi=?", owner_bi)
    if int((count or {}).get("n") or 0) >= MAX_WORKFLOWS:
        return _error("too_many_workflows", 409)
    workflow_id = _ap_uuid()
    workflow["hookToken"] = _ap_uuid() + _ap_uuid()
    active = data.get("active") is True
    minutes, next_run = _schedule_columns(workflow, active, now)
    await d1_run(
        env, "INSERT INTO workflows (workflow_id,owner_bi,active,"
        "schedule_minutes,next_run_at,last_run_at,last_status,created_at,"
        "updated_at,data) VALUES (?,?,?,?,?,0,'',?,?,?)",
        workflow_id, owner_bi, 1 if active else 0, minutes, next_run, now,
        now, await encrypt_row(env, workflow))
    row, record = await _load(env, workflow_id, owner_bi)
    return _ok({"workflow": _detail(row, record)}, status=201)


async def _update(env, owner_bi, workflow_id, data, now):
    row, record = await _load(env, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    workflow, error = engine.clean_workflow(data)
    if error:
        return _error(error, 400)
    workflow["hookToken"] = record.get("hookToken") or (_ap_uuid() + _ap_uuid())
    if data.get("rotateHook") is True:
        workflow["hookToken"] = _ap_uuid() + _ap_uuid()
    active = data.get("active") is True if "active" in data \
        else bool(row.get("active"))
    minutes, next_run = _schedule_columns(workflow, active, now, row)
    await d1_run(
        env, "UPDATE workflows SET active=?,schedule_minutes=?,next_run_at=?,"
        "updated_at=?,data=? WHERE workflow_id=? AND owner_bi=?",
        1 if active else 0, minutes, next_run, now,
        await encrypt_row(env, workflow), workflow_id, owner_bi)
    row, record = await _load(env, workflow_id, owner_bi)
    return _ok({"workflow": _detail(row, record)})


async def _delete(env, owner_bi, workflow_id):
    row, _record = await _load(env, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    await d1_run(env, "DELETE FROM workflow_runs WHERE workflow_id=?",
                 workflow_id)
    if workflow_id == desktop.seed_id(owner_bi):
        await d1_run(env, "UPDATE workflows SET owner_bi='',active=0,"
                     "schedule_minutes=0,next_run_at=0,data='' WHERE "
                     "workflow_id=? AND owner_bi=?", workflow_id, owner_bi)
    else:
        await d1_run(env, "DELETE FROM workflows WHERE workflow_id=? "
                     "AND owner_bi=?", workflow_id, owner_bi)
    return _ok({"deleted": workflow_id})


async def _credentials_for(env, owner_bi, workflow):
    wanted = sorted({node.get("credential") for node in
                     workflow.get("nodes") or [] if node.get("credential")})
    if not wanted:
        return {}
    rows = await d1_all(
        env, "SELECT credential_id,integration,data FROM workflow_credentials"
        " WHERE owner_bi=? AND credential_id IN (SELECT value FROM json_each(?))",
        owner_bi, json.dumps(wanted))
    out = {}
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        if isinstance(record, dict):
            out[row["credential_id"]] = {
                "integration": row["integration"],
                "values": record.get("values") or {}}
    return out


async def _http(method, url, headers, body):
    init = {"method": method, "headers": dict(headers, **{
        "User-Agent": instance_features.user_agent(USER_AGENT, "workflows")}),
        "redirect": "follow"}
    if body is not None:
        init["body"] = body
    try:
        response = await js_fetch_with_timeout(
            url, init, REQUEST_TIMEOUT_SECONDS)
    except Exception:
        return 0, ""
    if response is None:
        return 0, ""
    status = int(getattr(response, "status", 0) or 0)
    try:
        text = str(await response.text() or "")[:engine.MAX_RESPONSE_CHARS]
    except Exception:
        text = ""
    return status, text


async def _runs_last_hour(env, owner_bi, now):
    row = await d1_first(
        env, "SELECT COUNT(*) AS n FROM workflow_runs WHERE owner_bi=? "
        "AND started_at>?", owner_bi, now - HOUR_MS)
    return int((row or {}).get("n") or 0)


def _workflow_loader(env, owner_bi):
    async def load(ref):
        ref = str(ref or "").strip()
        if not ref:
            return None
        row, record = (await _load(env, ref, owner_bi)) if re.fullmatch(
            HEX32, ref) else (None, None)
        if not row:
            rows = await d1_all(
                env, "SELECT * FROM workflows WHERE owner_bi=? "
                "ORDER BY updated_at DESC LIMIT ?", owner_bi, MAX_WORKFLOWS)
            for candidate in rows or []:
                data = await decrypt_row(env, candidate.get("data"))
                if isinstance(data, dict) and str(data.get("name") or "").strip(
                        ).lower() == ref.lower():
                    row, record = candidate, data
                    break
        if not row:
            return None
        workflow, error = engine.clean_workflow(record)
        if error:
            return None
        credentials = await _credentials_for(env, owner_bi, workflow)
        return workflow, credentials, row["workflow_id"]
    return load


async def _execute(env, row, record, kind, trigger_item, now):
    owner_bi = row["owner_bi"]
    workflow_id = row["workflow_id"]
    max_requests, budget_ms = LIMITS[kind]
    credentials = await _credentials_for(env, owner_bi, record)
    report = await engine.execute(
        record, trigger_item, _http, credentials, max_requests,
        now + budget_ms, _now, now, workflow_id,
        load_workflow=_workflow_loader(env, owner_bi))
    report["output"] = engine.run_output(record, report)
    return await _store_run(env, row, kind, report, now, _now())


async def _store_run(env, row, kind, report, now, finished):
    owner_bi = row["owner_bi"]
    workflow_id = row["workflow_id"]
    run_id = _ap_uuid()
    report["trigger"] = kind
    await d1_run(
        env, "INSERT INTO workflow_runs (run_id,workflow_id,owner_bi,trigger_kind,"
        "status,started_at,finished_at,data) VALUES (?,?,?,?,?,?,?,?)",
        run_id, workflow_id, owner_bi, kind, report["status"], now, finished,
        await encrypt_row(env, report))
    await d1_run(
        env, "DELETE FROM workflow_runs WHERE workflow_id=? AND run_id NOT IN "
        "(SELECT run_id FROM workflow_runs WHERE workflow_id=? "
        "ORDER BY started_at DESC LIMIT ?)", workflow_id, workflow_id,
        RUNS_KEPT)
    await d1_run(
        env, "UPDATE workflows SET last_run_at=?,last_status=? "
        "WHERE workflow_id=?", now, report["status"], workflow_id)
    return dict(report, id=run_id, startedAt=now, finishedAt=finished)


def _manual_payload(record, data):
    payload = data.get("payload") if isinstance(data, dict) else None
    if isinstance(payload, dict):
        return payload
    trigger = engine.trigger_node(record)
    if trigger and trigger["type"] == "core.manual":
        raw = trigger["params"].get("payload")
        if isinstance(raw, dict):
            return raw
        if isinstance(raw, str) and raw.strip():
            try:
                parsed = json.loads(raw)
            except ValueError:
                return {}
            return parsed if isinstance(parsed, dict) else {"value": parsed}
    if trigger and trigger["type"] == "core.webhook":
        return {"method": "POST", "query": {}, "body": {}}
    return {}


async def _run_manual(env, owner_bi, workflow_id, data, now):
    row, record = await _load(env, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    if not engine.trigger_node(record):
        return _error("no_trigger", 400)
    if desktop.has_desktop_steps(record):
        return _error("runs_on_desktop", 409)
    if await _runs_last_hour(env, owner_bi, now) >= RUNS_PER_HOUR:
        return _error("rate_limited", 429)
    run = await _execute(env, row, record, "manual",
                         _manual_payload(record, data), now)
    return _ok({"run": run})


async def _record_run(env, owner_bi, workflow_id, data, now):
    row, record = await _load(env, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    if not desktop.has_desktop_steps(record):
        return _error("not_a_desktop_workflow", 400)
    cleaned, error = desktop.clean_report(
        record, data, engine._summarize_items, now)
    if error:
        return _error(error, 400)
    if await _runs_last_hour(env, owner_bi, now) >= RUNS_PER_HOUR:
        return _error("rate_limited", 429)
    report, started, finished = cleaned
    run = await _store_run(env, row, desktop.RUNS_ON, report, started,
                           finished)
    return _ok({"run": run})


async def _runs(env, owner_bi, workflow_id):
    row, _record = await _load(env, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    rows = await d1_all(
        env, "SELECT run_id,trigger_kind,status,started_at,finished_at,data "
        "FROM workflow_runs WHERE workflow_id=? ORDER BY started_at DESC "
        "LIMIT ?", workflow_id, RUNS_KEPT)
    runs = []
    for r in rows or []:
        report = await decrypt_row(env, r.get("data")) or {}
        output = report.get("output") if isinstance(report, dict) else None
        runs.append({
            "id": r["run_id"], "trigger": r["trigger_kind"],
            "status": r["status"], "startedAt": int(r["started_at"] or 0),
            "finishedAt": int(r["finished_at"] or 0),
            "output": output if isinstance(output, dict) else None})
    return _ok({"runs": runs})


async def _run_detail(env, owner_bi, workflow_id, run_id):
    row = await d1_first(
        env, "SELECT * FROM workflow_runs WHERE run_id=? AND workflow_id=? "
        "AND owner_bi=?", run_id, workflow_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    report = await decrypt_row(env, row.get("data")) or {}
    return _ok({"run": dict(report, id=row["run_id"],
                            startedAt=int(row["started_at"] or 0),
                            finishedAt=int(row["finished_at"] or 0))})


def _credential_view(row, record):
    return {
        "id": row["credential_id"],
        "integration": row["integration"],
        "name": (record or {}).get("name") or "",
        "hint": engine.credential_hint(row["integration"],
                                       (record or {}).get("values")),
        "createdAt": int(row.get("created_at") or 0),
    }


async def _list_credentials(env, owner_bi):
    rows = await d1_all(
        env, "SELECT * FROM workflow_credentials WHERE owner_bi=? "
        "ORDER BY created_at DESC LIMIT ?", owner_bi, MAX_CREDENTIALS)
    items = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        items.append(_credential_view(row, record))
    return items


async def _create_credential(env, owner_bi, data, now):
    integration = str(data.get("integration") or "")
    if integration not in integrations.INTEGRATIONS:
        return _error("unknown_integration", 400)
    if not integrations.INTEGRATIONS[integration]["auth"]["fields"]:
        return _error("no_credential_needed", 400)
    values, error = engine.credential_values(integration, data.get("values"))
    if error:
        return _error(error, 400)
    name = engine._clean_text(data.get("name"), 60) or (
        integrations.INTEGRATIONS[integration]["name"] + " account")
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM workflow_credentials WHERE owner_bi=?",
        owner_bi)
    if int((count or {}).get("n") or 0) >= MAX_CREDENTIALS:
        return _error("too_many_credentials", 409)
    credential_id = _ap_uuid()
    record = {"name": name, "values": values}
    await d1_run(
        env, "INSERT INTO workflow_credentials (credential_id,owner_bi,"
        "integration,created_at,updated_at,data) VALUES (?,?,?,?,?,?)",
        credential_id, owner_bi, integration, now, now,
        await encrypt_row(env, record))
    row = {"credential_id": credential_id, "integration": integration,
           "created_at": now}
    return _ok({"credential": _credential_view(row, record)}, status=201)


async def _delete_credential(env, owner_bi, credential_id):
    row = await d1_first(
        env, "SELECT credential_id FROM workflow_credentials WHERE "
        "credential_id=? AND owner_bi=?", credential_id, owner_bi)
    if not row:
        return _error("not_found", 404)
    await d1_run(env, "DELETE FROM workflow_credentials WHERE credential_id=? "
                 "AND owner_bi=?", credential_id, owner_bi)
    return _ok({"deleted": credential_id})


async def _hook(env, request, workflow_id, token, now):
    method = method_name(request)
    if method not in ("GET", "POST", "PUT", "PATCH"):
        return _not_allowed("GET, POST, PUT, PATCH")
    row, record = await _load(env, workflow_id)
    if not row or not hmac.compare_digest(
            str(record.get("hookToken") or ""), token):
        return _error("not_found", 404)
    trigger = engine.trigger_node(record)
    if not row.get("active") or not trigger or \
            trigger["type"] != "core.webhook" or trigger.get("disabled"):
        return _error("workflow_inactive", 409)
    if not await _installed(env, row["owner_bi"], engine.PACKAGE):
        return _error("workflow_inactive", 409)
    if await _runs_last_hour(env, row["owner_bi"], now) >= RUNS_PER_HOUR:
        return _error("rate_limited", 429)
    body = {}
    if method != "GET":
        try:
            text = await bounded_request_text(request, HOOK_BODY_MAX_BYTES)
        except Exception:
            return _error("invalid_body", 400)
        text = str(text or "")
        try:
            body = json.loads(text) if text.strip() else {}
        except ValueError:
            body = {"text": text}
    try:
        query = dict(parse_qsl(urlparse(str(request.url)).query))
    except Exception:
        query = {}
    run = await _execute(env, row, record, "webhook",
                         {"method": method, "query": query, "body": body}, now)
    return _ok({"runId": run["id"], "status": run["status"],
                "error": run["error"]})


async def run_due_schedules(env):
    now = _now()
    rows = await d1_all(
        env, "SELECT * FROM workflows WHERE active=1 AND schedule_minutes>0 "
        "AND next_run_at>0 AND next_run_at<=? ORDER BY next_run_at LIMIT ?",
        now, SCHEDULED_PER_TICK)
    ran = 0
    for row in rows or []:
        minutes = max(engine.SCHEDULE_MIN_MINUTES,
                      int(row.get("schedule_minutes") or 0))
        await d1_run(env, "UPDATE workflows SET next_run_at=? "
                     "WHERE workflow_id=?", now + minutes * MINUTE_MS,
                     row["workflow_id"])
        record = await decrypt_row(env, row.get("data"))
        if not isinstance(record, dict):
            continue
        if not await _installed(env, row["owner_bi"], engine.PACKAGE):
            continue
        await _execute(env, row, record, "schedule",
                       {"timestamp": engine.iso_time(now)}, now)
        ran += 1
    return {"ran": ran}


def _files_route(action, data, now):
    try:
        if action == "parse":
            document, warnings = files.parse_file(
                str(data.get("path") or ""), str(data.get("text") or ""))
            return _ok({"document": document, "warnings": warnings})
        if action == "new":
            document, warnings = files.new_document(
                str(data.get("kind") or ""), str(data.get("path") or ""))
            return _ok({"document": document, "warnings": warnings})
        if action == "render":
            text, warnings = files.render_file(data.get("document"), now)
            return _ok({"text": text, "warnings": warnings})
    except files.FileError as error:
        return _error(str(error), 400)
    return _error("not_found", 404)


def _convert(document, source=""):
    try:
        workflow, warnings = formats.from_document(document)
    except formats.FormatError as error:
        return _error(str(error), 400)
    return _ok({"workflow": workflow, "warnings": warnings[:50],
                "source": source})


def _download_name(name):
    slug = re.sub(r"[^a-z0-9]+", "-", str(name or "").lower()).strip("-")
    return (slug[:60] or "workflow") + ".json"


async def _export(env, owner_bi, data):
    workflow, error = engine.clean_workflow(data)
    if error:
        return _error(error, 400)
    names = {}
    for item in await _list_credentials(env, owner_bi):
        names[item["id"]] = item["name"]
    return _ok({"document": formats.to_document(workflow, names),
                "filename": _download_name(workflow["name"])})


def import_source(url):
    raw = str(url or "").strip()
    match = GITHUB_BLOB_RE.match(raw)
    if match:
        return "https://raw.githubusercontent.com/%s/%s/%s" % match.groups()
    match = GIST_RE.match(raw)
    if match:
        return "https://gist.githubusercontent.com/%s/%s/raw" % match.groups()
    match = N8N_TEMPLATE_RE.match(raw)
    if match:
        return ("https://api.n8n.io/api/templates/workflows/"
                + match.group(1))
    return raw


async def _import_url(url):
    source = import_source(url)
    if not engine.safe_url(source):
        return _error("invalid_import_url", 400)
    try:
        response = await js_fetch_with_timeout(
            source, {"method": "GET", "redirect": "follow", "headers": {
                "User-Agent": instance_features.user_agent(USER_AGENT, "workflows"),
                "Accept": "application/json"}},
            IMPORT_TIMEOUT_SECONDS)
    except Exception:
        response = None
    status = int(getattr(response, "status", 0) or 0) if response else 0
    if not 200 <= status < 300:
        return _error("import_fetch_failed", 502, {"status": status})
    try:
        text = str(await response.text() or "")
    except Exception:
        return _error("import_fetch_failed", 502)
    if len(text.encode("utf-8")) > formats.MAX_DOCUMENT_BYTES:
        return _error("document_too_large", 400)
    return _convert(text, source)


async def api_handler(env, request, path):
    method = method_name(request)
    now = _now()
    hook = HOOK_RE.match(path)
    if hook:
        return await _hook(env, request, hook.group(1), hook.group(2), now)
    if path in (PREFIX + "/catalog", PREFIX + "/catalog/"):
        if method != "GET":
            return _not_allowed("GET")
        return json_response(dict(engine.catalog(), files=files.catalog(),
                                   requests=engine.request_specs(), ok=True),
                             cache_control="public, max-age=300")
    if path in (PREFIX + "/desktop-script", PREFIX + "/desktop-script/"):
        if method != "GET":
            return _not_allowed("GET")
        script = desktop.script_source()
        if not script:
            return _error("script_unavailable", 404)
        return json_response(dict(script, ok=True),
                             cache_control="public, max-age=300")
    if path in (PREFIX + "/templates", PREFIX + "/templates/"):
        if method != "GET":
            return _not_allowed("GET")
        return json_response({"ok": True,
                              "templates": templates.template_list()},
                             cache_control="public, max-age=300")
    match = TEMPLATE_RE.match(path)
    if match:
        item = templates.template(match.group(1))
        if not item:
            return _error("not_found", 404)
        return json_response({"ok": True, "template": item},
                             cache_control="public, max-age=300")
    data = None
    digest = ""
    if method == "POST":
        data, digest = await _body(request)
        if data is None:
            return _error("invalid_json", 400)
    elif method != "GET":
        return _not_allowed("GET, POST")
    owner_bi = await _session_account(env, request, data, digest)
    if not owner_bi:
        return _error("unauthorized", 401)
    if not await _installed(env, owner_bi, engine.PACKAGE):
        return _error("plugin_not_installed", 403,
                      {"plugin": engine.PACKAGE})
    if path.startswith(PREFIX + "/files/") or path.rstrip("/") == \
            PREFIX + "/package-files":
        if not await _installed(env, owner_bi, engine.FILES_PACKAGE):
            return _error("plugin_not_installed", 403,
                          {"plugin": engine.FILES_PACKAGE})
    if path.rstrip("/") == PREFIX + "/package-files":
        if method != "GET":
            return _not_allowed("GET")
        return _ok({"files": await _package_files(env, owner_bi)})
    if path in (PREFIX, PREFIX + "/"):
        if method == "GET":
            return _ok({"workflows": await _list(env, owner_bi, now)})
        return await _create(env, owner_bi, data, now)
    if path.startswith(PREFIX + "/files/"):
        if method != "POST":
            return _not_allowed("POST")
        return _files_route(path[len(PREFIX + "/files/"):].strip("/"), data,
                            now)
    if path in (PREFIX + "/convert", PREFIX + "/convert/"):
        if method != "POST":
            return _not_allowed("POST")
        return _convert(data.get("document"))
    if path in (PREFIX + "/export", PREFIX + "/export/"):
        if method != "POST":
            return _not_allowed("POST")
        return await _export(env, owner_bi, data)
    if path in (PREFIX + "/import-url", PREFIX + "/import-url/"):
        if method != "POST":
            return _not_allowed("POST")
        return await _import_url(data.get("url"))
    if path in (PREFIX + "/credentials", PREFIX + "/credentials/"):
        if method == "GET":
            return _ok({"credentials": await _list_credentials(env, owner_bi)})
        return await _create_credential(env, owner_bi, data, now)
    match = CREDENTIAL_RE.match(path)
    if match:
        if method != "POST":
            return _not_allowed("POST")
        return await _delete_credential(env, owner_bi, match.group(1))
    match = RUN_RE.match(path)
    if match:
        if method != "GET":
            return _not_allowed("GET")
        return await _run_detail(env, owner_bi, match.group(1), match.group(2))
    match = WORKFLOW_RE.match(path)
    if not match:
        return _error("not_found", 404)
    workflow_id, action = match.group(1), match.group(2)
    if not action:
        if method == "GET":
            row, record = await _load(env, workflow_id, owner_bi)
            if not row:
                return _error("not_found", 404)
            return _ok({"workflow": _detail(row, record)})
        return await _update(env, owner_bi, workflow_id, data, now)
    if action == "runs":
        if method != "GET":
            return _not_allowed("GET")
        return await _runs(env, owner_bi, workflow_id)
    if method != "POST":
        return _not_allowed("POST")
    if action == "run":
        return await _run_manual(env, owner_bi, workflow_id, data, now)
    if action == "record":
        return await _record_run(env, owner_bi, workflow_id, data, now)
    return await _delete(env, owner_bi, workflow_id)
