#!/usr/bin/env python3
"""Users-vs-nodes claim + installer link-code flows (adhoc #53).

Two link paths are covered end to end against an in-memory store:
 - website claim: claim-node parks a confirmation code on the node's record,
   the node's signed heartbeat reply carries it, claim-confirm links the two;
 - installer link code: the fresh node's registration and the installing
   desktop's signed offer rendezvous in link_codes, in either order.
"""

import ast
import asyncio
import re
from pathlib import Path

from _dashboard_shell import assembled_dashboard
from _dashboard_bundle import assembled_dashboard_js
from worker_test_helpers import json_from_request_double


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
ROUTES = ROOT / "app" / "src" / "route_handlers.py"
QT_SETUP = ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
QT_CHAT = tuple(
    ROOT / "desktop" / "src" / name for name in MAIN_WINDOW_CHAT_SOURCES
)
INSTALL_SH = ROOT / "www" / "public" / "install.sh"
NATIVE_ACCOUNT = ROOT / "server" / "account.go"

FUNCS = {
    "_account_kind", "_owned_nodes", "_generate_confirm_code", "_claim_pending",
    "_resolve_user_by_password", "_link_node_to_user", "_account_claim_node",
    "_account_claim_confirm", "_redeem_or_park_link_code", "_account_link_node",
    "_account_link_install",
    "_parked_link_code_user", "_link_code_rotation_allowed",
    "_account_reclaim_node", "_account_link_self", "_account_link_grant",
    "_link_grant_fresh", "_link_grant_acceptor",
    "_account_heartbeat",
    "_resolve_claimable_node", "_account_row_by_pubkey", "valid_node_pubkey",
    "_transfer_pending", "_admin_authorized", "_admin_request_ownership",
    "_account_ownership_transfer_confirm", "_park_ownership_transfer",
}

# The registration endpoint the installer's Go daemon calls lives in the
# request-lazy route module, which entry.py binds these same primitives into.
ROUTE_FUNCS = {"_account_finalize"}


def _selected_functions(source, wanted):
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in selected} == wanted, (
        "missing claim/link functions in " + source.name)
    return selected


