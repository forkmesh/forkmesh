// What the iPhone app tells the watch about one agent, and about all of them
// (docs/native/PARITY.md, 4.2 option A). The phone is the only ForkMesh
// client: it unseals transcripts, turns them into beats and sends these
// compact frames; the watch only draws, plays and feels them.

import FMAgentWatch
import Foundation

/// A run's state as the watch colours it.
public enum WireStatus: String, Codable, CaseIterable, Sendable {
    case queued, running, waiting, done, failed, stopped, merged, unknown

    public init(from decoder: Decoder) throws {
        let raw = try decoder.singleValueContainer().decode(String.self)
        self = WireStatus(rawValue: raw) ?? .unknown
    }

    /// From a relay or desktop status string (`AgentSession.status`).
    public static func from(_ status: String, merged: Bool = false) -> WireStatus {
        if merged { return .merged }
        switch status.trimmingCharacters(in: .whitespaces).lowercased().replacingOccurrences(of: "_", with: "-") {
        case "queued", "pending", "claimed": return .queued
        case "running", "started", "in-progress": return .running
        case "waiting", "waiting-for-input": return .waiting
        case "done", "completed", "success", "succeeded": return .done
        case "failed", "error": return .failed
        case "stopped", "cancelled", "canceled": return .stopped
        case "merged": return .merged
        default: return .unknown
        }
    }

    public var isActive: Bool { self == .queued || self == .running || self == .waiting }

    public var label: String {
        switch self {
        case .queued: return "Queued"
        case .running: return "Running"
        case .waiting: return "Waiting"
        case .done: return "Done"
        case .failed: return "Failed"
        case .stopped: return "Stopped"
        case .merged: return "Merged"
        case .unknown: return "Unknown"
        }
    }

    /// Running first, then waiting and queued, then the finished.
    var rank: Int {
        switch self {
        case .running: return 0
        case .waiting: return 1
        case .queued: return 2
        default: return 3
        }
    }
}

/// Where a frame came from, which decides how its controls are carried out.
public enum WireSource: String, Codable, Sendable {
    /// A desktop's owner-sealed snapshot, read through the relay.
    case desktop
    /// A run in the relay queue (started from a phone or the website).
    case queued
    /// A desktop on the same network, over the local control channel.
    case lan
}

/// Which controls the phone can carry out for a frame right now. Pause and
/// Approve do not exist server-side (PARITY 4.1), so they are not here.
public struct WireControls: OptionSet, Codable, Hashable, Sendable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }

    public static let stop = WireControls(rawValue: 1 << 0)
    public static let `continue` = WireControls(rawValue: 1 << 1)
    public static let prompt = WireControls(rawValue: 1 << 2)
    public static let retry = WireControls(rawValue: 1 << 3)
    public static let diff = WireControls(rawValue: 1 << 4)
}

public struct AgentFrame: Codable, Equatable, Hashable, Sendable, Identifiable {
    /// Stable across refreshes: "desktop:<owner>/<repo>:<id>",
    /// "queued:<owner>/<repo>:<sessionId>" or "lan:<owner>/<repo>:<id>".
    public var key: String
    public var title: String
    public var repo: String
    public var status: WireStatus
    public var provider: String
    public var model: String
    public var turns: Int
    public var tokens: Int
    public var contextTokens: Int
    public var contextWindow: Int
    public var startedAtMs: Int64
    public var updatedAtMs: Int64
    public var source: WireSource
    public var controls: WireControls
    /// The latest beats, oldest first (at most `AgentFrame.maxBeats`).
    public var beats: [WireBeat]
    /// How many beats the whole transcript holds, so the watch knows which of
    /// `beats` are new since the last frame.
    public var beatCount: Int
    /// The last reply as one plain line, for the dot display.
    public var lastReply: String

    public static let maxBeats = 64

    public var id: String { key }

    public init(
        key: String, title: String, repo: String, status: WireStatus, provider: String = "", model: String = "",
        turns: Int = 0, tokens: Int = 0, contextTokens: Int = 0, contextWindow: Int = 0,
        startedAtMs: Int64 = 0, updatedAtMs: Int64 = 0, source: WireSource = .desktop,
        controls: WireControls = [], beats: [WireBeat] = [], beatCount: Int? = nil, lastReply: String = ""
    ) {
        self.key = key
        self.title = title
        self.repo = repo
        self.status = status
        self.provider = provider
        self.model = model
        self.turns = turns
        self.tokens = tokens
        self.contextTokens = contextTokens
        self.contextWindow = contextWindow
        self.startedAtMs = startedAtMs
        self.updatedAtMs = updatedAtMs
        self.source = source
        self.controls = controls
        self.beats = Array(beats.suffix(AgentFrame.maxBeats))
        self.beatCount = beatCount ?? beats.count
        self.lastReply = lastReply
    }

