import hashlib
import json
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "app" / "tests"))

import collaboration  # noqa: E402
from collaboration_fixtures import ACTORS, Builder, build_corpus, sign  # noqa: E402


def _state(events, policy):
    return collaboration.fold_events(events, policy).state


def _javascript_fold(events, policy, *, ndjson=None):
    node = shutil.which("node")
    assert node, "Node.js is required by the collaboration conformance gate"
    runner = ROOT / "app" / "tests" / "run_collaboration_js.js"
    completed = subprocess.run(
        [node, str(runner)],
        input=json.dumps(
            {"policy": policy, "ndjson": ndjson}
            if ndjson is not None
            else {"policy": policy, "events": events}
        ),
        text=True,
        capture_output=True,
        check=False,
        cwd=ROOT,
        timeout=30,
    )
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def _signed_mutation(
    corpus, *, object_name, parent_name, actor, action, payload, sequence
):
    created = corpus.named[object_name]
    parent = corpus.named[parent_name]
    unsigned = collaboration.unsigned_event(
        repository=corpus.repository,
        policy=collaboration.policy_id(corpus.policy),
        family=created["family"],
        object=created["object"],
        action=action,
        author=ACTORS[actor],
        sequence=sequence,
        clock=parent["clock"] + 1,
        issued_at=2_000_000_000_000 + sequence,
        parents=[parent["eventId"]],
        references=created["references"],
        payload=payload,
    )
    return collaboration.finish_event(
        unsigned, sign(actor, collaboration.signing_bytes(unsigned))
    )


def _unauthorized_family_events(corpus):
    specifications = [
        ("issue_create", "issue_create", "bob", "update", {"fields": {"title": "forged"}}),
        ("pull_create", "pull_create", "bob", "update", {"fields": {"status": "closed"}}),
        ("comment_create", "comment_create", "alice", "update", {"fields": {"body": "forged"}}),
        ("label_create", "label_create", "alice", "update", {"fields": {"description": "forged"}}),
        ("reaction_create", "reaction_create", "alice", "tombstone", {"reasonCode": "forged"}),
        ("review_create", "review_create", "alice", "update", {"fields": {"body": "forged"}}),
        ("milestone_create", "milestone_create", "alice", "update", {"fields": {"title": "forged"}}),
        ("board_create", "board_create", "alice", "update", {"fields": {"description": "forged"}}),
        ("notification_create", "notification_create", "bob", "update", {"fields": {"message": "forged"}}),
        (
            "moderation_decide",
            "moderation_appeal",
            "alice",
            "resolve",
            {
                "predecessor": corpus.named["moderation_decide"]["eventId"],
                "appeal": corpus.named["moderation_appeal"]["eventId"],
                "fields": {"decision": "hide", "reasonCode": "forged"},
            },
        ),
    ]
    return [
        _signed_mutation(
            corpus,
            object_name=object_name,
            parent_name=parent_name,
            actor=actor,
            action=action,
            payload=payload,
            sequence=90_000 + index,
        )
        for index, (object_name, parent_name, actor, action, payload) in enumerate(
            specifications
        )
    ]


def test_every_object_family_folds_with_explicit_conflicts_and_lineage():
    corpus = build_corpus()
    result = collaboration.fold_events(corpus.events, corpus.policy)

    assert result.audit["rejected"] == []
    assert result.audit["pending"] == []
    assert set(result.state["objects"]) == set(collaboration.FAMILIES)
    assert all(result.state["objects"][family] for family in collaboration.FAMILIES)
    assert result.telemetry["acceptedByFamily"] == {
        "issue": 7,
        "pull_request": 2,
        "comment": 4,
        "label": 3,
        "reaction": 3,
        "review": 3,
        "milestone": 3,
        "board": 2,
        "notification": 2,
        "moderation": 3,
    }

    issue = result.state["objects"]["issue"][corpus.objects["issue_create"]]
    assert issue["values"]["title"] == "Converge explicitly"
    assert issue["sets"]["labels"] == [corpus.objects["label_create"]]
    assert issue["tombstoned"] is False

    review = result.state["objects"]["review"][corpus.objects["review_create"]]
    assert "state" not in review["values"]
    assert {candidate["value"] for candidate in review["conflicts"]["state"]} == {
        "approved",
        "changes_requested",
    }

    comment = result.state["objects"]["comment"][corpus.objects["comment_create"]]
    assert comment["values"]["body"] == "Combined edit"
    assert comment["visibility"] == "visible"
    assert comment["moderation"] == [corpus.objects["moderation_decide"]]
    moderation = result.state["objects"]["moderation"][
        corpus.objects["moderation_decide"]
    ]
    assert moderation["values"]["decision"] == "allow"
    assert moderation["pendingAppeals"] == []


