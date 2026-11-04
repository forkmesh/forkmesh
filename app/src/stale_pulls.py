import base64
import json
import re
from datetime import datetime, timezone
from urllib.parse import quote, urlencode

INTERNAL_ORIGIN = "https://forkmesh.internal"
SETTING = "stalePulls"
DELETE_SETTING = "stalePullsDeleteBranch"
KIND = "pull_attention"
PULLS_DIR = ".forkmesh/pulls"
PULLS_REF = "forkmesh/pulls"
BOT_NAME = "ForkBot"
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS
STAGE_DELAYS = (DAY_MS, 3 * DAY_MS, 7 * DAY_MS, 8 * DAY_MS)
CLOSE_STAGE = len(STAGE_DELAYS)
FINAL_NOTICE_MS = STAGE_DELAYS[-1] - STAGE_DELAYS[-2]
SWEEP_MS = HOUR_MS
REPOS_PER_RUN = 2
SCAN_PER_RUN = 30
CHECKS_PER_RUN = 3
MAX_REF_READS = 4
MAX_EVENTS = 60
BLOB_BATCH = 30
MAX_PENDING_BRANCH_OPS = 1024
RETAIN_MS = 60 * DAY_MS
PRUNE_INTERVAL_MS = HOUR_MS
COMMENT_TYPES = frozenset((
    "comment", "review", "line-comment", "thread-comment", "thread-reply"))
AUTHOR_REPLY_TYPES = COMMENT_TYPES | frozenset((
    "thread-state", "suggestion-state"))
PUSH_TYPES = frozenset(("synchronize", "revision"))
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,255}$")
EVENT_FILE_RE = re.compile(r"^\d+-[A-Za-z-]+\.md$")
FRONT_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]{0,63}):[ \t]?(.*)$")

_prune_memo = {"at": 0}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _now(now=None):
    return int(now if now is not None else Date.now())


def oid(value):
    text = str(value or "").strip().lower()
    return text if OID_RE.fullmatch(text) else ""


def branch_name(value):
    name = str(value or "").strip()
    if not BRANCH_RE.fullmatch(name):
        return ""
    if (name[0] in "-/." or name.endswith("/") or name.endswith(".lock")
            or ".." in name or "//" in name or "@{" in name):
        return ""
    return name


def event_ms(value):
    try:
        number = int(float(str(value or "").strip()))
    except ValueError:
        return 0
    if number <= 0:
        return 0
    return number * 1000 if number < 100_000_000_000 else number


def parse_front_matter(text):
    lines = str(text or "").splitlines()
    if not lines or lines[0].strip() != "---":
        return {}, ""
    values = {}
    for index, line in enumerate(lines[1:], 1):
        if line.strip() == "---":
            return values, "\n".join(lines[index + 1:]).strip()
        match = FRONT_RE.match(line)
        if match and match.group(1) not in values:
            values[match.group(1)] = match.group(2).strip()
    return {}, ""


def is_draft(fields):
    status = str(fields.get("status") or "").strip().lower()
    flag = str(fields.get("draft") or "").strip().lower()
    return status == "draft" or flag in ("true", "yes", "1")


def is_open(fields):
    return str(fields.get("status") or "open").strip().lower() == "open"


def waiting_anchor(events):
    anchor = None
    ordered = sorted(enumerate(events or []),
                     key=lambda item: (int(item[1].get("ts") or 0), item[0]))
    for _index, event in ordered:
        role = event.get("role")
        kind = str(event.get("type") or "")
        if role == "bot":
            continue
        if kind in PUSH_TYPES:
            anchor = None
        elif role == "author":
            if kind in AUTHOR_REPLY_TYPES:
                anchor = None
        elif role == "maintainer" and kind in COMMENT_TYPES:
            state = str(event.get("state") or "")
            anchor = None if kind == "review" and state == "approved" \
                else event
    return anchor


