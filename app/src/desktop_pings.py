import re
import unicodedata


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


DESKTOP_PING_PROOF = "forkmesh-account-desktop-ping-v1"
DESKTOP_PING_DELETE_PROOF = "forkmesh-account-desktop-ping-delete-v1"
DESKTOP_PING_DELETE_MAX = 50
DESKTOP_PING_KIND = "desktop_ping"
DESKTOP_PING_PATH_RE = re.compile(r"^/api/notifications/desktop/?$")
DESKTOP_PING_ID_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,64}$")
DESKTOP_PING_NODE_RE = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
DESKTOP_PING_DESKTOP_KIND_RE = re.compile(r"^[A-Za-z0-9_-]{0,40}$")
DESKTOP_PING_SESSION_RE = re.compile(r"^[0-9]{0,12}$")
DESKTOP_PING_RUN_RE = re.compile(r"^(?:[1-9][0-9]{0,11})?$")
DESKTOP_PING_REPO_RE = re.compile(
    r"^$|^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
DESKTOP_PING_CATEGORIES = frozenset(
    {"", "agent-finished", "agent-failed", "ci-failed"})
DESKTOP_PING_ALARMS = {"power-battery": "power"}
DESKTOP_PING_TITLE_MAX = 160
DESKTOP_PING_BODY_MAX = 500
AGENT_APPROVAL_PROOF = "forkmesh-account-agent-approval-ping-v1"
AGENT_APPROVAL_KIND = "agent_approval"
AGENT_APPROVAL_CATEGORY = "agent-approval"
AGENT_APPROVAL_PATH_RE = re.compile(r"^/api/notifications/agent-approval/?$")
AGENT_APPROVAL_ID_RE = re.compile(r"^[0-9a-f]{32}$")
AGENT_APPROVAL_SESSION_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,64}$")
AGENT_APPROVAL_AGENT_MAX = 80
AGENT_APPROVAL_SUMMARY_MAX = 200
AGENT_APPROVAL_DEFAULT_AGENT = "An agent"
AGENT_APPROVAL_DEFAULT_SUMMARY = "Wants your approval"
AGENT_APPROVAL_LINE_BREAKERS = frozenset({"Cc", "Cf", "Zl", "Zp"})


def desktop_ping_resource(fields):
    run = [fields["runId"]] if fields.get("runId") else []
    return "\n".join([
        fields["pingId"], fields["node"], fields["kind"], fields["category"],
        fields["sessionId"], *run, fields["repo"],
        "1" if fields["warning"] else "0",
        fields["title"], fields["body"],
    ])


def desktop_ping_canonical(user, ts, fields):
    return "\n".join([
        DESKTOP_PING_PROOF, user, desktop_ping_resource(fields), str(ts),
    ]).encode()


def desktop_ping_delete_canonical(user, ts, fields):
    return "\n".join([
        DESKTOP_PING_DELETE_PROOF, user, fields["node"], *fields["pingIds"],
        str(ts),
    ]).encode()


def desktop_ping_delete_fields(data):
    if not isinstance(data, dict):
        return None
    node = data.get("node", "")
    ping_ids = data.get("pingIds")
    if not isinstance(node, str) or not DESKTOP_PING_NODE_RE.match(node):
        return None
    if (not isinstance(ping_ids, list) or not ping_ids
            or len(ping_ids) > DESKTOP_PING_DELETE_MAX):
        return None
    for ping_id in ping_ids:
        if not isinstance(ping_id, str) or not DESKTOP_PING_ID_RE.match(ping_id):
            return None
    return {"node": node, "pingIds": list(ping_ids)}


def _text(data, key, limit):
    value = data.get(key, "")
    if not isinstance(value, str) or len(value) > limit:
        return None
    return value


def desktop_ping_fields(data):
    if not isinstance(data, dict):
        return None
    fields = {}
    for key, pattern in (("pingId", DESKTOP_PING_ID_RE),
                         ("node", DESKTOP_PING_NODE_RE),
                         ("kind", DESKTOP_PING_DESKTOP_KIND_RE),
                         ("sessionId", DESKTOP_PING_SESSION_RE),
                         ("runId", DESKTOP_PING_RUN_RE),
                         ("repo", DESKTOP_PING_REPO_RE)):
        value = data.get(key, "")
        if not isinstance(value, str) or not pattern.match(value):
            return None
        fields[key] = value
    category = data.get("category", "")
    if category not in DESKTOP_PING_CATEGORIES:
        return None
    fields["category"] = category
    warning = data.get("warning", False)
    if not isinstance(warning, bool):
        return None
    fields["warning"] = warning
    title = _text(data, "title", DESKTOP_PING_TITLE_MAX)
    body = _text(data, "body", DESKTOP_PING_BODY_MAX)
    if title is None or body is None:
        return None
    if not title.strip() and not body.strip():
        return None
    fields["title"] = title
    fields["body"] = body
    return fields


def agent_approval_resource(fields):
    return "\n".join([
        fields["sessionId"], fields["approvalId"], fields["agent"],
        fields["summary"],
    ])


def agent_approval_canonical(user, ts, fields):
    return "\n".join([
        AGENT_APPROVAL_PROOF, user, agent_approval_resource(fields), str(ts),
    ]).encode()


