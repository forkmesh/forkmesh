#!/usr/bin/env python3
"""Reproduce, sign, and verify a repository availability-contract report.

Implements the forkmesh-availability-contract-report-v1 model
(www/docs/availability-contract.md) by reusing the reference implementation,
so the CLI and the shared fixture always agree.

  forkmesh_availability_contract.py explain bundle.json         plain-language summary
  forkmesh_availability_contract.py explain --json bundle.json  machine-readable report
  forkmesh_availability_contract.py sign bundle.json --private-key seed.txt [--key-id ID]
                                                                print a signed report
  forkmesh_availability_contract.py verify bundle.json signed.json [--public-key KEY]
                                                                recompute, rehash, and check
                                                                the signature
  forkmesh_availability_contract.py fixture                     verify the shared fixture

The input is a forkmesh-availability-contract-evidence-v1 bundle, or - for
stdin. Keys are base64url Ed25519 values: a 32-byte private seed, and a 32-byte
public key.

Exit codes: 0 contract met with admissible capacity (or verify/fixture
matched), 3 contract missed or capacity not admissible, 1 fixture or verify
mismatch, 2 usage, unreadable input, or invalid contract terms.
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "conformance" / "availability-contract" / "v1" / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import availability_contract  # noqa: E402
from verify_migration_report import ed25519_verify  # noqa: E402

EXIT_MET = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_AT_RISK = 3


def _read_json(path):
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    return json.loads(text)


def _b64decode(value):
    value = str(value or "").strip()
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def _b64encode(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _fixture_mismatches():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    mismatches = [
        case["id"] for case in fixture["cases"]
        if availability_contract.evaluate_contract(case["bundle"]) != case["expect"]
    ]
    return len(fixture["cases"]), mismatches


def _sign(report, seed, key_id):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    private = Ed25519PrivateKey.from_private_bytes(seed)
    public = private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    report_digest = availability_contract.report_hash(report)
    return {
        "schema": availability_contract.SIGNED_SCHEMA,
        "report": report,
        "reportHash": report_digest,
        "signature": {
            "algorithm": "ed25519",
            "keyId": key_id,
            "publicKey": _b64encode(public),
            "signature": _b64encode(private.sign(
                availability_contract.signing_message(report_digest))),
        },
    }


def _verify(bundle, signed, pinned_key):
    """Return a list of problems; empty when the signed report checks out."""
    if not isinstance(signed, dict) or signed.get("schema") != availability_contract.SIGNED_SCHEMA:
        return ["not a %s document" % availability_contract.SIGNED_SCHEMA]
    problems = []
    report = signed.get("report")
    if availability_contract.evaluate_contract(bundle) != report:
        problems.append("the report does not reproduce from the evidence bundle")
    report_digest = availability_contract.report_hash(report)
    if signed.get("reportHash") != report_digest:
        problems.append("reportHash does not match the report")
    signature = signed.get("signature") if isinstance(signed.get("signature"), dict) else {}
    if signature.get("algorithm") != "ed25519":
        problems.append("unsupported signature algorithm")
        return problems
    try:
        public = _b64decode(signature.get("publicKey"))
        raw_signature = _b64decode(signature.get("signature"))
    except (ValueError, TypeError):
        return problems + ["signature fields are not base64url"]
    if pinned_key is not None and public != pinned_key:
        problems.append("the report was signed by a different key")
    if not ed25519_verify(public, raw_signature,
                          availability_contract.signing_message(report_digest)):
        problems.append("the signature does not verify")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    explain = commands.add_parser("explain", help="explain one contract period")
    explain.add_argument("input", help="evidence bundle JSON file, or - for stdin")
    explain.add_argument("--json", action="store_true", help="print the machine-readable report")
    sign = commands.add_parser("sign", help="evaluate the bundle and print a signed report")
    sign.add_argument("input", help="evidence bundle JSON file, or - for stdin")
    sign.add_argument("--private-key", required=True,
                      help="file holding a base64url 32-byte Ed25519 private seed")
    sign.add_argument("--key-id", default="forkmesh-availability-contracts")
    verify = commands.add_parser(
        "verify", help="recompute the report and check its hash and signature")
    verify.add_argument("input", help="evidence bundle JSON file, or - for stdin")
    verify.add_argument("signed", help="signed report JSON file")
    verify.add_argument("--public-key", help="base64url Ed25519 public key the report must be signed by")
    commands.add_parser("fixture", help="verify the shared conformance fixture")
    args = parser.parse_args(argv)

    if args.command == "fixture":
        total, mismatches = _fixture_mismatches()
        print(json.dumps({"cases": total, "mismatches": mismatches}, sort_keys=True))
        return EXIT_MISMATCH if mismatches else EXIT_MET

    try:
        bundle = _read_json(args.input)
        signed = _read_json(args.signed) if args.command == "verify" else None
        seed = (_b64decode(Path(args.private_key).read_text(encoding="utf-8"))
                if args.command == "sign" else None)
        pinned = (_b64decode(args.public_key)
                  if args.command == "verify" and args.public_key else None)
        report = availability_contract.evaluate_contract(bundle)
    except (OSError, ValueError) as exc:
        print("cannot evaluate %s: %s" % (args.input, exc), file=sys.stderr)
        return EXIT_USAGE

    if args.command == "sign":
        if len(seed) != 32:
            print("the private key must be a base64url 32-byte Ed25519 seed", file=sys.stderr)
            return EXIT_USAGE
        print(json.dumps(_sign(report, seed, args.key_id), sort_keys=True))
        return EXIT_MET
    if args.command == "verify":
        problems = _verify(bundle, signed, pinned)
        if problems:
            for problem in problems:
                print("mismatch: " + problem, file=sys.stderr)
            return EXIT_MISMATCH
        print("reproduced%s: %s" % (
            "" if pinned is not None else " (signing key not pinned; pass --public-key)",
            report["summary"]))
        return EXIT_MET
    if args.json:
        print(json.dumps(report, sort_keys=True))
    else:
        print(report["summary"])
    return EXIT_MET if report["outcome"] == "met" and report["admission"]["admissible"] else EXIT_AT_RISK


if __name__ == "__main__":
    raise SystemExit(main())
