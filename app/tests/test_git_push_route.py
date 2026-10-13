#!/usr/bin/env python3
"""HTTPS git push route (mobile-node-contract.md section 7, Worker side).

A mobile_node (or desktop/org-member) device pushes over smart HTTP through
the app origin: ``GET /{owner}/{repo}/info/refs?service=git-receive-pack``
and ``POST /{owner}/{repo}/git-receive-pack``. The signed device headers are
the only authorization (no anonymous path), and an authorized push is
forwarded through the existing masked-proxy capability whose signed target is
``/v1/repositories/{owner}/{repo}/git-receive-pack``.
"""

import ast
import asyncio
import base64
import hashlib
import json
import re
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG_ROUTES = ROOT / "src" / "catalog_routes.py"
# The Default class (and with it _route/_git_push) lives in app_runtime.py,
# loaded lazily by entry.py; the push helpers themselves stay in entry.py, so
# the contracts below span both sources.
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + APP_RUNTIME.read_text(encoding="utf-8")
)
TREE = ast.parse(TEXT, filename=str(ENTRY))

SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import edge_routing as https_routing  # noqa: E402


NOW = 1_783_000_000_000
SKEW_MS = 5 * 60 * 1000
BODY_CAP = 8 * 1024 * 1024
OWNER_PRIMARY_KEY = "P" * 43
MOBILE_DEVICE_KEY = "M" * 43
CLIENT_DEVICE_KEY = "C" * 43
MEMBER_PRIMARY_KEY = "O" * 43
MOBILE_CAPS = "browse,comment,submit_issue,submit_pr,publish_repo,owner_sign"
CLIENT_CAPS = "browse,comment,submit_issue,submit_pr"


def _nodes(names):
    selected = [
        node for node in TREE.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in names
    ]
    missing = set(names) - {node.name for node in selected}
    assert not missing, missing
    return selected


