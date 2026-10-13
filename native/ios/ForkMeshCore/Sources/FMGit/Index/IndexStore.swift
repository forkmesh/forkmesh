public final class GitIndexStore {
    public let gitDirectory: String
    public let limits: GitResourceLimits

    public init(_ gitDirectory: String, _ limits: GitResourceLimits) {
        self.gitDirectory = gitDirectory
        self.limits = limits
    }

    public var file: String { GitFS.join(gitDirectory, "index") }

    public func read() throws -> GitIndexV2 {
        if !GitFS.fileExists(file) { return try GitIndexV2([]) }
        return try GitIndexV2.parse(try GitFS.readBytes(file), limits)
    }

    public func write(_ index: GitIndexV2) throws {
        let destination = file
        let lock = "\(destination).lock"
        if !GitFS.createFileExclusive(lock) {
            throw GitException(.refLocked, "index lock already exists")
        }
        defer {
            if GitFS.fileExists(lock) { try? GitFS.deleteFile(lock) }
        }
        try GitFS.writeBytesFlushed(lock, index.encode())
        try GitFS.rename(lock, destination)
    }
}
