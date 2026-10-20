#!/usr/bin/env python3
"""Once-only "How are we doing?" founder feedback email (24h after signup).

Contract under test, extracted straight from entry.py:
- only ACTIVE, USER-kind accounts with a VERIFIED email that are at least 24h
  old get the email; nodes, unverified and fresh signups are skipped;
- the feedback_email_sends row is claimed BEFORE the send (crash can never
  double-send) and released again when the send definitively fails;
- accounts with an existing row are excluded by the SQL join, so the email
  goes out exactly once per account;
- it is sent from founders@forkmesh.com so a plain reply reaches the founders.
"""

import ast
import asyncio
import importlib.util
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
_MAIL_BUDGET_SPEC = importlib.util.spec_from_file_location(
    "mail_budget_for_feedback", ENTRY.parent / "mail_budget.py")
MAIL_BUDGET = importlib.util.module_from_spec(_MAIL_BUDGET_SPEC)
_MAIL_BUDGET_SPEC.loader.exec_module(MAIL_BUDGET)

# clean_string lives in catalog.py (imported by entry.py), so it is stubbed
# in the harness rather than AST-extracted.
FUNCS = {"_send_feedback_emails", "_feedback_email_content", "_account_kind",
         "valid_node_name", "_stamp_account_email"}

CONSTANTS = [
    node for node in ast.parse(ENTRY_TEXT).body
    if isinstance(node, ast.Assign)
    and any(getattr(t, "id", "").startswith("FEEDBACK_EMAIL")
            # _stamp_account_email bounds the per-account send history.
            or getattr(t, "id", "") == "ACCOUNT_EMAIL_HISTORY"
            for t in node.targets)
]


