// The WatchConnectivity envelope: every message, application context and
// complication user-info is one dictionary {"fm": kind, "v": version,
// "body": JSON Data}, so both sides can reject what they do not understand
// and the property-list rules of WCSession never meet a Swift type.

import FMAgentWatch
import FMBase
import Foundation

/// What the watch asks the phone to do (PARITY 4.2): the phone runs the HTTP
/// or LAN call and replies with a `WatchReply`.
public enum WatchCommand: Codable, Equatable, Sendable {
    /// Read everything again (pull to refresh, the Crown at the top).
    case refresh
    /// The watch's live view is on this agent (nil: back on the list), so the
    /// phone reads its transcript first and more often.
    case watch(key: String?)
    case stop(key: String)
    case `continue`(key: String)
    case prompt(key: String, text: String)
    case retry(key: String)
    /// Listen switched on or off on the watch (the phone only records it).
    case listen(on: Bool)
    case diff(key: String)
    case start(text: String, repo: String?)

    /// The relay caps an agent prompt at this many UTF-16 units
    /// (`ApiService.maxAgentPromptChars`).
    public static let maxPromptChars = 8000

    public var key: String? {
        switch self {
        case .refresh, .listen, .start: return nil
        case .watch(let key): return key
        case .stop(let key), .continue(let key), .prompt(let key, _), .retry(let key), .diff(let key): return key
        }
    }

    enum CodingKeys: String, CodingKey { case op, key, text, on, repo }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let op = try c.decode(String.self, forKey: .op)
        let key = try c.decodeIfPresent(String.self, forKey: .key)
        func required() throws -> String {
            guard let key, !key.isEmpty else {
                throw DecodingError.dataCorruptedError(forKey: .key, in: c, debugDescription: "\(op) needs a key")
            }
            return key
        }
        switch op {
        case "refresh": self = .refresh
        case "watch": self = .watch(key: key)
        case "stop": self = .stop(key: try required())
        case "continue": self = .continue(key: try required())
        case "prompt":
            self = .prompt(key: try required(), text: try c.decodeIfPresent(String.self, forKey: .text) ?? "")
        case "retry": self = .retry(key: try required())
        case "listen": self = .listen(on: try c.decodeIfPresent(Bool.self, forKey: .on) ?? false)
        case "diff": self = .diff(key: try required())
        case "start":
            self = .start(
                text: try c.decodeIfPresent(String.self, forKey: .text) ?? "",
                repo: try c.decodeIfPresent(String.self, forKey: .repo))
        default:
            throw DecodingError.dataCorruptedError(forKey: .op, in: c, debugDescription: "unknown op \(op)")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .refresh: try c.encode("refresh", forKey: .op)
        case .watch(let key):
            try c.encode("watch", forKey: .op)
            try c.encodeIfPresent(key, forKey: .key)
        case .stop(let key):
            try c.encode("stop", forKey: .op)
            try c.encode(key, forKey: .key)
        case .continue(let key):
            try c.encode("continue", forKey: .op)
            try c.encode(key, forKey: .key)
        case .prompt(let key, let text):
            try c.encode("prompt", forKey: .op)
            try c.encode(key, forKey: .key)
            try c.encode(text, forKey: .text)
        case .retry(let key):
            try c.encode("retry", forKey: .op)
            try c.encode(key, forKey: .key)
        case .listen(let on):
            try c.encode("listen", forKey: .op)
            try c.encode(on, forKey: .on)
        case .diff(let key):
            try c.encode("diff", forKey: .op)
            try c.encode(key, forKey: .key)
        case .start(let text, let repo):
            try c.encode("start", forKey: .op)
            try c.encode(text, forKey: .text)
            try c.encodeIfPresent(repo, forKey: .repo)
        }
    }

    /// A prompt as the relay will take it: trimmed, and refused when empty or
    /// too long. Returns the sentence to show, or nil when it is fine.
    public static func promptProblem(_ text: String) -> String? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return "Say what the agent should do next." }
        if trimmed.utf16.count > maxPromptChars { return "That prompt is too long for the relay." }
        return nil
    }
}

/// The phone's answer to a command.
public struct WatchReply: Codable, Equatable, Sendable {
    public var ok: Bool
    /// A sentence for the watch to show ("Stopped.", "No desktop on this
    /// network can stop that run.").
    public var message: String

    public init(ok: Bool, message: String = "") {
        self.ok = ok
        self.message = message
    }

    public static func done(_ message: String = "") -> WatchReply { WatchReply(ok: true, message: message) }
    public static func refused(_ message: String) -> WatchReply { WatchReply(ok: false, message: message) }
}

public enum WatchWire {
    public static let version = 1
    /// WCSession's message and application-context payloads are capped near
    /// 64 KB; stay under with room for the envelope.
    public static let maxBodyBytes = 56_000

    public enum Kind: String, Sendable {
        case snapshot, command, reply, summary, diff
    }

    public enum WireError: Error, Equatable {
        case notForkMesh
        case wrongKind(String)
        case newerVersion(Int)
        case undecodable(String)
    }

