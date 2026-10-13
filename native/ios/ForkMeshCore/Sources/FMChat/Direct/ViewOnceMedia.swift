import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

public enum ViewOnceKind: String, Sendable, Equatable, CaseIterable {
    case video
    case voice

    public var maxBytes: Int { self == .video ? 12 * 1024 * 1024 : 4 * 1024 * 1024 }

    public var maxDurationMs: Int64 { self == .video ? 60_000 : 120_000 }

    public var serverMaxDurationMs: Int64 { self == .video ? 61_000 : 121_000 }

    public var fallbackText: String { self == .video ? "🎥 Video message" : "🎤 Voice message" }

    public var label: String { self == .video ? "Video message" : "Voice message" }

    public var mimePrefix: String { self == .video ? "video/" : "audio/" }

    public var defaultMime: String { self == .video ? "video/mp4" : "audio/mp4" }
}

public enum ViewOnceState: String, Sendable, Equatable {
    case sending
    case uploading
    case ready
    case opened
    case expired
    case withdrawn
    case failed

    public var isTerminal: Bool { self == .opened || self == .expired || self == .withdrawn || self == .failed }

    public static func server(_ value: String) -> ViewOnceState? {
        switch value {
        case "uploading": return .uploading
        case "ready": return .ready
        case "opened": return .opened
        case "expired": return .expired
        case "withdrawn": return .withdrawn
        default: return nil
        }
    }
}

public struct ViewOnceMedia: Equatable, Sendable {
    public let kind: ViewOnceKind
    public let media: String
    public let key: [UInt8]
    public let iv: [UInt8]
    public let mime: String
    public let durationMs: Int64
    public let size: Int
    public let parts: Int

    public init(
        kind: ViewOnceKind, media: String, key: [UInt8], iv: [UInt8], mime: String, durationMs: Int64, size: Int,
        parts: Int
    ) {
        self.kind = kind
        self.media = media
        self.key = key
        self.iv = iv
        self.mime = mime
        self.durationMs = durationMs
        self.size = size
        self.parts = parts
    }
}

public struct ViewOnceStatus: Equatable, Sendable {
    public let id: String
    public let state: ViewOnceState
    public let role: String
    public let sentAt: Int64
    public let openedAt: Int64
    public let expiresAt: Int64

    public init(id: String, state: ViewOnceState, role: String, sentAt: Int64 = 0, openedAt: Int64 = 0, expiresAt: Int64 = 0) {
        self.id = id
        self.state = state
        self.role = role
        self.sentAt = sentAt
        self.openedAt = openedAt
        self.expiresAt = expiresAt
    }
}

public enum ViewOnceError: Error, Equatable, CustomStringConvertible {
    case tooLarge
    case tooLong
    case empty
    case gone(ViewOnceState)
    case refused(String)
    case corrupt
    case unavailable

    public var description: String {
        switch self {
        case .tooLarge: return "That recording is too large to send."
        case .tooLong: return "That recording is too long to send."
        case .empty: return "Nothing was recorded."
        case .gone(let state):
            switch state {
            case .opened: return "This message was already opened."
            case .withdrawn: return "The sender withdrew this message."
            default: return "This message has expired."
            }
        case .refused(let code):
            switch code {
            case "media_quota_count": return "Too many of your view-once messages are still unopened. Try again later."
            case "media_quota_bytes": return "Your unopened view-once messages use all the space allowed. Try again later."
            case "media_too_large": return "That recording is too large to send."
            case "media_too_long": return "That recording is too long to send."
            case "invalid_session": return "Sign in again to send messages."
            default: return code.isEmpty ? "The message could not be sent." : code.replacingOccurrences(of: "_", with: " ")
            }
        case .corrupt: return "This message could not be decrypted."
        case .unavailable: return "The message could not be reached. Try again."
        }
    }
}

