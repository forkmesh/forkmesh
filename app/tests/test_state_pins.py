#!/usr/bin/env python3
"""Clone integrity pins: a mirror must serve a repo state its group's
working-copy holder (the source of truth) actually attested — never just its
own self-published hash — while honest mirrors that lag the source by a few
publishes still clone (recent pin history).

...and when the source of truth is gone, the fleet governs itself. A mesh whose
whole point is to outlive its source cannot let one departed node's frozen pin
refuse every mirror that is still standing, so once no mirror is inside that
window any state two distinct peers independently advertise is admitted too."""

import ast
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME = ENTRY.parent / "app_runtime.py"
MIRRORS = ENTRY.parent / "mirrors.py"
# Mirror grouping / clone-selection helpers were extracted from entry.py into
# mirrors.py; parse both sources so the AST loaders below still find them.
_WORKER_SRC = ENTRY.read_text(encoding="utf-8") + "\n" + MIRRORS.read_text(encoding="utf-8")


def _load(*names):
    tree = ast.parse(_WORKER_SRC, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Assign))
        and (getattr(node, "name", None) in names
             or any(getattr(t, "id", None) in names
                    for t in getattr(node, "targets", [])))
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return [namespace[name] for name in names]


(
    clone_state_pins,
    repo_mirror_same_group,
    STATE_PIN_HISTORY,
    admission_override_pins,
    peer_quorum_pins,
    PEER_QUORUM_MIN_NODES,
    _group_state_within,
) = _load("clone_state_pins", "repo_mirror_same_group", "STATE_PIN_HISTORY",
          "admission_override_pins", "peer_quorum_pins",
          "PEER_QUORUM_MIN_NODES", "_group_state_within")


def _rec(owner, *, source="local-node", state="", root="root1", name="forkmesh"):
    return {"owner": owner, "name": name, "source": source,
            "stateHash": state, "rootCommit": root}


def _row(key, rec):
    return {"key_bi": key, "data": rec}


def test_unpublished_target_is_unpinned():
    assert clone_state_pins(None, "k", [], {}) is None
    assert clone_state_pins({}, "k", [], {}) is None


def test_source_is_checked_against_its_own_pin_and_history():
    target = _rec("source", state="AAA")
    pins = clone_state_pins(target, "kS", [], {"kS": ["bbb", "ccc"]})
    assert pins == {"aaa", "bbb", "ccc"}  # normalized lowercase


def test_source_without_any_attestation_is_unpinned():
    assert clone_state_pins(_rec("source", state=""), "kS", [], {}) is None


def test_mirror_is_validated_against_source_pins_not_its_own():
    # The mirror self-attests "evil" — that hash must NOT be acceptable when a
    # working-copy holder in its group has attested the real state. This is the
    # whole point: a tampered mirror can always republish a matching self-pin.
    mirror = _rec("mirror", source="remote-clone", state="evil")
    rows = [_row("kS", _rec("source", state="good")), _row("kM", mirror)]
    pins = clone_state_pins(mirror, "kM", rows, {})
    assert pins == {"good"}
    assert "evil" not in pins


def test_mirror_lagging_the_source_matches_pin_history():
    # Source pushed + republished (pin now "new"); a mirror still serving the
    # previous state must clone fine because "old" is in the recent history.
    mirror = _rec("mirror", source="remote-clone", state="old")
    rows = [_row("kS", _rec("source", state="new")), _row("kM", mirror)]
    pins = clone_state_pins(mirror, "kM", rows, {"kS": ["old"]})
    assert pins == {"new", "old"}


def test_pin_history_window_absorbs_active_issue_churn():
    # Every issue/PR/discussion action republishes the catalog with a fresh
    # state hash, so a repo under active collaboration churns pins fast. The
    # window must be deep enough that a mirror lagging by many issue edits
    # between its periodic re-syncs still serves .forkmesh/issues/ instead of
    # falling out of the accepted set and forcing the "unavailable until a live
    # desktop host serves" fallback. Guard against the window regressing to the
    # handful-of-publishes depth that produced that symptom.
    assert STATE_PIN_HISTORY >= 100

    # A mirror pinned to a state that is dozens of publishes behind the source
    # still resolves as long as that state is inside the retained history.
    history = ["s%d" % i for i in range(STATE_PIN_HISTORY)]
    mirror = _rec("mirror", source="remote-clone", state="s90")
    rows = [_row("kS", _rec("source", state="newest")), _row("kM", mirror)]
    pins = clone_state_pins(mirror, "kM", rows, {"kS": history})
    assert "s90" in pins


