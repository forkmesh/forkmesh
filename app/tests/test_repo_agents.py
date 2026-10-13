#!/usr/bin/env python3
"""Agent-session sync endpoints (website "Agents" tab, adhoc #182).

The desktop app runs Claude Code coding agent sessions per repo/issue. These
tests exercise the three new routes end to end against an in-memory D1
double, following the same source-extraction harness pattern used by
test_users_nodes_claim_link.py: select just the functions under test out of
entry.py via ast, and stub the handful of I/O primitives they call
(D1/crypto/JS) so the real control-flow logic runs unmodified.

Covered:
 - POST /agents (desktop push, ts+sig signed) stores sessions; a follow-up
   POST /agents/list (owner, proven by a signed account session token) returns
   them.
 - POST /agents with a bad/missing signature -> 401.
 - POST /agents/list from a non-owner account -> 403, no data leaked.
 - POST /agents/<id>/prompt (owner session) enqueues a prompt; a subsequent
   GET /agents (desktop drain, ts+sig) returns and clears it.
 - A non-owner session still fails list/prompt with 403 not_authorized, and a
   self-asserted ownerAccount with no valid session token is rejected.
"""

import ast
import asyncio
import base64
import hashlib
import hmac
import importlib.util
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from worker_test_helpers import json_from_request_double


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
# clean_string was extracted from entry.py into catalog.py; parse both sources
# so the AST loader below still finds it.
CATALOG = ENTRY.parent / "catalog.py"
SCHEMA = ENTRY.parent / "schema.py"
SECURITY_SPEC = importlib.util.spec_from_file_location(
    "repo_agents_security_controls", ENTRY.parent / "security_controls.py")
SECURITY_CONTROL = importlib.util.module_from_spec(SECURITY_SPEC)
SECURITY_SPEC.loader.exec_module(SECURITY_CONTROL)
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n" + CATALOG.read_text(encoding="utf-8")
    + "\n" + SCHEMA.read_text(encoding="utf-8")
    + "\n" + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8"))

FUNCS = {
    "agents_handler", "agents_list_handler", "agents_prompt_handler",
    "agents_transcript_handler", "agents_ack_handler",
    "_clean_agent_session", "_authorize_owner", "_ring_agent_readers",
    "_authorized_owner_signing_key",
    "_verify_owner_signature", "_owner_signing_pubkeys",
    "_account_signing_pubkeys",
    "_authorize_owner_account", "_owner_pubkey", "_login_locked_until",
    "_login_record_fail", "_login_clear", "method_name", "clean_string",
    # Session-based owner authorization (agents tab): the caller proves identity
    # with a revocable session, not a self-asserted ownerAccount string.
    "valid_node_name",
    "_account_owns_node", "_owned_nodes",
}


