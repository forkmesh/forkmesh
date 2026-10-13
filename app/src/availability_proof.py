"""Availability proof and repository risk, reference implementation.

Answers three questions about one logical repository from the evidence the
mirrors API already publishes: is it actually preserved, how independently, and
what is still at risk. The contract lives in
``conformance/availability-proof/v1/model.json`` and the shared fixture in
``conformance/availability-proof/v1/fixture.json``; the model is documented in
``www/docs/availability-proof.md``.

The input is a ``GET /api/repo/<owner>/<repo>/mirrors`` payload after the
Worker overlays each node's signed endpoint receipt (``endpointFresh``,
``endpointHealthy``, ``endpointIntegrity``, ``checkedAt``) and its latest
direct clone probe (``directVerified``, ``directCheckedAt``,
``directRefsSha256``). ``generatedAt`` is the observation time, so the same
payload always reproduces the same report.

Operator accounts never appear in the output. Copies that share an operator
share an opaque operator number, so the report shows how independent the copies
are without naming who runs them.
"""

SCHEMA = "forkmesh-availability-proof-v1"

# A signed endpoint receipt is current for the same window the router accepts
# it (HTTPS_MIRROR_STATUS_FRESH_MS in entry.py). Dashboard values are therefore
# never older than this objective.
FRESHNESS_OBJECTIVE_MS = 10 * 60 * 1000
# Direct clone probes run on demand, so a passing probe stays evidence for a
# day before it is reported stale.
PROBE_FRESH_MS = 24 * 60 * 60 * 1000
# Independent operators that must hold the same verified state. Matches the
# peer quorum the clone integrity gate uses (mirrors.PEER_QUORUM_MIN_NODES).
QUORUM_TARGET = 2

DOCS_URL = "https://forkmesh.com/docs/availability-proof/"

COPY_CLASSES = ("declared", "observed", "verified", "stale", "failing")

PROOF_STATES = ("quorum", "partial", "none")

PROOF_LABELS = {
    "quorum": "Quorum proven",
    "partial": "Below quorum",
    "none": "Not proven",
}

PROOF_HEADLINES = {
    "quorum": "Independent operators hold the same verified copy",
    "partial": "Verified, but not by enough independent operators",
    "none": "No copy has fresh signed evidence",
}

LEVELS = (
    # (minimum score, level, label)
    (60, "critical", "Critical"),
    (30, "high", "High"),
    (10, "moderate", "Moderate"),
    (0, "low", "Low"),
)

# Closed risk vocabulary, in reporting order for equal weights.
RISKS = (
    {
        "code": "no_verified_copy",
        "severity": "critical",
        "weight": 50,
        "title": "No verified copy",
        "detail": "No copy has a fresh signed endpoint receipt or a passing clone probe, so nothing proves the repository can be served.",
        "action": {"kind": "request-mirror", "label": "Ask a node to mirror this repository"},
    },
    {
        "code": "quorum_not_met",
        "severity": "high",
        "weight": 25,
        "title": "Below trusted quorum",
        "detail": "Fewer independent operators than the target hold the same verified state; losing one operator could lose the repository.",
        "action": {"kind": "request-mirror", "label": "Ask another operator to mirror this repository"},
    },
    {
        "code": "state_disagreement",
        "severity": "high",
        "weight": 20,
        "title": "Verified copies disagree",
        "detail": "Clone probes returned different refs digests; only copies matching the best-supported digest count toward quorum.",
        "action": {"kind": "check-direct-url", "label": "Re-run the direct URL checks"},
    },
    {
        "code": "single_operator",
        "severity": "medium",
        "weight": 10,
        "title": "One operator runs every verified copy",
        "detail": "Several verified copies exist, but one operator runs all of them, so they fail together.",
        "action": {"kind": "request-mirror", "label": "Ask a different operator to mirror this repository"},
    },
    {
        "code": "failing_copy",
        "severity": "medium",
        "weight": 10,
        "title": "A copy is failing",
        "detail": "At least one copy was rejected by the integrity gate, blocked for abuse, or failed its latest clone probe.",
        "action": {"kind": "resync-mirror", "label": "Re-sync the failing mirror, then re-check it"},
    },
    {
        "code": "source_offline",
        "severity": "medium",
        "weight": 10,
        "title": "Source is offline",
        "detail": "The working-copy source is not online, so mirrors that fall behind cannot re-sync until it returns.",
        "action": {"kind": "start-source", "label": "Start the source node"},
    },
    {
        "code": "no_direct_clone_url",
        "severity": "medium",
        "weight": 10,
        "title": "No verified direct clone URL",
        "detail": "No verified copy has passed a direct clone probe in the last day, so clones depend on the ForkMesh router.",
        "action": {"kind": "check-direct-url", "label": "Register a direct mirror URL and check it"},
    },
    {
        "code": "partial_ref_coverage",
        "severity": "medium",
        "weight": 10,
        "title": "Branches are not fully replicated",
        "detail": "Fewer verified copies carry every branch than the quorum target requires.",
        "action": {"kind": "resync-mirror", "label": "Re-sync mirrors with every branch"},
    },
    {
        "code": "stale_copy",
        "severity": "low",
        "weight": 5,
        "title": "A copy has stale evidence",
        "detail": "At least one copy is offline, behind, or has no evidence newer than the freshness objective.",
        "action": {"kind": "resync-mirror", "label": "Bring the stale mirror back online"},
    },
    {
        "code": "partial_artifact_coverage",
        "severity": "low",
        "weight": 5,
        "title": "Artifacts are not fully replicated",
        "detail": "Fewer verified copies carry every release artifact than the quorum target requires.",
        "action": {"kind": "resync-mirror", "label": "Re-sync release artifacts to the mirrors"},
    },
    {
        "code": "single_region",
        "severity": "low",
        "weight": 5,
        "title": "Every verified copy is in one region",
        "detail": "All verified copies declared the same region, so a regional outage affects all of them.",
        "action": {"kind": "request-mirror", "label": "Ask a node in another region to mirror this repository"},
    },
    {
        "code": "declared_only_copy",
        "severity": "low",
        "weight": 3,
        "title": "A copy was never observed",
        "detail": "A node published a catalog record for this repository, but nothing has independently observed it serving.",
        "action": {"kind": "check-direct-url", "label": "Check the node's endpoint"},
    },
)

