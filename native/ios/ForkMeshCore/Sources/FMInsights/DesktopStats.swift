import FMApi
import FMBase
import FMControl
import Foundation
import Observation

public enum DesktopStatsMethod {
    public static let prefix = "stats."
    public static let timeline = "stats.timeline"
    public static let quality = "stats.quality"
    public static let repo = "stats.repo"
    public static let ratchetPrepare = "stats.ratchet.prepare"
    public static let ratchetRun = "stats.ratchet.run"
    public static let sentry = "stats.sentry"
    public static let codemagic = "stats.codemagic"
    public static let health = "diag.health"
}

public struct DesktopHealth: Equatable, Sendable {
    public var level: String
    public var cpuPercent: Double
    public var memoryTotal: Int64
    public var memoryAvailable: Int64
    public var diskTotal: Int64
    public var diskFree: Int64
    public var agentsRunning: Int
    public var agentsQueued: Int
    public var stalls: Int

    public init(json: JSONObject) {
        level = json.str("level")
        cpuPercent = json.double("cpuPercent", -1)
        let memory = json.obj("memory")
        memoryTotal = memory.int64("total")
        memoryAvailable = memory.int64("available")
        let disk = json.obj("disk")
        diskTotal = disk.int64("total")
        diskFree = disk.int64("free")
        let agents = json.obj("agents")
        agentsRunning = agents.int("running")
        agentsQueued = agents.int("queued")
        stalls = json["stalls"]?.intValue ?? json.list("stalls").count
    }

    public var memoryUsedShare: Double? {
        guard memoryTotal > 0 else { return nil }
        return Double(max(0, memoryTotal - memoryAvailable)) / Double(memoryTotal)
    }

    public var tone: StatsTileStatus {
        switch level {
        case "strained": return .critical
        case "busy": return .warning
        case "ok": return .good
        default: return .none
        }
    }
}

public struct CodemagicUsage: Equatable, Sendable {
    public var limitSeconds: Double
    public var usedSeconds: Double
    public var previousSeconds: Double
    public var paidSeconds: Double
    public var resetsAt: Int64

    public init?(json: JSONObject) {
        limitSeconds = json.double("limitSeconds")
        usedSeconds = max(0, json.double("usedSeconds"))
        previousSeconds = max(0, json.double("previousSeconds"))
        paidSeconds = max(0, json.double("paidSeconds"))
        resetsAt = json.int64("resetsAt")
        if limitSeconds <= 0 { return nil }
    }

    public var remainingSeconds: Double { max(0, limitSeconds - usedSeconds) }
    public var remainingMinutes: Int { Int((remainingSeconds / 60).rounded(.down)) }
    public var limitMinutes: Int { Int((limitSeconds / 60).rounded()) }
    public var usedMinutes: Int { Int((usedSeconds / 60).rounded(.up)) }
    public var remainingShare: Double { remainingSeconds / limitSeconds }

    public var tone: StatsTileStatus {
        remainingShare > 0.3 ? .good : remainingShare > 0.1 ? .warning : .critical
    }
}

public struct TimelineDay: Equatable, Sendable, Identifiable {
    public var dayTs: Int64
    public var errors: Int
    public var warnings: Int
    public var info: Int
    public var id: Int64 { dayTs }
    public var total: Int { errors + warnings + info }
}

public struct LogTimeline: Equatable, Sendable {
    public static let maxDays = 30

    public var days: [TimelineDay]

    public init(json: JSONObject) {
        days = json.maps("days").suffix(LogTimeline.maxDays).map {
            TimelineDay(dayTs: $0.int64("dayTs"), errors: $0.int("errors"), warnings: $0.int("warnings"), info: $0.int("info"))
        }
    }

    public var errorSeries: StatsTileSeries {
        StatsTileSeries(labels: days.map { String($0.dayTs) }, values: days.map { Double($0.errors) })
    }

    public var busiest: TimelineDay? { days.max { $0.total < $1.total } }
}

public struct QualityFile: Equatable, Sendable, Identifiable {
    public var path: String
    public var score: Int
    public var findings: [String]
    public var id: String { path }
}

