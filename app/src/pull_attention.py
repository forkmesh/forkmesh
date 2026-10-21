import json
import re
from urllib.parse import parse_qs, quote, urlencode, urlparse

INTERNAL_ORIGIN = "https://forkmesh.internal"
KIND = "pull_attention"
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,255}$")
REPLY_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
RECORD_REFS = ("forkmesh/pulls", "forkmesh/pull/{}", "refs/pr/{}/metadata")
REVIEW_STATES = ("approved", "changes_requested", "commented")
CRON_BATCH = 2
MAX_BRANCHES_PER_PUSH = 10
MAX_CONFLICT_FILES = 5
RECHECK_MS = 12 * 60 * 60 * 1000
RETAIN_MS = 30 * 24 * 60 * 60 * 1000
REPLY_RETAIN_MS = 90 * 24 * 60 * 60 * 1000
REPORT_COOLDOWN_MS = 60 * 1000
PRUNE_INTERVAL_MS = 60 * 60 * 1000
FAILURE_BACKOFF_MS = (10 * 60 * 1000, 30 * 60 * 1000, 2 * 60 * 60 * 1000,
                      6 * 60 * 60 * 1000)
MAX_SENT_LISTED = 50

_prune_memo = {"at": 0}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def branch_name(value):
    name = str(value or "").strip()
    if name.startswith("refs/heads/"):
        name = name[len("refs/heads/"):]
    if not BRANCH_RE.fullmatch(name):
        return ""
    if (name[0] in "-/." or name.endswith("/") or name.endswith(".lock")
            or ".." in name or "//" in name or "@{" in name):
        return ""
    return name


def oid(value):
    text = str(value or "").strip().lower()
    return text if OID_RE.fullmatch(text) else ""


def head_branch(value):
    text = str(value or "").strip()
    return branch_name(text[text.rfind(":") + 1:] if ":" in text else text)


def is_draft(fields):
    fields = fields or {}
    status = str(fields.get("status") or "").strip().lower()
    flag = str(fields.get("draft") or "").strip().lower()
    return status == "draft" or flag in ("true", "yes", "1")


def _zero(value):
    text = str(value or "")
    return bool(text) and set(text) == {"0"}


def _now(now=None):
    return int(now if now is not None else Date.now())


async def repo_key(env, owner, repo):
    return await blind_index(
        env, str(owner or "").strip().lower() + "/" + str(repo or "").strip())


async def _branch_key(env, repo_bi, branch):
    return await blind_index(
        env, "pull-watch-branch:" + str(repo_bi) + ":" + str(branch or ""))


async def _trial_key(env, base_oid, head_oid):
    return await blind_index(
        env, "pull-watch-trial:" + base_oid + ":" + head_oid)


async def _notified_key(env, base_oid, head_oid):
    return await blind_index(
        env, "pull-watch-notified:" + base_oid + ":" + head_oid)


async def _row(env, repo_bi, number):
    row = await d1_first(
        env,
        "SELECT repo_bi, number, base_bi, head_bi, due_at, checked_at, "
        "trial_bi, notified_bi, updated_at, data FROM pull_merge_watch "
        "WHERE repo_bi=? AND number=?",
        repo_bi, int(number))
    if not row:
        return None, {}
    try:
        data = await decrypt_row(env, row.get("data"))
    except Exception:
        data = None
    return row, data if isinstance(data, dict) else {}


async def _save(env, repo_bi, number, data, due_at=None, checked_at=None,
                trial_bi=None, now=None):
    now = _now(now)
    base_bi = await _branch_key(env, repo_bi, data.get("base") or "")
    head = head_branch(data.get("head")) if data.get(
        "derive") == "branch" else ""
    head_bi = await _branch_key(env, repo_bi, head) if head else ""
    await d1_run(
        env,
        "INSERT INTO pull_merge_watch (repo_bi, number, base_bi, head_bi, "
        "due_at, checked_at, trial_bi, notified_bi, updated_at, data) "
        "VALUES (?,?,?,?,?,?,?,'',?,?) ON CONFLICT(repo_bi, number) DO UPDATE "
        "SET base_bi=excluded.base_bi, head_bi=excluded.head_bi, "
        "due_at=CASE WHEN ? IS NULL THEN pull_merge_watch.due_at "
        "ELSE excluded.due_at END, "
        "checked_at=CASE WHEN ? IS NULL THEN pull_merge_watch.checked_at "
        "ELSE excluded.checked_at END, "
        "trial_bi=CASE WHEN ? IS NULL THEN pull_merge_watch.trial_bi "
        "ELSE excluded.trial_bi END, "
        "updated_at=excluded.updated_at, data=excluded.data",
        repo_bi, int(number), base_bi, head_bi,
        int(due_at or 0), int(checked_at or 0), trial_bi or "", now,
        await encrypt_row(env, data),
        due_at, checked_at, trial_bi)


