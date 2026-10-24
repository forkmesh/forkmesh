import json
import re
from urllib.parse import parse_qs, urlparse

import account_policy

INSTANCE_SCOPE = "moderation-scope:instance"
SUBJECT_KINDS = ("issue", "pull", "discussion")
TARGET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@+-]{0,159}$")
HIDDEN_TEXT = "*This post was hidden by the moderators.*"
POSTING_EVENTS = frozenset({"open", "comment", "vote", "claim", "reaction"})
HELD_EVENTS = frozenset({"open", "comment"})
MODERATOR_ISSUE_EVENTS = frozenset({
    "edit", "title", "status", "labels", "milestone", "dates", "priority",
    "progress", "assignees", "delete",
})
MODERATOR_DISCUSSION_EVENTS = frozenset({"status", "delete"})
MAX_REASON = 280
MAX_LIST = 500
MAX_QUEUE_PER_SCOPE = 2000
MAX_QUEUE_PER_AUTHOR = 20
MAX_LOG = 200
MAX_NUMBER = 1_000_000_000
CLAIM_MS = 5 * 60 * 1000
PRIVATE_CACHE = "private, no-store"
PUBLIC_CACHE = "public, max-age=5, stale-while-revalidate=30"
VARY = {"vary": "authorization, cookie"}
ORG_ACTIONS = frozenset({
    "add_moderator", "remove_moderator", "block", "unblock", "approve",
    "reject", "trust",
})
REPO_ACTIONS = frozenset({"hide", "unhide", "lock", "unlock"})
MESSAGES = {
    "account_required": "Sign in to post here.",
    "account_blocked": "A moderator has blocked this account from posting here.",
    "thread_locked": "This thread is locked. Only moderators can reply.",
    "new_account_limit": "New accounts can only post a few times a day. Try again tomorrow.",
    "review_queue_full": "Too many of your posts are already waiting for review.",
}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _now():
    return int(Date.now())  # noqa: F821


def clean_kind(value):
    kind = str(value or "").strip().lower()
    return kind if kind in SUBJECT_KINDS else ""


def clean_number(value):
    try:
        number = int(str(value if value is not None else "").strip())
    except (TypeError, ValueError):
        return 0
    return number if 0 < number <= MAX_NUMBER else 0


def clean_target(value):
    target = value if isinstance(value, str) else ""
    return target if TARGET_RE.fullmatch(target) else ""


def clean_name(value):
    name = str(value or "").strip().lower().lstrip("@")
    return name if valid_node_name(name) else ""  # noqa: F821


def clean_reason(value):
    return clean_string(value or "", MAX_REASON).strip()  # noqa: F821


def subject_key(kind, number):
    return "%s:%d" % (kind, number)


def refusal(code, status):
    return json_response(  # noqa: F821
        {"error": code, "message": MESSAGES.get(code, "")}, status=status,
        cache_control=PRIVATE_CACHE)


def apply_to_record(record, hidden=(), hidden_authors=()):
    if not isinstance(record, dict):
        return record
    events = record.get("events")
    if not isinstance(events, list):
        return record
    hidden = {str(item) for item in hidden or ()}
    authors = {str(name).lower() for name in hidden_authors or ()}
    if not hidden and not authors:
        return record
    opened = next((event for event in events if isinstance(event, dict)
                   and event.get("type") == "open"), {})
    open_id = str(opened.get("id") or "")
    covered = set()
    for event in events:
        if not isinstance(event, dict) or event.get("type") not in (
                "open", "comment"):
            continue
        event_id = str(event.get("id") or "")
        author = str(event.get("authorName") or "").lower()
        if (event_id and event_id in hidden) or (
                event.get("type") == "open" and "body" in hidden) or (
                author and author in authors):
            covered.add(event_id)
    if "body" in hidden and open_id:
        covered.add(open_id)
    if not covered:
        return record
    out = dict(record)
    patched = []
    for event in events:
        if not isinstance(event, dict):
            patched.append(event)
            continue
        kind = event.get("type")
        event_id = str(event.get("id") or "")
        target = str(event.get("target") or "")
        if (kind in ("open", "comment") and event_id in covered) or (
                kind == "edit" and (target in covered or (
                    not target and open_id in covered))):
            event = dict(event, body=HIDDEN_TEXT, attachments=[],
                         moderationHidden=True)
        patched.append(event)
    out["events"] = patched
    if open_id in covered and "body" in out:
        out["body"] = HIDDEN_TEXT
    return out