public struct RepoQuality: Equatable, Sendable {
    public static let maxFiles = 200

    public var score: Int
    public var grade: String
    public var computedAt: Int64
    public var files: [QualityFile]

    public init(json: JSONObject) {
        score = json.int("score")
        grade = json.str("grade")
        computedAt = json.int64("computedAt")
        files = json.maps("files").prefix(RepoQuality.maxFiles).map {
            QualityFile(path: $0.str("path"), score: $0.int("score"), findings: Array($0.strings("findings").prefix(10)))
        }.sorted { $0.score != $1.score ? $0.score < $1.score : $0.path < $1.path }
    }

    public var tone: StatsTileStatus {
        score >= 80 ? .good : score >= 60 ? .warning : .critical
    }
}

public struct RepoStatsSnapshot: Equatable, Sendable {
    public var commits: Int
    public var files: Int
    public var lines: Int
    public var ratchetEnabled: Bool
    public var ratchetNote: String

    public init(json: JSONObject) {
        commits = json.int("commits")
        files = json.int("files")
        lines = json.int("lines")
        let ratchet = json.obj("ratchet")
        ratchetEnabled = ratchet.bool("enabled")
        ratchetNote = ratchet.str("note")
    }
}

public struct SentryIssue: Equatable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var project: String
    public var level: String
    public var count: Int
    public var lastSeen: Int64

    public var tone: StatsTileStatus {
        switch level {
        case "fatal", "error": return .critical
        case "warning": return .warning
        default: return .none
        }
    }
}

public struct DesktopConfirmation: Equatable, Sendable {
    public var token: String
    public var summary: String
    public var expiresAt: Int64

    public init?(json: JSONObject) {
        token = json.str("token")
        summary = json.str("summary")
        expiresAt = json.int64("expiresAt")
        if token.isEmpty { return nil }
    }
}

@MainActor
public final class DesktopStatsClient {
    private let channel: DesktopControlCalling

    public init(channel: DesktopControlCalling) {
        self.channel = channel
    }

    public func health() async throws -> DesktopHealth {
        DesktopHealth(json: try await channel.request(DesktopStatsMethod.health, [:], timeout: 15))
    }

    public func timeline(days: Int = 30) async throws -> LogTimeline {
        LogTimeline(json: try await channel.request(
            DesktopStatsMethod.timeline, ["days": .int(Int64(min(max(days, 1), LogTimeline.maxDays)))], timeout: 20))
    }

    public func quality(repo: String) async throws -> RepoQuality {
        RepoQuality(json: try await channel.request(DesktopStatsMethod.quality, ["repo": .string(repo)], timeout: 45))
    }

    public func repoStats(repo: String) async throws -> RepoStatsSnapshot {
        RepoStatsSnapshot(json: try await channel.request(DesktopStatsMethod.repo, ["repo": .string(repo)], timeout: 30))
    }

    public func prepareRatchet(repo: String, enabled: Bool) async throws -> DesktopConfirmation {
        let reply = try await channel.request(
            DesktopStatsMethod.ratchetPrepare, ["repo": .string(repo), "enabled": .bool(enabled)], timeout: 20)
        guard let confirmation = DesktopConfirmation(json: reply) else { throw InsightsError("bad_answer", "The desktop sent an answer this app does not understand.") }
        return confirmation
    }

    public func runRatchet(_ confirmation: DesktopConfirmation) async throws -> RepoStatsSnapshot {
        RepoStatsSnapshot(json: try await channel.request(
            DesktopStatsMethod.ratchetRun, ["token": .string(confirmation.token)], timeout: 30))
    }

    public func sentry() async throws -> [SentryIssue] {
        try await channel.request(DesktopStatsMethod.sentry, [:], timeout: 30).maps("issues").prefix(100).map {
            SentryIssue(id: $0.str("id"), title: $0.str("title"), project: $0.str("project"), level: $0.str("level"),
                        count: $0.int("count"), lastSeen: $0.int64("lastSeen"))
        }
    }