def _load_functions(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


CORRECT_PASSWORD = "correct horse battery staple"


class _Headers:
    # _account_session_record reads request.headers.get("authorization") only
    # when the body carries no sessionToken; return empty so it falls through.
    @staticmethod
    def get(_name, default=None):
        return default


class _Request:
    def __init__(self, method="GET", url="https://forkmesh.test/x", body=None):
        self.method = method
        self.url = url
        self._body = body
        self.headers = _Headers()

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


def _harness(accounts, enforce_e2ee=False):
    """accounts: name (lowercase) -> {"pass_hash": bool-ish, "is_admin": bool}.

    D1 tables are plain in-memory containers; encrypt/decrypt are identity
    (dict copies) and blind_index is 'bi:' + value, matching the style already
    used by test_users_nodes_claim_link.py.
    """
    repo_agents = {}   # (repo_bi, agent_id) -> {"data":..., "updated_at":...}
    agent_prompts = []  # list of {"id","repo_bi","agent_id","data","queued_at"}
    login_attempts = {}  # id_bi -> {"fails","first_fail_ts","locked_until"}
    next_prompt_id = [1]
    now = [1_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        rec = accounts.get(key)
        if rec is None:
            return "bi:" + key, None
        # Real decrypted account records carry their own "name"; the fixtures key
        # by name, so mirror that so session-name resolution matches production.
        rec = dict(rec)
        rec.setdefault("name", key)
        return "bi:" + key, rec

    async def verify_password(password, _salt, _hash):
        return password == CORRECT_PASSWORD

    async def _is_admin(_env, name):
        return bool(accounts.get(str(name or "").strip().lower(), {}).get("is_admin"))

    def _account_session_token(_env, name):
        return "test-session:" + str(name or "").strip().lower()

    async def _account_session_record(_env, request, data=None):
        payload = data if isinstance(data, dict) else {}
        token = str(payload.get("sessionToken") or "")
        if not token:
            auth = request.headers.get("authorization") or ""
            token = auth[7:] if auth.lower().startswith("bearer ") else ""
        prefix = "test-session:"
        name = token[len(prefix):] if token.startswith(prefix) else ""
        rec = accounts.get(name)
        if not rec or rec.get("status") != "active":
            return "", None
        record = dict(rec)
        record.setdefault("name", name)
        return "bi:" + name, record

    async def ed25519_verify(_pubkey, sig, _canonical):
        # A stand-in signature scheme: only the literal "good-sig" verifies.
        return sig == "good-sig"

    async def _account_devices_list(_env, _account_bi):
        # No extra desktop devices in these fixtures; drain auth falls back to
        # the account's primary pubkey exactly as before.
        return []

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def decrypt_row(_env, stored, key=None):
        return dict(stored)

    async def d1_first(_env, sql, *args):
        if "FROM login_attempts" in sql:
            row = login_attempts.get(args[0])
            return dict(row) if row else None
        if "FROM repo_agents WHERE repo_bi=? AND agent_id=?" in sql:
            repo_bi, agent_id = args
            row = repo_agents.get((repo_bi, agent_id))
            return {"data": row["data"]} if row else None
        if "SELECT COUNT(*) AS c FROM agent_prompts" in sql:
            repo_bi = args[0]
            c = sum(1 for r in agent_prompts if r["repo_bi"] == repo_bi)
            return {"c": c}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO login_attempts"):
            login_attempts[args[0]] = {
                "fails": args[1], "first_fail_ts": args[2], "locked_until": args[3],
            }
            return
        if sql.startswith("DELETE FROM login_attempts"):
            login_attempts.pop(args[0], None)
            return
        if sql.startswith("DELETE FROM repo_agents"):
            repo_bi = args[0]
            for key in [k for k in repo_agents if k[0] == repo_bi]:
                del repo_agents[key]
            return
        if sql.startswith("INSERT INTO repo_agents"):
            repo_bi, agent_id, data, updated_at = args
            # Mirrors the real repo_agents PRIMARY KEY (repo_bi, agent_id): a
            # duplicate insert must raise, the same as D1's UNIQUE constraint.
            if (repo_bi, agent_id) in repo_agents:
                raise AssertionError(
                    "UNIQUE constraint failed: repo_agents.repo_bi, repo_agents.agent_id")
            repo_agents[(repo_bi, agent_id)] = {"data": data, "updated_at": updated_at}
            return
        if sql.startswith(
                "DELETE FROM agent_prompts WHERE repo_bi=? AND id IN"):
            repo_bi = args[0]
            drained = set(args[1:])
            agent_prompts[:] = [
                r for r in agent_prompts
                if r["repo_bi"] != repo_bi or r["id"] not in drained
            ]
            return
        if sql.startswith("DELETE FROM agent_prompts WHERE id IN"):
            drained = set(args)
            agent_prompts[:] = [r for r in agent_prompts
                                if r["id"] not in drained]
            return
        if sql.startswith("DELETE FROM agent_prompts"):
            repo_bi = args[0]
            agent_prompts[:] = [r for r in agent_prompts if r["repo_bi"] != repo_bi]
            return
        if sql.startswith("INSERT INTO agent_prompts"):
            repo_bi, agent_id, data, queued_at = args
            agent_prompts.append({
                "id": next_prompt_id[0], "repo_bi": repo_bi, "agent_id": agent_id,
                "data": data, "queued_at": queued_at,
            })
            next_prompt_id[0] += 1
            return
        raise AssertionError("unexpected d1_run: " + sql)

    async def d1_all(_env, sql, *args):
        if "FROM repo_agents" in sql:
            repo_bi = args[0]
            rows = [v for k, v in repo_agents.items() if k[0] == repo_bi]
            rows.sort(key=lambda r: r["updated_at"], reverse=True)
            return [{"data": r["data"]} for r in rows]
        if "FROM agent_prompts" in sql:
            repo_bi = args[0]
            rows = [r for r in agent_prompts if r["repo_bi"] == repo_bi]
            rows.sort(key=lambda r: r["id"])
            return [{"id": r["id"], "data": r["data"]} for r in rows]
        raise AssertionError("unexpected d1_all: " + sql)

    async def noop_notify_repo_host(_env, _owner, _repo, _topic):
        return None

    ns = _load_functions({
        "Date": _DateStub,
        "json": __import__("json"),
        "hashlib": __import__("hashlib"),
        "_AGENT_PUSH_DIGESTS": {},
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "notify_repo_host": noop_notify_repo_host,
        "_account_row": _account_row,
        "_account_session_token": _account_session_token,
        "_account_session_record": _account_session_record,
        "_account_devices_list": _account_devices_list,
        "verify_password": verify_password,
        "_is_admin": _is_admin,
        "ed25519_verify": ed25519_verify,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "d1_all": d1_all,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
        "LOGIN_MAX_FAILS": 10,
        "LOGIN_FAIL_WINDOW_MS": 15 * 60 * 1000,
        "LOGIN_LOCKOUT_MS": 15 * 60 * 1000,
        "MAX_AGENT_SESSIONS": 300,
        "MAX_AGENT_STRING": 300,
        "MAX_AGENT_TITLE": 240,
        "MAX_AGENT_PROMPT_TEXT": 8000,
        "MAX_PENDING_AGENT_PROMPTS": 50,
        "MAX_AGENT_PROMPT_IMAGES": 3,
        "MAX_AGENT_PROMPT_IMAGE_BYTES": 1_400_000,
        "MAX_AGENT_PROMPT_IMAGES_TOTAL_BYTES": 2_400_000,
        "MAX_AGENT_TRANSCRIPT": 16000,
        "security_control": SECURITY_CONTROL,
        # Session-token proof (agents-tab owner authorization).
        "hmac": hmac,
        "NODE_NAME_RE": re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"),
        "MAX_NODE_NAME": 63,
        "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
    })
    if enforce_e2ee:
        async def _repo_privacy_policy(_env, owner, repo):
            return {
                "repoBi": "bi:" + owner + "/" + repo,
                "ownerKeyId": _owner_envelope()["recipients"][0]["kid"],
                "requireAgentE2EE": True,
                "requireMirrorEncryption": True,
            }
        ns["_repo_privacy_policy"] = _repo_privacy_policy
    else:
        # Explicit legacy fixture only: production's policy reader is
        # mandatory/fail-closed.  These historical compatibility tests opt
        # into the old branch deliberately rather than relying on a missing
        # global to create a silent plaintext fallback.
        async def _legacy_repo_privacy_policy(_env, owner, repo):
            return {
                "repoBi": "bi:" + owner + "/" + repo,
                "ownerKeyId": "",
                "requireAgentE2EE": False,
                "requireMirrorEncryption": False,
            }
        ns["_repo_privacy_policy"] = _legacy_repo_privacy_policy
    ns["_now"] = now
    ns["_repo_agents"] = repo_agents
    ns["_agent_prompts"] = agent_prompts
    return ns


def _owner_account(is_admin=False):
    return {"pubkey": "PK-alice", "status": "active", "pass_hash": "h",
            "pass_salt": "s", "is_admin": is_admin}


def _session(agent_id=42, **overrides):
    base = {
        "id": agent_id, "issueNumber": 7, "issueTitle": "Fix the thing",
        "status": "running", "provider": "anthropic", "model": "claude-sonnet-4-5",
        "branchName": "agent/issue-7", "createdAtMs": 1000, "startedAtMs": 1010,
        "finishedAtMs": 0, "numTurns": 5, "durationMs": 12345, "costUsd": 0.42,
        "lastError": "",
    }
    base.update(overrides)
    return base


def _push_url(ts="1000000000", sig="good-sig"):
    return "https://forkmesh.test/api/repo/alice/proj/agents?ts=%s&sig=%s" % (ts, sig)


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _owner_envelope(body=b"opaque owner ciphertext", kid=None):
    return {
        "kind": "forkmesh.owner-sealed",
        "v": 1,
        "alg": "x25519+mlkem768/aes256gcm",
        "nonce": _b64url(b"n" * 12),
        "tag": _b64url(b"t" * 16),
        "body": _b64url(body),
        "recipients": [{
            "kid": kid or _b64url(hashlib.sha256(b"owner-key").digest()),
            "x25519": _b64url(b"x" * 32),
            "mlkem768": _b64url(b"m" * 1088),
            "nonce": _b64url(b"w" * 12),
            "tag": _b64url(b"g" * 16),
            "key": _b64url(b"k" * 32),
        }],
    }


def test_post_agents_valid_signature_stores_sessions_visible_via_list():
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    push = asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {"sessions": [_session()]}),
        "alice", "proj",
    ))
    assert push == {"status": 200, "data": {"ok": True}}

    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
        }),
        "alice", "proj",
    ))
    assert listed["status"] == 200
    assert listed["data"]["ok"] is True
    agents = listed["data"]["agents"]
    assert len(agents) == 1
    assert agents[0]["id"] == 42
    assert agents[0]["status"] == "running"
    assert agents[0]["issueTitle"] == "Fix the thing"
    assert agents[0]["costUsd"] == 0.42