REASONS = (
    "integrity_rejected", "abuse_blocked", "probe_failed", "offline",
    "behind", "evidence_expired", "awaiting_receipt", "never_observed",
    "limited_replica",
)

# A copy whose declared replicaScope is anything but "full" (a filtered,
# metadata-only, or deferred-object replica; see www/docs/offline-sync.md)
# never counts as verified, so it can never make or inflate a quorum.
FULL_REPLICA_SCOPE = "full"

_HEX = frozenset("0123456789abcdef")


def _int_or_none(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value)


def _positive(value):
    value = _int_or_none(value)
    return value if value is not None and value > 0 else None


def _count(value):
    value = _int_or_none(value)
    return value if value is not None and value >= 0 else None


def _digest(value):
    value = str(value or "").strip().lower() if isinstance(value, str) else ""
    return value if len(value) == 64 and set(value) <= _HEX else ""


def _text(value, limit):
    return value.strip()[:limit] if isinstance(value, str) else ""


def _operator_key(row):
    for field in ("ownerUser", "owner", "node"):
        value = _text(row.get(field), 80).lower()
        if value:
            return value
    return ""


def _plural(count, singular, plural):
    return "%d %s" % (count, singular if count == 1 else plural)


def _level(score):
    for minimum, level, label in LEVELS:
        if score >= minimum:
            return level, label
    return LEVELS[-1][1], LEVELS[-1][2]


def _replica_scope(row):
    # Absent means a legacy full mirror; anything unrecognised fails closed.
    value = row.get("replicaScope")
    if value is None:
        return FULL_REPLICA_SCOPE
    return str(value).strip().lower() or "filtered"


