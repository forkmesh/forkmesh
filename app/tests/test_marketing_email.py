import ast
import asyncio
import json
from pathlib import Path

import pytest

import mail_budget
import mail_providers
import mail_tracking
import marketing_email as me
import marketing_email_runtime as runtime

SRC = Path(__file__).resolve().parents[1] / "src"
PUBLIC = Path(__file__).resolve().parents[1] / "public"


def _function(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    node = next(n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name == name)
    return ast.Module(body=[node], type_ignores=[])


def test_consent_is_opt_in_and_records_when_and_where():
    rec = {}
    assert me.since(rec) == 0
    assert me.apply_consent(rec, True, "signup", 1000) is True
    assert rec["marketing_email"] == {"at": 1000, "source": "signup"}
    assert me.apply_consent(rec, True, "settings", 2000) is False
    assert me.since(rec) == 1000
    assert me.apply_consent(rec, False, "", 0) is True
    assert "marketing_email" not in rec
    assert me.apply_consent(rec, False, "", 0) is False
    me.apply_consent(rec, True, "anything", 5)
    assert rec["marketing_email"]["source"] == "settings"


@pytest.mark.parametrize("change,expected", [
    ({}, True),
    ({"marketing_email": None}, False),
    ({"email_verified": False}, False),
    ({"email": ""}, False),
    ({"status": "disabled"}, False),
    ({"kind": "node"}, False),
])
def test_only_verified_active_opted_in_users_are_subscribed(change, expected):
    rec = {"name": "ada", "email": "ada@example.org", "email_verified": True,
           "status": "active", "kind": "user",
           "marketing_email": {"at": 1, "source": "settings"}}
    rec.update(change)
    if rec.get("marketing_email") is None:
        rec.pop("marketing_email")
    assert me.subscribed(rec) is expected


def test_campaign_data_validates_and_normalizes():
    data, error = me.campaign_data({"subject": "  Hello \n world ",
                                    "body": "Line one\r\n\r\nLine two"})
    assert error == ""
    assert data["subject"] == "Hello world"
    assert data["kind"] == "update"
    assert data["body"] == "Line one\n\nLine two"
    assert data["linkUrl"] == "" and data["linkLabel"] == ""
    assert me.campaign_data({"body": "x"})[1] == "subject_required"
    assert me.campaign_data({"subject": "x"})[1] == "body_required"
    assert me.campaign_data({"subject": "x", "body": "y",
                             "kind": "spam"})[1] == "invalid_kind"
    assert me.campaign_data({"subject": "x", "body": "y",
                             "linkUrl": "http://a.example"})[1] == \
        "link_must_be_https"
    data, error = me.campaign_data({"subject": "x", "body": "y",
                                    "kind": "promotion",
                                    "linkUrl": "https://forkmesh.com/blog"})
    assert error == "" and data["linkLabel"] == "Read more"
    edited, error = me.campaign_data({"subject": "New"}, data)
    assert error == "" and edited["body"] == "y" and edited["kind"] == "promotion"


def test_every_email_carries_unsubscribe_settings_and_postal_address():
    campaign = {"kind": "promotion", "subject": "Offer",
                "body": "First <b>para</b>\n\nSecond",
                "linkUrl": "https://forkmesh.com/x", "linkLabel": "See it"}
    unsubscribe = me.unsubscribe_url("https://app.example", "ada", "a" * 32)
    assert unsubscribe == ("https://app.example/api/marketing-email/"
                           "unsubscribe?a=ada&s=" + "a" * 32)
    settings = "https://app.example" + me.SETTINGS_PATH
    text = me.text_body(campaign, settings, unsubscribe, "1 Main St")
    assert text.startswith("First <b>para</b>\n\nSecond\n\nSee it: "
                           "https://forkmesh.com/x\n\n--\n")
    assert "Unsubscribe: " + unsubscribe in text
    assert "Email settings: " + settings in text
    assert text.rstrip().endswith("ForkMesh, 1 Main St")
    intro, body, footer = me.html_parts(campaign, settings, unsubscribe,
                                        "1 Main St")
    assert intro == "Promotion from ForkMesh"
    assert "&lt;b&gt;para&lt;/b&gt;" in body and "<b>" not in body
    assert body.count("<p ") == 2
    assert unsubscribe.replace("&", "&amp;") in footer
    assert "ForkMesh, 1 Main St" in footer


def test_unsubscribe_link_shape_is_checked_before_any_lookup():
    assert me.valid_unsubscribe("ada", "0" * 32)
    assert not me.valid_unsubscribe("Ada", "0" * 32)
    assert not me.valid_unsubscribe("ada", "0" * 31)
    assert not me.valid_unsubscribe("../x", "0" * 32)


def test_marketing_mail_uses_the_mailtrap_bulk_stream():
    class Env:
        MAILTRAP_API_TOKEN = "token"

    message = {"sender": "a@forkmesh.com", "senderName": "ForkMesh",
               "to": "b@example.org", "subject": "s", "text": "t",
               "category": "Product updates and offers", "bulk": True}
    url, _, body = mail_providers.build_request(Env(), "mailtrap", message)
    assert url == mail_providers.MAILTRAP_BULK_URL
    assert "bulk" not in json.loads(body)
    message["bulk"] = False
    assert mail_providers.build_request(
        Env(), "mailtrap", message)[0] == mail_providers.MAILTRAP_URL


def test_marketing_has_its_own_daily_cap_and_unsubscribe_header():
    class Env:
        MAIL_MARKETING_DAILY_LIMIT = "2"

    buckets = {}

    async def execute(sql, *args):
        if sql.startswith("INSERT OR IGNORE"):
            created = args[0] not in buckets
            buckets.setdefault(args[0], 0)
            return 1 if created else 0
        if sql.startswith("UPDATE"):
            bucket, cap = args[1], args[2]
            if buckets[bucket] < cap:
                buckets[bucket] += 1
                return 1
            return 0
        return 0

    results = [asyncio.run(mail_budget.spend_marketing(execute, Env(), 0))
               for _ in range(3)]
    assert results == [True, True, False]
    assert mail_budget.is_marketing_kind("marketing")
    assert not mail_budget.is_account_kind("marketing")
    assert "marketing" in mail_tracking.UNSUBSCRIBE_KINDS


@pytest.mark.parametrize("reason,kind,expected", [
    ("unsubscribe", "marketing", "unsubscribe"),
    ("marketing_unsubscribe", "marketing", "marketing_unsubscribe"),
    ("marketing_unsubscribe", "notifications", ""),
    ("marketing_unsubscribe", "verification", ""),
    ("unsubscribe", "notifications", "unsubscribe"),
    ("unsubscribe", "verification", ""),
    ("bounce", "verification", "bounce"),
])
def test_marketing_unsubscribe_never_stops_other_mail(reason, kind, expected):
    async def d1_first(_env, _sql, _bi):
        return {"reason": reason}

    namespace = {"d1_first": d1_first, "mail_budget": mail_budget,
                 "mail_tracking": mail_tracking}
    exec(compile(_function(SRC / "entry.py", "_mail_suppressed"),
                 "entry.py", "exec"), namespace)
    assert asyncio.run(namespace["_mail_suppressed"](None, "bi", kind)) == \
        expected


class _Fake:
    def __init__(self, users, campaign_cursor=0):
        self.users = users
        self.campaign = {"campaign_id": "c1", "data": {"kind": "update",
                         "subject": "Hello", "body": "News"},
                         "cursor_rowid": campaign_cursor}
        self.claims = set()
        self.sent = []
        self.updates = []
        self.saved = []
        self.budget_left = 100

    def bind(self):
        fake = self

        class Date:
            @staticmethod
            def now():
                return 5000

        async def d1_first(_env, sql, *args):
            if "status='sending'" in sql:
                return dict(fake.campaign)
            return None

        async def d1_all(_env, sql, cursor, limit):
            rows = [{"rid": rid, "user_bi": "bi-" + rec["name"], "data": rec}
                    for rid, rec in fake.users if rid > cursor]
            return rows[:limit]

        async def d1_run(_env, sql, *args):
            if sql.startswith("DELETE FROM marketing_email_sends"):
                fake.claims.discard((args[0], args[1]))
                return
            fake.updates.append(args)

        async def decrypt_row(_env, data):
            return dict(data) if isinstance(data, dict) else None

        async def budget_execute(_env, sql, *args):
            if "marketing_email_sends" in sql:
                key = (args[0], args[1])
                if key in fake.claims:
                    return 0
                fake.claims.add(key)
                return 1
            if sql.startswith("UPDATE mail_budget"):
                if fake.budget_left <= 0:
                    return 0
                fake.budget_left -= 1
                return 1
            return 1

        async def send_email(_env, to, subject, text, html, email_kind):
            fake.sent.append((to, subject, email_kind, text))
            return True

        async def rec_by_bi(_env, bi):
            return {"name": bi[3:]}

        async def save_account(_env, bi, rec):
            fake.saved.append((bi, rec.get("last_email_kind")))

        async def nothing(*_args, **_kwargs):
            return False

        async def blind_index(_env, value):
            return "f" * 64

        def stamp(rec, kind, ok, now=None):
            rec["last_email_kind"] = kind
            return rec

        def card(heading, intro, body, footer_html="", action_label="",
                 action_url=""):
            return ("<html><body><div><div>" + heading + intro + body
                    + action_label + "</div></div></body></html>")

        for name, value in {
            "Date": Date, "d1_first": d1_first, "d1_all": d1_all,
            "d1_run": d1_run, "decrypt_row": decrypt_row,
            "_mail_budget_execute": budget_execute, "_send_email": send_email,
            "_account_identity_rec_by_bi": rec_by_bi,
            "_save_account": save_account, "_mail_provider_held": nothing,
            "ensure_schema": nothing, "_is_main_relay": lambda env: True,
            "blind_index": blind_index, "_stamp_account_email": stamp,
            "_account_kind": lambda rec: rec.get("kind") or "user",
            "clean_string": lambda value, limit=240: str(value or "")[:limit],
            "_forkmesh_email_card_html": card,
            "_html_escape": lambda value: str(value),
            "FORKMESH_SITE_URL": "https://forkmesh.com/",
            "mail_budget": mail_budget,
        }.items():
            setattr(runtime, name, value)
        return self


class _Env:
    MARKETING_EMAIL_POSTAL = "1 Main St, Springfield"
    API_ORIGIN = "https://app.example"


def _user(name, opted=True, **extra):
    rec = {"name": name, "email": name + "@mailbox.org",
           "email_verified": True, "status": "active", "kind": "user"}
    if opted:
        rec["marketing_email"] = {"at": 1, "source": "settings"}
    rec.update(extra)
    return rec


def test_cron_sends_a_small_batch_to_subscribers_only_and_moves_the_cursor():
    fake = _Fake([(1, _user("ada")), (2, _user("bob", opted=False)),
                  (3, _user("cy")), (4, _user("dee"))]).bind()
    result = asyncio.run(runtime.send_due(_Env()))
    assert [to for to, *_ in fake.sent] == ["ada@mailbox.org",
                                            "cy@mailbox.org"]
    assert all(kind == "marketing" for _, _, kind, _ in fake.sent)
    assert "api/marketing-email/unsubscribe?a=ada" in fake.sent[0][3]
    assert "1 Main St, Springfield" in fake.sent[0][3]
    assert result == {"campaign": "c1", "sent": 2, "failed": 0,
                      "skipped": 0, "held": ""}
    assert fake.updates[-1][0] == 3
    assert fake.saved == [("bi-ada", "marketing"), ("bi-cy", "marketing")]


def test_cron_finishes_the_campaign_after_the_last_account():
    fake = _Fake([(1, _user("ada")), (2, _user("bob", opted=False))]).bind()
    result = asyncio.run(runtime.send_due(_Env()))
    assert result["held"] == "done"
    assert fake.updates[-1][5] == 1


def test_cron_never_sends_twice_and_stops_at_the_daily_limit():
    fake = _Fake([(1, _user("ada")), (2, _user("cy"))]).bind()
    fake.claims.add(("c1", "bi-ada"))
    fake.budget_left = 1
    result = asyncio.run(runtime.send_due(_Env()))
    assert [to for to, *_ in fake.sent] == ["cy@mailbox.org"]
    fake.sent.clear()
    fake.campaign["cursor_rowid"] = 0
    fake.claims.clear()
    fake.budget_left = 0
    result = asyncio.run(runtime.send_due(_Env()))
    assert fake.sent == [] and result["held"] == "daily_limit"
    assert fake.updates[-1][0] == 0
    assert fake.claims == set()


def test_cron_skips_addresses_that_receive_no_mail():
    fake = _Fake([(1, _user("ada", email="ada@example.org")),
                  (2, _user("cy"))]).bind()
    result = asyncio.run(runtime.send_due(_Env()))
    assert [to for to, *_ in fake.sent] == ["cy@mailbox.org"]
    assert result["skipped"] == 1 and result["held"] == "done"


def test_cron_holds_without_a_postal_address():
    fake = _Fake([(1, _user("ada"))]).bind()

    class Env:
        API_ORIGIN = "https://app.example"

    assert asyncio.run(runtime.send_due(Env())) == {
        "campaign": "c1", "held": "postal_address"}
    assert fake.sent == []


def test_signup_and_settings_offer_the_switch_off_by_default():
    signup = (PUBLIC / "signup.html").read_text(encoding="utf-8")
    assert '<input type="checkbox" id="updates-agree" />' in signup
    assert "marketingEmail: Boolean(updatesAgree && updatesAgree.checked)" in (
        PUBLIC / "signup.js").read_text(encoding="utf-8")
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert 'if data.get("marketingEmail") is True:' in entry
    settings = (PUBLIC / "dashboard" / "partials" / "views"
                / "settings.html").read_text(encoding="utf-8")
    assert ('<button data-marketing-email type="button" role="switch" '
            'aria-checked="false"') in settings