def test_changed_agent_snapshot_rings_the_owner_account_socket():
    # The phone's Agents tab reloads on the "agents" account event instead of
    # waiting for a pull to refresh; the desktop's identical safety-net
    # re-push must not ring it again.
    ns = _harness({"alice": _owner_account()})
    rung = []

    async def notify_account_event(_env, owner, topic):
        rung.append((owner, topic))

    ns["notify_account_event"] = notify_account_event
    env = object()

    def push(session):
        return asyncio.run(ns["agents_handler"](
            env, _Request("POST", _push_url(), {"sessions": [session]}),
            "alice", "proj",
        ))

    assert push(_session())["data"] == {"ok": True}
    assert rung == [("alice", "agents")]
    assert push(_session())["data"]["unchanged"] is True
    assert rung == [("alice", "agents")]
    push(_session(status="finished"))
    assert rung == [("alice", "agents"), ("alice", "agents")]


def test_agent_snapshot_push_survives_a_failed_ring():
    ns = _harness({"alice": _owner_account()})

    async def notify_account_event(_env, _owner, _topic):
        raise RuntimeError("durable object unavailable")

    ns["notify_account_event"] = notify_account_event
    pushed = asyncio.run(ns["agents_handler"](
        object(), _Request("POST", _push_url(), {"sessions": [_session()]}),
        "alice", "proj",
    ))
    assert pushed == {"status": 200, "data": {"ok": True}}