def next_stage(anchor_at, stage, stage_at, now):
    elapsed = int(now) - int(anchor_at)
    reached = sum(1 for delay in STAGE_DELAYS if elapsed >= delay)
    if reached <= int(stage):
        return 0
    if reached < CLOSE_STAGE:
        return reached
    if int(stage) < CLOSE_STAGE - 1:
        return CLOSE_STAGE - 1
    if int(now) - int(stage_at) < FINAL_NOTICE_MS:
        return 0
    return CLOSE_STAGE


def close_at(anchor_at, stage=0, stage_at=0):
    planned = int(anchor_at) + STAGE_DELAYS[-1]
    if int(stage) >= CLOSE_STAGE - 1:
        return max(planned, int(stage_at) + FINAL_NOTICE_MS)
    return planned


def _day(ms):
    return datetime.fromtimestamp(int(ms) / 1000, timezone.utc).strftime(
        "%Y-%m-%d %H:%M UTC")


def notice(stage, number, author, maintainer, anchor_at, closes_at,
           branch=""):
    who = "@" + author if author else "The author"
    by = maintainer or "a maintainer"
    since = _day(anchor_at)
    closes = _day(closes_at)
    rule = ("This repository closes pull requests that wait on their author "
            "for 8 days after a maintainer's comment or change request. "
            "A reply on this pull request or a new push stops the clock.")
    if stage == 1:
        return ("Pull request #%d is waiting on your reply" % number,
                "%s, %s left feedback on this pull request on %s and it is "
                "waiting for your reply. Please answer it or push an update."
                "\n\n%s If nothing happens, it closes on %s."
                % (who, by, since, rule, closes))
    if stage == 2:
        return ("Pull request #%d is still waiting on your reply" % number,
                "%s, reminder: %s's feedback from %s has had no reply for 3 "
                "days. Please answer it or push an update.\n\n%s If nothing "
                "happens, it closes on %s." % (who, by, since, rule, closes))
    if stage == 3:
        return ("Pull request #%d closes in 1 day" % number,
                "%s, final reminder: this pull request closes in 1 day (%s) "
                "unless you reply to %s's feedback from %s or push an update."
                "\n\n%s" % (who, closes, by, since, rule))
    lines = ["Closed because %s's feedback from %s had no reply from %s for "
             "8 days." % (by, since, who),
             "To carry on, push your update and open a new pull request that "
             "mentions #%d, or ask a maintainer to pick it up." % number]
    if branch:
        lines.append("The head branch `%s` is being deleted with it; the "
                     "commits stay in your own clone." % branch)
    lines.append(rule)
    return ("Pull request #%d was closed after 8 days without a reply"
            % number, "\n\n".join(lines))


def _changed(result):
    try:
        return int(result.meta.changes) > 0
    except Exception:
        return True


async def _open(env, row):
    try:
        data = await decrypt_row(env, (row or {}).get("data"))
    except Exception:
        data = None
    return data if isinstance(data, dict) else {}


async def repo_key(env, node, repo):
    return await blind_index(env, str(node or "").strip().lower() + "/" +
                             str(repo or "").strip())


async def _node(env, owner, repo):
    try:
        node = await _org_repo_node(env, owner, repo)
    except Exception:
        node = ""
    return str(node or owner or "").strip().lower()


async def enroll(env, owner, repo, features, public_owner="", now=None):
    await ensure_schema(env)
    now = _now(now)
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip()
    if not owner or not repo:
        return False
    node = await _node(env, owner, repo)
    repo_bi = await repo_key(env, node, repo)
    if not (features or {}).get(SETTING):
        await d1_run(env, "DELETE FROM stale_pull_repos WHERE repo_bi=?",
                     repo_bi)
        await d1_run(env, "DELETE FROM stale_pulls WHERE repo_bi=?", repo_bi)
        return False
    row = await d1_first(
        env, "SELECT data FROM stale_pull_repos WHERE repo_bi=?", repo_bi)
    data = await _open(env, row)
    data.update({"owner": owner, "node": node, "repo": repo,
                 "publicOwner": str(public_owner or "").strip().lower()
                 or data.get("publicOwner") or owner, "cursor": 0})
    data["enabledAt"] = int(data.get("enabledAt") or now)
    await d1_run(
        env,
        "INSERT INTO stale_pull_repos (repo_bi, next_at, updated_at, data) "
        "VALUES (?,?,?,?) ON CONFLICT(repo_bi) DO UPDATE SET "
        "next_at=excluded.next_at, updated_at=excluded.updated_at, "
        "data=excluded.data",
        repo_bi, now, now, await encrypt_row(env, data))
    return True