    static func encoder() -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }

    /// The dictionary to hand WCSession.
    public static func envelope<T: Encodable>(_ kind: Kind, _ value: T) throws -> [String: Any] {
        ["fm": kind.rawValue, "v": version, "body": try encoder().encode(value)]
    }

    /// Which kind a received dictionary is, or nil when it is not ours.
    public static func kind(of dictionary: [String: Any]) -> Kind? {
        (dictionary["fm"] as? String).flatMap(Kind.init(rawValue:))
    }

    public static func open<T: Decodable>(_ kind: Kind, _ dictionary: [String: Any], as type: T.Type) throws -> T {
        guard let found = dictionary["fm"] as? String else { throw WireError.notForkMesh }
        guard found == kind.rawValue else { throw WireError.wrongKind(found) }
        let v = (dictionary["v"] as? Int) ?? 0
        if v > version { throw WireError.newerVersion(v) }
        guard let body = dictionary["body"] as? Data else { throw WireError.undecodable("no body") }
        do {
            return try JSONDecoder().decode(type, from: body)
        } catch {
            throw WireError.undecodable("\(error)")
        }
    }

    public static func encodedSize<T: Encodable>(_ value: T) -> Int {
        (try? encoder().encode(value).count) ?? Int.max
    }

    /// The snapshot cut down until it fits one WCSession payload: first the
    /// older beats of the finished runs, then of every run, then the oldest
    /// finished runs themselves. The frame the watch is looking at keeps
    /// its beats longest.
    public static func fitted(_ snapshot: WatchSnapshot, maxBytes: Int = maxBodyBytes, keep: String? = nil) -> WatchSnapshot {
        var out = snapshot
        out.frames = AgentFrame.ordered(out.frames)
        if encodedSize(out) <= maxBytes { return out }
        for limit in [16, 4, 0] {
            for i in out.frames.indices where !out.frames[i].status.isActive && out.frames[i].key != keep {
                out.frames[i].beats = Array(out.frames[i].beats.suffix(limit))
            }
            if encodedSize(out) <= maxBytes { return out }
        }
        for limit in [32, 16, 8, 0] {
            for i in out.frames.indices where out.frames[i].key != keep {
                out.frames[i].beats = Array(out.frames[i].beats.suffix(limit))
            }
            if encodedSize(out) <= maxBytes { return out }
        }
        for i in out.frames.indices {
            out.frames[i].beats = out.frames[i].beats.map { beat in
                var b = beat
                b.said = ""
                return b
            }
            if out.frames[i].lastReply.count > 120 {
                out.frames[i].lastReply = String(out.frames[i].lastReply.prefix(119)) + "\u{2026}"
            }
        }
        while encodedSize(out) > maxBytes, out.frames.count > 1 {
            if let drop = out.frames.lastIndex(where: { $0.key != keep }) {
                out.frames.remove(at: drop)
            } else {
                out.frames.removeLast()
            }
        }
        if encodedSize(out) > maxBytes, !out.frames.isEmpty {
            out.frames[0].beats = []
        }
        return out
    }

    /// The beats of `next` the watch has not played yet, given the frame it
    /// last had. Beats are a prefix-stable list, so the count is enough.
    public static func newBeats(previous: AgentFrame?, next: AgentFrame) -> [WireBeat] {
        guard let previous else { return [] }
        let fresh = next.beatCount - previous.beatCount
        if fresh <= 0 { return [] }
        return Array(next.beats.suffix(min(fresh, next.beats.count)))
    }
}

/// Where the watch app leaves the last summary for its widget extension.
public enum WatchGroup {
    public static let keychainService = "com.forkmesh.forkmesh.watchkitapp.summary"
    public static let keychainGroup = "742ZANCB57.com.forkmesh.forkmesh.watchkitapp"
    public static let summaryKey = "fm.watch.summary"
    public static let widgetKind = "ForkMeshAgents"
    /// The watch app's deep link to an agent's live view.
    public static let urlScheme = "forkmeshwatch"

    public static func agentURL(_ key: String) -> URL? {
        var components = URLComponents()
        components.scheme = urlScheme
        components.host = "agent"
        components.queryItems = [URLQueryItem(name: "key", value: key)]
        return components.url
    }

    public static func agentKey(from url: URL) -> String? {
        guard url.scheme == urlScheme, url.host == "agent" else { return nil }
        let key = URLComponents(url: url, resolvingAgainstBaseURL: false)?
            .queryItems?.first { $0.name == "key" }?.value ?? ""
        return key.isEmpty ? nil : key
    }

    public static func shared() -> SharedItemStore? {
        #if canImport(Security)
        return KeychainSharedItemStore(service: keychainService, accessGroup: keychainGroup)
        #else
        return nil
        #endif
    }

    public static func save(_ summary: ComplicationSummary, to items: SharedItemStore?) {
        guard let items, let data = try? JSONEncoder().encode(summary) else { return }
        items.set(data, summaryKey)
    }

    public static func load(from items: SharedItemStore?) -> ComplicationSummary? {
        guard let data = items?.data(summaryKey) else { return nil }
        return try? JSONDecoder().decode(ComplicationSummary.self, from: data)
    }
}

/// At most one haptic per interval (PARITY 4.3: 0.6 s), so a burst of beats
/// is one tap rather than a buzz. Taps inside the interval are dropped, not
/// queued; of a batch, the most important one plays.
public struct HapticPacer: Sendable {
    public let interval: TimeInterval
    private var lastAt: TimeInterval = -TimeInterval.infinity

    public init(interval: TimeInterval = 0.6) {
        self.interval = interval
    }

    /// The haptic to play for these beats now, or nil.
    public mutating func next(for beats: [WireBeat], now: TimeInterval) -> WatchHapticKind? {
        let candidates = beats.compactMap(\.haptic)
        guard let pick = candidates.max(by: { HapticPacer.weight($0) < HapticPacer.weight($1) }) else { return nil }
        return take(pick, now: now)
    }

    public mutating func take(_ haptic: WatchHapticKind, now: TimeInterval) -> WatchHapticKind? {
        if now - lastAt < interval { return nil }
        lastAt = now
        return haptic
    }

    static func weight(_ haptic: WatchHapticKind) -> Int {
        switch haptic {
        case .failure: return 5
        case .success: return 4
        case .notification: return 3
        case .directionUp: return 2
        case .click: return 1
        }
    }
}
