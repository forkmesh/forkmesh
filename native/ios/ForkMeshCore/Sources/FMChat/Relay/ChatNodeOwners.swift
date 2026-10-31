import FMBase
import Foundation

@MainActor
public final class ChatNodeOwners {
    nonisolated public static let ttl: TimeInterval = 60 * 60
    nonisolated public static let path = "/api/chat/node-owner"
    nonisolated public static let retryAfter: TimeInterval = 60
    nonisolated static let maxEntries = 2_000

    let lookup: @MainActor (String) async throws -> String
    let clock: Clock
    private var answers: [String: (account: String, at: Date)] = [:]
    private var pending: [String: Task<String?, Never>] = [:]
    private var failures: [String: Date] = [:]
    public var onAnswer: (@MainActor (String, String) -> Void)?
    private var answerObservers: [@MainActor (String, String) -> Void] = []

    public init(lookup: @escaping @MainActor (String) async throws -> String, clock: Clock = SystemClock()) {
        self.lookup = lookup
        self.clock = clock
    }

    public convenience init(api: ChatAccountCalling, clock: Clock = SystemClock()) {
        self.init(
            lookup: { node in
                let encoded = node.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? node
                let answer = try await api.accountApiCall("GET", "\(ChatNodeOwners.path)?node=\(encoded)", nil)
                return answer["account"]?.stringValue ?? ""
            }, clock: clock)
    }

    public func cached(_ node: String) -> String? {
        if let failed = failures[node] {
            if clock.now.timeIntervalSince(failed) < ChatNodeOwners.retryAfter { return "" }
            failures.removeValue(forKey: node)
        }
        guard let answer = answers[node] else { return nil }
        let life = answer.account.isEmpty ? ChatNodeOwners.retryAfter : ChatNodeOwners.ttl
        if clock.now.timeIntervalSince(answer.at) >= life {
            answers.removeValue(forKey: node)
            return nil
        }
        return answer.account
    }

    public func isVerified(node: String, claim: String) -> Bool {
        let wanted = ChatNodeOwners.normalized(claim)
        guard !wanted.isEmpty, let account = cached(node) else { return false }
        return account == wanted
    }

    public func account(for node: String) async -> String? {
        if let failed = failures[node], clock.now.timeIntervalSince(failed) < ChatNodeOwners.retryAfter { return nil }
        if let known = cached(node) { return known }
        if let running = pending[node] { return await running.value }
        let task = Task { @MainActor [lookup] () -> String? in
            guard let account = try? await lookup(node) else { return nil }
            return ChatNodeOwners.normalized(account)
        }
        pending[node] = task
        let account = await task.value
        pending.removeValue(forKey: node)
        if let account {
            if answers.count >= ChatNodeOwners.maxEntries { answers.removeAll() }
            answers[node] = (account, clock.now)
            failures.removeValue(forKey: node)
            onAnswer?(node, account)
            for observer in answerObservers { observer(node, account) }
        } else {
            if failures.count >= ChatNodeOwners.maxEntries { failures.removeAll() }
            failures[node] = clock.now
        }
        return account
    }

    public func addAnswerObserver(_ observer: @escaping @MainActor (String, String) -> Void) {
        answerObservers.append(observer)
    }

    public func clear() {
        answers.removeAll()
        failures.removeAll()
    }

    nonisolated public static func normalized(_ account: String) -> String {
        account.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }
}
