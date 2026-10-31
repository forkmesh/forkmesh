from __future__ import annotations

import contextvars
import html
import json

CODE = "read_only"
MESSAGE = ("ForkMesh is read-only while servers recover. You can keep "
           "reading; saving changes will work again in a few minutes.")
DETAIL = ("D1_ERROR: D1 DB is overloaded (read_only: ForkMesh is read-only "
          "while servers recover)")
HEADER = "x-forkmesh-read-only"
RETRY_AFTER = "30"
SAFE_METHODS = frozenset({"GET", "HEAD", "OPTIONS"})
WRITING_GET_PATHS = frozenset({
    "/api/accounts/verify-email",
    "/api/marketing-email/unsubscribe",
    "/api/integrations/patreon/callback",
    "/api/integrations/discord/callback",
})
READING_POST_SUFFIXES = ("/git-upload-pack",)
STATS = {"refused": 0, "skipped": 0, "answered": 0}

_REQUEST: contextvars.ContextVar = contextvars.ContextVar(
    "forkmesh_read_only_request", default=None)


class request_scope:
    def __init__(self, method="GET", path="/"):
        self.state = {"method": str(method or "GET").upper(),
                      "path": str(path or "/"), "refused": 0, "skipped": 0}

    def __enter__(self):
        self._token = _REQUEST.set(self.state)
        return self.state

    def __exit__(self, *exc):
        _REQUEST.reset(self._token)


def current():
    return _REQUEST.get()


def reads_only(method, path):
    method = str(method or "").upper()
    path = str(path or "/")
    if method in SAFE_METHODS:
        return path.rstrip("/") not in WRITING_GET_PATHS
    return method == "POST" and path.endswith(READING_POST_SUFFIXES)


def skips_writes():
    state = _REQUEST.get()
    return state is not None and reads_only(state["method"], state["path"])


def refuse(error_class):
    state = _REQUEST.get()
    kind = "skipped" if skips_writes() else "refused"
    STATS[kind] += 1
    if state is not None:
        state[kind] += 1
    if kind == "refused":
        raise error_class(DETAIL)
    return True


def wants_html(accept):
    return "text/html" in str(accept or "").lower()


def payload():
    return {"error": MESSAGE, "message": MESSAGE, "code": CODE,
            "readOnly": True, "retryAfter": int(RETRY_AFTER)}


def page():
    text = html.escape(MESSAGE)
    return ("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, "
            "initial-scale=1\"><title>ForkMesh is read-only</title>"
            "<style>body{font-family:system-ui,sans-serif;max-width:36rem;"
            "margin:15vh auto;padding:0 1.5rem;line-height:1.5}"
            "h1{font-size:1.4rem}</style></head><body>"
            "<h1>ForkMesh is read-only for now</h1><p>%s</p></body></html>"
            % text)


def answer(accept):
    if wants_html(accept):
        body = page().encode()
        content_type = "text/html; charset=utf-8"
    else:
        body = json.dumps(payload()).encode()
        content_type = "application/json"
    headers = {"content-type": content_type, "cache-control": "no-store",
               "retry-after": RETRY_AFTER, HEADER: "1",
               "x-forkmesh-expected-degraded": "1"}
    return 503, headers, body