public enum ViewOnceFrames {
    public static let field = "viewOnce"
    public static let version = 1
    public static let partBytes = 524_288
    public static let tagBytes = 16
    public static let keyBytes = 32
    public static let ivBytes = 12
    public static let relayKind = "forkmesh-media"
    public static let statusIdsMax = 50
    public static let maxMimeChars = 64

    public static func validMediaId(_ value: String) -> Bool {
        value.utf8.count == 32 && value.utf8.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) }
    }

    public static func parts(size: Int, partBytes: Int = partBytes) -> Int {
        size <= 0 ? 0 : (size + partBytes - 1) / partBytes
    }

    public static func decodeKeyBytes(_ text: String) -> [UInt8]? {
        guard !text.isEmpty, let data = try? B64.decodeUnpadded(text) else { return nil }
        return Array(data)
    }

    public static func parse(_ value: JSONValue?) -> ViewOnceMedia? {
        guard let object = value?.objectValue else { return nil }
        guard object["v"]?.intValue == version else { return nil }
        guard let kind = ViewOnceKind(rawValue: object["kind"]?.text ?? "") else { return nil }
        let media = object["media"]?.text ?? ""
        guard validMediaId(media) else { return nil }
        guard let key = decodeKeyBytes(object["key"]?.text ?? ""), key.count == keyBytes,
            let iv = decodeKeyBytes(object["iv"]?.text ?? ""), iv.count == ivBytes
        else { return nil }
        let mime = (object["mime"]?.text ?? "").lowercased()
        guard !mime.isEmpty, mime.count <= maxMimeChars, mime.hasPrefix(kind.mimePrefix) else { return nil }
        let durationMs = object["durationMs"]?.int64Value ?? -1
        guard durationMs >= 0, durationMs <= kind.serverMaxDurationMs else { return nil }
        let size = object["size"]?.intValue ?? 0
        guard size > tagBytes, size <= kind.maxBytes + 1024 else { return nil }
        let parts = object["parts"]?.intValue ?? 0
        guard parts == self.parts(size: size) else { return nil }
        return ViewOnceMedia(
            kind: kind, media: media, key: key, iv: iv, mime: mime, durationMs: durationMs, size: size, parts: parts)
    }

    public static func build(_ media: ViewOnceMedia) -> JSONValue {
        .object([
            "v": .int(Int64(version)),
            "kind": .string(media.kind.rawValue),
            "media": .string(media.media),
            "key": .string(B64.encodeUrlNoPadding(media.key)),
            "iv": .string(B64.encodeUrlNoPadding(media.iv)),
            "mime": .string(media.mime),
            "durationMs": .int(media.durationMs),
            "size": .int(Int64(media.size)),
            "parts": .int(Int64(media.parts)),
        ])
    }

    public static func relayUpdate(_ envelope: JSONObject) -> (id: String, state: ViewOnceState)? {
        guard envelope["kind"]?.text == relayKind, envelope["v"]?.intValue == 1 else { return nil }
        let id = envelope["id"]?.text ?? ""
        guard validMediaId(id), let state = ViewOnceState.server(envelope["state"]?.text ?? ""),
            state == .opened || state == .expired || state == .withdrawn
        else { return nil }
        return (id, state)
    }

    public static func status(_ value: JSONObject) -> ViewOnceStatus? {
        let id = value["id"]?.text ?? ""
        guard validMediaId(id), let state = ViewOnceState.server(value["state"]?.text ?? "") else { return nil }
        return ViewOnceStatus(
            id: id, state: state, role: value["role"]?.text ?? "", sentAt: value["sentAt"]?.int64Value ?? 0,
            openedAt: value["openedAt"]?.int64Value ?? 0, expiresAt: value["expiresAt"]?.int64Value ?? 0)
    }

    public static func isPlayable(_ mime: String) -> Bool {
        let base = mime.split(separator: ";").first.map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
        return ["video/mp4", "video/quicktime", "video/x-m4v", "audio/mp4", "audio/m4a", "audio/x-m4a", "audio/aac",
                "audio/mpeg"].contains(base)
    }

    public static func durationLabel(_ durationMs: Int64) -> String {
        let total = Int((Double(max(0, durationMs)) / 1000).rounded())
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

public struct ViewOnceSealed: Equatable, Sendable {
    public let key: [UInt8]
    public let iv: [UInt8]
    public let body: [UInt8]
}

public enum ViewOnceCrypto {
    public static func seal(_ plain: [UInt8], key: [UInt8]? = nil, iv: [UInt8]? = nil) throws -> ViewOnceSealed {
        let key = key ?? CryptoBytes.random(ViewOnceFrames.keyBytes)
        let iv = iv ?? CryptoBytes.random(ViewOnceFrames.ivBytes)
        let box = try AesGcm.encrypt(plain, key: key, nonce: iv)
        return ViewOnceSealed(key: key, iv: iv, body: box.cipherText + box.mac)
    }

    public static func open(_ body: [UInt8], key: [UInt8], iv: [UInt8]) throws -> [UInt8] {
        guard body.count >= ViewOnceFrames.tagBytes, key.count == ViewOnceFrames.keyBytes,
            iv.count == ViewOnceFrames.ivBytes
        else { throw ViewOnceError.corrupt }
        let split = body.count - ViewOnceFrames.tagBytes
        do {
            return try AesGcm.decrypt(
                cipherText: Array(body[0..<split]), nonce: iv, mac: Array(body[split...]), key: key)
        } catch {
            throw ViewOnceError.corrupt
        }
    }

    public static func split(_ body: [UInt8], partBytes: Int = ViewOnceFrames.partBytes) -> [[UInt8]] {
        guard partBytes > 0 else { return [] }
        return stride(from: 0, to: body.count, by: partBytes).map { Array(body[$0..<min($0 + partBytes, body.count)]) }
    }
}

public struct ViewOnceTransfer {
    public let api: ChatAccountCalling
    public let conversationId: String
    public let partBytes: Int

    public init(api: ChatAccountCalling, conversationId: String, partBytes: Int = ViewOnceFrames.partBytes) {
        self.api = api
        self.conversationId = conversationId
        self.partBytes = partBytes
    }

    var base: String { "\(DirectMessageFrames.endpoint)/\(conversationId)/media" }

    @MainActor
    public func upload(
        kind: ViewOnceKind, body: [UInt8], durationMs: Int64, progress: (@MainActor (Double) -> Void)? = nil
    ) async throws -> String {
        let parts = ViewOnceCrypto.split(body, partBytes: partBytes)
        guard !parts.isEmpty else { throw ViewOnceError.empty }
        let created = try await call(
            "POST", base,
            ["kind": .string(kind.rawValue), "size": .int(Int64(body.count)), "parts": .int(Int64(parts.count)),
             "durationMs": .int(durationMs)])
        let media = created["media"]?.objectValue?["id"]?.text ?? ""
        guard ViewOnceFrames.validMediaId(media) else { throw ViewOnceError.unavailable }
        do {
            for (index, part) in parts.enumerated() {
                _ = try await call("PUT", "\(base)/\(media)/parts/\(index)", ["data": .string(B64.encode(part))])
                progress?(Double(index + 1) / Double(parts.count))
            }
            _ = try await call("POST", "\(base)/\(media)/finish", nil)
        } catch {
            _ = try? await call("DELETE", "\(base)/\(media)", nil)
            throw error
        }
        return media
    }

    @MainActor
    public func download(_ media: ViewOnceMedia, progress: (@MainActor (Double) -> Void)? = nil) async throws -> [UInt8] {
        var body: [UInt8] = []
        body.reserveCapacity(media.size)
        for index in 0..<media.parts {
            let reply = try await call("GET", "\(base)/\(media.media)/parts/\(index)", nil)
            guard reply["index"]?.intValue == index, reply["parts"]?.intValue == media.parts,
                let data = B64.decodeOrNil(reply["data"]?.text ?? "")
            else { throw ViewOnceError.corrupt }
            body.append(contentsOf: data)
            if body.count > media.size { throw ViewOnceError.corrupt }
            progress?(Double(index + 1) / Double(media.parts))
        }
        guard body.count == media.size else { throw ViewOnceError.corrupt }
        return try ViewOnceCrypto.open(body, key: media.key, iv: media.iv)
    }

    @MainActor
    public func consume(_ media: String) async throws -> ViewOnceState {
        let reply = try await call("POST", "\(base)/\(media)/consume", nil)
        return ViewOnceState.server(reply["media"]?.objectValue?["state"]?.text ?? "") ?? .opened
    }

    @MainActor
    public func withdraw(_ media: String) async throws -> ViewOnceState {
        let reply = try await call("DELETE", "\(base)/\(media)", nil)
        return ViewOnceState.server(reply["media"]?.objectValue?["state"]?.text ?? "") ?? .withdrawn
    }

    @MainActor
    public func status(_ media: String) async throws -> ViewOnceStatus? {
        let reply = try await call("GET", "\(base)/\(media)", nil)
        return (reply["media"]?.objectValue).flatMap(ViewOnceFrames.status)
    }

    @MainActor
    public func statuses(_ ids: [String]) async throws -> [ViewOnceStatus] {
        var out: [ViewOnceStatus] = []
        let valid = ids.filter(ViewOnceFrames.validMediaId)
        for start in stride(from: 0, to: valid.count, by: ViewOnceFrames.statusIdsMax) {
            let chunk = valid[start..<min(start + ViewOnceFrames.statusIdsMax, valid.count)]
            let reply = try await call("GET", "\(base)?ids=\(chunk.joined(separator: ","))", nil)
            out.append(contentsOf: (reply["media"]?.objects ?? []).compactMap(ViewOnceFrames.status))
        }
        return out
    }

    @MainActor
    func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        do {
            return try await api.accountApiCall(method, path, body)
        } catch let error as OrgApiException {
            if error.code == "media_gone" { throw ViewOnceError.gone(.expired) }
            if error.code.hasPrefix("http_0") { throw ViewOnceError.unavailable }
            throw ViewOnceError.refused(error.code)
        } catch let error as ViewOnceError {
            throw error
        } catch {
            throw ViewOnceError.unavailable
        }
    }
}

