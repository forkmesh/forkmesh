import FMBase
import Foundation

public enum ChatV3 {
    public static let version: Int64 = 3
    public static let dayMs: Int64 = 24 * 60 * 60 * 1000
    public static let prekeyLifeMs: Int64 = 8 * dayMs
    public static let maxBundleLifeMs: Int64 = 9 * dayMs
    public static let rotateAfterMs: Int64 = dayMs
    public static let maxPrekeys = 10
    public static let maxRecipients = 64
    public static let replayWindow: Int64 = 1024
    public static let maxHeldPerRoom = 64
    public static let maxHeldBytes = 1024 * 1024
    public static let maxAcctChars = 128
    public static let maxReplaySenders = 256
    public static let keyIdChars = 16
    public static let nonceBytes = 12
    public static let tagBytes = 16
    public static let signatureBytes = 64

    public static func b64u(_ bytes: [UInt8]) -> String { B64.encodeUrlNoPadding(bytes) }

    public static func b64(_ bytes: [UInt8]) -> String { B64.encode(bytes) }

    public static func unb64u(_ text: String, length: Int? = nil) -> [UInt8]? {
        guard CryptoText.isBase64UrlText(text), let data = try? B64.decodeUnpadded(text) else { return nil }
        let bytes = Array(data)
        if let length, bytes.count != length { return nil }
        return bytes
    }

    public static func unb64(_ text: String, length: Int? = nil) -> [UInt8]? {
        guard !text.isEmpty, text.utf8.allSatisfy(isStandardBase64), let data = B64.decodeOrNil(text) else { return nil }
        let bytes = Array(data)
        if let length, bytes.count != length { return nil }
        return bytes
    }

    static func isStandardBase64(_ c: UInt8) -> Bool {
        (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c == 0x2b || c == 0x2f
            || c == 0x3d
    }

    static func bytes64u(_ value: JSONValue?, _ length: Int) -> [UInt8]? {
        guard let text = value?.stringValue else { return nil }
        return unb64u(text, length: length)
    }

    static func bytes64(_ value: JSONValue?, _ length: Int? = nil) -> [UInt8]? {
        guard let text = value?.stringValue else { return nil }
        return unb64(text, length: length)
    }

    public static func id16(_ bytes: [UInt8]) -> String {
        String(b64u(Sha256.hash(bytes)).prefix(keyIdChars))
    }

    public static func isKeyId(_ value: String) -> Bool {
        CryptoText.isBase64UrlText(value, length: keyIdChars)
    }

    static func int(_ value: JSONValue?) -> Int64? {
        if case .int(let number)? = value { return number }
        return nil
    }

    public static func isValidAcct(_ acct: String) -> Bool {
        let count = acct.unicodeScalars.count
        guard count >= 1, count <= maxAcctChars else { return false }
        return acct.unicodeScalars.allSatisfy(isPrintable)
    }

    static func isPrintable(_ scalar: Unicode.Scalar) -> Bool {
        if scalar == " " { return true }
        switch scalar.properties.generalCategory {
        case .control, .format, .surrogate, .privateUse, .unassigned, .lineSeparator, .paragraphSeparator,
            .spaceSeparator:
            return false
        default:
            return true
        }
    }

    static func lines(_ parts: [String]) -> [UInt8] { Array(parts.joined(separator: "\n").utf8) }

    static func ascending(_ a: String, _ b: String) -> Bool { a.utf8.lexicographicallyPrecedes(b.utf8) }

    static func storeSuffix(_ name: String) -> String { b64u(Array(name.utf8)) }
}

public struct ChatPrekey: Equatable, Sendable {
    public let id: String
    public let x: [UInt8]
    public let pq: [UInt8]
    public let ts: Int64
    public let exp: Int64

    public init(x: [UInt8], pq: [UInt8], ts: Int64, exp: Int64) {
        self.id = ChatV3.id16(x + pq)
        self.x = x
        self.pq = pq
        self.ts = ts
        self.exp = exp
    }

    public var json: JSONObject {
        [
            "id": .string(id), "x": .string(ChatV3.b64u(x)), "pq": .string(ChatV3.b64u(pq)), "ts": .int(ts),
            "exp": .int(exp),
        ]
    }

