"""Image attachments on website-queued agent prompts (launch and follow-up).

The phone sends ``attachments: [{name, mediaType, data}]`` with a launch or a
steer. The relay validates them, stores them sealed in D1-row-sized chunks,
puts only their metadata in the job, lets the node holding the job's lease
download them, lets the person who attached them read them back for a week,
and deletes them with the session or through the week-old sweep.
"""

import asyncio
import base64
import hashlib
import json
import re
from pathlib import Path
from types import SimpleNamespace

import test_org_agent_job_queue as job_queue


ROOT = Path(__file__).resolve().parents[1]
ROUTES_PATH = ROOT / "src" / "route_handlers.py"
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")

JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 60
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 60
GIF = b"GIF89a" + b"\x00" * 60
WEBP = b"RIFF\x00\x00\x00\x00WEBPVP8 " + b"\x00" * 60


def _b64(data):
    return base64.b64encode(data).decode("ascii")


class RequestBodyTooLarge(BaseException):
    pass


class Store:
    """In-memory D1 for the attachment and job statements."""

    def __init__(self, fail_insert_at=None):
        self.chunks = {}
        self.statements = []
        self.batches = []
        self.fail_insert_at = fail_insert_at
        self.jobs = {}

    async def run(self, _env, sql, *args):
        self.statements.append((sql, args))
        if sql.startswith("INSERT INTO org_agent_attachments"):
            if (
                self.fail_insert_at is not None
                and len(self.chunks) >= self.fail_insert_at
            ):
                raise RuntimeError("D1 write failed")
            session_id, attachment_id, index, org_bi, repo, data, now = args
            self.chunks[(session_id, attachment_id, index)] = {
                "org_bi": org_bi, "repo": repo, "data": data,
                "created_at": now,
            }
        elif sql.startswith("DELETE FROM org_agent_attachments WHERE "
                            "session_id=?"):
            session_id, *ids = args
            for key in list(self.chunks):
                if key[0] == session_id and (not ids or key[1] in ids):
                    del self.chunks[key]

    async def batch(self, _env, statements):
        self.batches.append(statements)

    async def all(self, _env, sql, *args):
        assert sql.startswith("SELECT chunk_index,data,created_at FROM "
                              "org_agent_attachments")
        session_id, org_bi, attachment_id = args
        return [
            {"chunk_index": key[2], "data": value["data"],
             "created_at": value["created_at"]}
            for key, value in sorted(self.chunks.items())
            if key[0] == session_id and key[1] == attachment_id
            and value["org_bi"] == org_bi
        ]

    async def first(self, _env, sql, *args):
        assert "FROM org_agent_jobs" in sql
        job_id, owner, repo, lease_id = args
        job = self.jobs.get(job_id)
        if job and job["lease_id"] == lease_id and job["target_node"] == owner:
            return dict(job)
        return None


