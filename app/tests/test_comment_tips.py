"""Comment tips: address validation, Solana Pay links, the request endpoint's
permissions and caps, read-only verification against a mocked RPC, the cron
phase, and administrator-only settings."""

import ast
import asyncio
import hashlib
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import comment_tips as tips  # noqa: E402
import schema  # noqa: E402

SIGNATURE = "5" * 88


def b58(data):
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    number = int.from_bytes(data, "big")
    out = ""
    while number:
        number, rem = divmod(number, 58)
        out = alphabet[rem] + out
    return "1" * (len(data) - len(data.lstrip(b"\0"))) + out


# Valid 32-byte keys (base58 of fixed bytes), never real wallets or mints.
BOB_WALLET = b58(bytes(range(1, 33)))
TOKEN_MINT = b58(bytes(range(40, 72)))


def bi(text):
    return hashlib.sha256(str(text).strip().lower().encode()).hexdigest()


def run(coro):
    return asyncio.run(coro)


def _entry_functions(*names, extra=None):
    text = (ROOT / "src/entry.py").read_text("utf-8")
    tree = ast.parse(text)
    nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert {node.name for node in nodes} == set(names)
    namespace = dict(extra or {})
    exec(compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
                 "entry.py", "exec"), namespace)
    return namespace


PAY = _entry_functions("_amount_sol", "_amount_units", "_solana_pay_uri",
                       extra={"LAMPORTS_PER_SOL": 10 ** 9, "quote": quote})


# --------------------------------------------------------------------------
# Pure helpers


def test_addresses_are_base58_public_keys():
    assert tips.valid_address(BOB_WALLET)
    assert tips.valid_address(tips.new_reference())
    assert len({tips.new_reference() for _ in range(20)}) == 20
    for bad in ("", "0" * 44, "O" * 44, "l" * 40, "1" * 31, "z" * 44,
                b58(bytes(range(1, 31))), b58(b"\xff" * 40), 12345, None):
        assert not tips.valid_address(bad), bad


def test_amounts_become_exact_base_units():
    assert tips.to_units("0.05", 9) == 50_000_000
    assert tips.to_units("1.5", 6) == 1_500_000
    assert tips.to_units("10", 0) == 10
    for bad in ("", "-1", "1e3", "0.0000000001", "abc", "1.", ".5", "1,5"):
        assert tips.to_units(bad, 9) == 0, bad
    assert tips.to_units("1.25", 1) == 0


def test_solana_pay_links_for_sol_keep_their_form_and_tokens_name_the_mint():
    uri = PAY["_solana_pay_uri"]
    legacy = uri(BOB_WALLET, 50_000_000, "REF", "Join ForkMesh")
    assert legacy == ("solana:%s?amount=0.050000000&reference=REF&label=ForkMesh"
                      "&message=Join%%20ForkMesh" % BOB_WALLET)
    assert uri(BOB_WALLET, 1, "") == "solana:%s?amount=0.000000001&label=ForkMesh&message=Join%%20ForkMesh" % BOB_WALLET
    token = uri(BOB_WALLET, 1_500_000, "REF", "Thanks for #7", spl_token=TOKEN_MINT,
                decimals=6, label="Tip for @bob")
    assert token == ("solana:%s?amount=1.5&spl-token=%s&reference=REF&label=Tip%%20for%%20%%40bob"
                     "&message=Thanks%%20for%%20%%237" % (BOB_WALLET, TOKEN_MINT))
    assert PAY["_amount_units"](10 ** 9, 9) == "1"
    assert PAY["_amount_units"](123, 0) == "123"
    assert PAY["_amount_units"](1, 18) == "0.000000000000000001"


def tx(recipient=BOB_WALLET, reference="REF", delta=50_000_000, err=None, token=None):
    keys = [{"pubkey": "Payer"}, {"pubkey": recipient}, {"pubkey": reference}]
    meta = {"err": err, "preBalances": [10 ** 9, 5, 0], "postBalances": [10 ** 9 - delta, 5 + delta, 0]}
    if token:
        meta["preTokenBalances"] = [{"accountIndex": 3, "mint": token, "owner": recipient,
                                     "uiTokenAmount": {"amount": "100"}}]
        meta["postTokenBalances"] = [{"accountIndex": 3, "mint": token, "owner": recipient,
                                      "uiTokenAmount": {"amount": str(100 + delta)}}]
        meta["preBalances"] = meta["postBalances"] = [1, 1, 1]
    return {"transaction": {"message": {"accountKeys": keys}}, "meta": meta}


