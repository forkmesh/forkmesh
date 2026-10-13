import FMBase
import FMNet
import Foundation

public enum AgentLiveEvent: Equatable, Sendable {
    case text(String, atMs: Int)
    case append(String, atMs: Int)
    case end(status: String)
}

public struct AgentLiveFrameParser: Sendable {
    public private(set) var retryMs: Int?
    private var event = ""
    private var data: [String] = []
    private var pending = Data()
    private var skipLineFeed = false

    public init() {}

    public mutating func feed(_ chunk: Data) -> [AgentLiveEvent] {
        var out: [AgentLiveEvent] = []
        var start = chunk.startIndex
        var index = chunk.startIndex
        while index < chunk.endIndex {
            let byte = chunk[index]
            if skipLineFeed {
                skipLineFeed = false
                if byte == 0x0A {
                    index += 1
                    start = index
                    continue
                }
            }
            if byte == 0x0A || byte == 0x0D {
                pending.append(chunk[start..<index])
                let line = String(decoding: pending, as: UTF8.self)
                pending.removeAll(keepingCapacity: true)
                if let event = feedLine(line) { out.append(event) }
                skipLineFeed = byte == 0x0D
                index += 1
                start = index
            } else {
                index += 1
            }
        }
        if start < chunk.endIndex { pending.append(chunk[start..<chunk.endIndex]) }
        return out
    }

    public mutating func feedLine(_ line: String) -> AgentLiveEvent? {
        if line.isEmpty { return dispatch() }
        if line.hasPrefix(":") { return nil }
        let field: Substring
        var value: Substring
        if let colon = line.firstIndex(of: ":") {
            field = line[..<colon]
            value = line[line.index(after: colon)...]
            if value.hasPrefix(" ") { value = value.dropFirst() }
        } else {
            field = line[...]
            value = ""
        }
        switch field {
        case "event":
            event = String(value)
        case "data":
            data.append(String(value))
        case "retry":
            if !value.isEmpty, value.allSatisfy({ $0.isASCII && $0.isNumber }), let ms = Int(value) { retryMs = ms }
        default:
            break
        }
        return nil
    }

    private mutating func dispatch() -> AgentLiveEvent? {
        let name = event
        let lines = data
        event = ""
        data = []
        if lines.isEmpty && name != "end" { return nil }
        return Self.decode(event: name, data: lines.joined(separator: "\n"))
    }

    public static func decode(event: String, data: String) -> AgentLiveEvent? {
        let body = JSON.parseObject(data)
        switch event {
        case "live":
            let at = agentLaunchMs(body["at"])
            if let text = body["text"]?.stringValue { return .text(text, atMs: at) }
            if let more = body["append"]?.stringValue { return .append(more, atMs: at) }
            return nil
        case "end":
            return .end(status: body.str("status"))
        default:
            return nil
        }
    }
}

public struct AgentLiveText: Equatable, Sendable {
    public private(set) var text = ""
    public private(set) var atMs = 0
    public private(set) var started = false

    public init() {}

    @discardableResult
    public mutating func apply(_ event: AgentLiveEvent) -> Bool {
        switch event {
        case .text(let full, let at):
            text = full
            atMs = at
            started = true
            return true
        case .append(let more, let at):
            guard started else { return false }
            text += more
            if at > 0 { atMs = at }
            return true
        case .end:
            return false
        }
    }
}

public enum AgentLiveOutcome: Equatable, Sendable {
    case ended(String)
    case refused(Int)
    case cancelled
}

public struct AgentLiveStream: Sendable {
    public static let firstBackoff: TimeInterval = 2
    public static let maxBackoff: TimeInterval = 30
    public static let defaultRetryMs = 1000

    public static var unavailable: HTTPStreamedResponse {
        HTTPStreamedResponse(status: 404, body: AsyncThrowingStream { $0.finish() })
    }

    let open: @MainActor @Sendable () async throws -> HTTPStreamedResponse
    let sleep: @Sendable (TimeInterval) async throws -> Void

    public init(
        open: @escaping @MainActor @Sendable () async throws -> HTTPStreamedResponse,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.open = open
        self.sleep = sleep
    }

    public static func isTransient(_ status: Int) -> Bool {
        status >= 500 || status == 408 || status == 429
    }

    @MainActor
    public func run(_ onLive: @MainActor (AgentLiveText) -> Void) async -> AgentLiveOutcome {
        var backoff = Self.firstBackoff
        while true {
            if Task.isCancelled { return .cancelled }
            var live = AgentLiveText()
            var parser = AgentLiveFrameParser()
            var failed = false
            do {
                let response = try await open()
                if (200...299).contains(response.status) {
                    for try await chunk in response.body {
                        for event in parser.feed(chunk) {
                            if case .end(let status) = event { return .ended(status) }
                            if live.apply(event) { onLive(live) }
                        }
                    }
                } else if Self.isTransient(response.status) {
                    failed = true
                } else {
                    return .refused(response.status)
                }
            } catch {
                if Task.isCancelled || error is CancellationError { return .cancelled }
                failed = true
            }
            let delay: TimeInterval
            if live.started {
                backoff = Self.firstBackoff
                delay = failed ? Self.firstBackoff : TimeInterval(parser.retryMs ?? Self.defaultRetryMs) / 1000
            } else {
                delay = backoff
                backoff = min(backoff * 2, Self.maxBackoff)
            }
            do {
                try await sleep(delay)
            } catch {
                return .cancelled
            }
        }
    }
}

extension ApiService {
    public func agentLiveStream(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> HTTPStreamedResponse {
        let url = agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope).appendingPathComponent("live")
        let request = HTTPRequest(
            method: "GET", url: url, headers: withAuth(["Accept": "text/event-stream", "Cache-Control": "no-cache"]),
            timeout: 45)
        return try await http.stream(request)
    }
}