def test_post_agents_duplicate_ids_in_one_push_deduped_not_500():
    # Regression: a push containing two sessions with the same id used to hit
    # the repo_agents PRIMARY KEY (repo_bi, agent_id) on the second INSERT
    # (DELETE only runs once before the loop), producing a 500 D1_ERROR.
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    push = asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {
            "sessions": [_session(status="running"), _session(status="completed")],
        }),
        "alice", "proj",
    ))
    assert push == {"status": 200, "data": {"ok": True}}

    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
        }),
        "alice", "proj",
    ))
    agents = listed["data"]["agents"]
    assert len(agents) == 1
    assert agents[0]["status"] == "completed"


def test_post_agents_bad_or_missing_signature_rejected():
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    missing_sig = asyncio.run(ns["agents_handler"](
        env,
        _Request("POST", "https://forkmesh.test/api/repo/alice/proj/agents?ts=1000000000",
                  {"sessions": [_session()]}),
        "alice", "proj",
    ))
    assert missing_sig == {"status": 401, "data": {"error": "unauthorized"}}

    bad_sig = asyncio.run(ns["agents_handler"](
        env,
        _Request("POST", _push_url(sig="bad-sig"), {"sessions": [_session()]}),
        "alice", "proj",
    ))
    assert bad_sig == {"status": 401, "data": {"error": "unauthorized"}}


def test_agents_list_non_owner_403_forbidden():
    accounts = {
        "alice": _owner_account(),
        "mallory": {"pubkey": "PK-mallory", "status": "active",
                    "pass_hash": "h", "pass_salt": "s", "is_admin": False},
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {"sessions": [_session()]}),
        "alice", "proj",
    ))

    resp = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={
            "ownerAccount": "mallory",
            "sessionToken": ns["_account_session_token"](env, "mallory"),
        }),
        "alice", "proj",
    ))
    assert resp["status"] == 403
    assert resp["data"] == {"error": "not_authorized"}
    assert "agents" not in resp["data"]
    assert "Fix the thing" not in str(resp["data"])