    public static func parse(_ value: JSONValue?) -> ChatPrekey? {
        guard let object = value?.objectValue, let id = object["id"]?.stringValue,
            let x = ChatV3.bytes64u(object["x"], X25519.keyBytes),
            let pq = ChatV3.bytes64u(object["pq"], MlKem768.publicKeyBytes),
            let ts = ChatV3.int(object["ts"]), let exp = ChatV3.int(object["exp"])
        else { return nil }
        let prekey = ChatPrekey(x: x, pq: pq, ts: ts, exp: exp)
        return prekey.id == id ? prekey : nil
    }
}

public struct ChatPrekeySecret: Sendable {
    public let prekey: ChatPrekey
    let xPriv: [UInt8]
    let pqSeed: [UInt8]

    public init(xPriv: [UInt8], pqSeed: [UInt8], ts: Int64) throws {
        guard xPriv.count == X25519.keyBytes, pqSeed.count == MlKem768.seedBytes else {
            throw CryptoArgumentError("A prekey needs a 32-byte X25519 key and a 64-byte ML-KEM seed.")
        }
        let x = try X25519.publicKey(privateKey: xPriv)
        let pq = try MlKem768.generateKeyPair(seed: pqSeed).publicKey
        self.prekey = ChatPrekey(x: x, pq: pq, ts: ts, exp: ts + ChatV3.prekeyLifeMs)
        self.xPriv = xPriv
        self.pqSeed = pqSeed
    }

    init(prekey: ChatPrekey, xPriv: [UInt8], pqSeed: [UInt8]) {
        self.prekey = prekey
        self.xPriv = xPriv
        self.pqSeed = pqSeed
    }

    static func generate(now: Int64) throws -> ChatPrekeySecret {
        try ChatPrekeySecret(xPriv: X25519.generatePrivateKey(), pqSeed: CryptoBytes.random(MlKem768.seedBytes), ts: now)
    }

    var stored: JSONObject {
        [
            "id": .string(prekey.id), "xPriv": .string(ChatV3.b64(xPriv)), "x": .string(ChatV3.b64u(prekey.x)),
            "pqSeed": .string(ChatV3.b64(pqSeed)), "pq": .string(ChatV3.b64u(prekey.pq)), "ts": .int(prekey.ts),
            "exp": .int(prekey.exp),
        ]
    }

    static func parseStored(_ value: JSONValue) -> ChatPrekeySecret? {
        guard let object = value.objectValue, let prekey = ChatPrekey.parse(value),
            let xPriv = ChatV3.bytes64(object["xPriv"], X25519.keyBytes),
            let pqSeed = ChatV3.bytes64(object["pqSeed"], MlKem768.seedBytes)
        else { return nil }
        return ChatPrekeySecret(prekey: prekey, xPriv: xPriv, pqSeed: pqSeed)
    }
}

struct ChatReplayEntry {
    var max: Int64 = 0
    var seen: Set<Int64> = []
}

public final class ChatKeyring: @unchecked Sendable {
    public static let identityStoreKey = "chat/e2ee/ed25519IdentityV3"
    public static let prekeysStoreKey = "chat/e2ee/prekeysV3"
    public static let counterFloorStoreKey = "chat/e2ee/ctrFloorV3"
    public static let counterEpochSeconds: Int64 = 1_767_225_600

    public let identityKey: [UInt8]
    public let idk: String
    public let kid: String

    private let seed: [UInt8]
    private let store: SecureStore?
    private let counterFloor: Int64
    private let lock = NSLock()
    private var prekeys: [ChatPrekeySecret]
    private var kemSecrets: [String: [UInt8]] = [:]
    private var trusts: [String: ChatTrustStore] = [:]
    private var counters: [String: Int64] = [:]
    private var replay: [String: [String: ChatReplayEntry]] = [:]
    private var replayOrder: [String: [String]] = [:]
    private var replayDirty: Set<String> = []
    private var replayWriteScheduled = false
    private static let replayQueue = DispatchQueue(label: "forkmesh.chat.replay", qos: .utility)
    public static let replayWriteDelay: DispatchTimeInterval = .milliseconds(250)
    private var ratchets: [String: [String: ChatRatchetPeer]] = [:]
    private let ratchetLock = NSLock()

    public func callSigningSeed() -> [UInt8] { seed }

    private init(seed: [UInt8], prekeys: [ChatPrekeySecret], store: SecureStore?, counterFloor: Int64 = 0) throws {
        guard seed.count == Ed25519.seedBytes else { throw CryptoArgumentError("The chat identity seed must be 32 bytes.") }
        self.seed = seed
        self.counterFloor = max(counterFloor, 0)
        self.identityKey = try Ed25519.publicKey(seed: seed)
        self.idk = ChatV3.b64u(identityKey)
        self.kid = ChatV3.id16(identityKey)
        self.store = store
        self.prekeys = prekeys.sorted { $0.prekey.ts < $1.prekey.ts }
    }

