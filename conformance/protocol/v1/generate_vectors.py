#!/usr/bin/env python3
"""Regenerate ForkMesh protocol suite v1 vectors and manifest digests.

Every expectation is computed by a reference implementation in ``app/src`` and
compared with the expectation written here, so a reference change that alters
wire behavior fails generation instead of silently rewriting the contract.
``--check`` exits 1 when committed vectors or manifest digests differ from a
fresh generation; it never writes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Callable

SUITE_DIR = Path(__file__).resolve().parent
ROOT = SUITE_DIR.parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "app" / "tests"))

import collaboration as collab  # noqa: E402
import git_http  # noqa: E402
import portable_sessions  # noqa: E402
import repository_migration  # noqa: E402
from collaboration_fixtures import ACTORS, Builder, public_key, sign  # noqa: E402

SUITE = "forkmesh-protocol"
SUITE_VERSION = "1.0.0-rc.1"
VECTOR_DIR = SUITE_DIR / "vectors"
MANIFEST = SUITE_DIR / "manifest.json"


class GenerationError(AssertionError):
    pass


def _expect(case_id: str, actual: Any, wanted: Any) -> None:
    if actual != wanted:
        raise GenerationError(f"{case_id}: reference produced {actual!r}, expected {wanted!r}")


def _collab_error(call: Callable[[], Any]) -> str | None:
    try:
        call()
    except collab.CollaborationError as exc:
        return exc.code
    return None


def _raw_fields(raw: bytes) -> dict[str, str]:
    fields = {"rawHex": raw.hex()}
    try:
        fields["rawText"] = raw.decode("utf-8")
    except UnicodeDecodeError:
        pass
    return fields


def _vector_file(name: str, procedure: str, **body: Any) -> dict[str, Any]:
    return {
        "suite": SUITE,
        "suiteVersion": SUITE_VERSION,
        "vectorSet": name,
        "procedure": procedure,
        **body,
    }


# --------------------------------------------------------------------------
# canonical-json


def canonical_json_vectors() -> dict[str, Any]:
    deep_ok = b'{"a":' + b"[" * 32 + b"]" * 32 + b"}"
    deep_bad = b'{"a":' + b"[" * 33 + b"]" * 33 + b"}"
    parse = [
        ("parse-minimal-object", ["FM-ENC-001"], b'{"a":1}', None),
        (
            "parse-sorted-nested-values",
            ["FM-ENC-001", "FM-ENC-002"],
            b'{"a":[true,false,null],"b":{"c":"d"},"z":-5}',
            None,
        ),
        (
            "parse-short-and-control-escapes",
            ["FM-ENC-003"],
            collab.canonical_json({"a": '"\\\b\f\n\r\t\x00\x1f/\x7f'}),
            None,
        ),
        (
            "parse-literal-non-ascii",
            ["FM-ENC-003"],
            collab.canonical_json({"a": "\u00e9\u2028\u2029\U0001F600"}),
            None,
        ),
        (
            "parse-safe-integer-bounds",
            ["FM-ENC-002"],
            b'{"a":9007199254740991,"b":-9007199254740991}',
            None,
        ),
        ("parse-depth-32", ["FM-ENC-005"], deep_ok, None),
        ("reject-insignificant-whitespace", ["FM-ENC-004"], b'{"a": 1}', "noncanonical_encoding"),
        ("reject-unsorted-keys", ["FM-ENC-001", "FM-ENC-004"], b'{"b":1,"a":2}', "noncanonical_encoding"),
        ("reject-trailing-newline", ["FM-ENC-001", "FM-ENC-004"], b'{"a":1}\n', "noncanonical_encoding"),
        ("reject-escaped-non-ascii", ["FM-ENC-003", "FM-ENC-004"], b'{"a":"\\u00e9"}', "noncanonical_encoding"),
        ("reject-escaped-solidus", ["FM-ENC-003", "FM-ENC-004"], b'{"a":"\\/"}', "noncanonical_encoding"),
        ("reject-uppercase-unicode-escape", ["FM-ENC-003", "FM-ENC-004"], b'{"a":"\\u001F"}', "noncanonical_encoding"),
        ("reject-non-nfc-string", ["FM-ENC-003", "FM-ENC-004"], '{"a":"e\u0301"}'.encode(), "noncanonical_encoding"),
        ("reject-duplicate-key", ["FM-ENC-004"], b'{"a":1,"a":1}', "duplicate_key"),
        ("reject-fraction", ["FM-ENC-002"], b'{"a":1.5}', "floating_point_forbidden"),
        ("reject-integral-fraction", ["FM-ENC-002"], b'{"a":1.0}', "floating_point_forbidden"),
        ("reject-exponent", ["FM-ENC-002"], b'{"a":1e2}', "floating_point_forbidden"),
        ("reject-negative-zero", ["FM-ENC-002", "FM-ENC-004"], b'{"a":-0}', "noncanonical_encoding"),
        ("reject-leading-zero", ["FM-ENC-002"], b'{"a":01}', "invalid_json"),
        ("reject-unsafe-integer", ["FM-ENC-002"], b'{"a":9007199254740992}', "unsafe_integer"),
        ("reject-nan", ["FM-ENC-002"], b'{"a":NaN}', "invalid_number"),
        ("reject-lone-surrogate", ["FM-ENC-003"], b'{"a":"\\ud800"}', "invalid_unicode"),
        ("reject-invalid-key-character", ["FM-ENC-001"], b'{"a b":1}', "invalid_key"),
        ("reject-byte-order-mark", ["FM-ENC-001"], b'\xef\xbb\xbf{"a":1}', "invalid_json"),
        ("reject-invalid-utf8", ["FM-ENC-001"], b'{"a":"\xff"}', "invalid_json"),
        ("reject-overlong-utf8", ["FM-ENC-001"], b'{"a":"\xc0\xaf"}', "invalid_json"),
        ("reject-top-level-array", ["FM-ENC-001"], b"[1]", "event_not_object"),
        ("reject-empty", ["FM-ENC-005"], b"", "invalid_event_size"),
        ("reject-depth-33", ["FM-ENC-005"], deep_bad, "json_too_deep"),
    ]
    parse_cases = []
    for case_id, requirements, raw, wanted in parse:
        _expect(case_id, _collab_error(lambda raw=raw: collab.parse_canonical_event(raw)), wanted)
        parse_cases.append({
            "id": case_id,
            "requirements": requirements,
            **_raw_fields(raw),
            "expect": {"valid": True} if wanted is None else {"valid": False, "error": wanted},
        })

    encode = [
        ("encode-sorts-keys", ["FM-ENC-001"], {"b": 1, "a": {"d": [3, 2], "c": None}}, '{"a":{"c":null,"d":[3,2]},"b":1}'),
        ("encode-normalizes-nfc", ["FM-ENC-003"], {"t": "e\u0301"}, '{"t":"\u00e9"}'),
        (
            "encode-escapes",
            ["FM-ENC-003"],
            {"t": "\b\f\n\r\t\"\\\x01\x7f/\u2028"},
            '{"t":"\\b\\f\\n\\r\\t\\"\\\\\\u0001\x7f/\u2028"}',
        ),
        ("encode-negative-integer", ["FM-ENC-002"], {"n": -9007199254740991}, '{"n":-9007199254740991}'),
    ]
    encode_rejects = [
        ("encode-rejects-unsafe-integer", ["FM-ENC-002"], {"n": 9007199254740992}),
        ("encode-rejects-fraction", ["FM-ENC-002"], {"n": 0.5}),
        ("encode-rejects-empty-key", ["FM-ENC-001"], {"": 1}),
        ("encode-rejects-lone-surrogate", ["FM-ENC-003"], {"t": "\ud800"}),
    ]
    encode_cases = []
    for case_id, requirements, value, wanted in encode:
        _expect(case_id, collab.canonical_json(value).decode("utf-8"), wanted)
        encode_cases.append({
            "id": case_id,
            "requirements": requirements,
            "value": value,
            "expect": {"valid": True, "canonicalText": wanted},
        })
    for case_id, requirements, value in encode_rejects:
        code = _collab_error(lambda value=value: collab.canonical_json(value))
        if code is None:
            raise GenerationError(f"{case_id}: reference accepted a forbidden value")
        encode_cases.append({
            "id": case_id,
            "requirements": requirements,
            "value": value,
            "expect": {"valid": False, "error": code},
        })

    return _vector_file(
        "canonical-json",
        "parse: decode rawHex, parse it as one canonical document (FM-ENC-004) and "
        "accept or reject; encode: parse the JSON value as written in this file and "
        "emit canonical bytes (FM-ENC-001..003) or reject.",
        parse=parse_cases,
        encode=encode_cases,
    )


# --------------------------------------------------------------------------
# identifiers


def _pkt(payload: str) -> bytes:
    data = payload.encode("utf-8")
    return b"%04x" % (len(data) + 4) + data


def identifier_vectors() -> dict[str, Any]:
    repository_cases = []
    for case_id, value, wanted_error in [
        ("repository-ascii-root", "git:forkmesh/conformance", None),
        ("repository-precomposed-root", "git:caf\u00e9", None),
        ("repository-decomposed-root-normalizes", "git:cafe\u0301", None),
        ("repository-empty-root", "", "invalid_repository_identity"),
    ]:
        if wanted_error:
            _expect(case_id, _collab_error(lambda value=value: collab.repository_id(value)), wanted_error)
            expect = {"valid": False, "error": wanted_error}
        else:
            expect = {"valid": True, "repositoryId": collab.repository_id(value)}
        repository_cases.append({
            "id": case_id,
            "requirements": ["FM-ID-001"],
            "verifiedRoot": value,
            "expect": expect,
        })
    _expect(
        "repository-decomposed-root-normalizes",
        repository_cases[2]["expect"]["repositoryId"],
        repository_cases[1]["expect"]["repositoryId"],
    )

    repository = collab.repository_id("git:forkmesh/conformance")
    valid_policy = collab.policy_document(
        repository,
        maintainers=[ACTORS["maintainer"]],
        moderators=[ACTORS["moderator"]],
        notifiers=[ACTORS["notifier"]],
    )
    two_maintainers = collab.policy_document(
        repository, maintainers=[ACTORS["maintainer"], ACTORS["alice"]]
    )
    unsorted = dict(two_maintainers, maintainers=list(reversed(two_maintainers["maintainers"])))
    duplicate = dict(valid_policy, maintainers=[ACTORS["alice"], ACTORS["alice"]])
    extra = dict(valid_policy, owners=[])
    bad_key = dict(valid_policy, moderators=["AAAA"])
    future = dict(valid_policy, schema="forkmesh-collaboration-policy-v2")
    policy_cases = []
    for case_id, requirements, document, wanted_error in [
        ("policy-single-role-members", ["FM-ID-002"], valid_policy, None),
        ("policy-sorted-members", ["FM-ID-002"], two_maintainers, None),
        ("policy-unsorted-members", ["FM-ID-002"], unsorted, "noncanonical_policy"),
        ("policy-duplicate-member", ["FM-ID-002"], duplicate, "noncanonical_policy"),
        ("policy-unknown-key", ["FM-ID-002"], extra, "invalid_policy"),
        ("policy-short-key", ["FM-ID-002", "FM-ENC-006"], bad_key, "invalid_policy_key"),
        ("policy-future-schema", ["FM-ID-002", "FM-VER-001"], future, "invalid_policy"),
    ]:
        code = _collab_error(lambda document=document: collab.policy_id(document))
        _expect(case_id, code, wanted_error)
        policy_cases.append({
            "id": case_id,
            "requirements": requirements,
            "policy": document,
            "expect": {"valid": True, "policyId": collab.policy_id(document)}
            if wanted_error is None
            else {"valid": False, "error": wanted_error},
        })

    def key(name: str, size: int = 16) -> str:
        return collab._b64url(hashlib.sha256(("object:" + name).encode()).digest()[:size])

    issue_key = key("issue")
    issue_id = collab.object_id(repository, "issue", ACTORS["alice"], issue_key)
    subject = [{"relation": "subject", "family": "issue", "object": issue_id}]
    object_cases = []
    for case_id, requirements, family, creator, object_key, references, wanted_error in [
        ("object-issue", ["FM-ID-003"], "issue", ACTORS["alice"], issue_key, [], None),
        ("object-comment-with-subject", ["FM-ID-003"], "comment", ACTORS["bob"], key("comment"), subject, None),
        ("object-short-object-key", ["FM-ID-003", "FM-ENC-006"], "issue", ACTORS["alice"], key("short", 15), [], "invalid_object_key"),
        ("object-padded-object-key", ["FM-ID-003", "FM-ENC-006"], "issue", ACTORS["alice"], issue_key + "==", [], "invalid_object_key"),
        ("object-comment-without-subject", ["FM-ID-003"], "comment", ACTORS["bob"], key("comment"), [], "comment_subject_required"),
        ("object-issue-with-subject", ["FM-ID-003"], "issue", ACTORS["bob"], key("issue2"), subject, "unexpected_subject"),
        ("object-unknown-family", ["FM-ID-003", "FM-VER-001"], "wiki", ACTORS["bob"], key("wiki"), [], "invalid_object_identity_input"),
    ]:
        call = lambda family=family, creator=creator, object_key=object_key, references=references: collab.object_id(  # noqa: E731
            repository, family, creator, object_key, references
        )
        _expect(case_id, _collab_error(call), wanted_error)
        object_cases.append({
            "id": case_id,
            "requirements": requirements,
            "repository": repository,
            "family": family,
            "creator": creator,
            "objectKey": object_key,
            "references": references,
            "expect": {"valid": True, "objectId": call()}
            if wanted_error is None
            else {"valid": False, "error": wanted_error},
        })

    shas = {name: (name * 40)[:40] for name in "abcdef"}
    refs_cases = []
    for case_id, lines, wanted_canonical in [
        (
            "refs-heads-and-tags-sorted-by-refname",
            [
                f"{shas['c']} HEAD\x00multi_ack side-band-64k symref=HEAD:refs/heads/main\n",
                f"{shas['b']} refs/heads/main\n",
                f"{shas['a']} refs/heads/zeta\n",
                f"{shas['c']} refs/pull/1/head\n",
                f"{shas['d']} refs/tags/v1\n",
                f"{shas['a']} refs/tags/v1^{{}}\n",
                f"{shas['e']} refs/forkmesh/merge-jobs/job-1\n",
            ],
            f"{shas['b']} refs/heads/main\n{shas['a']} refs/heads/zeta\n{shas['d']} refs/tags/v1",
        ),
        (
            "refs-bytewise-refname-order",
            [
                f"{shas['f']} refs/heads/a/b\x00agent=git/2.47\n",
                f"{shas['e']} refs/heads/a-b\n",
                f"{shas['d']} refs/heads/a\n",
            ],
            f"{shas['d']} refs/heads/a\n{shas['e']} refs/heads/a-b\n{shas['f']} refs/heads/a/b",
        ),
        ("refs-empty-advertisement", [], ""),
    ]:
        advertisement = b"".join(_pkt(line) for line in lines) + b"0000"
        canonical = git_http.advertised_refs_canonical(advertisement)
        _expect(case_id, canonical, wanted_canonical)
        refs_cases.append({
            "id": case_id,
            "requirements": ["FM-ID-005"],
            "advertisementHex": advertisement.hex(),
            "expect": {
                "canonicalText": canonical,
                "stateHash": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
            },
        })

    state_document = repository_migration.state_document(
        "forkmesh:alice/demo",
        "gitea:caf\u00e9",
        [
            {"object_type": "issues", "source_key": "k2", "payload_hash": "sha256:" + "2" * 64},
            {"object_type": "git_refs", "source_key": "k1", "payload_hash": "sha256:" + "1" * 64},
        ],
        ["wiki", "projects", "wiki"],
    )
    migration_cases = [
        {
            "id": "migration-job-id",
            "requirements": ["FM-ID-006"],
            "operation": "migrationId",
            "repositoryId": "forkmesh:alice/demo",
            "expect": {"value": repository_migration.migration_id("forkmesh:alice/demo")},
        },
        {
            "id": "migration-object-key",
            "requirements": ["FM-ID-006"],
            "operation": "objectKey",
            "objectType": "issues",
            "sourceId": "github:issue:42",
            "expect": {"value": repository_migration.object_key("issues", "github:issue:42")},
        },
        {
            "id": "migration-state-hash",
            "requirements": ["FM-ID-006", "FM-MIG-001"],
            "operation": "stateHash",
            "document": state_document,
            "expect": {"value": repository_migration.state_hash(state_document)},
        },
    ]
    _expect(
        "migration-object-key",
        migration_cases[1]["expect"]["value"],
        hashlib.sha256(b"forkmesh-migration-object-v1\nissues\ngithub:issue:42").hexdigest(),
    )

    return _vector_file(
        "identifiers",
        "Derive each identifier from its inputs and compare, or reject with the listed error.",
        repositories=repository_cases,
        policies=policy_cases,
        objects=object_cases,
        refsState=refs_cases,
        migration=migration_cases,
    )


# --------------------------------------------------------------------------
# signatures and authentication

_GROUP_L = 2**252 + 27742317777372353535851937790883648493
_RFC8032 = [
    (
        "ed25519-rfc8032-test-1",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "ed25519-rfc8032-test-2",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
]


def _alternate_spelling(value: str) -> str:
    """Flip unused low bits of the final base64url character (same bytes)."""

    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    index = alphabet.index(value[-1])
    return value[:-1] + alphabet[index | 1 if index % 2 == 0 else index & ~1]


def signature_vectors() -> dict[str, Any]:
    ed_cases = []

    def add_ed(case_id: str, requirements: list[str], pk: str, msg: str, sig: str, valid: bool) -> None:
        actual = collab._ed25519_verify_uncached(bytes.fromhex(pk), bytes.fromhex(sig), bytes.fromhex(msg))
        _expect(case_id, actual, valid)
        ed_cases.append({
            "id": case_id,
            "requirements": requirements,
            "publicKeyHex": pk,
            "messageHex": msg,
            "signatureHex": sig,
            "expect": {"valid": valid},
        })

    for case_id, pk, msg, sig in _RFC8032:
        add_ed(case_id, ["FM-SIG-001"], pk, msg, sig, True)
    pk2, sig2 = _RFC8032[1][1], _RFC8032[1][3]
    add_ed("ed25519-tampered-message", ["FM-SIG-001"], pk2, "73", sig2, False)
    scalar = int.from_bytes(bytes.fromhex(sig2)[32:], "little")
    malleable = sig2[:64] + (scalar + _GROUP_L).to_bytes(32, "little").hex()
    add_ed("ed25519-noncanonical-scalar-s-plus-l", ["FM-SIG-001"], pk2, "72", malleable, False)
    add_ed("ed25519-identity-public-key", ["FM-SIG-001"], "01" + "00" * 31, "72", sig2, False)
    add_ed("ed25519-order-2-public-key", ["FM-SIG-001"], "ec" + "ff" * 30 + "7f", "72", sig2, False)
    add_ed("ed25519-order-4-public-key", ["FM-SIG-001"], "00" * 32, "72", sig2, False)
    add_ed("ed25519-noncanonical-public-key-y", ["FM-SIG-001"], "ed" + "ff" * 30 + "7f", "72", sig2, False)
    add_ed("ed25519-short-signature", ["FM-SIG-001"], pk2, "72", sig2[:-2], False)

    signer = "alice"
    alice = ACTORS[signer]
    record = {
        "branch": "main",
        "description": "caf\u00e9 mirror",
        "maintainer": alice,
        "name": "demo",
        "owner": "alice",
        "signature": "descriptive-field-not-bound",
        "stateHash": "0" * 64,
        "updatedAt": "2026-09-13T12:00:00Z",
        "visibility": "private",
    }
    bound = {key: value for key, value in record.items() if key != "signature"}
    record_hash = hashlib.sha256(
        json.dumps(bound, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    refs_hash = hashlib.sha256(b"b" * 40 + b" refs/heads/main").hexdigest()
    empty_body = hashlib.sha256(b"").hexdigest()
    schemes = [
        ("catalog-v1", ["FM-SIG-002", "FM-ENC-007", "FM-AUTH-004"], "forkmesh-catalog-v1",
         [["owner", "alice"], ["name", "demo"], ["updatedAt", "2026-09-13T12:00:00Z"]]),
        ("catalog-v2", ["FM-SIG-002", "FM-AUTH-004", "FM-VER-002"], "forkmesh-catalog-v2",
         [["recordSha256", record_hash]]),
        ("repostate-v1", ["FM-SIG-002", "FM-ENC-007", "FM-ID-005"], "forkmesh-repostate-v1",
         [["owner", "alice"], ["repo", "demo"], ["stateHash", refs_hash], ["updatedAt", "2026-09-13T12:00:00Z"]]),
        ("private-manifest-v1", ["FM-SIG-002", "FM-ENC-007"], "forkmesh-private-manifest-v1",
         [["owner", "alice"], ["name", "demo"], ["opaqueRepoId", "0123456789abcdef0123456789abcdef"],
          ["keyEpoch", "3"], ["manifestHash", "f" * 64], ["updatedAt", "2026-09-13T12:00:00Z"]]),
        ("view-v1", ["FM-SIG-002", "FM-ENC-007", "FM-AUTH-001"], "forkmesh-view-v1",
         [["owner", "alice"], ["repo", "demo"], ["ts", "1789300000000"]]),
        ("rotate-v1", ["FM-SIG-002", "FM-ENC-007"], "forkmesh-rotate-v1",
         [["oldPubkey", alice], ["newPubkey", ACTORS["bob"]], ["ts", "1789300000000"]]),
        ("git-push-v1-advertisement", ["FM-SIG-002", "FM-AUTH-002"], "forkmesh-git-push-v1",
         [["owner", "alice"], ["repo", "demo"], ["method", "GET"], ["ts", "1789300000000"], ["bodySha256", empty_body]]),
    ]
    string_cases = []
    signed: dict[str, tuple[str, str]] = {}
    for case_id, requirements, prefix, fields in schemes:
        message = "\n".join([prefix] + [value for _name, value in fields])
        signature = sign(signer, message.encode("utf-8"))
        _expect(case_id, collab.ed25519_verify(
            collab._b64url_decode(alice, 32, "k"), collab._b64url_decode(signature, 64, "s"), message.encode()
        ), True)
        signed[case_id] = (message, signature)
        string_cases.append({
            "id": case_id,
            "requirements": requirements,
            "scheme": prefix,
            "fields": fields,
            "publicKey": alice,
            "messageText": message,
            "signature": signature,
            "expect": {"valid": True},
        })

    def negative(case_id: str, requirements: list[str], message: str, signature: str, public: str) -> None:
        valid = collab.ed25519_verify(
            collab._b64url_decode(public, 32, "k"), collab._b64url_decode(signature, 64, "s"), message.encode()
        )
        _expect(case_id, valid, False)
        string_cases.append({
            "id": case_id,
            "requirements": requirements,
            "publicKey": public,
            "messageText": message,
            "signature": signature,
            "expect": {"valid": False},
        })

    view_message, view_signature = signed["view-v1"]
    negative(
        "cross-version-view-signature",
        ["FM-SIG-002", "FM-VER-002"],
        view_message.replace("forkmesh-view-v1", "forkmesh-view-v2", 1),
        view_signature,
        alice,
    )
    push_message, push_signature = signed["git-push-v1-advertisement"]
    negative(
        "git-push-method-substitution",
        ["FM-SIG-002", "FM-AUTH-002"],
        push_message.replace("\nGET\n", "\nPOST\n"),
        push_signature,
        alice,
    )
    catalog_message, catalog_signature = signed["catalog-v1"]
    negative(
        "catalog-v1-signature-as-repostate",
        ["FM-SIG-002"],
        catalog_message.replace("forkmesh-catalog-v1", "forkmesh-repostate-v1", 1),
        catalog_signature,
        alice,
    )
    negative("repostate-other-signer", ["FM-SIG-002", "FM-AUTH-004"], signed["repostate-v1"][0],
             signed["repostate-v1"][1], ACTORS["bob"])

    push_normalization = [
        {
            "id": "git-push-normalizes-owner-method-digest",
            "requirements": ["FM-AUTH-002"],
            "input": {"owner": " Alice ", "repo": "demo", "method": "post", "ts": "1789300000000",
                      "bodySha256": "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"},
            "expect": {"valid": True, "messageText": "forkmesh-git-push-v1\nalice\ndemo\nPOST\n1789300000000\n"
                       "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        },
        {
            "id": "git-push-rejects-unsupported-method",
            "requirements": ["FM-AUTH-002"],
            "input": {"owner": "alice", "repo": "demo", "method": "PUT", "ts": "1789300000000", "bodySha256": empty_body},
            "expect": {"valid": False},
        },
        {
            "id": "git-push-rejects-short-digest",
            "requirements": ["FM-AUTH-002"],
            "input": {"owner": "alice", "repo": "demo", "method": "GET", "ts": "1789300000000", "bodySha256": "abc"},
            "expect": {"valid": False},
        },
        {
            "id": "git-push-rejects-overlong-timestamp",
            "requirements": ["FM-AUTH-002"],
            "input": {"owner": "alice", "repo": "demo", "method": "GET", "ts": "12345678901234567", "bodySha256": empty_body},
            "expect": {"valid": False},
        },
    ]
    _expect(
        "catalog-v2-record-hash-escapes-non-ascii",
        '"description":"caf\\u00e9 mirror"' in json.dumps(bound, sort_keys=True, separators=(",", ":")),
        True,
    )

    now = 1_789_300_000_000
    freshness = [
        ("fresh-equal", now, str(now), True),
        ("fresh-past-edge", now, str(now - 300_000), True),
        ("stale-past", now, str(now - 300_001), False),
        ("fresh-future-edge", now, str(now + 300_000), True),
        ("stale-future", now, str(now + 300_001), False),
        ("stale-non-numeric", now, "soon", False),
    ]
    freshness_cases = [
        {"id": case_id, "requirements": ["FM-AUTH-001"], "nowMs": now_ms, "ts": ts, "expect": {"fresh": fresh}}
        for case_id, now_ms, ts, fresh in freshness
    ]

    return _vector_file(
        "signatures",
        "ed25519: strict verification of raw bytes; canonicalStrings: rebuild messageText "
        "from scheme+fields (when given) and verify the signature against publicKey; "
        "gitPushMessages: normalize input and build the message or reject; freshness: "
        "apply the request timestamp window.",
        catalogV2Record=record,
        ed25519=ed_cases,
        canonicalStrings=string_cases,
        gitPushMessages=push_normalization,
        freshness=freshness_cases,
    )


# --------------------------------------------------------------------------
# collaboration events


def _resign(event: dict[str, Any], actor: str) -> dict[str, Any]:
    unsigned = {key: event[key] for key in collab._UNSIGNED_KEYS}
    signed = dict(unsigned)
    signed["signature"] = sign(actor, collab.signing_bytes(unsigned))
    signed["eventId"] = collab.event_id(signed)
    return signed


def _with_event_id(event: dict[str, Any]) -> dict[str, Any]:
    event = dict(event)
    event["eventId"] = collab.event_id(event)
    return event


def event_vectors() -> dict[str, Any]:
    builder = Builder()
    policy = builder.policy
    create = builder.create("issue", "issue", "alice", {"title": "Vector issue", "body": "Steps"})
    update = builder.event("retitle", "issue", "alice", "update", {"fields": {"title": "Renamed"}}, [create["eventId"]])

    other_repository = collab.repository_id("git:forkmesh/other")
    other_policy = collab.policy_document(other_repository, maintainers=[ACTORS["maintainer"]])
    object_key = create["payload"]["objectKey"]
    foreign = collab.unsigned_event(
        repository=other_repository,
        policy=collab.policy_id(other_policy),
        family="issue",
        object=collab.object_id(other_repository, "issue", ACTORS["alice"], object_key),
        action="create",
        author=ACTORS["alice"],
        sequence=0,
        clock=0,
        issued_at=create["issuedAt"],
        parents=[],
        references=[],
        payload=create["payload"],
    )
    foreign = collab.finish_event(foreign, sign("alice", collab.signing_bytes(foreign)))

    lookalike_policy = collab.policy_document(builder.repository, maintainers=[ACTORS["bob"]])
    unknown_policy = _resign(dict(create, policy=collab.policy_id(lookalike_policy)), "alice")

    tampered = _with_event_id(dict(create, payload=dict(create["payload"], fields={"title": "Forged", "body": "Steps"})))
    other_key = collab._b64url(hashlib.sha256(b"object:other").digest()[:16])
    retargeted = _resign(
        dict(create, object=collab.object_id(builder.repository, "issue", ACTORS["alice"], other_key)), "alice"
    )
    wrong_signer = dict(create)
    wrong_signer["signature"] = sign("bob", collab.signing_bytes({key: create[key] for key in collab._UNSIGNED_KEYS}))
    wrong_signer = _with_event_id(wrong_signer)
    future_schema = _resign(dict(create, schema="forkmesh-collaboration-event-v2"), "alice")
    alternate_signature = dict(create, signature=_alternate_spelling(create["signature"]))
    padded_author = dict(create, author=create["author"] + "=")
    extra_key = dict(create, note="unsigned")
    wrong_id = dict(create, eventId="sha256:" + "0" * 64)
    bad_signature_foreign = dict(foreign)
    bad_signature_foreign["signature"] = create["signature"]
    bad_signature_foreign = _with_event_id(bad_signature_foreign)

    cases_spec = [
        ("event-valid-create", ["FM-ID-003", "FM-ID-004", "FM-SIG-003"], [], collab.canonical_json(create), None),
        ("event-valid-update", ["FM-ID-004", "FM-SIG-003"], ["event-valid-create"], collab.canonical_json(update), None),
        (
            "event-noncanonical-whitespace",
            ["FM-ENC-004", "FM-SIG-004"],
            [],
            json.dumps(create, sort_keys=True, ensure_ascii=False).encode("utf-8"),
            "noncanonical_encoding",
        ),
        ("event-future-schema", ["FM-VER-001"], [], collab.canonical_json(future_schema), "unsupported_schema"),
        ("event-extra-envelope-key", ["FM-SIG-004"], [], collab.canonical_json(extra_key), "invalid_envelope_keys"),
        ("event-padded-author", ["FM-ENC-006"], [], collab.canonical_json(padded_author), "invalid_author"),
        (
            "event-alternate-signature-spelling",
            ["FM-ENC-006", "FM-SIG-001"],
            [],
            collab.canonical_json(alternate_signature),
            "invalid_signature_encoding",
        ),
        ("event-id-mismatch", ["FM-ID-004"], [], collab.canonical_json(wrong_id), "event_id_mismatch"),
        ("event-object-id-mismatch", ["FM-ID-003"], [], collab.canonical_json(retargeted), "object_id_mismatch"),
        ("event-wrong-repository", ["FM-SIG-004"], [], collab.canonical_json(foreign), "wrong_repository"),
        (
            "event-binding-checked-before-signature",
            ["FM-SIG-004"],
            [],
            collab.canonical_json(bad_signature_foreign),
            "wrong_repository",
        ),
        ("event-unknown-policy", ["FM-ID-002", "FM-SIG-004"], [], collab.canonical_json(unknown_policy), "unknown_policy"),
        ("event-tampered-payload", ["FM-SIG-003"], [], collab.canonical_json(tampered), "invalid_signature"),
        ("event-signed-by-other-key", ["FM-SIG-003"], [], collab.canonical_json(wrong_signer), "invalid_signature"),
    ]
    cases = []
    for case_id, requirements, context, raw, wanted in cases_spec:
        def verify(raw: bytes = raw) -> None:
            collab.verify_event(collab.parse_canonical_event(raw), policy)

        _expect(case_id, _collab_error(verify), wanted)
        expect: dict[str, Any] = {"valid": wanted is None}
        if wanted is None:
            expect["eventId"] = json.loads(raw)["eventId"]
        else:
            expect["error"] = wanted
        cases.append({
            "id": case_id,
            "requirements": requirements,
            "context": context,
            **_raw_fields(raw),
            "expect": expect,
        })

    return _vector_file(
        "events",
        "Parse rawHex as a canonical envelope, then verify shape, identifiers, repository "
        "and policy binding, and the Ed25519 signature against policies.primary "
        "(FM-SIG-004 order). Context cases are valid ancestors for folding implementations.",
        policies={"primary": policy},
        cases=cases,
    )


# --------------------------------------------------------------------------
# fold


def fold_vectors() -> dict[str, Any]:
    builder = Builder()
    policy = builder.policy
    create = builder.create("issue", "issue", "alice", {"title": "Original", "body": "Steps"})
    alice = builder.event("alice_title", "issue", "alice", "update", {"fields": {"title": "Alice title"}}, [create["eventId"]])
    maintainer = builder.event(
        "maintainer_title", "issue", "maintainer", "update", {"fields": {"title": "Maintainer title"}}, [create["eventId"]]
    )
    resolution = builder.event(
        "resolution", "issue", "maintainer", "update", {"fields": {"title": "Resolved title"}},
        [alice["eventId"], maintainer["eventId"]],
    )
    equivocation_a = builder.event(
        "equivocation_a", "issue", "alice", "update", {"fields": {"body": "first"}}, [create["eventId"]], sequence=50
    )
    equivocation_b = builder.event(
        "equivocation_b", "issue", "alice", "update", {"fields": {"body": "second"}}, [create["eventId"]], sequence=50
    )
    label = builder.create("label", "label", "alice", {"name": "bug", "color": "#d73a4a"})

    def raw(event: dict[str, Any]) -> str:
        return collab.canonical_json(event).decode("utf-8")

    scenarios_spec = [
        ("concurrent-scalar-conflict", ["FM-SYNC-002", "FM-SYNC-003"], [create, alice, maintainer], {"conflicts": 1}),
        ("conflict-resolution", ["FM-SYNC-002", "FM-SYNC-003"], [create, alice, maintainer, resolution], {"conflicts": 0}),
        (
            "permuted-duplicated-delivery",
            ["FM-SYNC-001", "FM-SYNC-002"],
            [resolution, maintainer, create, alice, create, maintainer],
            {"duplicateDeliveries": 2},
        ),
        ("missing-parent-pending", ["FM-SYNC-003"], [alice], {"pending": 1}),
        ("equivocation-excludes-both", ["FM-SYNC-003"], [create, equivocation_a, equivocation_b], {"equivocations": 1}),
        ("unauthorized-label-create", ["FM-SYNC-003"], [label], {"rejected": 1}),
    ]
    scenarios = []
    digests: dict[str, str] = {}
    telemetry_keys = ("accepted", "pending", "rejected", "duplicateDeliveries", "equivocations", "conflicts")
    for scenario_id, requirements, deliveries, wanted in scenarios_spec:
        ndjson = "".join(raw(event) + "\n" for event in deliveries)
        result = collab.SignedEventLog.from_ndjson(ndjson).fold(policy)
        telemetry = {key: result.telemetry[key] for key in telemetry_keys}
        for key, value in wanted.items():
            _expect(scenario_id + "." + key, telemetry[key], value)
        digests[scenario_id] = result.state_digest
        scenarios.append({
            "id": scenario_id,
            "requirements": requirements,
            "ndjson": ndjson,
            "expect": {
                "stateDigest": result.state_digest,
                "telemetry": telemetry,
                "rejectionReasons": result.telemetry["rejectionReasons"],
            },
        })
    _expect("permuted-duplicated-delivery", digests["permuted-duplicated-delivery"], digests["conflict-resolution"])

    crlf = raw(create) + "\r\n" + raw(alice) + "\n"
    result = collab.SignedEventLog.from_ndjson(crlf).fold(policy)
    _expect("crlf-record-is-audited", (result.telemetry["accepted"], result.telemetry["rejected"]), (0, 1))
    scenarios.append({
        "id": "crlf-record-is-audited",
        "requirements": ["FM-SYNC-001"],
        "ndjson": crlf,
        "expect": {
            "stateDigest": result.state_digest,
            "telemetry": {key: result.telemetry[key] for key in telemetry_keys},
            "rejectionReasons": result.telemetry["rejectionReasons"],
        },
    })

    return _vector_file(
        "fold",
        "Split ndjson on LF (retaining any CR), fold every record under policies.primary, "
        "and compare stateDigest = sha256 of the canonical state plus the telemetry counts.",
        policies={"primary": policy},
        scenarios=scenarios,
    )


# --------------------------------------------------------------------------
# portable sessions

_SESSION_ERRORS = {
    "portable session token is invalid": "token_invalid",
    "portable session claim is invalid": "claim_invalid",
    "portable session is expired or not current": "not_current",
    "portable session claim is not canonical": "claim_noncanonical",
    "portable session signature is invalid": "signature_invalid",
    "portable session encoding is invalid": "encoding_invalid",
}


def session_vectors() -> dict[str, Any]:
    router = public_key("router")
    router_bytes = collab._b64url_decode(router, 32, "router")
    issued = 1_789_300_000_000
    claim = portable_sessions.build_claim(
        account_id="a1" * 16,
        account_name="alice",
        session_id="session-000000001",
        grant_id="grant-0000000001",
        issued_at=issued,
        expires_at=issued + 3_600_000,
    )

    def token_for(claim_bytes: bytes, signer: str = "router") -> str:
        encoded = collab._b64url(claim_bytes)
        return "v1." + encoded + "." + sign(signer, portable_sessions.SESSION_MESSAGE_PREFIX + encoded.encode("ascii"))

    canonical = portable_sessions.canonical_json(claim)
    valid = token_for(canonical)
    long_lived = token_for(portable_sessions.canonical_json(dict(claim, exp=issued + 12 * 3_600_000 + 1)))
    future_version = token_for(portable_sessions.canonical_json(dict(claim, v=2)))
    wrong_audience = token_for(portable_sessions.canonical_json(dict(claim, aud="forkmesh-other")))
    spaced = token_for(json.dumps(claim, sort_keys=True).encode())
    other_signer = token_for(canonical, "bob")
    prefix, encoded, signature = valid.split(".")
    alternate = ".".join([prefix, encoded, _alternate_spelling(signature)])
    skew = portable_sessions.SESSION_CLOCK_SKEW_MS
    expires = issued + 3_600_000

    spec = [
        ("session-valid", ["FM-AUTH-003"], valid, issued, None),
        ("session-valid-within-expiry-skew", ["FM-AUTH-003"], valid, expires + skew - 1, None),
        ("session-expired", ["FM-AUTH-003"], valid, expires + skew, "not_current"),
        ("session-issued-in-future", ["FM-AUTH-003"], valid, issued - skew - 1, "not_current"),
        ("session-lifetime-over-12h", ["FM-AUTH-003"], long_lived, issued, "not_current"),
        ("session-future-token-prefix", ["FM-AUTH-003", "FM-VER-001"], "v2." + valid[3:], issued, "token_invalid"),
        ("session-future-claim-version", ["FM-AUTH-003", "FM-VER-001"], future_version, issued, "claim_invalid"),
        ("session-wrong-audience", ["FM-AUTH-003"], wrong_audience, issued, "claim_invalid"),
        ("session-noncanonical-claim", ["FM-AUTH-003", "FM-ENC-004"], spaced, issued, "claim_noncanonical"),
        ("session-other-signer", ["FM-AUTH-003"], other_signer, issued, "signature_invalid"),
        ("session-alternate-signature-spelling", ["FM-AUTH-003", "FM-ENC-006"], alternate, issued, "encoding_invalid"),
    ]
    cases = []
    for case_id, requirements, token, now_ms, wanted in spec:
        try:
            parsed = portable_sessions.parse_token(
                token,
                now_ms=now_ms,
                verify=lambda message, sig: collab.ed25519_verify(router_bytes, sig, message),
            )
            code = None
        except portable_sessions.PortableSessionError as exc:
            parsed = None
            code = _SESSION_ERRORS[str(exc)]
        _expect(case_id, code, wanted)
        cases.append({
            "id": case_id,
            "requirements": requirements,
            "token": token,
            "nowMs": now_ms,
            "expect": {"valid": True, "claim": parsed} if wanted is None else {"valid": False, "error": wanted},
        })
    return _vector_file(
        "sessions",
        "Parse each portable session token at nowMs and verify it with routerPublicKey.",
        routerPublicKey=router,
        cases=cases,
    )


# --------------------------------------------------------------------------

GENERATORS: dict[str, Callable[[], dict[str, Any]]] = {
    "canonical-json": canonical_json_vectors,
    "identifiers": identifier_vectors,
    "signatures": signature_vectors,
    "events": event_vectors,
    "fold": fold_vectors,
    "sessions": session_vectors,
}


def _serialize(value: Any) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_outputs() -> dict[Path, bytes]:
    outputs = {VECTOR_DIR / f"{name}.json": _serialize(generate()) for name, generate in GENERATORS.items()}
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    manifest["version"] = SUITE_VERSION
    vector_entries = {entry["path"]: entry for entry in manifest.get("vectors", [])}
    ordered = []
    for path, data in outputs.items():
        relative = path.relative_to(ROOT).as_posix()
        entry = dict(vector_entries.get(relative, {"path": relative}))
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        ordered.append(entry)
    manifest["vectors"] = ordered
    for group in ("documents", "schemas"):
        for entry in manifest.get(group, []):
            entry["sha256"] = _sha256_file(ROOT / entry["path"])
    register = manifest.get("threatRegister")
    if register:
        register["sha256"] = _sha256_file(ROOT / register["path"])
    outputs[MANIFEST] = _serialize(manifest)
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail instead of writing when output differs")
    args = parser.parse_args()
    try:
        outputs = build_outputs()
    except GenerationError as exc:
        print(f"vector generation failed: {exc}", file=sys.stderr)
        return 2
    stale = [path for path, data in outputs.items() if not path.exists() or path.read_bytes() != data]
    if args.check:
        for path in stale:
            print(f"stale: {path.relative_to(ROOT).as_posix()}", file=sys.stderr)
        return 1 if stale else 0
    VECTOR_DIR.mkdir(parents=True, exist_ok=True)
    for path in stale:
        path.write_bytes(outputs[path])
        print(f"wrote {path.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