def _load(names, extra_globals):
    module = ast.fix_missing_locations(
        ast.Module(body=_nodes(names), type_ignores=[]))
    namespace = {
        "re": re,
        "hashlib": hashlib,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        **extra_globals,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Date:
    @staticmethod
    def now():
        return NOW


def _valid_node_name(value):
    return bool(re.fullmatch(
        r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", str(value or "")))


def _safe_segment(value):
    value = str(value or "")
    return value if re.fullmatch(r"[A-Za-z0-9._-]{1,100}", value) else ""


def _valid_node_pubkey(value):
    return bool(re.fullmatch(r"[A-Za-z0-9_-]{43}", str(value or "")))


def _clean_string(value, maximum=240):
    return str(value or "")[:maximum]


def _json_response(data, status=200, extra_headers=None, **_kwargs):
    return {"status": status, "data": data, "headers": extra_headers or {}}


def _sig_for(pubkey, message):
    return "sig:" + pubkey + ":" + hashlib.sha256(message).hexdigest()


async def _stub_ed25519_verify(pubkey, sig, message):
    return sig == _sig_for(pubkey, bytes(message))


BASE_GLOBALS = {
    "GIT_PUSH_SIGNATURE_PREFIX": "forkmesh-git-push-v1",
    "valid_node_name": _valid_node_name,
    "safe_segment": _safe_segment,
    "valid_node_pubkey": _valid_node_pubkey,
    "clean_string": _clean_string,
    "Date": _Date,
    "LOGIN_MAX_SKEW_MS": SKEW_MS,
    "MAX_NODE_NAME": 64,
}


# ---------------------------------------------------------------------------
# Canonical signature message
# ---------------------------------------------------------------------------


def _message_builder():
    return _load({"_git_push_signature_message"}, dict(BASE_GLOBALS))[
        "_git_push_signature_message"]


def test_signature_message_binds_owner_repo_method_ts_and_body_digest():
    build = _message_builder()
    empty = hashlib.sha256(b"").hexdigest()
    assert build("alice", "app", "GET", "1783000000000", empty) == (
        "forkmesh-git-push-v1\nalice\napp\nGET\n1783000000000\n" + empty
    ).encode("utf-8")
    body = hashlib.sha256(b"pack-bytes").hexdigest()
    assert build("Alice", "app", "post", "17", body).decode().startswith(
        "forkmesh-git-push-v1\nalice\napp\nPOST\n17\n")


@pytest.mark.parametrize("owner,repo,method,ts,digest", [
    ("", "app", "GET", "1", "a" * 64),
    ("alice", "", "GET", "1", "a" * 64),
    ("alice", "app", "PUT", "1", "a" * 64),
    ("alice", "app", "GET", "not-a-ts", "a" * 64),
    ("alice", "app", "GET", "1" * 17, "a" * 64),
    ("alice", "app", "GET", "1", "zz"),
    ("alice", "../trick", "GET", "1", "a" * 64),
])
def test_malformed_signature_inputs_produce_no_message(
        owner, repo, method, ts, digest):
    assert _message_builder()(owner, repo, method, ts, digest) == b""


# ---------------------------------------------------------------------------
# Authorization
# ---------------------------------------------------------------------------


def _authorize_harness(
    *,
    owner_primary=OWNER_PRIMARY_KEY,
    devices=None,
    org_write_accounts=(),
    pubkey_accounts=None,
):
    """Build _git_push_authorize with account/device/org stubs."""
    devices = devices or {}
    pubkey_accounts = pubkey_accounts or {}
    audits = []

    async def _owner_signing_pubkeys(_env, owner):
        keys = []
        if owner == "alice" and owner_primary:
            keys.append(owner_primary)
        for pubkey, device in devices.items():
            if (
                device.get("enabled")
                and not device.get("revoked_at")
                and "owner_sign" in device.get("capabilities", "").split(",")
            ):
                keys.append(pubkey)
        return keys

    async def _owner_pubkey(_env, owner):
        return owner_primary if owner == "alice" else ""

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def _account_device_for_pubkey(_env, account_bi, pubkey):
        assert account_bi == "bi:alice"
        return devices.get(pubkey)

    async def _org_write_allowed(_env, owner, repo, account):
        return (owner, repo, account) in org_write_accounts

    async def _account_row_by_pubkey(_env, pubkey):
        record = pubkey_accounts.get(pubkey)
        return (("node-bi", record) if record else (None, None))

    async def d1_first(_env, sql, *args):
        return None

    async def decrypt_row(_env, _data):
        return None

    async def _audit_sensitive_action(
            _env, actor, action, target_type, target, outcome, details):
        audits.append({
            "actor": actor, "action": action, "target": target,
            "outcome": outcome, "details": details,
        })

    namespace = _load(
        {"_git_push_signature_message", "_git_push_account_for_pubkey",
         "_git_push_authorize"},
        {
            **BASE_GLOBALS,
            "ed25519_verify": _stub_ed25519_verify,
            "_owner_signing_pubkeys": _owner_signing_pubkeys,
            "_owner_pubkey": _owner_pubkey,
            "blind_index": blind_index,
            "_account_device_for_pubkey": _account_device_for_pubkey,
            "_org_write_allowed": _org_write_allowed,
            "_account_row_by_pubkey": _account_row_by_pubkey,
            "d1_first": d1_first,
            "decrypt_row": decrypt_row,
            "_audit_sensitive_action": _audit_sensitive_action,
        },
    )
    return namespace, audits


def _signed(pubkey, *, owner="alice", repo="app", method="POST",
            body=b"pack", ts=str(NOW)):
    message = (
        "forkmesh-git-push-v1\n%s\n%s\n%s\n%s\n%s"
        % (owner, repo, method, ts, hashlib.sha256(body).hexdigest())
    ).encode("utf-8")
    return {
        "owner": owner, "repo": repo, "method": method, "body": body,
        "pubkey": pubkey, "ts": ts, "sig": _sig_for(pubkey, message),
    }


def _run_authorize(namespace, request):
    # The proxy hands over the digest it holds or the one the device stated;
    # authorization never needs the pack itself.
    return asyncio.run(namespace["_git_push_authorize"](
        object(), request["owner"], request["repo"], request["method"],
        hashlib.sha256(request["body"]).hexdigest(), request["pubkey"],
        request["ts"], request["sig"]))


def _mobile_device(pubkey=MOBILE_DEVICE_KEY, *, enabled=1, revoked_at=0,
                   capabilities=MOBILE_CAPS):
    return {
        "device_bi": "device-bi", "pubkey": pubkey, "kind": "mobile_node",
        "capabilities": capabilities, "enabled": enabled,
        "revoked_at": revoked_at,
    }


def test_enabled_mobile_device_with_publish_repo_is_authorized():
    namespace, audits = _authorize_harness(
        devices={MOBILE_DEVICE_KEY: _mobile_device()})
    assert _run_authorize(namespace, _signed(MOBILE_DEVICE_KEY)) == "alice"
    assert audits[-1]["outcome"] == "success"
    assert audits[-1]["action"] == "git.push_authorize"
    assert audits[-1]["target"] == "alice/app"
    assert audits[-1]["details"]["operation"] == "git-receive-pack"


def test_owner_primary_key_is_authorized_without_a_device_row():
    namespace, audits = _authorize_harness()
    assert _run_authorize(namespace, _signed(OWNER_PRIMARY_KEY)) == "alice"
    assert audits[-1]["outcome"] == "success"


def test_revoked_device_key_is_denied():
    namespace, audits = _authorize_harness(
        devices={MOBILE_DEVICE_KEY: _mobile_device(revoked_at=NOW)})
    assert _run_authorize(namespace, _signed(MOBILE_DEVICE_KEY)) == ""
    assert audits[-1]["outcome"] == "denied"


def test_owner_sign_device_without_publish_repo_is_denied():
    namespace, audits = _authorize_harness(devices={
        MOBILE_DEVICE_KEY: _mobile_device(
            capabilities="browse,owner_sign"),
    })
    assert _run_authorize(namespace, _signed(MOBILE_DEVICE_KEY)) == ""
    assert audits[-1]["outcome"] == "denied"


def test_web_client_caps_key_is_denied():
    # A CLIENT-capability device never enters _owner_signing_pubkeys and has
    # no org permission, so its (validly signed) request is refused.
    namespace, audits = _authorize_harness(
        devices={CLIENT_DEVICE_KEY: _mobile_device(
            pubkey=CLIENT_DEVICE_KEY, capabilities=CLIENT_CAPS)})
    assert _run_authorize(namespace, _signed(CLIENT_DEVICE_KEY)) == ""
    assert audits[-1]["outcome"] == "denied"


def test_org_write_member_is_authorized_with_their_own_key():
    namespace, audits = _authorize_harness(
        org_write_accounts={("alice", "app", "bob")},
        pubkey_accounts={MEMBER_PRIMARY_KEY: {"name": "bob"}},
    )
    assert _run_authorize(namespace, _signed(MEMBER_PRIMARY_KEY)) == "bob"
    assert audits[-1]["outcome"] == "success"
    assert audits[-1]["actor"] == "bob"


def test_member_without_org_write_is_denied():
    namespace, audits = _authorize_harness(
        pubkey_accounts={MEMBER_PRIMARY_KEY: {"name": "bob"}})
    assert _run_authorize(namespace, _signed(MEMBER_PRIMARY_KEY)) == ""
    assert audits[-1]["outcome"] == "denied"


def test_stale_timestamp_is_denied():
    namespace, audits = _authorize_harness()
    stale = str(NOW - SKEW_MS - 1)
    assert _run_authorize(
        namespace, _signed(OWNER_PRIMARY_KEY, ts=stale)) == ""
    assert audits[-1]["outcome"] == "denied"


def test_bad_signature_is_denied():
    namespace, audits = _authorize_harness()
    request = _signed(OWNER_PRIMARY_KEY)
    request["sig"] = "not-the-signature"
    assert _run_authorize(namespace, request) == ""
    assert audits[-1]["outcome"] == "denied"


def test_signature_over_wrong_method_or_body_is_denied():
    namespace, _audits = _authorize_harness()
    request = _signed(OWNER_PRIMARY_KEY, method="GET", body=b"")
    request["method"] = "POST"  # replayed advertisement sig against a push
    assert _run_authorize(namespace, request) == ""
    tampered = _signed(OWNER_PRIMARY_KEY)
    tampered["body"] = b"different-pack"
    assert _run_authorize(namespace, tampered) == ""


# ---------------------------------------------------------------------------
# Proxy: method rules, bounds, endpoint selection, capability construction
# ---------------------------------------------------------------------------


class _Headers(dict):
    def get(self, key, default=None):
        for name, value in self.items():
            if name.lower() == str(key).lower():
                return value
        return default


class _JsNull:
    """What Workers hands Python for a bodiless request's ``body``."""


class _Stream:
    """Stands in for the inbound JS ReadableStream: opaque and single-use."""

    def __init__(self, label="stream"):
        self.label = label


class _Request:
    def __init__(self, url, method="POST", headers=None, body=b"",
                 stream=None):
        self.url = url
        self.method = method
        self.headers = _Headers(headers or {})
        self._body = body
        self.body_reads = 0
        # A real request exposes its body stream whether or not the proxy
        # ever reads it as bytes; ``None`` models a request without a body.
        self.body = _JsNull() if stream is None and not body else (
            stream if stream is not None else _Stream())

    async def bytes(self):
        self.body_reads += 1
        return self._body


class _Upstream:
    def __init__(self, status=200, headers=None, body="UPSTREAM-BODY"):
        self.status = status
        self.headers = _Headers(headers or {
            "content-type": "application/x-git-receive-pack-result",
            "x-internal-endpoint": "leak.example",
        })
        self.body = body


def _endpoint(node="write-node"):
    return {
        "node": node,
        "baseUrl": "https://%s.example.test" % node,
        "publicKey": "N" * 43,
        "signature": "S",
        "issuedAt": NOW,
        "checkedAt": NOW,
        "latencyMs": 5,
        "healthy": True,
        "integrity": "ok",
        "region": "",
        "abuseBlocked": False,
        "refsSha256": "a" * 64,
    }


def _proxy_harness(
    *,
    candidates=None,
    authorized="alice",
    router_ready=True,
    upstreams=None,
    basic_account="",
    core_nodes=(),
    core_owners=(),
    alias_owner="",
    tag_moves=None,
):
    signed_messages = []
    recorded = []
    audits = []
    fetches = []
    fetch_timeouts = []
    pinned = []
    pushers = []
    admitted = []
    candidate_pubkeys = []
    authorized_digests = []
    upstreams = list(upstreams or [])

    async def _git_push_authorize(
            _env, owner, repo, method, body_sha256, *_creds, alias_owner=""):
        authorized_digests.append(body_sha256)
        return authorized

    async def _https_mirror_push_candidates(
            _env, _request, _owner, _repo, core_allowed=False, alias=None):
        candidate_pubkeys.append(core_allowed)
        return [dict(endpoint) for endpoint in (candidates or [])]

    async def _core_push_permitted(_env, _owner, _repo, account):
        return account in core_owners

    async def core_push_pending_tag_moves(_env, _owner, _repo, _account):
        return dict(tag_moves or {})

    async def record_accepted_push(_env, owner, repo, endpoint, account,
                                   report, **kwargs):
        recorded.append((owner, repo, endpoint["node"], account, report,
                         kwargs))
        return len(report["updates"])

    async def _audit_sensitive_action(_env, actor, action, *_args):
        audits.append((actor, action) + tuple(_args))

    async def _admit_core_push_state(_env, owner, repo, endpoint, alias=""):
        admitted.append((owner, repo, endpoint["node"]))
        return "e" * 64

    async def ed25519_sign(_pub, _seed, message):
        signed_messages.append(bytes(message).decode("utf-8"))
        return "router-signature"

    async def js_fetch_until_response(target, init, timeout):
        fetches.append((target, init))
        fetch_timeouts.append(timeout)
        if not upstreams:
            raise RuntimeError("unreachable endpoint")
        upstream = upstreams.pop(0)
        if upstream is None:
            raise RuntimeError("unreachable endpoint")
        return upstream

    class _JsResponse:
        @staticmethod
        def new(body, init):
            return {"body": body, "init": init}

    class _Uint8Array:
        @staticmethod
        def new(value):
            return value

    # A stock git cannot sign per-request headers, so an account's browser
    # session may issue a Basic credential instead (issue #676, hosted_git).
    def has_hosted_push_credential(header):
        return str(header or "").lower().startswith("basic ")

    async def verify_hosted_push_credential(
            _env, _owner, _repo, header, _method=""):
        return basic_account if has_hosted_push_credential(header) else ""

    async def record_hosted_push_state(_env, owner, repo, endpoint,
                                       pusher=""):
        pinned.append((owner, repo, endpoint["node"]))
        pushers.append(pusher)
        return "d" * 64

    namespace = _load(
        {"_https_mirror_push_proxy", "_https_mirror_push_send",
         "_git_push_basic_challenge",
         "_https_mirror_request_body_stream", "_ref_updates_from"}, {
        **BASE_GLOBALS,
        "json": __import__("json"),
        "_core_push_permitted": _core_push_permitted,
        "core_push_pending_tag_moves": core_push_pending_tag_moves,
        "record_accepted_push": record_accepted_push,
        "_audit_sensitive_action": _audit_sensitive_action,
        "js_nullish": lambda value: (
            value is None or type(value).__name__ == "_JsNull"),
        "HTTPS_MIRROR_PUSH_RESPONSE_TIMEOUT_SECONDS": 600,
        "method_name": lambda request: request.method,
        "json_response": _json_response,
        "Response": lambda body, status=200, headers=None: {
            "status": status, "body": body},
        "HTTPS_MIRROR_BODY_MAX_BYTES": BODY_CAP,
        "_git_push_authorize": _git_push_authorize,
        "_https_mirror_router_public_key": (
            lambda _env: "R" * 43 if router_ready else ""),
        "_https_mirror_router_seed": (
            lambda _env: "s" * 43 if router_ready else ""),
        "EXPECTED_DEGRADED_HEADERS": {"X-Forkmesh-Expected-Degraded": "1"},
        "_https_mirror_push_candidates": _https_mirror_push_candidates,
        "_core_push_nodes": lambda _env: set(core_nodes),
        "_admit_core_push_state": _admit_core_push_state,
        "_git_push_alias_owner": lambda _env, _request, _owner, _repo: (
            asyncio.sleep(0, result=alias_owner)),
        "https_routing": https_routing,
        "_b64url_encode": lambda raw: base64.urlsafe_b64encode(
            raw).decode().rstrip("="),
        "_random_bytes": lambda count: b"\x01" * count,
        "Date": _Date,
        "ed25519_sign": ed25519_sign,
        "Uint8Array": _Uint8Array,
        "_to_js": lambda value: value,
        "to_js": lambda value: value,
        "js_fetch_until_response": js_fetch_until_response,
        "HTTPS_MIRROR_GIT_INITIAL_RESPONSE_TIMEOUT_SECONDS": 20,
        "HTTPS_MIRROR_RETRY_STATUSES": frozenset(
            {401, 403, 404, 408, 425, 429, 500, 502, 503, 504}),
        "JsResponse": _JsResponse,
        "has_hosted_push_credential": has_hosted_push_credential,
        "verify_hosted_push_credential": verify_hosted_push_credential,
        "record_hosted_push_state": record_hosted_push_state,
    })
    namespace["_pinned_states"] = pinned
    namespace["_pinned_pushers"] = pushers
    namespace["_admitted_states"] = admitted
    namespace["_candidate_pubkeys"] = candidate_pubkeys
    namespace["_recorded_pushes"] = recorded
    namespace["_audits"] = audits
    namespace["_fetch_timeouts"] = fetch_timeouts
    namespace["_authorized_digests"] = authorized_digests
    return namespace, signed_messages, fetches


def _signed_headers(pubkey=OWNER_PRIMARY_KEY):
    return {
        "x-forkmesh-pubkey": pubkey,
        "x-forkmesh-ts": str(NOW),
        "x-forkmesh-sig": "any",
    }


def _run_proxy(namespace, request, *, is_info):
    return asyncio.run(namespace["_https_mirror_push_proxy"](
        object(), request, "alice", "app", is_info))


def test_method_rules_info_refs_get_only_and_receive_pack_post_only():
    namespace, _messages, _fetches = _proxy_harness()
    info_post = _Request(
        "https://forkmesh.test/alice/app/info/refs?service=git-receive-pack",
        method="POST", headers=_signed_headers())
    assert _run_proxy(namespace, info_post, is_info=True)["status"] == 405
    pack_get = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        method="GET", headers=_signed_headers())
    assert _run_proxy(namespace, pack_get, is_info=False)["status"] == 405


def test_unsigned_probe_is_401_without_reading_the_body():
    namespace, _messages, _fetches = _proxy_harness()
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack", body=b"pack")
    response = _run_proxy(namespace, request, is_info=False)
    assert response["status"] == 401
    assert response["data"]["error"] == "push_unauthorized"
    assert request.body_reads == 0
    # The refusal names the Basic scheme, which is the only way a stock git
    # learns to send the credential the website issued (issue #676).
    assert response["headers"]["WWW-Authenticate"] == 'Basic realm="forkmesh"'


def test_accepted_push_records_the_hosted_repository_refs_pin():
    """A hosted repository has no node of its own to attest its refs.

    A public read is routed only against an authenticated refs attestation,
    so the accepted push is where the hosting node's freshly signed digest is
    pinned (issue #676). The advertisement is not a push and pins nothing.
    """
    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint()], upstreams=[_Upstream()])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    assert _run_proxy(namespace, request, is_info=False)["init"]["status"] \
        == 200
    assert namespace["_pinned_states"] == [("alice", "app", "write-node")]
    # The authorized pushing account travels with the pin, so the code-push
    # ping can leave an organization member's own push off their phone.
    assert namespace["_pinned_pushers"] == [namespace["_pinned_pushers"][0]]
    assert namespace["_pinned_pushers"][0]

    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint()], upstreams=[_Upstream()])
    advertisement = _Request(
        "https://forkmesh.test/alice/app/info/refs?service=git-receive-pack",
        method="GET", headers=_signed_headers())
    _run_proxy(namespace, advertisement, is_info=True)
    assert namespace["_pinned_states"] == []