def _routes(store=None, **overrides):
    """Every route_handlers function over in-memory fakes."""
    store = store or Store()
    ids = iter("%032x" % number for number in range(1, 1000))

    class Clock:
        @staticmethod
        def now():
            return 5_000_000

    def error(code, message, status, **details):
        return {"status": status, "data": {
            "ok": False, "error": code, "message": message, **details}}

    def response(payload, status=200, **_kwargs):
        return {"status": status, "data": payload}

    async def noop(*_args, **_kwargs):
        return None

    async def seal(_env, value):
        return "sealed:" + json.dumps(value)

    async def unseal(_env, value):
        if not str(value or "").startswith("sealed:"):
            return value if isinstance(value, dict) else None
        return json.loads(value[len("sealed:"):])

    namespace = {
        "base64": base64,
        "hashlib": hashlib,
        "json": json,
        "re": re,
        "Date": Clock,
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "ORG_AGENT_MAX_PROMPT": 8000,
        "ORG_AGENT_MAX_HISTORY": 80,
        "ORG_AGENT_MAX_SESSIONS": 50,
        "ORG_AGENT_PROVIDERS": ("claude-code", "codex"),
        "ORG_AGENT_GENERAL_PROVIDERS": ("agent", "bot", "auto"),
        "ORG_AGENT_MODEL_ALIASES": {
            "claude-code": {"opus": "claude-opus-5-5",
                            "opus-5": "claude-opus-5"},
            "codex": {},
        },
        "RequestBodyTooLarge": RequestBodyTooLarge,
        "_ap_uuid": lambda: next(ids),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "valid_node_name": lambda value: bool(value) and "/" not in value,
        "method_name": lambda request: request.method,
        "ensure_schema": noop,
        "encrypt_row": seal,
        "decrypt_row": unseal,
        "d1_run": store.run,
        "d1_all": store.all,
        "d1_first": store.first,
        "_contribution_run_batch": store.batch,
        "_audit_sensitive_action": noop,
        "notify_repo_host": noop,
        "json_response": response,
        "_org_agent_error": error,
        "_request_origin_allowed": lambda _env, _request: True,
        "_owner_agent_signed_triple": lambda _request: False,
        "_org_agent_member_context": lambda *_a, **_k: asyncio.sleep(0, result=({
            "org": "alice", "orgBi": "bi:alice", "repo": "project",
            "nodeOwner": "desktop1", "accountBi": "bi:alice",
            "actor": "alice", "role": "owner", "scope": "owner",
        }, None)),
        "_org_agent_target_mirror": lambda *_a, **_k: asyncio.sleep(0, result={
            "ok": True, "targetNode": "mirror1", "targetOnline": True,
            "queueState": "ready_for_claim",
        }),
        "_authorized_agent_node_signing_key": lambda *_a: asyncio.sleep(
            0, result="node-key"),
        "parse_qs": __import__("urllib.parse", fromlist=["x"]).parse_qs,
        "urlparse": __import__("urllib.parse", fromlist=["x"]).urlparse,
        "_org_agent_attachment_response": lambda data: {
            "status": 200, "bytes": bytes(data)},
    }
    source = ROUTES_PATH.read_text(encoding="utf-8")
    exec(compile(source, str(ROUTES_PATH), "exec"), namespace)
    # The export-only helpers the handlers reach through the runtime.
    namespace.update({
        "_org_agent_security_envelope": lambda _context, _owner: (
            {"state": "not_required"}, {"required": False}, "queued"),
        "_ring_cloud_agent_readers": noop,
        "_org_agent_session_projection": lambda row, record: {
            "id": row["session_id"], "history": record.get("history")},
        "_org_agent_attachment_response": lambda data: {
            "status": 200, "bytes": bytes(data)},
    })
    namespace.update(overrides)
    return namespace, store


def _body_request(payload, method="POST"):
    return SimpleNamespace(method=method, payload=payload)


def _with_body(namespace):
    async def bounded(request, _limit=None):
        if request.payload == "too-large":
            raise RequestBodyTooLarge()
        return request.payload
    namespace["bounded_json_request"] = bounded
    return namespace


# --- validation -------------------------------------------------------------

def test_valid_images_of_every_supported_type_are_accepted():
    ns, _store = _routes()
    items, error = ns["_org_agent_parse_attachments"]([
        {"name": "photo-1.jpg", "mediaType": "image/jpeg", "data": _b64(JPEG)},
        {"name": "shot.png", "mediaType": "image/png", "data": _b64(PNG)},
        {"name": "loop.gif", "mediaType": "image/gif", "data": _b64(GIF)},
        {"name": "pic.webp", "mediaType": "IMAGE/WEBP", "data": _b64(WEBP)},
    ])
    assert error is None
    assert [item["mediaType"] for item in items] == [
        "image/jpeg", "image/png", "image/gif", "image/webp"]
    assert items[0]["size"] == len(JPEG)
    assert items[0]["sha256"] == hashlib.sha256(JPEG).hexdigest()
    assert items[0]["bytes"] == JPEG
    assert all(re.fullmatch(r"[0-9a-f]{32}", item["id"]) for item in items)
    assert ns["_org_agent_parse_attachments"](None) == ([], None)


