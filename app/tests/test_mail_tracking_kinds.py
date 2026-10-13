import ast
import asyncio
from pathlib import Path

import pytest

import mail_tracking

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"


def _send_email_smtp():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    node = next(n for n in tree.body if isinstance(n, ast.AsyncFunctionDef)
                and n.name == "_send_email_smtp")
    built, tracked = [], []

    async def tracked_html(_env, _send_id, html):
        tracked.append(html)
        return html + "<img src=pixel>", "https://app.example/u/tok"

    async def anything(*_args, **_kwargs):
        return ""

    class SmtpMail:
        @staticmethod
        def build(*args, **kwargs):
            built.append((args, kwargs))
            return args

        @staticmethod
        async def send(_env, _message):
            return "<msg@forkmesh.com>"

    class Date:
        @staticmethod
        def now():
            return 1

    namespace = {
        "clean_string": lambda v, n=240: str(v or "")[:n],
        "_ap_uuid": lambda: "send-1", "blind_index": anything,
        "_mail_suppressed": anything, "_mail_budget_text": anything,
        "_mail_tracked_html": tracked_html, "mail_tracking": mail_tracking,
        "_safe_error_text": str, "smtp_mail": SmtpMail,
        "_mailtrap_account_for_email": anything, "d1_run": anything,
        "prune_mail_records": anything, "_record_mailtrap_send": anything,
        "_record_email_send": anything,
        "Date": Date,
    }
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    return namespace["_send_email_smtp"], built, tracked


def _send(kind):
    send, built, tracked = _send_email_smtp()

    async def log(*_args, **_kwargs):
        return None

    assert asyncio.run(send(None, "no-reply@forkmesh.com", "ForkMesh",
                            "ada@gmail.com", "Hi", "text",
                            '<a href="https://forkmesh.com/x">x</a>', kind, log))
    (args, kwargs), = built
    return args[5], kwargs["headers"], tracked


@pytest.mark.parametrize("kind", ["verification", "password_reset",
                                  "password_change", "account", "admin_test",
                                  "status_alert", "security_report"])
def test_account_and_security_mail_is_not_tracked(kind):
    html, headers, tracked = _send(kind)
    assert tracked == []
    assert html == '<a href="https://forkmesh.com/x">x</a>'
    assert headers == {}


@pytest.mark.parametrize("kind", mail_tracking.UNSUBSCRIBE_KINDS)
def test_notification_mail_is_tracked_with_one_click_unsubscribe(kind):
    html, headers, tracked = _send(kind)
    assert len(tracked) == 1 and html.endswith("<img src=pixel>")
    assert headers == {"List-Unsubscribe": "<https://app.example/u/tok>",
                       "List-Unsubscribe-Post": "List-Unsubscribe=One-Click"}


def test_tracking_pixel_is_not_hidden_with_display_none():
    html = mail_tracking.add_pixel("<body>x</body>", "https://a/o.gif")
    assert "display:none" not in html and 'width="1"' in html


def test_tracking_tokens_accept_the_send_ids_the_app_issues():
    send_id = "0123456789abcdef0123456789abcdef"
    signature = "a" * 24
    assert mail_tracking.parse_token(send_id + "." + signature) == (
        send_id, signature)
    legacy = "01234567-89ab-cdef-0123-456789abcdef"
    assert mail_tracking.parse_token(legacy + "." + signature) == (
        legacy, signature)
    assert mail_tracking.parse_token(send_id[:-1] + "." + signature) == (
        None, None)
