#!/usr/bin/env python3
"""Independently verify a signed ForkMesh repository migration report.

This verifier deliberately imports nothing from ForkMesh.  It re-derives the
report hash, the Ed25519 signature, the reconciliation arithmetic, the
superseded-report chain and, when the exported objects are supplied, every
object payload hash, source key and the manifest digest.

    python3 tools/verify_migration_report.py report.json \
        --objects objects.json --public-key <base64url router key>

``report.json`` is the body of ``GET /api/repository-imports/migrations/{id}/report``.
``objects.json`` is either a JSON list of object records or one or more
``…/objects`` export pages (a list of page bodies, or NDJSON of page bodies).

Exit status: 0 verified, 1 verification failed, 2 unreadable input.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import sys
from pathlib import Path


REPORT_SCHEMA = "forkmesh-repository-migration-report-v1"
EXPORT_SCHEMA = "forkmesh-repository-migration-report-export-v1"
MANIFEST_SCHEMA = "forkmesh-repository-migration-manifest-v1"
SIGNATURE_DOMAIN = "forkmesh-repository-migration-report-v1"
OBJECT_KEY_DOMAIN = "forkmesh-migration-object-v1"
CLASSES = ("imported", "skipped", "failed", "external")


def canonical_json(value) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def sha256_prefixed(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def object_key(object_type: str, source_id: str) -> str:
    body = "\n".join((str(object_type or ""), str(source_id or ""))).encode("utf-8")
    return hashlib.sha256(OBJECT_KEY_DOMAIN.encode("utf-8") + b"\n" + body).hexdigest()


# --- RFC 8032 Ed25519 verification (pure Python, strict encodings) ----------

_Q = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _Q - 2, _Q) % _Q
_I = pow(2, (_Q - 1) // 4, _Q)


def _xrecover(y: int) -> int | None:
    xx = (y * y - 1) * pow(_D * y * y + 1, _Q - 2, _Q)
    x = pow(xx, (_Q + 3) // 8, _Q)
    if (x * x - xx) % _Q:
        x = x * _I % _Q
    if (x * x - xx) % _Q:
        return None
    return x


_BY = 4 * pow(5, _Q - 2, _Q) % _Q
_BX = _xrecover(_BY)
_BASE = (_BX if _BX % 2 == 0 else _Q - _BX, _BY)
_IDENTITY = (0, 1)


def _add(p, q):
    x1, y1 = p
    x2, y2 = q
    t = _D * x1 * x2 * y1 * y2 % _Q
    x3 = (x1 * y2 + x2 * y1) * pow(1 + t, _Q - 2, _Q)
    y3 = (y1 * y2 + x1 * x2) * pow(1 - t, _Q - 2, _Q)
    return (x3 % _Q, y3 % _Q)


def _mult(point, scalar: int):
    result = _IDENTITY
    while scalar:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _decode_point(raw: bytes):
    if len(raw) != 32:
        return None
    encoded = int.from_bytes(raw, "little")
    y = encoded & ((1 << 255) - 1)
    if y >= _Q:
        return None
    x = _xrecover(y)
    if x is None:
        return None
    if x == 0 and encoded >> 255:
        return None
    if (x & 1) != encoded >> 255:
        x = _Q - x
    return (x, y)


def ed25519_verify(public_key: bytes, signature: bytes, message: bytes) -> bool:
    if len(public_key) != 32 or len(signature) != 64:
        return False
    public = _decode_point(public_key)
    nonce = _decode_point(signature[:32])
    scalar = int.from_bytes(signature[32:], "little")
    if public is None or nonce is None or scalar >= _L:
        return False
    if public == _IDENTITY or _mult(public, _L) != _IDENTITY:
        return False
    challenge = int.from_bytes(
        hashlib.sha512(signature[:32] + public_key + message).digest(), "little") % _L
    return _mult(_BASE, scalar) == _add(nonce, _mult(public, challenge))


def _b64url(value: str) -> bytes:
    text = str(value or "")
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


# --- checks -----------------------------------------------------------------


class Result:
    def __init__(self):
        self.checks = []

    def add(self, check: str, ok: bool, detail: str = "", level: str = ""):
        self.checks.append({
            "check": check,
            "result": level or ("pass" if ok else "fail"),
            "detail": detail,
        })

    @property
    def ok(self) -> bool:
        return all(item["result"] != "fail" for item in self.checks)


def verify_signature(result, label, digest, signature, public_key, hmac_key):
    message = (SIGNATURE_DOMAIN + "\n" + str(digest)).encode("utf-8")
    signature = signature if isinstance(signature, dict) else {}
    algorithm = signature.get("algorithm")
    if algorithm == "ed25519":
        embedded = str(signature.get("publicKey") or "")
        key = public_key or embedded
        if public_key and embedded and public_key != embedded:
            result.add(label, False, "report was signed by a different key")
            return
        try:
            valid = ed25519_verify(_b64url(key), _b64url(signature.get("signature")), message)
        except (ValueError, TypeError):
            valid = False
        detail = "Ed25519 signature by " + key
        if valid and not public_key:
            detail += " (key taken from the report; pass --public-key to pin it)"
        result.add(label, valid, detail if valid else "Ed25519 signature does not match")
        return
    if algorithm == "hmac-sha256":
        if not hmac_key:
            result.add(label, True, "HMAC relay signature cannot be checked without "
                       "the relay key; hashes are still verified", level="unverifiable")
            return
        expected = hmac.new(hmac_key.encode("utf-8"), message, hashlib.sha256).hexdigest()
        result.add(label, hmac.compare_digest(expected, str(signature.get("signature") or "")),
                   "HMAC relay signature")
        return
    result.add(label, False, "unsigned or unknown signature algorithm")


def verify_reconciliation(result, report):
    reconciliation = report.get("reconciliation") or {}
    rows = reconciliation.get("rows") or {}
    totals = {key: 0 for key in ("source",) + CLASSES}
    unclassified = 0
    ok = bool(rows)
    for kind, row in rows.items():
        values = {key: int(row.get(key) or 0) for key in ("source",) + CLASSES}
        remainder = values["source"] - sum(values[key] for key in CLASSES)
        if remainder != int(row.get("unclassified") or 0):
            result.add("reconciliation:" + kind, False, "row arithmetic was altered")
            ok = False
        reasons = row.get("skippedReasons") or {}
        if sum(int(value or 0) for value in reasons.values()) != values["skipped"]:
            result.add("reconciliation:" + kind, False, "skipped reasons do not add up")
            ok = False
        unclassified += abs(remainder)
        for key in totals:
            totals[key] += values[key]
    for key, value in totals.items():
        if int((reconciliation.get("totals") or {}).get(key) or 0) != value:
            result.add("reconciliation:totals", False, key + " total does not match rows")
            ok = False
    if unclassified != int(reconciliation.get("unclassified") or 0):
        result.add("reconciliation:unclassified", False, "unclassified count was altered")
        ok = False
    if bool(reconciliation.get("reconciled")) != (unclassified == 0):
        result.add("reconciliation:claim", False, "reconciled flag contradicts the rows")
        ok = False
    result.add("reconciliation", ok and unclassified == 0,
               "%d source records, %d unclassified" % (totals["source"], unclassified))


def load_objects(path: str):
    raw = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    raw = raw.strip()
    try:
        values = [json.loads(raw)]
    except ValueError:
        values = [json.loads(line) for line in raw.splitlines() if line.strip()]
    records = []

    def collect(value):
        if isinstance(value, list):
            for item in value:
                collect(item)
        elif isinstance(value, dict) and isinstance(value.get("objects"), list):
            records.extend(value["objects"])
        elif isinstance(value, dict) and "objectType" in value:
            records.append(value)
        else:
            raise ValueError("unrecognized objects export")

    for value in values:
        collect(value)
    return records


def verify_objects(result, report, records):
    manifest = ((report.get("checksums") or {}).get("objectManifest") or {})
    rows = []
    by_type = {}
    for record in records:
        kind = str(record.get("objectType") or "")
        source_id = str((record.get("source") or {}).get("id") or "")
        rows.append([kind, object_key(kind, source_id), sha256_prefixed(canonical_json(record))])
        by_type[kind] = by_type.get(kind, 0) + 1
    rows.sort()
    digest = sha256_prefixed(canonical_json({"schema": MANIFEST_SCHEMA, "objects": rows}))
    result.add(
        "objects:manifest",
        digest == manifest.get("digest") and len(rows) == int(manifest.get("count") or -1),
        "recomputed %d payload hashes and source keys" % len(rows)
        if digest == manifest.get("digest")
        else "exported objects do not match the signed manifest",
    )
    store = report.get("store") or {}
    reconciliation = (report.get("reconciliation") or {}).get("rows") or {}
    retained = store.get("retainedFromPriorScan") or {}
    mismatched = sorted(
        kind for kind in set(by_type) | set(retained)
        if by_type.get(kind, 0) != int((reconciliation.get(kind) or {}).get("imported") or 0)
        + int(retained.get(kind) or 0))
    result.add("objects:counts", not mismatched,
               "stored objects match imported counts" if not mismatched
               else "count mismatch for " + ", ".join(mismatched))


def verify_bundle(bundle, public_key="", hmac_key="", records=None):
    result = Result()
    if not isinstance(bundle, dict) or bundle.get("schema") != EXPORT_SCHEMA:
        result.add("schema", False, "not a migration report export")
        return result
    report = bundle.get("report")
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        result.add("schema", False, "report body missing (older history entries keep only hashes)")
        return result
    digest = sha256_prefixed(canonical_json(report))
    result.add("report:hash", digest == bundle.get("reportHash"),
               digest if digest == bundle.get("reportHash") else "report content was altered")
    verify_signature(result, "report:signature", bundle.get("reportHash"),
                     bundle.get("signature"), public_key, hmac_key)
    verify_reconciliation(result, report)

    history = bundle.get("history") or []
    previous = None
    chain_ok = True
    for entry in history:
        body = entry.get("report")
        if isinstance(body, dict):
            if sha256_prefixed(canonical_json(body)) != entry.get("reportHash"):
                result.add("history:%s" % entry.get("sequence"), False, "historical report altered")
                chain_ok = False
            link = body.get("supersedes")
            expected = ({"sequence": previous["sequence"], "reportHash": previous["reportHash"]}
                        if previous else None)
            if link != expected:
                result.add("history:%s" % entry.get("sequence"), False, "supersession link broken")
                chain_ok = False
        verify_signature(result, "history:%s:signature" % entry.get("sequence"),
                         entry.get("reportHash"), entry.get("signature"), public_key, hmac_key)
        previous = entry
    selected = [entry for entry in history if entry.get("reportHash") == bundle.get("reportHash")]
    if history and not selected:
        result.add("history", False, "selected report is not in its own history")
        chain_ok = False
    result.add("history", chain_ok, "%d report(s) in the supersession chain" % len(history))

    if records is not None:
        verify_objects(result, report, records)
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report", help="report export JSON, or - for stdin")
    parser.add_argument("--objects", help="objects export (JSON list, page bodies, or NDJSON)")
    parser.add_argument("--public-key", default="", help="trusted Ed25519 key (base64url)")
    parser.add_argument("--hmac-key", default="", help="relay key for HMAC-signed reports")
    args = parser.parse_args(argv)
    try:
        raw = sys.stdin.read() if args.report == "-" else Path(args.report).read_text(encoding="utf-8")
        bundle = json.loads(raw)
        records = load_objects(args.objects) if args.objects else None
    except (OSError, ValueError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    result = verify_bundle(bundle, args.public_key, args.hmac_key, records)
    report = bundle.get("report") if isinstance(bundle, dict) else None
    print(json.dumps({
        "ok": result.ok,
        "jobId": (report or {}).get("jobId"),
        "sequence": (report or {}).get("sequence"),
        "outcome": (report or {}).get("outcome"),
        "checks": result.checks,
    }, indent=2, sort_keys=True))
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