def apply_to_text(text, hidden=(), hidden_authors=()):
    if not hidden and not hidden_authors:
        return text
    try:
        record = json.loads(text)
    except (TypeError, ValueError):
        return text
    if not isinstance(record, dict):
        return text
    return json.dumps(apply_to_record(record, hidden, hidden_authors))


async def policy(env):
    now = _now()
    remembered = account_policy.cached(now)
    if remembered is not None:
        return remembered
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?",
            account_policy.SETTING_KEY)
    except Exception:
        stale = account_policy._cache.get("value")
        return dict(stale) if stale else dict(account_policy.DEFAULTS)
    setting = account_policy.parse((row or {}).get("doc") or "")
    account_policy.remember(setting, now)
    return setting


async def save_policy(env, setting):
    doc = account_policy.dump(setting)
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at",
        account_policy.SETTING_KEY, doc, _now())
    account_policy.remember(account_policy.parse(doc), _now())


async def instance_scope(env):
    return await blind_index(env, INSTANCE_SCOPE)  # noqa: F821


async def org_scope(env, org):
    org_bi, row = await _org_row(env, org)  # noqa: F821
    if not row:
        return "", ""
    return org_bi, str(row.get("name") or org).lower()


async def repo_orgs(env, owner, repo):
    found = {}
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip().lower()
    if not owner or not repo:
        return found
    try:
        org_bi, name = await org_scope(env, owner)
        if org_bi:
            found[org_bi] = name
        rows = await d1_all(  # noqa: F821
            env, "SELECT org_bi FROM org_repos WHERE node_owner=? AND repo=?",
            owner, repo)
        for row in rows or []:
            bi = str(row.get("org_bi") or "")
            if not bi or bi in found:
                continue
            named = await d1_first(  # noqa: F821
                env, "SELECT name FROM orgs WHERE org_bi=?", bi)
            found[bi] = str((named or {}).get("name") or "").lower()
    except Exception:
        return found
    return found


def scope_text(orgs, scope_bi):
    name = (orgs or {}).get(scope_bi)
    return "org:" + name if name else INSTANCE_SCOPE


def _placeholders(values):
    return ",".join("?" for _ in values)


async def _scopes(env, orgs):
    return list(orgs or {}) + [await instance_scope(env)]


async def _moderator_row(env, account_bi, scopes):
    if not account_bi or not scopes:
        return None
    return await d1_first(  # noqa: F821
        env, "SELECT scope_bi FROM moderation_roles WHERE account_bi=? "
        "AND scope_bi IN (" + _placeholders(scopes) + ") LIMIT 1",
        account_bi, *scopes)


async def _org_admin(env, orgs, account):
    for org_bi in orgs or {}:
        role = await _org_role(env, org_bi, account)  # noqa: F821
        if role in ("owner", "admin"):
            return True
    return False


async def can_moderate_repo(env, account, owner, repo, orgs=None):
    account = clean_name(account)
    if not account:
        return False
    try:
        if await _is_admin(env, account):  # noqa: F821
            return True
        if account == str(owner or "").lower() or await _account_owns_node(  # noqa: F821
                env, account, owner):
            return True
        if orgs is None:
            orgs = await repo_orgs(env, owner, repo)
        if await _org_admin(env, orgs, account):
            return True
        account_bi = await blind_index(env, account)  # noqa: F821
        return bool(await _moderator_row(
            env, account_bi, await _scopes(env, orgs)))
    except Exception:
        return False


async def can_moderate_org(env, account, org_bi):
    account = clean_name(account)
    if not account or not org_bi:
        return False
    if await _is_admin(env, account):  # noqa: F821
        return True
    if await _org_admin(env, {org_bi: ""}, account):
        return True
    account_bi = await blind_index(env, account)  # noqa: F821
    return bool(await _moderator_row(
        env, account_bi, [org_bi, await instance_scope(env)]))


async def can_manage_moderators(env, account, org_bi):
    account = clean_name(account)
    if not account:
        return False
    if await _is_admin(env, account):  # noqa: F821
        return True
    return await _org_admin(env, {org_bi: ""}, account)


async def block_row(env, account_bi, scopes):
    if not account_bi or not scopes:
        return None
    try:
        return await d1_first(  # noqa: F821
            env, "SELECT scope_bi, hide_content FROM moderation_blocks "
            "WHERE account_bi=? AND scope_bi IN (" + _placeholders(scopes)
            + ") LIMIT 1", account_bi, *scopes)
    except Exception:
        return None


