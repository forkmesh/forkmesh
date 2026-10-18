import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


SECRET_NAME_RE = re.compile(r"^[A-Z_][A-Z0-9_]{0,63}$")
VALUE_MAX_BYTES = 48 * 1024
BODY_MAX_BYTES = 64 * 1024
SECRET_LIMIT = 100
ADMIN_ROLES = ("owner", "admin")


def _error(error, status, message=""):
    body = {"error": error}
    if message:
        body["message"] = message
    return json_response(body, status=status, cache_control="no-store")


async def _actor(env, request, data=None):
    _, record = await _account_session_record(env, request, data or {})
    return clean_string(
        (record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()


async def _repo_allowed(env, actor, owner, repo):
    return actor == owner or await _repo_admin_allowed(env, actor, owner, repo)


async def _org_allowed(env, actor, org):
    org_bi, record = await _org_row(env, org)
    if not record:
        return False
    return await _org_role(env, org_bi, actor) in ADMIN_ROLES


async def _list(env, owner, repo):
    rows = await d1_all(
        env, "SELECT name,updated_at,updated_by FROM repo_action_secrets "
        "WHERE owner=? AND repo=? ORDER BY name LIMIT %d" % SECRET_LIMIT,
        owner, repo)
    return json_response({"ok": True, "secrets": [
        {"name": str(row.get("name") or ""),
         "updatedAt": int(row.get("updated_at") or 0),
         "updatedBy": str(row.get("updated_by") or "")}
        for row in rows or []]}, cache_control="no-store")


async def _handle(env, request, owner, repo, name, allowed, target_type,
                  target):
    method = method_name(request)
    if method not in ("GET", "PUT", "DELETE"):
        return _error("method_not_allowed", 405)
    if (method == "GET") == bool(name):
        return _error("not_found", 404)
    if name and not SECRET_NAME_RE.fullmatch(name):
        return _error("invalid_name", 400,
                      "Names use A-Z, 0-9 and _, up to 64 characters.")
    data = {}
    if method == "PUT":
        try:
            data = await bounded_json_request(request, BODY_MAX_BYTES)
        except Exception:
            return _error("invalid_json", 400)
        if not isinstance(data, dict):
            return _error("invalid_json", 400)
    actor = await _actor(env, request, data)
    if not actor:
        return _error("invalid_session", 401)
    await ensure_schema(env)
    action = {"GET": "list", "PUT": "set", "DELETE": "delete"}[method]
    try:
        permitted = await allowed(env, actor)
    except Exception:
        permitted = False
    if not permitted:
        await _audit_sensitive_action(
            env, actor, "actions.secret_" + action, target_type, target,
            "denied", {"name": name} if name else {})
        return _error("forbidden", 403)
    if method == "GET":
        return await _list(env, owner, repo)
    now = int(Date.now())
    if method == "DELETE":
        await d1_run(
            env, "DELETE FROM repo_action_secrets WHERE owner=? AND repo=? "
            "AND name=?", owner, repo, name)
    else:
        value = data.get("value")
        if (not isinstance(value, str) or not value
                or len(value.encode("utf-8")) > VALUE_MAX_BYTES):
            return _error("invalid_value", 400,
                          "A secret value is 1 byte to 48 KiB of text.")
        existing = await d1_first(
            env, "SELECT COUNT(*) AS c FROM repo_action_secrets WHERE "
            "owner=? AND repo=? AND name!=?", owner, repo, name)
        if int((existing or {}).get("c") or 0) >= SECRET_LIMIT:
            return _error("too_many_secrets", 409)
        await d1_run(
            env, "INSERT INTO repo_action_secrets (owner,repo,name,value,"
            "updated_by,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(owner,"
            "repo,name) DO UPDATE SET value=excluded.value,"
            "updated_by=excluded.updated_by,updated_at=excluded.updated_at",
            owner, repo, name, await encrypt_row(env, {"value": value}),
            actor, now)
    await _audit_sensitive_action(
        env, actor, "actions.secret_" + action, target_type, target,
        "success", {"name": name})
    return json_response({"ok": True, "name": name, "updatedAt": now},
                         cache_control="no-store")


async def repo_handler(env, request, owner, repo, name):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()

    async def allowed(env_, actor):
        return await _repo_allowed(env_, actor, owner, repo)

    return await _handle(env, request, owner, repo, str(name or ""), allowed,
                         "repository", owner + "/" + repo)


async def org_handler(env, request, org, name):
    org = clean_string(org, MAX_NODE_NAME).strip().lower()
    if not valid_node_name(org):
        return _error("not_found", 404)

    async def allowed(env_, actor):
        return await _org_allowed(env_, actor, org)

    return await _handle(env, request, org, "", str(name or ""), allowed,
                         "organization", org)
