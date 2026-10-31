import asyncio
import hashlib
import json
import re
import urllib.parse

PATH_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/moderation$")
HIDDEN_TEXT = "*This post was hidden by the moderators.*"
FRESH_SECONDS = 5.0
STALE_SECONDS = 300.0
BUDGET_SECONDS = 0.15
MAX_ENTRIES = 1024
MAX_BODY = 128 * 1024
MAX_ITEMS = 500
_states = {}
_loading = {}


class Read:
    def __init__(self, key, task, stale):
        self.key = key
        self.task = task
        self.stale = stale


def _key(owner, repo, number):
    return ("%s/%s|issue:%d" % (owner, repo, number)).lower()


def forget(scope):
    if scope.get("method", "GET").upper() == "GET":
        return
    match = PATH_RE.match(scope.get("path") or "")
    if not match:
        return
    prefix = ("%s/%s|" % (urllib.parse.unquote(match.group(1)),
                          urllib.parse.unquote(match.group(2)))).lower()
    for key in [k for k in _states if k.startswith(prefix)]:
        _states.pop(key, None)


def clean(data):
    if not isinstance(data, dict):
        return None
    hidden = data.get("hidden")
    authors = data.get("hiddenAuthors")
    if not isinstance(hidden, list) or not isinstance(authors, list):
        return None
    return {"hidden": sorted({str(item) for item in hidden[:MAX_ITEMS]}),
            "hiddenAuthors": sorted({str(item).lower()
                                     for item in authors[:MAX_ITEMS]}),
            "locked": bool(data.get("locked"))}


def active(state):
    return bool(state and (state["hidden"] or state["hiddenAuthors"]))


def digest(state):
    if not active(state):
        return ""
    return hashlib.sha256(json.dumps(
        [state["hidden"], state["hiddenAuthors"]]).encode()).hexdigest()[:16]


def apply(text, state):
    if not active(state):
        return text
    try:
        record = json.loads(text)
    except (TypeError, ValueError):
        return text
    if not isinstance(record, dict) or not isinstance(
            record.get("events"), list):
        return text
    hidden = set(state["hidden"])
    authors = set(state["hiddenAuthors"])
    events = record["events"]
    opened = next((e for e in events if isinstance(e, dict)
                   and e.get("type") == "open"), {})
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
        return text
    patched = []
    for event in events:
        if isinstance(event, dict):
            kind = event.get("type")
            event_id = str(event.get("id") or "")
            target = str(event.get("target") or "")
            if (kind in ("open", "comment") and event_id in covered) or (
                    kind == "edit" and (target in covered or (
                        not target and open_id in covered))):
                event = dict(event, body=HIDDEN_TEXT, attachments=[],
                             moderationHidden=True)
        patched.append(event)
    record["events"] = patched
    if open_id in covered and "body" in record:
        record["body"] = HIDDEN_TEXT
    return json.dumps(record)


async def _load(worker_fetch, origin, owner, repo, number, key):
    from . import js as _js
    parts = urllib.parse.urlsplit(origin)
    headers = _js.Headers({"host": parts.netloc,
                           "user-agent": "forkmesh-host-first-view",
                           "cf-connecting-ip": "127.0.0.1",
                           "accept": "application/json"})
    url = "%s/api/repo/%s/%s/moderation?%s" % (
        origin, urllib.parse.quote(owner, safe=""),
        urllib.parse.quote(repo, safe=""),
        urllib.parse.urlencode([("kind", "issue"), ("number", str(number))]))
    response = await worker_fetch(_js.Request(url, "GET", headers))
    if int(response.status) != 200:
        return None
    body = await response._read()
    if len(body) > MAX_BODY:
        return None
    state = clean(json.loads(body))
    if state is None:
        return None
    if len(_states) >= MAX_ENTRIES:
        _states.clear()
    _states[key] = (asyncio.get_running_loop().time(), state)
    return state


def read(worker_fetch, origin, owner, repo, view, number):
    if view != "issue" or not number or not origin or worker_fetch is None:
        return None
    key = _key(owner, repo, number)
    loop = asyncio.get_running_loop()
    entry = _states.get(key)
    age = loop.time() - entry[0] if entry is not None else None
    stale = entry[1] if age is not None and age < STALE_SECONDS else None
    if age is not None and age < FRESH_SECONDS:
        done = loop.create_future()
        done.set_result(entry[1])
        return Read(key, done, stale)
    task = _loading.get(key)
    if task is None:
        task = asyncio.ensure_future(_load(
            worker_fetch, origin, owner, repo, number, key))
        _loading[key] = task
        task.add_done_callback(lambda _t: _loading.pop(key, None))
    return Read(key, task, stale)


def cached(owner, repo, number):
    try:
        entry = _states.get(_key(owner, repo, int(number)))
    except (TypeError, ValueError):
        return None
    return entry[1] if entry is not None else None


async def within(read_state, seconds=BUDGET_SECONDS):
    if read_state is None:
        return None
    try:
        return await asyncio.wait_for(asyncio.shield(read_state.task), seconds)
    except Exception:
        return read_state.stale