async def watch(env, owner, repo, number, base="", head="", derive="",
                author="", author_name="", public_owner="", due=True,
                now=None):
    try:
        number = int(number)
    except (TypeError, ValueError):
        return False
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    if number <= 0 or not owner or not repo:
        return False
    now = _now(now)
    repo_bi = await repo_key(env, owner, repo)
    _row_value, data = await _row(env, repo_bi, number)
    data.update({"owner": owner, "repo": repo, "number": number})
    public = str(public_owner or "").strip().lower()
    if public:
        data["publicOwner"] = public
    if branch_name(base):
        data["base"] = branch_name(base)
    if str(head or "").strip():
        data["head"] = str(head).strip()[:300]
    if derive:
        data["derive"] = "branch" if derive == "branch" else "patch"
    if author:
        data["authorKey"] = str(author)[:120]
    if author_name and valid_node_name(str(author_name).strip().lower()):
        data["authorName"] = str(author_name).strip().lower()
    await _save(env, repo_bi, number, data,
                due_at=now if due else None, now=now)
    return True


async def forget(env, owner, repo, number):
    await d1_run(
        env, "DELETE FROM pull_merge_watch WHERE repo_bi=? AND number=?",
        await repo_key(env, owner, repo), int(number))


async def moved(env, owner, repo, branches, now=None):
    now = _now(now)
    repo_bi = await repo_key(env, owner, repo)
    marked = 0
    seen = set()
    for value in list(branches or [])[:MAX_BRANCHES_PER_PUSH]:
        branch = branch_name(value)
        if not branch or branch in seen or branch.startswith("forkmesh/"):
            continue
        seen.add(branch)
        key = await _branch_key(env, repo_bi, branch)
        await d1_run(
            env,
            "UPDATE pull_merge_watch SET due_at=? WHERE repo_bi=? "
            "AND due_at=0 AND (base_bi=? OR head_bi=?)",
            now, repo_bi, key, key)
        marked += 1
    return marked


async def pushed(env, owner, repo, updates, now=None):
    branches = []
    for update in list(updates or []):
        ref = str((update or {}).get("ref") or "")
        if ref.startswith("refs/heads/"):
            branches.append(ref[len("refs/heads/"):])
    if not branches:
        return 0
    return await moved(env, owner, repo, branches, now=now)


async def merged(env, owner, repo, number, now=None):
    now = _now(now)
    repo_bi = await repo_key(env, owner, repo)
    row, data = await _row(env, repo_bi, number)
    await d1_run(
        env, "DELETE FROM pull_merge_watch WHERE repo_bi=? AND number=?",
        repo_bi, int(number))
    if row and data.get("base"):
        return await moved(env, owner, repo, [data["base"]], now=now)
    await d1_run(
        env,
        "UPDATE pull_merge_watch SET due_at=? WHERE repo_bi=? AND due_at=0",
        now, repo_bi)
    return 0


async def _read(env, owner, repo, operation, query=None):
    url = "%s/api/repo/%s/%s/%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation)
    if query:
        url += "?" + urlencode(query)
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=True)
        status = int(getattr(response, "status", 0) or 0)
        text = str(await response.text())
        value = json.loads(text) if text else None
    except Exception:
        return 0, None
    return status, value if isinstance(value, dict) else None


def _front_matter(text):
    raw = str(text or "").lstrip()
    if not raw.startswith("---"):
        return {}
    body = raw[3:]
    end = body.find("\n---")
    if end < 0:
        return {}
    fields = {}
    for line in body[:end].splitlines():
        key, separator, value = line.partition(":")
        key = key.strip()
        if separator and key and key not in fields:
            fields[key] = value.strip()
    return fields