async def _read(env, owner, repo, operation, query):
    url = "%s/api/repo/%s/%s/%s?%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        operation, urlencode(query, doseq=True))
    try:
        response = await _routed_repository_read(
            env, url, operation, bypass_cache=True)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        value = json.loads(str(await response.text()))
    except Exception:
        return None
    return value if isinstance(value, dict) else None


def _blob_text(blob):
    if not isinstance(blob, dict) or blob.get("ok") is False:
        return ""
    content = blob.get("content")
    if not isinstance(content, str):
        return ""
    if blob.get("encoding") == "base64":
        try:
            return base64.b64decode(content).decode("utf-8", "replace")
        except Exception:
            return ""
    return content


async def list_pulls(env, owner, repo):
    tree = await _read(env, owner, repo, "tree",
                       {"path": PULLS_DIR, "ref": PULLS_REF})
    if not tree:
        return None
    refs = {}
    raw_refs = tree.get("pullRefs")
    for name, value in (raw_refs if isinstance(raw_refs, dict) else {}).items():
        if str(name).isdigit() and oid(value):
            refs[int(name)] = oid(value)
    numbers = {int(entry["name"]) for entry in tree.get("entries") or []
               if isinstance(entry, dict) and entry.get("type") == "tree"
               and str(entry.get("name") or "").isdigit()}
    numbers.update(refs)
    numbers = sorted((n for n in numbers if 0 < n < 10 ** 9), reverse=True)
    return numbers, oid(tree.get("commit")), refs


async def read_records(env, owner, repo, numbers, commit, refs):
    path = PULLS_DIR + "/%d/pull.md"
    records = {}
    indexed = [n for n in numbers if n not in refs]
    if indexed and commit:
        for start in range(0, len(indexed), BLOB_BATCH):
            chunk = indexed[start:start + BLOB_BATCH]
            found = await _read(env, owner, repo, "blobs", {
                "path": [path % n for n in chunk], "ref": commit})
            blobs = (found or {}).get("blobs") or {}
            for number in chunk:
                values, _body = parse_front_matter(
                    _blob_text(blobs.get(path % number)))
                if str(values.get("number") or "") == str(number):
                    records[number] = values
    reads, stop = 0, 0
    for number in numbers:
        if number not in refs:
            continue
        if reads >= MAX_REF_READS:
            stop = number
            break
        reads += 1
        found = await _read(env, owner, repo, "blobs", {
            "path": [path % number], "ref": refs[number]})
        values, _body = parse_front_matter(_blob_text(
            ((found or {}).get("blobs") or {}).get(path % number)))
        if str(values.get("number") or "") == str(number):
            records[number] = values
    return records, stop