def test_basic_credential_authorizes_a_push_from_a_stock_git():
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint()],
        upstreams=[_Upstream()],
        basic_account="alice",
    )
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({"authorization": "Basic YWxpY2U6Zmlyc3QtcHVzaA=="}),
        body=b"pack")
    response = _run_proxy(namespace, request, is_info=False)
    assert response["init"]["status"] == 200
    assert fetches, "an authorized Basic push must reach the write endpoint"

    # A Basic header the credential check refuses is still a 401 challenge.
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint()], basic_account="")
    refused = _run_proxy(namespace, _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({"authorization": "Basic bm9wZTpub3BlCg=="}),
        body=b"pack"), is_info=False)
    assert refused["status"] == 401
    assert refused["headers"]["WWW-Authenticate"] == 'Basic realm="forkmesh"'
    assert not fetches


def test_unauthorized_signature_is_401():
    namespace, _messages, _fetches = _proxy_harness(authorized="")
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    response = _run_proxy(namespace, request, is_info=False)
    assert response["status"] == 401
    assert response["data"]["error"] == "push_unauthorized"


def test_oversized_push_is_413_push_too_large():
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint()])
    announced = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({
            **_signed_headers(), "content-length": str(BODY_CAP + 1)}),
        body=b"pack")
    response = _run_proxy(namespace, announced, is_info=False)
    assert response["status"] == 413
    assert response["data"]["error"] == "push_too_large"
    assert announced.body_reads == 0

    oversized = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"x" * (BODY_CAP + 1))
    response = _run_proxy(namespace, oversized, is_info=False)
    assert response["status"] == 413
    assert response["data"]["error"] == "push_too_large"
    assert fetches == []