def test_permutation_duplicate_delay_and_replay_converge():
    corpus = build_corpus()
    expected = _state(corpus.events, corpus.policy)
    rng = random.Random(484)

    for _ in range(60):
        delivered = list(corpus.events)
        delivered.extend(rng.choices(corpus.events, k=rng.randrange(0, 20)))
        rng.shuffle(delivered)
        assert _state(delivered, corpus.policy) == expected

    permuted = collaboration.fold_events(delivered, corpus.policy)
    independent = _javascript_fold(delivered, corpus.policy)
    assert independent["state"] == permuted.state
    assert independent["stateDigest"] == permuted.state_digest
    assert independent["audit"] == permuted.audit
    assert independent["telemetry"] == permuted.telemetry

    delayed = collaboration.SignedEventLog()
    children_first = sorted(corpus.events, key=lambda event: event["clock"], reverse=True)
    delayed.extend(children_first[:10])
    assert delayed.fold(corpus.policy).state["pending"]
    delayed.extend(children_first[10:])
    delayed.extend(corpus.events)  # a complete replay changes metrics, never state
    recovered = delayed.fold(corpus.policy)
    assert recovered.state == expected
    assert recovered.telemetry["duplicateDeliveries"] == len(corpus.events)
    assert _javascript_fold(children_first + corpus.events, corpus.policy)[
        "state"
    ] == expected


def test_invalid_signatures_equivocation_and_unauthorized_moderation_fail_closed():
    builder = Builder()
    issue = builder.create(
        "issue", "issue", "alice", {"title": "A", "body": "B", "status": "open"}
    )

    unsigned = collaboration.unsigned_event(
        repository=builder.repository,
        policy=builder.policy_id,
        family="issue",
        object=builder.objects["issue"],
        action="update",
        author=ACTORS["alice"],
        sequence=builder.sequences["alice"],
        clock=1,
        issued_at=2,
        parents=[issue["eventId"]],
        references=[],
        payload={"fields": {"title": "tampered"}},
    )
    valid_signature = sign("alice", collaboration.signing_bytes(unsigned))
    signature_bytes = bytearray(collaboration._b64url_decode(valid_signature, 64, "test"))
    signature_bytes[-1] ^= 1
    forged = collaboration.finish_event(unsigned, collaboration._b64url(bytes(signature_bytes)))

    slot = builder.sequences["alice"] + 10
    left = builder.event(
        "left",
        "issue",
        "alice",
        "update",
        {"fields": {"title": "left"}},
        [issue["eventId"]],
        sequence=slot,
    )
    right = builder.event(
        "right",
        "issue",
        "alice",
        "update",
        {"fields": {"title": "right"}},
        [issue["eventId"]],
        sequence=slot,
    )

    subject_ref = [{
        "relation": "subject",
        "family": "issue",
        "object": builder.objects["issue"],
    }]
    object_key = collaboration._b64url(b"unauthorized-mod")
    moderation_object = collaboration.object_id(
        builder.repository, "moderation", ACTORS["alice"], object_key, subject_ref
    )
    unauthorized_unsigned = collaboration.unsigned_event(
        repository=builder.repository,
        policy=builder.policy_id,
        family="moderation",
        object=moderation_object,
        action="decide",
        author=ACTORS["alice"],
        sequence=slot + 1,
        clock=1,
        issued_at=3,
        parents=[issue["eventId"]],
        references=subject_ref,
        payload={
            "objectKey": object_key,
            "fields": {"decision": "hide", "reasonCode": "spam"},
        },
    )
    unauthorized = collaboration.finish_event(
        unauthorized_unsigned,
        sign("alice", collaboration.signing_bytes(unauthorized_unsigned)),
    )

    result = collaboration.fold_events(
        [issue, forged, left, right, unauthorized], builder.policy
    )
    reasons = {item["reason"] for item in result.audit["rejected"]}
    assert "invalid_signature" in reasons
    assert "author_sequence_equivocation" in reasons
    assert "unauthorized" in reasons
    state = result.state["objects"]["issue"][builder.objects["issue"]]
    assert state["values"]["title"] == "A"
    assert state["visibility"] == "visible"


