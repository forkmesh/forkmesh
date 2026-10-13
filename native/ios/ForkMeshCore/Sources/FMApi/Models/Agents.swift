// The owner-sealed agent snapshot models: AgentTranscript, AgentSessionPage,
// AgentSession. (The relay queue models are in AgentLaunch.swift.)

import Foundation
import FMBase

public struct AgentTranscript: Equatable, Hashable, Sendable {
    public var status: String
    public var transcript: String
    /// The relay held an owner-sealed transcript this device is not a recipient
    /// of. Distinct from an empty transcript: the work exists, it just cannot be
    /// read here until this device is enrolled as an owner device.
    public var sealedForOtherDevice: Bool

    public init(status: String, transcript: String, sealedForOtherDevice: Bool = false) {
        self.status = status
        self.transcript = transcript
        self.sealedForOtherDevice = sealedForOtherDevice
    }

    public init(json: JSONObject) {
        self.init(status: json.str("status"), transcript: json.str("transcript"))
    }
}

/// One repository's agent sessions, plus how many stayed unreadable here.
public struct AgentSessionPage: Equatable, Hashable, Sendable {
    public var sessions: [AgentSession]
    /// Owner-sealed sessions addressed only to other owner devices. Surfacing
    /// the count keeps "this repo has no agent sessions" honest and separate
    /// from "this phone is not enrolled to read them".
    public var sealedForOthers: Int

    public init(sessions: [AgentSession] = [], sealedForOthers: Int = 0) {
        self.sessions = sessions
        self.sealedForOthers = sealedForOthers
    }
}

public struct AgentSession: Equatable, Hashable, Sendable, Identifiable {
    public var id: Int
    public var issueNumber: Int
    public var issueTitle: String
    public var status: String
    public var provider: String
    public var model: String
    public var branchName: String
    public var lastError: String
    public var createdAtMs: Int
    public var startedAtMs: Int
    public var finishedAtMs: Int
    public var numTurns: Int
    public var durationMs: Int
    public var costUsd: Double
    public var createPr: Bool
    public var prNumber: Int
    public var baseRef: String
    public var baseBranch: String
    public var merged: Bool
    public var mergedAtMs: Int
    public var promptTokens: Int
    public var completionTokens: Int
    public var totalTokens: Int
    public var contextTokens: Int
    public var contextWindow: Int
    public var maxOutputTokens: Int
    public var estimatedCredits: Int
    public var spendBeforeUsd: Double
    public var spendAfterUsd: Double
    /// -1 when unknown (as are `ahead` and `behind`).
    public var filesChanged: Int
    public var ahead: Int
    public var behind: Int
    public var conflicted: Bool
    public var linesAdded: Int
    public var linesRemoved: Int
    public var hasWorktree: Bool
    public var statusReason: String
    public var rateLimitResetAtMs: Int
    public var rateLimitResetAfterMs: Int
    public var priority: Int

    public init(
        id: Int, issueNumber: Int = 0, issueTitle: String = "", status: String = "", provider: String = "",
        model: String = "", branchName: String = "", lastError: String = "", createdAtMs: Int = 0,
        startedAtMs: Int = 0, finishedAtMs: Int = 0, numTurns: Int = 0, durationMs: Int = 0,
        costUsd: Double = 0, createPr: Bool = false, prNumber: Int = 0, baseRef: String = "",
        baseBranch: String = "", merged: Bool = false, mergedAtMs: Int = 0, promptTokens: Int = 0,
        completionTokens: Int = 0, totalTokens: Int = 0, contextTokens: Int = 0, contextWindow: Int = 0,
        maxOutputTokens: Int = 0, estimatedCredits: Int = 0, spendBeforeUsd: Double = 0,
        spendAfterUsd: Double = 0, filesChanged: Int = -1, ahead: Int = -1, behind: Int = -1,
        conflicted: Bool = false, linesAdded: Int = -1, linesRemoved: Int = -1, hasWorktree: Bool = false,
        statusReason: String = "", rateLimitResetAtMs: Int = 0, rateLimitResetAfterMs: Int = 0,
        priority: Int = 50
    ) {
        self.id = id
        self.issueNumber = issueNumber
        self.issueTitle = issueTitle
        self.status = status
        self.provider = provider
        self.model = model
        self.branchName = branchName
        self.lastError = lastError
        self.createdAtMs = createdAtMs
        self.startedAtMs = startedAtMs
        self.finishedAtMs = finishedAtMs
        self.numTurns = numTurns
        self.durationMs = durationMs
        self.costUsd = costUsd
        self.createPr = createPr
        self.prNumber = prNumber
        self.baseRef = baseRef
        self.baseBranch = baseBranch
        self.merged = merged
        self.mergedAtMs = mergedAtMs
        self.promptTokens = promptTokens
        self.completionTokens = completionTokens
        self.totalTokens = totalTokens
        self.contextTokens = contextTokens
        self.contextWindow = contextWindow
        self.maxOutputTokens = maxOutputTokens
        self.estimatedCredits = estimatedCredits
        self.spendBeforeUsd = spendBeforeUsd
        self.spendAfterUsd = spendAfterUsd
        self.filesChanged = filesChanged
        self.ahead = ahead
        self.behind = behind
        self.conflicted = conflicted
        self.linesAdded = linesAdded
        self.linesRemoved = linesRemoved
        self.hasWorktree = hasWorktree
        self.statusReason = statusReason
        self.rateLimitResetAtMs = rateLimitResetAtMs
        self.rateLimitResetAfterMs = rateLimitResetAfterMs
        self.priority = priority
    }

