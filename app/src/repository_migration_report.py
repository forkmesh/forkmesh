"""Transparent, signed migration reports for resumable repository imports.

A migration report answers four questions about one import: what arrived,
what was deliberately skipped, what failed (and can be retried), and what
remains outside ForkMesh.  It is built from three inputs that already exist:

* the page ledger kept by :mod:`repository_imports` while it transfers pages
  (how many records the provider returned, and how each was classified),
* the stored ``repository_migration_objects`` rows (what actually landed), and
* the signed state receipt produced at completion.

Every observed source record must land in exactly one of ``imported``,
``skipped``, ``failed``, or ``external``.  Anything left over is reported as
``unclassified`` and fails the reconciliation check instead of being hidden.

The report is canonical JSON, hashed, and signed over a domain-separated
message.  ``tools/verify_migration_report.py`` re-derives every hash and the
reconciliation arithmetic without importing this module, so tampering with
either the report or the exported objects is detected independently.

Like :mod:`repository_migration`, this module has no Worker dependencies and
never receives a provider credential.
"""

import hashlib

import repository_migration as migration


REPORT_SCHEMA = "forkmesh-repository-migration-report-v1"
EXPORT_SCHEMA = "forkmesh-repository-migration-report-export-v1"
MANIFEST_SCHEMA = "forkmesh-repository-migration-manifest-v1"
SIGNATURE_DOMAIN = "forkmesh-repository-migration-report-v1"
ARTIFACT_TYPE = "release_assets"
CLASSES = ("imported", "skipped", "failed", "external")

# Reports are stored inside the encrypted job row, so every list is bounded and
# carries an explicit overflow count.  Digests always cover the full lists.
MAX_REPORT_ITEMS = 100
MAX_REPORT_REFS = 200
MAX_FULL_HISTORY = 5
MAX_FAILED_WORK = 500

REASON_REMEDIATION = {
    "provider_authorization_failed": (
        "The provider refused the request. Reconnect with a token that can "
        "read this repository, then retry failed work."),
    "provider_transfer_failed": (
        "The provider returned an error for this page. Retry failed work; "
        "only the affected pages are requested again."),
    "provider_page_invalid_shape": (
        "The provider returned a page ForkMesh could not read. Retry failed "
        "work; if it keeps failing, report the source URL to ForkMesh."),
    "adapter_stream_unavailable": (
        "The provider no longer offers this stream. Start a new full import "
        "to rescan the repository."),
}
_RECORD_REMEDIATION = (
    "The provider record cannot be stored without losing data, so it was not "
    "imported. It stays listed as failed; retry failed work after a ForkMesh "
    "update, or keep the original at its source URL.")


def empty_counts():
    return {"source": 0, "accepted": 0, "excluded": 0, "failed": 0}


def _counts(value):
    value = value if isinstance(value, dict) else {}
    return {key: max(0, int(value.get(key) or 0)) for key in empty_counts()}


def page_key(stream_id, page):
    return "%s#%d" % (str(stream_id or ""), max(1, int(page or 1)))


def new_ledger():
    return {
        "totals": {kind: empty_counts() for kind in migration.OBJECT_TYPES},
        "pages": {},
    }


def ledger(job):
    """Return the job's ledger, creating one for jobs that predate reports."""
    value = job.get("ledger")
    if not isinstance(value, dict):
        value = new_ledger()
        job["ledger"] = value
    value.setdefault("totals", {})
    value.setdefault("pages", {})
    for kind in migration.OBJECT_TYPES:
        value["totals"][kind] = _counts(value["totals"].get(kind))
    return value


def record_page(job, phase, stream_id, page, counts):
    """Record one page's classification, replacing any earlier attempt.

    Pages are keyed by stream and page number, so a page fetched again after a
    crash replaces its own entry instead of counting its records twice.
    """
    entry = _counts(counts)
    entry["phase"] = str(phase)
    ledger(job)["pages"][page_key(stream_id, page)] = entry
    return entry


def fold_pages(job):
    """Move finished-stream page entries into per-type totals."""
    book = ledger(job)
    for entry in book["pages"].values():
        phase = str(entry.get("phase") or "")
        if phase not in book["totals"]:
            continue
        adjust_totals(job, phase, entry)
    book["pages"] = {}