def test_no_write_endpoint_is_503_push_unavailable():
    namespace, _messages, _fetches = _proxy_harness(candidates=[])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    response = _run_proxy(namespace, request, is_info=False)
    assert response["status"] == 503
    assert response["data"]["error"] == "push_unavailable"
    assert response["headers"] == {"X-Forkmesh-Expected-Degraded": "1"}


def test_missing_router_identity_is_503_push_unavailable():
    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint()], router_ready=False)
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    assert _run_proxy(namespace, request, is_info=False)["status"] == 503


def test_post_capability_binds_receive_pack_masked_target():
    body = b"receive-pack-request-bytes"
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({
            **_signed_headers(), "content-encoding": "gzip"}),
        body=body)
    response = _run_proxy(namespace, request, is_info=False)

    assert len(messages) == 1
    lines = messages[0].split("\n")
    assert lines[0] == "forkmesh-masked-proxy-v1"
    assert lines[1] == "write-node"
    assert lines[2] == "POST"
    assert lines[3] == "/v1/repositories/alice/app/git-receive-pack"
    assert lines[4] == hashlib.sha256(body).hexdigest()
    assert re.fullmatch(r"[A-Za-z0-9_-]{12,80}", lines[5])
    assert lines[6] == str(NOW)

    target, init = fetches[0]
    assert target == (
        "https://write-node.example.test"
        "/v1/repositories/alice/app/git-receive-pack")
    assert init["method"] == "POST"
    assert init["body"] == body
    assert init["headers"]["Content-Type"] == (
        "application/x-git-receive-pack-request")
    assert init["headers"]["Content-Encoding"] == "gzip"
    assert init["headers"]["X-ForkMesh-Signature"] == "router-signature"
    assert init["headers"]["X-ForkMesh-Body-Sha256"] == (
        hashlib.sha256(body).hexdigest())

    # The endpoint identity never leaks beyond the public node name.
    assert response["body"] == "UPSTREAM-BODY"
    assert response["init"]["headers"]["X-ForkMesh-Served-By"] == "write-node"
    assert "x-internal-endpoint" not in {
        key.lower() for key in response["init"]["headers"]}