def test_prompt_enqueued_then_drained_by_desktop_get():
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    sent = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "please continue",
        }),
        "alice", "proj", "42",
    ))
    assert sent == {"status": 200, "data": {"ok": True}}

    drained = asyncio.run(ns["agents_handler"](
        env, _Request("GET", _push_url()), "alice", "proj",
    ))
    assert drained["status"] == 200
    prompts = drained["data"]["prompts"]
    assert len(prompts) == 1
    assert prompts[0]["agentId"] == "42"
    assert prompts[0]["text"] == "please continue"

    drained_again = asyncio.run(ns["agents_handler"](
        env, _Request("GET", _push_url()), "alice", "proj",
    ))
    assert drained_again["data"]["prompts"] == []


def test_new_agent_prompt_carries_pasted_screenshots():
    # adhoc #78: a "start agent" prompt from the website can attach pasted
    # screenshots. They ride to the node as data: URLs on the drained prompt.
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()
    good = "data:image/png;base64,aGVsbG8="
    sent = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "look at this",
            "images": [good, "not-a-data-url", 123],
        }),
        "alice", "proj", "new",
    ))
    assert sent == {"status": 200, "data": {"ok": True}}

    drained = asyncio.run(ns["agents_handler"](
        env, _Request("GET", _push_url()), "alice", "proj",
    ))
    prompts = drained["data"]["prompts"]
    assert len(prompts) == 1
    # Only the valid data: URL survives; junk entries are dropped.
    assert prompts[0]["images"] == [good]


def test_new_agent_prompt_rejects_oversized_screenshot():
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()
    huge = "data:image/png;base64," + ("A" * ns["MAX_AGENT_PROMPT_IMAGE_BYTES"])
    sent = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "look at this",
            "images": [huge],
        }),
        "alice", "proj", "new",
    ))
    assert sent["status"] == 400
    assert sent["data"]["error"] == "image_too_large"


def test_self_asserted_owner_without_session_is_rejected():
    # The security fix: authorization requires a signed account session token, so
    # an attacker who merely claims to be the owner (as the pre-fix client did,
    # ownerAccount only) cannot list agents or inject a prompt into the owner's
    # running coding agent. A forged/garbage token must also fail.
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {"sessions": [_session()]}),
        "alice", "proj",
    ))

    no_token = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={"ownerAccount": "alice"}),
        "alice", "proj",
    ))
    assert no_token == {"status": 403, "data": {"error": "not_authorized"}}

    forged = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": "alice.9999999999999.deadbeef",
            "text": "exfiltrate secrets",
        }),
        "alice", "proj", "42",
    ))
    assert forged == {"status": 403, "data": {"error": "not_authorized"}}


def test_non_owner_forbidden_even_without_password():
    accounts = {
        "alice": _owner_account(),
        "mallory": {"pubkey": "PK-mallory", "status": "active",
                    "pass_hash": "h", "pass_salt": "s", "is_admin": False},
    }
    ns = _harness(accounts)
    env = object()

    list_resp = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={
            "ownerAccount": "mallory",
            "sessionToken": ns["_account_session_token"](env, "mallory"),
        }),
        "alice", "proj",
    ))
    assert list_resp == {"status": 403, "data": {"error": "not_authorized"}}

    prompt_resp = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "mallory",
            "sessionToken": ns["_account_session_token"](env, "mallory"),
            "text": "hijack",
        }),
        "alice", "proj", "42",
    ))
    assert prompt_resp == {"status": 403, "data": {"error": "not_authorized"}}


def test_admin_account_cannot_read_non_owned_agent_data():
    accounts = {
        "alice": _owner_account(),
        "root-admin": {"pubkey": "PK-admin", "status": "active",
                       "pass_hash": "h", "pass_salt": "s", "is_admin": True},
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {"sessions": [_session()]}),
        "alice", "proj",
    ))

    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={
            "ownerAccount": "root-admin",
            "sessionToken": ns["_account_session_token"](env, "root-admin"),
        }),
        "alice", "proj",
    ))
    assert listed == {"status": 403, "data": {"error": "not_authorized"}}
    assert "Fix the thing" not in str(listed)


