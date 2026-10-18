import FMBase
import FMCrypto
import Foundation

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

public struct SecureDriveEpochKey: Equatable, Sendable {
    public var id: String
    public var number: Int
    public var key: [UInt8]

    public init(id: String, number: Int, key: [UInt8]) {
        self.id = id
        self.number = number
        self.key = key
    }

    static func fresh(number: Int) -> SecureDriveEpochKey {
        let key = CryptoBytes.random(32)
        return SecureDriveEpochKey(id: epochId(key), number: number, key: key)
    }

    static func epochId(_ key: [UInt8]) -> String { SecureDriveCrypto.hex(SecureDriveCrypto.hkdf(key, "key-id", length: 8)) }

    static func toJSON(_ epochs: [SecureDriveEpochKey]) -> JSONValue {
        .array(epochs.map { .object(["id": JSONValue($0.id), "n": JSONValue($0.number), "key": JSONValue(SecureDriveCrypto.base64($0.key))]) })
    }

    static func parse(_ array: [JSONValue]) -> [SecureDriveEpochKey] {
        var out = [SecureDriveEpochKey]()
        for value in array {
            guard let json = value.objectValue else { continue }
            guard let key = SecureDriveCrypto.unbase64(json["key"]?.stringValue ?? "_"), key.count == 32 else { continue }
            let id = epochId(key)
            if id != SecureDriveJSON.str(json, "id") { continue }
            out.append(SecureDriveEpochKey(id: id, number: SecureDriveJSON.int(json, "n"), key: key))
        }
        return out
    }
}

public struct SecureDriveHead: Equatable, Sendable {
    public var seq: Int64 = -1
    public var hash: String = ""

    public init(seq: Int64 = -1, hash: String = "") {
        self.seq = seq
        self.hash = hash
    }
}

public struct SecureDriveRun: Equatable, Hashable, Sendable {
    public var start: Int64
    public var count: Int64

    public init(start: Int64, count: Int64) {
        self.start = start
        self.count = count
    }

    static func parse(_ array: [JSONValue]) -> [SecureDriveRun] {
        var runs = [SecureDriveRun]()
        for value in array {
            let pair = value.arrayValue ?? []
            let start = SecureDriveJSON.num(pair.first, -1)
            let count = SecureDriveJSON.num(pair.count > 1 ? pair[1] : nil, -1)
            if start < SecureDriveLayout.dataPage || count <= 0 { return [] }
            runs.append(SecureDriveRun(start: start, count: count))
        }
        return runs
    }

    static func toJSON(_ runs: [SecureDriveRun]) -> JSONValue {
        .array(runs.map { .array([.int($0.start), .int($0.count)]) })
    }

    static func pages(_ runs: [SecureDriveRun]) -> Int64 { runs.reduce(0) { $0 + $1.count } }

    static func expand(_ runs: [SecureDriveRun]) -> [Int64] {
        var out = [Int64]()
        for run in runs {
            for i in 0..<max(run.count, 0) { out.append(run.start + i) }
        }
        return out
    }
}

public struct SecureDriveMembership: Equatable, Sendable {
    public var id: String = ""
    public var container: String = ""
    public var label: String = ""
    public var driveId: String = ""
    public var rootDevice: String = ""
    public var rootEd25519: [UInt8] = []
    public var epochs: [SecureDriveEpochKey] = []
    public var mailbox: Int = -1
    public var lastSeq: Int64 = -1
    public var lastHash: String = ""
    public var lamport: Int64 = 0
    public var heads: [String: SecureDriveHead] = [:]
    public var rootGeneration: Int64 = -1
    public var rootHash: String = ""
    public var removedDevices: [String] = []
    public var cachedRoot: String = ""
    public var keyringSeen: Bool = false
    public var joinSecret: [UInt8] = []
    public var joinedAt: Int64 = 0

    public init() {}

    public var isMember: Bool { !driveId.isEmpty }
    public var isPending: Bool { driveId.isEmpty && !joinSecret.isEmpty }

    public var currentEpoch: SecureDriveEpochKey? {
        var best: SecureDriveEpochKey?
        for epoch in epochs {
            if let current = best {
                if epoch.number > current.number || (epoch.number == current.number && epoch.id > current.id) { best = epoch }
            } else {
                best = epoch
            }
        }
        return best
    }

