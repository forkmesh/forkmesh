import html
import re
from urllib.parse import parse_qs, quote, urlparse

KINDS = {"issue": ("issues", "issues"), "pull": ("pulls", "pull"),
         "discussion": ("discussions", "discussions")}
COMMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@+-]{0,159}$")
LANG_RE = re.compile(r"^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,2}$")
HEX_RE = re.compile(r"^[0-9a-f]{64}$")
B64URL_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")
CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
SIGNING_PREFIX = "forkmesh-translation-v1"
MAX_NUMBER = 1_000_000_000
MAX_BODY = 20000
MAX_ROWS_PER_RECORD = 500
MAX_ROWS_PER_ACCOUNT_PER_RECORD = 60
MAX_WRITES_PER_MINUTE = 10
MAX_WRITES_PER_DAY = 100
MINUTE_MS = 60 * 1000
DAY_MS = 24 * 60 * 60 * 1000
CLOCK_SKEW_MS = 10 * 60 * 1000
PUBLIC_CACHE = "public, max-age=5, stale-while-revalidate=30"
PRIVATE_CACHE = "private, no-store"
VARY = {"vary": "authorization, cookie"}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_kind(value):
    kind = str(value or "").strip().lower()
    return kind if kind in KINDS else ""


def clean_number(value):
    try:
        number = int(str(value if value is not None else "").strip())
    except (TypeError, ValueError):
        return 0
    return number if 0 < number <= MAX_NUMBER else 0


def clean_comment(value):
    comment = value if isinstance(value, str) else ""
    return comment if COMMENT_RE.fullmatch(comment) else ""


def clean_lang(value):
    raw = value.strip().replace("_", "-") if isinstance(value, str) else ""
    if not raw:
        return ""
    parts = raw.split("-")
    parts[0] = parts[0].lower()
    lang = "-".join(parts)
    return lang if LANG_RE.fullmatch(lang) else ""


def clean_body(value):
    if not isinstance(value, str):
        return ""
    body = CONTROL_RE.sub("", value.replace("\r\n", "\n").replace("\r", "\n"))
    body = body.rstrip()
    return body if body.strip() and len(body) <= MAX_BODY else ""


def subject_key(kind, number):
    return "%s:%d" % (kind, number)


def signing_text(owner, repo, kind, number, comment, lang, pub, ts, body_hash):
    return "\n".join((
        SIGNING_PREFIX, "%s/%s" % (owner.strip().lower(), repo.strip().lower()),
        subject_key(kind, number), comment, lang, pub, str(int(ts)), body_hash))


async def _rows(env, repo_bi, subject, comment="", lang=""):
    sql = ("SELECT comment, lang, account_bi, created_at, updated_at, data "
           "FROM post_translations WHERE repo_bi=? AND subject=?")
    args = [repo_bi, subject]
    if comment:
        sql += " AND comment=?"
        args.append(comment)
    if lang:
        sql += " AND lang=?"
        args.append(lang)
    sql += " ORDER BY created_at LIMIT ?"
    args.append(MAX_ROWS_PER_RECORD)
    return await d1_all(env, sql, *args)


async def _entry(env, row, viewer_bi, with_body):
    try:
        data = await decrypt_row(env, row.get("data"))
    except Exception:
        data = None
    if not isinstance(data, dict):
        return None
    name = clean_string(data.get("name"), MAX_NODE_NAME)
    if not name:
        return None
    entry = {
        "lang": str(row.get("lang") or ""),
        "name": name,
        "agent": bool(data.get("agent")),
        "mine": bool(viewer_bi) and str(row.get("account_bi") or "") == viewer_bi,
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "source": str(data.get("source") or ""),
    }
    if with_body:
        entry.update({"body": str(data.get("body") or ""),
                      "pub": str(data.get("pub") or ""),
                      "sig": str(data.get("sig") or ""),
                      "ts": int(data.get("ts") or 0)})
    return entry


async def summarize(env, rows, viewer_bi="", with_body=False):
    out = {}
    for row in rows or []:
        comment = str(row.get("comment") or "")
        if not clean_comment(comment) or not clean_lang(str(row.get("lang") or "")):
            continue
        entry = await _entry(env, row, viewer_bi, with_body)
        if entry:
            out.setdefault(comment, []).append(entry)
    return out


async def _count(env, sql, *args):
    row = await d1_first(env, sql, *args)
    return int((row or {}).get("n") or 0)