    /// The short name of the model or provider the row's face shows:
    /// "claude-opus-5-5" reads "Opus", "gpt-5-codex" "GPT".
    public var face: String {
        let text = (model.isEmpty ? provider : model).lowercased()
        let known: [(String, String)] = [
            ("opus", "Opus"), ("sonnet", "Sonnet"), ("haiku", "Haiku"), ("fable", "Fable"),
            ("claude", "Claude"), ("codex", "Codex"), ("gpt", "GPT"), ("gemini", "Gemini"),
            ("gemma", "Gemma"), ("llama", "Llama"),
        ]
        for (needle, name) in known where text.contains(needle) { return name }
        if text.isEmpty { return "Agent" }
        return String(text.prefix(1)).uppercased() + String(text.dropFirst().prefix(7))
    }

    /// Context used, 0...1, or nil when the window is not known.
    public var contextFraction: Double? {
        guard contextWindow > 0 else { return nil }
        return min(1, max(0, Double(contextTokens) / Double(contextWindow)))
    }

    /// The hue the body is drawn in: the latest beat's kind.
    public var hue: Double { beats.last?.motion.hue ?? AgentMotion.tick.hue }

    /// Sort order: running first, then by recency.
    public static func ordered(_ frames: [AgentFrame]) -> [AgentFrame] {
        frames.enumerated().sorted { a, b in
            if a.element.status.rank != b.element.status.rank { return a.element.status.rank < b.element.status.rank }
            if a.element.updatedAtMs != b.element.updatedAtMs { return a.element.updatedAtMs > b.element.updatedAtMs }
            return a.offset < b.offset
        }.map(\.element)
    }

    enum CodingKeys: String, CodingKey {
        case key = "k", title = "ti", repo = "r", status = "st", provider = "p", model = "mo"
        case turns = "tu", tokens = "to", contextTokens = "ct", contextWindow = "cw"
        case startedAtMs = "sa", updatedAtMs = "ua", source = "so", controls = "c"
        case beats = "b", beatCount = "bc", lastReply = "lr"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        key = try c.decode(String.self, forKey: .key)
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? ""
        repo = try c.decodeIfPresent(String.self, forKey: .repo) ?? ""
        status = try c.decodeIfPresent(WireStatus.self, forKey: .status) ?? .unknown
        provider = try c.decodeIfPresent(String.self, forKey: .provider) ?? ""
        model = try c.decodeIfPresent(String.self, forKey: .model) ?? ""
        turns = try c.decodeIfPresent(Int.self, forKey: .turns) ?? 0
        tokens = try c.decodeIfPresent(Int.self, forKey: .tokens) ?? 0
        contextTokens = try c.decodeIfPresent(Int.self, forKey: .contextTokens) ?? 0
        contextWindow = try c.decodeIfPresent(Int.self, forKey: .contextWindow) ?? 0
        startedAtMs = try c.decodeIfPresent(Int64.self, forKey: .startedAtMs) ?? 0
        updatedAtMs = try c.decodeIfPresent(Int64.self, forKey: .updatedAtMs) ?? 0
        source = (try? c.decodeIfPresent(WireSource.self, forKey: .source)) ?? .desktop
        controls = try c.decodeIfPresent(WireControls.self, forKey: .controls) ?? []
        beats = try c.decodeIfPresent([WireBeat].self, forKey: .beats) ?? []
        beatCount = try c.decodeIfPresent(Int.self, forKey: .beatCount) ?? beats.count
        lastReply = try c.decodeIfPresent(String.self, forKey: .lastReply) ?? ""
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(key, forKey: .key)
        try c.encode(title, forKey: .title)
        try c.encode(repo, forKey: .repo)
        try c.encode(status, forKey: .status)
        if !provider.isEmpty { try c.encode(provider, forKey: .provider) }
        if !model.isEmpty { try c.encode(model, forKey: .model) }
        if turns != 0 { try c.encode(turns, forKey: .turns) }
        if tokens != 0 { try c.encode(tokens, forKey: .tokens) }
        if contextTokens != 0 { try c.encode(contextTokens, forKey: .contextTokens) }
        if contextWindow != 0 { try c.encode(contextWindow, forKey: .contextWindow) }
        if startedAtMs != 0 { try c.encode(startedAtMs, forKey: .startedAtMs) }
        if updatedAtMs != 0 { try c.encode(updatedAtMs, forKey: .updatedAtMs) }
        try c.encode(source, forKey: .source)
        if !controls.isEmpty { try c.encode(controls, forKey: .controls) }
        if !beats.isEmpty { try c.encode(beats, forKey: .beats) }
        if beatCount != beats.count { try c.encode(beatCount, forKey: .beatCount) }
        if !lastReply.isEmpty { try c.encode(lastReply, forKey: .lastReply) }
    }
}