    private static let resetAtKeys = [
        "rateLimitResetAtMs", "limitResetAtMs", "rateLimitResetAt", "limitResetAt", "ratelimitResetAt",
    ]
    private static let resetAfterKeys = [
        "rateLimitResetAfterMs", "retryAfterMs", "limitResetAfterMs", "retryAfter", "rateLimitResetAfter",
        "ratelimitResetAfter", "xRateLimitResetAfter", "x-ratelimit-reset-after", "xrateLimitResetAfter",
    ]

    /// `now` turns a relative "reset after" into the absolute reset time.
    public init(json: JSONObject, now: Date = Date()) {
        let diffStats = json.obj("diffStats")
        var resetAtMs = 0
        for key in Self.resetAtKeys {
            let value = ModelJSON.epochMs(json[key])
            if value > 0 {
                resetAtMs = value
                break
            }
        }
        var resetAfterMs = 0
        for key in Self.resetAfterKeys {
            let lower = key.lowercased()
            let value = ModelJSON.int(json[key])
            if value > 0 {
                resetAfterMs = lower.contains("after") && !lower.hasSuffix("ms") ? value * 1_000 : value
                break
            }
        }
        if resetAtMs <= 0 && resetAfterMs > 0 {
            resetAtMs = Int(now.epochMillis) + resetAfterMs
        }
        self.init(
            id: ModelJSON.int(json["id"]),
            issueNumber: ModelJSON.int(json["issueNumber"]),
            issueTitle: json.str("issueTitle"),
            status: json.str("status"),
            provider: json.str("provider"),
            model: json.str("model"),
            branchName: json.str("branchName"),
            lastError: json.str("lastError"),
            createdAtMs: ModelJSON.int(json["createdAtMs"]),
            startedAtMs: ModelJSON.int(json["startedAtMs"]),
            finishedAtMs: ModelJSON.int(json["finishedAtMs"]),
            numTurns: ModelJSON.int(json["numTurns"]),
            durationMs: ModelJSON.int(json["durationMs"]),
            costUsd: ModelJSON.double(json["costUsd"]),
            createPr: json.bool("createPr"),
            prNumber: ModelJSON.int(json["prNumber"]),
            baseRef: json.str("baseRef"),
            baseBranch: json.str("baseBranch"),
            merged: json.bool("merged"),
            mergedAtMs: ModelJSON.int(json["mergedAtMs"]),
            promptTokens: ModelJSON.int(json["promptTokens"]),
            completionTokens: ModelJSON.int(json["completionTokens"]),
            totalTokens: ModelJSON.int(json["totalTokens"]),
            contextTokens: ModelJSON.int(json["contextTokens"]),
            contextWindow: ModelJSON.int(json["contextWindow"]),
            maxOutputTokens: ModelJSON.int(json["maxOutputTokens"]),
            estimatedCredits: ModelJSON.int(json["estimatedCredits"]),
            spendBeforeUsd: ModelJSON.double(json["spendBeforeUsd"]),
            spendAfterUsd: ModelJSON.double(json["spendAfterUsd"]),
            filesChanged: ModelJSON.optionalInt(
                ModelJSON.coalesce(diffStats["files"], json["filesChanged"], json["changedFiles"])),
            ahead: ModelJSON.optionalInt(ModelJSON.coalesce(diffStats["ahead"], json["ahead"])),
            behind: ModelJSON.optionalInt(ModelJSON.coalesce(diffStats["behind"], json["behind"])),
            conflicted: diffStats.bool("conflicted") || json.bool("conflicted"),
            linesAdded: ModelJSON.optionalInt(ModelJSON.coalesce(diffStats["added"], json["linesAdded"])),
            linesRemoved: ModelJSON.optionalInt(ModelJSON.coalesce(diffStats["removed"], json["linesRemoved"])),
            hasWorktree: json.bool("worktree"),
            statusReason: ModelJSON.firstString(json, ["statusReason", "failureReason", "reason"]),
            rateLimitResetAtMs: resetAtMs,
            rateLimitResetAfterMs: resetAfterMs,
            priority: Self.priority(json["priority"])
        )
    }

