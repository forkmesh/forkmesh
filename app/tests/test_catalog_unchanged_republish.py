"""Unchanged catalog republishes must renew a lease, not act like a change.

Every mirror node re-POSTs its identical catalog record on a heartbeat
(30s-4min). Before the meaningful-change gate, each of those republishes
re-encrypted the row, purged the shared edge caches (keeping the catalog GET
permanently cache-cold), re-challenged endpoint health, and re-ran snapshot
ingestion. Now a record that differs only in volatile telemetry short-circuits:
within the lease window it skips even the D1 write, after it one write renews
the lease without touching the caches. Only real changes purge, bump the
catalog revision behind the weak ETag, and push the "catalog" node event.
"""

import re
from pathlib import Path

CATALOG_ROUTES = (
    Path(__file__).resolve().parents[1] / "src" / "catalog_routes.py")
CATALOG_SOURCE = CATALOG_ROUTES.read_text(encoding="utf-8")


def _load_module_namespace():
    # Module level is only constants and defs; nothing external runs at exec.
    namespace = {
        "re": re,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
    }
    exec(compile(CATALOG_SOURCE, str(CATALOG_ROUTES), "exec"), namespace)
    return namespace


NS = _load_module_namespace()
meaningful = NS["catalog_meaningful_change"]
mirror_update_needed = NS["catalog_mirror_update_needed"]
VOLATILE = NS["CATALOG_VOLATILE_FIELDS"]

BASE = {
    "owner": "alice", "name": "repo", "visibility": "public",
    "commit": "a" * 40, "stateHash": "b" * 64, "description": "d",
    "updatedAt": "1000", "lastSync": "1000", "sizeBytes": 10,
    "cpuPercent": 5, "clonesServed": "3", "stateSig": "sig1",
    "signature": "s1", "activityWeeks": [1, 2, 3],
}


def test_volatile_only_republish_is_not_meaningful():
    republish = dict(BASE)
    republish.update({
        "updatedAt": "2000", "lastSync": "2000", "sizeBytes": 11,
        "cpuPercent": 90, "clonesServed": "4", "stateSig": "sig2",
        "signature": "s2", "activityWeeks": [1, 2, 4],
    })
    assert meaningful(BASE, republish) is False


def test_real_changes_are_meaningful():
    for field, value in (
            ("commit", "c" * 40),
            ("stateHash", "e" * 64),
            ("description", "changed"),
            ("visibility", "private"),
            ("branch", "next"),
    ):
        changed = dict(BASE)
        changed[field] = value
        assert meaningful(BASE, changed) is True, field


def test_new_record_shape_is_meaningful():
    # A record gaining or losing a non-volatile field is a change.
    grown = dict(BASE)
    grown["actionsEnabled"] = True
    grown["actionsState"] = "enabled"
    assert meaningful(BASE, grown) is True


def test_volatile_set_never_hides_integrity_fields():
    # The fields that bind identity, refs, and visibility must always count.
    for field in ("owner", "name", "visibility", "commit", "stateHash",
                  "maintainer", "nodeId", "source", "cloneUrl"):
        assert field not in VOLATILE, field


def test_every_source_ref_change_wakes_the_mirror_fleet():
    prior_commit = "a" * 40
    prior_state = "b" * 64

    # A feature-branch or tag push changes the canonical heads-and-tags digest
    # without moving the default branch displayed as `commit`.
    branch_push = dict(
        BASE, commit=prior_commit, stateHash="c" * 64,
        source="local-node")
    assert mirror_update_needed(
        branch_push, True, prior_commit, prior_state) is True

    unchanged = dict(
        BASE, commit=prior_commit, stateHash=prior_state,
        source="local-node")
    assert mirror_update_needed(
        unchanged, True, prior_commit, prior_state) is False

    # Legacy publishers without a refs digest still retain the default-head
    # wake-up, while mirror echoes, private records, and first publication do
    # not start notification loops or pointless fetches.
    legacy_head_push = dict(
        BASE, commit="d" * 40, stateHash="", source="local-node")
    assert mirror_update_needed(
        legacy_head_push, True, prior_commit, "") is True
    assert mirror_update_needed(
        dict(branch_push, source="remote-clone"),
        True, prior_commit, prior_state) is False
    assert mirror_update_needed(
        dict(branch_push, visibility="private"),
        True, prior_commit, prior_state) is False
    assert mirror_update_needed(
        branch_push, False, prior_commit, prior_state) is False


def test_unchanged_short_circuit_precedes_the_write_side_effects():
    # The lease fast-return must run before encryption/upsert and before the
    # purge + revision-bump + notify block that only real changes may reach.
    post_body = CATALOG_SOURCE.split('if method == "POST":', 1)[1]
    unchanged_at = post_body.index("if not meaningful_change:")
    assert unchanged_at < post_body.index(
        "await _https_mirror_refresh_catalog_publisher_health")
    assert unchanged_at < post_body.index(
        "await purge_catalog_related_caches()")
    # A meaningful write both bumps the ETag revision and pushes the
    # payload-free "catalog" advisory over the node event socket. (The
    # device-mirror branch has its own earlier bump, so anchor after the
    # canonical purge.)
    purge_at = post_body.index("await purge_catalog_related_caches()")
    assert post_body.index("await _bump_catalog_rev(env)", purge_at) \
        < post_body.index(
            'notify_repo_host(env, owner, record["name"], "catalog")')