def test_prompt_validates_text_and_queue_cap():
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    empty = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"), "text": "   ",
        }),
        "alice", "proj", "42",
    ))
    assert empty == {"status": 400, "data": {"error": "text_required"}}

    too_long = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "x" * 8001,
        }),
        "alice", "proj", "42",
    ))
    assert too_long == {"status": 400, "data": {"error": "text_too_long"}}

    for i in range(50):
        ok = asyncio.run(ns["agents_prompt_handler"](
            env, _Request("POST", body={
                "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
                "text": "msg %d" % i,
            }),
            "alice", "proj", "42",
        ))
        assert ok["status"] == 200
    full = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "one too many",
        }),
        "alice", "proj", "42",
    ))
    assert full == {"status": 429, "data": {"error": "prompt_queue_full"}}


def test_transcript_pushed_stripped_from_list_but_served_by_detail_endpoint():
    # adhoc #259: the desktop pushes a bounded run-log tail per session for the
    # website's agent detail page. It must NOT bloat the list payload, but the
    # per-agent transcript endpoint returns it in full for the owner.
    accounts = {"alice": _owner_account()}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {
            "sessions": [_session(transcript="line one\nline two\n")],
        }),
        "alice", "proj",
    ))

    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body={"ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "alice", "proj",
    ))
    assert listed["status"] == 200
    assert "transcript" not in listed["data"]["agents"][0]

    transcript = asyncio.run(ns["agents_transcript_handler"](
        env, _Request("POST", body={"ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "alice", "proj", "42",
    ))
    assert transcript["status"] == 200
    assert transcript["data"]["transcript"] == "line one\nline two"
    assert transcript["data"]["status"] == "running"


def test_transcript_non_owner_403_and_missing_agent_404():
    accounts = {
        "alice": _owner_account(),
        "mallory": {"pubkey": "PK-mallory", "status": "active",
                    "pass_hash": "h", "pass_salt": "s", "is_admin": False},
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {"sessions": [_session(transcript="secret")]}),
        "alice", "proj",
    ))

    forbidden = asyncio.run(ns["agents_transcript_handler"](
        env, _Request("POST", body={"ownerAccount": "mallory",
            "sessionToken": ns["_account_session_token"](env, "mallory")}),
        "alice", "proj", "42",
    ))
    assert forbidden == {"status": 403, "data": {"error": "not_authorized"}}
    assert "secret" not in str(forbidden["data"])

    missing = asyncio.run(ns["agents_transcript_handler"](
        env, _Request("POST", body={"ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice")}),
        "alice", "proj", "999",
    ))
    assert missing == {"status": 404, "data": {"error": "not_found"}}


def test_mandatory_e2ee_rejects_plaintext_session_and_prompt_payloads():
    ns = _harness({"alice": _owner_account()}, enforce_e2ee=True)
    env = object()

    session = asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {
            "sessions": [_session(
                issueTitle="must never cross in plaintext",
                transcript="private transcript tail")],
        }),
        "alice", "proj",
    ))
    assert session == {
        "status": 426,
        "data": {
            "error": "owner_encryption_required",
            "requiredKeyId": _owner_envelope()["recipients"][0]["kid"],
        },
    }
    assert ns["_repo_agents"] == {}

    prompt = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body={
            "ownerAccount": "alice",
            "sessionToken": ns["_account_session_token"](env, "alice"),
            "text": "private steering prompt",
        }),
        "alice", "proj", "42",
    ))
    assert prompt["status"] == 426
    assert prompt["data"]["error"] == "owner_encryption_required"
    assert ns["_agent_prompts"] == []

    auth = {
        "ownerAccount": "alice",
        "sessionToken": ns["_account_session_token"](env, "alice"),
    }
    empty_list = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body=auth), "alice", "proj"))
    assert empty_list["data"]["privacyBoundary"] == "owner-only-e2ee"
    assert empty_list["data"]["serverCanDecryptEncryptedAgents"] is False
    empty_drain = asyncio.run(ns["agents_handler"](
        env, _Request("GET", _push_url()), "alice", "proj"))
    assert empty_drain["data"]["privacyBoundary"] == "owner-only-e2ee"
    assert empty_drain["data"]["serverCanDecryptEncryptedPrompts"] is False