def test_fork_source_does_not_pin_the_mirror():
    # A different root commit is a different logical repo; its attestations
    # must not become acceptable states for this mirror's group.
    mirror = _rec("mirror", source="remote-clone", state="own")
    rows = [_row("kF", _rec("forker", state="forked", root="otherroot"))]
    # No same-group source exists -> legacy fallback to the mirror's own pin.
    assert clone_state_pins(mirror, "kM", rows, {}) == {"own"}


def test_mirror_with_no_attesting_source_falls_back_to_self_pin():
    mirror = _rec("mirror", source="remote-clone", state="own")
    rows = [_row("kS", _rec("source", state=""))]  # source never attested
    assert clone_state_pins(mirror, "kM", rows, {}) == {"own"}
    # ...and with no self pin either, the repo is simply unpinned (fail-open).
    bare = _rec("mirror", source="remote-clone", state="")
    assert clone_state_pins(bare, "kM", rows, {}) is None


def test_legacy_record_without_source_field_counts_as_local_node():
    # Records published before the `source` field existed default to
    # local-node on write, and clone_state_pins applies the same default.
    legacy = {"owner": "old", "name": "forkmesh", "stateHash": "lll",
              "rootCommit": "root1"}
    mirror = _rec("mirror", source="remote-clone", state="own")
    assert clone_state_pins(mirror, "kM", [_row("kL", legacy)], {}) == {"lll"}
    assert clone_state_pins(legacy, "kL", [], {}) == {"lll"}


def test_multiple_working_copy_holders_union_their_pins():
    mirror = _rec("mirror", source="remote-clone", state="own")
    rows = [
        _row("kA", _rec("alice", state="a1")),
        _row("kB", _rec("bob", state="b1")),
    ]
    assert clone_state_pins(mirror, "kM", rows, {"kB": ["b0"]}) == \
        {"a1", "b1", "b0"}


def test_admission_override_pins_collects_only_valid_member_states():
    # The override widens the accepted set to exactly the states registered
    # group members currently advertise — never arbitrary or malformed input.
    members = [
        _row("kA", _rec("alice", state="a" * 64)),
        _row("kB", _rec("bob", source="remote-clone", state="B" * 64)),
        _row("kC", _rec("carol", state="not-a-hash")),   # dropped
        _row("kD", _rec("dave", state="")),               # dropped
        {"data": None},                                    # dropped
    ]
    assert admission_override_pins(members) == {"a" * 64, "b" * 64}
    assert admission_override_pins([]) == set()
    assert admission_override_pins(None) == set()


def test_override_widens_a_real_source_pin_never_fabricates_one():
    # A mirror stuck out of the window (state "own") is normally rejected in
    # favor of the source's "good" pin. The override is applied on TOP of that
    # real pin set — it admits the mirror's own advertised state, but it can
    # only ever be reached when a genuine source pin already exists.
    src_state = "1" * 64
    mirror_state = "2" * 64
    mirror = _rec("mirror", source="remote-clone", state=mirror_state)
    rows = [_row("kS", _rec("source", state=src_state)), _row("kM", mirror)]
    strict = clone_state_pins(mirror, "kM", rows, {})
    assert strict == {src_state}                     # mirror excluded by default
    widened = set(strict) | admission_override_pins(rows)
    assert widened == {src_state, mirror_state}      # override lets it serve

    # With no source attestation at all, clone_state_pins is None and the
    # resolver's `if pins and ...override...` guard means the override is never
    # consulted — an unpinned repo can never be force-opened.
    bare_mirror = _rec("mirror", source="remote-clone", state="")
    unpinned_rows = [_row("kS", _rec("source", state=""))]
    assert clone_state_pins(bare_mirror, "kM", unpinned_rows, {}) is None


