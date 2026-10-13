"""Human-readable synchronization status, reference implementation.

One state model explains where a repository or collaboration object has
reached, how many copies were verified, and whether the visible state is
current or conflicted. The contract lives in
``conformance/sync-status/v1/model.json`` and the shared fixture in
``conformance/sync-status/v1/fixture.json``; the Go daemon, desktop, and mobile
clients carry independent implementations that must match this one case for
case (``www/docs/sync-status.md``).

Output is built only from counts, timestamps, and closed vocabularies. Node
names, peer addresses, object names, and free-form error text in the evidence
never reach the summary, so a status is safe to show outside the object's
visibility policy.
"""

SCHEMA = "forkmesh-sync-status-v1"

STALE_AFTER_MS = 60 * 60 * 1000
SYNC_DEADLINE_MS = 15 * 60 * 1000

STATES = (
    "queued", "syncing", "verified", "stale", "degraded", "conflicted",
    "paused", "external-only", "failed",
)

LABELS = {
    "queued": "Queued",
    "syncing": "Syncing",
    "verified": "Verified",
    "stale": "Stale",
    "degraded": "Degraded",
    "conflicted": "Conflicted",
    "paused": "Paused",
    "external-only": "External only",
    "failed": "Failed",
}

SEVERITY = {
    "queued": "info",
    "syncing": "info",
    "verified": "ok",
    "stale": "warning",
    "degraded": "warning",
    "conflicted": "error",
    "paused": "info",
    "external-only": "info",
    "failed": "error",
}

HEADLINES = {
    "queued": "Waiting to synchronize",
    "syncing": "Synchronizing now",
    "verified": "Every copy is verified and current",
    "stale": "The visible state may be out of date",
    "degraded": "Verified, but some copies are not current",
    "conflicted": "Copies disagree about the latest state",
    "paused": "Synchronization is paused",
    "external-only": "Only stored on the external provider so far",
    "failed": "Synchronization failed",
}

# Codes a client may report as evidence.lastError. Anything else is "unknown".
REPORTED_ERRORS = (
    "rejected", "signature_invalid", "unauthorized", "provider_unavailable",
    "network", "quota_exceeded",
)

ACTIONS = {
    "conflict": "Review the conflicting changes and choose which state to keep.",
    "rejected": "A copy rejected the update; check the node's signing key and push again.",
    "signature_invalid": "The update's signature did not verify; re-sign it from a trusted device.",
    "unauthorized": "This node is not authorized to publish; reconnect the account and retry.",
    "provider_unavailable": "The external provider did not respond; ForkMesh retries automatically.",
    "network": "A network error interrupted synchronization; it retries when the connection returns.",
    "quota_exceeded": "A service quota was reached; synchronization resumes when the quota resets.",
    "sync_delayed": "Synchronization is taking longer than expected; check that the source node is online.",
    "unreachable": "One or more copies could not be reached; check that the mirrors are online.",
    "unverified": "A copy reports the latest state without a verified signature; wait for it to re-attest.",
    "copies_behind": "Some copies have not received the latest state yet; they normally catch up within minutes.",
    "confirmation_expired": "No copy has confirmed the state recently; check that the mirrors are online.",
    "unconfirmed": "No copy has confirmed the latest state yet.",
    "unknown": "An unexpected error stopped synchronization; see the node's log for details.",
}

COPY_STATUSES = ("current", "behind", "unreachable", "rejected")


def _int_or_none(value):
    # Booleans are ints in Python but never timestamps; floats from JSON
    # clients are truncated the way every other implementation truncates.
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value)


def _plural(count, singular, plural):
    return "%d %s" % (count, singular if count == 1 else plural)


