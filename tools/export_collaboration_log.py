#!/usr/bin/env python3
"""Export a checkout's issue records as a verifiable collaboration log (#706).

Issue records under ``.forkmesh/issues`` carry per-event legacy signatures that
nothing outside ForkMesh reads. This tool imports them into the
forkmesh-collaboration-event-v1 format so ``tools/verify_collaboration.py`` can
check them offline from a plain clone:

  export_collaboration_log.py keygen SEED_FILE
  export_collaboration_log.py export --repository IDENTITY --signing-key SEED_FILE \\
      [--checkout DIR] [--out DIR]

``export`` writes ``policy.json`` and ``issues.ndjson`` (default
``DIR/.forkmesh/collaboration``). The signing key is the repository authority:
it becomes the policy's only maintainer and signs one importer ``create`` event
per issue. Import events never claim a legacy author signed them. Each one binds
the SHA-256 of the exact record file bytes and records whether every legacy event
signature in that record verified (``valid``), any failed (``invalid``), or some
event was unsigned (``unavailable``). The output is deterministic: rerunning over
unchanged records yields identical bytes.

Exit codes: 0 written; 2 usage or unreadable input.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app" / "src"))

import collaboration  # noqa: E402
from collaboration import CollaborationError, SignedEventLog, canonical_json  # noqa: E402


LEGACY_OBJECT_DOMAIN = b"forkmesh-collaboration-legacy-object-v1\nissue\n"


class UsageError(Exception):
    pass


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _b64url_decode(text: str) -> bytes:
    text = text.strip()
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def _scalar(seed: bytes) -> tuple[int, bytes]:
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:]


def public_key(seed: bytes) -> bytes:
    scalar, _prefix = _scalar(seed)
    return collaboration._encode_point(
        collaboration._scalar_mult(collaboration._BASE, scalar)
    )


def sign(seed: bytes, message: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError:
        pass
    else:
        return Ed25519PrivateKey.from_private_bytes(seed).sign(message)
    scalar, prefix = _scalar(seed)
    public = public_key(seed)
    order = collaboration._GROUP_L
    nonce = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % order
    encoded_nonce = collaboration._encode_point(
        collaboration._scalar_mult(collaboration._BASE, nonce)
    )
    challenge = int.from_bytes(
        hashlib.sha512(encoded_nonce + public + message).digest(), "little"
    ) % order
    return encoded_nonce + ((nonce + challenge * scalar) % order).to_bytes(32, "little")


def load_seed(path) -> bytes:
    try:
        seed = _b64url_decode(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read signing key %s: %s" % (path, exc)) from exc
    if len(seed) != 32:
        raise UsageError("the signing key must be a base64url 32-byte Ed25519 seed")
    return seed


def legacy_issue_event_content(ev) -> str:
    t = ev.get("type", "")
    attachments = ",".join(ev.get("attachments") or [])
    if t == "open":
        return "\x00".join([ev.get("title", ""), ev.get("body", ""), attachments])
    if t in ("comment", "edit"):
        return "\x00".join([ev.get("body", ""), attachments])
    if t == "title":
        return ev.get("title", "")
    if t == "status":
        return ev.get("status", "")
    if t == "labels":
        return ",".join(ev.get("labels") or [])
    if t == "milestone":
        return ev.get("milestone", "") or ""
    if t == "dates":
        parts = []
        for key in ("startDate", "endDate"):
            try:
                parts.append(str(int(ev.get(key, 0) or 0)))
            except (TypeError, ValueError):
                parts.append("0")
        return "\x00".join(parts)
    if t == "priority":
        try:
            return str(int(ev.get("priority", 0)))
        except (TypeError, ValueError):
            return "0"
    if t == "progress":
        try:
            return str(int(ev.get("progress", 0)))
        except (TypeError, ValueError):
            return "0"
    if t == "bounty":
        try:
            amount = "%.2f" % float(ev.get("bountyUsd", 0))
        except (TypeError, ValueError):
            amount = "0.00"
        return "\x00".join([amount, ev.get("bountyAddress", "") or "",
                            ev.get("bountyStatus", "") or ""])
    if t == "assignees":
        return ",".join(ev.get("assignees") or [])
    if t == "claim":
        return "\x00".join([
            ev.get("assignee", "") or "", ev.get("claimState", "") or "",
        ])
    if t == "reporter":
        return ev.get("reporter", "") or ""
    if t == "agent":
        try:
            session_id = str(int(ev.get("agentSessionId", 0) or 0))
        except (TypeError, ValueError):
            session_id = "0"
        return "\x00".join([
            ev.get("agentProvider", "") or "",
            session_id,
            ev.get("agentStatus", "") or "",
            "pr" if ev.get("agentCreatePr") else "no-pr",
        ])
    if t == "reaction":
        return "\x00".join([ev.get("target", "") or "",
                            ev.get("reaction", "") or ""])
    if t == "delete":
        return ev.get("target", "")
    return ""


def legacy_event_signature_valid(number: int, ev) -> bool:
    try:
        author = _b64url_decode(str(ev.get("author", "")))
        signature = _b64url_decode(str(ev.get("sig", "")))
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    if len(author) != 32 or len(signature) != 64:
        return False
    content_hash = hashlib.sha256(legacy_issue_event_content(ev).encode()).hexdigest()
    numbers = (number, 0) if ev.get("type") == "open" else (number,)
    for candidate in numbers:
        message = (
            "forkmesh-issue-event-v1\n" + str(ev.get("type", "")) + "\n" +
            str(candidate) + "\n" + str(ev.get("author", "")) + "\n" + str(ts) +
            "\n" + content_hash
        ).encode()
        if collaboration.ed25519_verify(author, signature, message):
            return True
    return False


def signing_repository(repository_identity: str) -> str:
    parts = [part.strip().lower() for part in str(repository_identity or "").split("/")]
    if len(parts) < 2 or not parts[-2] or not parts[-1]:
        return ""
    return parts[-2] + "/" + parts[-1]


def event_signature_valid_v2(number: int, ev, repo_key: str) -> bool:
    if not repo_key:
        return False
    try:
        author = _b64url_decode(str(ev.get("author", "")))
        signature = _b64url_decode(str(ev.get("sig", "")))
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    if len(author) != 32 or len(signature) != 64:
        return False
    content_hash = hashlib.sha256(legacy_issue_event_content(ev).encode()).hexdigest()
    numbers = (number, 0) if ev.get("type") == "open" else (number,)
    for candidate in numbers:
        message = (
            "forkmesh-issue-event-v2\n" + repo_key + "\n" +
            str(ev.get("type", "")) + "\n" +
            str(candidate) + "\n" + str(ev.get("author", "")) + "\n" + str(ts) +
            "\n" + content_hash
        ).encode()
        if collaboration.ed25519_verify(author, signature, message):
            return True
    return False


def legacy_signature_status(record, repo_key: str = "") -> str:
    events = record.get("events") or []
    if not events:
        return "unavailable"
    unsigned = False
    for ev in events:
        if not ev.get("sig"):
            unsigned = True
        elif not (legacy_event_signature_valid(int(record["number"]), ev) or
                  event_signature_valid_v2(int(record["number"]), ev, repo_key)):
            return "invalid"
    return "unavailable" if unsigned else "valid"


def legacy_issue_fields(record) -> dict:
    events = record.get("events") or []
    opened = next((ev for ev in events if ev.get("type") == "open"), {})
    body = str(opened.get("body", "") or "")
    open_id = opened.get("id")
    edits = [
        ev for ev in events
        if ev.get("type") == "edit" and open_id and ev.get("target") == open_id
    ]
    for edit in sorted(edits, key=lambda ev: int(ev.get("ts", 0) or 0)):
        body = str(edit.get("body", "") or "")
    status = record.get("status")
    fields = {
        "title": str(record.get("title", "") or ""),
        "body": body,
        "status": status if status in ("open", "closed") else "open",
    }
    try:
        fields["priority"] = min(max(int(record.get("priority", 0) or 0), 0), 9999)
    except (TypeError, ValueError):
        pass
    return fields


def issue_records(checkout: Path) -> list[tuple[int, Path, bytes, dict]]:
    records = []
    for path in (checkout / ".forkmesh" / "issues").glob("*/*/issue-*.json"):
        raw = path.read_bytes()
        try:
            record = json.loads(raw)
        except ValueError as exc:
            raise UsageError("cannot parse %s: %s" % (path, exc)) from exc
        if not isinstance(record, dict) or record.get("schema") != "forkmesh-issue-v1":
            continue
        try:
            number = int(record.get("number"))
        except (TypeError, ValueError):
            continue
        if number > 0:
            records.append((number, path, raw, record))
    records.sort(key=lambda item: item[0])
    numbers = [item[0] for item in records]
    if len(numbers) != len(set(numbers)):
        raise UsageError("duplicate issue numbers under %s" % (checkout / ".forkmesh/issues"))
    return records


def export(checkout: Path, repository_identity: str, seed: bytes):
    repository = collaboration.repository_id(repository_identity)
    repo_key = signing_repository(repository_identity)
    importer = _b64url(public_key(seed))
    policy = collaboration.policy_document(repository, maintainers=[importer])
    policy_digest = collaboration.policy_id(policy)
    log = SignedEventLog()
    statuses = {"valid": 0, "invalid": 0, "unavailable": 0}
    skipped = []
    sequence = 0
    for number, _path, raw, record in issue_records(checkout):
        status = legacy_signature_status(record, repo_key)
        object_key = _b64url(
            hashlib.sha256(LEGACY_OBJECT_DOMAIN + str(number).encode()).digest()[:16]
        )
        try:
            issued_at = int(record.get("updatedAt") or record.get("createdAt") or 0)
            unsigned = collaboration.unsigned_event(
                repository=repository,
                policy=policy_digest,
                family="issue",
                object=collaboration.object_id(repository, "issue", importer, object_key, []),
                action="create",
                author=importer,
                sequence=sequence + 1,
                clock=0,
                issued_at=issued_at,
                parents=[],
                references=[],
                payload={
                    "objectKey": object_key,
                    "fields": legacy_issue_fields(record),
                    "legacy": {
                        "schema": "forkmesh-issue-v1",
                        "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                        "signatureStatus": status,
                    },
                },
            )
            signature = sign(seed, collaboration.signing_bytes(unsigned))
            event = collaboration.finish_event(unsigned, _b64url(signature))
        except (CollaborationError, ValueError) as exc:
            skipped.append({"issue": number, "reason": str(exc)})
            continue
        log.append(event)
        sequence += 1
        statuses[status] += 1
    summary = {
        "issues": sequence,
        "legacySignatures": statuses,
        "maintainer": importer,
        "skipped": skipped,
    }
    return policy, log, summary


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, path)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen", help="write a new Ed25519 authority seed")
    keygen.add_argument("seed_file")
    run = commands.add_parser("export", help="write policy.json and issues.ndjson")
    run.add_argument(
        "--repository",
        required=True,
        help="canonical repository identity, e.g. forkmesh.com/OWNER/NAME",
    )
    run.add_argument("--signing-key", required=True, help="base64url Ed25519 seed file")
    run.add_argument("--checkout", default=".", help="repository working tree")
    run.add_argument("--out", help="output directory (default CHECKOUT/.forkmesh/collaboration)")
    return parser


def main(argv=None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "keygen":
            path = Path(args.seed_file)
            if path.exists():
                raise UsageError("refusing to overwrite %s" % path)
            seed = os.urandom(32)
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                handle.write(_b64url(seed) + "\n")
            print(_b64url(public_key(seed)))
            return 0
        checkout = Path(args.checkout)
        if not (checkout / ".forkmesh" / "issues").is_dir():
            raise UsageError("no .forkmesh/issues directory under %s" % checkout)
        policy, log, summary = export(checkout, args.repository, load_seed(args.signing_key))
    except (UsageError, OSError) as exc:
        print("collaboration export failed: %s" % exc, file=sys.stderr)
        return 2
    out = Path(args.out) if args.out else checkout / ".forkmesh" / "collaboration"
    _write_atomic(out / "policy.json", canonical_json(policy) + b"\n")
    _write_atomic(out / "issues.ndjson", log.export_ndjson())
    summary["policy"] = str(out / "policy.json")
    summary["log"] = str(out / "issues.ndjson")
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