def test_client_names_are_display_only_and_never_carry_a_path():
    ns, _store = _routes()
    items, _error = ns["_org_agent_parse_attachments"]([
        {"name": "../../etc/passwd.jpg", "mediaType": "image/jpeg",
         "data": _b64(JPEG)},
        {"name": "C:\\Users\\a\\b.png", "mediaType": "image/png",
         "data": _b64(PNG)},
        {"name": "..", "mediaType": "image/gif", "data": _b64(GIF)},
    ])
    assert [item["name"] for item in items] == [
        "passwd.jpg", "b.png", "image-3.gif"]


def test_more_than_five_attachments_are_refused():
    ns, _store = _routes()
    image = {"mediaType": "image/jpeg", "data": _b64(JPEG)}
    items, error = ns["_org_agent_parse_attachments"]([image] * 6)
    assert items is None
    assert error[0] == "too_many_attachments"
    assert ns["_org_agent_parse_attachments"]([image] * 5)[1] is None


def test_an_image_over_the_size_cap_is_refused():
    ns, _store = _routes()
    cap = ns["ORG_AGENT_ATTACHMENT_MAX_BYTES"]
    assert cap == 5 * 1024 * 1024
    too_big = JPEG + b"\x00" * (cap + 1 - len(JPEG))
    _items, error = ns["_org_agent_parse_attachments"]([
        {"mediaType": "image/jpeg", "data": _b64(too_big)}])
    assert error[0] == "attachment_too_large"
    exactly = JPEG + b"\x00" * (cap - len(JPEG))
    assert ns["_org_agent_parse_attachments"]([
        {"mediaType": "image/jpeg", "data": _b64(exactly)}])[1] is None


def test_images_over_the_request_total_are_refused():
    ns, _store = _routes()
    each = JPEG + b"\x00" * (4 * 1024 * 1024)
    _items, error = ns["_org_agent_parse_attachments"]([
        {"mediaType": "image/jpeg", "data": _b64(each)}] * 3)
    assert error[0] == "attachment_too_large"
    assert "10 MB" in error[1]


def test_media_type_magic_mismatch_and_garbage_are_refused():
    ns, _store = _routes()
    parse = ns["_org_agent_parse_attachments"]
    for value in (
        [{"mediaType": "image/png", "data": _b64(JPEG)}],
        [{"mediaType": "image/jpeg", "data": _b64(b"<svg onload=x>")}],
        [{"mediaType": "image/svg+xml", "data": _b64(b"<svg/>")}],
        [{"mediaType": "application/pdf", "data": _b64(b"%PDF-1.7")}],
        [{"mediaType": "image/jpeg", "data": "not base64!!"}],
        [{"mediaType": "image/jpeg", "data": ""}],
        [{"mediaType": "image/jpeg"}],
        ["image"],
        {"mediaType": "image/jpeg"},
    ):
        items, error = parse(value)
        assert items is None, value
        assert error[0] == "attachment_invalid", value


# --- storage ----------------------------------------------------------------