def test_missing_privacy_reader_fails_closed_instead_of_legacy_fallback():
    ns = _harness({"alice": _owner_account()})
    ns.pop("_repo_privacy_policy", None)
    response = asyncio.run(ns["agents_handler"](
        object(), _Request("POST", _push_url(), {
            "sessions": [_session(transcript="must remain local")],
        }),
        "alice", "proj",
    ))
    assert response == {
        "status": 428,
        "data": {
            "error": "active_owner_key_required",
            "setupPath": "/api/security/owner-keys",
        },
    }
    assert ns["_repo_agents"] == {}


def test_owner_sealed_session_stays_opaque_through_list_and_transcript():
    ns = _harness({"alice": _owner_account()}, enforce_e2ee=True)
    env = object()
    envelope = _owner_envelope(b"ciphertext for session snapshot")

    pushed = asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {
            "encryptedSessions": [{"id": 42, "envelope": envelope}],
        }),
        "alice", "proj",
    ))
    assert pushed["status"] == 200
    assert pushed["data"]["privacyBoundary"] == "owner-only-e2ee"
    assert pushed["data"]["serverCanDecrypt"] is False
    stored = next(iter(ns["_repo_agents"].values()))["data"]
    assert isinstance(stored, str)
    assert stored.startswith("owner-sealed-v1:")
    assert "private transcript" not in stored

    auth = {
        "ownerAccount": "alice",
        "sessionToken": ns["_account_session_token"](env, "alice"),
    }
    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body=auth), "alice", "proj"))
    assert listed["status"] == 200
    assert listed["data"]["agents"] == []
    assert listed["data"]["encryptedAgents"] == [{
        "id": 42, "envelope": envelope,
    }]
    assert listed["data"]["serverCanDecryptEncryptedAgents"] is False

    transcript = asyncio.run(ns["agents_transcript_handler"](
        env, _Request("POST", body=auth), "alice", "proj", "42"))
    assert transcript["status"] == 200
    assert transcript["data"] == {
        "ok": True,
        "encrypted": True,
        "envelope": envelope,
        "privacyBoundary": "owner-only-e2ee",
        "serverCanDecrypt": False,
    }


def test_owner_sealed_prompt_requires_local_open_and_explicit_ack():
    ns = _harness({"alice": _owner_account()}, enforce_e2ee=True)
    env = object()
    envelope = _owner_envelope(b"ciphertext for steering prompt")
    auth = {
        "ownerAccount": "alice",
        "sessionToken": ns["_account_session_token"](env, "alice"),
        "envelope": envelope,
    }

    queued = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body=auth), "alice", "proj", "42"))
    assert queued["status"] == 200
    assert queued["data"]["privacyBoundary"] == "owner-only-e2ee"
    assert queued["data"]["serverCanDecrypt"] is False

    drained = asyncio.run(ns["agents_handler"](
        env, _Request("GET", _push_url()), "alice", "proj"))
    assert drained["status"] == 200
    assert drained["data"]["prompts"] == []
    assert drained["data"]["encryptedPrompts"] == [{
        "queueId": 1,
        "agentId": 42,
        "queuedAt": 1_000_000_000,
        "envelope": envelope,
    }]
    # Owner-sealed prompts are not destructive-read: a client must first open
    # and accept the ciphertext locally, then explicitly acknowledge its id.
    assert len(ns["_agent_prompts"]) == 1

    acked = asyncio.run(ns["agents_ack_handler"](
        env,
        _Request("POST", _push_url(), {"queueIds": [1]}),
        "alice", "proj",
    ))
    assert acked == {
        "status": 200,
        "data": {"ok": True, "acknowledged": 1},
    }
    assert ns["_agent_prompts"] == []


def test_owner_sealed_write_rejects_wrong_policy_recipient():
    ns = _harness({"alice": _owner_account()}, enforce_e2ee=True)
    wrong = _owner_envelope(
        kid=_b64url(hashlib.sha256(b"another-owner-key").digest()))
    response = asyncio.run(ns["agents_handler"](
        object(), _Request("POST", _push_url(), {
            "encryptedSessions": [{"id": 42, "envelope": wrong}],
        }),
        "alice", "proj",
    ))
    assert response["status"] == 409
    assert response["data"]["error"] == "wrong_owner_key"
    assert ns["_repo_agents"] == {}