async def account_blocked(env, account, owner, repo):
    account = clean_name(account)
    if not account:
        return False
    orgs = await repo_orgs(env, owner, repo)
    if await can_moderate_repo(env, account, owner, repo, orgs):
        return False
    account_bi = await blind_index(env, account)  # noqa: F821
    return bool(await block_row(env, account_bi, await _scopes(env, orgs)))


async def blocked_response(env, account, owner, repo):
    if await account_blocked(env, account, owner, repo):
        return refusal("account_blocked", 403)
    return None


async def repo_key(env, owner, repo):
    return await blind_index(env, str(owner) + "/" + str(repo))  # noqa: F821


async def flags(env, repo_bi, subject):
    try:
        rows = await d1_all(  # noqa: F821
            env, "SELECT target, kind FROM moderation_flags WHERE repo_bi=? "
            "AND subject=? LIMIT ?", repo_bi, subject, MAX_LIST)
    except Exception:
        rows = []
    hidden = sorted({str(row.get("target") or "") for row in rows or []
                     if row.get("kind") == "hide" and row.get("target")})
    locked = any(row.get("kind") == "lock" for row in rows or [])
    return hidden, locked


async def hidden_authors(env, orgs, setting=None):
    setting = setting or await policy(env)
    if not setting.get("hideBlockedPosts"):
        return []
    scopes = await _scopes(env, orgs)
    try:
        rows = await d1_all(  # noqa: F821
            env, "SELECT data FROM moderation_blocks WHERE hide_content=1 "
            "AND scope_bi IN (" + _placeholders(scopes) + ") LIMIT ?",
            *scopes, MAX_LIST)
    except Exception:
        return []
    names = set()
    for row in rows or []:
        data = await decrypt_row(env, row.get("data")) or {}  # noqa: F821
        name = clean_name(data.get("name"))
        if name:
            names.add(name)
    return sorted(names)


async def log(env, scope_bi, scope_text, actor, action, target="",
              detail=None):
    entry = {"scope": scope_text, "actor": actor, "action": action,
             "target": target, "detail": detail or {}}
    try:
        await d1_run(  # noqa: F821
            env, "INSERT INTO moderation_log (scope_bi, ts, data) VALUES (?,?,?)",
            scope_bi, _now(), await encrypt_row(env, entry))  # noqa: F821
    except Exception:
        pass
    try:
        await _audit_sensitive_action(  # noqa: F821
            env, actor, "moderation." + action, "account" if target else "",
            target, "success", detail or {})
    except Exception:
        pass


async def _count_post(env, account_bi, kind, now):
    day = account_policy.day_number(now)
    try:
        await d1_run(  # noqa: F821
            env, "DELETE FROM moderation_post_counts WHERE account_bi=? "
            "AND day<?", account_bi, day)
        row = await d1_first(  # noqa: F821
            env, "INSERT INTO moderation_post_counts (account_bi, day, kind, "
            "count) VALUES (?,?,?,1) ON CONFLICT(account_bi, day, kind) DO "
            "UPDATE SET count=moderation_post_counts.count+1 RETURNING count",
            account_bi, day, kind)
    except Exception:
        return 0
    return int((row or {}).get("count") or 1)


async def _session_account(env, request, data):
    try:
        account_bi, rec = await _account_session_record(  # noqa: F821
            env, request, data, allow_unverified=True)
    except Exception:
        return "", None
    if not rec:
        return "", None
    return account_bi, rec