def test_advertisement_capability_binds_get_and_service_query():
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")],
        upstreams=[_Upstream(headers={
            "content-type": "application/x-git-receive-pack-advertisement",
        })])
    request = _Request(
        "https://forkmesh.test/alice/app/info/refs?service=git-receive-pack",
        method="GET", headers=_signed_headers())
    response = _run_proxy(namespace, request, is_info=True)
    lines = messages[0].split("\n")
    assert lines[2] == "GET"
    assert lines[3] == (
        "/v1/repositories/alice/app/git-receive-pack"
        "?service=git-receive-pack")
    assert lines[4] == hashlib.sha256(b"").hexdigest()
    target, init = fetches[0]
    assert target.endswith("/git-receive-pack?service=git-receive-pack")
    assert "body" not in init
    assert response["init"]["headers"]["content-type"] == (
        "application/x-git-receive-pack-advertisement")


def test_failed_endpoint_fails_over_and_exhaustion_is_push_unavailable():
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("mirror-a"), _endpoint("mirror-b")],
        upstreams=[_Upstream(status=503), _Upstream()])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    response = _run_proxy(namespace, request, is_info=False)
    assert [fetch[0].split("//")[1].split(".")[0] for fetch in fetches] == [
        "mirror-a", "mirror-b"]
    assert response["init"]["headers"]["X-ForkMesh-Served-By"] == "mirror-b"

    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint("mirror-a")], upstreams=[None])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    exhausted = _run_proxy(namespace, request, is_info=False)
    assert exhausted["status"] == 503
    assert exhausted["data"]["error"] == "push_unavailable"


# ---------------------------------------------------------------------------
# Streamed packs: repositories larger than the Worker can hold
# ---------------------------------------------------------------------------


BASIC_HEADER = {"authorization": "Basic YWxpY2U6Zmlyc3QtcHVzaA=="}


def test_stock_git_pack_of_unannounced_length_streams_through_unread():
    """A stock git sends any push over http.postBuffer chunked, no length.

    The Worker must not buffer such a pack (a large repository does not fit
    the isolate) and cannot sign a digest of bytes it never holds, so the
    capability is bound to the fixed streamed-body digest instead and the
    inbound stream itself becomes the upstream body.
    """
    stream = _Stream("pack")
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()],
        basic_account="alice")
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers(BASIC_HEADER), body=b"x" * 1024, stream=stream)
    response = _run_proxy(namespace, request, is_info=False)
    assert response["init"]["status"] == 200
    assert request.body_reads == 0
    lines = messages[0].split("\n")
    assert lines[4] == https_routing.STREAMED_REQUEST_BODY_SHA256
    _target, init = fetches[0]
    assert init["body"] is stream
    assert init["headers"]["X-ForkMesh-Body-Sha256"] == (
        https_routing.STREAMED_REQUEST_BODY_SHA256)
    # Upload plus the gateway's receive-pack can take minutes; the
    # 20-second header window is for reads.
    assert namespace["_fetch_timeouts"] == [600]
    assert namespace["_pinned_states"] == [("alice", "app", "write-node")]


def test_stock_git_pack_announced_above_the_buffer_cap_streams_too():
    stream = _Stream("big")
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()],
        basic_account="alice")
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({
            **BASIC_HEADER, "content-length": str(50 * BODY_CAP)}),
        body=b"pack", stream=stream)
    response = _run_proxy(namespace, request, is_info=False)
    assert response["init"]["status"] == 200
    assert request.body_reads == 0
    assert fetches[0][1]["body"] is stream
    assert messages[0].split("\n")[4] == (
        https_routing.STREAMED_REQUEST_BODY_SHA256)


def test_small_announced_stock_git_pack_stays_digest_bound():
    """A pack the Worker can hold keeps the exact body binding."""
    body = b"small-pack"
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()],
        basic_account="alice")
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({**BASIC_HEADER, "content-length": str(len(body))}),
        body=body)
    assert _run_proxy(namespace, request, is_info=False)["init"]["status"] \
        == 200
    assert request.body_reads == 1
    assert messages[0].split("\n")[4] == hashlib.sha256(body).hexdigest()
    assert fetches[0][1]["body"] == body


def test_signing_device_may_state_its_digest_and_stream_the_pack():
    """Mobile/desktop signers cover the pack digest; stating it lets the
    Worker verify the signature without the pack, and the gateway checks the
    streamed bytes against that same digest before running git."""
    digest = hashlib.sha256(b"device-pack").hexdigest()
    stream = _Stream("device")
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({
            **_signed_headers(), "x-forkmesh-body-sha256": digest.upper()}),
        body=b"device-pack", stream=stream)
    response = _run_proxy(namespace, request, is_info=False)
    assert response["init"]["status"] == 200
    assert request.body_reads == 0
    assert namespace["_authorized_digests"] == [digest]
    assert messages[0].split("\n")[4] == digest
    assert fetches[0][1]["body"] is stream
    assert fetches[0][1]["headers"]["X-ForkMesh-Body-Sha256"] == digest

    # A stated digest must be a digest; anything else is refused unread.
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()])
    malformed = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({
            **_signed_headers(), "x-forkmesh-body-sha256": "not-a-digest"}),
        body=b"device-pack")
    assert _run_proxy(namespace, malformed, is_info=False)["status"] == 400
    assert malformed.body_reads == 0
    assert fetches == []