    private static func priority(_ value: JSONValue?) -> Int {
        let raw = ModelJSON.int(value)
        return raw <= 0 ? 50 : min(max(raw, 1), 99)
    }

    private var normalized: String { ModelJSON.normalizedStatus(status) }

    public var isActive: Bool {
        if merged { return false }
        return ["queued", "running", "waiting"].contains(normalized)
    }

    public var displayTitle: String {
        if !issueTitle.isEmpty { return issueTitle }
        return issueNumber > 0 ? "Issue #\(issueNumber)" : "Agent session #\(id)"
    }

    public var statusLabel: String {
        if merged { return "Merged" }
        switch normalized {
        case "queued": return "Queued"
        case "running": return "Running"
        case "waiting", "waiting-for-input": return "Waiting"
        case "done", "completed", "success": return "Done"
        case "failed", "error": return "Failed"
        case "stopped", "cancelled", "canceled": return "Stopped"
        default: return status.isEmpty ? "Unknown" : ModelJSON.titleCase(status)
        }
    }

    public var isSuccessful: Bool {
        if merged { return true }
        return ["done", "completed", "success"].contains(normalized)
    }

    public var isFailed: Bool { normalized == "failed" || normalized == "error" }

    public var statusReasonText: String { statusReason.isEmpty ? lastError : statusReason }

    public func successSummaryLabel() -> String {
        var parts: [String] = []
        if numTurns > 0 { parts.append("\(numTurns) turns") }
        if !durationLabel.isEmpty { parts.append(durationLabel) }
        if !costLabel.isEmpty { parts.append(costLabel) }
        if !diffLabel.isEmpty { parts.append(diffLabel) }
        return "Done" + (parts.isEmpty ? "" : " · " + parts.joined(separator: " · "))
    }

    public func latestStatusSummary(_ maxReasonLength: Int = 120) -> String {
        if isSuccessful { return successSummaryLabel() }
        if isFailed {
            let reason = statusReasonText.dartTrimmed
            if reason.isEmpty { return "Failed" }
            return "Failed: \(ModelJSON.truncate(reason, maxReasonLength))"
        }
        return statusLabel
    }

    public func statusFailureReason(_ maxLength: Int) -> String {
        ModelJSON.truncate(statusReasonText, maxLength)
    }

    public func remainingRateLimitResetMs(now: Date = Date()) -> Int {
        if rateLimitResetAtMs <= 0 { return 0 }
        return rateLimitResetAtMs - Int(now.epochMillis)
    }

    /// "Limit resets in 1h 2m", or "" once the limit has lifted.
    public func rateLimitCountdownLabel(now: Date = Date()) -> String {
        let remaining = remainingRateLimitResetMs(now: now)
        if remaining <= 0 { return "" }
        return "Limit resets in \(ModelJSON.compactDuration(milliseconds: remaining))"
    }

    public var providerLabel: String {
        switch provider.lowercased() {
        case "claude-code": return "Claude Code"
        case "claude-api": return "Claude API"
        case "openai": return "OpenAI"
        case "codex": return "Codex"
        case "cloudflare-ai": return "Cloudflare AI"
        case "local-llama": return "Gemma"
        default: return provider.isEmpty ? "Agent" : ModelJSON.titleCase(provider)
        }
    }

    public var durationLabel: String {
        if durationMs <= 0 { return "" }
        let totalSeconds = durationMs / 1_000
        let minutes = totalSeconds / 60
        let seconds = totalSeconds % 60
        if minutes <= 0 { return "\(seconds)s" }
        return "\(minutes)m \(seconds)s"
    }

    public var costLabel: String {
        if costUsd <= 0 { return "" }
        if costUsd < 0.01 { return "<$0.01" }
        return "$" + DartCore.toStringAsFixed(costUsd, 2)
    }

    public var prLabel: String { prNumber > 0 ? "PR #\(prNumber)" : "" }

    /// What `branchName` is compared against: the branch it was cut from, or
    /// the ref the desktop recorded when it did not name one. Empty lets the
    /// viewer fall back to the repository's default branch.
    public var diffBase: String { baseBranch.isEmpty ? baseRef : baseBranch }

    public var tokenLabel: String { totalTokens > 0 ? "\(totalTokens) tokens" : "" }

    public var diffLabel: String {
        var parts: [String] = []
        if filesChanged >= 0 { parts.append(filesChanged == 1 ? "1 file" : "\(filesChanged) files") }
        if ahead >= 0 && behind >= 0 && (ahead > 0 || behind > 0) { parts.append("↑\(ahead) ↓\(behind)") }
        return parts.joined(separator: " · ")
    }
}