def test_authorization_matrix_rejects_signed_mutations_for_every_family():
    corpus = build_corpus()
    expected = collaboration.fold_events(corpus.events, corpus.policy)
    unauthorized = _unauthorized_family_events(corpus)

    result = collaboration.fold_events(corpus.events + unauthorized, corpus.policy)

    assert result.state == expected.state
    assert result.telemetry["rejectionReasons"] == {"unauthorized": len(collaboration.FAMILIES)}
    assert {
        event["family"]
        for event in unauthorized
        if any(
            rejected["id"] == event["eventId"]
            for rejected in result.audit["rejected"]
        )
    } == set(collaboration.FAMILIES)


def test_concurrent_moderation_decisions_quarantine_instead_of_picking_a_winner():
    builder = Builder()
    issue = builder.create(
        "issue", "issue", "alice", {"title": "Moderate", "body": "Body"}
    )
    comment = builder.create(
        "comment",
        "comment",
        "bob",
        {"body": "Subject"},
        references=[builder.reference("subject", "issue", "issue")],
        parents=[issue["eventId"]],
    )
    subject = [builder.reference("subject", "comment", "comment")]
    hidden = builder.create(
        "hidden",
        "moderation",
        "moderator",
        {"decision": "hide", "reasonCode": "spam"},
        references=subject,
        parents=[comment["eventId"]],
    )
    allowed = builder.create(
        "allowed",
        "moderation",
        "moderator",
        {"decision": "allow", "reasonCode": "reviewed"},
        references=subject,
        parents=[comment["eventId"]],
    )

    expected = collaboration.fold_events(builder.events, builder.policy)
    state = expected.state["objects"]["comment"][builder.objects["comment"]]
    assert state["visibility"] == "quarantined"
    assert state["moderationConflict"] == sorted(
        [hidden["object"], allowed["object"]]
    )
    assert collaboration.fold_events(
        list(reversed(builder.events)), builder.policy
    ).state == expected.state
    assert _javascript_fold(
        list(reversed(builder.events)), builder.policy
    )["state"] == expected.state


def test_concurrent_tombstone_not_observed_by_restore_remains_active():
    builder = Builder()
    created = builder.create(
        "comment",
        "comment",
        "bob",
        {"body": "keep history"},
        references=[
            {
                "relation": "subject",
                "family": "issue",
                "object": "urn:forkmesh:collaboration:issue:sha256:" + "1" * 64,
            }
        ],
    )
    left = builder.event(
        "left", "comment", "bob", "tombstone", {"reasonCode": "duplicate"}, [created["eventId"]]
    )
    right = builder.event(
        "right", "comment", "bob", "tombstone", {"reasonCode": "spam"}, [created["eventId"]]
    )
    builder.event(
        "restore",
        "comment",
        "bob",
        "restore",
        {"reasonCode": "mistake", "observed": [left["eventId"]]},
        [left["eventId"]],
    )
    result = collaboration.fold_events(builder.events, builder.policy)
    state = result.state["objects"]["comment"][builder.objects["comment"]]
    assert state["tombstoned"] is True
    assert state["tombstones"] == [right["eventId"]]


def test_observed_remove_preserves_a_concurrent_set_add():
    builder = Builder()
    issue = builder.create(
        "issue", "issue", "alice", {"title": "Set", "body": "Body"}
    )
    label_key = collaboration._b64url(b"set-label-object")
    label = collaboration.object_id(
        builder.repository, "label", ACTORS["maintainer"], label_key
    )
    second_label_key = collaboration._b64url(
        hashlib.sha256(b"set-label-two").digest()[:16]
    )
    second_label = collaboration.object_id(
        builder.repository, "label", ACTORS["maintainer"], second_label_key
    )
    left = builder.event(
        "left",
        "issue",
        "alice",
        "add",
        {"field": "labels", "values": [label]},
        [issue["eventId"]],
    )
    right = builder.event(
        "right",
        "issue",
        "alice",
        "add",
        {"field": "labels", "values": [second_label]},
        [issue["eventId"]],
    )
    remove_left = builder.event(
        "remove_left",
        "issue",
        "alice",
        "remove",
        {"field": "labels", "values": [label], "observed": [left["eventId"]]},
        [left["eventId"]],
    )
    partial = collaboration.fold_events(builder.events, builder.policy)
    assert partial.state["objects"]["issue"][builder.objects["issue"]]["sets"][
        "labels"
    ] == [second_label]

    builder.event(
        "remove_both",
        "issue",
        "alice",
        "remove",
        {
            "field": "labels",
            "values": sorted([label, second_label]),
            "observed": sorted([left["eventId"], right["eventId"]]),
        },
        [remove_left["eventId"], right["eventId"]],
    )
    final = collaboration.fold_events(builder.events, builder.policy)
    assert final.state["objects"]["issue"][builder.objects["issue"]]["sets"][
        "labels"
    ] == []
    assert _javascript_fold(
        list(reversed(builder.events)), builder.policy
    )["state"] == final.state


