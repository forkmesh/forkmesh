#!/usr/bin/env python3
"""Reward fulfilment: missions and prizes that grant real memberships.

Runs the real hackathon_runtime against the in-memory D1 stand-in from
test_hackathon. The claims worth running rather than reading are the ones
that lose somebody a prize when they are wrong: a reward is granted once, two
rewards stack, and a billing provider's sync cannot erase either.
"""

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, body_of, harness, policy, run,
)

MONTH = policy.MONTH_MS


def _referrer_with_friends(harness, friends):
    module, slug = harness.module, harness.campaign()
    alice_bi = "bi:alice"
    harness.account("alice")
    run(module._create_participant(harness.env, slug, alice_bi, "alice"))
    referred = []
    for index in range(friends):
        name = "friend%d" % index
        harness.account(name)
        bi = "bi:" + name
        run(module._create_participant(
            harness.env, slug, bi, name, referrer_bi=alice_bi,
            referrer_name="alice"))
        referred.append(bi)
    return module, slug, alice_bi, referred


def _promotion(harness, account_bi, role):
    return harness.db.connection.execute(
        "SELECT expires_at,revoked_at,scope_type FROM role_grants "
        "WHERE account_bi=? AND role=?", (account_bi, role)).fetchone()


def test_three_qualified_referrals_grant_pro_and_supporter_once(harness):
    module, slug, alice, referred = _referrer_with_friends(harness, 4)
    for bi in referred[:3]:
        run(module.on_referral_credited(harness.env, alice, "alice", bi))
    pro = _promotion(harness, alice, "pro_member")
    supporter = _promotion(harness, alice, "supporting_member")
    assert pro["scope_type"] == policy.PROMOTION_SCOPE_TYPE
    assert pro["expires_at"] == NOW + 3 * MONTH
    assert supporter["expires_at"] == NOW + 3 * MONTH
    assert harness.db.count(
        "hackathon_reward_grants", "reward_key='mission:invite_three'") == 1
    # The same credit observed again, and a fourth friend, grant nothing more
    # for this mission.
    run(module.on_referral_credited(harness.env, alice, "alice", referred[0]))
    run(module.on_referral_credited(harness.env, alice, "alice", referred[3]))
    assert _promotion(harness, alice, "pro_member")["expires_at"] == (
        NOW + 3 * MONTH)
    rewards = [n for n in harness.notifications
               if n["kind"] == "hackathon_reward"]
    assert len(rewards) == 1 and "3 months of Pro" in rewards[0]["title"]
    assert rewards[0]["to"] == "alice"


def test_rewards_stack_and_a_badge_is_awarded_with_its_months(harness):
    module, slug, alice, referred = _referrer_with_friends(harness, 10)
    for bi in referred:
        run(module.on_referral_credited(harness.env, alice, "alice", bi))
    # invite_three (3 months) then invite_ten (6 months), end to end.
    assert _promotion(harness, alice, "pro_member")["expires_at"] == (
        NOW + 9 * MONTH)
    assert ("alice", alice, "hackathon_connector", "hackathon") in (
        harness.badges)


def test_a_billing_provider_sync_cannot_erase_a_prize(harness):
    module, slug, alice, referred = _referrer_with_friends(harness, 3)
    for bi in referred:
        run(module.on_referral_credited(harness.env, alice, "alice", bi))
    connection = harness.db.connection
    # Patreon's own revoke, verbatim in shape: every platform-scope row for the
    # role, with no scope_bi filter.
    for role in ("pro_member", "supporting_member"):
        connection.execute(
            "UPDATE role_grants SET revoked_at=? WHERE account_bi=? "
            "AND role=? AND scope_type=? AND revoked_at=0",
            (NOW, alice, role, "platform"))
    # A second provider revoke, scoped the way Patreon writes it.
    connection.execute(
        "UPDATE role_grants SET revoked_at=? WHERE account_bi=? AND "
        "role='supporting_member' AND scope_type='platform' "
        "AND scope_bi='' AND revoked_at=0", (NOW, alice))
    assert _promotion(harness, alice, "pro_member")["revoked_at"] == 0
    assert _promotion(harness, alice, "supporting_member")["revoked_at"] == 0


def test_a_lapsed_promotion_restarts_from_now_instead_of_the_past(harness):
    module, slug, alice, referred = _referrer_with_friends(harness, 3)
    for bi in referred:
        run(module.on_referral_credited(harness.env, alice, "alice", bi))
    later = NOW + 12 * MONTH
    harness.clock["now"] = later
    granted = run(module._grant_rewards(
        harness.env, slug, alice, "alice",
        [{"reward_key": "prize:submission:x", "kind": "pro_months",
          "months": 3, "badge": ""}]))
    assert len(granted) == 1
    assert _promotion(harness, alice, "pro_member")["expires_at"] == (
        later + 3 * MONTH)


def test_a_squad_crossing_its_goal_rewards_every_member_once(harness):
    module, slug, owner, members = _referrer_with_friends(harness, 24)
    # Owner plus 24 members is 25.
    squad = harness.db.connection.execute(
        "SELECT squad_id,member_count FROM hackathon_squads").fetchone()
    assert squad["member_count"] == 25
    everyone = [owner] + members
    for bi in everyone:
        assert _promotion(harness, bi, "pro_member") is not None, bi
    key = "mission:squad_twentyfive:" + squad["squad_id"]
    assert harness.db.count(
        "hackathon_reward_grants", "reward_key=? AND kind='pro_months'",
        key) == 25
    # The 26th arrival is evaluated alone and is rewarded too.
    harness.account("late")
    run(module._create_participant(
        harness.env, slug, "bi:late", "late", referrer_bi=owner,
        referrer_name="alice"))
    assert _promotion(harness, "bi:late", "pro_member") is not None
    assert harness.db.count(
        "hackathon_reward_grants", "reward_key=? AND kind='pro_months'",
        key) == 26


def test_the_hq_lists_what_was_granted_and_until_when(harness):
    module, slug, alice, referred = _referrer_with_friends(harness, 3)
    for bi in referred:
        run(module.on_referral_credited(harness.env, alice, "alice", bi))
    harness.login("alice")
    payload = body_of(run(module.me_handler(
        harness.env, harness.request("/api/hackathon/me"))))
    assert payload["rewards"] == [{
        "key": "mission:invite_three", "kind": "pro_months", "months": 3,
        "badge": "", "grantedAt": NOW, "expiresAt": NOW + 3 * MONTH,
    }]


def test_plan_status_reports_a_promotion_beside_the_paid_membership():
    import asyncio
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    import plan_services

    class Runtime:
        def __init__(self, row):
            self.row = row

        def now(self):
            return NOW

        async def d1_first(self, sql, *args):
            # The prize lane and the paid lane are the same query over two
            # scopes; only the promotion scope answers here.
            return self.row if "promotion" in args else None

    for row, expected in (
            ({"pro": NOW + MONTH, "supporter": NOW + MONTH},
             {"tier": "pro", "expiresAt": NOW + MONTH}),
            ({"pro": None, "supporter": NOW + MONTH},
             {"tier": "supporter", "expiresAt": NOW + MONTH}),
            ({"pro": None, "supporter": None}, None)):
        assert asyncio.run(plan_services.promotion_status(
            Runtime(row), "bi:alice", NOW)) == expected
        assert asyncio.run(plan_services._active_membership(
            Runtime(row), "bi:alice", NOW)) is None