    public func codemagic() async throws -> CodemagicUsage {
        guard let usage = CodemagicUsage(json: try await channel.request(DesktopStatsMethod.codemagic, [:], timeout: 25)) else {
            throw InsightsError("bad_answer", "The desktop sent an answer this app does not understand.")
        }
        return usage
    }

    public nonisolated static func explain(_ error: Error) -> String {
        if DesktopControlFamily.isUnsupported(error) {
            return "Your desktop does not offer this yet. Update ForkMesh on the desktop."
        }
        if let insights = error as? InsightsError { return insights.message }
        let text = dartErrorText(error)
        if text.contains("codemagic_not_configured") { return "Add CODEMAGIC_API_TOKEN to the desktop's Actions variables." }
        if text.contains("codemagic_unauthorized") { return "Codemagic refused the desktop's token." }
        if text.contains("codemagic_unreachable") { return "Codemagic did not answer the desktop." }
        if text.contains("sentry_not_installed") { return "Install the Sentry package on the desktop." }
        if text.contains("sentry_not_connected") { return "Connect Sentry in the desktop's Sentry package." }
        for code in ["timeout", "timed_out"] where text.contains(code) {
            return "Your desktop took too long to answer. Pull to try again."
        }
        for code in ["not_paired", "not_connected", "offline", "unreachable"] where text.contains(code) {
            return "Your desktop is not reachable right now."
        }
        for code in ["expired", "bad_token"] where text.contains(code) {
            return "The confirmation expired. Try again."
        }
        return text
    }
}

public enum DesktopLoad<Value: Equatable & Sendable>: Equatable, Sendable {
    case idle
    case loading
    case unsupported
    case failed(String)
    case loaded(Value)

    public var value: Value? {
        if case .loaded(let value) = self { return value }
        return nil
    }

    public static func from(_ error: Error) -> DesktopLoad<Value> {
        DesktopControlFamily.isUnsupported(error) ? .unsupported : .failed(DesktopStatsClient.explain(error))
    }
}

@MainActor
@Observable
public final class DesktopStatsModel {
    @ObservationIgnored public let client: DesktopStatsClient
    public private(set) var timeline: DesktopLoad<LogTimeline> = .idle
    public private(set) var sentry: DesktopLoad<[SentryIssue]> = .idle
    public private(set) var quality: DesktopLoad<RepoQuality> = .idle
    public private(set) var repoStats: DesktopLoad<RepoStatsSnapshot> = .idle
    public private(set) var pendingRatchet: DesktopConfirmation?
    public private(set) var ratchetError = ""

    public init(client: DesktopStatsClient) {
        self.client = client
    }

    public func loadTimeline() async {
        if timeline.value == nil { timeline = .loading }
        do { timeline = .loaded(try await client.timeline()) } catch { timeline = .from(error) }
    }

    public func loadSentry() async {
        if sentry.value == nil { sentry = .loading }
        do { sentry = .loaded(try await client.sentry()) } catch { sentry = .from(error) }
    }

    public func loadRepo(_ repo: String) async {
        if quality.value == nil { quality = .loading }
        if repoStats.value == nil { repoStats = .loading }
        do { repoStats = .loaded(try await client.repoStats(repo: repo)) } catch { repoStats = .from(error) }
        do { quality = .loaded(try await client.quality(repo: repo)) } catch { quality = .from(error) }
    }

    public func prepareRatchet(_ repo: String, enabled: Bool) async {
        ratchetError = ""
        do {
            pendingRatchet = try await client.prepareRatchet(repo: repo, enabled: enabled)
        } catch {
            pendingRatchet = nil
            ratchetError = DesktopStatsClient.explain(error)
        }
    }

    public func confirmRatchet() async {
        guard let confirmation = pendingRatchet else { return }
        pendingRatchet = nil
        do {
            repoStats = .loaded(try await client.runRatchet(confirmation))
        } catch {
            ratchetError = DesktopStatsClient.explain(error)
        }
    }

    public func cancelRatchet() { pendingRatchet = nil }
}
