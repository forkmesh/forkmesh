import FMBase
import FMChat
import Foundation

@MainActor
public final class DesktopTranscriptStream {
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
        retry?.cancel()
        retry = nil
        if let tapId { hub.removeEventTap(tapId) }
        if let linkId { hub.removeLinkListener(linkId) }
        tapId = nil
        linkId = nil
        if let link = hub.currentLink {
            let id = sessionId
            Task { _ = try? await link.request("agents.unfollow", ["id": .int(Int64(id))], timeout: 5) }
        }
    }

    private func follow() async -> Bool {
        generation += 1
        let attempt = generation
        awaiting = true
        early = []
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

    private func apply(_ data: JSONObject) {
        var delta = AgentTranscriptDelta.fromDesktop(data, since: next, sessionId: sessionId)
        if !delta.reset && delta.from < next { return }
        if !delta.reset && delta.from > next { delta.reset = true }
        next = delta.next
        onDelta(delta)
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