def test_signing_device_without_a_stated_digest_is_still_buffered():
    """Without a stated digest the Worker must hold the pack to verify the
    signature, so that push keeps the buffer cap (and its 413)."""
    body = b"unannounced-device-pack"
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("write-node")], upstreams=[_Upstream()])
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=body, stream=_Stream())
    assert _run_proxy(namespace, request, is_info=False)["init"]["status"] \
        == 200
    assert request.body_reads == 1
    assert namespace["_authorized_digests"] == [
        hashlib.sha256(body).hexdigest()]
    assert fetches[0][1]["body"] == body


def test_streamed_pack_is_offered_to_one_endpoint_only():
    """The stream is consumed by its first upstream attempt, so a failure
    cannot be retried on another mirror; git reports it and the user
    retries, instead of the Worker replaying a spent stream."""
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint("mirror-a"), _endpoint("mirror-b")],
        upstreams=[_Upstream(status=503), _Upstream()],
        basic_account="alice")
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers(BASIC_HEADER), body=b"pack", stream=_Stream())
    response = _run_proxy(namespace, request, is_info=False)
    assert response["status"] == 503
    assert response["data"]["error"] == "push_unavailable"
    assert len(fetches) == 1

    # A buffered pack still fails over exactly as before.
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint("mirror-a"), _endpoint("mirror-b")],
        upstreams=[_Upstream(status=503), _Upstream()],
        basic_account="alice")
    buffered = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_Headers({**BASIC_HEADER, "content-length": "4"}),
        body=b"pack")
    assert _run_proxy(namespace, buffered, is_info=False)["init"]["status"] \
        == 200
    assert len(fetches) == 2


def test_advertisement_keeps_the_short_read_timeout():
    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint("write-node")],
        upstreams=[_Upstream(headers={
            "content-type": "application/x-git-receive-pack-advertisement",
        })])
    request = _Request(
        "https://forkmesh.test/alice/app/info/refs?service=git-receive-pack",
        method="GET", headers=_signed_headers())
    _run_proxy(namespace, request, is_info=True)
    assert namespace["_fetch_timeouts"] == [20]


# ---------------------------------------------------------------------------
# Endpoint selection for writes
# ---------------------------------------------------------------------------


def _candidates_harness(*, context, routed, proofs, write_row, alias=""):
    proof_owners = []

    async def _git_push_alias_owner(_env, _request, _owner, _repo):
        return alias

    async def blind_index(_env, value):
        return "bi:" + value

    async def _https_mirror_public_context(_env, _owner, _repo):
        return context

    async def _https_mirror_request_candidates(
            _env, _context, _region, operation=""):
        assert operation == "git-receive-pack"
        return [dict(endpoint) for endpoint in routed]

    async def _https_mirror_repository_proof(
            _env, endpoint, _context, operation):
        assert operation == "git-receive-pack"
        proof_owners.append((
            _context.get("routeOwner") or _context["owner"],
            _context.get("repoBi")))
        return proofs.get(endpoint["node"], False)

    async def d1_first(_env, sql, *args):
        assert "FROM mirror_https_endpoints WHERE node_name=?" in sql
        return write_row if args == ("hosted-write",) else None

    namespace = _load(
        {"_https_mirror_push_candidates", "_hosted_git_write_endpoint",
         "_https_mirror_endpoint_projection", "_core_push_nodes"},
        {
            **BASE_GLOBALS,
            "_https_mirror_public_context": _https_mirror_public_context,
            "_https_mirror_request_candidates": (
                _https_mirror_request_candidates),
            "_https_mirror_repository_proof": _https_mirror_repository_proof,
            "world_request_country": lambda _request: "",
            "d1_first": d1_first,
            "https_routing": https_routing,
            "_git_push_alias_owner": _git_push_alias_owner,
            "blind_index": blind_index,
        },
    )
    namespace["_proof_owners"] = proof_owners
    return namespace


def _write_row(node="hosted-write", base_url="https://write.example.test"):
    return {
        "node_name": node, "base_url": base_url, "public_key": "N" * 43,
        "registration_sig": "S", "issued_at": NOW, "checked_at": NOW,
        "latency_ms": 4, "region": "", "healthy": 1, "integrity": "ok",
        "abuse_blocked": 0, "forkmesh_refs_sha256": "a" * 64,
    }


def _run_candidates(namespace, env=None, core_allowed=False):
    env = env or type("Env", (), {"HOSTED_GIT_WRITE_NODE": "hosted-write"})()
    return asyncio.run(namespace["_https_mirror_push_candidates"](
        env, object(), "alice", "app", core_allowed=core_allowed))


def test_only_endpoints_proving_receive_pack_qualify_plus_write_fallback():
    namespace = _candidates_harness(
        context={"owner": "alice", "repo": "app"},
        routed=[_endpoint("mirror-a"), _endpoint("mirror-b")],
        proofs={"mirror-a": False, "mirror-b": True},
        write_row=_write_row(),
    )
    nodes = [endpoint["node"] for endpoint in _run_candidates(namespace)]
    assert nodes == ["mirror-b", "hosted-write"]


def test_first_push_of_unknown_repo_uses_only_the_write_node():
    namespace = _candidates_harness(
        context=None, routed=[], proofs={}, write_row=_write_row())
    nodes = [endpoint["node"] for endpoint in _run_candidates(namespace)]
    assert nodes == ["hosted-write"]


def test_unset_or_unresolvable_write_node_yields_no_fallback():
    namespace = _candidates_harness(
        context=None, routed=[], proofs={}, write_row=_write_row())
    unset = type("Env", (), {})()
    assert _run_candidates(namespace, unset) == []

    unregistered = _candidates_harness(
        context=None, routed=[], proofs={}, write_row=None)
    assert _run_candidates(unregistered) == []

    bad_url = _candidates_harness(
        context=None, routed=[], proofs={},
        write_row=_write_row(base_url="http://insecure.example"))
    assert _run_candidates(bad_url) == []


def test_write_node_already_proving_receive_pack_is_not_duplicated():
    namespace = _candidates_harness(
        context={"owner": "alice", "repo": "app"},
        routed=[_endpoint("hosted-write")],
        proofs={"hosted-write": True},
        write_row=_write_row(),
    )
    nodes = [endpoint["node"] for endpoint in _run_candidates(namespace)]
    assert nodes == ["hosted-write"]


# ---------------------------------------------------------------------------
# Route wiring and masked-operation allowlist
# ---------------------------------------------------------------------------


