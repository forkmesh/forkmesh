import ast
import asyncio
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
sys.path.insert(0, str(ROOT / "src"))

import instance_features  # noqa: E402
import mail_providers  # noqa: E402


class _Budget:
    @staticmethod
    def deliverable(address, env=None):
        return not str(address).endswith("@next.forkmesh.com")


def _load(extra):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef)
                and node.name == "_send_email"]
    assert selected
    namespace = {
        "mail_providers": mail_providers,
        "instance_name": lambda _env: "ForkMesh",
        "instance_features": instance_features,
        "_instance_brand_email": lambda _env, text, html: (text, html),
    }
    namespace.update(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace["_send_email"]


def _run(env, to_email, smtp=True, details=None):
    notes = []

    async def note(_env, kind, recipient, subject, result, status=0,
                   error="", message_id="", **extra):
        notes.append((kind, recipient, subject, result, error))
        if details is not None:
            details.append(extra)
        return True

    async def budget_text(_env, kind):
        return "3 of 150 sent today (UTC)"

    class Smtp:
        @staticmethod
        def enabled(_env):
            return smtp

        @staticmethod
        def node():
            return "n1"

    send = _load({"smtp_mail": Smtp, "mail_budget": _Budget,
                  "_mail_budget_text": budget_text,
                  "_mail_log_note": note, "clean_string": lambda v, n: str(v)[:n]})
    result = asyncio.run(send(env, to_email, "Welcome", "hi",
                              email_kind="feedback"))
    return result, notes


class _Env:
    MAILTRAP_API_TOKEN = ""


def test_a_recipient_that_receives_no_mail_is_still_logged():
    result, notes = _run(_Env(), "jett@next.forkmesh.com")
    assert result is False
    assert notes == [("feedback", "jett@next.forkmesh.com", "Welcome",
                      "suppressed", "recipient domain receives no mail")]


def test_a_skipped_send_logs_sender_transport_body_and_budget():
    details = []
    _run(_Env(), "jett@next.forkmesh.com", details=details)
    assert details == [{
        "sender": "no-reply@forkmesh.com", "via": "postfix", "node": "n1",
        "body": "hi", "body_html": None,
        "budget": "3 of 150 sent today (UTC)"}]


def test_a_send_with_no_transport_is_still_logged():
    result, notes = _run(_Env(), "jett@gmail.com", smtp=False)
    assert result is False
    assert notes == [("feedback", "jett@gmail.com", "Welcome", "failed",
                      "no mail transport configured")]


def test_no_recipient_logs_nothing():
    result, notes = _run(_Env(), "")
    assert result is False and notes == []


def test_local_mail_sends_skip_the_daily_budget_and_the_mailtrap_hold():
    sent = []

    async def limited(*_args):
        raise AssertionError("the budget is only for Mailtrap")

    async def smtp_send(_env, sender, name, to_email, *rest, **_kwargs):
        sent.append(to_email)
        return True

    class Smtp:
        @staticmethod
        def enabled(_env):
            return True

        @staticmethod
        def node():
            return "n1"

    send = _load({"smtp_mail": Smtp, "mail_budget": _Budget,
                  "_mail_budget_spend": limited,
                  "_mail_provider_held": limited,
                  "_send_email_smtp": smtp_send,
                  "FORKMESH_SITE_URL": "https://forkmesh.com",
                  "MAILTRAP_SEND_URL": "",
                  "_forkmesh_email_card_html": lambda *parts: "<p></p>",
                  "_html_escape": str, "_light_email_fragment": str,
                  "clean_string": lambda v, n: str(v)[:n]})
    assert asyncio.run(send(_Env(), "jo@example.com", "Welcome", "hi",
                            email_kind="feedback")) is True
    assert sent == ["jo@example.com"]