def test_admission_override_endpoint_is_signed_bounded_and_ttl_healing():
    # The override is an authenticated, self-expiring lever — not a permanent
    # relaxation of the integrity gate. Guard the security-critical properties.
    text = (
        ENTRY.read_text(encoding="utf-8")
        + "\n\n"
        + APP_RUNTIME.read_text(encoding="utf-8")
    )
    # Reachable via a POST route.
    assert '"/api/mirrors/admission-override"' in text
    assert "_admin_set_admission_override" in text
    # Authenticated by the node's admin key (same auth as other admin mutations)
    # over a domain-separated canonical string.
    assert "forkmesh-admission-override-v1" in text
    assert "_admin_authorized(env, node, ts, sig, canonical)" in text
    # Stored with a native KV TTL and a hard ceiling, so it always heals back to
    # the strict policy on its own.
    assert "ADMISSION_OVERRIDE_MAX_TTL" in text
    assert "expirationTtl" in text
    # Consulted only when the strict path has no eligible serving endpoint,
    # guarded so an unpinned repo (pins is None) is never force-opened. During
    # that bounded window the fresh signed endpoint generations are admitted;
    # widening catalog pins alone cannot match the gateway's routing digest.
    assert "if pins and await _admission_override_active(" in text
    assert "pins.update(admission_override_pins(members)," in text
    assert "endpoint_digests.values()" in text
    # And audited like every other privileged mutation.
    assert '"admin.admission_override"' in text


def _entry_text():
    # schema DDL now lives in schema.py; concatenate it for the source checks.
    return (ENTRY.read_text(encoding="utf-8") + "\n"
            + (ENTRY.parent / "catalog_routes.py").read_text(encoding="utf-8")
            + "\n"
            + (ENTRY.parent / "schema.py").read_text(encoding="utf-8"))


def _method_source(class_name, method_name):
    tree = ast.parse(_entry_text(), filename=str(ENTRY))
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for item in node.body:
                if (isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
                        and item.name == method_name):
                    return ast.unparse(item)
    raise AssertionError(
        "%s.%s not found in entry.py" % (class_name, method_name))


def test_publish_records_source_pins_into_history():
    # Only working-copy holders' verified attestations enter the history that
    # mirrors are validated against, pruned to the newest STATE_PIN_HISTORY.
    text = _entry_text()
    assert "INSERT INTO repo_state_history" in text
    assert "record.get(\"source\") == \"local-node\"" in text
    assert "prior_state_hash" in text
    assert "history_states" in text
    assert "STATE_PIN_HISTORY" in text
    # Renaming an account carries its attested history to the new namespace.
    assert "UPDATE repo_state_history SET key_bi=" in text


def test_schema_has_pin_history_and_sticky_tables():
    text = _entry_text()
    assert "CREATE TABLE IF NOT EXISTS repo_state_history" in text
    assert "CREATE TABLE IF NOT EXISTS clone_sticky" in text


# --- Surviving the loss of the source of truth ---------------------------
#
# The failure this section exists to prevent: the source stops publishing, the
# mirrors keep taking collaboration writes (issues, comments, merges), every one
# of those moves them past the last state the source attested, and the fleet
# then refuses itself — permanently, with no path back, because a mirror's own
# catch-up fetch is the very same gated route. That is the exact opposite of
# what a mesh is for.

SRC_STATE = "1" * 64      # what the source attested before it went away
FLEET_STATE = "2" * 64    # where the mirrors converged without it
ODD_STATE = "3" * 64


def _mirror(name, state):
    return _row("k" + name, {"owner": name, "name": "forkmesh",
                             "machineName": name, "source": "remote-clone",
                             "stateHash": state, "rootCommit": "root1"})


def _source(state=SRC_STATE):
    return _row("kS", {"owner": "srcnode", "name": "forkmesh",
                       "machineName": "srcnode", "source": "local-node",
                       "stateHash": state, "rootCommit": "root1"})


def test_agreeing_peers_carry_the_repo_once_the_source_window_runs_out():
    fleet = [_mirror("mirror29", FLEET_STATE), _mirror("mirror30", FLEET_STATE),
             _mirror("mirror31", FLEET_STATE)]
    rows = [_source()] + fleet
    for row in fleet:
        pins = clone_state_pins(row["data"], row["key_bi"], rows, {})
        assert FLEET_STATE in pins, row["data"]["owner"]
        # The departed source's own attestation is never discarded, so a mirror
        # that never caught up keeps serving alongside the rest.
        assert SRC_STATE in pins