def _load_functions(extra_globals):
    selected = (_selected_functions(ENTRY, FUNCS)
                + _selected_functions(ROUTES, ROUTE_FUNCS))
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    # Per-isolate hot-path memos the heartbeat path references (module-level
    # in entry.py); fresh per load for test isolation.
    namespace.setdefault("_MIRRORED_ACCOUNT_BIS", set())
    namespace.setdefault("_HEARTBEAT_BALANCE_PROBES", {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Request:
    def __init__(self, body):
        self._body = body

    async def json(self):
        return self._body


NODE_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
NODE_PUBKEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")


def _harness(accounts):
    """In-memory identity store (name -> rec) plus a link_codes table double.

    `accounts` models the authoritative users/nodes tables (the legacy accounts
    table was dropped by migration 0042). Records are stored as plain dicts;
    encrypt/decrypt are identity-shaped and blind_index is 'bi:' + value so
    lookups stay readable in assertions.
    """
    link_rows = {}
    now = [1_000_000_000]
    sessions = {}
    audit = []

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def clean_string(value, _max_length=240):
        return str(value or "")[:_max_length]

    def valid_node_name(value):
        value = (value or "").strip()
        return bool(value) and bool(NODE_NAME_RE.match(value))

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def _account_row(_env, name):
        rec = accounts.get(name)
        return "bi:" + name, (dict(rec) if rec is not None else None)

    async def _save_account(_env, name_bi, rec, email_bi=None, ip_bi=None):
        accounts[name_bi[3:]] = dict(rec)

    async def d1_all(_env, sql, *args):
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_first(_env, sql, *args):
        if "FROM users WHERE email_bi" in sql:
            email = args[0][3:]
            for name, rec in accounts.items():
                if rec.get("email") == email:
                    return {"name_bi": "bi:" + name, "data": dict(rec)}
            return None
        if "FROM nodes WHERE pubkey" in sql:
            pubkey = args[0]
            for name, rec in accounts.items():
                if (rec.get("pubkey") == pubkey and
                        (rec.get("kind") == "node" or not rec.get("pass_hash"))):
                    return {"node_bi": "bi:" + name, "data": dict(rec)}
            return None
        if "FROM link_codes" in sql:
            row = link_rows.get(args[0])
            return dict(row) if row else None
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO link_codes"):
            link_rows[args[0]] = {"data": args[1], "ts": args[2]}
            return
        if sql.startswith("DELETE FROM link_codes"):
            link_rows.pop(args[0], None)
            return
        if "account_presence" in sql:
            return
        raise AssertionError("unexpected d1_run: " + sql)

    async def decrypt_row(_env, stored, key=None):
        return dict(stored)

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def verify_password(password, _salt, _hash):
        return password == "correct horse battery staple"

    async def ed25519_verify(_pubkey, _sig, _canonical):
        return True

    async def _is_admin(_env, name):
        return bool(accounts.get(name, {}).get("is_admin"))

    def clean_avatar_png(_value):
        return "", ""

    async def _solana_balance_lamports(_env, _wallet):
        return None

    def _random_bytes(n):
        return bytes([7] * n)  # deterministic -> code "434247"

    def _ts_ok(_ts):
        return True

    def _safe_error_text(error):
        return repr(error)

    async def _award_badge(env, name, name_bi, slug, granted_by="system"):
        # Badge awarding (adhoc #370) is covered in isolation by
        # test_badges.py; this harness only needs the call site to resolve.
        return None

    # Registration side effects _account_finalize performs around the identity
    # decisions under test; each is covered by its own suite.
    async def username_moderation_error(_env, _name):
        return None

    def established_username_moderation_error(_name):
        return ""

    async def log_blocked_signup(*_args, **_kwargs):
        return None

    async def hash_password(_password):
        return "salt", "hash"

    async def touch_registered_node(_env, _name_bi, _rec):
        return None

    async def _send_verification_email(_env, _request, _name, _email):
        return True

    async def _enqueue_verification(_env, _name_bi, _name, _email):
        return None

    async def _account_public_payload(_env, rec, **_kwargs):
        return {"name": rec.get("name", ""), "sessionToken": "session-token"}

    def _request_account_session_token(_request, payload=None):
        token = str((payload or {}).get("sessionToken", "") or "")
        return token, not token

    async def _authed_account_name(_env, _request, data=None):
        token = str((data or {}).get("sessionToken", "") or "")
        return sessions.get(token, "")

    async def _account_session_record(_env, _request, data=None):
        name = sessions.get(str((data or {}).get("sessionToken", "") or ""), "")
        rec = accounts.get(name)
        return ("bi:" + name, dict(rec)) if rec is not None else ("", None)

    async def _verify_owner_signature(_env, owner, sig, canonical):
        rec = accounts.get(owner) or {}
        pub = rec.get("pubkey", "")
        return bool(pub) and await namespace["ed25519_verify"](
            pub, sig, canonical)

    async def _audit_sensitive_action(_env, actor, action, target_type="",
                                      target="", outcome="success",
                                      details=None):
        audit.append({"actor": actor, "action": action, "target": target,
                      "outcome": outcome, "details": dict(details or {})})

    namespace = _load_functions({
        "Date": _DateStub,
        "_award_badge": _award_badge,
        "clean_string": clean_string,
        "valid_node_name": valid_node_name,
        "json_response": json_response,
        "blind_index": blind_index,
        "_account_row": _account_row,
        "_save_account": _save_account,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "verify_password": verify_password,
        "ed25519_verify": ed25519_verify,
        "_is_admin": _is_admin,
        "_is_admin_flag": _is_admin,
        "_is_main_relay": lambda _env: False,
        "clean_avatar_png": clean_avatar_png,
        "_solana_balance_lamports": _solana_balance_lamports,
        "_random_bytes": _random_bytes,
        "_ts_ok": _ts_ok,
        "_safe_error_text": _safe_error_text,
        "username_moderation_error": username_moderation_error,
        "established_username_moderation_error":
            established_username_moderation_error,
        "log_blocked_signup": log_blocked_signup,
        "NODE_FINALIZE_PATH": "/api/accounts/finalize",
        "hash_password": hash_password,
        "touch_registered_node": touch_registered_node,
        "_send_verification_email": _send_verification_email,
        "_enqueue_verification": _enqueue_verification,
        "_account_public_payload": _account_public_payload,
        "_request_account_session_token": _request_account_session_token,
        "_authed_account_name": _authed_account_name,
        "_account_session_record": _account_session_record,
        "_verify_owner_signature": _verify_owner_signature,
        "_audit_sensitive_action": _audit_sensitive_action,
        "LINK_GRANT_MAX_AGE_MS": 5 * 60 * 1000,
        "LINK_GRANT_MAX_FUTURE_MS": 60 * 1000,
        "LINK_GRANT_NONCE_RE": re.compile(r"^[A-Za-z0-9_-]{16,64}$"),
        "_account_session_device_label": lambda _request, desktop=False: "",
        "_account_session_client_ip": lambda _request: "",
        "_account_session_cookie": lambda token: "forkmesh_session=" + token,
        "_transient_client_address": lambda _request: "",
        "_signup_metadata": lambda _request: {"at": now[0]},
        "MAX_NODE_NAME": 63,
        "CLAIM_CODE_TTL_MS": 10 * 60 * 1000,
        "CLAIM_CODE_MAX_ATTEMPTS": 5,
        "OWNERSHIP_TRANSFER_TTL_MS": 24 * 60 * 60 * 1000,
        "LINK_CODE_TTL_MS": 30 * 60 * 1000,
        "LINK_CODE_RE": re.compile(r"^[0-9]{6}$"),
        "LINK_INSTALL_COOLDOWN_MS": 10 * 1000,
        "SOLANA_RE": re.compile(r"^solana-[a-z0-9]{4,60}$"),
        "NODE_PUBKEY_RE": NODE_PUBKEY_RE,
    })
    namespace["_now"] = now
    namespace["_link_rows"] = link_rows
    namespace["_sessions"] = sessions
    namespace["_audit"] = audit
    return namespace


def _user_rec(name="alice", email="alice@example.com"):
    return {
        "name": name, "email": email, "status": "active",
        "pass_salt": "salt", "pass_hash": "hash", "pubkey": "PK-" + name,
    }


def _node_rec(name="mirror1"):
    return {"name": name, "status": "active", "pubkey": "PK-" + name}


def _auth(extra):
    return {"identifier": "alice@example.com",
            "password": "correct horse battery staple", **extra}


def test_account_kind_can_be_pinned_independently_of_password():
    ns = _harness({})

    assert ns["_account_kind"]({"pass_hash": "hash"}) == "user"
    assert ns["_account_kind"]({"pubkey": "PK-node"}) == "node"
    assert ns["_account_kind"]({"kind": "node", "pass_hash": "hash"}) == "node"
    assert ns["_account_kind"]({"kind": "user", "pubkey": "PK-node"}) == "user"


def test_claim_flow_links_node_via_heartbeat_code():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    started = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "mirror1"}))))
    assert started["status"] == 201
    assert started["data"]["pending"] is True
    # The code never leaves via the website response.
    assert "code" not in str(started["data"])
    pending = accounts["mirror1"]["claim_pending"]
    assert pending["user"] == "alice"
    assert re.fullmatch(r"[0-9]{6}", pending["code"])

    # Only the node (signed heartbeat) is shown the code.
    beat = asyncio.run(ns["_account_heartbeat"](env, _Request(
        {"nodeName": "mirror1", "ts": "1", "sig": "s"})))
    assert beat["status"] == 200
    assert beat["data"]["claim"] == {"user": "alice", "code": pending["code"]}

    # A wrong code is rejected and counted.
    wrong = asyncio.run(ns["_account_claim_confirm"](
        env, _Request(_auth({"nodeId": "mirror1", "code": "000000"}))))
    assert wrong["status"] == 401
    assert wrong["data"]["error"] == "bad_code"
    assert accounts["mirror1"]["claim_pending"]["attempts"] == 1

    confirmed = asyncio.run(ns["_account_claim_confirm"](
        env, _Request(_auth({"nodeId": "mirror1", "code": pending["code"]}))))
    assert confirmed["status"] == 200
    assert confirmed["data"]["linked"] is True
    assert confirmed["data"]["nodes"] == ["mirror1"]
    assert accounts["mirror1"]["owner"] == "alice"
    assert "claim_pending" not in accounts["mirror1"]
    assert accounts["alice"]["nodes"] == ["mirror1"]


def test_owned_node_heartbeat_reports_its_user_admin_status():
    accounts = {
        "admin1": _admin_rec(),
        "mirror1": dict(_node_rec(), owner="admin1"),
    }
    ns = _harness(accounts)

    beat = asyncio.run(ns["_account_heartbeat"](
        object(), _Request({"nodeName": "mirror1", "ts": "1", "sig": "s"})))

    assert beat["status"] == 200
    assert beat["data"]["isAdmin"] is True