async def conversation(env, owner, repo, number, ref):
    folder = "%s/%d" % (PULLS_DIR, int(number))
    tree = await _read(env, owner, repo, "tree", {"path": folder, "ref": ref})
    if tree is None:
        return None
    names = sorted(
        str(entry.get("name") or "") for entry in tree.get("entries") or []
        if isinstance(entry, dict) and entry.get("type") != "tree"
        and EVENT_FILE_RE.fullmatch(str(entry.get("name") or "")))
    paths = [folder + "/" + name for name in names[-MAX_EVENTS:]]
    blobs = {}
    for start in range(0, len(paths), BLOB_BATCH):
        found = await _read(env, owner, repo, "blobs", {
            "path": paths[start:start + BLOB_BATCH], "ref": ref})
        if found is None:
            return None
        blobs.update(found.get("blobs") or {})
    events = []
    for path in paths:
        values, _body = parse_front_matter(_blob_text(blobs.get(path)))
        if not values:
            continue
        events.append({
            "type": values.get("type") or "comment",
            "id": values.get("id") or path.rsplit("/", 1)[-1],
            "authorKey": values.get("author", ""),
            "authorName": str(values.get("authorName") or "").lower(),
            "state": values.get("state", ""),
            "ts": event_ms(values.get("ts")),
        })
    return events


async def _key_account(env, pass_state, key):
    key = str(key or "").strip()
    if not key or not valid_node_pubkey(key):
        return ""
    names = pass_state.setdefault("keys", {})
    if key not in names:
        try:
            name = str(await _git_push_account_for_pubkey(env, key) or "")
        except Exception:
            name = ""
        name = name.strip().lower()
        names[key] = name if valid_node_name(name) else ""
    return names[key]


async def _actor(env, pass_state, key, name):
    resolved = await _key_account(env, pass_state, key)
    if resolved:
        return resolved
    name = str(name or "").strip().lower()
    return name if valid_node_name(name) else ""


async def is_maintainer(env, pass_state, account):
    account = str(account or "").strip().lower()
    if not account:
        return False
    cache = pass_state.setdefault("maintainers", {})
    if account not in cache:
        node, repo = pass_state["node"], pass_state["repo"]
        try:
            cache[account] = bool(
                account == node
                or await _account_owns_node(env, account, node)
                or await _org_write_allowed(env, node, repo, account))
        except Exception:
            cache[account] = False
    return cache[account]


async def role_of(env, pass_state, event, record_key, author):
    key = str(event.get("authorKey") or "").strip()
    name = str(event.get("authorName") or "").strip().lower()
    if key == FORKBOT_AUTHOR or name == FORKBOT_AUTHOR:
        return "bot"
    if record_key and key == record_key:
        return "author"
    actor = await _actor(env, pass_state, key, name)
    if author and actor == author:
        return "author"
    if actor and await is_maintainer(env, pass_state, actor):
        return "maintainer"
    return "other"


async def _head(env, pass_state, record):
    head = str(record.get("head") or "").strip()
    if str(record.get("derive") or "") == "branch" and ":" not in head:
        branch = branch_name(head)
        if not branch:
            return ""
        tree = await _read(env, pass_state["owner"], pass_state["repo"], "tree",
                           {"ref": branch})
        return oid((tree or {}).get("commit"))
    return oid(record.get("headOid"))


async def _row(env, repo_bi, number):
    row = await d1_first(
        env, "SELECT anchor_at, stage, stage_at, version, data FROM "
        "stale_pulls WHERE repo_bi=? AND number=?", repo_bi, int(number))
    if not row:
        return None, {}
    return row, await _open(env, row)


async def _forget(env, repo_bi, number):
    await d1_run(env, "DELETE FROM stale_pulls WHERE repo_bi=? AND number=?",
                 repo_bi, int(number))


async def _swap(env, repo_bi, number, version, anchor_at, stage, stage_at,
                data, now):
    return _changed(await d1_run(
        env, "UPDATE stale_pulls SET anchor_at=?, stage=?, stage_at=?, "
        "version=version+1, updated_at=?, data=? WHERE repo_bi=? AND "
        "number=? AND version=?",
        int(anchor_at), int(stage), int(stage_at), int(now),
        await encrypt_row(env, data), repo_bi, int(number), int(version)))


