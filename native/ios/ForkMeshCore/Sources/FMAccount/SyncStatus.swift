// Kotlin counterpart: api/SyncStatus.kt.

import FMBase
import Foundation

/// Human-readable synchronization status (forkmesh-sync-status-v1).
///
/// An independent implementation of `conformance/sync-status/v1/model.json`; the tests pin it to
/// the shared fixture the Worker, Go mirror node, desktop and Android implementations also pass.
/// The output is built only from counts, timestamps and closed vocabularies, so node names, peer
/// addresses and raw error text in the evidence never reach the summary.
public enum SyncStatus {
    public static let schema = "forkmesh-sync-status-v1"
    static let staleAfterMs: Int64 = 60 * 60 * 1000
    static let syncDeadlineMs: Int64 = 15 * 60 * 1000

    static let labels: [String: String] = [
        "queued": "Queued",
        "syncing": "Syncing",
        "verified": "Verified",
        "stale": "Stale",
        "degraded": "Degraded",
        "conflicted": "Conflicted",
        "paused": "Paused",
        "external-only": "External only",
        "failed": "Failed",
    ]

    static let severity: [String: String] = [
        "queued": "info",
        "syncing": "info",
        "verified": "ok",
        "stale": "warning",
        "degraded": "warning",
        "conflicted": "error",
        "paused": "info",
        "external-only": "info",
        "failed": "error",
    ]

    static let headlines: [String: String] = [
        "queued": "Waiting to synchronize",
        "syncing": "Synchronizing now",
        "verified": "Every copy is verified and current",
        "stale": "The visible state may be out of date",
        "degraded": "Verified, but some copies are not current",
        "conflicted": "Copies disagree about the latest state",
        "paused": "Synchronization is paused",
        "external-only": "Only stored on the external provider so far",
        "failed": "Synchronization failed",
    ]

    static let reportedErrors: Set<String> = [
        "rejected", "signature_invalid", "unauthorized", "provider_unavailable", "network", "quota_exceeded",
    ]

    static let actions: [String: String] = [
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
    ]

    /// `_intOrNull`: an int, or a finite double truncated; anything else is nil.
    static func intOrNull(_ value: JSONValue?) -> Int64? {
        switch value {
        case .int(let v)?: return v
        case .double(let d)? where d.isFinite: return Int64(exactly: d.rounded(.towardZero))
        default: return nil
        }
    }

    static func plural(_ count: Int, _ singular: String, _ plural: String) -> String {
        "\(count) \(count == 1 ? singular : plural)"
    }

    static func age(_ ms: Int64) -> String {
        let minutes = ms / 60_000
        if minutes < 60 { return "\(minutes) min" }
        let hours = ms / 3_600_000
        if hours < 48 { return "\(hours) h" }
        return "\(ms / 86_400_000) d"
    }

    static func optionalInt(_ value: Int64?) -> JSONValue { value.map(JSONValue.int) ?? .null }
    static func optionalString(_ value: String?) -> JSONValue { value.map(JSONValue.string) ?? .null }

