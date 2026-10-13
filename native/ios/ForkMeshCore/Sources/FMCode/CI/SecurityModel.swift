import FMApi
import FMBase
import Foundation
import Observation

public enum SecuritySeverity: String, CaseIterable, Sendable, Comparable {
    case critical, high, medium, low, info

    public var label: String { self == .info ? "Info" : rawValue.capitalized }

    var rank: Int { SecuritySeverity.allCases.firstIndex(of: self) ?? 0 }

    public static func < (lhs: SecuritySeverity, rhs: SecuritySeverity) -> Bool { lhs.rank < rhs.rank }

    public init(raw: String) {
        switch raw.lowercased() {
        case "critical": self = .critical
        case "high": self = .high
        case "medium", "moderate": self = .medium
        case "low": self = .low
        default: self = .info
        }
    }
}

public struct SecurityFinding: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var category: String
    public var severity: SecuritySeverity
    public var ruleId: String
    public var path: String
    public var line: Int
    public var summary: String
    public var recommendation: String
    public var reviewStatus: String
    public var advisoryIds: [String]

    public init?(json: JSONObject) {
        let id = json.str("id").lowercased()
        guard !id.isEmpty else { return nil }
        self.id = id
        category = json.str("category")
        severity = SecuritySeverity(raw: json.str("severity"))
        ruleId = json.str("ruleId")
        path = json.str("path")
        line = ModelJSON.int(json["line"])
        summary = json.str("summary")
        recommendation = json.str("recommendation")
        let status = json.str("falsePositiveStatus")
        reviewStatus = status.isEmpty ? "unreviewed" : status
        advisoryIds = json.strings("advisoryIds")
    }

    public var location: String { line > 0 ? "\(path):\(line)" : path }
}

public struct SecurityScanSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var receivedAt: String
    public var status: String
    public var scannedAt: String
    public var scanner: String
    public var commitHash: String
    public var counts: [SecuritySeverity: Int]
    public var reviewStatus: String
    public var findings: [SecurityFinding]
    public var hasDetail: Bool

    public init?(json: JSONObject) {
        let id = json.str("scanId")
        let clipboard = json.obj("clipboard")
        guard !id.isEmpty || !clipboard.isEmpty else { return nil }
        self.id = id
        receivedAt = json.str("receivedAt")
        status = clipboard.str("status")
        scannedAt = clipboard.str("scannedAt")
        scanner = clipboard.str("scanner")
        commitHash = clipboard.str("commitHash")
        let raw = clipboard.obj("findings")
        counts = [
            .critical: ModelJSON.int(raw["critical"]), .high: ModelJSON.int(raw["high"]),
            .medium: ModelJSON.int(raw["medium"]), .low: ModelJSON.int(raw["low"]),
            .info: ModelJSON.int(ModelJSON.coalesce(raw["informational"], raw["info"])),
        ]
        reviewStatus = clipboard.str("reviewStatus")
        let rich = json["rich"]?.objectValue
        hasDetail = rich != nil
        findings = (rich?.maps("findings") ?? []).prefix(500).compactMap(SecurityFinding.init(json:)).sorted {
            $0.severity != $1.severity ? $0.severity < $1.severity : $0.location < $1.location
        }
    }

    public var total: Int { counts.values.reduce(0, +) }
    public var shortCommit: String { String(commitHash.prefix(8)) }

    public var headline: String {
        if total == 0 { return "No findings" }
        let parts = SecuritySeverity.allCases.compactMap { severity -> String? in
            let count = counts[severity] ?? 0
            return count > 0 ? "\(count) \(severity.label.lowercased())" : nil
        }
        return parts.joined(separator: " · ")
    }

    public var worst: SecuritySeverity? { SecuritySeverity.allCases.first { (counts[$0] ?? 0) > 0 } }
}

public struct SecurityScanStatus: Equatable, Sendable {
    public var due: Bool
    public var status: String
    public var commit: String
    public var nextAttemptAt: Int

    public init(json: JSONObject) {
        due = json.bool("due")
        status = json.str("status")
        commit = json.str("commit")
        nextAttemptAt = ModelJSON.int(json["nextAttemptAt"])
    }

    public var label: String {
        if status == "leased" || status == "running" { return "A scan is running on a node." }
        if due { return "A new scan is due; the next node that picks it up will run it." }
        return "Up to date for the current commit."
    }
}

@MainActor
@Observable
public final class RepoSecurityModel {
    public static let triageStatuses = ["unreviewed", "confirmed", "dismissed"]

    public let repo: RepoRef
    public private(set) var latest: SecurityScanSummary?
    public private(set) var history: [SecurityScanSummary] = []
    public private(set) var scanStatus: SecurityScanStatus?
    public private(set) var loading = false
    public private(set) var error: String?
    public private(set) var busy = false
    public var severityFilter: SecuritySeverity?

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public var findings: [SecurityFinding] {
        let all = latest?.findings ?? []
        guard let filter = severityFilter else { return all }
        return all.filter { $0.severity == filter }
    }

    public func load() async {
        loading = true
        do {
            let rich = try await client.securityScans(repo, operation: "history", query: [("limit", "10"), ("detail", "rich")])
            history = rich.maps("history").compactMap(SecurityScanSummary.init(json:))
            latest = history.first
            error = nil
        } catch {
            do {
                let data = try await client.securityScans(repo, operation: "latest", query: [])
                latest = data["latest"]?.objectValue.flatMap(SecurityScanSummary.init(json:))
                history = latest.map { [$0] } ?? []
                self.error = nil
            } catch {
                self.error = "\(error)"
            }
        }
        if let status = try? await client.securityScans(repo, operation: "lease", query: []) {
            scanStatus = SecurityScanStatus(json: status)
        }
        loading = false
    }

    @discardableResult
    public func triage(_ finding: SecurityFinding, status: String, note: String = "") async -> Bool {
        guard RepoSecurityModel.triageStatuses.contains(status), let scan = latest, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            _ = try await client.securityTriage(
                repo, scanId: scan.id, findingId: finding.id, status: status, note: String(note.prefix(500)))
            if let index = latest?.findings.firstIndex(where: { $0.id == finding.id }) {
                latest?.findings[index].reviewStatus = status
            }
            error = nil
            return true
        } catch {
            self.error = "\(error)"
            return false
        }
    }
}
