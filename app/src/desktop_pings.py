import re


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


async def _desktop_ping_signed_account(env, request, canonical_for):
    url = urlparse(request.url)
    if not DESKTOP_PING_PATH_RE.match(url.path):
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
