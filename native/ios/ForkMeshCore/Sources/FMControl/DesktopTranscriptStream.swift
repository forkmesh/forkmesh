import FMBase
import FMChat
import Foundation

@MainActor
public final class DesktopTranscriptStream {
    private struct Holder {
        weak var stream: DesktopTranscriptStream?
    }

    private static var holders: [Holder] = []

    public let sessionId: Int
    public private(set) var next: Int
    public private(set) var isStreaming = false

    private let hub: DesktopControlHub
    private let onDelta: @MainActor (AgentTranscriptDelta) -> Void
    private let onEnded: @MainActor (String) -> Void
    private var tapId: UUID?
    private var linkId: UUID?
    private var generation = 0
    private var stopped = false
    private var awaiting = false
    private var followedLink: ObjectIdentifier?
    private var early: [JSONObject] = []
    private var filling = false
    private var held: [JSONObject] = []
    private var retry: Task<Void, Never>?
    private var retryDelay: TimeInterval = 1
    private let sleep: @Sendable (TimeInterval) async throws -> Void

    public init(
        hub: DesktopControlHub,
        sessionId: Int,
        since: Int = 0,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        onEnded: @escaping @MainActor (String) -> Void = { _ in },
        onDelta: @escaping @MainActor (AgentTranscriptDelta) -> Void
    ) {
        self.hub = hub
        self.sessionId = sessionId
        self.next = since
        self.sleep = sleep
        self.onEnded = onEnded
        self.onDelta = onDelta
    }

    @discardableResult
    public func start() async -> Bool {
        stopped = false
        hold()
        if tapId == nil {
            tapId = hub.addEventTap { [weak self] name, data in self?.onEvent(name, data) }
        }
        if linkId == nil {
            linkId = hub.addLinkListener { [weak self] in self?.linkChanged() }
        }
        return await follow()
    }

    @discardableResult
    public func refollow() async -> Bool {
        if stopped { return await start() }
        retry?.cancel()
        retry = nil
        return await follow()
    }

    public func stop() {
        if stopped { return }
        stopped = true
        generation += 1
        isStreaming = false
        filling = false
        held = []
        retry?.cancel()
        retry = nil
        if let tapId { hub.removeEventTap(tapId) }
        if let linkId { hub.removeLinkListener(linkId) }
        tapId = nil
        linkId = nil
        if !release(), let link = hub.currentLink {
            let id = sessionId
            Task { _ = try? await link.request("agents.unfollow", ["id": .int(Int64(id))], timeout: 5) }
        }
    }

    private func follow() async -> Bool {
        generation += 1
        let attempt = generation
        awaiting = true
        early = []
        filling = false
        held = []
        defer {
            if attempt == generation {
                awaiting = false
                early = []
            }
        }
        do {
            let link = try await hub.link()
            followedLink = ObjectIdentifier(link)
            let answer = try await link.request(
                "agents.follow", ["id": .int(Int64(sessionId)), "since": .int(Int64(next))], timeout: 20)
            if stopped || attempt != generation { return isStreaming }
            retryDelay = 1
            isStreaming = answer["streaming"] == .bool(true)
            apply(answer)
            let queued = early
            early = []
            awaiting = false
            for data in queued where isStreaming && !stopped { onEvent("agents.output", data) }
        } catch let refusal as LocalControlException where refusal.code == "unknown_session" {
            if attempt == generation { end(refusal.code) }
        } catch {
            if stopped || attempt != generation { return isStreaming }
            isStreaming = false
            if let refusal = error as? LocalControlException, refusal.code == "unsupported" { return false }
            scheduleRetry()
        }
        return isStreaming
    }

    private func onEvent(_ name: String, _ data: JSONObject) {
        guard !stopped, name == "agents.output", data.int("id") == sessionId else { return }
        if awaiting {
            early.append(data)
            return
        }
        guard isStreaming else { return }
        let error = data.str("error")
        if !error.isEmpty {
            end(error)
            return
        }
        apply(data)
    }

    private func apply(_ data: JSONObject, fills: Bool = true) {
        if filling {
            held.append(data)
            return
        }
        var delta = AgentTranscriptDelta.fromDesktop(data, since: next, sessionId: sessionId)
        if !delta.reset && delta.from > next {
            if fills {
                held.append(data)
                fill()
                return
            }
            delta.reset = true
        }
        if !delta.reset && delta.from < next {
            guard let unseen = Self.unseen(delta, after: next) else { return }
            delta = unseen
        }
        next = delta.next
        onDelta(delta)
    }

    private func fill() {
        filling = true
        let attempt = generation
        let since = next
        Task { [weak self] in await self?.fill(since: since, attempt: attempt) }
    }

    private func fill(since: Int, attempt: Int) async {
        var params: JSONObject = ["id": .int(Int64(sessionId))]
        if since > 0 { params["since"] = .int(Int64(since)) }
        let answer: JSONObject
        do {
            answer = try await hub.request("agents.transcript", params)
        } catch {
            if stopped || attempt != generation { return }
            filling = false
            held = []
            _ = await follow()
            return
        }
        if stopped || attempt != generation { return }
        filling = false
        let queued = held
        held = []
        apply(answer, fills: false)
        for data in queued where !stopped && attempt == generation { apply(data, fills: false) }
    }

    private static func unseen(_ delta: AgentTranscriptDelta, after next: Int) -> AgentTranscriptDelta? {
        let units = delta.text.utf16
        guard delta.next > next, units.count == delta.next - delta.from,
            let text = String(units.dropFirst(next - delta.from))
        else { return nil }
        var trimmed = delta
        trimmed.text = text
        trimmed.from = next
        return trimmed
    }

    private func hold() {
        Self.holders.removeAll { $0.stream == nil }
        if !Self.holders.contains(where: { $0.stream === self }) { Self.holders.append(Holder(stream: self)) }
    }

    private func release() -> Bool {
        Self.holders.removeAll { $0.stream == nil || $0.stream === self }
        return Self.holders.contains { holder in
            guard let other = holder.stream else { return false }
            return other.hub === hub && other.sessionId == sessionId
        }
    }

    private func end(_ reason: String) {
        isStreaming = false
        onEnded(reason)
        stop()
    }

    private func linkChanged() {
        if stopped || awaiting { return }
        if let link = hub.currentLink, ObjectIdentifier(link) == followedLink { return }
        isStreaming = false
        if hub.currentLink != nil {
            Task { await self.follow() }
        } else {
            scheduleRetry()
        }
    }

    private func scheduleRetry() {
        if stopped || retry != nil { return }
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, 15)
        let sleep = self.sleep
        retry = Task { [weak self] in
            do { try await sleep(delay) } catch { return }
            guard let self, !self.stopped else { return }
            self.retry = nil
            await self.follow()
        }
    }
}