def test_a_transfer_matches_only_when_it_paid_enough_to_the_recipient():
    match = tips.transfer_matches
    assert match(tx(), BOB_WALLET, "REF", "", 50_000_000)
    assert not match(tx(delta=49_999_999), BOB_WALLET, "REF", "", 50_000_000)
    assert not match(tx(err={"InstructionError": [0, "x"]}), BOB_WALLET, "REF", "", 1)
    assert not match(tx(recipient="Mallory"), BOB_WALLET, "REF", "", 1)
    assert not match(tx(reference="OTHER"), BOB_WALLET, "REF", "", 1)
    assert not match(None, BOB_WALLET, "REF", "", 1)
    spl = tx(token=TOKEN_MINT, delta=1_500_000)
    assert match(spl, BOB_WALLET, "REF", TOKEN_MINT, 1_500_000)
    assert not match(spl, BOB_WALLET, "REF", TOKEN_MINT, 1_500_001)
    assert not match(spl, BOB_WALLET, "REF", "OtherMint", 1)
    # Paying SOL does not satisfy a token request and vice versa.
    assert not match(tx(), BOB_WALLET, "REF", TOKEN_MINT, 1)


# --------------------------------------------------------------------------
# Endpoints


REPOS = {"alice/app": "public", "alice/secret": "private"}
WALLETS = {"bob": BOB_WALLET, "carol": "", "alice": BOB_WALLET}


class FakeD1:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in schema.SCHEMA_STATEMENTS:
            if re.search(r"comment_tips|comment_extras_rate|instance_settings|tip_notice", statement):
                self.db.execute(statement)

    async def all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def first(self, env, sql, *args):
        rows = await self.all(env, sql, *args)
        return rows[0] if rows else None

    async def run(self, env, sql, *args):
        self.db.execute(sql, args)


class FakeRpc:
    def __init__(self):
        self.calls = []
        self.signatures = []
        self.transactions = {}
        self.down = False

    async def __call__(self, env, method, params):
        self.calls.append((method, params))
        if self.down:
            return None
        if method == "getSignaturesForAddress":
            return {"result": [{"signature": sig, "err": None} for sig in self.signatures]}
        if method == "getTransaction":
            return {"result": self.transactions.get(params[0])}
        return None


