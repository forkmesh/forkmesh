#!/usr/bin/env python3
"""Challenge links: "Alex has 1,240 points. Can you beat that?"

Runs the real runtime against the in-memory D1 from test_hackathon. The
claims that matter: the target never moves once sent, a tie beats nobody, a
beat is announced once, beating pays no points, and accepting a challenge is
not a referral.
"""

import re
from pathlib import Path

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, body_of, harness, policy, run,
)

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
HQ_JS = (ROOT / "public" / "dashboard" / "js" / "09-hackathon.js").read_text(
    encoding="utf-8")
URLS = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
WWW_WORKER = (REPO / "www" / "worker.js").read_text(encoding="utf-8")
WWW_WRANGLER = (REPO / "www" / "wrangler.toml").read_text(encoding="utf-8")


def _joined(harness, name, points=0):
    module, slug = harness.module, "mesh-2026"
    harness.account(name)
    run(module._create_participant(harness.env, slug, "bi:" + name, name))
    if points:
        harness.db.connection.execute(
            "UPDATE hackathon_participants SET points=? WHERE account_bi=?",
            (points, "bi:" + name))
        harness.db.connection.commit()
    return "bi:" + name


def _send(harness, name):
    harness.login(name)
    response = run(harness.module.challenges_handler(
        harness.env, harness.request("/api/hackathon/challenges", "POST", {})))
    assert response.status == 201, body_of(response)
    return body_of(response)["sent"][0]


def _accept(harness, name, token):
    harness.login(name)
    return run(harness.module.challenge_accept_handler(
        harness.env,
        harness.request("/api/hackathon/challenges/%s/accept" % token,
                        "POST", {}),
        token))


def test_a_challenge_fixes_the_target_when_it_is_sent(harness):
    harness.campaign()
    _joined(harness, "alex", points=1240)
    sent = _send(harness, "alex")
    assert sent["target"] == 1240 and sent["open"]
    assert sent["link"] == (
        "https://forkmesh.com/hackathon/challenge/" + sent["token"])
    assert sent["expiresAt"] == NOW + 72 * policy.HOUR_MS
    # Alex keeps building; the challenge already sent does not move.
    harness.db.connection.execute(
        "UPDATE hackathon_participants SET points=5000 WHERE name='alex'")
    listed = body_of(run(harness.module.challenges_handler(
        harness.env, harness.request("/api/hackathon/challenges"))))
    assert listed["sent"][0]["target"] == 1240


def test_sending_needs_a_participant_and_is_capped(harness):
    harness.campaign()
    harness.account("stranger")
    harness.login("stranger")
    response = run(harness.module.challenges_handler(
        harness.env, harness.request("/api/hackathon/challenges", "POST", {})))
    assert response.status == 409
    _joined(harness, "alex", points=10)
    for _ in range(policy.MAX_OPEN_CHALLENGES):
        _send(harness, "alex")
    harness.login("alex")
    capped = run(harness.module.challenges_handler(
        harness.env, harness.request("/api/hackathon/challenges", "POST", {})))
    assert capped.status == 429


def test_accepting_joins_without_making_it_a_referral(harness):
    harness.campaign()
    _joined(harness, "alex", points=1240)
    token = _send(harness, "alex")["token"]
    harness.account("sam")
    response = _accept(harness, "sam", token)
    assert response.status == 201
    sam = harness.db.connection.execute(
        "SELECT referrer_bi,squad_id FROM hackathon_participants "
        "WHERE name='sam'").fetchone()
    assert sam["referrer_bi"] == "" and sam["squad_id"] == ""
    alex = harness.db.connection.execute(
        "SELECT referrals_joined,points FROM hackathon_participants "
        "WHERE name='alex'").fetchone()
    assert alex["referrals_joined"] == 0 and alex["points"] == 1240
    # Accepting twice is one entry.
    assert _accept(harness, "sam", token).status == 200
    assert harness.db.count("hackathon_challenge_entries") == 1
    assert harness.db.scalar(
        "SELECT accepted_count FROM hackathon_challenges") == 1


def test_own_expired_and_full_challenges_are_refused(harness):
    harness.campaign()
    _joined(harness, "alex", points=100)
    token = _send(harness, "alex")["token"]
    assert _accept(harness, "alex", token).status == 409
    harness.db.connection.execute(
        "UPDATE hackathon_challenges SET accepted_count=?",
        (policy.MAX_CHALLENGE_ENTRIES,))
    harness.account("late")
    assert body_of(_accept(harness, "late", token))["error"] == "challenge_full"
    harness.clock["now"] = NOW + 73 * policy.HOUR_MS
    harness.account("later")
    assert _accept(harness, "later", token).status == 410