def _age(ms):
    minutes = ms // 60000
    if minutes < 60:
        return "%d min" % minutes
    hours = ms // 3600000
    if hours < 48:
        return "%d h" % hours
    return "%d d" % (ms // 86400000)


def evaluate(evidence):
    """Return the machine-readable and plain-language status for evidence."""
    evidence = evidence if isinstance(evidence, dict) else {}
    now = _int_or_none(evidence.get("now")) or 0
    copies = [
        copy for copy in (evidence.get("copies") or [])
        if isinstance(copy, dict)
    ] if isinstance(evidence.get("copies"), list) else []
    required = _int_or_none(evidence.get("requiredCopies"))
    required = max(1, required if required is not None else 1)
    conflicts = _int_or_none(evidence.get("conflicts")) or 0
    queued_at = _int_or_none(evidence.get("queuedAt"))
    started_at = _int_or_none(evidence.get("syncStartedAt"))
    source_updated_at = _int_or_none(evidence.get("sourceUpdatedAt"))
    last_error = evidence.get("lastError")
    if not isinstance(last_error, str) or not last_error:
        last_error = None
    elif last_error not in REPORTED_ERRORS:
        last_error = "unknown"

    total = len(copies)
    reached = verified_current = trusted_current = 0
    rejected = unreachable = unverified = 0
    last_confirmed = current_confirmed = None
    for copy in copies:
        status = copy.get("status")
        if status not in COPY_STATUSES:
            status = "unreachable"
        verified = copy.get("verified") is True
        confirmed_at = _int_or_none(copy.get("confirmedAt"))
        if status != "unreachable":
            reached += 1
        if status == "rejected":
            rejected += 1
        elif status == "unreachable":
            unreachable += 1
        if status == "current" and verified:
            verified_current += 1
            if copy.get("trusted") is True:
                trusted_current += 1
            if confirmed_at is not None:
                current_confirmed = (
                    confirmed_at if current_confirmed is None
                    else max(current_confirmed, confirmed_at))
        elif status == "current":
            unverified += 1
        if verified and status != "rejected" and confirmed_at is not None:
            last_confirmed = (
                confirmed_at if last_confirmed is None
                else max(last_confirmed, confirmed_at))

    in_flight = started_at is not None
    delayed = in_flight and now - started_at > SYNC_DEADLINE_MS

    if evidence.get("paused") is True:
        state, error = "paused", None
    elif conflicts > 0:
        state, error = "conflicted", "conflict"
    elif (evidence.get("externalOnly") is True and total == 0
          and last_error is None):
        state, error = "external-only", None
    elif verified_current >= required:
        if current_confirmed is None or now - current_confirmed > STALE_AFTER_MS:
            state, error = "stale", "confirmation_expired"
        elif verified_current < total:
            state = "degraded"
            if rejected:
                error = "rejected"
            elif unreachable:
                error = "unreachable"
            elif unverified:
                error = "unverified"
            else:
                error = "copies_behind"
        else:
            state, error = "verified", None
    elif in_flight and not delayed:
        state, error = "syncing", None
    elif queued_at is not None and not in_flight:
        state, error = "queued", None
    elif rejected:
        state, error = "failed", last_error or "rejected"
    elif last_error is not None:
        state, error = "failed", last_error
    elif delayed:
        state, error = "stale", "sync_delayed"
    elif total and unreachable == total:
        state, error = "stale", "unreachable"
    else:
        state, error = "stale", "unconfirmed"

    if state == "verified":
        lag = 0
    elif source_updated_at is not None:
        lag = max(0, now - source_updated_at)
    else:
        lag = None

    parts = [
        HEADLINES[state] + ".",
        "%d of %s verified, %s, %s reached." % (
            verified_current,
            _plural(total, "copy", "copies"),
            _plural(trusted_current, "trusted mirror", "trusted mirrors"),
            _plural(reached, "node", "nodes"),
        ),
    ]
    if last_confirmed is None:
        parts.append("Never confirmed.")
    elif now - last_confirmed < 60000:
        parts.append("Last confirmed just now.")
    else:
        parts.append("Last confirmed %s ago." % _age(now - last_confirmed))
    if lag is not None and lag >= 60000:
        parts.append("%s behind the source." % _age(lag))
    action = ACTIONS[error] if error else None
    if action:
        parts.append(action)

    return {
        "schema": SCHEMA,
        "state": state,
        "label": LABELS[state],
        "severity": SEVERITY[state],
        "current": state in ("verified", "degraded"),
        "totalCopies": total,
        "nodesReached": reached,
        "verifiedCopies": verified_current,
        "trustedMirrors": trusted_current,
        "lastConfirmedAt": last_confirmed,
        "lagMs": lag,
        "error": error,
        "action": action,
        "summary": " ".join(parts),
    }


def mirrors_payload_evidence(payload):
    """Evidence for a GET /api/repo/<owner>/<repo>/mirrors payload."""
    payload = payload if isinstance(payload, dict) else {}
    summary = payload.get("summary") if isinstance(payload.get("summary"), dict) else {}
    copies = []
    for row in payload.get("mirrors") or []:
        if not isinstance(row, dict):
            continue
        integrity = row.get("integrity")
        if row.get("status") != "online":
            status = "unreachable"
        elif integrity == "rejected":
            status = "rejected"
        elif row.get("behind") is True or integrity == "healing":
            status = "behind"
        else:
            status = "current"
        copies.append({
            "status": status,
            "verified": integrity == "ok",
            "trusted": row.get("cloneAvailable") is True
            or row.get("endpointHealthy") is True,
            "confirmedAt": _int_or_none(row.get("lastSync")),
        })
    freshest = _int_or_none(summary.get("freshestSync"))
    return {
        "now": _int_or_none(payload.get("generatedAt")) or 0,
        "sourceUpdatedAt": freshest if freshest else None,
        "copies": copies,
    }


def mirrors_payload_status(payload):
    return evaluate(mirrors_payload_evidence(payload))