def test_images_are_sealed_in_row_sized_chunks_after_an_orphan_sweep():
    ns, store = _routes()
    chunk = ns["ORG_AGENT_ATTACHMENT_CHUNK_BYTES"]
    big = JPEG + bytes(range(256)) * ((2 * chunk) // 256 + 10)
    items, _error = ns["_org_agent_parse_attachments"]([
        {"mediaType": "image/jpeg", "data": _b64(big)},
        {"mediaType": "image/png", "data": _b64(PNG)},
    ])
    ok = asyncio.run(ns["_org_agent_store_attachments"](
        object(), "bi:alice", "project", "session-1", items))
    assert ok is True
    # The sweep runs first and only touches rows no live job can still use.
    sweep_sql, sweep_args = store.statements[0]
    assert sweep_sql.startswith("DELETE FROM org_agent_attachments WHERE "
                                "created_at<?")
    assert "j.status IN ('queued','leased','running')" in sweep_sql
    assert sweep_args == (5_000_000 - 7 * 24 * 60 * 60 * 1000,)
    first_id, second_id = items[0]["id"], items[1]["id"]
    assert sorted(k[2] for k in store.chunks if k[1] == first_id) == [0, 1, 2]
    assert [k[2] for k in store.chunks if k[1] == second_id] == [0]
    for value in store.chunks.values():
        assert value["data"].startswith("sealed:")
        # Base64 inside the sealed JSON stays well under D1's 2 MB value cap
        # even after the ciphertext is base64 encoded once more.
        assert len(value["data"]) * 4 // 3 < 1_900_000
    # The bytes left the item once they were stored.
    assert "bytes" not in items[0]


def test_a_failed_chunk_write_leaves_nothing_behind():
    ns, store = _routes(Store(fail_insert_at=1))
    items, _error = ns["_org_agent_parse_attachments"]([
        {"mediaType": "image/jpeg", "data": _b64(JPEG)},
        {"mediaType": "image/png", "data": _b64(PNG)},
    ])
    ok = asyncio.run(ns["_org_agent_store_attachments"](
        object(), "bi:alice", "project", "session-1", items))
    assert ok is False
    assert store.chunks == {}


# --- launch and steer routes ------------------------------------------------

def _launch(ns, payload):
    handler = ns["org_agent_bots_handler"]
    return asyncio.run(handler(
        object(), _body_request(payload), "desktop1", "project",
        owner_scope=True))


def test_launch_stores_images_and_the_job_carries_only_their_metadata():
    ns, store = _routes()
    _with_body(ns)
    response = _launch(ns, {
        "provider": "claude-code", "prompt": "Fix the header",
        "model": "opus", "createPr": True,
        "attachments": [
            {"name": "photo-1.jpg", "mediaType": "image/jpeg",
             "data": _b64(JPEG)},
        ],
    })
    assert response["status"] == 202
    assert len(store.batches) == 1
    (session_sql, session_args), (job_sql, job_args) = store.batches[0]
    assert "INSERT INTO org_agent_sessions" in session_sql
    record = json.loads(session_args[7][len("sealed:"):])
    job = json.loads(job_args[5][len("sealed:"):])
    assert job["model"] == "claude-opus-5-5"
    [meta] = job["attachments"]
    assert set(meta) == {"id", "name", "mediaType", "size", "sha256"}
    assert meta["name"] == "photo-1.jpg"
    assert meta["size"] == len(JPEG)
    assert meta["sha256"] == hashlib.sha256(JPEG).hexdigest()
    assert "data" not in json.dumps(job)
    # The session's user turn exposes the count to the phone, not the digest.
    assert record["history"][0]["attachments"] == [{
        "id": meta["id"], "name": "photo-1.jpg", "mediaType": "image/jpeg",
        "size": len(JPEG)}]
    session_id = session_args[0]
    assert list(store.chunks) == [(session_id, meta["id"], 0)]


def test_launch_takes_a_resolved_model_id_a_retry_sends_back():
    ns, store = _routes()
    _with_body(ns)
    response = _launch(ns, {
        "provider": "claude-code", "prompt": "Fix it",
        "model": "claude-opus-5-5"})
    assert response["status"] == 202
    job = json.loads(store.batches[0][1][1][5][len("sealed:"):])
    assert job["model"] == "claude-opus-5-5"
    response = _launch(ns, {
        "provider": "claude-code", "prompt": "Fix it",
        "model": "gpt-5.6-sol"})
    assert response["status"] == 400
    assert response["data"]["error"] == "invalid_model"


def test_launch_without_attachments_is_unchanged():
    ns, store = _routes()
    _with_body(ns)
    response = _launch(ns, {"provider": "claude-code", "prompt": "Hi"})
    assert response["status"] == 202
    job = json.loads(store.batches[0][1][1][5][len("sealed:"):])
    assert "attachments" not in job
    assert store.statements == []


def test_launch_with_a_bad_image_is_a_400_before_anything_is_queued():
    ns, store = _routes()
    _with_body(ns)
    response = _launch(ns, {
        "provider": "claude-code", "prompt": "Fix it",
        "attachments": [{"mediaType": "image/png", "data": _b64(JPEG)}],
    })
    assert response["status"] == 400
    assert response["data"]["ok"] is False
    assert response["data"]["error"] == "attachment_invalid"
    assert store.batches == [] and store.chunks == {}


def test_an_oversized_body_is_an_attachment_error_in_the_agent_shape():
    ns, store = _routes()
    _with_body(ns)
    response = _launch(ns, "too-large")
    assert response["status"] == 400
    assert response["data"]["error"] == "attachment_too_large"
    assert store.batches == []


def test_launch_whose_images_cannot_be_stored_queues_nothing():
    ns, store = _routes(Store(fail_insert_at=0))
    _with_body(ns)
    response = _launch(ns, {
        "provider": "claude-code", "prompt": "Fix it",
        "attachments": [{"mediaType": "image/jpeg", "data": _b64(JPEG)}],
    })
    assert response["status"] == 503
    assert response["data"]["error"] == "attachment_persistence_failed"
    assert store.batches == []


def test_steer_carries_images_to_the_follow_up_job():
    session_row = {
        "session_id": "session-9", "provider": "claude-code",
        "status": "completed", "target_node": "mirror1",
    }
    ns, store = _routes()
    _with_body(ns)
    ns["_org_agent_session_row"] = lambda *_a: asyncio.sleep(0, result=(
        dict(session_row), {"localAgentId": 7, "history": []}))
    response = asyncio.run(ns["org_agent_bot_handler"](
        object(), _body_request({
            "prompt": "Now match this mockup",
            "attachments": [{"name": "mock.png", "mediaType": "image/png",
                             "data": _b64(PNG)}],
        }), "desktop1", "project", "session-9", owner_scope=True))
    assert response["status"] == 202
    (_update_sql, update_args), (_job_sql, job_args) = store.batches[0]
    job = json.loads(job_args[5][len("sealed:"):])
    assert job["kind"] == "steer"
    [meta] = job["attachments"]
    assert meta["mediaType"] == "image/png"
    record = json.loads(update_args[1][len("sealed:"):])
    assert record["history"][-1]["attachments"][0]["id"] == meta["id"]
    assert ("session-9", meta["id"], 0) in store.chunks

    bad = asyncio.run(ns["org_agent_bot_handler"](
        object(), _body_request({
            "prompt": "again",
            "attachments": [{"mediaType": "image/gif", "data": _b64(PNG)}],
        }), "desktop1", "project", "session-9", owner_scope=True))
    assert bad["status"] == 400
    assert bad["data"]["error"] == "attachment_invalid"


def test_deleting_a_session_deletes_its_images_in_the_same_batch():
    ns, store = _routes()
    ns["_org_agent_session_row"] = lambda *_a: asyncio.sleep(0, result=({
        "session_id": "session-9", "provider": "codex", "status": "queued",
        "target_node": "mirror1"}, {}))
    response = asyncio.run(ns["org_agent_bot_handler"](
        object(), SimpleNamespace(method="DELETE"), "desktop1", "project",
        "session-9", owner_scope=True))
    assert response["data"]["deleted"] is True
    assert store.batches[0][0] == (
        "DELETE FROM org_agent_attachments WHERE session_id=? "
        "AND org_bi=? AND repo=?",
        ("session-9", "bi:alice", "project"),
    )


# --- node side: claim payload, download, deletion on report ----------------

def test_the_claim_payload_carries_attachment_metadata_without_bytes():
    store = job_queue.QueueStore(1)
    meta = {"id": "a" * 32, "name": "photo-1.jpg", "mediaType": "image/jpeg",
            "size": 64, "sha256": "b" * 64}
    store.rows[0]["data"]["attachments"] = [meta]
    handler = job_queue._load_handler(store)
    response = asyncio.run(handler(
        object(), SimpleNamespace(method="GET"), "desktop1", "forkmesh"))
    [job] = response["data"]["jobs"]
    assert job["attachments"] == [meta]
    assert job["jobId"] == 1 and job["leaseId"]


def _stored_job(ns, store, data=JPEG, status="leased"):
    items, _error = ns["_org_agent_parse_attachments"]([
        {"name": "photo-1.jpg", "mediaType": "image/jpeg", "data": _b64(data)}])
    job = {"sessionId": "session-1",
           "attachments": ns["_org_agent_attachment_meta"](items, True)}
    asyncio.run(ns["_org_agent_store_attachments"](
        object(), "bi:alice", "project", "session-1", items))
    store.jobs[42] = {
        "id": 42, "session_id": "session-1", "org_bi": "bi:alice",
        "target_node": "mirror1", "lease_id": "lease-1", "status": status,
        "data": "sealed:" + json.dumps(job),
    }
    return job["attachments"][0]["id"]


def _download(ns, attachment_id, lease="lease-1", job_id="42"):
    request = SimpleNamespace(
        method="GET",
        url="https://forkmesh.test/api/repo/mirror1/project/org-agent-jobs/"
            + job_id + "/attachments/" + attachment_id
            + "?owner=mirror1&ts=1&sig=x&leaseId=" + lease)
    return asyncio.run(ns["repo_org_agent_job_attachment_handler"](
        object(), request, "mirror1", "project", job_id, attachment_id))


def test_the_lease_holder_downloads_the_exact_bytes():
    ns, store = _routes()
    chunk = ns["ORG_AGENT_ATTACHMENT_CHUNK_BYTES"]
    data = JPEG + bytes(range(256)) * (chunk // 128)
    attachment_id = _stored_job(ns, store, data)
    response = _download(ns, attachment_id)
    assert response == {"status": 200, "bytes": data}


def test_download_needs_the_current_lease_and_a_signed_node():
    ns, store = _routes()
    attachment_id = _stored_job(ns, store)
    assert _download(ns, attachment_id, lease="stale")["status"] == 409
    assert _download(ns, "f" * 32)["status"] == 410
    ns["_authorized_agent_node_signing_key"] = lambda *_a: asyncio.sleep(
        0, result="")
    assert _download(ns, attachment_id)["status"] == 401


def test_a_tampered_or_missing_chunk_is_never_served():
    ns, store = _routes()
    attachment_id = _stored_job(ns, store)
    key = next(iter(store.chunks))
    store.chunks[key]["data"] = "sealed:" + json.dumps(
        {"d": _b64(PNG)})
    assert _download(ns, attachment_id)["status"] == 410
    store.chunks.clear()
    assert _download(ns, attachment_id)["status"] == 410


def test_a_result_report_keeps_the_images_for_the_transcript():
    source = ROUTES_PATH.read_text(encoding="utf-8")
    result = source[source.index(
        "async def _apply_org_agent_job_result"):]
    result = result[:result.index("\n\n\nasync def ")]
    assert '"running" if keep_running else "done"' in result
    assert "_org_agent_delete_attachments" not in result


# --- read back on the transcript ---------------------------------------------

def _session_with_image(ns, store, author="alice", data=JPEG):
    items, _error = ns["_org_agent_parse_attachments"]([
        {"name": "photo-1.jpg", "mediaType": "image/jpeg", "data": _b64(data)}])
    meta = ns["_org_agent_attachment_meta"](items)
    asyncio.run(ns["_org_agent_store_attachments"](
        object(), "bi:alice", "project", "session-1", items))
    record = {"history": [
        {"role": "user", "author": author, "text": "look", "attachments": meta},
        {"role": "assistant", "text": "done"},
    ]}
    ns["_org_agent_session_row"] = lambda _env, _context, session_id: (
        asyncio.sleep(0, result=(
            {"session_id": "session-1", "org_bi": "bi:alice"}, record)
            if session_id == "session-1" else (None, None)))
    return meta[0]["id"]


def _read_back(ns, attachment_id, session_id="session-1", method="GET",
               owner_scope=True):
    request = SimpleNamespace(
        method=method,
        url="https://forkmesh.test/api/repo/desktop1/project/agent-sessions/"
            + session_id + "/attachments/" + attachment_id)
    return asyncio.run(ns["org_agent_session_attachment_handler"](
        object(), request, "desktop1", "project", session_id, attachment_id,
        owner_scope=owner_scope))


def test_the_owner_reads_a_prompt_image_back_after_the_run():
    ns, store = _routes()
    chunk = ns["ORG_AGENT_ATTACHMENT_CHUNK_BYTES"]
    data = JPEG + bytes(range(256)) * (chunk // 128)
    attachment_id = _session_with_image(ns, store, data=data)
    assert _read_back(ns, attachment_id) == {"status": 200, "bytes": data}


def test_only_the_member_who_attached_an_org_image_reads_it():
    ns, store = _routes()
    attachment_id = _session_with_image(ns, store, author="bob")
    refused = _read_back(ns, attachment_id, owner_scope=False)
    assert refused["status"] == 403
    assert refused["data"]["error"] == "attachment_private"
    ns["_org_agent_member_context"] = lambda *_a, **_k: asyncio.sleep(
        0, result=({"org": "acme", "orgBi": "bi:alice", "repo": "project",
                    "actor": "Bob", "scope": "organization"}, None))
    assert _read_back(ns, attachment_id, owner_scope=False)["status"] == 200


def test_an_image_past_the_week_or_not_listed_is_gone():
    ns, store = _routes()
    attachment_id = _session_with_image(ns, store)
    assert _read_back(ns, "f" * 32)["status"] == 410
    for value in store.chunks.values():
        value["created_at"] = 5_000_000 - 7 * 24 * 60 * 60 * 1000 - 1
    expired = _read_back(ns, attachment_id)
    assert expired["status"] == 410
    assert expired["data"]["error"] == "attachment_expired"
    store.chunks.clear()
    assert _read_back(ns, attachment_id)["status"] == 410


def test_read_back_is_get_only_and_needs_the_session():
    ns, store = _routes()
    attachment_id = _session_with_image(ns, store)
    assert _read_back(ns, attachment_id, method="POST")["status"] == 405
    assert _read_back(
        ns, attachment_id, session_id="session-2")["status"] == 404
    denied = {"status": 403, "data": {"ok": False, "error": "forbidden"}}
    ns["_org_agent_member_context"] = lambda *_a, **_k: asyncio.sleep(
        0, result=(None, denied))
    assert _read_back(ns, attachment_id) == denied


def test_routes_schema_and_limits_are_wired():
    assert "REPO_ORG_AGENT_JOB_ATTACHMENT_RE" in URLS
    assert "/attachments/([0-9a-f]{32})$" in URLS
    assert "repo_org_agent_job_attachment_handler(" in RUNTIME
    assert "REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE" in URLS
    assert "ORG_AGENT_BOT_ATTACHMENT_RE" in URLS
    assert "org_agent_session_attachment_handler(" in RUNTIME
    assert "owner_agent_attachment_match.group(4), owner_scope=True" in RUNTIME
    assert '"ORG_AGENT_BOT_ATTACHMENT_RE"' in ENTRY
    assert '"REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE"' in ENTRY
    assert '_route_handlers.export(\n    "org_agent_session_attachment_handler")' in ENTRY
    assert "CREATE TABLE IF NOT EXISTS org_agent_attachments" in SCHEMA
    assert "PRIMARY KEY (session_id, attachment_id, chunk_index)" in SCHEMA
    ns, _store = _routes()
    assert ns["ORG_AGENT_ATTACHMENT_MAX_COUNT"] == 5
    assert ns["ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES"] == 10 * 1024 * 1024
    # The body limit admits the largest valid request.
    assert ns["ORG_AGENT_ATTACHMENT_BODY_LIMIT"] > (
        10 * 1024 * 1024 * 4 // 3 + 8000)
