#!/usr/bin/env python3
"""Member-to-member tip notices.

ForkMesh is not a party to a tip: the sender's desktop holds the wallet key in
its own encrypted local vault, signs the SOL transfer there, and broadcasts it
straight to a Solana RPC. POST /api/accounts/tip-notice arrives *after* that,
carrying only a public receipt.

Contract under test, extracted straight from entry.py:
- the sender is the signed-in session, never a name in the body — a guest is
  refused and nobody gets mailed;
- a desktop authenticates with this account's key and normally holds no session
  token at all, so it may instead sign the payout itself: the proof binds the
  recipient, amount, chain signature and reward, so it authorizes exactly one
  notice about one transfer and a session still takes precedence;
- a malformed signature/amount/network or an unknown recipient is rejected
  before any row is written or any mail is sent; self-tips are valid receipts;
- the chain signature is the idempotency key: a replayed notice answers 200 and
  sends nothing, so it can never mail a recipient twice;
- the row is claimed BEFORE the send, so a crash mid-send cannot double-send;
- a recipient with no verified address, a global email opt-out, or the tip kind
  switched off still gets the in-app ping but no email;
- a sender is capped per rolling day;
- nothing in the request, the row, or the email can carry private key material:
  the endpoint accepts no key-shaped field at all.
"""

import ast
import asyncio
import hashlib
from pathlib import Path
import re
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, urlencode, urlparse

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
SCHEMA = ROOT / "src" / "schema.py"
TEXT = ENTRY.read_text(encoding="utf-8")
SCHEMA_TEXT = SCHEMA.read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0128_tip_notice.sql"
sys.path.insert(0, str(ROOT / "src"))
import ledger as ledger_module  # noqa: E402

FUNCS = {
    "_format_sol",
    "_tip_explorer_url",
    "_send_tip_email",
    "_tip_work_payment_context",
    "_tip_notice_signed_session",
    "_account_tip_notice",
}
ASSIGNS = {
    "TIP_NOTICE_DAILY_LIMIT",
    "TIP_NOTICE_WINDOW_MS",
    "TIP_NOTICE_MAX_LAMPORTS",
    "TIP_SIGNATURE_RE",
    "TIP_NETWORKS",
    "TIP_NOTICE_PROOF",
    "TIP_WORK_PAYMENT_PROOF",
    "TIP_TASK_DESCRIPTION_MAX",
    "TIP_OUTPUT_URL_MAX",
    "TIP_PRICE_MAX_AGE_MS",
    "TIP_NOTICE_PATH_RE",
    "NOTIFICATION_EMAIL_DEFAULTS",
    "NOTIFICATION_EMAIL_KINDS",
}

NOW = 2_000_000_000_000
DAY = 24 * 60 * 60 * 1000
SIGNATURE = "5" * 88
OTHER_SIGNATURE = "4" * 88
PAYOUT_ADDRESS = "So11111111111111111111111111111111111111112"