def test_claim_node_accepts_pubkey_as_node_id():
    # issue #351: the desktop app's own profile card calls the node's Ed25519
    # public key its "Node ID", so a user pasting that value into the
    # website's claim form must resolve to the node's actual account name,
    # not be rejected as malformed.
    pubkey = "A" * 43
    accounts = {"alice": _user_rec(), "mirror1": dict(_node_rec(), pubkey=pubkey)}
    ns = _harness(accounts)
    env = object()

    started = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": pubkey}))))
    assert started["status"] == 201
    assert started["data"]["nodeId"] == "mirror1"
    pending = accounts["mirror1"]["claim_pending"]

    confirmed = asyncio.run(ns["_account_claim_confirm"](
        env, _Request(_auth({"nodeId": pubkey, "code": pending["code"]}))))
    assert confirmed["status"] == 200
    assert confirmed["data"]["nodeId"] == "mirror1"
    assert accounts["mirror1"]["owner"] == "alice"
    assert accounts["alice"]["nodes"] == ["mirror1"]

    # A key with no matching account is a 404, same as an unknown name.
    missing = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "B" * 43}))))
    assert missing["status"] == 404
    assert missing["data"]["error"] == "no_such_node"

    # Something that's neither a name nor a key-shaped value is still 400.
    malformed = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "!!!not-valid!!!"}))))
    assert malformed["status"] == 400
    assert malformed["data"]["error"] == "invalid_node_id"


def test_claim_rejections():
    accounts = {
        "alice": _user_rec(),
        "bob": _user_rec("bob", "bob@example.com"),
        "mirror1": dict(_node_rec(), owner="bob"),
    }
    ns = _harness(accounts)
    env = object()

    bad_pass = asyncio.run(ns["_account_claim_node"](env, _Request(
        {"identifier": "alice@example.com", "password": "wrong",
         "nodeId": "mirror1"})))
    assert bad_pass["status"] == 401

    not_node = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "bob"}))))
    assert not_node["status"] == 403
    assert not_node["data"]["error"] == "not_a_node"

    owned = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "mirror1"}))))
    assert owned["status"] == 409
    assert owned["data"]["error"] == "node_already_owned"

    self_claim = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "alice"}))))
    assert self_claim["status"] == 400
    assert self_claim["data"]["error"] == "cannot_claim_self"

    missing = asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "ghost"}))))
    assert missing["status"] == 404


def test_expired_claim_is_cleaned_up_by_heartbeat():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "mirror1"}))))
    ns["_now"][0] += 11 * 60 * 1000  # past CLAIM_CODE_TTL_MS

    confirm = asyncio.run(ns["_account_claim_confirm"](
        env, _Request(_auth({"nodeId": "mirror1",
                             "code": accounts["mirror1"]["claim_pending"]["code"]}))))
    assert confirm["status"] == 404
    assert confirm["data"]["error"] == "no_pending_claim"

    beat = asyncio.run(ns["_account_heartbeat"](env, _Request(
        {"nodeName": "mirror1", "ts": "1", "sig": "s"})))
    assert "claim" not in beat["data"]
    assert "claim_pending" not in accounts["mirror1"]


def test_too_many_wrong_codes_invalidate_the_claim():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_account_claim_node"](
        env, _Request(_auth({"nodeId": "mirror1"}))))
    for _ in range(5):
        asyncio.run(ns["_account_claim_confirm"](
            env, _Request(_auth({"nodeId": "mirror1", "code": "000000"}))))
    assert "claim_pending" not in accounts["mirror1"]


def _admin_rec(name="admin1"):
    return {"name": name, "status": "active", "pubkey": "PK-" + name,
            "is_admin": True, "totp_enrolled": True}


def test_admin_ownership_transfer_approved_via_node_heartbeat():
    accounts = {"admin1": _admin_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    requested = asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "mirror1", "ts": "1", "sig": "s"})))
    assert requested["status"] == 201
    pending = accounts["mirror1"]["ownership_transfer_pending"]
    assert pending["admin"] == "admin1"

    # The prompt only ever reaches the target node via its own heartbeat.
    beat = asyncio.run(ns["_account_heartbeat"](env, _Request(
        {"nodeName": "mirror1", "ts": "1", "sig": "s"})))
    assert beat["data"]["ownershipTransfer"] == {"admin": "admin1"}

    confirmed = asyncio.run(ns["_account_ownership_transfer_confirm"](
        env, _Request({"nodeName": "mirror1", "ts": "1", "sig": "s",
                      "action": "approve"})))
    assert confirmed["status"] == 200
    assert confirmed["data"]["owner"] == "admin1"
    assert accounts["mirror1"]["owner"] == "admin1"
    assert "ownership_transfer_pending" not in accounts["mirror1"]
    assert accounts["admin1"]["nodes"] == ["mirror1"]


def test_admin_ownership_transfer_denied_leaves_owner_unchanged():
    accounts = {"admin1": _admin_rec(),
               "mirror1": dict(_node_rec(), owner="bob")}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "mirror1", "ts": "1", "sig": "s"})))

    denied = asyncio.run(ns["_account_ownership_transfer_confirm"](
        env, _Request({"nodeName": "mirror1", "ts": "1", "sig": "s",
                      "action": "deny"})))
    assert denied["status"] == 200
    assert denied["data"]["denied"] is True
    assert accounts["mirror1"]["owner"] == "bob"
    assert "ownership_transfer_pending" not in accounts["mirror1"]


def test_admin_ownership_transfer_rejections():
    accounts = {
        "admin1": _admin_rec(),
        "alice": _user_rec(),
        "mirror1": _node_rec(),
    }
    ns = _harness(accounts)
    env = object()

    not_admin = asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "alice", "target": "mirror1", "ts": "1", "sig": "s"})))
    assert not_admin["status"] == 401

    not_a_node = asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "alice", "ts": "1", "sig": "s"})))
    assert not_a_node["status"] == 403
    assert not_a_node["data"]["error"] == "not_a_node"

    self_request = asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "admin1", "ts": "1", "sig": "s"})))
    assert self_request["status"] == 400
    assert self_request["data"]["error"] == "cannot_request_self"

    missing = asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "ghost", "ts": "1", "sig": "s"})))
    assert missing["status"] == 404

    # No pending marker at all -> confirm is a no-op 404.
    no_pending = asyncio.run(ns["_account_ownership_transfer_confirm"](
        env, _Request({"nodeName": "mirror1", "ts": "1", "sig": "s",
                      "action": "approve"})))
    assert no_pending["status"] == 404
    assert no_pending["data"]["error"] == "no_pending_transfer"