async def _track(env, repo_bi, number, anchor, anchor_at, head, now):
    row, data = await _row(env, repo_bi, number)
    fresh = {"anchor": str(anchor.get("id") or ""),
             "maintainer": anchor.get("actor") or "", "head": head}
    if row is None:
        await d1_run(
            env, "INSERT OR IGNORE INTO stale_pulls (repo_bi, number, "
            "anchor_at, stage, stage_at, version, updated_at, data) "
            "VALUES (?,?,?,0,0,0,?,?)",
            repo_bi, int(number), int(anchor_at), int(now),
            await encrypt_row(env, fresh))
        return await _row(env, repo_bi, number)
    if data.get("anchor") != fresh["anchor"]:
        await _swap(env, repo_bi, number, row["version"], anchor_at, 0, 0,
                    fresh, now)
        return await _row(env, repo_bi, number)
    if head and head != data.get("head") and not data.get("answered"):
        data["answered"] = bool(data.get("head"))
        data["head"] = head
        await _swap(env, repo_bi, number, row["version"], row["anchor_at"],
                    row["stage"], row["stage_at"], data, now)
        return await _row(env, repo_bi, number)
    return row, data


def pull_href(pass_state, number):
    segment = NOTIFICATION_ITEM_URL_SEGMENTS.get("pull") or "pull"
    return "%s/%s/%d" % (repo_web_href(pass_state["public"], pass_state["repo"]), segment,
                         int(number))


async def _post(env, pass_state, events, now):
    repo_bi = pass_state["repo_bi"]
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM pull_inbox WHERE repo_bi=?", repo_bi)
    if count and int(count.get("c", 0) or 0) + len(events) > \
            MAX_PENDING_PULLS:
        return False
    submitter_bi = await blind_index(env, FORKBOT_AUTHOR)
    if await _inbox_author_over_quota(env, "pull_inbox", repo_bi,
                                      submitter_bi):
        return False
    for number, event in events:
        item = {"number": int(number), "event": event,
                "submitter": FORKBOT_AUTHOR, "submittedAt": now,
                "source": "stale-pull"}
        await d1_run(
            env, "INSERT INTO pull_inbox (repo_bi, data, submitter_bi) "
            "VALUES (?,?,?)", repo_bi, await encrypt_row(env, item),
            submitter_bi)
    for notify in (notify_repo_host, notify_repo_mirrors):
        try:
            await notify(env, pass_state["node"], pass_state["repo"], "pulls")
        except Exception:
            pass
    return True


async def deletable_head(env, pass_state, record, head):
    if not pass_state["features"].get(DELETE_SETTING) or not oid(head):
        return ""
    raw = str(record.get("head") or "").strip()
    branch = branch_name(raw)
    if (str(record.get("derive") or "") != "branch" or ":" in raw
            or not branch or branch.startswith("forkmesh/")):
        return ""
    node, repo = pass_state["node"], pass_state["repo"]
    try:
        catalog = await _repo_catalog_record(env, node, repo)
        if isinstance(catalog, dict) and catalog.get("source") == "hosted-web":
            return ""
        default = await _branch_protection.default_branch(env, node, repo)
        if not default or branch == default:
            return ""
        refs, problem = await _branch_protection.push_protection(
            env, pass_state["owner"], repo)
        if problem or _branch_protection.branch_is_protected(branch, refs):
            return ""
        rows = await d1_all(
            env, "SELECT data FROM branch_op_inbox WHERE repo_bi=? LIMIT ?",
            pass_state["repo_bi"], MAX_PENDING_BRANCH_OPS)
    except Exception:
        return ""
    rows = list(rows or [])
    if len(rows) >= MAX_PENDING_BRANCH_OPS:
        return ""
    for row in rows:
        op = await _open(env, row)
        if op.get("op") == "delete" and op.get("branch") == branch:
            return ""
    return branch


async def _delete_branch(env, pass_state, branch, head, now):
    record = {"op": "delete", "branch": branch, "expectedOid": oid(head),
              "requestedBy": FORKBOT_AUTHOR, "queuedAt": now,
              "source": "stale-pull"}
    await d1_run(
        env, "INSERT INTO branch_op_inbox (repo_bi, data, queued_at) "
        "VALUES (?,?,?)", pass_state["repo_bi"], await encrypt_row(env, record), now)
    try:
        await notify_repo_host(env, pass_state["node"], pass_state["repo"], "branches")
    except Exception:
        pass