def adjust_totals(job, phase, add=None, subtract=None):
    totals = ledger(job)["totals"].setdefault(phase, empty_counts())
    add = _counts(add)
    subtract = _counts(subtract)
    for key in empty_counts():
        totals[key] = max(0, totals[key] + add[key] - subtract[key])


def failed_work(job):
    value = job.get("failedWork")
    if not isinstance(value, dict):
        value = {}
        job["failedWork"] = value
    return value


def note_failed_work(job, kind, phase, stream_id, page, reasons, counts=None,
                     attempts=0, failed_at=0):
    """Remember retryable work by page.

    ``kind`` is ``page`` when individual records on a fetched page failed, or
    ``stream_from_page`` when a page could not be fetched at all and the rest
    of that stream was deferred (its record count is therefore unknown).
    """
    work = failed_work(job)
    key = page_key(stream_id, page)
    if key not in work and len(work) >= MAX_FAILED_WORK:
        job["failedWorkOverflow"] = int(job.get("failedWorkOverflow") or 0) + 1
        return None
    previous = work.get(key) or {}
    work[key] = {
        "kind": kind,
        "phase": str(phase),
        "stream": str(stream_id),
        "page": max(1, int(page or 1)),
        "reasons": sorted(set(str(reason) for reason in reasons or [])),
        "counts": _counts(counts),
        "attempts": max(int(attempts or 0), int(previous.get("attempts") or 0)),
        "firstFailedAt": int(previous.get("firstFailedAt") or failed_at or 0),
    }
    return work[key]


def retry_queue(job):
    """Failed work in transfer order: phase, then stream, then page."""
    order = {kind: index for index, kind in enumerate(migration.PHASES)}
    return sorted(
        failed_work(job),
        key=lambda key: (
            order.get(failed_work(job)[key].get("phase"), len(order)),
            failed_work(job)[key].get("stream") or "",
            int(failed_work(job)[key].get("page") or 1),
        ),
    )


def remediation_for(reason):
    reason = str(reason or "")
    if reason in REASON_REMEDIATION:
        return REASON_REMEDIATION[reason]
    if reason.startswith("provider_object_") or reason in (
            "unsupported_object_type",):
        return _RECORD_REMEDIATION
    return REASON_REMEDIATION["provider_transfer_failed"]


def item_source_id(stream, item):
    """Best-effort provider id for a record that could not be normalized."""
    if not isinstance(item, dict):
        return ""
    for name in tuple(stream.get("idFields") or ("id", "number", "iid")):
        value = item.get(name)
        if isinstance(value, (str, int)) and not isinstance(value, bool) \
                and str(value) != "":
            source_id = str(value)[:160]
            parent = str(stream.get("parent") or "")
            prefix = str(stream.get("sourcePrefix") or "")
            if parent:
                source_id = parent + ":" + source_id
            if prefix:
                source_id = prefix + ":" + source_id
            return source_id
    return ""


def classify_page(adapter, stream, items, repository_id):
    """Normalize one provider page, classifying every record exactly once.

    Returns ``(normalized, counts, failures)``.  One malformed record no longer
    fails its whole page: it is counted as failed with its source id and
    reason, and the rest of the page is imported.
    """
    counts = empty_counts()
    counts["source"] = len(items)
    normalized = []
    failures = []
    for item in items:
        if stream.get("excludePulls") and isinstance(item, dict) \
                and item.get("pull_request"):
            # Providers list pull requests on the issues endpoint too; they are
            # imported (and counted) by the pull request stream instead.
            counts["excluded"] += 1
            continue
        try:
            normalized.append(migration.normalize_object(
                adapter, stream, item, repository_id))
        except migration.MigrationContractError as exc:
            counts["failed"] += 1
            failures.append({
                "sourceId": item_source_id(stream, item),
                "reason": str(exc),
            })
    counts["accepted"] = len(normalized)
    return normalized, counts, failures


def manifest_digest(entries):
    """Digest of every stored ``(objectType, sourceKey, payloadHash)``."""
    rows = sorted(
        [str(kind), str(key), str(digest)] for kind, key, digest in entries)
    body = migration.canonical_json({"schema": MANIFEST_SCHEMA, "objects": rows})
    return "sha256:" + hashlib.sha256(body).hexdigest()


def report_hash(report):
    return "sha256:" + hashlib.sha256(migration.canonical_json(report)).hexdigest()


def signing_message(digest):
    return (SIGNATURE_DOMAIN + "\n" + str(digest or "")).encode("utf-8")