async def submission_gate(env, request, data, owner, repo, event,
                          surface="issue", actions_job=False):
    context = {"hold": False, "moderator": False, "account": "",
               "accountBi": "", "orgs": {}}
    if actions_job or not isinstance(event, dict):
        return None, context
    event_type = str(event.get("type") or "")
    if surface == "issue" and event_type not in POSTING_EVENTS:
        return None, context
    setting = await policy(env)
    account = clean_name(event.get("authorName"))
    if not account:
        if setting["accountRequired"]:
            return refusal("account_required", 401), context
        if surface == "issue" and event_type in HELD_EVENTS and \
                setting["firstPostApproval"]:
            context["hold"] = True
        return None, context
    orgs = await repo_orgs(env, owner, repo)
    context["orgs"] = orgs
    context["account"] = account
    if await can_moderate_repo(env, account, owner, repo, orgs):
        context["moderator"] = True
        return None, context
    account_bi = await blind_index(env, account)  # noqa: F821
    context["accountBi"] = account_bi
    if await block_row(env, account_bi, await _scopes(env, orgs)):
        return refusal("account_blocked", 403), context
    if surface != "issue" or event_type not in HELD_EVENTS:
        return None, context
    number = clean_number(data.get("number") if isinstance(data, dict) else 0)
    if event_type == "comment" and number:
        _hidden, locked = await flags(
            env, await repo_key(env, owner, repo),
            subject_key("issue", number))
        if locked:
            return refusal("thread_locked", 403), context
    session_bi, rec = await _session_account(env, request, data)
    if not rec or session_bi != account_bi:
        _bi, rec = await _account_row(env, account)  # noqa: F821
    now = _now()
    if account_policy.is_new_account(setting, rec, now):
        kind = "thread" if event_type == "open" else "comment"
        limit = account_policy.daily_limit(setting, kind)
        if limit and await _count_post(env, account_bi, kind, now) > limit:
            return refusal("new_account_limit", 429), context
    if account_policy.needs_approval(setting, rec):
        context["hold"] = True
    return None, context


async def hold(env, owner, repo, repo_bi, item, context, public_owner=""):
    scopes = list(context.get("orgs") or {})
    scope_bi = scopes[0] if scopes else await instance_scope(env)
    author_text = context.get("account") or "anonymous:" + str(
        (item.get("event") or {}).get("author") or "")
    author_bi = context.get("accountBi") or await blind_index(  # noqa: F821
        env, author_text)
    counts = await d1_first(  # noqa: F821
        env, "SELECT COUNT(*) AS n FROM moderation_queue WHERE scope_bi=?",
        scope_bi)
    if int((counts or {}).get("n") or 0) >= MAX_QUEUE_PER_SCOPE:
        return refusal("review_queue_full", 429)
    mine = await d1_first(  # noqa: F821
        env, "SELECT COUNT(*) AS n FROM moderation_queue WHERE author_bi=?",
        author_bi)
    if int((mine or {}).get("n") or 0) >= MAX_QUEUE_PER_AUTHOR:
        return refusal("review_queue_full", 429)
    event = item.get("event") or {}
    number = int(item.get("number") or 0)
    held = {"owner": owner, "repo": repo, "publicOwner": public_owner or owner,
            "repoKey": str(owner) + "/" + str(repo),
            "scope": scope_text(context.get("orgs"), scope_bi),
            "author": author_text, "account": context.get("account") or "",
            "item": item}
    subject = subject_key("issue", number) if number else "issue:new"
    await d1_run(  # noqa: F821
        env, "INSERT INTO moderation_queue (repo_bi, scope_bi, author_bi, "
        "subject, kind, created_at, data) VALUES (?,?,?,?,?,?,?)",
        repo_bi, scope_bi, author_bi, subject,
        "thread" if event.get("type") == "open" else "comment", _now(),
        await encrypt_row(env, held))  # noqa: F821
    return json_response(  # noqa: F821
        {"ok": True, "pending": "review", "number": number,
         "message": "Sent to the moderators. It appears once a moderator "
         "approves it."}, status=202, cache_control=PRIVATE_CACHE)


async def pending_mine(env, account, repo_bi, number):
    account = clean_name(account)
    if not account:
        return 0
    account_bi = await blind_index(env, account)  # noqa: F821
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT COUNT(*) AS n FROM moderation_queue WHERE "
            "author_bi=? AND repo_bi=? AND subject IN (?, 'issue:new')",
            account_bi, repo_bi, subject_key("issue", number))
    except Exception:
        return 0
    return int((row or {}).get("n") or 0)


async def _viewer(env, request, data=None):
    try:
        _bi, rec = await _account_session_record(env, request, data)  # noqa: F821
    except Exception:
        return ""
    return clean_name((rec or {}).get("name"))


async def repo_handler(env, request, owner, repo):
    method = method_name(request)  # noqa: F821
    if method == "GET":
        return await _repo_read(env, request, owner, repo)
    if method == "POST":
        return await _repo_write(env, request, owner, repo)
    return json_response({"error": "method_not_allowed"}, status=405)  # noqa: F821