    public func epoch(_ id: String) -> SecureDriveEpochKey? { epochs.first { $0.id == id } }

    public func toJSON() -> JSONObject {
        var headsJSON = JSONObject()
        for key in heads.keys.sorted() {
            let head = heads[key]!
            headsJSON[key] = .array([.int(head.seq), .string(head.hash)])
        }
        return [
            "id": JSONValue(id),
            "container": JSONValue(container),
            "label": JSONValue(label),
            "drive": JSONValue(driveId),
            "root": JSONValue(rootDevice),
            "rootEd25519": JSONValue(SecureDriveCrypto.base64(rootEd25519)),
            "epochs": SecureDriveEpochKey.toJSON(epochs),
            "mailbox": JSONValue(mailbox),
            "lastSeq": .int(lastSeq),
            "lastHash": JSONValue(lastHash),
            "lamport": .int(lamport),
            "heads": .object(headsJSON),
            "rootGeneration": .int(rootGeneration),
            "rootHash": JSONValue(rootHash),
            "removed": JSONValue(removedDevices),
            "cachedRoot": JSONValue(cachedRoot),
            "keyringSeen": .bool(keyringSeen),
            "joinSecret": JSONValue(SecureDriveCrypto.base64(joinSecret)),
            "joinedAt": .int(joinedAt),
        ]
    }

    public static func parse(_ json: JSONObject) -> SecureDriveMembership {
        var m = SecureDriveMembership()
        m.id = SecureDriveJSON.str(json, "id")
        m.container = SecureDriveJSON.str(json, "container")
        m.label = SecureDriveJSON.str(json, "label")
        m.driveId = SecureDriveJSON.str(json, "drive")
        m.rootDevice = SecureDriveJSON.str(json, "root")
        m.rootEd25519 = SecureDriveCrypto.unbase64(SecureDriveJSON.str(json, "rootEd25519")) ?? []
        m.epochs = SecureDriveEpochKey.parse(SecureDriveJSON.arr(json, "epochs"))
        m.mailbox = SecureDriveJSON.int(json, "mailbox", -1)
        m.lastSeq = SecureDriveJSON.num(json, "lastSeq", -1)
        m.lastHash = SecureDriveJSON.str(json, "lastHash")
        m.lamport = SecureDriveJSON.num(json, "lamport")
        for (key, value) in SecureDriveJSON.obj(json, "heads") {
            let pair = value.arrayValue ?? []
            m.heads[key] = SecureDriveHead(seq: SecureDriveJSON.num(pair.first, -1), hash: pair.count > 1 ? (pair[1].stringValue ?? "") : "")
        }
        m.rootGeneration = SecureDriveJSON.num(json, "rootGeneration", -1)
        m.rootHash = SecureDriveJSON.str(json, "rootHash")
        m.cachedRoot = SecureDriveJSON.str(json, "cachedRoot")
        m.keyringSeen = json["keyringSeen"]?.boolValue ?? false
        m.removedDevices = SecureDriveJSON.arr(json, "removed").map { $0.stringValue ?? "" }
        m.joinSecret = SecureDriveCrypto.unbase64(SecureDriveJSON.str(json, "joinSecret")) ?? []
        m.joinedAt = SecureDriveJSON.num(json, "joinedAt")
        return m
    }

    mutating func wipe() {
        for i in epochs.indices { CryptoBytes.wipe(&epochs[i].key) }
        CryptoBytes.wipe(&joinSecret)
    }
}

public struct SecureDriveKdfCost: Equatable, Sendable {
    public var memoryKiB: Int
    public var passes: Int

    public init(memoryKiB: Int, passes: Int) {
        self.memoryKiB = memoryKiB
        self.passes = passes
    }

    public static let standard = SecureDriveKdfCost(memoryKiB: 65536, passes: 3)
}

public final class SecureDriveLocalState {
    public let directory: URL
    let cost: SecureDriveKdfCost
    let floor: SecureDriveKdfCost
    private var key: [UInt8] = []
    private var kdf: SecureDriveKdf?

    public var deviceId: String = ""
    public var deviceName: String = ""
    public var keys = SecureDriveDeviceKeys(x25519: [], mlkemSeed: [], ed25519: [])
    public var publicKeys = SecureDrivePublicKeys()
    public var drives: [SecureDriveMembership] = []
    public var bookmarks: [String: [UInt8]] = [:]