def _load(extra):
    tree = ast.parse(TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & ASSIGNS:
            selected.append(node)
        elif (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS):
            selected.append(node)
    found = {getattr(n, "name", "") for n in selected}
    assert FUNCS <= found | {""}, sorted(FUNCS - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    namespace.setdefault("_award_genies_gift", _no_badge)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


async def _no_badge(*args, **kwargs):
    return False


def _harness(sender="alice", recipient_rec=None, rows=None, sent=True,
             now=NOW, reward=None, org_owner=True, chain_match=True,
             reward_update_changes=1, signer_rec=None,
             quote_price_micros=150_000_000,
             source_balance_lamports=997_000_000):
    """A tip-notice runtime over one signed-in sender and one recipient.

    `sender` is the browser-style session. `signer_rec` is the account a
    key-signed desktop can prove it is — the ordinary desktop, which holds this
    account's key and no session token at all.
    """
    if signer_rec is None:
        signer_rec = {"name": "alice", "status": "active", "kind": "user"}
    if recipient_rec is None:
        recipient_rec = {"name": "bob", "email": "bob@example.com",
                         "email_verified": True, "status": "active",
                         "solana": PAYOUT_ADDRESS}
    inserted = []
    updated = []
    emails = []
    pings = []
    stamped = []
    reward_writes = []
    chain_plans = []
    balance_reads = []
    balance_writes = []
    table = list(rows or [])
    reward_row = dict(reward) if reward else None

    class _RunResult:
        def __init__(self, changes=1):
            self.meta = SimpleNamespace(changes=changes)

    class _DateStub:
        @staticmethod
        def now():
            return now

    def json_response(body, status=200, **_kwargs):
        return {"status": status, "body": body}

    async def _account_session_record(_env, _request, _data=None):
        if not sender:
            return "", None
        return "bi:" + sender, {"name": sender}

    async def _account_row(_env, name):
        if name == signer_rec.get("name"):
            return "bi:" + name, dict(signer_rec)
        if name != recipient_rec.get("name"):
            return "", None
        return "bi:" + name, dict(recipient_rec)

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_first(_env, sql, *args):
        if "FROM contributor_reward_estimates" in sql:
            if not reward_row or args[0] != reward_row.get("reward_id"):
                return None
            return dict(reward_row)
        if "SELECT emailed FROM tip_notice" in sql:
            for row in table:
                if row["signature"] == args[0]:
                    return dict(row)
            return None
        if "COUNT(*)" in sql:
            from_bi, since = args
            return {"n": sum(1 for row in table
                             if row["from_bi"] == from_bi
                             and row["ts"] >= since)}
        raise AssertionError("unexpected query: " + sql)

    async def d1_run(_env, sql, *args):
        if "UPDATE contributor_reward_estimates" in sql:
            reward_writes.append((sql, args))
            if "status='paid'" in sql and reward_update_changes:
                reward_row.update({
                    "status": "paid",
                    "paid_at": args[0],
                    "updated_at": args[1],
                    "source_address": args[2],
                    "destination_address": args[3],
                    "tx_signature": args[4],
                    "payment_usd_micros": args[5],
                    "payment_sol_usd_micros": args[6],
                    "payment_valued_at": args[7],
                })
            elif "status='submitted'" in sql:
                reward_row.update({
                    "status": "submitted",
                    "tx_signature": args[4],
                    "payment_usd_micros": args[5],
                    "payment_sol_usd_micros": args[6],
                    "payment_valued_at": args[7],
                })
            return _RunResult(
                reward_update_changes if "status='paid'" in sql else 1)
        if sql.strip().startswith("INSERT INTO tip_notice"):
            inserted.append(args)
            table.append({"signature": args[0], "from_bi": args[1],
                          "to_bi": args[2], "lamports": args[3],
                          "network": args[4], "ts": args[5], "emailed": 0})
            return _RunResult()
        if "UPDATE tip_notice SET emailed=1" in sql:
            updated.append(args)
            return _RunResult()
        raise AssertionError("unexpected write: " + sql)

    async def _org_row(_env, name):
        return "org-bi:" + name, {"name": name}

    async def _org_role(_env, _org_bi, _actor):
        return "owner" if org_owner else "member"

    async def _account_owns_node(_env, _actor, _node):
        return org_owner

    async def _solana_transaction_matches_plan(_env, signature, plan):
        chain_plans.append((signature, plan))
        return chain_match

    async def _sol_usd_quote():
        return {
            "priceMicros": quote_price_micros,
            "observedAt": now,
        }

    async def _reward_balance_lamports(_env, address):
        balance_reads.append(address)
        return source_balance_lamports

    async def _store_wallet_balance(
            _env, address, lamports, checked_at, name=""):
        balance_writes.append((address, lamports, checked_at, name))

    async def _send_email(_env, to_email, subject, text, html=None, **kwargs):
        emails.append({"to": to_email, "subject": subject, "text": text,
                       "html": html, **kwargs})
        return sent

    async def _record_account_email(_env, name, kind, ok):
        stamped.append((name, kind, ok))

    async def enqueue_notification(_env, target, kind, title, **kwargs):
        pings.append({"recipient": target, "kind": kind, "title": title,
                      **kwargs})
        return True

    async def bounded_json_request(request):
        return dict(getattr(request, "payload", {}) or {})

    for name, value in {
        "Date": _DateStub,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "json_response": json_response,
        "_org_row": _org_row,
        "_org_role": _org_role,
        "_account_owns_node": _account_owns_node,
        "_solana_transaction_matches_plan": (
            _solana_transaction_matches_plan),
        "_sol_usd_quote": _sol_usd_quote,
        "_reward_balance_lamports": _reward_balance_lamports,
        "_store_wallet_balance": _store_wallet_balance,
    }.items():
        setattr(ledger_module, name, value)

    async def _verify_owner_signature(_env, node, sig, canonical):
        # A stub with the property that matters: a signature authorizes exactly
        # the canonical it was made over, and nothing else.
        return sig in {
            "sig:" + node + ":" + canonical.decode(),
            "sig:sha256:" + hashlib.sha256(canonical).hexdigest(),
        }

    namespace = _load({
        "re": re,
        "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "method_name": lambda request: getattr(request, "method", "POST"),
        "_ts_ok": lambda ts: (
            str(ts).lstrip("-").isdigit() and abs(now - int(ts)) <= 120000),
        "_verify_owner_signature": _verify_owner_signature,
        "_account_kind": lambda rec: (rec or {}).get("kind", "user"),
        "quote": lambda value, safe="/": str(value),
        "Date": _DateStub,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "json_response": json_response,
        "bounded_json_request": bounded_json_request,
        "_account_session_record": _account_session_record,
        "_account_row": _account_row,
        "_org_row": _org_row,
        "_org_role": _org_role,
        "_account_owns_node": _account_owns_node,
        "_solana_transaction_matches_plan": _solana_transaction_matches_plan,
        "contributor_reward_payment": (
            ledger_module.contributor_reward_payment),
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "_send_email": _send_email,
        "_record_account_email": _record_account_email,
        "enqueue_notification": enqueue_notification,
        "notification_email_enabled": lambda rec, kind: bool(
            (rec.get("notification_preferences") or {}).get(kind, True)),
        "_html_escape": lambda value: str(value or ""),
        "_forkmesh_email_card_html": lambda *a, **k: "<html>%s</html>" % a[0],
        "_forkmesh_email_action_html": lambda label, url: url,
    })
    namespace["_inserted"] = inserted
    namespace["_updated"] = updated
    namespace["_emails"] = emails
    namespace["_pings"] = pings
    namespace["_stamped"] = stamped
    namespace["_table"] = table
    namespace["_reward"] = reward_row
    namespace["_reward_writes"] = reward_writes
    namespace["_chain_plans"] = chain_plans
    namespace["_balance_reads"] = balance_reads
    namespace["_balance_writes"] = balance_writes
    return namespace


TIP_NOTICE_URL = "https://app.forkmesh.com/api/accounts/tip-notice"


def _notice(namespace, query="", **payload):
    body = {"toNodeName": "bob", "lamports": 50000000,
            "signature": SIGNATURE, "network": "mainnet"}
    body.update(payload)
    url = TIP_NOTICE_URL + ("?" + query if query else "")
    return asyncio.run(namespace["_account_tip_notice"](
        object(),
        SimpleNamespace(method="POST", headers={}, payload=body, url=url)))


def _signed_query(user="alice", ts=NOW, recipient="bob", lamports=50000000,
                  signature=SIGNATURE, reward_id="", proof=None):
    """The user/ts/sig triple a desktop with no session token sends.

    The canonical is the payout itself, so a proof made for one transfer names
    that transfer and cannot stand in for another.
    """
    canonical = "\n".join([
        "forkmesh-account-tip-notice-v1", user, str(ts), recipient,
        str(lamports), signature, reward_id,
    ])
    sig = proof if proof is not None else (
        "sig:sha256:" + hashlib.sha256(canonical.encode()).hexdigest())
    return urlencode({"user": user, "ts": str(ts), "sig": sig})


def _work_payload(**changes):
    body = {
        "taskDescription": "Implement the Ledger payment dialog",
        "outputUrl": "https://github.com/forkmesh/forkmesh/pull/777",
        "enteredCurrency": "USD",
        "enteredUsdMicros": 5_000_000,
        "solUsdPriceMicros": 100_000_000,
        "priceObservedAt": NOW,
        "fromAddress": "1" * 32,
    }
    body.update(changes)
    return body


def _signed_work_query(user="alice", ts=NOW, recipient="bob",
                       lamports=50_000_000, signature=SIGNATURE,
                       reward_id="", proof=None, **changes):
    body = _work_payload(**changes)
    canonical = "\n".join([
        "forkmesh-account-tip-work-payment-v2", user, str(ts), recipient,
        str(lamports), signature, reward_id, body["taskDescription"],
        body["outputUrl"], body["enteredCurrency"],
        str(body["enteredUsdMicros"]), str(body["solUsdPriceMicros"]),
        str(body["priceObservedAt"]), body["fromAddress"],
    ])
    sig = proof if proof is not None else (
        "sig:sha256:" + hashlib.sha256(canonical.encode()).hexdigest())
    return urlencode({"user": user, "ts": str(ts), "sig": sig})


def _reward(status="ready"):
    return {
        "reward_id": "reward-forkmesh-forkmesh-issue-635",
        "recipient_name": "bob",
        "repo_owner": "forkmesh",
        "repo_name": "forkmesh",
        "amount_lamports": 3_000_000,
        "status": status,
        "tx_signature": "",
        "payment_usd_micros": 0,
        "payment_sol_usd_micros": 0,
        "payment_valued_at": 0,
    }


def _reward_notice(namespace, **payload):
    body = {
        "rewardId": "reward-forkmesh-forkmesh-issue-635",
        "fromAddress": "1" * 32,
        "lamports": 3_000_000,
    }
    body.update(payload)
    return _notice(namespace, **body)


# --- the happy path ---------------------------------------------------------

def test_a_valid_notice_records_the_receipt_emails_and_pings():
    ns = _harness()
    result = _notice(ns)
    assert result["status"] == 200
    assert result["body"]["ok"] is True
    assert result["body"]["emailed"] is True
    assert result["body"]["amount"] == "0.05 SOL"
    assert result["body"]["custody"] == "self-custodial-sender-signed"

    assert len(ns["_inserted"]) == 1
    signature, from_bi, to_bi, lamports, network, ts = ns["_inserted"][0]
    assert signature == SIGNATURE
    assert (from_bi, to_bi) == ("bi:alice", "bi:bob")
    assert (lamports, network, ts) == (50000000, "mainnet", NOW)

    assert len(ns["_emails"]) == 1
    email = ns["_emails"][0]
    assert email["to"] == "bob@example.com"
    assert "alice" in email["subject"]
    assert "0.05 SOL" in email["text"]
    assert SIGNATURE in email["text"]  # explorer link, for on-chain proof
    assert ns["_updated"] == [(SIGNATURE,)]
    assert ns["_stamped"] == [("bob", "tip", True)]

    assert len(ns["_pings"]) == 1
    assert ns["_pings"][0]["recipient"] == "bob"
    assert ns["_pings"][0]["kind"] == "tip_sent"
    assert ns["_pings"][0]["actor"] == "alice"
    assert ns["_pings"][0]["dedupe"] == "tip:" + SIGNATURE


def test_the_email_states_that_forkmesh_did_not_hold_the_funds():
    ns = _harness()
    _notice(ns)
    text = ns["_emails"][0]["text"]
    assert "ForkMesh did not hold or move these funds" in text
    # The amount is the sender's claim, so the mail must say so rather than
    # present it as something the relay verified.
    assert "what they reported" in text
    assert ns["_emails"][0]["email_kind"] == "tip"


def test_amounts_render_without_binary_float_drift():
    ns = _harness()
    assert ns["_format_sol"](50000000) == "0.05 SOL"
    assert ns["_format_sol"](1000000000) == "1.0 SOL"
    assert ns["_format_sol"](1000000001) == "1.000000001 SOL"


def test_explorer_link_carries_the_cluster_for_non_mainnet():
    ns = _harness()
    assert ns["_tip_explorer_url"](SIGNATURE, "mainnet").endswith(SIGNATURE)
    assert ns["_tip_explorer_url"](SIGNATURE, "devnet").endswith(
        "?cluster=devnet")


# --- authorization ----------------------------------------------------------

def test_a_guest_cannot_make_anyone_receive_a_tip_email():
    ns = _harness(sender="")
    result = _notice(ns)
    assert result["status"] == 401
    assert not ns["_emails"] and not ns["_inserted"] and not ns["_pings"]


def test_the_sender_is_the_session_not_a_name_in_the_body():
    ns = _harness(sender="alice")
    _notice(ns, fromNodeName="mallory")
    assert ns["_inserted"][0][1] == "bi:alice"
    assert ns["_pings"][0]["actor"] == "alice"


# --- the desktop that holds keys and no session token -----------------------
#
# A desktop signs the transfer with a wallet on its own disk and authenticates
# to ForkMesh with this account's Ed25519 key, not a password — so it normally
# has no session token at all. Without a second credential the transfer lands
# and the notice comes back 401, which is how a payout ends up on-chain with
# nobody told and every later payout held behind it.

def test_a_key_signed_desktop_can_report_a_payout_without_a_session():
    ns = _harness(sender="")
    result = _notice(ns, query=_signed_query())
    assert result["status"] == 200
    assert result["body"]["ok"] is True
    assert ns["_inserted"][0][1] == "bi:alice"
    assert ns["_pings"][0]["actor"] == "alice"


def test_a_key_signed_desktop_can_record_a_verified_usd_work_payment():
    ns = _harness(sender="")
    work = _work_payload()
    result = _notice(ns, query=_signed_work_query(), **work)

    assert result["status"] == 200
    assert result["body"]["taskDescription"] == work["taskDescription"]
    assert result["body"]["outputUrl"] == work["outputUrl"]
    assert ns["_chain_plans"] == [(SIGNATURE, {
        "network": "mainnet-beta",
        "sourceAddress": "1" * 32,
        "transfers": [{
            "address": PAYOUT_ADDRESS,
            "lamports": 50_000_000,
        }],
    })]
    assert ns["_inserted"][0][6:] == (
        work["taskDescription"], work["outputUrl"], "USD", 5_000_000,
        100_000_000, NOW)
    assert "paid you for work" in ns["_emails"][0]["subject"]
    assert work["outputUrl"] in ns["_emails"][0]["text"]
    assert ns["_pings"][0]["title"] == (
        "alice paid you for completed work")


def test_a_work_payment_waits_for_finality_and_rejects_a_mismatch():
    pending = _harness(chain_match=None)
    result = _notice(pending, **_work_payload())
    assert result["status"] == 202
    assert result["body"]["verification"] == "pending_finality"
    assert not pending["_inserted"] and not pending["_emails"]

    mismatch = _harness(chain_match=False)
    result = _notice(mismatch, **_work_payload())
    assert result["status"] == 409
    assert result["body"]["error"] == "payment_transaction_mismatch"
    assert not mismatch["_inserted"] and not mismatch["_emails"]


def test_a_usd_work_payment_must_match_its_exact_sol_conversion():
    ns = _harness()
    result = _notice(ns, lamports=49_999_000, **_work_payload())
    assert result["status"] == 400
    assert result["body"]["error"] == "invalid_payment_context"
    assert not ns["_chain_plans"] and not ns["_inserted"]


def test_the_signed_sender_is_the_signed_user_not_a_name_in_the_body():
    ns = _harness(sender="")
    _notice(ns, query=_signed_query(), fromNodeName="mallory")
    assert ns["_inserted"][0][1] == "bi:alice"


def test_the_signer_is_named_as_a_user_not_a_node():
    # The proof is an account key's, and an account is a person, not a
    # machine: the desktop that signed is one of that person's nodes and has
    # no part in the proof. So the query names the signer `user`, and the same
    # triple sent as `node` — the spelling the first desktop build put in the
    # network log ("node=jett") — is not a credential at all.
    assert _signed_query().startswith("user=alice&")
    ns = _harness(sender="")
    result = _notice(ns, query=_signed_query().replace("user=", "node=", 1))
    assert result["status"] == 401
    assert not ns["_emails"] and not ns["_inserted"] and not ns["_pings"]


def test_a_proof_for_one_payout_does_not_authorize_another():
    # Every field the proof covers, changed one at a time after signing. Each
    # would otherwise let one captured signature mail a different recipient, a
    # different amount, or claim a different transfer.
    for changed in ({"toNodeName": "carol"}, {"lamports": 1},
                    {"signature": OTHER_SIGNATURE},
                    {"rewardId": "reward-somebody-elses"}):
        ns = _harness(sender="")
        result = _notice(ns, query=_signed_query(), **changed)
        assert result["status"] == 401, changed
        assert not ns["_emails"] and not ns["_inserted"] and not ns["_pings"]


def test_an_unsigned_or_forged_guest_is_still_refused():
    for query in ("", _signed_query(proof="not-a-signature"),
                  _signed_query(user="mallory")):
        ns = _harness(sender="")
        result = _notice(ns, query=query)
        assert result["status"] == 401, query
        assert not ns["_emails"] and not ns["_inserted"] and not ns["_pings"]


def test_a_stale_proof_is_refused():
    ns = _harness(sender="")
    result = _notice(ns, query=_signed_query(ts=NOW - DAY))
    assert result["status"] == 401
    assert not ns["_emails"] and not ns["_inserted"]


def test_a_signed_proof_is_bound_to_the_tip_notice_path():
    # The proof carries no path of its own, so the resolver checks one. Without
    # that, the same triple would authorize any POST it were replayed onto.
    ns = _harness(sender="")
    request = SimpleNamespace(
        method="POST", headers={}, payload={},
        url="https://app.forkmesh.com/api/accounts/profile?" + _signed_query())
    resolved = asyncio.run(
        ns["_tip_notice_signed_session"](object(), request, {
            "toNodeName": "bob", "lamports": 50000000,
            "signature": SIGNATURE}))
    assert resolved == ("", None)


def test_a_session_still_wins_over_a_signed_triple():
    # The fallback only ever runs when there is no session, so a signed-in
    # browser keeps behaving exactly as it did.
    ns = _harness(sender="alice")
    result = _notice(ns, query=_signed_query(user="mallory"))
    assert result["status"] == 200
    assert ns["_inserted"][0][1] == "bi:alice"


def test_reward_payment_requires_the_repository_organization_owner():
    ns = _harness(reward=_reward(), org_owner=False)
    result = _reward_notice(ns)
    assert result == {
        "status": 403,
        "body": {"error": "reward_owner_required"},
    }
    assert not ns["_chain_plans"]
    assert not ns["_reward_writes"]
    assert not ns["_inserted"]


def test_reward_payment_matches_exact_recipient_amount_and_finalized_transfer():
    ns = _harness(reward=_reward())
    result = _reward_notice(ns)
    assert result["status"] == 200
    assert result["body"]["rewardStatus"] == "paid"
    assert ns["_reward"]["status"] == "paid"
    assert ns["_reward"]["tx_signature"] == SIGNATURE
    assert ns["_reward"]["payment_usd_micros"] == 450_000
    assert ns["_reward"]["payment_sol_usd_micros"] == 150_000_000
    assert ns["_reward"]["payment_valued_at"] == NOW
    assert result["body"]["sourceBalanceLamports"] == 997_000_000
    assert result["body"]["sourceBalanceCheckedAt"] == NOW
    assert ns["_balance_reads"] == ["1" * 32]
    assert ns["_balance_writes"] == [
        ("1" * 32, 997_000_000, NOW, ""),
    ]
    assert ns["_chain_plans"] == [(SIGNATURE, {
        "network": "mainnet-beta",
        "sourceAddress": "1" * 32,
        "transfers": [{
            "address": PAYOUT_ADDRESS,
            "lamports": 3_000_000,
        }],
    })]
    assert "status='paid'" in ns["_reward_writes"][0][0]
    assert len(ns["_inserted"]) == 1


def test_reward_payment_waits_for_chain_finality_without_marking_paid():
    ns = _harness(reward=_reward(), chain_match=None)
    result = _reward_notice(ns)
    assert result["status"] == 202
    assert result["body"]["verification"] == "pending_finality"
    assert ns["_reward"]["status"] == "submitted"
    assert ns["_reward"]["payment_usd_micros"] == 450_000
    assert not ns["_inserted"]


def test_reward_payment_retries_never_reprice_the_first_receipt():
    reward = _reward(status="submitted")
    reward.update({
        "tx_signature": SIGNATURE,
        "payment_usd_micros": 300_000,
        "payment_sol_usd_micros": 100_000_000,
        "payment_valued_at": NOW - 5_000,
    })
    ns = _harness(reward=reward, quote_price_micros=900_000_000)

    result = _reward_notice(ns)

    assert result["status"] == 200
    assert ns["_reward"]["payment_usd_micros"] == 300_000
    assert ns["_reward"]["payment_sol_usd_micros"] == 100_000_000
    assert ns["_reward"]["payment_valued_at"] == NOW - 5_000


def test_paid_reward_retry_returns_a_fresh_source_wallet_balance():
    reward = _reward(status="paid")
    reward["tx_signature"] = SIGNATURE
    ns = _harness(reward=reward, source_balance_lamports=996_000_000)

    result = _reward_notice(ns)

    assert result["status"] == 200
    assert result["body"]["duplicate"] is True
    assert result["body"]["rewardStatus"] == "paid"
    assert result["body"]["sourceBalanceLamports"] == 996_000_000
    assert ns["_balance_reads"] == ["1" * 32]
    assert not ns["_inserted"]


def test_reward_payment_rejects_chain_mismatch_and_concurrent_second_payment():
    mismatch = _harness(reward=_reward(), chain_match=False)
    assert _reward_notice(mismatch)["body"] == {
        "error": "reward_transaction_mismatch",
    }
    assert not mismatch["_reward_writes"]

    raced = _harness(reward=_reward(), reward_update_changes=0)
    result = _reward_notice(raced)
    assert result["status"] == 409
    assert result["body"] == {"error": "reward_already_paid"}
    assert not raced["_inserted"]


def test_reward_payment_rejects_an_estimate_amount_override():
    ns = _harness(reward=_reward())
    result = _reward_notice(ns, lamports=2_000_000)
    assert result["status"] == 409
    assert result["body"] == {"error": "reward_payment_mismatch"}
    assert not ns["_chain_plans"]


# --- validation -------------------------------------------------------------

def test_a_malformed_signature_is_rejected_before_any_write():
    for bad in ("", "too-short", "0" * 88, "!" * 70, "5" * 200):
        ns = _harness()
        result = _notice(ns, signature=bad)
        assert result["status"] == 400, bad
        assert not ns["_inserted"] and not ns["_emails"]


def test_a_nonsensical_amount_is_rejected():
    for bad in (0, -1, "abc", None, 10**18):
        ns = _harness()
        result = _notice(ns, lamports=bad)
        assert result["status"] == 400, bad
        assert not ns["_inserted"] and not ns["_emails"]


def test_an_unknown_network_is_rejected():
    ns = _harness()
    assert _notice(ns, network="mainnet-beta")["status"] == 400
    assert not ns["_inserted"]


def test_an_unknown_recipient_is_rejected():
    ns = _harness()
    result = _notice(ns, toNodeName="nobody")
    assert result["status"] == 404
    assert not ns["_inserted"] and not ns["_emails"]


def test_a_suspended_recipient_is_rejected():
    ns = _harness(recipient_rec={"name": "bob", "email": "bob@example.com",
                                 "email_verified": True,
                                 "status": "suspended",
                                 "solana": PAYOUT_ADDRESS})
    assert _notice(ns)["status"] == 404
    assert not ns["_emails"]


def test_a_recipient_with_no_payout_address_is_rejected():
    """The email would claim a transfer is heading for an address they never
    published, so the relay refuses to pass the claim on."""
    for missing in ({}, {"solana": ""}, {"solana": "not-an-address"}):
        rec = {"name": "bob", "email": "bob@example.com",
               "email_verified": True, "status": "active"}
        rec.update(missing)
        ns = _harness(recipient_rec=rec)
        result = _notice(ns)
        assert result["status"] == 409, missing
        assert result["body"]["error"] == "no_payout_address"
        assert not ns["_inserted"] and not ns["_emails"] and not ns["_pings"]


def test_tipping_yourself_is_recorded_without_an_error():
    ns = _harness(sender="bob")
    result = _notice(ns, toNodeName="bob")
    assert result["status"] == 200
    assert result["body"]["ok"] is True
    assert result["body"]["notified"] is True
    assert len(ns["_inserted"]) == 1
    assert ns["_inserted"][0][1:3] == ("bi:bob", "bi:bob")
    assert len(ns["_emails"]) == 1
    assert ns["_emails"][0]["to"] == "bob@example.com"
    assert len(ns["_pings"]) == 1
    assert ns["_pings"][0]["recipient"] == "bob"
    assert ns["_pings"][0]["actor"] == "bob"


# --- idempotency and ordering ----------------------------------------------

def test_a_replayed_signature_answers_ok_and_mails_nobody():
    ns = _harness(rows=[{"signature": SIGNATURE, "from_bi": "bi:alice",
                         "to_bi": "bi:bob", "lamports": 50000000,
                         "network": "mainnet", "ts": NOW, "emailed": 1}])
    result = _notice(ns)
    assert result["status"] == 200
    assert result["body"]["duplicate"] is True
    assert not ns["_inserted"] and not ns["_emails"] and not ns["_pings"]


def test_the_row_is_claimed_before_the_email_is_attempted():
    """A crash between the two must not leave a replay able to re-send."""
    order = []
    ns = _harness()
    original_run = ns["d1_run"]
    original_send = ns["_send_email"]

    async def run(env, sql, *args):
        order.append("insert" if "INSERT" in sql else "update")
        return await original_run(env, sql, *args)

    async def send(*args, **kwargs):
        order.append("email")
        return await original_send(*args, **kwargs)

    ns["d1_run"] = run
    ns["_send_email"] = send
    _notice(ns)
    assert order == ["insert", "email", "update"]


def test_a_failed_send_leaves_the_row_unflagged_but_still_pings():
    ns = _harness(sent=False)
    result = _notice(ns)
    assert result["status"] == 200
    assert result["body"]["emailed"] is False
    assert len(ns["_inserted"]) == 1
    assert not ns["_updated"]        # emailed stays 0
    assert not ns["_stamped"]        # nothing to stamp on the recipient
    assert len(ns["_pings"]) == 1    # the in-app ping is independent


# --- recipient preferences --------------------------------------------------

def test_an_unverified_address_gets_the_ping_but_no_email():
    ns = _harness(recipient_rec={"name": "bob", "email": "bob@example.com",
                                 "email_verified": False, "status": "active",
                                 "solana": PAYOUT_ADDRESS})
    result = _notice(ns)
    assert result["status"] == 200 and result["body"]["emailed"] is False
    assert not ns["_emails"]
    assert len(ns["_pings"]) == 1
    assert len(ns["_inserted"]) == 1


def test_a_global_email_optout_is_honored():
    ns = _harness(recipient_rec={"name": "bob", "email": "bob@example.com",
                                 "email_verified": True, "status": "active",
                                 "solana": PAYOUT_ADDRESS,
                                 "email_notifications": False})
    assert _notice(ns)["body"]["emailed"] is False
    assert not ns["_emails"]


def test_the_tip_kind_can_be_switched_off_on_its_own():
    ns = _harness(recipient_rec={
        "name": "bob", "email": "bob@example.com", "email_verified": True,
        "status": "active", "solana": PAYOUT_ADDRESS,
        "notification_preferences": {"tip_sent": False}})
    assert _notice(ns)["body"]["emailed"] is False
    assert not ns["_emails"]
    assert len(ns["_pings"]) == 1


# --- rate limiting ----------------------------------------------------------

def test_a_sender_is_capped_per_rolling_day():
    limit = _harness()["TIP_NOTICE_DAILY_LIMIT"]
    rows = [{"signature": "s%d" % index, "from_bi": "bi:alice",
             "to_bi": "bi:bob", "lamports": 1, "network": "mainnet",
             "ts": NOW - 1000, "emailed": 1} for index in range(limit)]
    ns = _harness(rows=rows)
    result = _notice(ns)
    assert result["status"] == 429
    assert not ns["_emails"] and len(ns["_inserted"]) == 0


def test_notices_older_than_the_window_do_not_count():
    limit = _harness()["TIP_NOTICE_DAILY_LIMIT"]
    rows = [{"signature": "s%d" % index, "from_bi": "bi:alice",
             "to_bi": "bi:bob", "lamports": 1, "network": "mainnet",
             "ts": NOW - DAY - 1000, "emailed": 1} for index in range(limit)]
    ns = _harness(rows=rows)
    assert _notice(ns)["status"] == 200


def test_another_senders_notices_do_not_consume_your_quota():
    limit = _harness()["TIP_NOTICE_DAILY_LIMIT"]
    rows = [{"signature": "s%d" % index, "from_bi": "bi:mallory",
             "to_bi": "bi:bob", "lamports": 1, "network": "mainnet",
             "ts": NOW - 1000, "emailed": 1} for index in range(limit)]
    ns = _harness(rows=rows)
    assert _notice(ns)["status"] == 200


# --- non-custodial boundary -------------------------------------------------

def test_the_endpoint_reads_no_key_shaped_field():
    """Nothing the desktop signs with may be accepted, even by accident."""
    source = ast.get_source_segment(
        TEXT, _tip_notice_node()) or ""
    for forbidden in ("privateKey", "private_key", "secretKey", "secret_key",
                      "seed", "keypair", "passphrase", "mnemonic",
                      "signedTransaction", "sendTransaction"):
        assert forbidden not in source, forbidden


def test_the_notice_endpoint_never_broadcasts_a_transaction():
    ns = _harness()
    _notice(ns, signedTransaction="AQAB", privateKey="hunter2")
    # The extra fields are simply not read; the recorded row is unchanged.
    assert ns["_inserted"][0][:5] == (SIGNATURE, "bi:alice", "bi:bob",
                                      50000000, "mainnet")


def _tip_notice_node():
    for node in ast.parse(TEXT, filename=str(ENTRY)).body:
        if (isinstance(node, ast.AsyncFunctionDef)
                and node.name == "_account_tip_notice"):
            return node
    raise AssertionError("_account_tip_notice not found")


# --- deployment wiring ------------------------------------------------------

def test_the_route_is_registered():
    assert ('if url.path == "/api/accounts/tip-notice" and method == "POST":'
            in TEXT)
    assert "return await _account_tip_notice(env, request)" in TEXT


def test_the_notification_kind_is_registered_for_inbox_and_email():
    ns = _harness()
    assert "tip_sent" in ns["NOTIFICATION_EMAIL_KINDS"]
    assert ns["NOTIFICATION_EMAIL_DEFAULTS"]["tip_sent"] is True
    assert '    "tip_sent",\n' in TEXT  # NOTIFICATION_KINDS membership


def test_the_table_exists_in_both_schema_paths():
    assert "CREATE TABLE IF NOT EXISTS tip_notice" in SCHEMA_TEXT
    assert "idx_tip_notice_from_ts" in SCHEMA_TEXT
    assert MIGRATION.is_file()
    migration = MIGRATION.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS tip_notice" in migration
    assert "signature TEXT PRIMARY KEY" in migration
    # The runtime DDL and the native migration must agree, or whichever runs
    # first silently wins.
    for constraint in ("CHECK (lamports > 0)",
                       "CHECK (emailed IN (0,1))",
                       "CHECK (length(signature) BETWEEN 64 AND 88)"):
        assert constraint in migration, constraint
        assert constraint in SCHEMA_TEXT, constraint


def test_the_worker_solana_module_stays_read_only():
    """The relay must not gain a signing path just because tips exist."""
    solana = (ROOT / "src" / "solana.py").read_text(encoding="utf-8")
    assert "_SOLANA_READ_ONLY_METHODS" in solana
    assert "sendTransaction" not in TEXT.split(
        "async def _account_tip_notice")[1].split("\nasync def ")[0]


# --- a landed pull's estimate still at pending_merge -------------------------

def _merged_pull_reward():
    reward = _reward(status="pending_merge")
    reward.update({
        "source_kind": "pull",
        "condition": "merged",
        "subject_number": 14,
    })
    return reward


def _pull_record_reader(monkeypatch, content):
    reads = []

    async def gateway(_env, owner, repo, action):
        reads.append((owner, repo, action))
        return {"ok": True, "content": content} if content else None

    monkeypatch.setattr(ledger_module, "_forkbot_repo_gateway_json", gateway)
    monkeypatch.setattr(
        ledger_module, "_forkbot_blob_text",
        lambda blob: (blob or {}).get("content", ""))
    return reads


def test_reward_payment_accepts_a_verified_merge_the_queue_never_synced(
        monkeypatch):
    ns = _harness(reward=_merged_pull_reward())
    reads = _pull_record_reader(monkeypatch, (
        "---\nnumber: 14\nstatus: merged\n"
        "rewardId: reward-forkmesh-forkmesh-issue-635\n---\n"))

    result = _reward_notice(ns)

    assert result["status"] == 200
    assert result["body"]["rewardStatus"] == "paid"
    assert reads and reads[0][2].endswith("&ref=forkmesh%2Fpull%2F14")
    promote = ns["_reward_writes"][0][0]
    assert "SET status='pending_wallet'" in promote
    assert "status='pending_merge'" in promote
    assert "status='paid'" in ns["_reward_writes"][1][0]


def test_reward_payment_still_refuses_a_pull_that_has_not_merged(monkeypatch):
    for content in ("---\nnumber: 14\nstatus: open\n"
                    "rewardId: reward-forkmesh-forkmesh-issue-635\n---\n",
                    "---\nnumber: 14\nstatus: merged\n---\n",
                    ""):
        ns = _harness(reward=_merged_pull_reward())
        _pull_record_reader(monkeypatch, content)

        result = _reward_notice(ns)

        assert result == {
            "status": 409,
            "body": {"error": "reward_not_payable",
                     "rewardStatus": "pending_merge"},
        }
        assert not ns["_chain_plans"]
        assert not ns["_reward_writes"]