@pytest.fixture
def rt(monkeypatch):
    d1 = FakeD1()
    clock = SimpleNamespace(now=10_000_000_000)
    rpc = FakeRpc()
    pings = []

    async def access(env, request, owner, repo, viewer=""):
        visibility = REPOS.get((owner + "/" + repo).lower())
        if visibility is None or (visibility == "private" and viewer != "alice"):
            return None
        return {"repoBi": bi(owner + "/" + repo), "visibility": visibility, "viewer": viewer}

    async def session(env, request, data=None):
        return (bi(request.account), {"name": request.account}) if request.account else ("", None)

    async def account_row(env, name):
        return bi(name), ({"solana": WALLETS[name]} if name in WALLETS else None)

    async def blind_index(env, text):
        return bi(text)

    async def encrypt_row(env, obj):
        return "enc:" + json.dumps(obj)

    async def decrypt_row(env, stored):
        return json.loads(stored[4:])

    async def body(request, max_bytes=None):
        return request.body

    async def nothing(*_args, **_kwargs):
        return None

    async def enqueue(env, recipient, kind, title, **kwargs):
        pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))
        return True

    def json_response(data, status=200, cache_control=None, extra_headers=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control,
                               headers=extra_headers or {})

    for name, value in {
        "d1_all": d1.all, "d1_first": d1.first, "d1_run": d1.run,
        "_repository_access_context": access, "_account_session_record": session,
        "_account_row": account_row, "blind_index": blind_index,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "bounded_json_request": body, "json_response": json_response,
        "ensure_schema": nothing, "_audit_sensitive_action": nothing,
        "enqueue_notification": enqueue, "_solana_rpc": rpc,
        "_solana_pay_uri": PAY["_solana_pay_uri"], "_amount_units": PAY["_amount_units"],
        "_is_admin": lambda env, name: _async(name == "alice"),
        "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
        "repo_web_href": lambda owner, repo: "/%s/%s" % (owner, repo),
        "method_name": lambda request: request.method,
        "valid_node_name": lambda value: bool(re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", value or "")),
        "clean_string": lambda value, n=240: value.strip()[:n] if isinstance(value, str) else "",
        "MAX_NODE_NAME": 63, "Date": SimpleNamespace(now=lambda: clock.now),
    }.items():
        monkeypatch.setattr(tips, name, value, raising=False)
    return SimpleNamespace(d1=d1, clock=clock, rpc=rpc, pings=pings)


async def _async(value):
    return value


def intent(account, repo="app", **fields):
    body = {"kind": "issue", "number": 7, "comment": "c1", "recipient": "bob",
            "token": "sol", "amount": "0.05"}
    body.update(fields)
    request = SimpleNamespace(method="POST", account=account, body=body,
                              url="https://forge.test/api/repo/alice/%s/tips" % repo)
    return run(tips.intent_handler(SimpleNamespace(), request, "alice", repo))


def call(rest, method="GET", account="", query="", body=None):
    request = SimpleNamespace(method=method, account=account, body=body,
                              url="https://forge.test/api/tips/%s%s" % (rest, query))
    return run(tips.handler(SimpleNamespace(), request, rest))


def test_a_tip_request_is_a_solana_pay_link_with_a_fresh_reference(rt):
    reply = intent("dave")
    assert reply.status == 200, reply.data
    data = reply.data
    assert tips.valid_address(data["reference"])
    assert data["uri"].startswith("solana:%s?amount=0.050000000&reference=%s&label=Tip%%20for%%20%%40bob"
                                  % (BOB_WALLET, data["reference"]))
    assert "never holds" in data["disclaimer"]
    row = dict(rt.d1.db.execute("SELECT * FROM comment_tips").fetchone())
    assert row["status"] == "pending" and row["amount"] == "50000000"
    assert row["tipper_bi"] == bi("dave") and row["recipient_bi"] == bi("bob")
    assert "dave" not in json.dumps({k: v for k, v in row.items() if k != "data"})
    second = intent("dave")
    assert second.data["reference"] != data["reference"]


@pytest.mark.parametrize("account, fields, repo, status, error", [
    ("", {}, "app", 401, "unauthorized"),
    ("bob", {}, "app", 400, "self_tip"),
    ("dave", {"recipient": "carol"}, "app", 404, "no_wallet"),
    ("dave", {"recipient": "zed"}, "app", 404, "no_wallet"),
    ("dave", {}, "secret", 404, "not_found"),
    ("dave", {"token": "doge"}, "app", 400, "invalid_token"),
    ("dave", {"token": "token"}, "app", 400, "invalid_token"),
    ("dave", {"amount": "0"}, "app", 400, "invalid_amount"),
    ("dave", {"amount": "1001"}, "app", 400, "invalid_amount"),
    ("dave", {"amount": "0.0000000001"}, "app", 400, "invalid_amount"),
    ("dave", {"kind": "wiki"}, "app", 400, "invalid_target"),
    ("dave", {"comment": "../x y"}, "app", 400, "invalid_target"),
])
def test_tip_requests_are_checked(rt, account, fields, repo, status, error):
    reply = intent(account, repo, **fields)
    assert (reply.status, reply.data.get("error")) == (status, error)


def test_tip_requests_are_rate_limited_and_bounded(rt, monkeypatch):
    for _ in range(tips.MAX_PENDING_PER_ACCOUNT + 3):
        assert intent("dave").status == 200
        rt.clock.now += 61_000
    pending = rt.d1.db.execute("SELECT COUNT(*) FROM comment_tips WHERE tipper_bi=?",
                               (bi("dave"),)).fetchone()[0]
    # The oldest pending request makes way for a new one.
    assert pending == tips.MAX_PENDING_PER_ACCOUNT
    monkeypatch.setattr(tips, "INTENT_RATE", 1)
    assert intent("erin").status == 200
    assert intent("erin").data["error"] == "rate_limited"
    rt.clock.now += tips.PENDING_TTL_MS + 1
    assert intent("erin").status == 200
    assert rt.d1.db.execute("SELECT COUNT(*) FROM comment_tips").fetchone()[0] == 1


def test_tokens_follow_the_instance_settings(rt):
    assert [t["id"] for t in call("config").data["tokens"]] == ["sol", "usdc"]
    assert call("settings", "POST", "bob", body={"usdc": False}).status == 403
    bad = call("settings", "POST", "alice", body={"token": {"mint": "nope", "symbol": "X", "decimals": 9}})
    assert bad.data["error"] == "invalid_token"
    saved = call("settings", "POST", "alice", body={
        "usdc": False, "token": {"mint": TOKEN_MINT, "symbol": "TOKEN", "decimals": 9}})
    assert [t["id"] for t in saved.data["tokens"]] == ["sol", "token"]
    reply = intent("dave", token="token", amount="12.5")
    assert reply.status == 200
    assert "amount=12.5&spl-token=%s" % TOKEN_MINT in reply.data["uri"]
    assert intent("dave", token="usdc").data["error"] == "invalid_token"
    call("settings", "POST", "alice", body={"enabled": False})
    assert call("config").data == {"ok": True, "enabled": False, "tokens": [],
                                   "disclaimer": tips.DISCLAIMER}
    assert intent("dave").data["error"] == "tips_disabled"


def verify(account, reference):
    return call("verify", "POST", account, body={"reference": reference})


def test_verification_confirms_a_matching_transfer_once(rt):
    reference = intent("dave").data["reference"]
    assert verify("dave", reference).data["status"] == "pending"
    rt.rpc.signatures = [SIGNATURE]
    rt.rpc.transactions[SIGNATURE] = tx(reference=reference, delta=40_000_000)
    assert verify("dave", reference).data["status"] == "pending"
    rt.rpc.transactions[SIGNATURE] = tx(reference=reference)
    done = verify("dave", reference)
    assert done.data == {"ok": True, "status": "confirmed", "signature": SIGNATURE}
    calls = len(rt.rpc.calls)
    assert verify("dave", reference).data["status"] == "confirmed"
    assert len(rt.rpc.calls) == calls
    assert [(p["recipient"], p["kind"], p["dedupe"]) for p in rt.pings] == [
        ("bob", "tip_sent", "tip:" + SIGNATURE)]
    assert rt.pings[0]["title"] == "dave tipped your comment on alice/app#7"
    assert "0.05 SOL" in rt.pings[0]["body"]
    notice = dict(rt.d1.db.execute("SELECT * FROM tip_notice").fetchone())
    assert notice["lamports"] == 50_000_000 and notice["to_bi"] == bi("bob")
    # The same transaction cannot confirm a second request.
    other = intent("dave").data["reference"]
    rt.rpc.transactions[SIGNATURE] = tx(reference=other)
    assert verify("dave", other).data["status"] == "pending"
    # Only the tipper can ask, and only methods the read-only helper allows.
    assert verify("erin", reference).status == 404
    assert verify("dave", "not a key").data["error"] == "invalid_reference"
    assert {method for method, _ in rt.rpc.calls} <= {"getSignaturesForAddress", "getTransaction"}


def test_an_rpc_outage_is_reported_and_token_tips_skip_the_sol_receipt(rt):
    call("settings", "POST", "alice", body={"token": {"mint": TOKEN_MINT, "symbol": "TOKEN", "decimals": 6}})
    reference = intent("dave", token="token", amount="1.5").data["reference"]
    rt.rpc.down = True
    assert verify("dave", reference).data["status"] == "unavailable"
    rt.rpc.down = False
    rt.rpc.signatures = [SIGNATURE]
    rt.rpc.transactions[SIGNATURE] = tx(reference=reference, token=TOKEN_MINT, delta=1_500_000)
    assert verify("dave", reference).data["status"] == "confirmed"
    assert rt.d1.db.execute("SELECT COUNT(*) FROM tip_notice").fetchone()[0] == 0
    assert "1.5 TOKEN" in rt.pings[-1]["body"]


def test_the_cron_phase_checks_a_few_recent_tips_and_drops_stale_ones(rt):
    references = []
    for name in ("dave", "erin", "fay", "gus"):
        references.append(intent(name).data["reference"])
        rt.clock.now += 1000
    stale_reference = references[0]
    rt.d1.db.execute("UPDATE comment_tips SET created_at=? WHERE reference=?",
                     (rt.clock.now - tips.PENDING_TTL_MS - 1, stale_reference))
    rt.clock.now += tips.CRON_RECHECK_MS + 1
    rt.rpc.signatures = [SIGNATURE]
    rt.rpc.transactions[SIGNATURE] = tx(reference=references[2])
    result = run(tips.verify_cron(SimpleNamespace()))
    assert result == {"checked": 3, "confirmed": 1}
    assert rt.d1.db.execute("SELECT COUNT(*) FROM comment_tips WHERE reference=?",
                            (stale_reference,)).fetchone()[0] == 0
    signature_calls = [c for c in rt.rpc.calls if c[0] == "getSignaturesForAddress"]
    assert len(signature_calls) == 3
    # Just checked: nothing is due again yet.
    assert run(tips.verify_cron(SimpleNamespace())) == {"checked": 0, "confirmed": 0}


def test_recipients_lists_only_public_tip_addresses(rt):
    reply = call("recipients", query="?names=bob,carol,zed,BAD%20NAME")
    assert reply.data["wallets"] == {"bob": BOB_WALLET}
    assert reply.cache_control.startswith("public")


def test_tips_never_sign_or_send():
    source = (ROOT / "src/comment_tips.py").read_text("utf-8")
    for forbidden in ("_solana_rpc_write", "sendTransaction", "_solana_send_transfers",
                      "sign(", "private_key", "seed"):
        assert forbidden not in source
    entry = (ROOT / "src/entry.py").read_text("utf-8")
    assert 'if phase == "tip-verify":' in entry
    cron = (ROOT / "src/cron_activity.py").read_text("utf-8")
    assert '("tip-verify", 5, (2,), "rewards",' in cron
