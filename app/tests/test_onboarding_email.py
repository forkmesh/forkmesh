import ast
import html
import re
from pathlib import Path

import pytest

import onboarding_email as oe

SRC = Path(__file__).resolve().parents[1] / "src"
DOC = Path(__file__).resolve().parents[2] / "docs" / "app" / "onboarding-email.md"
UNSUB = "https://app.forkmesh.com/api/onboarding-email/unsubscribe?a=ada&s=" + "0" * 32
FIELDS = ("step", "subject", "preheader", "heading", "intro_html", "text",
          "body_html", "footer_html", "action_label", "action_url")


def _card():
    tree = ast.parse((SRC / "entry.py").read_text(encoding="utf-8"))
    wanted = {"_light_email_fragment", "_forkmesh_email_action_html",
              "_forkmesh_email_card_html"}
    nodes = [n for n in tree.body
             if isinstance(n, ast.FunctionDef) and n.name in wanted]
    assert len(nodes) == 3
    namespace = {"_html_escape": html.escape,
                 "FORKMESH_SITE_URL": "https://forkmesh.com/"}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "entry.py", "exec"),
         namespace)
    return namespace["_forkmesh_email_card_html"]


def _render(msg):
    return _card()(msg["heading"], msg["intro_html"], msg["body_html"],
                   msg["footer_html"], msg["action_label"], msg["action_url"],
                   preheader=msg["preheader"])


@pytest.mark.parametrize("step", oe.STEPS)
def test_every_step_has_subject_preheader_text_and_html(step):
    msg = oe.message(step, "ada", repo="ada/notes", unsubscribe=UNSUB)
    for field in FIELDS:
        assert msg[field], field
    assert msg["step"] == step
    assert len(msg["subject"]) <= 60
    assert 20 <= len(msg["preheader"]) <= 90
    assert msg["text"].startswith("Hi ada,")
    assert oe.FEEDBACK in msg["text"]
    assert msg["action_url"].startswith("https://")
    rendered = _render(msg)
    assert rendered.startswith("<!doctype html>")
    assert rendered.index(html.escape(msg["preheader"])) < rendered.index("<h1")
    assert "#4ade80" not in rendered


@pytest.mark.parametrize("step", oe.STEPS)
def test_every_step_carries_an_unsubscribe_and_settings_link(step):
    msg = oe.message(step, "ada", repo="ada/notes", unsubscribe=UNSUB)
    assert "Stop getting-started emails: " + UNSUB in msg["text"]
    assert "Email settings: " + oe.SETTINGS_URL in msg["text"]
    assert html.escape(UNSUB, quote=True) in msg["footer_html"]
    assert oe.SETTINGS_URL in msg["footer_html"]


def test_first_repo_names_the_repository_and_its_clone_url():
    msg = oe.message("first_repo", "ada", repo="ada/notes")
    assert msg["heading"] == "ada/notes is on ForkMesh"
    assert "git clone https://forkmesh.com/ada/notes" in msg["text"]
    assert "git clone https://forkmesh.com/ada/notes" in msg["body_html"]
    assert oe.INSTALL in msg["text"]


def test_first_repo_without_a_valid_repository_drops_the_clone_line():
    msg = oe.message("first_repo", "ada", repo="<script>/x y")
    assert "git clone" not in msg["text"]
    assert "<script>" not in msg["body_html"] + msg["heading"]
    assert msg["heading"] == "Your repository is on ForkMesh"


def test_day_3_points_at_a_repository_or_a_mirror():
    with_repo = oe.message("day_3", "ada", has_repo=True)
    without = oe.message("day_3", "ada", has_repo=False)
    assert with_repo["action_url"] == oe.DESKTOP_URL
    assert without["action_url"] == oe.NEW_REPO_URL
    assert oe.PROMISE_URL in with_repo["text"]
    assert "take the first host offline" in with_repo["text"].lower()


def test_names_are_escaped_in_html():
    msg = oe.message("day_7", "<b>x</b>")
    assert "<b>x</b>" not in msg["intro_html"]
    assert "&lt;b&gt;x&lt;/b&gt;" in msg["intro_html"]
    assert oe.message("day_7", "   ")["text"].startswith("Hi there,")


def test_without_an_unsubscribe_url_only_settings_are_linked():
    msg = oe.message("day_7", "ada")
    assert "Stop getting-started" not in msg["text"]
    assert "Stop getting-started" not in msg["footer_html"]


def test_unknown_step_is_refused():
    with pytest.raises(ValueError):
        oe.message("day_30", "ada")


def test_links_are_public_forkmesh_urls():
    for step in oe.STEPS:
        msg = oe.message(step, "ada", repo="ada/notes")
        for url in re.findall(r'href="([^"]+)"', msg["body_html"]):
            assert url.startswith(("https://forkmesh.com/",
                                   "https://app.forkmesh.com/", "mailto:"))


def test_the_page_documents_every_step_and_that_sending_is_off():
    doc = DOC.read_text(encoding="utf-8")
    for step in oe.STEPS:
        assert "`" + step + "`" in doc
    assert "not wired" in doc


def test_card_without_preheader_is_unchanged():
    card = _card()
    assert "display:none" not in card("h", "i", "b")
