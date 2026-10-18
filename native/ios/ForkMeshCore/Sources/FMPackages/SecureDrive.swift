import FMBase
import FMCrypto
import Foundation

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

public final class SecureDrive {
    final class Txn {
        let file: SecureDriveFile
        var records: [(String, JSONObject)] = []
        var reserved = Set<Int64>()
        var freed: [SecureDriveRun] = []
        var afterCommit: [() -> Bool] = []
        var renew: (() -> Void)?
        var bothRootSlots = false

        init(file: SecureDriveFile) {
            self.file = file
        }
    }

    final class LoadScratch {
        var chains: [String: [Int64: SecureDriveRecord]] = [:]
        var accepted: [SecureDriveRecord] = []
        var usedKeys = Set<String>()
        var unreadable = 0
        var chosen: RootCandidate?
        var fromCache = false
    }

    struct RootCandidate {
        var generation: Int64 = -1
        var keyNumber = -1
        var keyId = ""
        var body = JSONObject()
        var bodyBytes: [UInt8] = []
        var signature: [UInt8] = []
        var hash = ""
    }

    public static let standardInviteLifetimeMs: Int64 = 24 * 60 * 60 * 1000
    static let logUnitLimitPages = 4
    static let page = Int64(SecureDriveLayout.pageBytes)

    public let state: SecureDriveLocalState
    public let id: String
    public let storage: SecureDriveStorage
    public var timing: SecureDriveLeaseTiming
    public var inviteLifetimeMs: Int64 = SecureDrive.standardInviteLifetimeMs

    public private(set) var scanned = false
    var opened = false
    public private(set) var joinRequestMissing = false
    public private(set) var removedFromDrive = false
    var fingerprintValue: [UInt8] = []
    var leaseBytes: [UInt8] = []
    var rootABytes: [UInt8] = []
    var rootBBytes: [UInt8] = []
    var rootGeneration: Int64 = -1
    var rootHash = ""
    var totalPages: Int64 = 0
    var logs: [String: [SecureDriveRun]] = [:]
    var myLastRecords: [JSONValue] = []
    public private(set) var records: [SecureDriveRecord] = []
    var deviceMap: [String: SecureDriveDevice] = [:]
    var fileMap: [String: SecureDriveFileEntry] = [:]
    var invites: [String: SecureDriveInvite] = [:]
    public private(set) var requests: [SecureDriveJoinRequest] = []
    public private(set) var issues: [SecureDriveIssue] = []
    var driveLabel = ""
    var rewriteKeyring = false

    public init(state: SecureDriveLocalState, id: String, storage: SecureDriveStorage = SecureDriveLocalStorage(), timing: SecureDriveLeaseTiming = .standard) {
        self.state = state
        self.id = id
        self.storage = storage
        self.timing = timing
    }

    public var membership: SecureDriveMembership? { state.membership(id) }

    static func now() -> Int64 { Int64((Date().timeIntervalSince1970 * 1000).rounded(.down)) }

    static func uptimeMs() -> Int64 { Int64(ProcessInfo.processInfo.systemUptime * 1000) }

    static func sleep(ms: Int) {
        if ms > 0 { usleep(UInt32(ms) * 1000) }
    }

    static func label(_ name: String) -> [UInt8] { SecureDriveCrypto.label(name) }

    static func recordMessage(_ driveId: String, _ body: [UInt8]) -> [UInt8] { label("record\n") + Array(driveId.utf8) + [0x0a] + body }
    static func rootMessage(_ body: [UInt8]) -> [UInt8] { label("root\n") + body }
    static func keyringMessage(_ body: [UInt8]) -> [UInt8] { label("keyring\n") + body }
    static func requestMessage(_ body: [UInt8]) -> [UInt8] { label("request\n") + body }
    static func mailboxLabel(_ mailbox: Int) -> [UInt8] { Array("keyring/\(mailbox)".utf8) }
    static func requestAad(_ mailbox: Int) -> [UInt8] { label("request/\(mailbox)") }
    static func logAad(_ driveId: String, _ start: Int64) -> [UInt8] { label("log/\(driveId)/\(start)") }
    static func blobAad(_ driveId: String, _ blob: String, _ chunk: Int64, _ last: Bool, _ page: Int64) -> [UInt8] {
        label("blob/\(driveId)/\(blob)/\(chunk)/\(last ? "1" : "0")/\(page)")
    }
    static func rootAad(_ slotA: Bool) -> [UInt8] { label(slotA ? "root/A" : "root/B") }
    static func inviteIdBytes(_ secret: [UInt8]) -> [UInt8] { SecureDriveCrypto.hkdf(secret, "invite-id", length: 12) }

    public static func mailbox(forInvite secret: [UInt8]) -> Int {
        let bytes = SecureDriveCrypto.hkdf(secret, "mailbox", length: 2)
        guard bytes.count == 2 else { return 0 }
        return ((Int(bytes[0]) << 8) | Int(bytes[1])) % SecureDriveLayout.mailboxSlots
    }

    static func chunkCount(_ size: Int64) -> Int64 {
        max(1, (SecureDriveCrypto.paddedSize(size) + Int64(SecureDriveLayout.chunkBytes) - 1) / Int64(SecureDriveLayout.chunkBytes))
    }

    static func hashOf(_ message: [UInt8], _ signature: [UInt8]) -> String {
        SecureDriveCrypto.base64(SecureDriveCrypto.sha256(message + signature))
    }

    static func cleanName(_ name: String) -> String? {
        let cleaned = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleaned.isEmpty || cleaned.utf16.count > 255 || cleaned.contains("/") || cleaned.contains("\\") { return nil }
        return cleaned
    }

    func fingerprint(_ file: SecureDriveFile, _ m: SecureDriveMembership) -> [UInt8] {
        var bytes = Array(String(file.size).utf8)
        bytes += file.read(at: SecureDriveLayout.rootAPage * Self.page, count: Int(2 * SecureDriveLayout.rootPages * Self.page)) ?? []
        bytes += file.read(at: SecureDriveLayout.mailboxPage * Self.page, count: SecureDriveLayout.mailboxSlots * SecureDriveLayout.slotBytes) ?? []
        return SecureDriveCrypto.sha256(bytes)
    }

    public var changedSinceScan: Bool {
        guard let m = membership else { return true }
        let print = try? storage.withFile(m.container, writable: false) { fingerprint($0, m) }
        return print != fingerprintValue
    }