async def pull_record(env, owner, repo, number):
    for ref in RECORD_REFS:
        status, value = await _read(
            env, owner, repo, "blob",
            {"path": ".forkmesh/pulls/%d/pull.md" % int(number),
             "ref": ref.format(int(number))})
        if status != 200 or not value:
            continue
        fields = _front_matter(_forkbot_blob_text(value))
        if str(fields.get("number") or "").strip() == str(int(number)):
            return fields
    return {}


async def _tips(env, owner, repo):
    status, value = await _read(env, owner, repo, "branches")
    if status != 200 or not value:
        return None, ""
    tips = {}
    for item in value.get("branches") or []:
        if not isinstance(item, dict):
            continue
        name = branch_name(item.get("name"))
        commit = oid(item.get("commit"))
        if name and commit:
            tips[name] = commit
    return tips, branch_name(value.get("defaultBranch"))


async def _trial(env, owner, repo, base_oid, head_oid):
    status, value = await _read(
        env, owner, repo, "merge-preview", {"base": base_oid, "head": head_oid})
    if status != 200 or not value:
        return None
    if oid(value.get("baseOid")) != base_oid:
        return None
    conflicts = [str(path) for path in (value.get("conflicts") or [])
                 if isinstance(path, str) and path][:50]
    clean = value.get("clean") is True
    try:
        behind = max(0, int(value.get("behind") or 0))
        ahead = max(0, int(value.get("ahead") or 0))
    except (TypeError, ValueError):
        behind = ahead = 0
    return {"state": "clean" if clean else "conflict", "conflicts": conflicts,
            "behind": behind, "ahead": ahead}


async def recipient_for(env, data, fields=None):
    fields = fields or {}
    key = str(fields.get("author") or data.get("authorKey") or "").strip()
    if key and valid_node_pubkey(key):
        try:
            name = str(await _git_push_account_for_pubkey(env, key) or "")
        except Exception:
            name = ""
        name = name.strip().lower()
        if valid_node_name(name):
            return name
    name = str(fields.get("authorName") or data.get("authorName") or "")
    name = name.strip().lower()
    return name if valid_node_name(name) else ""


def pull_href(data):
    owner = data.get("publicOwner") or data.get("owner") or ""
    segment = NOTIFICATION_ITEM_URL_SEGMENTS.get("pull") or "pulls"
    return "%s/%s/%d" % (repo_web_href(owner, data.get("repo") or ""),
                         segment, int(data.get("number") or 0))


def repo_label(data):
    return "%s/%s" % (data.get("publicOwner") or data.get("owner") or "",
                      data.get("repo") or "")


def conflict_commands(data, clone_url=""):
    base = data.get("base") or "main"
    if data.get("derive") == "branch":
        head = head_branch(data.get("head")) or "your-branch"
        return [
            "git fetch origin",
            "git switch " + head,
            "git merge origin/" + base,
            "# fix the conflicted files, then",
            "git add -A && git commit",
            "git push",
        ]
    source = clone_url or "<this repository>"
    return [
        "git fetch " + source + " " + base,
        "git rebase FETCH_HEAD",
        "# fix the conflicted files, then",
        "git add -A && git rebase --continue",
        "# and submit the pull request again",
    ]


def conflict_message(data, conflicts, clone_url=""):
    number = int(data.get("number") or 0)
    base = data.get("base") or "main"
    title = "Pull request #%d has conflicts with %s" % (number, base)
    lead = ("Your pull request #%d has conflicts with %s; please rebase or "
            "merge %s and push again." % (number, base, base))
    files = list(conflicts or [])
    lines = [lead]
    if files:
        shown = ", ".join(files[:MAX_CONFLICT_FILES])
        more = len(files) - MAX_CONFLICT_FILES
        lines.append("Conflicting files: " + shown + (
            " and %d more" % more if more > 0 else ""))
    lines.append("\n".join(conflict_commands(data, clone_url)))
    return title, "\n\n".join(lines)