async def _repo_read(env, request, owner, repo):
    query = parse_qs(urlparse(str(request.url)).query)
    kind = clean_kind((query.get("kind") or ["issue"])[0])
    number = clean_number((query.get("number") or [""])[0])
    if not kind or not number:
        return json_response({"error": "invalid_target"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    access = await _repository_access_context(env, request, owner, repo)  # noqa: F821
    if access is None:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    viewer = clean_name(access.get("viewer"))
    repo_bi = await repo_key(env, owner, repo)
    orgs = await repo_orgs(env, owner, repo)
    hidden, locked = await flags(env, repo_bi, subject_key(kind, number))
    authors = await hidden_authors(env, orgs)
    payload = {"ok": True, "kind": kind, "number": number, "hidden": hidden,
               "hiddenAuthors": authors, "locked": locked,
               "placeholder": HIDDEN_TEXT, "signedIn": bool(viewer),
               "canModerate": False, "pendingMine": 0}
    if viewer:
        payload["canModerate"] = await can_moderate_repo(
            env, viewer, owner, repo, orgs)
        if kind == "issue":
            payload["pendingMine"] = await pending_mine(
                env, viewer, repo_bi, number)
    shared = not viewer and access.get("visibility") == "public"
    return json_response(  # noqa: F821
        payload, cache_control=PUBLIC_CACHE if shared else PRIVATE_CACHE,
        extra_headers=VARY)


async def _repo_write(env, request, owner, repo):
    try:
        data = await bounded_json_request(request)  # noqa: F821
    except Exception:
        return json_response({"error": "invalid_json"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    actor = await _viewer(env, request, data)
    if not actor:
        return json_response({"error": "unauthorized"}, status=401,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    action = str(data.get("action") or "")
    kind = clean_kind(data.get("kind") or "issue")
    number = clean_number(data.get("number"))
    if action not in REPO_ACTIONS or not kind or not number:
        return json_response({"error": "invalid_action"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    target = "thread" if action in ("lock", "unlock") else clean_target(
        data.get("target"))
    if not target:
        return json_response({"error": "invalid_target"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    access = await _repository_access_context(  # noqa: F821
        env, request, owner, repo, viewer=actor)
    if access is None:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    orgs = await repo_orgs(env, owner, repo)
    if not await can_moderate_repo(env, actor, owner, repo, orgs):
        return json_response({"error": "forbidden"}, status=403,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    repo_bi = await repo_key(env, owner, repo)
    subject = subject_key(kind, number)
    flag = "lock" if action in ("lock", "unlock") else "hide"
    if action in ("hide", "lock"):
        sealed = await encrypt_row(env, {  # noqa: F821
            "repo": str(owner) + "/" + str(repo), "by": actor,
            "reason": clean_reason(data.get("reason"))})
        await d1_run(  # noqa: F821
            env, "INSERT INTO moderation_flags (repo_bi, subject, target, "
            "kind, created_at, data) VALUES (?,?,?,?,?,?) ON CONFLICT("
            "repo_bi, subject, target, kind) DO UPDATE SET "
            "data=excluded.data", repo_bi, subject, target, flag, _now(),
            sealed)
    else:
        await d1_run(  # noqa: F821
            env, "DELETE FROM moderation_flags WHERE repo_bi=? AND subject=? "
            "AND target=? AND kind=?", repo_bi, subject, target, flag)
    scopes = list(orgs)
    scope_bi = scopes[0] if scopes else await instance_scope(env)
    await log(env, scope_bi, scope_text(orgs, scope_bi), actor,
              action, "", {"repo": str(owner) + "/" + str(repo),
                           "subject": subject, "target": target})
    hidden, locked = await flags(env, repo_bi, subject)
    return json_response(  # noqa: F821
        {"ok": True, "hidden": hidden, "locked": locked},
        cache_control=PRIVATE_CACHE)


async def _rows(env, table, scope_bi):
    rows = await d1_all(  # noqa: F821
        env, "SELECT account_bi, created_at, data" + (
            ", hide_content" if table == "moderation_blocks" else "")
        + " FROM " + table + " WHERE scope_bi=? ORDER BY created_at DESC "
        "LIMIT ?", scope_bi, MAX_LIST)
    out = []
    for row in rows or []:
        data = await decrypt_row(env, row.get("data")) or {}  # noqa: F821
        entry = {"name": clean_name(data.get("name")),
                 "by": clean_name(data.get("by")),
                 "at": int(row.get("created_at") or 0)}
        if table == "moderation_blocks":
            entry["hideContent"] = bool(int(row.get("hide_content") or 0))
            entry["reason"] = clean_reason(data.get("reason"))
        if entry["name"]:
            out.append(entry)
    return out


async def queue_rows(env, scope_bi):
    rows = await d1_all(  # noqa: F821
        env, "SELECT id, kind, subject, created_at, data FROM "
        "moderation_queue WHERE scope_bi=? ORDER BY id LIMIT ?",
        scope_bi, MAX_LIST)
    out = []
    for row in rows or []:
        held = await decrypt_row(env, row.get("data")) or {}  # noqa: F821
        item = held.get("item") or {}
        event = item.get("event") or {}
        out.append({
            "id": int(row.get("id") or 0),
            "kind": str(row.get("kind") or ""),
            "repo": "%s/%s" % (held.get("publicOwner") or held.get("owner"),
                               held.get("repo")),
            "number": int(item.get("number") or 0),
            "title": clean_string(item.get("titleIfNew") or "", 240),  # noqa: F821
            "body": clean_string(event.get("body") or "", 2000),  # noqa: F821
            "author": clean_name(held.get("account")) or "anonymous",
            "at": int(row.get("created_at") or 0),
        })
    return out


async def log_rows(env, scope_bi):
    rows = await d1_all(  # noqa: F821
        env, "SELECT ts, data FROM moderation_log WHERE scope_bi=? "
        "ORDER BY id DESC LIMIT ?", scope_bi, MAX_LOG)
    out = []
    for row in rows or []:
        data = await decrypt_row(env, row.get("data")) or {}  # noqa: F821
        detail = data.get("detail") if isinstance(
            data.get("detail"), dict) else {}
        out.append({"at": int(row.get("ts") or 0),
                    "actor": clean_name(data.get("actor")),
                    "action": str(data.get("action") or ""),
                    "target": clean_name(data.get("target")),
                    "detail": {key: str(value)[:200] for key, value in
                               detail.items()}})
    return out


async def scope_state(env, scope_bi):
    return {
        "moderators": await _rows(env, "moderation_roles", scope_bi),
        "blocked": await _rows(env, "moderation_blocks", scope_bi),
        "queue": await queue_rows(env, scope_bi),
        "log": await log_rows(env, scope_bi),
    }


async def org_handler(env, request, org):
    org_bi, org_name = await org_scope(env, org)
    if not org_bi:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    method = method_name(request)  # noqa: F821
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request)  # noqa: F821
        except Exception:
            return json_response({"error": "invalid_json"}, status=400,  # noqa: F821
                                 cache_control=PRIVATE_CACHE)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400,  # noqa: F821
                                 cache_control=PRIVATE_CACHE)
    elif method != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)  # noqa: F821
    actor = await _viewer(env, request, data)
    if not actor:
        return json_response({"error": "unauthorized"}, status=401,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if not await can_moderate_org(env, actor, org_bi):
        return json_response({"error": "forbidden"}, status=403,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    manage = await can_manage_moderators(env, actor, org_bi)
    if method == "POST":
        result = await apply_action(
            env, request, org_bi, "org:" + org_name, actor, manage, data)
        if result is not None:
            return result
    state = await scope_state(env, org_bi)
    setting = await policy(env)
    return json_response(dict(state, **{  # noqa: F821
        "ok": True, "org": org_name, "canManageModerators": manage,
        "policy": {key: setting[key] for key in (
            "firstPostApproval", "accountRequired", "hideBlockedPosts",
            "newAccountDays", "newThreadsPerDay", "newCommentsPerDay")},
    }), cache_control=PRIVATE_CACHE)


async def apply_action(env, request, scope_bi, scope_text, actor, manage,
                       data):
    action = str(data.get("action") or "")
    if action not in ORG_ACTIONS:
        return json_response({"error": "invalid_action"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if action in ("approve", "reject"):
        return await _decide(env, request, scope_bi, scope_text, actor,
                             action, data.get("id"))
    name = clean_name(data.get("name"))
    if not name:
        return json_response({"error": "invalid_name"}, status=400,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    target_bi, rec = await _account_row(env, name)  # noqa: F821
    if not rec or rec.get("status") != "active":
        return json_response({"error": "no_such_account"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if action in ("add_moderator", "remove_moderator") and not manage:
        return json_response({"error": "forbidden"}, status=403,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    now = _now()
    detail = {}
    if action == "add_moderator":
        await d1_run(  # noqa: F821
            env, "INSERT INTO moderation_roles (scope_bi, account_bi, role, "
            "created_at, data) VALUES (?,?,'moderator',?,?) ON CONFLICT("
            "scope_bi, account_bi) DO NOTHING", scope_bi, target_bi, now,
            await encrypt_row(env, {  # noqa: F821
                "name": name, "by": actor, "scope": scope_text}))
    elif action == "remove_moderator":
        await d1_run(  # noqa: F821
            env, "DELETE FROM moderation_roles WHERE scope_bi=? AND "
            "account_bi=?", scope_bi, target_bi)
    elif action == "block":
        if name == actor or await _is_admin(env, name):  # noqa: F821
            return json_response({"error": "cannot_block"}, status=400,  # noqa: F821
                                 cache_control=PRIVATE_CACHE)
        hide = bool(data.get("hideContent"))
        reason = clean_reason(data.get("reason"))
        detail = {"hideContent": hide}
        await d1_run(  # noqa: F821
            env, "INSERT INTO moderation_blocks (scope_bi, account_bi, "
            "created_at, hide_content, data) VALUES (?,?,?,?,?) ON CONFLICT("
            "scope_bi, account_bi) DO UPDATE SET hide_content="
            "excluded.hide_content, data=excluded.data", scope_bi, target_bi,
            now, 1 if hide else 0, await encrypt_row(env, {  # noqa: F821
                "name": name, "by": actor, "reason": reason,
                "scope": scope_text}))
        if scope_text == INSTANCE_SCOPE:
            await d1_run(  # noqa: F821
                env, "DELETE FROM moderation_queue WHERE author_bi=?",
                target_bi)
        else:
            await d1_run(  # noqa: F821
                env, "DELETE FROM moderation_queue WHERE author_bi=? AND "
                "scope_bi=?", target_bi, scope_bi)
    elif action == "unblock":
        await d1_run(  # noqa: F821
            env, "DELETE FROM moderation_blocks WHERE scope_bi=? AND "
            "account_bi=?", scope_bi, target_bi)
    elif action == "trust":
        await trust_account(env, name)
    await log(env, scope_bi, scope_text, actor, action, name, detail)
    return None


async def trust_account(env, name):
    name_bi, rec = await _account_row(env, name)  # noqa: F821
    if not rec or rec.get("moderation_trusted_at"):
        return False
    rec["moderation_trusted_at"] = _now()
    await _save_account(env, name_bi, rec)  # noqa: F821
    return True


async def _response_json(response):
    try:
        payload = json.loads(await response.text())
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


async def _decide(env, request, scope_bi, scope_text, actor, action,
                  queue_id):
    try:
        queue_id = int(queue_id)
    except (TypeError, ValueError):
        queue_id = 0
    now = _now()
    row = await d1_first(  # noqa: F821
        env, "UPDATE moderation_queue SET claimed_at=? WHERE id=? AND "
        "scope_bi=? AND claimed_at<? RETURNING id, data", now, queue_id,
        scope_bi, now - CLAIM_MS) if queue_id > 0 else None
    if not row:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    held = await decrypt_row(env, row.get("data")) or {}  # noqa: F821
    account = clean_name(held.get("account"))
    detail = {"repo": "%s/%s" % (held.get("publicOwner") or "",
                                 held.get("repo") or "")}
    if action == "approve":
        commit = globals().get("_moderation_commit_issue")
        response = await commit(env, request, held) if commit else None
        status = int(getattr(response, "status", 0) or 0)
        payload = await _response_json(response)
        if not 200 <= status < 300 or payload.get("pending"):
            await d1_run(  # noqa: F821
                env, "UPDATE moderation_queue SET claimed_at=0 WHERE id=?",
                queue_id)
            return response if response is not None else json_response(  # noqa: F821
                {"error": "approve_failed"}, status=503,
                cache_control=PRIVATE_CACHE)
        if account:
            await trust_account(env, account)
        detail["number"] = str((payload or {}).get("number") or "")
    await d1_run(  # noqa: F821
        env, "DELETE FROM moderation_queue WHERE id=?", queue_id)
    await log(env, scope_bi, scope_text, actor, action, account, detail)
    return None


async def console_state(env):
    scope_bi = await instance_scope(env)
    return scope_bi, await scope_state(env, scope_bi)