def test_passing_the_target_is_announced_once_and_a_tie_beats_nobody(harness):
    module, slug = harness.module, harness.campaign()
    _joined(harness, "alex", points=300)
    token = _send(harness, "alex")["token"]
    sam = _joined(harness, "sam")
    _accept(harness, "sam", token)
    # "joined" is worth 50; a first mirror 300 more lands sam on 350 > 300.
    harness.db.connection.execute(
        "UPDATE hackathon_participants SET points=250 WHERE name='sam'")
    run(module._award_points(harness.env, slug, sam, "verified"))  # 400
    beaten = harness.db.scalar(
        "SELECT beaten_at FROM hackathon_challenge_entries WHERE account_bi=?",
        sam)
    assert beaten == NOW
    notes = [n for n in harness.notifications if "beat" in n["title"]]
    assert {n["to"] for n in notes} == {"sam", "alex"}
    run(module._award_points(harness.env, slug, sam, "first_mirror"))
    assert len([n for n in harness.notifications
                if "beat" in n["title"]]) == 2
    assert harness.db.scalar("SELECT beaten_count FROM hackathon_challenges") == 1

    # A tie does not beat a challenge.
    token2 = _send(harness, "alex")["token"]
    target = harness.db.scalar(
        "SELECT target_points FROM hackathon_challenges WHERE token=?", token2)
    tia = _joined(harness, "tia")
    _accept(harness, "tia", token2)
    harness.db.connection.execute(
        "UPDATE hackathon_participants SET points=? WHERE name='tia'",
        (target - policy.point_value("verified"),))
    run(module._award_points(harness.env, slug, tia, "verified"))
    assert harness.db.scalar(
        "SELECT beaten_at FROM hackathon_challenge_entries WHERE account_bi=?",
        tia) == 0


def test_beating_a_challenge_pays_no_points(harness):
    module, slug = harness.module, harness.campaign()
    _joined(harness, "alex", points=10)
    token = _send(harness, "alex")["token"]
    sam = _joined(harness, "sam", points=0)
    _accept(harness, "sam", token)
    before = harness.db.count("hackathon_points", "account_bi=?", sam)
    run(module._award_points(harness.env, slug, sam, "verified"))
    after = harness.db.count("hackathon_points", "account_bi=?", sam)
    assert after == before + 1  # only the verified points themselves
    assert harness.db.scalar(
        "SELECT beaten_at FROM hackathon_challenge_entries") == NOW


def test_someone_already_ahead_beats_the_challenge_on_arrival(harness):
    harness.campaign()
    _joined(harness, "alex", points=100)
    token = _send(harness, "alex")["token"]
    _joined(harness, "pro", points=900)
    body = body_of(_accept(harness, "pro", token))
    assert body["challenge"]["you"]["beaten"] is True


def test_the_challenge_page_is_escaped_noindexed_and_accepts_on_the_hq(harness):
    harness.campaign()
    _joined(harness, "alex", points=1240)
    token = _send(harness, "alex")["token"]
    page = run(harness.module.challenge_page_handler(
        harness.env, harness.request("/hackathon/challenge/" + token), token))
    assert page.status == 200
    html = page.body
    assert '<meta name="robots" content="noindex">' in html
    assert "/hackathon/me?accept=" + token in html
    assert 'href="/r/alex"' in html
    assert '<h1><a href="/@alex">alex</a> challenged you.</h1>' in html
    assert "pays no points" in html
    assert "1,240" in html
    assert run(harness.module.challenge_page_handler(
        harness.env, harness.request("/hackathon/challenge/x"), "x" * 20)) is None


def test_a_draft_campaign_has_no_challenge_pages(harness):
    harness.campaign(status="draft")
    assert run(harness.module.challenge_page_handler(
        harness.env, harness.request("/hackathon/challenge/t"), "t" * 20)) is None


def test_challenge_pages_are_routed_and_proxied_like_invites():
    assert "HACKATHON_CHALLENGE_RE" in URLS
    pattern = re.search(r"HACKATHON_APP_DOCUMENTS = (/.*?/);", WWW_WORKER)
    assert pattern and "challenge" in pattern.group(1)
    assert '"/hackathon/challenge/*",' in WWW_WRANGLER


def test_the_hq_consumes_the_accept_token_and_keeps_it_through_login():
    assert 'url.searchParams.delete("accept")' in HQ_JS
    assert "history.replaceState" in HQ_JS
    assert "/login?next=${next}" in HQ_JS
    assert "location.search" in HQ_JS