    public static func fromSecrets(
        identitySeed: [UInt8], prekeys: [ChatPrekeySecret] = [], store: SecureStore? = nil
    ) throws -> ChatKeyring {
        try ChatKeyring(seed: identitySeed, prekeys: prekeys, store: store)
    }

    public static func storedSeed(_ text: String?) -> [UInt8]? {
        guard let text, let bytes = B64.decodeOrNil(text.trimmingCharacters(in: .whitespacesAndNewlines)),
            bytes.count == Ed25519.seedBytes
        else { return nil }
        return Array(bytes)
    }

    public static func derivedSeed(nodeSeed: [UInt8]) -> [UInt8] {
        Hkdf.sha256(
            ikm: nodeSeed, salt: Array("ForkMesh chat identity salt v3".utf8),
            info: Array("forkmesh-chat-identity-v3".utf8), length: Ed25519.seedBytes)
    }

    public static func load(_ store: SecureStore, create: Bool, now: Int64 = Date().epochMillis) throws -> ChatKeyring? {
        var seed = storedSeed(store.read(identityStoreKey))
        if seed == nil {
            guard create else { return nil }
            let fresh: [UInt8]
            if let nodeSeed = storedSeed(store.read(Identity.seedKey)) {
                fresh = derivedSeed(nodeSeed: nodeSeed)
                store.write(counterFloorStoreKey, String(max(now / 1000 - counterEpochSeconds, 0)))
            } else {
                fresh = Ed25519.generateSeed()
            }
            store.write(identityStoreKey, B64.encode(fresh))
            seed = fresh
        }
        var loaded: [ChatPrekeySecret] = []
        if let text = store.read(prekeysStoreKey), let list = (try? JSON.parse(text))?.arrayValue {
            loaded = list.compactMap(ChatPrekeySecret.parseStored)
        }
        let floor = Int64(store.read(counterFloorStoreKey)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "") ?? 0
        return try ChatKeyring(seed: seed ?? [], prekeys: loaded, store: store, counterFloor: floor)
    }

    private final class Entry {
        let store: SecureStore
        let marker: String
        let keyring: ChatKeyring

        init(store: SecureStore, marker: String, keyring: ChatKeyring) {
            self.store = store
            self.marker = marker
            self.keyring = keyring
        }
    }

    private static let registryLock = NSLock()
    private static var registry: [ObjectIdentifier: Entry] = [:]

    public static func shared(_ store: SecureStore, now: Int64 = Date().epochMillis) throws -> ChatKeyring {
        registryLock.lock()
        defer { registryLock.unlock() }
        let marker = store.read(identityStoreKey) ?? ""
        let id = ObjectIdentifier(store)
        if let entry = registry[id], entry.store === store, !marker.isEmpty, entry.marker == marker {
            return entry.keyring
        }
        guard let keyring = try load(store, create: true, now: now) else {
            throw CryptoArgumentError("The chat identity could not be created.")
        }
        keyring.rotateIfNeeded(now: now)
        registry[id] = Entry(store: store, marker: store.read(identityStoreKey) ?? "", keyring: keyring)
        return keyring
    }

    public func sign(_ message: [UInt8]) throws -> [UInt8] {
        try Ed25519.sign(seed: seed, message: message, publicKey: identityKey)
    }

    public var prekeyIds: [String] { locked { prekeys.map(\.prekey.id) } }

    public func newestPrekey() -> ChatPrekey? { locked { prekeys.last?.prekey } }

    public func addPrekey(_ secret: ChatPrekeySecret) {
        locked {
            prekeys.removeAll { $0.prekey.id == secret.prekey.id }
            prekeys.append(secret)
            prekeys.sort { $0.prekey.ts < $1.prekey.ts }
            trimPrekeys()
            persistPrekeys()
        }
    }

    @discardableResult
    public func rotateIfNeeded(now: Int64) -> Bool {
        locked {
            let wiped = wipeLocked(now: now)
            if let newest = prekeys.last, now - newest.prekey.ts < ChatV3.rotateAfterMs {
                if wiped { persistPrekeys() }
                return false
            }
            guard let fresh = try? ChatPrekeySecret.generate(now: now) else {
                if wiped { persistPrekeys() }
                return false
            }
            prekeys.append(fresh)
            trimPrekeys()
            persistPrekeys()
            return true
        }
    }

    public func wipeExpired(now: Int64) {
        locked { if wipeLocked(now: now) { persistPrekeys() } }
    }

