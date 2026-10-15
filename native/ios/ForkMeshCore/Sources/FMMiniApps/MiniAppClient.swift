import FMBase
import FMControl
import Foundation

@MainActor
public final class MiniAppClient {
    public enum Outcome: Equatable, Sendable {
        case done(JSONObject)
        case queued(String)
    }

    private let link: () -> DesktopControlLink?
    private let now: () -> Date
    public let outbox: MiniAppOutbox
    public let snapshots: MiniAppSnapshots
    private var taps: [UUID: UUID] = [:]

    public init(store: KeyValueStore, link: @escaping () -> DesktopControlLink?, now: @escaping () -> Date = Date.init) {
        self.link = link
        self.now = now
        outbox = MiniAppOutbox(store: store)
        snapshots = MiniAppSnapshots(store: store)
    }

    private var nowMs: Int64 { Int64(now().timeIntervalSince1970 * 1000) }

    public var reach: MiniAppReach {
        guard let link = link() else { return .unpaired }
        return link.isOpen ? .live(transport: link.transport.rawValue) : .desktopAway(lastSeenMs: nil)
    }

    private func openLink() throws -> DesktopControlLink {
        guard let link = link(), link.isOpen else { throw MiniAppError("desktop_unavailable") }
        return link
    }

    public func catalog() async throws -> [MiniAppDescriptor] {
        MiniAppDescriptor.catalog(try await openLink().request(MiniAppContract.listMethod, [:]))
    }

    public func card(for app: MiniAppDescriptor) async throws -> MiniAppCard {
        guard app.hasCard else { throw MiniAppError("unknown_method") }
        let result = try await openLink().request(MiniAppContract.method(app.id, MiniAppContract.cardMethod), [:])
        let card = MiniAppCard(result, for: app)
        snapshots.save(card, for: app.id, atMs: nowMs)
        return card
    }

    public func cachedCard(for app: MiniAppDescriptor) -> (card: MiniAppCard, fetchedMs: Int64)? {
        snapshots.load(app)
    }

    public func perform(_ action: MiniAppCard.Action, on app: MiniAppDescriptor, queueWhenAway: Bool = true) async throws -> Outcome {
        guard app.allows(action.method) else { throw MiniAppError("unknown_method") }
        if let link = link(), link.isOpen {
            return .done(try await link.request(MiniAppContract.method(app.id, action.method), action.params))
        }
        guard queueWhenAway else { throw MiniAppError("desktop_unavailable") }
        let intent = MiniAppIntent(plugin: app.id, method: action.method, params: action.params, createdMs: nowMs)
        guard outbox.enqueue(intent) else { throw MiniAppError("outbox_full") }
        return .queued(intent.id)
    }

    public func flushOutbox() async -> MiniAppFlushReport {
        guard let link = link(), link.isOpen else { return MiniAppFlushReport(kept: outbox.pending.map(\.id)) }
        return await outbox.flush(nowMs: nowMs) { method, params in
            try await link.request(method, params)
        }
    }

    @discardableResult
    public func observe(_ app: MiniAppDescriptor, _ handler: @escaping @MainActor (String, JSONObject) -> Void) -> UUID? {
        guard let link = link() else { return nil }
        let token = UUID()
        taps[token] = link.addEventTap { event, data in
            guard let parsed = MiniAppContract.split(event: event), parsed.plugin == app.id,
                  app.events.contains(parsed.name) else { return }
            handler(parsed.name, data)
        }
        return token
    }

    public func stopObserving(_ token: UUID) {
        guard let tap = taps.removeValue(forKey: token) else { return }
        link()?.removeEventTap(tap)
    }
}