def test_one_mirror_can_never_admit_its_own_state():
    # The property the whole pin system exists for: a tampered mirror publishes
    # a hash matching its forged refs, so possession proves nothing. Losing the
    # source must not turn a single mirror into its own authority.
    lone = _mirror("mirror29", FLEET_STATE)
    rows = [_source(), lone]
    assert clone_state_pins(lone["data"], lone["key_bi"], rows, {}) == \
        {SRC_STATE}


def test_disagreeing_peers_admit_nothing():
    # No split brain: two mirrors that each claim a different state are two
    # unsupported claims, not two votes.
    a = _mirror("mirror29", FLEET_STATE)
    b = _mirror("mirror30", ODD_STATE)
    rows = [_source(), a, b]
    for row in (a, b):
        pins = clone_state_pins(row["data"], row["key_bi"], rows, {})
        assert pins == {SRC_STATE}, row["data"]["owner"]


def test_a_live_source_window_is_never_overridden_by_its_peers():
    # The control. While even one mirror is still inside the window the source
    # is governing the group and stays its sole authority, so peers cannot
    # advance the route past it — this is not a general relaxation of the gate.
    in_window = _mirror("mirror9", SRC_STATE)
    ahead = [_mirror("mirror29", FLEET_STATE), _mirror("mirror30", FLEET_STATE)]
    rows = [_source(), in_window] + ahead
    for row in ahead:
        pins = clone_state_pins(row["data"], row["key_bi"], rows, {})
        assert pins == {SRC_STATE}
        assert FLEET_STATE not in pins
    assert SRC_STATE in clone_state_pins(
        in_window["data"], in_window["key_bi"], rows, {})


def test_the_source_record_alone_does_not_hold_its_window_open():
    # A catalog record outlives the machine that published it. If the source's
    # own frozen row counted as "still inside the window", the window would
    # never close and the group could never recover from losing it.
    assert not _group_state_within({SRC_STATE}, _source()["data"], [_source()])
    fleet = [_mirror("mirror29", FLEET_STATE), _mirror("mirror30", FLEET_STATE)]
    assert _group_state_within(
        {FLEET_STATE}, fleet[0]["data"], [_source()] + fleet)


def test_peer_quorum_counts_distinct_machines_not_rows():
    # One machine publishing several catalog rows (its repo record plus its
    # secondary-device rows) is one voice, not a quorum of itself.
    same_machine = [_mirror("mirror29", FLEET_STATE),
                    _mirror("mirror29", FLEET_STATE)]
    target = same_machine[0]["data"]
    assert peer_quorum_pins(target, same_machine) == set()
    two_machines = [_mirror("mirror29", FLEET_STATE),
                    _mirror("mirror30", FLEET_STATE)]
    assert peer_quorum_pins(target, two_machines) == {FLEET_STATE}


def test_peer_quorum_ignores_malformed_states_and_other_repos():
    target = _mirror("mirror29", FLEET_STATE)["data"]
    rows = [
        _mirror("mirror29", FLEET_STATE),
        _mirror("mirror30", FLEET_STATE),
        _mirror("mirror31", "not-a-hash"),
        _mirror("mirror32", ""),
        _row("kFork", {"owner": "forker", "name": "forkmesh",
                       "machineName": "forker", "source": "remote-clone",
                       "stateHash": ODD_STATE, "rootCommit": "otherroot"}),
        _row("kFork2", {"owner": "forker2", "name": "forkmesh",
                        "machineName": "forker2", "source": "remote-clone",
                        "stateHash": ODD_STATE, "rootCommit": "otherroot"}),
    ]
    # The two agreeing forks share a root commit with each other but not with
    # the target, so they are a different logical repo and cannot pin this one.
    assert peer_quorum_pins(target, rows) == {FLEET_STATE}


def test_quorum_threshold_is_two_distinct_nodes():
    # Guard the security parameter itself. One is possession, not agreement.
    assert PEER_QUORUM_MIN_NODES >= 2
