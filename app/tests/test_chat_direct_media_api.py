import asyncio
import base64
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = Path(__file__).resolve().parent
for path in (SRC, TESTS):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import chat_direct_media_api as media  # noqa: E402
from test_chat_direct_messages_api import FakeRuntime, start_dm  # noqa: E402


DAY = 24 * 60 * 60 * 1000
PART = media.MEDIA_PART_BYTES


class MediaRuntime(FakeRuntime):
    def __init__(self):
        super().__init__()
        self.db.executescript(
            (ROOT / "migrations" / "0258_chat_direct_media.sql")
            .read_text(encoding="utf-8")
            .split("INSERT INTO schema_meta")[0]
        )
        self.media_announcements = []

    async def announce_media(
            self, conversation_id, key_version, account_bi, media_id, state):
        self.media_announcements.append(
            (conversation_id, key_version, account_bi, media_id, state))


def run(coro):
    return asyncio.run(coro)


def base(conversation_id):
    return f"/api/chat/direct-messages/{conversation_id}/media"


def call(runtime, method, actor, path, data=None, query=None):
    return run(media.handle(runtime.use(method, actor, data, query), path))


def conversation(runtime, actor="alice", other="bob"):
    started = run(start_dm(runtime, actor, other))
    assert started["status"] in (200, 201)
    return started["data"]["conversation"]["id"]


def create(runtime, cid, actor="alice", kind="voice", size=None,
           duration_ms=4000):
    size = PART + 100 if size is None else size
    return call(runtime, "POST", actor, base(cid), {
        "kind": kind,
        "size": size,
        "parts": (size + PART - 1) // PART,
        "durationMs": duration_ms,
    })


def blob(size):
    return bytes((index * 7) % 251 for index in range(size))


def upload(runtime, cid, mid, payload, actor="alice"):
    for index, start in enumerate(range(0, len(payload), PART)):
        chunk = payload[start:start + PART]
        result = call(
            runtime, "PUT", actor, f"{base(cid)}/{mid}/parts/{index}",
            {"data": base64.b64encode(chunk).decode("ascii")})
        assert result["status"] == 200, result
    return call(runtime, "POST", actor, f"{base(cid)}/{mid}/finish")


def sent(runtime, cid, actor="alice", size=PART + 100):
    created = create(runtime, cid, actor=actor, size=size)
    assert created["status"] == 201, created
    mid = created["data"]["media"]["id"]
    payload = blob(size)
    finished = upload(runtime, cid, mid, payload, actor=actor)
    assert finished["status"] == 200, finished
    assert finished["data"]["media"]["state"] == "ready"
    return mid, payload


def download(runtime, cid, mid, actor="bob"):
    first = call(runtime, "GET", actor, f"{base(cid)}/{mid}/parts/0")
    if first["status"] != 200:
        return first, b""
    chunks = [base64.b64decode(first["data"]["data"])]
    for index in range(1, first["data"]["parts"]):
        result = call(
            runtime, "GET", actor, f"{base(cid)}/{mid}/parts/{index}")
        assert result["status"] == 200, result
        chunks.append(base64.b64decode(result["data"]["data"]))
    return first, b"".join(chunks)


def part_rows(runtime, mid=None):
    if mid is None:
        return runtime.scalar("SELECT COUNT(*) FROM chat_direct_media_parts")
    return runtime.db.execute(
        "SELECT COUNT(*) FROM chat_direct_media_parts WHERE media_id=?",
        (mid,)).fetchone()[0]


def state(runtime, mid):
    row = runtime.db.execute(
        "SELECT state FROM chat_direct_media WHERE media_id=?",
        (mid,)).fetchone()
    return row[0] if row else None


def test_recipient_downloads_once_and_consuming_deletes_the_blob():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, payload = sent(runtime, cid)
    assert part_rows(runtime, mid) == 2

    stored = runtime.db.execute(
        "SELECT data FROM chat_direct_media_parts WHERE media_id=?",
        (mid,)).fetchall()
    assert all(json.loads(row[0]).keys() == {"d"} for row in stored)

    first, received = download(runtime, cid, mid)
    assert first["status"] == 200
    assert received == payload

    consumed = call(runtime, "POST", "bob", f"{base(cid)}/{mid}/consume")
    assert consumed["status"] == 200
    assert consumed["data"]["media"]["state"] == "opened"
    assert consumed["data"]["media"]["openedAt"] == runtime.clock
    assert part_rows(runtime, mid) == 0
    assert state(runtime, mid) == "opened"
    assert runtime.media_announcements == [
        (cid, 1, "bi:bob", mid, "opened")]

    again, _ = download(runtime, cid, mid)
    assert again["status"] == 410
    assert again["data"] == {"error": "media_gone", "state": "opened"}

    repeat = call(runtime, "POST", "bob", f"{base(cid)}/{mid}/consume")
    assert repeat["status"] == 200
    assert len(runtime.media_announcements) == 1


