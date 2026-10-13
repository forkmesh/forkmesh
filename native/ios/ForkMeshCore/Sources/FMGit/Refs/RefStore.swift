public struct GitRefName: Hashable, CustomStringConvertible, Sendable {
    public let value: String

    private init(unchecked value: String) { self.value = value }

    public static func branch(_ value: String) throws -> GitRefName {
        if !value.u8HasPrefix("refs/heads/") {
            throw GitException(.invalidRef, "expected branch ref")
        }
        try validate(value)
        return GitRefName(unchecked: value)
    }

    public static func parse(_ value: String) throws -> GitRefName {
        if !value.u8HasPrefix("refs/heads/") && !value.u8HasPrefix("refs/tags/")
            && !value.u8HasPrefix("refs/remotes/")
        {
            throw GitException(.invalidRef, "unsupported ref namespace")
        }
        try validate(value)
        return GitRefName(unchecked: value)
    }

    private static func validate(_ value: String) throws {
        let bytes = Array(value.utf8)
        // The forbidden character class is [\x00-\x20~^:?*\\[\\]: controls and
        // space, `~ ^ : ? *`, backslash and `[` (but not `]`).
        let forbidden = bytes.contains { byte in
            byte <= 0x20 || byte == 0x7e || byte == 0x5e || byte == 0x3a || byte == 0x3f
                || byte == 0x2a || byte == 0x5c || byte == 0x5b
        }
        if bytes.count > GitResourceLimits.hardMaxRefNameBytes
            || bytes.last == 0x2f
            || bytes.last == 0x2e
            || value.u8HasSuffix(".lock")
            || value.u8Contains("..")
            || value.u8Contains("//")
            || forbidden
        {
            throw GitException(.invalidRef, "invalid ref spelling")
        }
        for component in value.u8Split(0x2f) {
            let raw = Array(component.utf8)
            if raw.isEmpty || raw == [0x2e] || raw == [0x2e, 0x2e] {
                throw GitException(.invalidRef, "invalid ref component")
            }
        }
    }

    public static func == (left: GitRefName, right: GitRefName) -> Bool { left.value.sameText(right.value) }

    public func hash(into hasher: inout Hasher) { hasher.combine(Array(value.utf8)) }

    public var description: String { value }
}

public struct GitSymbolicRef: Hashable, Sendable {
    public let target: String

    public init(_ target: String) { self.target = target }

    public static func == (left: GitSymbolicRef, right: GitSymbolicRef) -> Bool {
        left.target.sameText(right.target)
    }

    public func hash(into hasher: inout Hasher) { hasher.combine(Array(target.utf8)) }
}

public enum GitRefUpdateResult: Sendable { case updated, nonFastForward }

public final class RefStore {
    public let gitDirectory: String

    public init(_ gitDirectory: String) { self.gitDirectory = gitDirectory }

    private func fileFor(_ ref: GitRefName) -> String { GitFS.join(gitDirectory, ref.value) }

    public func readDirect(_ ref: GitRefName) throws -> GitObjectId? {
        try readOptional(fileFor(ref))
    }

    /// Lists direct refs under `prefix` (for example `refs/heads/`) from loose
    /// files and `packed-refs`, with loose values shadowing packed ones.
    ///
    /// Symbolic refs under the prefix are skipped. The result is bounded by `limits`.
    public func listDirect(_ prefix: String, _ limits: GitResourceLimits) throws -> [String: GitObjectId] {
        if !prefix.u8HasPrefix("refs/") || !prefix.u8HasSuffix("/") {
            throw GitException(.invalidRef, "ref prefix is invalid")
        }
        var result = [String: GitObjectId]()
        let packed = GitFS.join(gitDirectory, "packed-refs")
        if GitFS.fileExists(packed) {
            let bytes = try GitFS.readBytes(packed)
            if bytes.count > 16 * 1024 * 1024 {
                throw GitException(.resourceLimitExceeded, "packed-refs exceeds the configured limit")
            }
            let refs = try PackedRefs.parse(bytes, limits)
            for (name, entry) in refs.entries where name.u8HasPrefix(prefix) {
                result[name] = entry.objectId
            }
        }
        let root = GitFS.join(gitDirectory, prefix.u8Substring(0, prefix.utf8.count - 1))
        if GitFS.directoryExists(root) {
            let rootLength = root.utf8.count
            try GitFS.walk(root) { path, type in
                if type != .file { return }
                let relative = path.u8Substring(rootLength + 1)
                if relative.u8HasSuffix(".lock") { return }
                guard let ref = try? GitRefName.parse(prefix + relative) else { return }
                if result.count >= limits.maxRefCount {
                    throw GitException(.resourceLimitExceeded, "local refs exceed the configured limit")
                }
                let text = try readText(path).dartTrim()
                if text.u8HasPrefix("ref: ") { return }
                result[ref.value] = try GitObjectId.parseSha1(text)
            }
        }
        return result
    }