def test_expired_ownership_transfer_is_cleaned_up_by_heartbeat():
    accounts = {"admin1": _admin_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_admin_request_ownership"](env, _Request(
        {"node": "admin1", "target": "mirror1", "ts": "1", "sig": "s"})))
    ns["_now"][0] += 25 * 60 * 60 * 1000  # past OWNERSHIP_TRANSFER_TTL_MS

    confirm = asyncio.run(ns["_account_ownership_transfer_confirm"](
        env, _Request({"nodeName": "mirror1", "ts": "1", "sig": "s",
                      "action": "approve"})))
    assert confirm["status"] == 404

    beat = asyncio.run(ns["_account_heartbeat"](env, _Request(
        {"nodeName": "mirror1", "ts": "1", "sig": "s"})))
    assert "ownershipTransfer" not in beat["data"]
    assert "ownership_transfer_pending" not in accounts["mirror1"]


def test_link_self_attaches_node_with_node_key_and_user_password():
    # The desktop "Log in as a user" path: the node signs with its own key and
    # supplies the user's password, so the link completes in one call with no
    # confirmation code.
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _harness(accounts)
    env = object()

    linked = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "mirror1", "ts": "1", "sig": "s"}))))
    assert linked["status"] == 200
    assert linked["data"]["linked"] is True
    assert linked["data"]["nodes"] == ["mirror1"]
    assert accounts["mirror1"]["owner"] == "alice"
    assert accounts["alice"]["nodes"] == ["mirror1"]

    # Re-running is idempotent (already linked to the same user).
    again = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "mirror1", "ts": "1", "sig": "s"}))))
    assert again["data"]["alreadyLinked"] is True


def test_link_self_rejections():
    accounts = {
        "alice": _user_rec(),
        "bob": _user_rec("bob", "bob@example.com"),
        "owned": dict(_node_rec("owned"), owner="bob"),
        "mirror1": _node_rec(),
    }
    ns = _harness(accounts)
    env = object()

    bad_pass = asyncio.run(ns["_account_link_self"](env, _Request(
        {"identifier": "alice@example.com", "password": "wrong",
         "nodeName": "mirror1", "ts": "1", "sig": "s"})))
    assert bad_pass["status"] == 401
    assert bad_pass["data"]["error"] == "invalid_credentials"

    missing = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "ghost", "ts": "1", "sig": "s"}))))
    assert missing["status"] == 404

    not_node = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "bob", "ts": "1", "sig": "s"}))))
    assert not_node["status"] == 403
    assert not_node["data"]["error"] == "not_a_node"

    self_link = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "alice", "ts": "1", "sig": "s"}))))
    assert self_link["status"] == 400
    assert self_link["data"]["error"] == "cannot_link_self"

    owned = asyncio.run(ns["_account_link_self"](env, _Request(
        _auth({"nodeName": "owned", "ts": "1", "sig": "s"}))))
    assert owned["status"] == 409
    assert owned["data"]["error"] == "node_already_owned"


def _fake_sign(pubkey, canonical):
    return "sig|" + pubkey + "|" + canonical.decode()


async def _fake_verify(pubkey, sig, canonical):
    return sig == _fake_sign(pubkey, canonical)


def _grant_harness(accounts):
    ns = _harness(accounts)
    ns["ed25519_verify"] = _fake_verify
    ns["_ts_ok"] = lambda ts: abs(ns["_now"][0] - int(ts)) <= 5 * 60 * 1000
    ns["_sessions"].update({"sess-" + name: name for name, rec in
                            accounts.items() if rec.get("pass_hash")})
    return ns


def _v1(ns, node="mirror1", ts=None, session="", user=None):
    ts = str(ns["_now"][0] if ts is None else ts)
    canonical = ("forkmesh-link-grant-v1\n" + node + "\n" + ts).encode()
    body = {"nodeName": node, "ts": ts,
            "sig": _fake_sign("PK-" + node, canonical)}
    if session:
        body["sessionToken"] = session
    if user is not None:
        body["user"] = user
    return body


def _v2(ns, target, node="mirror1", ts=None, nonce="n0nce-0123456789abcdef",
        signer=None):
    ts = str(ns["_now"][0] if ts is None else ts)
    canonical = ("forkmesh-link-grant-v2\n" + node + "\n" + target + "\n" +
                 ts + "\n" + nonce).encode()
    return {"nodeName": node, "user": target, "ts": ts, "nonce": nonce,
            "sig": _fake_sign("PK-" + (signer or node), canonical)}


def _accept(ns, body, account, ts=None):
    ts = str(ns["_now"][0] if ts is None else ts)
    canonical = ("forkmesh-link-accept-v1\n" + body["nodeName"] + "\n" +
                 account + "\n" + body["sig"] + "\n" + ts).encode()
    return dict(body, user=account, acceptTs=ts,
                acceptSig=_fake_sign("PK-" + account, canonical))


def _link(ns, body):
    return asyncio.run(ns["_account_link_grant"](object(), _Request(body)))


def test_link_grant_links_node_to_the_session_account():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _grant_harness(accounts)
    grant = _v1(ns, session="sess-alice", user="alice")

    linked = _link(ns, grant)
    assert linked["status"] == 200
    assert linked["data"]["linked"] is True
    assert linked["data"]["user"] == "alice"
    assert accounts["mirror1"]["owner"] == "alice"
    assert accounts["alice"]["nodes"] == ["mirror1"]
    assert ns["_link_rows"], "redeemed grant must be parked as used"
    assert ns["_audit"][-1]["outcome"] == "success"
    assert ns["_audit"][-1]["details"]["via"] == "session"
    assert ns["_audit"][-1]["details"]["to"] == "alice"

    again = _link(ns, grant)
    assert again["status"] == 200
    assert again["data"]["alreadyLinked"] is True

    del accounts["mirror1"]["owner"]
    replay = _link(ns, grant)
    assert replay["status"] == 409
    assert replay["data"]["error"] == "grant_used"
    assert "owner" not in accounts["mirror1"]
    assert ns["_audit"][-1]["outcome"] == "denied"
    assert ns["_audit"][-1]["details"]["reason"] == "grant_used"