public struct ViewOnceEntry: Equatable, Sendable, Identifiable {
    public let messageId: String
    public let conversation: String
    public let mediaId: String
    public let kind: ViewOnceKind
    public let mime: String
    public let durationMs: Int64
    public let mine: Bool
    public var state: ViewOnceState
    public var material: ViewOnceMedia?

    public var id: String { messageId }

    public var canPlay: Bool { !mine && state == .ready && material != nil }

    public var stateLabel: String {
        switch state {
        case .sending, .uploading: return "Sending"
        case .ready: return mine ? "Sent" : "Tap to play once"
        case .opened: return "Opened"
        case .expired: return "Expired"
        case .withdrawn: return "Withdrawn"
        case .failed: return "Not sent"
        }
    }

    public var accessibilityLabel: String {
        "\(kind.label), \(ViewOnceFrames.durationLabel(durationMs)), \(stateLabel)"
    }
}

@MainActor
@Observable
public final class ViewOnceStore {
    public static let spentKey = "chat/viewOnce/spent"
    public static let spentLimit = 512
    public private(set) var entries: [String: ViewOnceEntry] = [:]
    public private(set) var busy: Set<String> = []
    @ObservationIgnored private var byMedia: [String: String] = [:]
    @ObservationIgnored private var spent: [String: ViewOnceState] = [:]
    @ObservationIgnored private var spentOrder: [String] = []
    @ObservationIgnored public var pendingConsume: Set<String> = []
    @ObservationIgnored let prefs: KeyValueStore?

