import FMBase
import Foundation

public actor AgentAttachmentStore {
    public static let shared = AgentAttachmentStore()

    public let byteBudget: Int
    private var entries: [String: Data] = [:]
    private var order: [String] = []
    public private(set) var totalBytes = 0
    private var inflight: [String: Task<Data, Error>] = [:]
    private var previews: [String: AgentAttachmentPayload] = [:]
    private var inflightPayloads: [String: Task<AgentAttachmentPayload, Error>] = [:]

    public init(byteBudget: Int = 48 * 1024 * 1024) {
        self.byteBudget = max(0, byteBudget)
    }

    public func cached(_ key: String) -> Data? {
        guard let data = entries[key] else { return nil }
        touch(key)
        return data
    }

    public func contains(_ key: String) -> Bool { entries[key] != nil }

    public func put(_ key: String, _ data: Data) {
        remove(key)
        if data.isEmpty || data.count > byteBudget / 2 { return }
        entries[key] = data
        order.append(key)
        totalBytes += data.count
        while totalBytes > byteBudget, let oldest = order.first {
            remove(oldest)
        }
    }

    public func remove(_ key: String) {
        previews.removeValue(forKey: key)
        guard let data = entries.removeValue(forKey: key) else { return }
        totalBytes -= data.count
        order.removeAll { $0 == key }
    }

    public func removeAll() {
        entries.removeAll()
        previews.removeAll()
        order.removeAll()
        totalBytes = 0
    }

    public func data(for key: String, load: @escaping @Sendable () async throws -> Data) async throws -> Data {
        if let hit = cached(key) { return hit }
        if let running = inflight[key] { return try await running.value }
        let task = Task { try await load() }
        inflight[key] = task
        do {
            let data = try await task.value
            inflight[key] = nil
            put(key, data)
            return data
        } catch {
            inflight[key] = nil
            throw error
        }
    }

    public func cachedPreview(_ key: String) -> AgentAttachmentPayload? {
        guard let preview = previews[key], let data = cached(key) else { return nil }
        return AgentAttachmentPayload(
            data: data, preview: preview.preview, originalSize: preview.originalSize, width: preview.width,
            height: preview.height)
    }

    public func putPreview(_ key: String, _ payload: AgentAttachmentPayload) {
        put(key, payload.data)
        if entries[key] != nil { previews[key] = payload.withoutData }
    }

    public func payload(
        fullKey: String, previewKey: String, relayed: Bool,
        load: @escaping @Sendable () async throws -> AgentAttachmentPayload
    ) async throws -> AgentAttachmentPayload {
        if let hit = cached(fullKey) { return .full(hit) }
        if relayed, let hit = cachedPreview(previewKey) { return hit }
        let flight = relayed ? previewKey : fullKey
        if let running = inflightPayloads[flight] { return try await running.value }
        let task = Task { try await load() }
        inflightPayloads[flight] = task
        do {
            let payload = try await task.value
            inflightPayloads[flight] = nil
            if payload.preview {
                putPreview(previewKey, payload)
            } else {
                put(fullKey, payload.data)
                remove(previewKey)
            }
            return payload
        } catch {
            inflightPayloads[flight] = nil
            throw error
        }
    }

    private func touch(_ key: String) {
        guard let index = order.firstIndex(of: key), index != order.count - 1 else { return }
        order.remove(at: index)
        order.append(key)
    }
}

public struct AgentAttachmentLoader: Sendable {
    public let store: AgentAttachmentStore
    private let relay: @Sendable (AgentAttachmentRef) async throws -> Data
    private let desktopPayload: @Sendable (AgentAttachmentRef, Bool) async throws -> AgentAttachmentPayload
    private let desktopRelayed: @Sendable () async -> Bool

    public init(
        store: AgentAttachmentStore = .shared,
        relay: @escaping @Sendable (AgentAttachmentRef) async throws -> Data = { _ in
            throw AgentAttachmentError("This attachment cannot be opened here.")
        },
        desktop: @escaping @Sendable (AgentAttachmentRef) async throws -> Data = { _ in
            throw AgentAttachmentError("Connect to the desktop on this network to open this attachment.")
        }
    ) {
        self.store = store
        self.relay = relay
        self.desktopPayload = { ref, _ in .full(try await desktop(ref)) }
        self.desktopRelayed = { false }
    }

    public init(
        store: AgentAttachmentStore = .shared,
        relay: @escaping @Sendable (AgentAttachmentRef) async throws -> Data,
        desktopPayload: @escaping @Sendable (AgentAttachmentRef, Bool) async throws -> AgentAttachmentPayload,
        desktopRelayed: @escaping @Sendable () async -> Bool
    ) {
        self.store = store
        self.relay = relay
        self.desktopPayload = desktopPayload
        self.desktopRelayed = desktopRelayed
    }

    public func loadPayload(_ ref: AgentAttachmentRef) async throws -> AgentAttachmentPayload {
        guard case .desktop = ref.origin else { return .full(try await load(ref)) }
        let fetch = desktopPayload
        let relayed = await desktopRelayed()
        let isImage = ref.isImage
        return try await store.payload(fullKey: ref.cacheKey, previewKey: ref.previewCacheKey, relayed: relayed) {
            let payload = try await fetch(ref, relayed)
            if isImage || payload.preview { _ = try AgentAttachmentMedia.checkImage(payload.data) }
            return payload
        }
    }

    public func load(_ ref: AgentAttachmentRef) async throws -> Data {
        if case .desktop = ref.origin { return try await loadPayload(ref).data }
        let relay = self.relay
        return try await store.data(for: ref.cacheKey) {
            let data: Data
            switch ref.origin {
            case .relay: data = try await relay(ref)
            case .desktop: throw AgentAttachmentError("This attachment is on the desktop.")
            case .local: throw AgentAttachmentError("This photo is no longer on this phone.")
            case .elsewhere: throw AgentAttachmentError("This file is on the machine that ran the agent.")
            }
            if ref.isImage { _ = try AgentAttachmentMedia.checkImage(data) }
            return data
        }
    }

    public func seed(_ pairs: [(meta: AgentAttachmentMeta, photo: PromptAttachment)], origin: AgentAttachmentRef.Origin) async {
        for pair in pairs {
            await store.put(AgentAttachmentRef(meta: pair.meta, origin: origin).cacheKey, pair.photo.bytes)
        }
    }

    public func seedLocal(_ photos: [PromptAttachment]) async {
        for photo in photos {
            await store.put(AgentAttachmentRef.local(photo).cacheKey, photo.bytes)
        }
    }
}