def test_link_grant_owner_comes_from_session_not_body():
    accounts = {"alice": _user_rec(),
                "bob": _user_rec("bob", "bob@example.com"),
                "mirror1": _node_rec()}
    ns = _grant_harness(accounts)

    anonymous = _link(ns, _v1(ns, user="bob"))
    assert anonymous["status"] == 401
    assert anonymous["data"]["error"] == "session_required"
    assert "owner" not in accounts["mirror1"]
    assert ns["_audit"][-1]["details"]["reason"] == "session_required"
    assert ns["_audit"][-1]["details"]["named"] == "bob"

    mismatch = _link(ns, _v1(ns, session="sess-alice", user="bob"))
    assert mismatch["status"] == 403
    assert mismatch["data"]["error"] == "grant_user_mismatch"
    assert "owner" not in accounts["mirror1"]
    assert "nodes" not in accounts["bob"]

    app_token = _link(ns, _v1(ns, session="fmat_app", user="bob"))
    assert app_token["data"]["error"] == "session_required"

    ns["_now"][0] += 1
    unnamed = _link(ns, _v1(ns, session="sess-bob"))
    assert unnamed["status"] == 200
    assert accounts["mirror1"]["owner"] == "bob"


def test_attacker_with_node_key_cannot_assign_node_to_another_user():
    accounts = {"alice": _user_rec(),
                "victim": _user_rec("victim", "victim@example.com"),
                "mirror1": _node_rec()}
    ns = _grant_harness(accounts)
    grant = _v2(ns, "victim")

    unaccepted = _link(ns, grant)
    assert unaccepted["status"] == 401
    assert unaccepted["data"]["error"] == "session_required"

    wrong_session = _link(ns, dict(grant, sessionToken="sess-alice"))
    assert wrong_session["status"] == 403
    assert wrong_session["data"]["error"] == "grant_user_mismatch"

    forged_accept = dict(_accept(ns, grant, "victim"), acceptSig="made-up")
    rejected = _link(ns, forged_accept)
    assert rejected["status"] == 401
    assert rejected["data"]["error"] == "bad_accept_signature"

    by_alice_key = _accept(ns, grant, "alice")
    by_alice_key["user"] = "victim"
    assert _link(ns, by_alice_key)["data"]["error"] == "bad_accept_signature"

    alice_accepts = _link(ns, _accept(ns, grant, "alice"))
    assert alice_accepts["status"] == 401
    assert alice_accepts["data"]["error"] == "bad_signature"

    assert "owner" not in accounts["mirror1"]
    assert "nodes" not in accounts["victim"]
    denied = [row for row in ns["_audit"] if row["outcome"] == "denied"]
    assert {row["details"]["reason"] for row in denied} == {
        "session_required", "grant_user_mismatch", "bad_accept_signature"}


def test_named_grant_accepted_by_target_session_or_key():
    accounts = {"jett": _user_rec("jett", "jett@example.com"),
                "old": _user_rec("old", "old@example.com"),
                "n1": dict(_node_rec("n1"), owner="old"),
                "n2": dict(_node_rec("n2"), owner="old")}
    accounts["old"]["nodes"] = ["n1", "n2"]
    ns = _grant_harness(accounts)

    by_session = _link(ns, dict(_v2(ns, "jett", node="n1"),
                                sessionToken="sess-jett"))
    assert by_session["status"] == 200
    assert accounts["n1"]["owner"] == "jett"

    grant = _v2(ns, "jett", node="n2", nonce="another-nonce-0123456")
    by_key = _link(ns, _accept(ns, grant, "jett"))
    assert by_key["status"] == 200
    assert accounts["n2"]["owner"] == "jett"
    assert accounts["old"]["nodes"] == []
    assert ns["_audit"][-1]["details"] == {
        "grant": "v2", "via": "signature", "from": "old", "to": "jett"}

    accounts["n2"]["owner"] = "old"
    replay = _link(ns, _accept(ns, grant, "jett"))
    assert replay["status"] == 409
    assert replay["data"]["error"] == "grant_used"

    tampered = dict(_v2(ns, "old", node="n2"), user="jett",
                    sessionToken="sess-jett")
    assert _link(ns, tampered)["data"]["error"] == "bad_signature"

    bad_nonce = _link(ns, dict(_v2(ns, "jett", node="n2", nonce="short"),
                               sessionToken="sess-jett"))
    assert bad_nonce["status"] == 400


def test_link_grant_expiry():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec()}
    ns = _grant_harness(accounts)
    now = ns["_now"][0]

    stale = _link(ns, _v1(ns, ts=now - 5 * 60 * 1000 - 1, session="sess-alice"))
    assert stale["status"] == 401
    assert stale["data"]["error"] == "grant_expired"
    assert ns["_audit"][-1]["details"]["reason"] == "grant_expired"

    future = _link(ns, _v1(ns, ts=now + 2 * 60 * 1000, session="sess-alice"))
    assert future["data"]["error"] == "grant_expired"

    grant = _v2(ns, "alice")
    old_accept = _link(ns, _accept(ns, grant, "alice", ts=now - 10 * 60 * 1000))
    assert old_accept["data"]["error"] == "accept_expired"

    fresh = _link(ns, _v1(ns, ts=now - 60 * 1000, session="sess-alice"))
    assert fresh["status"] == 200
    assert accounts["mirror1"]["owner"] == "alice"


def test_link_grant_rejections():
    accounts = {"alice": _user_rec(), "mirror1": _node_rec(),
                "stray": _node_rec("stray")}
    ns = _grant_harness(accounts)
    ns["_sessions"]["sess-stray"] = "stray"
    ns["_sessions"]["sess-ghost"] = "ghost"

    missing = _link(ns, _v1(ns, node="ghost", session="sess-alice"))
    assert missing["status"] == 404
    assert missing["data"]["error"] == "no_such_node"

    ghost_user = _link(ns, _v1(ns, session="sess-ghost"))
    assert ghost_user["status"] == 404
    assert ghost_user["data"]["error"] == "no_such_user"

    not_user = _link(ns, _v1(ns, session="sess-stray"))
    assert not_user["status"] == 403
    assert not_user["data"]["error"] == "not_a_user"

    forged = dict(_v1(ns, session="sess-alice"), sig="forged")
    rejected = _link(ns, forged)
    assert rejected["status"] == 401
    assert rejected["data"]["error"] == "bad_signature"

    self_account = _link(ns, dict(_v1(ns, node="alice"),
                                  sessionToken="sess-alice"))
    assert self_account["data"]["selfAccount"] is True
    assert "owner" not in accounts["alice"]
    assert "owner" not in accounts["mirror1"]