def _companion_recipient(seed):
    return {
        "kid": _b64url(hashlib.sha256(seed).digest()),
        "x25519": _b64url(b"x" * 32),
        "mlkem768": _b64url(b"m" * 1088),
        "nonce": _b64url(b"w" * 12),
        "tag": _b64url(b"g" * 16),
        "key": _b64url(b"k" * 32),
    }


def test_owner_sealed_rows_carry_companion_device_recipients():
    """The policy key must be a recipient; companion owner devices ride along.

    The desktop seals sessions to itself plus enrolled companion devices (the
    mobile node); the phone seals prompts to itself plus the desktop policy
    key. Authorization is membership of the policy key, not first position.
    """
    ns = _harness({"alice": _owner_account()}, enforce_e2ee=True)
    env = object()

    session_envelope = _owner_envelope(b"ciphertext for session snapshot")
    session_envelope["recipients"].append(
        _companion_recipient(b"phone-owner-key"))
    pushed = asyncio.run(ns["agents_handler"](
        env, _Request("POST", _push_url(), {
            "encryptedSessions": [{"id": 42, "envelope": session_envelope}],
        }),
        "alice", "proj",
    ))
    assert pushed["status"] == 200

    auth = {
        "ownerAccount": "alice",
        "sessionToken": ns["_account_session_token"](env, "alice"),
    }
    listed = asyncio.run(ns["agents_list_handler"](
        env, _Request("POST", body=auth), "alice", "proj"))
    assert listed["status"] == 200
    assert listed["data"]["encryptedAgents"] == [{
        "id": 42, "envelope": session_envelope,
    }]

    # A phone-sealed prompt lists the phone's own wrap entry first and the
    # desktop policy key second; the desktop can still open it, so it queues.
    prompt_envelope = _owner_envelope(
        b"ciphertext for steering prompt",
        kid=_b64url(hashlib.sha256(b"phone-owner-key").digest()))
    prompt_envelope["recipients"].append(_companion_recipient(b"owner-key"))
    queued = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body=dict(auth, envelope=prompt_envelope)),
        "alice", "proj", "42"))
    assert queued["status"] == 200
    assert queued["data"]["privacyBoundary"] == "owner-only-e2ee"

    # Companion recipients never substitute for the policy key itself.
    strangers = _owner_envelope(
        kid=_b64url(hashlib.sha256(b"another-owner-key").digest()))
    strangers["recipients"].append(_companion_recipient(b"phone-owner-key"))
    refused = asyncio.run(ns["agents_prompt_handler"](
        env, _Request("POST", body=dict(auth, envelope=strangers)),
        "alice", "proj", "42"))
    assert refused["status"] == 409
    assert refused["data"]["error"] == "wrong_owner_key"


def test_worker_wires_up_all_three_agent_routes():
    urls_text = (ENTRY.parent / "urls.py").read_text(encoding="utf-8")
    assert "REPO_AGENTS_RE = re.compile" in urls_text
    assert "REPO_AGENTS_LIST_RE = re.compile" in urls_text
    assert "REPO_AGENTS_PROMPT_RE = re.compile" in urls_text
    assert "async def agents_handler" in ENTRY_TEXT
    assert "async def agents_list_handler" in ENTRY_TEXT
    assert "async def agents_prompt_handler" in ENTRY_TEXT
    assert "REPO_AGENTS_LIST_RE.match(url.path)" in ENTRY_TEXT
    assert "REPO_AGENTS_PROMPT_RE.match(url.path)" in ENTRY_TEXT
    assert "REPO_AGENTS_RE.match(url.path)" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS repo_agents" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS agent_prompts" in ENTRY_TEXT


def test_issues_handler_wants_agent_uses_authorization_helper():
    # wantsAgent is gated by _authorize_owner_account (same as agents_list/prompt),
    # which now authorizes via the caller's signed session token rather than a
    # self-asserted ownerAccount string, so request is threaded through.
    start = ENTRY_TEXT.index('if meta_in.get("wantsAgent"):')
    wants_agent_body = ENTRY_TEXT[start:ENTRY_TEXT.index("meta = {", start)]
    assert "_authorize_owner_account(env, owner, data, request)" in wants_agent_body
