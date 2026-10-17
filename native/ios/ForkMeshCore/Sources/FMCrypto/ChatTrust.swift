import FMBase
import Foundation

public enum ChatTrustState: String, Equatable, Sendable {
    case approved, pending, blocked
}

public enum ChatTrustPolicy: String, Equatable, Sendable {
    case open, strict
}

public struct ChatTrustRecord: Equatable, Sendable, Identifiable {
    public let kid: String
    public let idk: String
    public let acct: String
    public let node: String
    public internal(set) var spk: ChatPrekey
    public internal(set) var state: ChatTrustState
    public let firstSeen: Int64
    public internal(set) var verified: Bool
    public internal(set) var lastSeen: Int64 = 0

    public var id: String { kid }

    public var identityKey: [UInt8] { ChatV3.unb64u(idk, length: Ed25519.publicKeyBytes) ?? [] }

    var json: JSONObject {
        [
            "kid": .string(kid), "idk": .string(idk), "acct": .string(acct), "node": .string(node),
            "spk": .object(spk.json), "state": .string(state.rawValue), "firstSeen": .int(firstSeen),
            "verified": .bool(verified), "lastSeen": .int(lastSeen),
        ]
    }

    static func parse(_ value: JSONValue) -> ChatTrustRecord? {
        guard let object = value.objectValue, let kid = object["kid"]?.stringValue, ChatV3.isKeyId(kid),
            let idk = object["idk"]?.stringValue, let key = ChatV3.unb64u(idk, length: Ed25519.publicKeyBytes),
            ChatV3.id16(key) == kid, let acct = object["acct"]?.stringValue, ChatV3.isValidAcct(acct),
            let node = object["node"]?.stringValue, ChatV3.isValidAcct(node), let spk = ChatPrekey.parse(object["spk"]),
            let state = ChatTrustState(rawValue: object["state"]?.stringValue ?? "")
        else { return nil }
        return ChatTrustRecord(
            kid: kid, idk: idk, acct: acct, node: node, spk: spk, state: state,
            firstSeen: ChatV3.int(object["firstSeen"]) ?? 0, verified: object["verified"] == .bool(true),
            lastSeen: ChatV3.int(object["lastSeen"]) ?? 0)
    }
}

public struct ChatSafetyNumber: Equatable, Sendable {
    public static let iterations = 5199

    public let scope: String
    public let digits: String
    public let qr: String
    public let verified: Bool
    public let changed: Bool

    public var groups: [String] {
        let characters = Array(digits)
        return stride(from: 0, to: characters.count, by: 5).map { String(characters[$0..<min($0 + 5, characters.count)]) }
    }

    public static func compute(scope: String, identityKeys: [[UInt8]]) -> (digits: String, qr: String) {
        let joined = identityKeys.sorted { $0.lexicographicallyPrecedes($1) }.flatMap { $0 }
        var hash = Sha512.hash(Array("forkmesh-safety-v3\n\(scope)\n".utf8), joined)
        for _ in 0..<iterations { hash = Sha512.hash(hash, joined) }
        var digits = ""
        for start in stride(from: 0, to: 60, by: 5) {
            var value: UInt64 = 0
            for byte in hash[start..<(start + 5)] { value = (value << 8) | UInt64(byte) }
            let text = String(value % 100_000)
            digits += String(repeating: "0", count: 5 - text.count) + text
        }
        return (digits, "forkmesh-safety:3:\(scope):\(ChatV3.b64u(Array(hash[0..<32])))")
    }
}

public struct ChatBundleResult: Equatable, Sendable {
    public let kid: String
    public let acct: String
    public let node: String
    public let state: ChatTrustState
    public let isNew: Bool
    public let updated: Bool
}

public final class ChatTrustStore: @unchecked Sendable {
    public let scope: String
    private let store: SecureStore?
    private let lock = NSLock()
    private var records: [ChatTrustRecord] = []
    private var held: [String: [JSONObject]] = [:]
    private var lastRoom = ""
    private var lastSafety = ""
    private var lastSafetyAccts = 0
    private var safetyChanged = false
    private var changeCount = 0

    public static func storeKey(_ scope: String) -> String { "chat/e2ee/trustV3/\(ChatV3.storeSuffix(scope))" }

    init(scope: String, store: SecureStore?) {
        self.scope = scope
        self.store = store
        load()
    }

    public var revision: Int { locked { changeCount } }

    public var all: [ChatTrustRecord] { locked { records } }

    public func record(_ kid: String) -> ChatTrustRecord? { locked { records.first { $0.kid == kid } } }

    public func records(in state: ChatTrustState) -> [ChatTrustRecord] { locked { records.filter { $0.state == state } } }

    public var room: String {
        get { locked { lastRoom } }
        set {
            locked {
                if lastRoom == newValue { return }
                lastRoom = newValue
                persist()
            }
        }
    }

    func accept(
        kid: String, idk: String, acct: String, node: String, spk: ChatPrekey, policy: ChatTrustPolicy, ownKid: String,
        now: Int64
    ) -> ChatBundleResult? {
        locked {
            if let index = records.firstIndex(where: { $0.kid == kid }) {
                let known = records[index]
                if known.idk != idk || known.acct != acct || known.node != node { return nil }
                var updated = false
                records[index].lastSeen = max(known.lastSeen, now)
                if spk.ts > known.spk.ts {
                    records[index].spk = spk
                    updated = true
                    persist()
                }
                return ChatBundleResult(
                    kid: kid, acct: acct, node: node, state: known.state, isNew: false, updated: updated)
            }
            var state = ChatTrustState.approved
            if policy == .strict && kid != ownKid
                && records.contains(where: { $0.acct == acct && $0.state == .approved })
            {
                state = .pending
            }
            records.append(
                ChatTrustRecord(
                    kid: kid, idk: idk, acct: acct, node: node, spk: spk, state: state, firstSeen: now, verified: false,
                    lastSeen: now))
            if state == .approved { noteApprovedSetChanged() }
            changeCount += 1
            persist()
            return ChatBundleResult(kid: kid, acct: acct, node: node, state: state, isNew: true, updated: true)
        }
    }