    public static func create(state: SecureDriveLocalState, storage: SecureDriveStorage = SecureDriveLocalStorage(), container path: String, label: String,
                              timing: SecureDriveLeaseTiming = .standard) throws -> String {
        guard state.isUnlocked else { throw SecureDriveError("locked") }
        let container = storage.canonical(path)
        if storage.exists(container) { throw SecureDriveError("container_exists") }
        for m in state.drives where storage.canonical(m.container) == container {
            throw SecureDriveError("folder_already_connected")
        }
        try storage.createEmpty(container)
        do {
            let ok = try storage.withFile(container, writable: true) { $0.appendRandom(pages: SecureDriveLayout.dataPage + SecureDriveLayout.initialDataPages) }
            if !ok { throw SecureDriveError("drive_write_failed") }
        } catch {
            storage.removeCreated(container)
            throw SecureDriveError("drive_write_failed")
        }
        var m = SecureDriveMembership()
        m.id = SecureDriveCrypto.randomId()
        m.container = container
        let trimmed = label.trimmingCharacters(in: .whitespacesAndNewlines)
        m.label = trimmed.isEmpty ? storage.displayName(container) : SecureDriveJSON.prefix(trimmed, 80)
        m.driveId = SecureDriveCrypto.randomId()
        m.rootDevice = state.deviceId
        m.rootEd25519 = state.publicKeys.ed25519
        m.mailbox = Int(SecureDriveCrypto.randomBytes(1)[0]) % SecureDriveLayout.mailboxSlots
        m.epochs = [SecureDriveEpochKey.fresh(number: 0)]
        m.joinedAt = now()
        state.drives.append(m)
        let drive = SecureDrive(state: state, id: m.id, storage: storage, timing: timing)
        let device: JSONObject = ["id": JSONValue(state.deviceId), "name": JSONValue(state.deviceName), "keys": .object(state.publicKeys.toJSON())]
        let mailbox = m.mailbox
        let driveLabel = m.label
        do {
            try drive.transact { txn in
                txn.records.append(("drive_created", ["label": JSONValue(driveLabel), "device": .object(device), "mailbox": JSONValue(mailbox)]))
                var me = SecureDriveDevice()
                me.id = state.deviceId
                me.keys = state.publicKeys
                let unit = try drive.keyringUnit(me, mailbox: mailbox, approveKey: [], inviteId: "")
                let file = txn.file
                txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), unit) && file.sync() }
            }
        } catch {
            state.forget(m.id)
            storage.removeCreated(container)
            throw error
        }
        return m.id
    }

    public static func requestJoin(state: SecureDriveLocalState, storage: SecureDriveStorage = SecureDriveLocalStorage(), container path: String, code: String) throws -> (id: String, safety: String) {
        guard state.isUnlocked else { throw SecureDriveError("locked") }
        guard var secret = SecureDriveCrypto.parseInviteCode(code) else { throw SecureDriveError("invalid_invite_code") }
        let container = storage.canonical(path)
        for m in state.drives where storage.canonical(m.container) == container {
            throw SecureDriveError("folder_already_connected")
        }
        let inviteId = inviteIdBytes(secret)
        let mailbox = mailbox(forInvite: secret)
        let body = SecureDriveJSON.encode([
            "suite": JSONValue(SecureDriveCrypto.suite),
            "invite": JSONValue(SecureDriveCrypto.base64(inviteId)),
            "device": JSONValue(state.deviceId),
            "name": JSONValue(state.deviceName),
            "keys": .object(state.publicKeys.toJSON()),
            "time": .int(now()),
        ])
        let signature = try SecureDriveCrypto.sign(state.keys.ed25519, requestMessage(body))
        var requestKey = SecureDriveCrypto.hkdf(secret, "request")
        let unit = SecureDriveCrypto.sealUnit(requestKey, requestAad(mailbox), SecureDriveJSON.encode(SecureDriveJSON.envelope(body, signature)), size: SecureDriveLayout.slotBytes)
        CryptoBytes.wipe(&requestKey)
        guard let unit else { throw SecureDriveError("encryption_failed") }
        try storage.withFile(container, writable: true) { file in
            if file.pages < SecureDriveLayout.dataPage { throw SecureDriveError("not_a_drive") }
            if !file.write(at: SecureDriveLayout.mailboxOffset(mailbox), unit) || !file.sync() { throw SecureDriveError("drive_write_failed") }
        }
        var m = SecureDriveMembership()
        m.id = SecureDriveCrypto.randomId()
        m.container = container
        m.label = storage.displayName(container)
        m.joinSecret = secret
        m.mailbox = mailbox
        state.drives.append(m)
        do {
            try state.save()
        } catch {
            state.forget(m.id)
            throw error
        }
        CryptoBytes.wipe(&secret)
        return (m.id, SecureDriveCrypto.safetyNumber(inviteId, state.publicKeys))
    }

    public func cancelJoin() throws {
        guard let m = membership, m.isPending else { throw SecureDriveError("not_pending") }
        try? storage.withFile(m.container, writable: true) { file in
            var requestKey = SecureDriveCrypto.hkdf(m.joinSecret, "request")
            defer { CryptoBytes.wipe(&requestKey) }
            let slot = file.read(at: SecureDriveLayout.mailboxOffset(m.mailbox), count: SecureDriveLayout.slotBytes)
            if SecureDriveCrypto.openUnit(requestKey, Self.requestAad(m.mailbox), slot) != nil {
                _ = file.write(at: SecureDriveLayout.mailboxOffset(m.mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes))
                _ = file.sync()
            }
        }
        state.forget(id)
        try state.save()
    }

    public var joinSafetyNumber: String {
        guard let m = membership, !m.joinSecret.isEmpty else { return "" }
        return SecureDriveCrypto.safetyNumber(Self.inviteIdBytes(m.joinSecret), state.publicKeys)
    }

    public func scan() throws {
        guard let m = membership else { throw SecureDriveError("unknown_drive") }
        try storage.withFile(m.container, writable: false) { file in
            let print = fingerprint(file, m)
            try load(file)
            fingerprintValue = print
        }
        if !opened, let current = membership, current.isMember, !removedFromDrive {
            opened = true
            try note("drive_opened", [:])
        }
    }

    func loadPending(_ file: SecureDriveFile) throws {
        guard var m = membership else { throw SecureDriveError("unknown_drive") }
        let slot = file.read(at: SecureDriveLayout.mailboxOffset(m.mailbox), count: SecureDriveLayout.slotBytes) ?? []
        var approveKey = SecureDriveCrypto.hkdf(m.joinSecret, "approve")
        var requestKey = SecureDriveCrypto.hkdf(m.joinSecret, "request")
        defer {
            CryptoBytes.wipe(&approveKey)
            CryptoBytes.wipe(&requestKey)
        }
        let inviteId = SecureDriveCrypto.base64(Self.inviteIdBytes(m.joinSecret))
        var adopted = false
        if var plain = try? SecureDriveCrypto.openFromDevice(state.keys, label: Self.mailboxLabel(m.mailbox), slot) {
            if let outer = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain)),
               let ring = SecureDriveJSON.openEnvelope(outer),
               let mac = SecureDriveCrypto.unbase64(outer["m"]?.stringValue ?? "_"),
               SecureDriveCrypto.constantTimeEqual(mac, SecureDriveCrypto.hmac(approveKey, ring.body)),
               SecureDriveJSON.str(ring.parsed, "device") == state.deviceId,
               SecureDriveJSON.str(ring.parsed, "invite") == inviteId,
               let issuer = SecureDrivePublicKeys.parse(ring.parsed["issuerKeys"]?.objectValue),
               SecureDriveCrypto.verify(issuer.ed25519, Self.keyringMessage(ring.body), ring.signature),
               let rootEd = SecureDriveCrypto.unbase64(SecureDriveJSON.str(SecureDriveJSON.obj(ring.parsed, "root"), "ed25519")),
               rootEd.count == 32 {
                let epochs = SecureDriveEpochKey.parse(SecureDriveJSON.arr(ring.parsed, "epochs"))
                if !epochs.isEmpty {
                    m.driveId = SecureDriveJSON.str(ring.parsed, "drive")
                    m.rootDevice = SecureDriveJSON.str(SecureDriveJSON.obj(ring.parsed, "root"), "device")
                    m.rootEd25519 = rootEd
                    m.epochs = epochs
                    CryptoBytes.wipe(&m.joinSecret)
                    m.joinSecret = []
                    m.joinedAt = Self.now()
                    adopted = true
                }
            }
            CryptoBytes.wipe(&plain)
        }
        if adopted {
            state.update(id) { $0 = m }
            try state.save()
            try load(file)
            return
        }
        joinRequestMissing = SecureDriveCrypto.openUnit(requestKey, Self.requestAad(m.mailbox), slot) == nil
        scanned = true
    }

    func load(_ file: SecureDriveFile) throws {
        guard var m = membership else { throw SecureDriveError("unknown_drive") }
        issues = []
        requests = []
        joinRequestMissing = false
        removedFromDrive = false
        totalPages = file.pages
        if totalPages < SecureDriveLayout.dataPage { throw SecureDriveError("not_a_drive") }
        if m.isPending { return try loadPending(file) }
        if !m.isMember { throw SecureDriveError("unknown_drive") }
        let stateBefore = JSON.encode(m.toJSON())
        let me = state.deviceId

        struct Ring {
            var body: JSONObject
            var bodyBytes: [UInt8]
            var signature: [UInt8]
            var epochs: [SecureDriveEpochKey]
        }
        var ring: Ring?
        if m.mailbox >= 0 {
            let slot = file.read(at: SecureDriveLayout.mailboxOffset(m.mailbox), count: SecureDriveLayout.slotBytes) ?? []
            if var plain = try? SecureDriveCrypto.openFromDevice(state.keys, label: Self.mailboxLabel(m.mailbox), slot) {
                if let outer = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain)),
                   let opened = SecureDriveJSON.openEnvelope(outer),
                   SecureDriveJSON.str(opened.parsed, "drive") == m.driveId,
                   SecureDriveJSON.str(opened.parsed, "device") == me {
                    ring = Ring(body: opened.parsed, bodyBytes: opened.body, signature: opened.signature,
                                epochs: SecureDriveEpochKey.parse(SecureDriveJSON.arr(opened.parsed, "epochs")))
                }
                CryptoBytes.wipe(&plain)
            }
        }

        rootABytes = file.read(at: SecureDriveLayout.rootAPage * Self.page, count: Int(SecureDriveLayout.rootPages * Self.page)) ?? []
        rootBBytes = file.read(at: SecureDriveLayout.rootBPage * Self.page, count: Int(SecureDriveLayout.rootPages * Self.page)) ?? []

        let scratch = LoadScratch()

        func attempt(_ keys: [SecureDriveEpochKey]) {
            issues = []
            scratch.chains = [:]
            scratch.accepted = []
            scratch.usedKeys = []
            scratch.unreadable = 0
            scratch.chosen = nil
            scratch.fromCache = false
            for slotA in [true, false] {
                let bytes = slotA ? rootABytes : rootBBytes
                for epoch in keys {
                    guard let plain = SecureDriveCrypto.openUnit(SecureDriveCrypto.hkdf(epoch.key, "root"), Self.rootAad(slotA), bytes) else { continue }
                    if let outer = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain)),
                       let opened = SecureDriveJSON.openEnvelope(outer),
                       SecureDriveJSON.str(opened.parsed, "drive") == m.driveId,
                       SecureDriveJSON.int(opened.parsed, "v") == 2 {
                        var candidate = RootCandidate()
                        candidate.body = opened.parsed
                        candidate.bodyBytes = opened.body
                        candidate.signature = opened.signature
                        candidate.generation = SecureDriveJSON.num(opened.parsed, "gen", -1)
                        candidate.keyNumber = epoch.number
                        candidate.keyId = epoch.id
                        candidate.hash = Self.hashOf(Self.rootMessage(opened.body), opened.signature)
                        let writer = SecureDriveJSON.str(opened.parsed, "writer")
                        if m.removedDevices.contains(writer) {
                            addIssue("untrusted_root", writer, "")
                            break
                        }
                        if let current = scratch.chosen {
                            if candidate.keyNumber > current.keyNumber || (candidate.keyNumber == current.keyNumber && candidate.generation > current.generation) {
                                scratch.chosen = candidate
                            }
                        } else {
                            scratch.chosen = candidate
                        }
                    }
                    break
                }
            }
            if !m.cachedRoot.isEmpty, let cachedBytes = SecureDriveCrypto.unbase64(m.cachedRoot),
               let cachedOuter = SecureDriveJSON.parse(cachedBytes),
               let opened = SecureDriveJSON.openEnvelope(cachedOuter),
               SecureDriveJSON.str(opened.parsed, "drive") == m.driveId {
                var cached = RootCandidate()
                cached.body = opened.parsed
                cached.bodyBytes = opened.body
                cached.signature = opened.signature
                cached.generation = SecureDriveJSON.num(opened.parsed, "gen", -1)
                cached.hash = Self.hashOf(Self.rootMessage(opened.body), opened.signature)
                if cached.hash == m.rootHash && (scratch.chosen == nil || cached.generation > scratch.chosen!.generation) {
                    scratch.chosen = cached
                    scratch.fromCache = true
                    addIssue("index_restored", "", "")
                }
            }
            logs = [:]
            myLastRecords = []
            guard let root = scratch.chosen else { return }
            scratch.usedKeys.insert(root.keyId)
            let logsJSON = SecureDriveJSON.obj(root.body, "logs")
            for device in logsJSON.keys.sorted() {
                let runs = SecureDriveRun.parse(logsJSON[device]?.arrayValue ?? [])
                logs[device] = runs
                for (r, run) in runs.enumerated() {
                    if run.start + run.count > totalPages {
                        scratch.unreadable += 1
                        continue
                    }
                    let bytes = file.read(at: run.start * Self.page, count: Int(run.count * Self.page))
                    var payload: JSONObject?
                    for epoch in keys {
                        guard let plain = SecureDriveCrypto.openUnit(SecureDriveCrypto.hkdf(epoch.key, "log"), Self.logAad(m.driveId, run.start), bytes) else { continue }
                        payload = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain))
                        scratch.usedKeys.insert(epoch.id)
                        break
                    }
                    guard let payload, SecureDriveJSON.str(payload, "device") == device else {
                        scratch.unreadable += 1
                        continue
                    }
                    let stored = SecureDriveJSON.arr(payload, "records")
                    if device == me && r == runs.count - 1 { myLastRecords = stored }
                    for value in stored {
                        guard let envelope = value.objectValue, let opened = SecureDriveJSON.openEnvelope(envelope) else { continue }
                        let body = opened.parsed
                        var record = SecureDriveRecord()
                        record.device = SecureDriveJSON.str(body, "d")
                        record.seq = SecureDriveJSON.num(body, "s", -1)
                        record.lamport = SecureDriveJSON.num(body, "l")
                        record.time = SecureDriveJSON.num(body, "t")
                        record.action = SecureDriveJSON.str(body, "a")
                        record.data = SecureDriveJSON.obj(body, "x")
                        record.prev = SecureDriveJSON.str(body, "p")
                        record.witness = SecureDriveJSON.obj(body, "w")
                        record.stored = envelope
                        record.hash = Self.hashOf(Self.recordMessage(m.driveId, opened.body), opened.signature)
                        if record.device != device || record.seq < 0 { continue }
                        if let found = scratch.chains[device]?[record.seq] {
                            if found.hash != record.hash { addIssue("log_forked", device, "") }
                            continue
                        }
                        scratch.chains[device, default: [:]][record.seq] = record
                    }
                }
            }

            var trusted: [String: [UInt8]] = [m.rootDevice: m.rootEd25519]
            var verified = Set<String>()
            var changed = true
            while changed {
                changed = false
                for device in trusted.keys.sorted() {
                    if verified.contains(device) { continue }
                    verified.insert(device)
                    changed = true
                    let ed = trusted[device] ?? []
                    var expected: Int64 = 0
                    var lastHash = ""
                    let chain = scratch.chains[device] ?? [:]
                    for seq in chain.keys.sorted() {
                        let record = chain[seq]!
                        guard let opened = SecureDriveJSON.openEnvelope(record.stored),
                              SecureDriveCrypto.verify(ed, Self.recordMessage(m.driveId, opened.body), opened.signature) else {
                            addIssue("bad_signature", device, "")
                            continue
                        }
                        if record.seq != expected {
                            addIssue("records_missing", device,
                                     record.seq - expected == 1 ? "record \(expected + 1)" : "records \(expected + 1) to \(record.seq)")
                        } else if record.prev != lastHash {
                            addIssue("chain_broken", device, "record \(record.seq + 1)")
                        }
                        expected = record.seq + 1
                        lastHash = record.hash
                        scratch.accepted.append(record)
                        if record.action == "device_added" {
                            let added = SecureDriveJSON.str(record.data, "device")
                            if let keys = SecureDrivePublicKeys.parse(record.data["keys"]?.objectValue), trusted[added] == nil {
                                trusted[added] = keys.ed25519
                            }
                        }
                    }
                }
            }
            for device in scratch.chains.keys.sorted() where trusted[device] == nil {
                addIssue("unknown_device", device, "")
            }
            fold(scratch.accepted, m)
        }

        var keys = m.epochs
        var ringKeys = Set<String>()
        if let ring {
            for epoch in ring.epochs where m.epoch(epoch.id) == nil {
                keys.append(epoch)
                ringKeys.insert(epoch.id)
            }
        }
        keys.sort { $0.number > $1.number }
        attempt(keys)

        var ringValid = false
        if let ring {
            let issuer = SecureDriveJSON.str(ring.body, "issuer")
            if let device = deviceMap[issuer], !device.removed {
                ringValid = SecureDriveCrypto.verify(device.keys.ed25519, Self.keyringMessage(ring.bodyBytes), ring.signature)
            }
            if !ringValid && !ringKeys.isEmpty {
                let usedRingKey = !ringKeys.isDisjoint(with: scratch.usedKeys)
                keys.removeAll { ringKeys.contains($0.id) }
                if usedRingKey { attempt(keys) }
                addIssue("untrusted_keyring", issuer, "")
            }
            if ringValid {
                m.keyringSeen = true
                for epoch in ring.epochs where m.epoch(epoch.id) == nil { m.epochs.append(epoch) }
            }
        }
        let keyringLost = ring == nil && m.keyringSeen
        let keyringGone = keyringLost && (scratch.chosen == nil || scratch.fromCache)
        rewriteKeyring = keyringLost && !keyringGone && m.mailbox >= 0
        if keyringGone { addIssue("keyring_missing", me, "") }
        if rewriteKeyring { addIssue("keyring_restored", me, "") }

        if let root = scratch.chosen {
            rootGeneration = root.generation
            rootHash = root.hash
            let writer = SecureDriveJSON.str(root.body, "writer")
            if let device = deviceMap[writer], !device.removed,
               SecureDriveCrypto.verify(device.keys.ed25519, Self.rootMessage(root.bodyBytes), root.signature) {
            } else {
                addIssue("untrusted_root", writer, "")
            }
            if SecureDriveJSON.num(root.body, "pages") > totalPages { addIssue("drive_truncated", "", "") }
            if root.generation < m.rootGeneration {
                addIssue("rolled_back", "", "index \(root.generation), this device saw \(m.rootGeneration)")
            } else if root.generation == m.rootGeneration && root.hash != m.rootHash {
                addIssue("log_forked", "", "")
            }
            if root.generation > m.rootGeneration {
                m.rootGeneration = root.generation
                m.rootHash = root.hash
                m.cachedRoot = SecureDriveCrypto.base64(SecureDriveJSON.encode(SecureDriveJSON.envelope(root.bodyBytes, root.signature)))
            }
        } else {
            let fresh = m.rootGeneration < 0 && m.rootDevice == me && m.lastSeq < 0
            if !fresh {
                state.update(id) { $0 = m }
                throw SecureDriveError("drive_unreadable")
            }
            rootGeneration = -1
            rootHash = ""
        }
        if scratch.unreadable > 0 { addIssue("unreadable_objects", "", String(scratch.unreadable)) }

        checkWitnesses(scratch.chains, m)
        let own = scratch.chains[me] ?? [:]
        let ownMax = own.keys.max() ?? -1
        if ownMax < m.lastSeq { addIssue("own_records_missing", me, "") }
        if ownMax > m.lastSeq && m.lastSeq >= 0 { addIssue("log_forked", me, "") }
        if m.lastSeq < 0 && ownMax >= 0 {
            m.lastSeq = ownMax
            m.lastHash = own[ownMax]!.hash
        }
        for record in records { m.lamport = max(m.lamport, record.lamport) }
        for device in scratch.chains.keys.sorted() {
            guard deviceMap[device] != nil, let chain = scratch.chains[device], let lastSeq = chain.keys.max() else { continue }
            let last = chain[lastSeq]!
            let head = m.heads[device] ?? SecureDriveHead()
            if last.seq > head.seq { m.heads[device] = SecureDriveHead(seq: last.seq, hash: last.hash) }
        }

        let moment = Self.now()
        for inviteId in invites.keys.sorted() {
            let invite = invites[inviteId]!
            if !invite.isOpen(moment) || invite.mailbox < 0 { continue }
            let slot = file.read(at: SecureDriveLayout.mailboxOffset(invite.mailbox), count: SecureDriveLayout.slotBytes)
            guard let plain = SecureDriveCrypto.openUnit(invite.requestKey, Self.requestAad(invite.mailbox), slot),
                  let outer = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain)),
                  let opened = SecureDriveJSON.openEnvelope(outer),
                  let keys = SecureDrivePublicKeys.parse(opened.parsed["keys"]?.objectValue),
                  SecureDriveCrypto.verify(keys.ed25519, Self.requestMessage(opened.body), opened.signature) else { continue }
            var request = SecureDriveJoinRequest()
            request.mailbox = invite.mailbox
            request.keys = keys
            request.inviteId = SecureDriveJSON.str(opened.parsed, "invite")
            request.device = SecureDriveJSON.str(opened.parsed, "device")
            request.name = SecureDriveJSON.prefix(SecureDriveJSON.str(opened.parsed, "name"), 64)
            request.time = SecureDriveJSON.num(opened.parsed, "time")
            if request.inviteId != invite.id || deviceMap[request.device] != nil { continue }
            request.safety = SecureDriveCrypto.safetyNumber(SecureDriveCrypto.unbase64(invite.id) ?? [], request.keys)
            requests.append(request)
        }

        for fileId in fileMap.keys.sorted() {
            let entry = fileMap[fileId]!
            guard !entry.deleted, let current = entry.current else { continue }
            for run in current.pages where run.start + run.count > totalPages {
                addIssue("missing_content", "", entry.name)
                break
            }
        }

        if !driveLabel.isEmpty { m.label = driveLabel }
        removedFromDrive = (deviceMap[me]?.removed ?? false) || keyringGone
        for deviceId in deviceMap.keys.sorted() {
            if deviceMap[deviceId]!.removed && !m.removedDevices.contains(deviceId) { m.removedDevices.append(deviceId) }
        }
        state.update(id) { $0 = m }
        if JSON.encode(m.toJSON()) != stateBefore { try state.save() }
        scanned = true
    }

    func addIssue(_ code: String, _ device: String, _ detail: String) {
        let perFile = code == "missing_content" || code == "content_damaged"
        for issue in issues where issue.code == code && issue.device == device && (!perFile || issue.detail == detail) { return }
        issues.append(SecureDriveIssue(code: code, device: device, detail: detail))
    }

    func checkWitnesses(_ chains: [String: [Int64: SecureDriveRecord]], _ m: SecureDriveMembership) {
        for record in records {
            for (device, value) in record.witness {
                let pair = value.arrayValue ?? []
                let seq = SecureDriveJSON.num(pair.first, -1)
                let hash = pair.count > 1 ? (pair[1].stringValue ?? "") : ""
                if seq < 0 || device == state.deviceId { continue }
                if let found = chains[device]?[seq] {
                    if found.hash != hash { addIssue("log_forked", device, "") }
                } else {
                    addIssue("records_missing", device, "\(deviceName(record.device)) saw record \(seq + 1)")
                }
            }
        }
        for device in m.heads.keys.sorted() {
            let head = m.heads[device]!
            if device == state.deviceId || head.seq < 0 { continue }
            if let found = chains[device]?[head.seq] {
                if found.hash != head.hash { addIssue("log_forked", device, "") }
            } else {
                addIssue("rolled_back", device, "this device saw record \(head.seq + 1)")
            }
        }
    }

    func fold(_ input: [SecureDriveRecord], _ m: SecureDriveMembership) {
        let sorted = input.sorted { a, b in
            if a.lamport != b.lamport { return a.lamport < b.lamport }
            if a.device != b.device { return a.device < b.device }
            return a.seq < b.seq
        }
        records = []
        driveLabel = ""
        deviceMap = [:]
        fileMap = [:]
        invites = [:]
        var cutoff: [String: Int64] = [:]
        for r in sorted where r.action == "device_removed" {
            let target = SecureDriveJSON.str(r.data, "device")
            let upTo = SecureDriveJSON.num(r.data, "upTo", -1)
            cutoff[target] = cutoff[target].map { min($0, upTo) } ?? upTo
        }
        for r in sorted {
            let x = r.data
            if let limit = cutoff[r.device], r.seq > limit {
                addIssue("after_removal", r.device, "")
                continue
            }
            if r.action == "drive_created" {
                if r.device != m.rootDevice || r.seq != 0 || !deviceMap.isEmpty { continue }
                let device = SecureDriveJSON.obj(x, "device")
                var info = SecureDriveDevice()
                info.id = r.device
                info.name = SecureDriveJSON.prefix(SecureDriveJSON.str(device, "name"), 64)
                info.keys = SecureDrivePublicKeys.parse(device["keys"]?.objectValue) ?? SecureDrivePublicKeys()
                info.addedBy = r.device
                info.addedAt = r.time
                info.mailbox = SecureDriveJSON.int(x, "mailbox", -1)
                info.root = true
                deviceMap[info.id] = info
                driveLabel = SecureDriveJSON.prefix(SecureDriveJSON.str(x, "label"), 80)
                records.append(r)
                continue
            }
            if deviceMap[r.device] == nil { continue }
            records.append(r)
            let fileId = SecureDriveJSON.str(x, "file")
            func version() -> SecureDriveFileVersion {
                var v = SecureDriveFileVersion()
                v.blob = SecureDriveJSON.str(x, "blob")
                v.key = SecureDriveCrypto.unbase64(SecureDriveJSON.str(x, "key")) ?? []
                v.pages = SecureDriveRun.parse(SecureDriveJSON.arr(x, "pages"))
                v.size = SecureDriveJSON.num(x, "size")
                v.sha256 = SecureDriveJSON.str(x, "sha256")
                v.time = r.time
                v.device = r.device
                return v
            }
            switch r.action {
            case "device_added":
                let added = SecureDriveJSON.str(x, "device")
                if added.isEmpty || deviceMap[added] != nil { continue }
                var info = SecureDriveDevice()
                info.id = added
                info.name = SecureDriveJSON.prefix(SecureDriveJSON.str(x, "name"), 64)
                info.keys = SecureDrivePublicKeys.parse(x["keys"]?.objectValue) ?? SecureDrivePublicKeys()
                info.addedBy = r.device
                info.addedAt = r.time
                info.mailbox = SecureDriveJSON.int(x, "mailbox", -1)
                deviceMap[added] = info
            case "device_removed":
                let target = SecureDriveJSON.str(x, "device")
                if var info = deviceMap[target], !info.removed {
                    info.removed = true
                    info.removedBy = r.device
                    info.removedAt = r.time
                    info.removedSeq = SecureDriveJSON.num(x, "upTo", -1)
                    deviceMap[target] = info
                }
            case "invite_created":
                var invite = SecureDriveInvite()
                invite.id = SecureDriveJSON.str(x, "invite")
                invite.requestKey = SecureDriveCrypto.unbase64(SecureDriveJSON.str(x, "requestKey")) ?? []
                invite.approveKey = SecureDriveCrypto.unbase64(SecureDriveJSON.str(x, "approveKey")) ?? []
                invite.mailbox = SecureDriveJSON.int(x, "mailbox", -1)
                invite.created = r.time
                invite.expires = SecureDriveJSON.num(x, "expires")
                invite.createdBy = r.device
                if !invite.id.isEmpty && invites[invite.id] == nil { invites[invite.id] = invite }
            case "invite_used", "invite_revoked":
                let inviteId = SecureDriveJSON.str(x, "invite")
                if var invite = invites[inviteId] {
                    if r.action == "invite_used" { invite.used = true } else { invite.revoked = true }
                    invites[inviteId] = invite
                }
            case "file_created":
                if fileId.isEmpty || fileMap[fileId] != nil { continue }
                var entry = SecureDriveFileEntry()
                entry.id = fileId
                entry.name = SecureDriveJSON.str(x, "name")
                entry.kind = SecureDriveJSON.str(x, "kind")
                entry.versions = [version()]
                entry.created = r.time
                entry.modified = r.time
                entry.createdBy = r.device
                entry.modifiedBy = r.device
                fileMap[fileId] = entry
            case "file_saved", "file_restored":
                guard var entry = fileMap[fileId], !entry.deleted else { continue }
                if r.action == "file_saved", let current = entry.current, SecureDriveJSON.str(x, "base") != current.blob {
                    entry.conflicts += 1
                }
                entry.versions.append(version())
                entry.modified = r.time
                entry.modifiedBy = r.device
                fileMap[fileId] = entry
            case "file_renamed":
                if var entry = fileMap[fileId], !entry.deleted {
                    entry.name = SecureDriveJSON.str(x, "name")
                    fileMap[fileId] = entry
                }
            case "file_deleted":
                if var entry = fileMap[fileId] {
                    entry.deleted = true
                    fileMap[fileId] = entry
                }
            case "file_rekeyed":
                guard var entry = fileMap[fileId] else { continue }
                for value in SecureDriveJSON.arr(x, "versions") {
                    let change = value.objectValue ?? JSONObject()
                    let old = SecureDriveJSON.str(change, "old")
                    for i in entry.versions.indices where entry.versions[i].blob == old {
                        entry.versions[i].blob = SecureDriveJSON.str(change, "blob")
                        entry.versions[i].key = SecureDriveCrypto.unbase64(SecureDriveJSON.str(change, "key")) ?? []
                        entry.versions[i].pages = SecureDriveRun.parse(SecureDriveJSON.arr(change, "pages"))
                    }
                }
                fileMap[fileId] = entry
            case "file_opened":
                if var entry = fileMap[fileId] {
                    entry.opens += 1
                    entry.lastOpened = r.time
                    entry.lastOpenedBy = r.device
                    fileMap[fileId] = entry
                }
            default:
                break
            }
        }
    }

    public var files: [SecureDriveFileEntry] {
        fileMap.values.filter { !$0.deleted }.sorted { a, b in
            let order = a.name.compare(b.name, options: .caseInsensitive)
            return order == .orderedSame ? a.id < b.id : order == .orderedAscending
        }
    }

    public func file(_ fileId: String) -> SecureDriveFileEntry? {
        guard let entry = fileMap[fileId], !entry.deleted else { return nil }
        return entry
    }

    public var devices: [SecureDriveDevice] { deviceMap.keys.sorted().map { deviceMap[$0]! } }

    public var openInvites: [SecureDriveInvite] {
        let moment = Self.now()
        return invites.keys.sorted().map { invites[$0]! }.filter { $0.isOpen(moment) }
    }

    public func deviceName(_ deviceId: String) -> String {
        if deviceId == state.deviceId && !deviceId.isEmpty { return state.deviceName + " (this device)" }
        if let found = deviceMap[deviceId], !found.name.isEmpty { return found.name }
        return deviceId.isEmpty ? "A device" : "Device \(deviceId.prefix(8))"
    }

    public func deviceSafetyNumber(_ deviceId: String) -> String {
        guard let found = deviceMap[deviceId], found.keys.isValid else { return "" }
        return SecureDriveCrypto.safetyNumber(Array("device".utf8), found.keys)
    }

    public var thisDeviceSafetyNumber: String { SecureDriveCrypto.safetyNumber(Array("device".utf8), state.publicKeys) }

    public var label: String { membership?.label ?? "" }

    public var containerBytes: Int64 { totalPages * Self.page }

    public var usedBytes: Int64 {
        var pages = SecureDriveLayout.dataPage
        for runs in logs.values { pages += SecureDriveRun.pages(runs) }
        for entry in fileMap.values where !entry.deleted {
            var seen = Set<Int64>()
            for version in entry.versions {
                for run in version.pages where !seen.contains(run.start) {
                    seen.insert(run.start)
                    pages += run.count
                }
            }
        }
        return pages * Self.page
    }

    public var dataRunsInUse: [SecureDriveRun] {
        var runs = [SecureDriveRun]()
        for device in logs.keys.sorted() { runs += logs[device]! }
        for entry in fileMap.values where !entry.deleted {
            for version in entry.versions { runs += version.pages }
        }
        return runs
    }

    public var mailboxesInUse: [Int] { usedMailboxes().sorted() }

    func usedMailboxes() -> Set<Int> {
        var used = Set<Int>()
        for device in deviceMap.values where !device.removed && device.mailbox >= 0 { used.insert(device.mailbox) }
        let moment = Self.now()
        for invite in invites.values where invite.isOpen(moment) && invite.mailbox >= 0 { used.insert(invite.mailbox) }
        return used
    }

    func freePages(_ txn: Txn) -> [Int64] {
        let total = txn.file.pages
        var used = [Bool](repeating: false, count: Int(max(total, 0)))
        for page in 0..<min(total, SecureDriveLayout.dataPage) { used[Int(page)] = true }
        func mark(_ runs: [SecureDriveRun]) {
            for run in runs {
                for i in 0..<max(run.count, 0) where run.start + i < total && run.start + i >= 0 { used[Int(run.start + i)] = true }
            }
        }
        for runs in logs.values { mark(runs) }
        for entry in fileMap.values where !entry.deleted {
            for version in entry.versions { mark(version.pages) }
        }
        for page in txn.reserved where page < total && page >= 0 { used[Int(page)] = true }
        var free = [Int64]()
        if total > SecureDriveLayout.dataPage {
            for page in SecureDriveLayout.dataPage..<total where !used[Int(page)] { free.append(page) }
        }
        return free
    }

    func allocate(_ txn: Txn, count: Int64, contiguous: Bool) throws -> [SecureDriveRun] {
        var free = freePages(txn)
        func findRun() -> Int64 {
            var length: Int64 = 0
            for i in free.indices {
                length = (i > 0 && free[i] == free[i - 1] + 1) ? length + 1 : 1
                if length >= count { return free[i] - count + 1 }
            }
            return -1
        }
        var start: Int64 = contiguous ? findRun() : 0
        if (contiguous && start < 0) || (!contiguous && Int64(free.count) < count) {
            let total = txn.file.pages
            let shortfall = contiguous ? count : count - Int64(free.count)
            let grow = max(shortfall, SecureDriveLayout.initialDataPages, total / 8)
            if !txn.file.appendRandom(pages: grow) { throw SecureDriveError("drive_write_failed") }
            txn.renew?()
            free = freePages(txn)
            start = contiguous ? findRun() : 0
            if (contiguous && start < 0) || (!contiguous && Int64(free.count) < count) { throw SecureDriveError("drive_full") }
        }
        let taken: [Int64] = contiguous ? (0..<count).map { start + $0 } : Array(free.prefix(Int(count)))
        var runs = [SecureDriveRun]()
        for page in taken {
            txn.reserved.insert(page)
            if let last = runs.last, last.start + last.count == page {
                runs[runs.count - 1].count += 1
            } else {
                runs.append(SecureDriveRun(start: page, count: 1))
            }
        }
        return runs
    }

    func leaseUnit(_ epoch: SecureDriveEpochKey) -> [UInt8]? {
        SecureDriveCrypto.sealUnit(SecureDriveCrypto.hkdf(epoch.key, "lease"), Self.label("lease"),
                                   SecureDriveJSON.encode(["device": JSONValue(state.deviceId), "token": JSONValue(SecureDriveCrypto.randomId())]),
                                   size: SecureDriveLayout.pageBytes)
    }

    func acquireLease(_ file: SecureDriveFile) throws {
        guard let m = membership, let epoch = m.currentEpoch else { throw SecureDriveError("no_drive_key") }
        var observed: [UInt8]?
        var seenAt = Self.uptimeMs()
        let startedAt = Self.uptimeMs()
        while true {
            let bytes = file.read(at: SecureDriveLayout.leasePage * Self.page, count: SecureDriveLayout.pageBytes) ?? []
            var holder = ""
            for key in m.epochs {
                if let plain = SecureDriveCrypto.openUnit(SecureDriveCrypto.hkdf(key.key, "lease"), Self.label("lease"), bytes),
                   let lease = SecureDriveJSON.parse(SecureDriveCrypto.stripPadding(plain)) {
                    holder = SecureDriveJSON.str(lease, "device")
                    break
                }
            }
            var free = holder.isEmpty || holder == state.deviceId || (deviceMap[holder]?.removed ?? false)
            if !free {
                if bytes != observed {
                    observed = bytes
                    seenAt = Self.uptimeMs()
                } else if Self.uptimeMs() - seenAt >= Int64(timing.staleMs) {
                    free = true
                }
            }
            if free {
                guard let mine = leaseUnit(epoch),
                      file.write(at: SecureDriveLayout.leasePage * Self.page, mine), file.sync() else {
                    throw SecureDriveError("drive_write_failed")
                }
                Self.sleep(ms: timing.settleMs)
                if file.read(at: SecureDriveLayout.leasePage * Self.page, count: SecureDriveLayout.pageBytes) == mine {
                    leaseBytes = mine
                    return
                }
                observed = nil
            }
            if Self.uptimeMs() - startedAt > Int64(timing.staleMs) * 2 + 10000 { throw SecureDriveError("drive_busy") }
            Self.sleep(ms: max(1, timing.pollMs))
        }
    }

    @discardableResult
    func renewLease(_ file: SecureDriveFile) -> Bool {
        guard let epoch = membership?.currentEpoch,
              file.read(at: SecureDriveLayout.leasePage * Self.page, count: SecureDriveLayout.pageBytes) == leaseBytes,
              let mine = leaseUnit(epoch),
              file.write(at: SecureDriveLayout.leasePage * Self.page, mine), file.sync() else { return false }
        leaseBytes = mine
        return true
    }

    func releaseLease(_ file: SecureDriveFile) {
        if !leaseBytes.isEmpty && file.read(at: SecureDriveLayout.leasePage * Self.page, count: SecureDriveLayout.pageBytes) == leaseBytes {
            _ = file.write(at: SecureDriveLayout.leasePage * Self.page, CryptoBytes.random(SecureDriveLayout.pageBytes))
            _ = file.sync()
        }
        leaseBytes = []
    }

    func transact(_ body: (Txn) throws -> Void) throws {
        guard let m = membership, m.isMember else { throw SecureDriveError("unknown_drive") }
        try storage.withFile(m.container, writable: true) { file in
            try load(file)
            if removedFromDrive { throw SecureDriveError("device_removed") }
            try acquireLease(file)
            var failure: Error?
            do {
                try load(file)
            } catch {
                failure = error
            }
            let before = membership
            let txn = Txn(file: file)
            var renewedAt = Self.uptimeMs()
            txn.renew = { [unowned self] in
                if Self.uptimeMs() - renewedAt > Int64(max(1000, self.timing.staleMs / 8)) {
                    self.renewLease(file)
                    renewedAt = Self.uptimeMs()
                }
            }
            if failure == nil {
                do {
                    if removedFromDrive { throw SecureDriveError("device_removed") }
                    try body(txn)
                    try commit(txn)
                } catch {
                    failure = error
                }
            }
            if failure != nil, let before {
                state.update(id) { $0 = before }
            }
            releaseLease(file)
            if let failure { throw failure }
            try? load(file)
        }
    }

    func commit(_ txn: Txn) throws {
        guard var m = membership, let epoch = m.currentEpoch else { throw SecureDriveError("no_drive_key") }
        let me = state.deviceId
        var mine = logs[me] ?? []
        var reuse = !myLastRecords.isEmpty && !mine.isEmpty
        var added = [JSONValue]()
        for (action, data) in txn.records {
            var witness = JSONObject()
            for device in m.heads.keys.sorted() {
                let head = m.heads[device]!
                if device != me && head.seq >= 0 { witness[device] = .array([.int(head.seq), .string(head.hash)]) }
            }
            let bodyBytes = SecureDriveJSON.encode([
                "d": JSONValue(me),
                "s": .int(m.lastSeq + 1),
                "l": .int(m.lamport + 1),
                "t": .int(Self.now()),
                "a": JSONValue(action),
                "x": .object(data),
                "p": JSONValue(m.lastHash),
                "w": .object(witness),
            ])
            let message = Self.recordMessage(m.driveId, bodyBytes)
            let signature = try SecureDriveCrypto.sign(state.keys.ed25519, message)
            let hash = Self.hashOf(message, signature)
            m.lastSeq += 1
            m.lastHash = hash
            m.lamport += 1
            m.heads[me] = SecureDriveHead(seq: m.lastSeq, hash: hash)
            added.append(.object(SecureDriveJSON.envelope(bodyBytes, signature)))
        }
        func payload(_ records: [JSONValue]) -> [UInt8] {
            SecureDriveJSON.encode(["device": JSONValue(me), "records": .array(records)])
        }
        func pagesFor(_ bytes: [UInt8]) -> Int64 {
            (Int64(bytes.count) + Int64(SecureDriveLayout.unitOverhead) + Self.page - 1) / Self.page
        }
        var plain = payload((reuse ? myLastRecords : []) + added)
        if reuse && pagesFor(plain) > Int64(Self.logUnitLimitPages) {
            reuse = false
            plain = payload(added)
        }
        let pages = max(1, pagesFor(plain))
        let unit = try allocate(txn, count: pages, contiguous: true)[0]
        guard let sealed = SecureDriveCrypto.sealUnit(SecureDriveCrypto.hkdf(epoch.key, "log"), Self.logAad(m.driveId, unit.start), plain, size: Int(unit.count * Self.page)),
              txn.file.write(at: unit.start * Self.page, sealed) else { throw SecureDriveError("drive_write_failed") }
        if reuse { txn.freed.append(mine.removeLast()) }
        mine.append(unit)
        var nextLogs = logs
        nextLogs[me] = mine
        var logsJSON = JSONObject()
        for device in nextLogs.keys.sorted() { logsJSON[device] = SecureDriveRun.toJSON(nextLogs[device]!) }
        let generation = rootGeneration + 1
        let rootBody = SecureDriveJSON.encode([
            "v": 2,
            "drive": JSONValue(m.driveId),
            "gen": .int(generation),
            "pages": .int(txn.file.pages),
            "logs": .object(logsJSON),
            "writer": JSONValue(me),
            "time": .int(Self.now()),
            "prev": JSONValue(rootHash),
        ])
        let rootSignature = try SecureDriveCrypto.sign(state.keys.ed25519, Self.rootMessage(rootBody))
        let slotA = generation % 2 == 0
        guard let rootUnit = SecureDriveCrypto.sealUnit(SecureDriveCrypto.hkdf(epoch.key, "root"), Self.rootAad(slotA),
                                                        SecureDriveJSON.encode(SecureDriveJSON.envelope(rootBody, rootSignature)),
                                                        size: Int(SecureDriveLayout.rootPages * Self.page)) else {
            throw SecureDriveError("drive_index_full")
        }
        var otherUnit: [UInt8]?
        if txn.bothRootSlots {
            otherUnit = SecureDriveCrypto.sealUnit(SecureDriveCrypto.hkdf(epoch.key, "root"), Self.rootAad(!slotA),
                                                   SecureDriveJSON.encode(SecureDriveJSON.envelope(rootBody, rootSignature)),
                                                   size: Int(SecureDriveLayout.rootPages * Self.page))
            if otherUnit == nil { throw SecureDriveError("drive_index_full") }
        }
        if rewriteKeyring && m.mailbox >= 0 {
            var me = SecureDriveDevice()
            me.id = state.deviceId
            me.keys = state.publicKeys
            state.update(id) { $0 = m }
            let unit = try keyringUnit(me, mailbox: m.mailbox, approveKey: [], inviteId: "")
            let file = txn.file
            let mailbox = m.mailbox
            txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), unit) && file.sync() }
        }
        if !txn.file.sync() { throw SecureDriveError("drive_write_failed") }
        if txn.file.read(at: SecureDriveLayout.leasePage * Self.page, count: SecureDriveLayout.pageBytes) != leaseBytes {
            throw SecureDriveError("drive_busy")
        }
        let rootCount = Int(SecureDriveLayout.rootPages * Self.page)
        if txn.file.read(at: SecureDriveLayout.rootAPage * Self.page, count: rootCount) != rootABytes
            || txn.file.read(at: SecureDriveLayout.rootBPage * Self.page, count: rootCount) != rootBBytes {
            throw SecureDriveError("drive_changed")
        }
        let rootOffset = (slotA ? SecureDriveLayout.rootAPage : SecureDriveLayout.rootBPage) * Self.page
        guard txn.file.write(at: rootOffset, rootUnit), txn.file.sync(),
              txn.file.read(at: rootOffset, count: rootCount) == rootUnit else { throw SecureDriveError("drive_write_failed") }
        if let otherUnit {
            let otherOffset = (slotA ? SecureDriveLayout.rootBPage : SecureDriveLayout.rootAPage) * Self.page
            guard txn.file.write(at: otherOffset, otherUnit), txn.file.sync(),
                  txn.file.read(at: otherOffset, count: rootCount) == otherUnit else { throw SecureDriveError("drive_write_failed") }
        }
        m.rootGeneration = generation
        m.rootHash = Self.hashOf(Self.rootMessage(rootBody), rootSignature)
        m.cachedRoot = SecureDriveCrypto.base64(SecureDriveJSON.encode(SecureDriveJSON.envelope(rootBody, rootSignature)))
        state.update(id) { $0 = m }
        var after = true
        for action in txn.afterCommit { after = action() && after }
        for run in txn.freed {
            _ = txn.file.write(at: run.start * Self.page, CryptoBytes.random(Int(run.count * Self.page)))
        }
        _ = txn.file.sync()
        try state.save()
        if !after { throw SecureDriveError("drive_write_failed") }
    }

    public func note(_ action: String, _ data: JSONObject) throws {
        try transact { txn in txn.records.append((action, data)) }
    }

    @discardableResult
    public func createInvite() throws -> (code: String, inviteId: String) {
        var text = ""
        var inviteId = ""
        try transact { txn in
            let used = usedMailboxes()
            if used.count >= SecureDriveLayout.mailboxSlots { throw SecureDriveError("drive_full") }
            var secret: [UInt8] = []
            var mailbox = -1
            for _ in 0..<4096 {
                let made = SecureDriveCrypto.newInviteCode()
                text = made.code
                secret = made.secret
                mailbox = Self.mailbox(forInvite: secret)
                if !used.contains(mailbox) { break }
                mailbox = -1
            }
            if mailbox < 0 { throw SecureDriveError("drive_full") }
            inviteId = SecureDriveCrypto.base64(Self.inviteIdBytes(secret))
            let requestKey = SecureDriveCrypto.hkdf(secret, "request")
            let approveKey = SecureDriveCrypto.hkdf(secret, "approve")
            CryptoBytes.wipe(&secret)
            txn.records.append(("invite_created", [
                "invite": JSONValue(inviteId),
                "requestKey": JSONValue(SecureDriveCrypto.base64(requestKey)),
                "approveKey": JSONValue(SecureDriveCrypto.base64(approveKey)),
                "mailbox": JSONValue(mailbox),
                "expires": .int(Self.now() + inviteLifetimeMs),
            ]))
            let file = txn.file
            let slot = mailbox
            txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(slot), CryptoBytes.random(SecureDriveLayout.slotBytes)) && file.sync() }
        }
        return (text, inviteId)
    }

    public func revokeInvite(_ inviteId: String) throws {
        guard let invite = invites[inviteId] else { throw SecureDriveError("unknown_invite") }
        let mailbox = invite.mailbox
        try transact { txn in
            txn.records.append(("invite_revoked", ["invite": JSONValue(inviteId)]))
            let file = txn.file
            if mailbox >= 0 && !usedMailboxes().contains(mailbox) {
                txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes)) && file.sync() }
            }
        }
    }

    func keyringUnit(_ recipient: SecureDriveDevice, mailbox: Int, approveKey: [UInt8], inviteId: String) throws -> [UInt8] {
        guard let m = membership else { throw SecureDriveError("unknown_drive") }
        var body: JSONObject = [
            "suite": JSONValue(SecureDriveCrypto.suite),
            "drive": JSONValue(m.driveId),
            "device": JSONValue(recipient.id),
            "mailbox": JSONValue(mailbox),
            "epochs": SecureDriveEpochKey.toJSON(m.epochs),
            "root": ["device": JSONValue(m.rootDevice), "ed25519": JSONValue(SecureDriveCrypto.base64(m.rootEd25519))],
            "issuer": JSONValue(state.deviceId),
            "issuerKeys": .object(state.publicKeys.toJSON()),
            "time": .int(Self.now()),
        ]
        if !inviteId.isEmpty { body["invite"] = JSONValue(inviteId) }
        let bodyBytes = SecureDriveJSON.encode(body)
        let signature = try SecureDriveCrypto.sign(state.keys.ed25519, Self.keyringMessage(bodyBytes))
        var outer = SecureDriveJSON.envelope(bodyBytes, signature)
        if !approveKey.isEmpty { outer["m"] = JSONValue(SecureDriveCrypto.base64(SecureDriveCrypto.hmac(approveKey, bodyBytes))) }
        var plain = SecureDriveJSON.encode(outer)
        defer { CryptoBytes.wipe(&plain) }
        let capacity = SecureDriveLayout.slotBytes - 32 - SecureDriveCrypto.mlkemCiphertextBytes - SecureDriveLayout.unitOverhead
        if plain.count > capacity { throw SecureDriveError("keyring_too_large") }
        plain.append(contentsOf: [UInt8](repeating: 0, count: capacity - plain.count))
        let sealed = try SecureDriveCrypto.sealToDevice(recipient.keys, label: Self.mailboxLabel(mailbox), plain)
        guard sealed.count == SecureDriveLayout.slotBytes else { throw SecureDriveError("encryption_failed") }
        return sealed
    }

    public func approve(mailbox: Int) throws {
        guard let request = requests.first(where: { $0.mailbox == mailbox }) else { throw SecureDriveError("unknown_request") }
        try transact { txn in
            guard let invite = invites[request.inviteId], invite.isOpen(Self.now()) else { throw SecureDriveError("invite_expired") }
            var device = SecureDriveDevice()
            device.id = request.device
            device.name = request.name
            device.keys = request.keys
            let unit = try keyringUnit(device, mailbox: mailbox, approveKey: invite.approveKey, inviteId: invite.id)
            txn.records.append(("device_added", [
                "device": JSONValue(device.id),
                "name": JSONValue(device.name),
                "keys": .object(device.keys.toJSON()),
                "invite": JSONValue(request.inviteId),
                "mailbox": JSONValue(mailbox),
            ]))
            txn.records.append(("invite_used", [
                "invite": JSONValue(request.inviteId),
                "device": JSONValue(device.id),
                "name": JSONValue(device.name),
            ]))
            let file = txn.file
            txn.afterCommit.append {
                file.write(at: SecureDriveLayout.mailboxOffset(mailbox), unit) && file.sync()
                    && file.read(at: SecureDriveLayout.mailboxOffset(mailbox), count: SecureDriveLayout.slotBytes) == unit
            }
        }
    }

    public func deny(mailbox: Int) throws {
        guard let request = requests.first(where: { $0.mailbox == mailbox }) else { throw SecureDriveError("unknown_request") }
        try transact { txn in
            txn.records.append(("join_denied", [
                "invite": JSONValue(request.inviteId),
                "device": JSONValue(request.device),
                "name": JSONValue(request.name),
            ]))
            txn.records.append(("invite_revoked", ["invite": JSONValue(request.inviteId)]))
            let file = txn.file
            txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes)) && file.sync() }
        }
    }

    func writeBlob(_ txn: Txn, _ content: SecureDriveContent) throws -> SecureDriveFileVersion {
        guard let m = membership else { throw SecureDriveError("unknown_drive") }
        return try content.withReader { length, read in
            let chunks = Self.chunkCount(length)
            let runs = try allocate(txn, count: chunks, contiguous: false)
            let pages = SecureDriveRun.expand(runs)
            let key = CryptoBytes.random(32)
            let blob = SecureDriveCrypto.randomId()
            var hasher = Sha256Stream()
            var remaining = length
            for i in 0..<chunks {
                let take = min(Int64(SecureDriveLayout.chunkBytes), max(remaining, 0))
                var plain = take > 0 ? try read(Int(take)) : []
                if Int64(plain.count) != take { throw SecureDriveError("read_failed") }
                hasher.update(plain)
                remaining -= take
                plain.append(contentsOf: [UInt8](repeating: 0, count: SecureDriveLayout.chunkBytes - plain.count))
                let page = pages[Int(i)]
                let unit = SecureDriveCrypto.sealUnit(key, Self.blobAad(m.driveId, blob, i, i == chunks - 1, page), plain, size: SecureDriveLayout.pageBytes)
                CryptoBytes.wipe(&plain)
                guard let unit, txn.file.write(at: page * Self.page, unit) else { throw SecureDriveError("drive_write_failed") }
                if i % 256 == 255 { txn.renew?() }
            }
            var version = SecureDriveFileVersion()
            version.blob = blob
            version.key = key
            version.pages = runs
            version.size = length
            version.sha256 = SecureDriveCrypto.hex(hasher.finish())
            return version
        }
    }

    func readBlob(_ file: SecureDriveFile, _ version: SecureDriveFileVersion, _ sink: ([UInt8]) throws -> Void) throws {
        guard let m = membership else { throw SecureDriveError("unknown_drive") }
        let pages = SecureDriveRun.expand(version.pages)
        let chunks = Self.chunkCount(version.size)
        if Int64(pages.count) != chunks { throw SecureDriveError("content_damaged") }
        var hasher = Sha256Stream()
        var remaining = version.size
        for i in 0..<chunks {
            let page = pages[Int(i)]
            guard var plain = SecureDriveCrypto.openUnit(version.key, Self.blobAad(m.driveId, version.blob, i, i == chunks - 1, page),
                                                         file.read(at: page * Self.page, count: SecureDriveLayout.pageBytes)) else {
                throw SecureDriveError("content_damaged")
            }
            let take = Int(min(remaining, Int64(plain.count)))
            if take > 0 {
                let part = Array(plain[0..<take])
                hasher.update(part)
                do {
                    try sink(part)
                } catch {
                    CryptoBytes.wipe(&plain)
                    throw SecureDriveError("write_failed")
                }
            }
            CryptoBytes.wipe(&plain)
            remaining -= Int64(take)
        }
        if SecureDriveCrypto.hex(hasher.finish()) != version.sha256 { throw SecureDriveError("content_damaged") }
    }

    @discardableResult
    public func addFile(_ name: String, _ content: SecureDriveContent, kind: String = "") throws -> String {
        guard let cleaned = Self.cleanName(name) else { throw SecureDriveError("invalid_name") }
        let fileId = SecureDriveCrypto.randomId()
        try transact { txn in
            let version = try writeBlob(txn, content)
            txn.records.append(("file_created", [
                "file": JSONValue(fileId),
                "name": JSONValue(cleaned),
                "kind": JSONValue(kind.isEmpty ? SecureDriveText.fileKind(cleaned) : kind),
                "blob": JSONValue(version.blob),
                "pages": SecureDriveRun.toJSON(version.pages),
                "key": JSONValue(SecureDriveCrypto.base64(version.key)),
                "size": .int(version.size),
                "sha256": JSONValue(version.sha256),
            ]))
        }
        return fileId
    }

    public func saveFile(_ fileId: String, _ content: SecureDriveContent, base startedFrom: String = "") throws {
        try transact { txn in
            guard let entry = file(fileId), let current = entry.current else { throw SecureDriveError("unknown_file") }
            let base = startedFrom.isEmpty ? current.blob : startedFrom
            let version = try writeBlob(txn, content)
            txn.records.append(("file_saved", [
                "file": JSONValue(fileId),
                "name": JSONValue(entry.name),
                "blob": JSONValue(version.blob),
                "pages": SecureDriveRun.toJSON(version.pages),
                "key": JSONValue(SecureDriveCrypto.base64(version.key)),
                "size": .int(version.size),
                "sha256": JSONValue(version.sha256),
                "base": JSONValue(base),
            ]))
        }
    }

    public func readFile(_ fileId: String, version versionIndex: Int = -1, purpose: String = "view", into sink: ([UInt8]) throws -> Void) throws {
        guard let entry = file(fileId), !entry.versions.isEmpty, let m = membership else { throw SecureDriveError("unknown_file") }
        let index = versionIndex < 0 ? entry.versions.count - 1 : versionIndex
        if index >= entry.versions.count { throw SecureDriveError("unknown_version") }
        let version = entry.versions[index]
        let name = entry.name
        do {
            try storage.withFile(m.container, writable: false) { file in
                try readBlob(file, version, sink)
            }
        } catch let error as SecureDriveError {
            if error.code == "content_damaged" {
                try? note("integrity_alert", ["code": "content_damaged", "file": JSONValue(fileId), "detail": JSONValue(name)])
                addIssue("content_damaged", "", name)
            }
            throw error
        }
        try note("file_opened", [
            "file": JSONValue(fileId),
            "name": JSONValue(name),
            "version": JSONValue(index),
            "purpose": JSONValue(purpose.isEmpty ? "view" : purpose),
        ])
    }

    public func read(_ fileId: String, version: Int = -1, purpose: String = "view") throws -> [UInt8] {
        var out = [UInt8]()
        try readFile(fileId, version: version, purpose: purpose) { out.append(contentsOf: $0) }
        return out
    }

    public func renameFile(_ fileId: String, to name: String) throws {
        guard let cleaned = Self.cleanName(name) else { throw SecureDriveError("invalid_name") }
        try transact { txn in
            guard let entry = file(fileId) else { throw SecureDriveError("unknown_file") }
            txn.records.append(("file_renamed", ["file": JSONValue(fileId), "name": JSONValue(cleaned), "from": JSONValue(entry.name)]))
        }
    }

    public func deleteFile(_ fileId: String) throws {
        try transact { txn in
            guard let entry = file(fileId) else { throw SecureDriveError("unknown_file") }
            var seen = Set<Int64>()
            for version in entry.versions {
                for run in version.pages where !seen.contains(run.start) {
                    seen.insert(run.start)
                    txn.freed.append(run)
                }
            }
            txn.records.append(("file_deleted", ["file": JSONValue(fileId), "name": JSONValue(entry.name)]))
        }
    }

    public func restoreVersion(_ fileId: String, version index: Int) throws {
        try transact { txn in
            guard let entry = file(fileId) else { throw SecureDriveError("unknown_file") }
            if index < 0 || index >= entry.versions.count { throw SecureDriveError("unknown_version") }
            let version = entry.versions[index]
            txn.records.append(("file_restored", [
                "file": JSONValue(fileId),
                "name": JSONValue(entry.name),
                "version": JSONValue(index),
                "blob": JSONValue(version.blob),
                "pages": SecureDriveRun.toJSON(version.pages),
                "key": JSONValue(SecureDriveCrypto.base64(version.key)),
                "size": .int(version.size),
                "sha256": JSONValue(version.sha256),
            ]))
        }
    }

    public func removeDevice(_ deviceId: String, progress: ((Int, Int) -> Void)? = nil) throws {
        if deviceId == state.deviceId { throw SecureDriveError("cannot_remove_self") }
        try transact { txn in
            guard var m = membership else { throw SecureDriveError("unknown_drive") }
            guard let target = deviceMap[deviceId], !target.removed else { throw SecureDriveError("unknown_device") }
            let next = SecureDriveEpochKey.fresh(number: (m.currentEpoch?.number ?? 0) + 1)
            m.epochs.append(next)
            state.update(id) { $0 = m }
            let file = txn.file
            for device in devices {
                if device.removed || device.id == deviceId || device.id == state.deviceId || device.mailbox < 0 { continue }
                let unit = try keyringUnit(device, mailbox: device.mailbox, approveKey: [], inviteId: "")
                let mailbox = device.mailbox
                txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), unit) && file.sync() }
            }
            if target.mailbox >= 0 {
                let mailbox = target.mailbox
                txn.afterCommit.append { file.write(at: SecureDriveLayout.mailboxOffset(mailbox), CryptoBytes.random(SecureDriveLayout.slotBytes)) && file.sync() }
            }
            txn.bothRootSlots = true
            txn.records.append(("device_removed", [
                "device": JSONValue(deviceId),
                "name": JSONValue(target.name),
                "epoch": JSONValue(next.number),
                "keyId": JSONValue(next.id),
                "upTo": .int(m.heads[deviceId]?.seq ?? -1),
            ]))
        }
        if files.isEmpty { return }
        try transact { txn in
            guard let m = membership else { throw SecureDriveError("unknown_drive") }
            let current = files
            let total = current.reduce(0) { $0 + $1.versions.count }
            var done = 0
            for entry in current {
                var moved = Set<String>()
                var changes = [JSONValue]()
                for version in entry.versions {
                    done += 1
                    progress?(done, total)
                    if moved.contains(version.blob) { continue }
                    let runs = try allocate(txn, count: SecureDriveRun.pages(version.pages), contiguous: false)
                    let target = SecureDriveRun.expand(runs)
                    let key = CryptoBytes.random(32)
                    let blob = SecureDriveCrypto.randomId()
                    let chunks = Self.chunkCount(version.size)
                    let source = SecureDriveRun.expand(version.pages)
                    var intact = Int64(source.count) == chunks
                    var i: Int64 = 0
                    while intact && i < chunks {
                        let from = source[Int(i)]
                        guard var plain = SecureDriveCrypto.openUnit(version.key, Self.blobAad(m.driveId, version.blob, i, i == chunks - 1, from),
                                                                     txn.file.read(at: from * Self.page, count: SecureDriveLayout.pageBytes)) else {
                            intact = false
                            break
                        }
                        let to = target[Int(i)]
                        let unit = SecureDriveCrypto.sealUnit(key, Self.blobAad(m.driveId, blob, i, i == chunks - 1, to), plain, size: SecureDriveLayout.pageBytes)
                        CryptoBytes.wipe(&plain)
                        guard let unit, txn.file.write(at: to * Self.page, unit) else { throw SecureDriveError("drive_write_failed") }
                        if i % 256 == 255 { txn.renew?() }
                        i += 1
                    }
                    if !intact {
                        for page in target { txn.reserved.remove(page) }
                        addIssue("content_damaged", "", entry.name)
                        continue
                    }
                    moved.insert(version.blob)
                    changes.append(.object([
                        "old": JSONValue(version.blob),
                        "blob": JSONValue(blob),
                        "pages": SecureDriveRun.toJSON(runs),
                        "key": JSONValue(SecureDriveCrypto.base64(key)),
                    ]))
                    txn.freed.append(contentsOf: version.pages)
                    txn.renew?()
                }
                if !changes.isEmpty {
                    txn.records.append(("file_rekeyed", ["file": JSONValue(entry.id), "name": JSONValue(entry.name), "versions": .array(changes)]))
                }
            }
            if txn.records.isEmpty { txn.records.append(("drive_rekeyed", [:])) }
        }
    }

    public func activityCsv() -> String {
        let sorted = records.enumerated().sorted { a, b in
            a.element.time != b.element.time ? a.element.time < b.element.time : a.offset < b.offset
        }.map(\.element)
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = TimeZone(identifier: "UTC")
        var lines = ["time,device,activity,record,hash"]
        for record in sorted {
            let when = formatter.string(from: Date(timeIntervalSince1970: Double(record.time) / 1000))
            lines.append([
                when,
                Self.csvField(deviceName(record.device)),
                Self.csvField(SecureDriveText.describe(record) { self.deviceName($0) }),
                String(record.seq + 1),
                record.hash,
            ].joined(separator: ","))
        }
        return lines.joined(separator: "\n") + "\n"
    }

    public func describe(_ record: SecureDriveRecord) -> String {
        SecureDriveText.describe(record) { self.deviceName($0) }
    }

    public func issueText(_ issue: SecureDriveIssue) -> String {
        SecureDriveText.issueText(issue) { self.deviceName($0) }
    }

    static func csvField(_ text: String) -> String {
        if text.contains(",") || text.contains("\"") || text.contains("\n") {
            return "\"" + text.replacingOccurrences(of: "\"", with: "\"\"") + "\""
        }
        return text
    }
}