def _payload(record):
    payload = record.get("providerPayload") if isinstance(record, dict) else None
    return payload if isinstance(payload, dict) else {}


def _ref_entry(record):
    source = record.get("source") if isinstance(record.get("source"), dict) else {}
    source_id = str(source.get("id") or "")
    kind, _, name = source_id.partition(":")
    payload = _payload(record)
    commit = payload.get("commit") if isinstance(payload.get("commit"), dict) else {}
    target = commit.get("sha") or commit.get("id") or payload.get("target") or ""
    return {
        "kind": kind or "ref",
        "name": str(payload.get("name") or name)[:255],
        "target": str(target)[:80],
    }


def _bounded(items, limit):
    items = list(items)
    return items[:limit], max(0, len(items) - limit)


def _plural(count, singular, plural=None):
    return "%d %s" % (count, singular if count == 1 else (plural or singular + "s"))


def build_report(job, objects, outcome, generated_at, sequence, supersedes=None,
                 objects_path="", report_path=""):
    """Build the unsigned canonical report document.

    ``objects`` are stored rows: ``objectType``, ``sourceKey``, ``payloadHash``,
    ``generation``, and the decrypted ``record``.
    """
    generation = int(job.get("generation") or 1)
    book = ledger(job)
    totals = {kind: dict(book["totals"][kind]) for kind in migration.OBJECT_TYPES}
    for entry in book["pages"].values():
        phase = str(entry.get("phase") or "")
        if phase in totals:
            for key in empty_counts():
                totals[phase][key] += int(entry.get(key) or 0)

    imported = {kind: 0 for kind in migration.OBJECT_TYPES}
    retained = {kind: 0 for kind in migration.OBJECT_TYPES}
    refs = []
    artifacts = []
    for row in objects:
        kind = str(row.get("objectType") or "")
        if kind not in imported:
            continue
        current = int(row.get("generation") or 0) == generation
        (imported if current else retained)[kind] += 1
        record = row.get("record") if isinstance(row.get("record"), dict) else {}
        if kind == "git_refs":
            refs.append(_ref_entry(record))
        elif kind == "releases" and current:
            payload = _payload(record)
            release = str(
                payload.get("tag_name") or payload.get("tag")
                or (record.get("source") or {}).get("id") or "")[:160]
            for index, asset in enumerate(migration._release_assets(payload)):
                artifacts.append({
                    "release": release,
                    "name": migration._release_asset_name(asset, index),
                    "providerChecksum": migration._release_asset_digest(asset),
                    "retained": False,
                })

    rows = {}
    for kind in migration.OBJECT_TYPES:
        counts = totals[kind]
        duplicates = max(0, counts["accepted"] - imported[kind])
        row = {
            "source": counts["source"],
            "imported": imported[kind],
            "skipped": counts["excluded"] + duplicates,
            "failed": counts["failed"],
            "external": 0,
            "skippedReasons": {
                "representedByAnotherType": counts["excluded"],
                "duplicateSourceRecord": duplicates,
            },
        }
        rows[kind] = row
    # Release binaries are declared by the source but their bytes are not
    # copied by this metadata transfer: every one is an external record.
    rows[ARTIFACT_TYPE] = {
        "source": len(artifacts), "imported": 0, "skipped": 0, "failed": 0,
        "external": len(artifacts),
        "skippedReasons": {"representedByAnotherType": 0,
                           "duplicateSourceRecord": 0},
    }
    grand = {key: 0 for key in ("source",) + CLASSES}
    for row in rows.values():
        row["unclassified"] = row["source"] - sum(row[key] for key in CLASSES)
        for key in grand:
            grand[key] += row[key]
    grand["unclassified"] = grand["source"] - sum(grand[key] for key in CLASSES)
    unclassified_total = sum(abs(row["unclassified"]) for row in rows.values())

    work = failed_work(job)
    unmeasured = sorted(
        ({"phase": entry["phase"], "stream": entry["stream"],
          "fromPage": entry["page"], "reasons": entry["reasons"]}
         for entry in work.values() if entry.get("kind") == "stream_from_page"),
        key=lambda entry: (entry["phase"], entry["stream"], entry["fromPage"]))
    failed_items = sorted(
        ({"kind": entry["kind"], "phase": entry["phase"],
          "stream": entry["stream"], "page": entry["page"],
          "reasons": entry["reasons"],
          "failedRecords": int((entry.get("counts") or {}).get("failed") or 0),
          "attempts": int(entry.get("attempts") or 0)}
         for entry in work.values()),
        key=lambda entry: (entry["phase"], entry["stream"], entry["page"]))
    report_state = job.get("report") if isinstance(job.get("report"), dict) else {}
    events = list(report_state.get("failures") or [])

    manifest_entries = [
        (row.get("objectType"), row.get("sourceKey"), row.get("payloadHash"))
        for row in objects]
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "algorithm": "sha256",
        "digest": manifest_digest(manifest_entries),
        "count": len(manifest_entries),
    }
    refs.sort(key=lambda ref: (ref["kind"], ref["name"], ref["target"]))
    artifacts.sort(key=lambda item: (item["release"], item["name"]))
    ref_items, ref_overflow = _bounded(refs, MAX_REPORT_REFS)
    artifact_items, artifact_overflow = _bounded(artifacts, MAX_REPORT_ITEMS)
    event_items, event_overflow = _bounded(events, MAX_REPORT_ITEMS)
    work_items, work_overflow = _bounded(failed_items, MAX_REPORT_ITEMS)

    coverage = report_state.get("releaseCoverage")
    coverage = coverage if isinstance(coverage, dict) else {}
    external = []
    for note in sorted(set(str(value) for value in report_state.get("unsupported") or [])):
        external.append({"kind": "unsupported_source_feature", "detail": note,
                         "remediation": "Mirror or export this separately; "
                                        "this import does not copy it."})
    for skipped in report_state.get("skipped") or []:
        if isinstance(skipped, dict):
            external.append({
                "kind": "provider_endpoint_unavailable",
                "detail": "%s/%s" % (skipped.get("phase"), skipped.get("stream")),
                "remediation": "The provider did not offer this optional "
                               "stream; nothing was available to copy.",
            })
    if artifacts:
        external.append({
            "kind": "release_asset_bytes",
            "detail": _plural(len(artifacts), "release asset")
            + " declared by the source; bytes were not copied.",
            "remediation": "Download the assets from the source release and "
                           "publish them as ForkMesh release assets.",
        })
    for label in ("missingTags", "missingManifests", "missingProvenance"):
        values = coverage.get(label) or []
        if values:
            external.append({
                "kind": "release_" + label[len("missing"):].lower(),
                "detail": "%s: %d" % (label, len(values)),
                "remediation": "See report.checksums.releaseCoverage for the "
                               "exact releases.",
            })

    state = job.get("state") if isinstance(job.get("state"), dict) else None
    job_id = str(job.get("id") or "")
    failed_total = grand["failed"]
    reconciled = unclassified_total == 0
    if outcome in ("abandoned", "in_progress"):
        final_outcome = outcome
    elif not reconciled:
        final_outcome = "unreconciled"
    elif unmeasured:
        final_outcome = "completed_with_unmeasured_sources"
    elif failed_total:
        final_outcome = "completed_with_failures"
    else:
        final_outcome = "verified"

    verification = [
        {
            "id": "reconciliation",
            "result": "pass" if reconciled else "fail",
            "detail": "source = imported + skipped + failed + external for "
                      "every object type; unclassified %d" % unclassified_total,
            "evidence": {"kind": "report_section", "pointer": "/reconciliation"},
        },
        {
            "id": "object-manifest",
            "result": "pass",
            "detail": "%d stored objects hashed into the manifest digest"
                      % manifest["count"],
            "evidence": {
                "kind": "export", "path": objects_path,
                "digest": manifest["digest"], "count": manifest["count"],
                "reproduce": "python3 tools/verify_migration_report.py "
                             "report.json --objects objects.json",
            },
        },
        {
            "id": "state-receipt",
            "result": "pass" if state and state.get("hash") else "not_applicable",
            "detail": "signed completion state for this generation"
                      if state else "the import did not reach completion",
            "evidence": {
                "kind": "receipt", "path": objects_path,
                "stateHash": str((state or {}).get("hash") or ""),
            },
        },
        {
            "id": "failures",
            "result": "pass" if not failed_total and not unmeasured else "fail",
            "detail": "%d failed records, %d unmeasured streams"
                      % (failed_total, len(unmeasured)),
            "evidence": {"kind": "report_section", "pointer": "/failures"},
        },
        {
            "id": "external-dependencies",
            "result": "pass" if not external else "incomplete",
            "detail": _plural(len(external), "unresolved external dependency",
                              "unresolved external dependencies"),
            "evidence": {"kind": "report_section",
                         "pointer": "/externalDependencies"},
        },
        {
            "id": "credential-redaction",
            "result": "pass",
            "detail": "credential-shaped provider fields replaced by a marker",
            "evidence": {
                "kind": "report_section", "pointer": "/redactedCredentialFields",
                "count": int(report_state.get("redactedCredentialFields") or 0),
            },
        },
    ]

    retry_path = report_path.rsplit("/", 1)[0] + "/retry" if report_path else ""
    remediation = []
    reasons = sorted({reason for entry in work.values()
                      for reason in entry.get("reasons") or []})
    for reason in reasons:
        remediation.append({
            "reason": reason,
            "text": remediation_for(reason),
            "action": {"kind": "retry_failed_work", "method": "POST",
                       "path": retry_path},
        })
    if int(job.get("failedWorkOverflow") or 0):
        remediation.append({
            "reason": "failed_work_index_full",
            "text": "More pages failed than ForkMesh can retry one by one. "
                    "Start a new full import to rescan the repository.",
            "action": {"kind": "restart_import", "method": "POST",
                       "path": "/api/repository-imports/migrations"},
        })
    if not reconciled:
        remediation.append({
            "reason": "unclassified_records",
            "text": "Some records could not be accounted for, usually because "
                    "the source changed during the import. Start a new full "
                    "import; a rescan recounts every record.",
            "action": {"kind": "restart_import", "method": "POST",
                       "path": "/api/repository-imports/migrations"},
        })
    for dependency in external:
        remediation.append({
            "reason": dependency["kind"], "text": dependency["remediation"],
            "action": {"kind": "manual"},
        })

    source_label = str(job.get("sourceUrl") or job.get("provider") or "the source")
    summary = [
        "ForkMesh imported %s of %s from %s." % (
            grand["imported"], _plural(grand["source"], "source record"),
            source_label),
    ]
    if grand["skipped"]:
        summary.append(
            "%s skipped because another record type already holds it or the "
            "provider listed it twice." % _plural(grand["skipped"], "record was",
                                                  "records were"))
    if failed_total:
        kinds = sorted(kind for kind, row in rows.items() if row["failed"])
        summary.append("%s and can be retried (%s)." % (
            _plural(failed_total, "record failed", "records failed"),
            ", ".join(kind.replace("_", " ") for kind in kinds)))
    if unmeasured:
        summary.append(
            "%s could not be read, so their records are not counted yet. "
            "Retry failed work to finish them." % _plural(
                len(unmeasured), "provider stream", "provider streams"))
    if artifacts:
        summary.append(
            "%s stay%s external: the files were not copied." % (
                _plural(len(artifacts), "release asset"),
                "s" if len(artifacts) == 1 else ""))
    if reconciled:
        summary.append("Every source record is accounted for.")
    else:
        summary.append(
            "%s unaccounted for. Start a new full import." % _plural(
                unclassified_total, "record is", "records are"))
    if outcome == "abandoned":
        summary.append("The import was stopped before it finished.")
    summary.append("No Git contents were mirrored by this metadata import.")

    return {
        "schema": REPORT_SCHEMA,
        "jobId": job_id,
        "repositoryId": str(job.get("repositoryId") or ""),
        "provider": str(job.get("provider") or ""),
        "providerRepositoryId": str(job.get("providerRepositoryId") or ""),
        "sourceUrl": source_label,
        "generation": generation,
        "sequence": int(sequence),
        "outcome": final_outcome,
        "generatedAt": int(generated_at),
        "supersedes": supersedes,
        "summary": summary,
        "reconciliation": {
            "rows": rows,
            "totals": grand,
            "unclassified": unclassified_total,
            "reconciled": reconciled,
            "sourceTotalsExact": not unmeasured and outcome != "abandoned",
            "unmeasuredSources": unmeasured,
        },
        "store": {
            "objects": len(objects),
            "importedThisGeneration": sum(imported.values()),
            "retainedFromPriorScan": retained,
        },
        "refs": {
            "count": len(refs),
            "digest": "sha256:" + hashlib.sha256(
                migration.canonical_json(refs)).hexdigest(),
            "items": ref_items,
            "overflow": ref_overflow,
        },
        "artifacts": {
            "count": len(artifacts),
            "items": artifact_items,
            "overflow": artifact_overflow,
        },
        "checksums": {
            "objectManifest": manifest,
            "state": {
                "schema": str((state or {}).get("schema") or ""),
                "hash": str((state or {}).get("hash") or ""),
                "algorithm": str((state or {}).get("algorithm") or ""),
                "keyId": str((state or {}).get("keyId") or ""),
                "signature": str((state or {}).get("signature") or ""),
            },
            "releaseCoverage": coverage,
        },
        "retries": {
            "providerRetries": int(job.get("retryCount") or 0),
            "failedWorkRetries": list(job.get("retryHistory") or []),
        },
        "failures": {
            "records": failed_total,
            "unmeasuredStreams": len(unmeasured),
            "events": event_items,
            "eventOverflow": event_overflow + int(
                report_state.get("failureOverflow") or 0),
            "work": work_items,
            "workOverflow": work_overflow + int(job.get("failedWorkOverflow") or 0),
            "retryable": bool(work),
        },
        "externalDependencies": external,
        "redactedCredentialFields": int(
            report_state.get("redactedCredentialFields") or 0),
        "verification": verification,
        "remediation": remediation,
        "links": {"report": report_path, "objects": objects_path,
                  "retry": retry_path},
        "credentialStored": False,
    }