/// Everything the watch shows: the phone's latest view of the account's agents.
public struct WatchSnapshot: Codable, Equatable, Sendable {
    public var generatedAtMs: Int64
    public var signedIn: Bool
    /// A sentence for an empty or failed list ("Sign in on your iPhone.").
    public var note: String
    public var frames: [AgentFrame]

    public init(generatedAtMs: Int64 = 0, signedIn: Bool = true, note: String = "", frames: [AgentFrame] = []) {
        self.generatedAtMs = generatedAtMs
        self.signedIn = signedIn
        self.note = note
        self.frames = frames
    }

    public var runningCount: Int { frames.filter { $0.status == .running || $0.status == .waiting }.count }
    public var activeCount: Int { frames.filter(\.status.isActive).count }

    public func frame(_ key: String) -> AgentFrame? { frames.first { $0.key == key } }

    /// What the complications show.
    public var summary: ComplicationSummary {
        let top = AgentFrame.ordered(frames).first
        return ComplicationSummary(
            generatedAtMs: generatedAtMs, running: runningCount, active: activeCount, total: frames.count,
            topKey: top?.key ?? "", topTitle: top?.title ?? "", topStatus: top?.status ?? .unknown,
            topFace: top?.face ?? "", topHue: top?.hue ?? 0.58,
            topProgress: top?.contextFraction ?? 0,
            lastFinishedAtMs: frames.filter { !$0.status.isActive }.map(\.updatedAtMs).max() ?? 0)
    }
}

/// The last snapshot, boiled down for the complications and the Smart Stack.
/// Travels in `transferCurrentComplicationUserInfo` and sits in the App Group.
public struct ComplicationSummary: Codable, Equatable, Sendable {
    public var generatedAtMs: Int64
    public var running: Int
    public var active: Int
    public var total: Int
    public var topKey: String
    public var topTitle: String
    public var topStatus: WireStatus
    public var topFace: String
    public var topHue: Double
    public var topProgress: Double
    public var lastFinishedAtMs: Int64

    public init(
        generatedAtMs: Int64 = 0, running: Int = 0, active: Int = 0, total: Int = 0, topKey: String = "",
        topTitle: String = "", topStatus: WireStatus = .unknown, topFace: String = "", topHue: Double = 0.58,
        topProgress: Double = 0, lastFinishedAtMs: Int64 = 0
    ) {
        self.generatedAtMs = generatedAtMs
        self.running = running
        self.active = active
        self.total = total
        self.topKey = topKey
        self.topTitle = topTitle
        self.topStatus = topStatus
        self.topFace = topFace
        self.topHue = topHue
        self.topProgress = topProgress
        self.lastFinishedAtMs = lastFinishedAtMs
    }

    /// "2 agents running", "1 agent queued", "No agents running".
    public var inlineText: String {
        if running > 0 { return running == 1 ? "1 agent running" : "\(running) agents running" }
        if active > 0 { return active == 1 ? "1 agent queued" : "\(active) agents queued" }
        return "No agents running"
    }

    /// Smart Stack relevance, 0...1: high while a run is active, raised for
    /// a quarter of an hour after one finished, low otherwise.
    public func relevance(nowMs: Int64) -> Double {
        if running > 0 { return 1.0 }
        if active > 0 { return 0.7 }
        if lastFinishedAtMs > 0 && nowMs - lastFinishedAtMs < 15 * 60 * 1000 { return 0.6 }
        return 0.05
    }
}
