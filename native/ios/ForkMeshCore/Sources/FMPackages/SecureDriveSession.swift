import FMBase
import FMCrypto
import Foundation

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

public struct SecureDriveSummary: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var pending: Bool
    public var container: String
}

public struct SecureDriveRecordRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var device: String
    public var deviceName: String
    public var action: String
    public var text: String
    public var time: Int64
    public var seq: Int64
}

public struct SecureDriveDeviceRow: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var safety: String
    public var removed: Bool
    public var root: Bool
    public var addedAt: Int64
    public var addedBy: String
    public var isThisDevice: Bool
}

public struct SecureDriveSnapshot: Equatable, Sendable {
    public var id: String = ""
    public var label: String = ""
    public var container: String = ""
    public var pending = false
    public var joinSafety = ""
    public var joinRequestMissing = false
    public var removed = false
    public var scanned = false
    public var files: [SecureDriveFileEntry] = []
    public var records: [SecureDriveRecordRow] = []
    public var requests: [SecureDriveJoinRequest] = []
    public var devices: [SecureDriveDeviceRow] = []
    public var invites: [SecureDriveInvite] = []
    public var issues: [String] = []
    public var containerBytes: Int64 = 0
    public var usedBytes: Int64 = 0
    public var thisDeviceId = ""
    public var thisDeviceSafety = ""
    public var names: [String: String] = [:]

    public init() {}

    public func name(_ device: String) -> String {
        if let name = names[device] { return name }
        return device.isEmpty ? "A device" : "Device \(device.prefix(8))"
    }

    public var members: Int { devices.filter { !$0.removed }.count }

    public func file(_ id: String) -> SecureDriveFileEntry? { files.first { $0.id == id } }
}

public enum SecureDriveEditorText {
    public static let newDocument = Array("<html><body><p></p></body></html>".utf8)
    public static let newSpreadsheet: [UInt8] = []
    public static var newDrawing: [UInt8] { SecureDriveDrawing().toSvg() }

    public static func editable(_ kind: String) -> Bool {
        kind == "document" || kind == "text" || kind == "spreadsheet" || kind == "drawing"
    }

    public static func kindLabel(_ kind: String) -> String {
        switch kind {
        case "document": return "Document"
        case "spreadsheet": return "Spreadsheet"
        case "drawing": return "Drawing"
        case "text": return "Text"
        case "image": return "Picture"
        default: return "File"
        }
    }

    public static func symbol(_ kind: String) -> String {
        switch kind {
        case "document": return "doc.richtext"
        case "spreadsheet": return "tablecells"
        case "drawing": return "scribble.variable"
        case "text": return "doc.plaintext"
        case "image": return "photo"
        default: return "doc"
        }
    }

    public static func untitled(_ kind: String, existing: [String]) -> String {
        let base: String
        let ext: String
        switch kind {
        case "document": base = "Untitled document"; ext = "html"
        case "spreadsheet": base = "Untitled spreadsheet"; ext = "csv"
        default: base = "Untitled drawing"; ext = "svg"
        }
        let taken = Set(existing.map { $0.lowercased() })
        var candidate = "\(base).\(ext)"
        var n = 2
        while taken.contains(candidate.lowercased()) {
            candidate = "\(base) \(n).\(ext)"
            n += 1
        }
        return candidate
    }

    public static func plural(_ count: Int, _ word: String, _ suffix: String = "s") -> String {
        "\(count) \(word)\(count == 1 ? "" : suffix)"
    }
}

public final class SecureDriveService: @unchecked Sendable {
    public let directory: URL
    let queue = DispatchQueue(label: "forkmesh.secure-drive", qos: .userInitiated)
    let state: SecureDriveLocalState
    let storage: SecureDriveStorage
    let timing: SecureDriveLeaseTiming
    var drives: [String: SecureDrive] = [:]

    public init(directory: URL, timing: SecureDriveLeaseTiming = .standard, makeStorage: (SecureDriveLocalState) -> SecureDriveStorage) {
        self.directory = directory
        self.timing = timing
        state = SecureDriveLocalState(directory: directory)
        storage = makeStorage(state)
    }

    public init(directory: URL, testingKdf: SecureDriveKdfCost, timing: SecureDriveLeaseTiming, storage: SecureDriveStorage = SecureDriveLocalStorage()) {
        self.directory = directory
        self.timing = timing
        state = SecureDriveLocalState(directory: directory, testingKdf: testingKdf)
        self.storage = storage
    }

