#!/usr/bin/env python3
"""/admin/hackathon: the growth dashboard and administrator controls."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

import pytest

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, Request, harness, policy, run,
)


@pytest.fixture
def admin(harness):
    module = harness.module
    state = {"actor": "", "origin_ok": True}

    async def check(_env, _request):
        return bool(state["actor"]) and state["actor"] in harness.admins

    async def cookie_name(_env, _request):
        return state["actor"]

    async def request_text(request, _limit=None):
        return await request.text()

    shell_spec = importlib.util.spec_from_file_location(
        "admin_shell_under_test",
        Path(__file__).resolve().parents[1] / "src" / "admin_shell.py")
    shell = importlib.util.module_from_spec(shell_spec)
    shell_spec.loader.exec_module(shell)

    async def no_rows(_env, _sql, *_args):
        return None

    shell._bind_runtime({"d1_first": no_rows,
                         "Date": SimpleNamespace(now=lambda: NOW)})
    module._admin_shell = shell
    module._check_admin_page_auth = check
    module._account_cookie_name = cookie_name
    module._request_origin_allowed = lambda _env, _request: state["origin_ok"]
    module.bounded_request_text = request_text
    harness.account("root")
    harness.admins.add("root")
    harness.admin_state = state
    return harness


def _get(harness, query=""):
    return run(harness.module.admin_page_handler(
        harness.env, Request("https://app.forkmesh.com/admin/hackathon" + query)))


def _post(harness, fields):
    from urllib.parse import urlencode
    return run(harness.module.admin_page_handler(harness.env, Request(
        "https://app.forkmesh.com/admin/hackathon", "POST",
        {"content-type": "application/x-www-form-urlencoded"},
        urlencode(fields))))


def _redirect_params(response):
    assert response.status == 303, response.status
    return parse_qs(urlparse(response.headers["location"]).query)


def test_signed_out_visitors_are_sent_to_log_in(admin):
    response = _get(admin)
    assert response.status == 302
    assert response.headers["location"].startswith("/login?next=")
    admin.admin_state["actor"] = "mallory"
    admin.account("mallory")
    assert _get(admin).status == 302
    assert _post(admin, {"form": "configure", "slug": "x-2026"}).status == 403


def test_a_cross_origin_form_is_refused(admin):
    admin.admin_state["actor"] = "root"
    admin.admin_state["origin_ok"] = False
    assert _post(admin, {"form": "configure", "slug": "x-2026"}).status == 403


def test_saving_a_campaign_validates_before_it_writes(admin):
    admin.admin_state["actor"] = "root"
    import json
    config = policy.default_config()
    ok = _redirect_params(_post(admin, {
        "form": "configure", "slug": "spring-2027", "title": "Spring",
        "status": "draft", "startsAt": "2027-03-01T09:00",
        "config": json.dumps(config)}))
    assert ok["notice"] == ["saved"]
    row = admin.db.connection.execute(
        "SELECT status,starts_at FROM hackathons WHERE slug='spring-2027'"
    ).fetchone()
    assert row["status"] == "draft"
    from datetime import datetime, timezone
    assert row["starts_at"] == int(datetime(
        2027, 3, 1, 9, 0, tzinfo=timezone.utc).timestamp() * 1000)
    config["missions"][0]["reward"] = {"kind": "cash", "amount": 500}
    refused = _redirect_params(_post(admin, {
        "form": "configure", "slug": "spring-2027",
        "config": json.dumps(config)}))
    assert refused["error"][0].startswith("invalid_config")
    broken = _redirect_params(_post(admin, {
        "form": "configure", "slug": "spring-2027", "config": "{not json"}))
    assert broken["error"] == ["invalid_config"]


def test_the_dashboard_shows_the_funnel_and_escapes_names(admin):
    slug = admin.campaign(status="live")
    admin.admin_state["actor"] = "root"
    module = admin.module
    admin.account("alice")
    run(module._create_participant(admin.env, slug, "bi:alice", "alice"))
    for name in ("bob", "cy"):
        admin.account(name)
        run(module._create_participant(
            admin.env, slug, "bi:" + name, name, referrer_bi="bi:alice",
            referrer_name="alice"))
    connection = admin.db.connection
    for index in range(6):
        connection.execute(
            "INSERT INTO hackathon_share_events (hackathon_slug,account_bi,"
            "channel,surface,minute_bucket,created_at) VALUES (?,?,?,?,?,?)",
            (slug, "bi:alice", "x" if index % 2 else "copy", "hq", index, NOW))
    connection.commit()
    report = run(module.growth_report(admin.env, run(
        module._campaign(admin.env, slug))))
    funnel = report["funnel"]
    # 3 participants, 6 presses, 2 referred joins.
    assert funnel["invitesPerParticipant"] == 2.0
    assert funnel["kFactor"] == round(2 / 3, 4)
    page = _get(admin, "?slug=" + slug)
    assert page.status == 200
    assert "default-src 'none'" in page.headers["content-security-policy"]
    assert "noindex" in page.body
    assert "Viral funnel" in page.body and "0.67" in page.body
    assert "<script" not in page.body
    assert '<a class="ab-item on" href="/admin/hackathon"' in page.body
    # Tabs hold the other areas; an unknown tab falls back to Growth.
    assert "Save campaign" not in page.body
    assert "Save campaign" in _get(admin, "?slug=%s&tab=campaign" % slug).body
    assert "Viral funnel" in _get(admin, "?slug=%s&tab=nope" % slug).body


def test_judges_and_results_are_managed_from_the_page(admin):
    slug = admin.campaign(status="live")
    admin.admin_state["actor"] = "root"
    admin.account("judy")
    added = _redirect_params(_post(admin, {
        "form": "add_judge", "slug": slug, "name": "judy"}))
    assert added["notice"] == ["judge_added"]
    assert added["tab"] == ["judging"]  # back to the tab the form was on
    assert admin.db.count("hackathon_judges") == 1
    page = _get(admin, "?slug=" + slug + "&tab=judging")
    assert '<a href="/admin/hackathon?slug=%s&amp;tab=judging" aria-current="page">' % slug in page.body
    assert "judy" in page.body
    assert "Finalize results</button> <span" in page.body  # disabled while live
    early = _redirect_params(_post(admin, {"form": "finalize", "slug": slug}))
    assert early["error"] == ["not_judging"]
    admin.db.connection.execute("UPDATE hackathons SET status='judging'")
    admin.db.connection.commit()
    done = _redirect_params(_post(admin, {"form": "finalize", "slug": slug}))
    assert done["notice"] == ["finalized"]
    assert admin.db.scalar("SELECT status FROM hackathons") == "closed"