def test_link_code_rendezvous_node_registers_first():
    accounts = {"alice": _user_rec(), "mirror2": _node_rec("mirror2")}
    ns = _harness(accounts)
    env = object()

    # The fresh node's finalize parks its half first…
    parked = asyncio.run(ns["_redeem_or_park_link_code"](
        env, "123456", node="mirror2"))
    assert parked == {"linked": False}

    # …then the installing desktop (a user account) offers the same code.
    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "123456", "ts": "1", "sig": "s"})))
    assert offer["status"] == 200
    assert offer["data"]["linked"] is True
    assert offer["data"]["node"] == "mirror2"
    assert accounts["mirror2"]["owner"] == "alice"
    assert accounts["alice"]["nodes"] == ["mirror2"]
    assert not ns["_link_rows"], "code must be burned after linking"


def test_link_code_rendezvous_offer_arrives_first():
    accounts = {"alice": _user_rec(), "mirror2": _node_rec("mirror2")}
    ns = _harness(accounts)
    env = object()

    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "654321", "ts": "1", "sig": "s"})))
    assert offer["status"] == 202
    assert offer["data"]["pending"] is True

    redeemed = asyncio.run(ns["_redeem_or_park_link_code"](
        env, "654321", node="mirror2"))
    assert redeemed["linked"] is True
    assert redeemed["user"] == "alice"
    assert accounts["mirror2"]["owner"] == "alice"
    assert accounts["alice"]["nodes"] == ["mirror2"]
    assert not ns["_link_rows"]


def test_link_install_mints_a_code_the_fresh_node_redeems():
    accounts = {"alice": _user_rec(), "mirror3": _node_rec("mirror3")}
    ns = _harness(accounts)
    ns["_sessions"]["sess-alice"] = "alice"
    env = object()

    minted = asyncio.run(ns["_account_link_install"](
        env, _Request({"sessionToken": "sess-alice"})))
    assert minted["status"] == 201
    code = minted["data"]["code"]
    assert re.fullmatch(r"[0-9]{6}", code)
    assert minted["data"]["user"] == "alice"

    redeemed = asyncio.run(ns["_redeem_or_park_link_code"](
        env, code, node="mirror3"))
    assert redeemed["linked"] is True
    assert accounts["mirror3"]["owner"] == "alice"
    assert "bi:link:" + code not in ns["_link_rows"]


def test_link_install_needs_a_session_and_is_throttled():
    accounts = {"alice": _user_rec()}
    ns = _harness(accounts)
    ns["_sessions"]["sess-alice"] = "alice"
    env = object()

    anonymous = asyncio.run(ns["_account_link_install"](env, _Request({})))
    assert anonymous["status"] == 401

    first = asyncio.run(ns["_account_link_install"](
        env, _Request({"sessionToken": "sess-alice"})))
    assert first["status"] == 201
    again = asyncio.run(ns["_account_link_install"](
        env, _Request({"sessionToken": "sess-alice"})))
    assert again["status"] == 429

    ns["_now"][0] += 11 * 1000
    later = asyncio.run(ns["_account_link_install"](
        env, _Request({"sessionToken": "sess-alice"})))
    assert later["status"] == 503


def test_link_install_never_reuses_a_parked_code():
    accounts = {"alice": _user_rec(), "stranger": _node_rec("stranger")}
    ns = _harness(accounts)
    ns["_sessions"]["sess-alice"] = "alice"
    env = object()
    asyncio.run(ns["_redeem_or_park_link_code"](
        env, ns["_generate_confirm_code"](), node="stranger"))

    minted = asyncio.run(ns["_account_link_install"](
        env, _Request({"sessionToken": "sess-alice"})))
    assert minted["status"] == 503
    assert "owner" not in accounts["stranger"]


def test_installer_reclaim_rotates_the_owners_node_key_offer_first():
    new_pubkey = "A" * 43
    accounts = {
        "oldowner": dict(_user_rec("oldowner", "old@example.com"),
                         nodes=["mirror2"]),
        "mirror2": dict(_node_rec("mirror2"), owner="oldowner",
                        pubkey="old-node-key"),
    }
    ns = _harness(accounts)
    env = object()

    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "oldowner", "code": "222333", "ts": "1", "sig": "s"})))
    assert offer["status"] == 202

    reclaim = asyncio.run(ns["_account_reclaim_node"](env, _Request(
        {"nodeName": "mirror2", "pubkey": new_pubkey, "linkCode": "222333",
         "ts": "1", "sig": "s"})))
    assert reclaim["status"] == 200
    assert reclaim["data"]["linked"] is True
    assert reclaim["data"]["user"] == "oldowner"
    assert accounts["mirror2"]["owner"] == "oldowner"
    assert accounts["mirror2"]["pubkey"] == new_pubkey
    assert accounts["oldowner"]["nodes"] == ["mirror2"]
    assert not ns["_link_rows"]


def test_installer_reclaim_rotates_the_owners_node_key_node_first():
    new_pubkey = "B" * 43
    accounts = {
        "oldowner": dict(_user_rec("oldowner", "old@example.com"),
                         nodes=["mirror3"]),
        "mirror3": dict(_node_rec("mirror3"), owner="oldowner",
                        pubkey="old-node-key"),
    }
    ns = _harness(accounts)
    env = object()

    reclaim = asyncio.run(ns["_account_reclaim_node"](env, _Request(
        {"nodeName": "mirror3", "pubkey": new_pubkey, "linkCode": "333444",
         "ts": "1", "sig": "s"})))
    assert reclaim["status"] == 202
    assert reclaim["data"]["pending"] is True
    assert accounts["mirror3"]["pubkey"] == "old-node-key"

    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "oldowner", "code": "333444", "ts": "1", "sig": "s"})))
    assert offer["status"] == 200
    assert offer["data"]["linked"] is True
    assert accounts["mirror3"]["owner"] == "oldowner"
    assert accounts["mirror3"]["pubkey"] == new_pubkey
    assert not ns["_link_rows"]


