#!/usr/bin/env python3
"""forkmesh-conformance-adapter-v1 adapter for the clean-room Python verifier.

Reads one JSON request per line on stdin and writes one JSON response per line
(www/docs/protocol/certification.md#adapter-protocol). ``--fault`` deliberately
breaks one protocol property so the harness can prove it detects that class of
defect; a faulted run is never evidence and cannot be attested.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
import types
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "protocol" / "v1"))

import verify_suite as suite  # noqa: E402

PROTOCOL = "forkmesh-conformance-adapter-v1"
FAULTS = (
    "accept-malformed-signatures",
    "skip-normalization",
    "downgrade-on-failure",
    "ignore-expiry",
    "lose-data",
    "accept-future-versions",
    "old-suite",
    "crash-midway",
    "hang",
    "slow",
)


def _lenient_ed25519(public_key: bytes, signature: bytes, message: bytes) -> bool:
    # The RFC 8032 equation without FM-SIG-001 strictness: no scalar range,
    # identity, or small-order checks.
    public = suite._decode_point(public_key) if len(public_key) == 32 else None
    nonce = suite._decode_point(signature[:32]) if len(signature) == 64 else None
    if public is None or nonce is None:
        return False
    scalar = int.from_bytes(signature[32:], "little")
    challenge = int.from_bytes(hashlib.sha512(signature[:32] + public_key + message).digest(), "little") % suite._L
    return suite._point_equal(
        suite._scalar_mult(scalar, suite._BASE),
        suite._point_add(nonce, suite._scalar_mult(challenge, public)),
    )


def apply_fault(fault: str | None) -> None:
    if fault == "accept-malformed-signatures":
        suite.ed25519_verify = _lenient_ed25519
    elif fault == "skip-normalization":
        suite.unicodedata = types.SimpleNamespace(normalize=lambda _form, value: value)
    elif fault == "ignore-expiry":
        suite.FRESHNESS_WINDOW_MS = 10**16
        suite.SESSION_MAX_LIFETIME_MS = 10**16
        suite.SESSION_SKEW_MS = 10**16
    elif fault == "lose-data":
        original_refs = suite.refs_canonical
        suite.refs_canonical = lambda advertisement: "\n".join(original_refs(advertisement).split("\n")[:-2] + [""])
    elif fault == "accept-future-versions":
        suite._SESSION_TOKEN_RE = re.compile(r"v[0-9]+\.([A-Za-z0-9_-]{80,1024})\.([A-Za-z0-9_-]{86})")
        original_policy_id = suite.policy_id

        def policy_id(policy: Any) -> str:
            if isinstance(policy, dict) and str(policy.get("schema", "")).startswith("forkmesh-collaboration-policy-v"):
                policy = {**policy, "schema": suite.POLICY_SCHEMA}
            return original_policy_id(policy)

        suite.policy_id = policy_id


def _canonical_string_valid(request: dict[str, Any], fault: str | None) -> bool:
    message = request["messageText"]
    if "scheme" in request:
        if "\n".join([request["scheme"]] + [value for _name, value in request["fields"]]) != message:
            return False
        if request["scheme"] == "forkmesh-catalog-v2":
            if dict(request["fields"]).get("recordSha256") != suite.catalog_v2_record_hash(request["catalogV2Record"]):
                return False
    try:
        key = suite.b64url_decode(request["publicKey"], 32, "key")
        signature = suite.b64url_decode(request["signature"], 64, "signature")
    except suite.Reject:
        return False
    if suite.ed25519_verify(key, signature, message.encode("utf-8")):
        return True
    if fault == "downgrade-on-failure":
        head, _, rest = message.partition("\n")
        match = re.fullmatch(r"(forkmesh-[a-z-]+-v)([0-9]+)", head)
        for older in range(int(match.group(2)) - 1, 0, -1) if match else ():
            if suite.ed25519_verify(key, signature, f"{match.group(1)}{older}\n{rest}".encode("utf-8")):
                return True
    return False


def _migration(request: dict[str, Any]) -> str:
    if request["operation"] == "migrationId":
        return "mig_" + suite.migration_digest("forkmesh-migration-job-v1", request["repositoryId"])[:24]
    if request["operation"] == "objectKey":
        return suite.migration_digest("forkmesh-migration-object-v1", request["objectType"], request["sourceId"])
    return "sha256:" + hashlib.sha256(suite.encode(request["document"]).encode("utf-8")).hexdigest()


def _refs_state(request: dict[str, Any]) -> dict[str, str]:
    text = suite.refs_canonical(bytes.fromhex(request["advertisementHex"]))
    return {"canonicalText": text, "stateHash": hashlib.sha256(text.encode("utf-8")).hexdigest()}


def _git_push(request: dict[str, Any]) -> str:
    message = suite.git_push_message(
        request["owner"], request["repo"], request["method"], request["ts"], request["bodySha256"]
    )
    if message is None:
        raise suite.Reject("invalid_git_push_message")
    return message


def _fresh(request: dict[str, Any]) -> bool:
    if not suite.request_is_fresh(request["ts"], request["nowMs"]):
        raise suite.Reject("stale_request")
    return True


def _ed25519(request: dict[str, Any]) -> bool:
    if not suite.ed25519_verify(
        bytes.fromhex(request["publicKeyHex"]), bytes.fromhex(request["signatureHex"]), bytes.fromhex(request["messageHex"])
    ):
        raise suite.Reject("invalid_signature")
    return True


def operations(fault: str | None) -> dict[str, Callable[[dict[str, Any]], Any]]:
    def canonical_string(request: dict[str, Any]) -> bool:
        if not _canonical_string_valid(request, fault):
            raise suite.Reject("invalid_signature")
        return True

    return {
        "canonical.parse": lambda r: (suite.parse_canonical_document(bytes.fromhex(r["rawHex"])), None)[1],
        "canonical.encode": lambda r: suite.canonical_bytes(r["value"]).decode("utf-8"),
        "identifier.repository": lambda r: suite.repository_id(r["verifiedRoot"]),
        "identifier.policy": lambda r: suite.policy_id(r["policy"]),
        "identifier.object": lambda r: suite.object_id(
            r["repository"], r["family"], r["creator"], r["objectKey"], r["references"]
        ),
        "identifier.refs-state": _refs_state,
        "identifier.migration": _migration,
        "signature.ed25519": lambda r: (_ed25519(r), None)[1],
        "signature.canonical-string": lambda r: (canonical_string(r), None)[1],
        "signature.git-push-message": _git_push,
        "request.freshness": lambda r: (_fresh(r), None)[1],
        "event.verify": lambda r: suite.verify_event(bytes.fromhex(r["rawHex"]), r["policy"])["eventId"],
        "session.parse": lambda r: suite.parse_session(
            r["token"], r["nowMs"], suite.b64url_decode(r["routerPublicKey"], 32, "router")
        ),
    }


def _write(message: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(message, sort_keys=True) + "\n")
    sys.stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fault", choices=FAULTS, help="break one property to prove the harness detects it")
    args = parser.parse_args()
    apply_fault(args.fault)
    handlers = operations(args.fault)
    handled = 0
    for line in sys.stdin:
        if not line.strip():
            continue
        request = json.loads(line)
        if request.get("type") == "hello":
            _write({
                "type": "hello",
                "protocol": PROTOCOL,
                "implementation": {"name": "forkmesh-clean-room-python", "version": "1.0.0"},
                "suiteVersions": ["0.9.0"] if args.fault == "old-suite" else ["1.0.0-rc.1"],
                "operations": sorted(handlers),
            })
            continue
        handled += 1
        if args.fault == "crash-midway" and handled > 10:
            return 3
        if args.fault == "hang" and handled == 3:
            time.sleep(3600)
        if args.fault == "slow":
            time.sleep(0.05)
        handler = handlers.get(request.get("operation"))
        if handler is None:
            _write({"id": request.get("id"), "unsupported": True})
            continue
        try:
            _write({"id": request["id"], "valid": True, "value": handler(request["input"])})
        except suite.Reject as exc:
            _write({"id": request["id"], "valid": False, "error": exc.code})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