def _clone_url(env, data):
    try:
        origin = str(_public_base_url(env) or "").rstrip("/")
    except Exception:
        origin = ""
    if not origin:
        return ""
    return "%s/%s/%s.git" % (
        origin, quote(data.get("publicOwner") or data.get("owner") or ""),
        quote(data.get("repo") or ""))


async def _notify_conflict(env, repo_bi, data, base_oid, head_oid, fields):
    key = await _notified_key(env, base_oid, head_oid)
    result = await d1_run(
        env,
        "UPDATE pull_merge_watch SET notified_bi=? WHERE repo_bi=? "
        "AND number=? AND notified_bi<>?",
        key, repo_bi, int(data["number"]), key)
    try:
        claimed = int(result.meta.changes) > 0
    except Exception:
        claimed = True
    if not claimed:
        return False
    recipient = await recipient_for(env, data, fields)
    if not recipient:
        return False
    title, body = conflict_message(
        data, data.get("conflicts") or [], _clone_url(env, data))
    return await enqueue_notification(
        env, recipient, KIND, title, body=body, repo=repo_label(data),
        href=pull_href(data), source="pull",
        dedupe="pull-conflict:%s/%s#%d:%s:%s" % (
            data["owner"], data["repo"], int(data["number"]),
            base_oid, head_oid),
        meta={"number": int(data["number"]), "source": "pull",
              "reason": "conflict", "base": data.get("base") or "",
              "head": head_branch(data.get("head")),
              "files": list(data.get("conflicts") or [])[:MAX_CONFLICT_FILES]})


async def _backoff(env, repo_bi, number, data, now):
    failures = int(data.get("failures") or 0) + 1
    data["failures"] = failures
    due = (now + FAILURE_BACKOFF_MS[failures - 1]
           if failures <= len(FAILURE_BACKOFF_MS) else 0)
    await _save(env, repo_bi, number, data, due_at=due, checked_at=now,
                now=now)
    return {"state": "unknown"}


async def check(env, owner, repo, number, now=None):
    now = _now(now)
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    number = int(number)
    repo_bi = await repo_key(env, owner, repo)
    row, data = await _row(env, repo_bi, number)
    data.update({"owner": owner, "repo": repo, "number": number})
    fields = await pull_record(env, owner, repo, number)
    if not fields:
        return await _backoff(env, repo_bi, number, data, now)
    status = str(fields.get("status") or "open").strip().lower()
    draft = is_draft(fields)
    if status != "open" and not draft:
        await forget(env, owner, repo, number)
        return {"state": "closed"}
    data["derive"] = "branch" if fields.get("derive") == "branch" else "patch"
    data["head"] = str(fields.get("head") or data.get("head") or "")[:300]
    if fields.get("author"):
        data["authorKey"] = str(fields.get("author"))[:120]
    name = str(fields.get("authorName") or "").strip().lower()
    if valid_node_name(name):
        data["authorName"] = name
    data["draft"] = draft
    tips, default = await _tips(env, owner, repo)
    if tips is None:
        return await _backoff(env, repo_bi, number, data, now)
    data["base"] = branch_name(fields.get("base")) or default or "main"
    base_oid = tips.get(data["base"], "")
    head_oid = (tips.get(head_branch(data["head"]), "")
                if data["derive"] == "branch" else oid(fields.get("headOid")))
    data["failures"] = 0
    if not base_oid or not head_oid:
        data.update({"state": "unknown", "baseOid": base_oid,
                     "headOid": head_oid, "conflicts": []})
        await _save(env, repo_bi, number, data, due_at=0, checked_at=now,
                    trial_bi="", now=now)
        return {"state": "unknown"}
    trial_bi = await _trial_key(env, base_oid, head_oid)
    cached = (row and row.get("trial_bi") == trial_bi
              and data.get("state") in ("clean", "conflict"))
    if not cached:
        result = await _trial(env, owner, repo, base_oid, head_oid)
        if result is None:
            return await _backoff(env, repo_bi, number, data, now)
        data.update(result)
    data.update({"baseOid": base_oid, "headOid": head_oid})
    await _save(env, repo_bi, number, data, due_at=0, checked_at=now,
                trial_bi=trial_bi, now=now)
    notified = False
    if data.get("state") == "conflict" and not draft:
        notified = await _notify_conflict(
            env, repo_bi, data, base_oid, head_oid, fields)
    return {"state": data.get("state"), "baseOid": base_oid,
            "headOid": head_oid, "notified": bool(notified),
            "cached": bool(cached)}