async def _refusal(env, repo_bi, subject, account_bi, now, exists):
    if await _count(
            env, "SELECT COUNT(*) AS n FROM post_translations WHERE "
            "account_bi=? AND updated_at>?",
            account_bi, now - MINUTE_MS) >= MAX_WRITES_PER_MINUTE:
        return "rate_limited", 429
    if await _count(
            env, "SELECT COUNT(*) AS n FROM post_translations WHERE "
            "account_bi=? AND updated_at>?",
            account_bi, now - DAY_MS) >= MAX_WRITES_PER_DAY:
        return "daily_limit", 429
    if exists:
        return None
    if await _count(
            env, "SELECT COUNT(*) AS n FROM post_translations WHERE repo_bi=? "
            "AND subject=? AND account_bi=?",
            repo_bi, subject, account_bi) >= MAX_ROWS_PER_ACCOUNT_PER_RECORD:
        return "too_many_translations", 409
    if await _count(
            env, "SELECT COUNT(*) AS n FROM post_translations WHERE repo_bi=? "
            "AND subject=?", repo_bi, subject) >= MAX_ROWS_PER_RECORD:
        return "record_full", 409
    return None


def _is_agent(request, data):
    try:
        token, cookie_auth = _request_account_session_token(request, data)
    except Exception:
        return False
    return not cookie_auth and str(token or "").startswith("fmat_")


def _error(error, status):
    return json_response({"error": error}, status=status,
                         cache_control=PRIVATE_CACHE)


def _page(owner, repo, kind, number, translations, comment, lang):
    record = "%s/%s %s #%d" % (owner, repo, {"issue": "issue", "pull": "pull request",
                                             "discussion": "discussion"}[kind], number)
    back = "/%s/%s/%s/%d" % (quote(owner), quote(repo), KINDS[kind][1], number)
    parts = []
    for post, entries in translations.items():
        if comment and post != comment:
            continue
        for entry in entries:
            if lang and entry["lang"] != lang:
                continue
            parts.append(
                '<article id="%s-%s"><h2>%s · %s</h2><p class="meta">Community '
                'translation by %s. The original post is the source of truth.</p>'
                '<div class="body" lang="%s" dir="auto">%s</div></article>' % (
                    html.escape(post), html.escape(entry["lang"]),
                    "Opening post" if post == "body" else "Comment " + html.escape(post),
                    html.escape(entry["lang"]), html.escape(entry["name"]),
                    html.escape(entry["lang"]), html.escape(entry.get("body", ""))))
    content = "".join(parts) or "<p>No community translations yet.</p>"
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<meta name="robots" content="noindex">'
        "<title>Translations of %s</title><style>body{font:16px/1.5 system-ui,"
        "sans-serif;max-width:46rem;margin:2rem auto;padding:0 1rem}"
        ".meta{color:#57606a;font-size:14px}.body{white-space:pre-wrap;"
        "overflow-wrap:anywhere}article{border-top:1px solid #d0d7de;"
        "padding:1rem 0}</style></head><body><h1>Translations of %s</h1>"
        '<p><a href="%s">Back to the original</a></p>%s</body></html>') % (
            html.escape(record), html.escape(record), html.escape(back), content)


async def _read(env, request, owner, repo):
    query = parse_qs(urlparse(str(request.url)).query)
    kind = clean_kind((query.get("kind") or [""])[0])
    number = clean_number((query.get("number") or [""])[0])
    comment_raw = (query.get("comment") or [""])[0]
    lang_raw = (query.get("lang") or [""])[0]
    comment = clean_comment(comment_raw)
    lang = clean_lang(lang_raw)
    page = (query.get("format") or [""])[0] == "html"
    if not kind or not number or (comment_raw and not comment) \
            or (lang_raw and not lang):
        return _error("invalid_target", 400)
    access = await _repository_access_context(env, request, owner, repo)
    if access is None:
        return _error("not_found", 404)
    viewer = str(access.get("viewer") or "")
    viewer_bi = await blind_index(env, viewer) if viewer else ""
    with_body = page or bool(comment)
    rows = await _rows(env, access["repoBi"], subject_key(kind, number),
                       comment, lang)
    translations = await summarize(env, rows, viewer_bi, with_body)
    shared = not viewer and access.get("visibility") == "public"
    cache = PUBLIC_CACHE if shared else PRIVATE_CACHE
    if page:
        return Response(
            _page(owner, repo, kind, number, translations, comment, lang),
            status=200, headers=dict(VARY, **{
                "content-type": "text/html; charset=utf-8",
                "cache-control": cache,
                "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'",
                "x-content-type-options": "nosniff"}))
    return json_response(
        {"ok": True, "kind": kind, "number": number, "signedIn": bool(viewer),
         "translations": translations},
        cache_control=cache, extra_headers=VARY)