def test_installer_reclaim_never_hands_another_users_node_to_the_caller():
    new_pubkey = "A" * 43
    accounts = {
        "alice": _user_rec(),
        "oldowner": dict(_user_rec("oldowner", "old@example.com"),
                         nodes=["mirror2", "mirror3"]),
        "mirror2": dict(_node_rec("mirror2"), owner="oldowner",
                        pubkey="old-node-key"),
        "mirror3": dict(_node_rec("mirror3"), owner="oldowner",
                        pubkey="old-node-key"),
        "stray": dict(_node_rec("stray"), pubkey="stray-key"),
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "222333", "ts": "1", "sig": "s"})))
    offer_first = asyncio.run(ns["_account_reclaim_node"](env, _Request(
        {"nodeName": "mirror2", "pubkey": new_pubkey, "linkCode": "222333",
         "ts": "1", "sig": "s"})))
    assert offer_first["status"] == 403
    assert offer_first["data"]["error"] == "not_node_owner"

    asyncio.run(ns["_account_reclaim_node"](env, _Request(
        {"nodeName": "mirror3", "pubkey": new_pubkey, "linkCode": "333444",
         "ts": "1", "sig": "s"})))
    node_first = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "333444", "ts": "1", "sig": "s"})))
    assert node_first["status"] == 403
    assert node_first["data"]["error"] == "not_node_owner"

    asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "444555", "ts": "1", "sig": "s"})))
    unowned = asyncio.run(ns["_account_reclaim_node"](env, _Request(
        {"nodeName": "stray", "pubkey": new_pubkey, "linkCode": "444555",
         "ts": "1", "sig": "s"})))
    assert unowned["status"] == 403

    for node in ("mirror2", "mirror3"):
        assert accounts[node]["owner"] == "oldowner"
        assert accounts[node]["pubkey"] == "old-node-key"
    assert "owner" not in accounts["stray"]
    assert accounts["stray"]["pubkey"] == "stray-key"
    assert accounts["oldowner"]["nodes"] == ["mirror2", "mirror3"]
    assert "nodes" not in accounts["alice"] or accounts["alice"]["nodes"] == []


def _key_scoped_verify(accepted_pubkey):
    """ed25519_verify that only the named key's signatures pass."""
    async def verify(pubkey, _sig, _canonical):
        return pubkey == accepted_pubkey
    return verify


def _finalize(ns, env, node, pubkey, link_code):
    return asyncio.run(ns["_account_finalize"](env, _Request(
        {"nodeName": node, "email": "", "password": "", "pubkey": pubkey,
         "ts": "1", "sig": "s", "linkCode": link_code})))


def test_finalize_reclaims_a_reused_node_name_with_the_installer_link_code():
    new_pubkey = "C" * 43
    accounts = {
        "alice": _user_rec(),
        "oldowner": dict(_user_rec("oldowner", "old@example.com"),
                         nodes=["mirror20"]),
        "mirror20": dict(_node_rec("mirror20"), owner="oldowner",
                         pubkey="retired-machine-key"),
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "920843", "ts": "1", "sig": "s"})))
    ns["ed25519_verify"] = _key_scoped_verify(new_pubkey)
    hijack = _finalize(ns, env, "mirror20", new_pubkey, "920843")
    assert hijack["status"] == 401
    assert hijack["data"]["error"] == "bad_signature"
    assert accounts["mirror20"]["pubkey"] == "retired-machine-key"
    assert accounts["mirror20"]["owner"] == "oldowner"

    ns["ed25519_verify"] = _key_scoped_verify("PK-oldowner")
    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "oldowner", "code": "920844", "ts": "1", "sig": "s"})))
    assert offer["status"] == 202

    ns["ed25519_verify"] = _key_scoped_verify(new_pubkey)
    finalized = _finalize(ns, env, "mirror20", new_pubkey, "920844")
    assert finalized["status"] == 201
    assert accounts["mirror20"]["pubkey"] == new_pubkey
    assert accounts["mirror20"]["owner"] == "oldowner"
    assert accounts["oldowner"]["nodes"] == ["mirror20"]
    assert "bi:link:920844" not in ns["_link_rows"]


def test_finalize_reclaim_needs_a_live_offer_a_self_signature_and_a_node():
    new_pubkey = "D" * 43
    accounts = {
        "alice": _user_rec(),
        "mirror21": dict(_node_rec("mirror21"), pubkey="retired-machine-key"),
    }
    ns = _harness(accounts)
    env = object()

    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "920843", "ts": "1", "sig": "s"})))
    assert offer["status"] == 202

    # No offer parked on the code this daemon presents.
    ns["ed25519_verify"] = _key_scoped_verify(new_pubkey)
    unclaimed = _finalize(ns, env, "mirror21", new_pubkey, "111111")
    assert unclaimed["status"] == 401
    assert unclaimed["data"]["error"] == "bad_signature"

    # The offer is live, but the request is not signed by the key it presents.
    ns["ed25519_verify"] = _key_scoped_verify("some-other-key")
    forged = _finalize(ns, env, "mirror21", new_pubkey, "920843")
    assert forged["status"] == 401
    assert forged["data"]["error"] == "bad_signature"

    # A user account is never re-homed onto a fresh key by this path.
    ns["ed25519_verify"] = _key_scoped_verify(new_pubkey)
    hijack = _finalize(ns, env, "alice", new_pubkey, "920843")
    assert hijack["status"] == 401
    assert hijack["data"]["error"] == "bad_signature"

    # An expired offer is no proof either.
    ns["_now"][0] += 31 * 60 * 1000  # past LINK_CODE_TTL_MS
    stale = _finalize(ns, env, "mirror21", new_pubkey, "920843")
    assert stale["status"] == 401
    assert stale["data"]["error"] == "bad_signature"

    assert accounts["mirror21"]["pubkey"] == "retired-machine-key"
    assert accounts["alice"]["pubkey"] == "PK-alice"
    assert "owner" not in accounts["mirror21"]


def test_finalize_still_refuses_an_unproven_key_on_a_fresh_reservation():
    # The ordinary bound-key path is untouched: a reserved account whose key
    # signs nothing verifiable is still refused, link code or not.
    accounts = {"mirror22": dict(_node_rec("mirror22"), status="reserved")}
    ns = _harness(accounts)
    env = object()

    async def _reject(_pubkey, _sig, _canonical):
        return False
    ns["ed25519_verify"] = _reject
    refused = _finalize(ns, env, "mirror22", "E" * 43, "920843")
    assert refused["status"] == 401
    assert refused["data"]["error"] == "bad_signature"
    assert accounts["mirror22"]["pubkey"] == "PK-mirror22"


def test_link_offer_resolves_the_user_behind_an_owned_node():
    # An owned node's key can offer a code on behalf of its owning user; a
    # bare, unowned node account cannot confer ownership at all.
    accounts = {
        "alice": _user_rec(),
        "mirror1": dict(_node_rec(), owner="alice"),
        "stray": _node_rec("stray"),
        "mirror3": _node_rec("mirror3"),
    }
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_redeem_or_park_link_code"](env, "111222", node="mirror3"))
    via_owned = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "mirror1", "code": "111222", "ts": "1", "sig": "s"})))
    assert via_owned["data"]["user"] == "alice"
    assert accounts["mirror3"]["owner"] == "alice"

    refused = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "stray", "code": "333444", "ts": "1", "sig": "s"})))
    assert refused["status"] == 403
    assert refused["data"]["error"] == "no_user_account"