async def _prune(env, now):
    if now - _prune_memo["at"] < PRUNE_INTERVAL_MS:
        return
    _prune_memo["at"] = now
    await d1_run(env, "DELETE FROM pull_merge_watch WHERE updated_at<?",
                 now - RETAIN_MS)
    await d1_run(env, "DELETE FROM pull_quick_replies WHERE created_at<?",
                 now - REPLY_RETAIN_MS)


async def cron(env, batch=CRON_BATCH, now=None):
    await ensure_schema(env)
    now = _now(now)
    rows = await d1_all(
        env,
        "SELECT repo_bi, number, data FROM pull_merge_watch "
        "WHERE due_at>0 AND due_at<=? ORDER BY due_at LIMIT ?",
        now, int(batch))
    rows = list(rows or [])
    if len(rows) < batch:
        stale = await d1_all(
            env,
            "SELECT repo_bi, number, data FROM pull_merge_watch "
            "WHERE due_at=0 AND checked_at<? ORDER BY checked_at LIMIT ?",
            now - RECHECK_MS, int(batch) - len(rows))
        rows.extend(stale or [])
    checked = 0
    for row in rows:
        if _cron_budget_exhausted():
            break
        try:
            data = await decrypt_row(env, row.get("data"))
        except Exception:
            data = None
        if not isinstance(data, dict) or not data.get("owner"):
            await d1_run(
                env,
                "DELETE FROM pull_merge_watch WHERE repo_bi=? AND number=?",
                row.get("repo_bi"), int(row.get("number") or 0))
            continue
        try:
            await check(env, data["owner"], data["repo"],
                        int(row.get("number") or 0), now=now)
        except Exception:
            if _cron_budget_exhausted():
                raise
        checked += 1
    if not _cron_budget_exhausted():
        await _prune(env, now)
    return {"ok": True, "checked": checked}


async def _reviewer_key(env, reviewer):
    return await blind_index(env, "pull-quick-reviewer:" + str(reviewer))


def quick_reply_fields(value):
    if not isinstance(value, dict):
        return None
    reply_id = str(value.get("id") or "").strip()
    if not REPLY_ID_RE.fullmatch(reply_id):
        return None
    return {"id": reply_id, "head": oid(value.get("head"))}


async def claim_quick_reply(env, repo_bi, number, reviewer, quick, now=None):
    fields = quick_reply_fields(quick)
    if fields is None or not reviewer:
        return None
    result = await d1_run(
        env,
        "INSERT OR IGNORE INTO pull_quick_replies (repo_bi, number, "
        "reviewer_bi, head, reply_id, created_at) VALUES (?,?,?,?,?,?)",
        repo_bi, int(number), await _reviewer_key(env, reviewer),
        fields["head"], fields["id"], _now(now))
    try:
        return int(result.meta.changes) > 0
    except Exception:
        return True


async def sent_quick_replies(env, repo_bi, number, reviewer, head):
    if not reviewer:
        return []
    rows = await d1_all(
        env,
        "SELECT reply_id FROM pull_quick_replies WHERE repo_bi=? AND "
        "number=? AND reviewer_bi=? AND head=? ORDER BY created_at LIMIT ?",
        repo_bi, int(number), await _reviewer_key(env, reviewer),
        oid(head), MAX_SENT_LISTED)
    return [str(row.get("reply_id") or "") for row in rows or []
            if row.get("reply_id")]


def review_title(actor, state, number):
    who = actor or "Someone"
    if state == "approved":
        return "%s approved your pull request #%d" % (who, number)
    if state == "changes_requested":
        return "%s requested changes on your pull request #%d" % (who, number)
    return "%s commented on your pull request #%d" % (who, number)