def test_raw_log_export_rollback_recovery_and_corruption_are_auditable():
    corpus = build_corpus()
    log = collaboration.SignedEventLog()
    log.extend(corpus.events)
    exported = log.export_ndjson()
    restored = collaboration.SignedEventLog.from_ndjson(exported)
    assert restored.raw_events == log.raw_events
    assert restored.fold(corpus.policy).state == log.fold(corpus.policy).state
    independent = _javascript_fold(
        [], corpus.policy, ndjson=exported.decode("utf-8")
    )
    assert independent["state"] == log.fold(corpus.policy).state

    # Rollback is a view over an earlier raw event set, not destructive history
    # rewriting. Re-adding the retained tail exactly recovers the state digest.
    prefix = collaboration.SignedEventLog()
    prefix.extend(corpus.events[:10])
    rolled_back_digest = prefix.fold(corpus.policy).state_digest
    assert rolled_back_digest != log.fold(corpus.policy).state_digest
    prefix.extend(corpus.events[10:])
    assert prefix.fold(corpus.policy).state_digest == log.fold(corpus.policy).state_digest

    noncanonical = json.dumps(corpus.events[0], indent=2).encode()
    restored.append(noncanonical)
    result = restored.fold(corpus.policy)
    assert any(
        item["reason"] == "noncanonical_encoding" for item in result.audit["rejected"]
    )
    assert noncanonical in result.raw_events

    crlf = collaboration.SignedEventLog.from_ndjson(
        collaboration.canonical_json(corpus.events[0]) + b"\r\n"
    ).fold(corpus.policy)
    assert crlf.telemetry["accepted"] == 0
    assert crlf.telemetry["rejectionReasons"] == {"noncanonical_encoding": 1}
    independent_crlf = _javascript_fold(
        [],
        corpus.policy,
        ndjson=(collaboration.canonical_json(corpus.events[0]) + b"\r\n").decode(),
    )
    assert independent_crlf["audit"] == crlf.audit
    assert independent_crlf["telemetry"] == crlf.telemetry


def test_telemetry_is_aggregate_only_and_cli_recomputes_the_same_state(tmp_path):
    corpus = build_corpus()
    log = collaboration.SignedEventLog()
    log.extend(corpus.events)
    result = log.fold(corpus.policy)
    encoded_telemetry = json.dumps(result.telemetry, sort_keys=True)
    for secret_value in (
        ACTORS["alice"],
        corpus.objects["issue_create"],
        corpus.named["issue_create"]["signature"],
        "Initial body",
    ):
        assert secret_value not in encoded_telemetry

    policy_path = tmp_path / "policy.json"
    log_path = tmp_path / "events.ndjson"
    policy_path.write_text(json.dumps(corpus.policy), encoding="utf-8")
    log_path.write_bytes(log.export_ndjson())
    completed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "verify_collaboration.py"),
            "--policy",
            str(policy_path),
            "--require-complete",
            str(log_path),
        ],
        capture_output=True,
        check=False,
        cwd=ROOT,
    )
    assert completed.returncode == 0, completed.stderr.decode()
    verified = json.loads(completed.stdout)
    assert verified["state"] == result.state
    assert verified["stateDigest"] == result.state_digest