def _classify(row, now):
    online = row.get("status") == "online"
    behind = row.get("behind") is True
    integrity = row.get("integrity")
    checked_at = _positive(row.get("checkedAt"))
    probe_at = _positive(row.get("directCheckedAt"))
    last_seen = _positive(row.get("lastSeen"))

    receipt_fresh = bool(
        row.get("endpointFresh") is True
        and row.get("endpointHealthy") is True
        and row.get("endpointIntegrity") == "ok"
        and checked_at is not None
        and 0 <= now - checked_at <= FRESHNESS_OBJECTIVE_MS
    )
    if probe_at is None:
        probe = "none"
    elif now - probe_at > PROBE_FRESH_MS:
        probe = "stale"
    elif row.get("directVerified") is True:
        probe = "pass"
    else:
        probe = "fail"

    observed = [value for value in (checked_at, probe_at, last_seen) if value]
    observed_at = max(observed) if observed else None

    reasons = []
    if integrity == "rejected":
        reasons.append("integrity_rejected")
    if row.get("abuseBlocked") is True:
        reasons.append("abuse_blocked")
    if probe == "fail":
        reasons.append("probe_failed")
    if reasons:
        return "failing", reasons, receipt_fresh, probe, observed_at

    limited = ["limited_replica"] if _replica_scope(row) != FULL_REPLICA_SCOPE else []
    if online and not behind and integrity == "ok" and (
            receipt_fresh or probe == "pass"):
        if limited:
            return "observed", limited, receipt_fresh, probe, observed_at
        return "verified", [], receipt_fresh, probe, observed_at

    if observed_at is None:
        return "declared", ["never_observed"] + limited, receipt_fresh, probe, observed_at

    if not online:
        reasons.append("offline")
    if behind:
        reasons.append("behind")
    if now - observed_at > FRESHNESS_OBJECTIVE_MS:
        reasons.append("evidence_expired")
    if reasons:
        return "stale", reasons + limited, receipt_fresh, probe, observed_at
    return "observed", ["awaiting_receipt"] + limited, receipt_fresh, probe, observed_at


def _coverage(field, verified_rows, source):
    known = [
        value for value in (_count(row.get(field)) for row in verified_rows)
        if value is not None
    ]
    source_value = _count((source or {}).get(field))
    if source_value is not None:
        known.append(source_value)
    reference = max(known) if known else None
    complete = sum(
        1 for row in verified_rows
        if reference is not None and _count(row.get(field)) is not None
        and _count(row.get(field)) >= reference
    )
    unknown = sum(1 for row in verified_rows if _count(row.get(field)) is None)
    gap = bool(
        reference and verified_rows
        and complete < min(QUORUM_TARGET, len(verified_rows))
    )
    return {"reference": reference, "complete": complete, "unknown": unknown}, gap