def append_history(job, report, digest, signature):
    """Record a signed report; older reports keep only their hash and signature."""
    history = job.get("reports")
    history = history if isinstance(history, list) else []
    history.append({
        "sequence": int(report["sequence"]),
        "reportHash": digest,
        "outcome": report["outcome"],
        "generation": int(report["generation"]),
        "generatedAt": int(report["generatedAt"]),
        "signature": signature,
        "report": report,
    })
    full = [entry for entry in history if entry.get("report")]
    for entry in full[:max(0, len(full) - MAX_FULL_HISTORY)]:
        entry["report"] = None
    job["reports"] = history
    return history


def latest(job):
    history = job.get("reports")
    return history[-1] if isinstance(history, list) and history else None


def export_bundle(job, sequence=None):
    """Machine-readable export: one report plus the chain it supersedes."""
    history = job.get("reports") if isinstance(job.get("reports"), list) else []
    if not history:
        return None
    selected = history[-1]
    if sequence is not None:
        matches = [entry for entry in history
                   if int(entry.get("sequence") or 0) == int(sequence)]
        if not matches:
            return None
        selected = matches[0]
    chain = []
    for entry in history:
        superseded_by = next(
            (later["sequence"] for later in history
             if int(later["sequence"]) == int(entry["sequence"]) + 1), None)
        chain.append({
            "sequence": entry["sequence"],
            "reportHash": entry["reportHash"],
            "outcome": entry["outcome"],
            "generation": entry.get("generation"),
            "generatedAt": entry["generatedAt"],
            "signature": entry["signature"],
            "supersededBy": superseded_by,
            "report": entry.get("report"),
        })
    return {
        "schema": EXPORT_SCHEMA,
        "report": selected.get("report"),
        "reportHash": selected["reportHash"],
        "signature": selected["signature"],
        "latest": selected is history[-1],
        "history": chain,
    }