    public func run<T: Sendable>(_ body: @escaping @Sendable (SecureDriveService) throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<T, Error>) in
            queue.async {
                do {
                    continuation.resume(returning: try body(self))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    public var exists: Bool { state.exists }
    public var isUnlocked: Bool { state.isUnlocked }
    public var deviceName: String { state.deviceName }
    public var localState: SecureDriveLocalState { state }

    public func setup(name: String, passphrase: String) throws {
        try state.create(passphrase: passphrase, deviceName: name)
    }

    public func unlock(passphrase: String) throws {
        try state.unlock(passphrase: passphrase)
        drives = [:]
    }

    public func lock() {
        drives = [:]
        state.lock()
    }

    public func changePassphrase(current: String, next: String) throws {
        try state.changePassphrase(current: current, next: next)
    }

    public func summaries() -> [SecureDriveSummary] {
        state.drives.map { SecureDriveSummary(id: $0.id, label: $0.label, pending: $0.isPending, container: $0.container) }
    }

    public func drive(_ id: String) throws -> SecureDrive {
        guard state.isUnlocked else { throw SecureDriveError("locked") }
        guard state.membership(id) != nil else { throw SecureDriveError("unknown_drive") }
        if let drive = drives[id] { return drive }
        let drive = SecureDrive(state: state, id: id, storage: storage, timing: timing)
        drives[id] = drive
        return drive
    }

    public func snapshot(_ id: String, scan: Bool, force: Bool = false) throws -> SecureDriveSnapshot {
        let drive = try drive(id)
        if scan && (force || !drive.scanned || drive.changedSinceScan) { try drive.scan() }
        return snapshot(of: drive)
    }

    public func snapshot(of drive: SecureDrive) -> SecureDriveSnapshot {
        var shot = SecureDriveSnapshot()
        guard let m = drive.membership else { return shot }
        shot.id = drive.id
        shot.label = m.label
        shot.container = m.container
        shot.pending = m.isPending
        shot.joinSafety = drive.joinSafetyNumber
        shot.joinRequestMissing = drive.joinRequestMissing
        shot.removed = drive.removedFromDrive
        shot.scanned = drive.scanned
        shot.thisDeviceId = state.deviceId
        shot.thisDeviceSafety = drive.thisDeviceSafetyNumber
        if m.isPending { return shot }
        var names: [String: String] = [:]
        for device in drive.devices { names[device.id] = drive.deviceName(device.id) }
        names[state.deviceId] = drive.deviceName(state.deviceId)
        shot.names = names
        shot.files = drive.files
        shot.records = drive.records.map { record in
            SecureDriveRecordRow(id: record.hash, device: record.device, deviceName: drive.deviceName(record.device), action: record.action,
                                 text: drive.describe(record), time: record.time, seq: record.seq)
        }.sorted { $0.time != $1.time ? $0.time > $1.time : $0.id < $1.id }
        shot.requests = drive.requests
        shot.devices = drive.devices.map { device in
            SecureDriveDeviceRow(id: device.id, name: drive.deviceName(device.id), safety: drive.deviceSafetyNumber(device.id), removed: device.removed,
                                 root: device.root, addedAt: device.addedAt, addedBy: drive.deviceName(device.addedBy), isThisDevice: device.id == state.deviceId)
        }
        shot.invites = drive.openInvites
        var issues = [String]()
        if drive.removedFromDrive {
            issues.append("This iPhone was removed from the drive. It can still read what it opened before, but nothing new.")
        }
        issues += drive.issues.map { drive.issueText($0) }
        shot.issues = issues
        shot.containerBytes = drive.containerBytes
        shot.usedBytes = drive.usedBytes
        return shot
    }

    public func create(label: String, at path: String) throws -> String {
        try SecureDrive.create(state: state, storage: storage, container: path, label: label, timing: timing)
    }

    public func relocate(_ id: String, to path: String, bookmark: [UInt8]?) throws {
        state.update(id) { $0.container = path }
        if let bookmark { state.bookmarks[id] = bookmark } else { state.bookmarks[id] = nil }
        drives[id] = nil
        try state.save()
    }

    public func join(path: String, bookmark: [UInt8]?, code: String) throws -> (id: String, safety: String) {
        if let bookmark, let pending = storage as? SecureDriveBookmarkHolder { pending.hold(path, bookmark) }
        defer { (storage as? SecureDriveBookmarkHolder)?.release(path) }
        let made = try SecureDrive.requestJoin(state: state, storage: storage, container: path, code: code)
        if let bookmark {
            state.bookmarks[made.id] = bookmark
            try state.save()
        }
        return made
    }

    public func cancelJoin(_ id: String) throws {
        try drive(id).cancelJoin()
        drives[id] = nil
    }

    public func forget(_ id: String) throws {
        drives[id] = nil
        state.forget(id)
        try state.save()
    }

    @discardableResult
    public func add(_ id: String, name: String, content: SecureDriveContent, kind: String = "") throws -> String {
        try drive(id).addFile(name, content, kind: kind)
    }

    public func save(_ id: String, file: String, bytes: [UInt8], base: String) throws {
        try drive(id).saveFile(file, .bytes(bytes), base: base)
    }

    public func read(_ id: String, file: String, version: Int, purpose: String) throws -> [UInt8] {
        try drive(id).read(file, version: version, purpose: purpose)
    }

    public func read(_ id: String, file: String, version: Int, purpose: String, to url: URL) throws {
        guard FileManager.default.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600]),
              let handle = try? FileHandle(forWritingTo: url) else { throw SecureDriveError("write_failed") }
        do {
            try drive(id).readFile(file, version: version, purpose: purpose) { part in
                try handle.write(contentsOf: Data(part))
            }
            try handle.close()
        } catch {
            try? handle.close()
            SecureDriveScratchFiles.shred(url)
            throw error
        }
    }