async def _write(env, request, owner, repo):
    try:
        data = await bounded_json_request(request, max_bytes=96 * 1024)
    except Exception:
        return _error("invalid_json", 400)
    if not isinstance(data, dict):
        return _error("invalid_json", 400)
    account_bi, rec = await _account_session_record(env, request, data)
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()
    if not rec or not account_bi or not account:
        return _error("unauthorized", 401)
    kind = clean_kind(data.get("kind"))
    number = clean_number(data.get("number"))
    comment = clean_comment(data.get("comment"))
    lang = clean_lang(data.get("lang"))
    if not kind or not number or not comment:
        return _error("invalid_target", 400)
    if not lang:
        return _error("invalid_language", 400)
    access = await _repository_access_context(env, request, owner, repo,
                                           viewer=account)
    if access is None:
        return _error("not_found", 404)
    disabled = await _repo_feature_off(env, owner, repo, KINDS[kind][0])
    if disabled is not None:
        return disabled
    repo_bi = access["repoBi"]
    subject = subject_key(kind, number)
    if data.get("remove") is True:
        target = clean_string(data.get("account"), MAX_NODE_NAME).strip().lower() \
            if isinstance(data.get("account"), str) else ""
        target_bi = account_bi
        if target and target != account:
            allowed, _ = await _authorize_issue_manager(env, owner, data, request)
            moderates = globals().get("moderation_can_moderate_repo")
            if not allowed and moderates is not None:
                allowed = await moderates(env, account, owner, repo)
            if not allowed:
                return _error("not_authorized", 403)
            target_bi = await blind_index(env, target)
        await d1_run(
            env, "DELETE FROM post_translations WHERE repo_bi=? AND subject=? "
            "AND comment=? AND lang=? AND account_bi=?",
            repo_bi, subject, comment, lang, target_bi)
        return json_response({"ok": True, "removed": True, "comment": comment,
                              "lang": lang}, cache_control=PRIVATE_CACHE)
    blocked = globals().get("moderation_blocked_response")
    refusal = await blocked(env, account, owner, repo) if blocked is not None else None
    if refusal is not None:
        return refusal
    body = clean_body(data.get("body"))
    if not body:
        return _error("invalid_body", 400)
    pub = data.get("pub") if isinstance(data.get("pub"), str) else ""
    sig = data.get("sig") if isinstance(data.get("sig"), str) else ""
    source = data.get("source") if isinstance(data.get("source"), str) else ""
    try:
        ts = int(data.get("ts"))
    except (TypeError, ValueError):
        ts = 0
    now = int(Date.now())
    if source and not HEX_RE.fullmatch(source):
        return _error("invalid_source", 400)
    if not B64URL_RE.fullmatch(pub) or not B64URL_RE.fullmatch(sig) \
            or ts <= 0 or abs(now - ts) > CLOCK_SKEW_MS:
        return _error("bad_signature", 400)
    canonical = signing_text(owner, repo, kind, number, comment, lang, pub, ts,
                             await sha256_hex(body))
    if not await ed25519_verify(pub, sig, canonical.encode("utf-8")):
        return _error("bad_signature", 400)
    existing = await d1_first(
        env, "SELECT created_at FROM post_translations WHERE repo_bi=? AND "
        "subject=? AND comment=? AND lang=? AND account_bi=?",
        repo_bi, subject, comment, lang, account_bi)
    refused = await _refusal(env, repo_bi, subject, account_bi, now,
                             existing is not None)
    if refused is not None:
        return _error(*refused)
    stored = await encrypt_row(env, {
        "name": clean_string(rec.get("name"), MAX_NODE_NAME) or account,
        "agent": _is_agent(request, data), "body": body, "pub": pub,
        "sig": sig, "ts": ts, "source": source})
    created = int((existing or {}).get("created_at") or now)
    await d1_run(
        env,
        "INSERT INTO post_translations (repo_bi, subject, comment, lang, "
        "account_bi, created_at, updated_at, data) VALUES (?,?,?,?,?,?,?,?) "
        "ON CONFLICT(repo_bi, subject, comment, lang, account_bi) DO UPDATE "
        "SET updated_at=excluded.updated_at, data=excluded.data",
        repo_bi, subject, comment, lang, account_bi, created, now, stored)
    rows = await _rows(env, repo_bi, subject, comment, lang)
    return json_response(
        {"ok": True, "kind": kind, "number": number, "comment": comment,
         "lang": lang, "translations": (await summarize(
             env, rows, account_bi, True)).get(comment, [])},
        cache_control=PRIVATE_CACHE)


async def handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    if method == "GET":
        return await _read(env, request, owner, repo)
    if method == "POST":
        return await _write(env, request, owner, repo)
    return _error("method_not_allowed", 405)
