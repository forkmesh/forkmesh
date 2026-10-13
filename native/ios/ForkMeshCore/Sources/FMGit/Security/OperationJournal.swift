import Foundation

private let journalOperationPattern = GitRegex("^[a-z][a-z-]{0,63}\\z")
private let journalEntryPattern = GitRegex(
    "^forkmesh-journal-v1\\noperation=[a-z][a-z-]{0,63}\\nstate=prepared\\n\\z"
)

public final class GitOperationJournal {
    private let file: String

    private init(_ file: String) { self.file = file }

    public static func begin(_ gitDirectory: String, _ operation: String) throws -> GitOperationJournal {
        if !journalOperationPattern.hasMatch(operation) {
            throw GitArgumentError("operation")
        }
        let directory = GitFS.join(gitDirectory, "forkmesh-journal")
        try GitFS.createDirectories(directory)
        for attempt in 0..<32 {
            let file = GitFS.join(directory, "\(GitFS.nowMicros())-\(attempt).journal")
            if !GitFS.createFileExclusive(file) { continue }
            try GitFS.writeStringFlushed(
                file,
                "forkmesh-journal-v1\noperation=\(operation)\nstate=prepared\n"
            )
            return GitOperationJournal(file)
        }
        throw GitException(.refLocked, "could not allocate operation journal")
    }

    public func complete() throws {
        if GitFS.fileExists(file) { try GitFS.deleteFile(file) }
    }

    public static func recover(_ gitDirectory: String) throws {
        let directory = GitFS.join(gitDirectory, "forkmesh-journal")
        if !GitFS.directoryExists(directory) { return }
        for name in try GitFS.listNames(directory) {
            let entity = GitFS.join(directory, name)
            if GitFS.entityType(entity) != .file || !name.u8HasSuffix(".journal") {
                throw GitException(.operationInterrupted, "operation journal directory is malformed")
            }
            let bytes = try GitFS.readBytes(entity)
            if bytes.count > 1024 || !isValid(bytes) {
                throw GitException(.operationInterrupted, "operation journal entry is malformed")
            }
            try GitFS.deleteFile(entity)
        }
    }

    private static func isValid(_ bytes: [UInt8]) -> Bool {
        journalEntryPattern.hasMatch(latin1Decode(bytes))
    }
}
