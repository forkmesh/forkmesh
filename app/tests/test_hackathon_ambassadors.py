#!/usr/bin/env python3
"""The ambassador street team: apply, review, badge, and an honest funnel."""

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, body_of, harness, run,
)


def _participant(harness, name, verified=True):
    harness.account(name)
    run(harness.module._create_participant(
        harness.env, "mesh-2026", "bi:" + name, name))
    if verified:
        harness.db.connection.execute(
            "UPDATE hackathon_participants SET verified_at=? WHERE name=?",
            (NOW, name))
        harness.db.connection.commit()


def _apply(harness, name, pitch="I run the university open-source club."):
    harness.login(name)
    response = run(harness.module.ambassadors_handler(
        harness.env, harness.request("/api/hackathon/ambassadors", "POST",
                                     {"pitch": pitch})))
    return response, body_of(response)


def _review(harness, admin, name, approve=True):
    harness.login(admin)
    return body_of(run(harness.module.admin_handler(
        harness.env, harness.request("/api/admin/hackathon", "POST", {
            "slug": "mesh-2026", "name": name,
            "action": "approve_ambassador" if approve
            else "reject_ambassador"}))))


def test_applying_needs_a_verified_participant_and_a_real_pitch(harness):
    harness.campaign(status="live")
    _participant(harness, "new", verified=False)
    _response, body = _apply(harness, "new")
    assert body["error"] == "verify_email_first"
    _participant(harness, "ana")
    _response, short = _apply(harness, "ana", pitch="hi")
    assert short["error"] == "pitch_too_short"
    response, applied = _apply(harness, "ana")
    assert response.status == 201
    assert applied["ambassador"]["status"] == "pending"


def test_only_an_administrator_approves_and_approval_awards_the_badge(harness):
    harness.campaign(status="live")
    _participant(harness, "ana")
    _apply(harness, "ana")
    harness.account("mallory")
    denied = _review(harness, "mallory", "ana")
    assert denied["error"] == "admin_required"
    harness.account("admin")
    harness.admins.add("admin")
    approved = _review(harness, "admin", "ana")
    assert approved["status"] == "approved"
    assert ("ana", "bi:ana", "hackathon_ambassador", "hackathon") in (
        harness.badges)
    assert any(n["to"] == "ana" and "ambassador" in n["title"]
               for n in harness.notifications)
    _response, again = _apply(harness, "ana")
    assert again["error"] == "already_an_ambassador"


def test_a_rejected_applicant_can_apply_again(harness):
    harness.campaign(status="live")
    _participant(harness, "ana")
    _apply(harness, "ana")
    harness.account("admin")
    harness.admins.add("admin")
    _review(harness, "admin", "ana", approve=False)
    response, body = _apply(harness, "ana", pitch="Now I also run a meetup group.")
    assert response.status == 201 and body["ambassador"]["status"] == "pending"


def test_an_approved_ambassador_sees_their_own_funnel(harness):
    slug = harness.campaign(status="live")
    _participant(harness, "ana")
    _apply(harness, "ana")
    harness.account("admin")
    harness.admins.add("admin")
    _review(harness, "admin", "ana")
    connection = harness.db.connection
    for index, decision in enumerate(("accepted", "accepted", "duplicate",
                                      "accepted")):
        connection.execute(
            "INSERT INTO referral_click_audit (visit_bi,referrer_bi,decision,"
            "data,first_seen_at,last_seen_at) VALUES (?,?,?,?,?,?)",
            ("v%d" % index, "bi:ana", decision, "{}", NOW, NOW))
    # One click from before the campaign opened does not count.
    connection.execute(
        "INSERT INTO referral_click_audit (visit_bi,referrer_bi,decision,data,"
        "first_seen_at,last_seen_at) VALUES ('old','bi:ana','accepted','{}',?,?)",
        (NOW - 10, NOW - 10))
    connection.commit()
    harness.account("friend")
    run(harness.module._create_participant(
        harness.env, slug, "bi:friend", "friend", referrer_bi="bi:ana",
        referrer_name="ana"))
    harness.login("ana")
    payload = body_of(run(harness.module.me_handler(
        harness.env, harness.request("/api/hackathon/me"))))
    stats = payload["ambassador"]["stats"]
    assert (stats["clicks"], stats["joins"], stats["qualified"]) == (3, 1, 0)
    assert stats["joinRate"] == round(1 / 3, 4)