def test_sender_sees_opened_status_but_never_the_media():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, _payload = sent(runtime, cid)

    sender_part = call(runtime, "GET", "alice", f"{base(cid)}/{mid}/parts/0")
    assert sender_part["status"] == 403
    assert sender_part["data"] == {"error": "recipient_only"}
    sender_consume = call(
        runtime, "POST", "alice", f"{base(cid)}/{mid}/consume")
    assert sender_consume["status"] == 403
    assert part_rows(runtime, mid) == 2

    call(runtime, "POST", "bob", f"{base(cid)}/{mid}/consume")
    listed = call(runtime, "GET", "alice", base(cid), query={"ids": mid})
    assert listed["status"] == 200
    [status] = listed["data"]["media"]
    assert status["state"] == "opened"
    assert status["role"] == "sender"
    single = call(runtime, "GET", "alice", f"{base(cid)}/{mid}")
    assert single["data"]["media"]["state"] == "opened"


def test_outsiders_get_not_found_everywhere():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, _payload = sent(runtime, cid)
    for method, path in (
            ("GET", base(cid)),
            ("POST", base(cid)),
            ("GET", f"{base(cid)}/{mid}"),
            ("DELETE", f"{base(cid)}/{mid}"),
            ("GET", f"{base(cid)}/{mid}/parts/0"),
            ("PUT", f"{base(cid)}/{mid}/parts/0"),
            ("POST", f"{base(cid)}/{mid}/consume"),
            ("POST", f"{base(cid)}/{mid}/finish")):
        result = call(runtime, method, "carol", path, {})
        assert result["status"] == 404, (method, path, result)
    assert part_rows(runtime, mid) == 2
    unsigned = call(runtime, "GET", "", f"{base(cid)}/{mid}/parts/0")
    assert unsigned["status"] == 401


def test_media_of_another_conversation_is_not_found():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    other = conversation(runtime, "bob", "carol")
    mid, _payload = sent(runtime, cid)
    result = call(runtime, "GET", "bob", f"{base(other)}/{mid}/parts/0")
    assert result["status"] == 404


def test_limits_and_validation():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    big = media.MEDIA_KINDS["voice"]["max_bytes"] + \
        media.MEDIA_SEAL_OVERHEAD_BYTES + 1
    assert create(runtime, cid, size=big)["status"] == 413
    assert create(runtime, cid, kind="video", size=PART)["status"] == 201
    assert create(runtime, cid, kind="image")["data"] == {
        "error": "invalid_kind"}
    assert create(runtime, cid, duration_ms=10 * 60 * 1000)["status"] == 400
    wrong_parts = call(runtime, "POST", "alice", base(cid), {
        "kind": "voice", "size": 10, "parts": 2})
    assert wrong_parts["data"] == {"error": "invalid_parts"}

    created = create(runtime, cid, size=PART + 10)
    mid = created["data"]["media"]["id"]
    short = call(runtime, "PUT", "alice", f"{base(cid)}/{mid}/parts/0", {
        "data": base64.b64encode(b"x" * 10).decode()})
    assert short["data"] == {"error": "invalid_part_size"}
    beyond = call(runtime, "PUT", "alice", f"{base(cid)}/{mid}/parts/2", {
        "data": base64.b64encode(b"x" * 10).decode()})
    assert beyond["data"] == {"error": "invalid_part"}
    garbage = call(runtime, "PUT", "alice", f"{base(cid)}/{mid}/parts/1", {
        "data": "!!!"})
    assert garbage["data"] == {"error": "invalid_part"}
    by_recipient = call(runtime, "PUT", "bob", f"{base(cid)}/{mid}/parts/1", {
        "data": base64.b64encode(b"x" * 10).decode()})
    assert by_recipient["status"] == 403
    incomplete = call(runtime, "POST", "alice", f"{base(cid)}/{mid}/finish")
    assert incomplete["status"] == 409
    not_ready, _ = download(runtime, cid, mid)
    assert not_ready["status"] == 410
    assert not_ready["data"]["state"] == "uploading"
    early = call(runtime, "POST", "bob", f"{base(cid)}/{mid}/consume")
    assert early["status"] == 409
    assert part_rows(runtime, mid) == 0
    assert state(runtime, mid) == "uploading"


