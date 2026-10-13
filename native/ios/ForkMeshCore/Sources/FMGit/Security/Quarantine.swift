import Foundation

/// Private operation storage for untrusted transport input.
///
/// No installation method is exposed on purpose: a reviewed caller validates
/// every staged object and performs its own audited rename.
public final class GitObjectQuarantine {
    public let directory: String
    private let cancellation: GitCancellationToken?
    private let maximumStagedBytes: Int

    private init(_ directory: String, _ cancellation: GitCancellationToken?, _ maximumStagedBytes: Int) {
        self.directory = directory
        self.cancellation = cancellation
        self.maximumStagedBytes = maximumStagedBytes
    }

    public static func begin(
        _ gitDirectory: String,
        cancellation: GitCancellationToken? = nil,
        limits: GitResourceLimits? = nil
    ) throws -> GitObjectQuarantine {
        let root = GitFS.join(gitDirectory, "forkmesh-quarantine")
        try GitFS.createDirectories(root)
        var generator = SystemRandomNumberGenerator()
        for attempt in 0..<32 {
            let random = String(UInt32.random(in: 0...UInt32.max, using: &generator), radix: 16)
            let directory = GitFS.join(root, "\(GitFS.nowMicros())-\(random)-\(attempt)")
            if GitFS.directoryExists(directory) { continue }
            try GitFS.createDirectory(directory)
            return GitObjectQuarantine(
                directory,
                cancellation,
                limits?.maxInflatedBytes ?? GitResourceLimits.defaultMaxInflatedBytes
            )
        }
        throw GitException(.quarantineRejected, "could not allocate operation quarantine")
    }

    @discardableResult
    public func stage(_ name: String, _ bytes: [UInt8]) throws -> String {
        if !Self.isValidName(name) {
            throw GitException(.quarantineRejected, "quarantine entry name is invalid")
        }
        do {
            try cancellation?.throwIfCancelled()
            if bytes.count > maximumStagedBytes {
                throw GitException(.resourceLimitExceeded, "quarantine entry exceeds the operation limit")
            }
            let destination = GitFS.join(directory, name)
            if GitFS.fileExists(destination) {
                throw GitException(.quarantineRejected, "quarantine entry already exists")
            }
            try GitFS.writeBytesFlushed(destination, bytes)
            try cancellation?.throwIfCancelled()
            return destination
        } catch {
            try? reject()
            throw error
        }
    }

    public func reject() throws {
        if GitFS.directoryExists(directory) { try GitFS.deleteRecursive(directory) }
    }

    public static func recover(_ gitDirectory: String) throws {
        let root = GitFS.join(gitDirectory, "forkmesh-quarantine")
        if !GitFS.directoryExists(root) { return }
        for name in try GitFS.listNames(root) {
            try GitFS.deleteRecursive(GitFS.join(root, name))
        }
    }

    /// `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`
    private static func isValidName(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        if bytes.isEmpty || bytes.count > 128 { return false }
        func alphanumeric(_ byte: UInt8) -> Bool {
            (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a) || (byte >= 0x61 && byte <= 0x7a)
        }
        if !alphanumeric(bytes[0]) { return false }
        for byte in bytes.dropFirst() where !(alphanumeric(byte) || byte == 0x2e || byte == 0x5f || byte == 0x2d) {
            return false
        }
        return true
    }
}