    public init(prefs: KeyValueStore? = nil) {
        self.prefs = prefs
        for item in prefs?.stringList(forKey: ViewOnceStore.spentKey) ?? [] {
            let pieces = item.split(separator: ":")
            guard pieces.count == 2, ViewOnceFrames.validMediaId(String(pieces[0])),
                let state = ViewOnceState(rawValue: String(pieces[1])), state.isTerminal
            else { continue }
            spent[String(pieces[0])] = state
            spentOrder.append(String(pieces[0]))
        }
    }

    public func entry(_ messageId: String) -> ViewOnceEntry? { entries[messageId] }

    public func entry(media: String) -> ViewOnceEntry? { byMedia[media].flatMap { entries[$0] } }

    public func isBusy(_ messageId: String) -> Bool { busy.contains(messageId) }

    func setBusy(_ messageId: String, _ on: Bool) {
        if on { busy.insert(messageId) } else { busy.remove(messageId) }
    }

    @discardableResult
    public func register(
        messageId: String, conversation: String, media: ViewOnceMedia, mine: Bool, state: ViewOnceState = .ready
    ) -> ViewOnceEntry {
        if let existing = entries[messageId] { return existing }
        let known = spent[media.media]
        let resolved = known ?? state
        let entry = ViewOnceEntry(
            messageId: messageId, conversation: conversation, mediaId: media.media, kind: media.kind, mime: media.mime,
            durationMs: media.durationMs, mine: mine, state: resolved,
            material: mine || resolved.isTerminal ? nil : media)
        entries[messageId] = entry
        byMedia[media.media] = messageId
        return entry
    }

