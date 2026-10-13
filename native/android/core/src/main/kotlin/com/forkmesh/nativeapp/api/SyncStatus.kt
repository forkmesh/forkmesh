package com.forkmesh.nativeapp.api

const val SYNC_STATUS_SCHEMA = "forkmesh-sync-status-v1"

private const val STALE_AFTER_MS = 60L * 60 * 1000
private const val SYNC_DEADLINE_MS = 15L * 60 * 1000

private val LABELS = mapOf(
    "queued" to "Queued",
    "syncing" to "Syncing",
    "verified" to "Verified",
    "stale" to "Stale",
    "degraded" to "Degraded",
    "conflicted" to "Conflicted",
    "paused" to "Paused",
    "external-only" to "External only",
    "failed" to "Failed",
)

private val SEVERITY = mapOf(
    "queued" to "info",
    "syncing" to "info",
    "verified" to "ok",
    "stale" to "warning",
    "degraded" to "warning",
    "conflicted" to "error",
    "paused" to "info",
    "external-only" to "info",
    "failed" to "error",
)

private val HEADLINES = mapOf(
    "queued" to "Waiting to synchronize",
    "syncing" to "Synchronizing now",
    "verified" to "Every copy is verified and current",
    "stale" to "The visible state may be out of date",
    "degraded" to "Verified, but some copies are not current",
    "conflicted" to "Copies disagree about the latest state",
    "paused" to "Synchronization is paused",
    "external-only" to "Only stored on the external provider so far",
    "failed" to "Synchronization failed",
)

private val REPORTED_ERRORS = setOf(
    "rejected",
    "signature_invalid",
    "unauthorized",
    "provider_unavailable",
    "network",
    "quota_exceeded",
)

private val ACTIONS = mapOf(
    "conflict" to "Review the conflicting changes and choose which state to keep.",
    "rejected" to "A copy rejected the update; check the node's signing key and push again.",
    "signature_invalid" to "The update's signature did not verify; re-sign it from a trusted device.",
    "unauthorized" to "This node is not authorized to publish; reconnect the account and retry.",
    "provider_unavailable" to "The external provider did not respond; ForkMesh retries automatically.",
    "network" to "A network error interrupted synchronization; it retries when the connection returns.",
    "quota_exceeded" to "A service quota was reached; synchronization resumes when the quota resets.",
    "sync_delayed" to "Synchronization is taking longer than expected; check that the source node is online.",
    "unreachable" to "One or more copies could not be reached; check that the mirrors are online.",
    "unverified" to "A copy reports the latest state without a verified signature; wait for it to re-attest.",
    "copies_behind" to "Some copies have not received the latest state yet; they normally catch up within minutes.",
    "confirmation_expired" to "No copy has confirmed the state recently; check that the mirrors are online.",
    "unconfirmed" to "No copy has confirmed the latest state yet.",
    "unknown" to "An unexpected error stopped synchronization; see the node's log for details.",
)

private fun intOrNull(value: Any?): Long? = when (value) {
    is Long -> value
    is Int -> value.toLong()
    is Short -> value.toLong()
    is Byte -> value.toLong()
    is Double -> if (value.isFinite()) value.toLong() else null
    is Float -> if (value.isFinite()) value.toLong() else null
    else -> null
}

private fun plural(count: Long, singular: String, plural: String): String =
    "$count ${if (count == 1L) singular else plural}"

private fun age(ms: Long): String {
    val minutes = ms / 60_000
    if (minutes < 60) return "$minutes min"
    val hours = ms / 3_600_000
    if (hours < 48) return "$hours h"
    return "${ms / 86_400_000} d"
}

