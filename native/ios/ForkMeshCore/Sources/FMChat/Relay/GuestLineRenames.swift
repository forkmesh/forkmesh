import FMBase
import Foundation

@MainActor
public final class GuestLineRenames {
    nonisolated public static let maxNodes = 256
    nonisolated public static let maxLinesPerNode = 256
    nonisolated public static let maxRechecks = 10

    public struct Line: Equatable, Sendable {
        public let id: String
        public let claim: String
        public let name: String
    }

    private var pending: [String: [Line]] = [:]
    private var order: [String] = []
    private var rechecking: Set<String> = []
    private var generation = 0
    private let sleep: @Sendable (TimeInterval) async throws -> Void

    public init(
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.sleep = sleep
    }

    public func note(_ frame: JSONObject, node: String, claim: String, owners: ChatNodeOwners?) {
        let type = frame["type"]?.text ?? ""
        guard type == "chat" || type == "thread-reply" else { return }
        let id = frame["id"]?.text ?? ""
        let wanted = ChatNodeOwners.normalized(claim)
        guard !node.isEmpty, !id.isEmpty, !wanted.isEmpty else { return }
        var lines = pending[node] ?? []
        if lines.contains(where: { $0.id == id }) { return }
        let sender = (frame["sender"]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let name = sender.isEmpty ? wanted : String(sender.prefix(ChatLimits.maxDisplayNameChars))
        lines.append(Line(id: id, claim: wanted, name: name))
        if lines.count > GuestLineRenames.maxLinesPerNode { lines.removeFirst(lines.count - GuestLineRenames.maxLinesPerNode) }
        if pending[node] == nil {
            order.append(node)
            while order.count > GuestLineRenames.maxNodes { pending.removeValue(forKey: order.removeFirst()) }
        }
        pending[node] = lines
        if let owners { recheck(node, owners) }
    }

    public func confirmed(node: String, account: String) -> [Line] {
        let wanted = ChatNodeOwners.normalized(account)
        guard !wanted.isEmpty, var lines = pending[node] else { return [] }
        let hits = lines.filter { $0.claim == wanted }
        if hits.isEmpty { return [] }
        lines.removeAll { $0.claim == wanted }
        if lines.isEmpty {
            pending.removeValue(forKey: node)
            order.removeAll { $0 == node }
        } else {
            pending[node] = lines
        }
        return hits
    }

    public func isPending(_ node: String) -> Bool { pending[node] != nil }

    public func clear() {
        pending.removeAll()
        order.removeAll()
        rechecking.removeAll()
        generation &+= 1
    }

    private func recheck(_ node: String, _ owners: ChatNodeOwners) {
        if rechecking.contains(node) { return }
        rechecking.insert(node)
        let started = generation
        let pause = sleep
        Task { [weak self, weak owners] in
            for _ in 0..<GuestLineRenames.maxRechecks {
                try? await pause(ChatNodeOwners.retryAfter + 1)
                guard let self, let owners, started == self.generation, self.isPending(node) else { break }
                _ = await owners.account(for: node)
            }
            if let self, started == self.generation { self.rechecking.remove(node) }
        }
    }
}
