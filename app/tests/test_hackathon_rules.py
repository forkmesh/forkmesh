#!/usr/bin/env python3
"""Pure rules for the hackathon's later mechanics.

Challenges, reward fulfilment, submissions, judging, Community Choice, the
growth funnel and ambassadors. Everything here is arithmetic over plain
dictionaries, so it runs under ordinary CPython with no Worker runtime.
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import hackathon as policy  # noqa: E402

NOW = 1_800_000_000_000
DAY = policy.DAY_MS


# --- rewards never double-pay ----------------------------------------------------

def test_missions_can_award_memberships_and_badges_but_never_points_or_cash():
    assert "points" not in policy.REWARD_KINDS
    assert "cash" not in policy.REWARD_KINDS
    for mission in policy.DEFAULT_MISSIONS:
        assert mission["reward"]["kind"] in policy.REWARD_KINDS, mission["key"]
    with pytest.raises(policy.ConfigError):
        policy.validate_config({"missions": [{
            "key": "double", "metric": "mirrors", "threshold": 1,
            "reward": {"kind": "points", "points": 300}}]})


def test_the_default_missions_describe_the_event_points_they_do_not_pay():
    missions = {m["key"]: m for m in policy.DEFAULT_MISSIONS}
    assert "300 points" in missions["first_mirror"]["description"]
    assert policy.point_value("first_mirror") == 300
    assert "100 points" in missions["invite_one"]["description"]
    assert policy.point_value("referral_joined") == 100
    # The submission prize is the published promise; the mission that tracks
    # the same entry pays nothing, or a valid project would earn twice.
    assert missions["submit_project"]["reward"] == {"kind": "none"}
    assert any(p["id"] == "submission" and p["proMonths"]
               for p in policy.default_prizes())


def test_membership_months_stack_from_the_later_of_now_and_the_current_expiry():
    assert policy.extend_expiry(0, NOW, 3) == NOW + 90 * DAY
    running = NOW + 10 * DAY
    assert policy.extend_expiry(running, NOW, 1) == running + 30 * DAY
    lapsed = NOW - 10 * DAY
    assert policy.extend_expiry(lapsed, NOW, 1) == NOW + 30 * DAY


def test_a_pro_prize_also_grants_supporter_so_the_world_perk_follows():
    assert policy.MEMBERSHIP_ROLES["pro_months"] == (
        "pro_member", "supporting_member")
    assert policy.PROMOTION_SCOPE_TYPE != "platform"


def test_a_reward_with_months_and_a_badge_is_two_rows_under_one_key():
    rows = policy.reward_grants(
        "mission:invite_ten",
        {"kind": "pro_months", "months": 6, "badge": "hackathon_connector"})
    assert rows == [
        {"reward_key": "mission:invite_ten", "kind": "pro_months",
         "months": 6, "badge": ""},
        {"reward_key": "mission:invite_ten", "kind": "badge",
         "months": 0, "badge": "hackathon_connector"},
    ]
    assert policy.reward_grants("x", {"kind": "none"}) == []


def test_squad_missions_are_keyed_by_the_squad_and_need_one():
    missions = policy.DEFAULT_MISSIONS
    counters = {"squad_size": 25}
    assert policy.completed_mission_grants(missions, counters) == []
    rows = policy.completed_mission_grants(missions, counters, squad_id="sq1")
    assert [r["reward_key"] for r in rows] == ["mission:squad_twentyfive:sq1"]
    assert policy.completed_mission_grants(
        missions, {"squad_size": 24}, squad_id="sq1") == []


# --- challenges --------------------------------------------------------------------

def test_challenge_window_comes_from_configuration_and_is_clamped():
    assert policy.challenge_ttl_ms({}) == 72 * policy.HOUR_MS
    assert policy.challenge_ttl_ms({"challengeHours": 24}) == 24 * policy.HOUR_MS
    assert policy.challenge_ttl_ms({"challengeHours": 99999}) == (
        policy.CHALLENGE_HOURS_MAX * policy.HOUR_MS)
    with pytest.raises(policy.ConfigError):
        policy.validate_config({"challengeHours": 0})


def test_a_tie_does_not_beat_a_challenge():
    assert policy.challenge_beaten(1241, 1240)
    assert not policy.challenge_beaten(1240, 1240)
    assert not policy.challenge_beaten(None, 0)


def test_challenge_state_reports_open_expired_full_and_the_viewer():
    row = {"token": "t" * 20, "challenger_name": "alex", "target_points": 1240,
           "accepted_count": policy.MAX_CHALLENGE_ENTRIES, "beaten_count": 3,
           "created_at": NOW - DAY, "expires_at": NOW + DAY}
    state = policy.challenge_state(row, NOW, entry={"accepted_at": NOW,
                                                      "beaten_at": 0})
    assert state["open"] and state["full"]
    assert state["target"] == 1240 and state["challenger"] == "alex"
    assert state["you"] == {"accepted": True, "beaten": False, "beatenAt": 0}
    assert not policy.challenge_state(row, NOW + 2 * DAY)["open"]
    assert "you" not in policy.challenge_state(row, NOW)


# --- submissions -------------------------------------------------------------------

def _config():
    return policy.default_config()


def test_submission_accepts_a_repository_url_or_owner_slash_name():
    for repo in ("alice/keepalive", "https://forkmesh.com/alice/keepalive",
                 "https://app.forkmesh.com/alice/keepalive/"):
        fields, error = policy.normalize_submission({
            "track": "preserve", "repo": repo, "title": "Keepalive",
        }, _config())
        assert error == "" and fields["repo_owner"] == "alice"
        assert fields["repo_name"] == "keepalive"


@pytest.mark.parametrize("data,error", [
    ({"track": "nope", "repo": "a/b", "title": "Good"}, "invalid_track"),
    ({"track": "agents", "repo": "a", "title": "Good"}, "invalid_repository"),
    ({"track": "agents", "repo": "a/b/c", "title": "Good"},
     "invalid_repository"),
    ({"track": "agents", "repo": "a/b", "title": "no"}, "invalid_title"),
    ({"track": "agents", "repo": "a/b", "title": "Good", "pitch": "x" * 281},
     "pitch_too_long"),
    ({"track": "agents", "repo": "a/b", "title": "Good",
      "demoUrl": "http://plain.example"}, "invalid_demo_url"),
    ({"track": "agents", "repo": "a/b", "title": "Good",
      "videoUrl": "javascript:alert(1)"}, "invalid_video_url"),
])
def test_submission_refuses_what_a_gallery_must_not_show(data, error):
    assert policy.normalize_submission(data, _config()) == (None, error)


def test_review_flags_mark_for_a_human_rather_than_reject():
    starts, ends = NOW, NOW + 21 * DAY
    assert policy.submission_review_flags(True, NOW + DAY, starts, ends) == []
    assert policy.submission_review_flags(False, 0, starts, ends) == [
        "repository_not_public"]
    early = starts - policy.HOSTING_GRACE_MS - 1
    assert policy.submission_review_flags(True, early, starts, ends) == [
        "hosted_before_campaign"]
    # Inside the grace week before opening is fine.
    assert policy.submission_review_flags(True, starts - DAY, starts, ends) == []
    assert policy.submission_status_for([]) == "valid"
    assert policy.submission_status_for(["x"]) == "needs_review"


# --- judging ------------------------------------------------------------------------

def test_scores_are_four_criteria_from_one_to_five():
    scores, error = policy.normalize_score(
        {"fit": 5, "working": 4, "portability": "3", "craft": 2})
    assert error == "" and scores["total"] == 14
    for bad in ({"fit": 6, "working": 4, "portability": 3, "craft": 2},
                {"fit": 5, "working": 4, "portability": 3},
                {"fit": "x", "working": 4, "portability": 3, "craft": 2}):
        assert policy.normalize_score(bad) == (None, "invalid_score")


def _submission(sid, track, total, count, submitted, status="valid",
                team="team"):
    return {"submission_id": sid, "track": track, "score_total": total,
            "score_count": count, "submitted_at": submitted,
            "status": status, "team_id": team}


def test_ranking_uses_average_then_judge_count_then_earliest_submission():
    rows = [
        _submission("a", "agents", 30, 2, 10),   # avg 15, 2 judges
        _submission("b", "agents", 45, 3, 20),   # avg 15, 3 judges -> wins tie
        _submission("c", "agents", 16, 1, 5),    # avg 16 -> first
        _submission("d", "agents", 0, 0, 1),     # unscored cannot place
        _submission("e", "agents", 20, 1, 1, status="needs_review"),
    ]
    assert [r["submission_id"] for r in policy.rank_track(rows)] == [
        "c", "b", "a"]
    earlier = [_submission("x", "t", 10, 1, 99), _submission("y", "t", 10, 1, 1)]
    assert policy.rank_track(earlier)[0]["submission_id"] == "y"


def test_placements_are_per_track():
    rows = [_submission("a", "agents", 20, 1, 1),
            _submission("b", "agents", 10, 1, 1),
            _submission("c", "agents", 5, 1, 1),
            _submission("d", "preserve", 12, 1, 1)]
    assert policy.placements(rows) == {"a": "first", "b": "second",
                                       "d": "first"}


def test_a_submission_milestone_only_ever_raises_the_grant():
    config = _config()
    assert policy.submission_grant_months(config, []) == 3
    boosted = [{"reward": '{"kind":"pro_months","months":6,'
                          '"scope":"submission"}'}]
    assert policy.submission_grant_months(config, boosted) == 6
    lower = [{"reward": {"kind": "pro_months", "months": 1,
                         "scope": "submission"}}]
    assert policy.submission_grant_months(config, lower) == 3


def test_the_prize_plan_pays_memberships_and_badges_and_never_cash():
    config = _config()
    submissions = [
        _submission("s1", "agents", 20, 1, 1, team="t1"),
        _submission("s2", "agents", 10, 1, 2, team="t2"),
        _submission("s3", "agents", 5, 1, 3, team="t3"),
        _submission("s4", "agents", 0, 0, 4, status="invalid", team="t4"),
    ]
    members = {"t1": ["alice", "bob"], "t2": ["carol"], "t3": ["dan"],
               "t4": ["eve"]}
    recruiters = [{"account_bi": "rec1", "qualified": 9},
                  {"account_bi": "rec2", "qualified": 4},
                  {"account_bi": "rec3", "qualified": 0}]
    plan = policy.prize_grants(config, submissions, members, recruiters, [])
    assert {row["kind"] for row in plan} <= {"pro_months", "badge"}
    by = {}
    for row in plan:
        by.setdefault(row["account_bi"], []).append(row)
    # Every member of a valid submission gets the base grant and badge.
    for who in ("alice", "bob", "carol", "dan"):
        keys = {(r["reward_key"], r["kind"]) for r in by[who]}
        assert any(k.startswith("prize:submission:") for k, _ in keys)
    assert "eve" not in by
    alice = {(r["reward_key"], r["kind"], r["months"], r["badge"])
             for r in by["alice"]}
    assert ("prize:first:agents", "pro_months", 12, "") in alice
    assert ("prize:first:agents", "badge", 0, "hackathon_winner") in alice
    carol = {(r["reward_key"], r["months"]) for r in by["carol"]
             if r["kind"] == "pro_months"}
    assert ("prize:second:agents", 6) in carol
    # Recruiters earn 6 and 3 months; a recruiter with nobody qualified, none.
    assert [(r["reward_key"], r["months"]) for r in by["rec1"]
            if r["kind"] == "pro_months"] == [("prize:recruiter:1", 6)]
    assert [(r["reward_key"], r["months"]) for r in by["rec2"]
            if r["kind"] == "pro_months"] == [("prize:recruiter:2", 3)]
    assert "rec3" not in by


def test_community_choice_opens_with_its_milestone_and_closes_after_judging():
    assert not policy.voting_open("live", [])
    assert policy.voting_open("live", ["community_choice"])
    assert policy.voting_open("judging", ["community_choice"])
    assert not policy.voting_open("closed", ["community_choice"])


# --- growth and ambassadors -----------------------------------------------------------

def test_k_factor_is_referred_joins_per_participant():
    funnel = policy.growth_funnel({
        "participants": 200, "sharers": 90, "shares": 600, "clicks": 240,
        "referredJoins": 60, "referredVerified": 48, "qualified": 20,
        "activated": 30,
    })
    assert funnel["invitesPerParticipant"] == 3.0
    assert funnel["invitationConversion"] == 0.1
    assert funnel["kFactor"] == 0.3
    assert funnel["kFactor"] == round(
        funnel["invitesPerParticipant"] * funnel["invitationConversion"], 4)
    stages = {s["key"]: s for s in funnel["stages"]}
    assert stages["participants"]["fromPrevious"] is None
    assert stages["sharers"]["fromPrevious"] == 0.45
    assert stages["referredVerified"]["fromPrevious"] == 0.8
    # Not a subset of the stage before it, so no percentage of it.
    assert stages["shares"]["fromPrevious"] is None
    assert stages["activated"]["fromPrevious"] is None
    assert policy.growth_funnel({})["kFactor"] == 0.0


def test_ambassador_stats_and_pitch():
    assert policy.ambassador_stats(100, 25, 5) == {
        "clicks": 100, "joins": 25, "qualified": 5,
        "joinRate": 0.25, "qualifiedRate": 0.2}
    assert policy.ambassador_stats(0, 0, 0)["joinRate"] == 0.0
    assert len(policy.normalize_ambassador_pitch("x " * 500)) == (
        policy.MAX_AMBASSADOR_PITCH)
