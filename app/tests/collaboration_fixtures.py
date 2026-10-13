"""Deterministic signed corpus shared by collaboration protocol tests."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass

import collaboration


def _seed(name: str) -> bytes:
    return hashlib.sha256(("forkmesh-test-actor:" + name).encode()).digest()


def _private_scalar(seed: bytes) -> tuple[int, bytes]:
    expanded = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(expanded[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, expanded[32:]


def public_key(name: str) -> str:
    scalar, _prefix = _private_scalar(_seed(name))
    point = collaboration._scalar_mult(collaboration._BASE, scalar)
    return collaboration._b64url(collaboration._encode_point(point))


def sign(name: str, message: bytes) -> str:
    scalar, prefix = _private_scalar(_seed(name))
    public = collaboration._encode_point(
        collaboration._scalar_mult(collaboration._BASE, scalar)
    )
    nonce = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little")
    nonce %= collaboration._GROUP_L
    encoded_nonce = collaboration._encode_point(
        collaboration._scalar_mult(collaboration._BASE, nonce)
    )
    challenge = int.from_bytes(
        hashlib.sha512(encoded_nonce + public + message).digest(), "little"
    ) % collaboration._GROUP_L
    signature = encoded_nonce + (
        (nonce + challenge * scalar) % collaboration._GROUP_L
    ).to_bytes(32, "little")
    return collaboration._b64url(signature)


ACTORS = {
    name: public_key(name)
    for name in ("maintainer", "moderator", "notifier", "alice", "bob", "charlie")
}


@dataclass
class Corpus:
    repository: str
    policy: dict
    events: list[dict]
    objects: dict[str, str]
    named: dict[str, dict]


class Builder:
    def __init__(self) -> None:
        self.repository = collaboration.repository_id("git:forkmesh/conformance")
        self.policy = collaboration.policy_document(
            self.repository,
            maintainers=[ACTORS["maintainer"]],
            moderators=[ACTORS["moderator"]],
            notifiers=[ACTORS["notifier"]],
        )
        self.policy_id = collaboration.policy_id(self.policy)
        self.events: list[dict] = []
        self.by_id: dict[str, dict] = {}
        self.objects: dict[str, str] = {}
        self.references: dict[str, list[dict]] = {}
        self.creators: dict[str, str] = {}
        self.named: dict[str, dict] = {}
        self.sequences = {name: 0 for name in ACTORS}
        self.issued_at = 1_800_000_000_000

    def _next_sequence(self, actor: str) -> int:
        value = self.sequences[actor]
        self.sequences[actor] += 1
        return value

    def _finish(
        self,
        *,
        actor: str,
        family: str,
        current_object: str,
        action: str,
        payload: dict,
        parents: list[str],
        references: list[dict],
        name: str | None,
        sequence: int | None = None,
        issued_at: int | None = None,
    ) -> dict:
        parents = sorted(set(parents))
        references = sorted(
            references,
            key=lambda item: (item["relation"], item["family"], item["object"]),
        )
        clock = 0 if not parents else 1 + max(self.by_id[parent]["clock"] for parent in parents)
        unsigned = collaboration.unsigned_event(
            repository=self.repository,
            policy=self.policy_id,
            family=family,
            object=current_object,
            action=action,
            author=ACTORS[actor],
            sequence=self._next_sequence(actor) if sequence is None else sequence,
            clock=clock,
            issued_at=self.issued_at if issued_at is None else issued_at,
            parents=parents,
            references=references,
            payload=payload,
        )
        self.issued_at -= 17  # deliberately moves backwards; time is display-only
        event = collaboration.finish_event(
            unsigned, sign(actor, collaboration.signing_bytes(unsigned))
        )
        self.events.append(event)
        self.by_id[event["eventId"]] = event
        if name:
            self.named[name] = event
        return event

    def create(
        self,
        name: str,
        family: str,
        actor: str,
        fields: dict,
        *,
        references: list[dict] | None = None,
        parents: list[str] | None = None,
        legacy: dict | None = None,
    ) -> dict:
        refs = list(references or [])
        object_key = collaboration._b64url(
            hashlib.sha256(("object:" + name).encode()).digest()[:16]
        )
        current_object = collaboration.object_id(
            self.repository, family, ACTORS[actor], object_key, refs
        )
        payload = {"objectKey": object_key, "fields": fields}
        if legacy is not None:
            payload["legacy"] = legacy
        event = self._finish(
            actor=actor,
            family=family,
            current_object=current_object,
            action="decide" if family == "moderation" else "create",
            payload=payload,
            parents=list(parents or []),
            references=refs,
            name=name,
        )
        self.objects[name] = current_object
        self.references[current_object] = event["references"]
        self.creators[current_object] = actor
        return event

    def event(
        self,
        name: str,
        object_name: str,
        actor: str,
        action: str,
        payload: dict,
        parents: list[str],
        *,
        sequence: int | None = None,
    ) -> dict:
        current_object = self.objects[object_name]
        family = current_object.split(":")[3]
        return self._finish(
            actor=actor,
            family=family,
            current_object=current_object,
            action=action,
            payload=payload,
            parents=parents,
            references=self.references[current_object],
            name=name,
            sequence=sequence,
        )

    def reference(self, relation: str, family: str, object_name: str) -> dict:
        return {
            "relation": relation,
            "family": family,
            "object": self.objects[object_name],
        }


def build_corpus() -> Corpus:
    builder = Builder()

    label = builder.create(
        "label_create", "label", "maintainer", {"name": "security", "color": "#d73a4a"}
    )
    milestone = builder.create(
        "milestone_create",
        "milestone",
        "maintainer",
        {"title": "Public beta", "status": "open"},
    )
    issue = builder.create(
        "issue_create",
        "issue",
        "alice",
        {"title": "Converge", "body": "Initial body", "status": "open"},
    )
    pull = builder.create(
        "pull_create",
        "pull_request",
        "alice",
        {"title": "Implement", "body": "Patch", "base": "main", "head": "topic", "status": "open"},
    )
    comment = builder.create(
        "comment_create",
        "comment",
        "bob",
        {"body": "Review note"},
        references=[builder.reference("subject", "issue", "issue_create")],
        parents=[issue["eventId"]],
    )
    reaction = builder.create(
        "reaction_create",
        "reaction",
        "bob",
        {"emoji": "👍"},
        references=[builder.reference("subject", "comment", "comment_create")],
        parents=[comment["eventId"]],
    )
    review = builder.create(
        "review_create",
        "review",
        "charlie",
        {"state": "commented", "body": "Looking", "revision": "a" * 40},
        references=[builder.reference("subject", "pull_request", "pull_create")],
        parents=[pull["eventId"]],
    )
    board = builder.create(
        "board_create",
        "board",
        "maintainer",
        {"name": "Beta", "status": "open"},
    )
    notification = builder.create(
        "notification_create",
        "notification",
        "notifier",
        {"kind": "mention", "recipient": ACTORS["alice"], "status": "unread"},
        references=[builder.reference("subject", "comment", "comment_create")],
        parents=[comment["eventId"]],
    )
    moderation = builder.create(
        "moderation_decide",
        "moderation",
        "moderator",
        {"decision": "hide", "reasonCode": "spam"},
        references=[builder.reference("subject", "comment", "comment_create")],
        parents=[comment["eventId"]],
    )

    builder.event(
        "label_update",
        "label_create",
        "maintainer",
        "update",
        {"fields": {"description": "Security-sensitive work"}},
        [label["eventId"]],
    )
    builder.event(
        "label_attach",
        "label_create",
        "maintainer",
        "add",
        {
            "field": "subjects",
            "values": [{"family": "issue", "object": builder.objects["issue_create"]}],
        },
        [builder.named["label_update"]["eventId"]],
    )
    builder.event(
        "milestone_update",
        "milestone_create",
        "maintainer",
        "update",
        {"fields": {"dueAt": 1_900_000_000_000}},
        [milestone["eventId"]],
    )
    builder.event(
        "milestone_attach",
        "milestone_create",
        "maintainer",
        "add",
        {
            "field": "subjects",
            "values": [{"family": "issue", "object": builder.objects["issue_create"]}],
        },
        [builder.named["milestone_update"]["eventId"]],
    )
    builder.event(
        "issue_label",
        "issue_create",
        "alice",
        "add",
        {"field": "labels", "values": [builder.objects["label_create"]]},
        [issue["eventId"]],
    )
    issue_left = builder.event(
        "issue_title_left",
        "issue_create",
        "alice",
        "update",
        {"fields": {"title": "Converge left"}},
        [issue["eventId"]],
    )
    issue_right = builder.event(
        "issue_title_right",
        "issue_create",
        "maintainer",
        "update",
        {"fields": {"title": "Converge right"}},
        [issue["eventId"]],
    )
    issue_resolve = builder.event(
        "issue_title_resolve",
        "issue_create",
        "alice",
        "update",
        {"fields": {"title": "Converge explicitly"}},
        [issue_left["eventId"], issue_right["eventId"]],
    )
    issue_tombstone = builder.event(
        "issue_tombstone",
        "issue_create",
        "alice",
        "tombstone",
        {"reasonCode": "duplicate"},
        [issue_resolve["eventId"]],
    )
    builder.event(
        "issue_restore",
        "issue_create",
        "alice",
        "restore",
        {"reasonCode": "reopened", "observed": [issue_tombstone["eventId"]]},
        [issue_tombstone["eventId"]],
    )
    builder.event(
        "pull_merge",
        "pull_create",
        "maintainer",
        "update",
        {"fields": {"status": "merged", "mergeCommit": "b" * 40}},
        [pull["eventId"]],
    )
    comment_left = builder.event(
        "comment_left",
        "comment_create",
        "bob",
        "update",
        {"fields": {"body": "Left edit"}},
        [comment["eventId"]],
    )
    comment_right = builder.event(
        "comment_right",
        "comment_create",
        "bob",
        "update",
        {"fields": {"body": "Right edit"}},
        [comment["eventId"]],
    )
    builder.event(
        "comment_resolve",
        "comment_create",
        "bob",
        "update",
        {"fields": {"body": "Combined edit"}},
        [comment_left["eventId"], comment_right["eventId"]],
    )
    reaction_tombstone = builder.event(
        "reaction_tombstone",
        "reaction_create",
        "bob",
        "tombstone",
        {"reasonCode": "undo"},
        [reaction["eventId"]],
    )
    builder.event(
        "reaction_restore",
        "reaction_create",
        "bob",
        "restore",
        {"reasonCode": "redo", "observed": [reaction_tombstone["eventId"]]},
        [reaction_tombstone["eventId"]],
    )
    builder.event(
        "review_left",
        "review_create",
        "charlie",
        "update",
        {"fields": {"state": "approved"}},
        [review["eventId"]],
    )
    builder.event(
        "review_right",
        "review_create",
        "charlie",
        "update",
        {"fields": {"state": "changes_requested"}},
        [review["eventId"]],
    )
    builder.event(
        "board_item",
        "board_create",
        "maintainer",
        "add",
        {
            "field": "items",
            "values": [{"family": "issue", "object": builder.objects["issue_create"]}],
        },
        [board["eventId"]],
    )
    builder.event(
        "notification_read",
        "notification_create",
        "alice",
        "update",
        {"fields": {"status": "read"}},
        [notification["eventId"]],
    )
    appeal = builder.event(
        "moderation_appeal",
        "moderation_decide",
        "bob",
        "appeal",
        {"predecessor": moderation["eventId"], "reasonCode": "not_spam", "body": "Please review"},
        [moderation["eventId"]],
    )
    builder.event(
        "moderation_resolve",
        "moderation_decide",
        "moderator",
        "resolve",
        {
            "predecessor": moderation["eventId"],
            "appeal": appeal["eventId"],
            "fields": {"decision": "allow", "reasonCode": "appeal_upheld"},
        },
        [appeal["eventId"]],
    )

    return Corpus(
        repository=builder.repository,
        policy=builder.policy,
        events=builder.events,
        objects=builder.objects,
        named=builder.named,
    )
