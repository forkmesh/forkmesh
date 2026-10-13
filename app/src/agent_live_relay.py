import asyncio
import json


PUBLISH_PATH = "/api/nodes/agent-live/publish"
WATCH_PATH = "/api/nodes/agent-live/watch"
KEEPALIVE_SECONDS = 15
STREAM_SECONDS = 300
RETRY_MS = 1000
PERSIST_MS = 2500
MAX_SESSIONS = 64
MAX_WATCHERS = 16
MAX_SESSION_ID = 64
MAX_TEXT = 16000
LIVE_STATUSES = ("queued", "running")
SSE_HEADERS = {
    "content-type": "text/event-stream; charset=utf-8",
    "cache-control": "no-store",
    "x-accel-buffering": "no",
}


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def session_key(value):
    value = str(value or "").strip()
    if not value or len(value) > MAX_SESSION_ID:
        return ""
    return value


def frame(event, payload):
    return ("event: %s\ndata: %s\n\n" % (
        event, json.dumps(payload, separators=(",", ":"),
                          ensure_ascii=False))).encode("utf-8")


def ended_stream(status):
    async def frames():
        yield ("retry: %d\n\n" % RETRY_MS).encode("ascii")
        yield frame("end", {"status": str(status or "")})
    return frames()


class Relay:
    def __init__(self):
        self.sessions = {}

    def watchers(self, session_id):
        entry = self.sessions.get(session_id)
        return entry["watchers"] if entry else 0

    def admits(self, session_id):
        entry = self.sessions.get(session_id)
        if entry is None:
            return len(self.sessions) < MAX_SESSIONS
        return entry["watchers"] < MAX_WATCHERS

    def publish(self, session_id, at, text, status=""):
        entry = self.sessions.get(session_id)
        if entry is None:
            return 0
        text = str(text or "")[-MAX_TEXT:]
        if text and at >= entry["at"]:
            entry["at"], entry["text"] = at, text
        status = str(status or "")
        if status and status not in LIVE_STATUSES:
            entry["status"] = status
        changed, entry["changed"] = entry["changed"], asyncio.Event()
        changed.set()
        return entry["watchers"]

    def _join(self, session_id, at, text):
        entry = self.sessions.get(session_id)
        if entry is None:
            entry = {"at": 0, "text": "", "status": "", "watchers": 0,
                     "changed": asyncio.Event()}
            self.sessions[session_id] = entry
        text = str(text or "")[-MAX_TEXT:]
        if text and at > entry["at"]:
            entry["at"], entry["text"] = at, text
        entry["watchers"] += 1
        return entry

    def _leave(self, session_id, entry):
        entry["watchers"] -= 1
        if entry["watchers"] <= 0 and self.sessions.get(session_id) is entry:
            del self.sessions[session_id]

    async def watch(self, session_id, at=0, text="",
                    keepalive=KEEPALIVE_SECONDS, lifetime=STREAM_SECONDS):
        entry = self._join(session_id, _int(at), text)
        try:
            yield ("retry: %d\n\n" % RETRY_MS).encode("ascii")
            loop = asyncio.get_running_loop()
            deadline = loop.time() + lifetime
            sent = ""
            while True:
                changed = entry["changed"]
                current = entry["text"]
                if current and current != sent:
                    if sent and current.startswith(sent):
                        payload = {"at": entry["at"],
                                   "append": current[len(sent):]}
                    else:
                        payload = {"at": entry["at"], "text": current}
                    sent = current
                    yield frame("live", payload)
                if entry["status"]:
                    yield frame("end", {"status": entry["status"]})
                    return
                remaining = deadline - loop.time()
                if remaining <= 0:
                    return
                try:
                    await asyncio.wait_for(changed.wait(),
                                           min(keepalive, remaining))
                except asyncio.TimeoutError:
                    yield b": keepalive\n\n"
        finally:
            self._leave(session_id, entry)


def relay_for(target):
    relay = getattr(target, "_agent_live_relay", None)
    if relay is None:
        relay = Relay()
        target._agent_live_relay = relay
    return relay


def parse_body(raw):
    try:
        data = json.loads(raw or "{}")
    except (TypeError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


async def handle(target, request, path):
    data = parse_body(await request.text())
    session_id = session_key(data.get("session"))
    if not session_id:
        return json_response({"error": "invalid_session"}, status=400)  # noqa: F821
    relay = relay_for(target)
    if path == PUBLISH_PATH:
        return json_response({"ok": True, "watchers": relay.publish(  # noqa: F821
            session_id, _int(data.get("at")), data.get("text"),
            data.get("status"))})
    if not relay.admits(session_id):
        return json_response({"error": "too_many_watchers"}, status=429)  # noqa: F821
    return Response(  # noqa: F821
        ReadableStream(relay.watch(  # noqa: F821
            session_id, data.get("at"), data.get("text"))),
        headers=dict(SSE_HEADERS))