    /// The machine-readable and plain-language status for `evidence` (`evaluateSyncStatus`).
    public static func evaluate(_ evidence: JSONObject) -> JSONObject {
        let now = intOrNull(evidence["now"]) ?? 0
        let copies = evidence["copies"]?.arrayValue?.compactMap(\.objectValue) ?? []
        let requiredValue = intOrNull(evidence["requiredCopies"]) ?? 1
        let required = requiredValue < 1 ? 1 : requiredValue
        let conflicts = intOrNull(evidence["conflicts"]) ?? 0
        let queuedAt = intOrNull(evidence["queuedAt"])
        let startedAt = intOrNull(evidence["syncStartedAt"])
        let sourceUpdatedAt = intOrNull(evidence["sourceUpdatedAt"])
        var lastError: String?
        if let raw = evidence["lastError"]?.stringValue, !raw.isEmpty {
            lastError = reportedErrors.contains(raw) ? raw : "unknown"
        }

        let total = copies.count
        var reached = 0, verifiedCurrent = 0, trustedCurrent = 0
        var rejected = 0, unreachable = 0, unverified = 0
        var lastConfirmed: Int64?
        var currentConfirmed: Int64?
        for copy in copies {
            var status = copy["status"]?.stringValue ?? ""
            if status != "current" && status != "behind" && status != "rejected" { status = "unreachable" }
            let verified = copy["verified"] == .bool(true)
            let confirmedAt = intOrNull(copy["confirmedAt"])
            if status != "unreachable" { reached += 1 }
            if status == "rejected" {
                rejected += 1
            } else if status == "unreachable" {
                unreachable += 1
            }
            if status == "current" && verified {
                verifiedCurrent += 1
                if copy["trusted"] == .bool(true) { trustedCurrent += 1 }
                if let confirmedAt, currentConfirmed == nil || confirmedAt > currentConfirmed! {
                    currentConfirmed = confirmedAt
                }
            } else if status == "current" {
                unverified += 1
            }
            if verified, status != "rejected", let confirmedAt, lastConfirmed == nil || confirmedAt > lastConfirmed! {
                lastConfirmed = confirmedAt
            }
        }

        let inFlight = startedAt != nil
        let delayed = inFlight && now - (startedAt ?? 0) > syncDeadlineMs

        var state: String
        var error: String?
        if evidence["paused"] == .bool(true) {
            state = "paused"
        } else if conflicts > 0 {
            state = "conflicted"
            error = "conflict"
        } else if evidence["externalOnly"] == .bool(true) && total == 0 && lastError == nil {
            state = "external-only"
        } else if verifiedCurrent >= required {
            if currentConfirmed == nil || now - currentConfirmed! > staleAfterMs {
                state = "stale"
                error = "confirmation_expired"
            } else if verifiedCurrent < total {
                state = "degraded"
                error = rejected > 0 ? "rejected" : unreachable > 0 ? "unreachable" : unverified > 0 ? "unverified" : "copies_behind"
            } else {
                state = "verified"
            }
        } else if inFlight && !delayed {
            state = "syncing"
        } else if queuedAt != nil && !inFlight {
            state = "queued"
        } else if rejected > 0 {
            state = "failed"
            error = lastError ?? "rejected"
        } else if let lastError {
            state = "failed"
            error = lastError
        } else if delayed {
            state = "stale"
            error = "sync_delayed"
        } else if total > 0 && unreachable == total {
            state = "stale"
            error = "unreachable"
        } else {
            state = "stale"
            error = "unconfirmed"
        }

        let lag: Int64?
        if state == "verified" {
            lag = 0
        } else if let sourceUpdatedAt {
            lag = max(0, now - sourceUpdatedAt)
        } else {
            lag = nil
        }

        var parts = [
            "\(headlines[state] ?? "").",
            "\(verifiedCurrent) of \(plural(total, "copy", "copies")) verified, "
                + "\(plural(trustedCurrent, "trusted mirror", "trusted mirrors")), "
                + "\(plural(reached, "node", "nodes")) reached.",
        ]
        if let lastConfirmed {
            parts.append(now - lastConfirmed < 60_000 ? "Last confirmed just now." : "Last confirmed \(age(now - lastConfirmed)) ago.")
        } else {
            parts.append("Never confirmed.")
        }
        if let lag, lag >= 60_000 { parts.append("\(age(lag)) behind the source.") }
        let action = error.flatMap { actions[$0] }
        if let action { parts.append(action) }

        return [
            "schema": .string(schema),
            "state": .string(state),
            "label": optionalString(labels[state]),
            "severity": optionalString(severity[state]),
            "current": .bool(state == "verified" || state == "degraded"),
            "totalCopies": .int(Int64(total)),
            "nodesReached": .int(Int64(reached)),
            "verifiedCopies": .int(Int64(verifiedCurrent)),
            "trustedMirrors": .int(Int64(trustedCurrent)),
            "lastConfirmedAt": optionalInt(lastConfirmed),
            "lagMs": optionalInt(lag),
            "error": optionalString(error),
            "action": optionalString(action),
            "summary": .string(parts.joined(separator: " ")),
        ]
    }

    /// Evidence for a `GET /api/repo/<owner>/<repo>/mirrors` payload (`mirrorsPayloadEvidence`).
    public static func mirrorsPayloadEvidence(_ payload: JSONObject) -> JSONObject {
        let rows = payload["mirrors"]?.arrayValue?.compactMap(\.objectValue) ?? []
        var copies: [JSONValue] = []
        for row in rows {
            let integrity = row["integrity"]
            let status: String
            if row["status"] != .string("online") {
                status = "unreachable"
            } else if integrity == .string("rejected") {
                status = "rejected"
            } else if row["behind"] == .bool(true) || integrity == .string("healing") {
                status = "behind"
            } else {
                status = "current"
            }
            copies.append(.object([
                "status": .string(status),
                "verified": .bool(integrity == .string("ok")),
                "trusted": .bool(row["cloneAvailable"] == .bool(true) || row["endpointHealthy"] == .bool(true)),
                "confirmedAt": optionalInt(intOrNull(row["lastSync"])),
            ]))
        }
        let freshest = payload["summary"]?.objectValue.flatMap { intOrNull($0["freshestSync"]) }
        return [
            "now": .int(intOrNull(payload["generatedAt"]) ?? 0),
            "sourceUpdatedAt": freshest == nil || freshest == 0 ? .null : .int(freshest!),
            "copies": .array(copies),
        ]
    }

    /// `mirrorsPayloadSyncStatus`.
    public static func mirrorsPayloadStatus(_ payload: JSONObject) -> JSONObject {
        evaluate(mirrorsPayloadEvidence(payload))
    }
}
