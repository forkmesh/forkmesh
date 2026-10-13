"""Mailtrap lifecycle webhook and public status projection contracts."""

import ast
import json
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
)
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
SCENE = world_scene_source()
ENV_EXAMPLE = (ROOT / ".env.production.example").read_text(encoding="utf-8")


def _helper(name):
    tree = ast.parse(ENTRY)
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef) and item.name == name
    )
    namespace = {"json": json}
    exec(compile(ast.Module(body=[node], type_ignores=[]), "<helper>", "exec"),
         namespace)
    return namespace[name]


def test_webhook_parser_accepts_json_envelopes_arrays_and_jsonl():
    parse = _helper("_mailtrap_webhook_payload")
    event = {"event_id": "evt-1", "event": "delivery"}
    assert parse(json.dumps(event)) == [event]
    assert parse(json.dumps({"events": [event]})) == [event]
    assert parse(json.dumps([event, event])) == [event, event]
    assert parse(json.dumps(event) + "\n" + json.dumps(event)) == [event, event]
    assert parse("{broken") is None


def test_webhook_authenticates_raw_body_and_is_idempotent():
    assert 'request.headers.get("mailtrap-signature")' in ENTRY
    assert "hmac.compare_digest(presented, expected)" in ENTRY
    assert "MAILTRAP_WEBHOOK_EVENTS" in ENTRY
    assert "len(events) > 500" in ENTRY
    assert "mailtrap_webhook_events WHERE event_id=?" in ENTRY
    assert "DELETE FROM mailtrap_webhook_events WHERE received_at<?" in ENTRY
    assert 'is_open = 1 if status == "open" else 0' in ENTRY
    assert 'is_click = 1 if status == "click" else 0' in ENTRY
    assert "open_count=open_count+?" in ENTRY
    assert "click_count=click_count+?" in ENTRY
    assert "clicked_at=CASE WHEN ?=1" in ENTRY


def test_plugin_hooks_share_the_authenticated_webhook_boundary():
    """Plugins receive Mailtrap events only through the deduplicated,
    authenticated webhook loop — never from a separate unauthenticated path."""
    webhook = ENTRY.split("async def mailtrap_webhook_handler", 1)[1].split(
        "\nasync def", 1
    )[0]
    assert "process_mail_event(env, event, now)" in webhook
    shared = ENTRY.split("async def process_mail_event", 1)[1].split(
        "\nasync def", 1
    )[0]
    dispatch = shared.split("plugin_data.deliver_mailtrap_event", 1)
    assert len(dispatch) == 2, "the webhook must hand events to plugin hooks"
    assert "mailtrap_webhook_events" in dispatch[0], (
        "plugin hooks must run after webhook dedup, inside the same handler")

def test_plugin_webhook_hooks_tolerate_an_uninstalled_plugin_schema():
    """Uninstalling a plugin everywhere drops its tables; late webhooks for
    already-exported sends must not fail the whole authenticated batch."""
    engine = (ROOT / "src" / "plugin_data.py").read_text(encoding="utf-8")
    dispatch = engine.split("async def deliver_mailtrap_event", 1)[1].split(
        "\nasync def", 1
    )[0]
    assert "try:" in dispatch
    assert '"no such table" not in str(error).lower()' in dispatch
    assert "raise" in dispatch


def test_sends_have_stable_mailtrap_categories():
    assert '"category": _mailtrap_email_category(email_kind)' in ENTRY
    for category in (
        "Welcome verification", "Password reset", "Onboarding feedback",
        "Billing membership", "Notification digest", "Operational status alert",
    ):
        assert category in ENTRY
    assert 'email_kind="verification"' in ENTRY
    assert 'email_kind="password_reset"' in ENTRY


def test_mailtrap_engagement_events_never_verify_accounts_or_referrals():
    webhook = ENTRY.split("async def mailtrap_webhook_handler", 1)[1].split(
        "\nasync def", 1
    )[0]
    assert "email_verified" not in webhook
    assert "_finalize_referral_signup" not in webhook
    assert "optional telemetry only" in ENV_EXAMPLE
    assert "one-use confirmation POST" in ENV_EXAMPLE


def test_email_status_contains_no_recipient_or_content_fields():
    assert "CREATE TABLE IF NOT EXISTS mailtrap_email_sends" in SCHEMA
    table = SCHEMA.split(
        "CREATE TABLE IF NOT EXISTS mailtrap_email_sends", 1)[1].split(
        '""",', 1)[0]
    for forbidden in ("email", "subject", "body", "recipient"):
        assert forbidden not in table.lower()
    for lifecycle in ("opened_at", "clicked_at", "open_count", "click_count"):
        assert lifecycle in table
    assert "click_url" not in ENTRY
    assert "clicked_url" not in ENTRY
    assert '"lastEmailAt"' in ENTRY
    assert '"lastEmailStatus"' in ENTRY
    assert "badgeEmailLabel(identity)" in SCENE


def test_operator_template_documents_callback_events_and_secret():
    assert "MAILTRAP_WEBHOOK_SECRET=CHANGE-ME" in ENV_EXAMPLE
    assert "https://app.forkmesh.com/api/integrations/mailtrap/webhook" in ENV_EXAMPLE
    for event in (
        "delivery", "open", "click", "unsubscribe", "spam",
        "soft bounce", "bounce", "suspension", "reject",
    ):
        assert event in ENTRY