def test_public_schema_docs_and_automation_cover_the_protocol_contract():
    schema_path = (
        ROOT / "www" / "public" / "docs" / "protocol"
        / "collaboration-event-v1.schema.json"
    )
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    assert schema["properties"]["schema"]["const"] == collaboration.EVENT_SCHEMA
    assert set(schema["properties"]["family"]["enum"]) == set(collaboration.FAMILIES)
    design = (
        ROOT / "www" / "docs" / "design" / "conflict-safe-collaboration.md"
    ).read_text(encoding="utf-8")
    for marker in (
        "Canonical encoding",
        "Causality and replay defense",
        "Moderation lineage",
        "Storage, migration, and recovery",
        "Threat, abuse, privacy, and telemetry",
        "Linux, macOS, and Windows",
    ):
        assert marker in design
    workflow = (
        ROOT / ".github" / "workflows" / "collaboration-conformance.yml"
    ).read_text(encoding="utf-8")
    assert "ubuntu-latest, macos-latest, windows-latest" in workflow
    assert "collaboration-conformance.xml" in workflow


def test_canonical_encoding_is_unicode_stable_and_rejects_ambiguous_json():
    assert collaboration.canonical_json({"z": "e\u0301", "a": [1, True, None]}) == (
        '{"a":[1,true,null],"z":"é"}'.encode()
    )
    with pytest.raises(collaboration.CollaborationError, match="unsafe_integer"):
        collaboration.canonical_json({"n": 2**53})
    with pytest.raises(collaboration.CollaborationError, match="floating_point_forbidden"):
        collaboration.canonical_json({"n": 1.25})
    with pytest.raises(collaboration.CollaborationError, match="duplicate_key"):
        collaboration.parse_canonical_event(b'{"a":1,"a":2}')
    nested = None
    for _ in range(34):
        nested = [nested]
    with pytest.raises(collaboration.CollaborationError, match="json_too_deep"):
        collaboration.canonical_json(nested)


def test_deep_causal_graph_analysis_does_not_depend_on_recursion_depth():
    events = {}
    previous = None
    for index in range(1_200):
        current = "sha256:" + f"{index:064x}"
        events[current] = {"parents": [] if previous is None else [previous]}
        previous = current
    ancestors = collaboration._ancestor_map(events)
    assert len(ancestors[previous]) == len(events) - 1


def test_migration_wraps_every_legacy_family_without_impersonating_authorship():
    corpus = build_corpus()
    legacy_fields = {
        "issue": {"title": "Legacy issue", "body": "Body"},
        "pull_request": {"title": "Legacy PR", "base": "main", "head": "topic"},
        "comment": {"body": "Legacy comment"},
        "label": {"name": "legacy", "color": "#123abc"},
        "reaction": {"emoji": "🎉"},
        "review": {"state": "approved", "revision": "c" * 40},
        "milestone": {"title": "Legacy milestone"},
        "board": {"name": "Legacy board"},
        "notification": {"kind": "legacy", "recipient": ACTORS["alice"]},
        "moderation": {"decision": "allow", "reasonCode": "legacy_import"},
    }
    legacy_schemas = {
        "issue": "forkmesh-issue-v1",
        "pull_request": "forkmesh-pull-v1",
        "comment": "forkmesh-comment-v1",
        "label": "forkmesh-label-v1",
        "reaction": "forkmesh-reaction-v1",
        "review": "forkmesh-review-v1",
        "milestone": "forkmesh-milestone-v1",
        "board": "forkmesh-project-v1",
        "notification": "forkmesh-notification-v1",
        "moderation": "forkmesh-moderation-v1",
    }
    protected = {"label", "milestone", "board"}
    refs = {
        "comment": [{"relation": "subject", "family": "issue", "object": corpus.objects["issue_create"]}],
        "reaction": [{"relation": "subject", "family": "comment", "object": corpus.objects["comment_create"]}],
        "review": [{"relation": "subject", "family": "pull_request", "object": corpus.objects["pull_create"]}],
        "notification": [{"relation": "subject", "family": "issue", "object": corpus.objects["issue_create"]}],
        "moderation": [{"relation": "subject", "family": "comment", "object": corpus.objects["comment_create"]}],
    }
    actor_sequences = {name: 100 for name in ACTORS}
    migrated = []
    for family in collaboration.FAMILIES:
        actor = (
            "moderator" if family == "moderation" else
            "notifier" if family == "notification" else
            "maintainer" if family in protected else "alice"
        )
        legacy = {"schema": legacy_schemas[family], **legacy_fields[family], "author": "legacy-user"}
        draft = collaboration.migration_draft(
            legacy,
            repository=corpus.repository,
            policy=collaboration.policy_id(corpus.policy),
            family=family,
            importer=ACTORS[actor],
            sequence=actor_sequences[actor],
            clock=0,
            issued_at=1,
            references=refs.get(family, []),
        )
        actor_sequences[actor] += 1
        migrated.append(
            collaboration.finish_event(draft, sign(actor, collaboration.signing_bytes(draft)))
        )

    result = collaboration.fold_events(migrated, corpus.policy)
    assert result.audit["rejected"] == []
    assert result.audit["pending"] == []
    assert all(result.state["objects"][family] for family in collaboration.FAMILIES)
    for event in migrated:
        assert event["author"] != "legacy-user"
        assert event["payload"]["legacy"]["signatureStatus"] == "unavailable"
        assert event["payload"]["legacy"]["digest"].startswith("sha256:")

    independent = _javascript_fold(migrated, corpus.policy)
    assert independent["state"] == result.state
    assert independent["stateDigest"] == result.state_digest
    assert independent["audit"] == result.audit
    assert independent["telemetry"] == result.telemetry


