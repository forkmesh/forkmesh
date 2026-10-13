import asyncio
import importlib.util
import types
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[1] / "src" / "smtp_mail.py"


def load():
    spec = importlib.util.spec_from_file_location("smtp_mail_under_test", SRC)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_headers_cannot_inject():
    smtp = load()
    message = smtp.build("no-reply@forkmesh.com", "Fork\nMesh", "a@example.com",
                         "Hi\r\nBcc: x@example.net", "body", "<b>x</b>", "tip")
    head = message.as_string().split("\n\n", 1)[0]
    assert "\nBcc:" not in head
    assert message["Message-ID"].endswith("@forkmesh.com>")


def test_enabled_only_for_smtp():
    smtp = load()
    assert smtp.enabled(types.SimpleNamespace(MAIL_TRANSPORT="smtp"))
    assert not smtp.enabled(types.SimpleNamespace())


def test_refuses_non_loopback_relay():
    smtp = load()
    env = types.SimpleNamespace(MAIL_SMTP_HOST="mail.example.net")
    with pytest.raises(ValueError):
        asyncio.run(smtp.send(env, smtp.build(
            "no-reply@forkmesh.com", "ForkMesh", "a@example.com", "s", "t")))


def test_delivers_to_loopback(monkeypatch):
    smtp = load()
    sent = []

    class Client:
        def __init__(self, host, port, timeout):
            sent.append((host, port))

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def send_message(self, message):
            sent.append(message["To"])
            return {}

    monkeypatch.setattr(smtp.smtplib, "SMTP", Client)
    message_id = asyncio.run(smtp.send(types.SimpleNamespace(), smtp.build(
        "no-reply@forkmesh.com", "ForkMesh", "a@example.com", "s", "t")))
    assert sent == [("127.0.0.1", 25), "a@example.com"]
    assert message_id.startswith("<")


def test_marks_automated_mail_and_tags_feedback_id():
    smtp = load()
    message = smtp.build("no-reply@forkmesh.com", "ForkMesh", "a@gmail.com",
                         "s", "t", "<p>x</p>", "password_reset")
    assert message["Auto-Submitted"] == "auto-generated"
    assert message["Feedback-ID"] == "password_reset:forkmesh"
    assert smtp.build("no-reply@forkmesh.com", "ForkMesh", "a@gmail.com", "s",
                      "t", kind="a:b c\n")["Feedback-ID"] == "abc:forkmesh"


def test_mime_version_only_on_the_top_level():
    smtp = load()
    message = smtp.build("no-reply@forkmesh.com", "ForkMesh", "a@gmail.com",
                         "s", "t", "<p>x</p>", "verification")
    assert message.as_string().count("MIME-Version:") == 1
    assert [p.get_content_type() for p in message.iter_parts()] == [
        "text/plain", "text/html"]