    private func wipeLocked(now: Int64) -> Bool {
        let before = prekeys.count
        prekeys.removeAll { now > $0.prekey.exp }
        if prekeys.count == before { return false }
        let kept = Set(prekeys.map(\.prekey.id))
        for id in Array(kemSecrets.keys) where !kept.contains(id) {
            if var secret = kemSecrets.removeValue(forKey: id) { CryptoBytes.wipe(&secret) }
        }
        return true
    }

    private func trimPrekeys() {
        while prekeys.count > ChatV3.maxPrekeys {
            let dropped = prekeys.removeFirst()
            if var secret = kemSecrets.removeValue(forKey: dropped.prekey.id) { CryptoBytes.wipe(&secret) }
        }
    }

    private func persistPrekeys() {
        guard let store else { return }
        store.write(ChatKeyring.prekeysStoreKey, JSON.encode(.array(prekeys.map { .object($0.stored) })))
    }

    func prekeySecret(_ id: String) -> (xPriv: [UInt8], kem: [UInt8])? {
        locked {
            guard let entry = prekeys.first(where: { $0.prekey.id == id }) else { return nil }
            if let cached = kemSecrets[id] { return (entry.xPriv, cached) }
            guard let pair = try? MlKem768.generateKeyPair(seed: entry.pqSeed) else { return nil }
            kemSecrets[id] = pair.secretKey
            return (entry.xPriv, pair.secretKey)
        }
    }

    public func trust(_ scope: String) -> ChatTrustStore {
        locked {
            if let existing = trusts[scope] { return existing }
            let created = ChatTrustStore(scope: scope, store: store)
            trusts[scope] = created
            return created
        }
    }

    private static func counterKey(_ room: String) -> String { "chat/e2ee/ctrV3/\(ChatV3.storeSuffix(room))" }

    private static func seenKey(_ room: String) -> String { "chat/e2ee/seenV3/\(ChatV3.storeSuffix(room))" }

    func nextCounter(_ room: String) -> Int64 {
        locked {
            var current = counters[room]
            if current == nil {
                let text = store?.read(ChatKeyring.counterKey(room))?.trimmingCharacters(in: .whitespaces) ?? ""
                current = Int64(text) ?? 0
            }
            let next = max(current ?? 0, counterFloor, 0) + 1
            counters[room] = next
            store?.write(ChatKeyring.counterKey(room), String(next))
            return next
        }
    }

    func isReplay(_ room: String, _ sender: String, _ counter: Int64) -> Bool {
        locked {
            loadReplay(room)
            guard let entry = replay[room]?[sender] else { return false }
            return counter <= entry.max - ChatV3.replayWindow || entry.seen.contains(counter)
        }
    }

    func highestSeen(_ room: String, _ sender: String) -> Int64 {
        locked {
            loadReplay(room)
            return replay[room]?[sender]?.max ?? 0
        }
    }

    func recordSeen(_ room: String, _ sender: String, _ counter: Int64) -> Bool {
        locked {
            loadReplay(room)
            var entry = replay[room]?[sender] ?? ChatReplayEntry()
            if counter <= entry.max - ChatV3.replayWindow || entry.seen.contains(counter) { return false }
            entry.seen.insert(counter)
            entry.max = max(entry.max, counter)
            let floor = entry.max - ChatV3.replayWindow
            entry.seen = entry.seen.filter { $0 > floor }
            var book = replay[room] ?? [:]
            book[sender] = entry
            var order = replayOrder[room] ?? []
            order.removeAll { $0 == sender }
            order.append(sender)
            while order.count > ChatV3.maxReplaySenders { book.removeValue(forKey: order.removeFirst()) }
            replay[room] = book
            replayOrder[room] = order
            persistReplay(room)
            return true
        }
    }

    private func loadReplay(_ room: String) {
        if replay[room] != nil { return }
        var book: [String: ChatReplayEntry] = [:]
        var order: [String] = []
        if let text = store?.read(ChatKeyring.seenKey(room)), let root = (try? JSON.parse(text))?.objectValue {
            for (sender, value) in root {
                guard let object = value.objectValue, let maxSeen = ChatV3.int(object["m"]) else { continue }
                let seen = (object["s"]?.arrayValue ?? []).compactMap { ChatV3.int($0) }
                book[sender] = ChatReplayEntry(max: maxSeen, seen: Set(seen))
                order.append(sender)
            }
        }
        replay[room] = book
        replayOrder[room] = order
    }

    private func persistReplay(_ room: String) {
        guard store != nil else { return }
        replayDirty.insert(room)
        if replayWriteScheduled { return }
        replayWriteScheduled = true
        ChatKeyring.replayQueue.asyncAfter(deadline: .now() + ChatKeyring.replayWriteDelay) { self.writeDirtyReplay() }
    }

