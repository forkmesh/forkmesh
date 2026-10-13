import contextlib
import contextvars
import hashlib
import urllib.parse

HEADER = "x-fm-d1-caller"
LABEL_MAX_CHARS = 200
PATH_MAX_CHARS = 120
SESSION_PREFIX_CHARS = 8
DONE_PREFIX = "D1-done"

_done = {"on": False, "max_chars": 360}

_CALLER = contextvars.ContextVar("forkmesh_d1_caller", default=None)


def _clean(text, limit):
    text = str(text or "").replace("(", " ").replace(")", " ")
    return " ".join(text.split())[:limit]


@contextlib.contextmanager
def scope(label=""):
    token = _CALLER.set({"label": _clean(label, LABEL_MAX_CHARS),
                         "user": "", "session": "",
                         "db_count": 0, "db_ms": 0.0})
    try:
        yield
    finally:
        _CALLER.reset(token)


@contextlib.contextmanager
def request_scope(request):
    holder = _CALLER.get()
    if holder is None:
        with scope(request_label(request)):
            yield
        return
    if not holder["label"]:
        holder["label"] = _clean(request_label(request), LABEL_MAX_CHARS)
    yield


def request_label(request):
    try:
        method = str(request.method or "GET").upper()
        path = urllib.parse.urlsplit(str(request.url)).path or "/"
        address = str(request.headers.get("cf-connecting-ip") or "")
    except Exception:
        return ""
    fields = ["%s %s" % (method, path[:PATH_MAX_CHARS])]
    if address:
        fields.append("IP " + address)
    return ", ".join(fields)


def note_session(name, session_id):
    holder = _CALLER.get()
    if holder is None:
        return
    holder["user"] = _clean(name, 64)
    holder["session"] = _clean(session_id, 64)[:SESSION_PREFIX_CHARS]


def note_query(elapsed_ms, count=1):
    holder = _CALLER.get()
    if holder is None:
        return
    holder["db_count"] = holder.get("db_count", 0) + int(count)
    holder["db_ms"] = holder.get("db_ms", 0.0) + float(elapsed_ms)


def timing():
    holder = _CALLER.get()
    if holder is None:
        return 0, 0.0
    return holder.get("db_count", 0), holder.get("db_ms", 0.0)


def clause():
    holder = _CALLER.get()
    if holder is None:
        return ""
    fields = [holder["label"]] if holder["label"] else []
    if holder["user"]:
        fields.append("user " + holder["user"])
    if holder["session"]:
        fields.append("session " + holder["session"])
    return _clean(", ".join(fields), LABEL_MAX_CHARS + 100)


def header():
    text = clause()
    return [(HEADER, text)] if text else []


def enable_done(max_chars=360):
    _done["on"] = True
    _done["max_chars"] = max(8, int(max_chars or 360))


def disable_done():
    _done["on"] = False


def statement_key(sql, max_chars=None):
    limit = _done["max_chars"] if max_chars is None else max_chars
    text = " ".join(str(sql or "").split())
    if len(text) > limit:
        text = text[:limit - 3] + "..."
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:10]


def done_line(sql, elapsed_ms, rows=0, changes=0, served="", failed=False):
    fields = ["%.1f ms" % max(0.0, float(elapsed_ms)),
              "%d rows" % max(0, int(rows or 0)),
              "%d changed" % max(0, int(changes or 0))]
    if served:
        fields.append(_clean(served, 24))
    if failed:
        fields.append("failed")
    return "%s %s #%s" % (DONE_PREFIX, ", ".join(fields), statement_key(sql))


def note_done(sql, elapsed_ms, rows=0, changes=0, served="", failed=False):
    if not _done["on"]:
        return
    print(done_line(sql, elapsed_ms, rows, changes, served, failed))