    public func rename(_ id: String, file: String, to name: String) throws { try drive(id).renameFile(file, to: name) }

    public func delete(_ id: String, file: String) throws { try drive(id).deleteFile(file) }

    public func restore(_ id: String, file: String, version: Int) throws { try drive(id).restoreVersion(file, version: version) }

    public func invite(_ id: String) throws -> String { try drive(id).createInvite().code }

    public func cancelInvites(_ id: String) throws {
        let drive = try drive(id)
        for invite in drive.openInvites { try drive.revokeInvite(invite.id) }
    }

    public func approve(_ id: String, mailbox: Int) throws { try drive(id).approve(mailbox: mailbox) }

    public func deny(_ id: String, mailbox: Int) throws { try drive(id).deny(mailbox: mailbox) }

    public func remove(_ id: String, device: String, progress: @escaping (Int, Int) -> Void) throws {
        try drive(id).removeDevice(device, progress: progress)
    }

    public func exportActivity(_ id: String) throws -> String {
        let drive = try drive(id)
        try drive.note("log_exported", [:])
        return drive.activityCsv()
    }
}

public protocol SecureDriveBookmarkHolder: AnyObject {
    func hold(_ container: String, _ bookmark: [UInt8])
    func release(_ container: String)
}

public enum SecureDriveScratchFiles {
    public static func shred(_ url: URL) {
        let path = url.path
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: path),
              attributes[.type] as? FileAttributeType == .typeRegular else { return }
        if let handle = try? FileHandle(forWritingTo: url) {
            var left = (attributes[.size] as? NSNumber)?.int64Value ?? 0
            let zeros = Data(count: 1 << 16)
            try? handle.seek(toOffset: 0)
            while left > 0 {
                let count = Int(min(left, Int64(zeros.count)))
                try? handle.write(contentsOf: zeros.prefix(count))
                left -= Int64(count)
            }
            try? handle.synchronize()
            try? handle.close()
        }
        _ = unlink(path)
    }

    public static func makeFolder(in parent: URL) -> URL? {
        let folder = parent.appendingPathComponent(SecureDriveCrypto.randomId(), isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            return folder
        } catch {
            return nil
        }
    }

    public static func clear(_ folder: URL, inside parent: URL) {
        let parentPath = parent.standardizedFileURL.path
        let path = folder.standardizedFileURL.path
        guard path.hasPrefix(parentPath + "/"), !path.dropFirst(parentPath.count + 1).contains("/") else { return }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: path)) ?? []
        for name in names where !name.contains("/") {
            shred(URL(fileURLWithPath: path).appendingPathComponent(name))
        }
        _ = rmdir(path)
    }

    public static func clearAll(_ parent: URL) {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: parent.path)) ?? []
        for name in names where !name.contains("/") && name.count == 32 {
            clear(parent.appendingPathComponent(name, isDirectory: true), inside: parent)
        }
    }
}