def test_expired_link_code_is_not_redeemed():
    accounts = {"alice": _user_rec(), "mirror2": _node_rec("mirror2")}
    ns = _harness(accounts)
    env = object()

    asyncio.run(ns["_redeem_or_park_link_code"](env, "999888", node="mirror2"))
    ns["_now"][0] += 31 * 60 * 1000  # past LINK_CODE_TTL_MS

    offer = asyncio.run(ns["_account_link_node"](env, _Request(
        {"nodeName": "alice", "code": "999888", "ts": "1", "sig": "s"})))
    assert offer["status"] == 202, "stale half must not link; offer re-parks"
    assert "owner" not in accounts["mirror2"]


def test_wire_contracts_across_worker_qt_and_installer():
    entry = ENTRY.read_text(encoding="utf-8")
    qt_setup = QT_SETUP.read_text(encoding="utf-8")
    qt_chat = "".join(path.read_text(encoding="utf-8") for path in QT_CHAT)
    install = INSTALL_SH.read_text(encoding="utf-8")

    # The link-offer canonical string matches on both ends.
    assert '"forkmesh-link-v1\\n" + name + "\\n" + code + "\\n" + ts' in entry
    assert '"forkmesh-link-v1\\n" + signer + "\\n" + code + "\\n" + ts' in qt_chat

    # The installer's printed marker is exactly what the desktop app scans for.
    assert 'FORKMESH LINK CODE: $FORKMESH_LINK_CODE' in install
    assert 'FORKMESH LINK CODE:\\\\s*([0-9]{6})' in qt_chat

    # The native node registers the code directly; the desktop is never
    # daemonized as the server.
    assert '--register-link-code "$FORKMESH_LINK_CODE"' in install
    assert "forkmesh-mirror-node.service" in install
    native_account = NATIVE_ACCOUNT.read_text(encoding="utf-8")
    assert '"linkCode": linkCode' in native_account
    assert 'data.get("linkCode", "")' in entry
    assert '"/api/accounts/reclaim-node"' in entry
    assert '"forkmesh-reclaim-node-v1\\n" + name + "\\n" + pubkey +' in entry
    assert '"forkmesh-reclaim-node-v1\\n"' in native_account
    assert 'reclaimNodeAccount(ctx, base, node, linkCode, identity)' in native_account

    # The heartbeat is the only channel that carries the claim code out.
    assert 'response["claim"]' in entry
    assert 'resp.value(QStringLiteral("claim")).toObject()' in qt_setup

    # "Log in as a user" (link-self): the node signs its own key over the same
    # canonical string the worker verifies, and POSTs it to /link-self.
    assert '"forkmesh-link-self-v1\\n" + node_name + "\\n" + identifier +' in entry
    assert '"/api/accounts/link-self"' in entry
    assert '"forkmesh-link-self-v1\\n" + node + "\\n" + id + "\\n" + ts' in qt_chat
    assert 'accountsApiUrl("link-self")' in qt_chat

    # The public account lookup lists the nodes linked to the account (the same
    # data _account_public_payload already exposes), so a node's profile can
    # render its user's whole fleet — the "show the linked nodes" ask.
    assert '"nodes": _owned_nodes(rec),' in entry
    assert 'profileNodesFromJson(resp.value(QStringLiteral("nodes")))' in qt_chat
    # The profile knows when its own account is a user (so it stops nagging to
    # "log in as a user" and instead lists the nodes it owns).
    assert 'm_profileIsUserAccount' in qt_chat


def test_link_grant_wire_contract_across_worker_qt_and_dashboard():
    entry = ENTRY.read_text(encoding="utf-8")
    qt_chat = "".join(path.read_text(encoding="utf-8") for path in QT_CHAT)
    # dashboard.js is built from ordered public/dashboard/js/*.js fragments.
    dashboard_js = assembled_dashboard_js()
    login_js = (ROOT / "app" / "public" / "login.js").read_text(
        encoding="utf-8")

    # The grant canonical string matches on both ends, and the endpoint exists.
    assert '"forkmesh-link-grant-v1\\n" + node_name + "\\n" + ts' in entry
    assert '"forkmesh-link-grant-v2\\n" + node_name + "\\n" + named' in entry
    assert '"forkmesh-link-accept-v1\\n" + node_name + "\\n" + user' in entry
    assert '"/api/accounts/link-grant"' in entry
    assert '"forkmesh-link-grant-v1\\n" + node + "\\n" + ts' in qt_chat

    # The desktop app opens /dashboard?link_node=…&link_ts=…&link_sig=… and the
    # dashboard redeems exactly those params against the endpoint.
    for marker in ("link_node", "link_ts", "link_sig"):
        assert marker in qt_chat
        assert marker in dashboard_js
    assert '"/api/accounts/link-grant"' in dashboard_js
    assert "payload.sessionToken = token" in dashboard_js
    assert "offerLinkGrant" in dashboard_js
    assert "redeemLinkGrant" in dashboard_js

    # The browser side asks for one explicit "Authenticate & link" click; the
    # confirm row ships in the prebuilt settings page document.
    settings_html = (ROOT / "app" / "public" / "dashboard" /
                     "settings" / "index.html").read_text(encoding="utf-8")
    assert "data-link-grant-row" in settings_html
    composed = assembled_dashboard()
    assert "data-link-grant-row" in composed
    assert "data-link-grant-confirm" in composed
    assert "data-link-grant-confirm" in dashboard_js

    # A logged-out browser bounces through login and resumes via ?next= (local
    # paths only, so the bounce can't become an open redirect). The guard rejects
    # "//host" and the "/\host" backslash bypass and confirms the resolved origin
    # matches ours.
    assert "/login?next=" in dashboard_js
    assert "nextPath()" in login_js
    assert 'value.startsWith("//")' in login_js
    assert 'new URL(value, location.origin).origin !== location.origin' in login_js


def test_dashboard_exposes_a_claim_node_panel():
    dashboard_js = assembled_dashboard_js()
    for marker in ("claim-node", "claim-confirm", "renderClaimNodePanel",
                   "data-claim-node-input", "data-claim-code-input"):
        assert marker in dashboard_js

    settings_html = (ROOT / "app" / "public" / "dashboard" /
                     "settings" / "index.html").read_text(encoding="utf-8")
    # The claim panel lives in the settings view and is present in the prebuilt
    # settings page document.
    assert "data-claim-node-input" in settings_html
    composed = assembled_dashboard()
    assert "data-claim-node-input" in composed
    assert "data-claim-code-confirm" in composed