def _line(data, key, limit):
    value = _text(data, key, limit)
    if value is None or any(
            unicodedata.category(c) in AGENT_APPROVAL_LINE_BREAKERS
            for c in value):
        return None
    return value


def agent_approval_fields(data):
    if not isinstance(data, dict):
        return None
    session = data.get("sessionId", "")
    approval = data.get("approvalId", "")
    if (not isinstance(session, str)
            or not AGENT_APPROVAL_SESSION_RE.match(session)):
        return None
    if (not isinstance(approval, str)
            or not AGENT_APPROVAL_ID_RE.match(approval)):
        return None
    agent = _line(data, "agent", AGENT_APPROVAL_AGENT_MAX)
    summary = _line(data, "summary", AGENT_APPROVAL_SUMMARY_MAX)
    if agent is None or summary is None:
        return None
    return {"sessionId": session, "approvalId": approval, "agent": agent,
            "summary": summary}


async def _desktop_ping_signed_account(env, request, canonical_for,
                                       path_re=DESKTOP_PING_PATH_RE):
    url = urlparse(request.url)
    if not path_re.match(url.path):
        return ""
    params = parse_qs(url.query)
    user = clean_string(params.get("user", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    if not valid_node_name(user) or not sig or not _ts_ok(ts):
        return ""
    canonical = canonical_for(user, ts)
    if not await _verify_owner_signature(env, user, sig, canonical):
        return ""
    _, record = await _account_row(env, user)
    if not record or record.get("status") != "active":
        return ""
    return user


async def _desktop_ping_delete(env, request):
    no_store = "no-store"
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    fields = desktop_ping_delete_fields(data)
    if fields is None:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control=no_store)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _desktop_ping_signed_account(
            env, request,
            lambda user, ts: desktop_ping_delete_canonical(user, ts, fields))
    if not valid_node_name(account):
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control=no_store)
    account = account.lower()
    node = fields["node"].lower()
    recipient_bi = await blind_index(env, account)
    dedupe_bis = []
    for ping_id in dict.fromkeys(fields["pingIds"]):
        dedupe_bis.append(await blind_index(
            env, "notification:" + account + ":desktop:" + node + ":" + ping_id))
    await d1_run(
        env,
        "DELETE FROM notifications WHERE recipient_bi=? AND dedupe_bi IN ("
        + ",".join("?" for _ in dedupe_bis) + ")",
        recipient_bi, *dedupe_bis)
    await notify_account_event(env, account, "pings")
    return json_response({"ok": True}, cache_control=no_store)


async def desktop_ping_handler(env, request):
    await ensure_schema(env)
    no_store = "no-store"
    method = method_name(request)
    if method == "DELETE":
        return await _desktop_ping_delete(env, request)
    if method != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST, DELETE"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    fields = desktop_ping_fields(data)
    if fields is None:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control=no_store)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _desktop_ping_signed_account(
            env, request,
            lambda user, ts: desktop_ping_canonical(user, ts, fields))
    if not valid_node_name(account):
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control=no_store)

    title = fields["title"].strip()
    body = fields["body"].strip()
    if not title:
        title, body = body, ""
    node = fields["node"].lower()
    repo = fields["repo"].lower()
    href = ""
    if repo and fields["sessionId"]:
        href = "/" + repo + "/agents/" + fields["sessionId"]
    elif repo and fields["runId"]:
        href = "/" + repo + "/actions/runs/" + fields["runId"]
    meta = {"node": node, "desktopKind": fields["kind"],
            "warning": fields["warning"], "pingId": fields["pingId"]}
    if fields["category"]:
        meta["category"] = fields["category"]
    if fields["sessionId"]:
        meta["sessionId"] = fields["sessionId"]
    if fields["runId"]:
        meta["runId"] = fields["runId"]
    alarm = DESKTOP_PING_ALARMS.get(fields["kind"], "")
    if alarm:
        meta["alarm"] = alarm
    await enqueue_notification(
        env, account, DESKTOP_PING_KIND, title, body=body,
        repo=repo, href=href, source="desktop",
        dedupe="desktop:" + node + ":" + fields["pingId"],
        ts=int(Date.now()), meta=meta, force=bool(alarm))
    return json_response({"ok": True}, cache_control=no_store)


async def agent_approval_ping_handler(env, request):
    await ensure_schema(env)
    no_store = "no-store"
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    fields = agent_approval_fields(data)
    if fields is None:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control=no_store)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _desktop_ping_signed_account(
            env, request,
            lambda user, ts: agent_approval_canonical(user, ts, fields),
            AGENT_APPROVAL_PATH_RE)
    if not valid_node_name(account):
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control=no_store)

    agent = fields["agent"].strip() or AGENT_APPROVAL_DEFAULT_AGENT
    summary = fields["summary"].strip() or AGENT_APPROVAL_DEFAULT_SUMMARY
    meta = {"category": AGENT_APPROVAL_CATEGORY,
            "sessionId": fields["sessionId"],
            "approvalId": fields["approvalId"], "agent": agent}
    await enqueue_notification(
        env, account.lower(), AGENT_APPROVAL_KIND,
        agent + " needs your approval", body=summary, source="desktop",
        dedupe="agent-approval:" + fields["approvalId"],
        ts=int(Date.now()), meta=meta)
    return json_response({"ok": True}, cache_control=no_store)
