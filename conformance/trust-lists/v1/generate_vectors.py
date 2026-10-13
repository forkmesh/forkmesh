#!/usr/bin/env python3
"""Build or check the shared forkmesh-trust-list-v1 vectors.

Every case carries a hand-written ``expectSummary`` (list states, effective
assertions, rejection codes, selected telemetry). The reference resolver
(app/src/trust_lists.py) must agree with it before its full resolution is
written to ``expect``; the JavaScript, Dart, and desktop C++ resolvers are then
held to that full output byte for byte. ``--check`` fails when the committed
file differs from what this script produces.

Keys come from deterministic test identities and MUST NOT be used outside
tests.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[2]
VECTORS = SUITE / "vectors.json"
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "app" / "tests"))

import collaboration  # noqa: E402
import trust_lists as tl  # noqa: E402
from collaboration_fixtures import public_key, sign  # noqa: E402


NOW = 1_800_000_000_000
DAY = 86_400_000
ISSUED = NOW - 10 * DAY
EXPIRES = NOW + 90 * DAY

ACTOR_NAMES = (
    "alice", "alice-recovery", "alice-successor", "bob", "charlie", "dave",
    "erin", "maintainer", "mallory", "moderator", "chain-a", "chain-b",
    "chain-c", "chain-d", "chain-e",
)
KEYS = {name: public_key(name) for name in ACTOR_NAMES}
NAMES_BY_KEY = {key: name for name, key in KEYS.items()}


def signed(unsigned: dict[str, Any], signer: str) -> str:
    return tl.sign_document(unsigned, lambda message: sign(signer, message))


def entry(subject: str, role: str, scope: str, stance: str = "trust", note: str = "") -> dict:
    return {"subject": KEYS[subject], "role": role, "scope": scope, "stance": stance, "note": note}


def edge(kind: str, issuer: str, name: str, roles: list[str], communities: list[str],
         expires_at: int | None = None) -> dict:
    return {"kind": kind, "issuer": KEYS[issuer], "name": name, "roles": roles,
            "communities": communities, "expiresAt": expires_at}


def trust_list(issuer: str, name: str, *, roles: list[str], communities: list[str],
               entries: list[dict] = (), edges: list[dict] = (), version: int = 1,
               previous: str | None = None, issued_at: int = ISSUED,
               expires_at: int = EXPIRES, revocation_keys: list[str] = (),
               signer: str | None = None, sort: bool = True, **extra: Any) -> str:
    entries = list(entries)
    edges = list(edges)
    if sort:
        entries.sort(key=lambda e: (e["subject"], e["role"], e["scope"]))
        edges.sort(key=lambda e: (e["issuer"], e["name"]))
    document = {
        "schema": tl.LIST_SCHEMA,
        "issuer": KEYS[issuer],
        "name": name,
        "version": version,
        "previous": tl.document_id(previous) if previous is not None else None,
        "issuedAt": issued_at,
        "expiresAt": expires_at,
        "roles": roles,
        "communities": communities,
        "revocationKeys": sorted(KEYS[k] for k in revocation_keys),
        "entries": entries,
        "edges": edges,
    }
    document.update(extra)
    return signed(document, signer or issuer)


def revocation(issuer: str, name: str, *, from_version: int, revoked_by: str,
               reason: str, successor: tuple[str, str] | None = None) -> str:
    return signed(
        {
            "schema": tl.REVOCATION_SCHEMA,
            "list": tl.list_id(KEYS[issuer], name),
            "issuer": KEYS[issuer],
            "name": name,
            "fromVersion": from_version,
            "revokedBy": KEYS[revoked_by],
            "revokedAt": NOW - DAY,
            "reason": reason,
            "successor": (
                {"issuer": KEYS[successor[0]], "name": successor[1]} if successor else None
            ),
        },
        revoked_by,
    )


def appeal(subject: str, issuer: str, name: str, *, version: int, role: str, scope: str,
           signer: str | None = None) -> str:
    return signed(
        {
            "schema": tl.APPEAL_SCHEMA,
            "subject": KEYS[subject],
            "list": tl.list_id(KEYS[issuer], name),
            "version": version,
            "role": role,
            "scope": scope,
            "statement": "The advisory was about a fork I do not maintain.",
            "issuedAt": NOW - DAY,
        },
        signer or subject,
    )


def follow(issuer: str, name: str, roles: list[str] | None = None,
           communities: list[str] | None = None) -> dict:
    return {"issuer": KEYS[issuer], "name": name, "roles": roles, "communities": communities}


def override(subject: str, role: str, scope: str, stance: str) -> dict:
    return {"subject": KEYS[subject], "role": role, "scope": scope, "stance": stance}


def resign_raw(raw: str, signer: str, **changes: Any) -> str:
    document = json.loads(raw)
    document.update(changes)
    document.pop("signature")
    return signed(document, signer)


def _b64(raw: bytes) -> str:
    return collaboration._b64url(raw)


def _forge(issuer_bytes: bytes, name: str, sign_for) -> str:
    """Sign a well-formed list for a weak key via ``sign_for(key, message)``."""

    document = {
        "schema": tl.LIST_SCHEMA, "issuer": _b64(issuer_bytes), "name": name, "version": 1,
        "previous": None, "issuedAt": ISSUED, "expiresAt": EXPIRES, "roles": ["reviewer"],
        "communities": ["org:forkmesh"], "revocationKeys": [], "edges": [],
        "entries": [entry("mallory", "reviewer", "org:forkmesh")],
    }
    return tl.sign_document(document, lambda message: sign_for(issuer_bytes, message))


def _challenge(nonce: bytes, key: bytes, message: bytes) -> int:
    import hashlib

    return int.from_bytes(hashlib.sha512(nonce + key + message).digest(), "little") % collaboration._GROUP_L


def weak_key_documents() -> list[str]:
    """Signatures that satisfy [S]B = R + [k]A for keys a strict verifier refuses."""

    base, group_l = collaboration._BASE, collaboration._GROUP_L
    mult, add, encode = collaboration._scalar_mult, collaboration._edwards_add, collaboration._encode_point
    order_two = (0, collaboration._FIELD_Q - 1)

    def identity_signer(_key: bytes, _message: bytes) -> str:
        return _b64(encode(mult(base, 5)) + (5).to_bytes(32, "little"))

    def torsion_signer(key: bytes, message: bytes) -> str:
        for scalar in range(1, 256):
            nonce = encode(mult(base, scalar))
            if _challenge(nonce, key, message) % 2 == 0:
                return _b64(nonce + scalar.to_bytes(32, "little"))
        raise SystemExit("no even challenge found")

    secret_scalar = 0x1234567890ABCDEF
    mixed = encode(add(mult(base, secret_scalar), order_two))

    def mixed_signer(key: bytes, message: bytes) -> str:
        for scalar in range(1, 256):
            nonce = encode(mult(base, scalar))
            challenge = _challenge(nonce, key, message)
            if challenge % 2 == 0:
                s_value = (scalar + challenge * secret_scalar) % group_l
                return _b64(nonce + s_value.to_bytes(32, "little"))
        raise SystemExit("no even challenge found")

    identity = (1).to_bytes(32, "little")
    identity_with_sign = bytes([1]) + bytes(30) + bytes([0x80])
    documents = [
        _forge(identity, "identity", identity_signer),
        _forge(identity_with_sign, "identity-sign-bit", identity_signer),
        _forge(encode(order_two), "order-two", torsion_signer),
        _forge(mixed, "mixed-order", mixed_signer),
    ]
    for raw in documents:
        document = json.loads(raw)
        key = collaboration._b64url_decode(document["issuer"], 32, "key")
        signature = collaboration._b64url_decode(document["signature"], 64, "sig")
        message = tl.signing_text(document).encode("ascii")
        nonce, scalar = collaboration._decode_point(signature[:32]), int.from_bytes(signature[32:], "little")
        point = collaboration._xrecover(int.from_bytes(key, "little") & ((1 << 255) - 1))
        y = int.from_bytes(key, "little") & ((1 << 255) - 1)
        x = point if (point & 1) == (key[31] >> 7) or point == 0 else collaboration._FIELD_Q - point
        lax = mult(base, scalar) == add(nonce, mult((x, y), _challenge(signature[:32], key, message)))
        if not lax:
            raise SystemExit(f"forged signature for {document['name']} does not satisfy the lax equation")
    return documents


SR = "security-researcher"


def security_list(**kwargs: Any) -> str:
    base = dict(
        roles=[SR],
        communities=["org:forkmesh"],
        entries=[
            entry("bob", SR, "org:forkmesh", note="Reported CVE-2026-0001 responsibly"),
            entry("charlie", SR, "repo:forkmesh/app", "distrust", note="Published a fake advisory"),
        ],
    )
    base.update(kwargs)
    return trust_list("alice", "security", **base)


def cases() -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []

    def case(case_id: str, description: str, documents: list[str], summary: dict, *,
             follows: list[dict] = (), overrides: list[dict] = (), now: int = NOW) -> None:
        out.append(
            {
                "id": case_id,
                "description": description,
                "input": {
                    "now": now,
                    "documents": list(documents),
                    "follows": list(follows),
                    "overrides": list(overrides),
                },
                "expectSummary": summary,
            }
        )

    sec = security_list()
    case(
        "follow-single-list",
        "A followed list contributes its entries within its own role and scope.",
        [sec],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "trust",
                f"charlie {SR} repo:forkmesh/app": "distrust",
            },
        },
        follows=[follow("alice", "security")],
    )
    case(
        "unfollowed-lists-grant-nothing",
        "Holding a valid list without following it yields no assertions.",
        [sec],
        {"states": {"alice/security": "active"}, "effective": {}},
    )

    wide = trust_list(
        "maintainer", "people", roles=["maintainer", "reviewer"],
        communities=["community:rust", "org:forkmesh"],
        entries=[
            entry("bob", "maintainer", "org:forkmesh"),
            entry("bob", "reviewer", "community:rust"),
            entry("charlie", "reviewer", "repo:forkmesh/app"),
        ],
    )
    case(
        "follow-narrowing",
        "A local follow narrows a list to one role and one repository.",
        [wide],
        {
            "states": {"maintainer/people": "active"},
            "effective": {"charlie reviewer repo:forkmesh/app": "trust"},
        },
        follows=[follow("maintainer", "people", ["reviewer"], ["repo:forkmesh/app"])],
    )

    broad_security = trust_list(
        "alice", "security", roles=[SR], communities=["org:forkmesh", "org:other"],
        entries=[
            entry("bob", SR, "org:forkmesh"),
            entry("charlie", SR, "repo:forkmesh/app", "distrust"),
            entry("dave", SR, "org:other"),
        ],
    )
    mods = trust_list(
        "moderator", "mods", roles=["moderator", SR], communities=["org:forkmesh"],
        entries=[entry("alice", "moderator", "org:forkmesh")],
        edges=[edge("include", "alice", "security", [SR], ["repo:forkmesh/app"])],
    )
    case(
        "include-composes-within-intersection",
        "An included list contributes only inside the intersection of both scopes.",
        [mods, broad_security],
        {
            "states": {"alice/security": "active", "moderator/mods": "active"},
            "effective": {
                "alice moderator org:forkmesh": "trust",
                f"charlie {SR} repo:forkmesh/app": "distrust",
            },
        },
        follows=[follow("moderator", "mods")],
    )

    reviewers = trust_list(
        "maintainer", "reviewers", roles=["reviewer"], communities=["org:forkmesh"],
        edges=[
            edge("delegate", "bob", "app-reviewers", ["reviewer"], ["repo:forkmesh/app"], NOW + DAY),
            edge("delegate", "charlie", "old-reviewers", ["reviewer"], ["repo:forkmesh/app"], NOW - DAY),
        ],
    )
    bob_reviewers = trust_list(
        "bob", "app-reviewers", roles=["reviewer"], communities=["org:forkmesh"],
        entries=[entry("dave", "reviewer", "repo:forkmesh/app"), entry("erin", "reviewer", "repo:forkmesh/site")],
    )
    charlie_reviewers = trust_list(
        "charlie", "old-reviewers", roles=["reviewer"], communities=["repo:forkmesh/app"],
        entries=[entry("mallory", "reviewer", "repo:forkmesh/app")],
    )
    case(
        "delegation-expiry",
        "A live delegation admits the delegate's entries in the delegated scope; an expired one admits nothing.",
        [reviewers, bob_reviewers, charlie_reviewers],
        {
            "states": {
                "maintainer/reviewers": "active",
                "bob/app-reviewers": "active",
                "charlie/old-reviewers": "active",
            },
            "effective": {"dave reviewer repo:forkmesh/app": "trust"},
            "telemetry": {"expiredDelegations": 1},
        },
        follows=[follow("maintainer", "reviewers")],
    )

    sec_trust = trust_list(
        "alice", "security", roles=[SR], communities=["org:forkmesh"],
        entries=[entry("bob", SR, "org:forkmesh"), entry("charlie", SR, "org:forkmesh")],
    )
    mods_distrust = trust_list(
        "moderator", "mods", roles=[SR], communities=["org:forkmesh"],
        entries=[
            entry("bob", SR, "org:forkmesh", "distrust"),
            entry("charlie", SR, "org:forkmesh", "distrust"),
        ],
    )
    case(
        "conflict-visible-and-local-override",
        "Disagreeing lists never pick a quiet winner; a local override decides one tuple.",
        [sec_trust, mods_distrust],
        {
            "states": {"alice/security": "active", "moderator/mods": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "trust",
                f"charlie {SR} org:forkmesh": "conflict",
            },
            "telemetry": {"stanceConflicts": 2, "overrides": 1},
        },
        follows=[follow("alice", "security"), follow("moderator", "mods")],
        overrides=[override("bob", SR, "org:forkmesh", "trust")],
    )
    case(
        "override-ignore-and-local-only",
        "An override can ignore a list's assertion or add a local-only one.",
        [sec],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "ignore",
                f"charlie {SR} repo:forkmesh/app": "distrust",
                "dave maintainer repo:forkmesh/app": "trust",
            },
        },
        follows=[follow("alice", "security")],
        overrides=[
            override("bob", SR, "org:forkmesh", "ignore"),
            override("dave", "maintainer", "repo:forkmesh/app", "trust"),
        ],
    )

    v1 = trust_list("alice", "security", roles=[SR], communities=["org:forkmesh"],
                    entries=[entry("bob", SR, "org:forkmesh")], revocation_keys=["alice-recovery"])
    v2 = trust_list("alice", "security", roles=[SR], communities=["org:forkmesh"], version=2,
                    previous=v1, issued_at=ISSUED + DAY,
                    entries=[entry("bob", SR, "org:forkmesh", "distrust")],
                    revocation_keys=["alice-recovery"])
    case(
        "version-history-head",
        "The newest consistent version is the head.",
        [v2, v1],
        {"states": {"alice/security": "active"}, "effective": {f"bob {SR} org:forkmesh": "distrust"}},
        follows=[follow("alice", "security")],
    )
    v3_fork = trust_list("alice", "security", roles=[SR], communities=["org:forkmesh"], version=3,
                         previous=v1, issued_at=ISSUED + 2 * DAY,
                         entries=[entry("mallory", SR, "org:forkmesh")])
    case(
        "history-fork-recovers",
        "A version whose previous digest skips its predecessor cuts the list back.",
        [v1, v2, v3_fork],
        {
            "states": {"alice/security": "recovered"},
            "effective": {f"bob {SR} org:forkmesh": "distrust"},
        },
        follows=[follow("alice", "security")],
    )
    v2_equivocal = trust_list("alice", "security", roles=[SR], communities=["org:forkmesh"], version=2,
                              previous=v1, issued_at=ISSUED + DAY,
                              entries=[entry("mallory", SR, "org:forkmesh")])
    case(
        "equivocation-recovers",
        "Two different documents for one version cut the list back below that version.",
        [v1, v2, v2_equivocal],
        {"states": {"alice/security": "recovered"}, "effective": {f"bob {SR} org:forkmesh": "trust"}},
        follows=[follow("alice", "security")],
    )

    v1_equivocal = trust_list("alice", "security", roles=[SR], communities=["org:forkmesh"],
                              entries=[entry("mallory", SR, "org:forkmesh")], revocation_keys=["alice-recovery"])
    case(
        "equivocation-at-first-version-conflicted",
        "Two different first versions leave no usable version; the list is conflicted, not silently chosen.",
        [v1, v1_equivocal, v2],
        {"states": {"alice/security": "conflicted"}, "effective": {}},
        follows=[follow("alice", "security")],
    )

    v3_malicious = trust_list(
        "alice", "security", roles=[SR], communities=["org:forkmesh"], version=3, previous=v2,
        issued_at=ISSUED + 2 * DAY,
        entries=[
            entry("bob", SR, "org:forkmesh", "distrust"),
            entry("maintainer", SR, "org:forkmesh", "distrust", note="Compromised maintainer"),
            entry("mallory", SR, "org:forkmesh", note="Trusted auditor"),
        ],
        revocation_keys=["mallory"],
    )
    successor = trust_list("alice-successor", "security", roles=[SR], communities=["org:forkmesh"],
                           issued_at=NOW - DAY, entries=[entry("bob", SR, "org:forkmesh", "distrust")])
    compromise_revocation = revocation("alice", "security", from_version=3, revoked_by="alice-recovery",
                                       reason="compromised", successor=("alice-successor", "security"))
    case(
        "compromise-drill-before-revocation",
        "Drill step 1: a stolen issuer key publishes a malicious version that followers accept.",
        [v1, v2, v3_malicious],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "distrust",
                f"maintainer {SR} org:forkmesh": "distrust",
                f"mallory {SR} org:forkmesh": "trust",
            },
        },
        follows=[follow("alice", "security")],
    )
    case(
        "compromise-drill-revoked-and-recovered",
        "Drill step 2: the pre-committed recovery key revokes from the malicious version; followers fall back to the last good version and see the successor.",
        [v1, v2, v3_malicious, compromise_revocation, successor],
        {
            "states": {"alice/security": "recovered", "alice-successor/security": "active"},
            "effective": {f"bob {SR} org:forkmesh": "distrust"},
            "telemetry": {"revocations": 1},
        },
        follows=[follow("alice", "security"), follow("alice-successor", "security")],
    )
    attacker_revocation = revocation("alice", "security", from_version=1, revoked_by="mallory",
                                     reason="malicious")
    late_key_revocation = revocation("alice", "security", from_version=2, revoked_by="mallory",
                                     reason="malicious")
    case(
        "unauthorized-revocations-rejected",
        "A key never named before the revoked version cannot revoke the list.",
        [v1, v2, v3_malicious, attacker_revocation, late_key_revocation],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "distrust",
                f"maintainer {SR} org:forkmesh": "distrust",
                f"mallory {SR} org:forkmesh": "trust",
            },
            "rejected": ["unauthorized_revoker", "unauthorized_revoker"],
        },
        follows=[follow("alice", "security")],
    )
    case(
        "issuer-retires-list",
        "The issuer revokes every version; the list contributes nothing and says so.",
        [sec, revocation("alice", "security", from_version=1, revoked_by="alice", reason="retired")],
        {"states": {"alice/security": "revoked"}, "effective": {}},
        follows=[follow("alice", "security")],
    )
    case(
        "expired-list",
        "An expired head contributes nothing; no older version is revived.",
        [security_list(issued_at=NOW - 30 * DAY, expires_at=NOW - 1)],
        {"states": {"alice/security": "expired"}, "effective": {}},
        follows=[follow("alice", "security")],
    )
    case(
        "missing-followed-list",
        "A followed list with no verified documents is visible as missing.",
        [],
        {"states": {"alice/security": "missing"}, "effective": {}},
        follows=[follow("alice", "security")],
    )
    retired = revocation("alice", "security", from_version=1, revoked_by="alice", reason="retired")
    case(
        "revoked-include-drops-out",
        "A revoked included list stops contributing while the including list keeps working.",
        [mods, broad_security, retired],
        {
            "states": {"alice/security": "revoked", "moderator/mods": "active"},
            "effective": {"alice moderator org:forkmesh": "trust"},
        },
        follows=[follow("moderator", "mods")],
    )

    chain = ["chain-a", "chain-b", "chain-c", "chain-d", "chain-e"]
    subjects = ["bob", "charlie", "dave", "erin", "mallory"]
    chain_docs = []
    for index, owner in enumerate(chain):
        edges = []
        if index + 1 < len(chain):
            edges.append(edge("include", chain[index + 1], "chain", ["reviewer"], ["org:forkmesh"]))
        if owner == "chain-d":
            edges.append(edge("include", "chain-a", "chain", ["reviewer"], ["org:forkmesh"]))
        chain_docs.append(trust_list(owner, "chain", roles=["reviewer"], communities=["org:forkmesh"],
                                     entries=[entry(subjects[index], "reviewer", "org:forkmesh")],
                                     edges=edges))
    case(
        "cycles-and-depth",
        "Composition stops at four lists and never loops.",
        chain_docs,
        {
            "states": {f"{owner}/chain": "active" for owner in chain},
            "effective": {f"{s} reviewer org:forkmesh": "trust" for s in subjects[:4]},
            "telemetry": {"depthLimitedEdges": 1, "cycleEdges": 1},
        },
        follows=[follow("chain-a", "chain")],
    )

    appeal_v1 = appeal("charlie", "alice", "security", version=1, role=SR, scope="repo:forkmesh/app")
    case(
        "appeal-open",
        "A subject's signed appeal is attached to the assertion it contests.",
        [sec, appeal_v1],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "trust",
                f"charlie {SR} repo:forkmesh/app": "distrust",
            },
            "telemetry": {"appeals": {"open": 1, "resolved": 0, "unattached": 0}},
        },
        follows=[follow("alice", "security")],
    )
    sec_v2 = security_list(
        version=2, previous=sec, issued_at=ISSUED + DAY,
        entries=[
            entry("bob", SR, "org:forkmesh"),
            entry("charlie", SR, "repo:forkmesh/app", note="Advisory withdrawn after appeal"),
        ],
    )
    forged_appeal = appeal("charlie", "alice", "security", version=1, role=SR, scope="org:forkmesh",
                           signer="mallory")
    stray_appeal = appeal("dave", "moderator", "mods", version=1, role="moderator", scope="org:forkmesh")
    case(
        "appeal-resolved-forged-and-unattached",
        "A later version without the contested distrust resolves the appeal; forged and stray appeals are not attached.",
        [sec, sec_v2, appeal_v1, forged_appeal, stray_appeal],
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "trust",
                f"charlie {SR} repo:forkmesh/app": "trust",
            },
            "rejected": ["invalid_signature"],
            "telemetry": {"appeals": {"open": 0, "resolved": 1, "unattached": 1}},
        },
        follows=[follow("alice", "security")],
    )

    case(
        "forged-weak-keys",
        "Identity, small-order, and mixed-order issuer keys are rejected even when a cofactorless equation holds for the forged signature.",
        weak_key_documents(),
        {"states": {}, "effective": {}, "rejected": ["invalid_signature"] * 4},
    )
    case(
        "malformed-structure",
        "A non-string schema and excessive nesting are rejected before any field is trusted.",
        ['{"schema":[]}', '{"schema":7}', "[" * 9 + "]" * 9, '{"a":' + "[" * 8 + "]" * 8 + "}"],
        {"states": {}, "effective": {}, "rejected": ["invalid_json", "invalid_json", "unsupported_schema", "unsupported_schema"]},
    )

    def tamper(raw: str, index: int) -> str:
        at = raw.index('"signature":"') + len('"signature":"') + index
        return raw[:at] + ("B" if raw[at] != "B" else "C") + raw[at + 1:]

    signature = collaboration._b64url_decode(json.loads(sec)["signature"], 64, "sig")
    malleated = int.from_bytes(signature[32:], "little") + collaboration._GROUP_L
    malleated_sig = collaboration._b64url(signature[:32] + malleated.to_bytes(32, "little"))
    malleated_doc = json.loads(sec)
    malleated_doc["signature"] = malleated_sig
    rejected_docs = [
        tamper(sec, 10),
        sec.replace('{"communities"', '{ "communities"', 1),
        sec[:-5],
        "[]",
        resign_raw(sec, "alice", schema="forkmesh-trust-list-v2"),
        resign_raw(sec, "alice", extra=True),
        tl.canonical_json(malleated_doc),
        trust_list("alice", "wrong-signer", roles=[SR], communities=["org:forkmesh"], signer="bob"),
        trust_list("alice", "outside", roles=[SR], communities=["repo:forkmesh/app"],
                   entries=[entry("bob", SR, "org:forkmesh")]),
        trust_list("alice", "unsorted", roles=[SR], communities=["org:forkmesh"], sort=False,
                   entries=sorted([entry("bob", SR, "org:forkmesh"), entry("charlie", SR, "org:forkmesh")],
                                  key=lambda e: e["subject"], reverse=True)),
        trust_list("alice", "no-previous", roles=[SR], communities=["org:forkmesh"], version=2),
        trust_list("alice", "long-lived", roles=[SR], communities=["org:forkmesh"],
                   expires_at=ISSUED + 401 * DAY),
        trust_list("alice", "self-edge", roles=[SR], communities=["org:forkmesh"],
                   edges=[edge("include", "alice", "self-edge", [SR], ["org:forkmesh"])]),
        trust_list("alice", "wide-edge", roles=[SR], communities=["org:forkmesh"],
                   edges=[edge("include", "bob", "other", ["reviewer"], ["org:forkmesh"])]),
        trust_list("alice", "own-recovery", roles=[SR], communities=["org:forkmesh"],
                   revocation_keys=["alice"]),
        trust_list("alice", "global", roles=[SR], communities=["*"]),
        trust_list("alice", "future", roles=[SR], communities=["org:forkmesh"],
                   issued_at=NOW + 600_000, expires_at=NOW + 30 * DAY),
        sec,
        sec,
    ]
    case(
        "rejections",
        "Each malformed, forged, or out-of-scope document is rejected with a stable code and never contributes.",
        rejected_docs,
        {
            "states": {"alice/security": "active"},
            "effective": {
                f"bob {SR} org:forkmesh": "trust",
                f"charlie {SR} repo:forkmesh/app": "distrust",
            },
            "rejected": sorted([
                "invalid_signature", "noncanonical", "invalid_json", "unsupported_schema",
                "unsupported_schema", "invalid_keys", "invalid_signature", "invalid_signature",
                "entry_outside_scope", "unsorted_entries", "invalid_previous", "invalid_time",
                "invalid_edge", "edge_outside_scope", "invalid_revocation_keys", "invalid_scope",
                "issued_in_future",
            ]),
            "telemetry": {"duplicateDocuments": 1},
        },
        follows=[follow("alice", "security")],
    )
    return out


def _label(key: str) -> str:
    return NAMES_BY_KEY.get(key, key)


def summarize(resolution: dict[str, Any]) -> dict[str, Any]:
    return {
        "states": {
            f"{_label(row['issuer'])}/{row['name']}": row["state"] for row in resolution["lists"]
        },
        "effective": {
            f"{_label(a['subject'])} {a['role']} {a['scope']}": a["effective"]
            for a in resolution["assertions"]
        },
        "rejected": sorted(item["code"] for item in resolution["rejected"]),
    }


def build() -> dict[str, Any]:
    built = []
    for item in cases():
        data = item["input"]
        resolution = tl.resolve(
            data["documents"], now=data["now"], follows=data["follows"], overrides=data["overrides"]
        )
        reversed_resolution = tl.resolve(
            list(reversed(data["documents"])), now=data["now"],
            follows=list(reversed(data["follows"])), overrides=list(reversed(data["overrides"])),
        )
        if reversed_resolution != resolution:
            raise SystemExit(f"{item['id']}: resolution depends on input order")
        wanted = item["expectSummary"]
        got = summarize(resolution)
        for field in ("states", "effective"):
            if got[field] != wanted[field]:
                raise SystemExit(f"{item['id']}: {field} {got[field]} != {wanted[field]}")
        if got["rejected"] != sorted(wanted.get("rejected", [])):
            raise SystemExit(f"{item['id']}: rejected {got['rejected']} != {wanted.get('rejected', [])}")
        for key, value in wanted.get("telemetry", {}).items():
            if resolution["telemetry"][key] != value:
                raise SystemExit(f"{item['id']}: telemetry {key} {resolution['telemetry'][key]} != {value}")
        item["expect"] = resolution
        built.append(item)
    return {
        "schema": "forkmesh-trust-list-vectors-v1",
        "specification": "www/docs/design/scoped-trust-lists.md",
        "reference": "app/src/trust_lists.py",
        "testKeys": {name: KEYS[name] for name in ACTOR_NAMES},
        "cases": built,
    }


def serialize(vectors: dict[str, Any]) -> str:
    return json.dumps(vectors, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail when vectors.json is stale")
    args = parser.parse_args()
    text = serialize(build())
    if args.check:
        if not VECTORS.exists() or VECTORS.read_text(encoding="utf-8") != text:
            print("conformance/trust-lists/v1/vectors.json is stale; run generate_vectors.py", file=sys.stderr)
            return 1
        return 0
    VECTORS.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
