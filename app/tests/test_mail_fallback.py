import ast
import asyncio
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
sys.path.insert(0, str(ROOT / "src"))

import instance_features  # noqa: E402
import mail_providers  # noqa: E402


class _Budget:
    @staticmethod
    def deliverable(address, env=None):
        return True

    @staticmethod
    def is_marketing_kind(kind):
        return False


class _Env:
    MAIL_TRANSPORT = "smtp"
    MAILTRAP_API_TOKEN = "token"


class _Response:
    status = 200
    headers = {}

    async def text(self):
        return json.dumps({"success": True, "message_ids": ["mt-1"]})


def _send(smtp_result, env=None, held=False, budget=True):
    calls = {"fetch": [], "notes": []}

    async def send_email_smtp(*args, **kwargs):
        return smtp_result

    async def fetch(url, init, timeout):
        calls["fetch"].append((url, json.loads(init["body"])))
        return _Response()

    async def note(_env, kind, to, subject, result, **extra):
        calls["notes"].append((result, extra.get("via"), extra.get("error", "")))
        return True

    async def nothing(*args, **kwargs):
        return None

    async def flag(value):
        return value

    class Smtp:
        SENDER = "founders@forkmesh.com"

        @staticmethod
        def enabled(env):
            return True

        @staticmethod
        def node():
            return "n4"

    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [node for node in tree.body if isinstance(node, ast.AsyncFunctionDef)
                and node.name == "_send_email"]
    namespace = {
        "mail_providers": mail_providers, "instance_features": instance_features,
        "instance_name": lambda env: "ForkMesh",
        "_instance_brand_email": lambda env, text, html: (text, html),
        "smtp_mail": Smtp, "mail_budget": _Budget,
        "_send_email_smtp": send_email_smtp, "_mail_log_note": note,
        "_mail_provider_held": lambda env: flag(held),
        "_mail_budget_spend": lambda env, kind: flag(budget),
        "_mail_budget_text": lambda env, kind: flag("1 of 150"),
        "_mail_provider_hold": nothing, "_record_email_send": nothing,
        "_record_mailtrap_send": nothing,
        "_mailtrap_account_for_email": lambda env, to: flag("acct"),
        "_mailtrap_email_category": lambda kind: "Password reset",
        "_forkmesh_email_card_html": lambda *a: "<p>card</p>",
        "_forkmesh_email_action_html": lambda *a: "",
        "_light_email_fragment": lambda html: html, "_html_escape": str,
        "_ap_uuid": lambda: "send-1", "_safe_error_text": str,
        "js_fetch_with_timeout": fetch, "CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS": 10,
        "FORKMESH_SITE_URL": "https://forkmesh.com/",
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    result = asyncio.run(namespace["_send_email"](
        env or _Env(), "someone@yahoo.com", "Reset", "text",
        email_kind="password_reset"))
    return result, calls


def test_a_failed_local_postfix_falls_back_to_the_mailtrap_api():
    sent, calls = _send(None)
    assert sent is True
    assert [url for url, _ in calls["fetch"]] == [mail_providers.MAILTRAP_URL]
    body = calls["fetch"][0][1]
    assert body["to"] == [{"email": "someone@yahoo.com"}]
    assert body["from"]["email"] == "founders@forkmesh.com"
    assert calls["notes"] == [("accepted", "mailtrap", "")]


@pytest.mark.parametrize("smtp_result", [True, False])
def test_postfix_results_and_suppressions_never_fall_back(smtp_result):
    sent, calls = _send(smtp_result)
    assert sent is smtp_result
    assert calls["fetch"] == []


def test_no_fallback_without_a_mailtrap_token():
    class Env(_Env):
        MAILTRAP_API_TOKEN = ""
    sent, calls = _send(None, env=Env())
    assert sent is False and calls["fetch"] == []


@pytest.mark.parametrize("held,budget", [(True, True), (False, False)])
def test_the_fallback_respects_the_provider_hold_and_daily_budget(held, budget):
    sent, calls = _send(None, held=held, budget=budget)
    assert sent is False and calls["fetch"] == []
    assert calls["notes"] == [("suppressed", "mailtrap", "fallback mail provider limit")]
