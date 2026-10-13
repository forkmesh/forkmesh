import hashlib
import json
import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


OPERATOR_ALERT_PROOF = "forkmesh-admin-operator-alert-v1"
OPERATOR_ALERT_PATH = "/api/accounts/admin-operator-alert"
OPERATOR_ALERT_KIND = "operational_alert"
OPERATOR_ALERT_SEVERITIES = ("ping", "email")
OPERATOR_ALERT_KEY_RE = re.compile(r"^[A-Za-z0-9:._-]{1,80}$")
OPERATOR_ALERT_TITLE_MAX = 120
OPERATOR_ALERT_BODY_MAX = 2000
OPERATOR_ALERT_ADMIN_LIMIT = 50


def operator_alert_canonical(node, ts, severity, key, title, body):
    digest = hashlib.sha256(json.dumps(
        [severity, key, title, body], separators=(",", ":")).encode()).hexdigest()
    return "\n".join([OPERATOR_ALERT_PROOF, node, str(ts), digest]).encode()


def _operator_alert_fields(data):
    if not isinstance(data, dict):
        return None
    severity = data.get("severity")
    key = data.get("key")
    title = data.get("title")
    body = data.get("body", "")
    if severity not in OPERATOR_ALERT_SEVERITIES:
        return None
    if not isinstance(key, str) or not OPERATOR_ALERT_KEY_RE.match(key):
        return None
    if not isinstance(title, str) or not title.strip() \
            or len(title) > OPERATOR_ALERT_TITLE_MAX:
        return None
    if not isinstance(body, str) or len(body) > OPERATOR_ALERT_BODY_MAX:
        return None
    return severity, key, title, body


async def _operator_alert_admins(env):
    rows = await d1_all(
        env, "SELECT data FROM users WHERE is_admin=1 ORDER BY user_bi LIMIT ?",
        OPERATOR_ALERT_ADMIN_LIMIT)
    names = []
    for row in rows or []:
        try:
            record = await decrypt_row(env, row.get("data", "")) or {}
        except Exception:
            continue
        if record.get("service_account") is True:
            continue
        name = clean_string(record.get("name", ""), MAX_NODE_NAME).strip().lower()
        if valid_node_name(name) and name not in names:
            names.append(name)
    return names


async def operator_alert_handler(env, request):
    no_store = "no-store"
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST"})
    await ensure_schema(env)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    fields = _operator_alert_fields(data)
    if fields is None:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control=no_store)
    params = parse_qs(urlparse(request.url).query)
    node = clean_string(params.get("node", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    severity, key, title, body = fields
    canonical = operator_alert_canonical(node, ts, severity, key, title, body)
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control=no_store)
    now = int(Date.now())
    title = clean_string(title, OPERATOR_ALERT_TITLE_MAX).strip()
    body = clean_string(body, OPERATOR_ALERT_BODY_MAX).strip()
    pings = 0
    for admin in await _operator_alert_admins(env):
        if await enqueue_notification(
                env, admin, OPERATOR_ALERT_KIND, title, body=body,
                href="/status", source="operator", dedupe="operator:" + key,
                ts=now, meta={"key": key, "from": node,
                              "severity": severity}):
            pings += 1
    emails = 0
    if severity == "email":
        for address in await _repository_monitor_admin_emails(env) or []:
            if await _send_email(env, address, "ForkMesh: " + title, body,
                                 email_kind="operational"):
                emails += 1
    return json_response(
        {"ok": True, "pings": pings, "emails": emails},
        cache_control=no_store)