async def _deliver(env, pass_state, number, record, author, data, row, stage, head,
                   now):
    anchor_at = int(row["anchor_at"])
    closes_at = close_at(anchor_at, stage, now)
    branch = ""
    if stage == CLOSE_STAGE:
        branch = await deletable_head(env, pass_state, record, head)
    title, body = notice(stage, number, author, data.get("maintainer") or "",
                         anchor_at, closes_at, branch)
    base_id = "stale-pull-%d-%d" % (int(number), anchor_at)
    events = [(number, {
        "type": "comment", "id": "%s-%d" % (base_id, stage),
        "author": FORKBOT_AUTHOR, "authorName": BOT_NAME, "ts": now,
        "body": body, "sig": ""})]
    if stage == CLOSE_STAGE:
        events.append((number, {
            "type": "status", "state": "closed", "id": base_id + "-close",
            "author": FORKBOT_AUTHOR, "authorName": BOT_NAME, "ts": now,
            "body": "", "sig": ""}))
    if not await _post(env, pass_state, events, now):
        return False
    if author:
        try:
            await enqueue_notification(
                env, author, KIND, title, body=body,
                repo="%s/%s" % (pass_state["public"], pass_state["repo"]),
                href=pull_href(pass_state, number), actor=FORKBOT_AUTHOR,
                source="pull",
                dedupe="pull-stale:%s/%s#%d:%d:%d" % (
                    pass_state["node"], pass_state["repo"], int(number), anchor_at, stage),
                meta={"number": int(number), "source": "pull",
                      "reason": "stale", "stage": stage,
                      "closesAt": closes_at})
        except Exception:
            pass
    if branch:
        try:
            await _delete_branch(env, pass_state, branch, head, now)
        except Exception:
            pass
    return True


async def check(env, pass_state, number, record, now):
    repo_bi = pass_state["repo_bi"]
    if not is_open(record) or is_draft(record):
        await _forget(env, repo_bi, number)
        return "skipped"
    record_key = str(record.get("author") or "").strip()
    author = await _actor(env, pass_state, record_key, record.get("authorName"))
    if not author and not record_key:
        await _forget(env, repo_bi, number)
        return "skipped"
    if author and await is_maintainer(env, pass_state, author):
        await _forget(env, repo_bi, number)
        return "maintainer"
    ref = pass_state["refs"].get(number) or pass_state["commit"]
    if not ref:
        return "unreadable"
    events = await conversation(env, pass_state["owner"], pass_state["repo"], number, ref)
    if events is None:
        return "unreadable"
    for event in events:
        event["role"] = await role_of(env, pass_state, event, record_key, author)
        if event["role"] == "maintainer":
            event["actor"] = await _actor(
                env, pass_state, event.get("authorKey"), event.get("authorName"))
    anchor = waiting_anchor(events)
    if anchor is None or not anchor.get("ts"):
        await _forget(env, repo_bi, number)
        return "clear"
    anchor_at = max(int(anchor["ts"]), int(pass_state.get("enabledAt") or 0))
    head = await _head(env, pass_state, record)
    row, data = await _track(env, repo_bi, number, anchor, anchor_at, head,
                             now)
    if row is None:
        return "raced"
    if data.get("answered"):
        return "answered"
    stage = next_stage(row["anchor_at"], row["stage"], row["stage_at"], now)
    if not stage:
        return "waiting"
    if not await _swap(env, repo_bi, number, row["version"], row["anchor_at"],
                       stage, now, data, now):
        return "raced"
    if not await _deliver(env, pass_state, number, record, author, data, row, stage,
                          head, now):
        await _swap(env, repo_bi, number, int(row["version"]) + 1,
                    row["anchor_at"], row["stage"], row["stage_at"], data,
                    now)
        return "failed"
    return "closed" if stage == CLOSE_STAGE else "warned"