    public init(directory: URL) {
        self.directory = directory
        cost = .standard
        floor = SecureDriveKdfCost(memoryKiB: SecureDriveKdf.floorMemoryKiB, passes: SecureDriveKdf.floorPasses)
    }

    public init(directory: URL, testingKdf: SecureDriveKdfCost) {
        self.directory = directory
        cost = testingKdf
        floor = SecureDriveKdfCost(
            memoryKiB: min(testingKdf.memoryKiB, SecureDriveKdf.floorMemoryKiB),
            passes: min(testingKdf.passes, SecureDriveKdf.floorPasses))
    }

    deinit { lock() }

    public var fileURL: URL { directory.appendingPathComponent("device.json") }
    public var exists: Bool { FileManager.default.fileExists(atPath: fileURL.path) }
    public var isUnlocked: Bool { !key.isEmpty }

    public static func defaultName() -> String { "This iPhone" }

    public func create(passphrase: String, deviceName name: String) throws {
        if exists { throw SecureDriveError("device_exists") }
        if passphrase.utf16.count < 10 { throw SecureDriveError("passphrase_too_short") }
        let fresh = SecureDriveDeviceKeys.generate()
        let published = try fresh.publicKeys()
        let made = SecureDriveKdf.fresh(memoryKiB: cost.memoryKiB, passes: cost.passes)
        let stretched = try SecureDriveCrypto.stretch(passphrase, made)
        keys = fresh
        publicKeys = published
        deviceId = SecureDriveCrypto.randomId()
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        deviceName = trimmed.isEmpty ? Self.defaultName() : SecureDriveJSON.prefix(trimmed, 64)
        drives = []
        bookmarks = [:]
        kdf = made
        key = stretched
        do {
            try save()
        } catch {
            lock()
            throw error
        }
    }

    public func unlock(passphrase: String) throws {
        guard let data = try? Data(contentsOf: fileURL), data.count <= (1 << 24),
              let document = SecureDriveJSON.parse(Array(data)),
              SecureDriveJSON.str(document, "suite") == SecureDriveCrypto.suite else {
            throw SecureDriveError("device_unreadable")
        }
        let parsed = try SecureDriveKdf.parse(SecureDriveJSON.obj(document, "kdf"), floorMemoryKiB: floor.memoryKiB, floorPasses: floor.passes)
        var stretched = try SecureDriveCrypto.stretch(passphrase, parsed)
        defer { CryptoBytes.wipe(&stretched) }
        try load(stretched, parsed, document)
    }

    private func load(_ stretched: [UInt8], _ parsed: SecureDriveKdf, _ document: JSONObject) throws {
        guard let nonce = SecureDriveCrypto.unbase64(SecureDriveJSON.str(document, "nonce")),
              let sealed = SecureDriveCrypto.unbase64(SecureDriveJSON.str(document, "sealed")) else {
            throw SecureDriveError("device_unreadable")
        }
        guard var plain = SecureDriveCrypto.aeadOpen(key: stretched, nonce: nonce, aad: SecureDriveCrypto.label("local"), sealed: sealed) else {
            throw SecureDriveError("wrong_passphrase")
        }
        let inner = SecureDriveJSON.parse(plain)
        CryptoBytes.wipe(&plain)
        guard let inner, let loaded = try? SecureDriveDeviceKeys.parse(SecureDriveJSON.obj(inner, "keys")),
              let published = try? loaded.publicKeys() else {
            throw SecureDriveError("device_unreadable")
        }
        keys = loaded
        publicKeys = published
        deviceId = SecureDriveJSON.str(inner, "device")
        deviceName = SecureDriveJSON.str(inner, "name")
        drives = SecureDriveJSON.arr(inner, "drives").compactMap(\.objectValue).map(SecureDriveMembership.parse)
        bookmarks = [:]
        for (id, value) in SecureDriveJSON.obj(inner, "bookmarks") {
            if let bytes = SecureDriveCrypto.unbase64(value.stringValue ?? "_") { bookmarks[id] = bytes }
        }
        kdf = parsed
        key = stretched
    }