fun evaluateSyncStatus(evidence: Map<String, Any?>): Map<String, Any?> {
    val now = intOrNull(evidence["now"]) ?: 0L
    val rawCopies = evidence["copies"]
    val copies: List<Map<*, *>> = if (rawCopies is List<*>) rawCopies.filterIsInstance<Map<*, *>>() else emptyList()
    val requiredValue = intOrNull(evidence["requiredCopies"]) ?: 1L
    val required = if (requiredValue < 1) 1L else requiredValue
    val conflicts = intOrNull(evidence["conflicts"]) ?: 0L
    val queuedAt = intOrNull(evidence["queuedAt"])
    val startedAt = intOrNull(evidence["syncStartedAt"])
    val sourceUpdatedAt = intOrNull(evidence["sourceUpdatedAt"])
    val rawError = evidence["lastError"]
    var lastError: String? = if (rawError is String && rawError.isNotEmpty()) rawError else null
    if (lastError != null && lastError !in REPORTED_ERRORS) lastError = "unknown"

    val total = copies.size.toLong()
    var reached = 0L
    var verifiedCurrent = 0L
    var trustedCurrent = 0L
    var rejected = 0L
    var unreachable = 0L
    var unverified = 0L
    var lastConfirmed: Long? = null
    var currentConfirmed: Long? = null
    for (copy in copies) {
        var status = copy["status"]
        if (status != "current" && status != "behind" && status != "rejected") status = "unreachable"
        val verified = copy["verified"] == true
        val confirmedAt = intOrNull(copy["confirmedAt"])
        if (status != "unreachable") reached++
        if (status == "rejected") {
            rejected++
        } else if (status == "unreachable") {
            unreachable++
        }
        if (status == "current" && verified) {
            verifiedCurrent++
            if (copy["trusted"] == true) trustedCurrent++
            if (confirmedAt != null && (currentConfirmed == null || confirmedAt > currentConfirmed)) {
                currentConfirmed = confirmedAt
            }
        } else if (status == "current") {
            unverified++
        }
        if (verified && status != "rejected" && confirmedAt != null &&
            (lastConfirmed == null || confirmedAt > lastConfirmed)
        ) {
            lastConfirmed = confirmedAt
        }
    }

    val inFlight = startedAt != null
    val delayed = startedAt != null && now - startedAt > SYNC_DEADLINE_MS

    val state: String
    var error: String? = null
    if (evidence["paused"] == true) {
        state = "paused"
    } else if (conflicts > 0) {
        state = "conflicted"
        error = "conflict"
    } else if (evidence["externalOnly"] == true && total == 0L && lastError == null) {
        state = "external-only"
    } else if (verifiedCurrent >= required) {
        if (currentConfirmed == null || now - currentConfirmed > STALE_AFTER_MS) {
            state = "stale"
            error = "confirmation_expired"
        } else if (verifiedCurrent < total) {
            state = "degraded"
            error = when {
                rejected > 0 -> "rejected"
                unreachable > 0 -> "unreachable"
                unverified > 0 -> "unverified"
                else -> "copies_behind"
            }
        } else {
            state = "verified"
        }
    } else if (inFlight && !delayed) {
        state = "syncing"
    } else if (queuedAt != null && !inFlight) {
        state = "queued"
    } else if (rejected > 0) {
        state = "failed"
        error = lastError ?: "rejected"
    } else if (lastError != null) {
        state = "failed"
        error = lastError
    } else if (delayed) {
        state = "stale"
        error = "sync_delayed"
    } else if (total > 0 && unreachable == total) {
        state = "stale"
        error = "unreachable"
    } else {
        state = "stale"
        error = "unconfirmed"
    }

    val lag: Long? = when {
        state == "verified" -> 0L
        sourceUpdatedAt == null -> null
        else -> if (now - sourceUpdatedAt < 0) 0L else now - sourceUpdatedAt
    }

    val parts = ArrayList<String>()
    parts.add("${HEADLINES[state]}.")
    parts.add(
        "$verifiedCurrent of ${plural(total, "copy", "copies")} verified, " +
            "${plural(trustedCurrent, "trusted mirror", "trusted mirrors")}, " +
            "${plural(reached, "node", "nodes")} reached.",
    )
    val confirmed = lastConfirmed
    when {
        confirmed == null -> parts.add("Never confirmed.")
        now - confirmed < 60_000 -> parts.add("Last confirmed just now.")
        else -> parts.add("Last confirmed ${age(now - confirmed)} ago.")
    }
    if (lag != null && lag >= 60_000) parts.add("${age(lag)} behind the source.")
    val action = if (error == null) null else ACTIONS[error]
    if (action != null) parts.add(action)

    val result = LinkedHashMap<String, Any?>()
    result["schema"] = SYNC_STATUS_SCHEMA
    result["state"] = state
    result["label"] = LABELS[state]
    result["severity"] = SEVERITY[state]
    result["current"] = state == "verified" || state == "degraded"
    result["totalCopies"] = total
    result["nodesReached"] = reached
    result["verifiedCopies"] = verifiedCurrent
    result["trustedMirrors"] = trustedCurrent
    result["lastConfirmedAt"] = lastConfirmed
    result["lagMs"] = lag
    result["error"] = error
    result["action"] = action
    result["summary"] = parts.joinToString(" ")
    return result
}

fun mirrorsPayloadEvidence(payload: Map<String, Any?>): Map<String, Any?> {
    val summary = payload["summary"]
    val rows = payload["mirrors"]
    val copies = ArrayList<Map<String, Any?>>()
    val rowMaps: List<Map<*, *>> = if (rows is List<*>) rows.filterIsInstance<Map<*, *>>() else emptyList()
    for (row in rowMaps) {
        val integrity = row["integrity"]
        val status = when {
            row["status"] != "online" -> "unreachable"
            integrity == "rejected" -> "rejected"
            row["behind"] == true || integrity == "healing" -> "behind"
            else -> "current"
        }
        val copy = LinkedHashMap<String, Any?>()
        copy["status"] = status
        copy["verified"] = integrity == "ok"
        copy["trusted"] = row["cloneAvailable"] == true || row["endpointHealthy"] == true
        copy["confirmedAt"] = intOrNull(row["lastSync"])
        copies.add(copy)
    }
    val freshest = if (summary is Map<*, *>) intOrNull(summary["freshestSync"]) else null
    val evidence = LinkedHashMap<String, Any?>()
    evidence["now"] = intOrNull(payload["generatedAt"]) ?: 0L
    evidence["sourceUpdatedAt"] = if (freshest == null || freshest == 0L) null else freshest
    evidence["copies"] = copies
    return evidence
}

fun mirrorsPayloadSyncStatus(payload: Map<String, Any?>): Map<String, Any?> =
    evaluateSyncStatus(mirrorsPayloadEvidence(payload))