async def _drop_repo(env, repo_bi):
    await d1_run(env, "DELETE FROM stale_pull_repos WHERE repo_bi=?", repo_bi)
    await d1_run(env, "DELETE FROM stale_pulls WHERE repo_bi=?", repo_bi)


async def _schedule(env, repo_bi, data, next_at, now):
    await d1_run(
        env, "UPDATE stale_pull_repos SET next_at=?, updated_at=?, data=? "
        "WHERE repo_bi=?", int(next_at), int(now),
        await encrypt_row(env, data), repo_bi)


async def sweep(env, row, now, totals):
    repo_bi = row.get("repo_bi")
    data = await _open(env, row)
    owner, repo = data.get("owner") or "", data.get("repo") or ""
    if not owner or not repo:
        await _drop_repo(env, repo_bi)
        return
    features = await _repo_features_get(env, owner, repo)
    if not features.get(SETTING):
        await _drop_repo(env, repo_bi)
        return
    listing = await list_pulls(env, owner, repo)
    if listing is None:
        await _schedule(env, repo_bi, data, now + SWEEP_MS, now)
        return
    numbers, commit, refs = listing
    cursor = int(data.get("cursor") or 0)
    pending = [n for n in numbers if not cursor or n < cursor]
    batch = pending[:SCAN_PER_RUN]
    records, stop = await read_records(env, owner, repo, batch, commit,
                                       refs)
    pass_state = {"owner": owner, "repo": repo, "node": data.get("node") or owner,
           "public": data.get("publicOwner") or owner, "repo_bi": repo_bi,
           "features": features, "enabledAt": int(data.get("enabledAt") or 0),
           "commit": commit, "refs": refs}
    done, checks, settled = 0, 0, []
    for number in batch:
        if number == stop:
            break
        record = records.get(number)
        if record is not None and is_open(record) and not is_draft(record):
            if checks >= CHECKS_PER_RUN or _cron_budget_exhausted():
                break
            checks += 1
            outcome = await check(env, pass_state, number, record, now)
            totals[outcome] = totals.get(outcome, 0) + 1
        elif record is not None:
            settled.append(int(number))
        done += 1
    if settled:
        await d1_run(
            env, "DELETE FROM stale_pulls WHERE repo_bi=? AND number IN (%s)"
            % ",".join("?" * len(settled)), repo_bi, *settled)
    totals["checked"] = totals.get("checked", 0) + checks
    if done >= len(pending):
        data["cursor"] = 0
        await _schedule(env, repo_bi, data, now + SWEEP_MS, now)
    else:
        data["cursor"] = batch[done - 1] if done else cursor
        await _schedule(env, repo_bi, data, now, now)


async def _prune(env, now):
    if now - _prune_memo["at"] < PRUNE_INTERVAL_MS:
        return
    _prune_memo["at"] = now
    await d1_run(env, "DELETE FROM stale_pulls WHERE updated_at<?",
                 now - RETAIN_MS)


async def cron(env, now=None):
    await ensure_schema(env)
    now = _now(now)
    rows = await d1_all(
        env, "SELECT repo_bi, data FROM stale_pull_repos WHERE next_at<=? "
        "ORDER BY next_at LIMIT ?", now, REPOS_PER_RUN)
    totals = {"ok": True, "repos": 0}
    for row in rows or []:
        if _cron_budget_exhausted():
            break
        try:
            await sweep(env, row, now, totals)
        except Exception:
            if _cron_budget_exhausted():
                raise
            await d1_run(
                env, "UPDATE stale_pull_repos SET next_at=? WHERE repo_bi=?",
                now + SWEEP_MS, row.get("repo_bi"))
        totals["repos"] += 1
    if not _cron_budget_exhausted():
        await _prune(env, now)
    return totals