    public func changePassphrase(current: String, next: String) throws {
        guard isUnlocked, let kdf else { throw SecureDriveError("locked") }
        if next.utf16.count < 10 { throw SecureDriveError("passphrase_too_short") }
        var check = try SecureDriveCrypto.stretch(current, kdf)
        defer { CryptoBytes.wipe(&check) }
        guard SecureDriveCrypto.constantTimeEqual(check, key) else { throw SecureDriveError("wrong_passphrase") }
        let made = SecureDriveKdf.fresh(memoryKiB: cost.memoryKiB, passes: cost.passes)
        let stretched = try SecureDriveCrypto.stretch(next, made)
        let previousKdf = kdf
        let previousKey = key
        self.kdf = made
        key = stretched
        do {
            try save()
        } catch {
            self.kdf = previousKdf
            key = previousKey
            throw error
        }
    }

    public func lock() {
        CryptoBytes.wipe(&key)
        key = []
        keys.wipe()
        publicKeys = SecureDrivePublicKeys()
        for i in drives.indices { drives[i].wipe() }
        drives = []
        bookmarks = [:]
        deviceId = ""
    }

    func innerJSON(includeBookmarks: Bool) -> JSONObject {
        var inner: JSONObject = [
            "device": JSONValue(deviceId),
            "name": JSONValue(deviceName),
            "keys": .object(keys.toJSON()),
            "drives": .array(drives.map { .object($0.toJSON()) }),
        ]
        if includeBookmarks && !bookmarks.isEmpty {
            var marks = JSONObject()
            for id in bookmarks.keys.sorted() { marks[id] = JSONValue(SecureDriveCrypto.base64(bookmarks[id]!)) }
            inner["bookmarks"] = .object(marks)
        }
        return inner
    }

    public func save() throws {
        guard isUnlocked, let kdf else { throw SecureDriveError("locked") }
        var plain = SecureDriveJSON.encode(innerJSON(includeBookmarks: true))
        defer { CryptoBytes.wipe(&plain) }
        let nonce = CryptoBytes.random(12)
        guard let sealed = SecureDriveCrypto.aeadSeal(key: key, nonce: nonce, aad: SecureDriveCrypto.label("local"), plaintext: plain) else {
            throw SecureDriveError("encryption_failed")
        }
        let document: JSONObject = [
            "suite": JSONValue(SecureDriveCrypto.suite),
            "v": 1,
            "kdf": .object(kdf.toJSON()),
            "nonce": JSONValue(SecureDriveCrypto.base64(nonce)),
            "sealed": JSONValue(SecureDriveCrypto.base64(sealed)),
        ]
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            _ = chmod(directory.path, 0o700)
            let data = Data(JSON.encodePretty(.object(document)).utf8)
            #if os(iOS)
            try data.write(to: fileURL, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            #else
            try data.write(to: fileURL, options: [.atomic])
            #endif
            _ = chmod(fileURL.path, 0o600)
        } catch {
            throw SecureDriveError("device_write_failed")
        }
    }

    public func exportDevice() -> JSONObject { innerJSON(includeBookmarks: false) }

    public func importDevice(_ device: JSONObject, passphrase: String) throws {
        if passphrase.utf16.count < 10 { throw SecureDriveError("passphrase_too_short") }
        let loaded = try SecureDriveDeviceKeys.parse(SecureDriveJSON.obj(device, "keys"))
        let published = try loaded.publicKeys()
        let made = SecureDriveKdf.fresh(memoryKiB: cost.memoryKiB, passes: cost.passes)
        let stretched = try SecureDriveCrypto.stretch(passphrase, made)
        keys = loaded
        publicKeys = published
        deviceId = SecureDriveJSON.str(device, "device")
        deviceName = SecureDriveJSON.str(device, "name")
        drives = SecureDriveJSON.arr(device, "drives").compactMap(\.objectValue).map(SecureDriveMembership.parse)
        bookmarks = [:]
        kdf = made
        key = stretched
        try save()
    }

    public func membership(_ id: String) -> SecureDriveMembership? { drives.first { $0.id == id } }

    public func update(_ id: String, _ change: (inout SecureDriveMembership) -> Void) {
        guard let index = drives.firstIndex(where: { $0.id == id }) else { return }
        change(&drives[index])
    }

    public func forget(_ id: String) {
        guard let index = drives.firstIndex(where: { $0.id == id }) else { return }
        drives[index].wipe()
        drives.remove(at: index)
        bookmarks[id] = nil
    }
}
