"""All outbound mail has one contrast mode and a site action."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")


def test_shared_email_card_is_light_only_and_high_contrast():
    card = ENTRY[
        ENTRY.index("def _light_email_fragment"):
        ENTRY.index("def _format_email_ts")
    ]
    assert 'name=\\"color-scheme\\" content=\\"light\\"' in card
    assert "prefers-color-scheme:dark" not in card
    assert '"#f5f5f5", "#24292f"' in card
    assert '"#d4d4d8", "#24292f"' in card
    assert '"#0f0f11", "#f6f8fa"' in card


def test_shared_email_card_shows_the_forkmesh_logo():
    card = ENTRY[
        ENTRY.index("def _forkmesh_email_card_html"):
        ENTRY.index("def _format_email_ts")
    ]
    assert 'src=\\"https://app.forkmesh.com/assets/logo-96.png\\"' in card
    assert "width:32px;height:auto" in card
    assert "background:#18181b" not in card
    assert "background:#171717" in ENTRY[ENTRY.index("def _forkmesh_email_action_html"):]
    assert "data-forkmesh-email-footer" in card
    assert (ROOT.parent / "www" / "public" / "assets" / "logo-96.png").is_file()


def test_bimi_logo_is_square_tiny_ps_svg():
    import re
    import xml.etree.ElementTree as ET

    path = ROOT.parent / "www" / "public" / "assets" / "forkmesh-bimi.svg"
    text = path.read_text(encoding="utf-8")
    assert len(text.encode()) < 32 * 1024
    root = ET.fromstring(text)
    ns = "{http://www.w3.org/2000/svg}"
    assert root.tag == ns + "svg"
    assert root.get("version") == "1.2"
    assert root.get("baseProfile") == "tiny-ps"
    assert root.get("x") is None and root.get("y") is None
    _, _, width, height = root.get("viewBox").split()
    assert width == height
    assert root.find(ns + "title").text == "ForkMesh"
    tags = {el.tag for el in root.iter()}
    assert tags <= {ns + "svg", ns + "title", ns + "rect", ns + "path"}
    assert not re.search(r"href|<script|<image|<style|@import", text)


def test_sender_adds_html_and_site_action_even_for_plain_text_mail():
    sender = ENTRY[
        ENTRY.index("async def _send_email"):
        ENTRY.index("# --- Account email activity")
    ]
    assert "Open ForkMesh: " in sender
    assert "if not html:" in sender
    assert "_forkmesh_email_card_html(" in sender
    assert 'if "data-forkmesh-site-action" not in html:' in sender
    assert "_forkmesh_email_action_html(" in sender


def test_how_are_we_doing_links_to_dashboard_feedback():
    feedback = ENTRY[
        ENTRY.index("def _feedback_email_content"):
        ENTRY.index("async def _send_feedback_emails")
    ]
    assert '"https://forkmesh.com/dashboard?feedback=1"' in feedback
    assert "Send feedback</a>" in feedback
    assert "lobby" not in feedback