def test_mutation_fuzz_covers_every_family_without_admitting_corrupt_bytes():
    corpus = build_corpus()
    rng = random.Random(0x484)
    mutants = []
    create_by_family = {
        event["family"]: event
        for event in corpus.events
        if event["action"] in {"create", "decide"}
    }
    assert set(create_by_family) == set(collaboration.FAMILIES)
    for family in collaboration.FAMILIES:
        original = collaboration.canonical_json(create_by_family[family])
        for _ in range(20):
            changed = bytearray(original)
            index = rng.randrange(len(changed))
            changed[index] ^= rng.randrange(1, 256)
            mutants.append(bytes(changed))
    result = collaboration.fold_events(mutants, corpus.policy)
    assert result.telemetry["accepted"] == 0
    assert result.telemetry["rejected"] == len({hashlib.sha256(raw).digest() for raw in mutants})


def test_independent_javascript_verifier_has_adversarial_state_and_audit_parity():
    corpus = build_corpus()
    forged = dict(corpus.named["issue_create"])
    signature = bytearray(
        collaboration._b64url_decode(forged["signature"], 64, "test")
    )
    signature[-1] ^= 1
    forged["signature"] = collaboration._b64url(bytes(signature))
    forged["eventId"] = collaboration.event_id(forged)
    issue = corpus.named["issue_create"]
    malformed_unsigned = collaboration.unsigned_event(
        repository=corpus.repository,
        policy=collaboration.policy_id(corpus.policy),
        family="issue",
        object=issue["object"],
        action="tombstone",
        author=ACTORS["alice"],
        sequence=100_000,
        clock=issue["clock"] + 1,
        issued_at=2_100_000_000_000,
        parents=[issue["eventId"]],
        references=issue["references"],
        payload={"reasonCode": ["forged"]},
    )
    malformed = {
        **malformed_unsigned,
        "signature": sign(
            "alice", collaboration.signing_bytes(malformed_unsigned)
        ),
    }
    malformed["eventId"] = collaboration.event_id(malformed)
    noncanonical = json.dumps(corpus.named["pull_create"], indent=2)
    lone_surrogate = '{"payload":"\\ud800"}'
    deeply_nested = '{"a":' + "[" * 40 + "null" + "]" * 40 + "}"
    prototype_key = '{"__proto__":1}'
    events = (
        corpus.events
        + _unauthorized_family_events(corpus)
        + [
            forged,
            malformed,
            noncanonical,
            lone_surrogate,
            deeply_nested,
            prototype_key,
        ]
    )
    expected = collaboration.fold_events(events, corpus.policy)
    actual = _javascript_fold(events, corpus.policy)
    assert actual["state"] == expected.state
    assert actual["stateDigest"] == expected.state_digest
    assert actual["audit"] == expected.audit
    assert actual["telemetry"] == expected.telemetry


def test_fold_hot_path_budget_after_signature_cache_is_warm():
    corpus = build_corpus()
    collaboration.fold_events(corpus.events, corpus.policy)
    started = time.perf_counter()
    for _ in range(25):
        collaboration.fold_events(corpus.events, corpus.policy)
    elapsed = time.perf_counter() - started
    assert elapsed < 3.0