    @discardableResult
    public func setState(_ kid: String, _ state: ChatTrustState) -> Bool {
        locked {
            guard let index = records.firstIndex(where: { $0.kid == kid }), records[index].state != state else {
                return false
            }
            let wasApproved = records[index].state == .approved
            records[index].state = state
            if state != .approved { records[index].verified = false }
            if state == .blocked {
                for room in Array(held.keys) {
                    held[room]?.removeAll { $0["kid"]?.stringValue == kid }
                }
            }
            if wasApproved || state == .approved { noteApprovedSetChanged() }
            changeCount += 1
            persist()
            return true
        }
    }

    @discardableResult
    public func approve(_ kid: String) -> Bool { setState(kid, .approved) }

    @discardableResult
    public func block(_ kid: String) -> Bool { setState(kid, .blocked) }

    public func markVerified() {
        locked {
            for index in records.indices where records[index].state == .approved { records[index].verified = true }
            safetyChanged = false
            changeCount += 1
            persist()
        }
    }

    public func acknowledgeSafetyChange() {
        locked {
            if !safetyChanged { return }
            safetyChanged = false
            changeCount += 1
            persist()
        }
    }

    public func verify(scanned: String) -> Bool {
        let current = safetyNumber()
        guard !scanned.isEmpty, scanned.trimmingCharacters(in: .whitespacesAndNewlines) == current.qr else { return false }
        markVerified()
        return true
    }

    public func safetyNumber() -> ChatSafetyNumber {
        locked {
            let approved = records.filter { $0.state == .approved }
            let computed = ChatSafetyNumber.compute(scope: scope, identityKeys: approved.map(\.identityKey))
            return ChatSafetyNumber(
                scope: scope, digits: computed.digits, qr: computed.qr,
                verified: !approved.isEmpty && approved.allSatisfy(\.verified), changed: safetyChanged)
        }
    }

    public var safetyNumberChanged: Bool { locked { safetyChanged } }

    private func noteApprovedSetChanged() {
        let approved = records.filter { $0.state == .approved }
        let qr = ChatSafetyNumber.compute(scope: scope, identityKeys: approved.map(\.identityKey)).qr
        if qr == lastSafety { return }
        if lastSafetyAccts >= 2 {
            safetyChanged = true
            for index in records.indices { records[index].verified = false }
        }
        lastSafety = qr
        lastSafetyAccts = Set(approved.map(\.acct)).count
    }

    func hold(_ room: String, _ frame: JSONObject) {
        locked {
            let signature = frame["sig"]?.stringValue ?? ""
            var list = held[room] ?? []
            if list.contains(where: { $0["sig"]?.stringValue == signature }) { return }
            list.append(frame)
            if list.count > ChatV3.maxHeldPerRoom { list.removeFirst(list.count - ChatV3.maxHeldPerRoom) }
            held[room] = list
            changeCount += 1
            persist()
        }
    }

    func takeHeld(_ room: String) -> [JSONObject] {
        locked {
            guard let list = held.removeValue(forKey: room), !list.isEmpty else { return [] }
            persist()
            return list
        }
    }

    public func heldCount(_ room: String) -> Int { locked { held[room]?.count ?? 0 } }

    func noteSeen(_ kid: String, _ now: Int64) {
        locked {
            guard let index = records.firstIndex(where: { $0.kid == kid }), records[index].lastSeen < now else { return }
            records[index].lastSeen = now
        }
    }

    public func heldKeyIds(_ room: String) -> [String] {
        locked { (held[room] ?? []).compactMap { $0["kid"]?.stringValue } }
    }

    private func load() {
        guard let store, let text = store.read(ChatTrustStore.storeKey(scope)),
            let root = (try? JSON.parse(text))?.objectValue
        else { return }
        records = (root["records"]?.arrayValue ?? []).compactMap(ChatTrustRecord.parse)
        for (room, value) in root["held"]?.objectValue ?? JSONObject() {
            let frames = (value.arrayValue ?? []).compactMap(\.objectValue)
            if !frames.isEmpty { held[room] = Array(frames.suffix(ChatV3.maxHeldPerRoom)) }
        }
        lastRoom = root["room"]?.stringValue ?? ""
        lastSafety = root["safety"]?.stringValue ?? ""
        lastSafetyAccts = Int(ChatV3.int(root["safetyAccts"]) ?? 0)
        safetyChanged = root["changed"] == .bool(true)
    }

    private func persist() {
        guard let store else { return }
        var heldJSON = JSONObject()
        var budget = ChatV3.maxHeldBytes
        for room in held.keys.sorted() {
            var kept: [JSONValue] = []
            for frame in (held[room] ?? []).reversed() {
                let size = JSON.encode(frame).utf8.count
                if size > budget { continue }
                budget -= size
                kept.insert(.object(frame), at: 0)
            }
            if !kept.isEmpty { heldJSON[room] = .array(kept) }
        }
        let root: JSONObject = [
            "v": .int(ChatV3.version), "scope": .string(scope), "room": .string(lastRoom),
            "records": .array(records.map { .object($0.json) }), "held": .object(heldJSON),
            "safety": .string(lastSafety), "safetyAccts": .int(Int64(lastSafetyAccts)), "changed": .bool(safetyChanged),
        ]
        store.write(ChatTrustStore.storeKey(scope), JSON.encode(root))
    }

    private func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}