def public_summary(job):
    entry = latest(job)
    work = failed_work(job)
    if not entry:
        return {"available": False, "failedWork": len(work)}
    report = entry.get("report") or {}
    reconciliation = report.get("reconciliation") or {}
    return {
        "available": True,
        "sequence": entry["sequence"],
        "reportHash": entry["reportHash"],
        "outcome": entry["outcome"],
        "generatedAt": entry["generatedAt"],
        "totals": dict(reconciliation.get("totals") or {}),
        "reconciled": bool(reconciliation.get("reconciled")),
        "summary": list(report.get("summary") or []),
        "failedWork": len(work),
        "retryable": bool(work),
        "signatureAlgorithm": str((entry.get("signature") or {}).get("algorithm") or ""),
    }


__all__ = (
    "ARTIFACT_TYPE", "EXPORT_SCHEMA", "MANIFEST_SCHEMA", "REPORT_SCHEMA",
    "SIGNATURE_DOMAIN", "adjust_totals", "append_history", "build_report",
    "classify_page", "export_bundle", "failed_work", "fold_pages", "latest",
    "ledger", "manifest_digest", "new_ledger", "note_failed_work", "page_key",
    "public_summary", "record_page", "remediation_for", "report_hash",
    "retry_queue", "signing_message",
)