    public func update(_ ref: GitRefName, expectedOld: GitObjectId?, next: GitObjectId) throws -> GitRefUpdateResult {
        let destination = fileFor(ref)
        let lock = "\(destination).lock"
        try GitFS.createDirectories(GitFS.parent(lock))
        if !GitFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "ref lock already exists")
        }
        defer {
            if GitFS.fileExists(lock) { try? GitFS.deleteFile(lock) }
        }
        let current = try readOptional(destination)
        if current != expectedOld { return .nonFastForward }
        try GitFS.writeStringFlushed(lock, "\(next.hex)\n")
        try GitFS.rename(lock, destination)
        return .updated
    }

    private func readOptional(_ file: String) throws -> GitObjectId? {
        if !GitFS.fileExists(file) { return nil }
        let value = try readText(file).dartTrim()
        if value.u8HasPrefix("ref: ") {
            throw GitException(.invalidRef, "expected direct ref")
        }
        return try GitObjectId.parseSha1(value)
    }
}

/// `File(path).readAsString()`: strict UTF-8.
func readText(_ path: String) throws -> String {
    guard let text = utf8DecodeStrict(try GitFS.readBytes(path)) else {
        throw GitFormatError("file is not valid UTF-8: \(path)")
    }
    return text
}

public struct PackedRef: Sendable {
    public let objectId: GitObjectId
    public let peeled: GitObjectId?

    public init(_ objectId: GitObjectId, _ peeled: GitObjectId?) {
        self.objectId = objectId
        self.peeled = peeled
    }
}

public struct PackedRefs: Sendable {
    let entries: [String: PackedRef]

    public static func parse(_ bytes: [UInt8], _ limits: GitResourceLimits) throws -> PackedRefs {
        guard let text = asciiDecode(bytes) else {
            throw GitFormatError("packed-refs is not ASCII")
        }
        var entries = [String: PackedRef]()
        var previousName: String?
        for line in dartSplitLines(text) {
            if line.utf8.isEmpty || line.u8HasPrefix("#") { continue }
            if line.u8HasPrefix("^") {
                guard let name = previousName, let current = entries[name], current.peeled == nil else {
                    throw GitException(.invalidRef, "invalid peeled ref")
                }
                entries[name] = PackedRef(current.objectId, try GitObjectId.parseSha1(line.u8Substring(1)))
                continue
            }
            let separator = line.u8IndexOf(" ")
            if separator != 40 || separator == line.utf8.count - 1 || entries.count >= limits.maxRefCount {
                throw GitException(.invalidRef, "packed ref record is invalid")
            }
            let name = line.u8Substring(separator + 1)
            _ = try GitRefName.parse(name)
            if let previous = previousName, previous.dartCompare(name) >= 0 {
                throw GitException(.invalidRef, "packed refs are not sorted")
            }
            entries[name] = PackedRef(try GitObjectId.parseSha1(line.u8Substring(0, separator)), nil)
            previousName = name
        }
        return PackedRefs(entries: entries)
    }

    public func lookup(_ name: String) -> PackedRef? { entries[name] }
}