    @discardableResult
    public func apply(media: String, state: ViewOnceState) -> Bool {
        guard let messageId = byMedia[media], var entry = entries[messageId] else {
            if state.isTerminal { remember(media, state) }
            return false
        }
        if entry.state.isTerminal && entry.state != .failed && !state.isTerminal { return false }
        if entry.state == state && (entry.material == nil || !state.isTerminal) { return false }
        entry.state = state
        if state.isTerminal || entry.mine { entry.material = nil }
        entries[messageId] = entry
        if state.isTerminal { remember(media, state) }
        return true
    }

    public func apply(_ statuses: [ViewOnceStatus]) {
        for status in statuses { apply(media: status.id, state: status.state) }
    }

    public func unresolved(_ conversation: String) -> [String] {
        entries.values.filter { $0.conversation == conversation && !$0.state.isTerminal && $0.state != .sending }
            .map(\.mediaId).sorted()
    }

    public func isSpent(_ media: String) -> Bool { spent[media] != nil }

    public func forget(conversation: String) {
        for (id, entry) in entries where entry.conversation == conversation {
            entries.removeValue(forKey: id)
            byMedia.removeValue(forKey: entry.mediaId)
        }
    }

    public func clear() {
        entries = [:]
        byMedia = [:]
        busy = []
        pendingConsume = []
    }

    private func remember(_ media: String, _ state: ViewOnceState) {
        if spent[media] == nil { spentOrder.append(media) }
        spent[media] = state
        if spentOrder.count > ViewOnceStore.spentLimit {
            for old in spentOrder.prefix(spentOrder.count - ViewOnceStore.spentLimit) { spent.removeValue(forKey: old) }
            spentOrder.removeFirst(spentOrder.count - ViewOnceStore.spentLimit)
        }
        prefs?.set(spentOrder.compactMap { id in spent[id].map { "\(id):\($0.rawValue)" } }, forKey: ViewOnceStore.spentKey)
    }
}

public struct ViewOnceClip: Sendable {
    public let messageId: String
    public let kind: ViewOnceKind
    public let mime: String
    public let durationMs: Int64
    public let data: Data
}