def test_pending_quota_per_sender():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    for _ in range(media.MEDIA_PENDING_MAX_COUNT):
        assert create(runtime, cid, size=1000)["status"] == 201
    refused = create(runtime, cid, size=1000)
    assert refused["status"] == 429
    assert refused["data"] == {"error": "media_quota_count"}
    assert create(runtime, cid, actor="bob", size=1000)["status"] == 201

    runtime2 = MediaRuntime()
    cid2 = conversation(runtime2)
    video = media.MEDIA_KINDS["video"]["max_bytes"]
    for _ in range(media.MEDIA_PENDING_MAX_BYTES // video):
        assert create(runtime2, cid2, kind="video", size=video)["status"] == 201
    over = create(runtime2, cid2, kind="video", size=video)
    assert over["data"] == {"error": "media_quota_bytes"}


def test_unopened_media_expires_after_seven_days_and_is_deleted():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, _payload = sent(runtime, cid)
    runtime.clock += media.MEDIA_TTL_MS

    late, _ = download(runtime, cid, mid)
    assert late["status"] == 410
    assert late["data"]["state"] == "expired"
    late_consume = call(runtime, "POST", "bob", f"{base(cid)}/{mid}/consume")
    assert late_consume["status"] == 410
    assert part_rows(runtime, mid) == 0
    assert state(runtime, mid) == "expired"

    other_mid, _ = sent(runtime, cid)
    runtime.clock += media.MEDIA_TTL_MS
    expired = run(media.sweep(runtime))
    assert expired == 1
    assert part_rows(runtime) == 0
    assert state(runtime, other_mid) == "expired"
    assert runtime.media_announcements[-1] == (
        cid, 1, "", other_mid, "expired")

    runtime.clock += media.MEDIA_TOMBSTONE_TTL_MS
    run(media.sweep(runtime))
    assert state(runtime, mid) is None


def test_abandoned_uploads_and_orphan_parts_are_swept():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    created = create(runtime, cid, size=1000)
    mid = created["data"]["media"]["id"]
    call(runtime, "PUT", "alice", f"{base(cid)}/{mid}/parts/0", {
        "data": base64.b64encode(blob(1000)).decode()})
    runtime.db.execute(
        "INSERT INTO chat_direct_media_parts "
        "(media_id,part_index,size,data,created_at) VALUES (?,?,?,?,?)",
        ("9" * 32, 0, 3, "{}", runtime.clock))
    runtime.clock += media.MEDIA_UPLOAD_TTL_MS
    run(media.sweep(runtime))
    assert part_rows(runtime) == 0
    assert state(runtime, mid) == "expired"
    assert runtime.media_announcements == []


def test_sender_can_withdraw_before_it_is_opened():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, _payload = sent(runtime, cid)
    by_recipient = call(runtime, "DELETE", "bob", f"{base(cid)}/{mid}")
    assert by_recipient["status"] == 403
    withdrawn = call(runtime, "DELETE", "alice", f"{base(cid)}/{mid}")
    assert withdrawn["data"]["media"]["state"] == "withdrawn"
    assert part_rows(runtime, mid) == 0
    assert runtime.media_announcements == [
        (cid, 1, "bi:alice", mid, "withdrawn")]
    gone, _ = download(runtime, cid, mid)
    assert gone["status"] == 410


def test_deleting_the_conversation_deletes_its_media():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    mid, _payload = sent(runtime, cid)
    run(media.delete_conversation(runtime, cid))
    assert part_rows(runtime) == 0
    assert state(runtime, mid) is None


def test_part_body_limit_and_methods():
    runtime = MediaRuntime()
    cid = conversation(runtime)
    assert call(runtime, "PATCH", "alice", base(cid))["status"] == 405
    assert media.matches(base(cid))
    assert media.matches(base(cid) + "/" + "a" * 32 + "/parts/0")
    assert not media.matches(f"/api/chat/direct-messages/{cid}/read")
    assert media.MEDIA_PART_BODY_MAX_BYTES > (PART + 2) // 3 * 4 + 64
