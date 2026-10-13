import FMAccount
import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class ChatRetentionService {
    public static let cacheKey = "chat/retention"

    @ObservationIgnored let api: ApiService
    @ObservationIgnored let store: KeyValueStore?
    @ObservationIgnored public let sessionToken: @MainActor () -> String
    @ObservationIgnored public let account: @MainActor () -> String
    @ObservationIgnored public let listeners = Listeners()

    public private(set) var retention: ChatRetention = .standard
    public private(set) var known = false
    public private(set) var busy = false
    public private(set) var revision = 0
    @ObservationIgnored private var token = ""

    public init(
        api: ApiService, store: KeyValueStore? = nil, sessionToken: @escaping @MainActor () -> String,
        account: @escaping @MainActor () -> String = { "" }
    ) {
        self.api = api
        self.store = store
        self.sessionToken = sessionToken
        self.account = account
        if !sessionToken().isEmpty, let cached = cached(for: account()) { retention = cached }
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    public var signedIn: Bool { !sessionToken().isEmpty }

    public func syncSession() async {
        let current = sessionToken()
        if current == token { return }
        token = current
        if current.isEmpty {
            known = false
            adopt(.standard, persist: false)
            return
        }
        adopt(cached(for: account()) ?? .standard, persist: false)
        do {
            try await refresh()
        } catch {
            Log.warning("chat-retention", "Chat retention read failed: \(error)")
        }
    }

    public func refresh() async throws {
        if !signedIn { return }
        let current = sessionToken()
        let next = try await api.updateChatRetention()
        if current != sessionToken() { return }
        known = true
        adopt(next)
    }

    public func adoptAccountPayload(_ json: JSONValue?) {
        guard signedIn, json?.objectValue != nil else { return }
        known = true
        adopt(ChatRetention(json: json))
    }

    public func setDefault(_ ms: Int64?) async throws {
        let value: JSONValue = ms.map { .int(ChatRetention.clamp($0)) } ?? .null
        try await write(retention.settingDefault(ms), ["defaultMs": value])
    }

    public func setChat(_ key: String, _ ms: Int64?) async throws {
        if key.isEmpty { return }
        let value: JSONValue = ms.map { .int(ChatRetention.clamp($0)) } ?? .null
        try await write(retention.settingChat(key, ms), ["chat": .string(key), "retentionMs": value])
    }

    private func write(_ optimistic: ChatRetention, _ changes: JSONObject) async throws {
        let before = retention
        let current = sessionToken()
        adopt(optimistic, persist: false)
        busy = true
        notify()
        defer {
            busy = false
            notify()
        }
        do {
            let next = try await api.updateChatRetention(changes)
            if current != sessionToken() { return }
            known = true
            adopt(next)
        } catch {
            if current == sessionToken() { adopt(before, persist: false) }
            throw error
        }
    }

    private func adopt(_ next: ChatRetention, persist: Bool = true) {
        if persist { save(next) }
        if next == retention { return }
        retention = next
        notify()
    }

    private func notify() {
        revision &+= 1
        listeners.notify()
    }

    private func cached(for account: String) -> ChatRetention? {
        guard let store, let text = store.string(forKey: ChatRetentionService.cacheKey) else { return nil }
        let root = JSON.parseObject(text)
        let owner = root.str("account")
        if owner.isEmpty || owner.lowercased() != account.lowercased() { return nil }
        return root["retention"]?.objectValue == nil ? nil : ChatRetention(json: root["retention"])
    }

    private func save(_ value: ChatRetention) {
        let owner = account()
        guard let store, !owner.isEmpty else { return }
        let root: JSONObject = ["account": .string(owner.lowercased()), "retention": .object(value.toJSON())]
        store.set(JSON.encode(root), forKey: ChatRetentionService.cacheKey)
    }
}
