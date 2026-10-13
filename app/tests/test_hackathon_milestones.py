#!/usr/bin/env python3
"""Community milestones unlock where their counters move, exactly once."""

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, body_of, harness, policy, run,
)


def _low_thresholds(harness, slug, **thresholds):
    for key, value in thresholds.items():
        harness.db.connection.execute(
            "UPDATE hackathon_milestones SET threshold=? "
            "WHERE hackathon_slug=? AND key=?", (value, slug, key))
    harness.db.connection.commit()


def _join(harness, slug, name):
    harness.account(name)
    run(harness.module._create_participant(
        harness.env, slug, "bi:" + name, name))


def _unlocked(harness, key):
    return harness.db.scalar(
        "SELECT unlocked_at FROM hackathon_milestones WHERE key=?", key)


def test_the_join_that_reaches_a_threshold_unlocks_it_once(harness):
    slug = harness.campaign(status="live")
    _low_thresholds(harness, slug, community_choice=2)
    _join(harness, slug, "alice")
    assert _unlocked(harness, "community_choice") == 0
    harness.clock["now"] = NOW + 5
    _join(harness, slug, "bob")
    assert _unlocked(harness, "community_choice") == NOW + 5
    harness.clock["now"] = NOW + 99
    _join(harness, slug, "cy")
    assert _unlocked(harness, "community_choice") == NOW + 5


def test_an_unlock_never_relocks_when_a_counter_falls(harness):
    slug = harness.campaign(status="live")
    _low_thresholds(harness, slug, community_choice=1)
    _join(harness, slug, "alice")
    assert _unlocked(harness, "community_choice") == NOW
    harness.db.connection.execute(
        "UPDATE hackathons SET participant_count=0")
    harness.db.connection.commit()
    run(harness.module._check_milestones(harness.env, slug))
    assert _unlocked(harness, "community_choice") == NOW


def test_unlocking_community_choice_opens_voting(harness):
    slug = harness.campaign(status="live")
    _low_thresholds(harness, slug, community_choice=1)
    _join(harness, slug, "alice")
    harness.login("alice")
    payload = body_of(run(harness.module.me_handler(
        harness.env, harness.request("/api/hackathon/me"))))
    assert payload["votingOpen"] is True
    by_key = {m["key"]: m for m in payload["milestones"]}
    assert by_key["community_choice"]["unlocked"] is True
    assert by_key["newcomer_prize"]["unlocked"] is False


def test_lowering_a_threshold_takes_effect_without_waiting_for_a_join(harness):
    slug = harness.campaign(status="live")
    _join(harness, slug, "alice")
    harness.account("admin")
    harness.admins.add("admin")
    config = policy.default_config()
    for entry in config["milestones"]:
        if entry["key"] == "community_choice":
            entry["threshold"] = 1
    harness.login("admin")
    run(harness.module.admin_handler(harness.env, harness.request(
        "/api/admin/hackathon", "POST",
        {"slug": slug, "config": config})))
    assert _unlocked(harness, "community_choice") == NOW


def test_the_campaign_cache_is_dropped_when_something_unlocks(harness):
    slug = harness.campaign(status="live")
    _low_thresholds(harness, slug, community_choice=1)
    harness.cache[harness.module.CAMPAIGN_CACHE_KEY] = "stale"
    _join(harness, slug, "alice")
    assert harness.module.CAMPAIGN_CACHE_KEY not in harness.cache