def test_route_wires_receive_pack_service_and_post_to_git_push():
    route = next(
        ast.get_source_segment(TEXT, child)
        for node in TREE.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
        for child in node.body
        if isinstance(child, ast.AsyncFunctionDef) and child.name == "_route"
    )
    assert 'service == "git-receive-pack"' in route
    assert route.count("self._git_push(") == 2
    receive_block = route.split("GIT_RECEIVE_RE.match", 1)[1]
    assert '!= "POST"' in receive_block.split("self._git_push", 1)[0]

    push = next(
        ast.get_source_segment(TEXT, child)
        for node in TREE.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
        for child in node.body
        if isinstance(child, ast.AsyncFunctionDef)
        and child.name == "_git_push"
    )
    assert "_https_mirror_push_proxy" in push


def test_masked_target_url_allows_receive_pack_operation():
    assert "git-receive-pack" in https_routing.PUBLIC_OPERATIONS
    assert https_routing.masked_target_url(
        "https://mirror.example.test", "alice", "app", "git-receive-pack",
        {"service": "git-receive-pack"},
    ) == (
        "https://mirror.example.test/v1/repositories/alice/app/"
        "git-receive-pack?service=git-receive-pack"
    )


# ---------------------------------------------------------------------------
# Catalog account_required relaxation
# ---------------------------------------------------------------------------


def _signing_keys(owner_primary, device_rows):
    async def _account_row(_env, name):
        return "bi:" + str(name), {"name": name, "pubkey": owner_primary}

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def d1_all(_env, sql, *_args):
        assert "FROM account_devices WHERE account_bi=?" in sql
        return list(device_rows)

    namespace = _load(
        {"_owner_signing_pubkeys", "_account_signing_pubkeys",
         "_account_devices_list"},
        {
            **BASE_GLOBALS,
            "_account_row": _account_row,
            "blind_index": blind_index,
            "d1_all": d1_all,
        },
    )
    return asyncio.run(namespace["_owner_signing_pubkeys"](
        object(), "alice"))


def test_phone_only_account_qualifies_for_catalog_publication():
    device = {
        "device_bi": "device-bi", "pubkey": MOBILE_DEVICE_KEY,
        "kind": "mobile_node", "label": "phone", "capabilities": MOBILE_CAPS,
        "enabled": 1, "last_seen": 1, "created_at": 1, "revoked_at": 0,
    }
    assert _signing_keys("", [device]) == [MOBILE_DEVICE_KEY]
    assert _signing_keys("", []) == []
    revoked = dict(device, revoked_at=NOW)
    assert _signing_keys("", [revoked]) == []


def test_catalog_gate_requires_any_signing_key_not_the_primary_key():
    source = CATALOG_ROUTES.read_text(encoding="utf-8")
    gate = source.split("await _account_row(env, owner)", 1)[1]
    gate = gate.split("_catalog_publication_key", 1)[0]
    assert "if not publish_owner_rec:" in gate
    assert "if not await _owner_signing_pubkeys(env, owner):" in gate
    assert gate.count('{"error": "account_required"}') == 2
    assert "primary_owner_pub" not in source


def test_core_endpoints_take_a_push_only_from_a_permitted_account():
    namespace = _candidates_harness(
        context={"owner": "alice", "repo": "app"},
        routed=[_endpoint("core-a"), _endpoint("mirror-b")],
        proofs={"core-a": True, "mirror-b": True},
        write_row=None,
    )
    namespace["hosted_core_nodes"] = lambda _env: ["core-a", "core-b"]
    env = type("Env", (), {})()
    assert [e["node"] for e in _run_candidates(namespace, env)] == [
        "mirror-b"]
    assert [e["node"] for e in _run_candidates(
        namespace, env, core_allowed=True)] == ["core-a", "mirror-b"]


def _core_push(core_owners=("jett",), authorized="jett", upstream=None,
               tag_moves=None, headers=None):
    namespace, messages, fetches = _proxy_harness(
        candidates=[_endpoint("core-a")],
        upstreams=[upstream or _Upstream()],
        authorized=authorized, core_nodes=("alice", "core-a"),
        core_owners=core_owners, tag_moves=tag_moves)
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=headers or _signed_headers(), body=b"pack")
    return namespace, messages, fetches, _run_proxy(
        namespace, request, is_info=False)


def test_an_organization_owner_push_to_a_core_admits_and_records_it():
    updates = {"total": 2, "updates": [
        {"ref": "refs/heads/main", "old": "1" * 40, "new": "2" * 40,
         "commits": 3},
        {"ref": "refs/tags/v1", "old": "0" * 40, "new": "3" * 40}]}
    upstream = _Upstream(headers={
        "content-type": "application/x-git-receive-pack-result",
        "x-forkmesh-ref-updates": json.dumps(updates)})
    namespace, _messages, fetches, response = _core_push(upstream=upstream)
    assert response["init"]["status"] == 200
    assert namespace["_candidate_pubkeys"] == [True]
    assert namespace["_admitted_states"] == [("alice", "app", "core-a")]
    ((owner, repo, node, account, report, extra),) = namespace[
        "_recorded_pushes"]
    assert (owner, repo, node, account) == ("alice", "app", "core-a", "jett")
    assert report == {"total": 2, "updates": [
        dict(updates["updates"][0]), dict(updates["updates"][1], commits=0)]}
    assert extra["core"] is True and extra["state"] == "e" * 64
    assert extra["credential"] == "key"
    assert "tagMoves" not in fetches[0][0]


def test_a_non_owner_is_refused_before_any_core_is_reached():
    for account in ("maintainer", "member", "stranger"):
        namespace, _messages, fetches, response = _core_push(
            authorized=account)
        assert response["status"] == 403
        assert response["data"]["error"] == "push_forbidden"
        assert not fetches
        assert namespace["_admitted_states"] == []
        assert namespace["_recorded_pushes"] == []
        assert namespace["_audits"][-1][:2] == (account, "git.push_authorize")
        assert namespace["_audits"][-1][4] == "denied"


def test_an_anonymous_push_to_a_core_is_challenged():
    namespace, _messages, fetches, response = _core_push(
        headers={"content-type": "application/x-git-receive-pack-request"})
    assert response["status"] == 401
    assert not fetches


