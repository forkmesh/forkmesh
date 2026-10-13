import base64
import binascii
import re


MAX_CHUNK_BYTES = 8192
RING_CHUNKS = 256
READ_PAGE = 64
WATCH_MS = 30_000
ENDED_KEEP_MS = 2 * 60 * 1000
STALE_MS = 3 * 60 * 60 * 1000
MAX_WATCHERS = 8
TOPIC = "cloud-agent-stream"
LIVE_STATUSES = ("queued", "running")

_TOKEN_PATTERNS = (
    re.compile(rb"sk-ant-[A-Za-z0-9_-]{6,}"),
    re.compile(rb"sk-(?:proj-)?[A-Za-z0-9_-]{20,}"),
    re.compile(rb"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}"),
)


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def scrub(raw):
    for pattern in _TOKEN_PATTERNS:
        raw = pattern.sub(b"[redacted]", raw)
    return raw


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


async def sweep(store, now):
    await store.d1_run(
        "DELETE FROM agent_stream_watchers WHERE until<?", now)
    await store.d1_run(
        "DELETE FROM agent_stream_chunks WHERE session_id IN ("
        "SELECT session_id FROM agent_stream_state WHERE "
        "(ended_at>0 AND ended_at<?) OR updated_at<?)",
        now - ENDED_KEEP_MS, now - STALE_MS)
    await store.d1_run(
        "DELETE FROM agent_stream_state WHERE (ended_at>0 AND ended_at<?) "
        "OR updated_at<?", now - ENDED_KEEP_MS, now - STALE_MS)


async def _state(store, session_id):
    return await store.d1_first(
        "SELECT * FROM agent_stream_state WHERE session_id=?", session_id)


async def _ensure_state(store, session_id, job_id, now):
    await store.d1_run(
        "INSERT INTO agent_stream_state (session_id,job_id,created_at,"
        "updated_at) VALUES (?,?,?,?) ON CONFLICT(session_id) DO NOTHING",
        session_id, job_id, now, now)
    return await _state(store, session_id)


async def _ring(store, session_id, now):
    rows = await store.d1_all(
        "SELECT account FROM agent_stream_watchers WHERE session_id=? AND "
        "until>=? ORDER BY until DESC LIMIT %d" % MAX_WATCHERS,
        session_id, now)
    for row in rows or []:
        try:
            await store.notify(str(row.get("account") or ""), TOPIC)
        except Exception:
            pass


async def accept(store, session_id, job_id, data):
    now = store.now()
    await sweep(store, now)
    seq = _int(data.get("seq"), -1)
    if seq < 0:
        return 400, {"ok": False, "error": "invalid_seq"}
    chunk = b""
    encoded = data.get("data")
    if seq > 0:
        try:
            chunk = base64.b64decode(str(encoded or ""), validate=True)
        except (binascii.Error, ValueError):
            chunk = b""
        if not chunk or len(chunk) > MAX_CHUNK_BYTES:
            return 400, {"ok": False, "error": "invalid_data"}
    state = await _ensure_state(store, session_id, job_id, now)
    current_job = _int(state.get("job_id"))
    if job_id < current_job:
        return 409, {"ok": False, "error": "stale_job"}
    if job_id > current_job:
        await store.d1_run(
            "UPDATE agent_stream_state SET job_id=?,job_seq=0,ended_at=0,"
            "updated_at=? WHERE session_id=? AND job_id=?",
            job_id, now, session_id, current_job)
        state = await _state(store, session_id)
        if _int(state.get("job_id")) != job_id:
            return 409, {"ok": False, "error": "stale_job"}
        await store.d1_run(
            "DELETE FROM agent_stream_chunks WHERE session_id=? AND job_id<?",
            session_id, job_id)
    if _int(state.get("ended_at")):
        return 409, {"ok": False, "error": "stream_ended"}
    job_seq = _int(state.get("job_seq"))
    if seq > job_seq + 1:
        return 409, {"ok": False, "error": "out_of_order"}
    if seq == job_seq + 1:
        relay_seq = _int(state.get("seq")) + 1
        result = await store.d1_run(
            "UPDATE agent_stream_state SET job_seq=?,seq=?,updated_at=? "
            "WHERE session_id=? AND job_id=? AND job_seq=? AND seq=?",
            seq, relay_seq, now, session_id, job_id, job_seq, relay_seq - 1)
        if store.changes(result) == 1:
            await store.d1_run(
                "INSERT OR REPLACE INTO agent_stream_chunks (session_id,seq,"
                "job_id,data,created_at) VALUES (?,?,?,?,?)",
                session_id, relay_seq, job_id,
                base64.b64encode(scrub(chunk)).decode("ascii"), now)
            await store.d1_run(
                "DELETE FROM agent_stream_chunks WHERE session_id=? AND seq<=?",
                session_id, relay_seq - RING_CHUNKS)
            await _ring(store, session_id, now)
            job_seq = seq
    else:
        await store.d1_run(
            "UPDATE agent_stream_state SET updated_at=? WHERE session_id=?",
            now, session_id)
    state = await _state(store, session_id)
    return 200, {
        "ok": True,
        "watched": _int((state or {}).get("watched_until")) >= now,
        "seq": job_seq,
    }