def evaluate_mirrors_payload(payload):
    """Return the availability proof and risk report for a mirrors payload."""
    payload = payload if isinstance(payload, dict) else {}
    now = _int_or_none(payload.get("generatedAt")) or 0
    rows = [
        row for row in payload.get("mirrors") or []
        if isinstance(row, dict)
    ] if isinstance(payload.get("mirrors"), list) else []
    source = payload.get("sourceOfTruth")
    source = source if isinstance(source, dict) else None

    operator_index = {}
    copies = []
    classified = []
    for row in rows:
        copy_class, reasons, receipt_fresh, probe, observed_at = _classify(row, now)
        operator = _operator_key(row)
        if operator not in operator_index:
            operator_index[operator] = len(operator_index) + 1
        region = _text(row.get("region"), 40).lower() or None
        classified.append((row, copy_class, operator, region, probe))
        copies.append({
            "node": _text(row.get("node"), 80),
            "class": copy_class,
            "operator": operator_index[operator],
            "region": region,
            "online": row.get("status") == "online",
            "observedAt": observed_at,
            "evidenceAgeMs": max(0, now - observed_at) if observed_at else None,
            "receiptFresh": receipt_fresh,
            "probe": probe,
            "cloneAvailable": row.get("cloneAvailable") is True,
            "branchCount": _count(row.get("branchCount")),
            "artifactCount": _count(row.get("artifactCount")),
            "replicaScope": _replica_scope(row),
            "reasons": reasons,
        })

    counts = {name: 0 for name in COPY_CLASSES}
    for _, copy_class, _, _, _ in classified:
        counts[copy_class] += 1
    verified = [item for item in classified if item[1] == "verified"]
    verified_rows = [item[0] for item in verified]

    # Quorum counts distinct operators, never nodes, so one operator running
    # several machines cannot manufacture it. When clone probes disagree about
    # the refs digest only the best-supported digest counts.
    digests = {}
    for row, _, operator, _, probe in verified:
        digest = _digest(row.get("directRefsSha256")) if probe == "pass" else ""
        if digest:
            digests.setdefault(digest, set()).add(operator)
    disagreement = len(digests) > 1
    if disagreement:
        leading = sorted(digests, key=lambda d: (-len(digests[d]), d))[0]
        counting = [
            item for item in verified
            if item[4] == "pass" and _digest(item[0].get("directRefsSha256")) == leading
        ]
    else:
        counting = verified
    quorum_operators = {item[2] for item in counting}

    source_operator = _operator_key(source) if source else ""
    independent = {op for op in quorum_operators if op != source_operator}
    verified_regions = {item[3] for item in verified if item[3]}
    undeclared_regions = sum(1 for item in verified if not item[3])

    if source is not None and source.get("status") in ("online", "offline"):
        source_online = source.get("status") == "online"
    else:
        local_rows = [
            row for row in rows
            if str(row.get("source") or "") == "local-node"
        ]
        source_online = (
            any(row.get("status") == "online" for row in local_rows)
            if local_rows else None
        )

    probe_times = [
        value for value in (_positive(row.get("directCheckedAt")) for row in rows)
        if value
    ]
    probes = {
        "probed": len(probe_times),
        "passing": sum(1 for item in classified if item[4] == "pass"),
        "failing": sum(1 for item in classified if item[4] == "fail"),
        "stale": sum(1 for item in classified if item[4] == "stale"),
        "lastProbeAt": max(probe_times) if probe_times else None,
    }
    transports = {
        "router": sum(1 for row in verified_rows if row.get("cloneAvailable") is True),
        "directHttps": sum(1 for item in verified if item[4] == "pass"),
        "smartHttp": sum(
            1 for row in verified_rows
            if isinstance(row.get("operations"), list)
            and "git-upload-pack" in row.get("operations")
        ),
    }
    branches, branch_gap = _coverage("branchCount", verified_rows, source)
    artifacts, artifact_gap = _coverage("artifactCount", verified_rows, source)

    quorum_met = len(quorum_operators) >= QUORUM_TARGET
    triggered = {
        "no_verified_copy": not verified,
        "quorum_not_met": not quorum_met,
        "state_disagreement": disagreement,
        "single_operator": len(verified) >= 2 and len({i[2] for i in verified}) == 1,
        "failing_copy": counts["failing"] > 0,
        "source_offline": source_online is False,
        "no_direct_clone_url": bool(verified) and transports["directHttps"] == 0,
        "partial_ref_coverage": branch_gap,
        "stale_copy": counts["stale"] > 0,
        "partial_artifact_coverage": artifact_gap,
        "single_region": (
            len(verified) >= 2 and undeclared_regions == 0
            and len(verified_regions) == 1
        ),
        "declared_only_copy": counts["declared"] > 0,
    }
    risks = []
    for risk in RISKS:
        if not triggered[risk["code"]]:
            continue
        risks.append({
            "code": risk["code"],
            "severity": risk["severity"],
            "weight": risk["weight"],
            "title": risk["title"],
            "detail": risk["detail"],
            "action": {
                "kind": risk["action"]["kind"],
                "label": risk["action"]["label"],
                "href": DOCS_URL + "#" + risk["code"],
            },
        })
    risks.sort(key=lambda risk: -risk["weight"])
    score = min(100, sum(risk["weight"] for risk in risks))
    level, level_label = _level(score)

    if quorum_met:
        proof = "quorum"
    elif verified:
        proof = "partial"
    else:
        proof = "none"

    parts = [
        PROOF_HEADLINES[proof] + ".",
        "%d of %s verified by %s (target %d)." % (
            len(verified),
            _plural(len(rows), "copy", "copies"),
            _plural(len(quorum_operators), "independent operator",
                    "independent operators"),
            QUORUM_TARGET,
        ),
        "Risk %d of 100 (%s)." % (score, level_label.lower()),
    ]
    if risks:
        parts.append("Top risk: %s." % risks[0]["title"])

    return {
        "schema": SCHEMA,
        "generatedAt": now,
        "freshnessObjectiveMs": FRESHNESS_OBJECTIVE_MS,
        "proof": proof,
        "label": PROOF_LABELS[proof],
        "score": score,
        "level": level,
        "levelLabel": level_label,
        "summary": " ".join(parts),
        "quorum": {
            "met": quorum_met,
            "target": QUORUM_TARGET,
            "operators": len(quorum_operators),
            "copies": len(counting),
        },
        "counts": dict(
            total=len(rows),
            active=sum(1 for row in rows if row.get("status") == "online"),
            **counts
        ),
        "diversity": {
            "operators": len({item[2] for item in verified}),
            "independentOperators": len(independent),
            "regions": len(verified_regions),
            "regionsUndeclared": undeclared_regions,
        },
        "sourceOnline": source_online,
        "probes": probes,
        "transports": transports,
        "coverage": {"branches": branches, "artifacts": artifacts},
        "risks": risks,
        "copies": copies,
    }