    private func writeDirtyReplay() {
        let pending: [(room: String, book: [String: ChatReplayEntry], order: [String])] = locked {
            replayWriteScheduled = false
            let rooms = replayDirty.sorted()
            replayDirty.removeAll()
            return rooms.compactMap { room in replay[room].map { (room, $0, replayOrder[room] ?? []) } }
        }
        guard let store else { return }
        for item in pending {
            var root = JSONObject()
            for sender in item.order {
                guard let entry = item.book[sender] else { continue }
                root[sender] = .object(["m": .int(entry.max), "s": .array(entry.seen.sorted().map { .int($0) })])
            }
            store.write(ChatKeyring.seenKey(item.room), JSON.encode(root))
        }
    }

    public func flushReplay() {
        ChatKeyring.replayQueue.sync { writeDirtyReplay() }
    }

    public static func persistPendingReplay() {
        registryLock.lock()
        let keyrings = registry.values.map(\.keyring)
        registryLock.unlock()
        for keyring in keyrings {
            replayQueue.async { keyring.writeDirtyReplay() }
        }
    }

    func ratchetLocked<R>(_ body: () throws -> R) rethrows -> R {
        ratchetLock.lock()
        defer { ratchetLock.unlock() }
        return try body()
    }

    public static func ratchetStoreKey(_ room: String) -> String { ChatV4.storeKey(room) }

    private func loadRatchets(_ room: String) -> [String: ChatRatchetPeer] {
        if let cached = ratchets[room] { return cached }
        var peers: [String: ChatRatchetPeer] = [:]
        if let text = store?.read(ChatV4.storeKey(room)), let root = (try? JSON.parse(text))?.objectValue,
            root["v"]?.numEquals(ChatV4.version) == true
        {
            peers = ChatRatchetBook.decode(root)
        }
        ratchets[room] = peers
        return peers
    }

    private func persistRatchets(_ room: String) {
        guard let store else { return }
        let peers = ratchets[room] ?? [:]
        if peers.isEmpty {
            store.delete(ChatV4.storeKey(room))
        } else {
            store.write(ChatV4.storeKey(room), JSON.encode(ChatRatchetBook.encode(peers)))
        }
    }

    public func ratchetPeer(_ room: String, _ kid: String) -> ChatRatchetPeer {
        locked { loadRatchets(room)[kid] ?? ChatRatchetPeer() }
    }

    func storeRatchetPeers(_ room: String, _ updates: [String: ChatRatchetPeer], now: Int64) {
        if updates.isEmpty { return }
        locked {
            var peers = loadRatchets(room)
            for (kid, peer) in updates {
                var trimmed = peer
                trimmed.trim(now: now)
                peers[kid] = trimmed
            }
            ratchets[room] = peers
            persistRatchets(room)
        }
    }

    public func resetRatchet(_ room: String, _ kid: String) {
        ratchetLocked {
            locked {
                var peers = loadRatchets(room)
                guard peers.removeValue(forKey: kid) != nil else { return }
                ratchets[room] = peers
                persistRatchets(room)
            }
        }
    }

    public func exportRatchets(_ room: String) -> JSONObject {
        locked { ChatRatchetBook.encode(loadRatchets(room)) }
    }

    public func importRatchets(_ room: String, _ stored: JSONObject) {
        ratchetLocked {
            locked {
                ratchets[room] = ChatRatchetBook.decode(stored)
                persistRatchets(room)
            }
        }
    }

    private func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

public final class OverlaySecureStore: SecureStore, @unchecked Sendable {
    private let base: SecureStore
    private let lock = NSLock()
    private var written: [String: String] = [:]
    private var deleted: Set<String> = []

    public init(base: SecureStore) {
        self.base = base
    }

    public func read(_ key: String) -> String? {
        lock.lock()
        if let value = written[key] {
            lock.unlock()
            return value
        }
        let gone = deleted.contains(key)
        lock.unlock()
        return gone ? nil : base.read(key)
    }

    public func write(_ key: String, _ value: String) {
        lock.lock()
        written[key] = value
        deleted.remove(key)
        lock.unlock()
    }

    public func delete(_ key: String) {
        lock.lock()
        written.removeValue(forKey: key)
        deleted.insert(key)
        lock.unlock()
    }

    public func readAll() -> [String: String] {
        var values = base.readAll()
        lock.lock()
        for key in deleted { values.removeValue(forKey: key) }
        for (key, value) in written { values[key] = value }
        lock.unlock()
        return values
    }
}