async def read(store, session_id, viewer, after, status):
    now = store.now()
    await sweep(store, now)
    live = str(status or "") in LIVE_STATUSES
    viewer = str(viewer or "").strip().lower()
    if live:
        until = now + WATCH_MS
        await store.d1_run(
            "INSERT INTO agent_stream_state (session_id,watched_until,"
            "created_at,updated_at) VALUES (?,?,?,?) ON CONFLICT(session_id) "
            "DO UPDATE SET watched_until=excluded.watched_until,"
            "updated_at=excluded.updated_at", session_id, until, now, now)
        if viewer:
            await store.d1_run(
                "INSERT INTO agent_stream_watchers (session_id,account,until) "
                "VALUES (?,?,?) ON CONFLICT(session_id,account) "
                "DO UPDATE SET until=excluded.until", session_id, viewer, until)
    state = await _state(store, session_id) or {}
    last = _int(state.get("seq"))
    after = max(0, _int(after))
    reset = after > last
    if reset:
        after = 0
    job = _int(state.get("job_id"))
    ended = _int(state.get("ended_at"))
    rows = await store.d1_all(
        "SELECT seq,job_id,data FROM agent_stream_chunks WHERE session_id=? "
        "AND job_id=? AND seq>? ORDER BY seq LIMIT %d" % READ_PAGE,
        session_id, job, after) if job and not ended else []
    chunks = [{"seq": _int(r["seq"]), "job": _int(r["job_id"]),
               "data": r["data"]} for r in rows or []]
    first = chunks[0]["seq"] if chunks else 0
    return {
        "ok": True,
        "stream": _int(state.get("created_at")),
        "job": job,
        "chunks": chunks,
        "nextSeq": chunks[-1]["seq"] if chunks else after,
        "lastSeq": last,
        "reset": reset,
        "truncated": bool(chunks) and first > max(
            after, last - _int(state.get("job_seq"))) + 1,
        "more": len(chunks) == READ_PAGE,
        "live": live and not ended,
        "watchMs": WATCH_MS if live else 0,
    }


async def end(store, session_id, now):
    await store.d1_run(
        "UPDATE agent_stream_state SET ended_at=?,updated_at=? "
        "WHERE session_id=? AND ended_at=0", now, now, session_id)
    await store.d1_run(
        "DELETE FROM agent_stream_watchers WHERE session_id=?", session_id)


async def forget(store, session_id):
    await store.d1_run(
        "DELETE FROM agent_stream_chunks WHERE session_id=?", session_id)
    await store.d1_run(
        "DELETE FROM agent_stream_watchers WHERE session_id=?", session_id)
    await store.d1_run(
        "DELETE FROM agent_stream_state WHERE session_id=?", session_id)


class WorkerStore:
    def __init__(self, env):
        self.env = env

    def now(self):
        return int(Date.now())  # noqa: F821

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)  # noqa: F821

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)  # noqa: F821

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)  # noqa: F821

    async def notify(self, account, topic):
        await notify_account_event(self.env, account, topic)  # noqa: F821

    @staticmethod
    def changes(result):
        try:
            return int(result.meta.changes)
        except Exception:
            try:
                converted = result.to_py() if hasattr(result, "to_py") else result
                return int((converted.get("meta") or {}).get("changes") or 0)
            except Exception:
                return 0