def test_pending_tag_moves_ride_the_signed_capability_target():
    namespace, messages, fetches, response = _core_push(
        tag_moves={"refs/tags/v1.2.4": "a" * 40, "refs/tags/v1": "b" * 40})
    assert response["init"]["status"] == 200
    target = fetches[0][0]
    query = parse_qs(urlparse(target).query)
    assert query == {"tagMoves": [
        "refs/tags/v1:%s refs/tags/v1.2.4:%s" % ("b" * 40, "a" * 40)]}
    signed_path = urlparse(target).path + "?" + urlparse(target).query
    assert signed_path in messages[0]
    assert namespace["_recorded_pushes"][0][5]["tag_moves"] == {
        "refs/tags/v1.2.4": "a" * 40, "refs/tags/v1": "b" * 40}


def test_push_elsewhere_admits_nothing_and_advertisements_record_nothing():
    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint("core-a")], upstreams=[_Upstream()],
        core_nodes=())
    request = _Request(
        "https://forkmesh.test/alice/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    _run_proxy(namespace, request, is_info=False)
    assert namespace["_admitted_states"] == []
    assert namespace["_recorded_pushes"][0][5]["core"] is False
    namespace, _messages, _fetches = _proxy_harness(
        candidates=[_endpoint("core-a")], upstreams=[_Upstream()],
        core_nodes=("alice", "core-a"), core_owners=("alice",))
    advertisement = _Request(
        "https://forkmesh.test/alice/app/info/refs?service=git-receive-pack",
        method="GET", headers=_signed_headers())
    _run_proxy(namespace, advertisement, is_info=True)
    assert namespace["_admitted_states"] == []
    assert namespace["_recorded_pushes"] == []


def test_ref_updates_header_is_validated_field_by_field():
    namespace, *_ = _proxy_harness()
    parse = namespace["_ref_updates_from"]

    class _Up:
        def __init__(self, value):
            self.headers = _Headers(
                {} if value is None else {"x-forkmesh-ref-updates": value})

    assert parse(_Up(None)) == {"total": 0, "updates": []}
    assert parse(_Up("not json")) == {"total": 0, "updates": []}
    raw = json.dumps({"total": 5, "updates": [
        {"ref": "refs/heads/x", "old": "0" * 40, "new": "1" * 40,
         "commits": "2"},
        {"ref": "refs/notes/x", "old": "0" * 40, "new": "1" * 40},
        {"ref": "refs/tags/y", "old": "zz", "new": "1" * 40},
    ]})
    assert parse(_Up(raw)) == {"total": 5, "updates": [
        {"ref": "refs/heads/x", "old": "0" * 40, "new": "1" * 40,
         "commits": 2}]}


def test_a_key_among_the_owners_signers_still_gets_the_org_write_check():
    namespace, audits = _authorize_harness(
        devices={MOBILE_DEVICE_KEY: _mobile_device(
            capabilities="browse,owner_sign")},
        org_write_accounts={("alice", "app", "bob")},
        pubkey_accounts={MOBILE_DEVICE_KEY: {"name": "bob"}},
    )
    assert _run_authorize(namespace, _signed(MOBILE_DEVICE_KEY)) == "bob"
    assert audits[-1]["outcome"] == "success"
    assert audits[-1]["actor"] == "bob"


def test_a_signature_over_the_org_alias_owner_is_accepted_for_its_node():
    namespace, audits = _authorize_harness(
        org_write_accounts={("alice", "app", "bob")},
        pubkey_accounts={MOBILE_DEVICE_KEY: {"name": "bob"}},
    )
    request = _signed(MOBILE_DEVICE_KEY, owner="acme")
    run = lambda alias: asyncio.run(namespace["_git_push_authorize"](
        object(), "alice", "app", request["method"],
        hashlib.sha256(request["body"]).hexdigest(), request["pubkey"],
        request["ts"], request["sig"], alias_owner=alias))
    assert run("") == ""
    assert run("other") == ""
    assert run("acme") == "bob"


def test_alias_owner_comes_from_the_original_url_and_must_map_to_the_node():
    import urllib.parse as _parse
    import re as _re

    async def _org_repo_node(_env, org, repo):
        return {("acme", "app"): "alice"}.get((org, repo), "")

    namespace = _load({"_git_push_alias_owner"}, {
        **BASE_GLOBALS,
        "urlparse": _parse.urlparse,
        "GIT_INFO_RE": _re.compile(r"^/([^/]+)/([^/]+?)(?:\.git)?/info/refs$"),
        "GIT_RECEIVE_RE": _re.compile(
            r"^/([^/]+)/([^/]+?)(?:\.git)?/git-receive-pack$"),
        "safe_segment": lambda value: value if value and "/" not in value else "",
        "_org_repo_node": _org_repo_node,
    })
    alias = namespace["_git_push_alias_owner"]

    class Req:
        def __init__(self, url):
            self.url = url

    base = "https://app.example"
    assert asyncio.run(alias(object(), Req(
        base + "/acme/app.git/git-receive-pack"), "alice", "app")) == "acme"
    assert asyncio.run(alias(object(), Req(
        base + "/acme/app.git/info/refs?service=git-receive-pack"),
        "alice", "app")) == "acme"
    assert asyncio.run(alias(object(), Req(
        base + "/acme/app.git/git-receive-pack"), "mallory", "app")) == ""
    assert asyncio.run(alias(object(), Req(
        base + "/alice/app.git/git-receive-pack"), "alice", "app")) == ""
    assert asyncio.run(alias(object(), Req(
        base + "/acme/other.git/git-receive-pack"), "alice", "app")) == ""


def test_push_proofs_ask_for_the_org_alias_the_gateways_serve():
    namespace = _candidates_harness(
        context={"owner": "core-a", "repo": "app", "repoBi": "bi:core-a/app"},
        routed=[_endpoint("mirror-b")],
        proofs={"mirror-b": True},
        write_row=None,
        alias="acme",
    )
    assert [e["node"] for e in _run_candidates(namespace)] == ["mirror-b"]
    assert namespace["_proof_owners"] == [("acme", "bi:acme/app")]


def test_a_push_through_an_org_alias_targets_the_alias_path_on_the_gateway():
    namespace, _messages, fetches = _proxy_harness(
        candidates=[_endpoint("core-a")], upstreams=[_Upstream()],
        alias_owner="acme")
    request = _Request(
        "https://forkmesh.test/acme/app/git-receive-pack",
        headers=_signed_headers(), body=b"pack")
    _run_proxy(namespace, request, is_info=False)
    assert "/acme/app/" in fetches[0][0]
    assert "/alice/app/" not in fetches[0][0]