def _load(extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = list(CONSTANTS) + [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {getattr(n, "name", "") for n in selected}
    assert FUNCS <= found | {""}
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


DAY = 24 * 60 * 60 * 1000
NOW = 2_000_000_000_000


def _harness(accounts, send_results=None):
    """accounts: list of dicts with name_bi + decrypted record fields."""
    sends = []          # (to, subject, from_email, from_name)
    send_results = list(send_results or [])
    table = {}          # account_bi -> (name, sent_at)
    saved = {}          # account_bi -> stamped record

    class _DateStub:
        @staticmethod
        def now():
            return NOW

    async def d1_all(_env, sql, *args):
        assert "LEFT JOIN feedback_email_sends" in sql
        return [{"name_bi": a["name_bi"], "data": a}
                for a in accounts if a["name_bi"] not in table]

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT OR IGNORE INTO feedback_email_sends"):
            bi, name, ts = args
            table.setdefault(bi, (name, ts))
            return
        if sql.startswith("DELETE FROM feedback_email_sends"):
            table.pop(args[0], None)
            return
        raise AssertionError("unexpected d1_run: " + sql)

    async def decrypt_row(_env, stored, key=None):
        return dict(stored) if isinstance(stored, dict) else None

    async def _send_email(_env, to_email, subject, text, html=None,
                          from_email=None, from_name=None, **_kwargs):
        sends.append((to_email, subject, from_email, from_name))
        return send_results.pop(0) if send_results else True

    async def _save_account(_env, account_bi, rec):
        saved[account_bi] = dict(rec)

    def _html_escape(value):
        return (str(value).replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;"))

    def _forkmesh_email_card_html(heading, intro, body, footer=""):
        return "<html>%s|%s|%s|%s</html>" % (heading, intro, body, footer)

    ns = _load({
        "instance_branded": lambda _env: False,
        "Date": _DateStub,
        "mail_budget": MAIL_BUDGET,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": decrypt_row,
        "_send_email": _send_email,
        "_save_account": _save_account,
        "_html_escape": _html_escape,
        "_forkmesh_email_card_html": _forkmesh_email_card_html,
        "MAX_NODE_NAME": 63,
        "NODE_NAME_RE": __import__("re").compile(
            r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"),
        "clean_string": lambda value, max_length=240: str(value or "")[:max_length],
    })
    ns["_sends"] = sends
    ns["_table"] = table
    ns["_saved"] = saved
    return ns


def _user(bi, name, age_ms=2 * DAY, verified=True, kind="user",
          status="active", email="u@gmail.com"):
    return {"name_bi": bi, "name": name, "kind": kind, "status": status,
            "email": email, "email_verified": verified,
            "created_at": NOW - age_ms,
            "pass_hash": "h" if kind == "user" else ""}


def test_sends_once_to_eligible_users_only():
    accounts = [
        _user("bi:alice", "alice"),                       # eligible
        _user("bi:bob", "bob", age_ms=DAY // 2),          # too fresh
        _user("bi:carol", "carol", verified=False),       # unverified email
        _user("bi:node1", "node1", kind="node"),          # node account
        _user("bi:eve", "eve", email=""),                 # no email at all
        _user("bi:mallory", "mallory", status="banned"),  # not active
    ]
    ns = _harness(accounts)
    sent = asyncio.run(ns["_send_feedback_emails"](object()))
    assert sent == 1
    assert [s[0] for s in ns["_sends"]] == ["u@gmail.com"]
    to, subject, from_email, from_name = ns["_sends"][0]
    assert from_email == "founders@forkmesh.com"
    assert "alice" in subject
    assert set(ns["_table"]) == {"bi:alice"}

    # Second run: the send-log row excludes alice; nothing further goes out.
    # The account screen's "last emailed" stamp records the send and outcome.
    stamp = ns["_saved"]["bi:alice"]
    assert stamp["last_email_kind"] == "feedback"
    assert stamp["last_email_ok"] is True
    assert stamp["last_email_ts"] == NOW

    sent_again = asyncio.run(ns["_send_feedback_emails"](object()))
    assert sent_again == 0
    assert len(ns["_sends"]) == 1


def test_failed_send_releases_the_claim_for_retry():
    ns = _harness([_user("bi:alice", "alice")], send_results=[False, True])
    assert asyncio.run(ns["_send_feedback_emails"](object())) == 0
    assert ns["_table"] == {}          # claim released on failure
    assert ns["_saved"]["bi:alice"]["last_email_ok"] is False
    assert asyncio.run(ns["_send_feedback_emails"](object())) == 1
    assert set(ns["_table"]) == {"bi:alice"}
    assert len(ns["_sends"]) == 2      # retried exactly once


def test_undeliverable_recipient_leaves_the_queue_without_a_send():
    accounts = [
        _user("bi:fmnext", "fmnext", email="admin@next.forkmesh.com"),
        _user("bi:alice", "alice"),
    ]
    ns = _harness(accounts)
    assert asyncio.run(ns["_send_feedback_emails"](object())) == 1
    assert [s[0] for s in ns["_sends"]] == ["u@gmail.com"]
    assert set(ns["_table"]) == {"bi:fmnext", "bi:alice"}
    assert "bi:fmnext" not in ns["_saved"]
    assert asyncio.run(ns["_send_feedback_emails"](object())) == 0
    assert len(ns["_sends"]) == 1


def test_undeliverable_recipients_do_not_use_the_batch():
    accounts = [
        _user("bi:x%d" % i, "x%d" % i, email="a%d@next.forkmesh.com" % i)
        for i in range(10)
    ] + [_user("bi:alice", "alice")]
    ns = _harness(accounts)
    assert asyncio.run(ns["_send_feedback_emails"](object())) == 1
    assert len(ns["_table"]) == 11


def test_content_is_reply_geared_and_from_founders():
    ns = _harness([])
    subject, text, html = ns["_feedback_email_content"]("jett")
    assert "jett" in subject
    for fragment in ("hit reply", "founders", "1.", "2.", "3."):
        assert fragment in text
    assert "@jett" in html
    assert "https://forkmesh.com/dashboard?feedback=1" in text
    assert "https://forkmesh.com/dashboard?feedback=1" in html
    assert "lobby" not in text + html
    assert ns["FEEDBACK_EMAIL_FROM"] == "founders@forkmesh.com"
    assert ns["FEEDBACK_EMAIL_DELAY_MS"] == DAY


def test_schema_and_migration_carry_the_send_log():
    schema = (ENTRY.parent / "schema.py").read_text(encoding="utf-8")
    assert "feedback_email_sends" in schema
    migration = ENTRY.parent.parent / "migrations" / "0033_feedback_email_sends.sql"
    assert "feedback_email_sends" in migration.read_text(encoding="utf-8")


if __name__ == "__main__":
    test_sends_once_to_eligible_users_only()
    test_failed_send_releases_the_claim_for_retry()
    test_content_is_reply_geared_and_from_founders()
    test_schema_and_migration_carry_the_send_log()
    print("ok")