async def review_posted(env, owner, repo, number, event, actor,
                        public_owner="", quick=None):
    event = event if isinstance(event, dict) else {}
    kind = str(event.get("type") or "")
    state = str(event.get("state") or "")
    quick_fields = quick_reply_fields(quick)
    if kind == "review" and state in REVIEW_STATES:
        pass
    elif kind == "comment" and quick_fields:
        state = "commented"
    else:
        return ""
    try:
        number = int(number)
    except (TypeError, ValueError):
        return ""
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    repo_bi = await repo_key(env, owner, repo)
    row, data = await _row(env, repo_bi, number)
    fields = None
    if not row or not (data.get("authorKey") or data.get("authorName")):
        fields = await pull_record(env, owner, repo, number)
        if not fields:
            return ""
        if str(fields.get("status") or "open").strip().lower() not in (
                "open", "draft"):
            return ""
        await watch(
            env, owner, repo, number, base=fields.get("base") or "",
            head=fields.get("head") or "",
            derive="branch" if fields.get("derive") == "branch" else "patch",
            author=fields.get("author") or "",
            author_name=fields.get("authorName") or "",
            public_owner=public_owner, due=True)
        row, data = await _row(env, repo_bi, number)
    data.update({"owner": owner, "repo": repo, "number": number})
    if public_owner:
        data["publicOwner"] = str(public_owner).strip().lower()
    recipient = await recipient_for(env, data, fields)
    actor = str(actor or "").strip().lower()
    if not recipient or recipient == actor:
        return ""
    body = clean_string(event.get("body") or "", NOTIFICATION_BODY_MAX)
    event_id = clean_string(event.get("id") or "", 160) or str(
        event.get("ts") or "")
    meta = {"number": number, "source": "pull", "reason": "review",
            "state": state}
    if quick_fields:
        meta["quickReply"] = quick_fields["id"]
    await enqueue_notification(
        env, recipient, KIND, review_title(actor, state, number),
        body=body or "Open the pull request to read the review.",
        repo=repo_label(data), href=pull_href(data), actor=actor,
        source="pull",
        dedupe="pull-review:%s/%s#%d:%s" % (owner, repo, number, event_id),
        meta=meta)
    return recipient


def _public_owner(request, owner):
    match = REPO_API_PREFIX_RE.match(urlparse(request.url).path)
    public = safe_segment(match.group(1)) if match else ""
    return str(public or owner or "").strip().lower()


async def handler(env, request, owner, repo, number):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405,
                             extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
    account = await _authed_account_name(env, request, data)
    if not account:
        return json_response({"error": "unauthorized"}, status=401,
                             cache_control="no-store")
    if await _repo_is_private(env, owner, repo):
        return json_response({"error": "not_found"}, status=404)
    repo_bi = await repo_key(env, owner, repo)
    if method == "GET":
        head = oid(parse_qs(urlparse(request.url).query).get(
            "head", [""])[0])
        return json_response(
            {"ok": True, "head": head,
             "sent": await sent_quick_replies(
                 env, repo_bi, number, account, head)},
            cache_control="private, no-store")
    base_oid = oid(data.get("baseOid"))
    head_oid = oid(data.get("headOid"))
    if not base_oid or not head_oid:
        return json_response({"error": "invalid_request"}, status=400)
    now = _now()
    row, stored = await _row(env, repo_bi, number)
    trial_bi = await _trial_key(env, base_oid, head_oid)
    if row and row.get("trial_bi") == trial_bi:
        return json_response(
            {"ok": True, "state": stored.get("state") or "unknown",
             "cached": True}, cache_control="no-store")
    public = _public_owner(request, owner)
    if not row:
        await watch(env, owner, repo, number,
                    base=str(data.get("base") or ""),
                    head=str(data.get("head") or ""),
                    derive=str(data.get("derive") or ""),
                    public_owner=public, due=True, now=now)
    elif public and stored.get("publicOwner") != public:
        await watch(env, owner, repo, number, public_owner=public,
                    due=False, now=now)
    recent = row and now - int(row.get("checked_at") or 0) < REPORT_COOLDOWN_MS
    if data.get("clean") is False and not recent:
        result = await check(env, owner, repo, number, now=now)
        return json_response({"ok": True, **result},
                             cache_control="no-store")
    if row and not int(row.get("due_at") or 0):
        await d1_run(
            env,
            "UPDATE pull_merge_watch SET due_at=? WHERE repo_bi=? AND "
            "number=? AND due_at=0",
            now, repo_bi, int(number))
    return json_response({"ok": True, "state": "queued"},
                         cache_control="no-store")
