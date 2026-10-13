import FMLocalAI
import Foundation

public struct NextTaskFeedback: Codable, Equatable, Sendable {
    public struct Turn: Codable, Equatable, Sendable {
        public var role: String
        public var content: String

        public init(role: String, content: String) {
            self.role = role
            self.content = content
        }
    }

    public var verdict: String
    public var reason: String
    public var title: String
    public var task: String
    public var why: String
    public var repo: String
    public var atMs: Int
    public var prompt: [Turn]

    public init(
        verdict: String, reason: String = "", title: String, task: String, why: String = "", repo: String = "", atMs: Int,
        prompt: [Turn] = []
    ) {
        self.verdict = verdict
        self.reason = reason
        self.title = title
        self.task = task
        self.why = why
        self.repo = repo
        self.atMs = atMs
        self.prompt = prompt
    }

    public init(
        verdict: String, reason: String, title: String, task: String, why: String, repo: String, atMs: Int,
        messages: [LocalChatMessage]
    ) {
        self.init(
            verdict: verdict, reason: reason, title: title, task: task, why: why, repo: repo, atMs: atMs,
            prompt: messages.map { Turn(role: $0.role.rawValue, content: $0.content) })
    }
}

public final class NextTaskTrainingLog: @unchecked Sendable {
    public static let fileName = "next-task-feedback.jsonl"

    public let maxBytes: Int
    private let directory: () throws -> URL
    private let lock = NSLock()

    public init(directory: @escaping () throws -> URL, maxBytes: Int = 4_000_000) {
        self.directory = directory
        self.maxBytes = maxBytes
    }

    public static func forDevice() -> NextTaskTrainingLog {
        NextTaskTrainingLog(directory: {
            try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                .appendingPathComponent("NextTask", isDirectory: true)
        })
    }

    public var url: URL? { try? directory().appendingPathComponent(Self.fileName) }

    public func append(_ feedback: NextTaskFeedback) {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        guard var line = try? encoder.encode(feedback) else { return }
        line.append(0x0A)
        lock.lock()
        defer { lock.unlock() }
        guard let folder = try? directory() else { return }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let file = folder.appendingPathComponent(Self.fileName)
        if let handle = try? FileHandle(forWritingTo: file) {
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: line)
            try? handle.close()
        } else {
            try? line.write(to: file, options: .atomic)
        }
        let size = (try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? Int) ?? 0
        if size > maxBytes, let data = try? Data(contentsOf: file) {
            try? Self.trim(data, to: maxBytes / 2).write(to: file, options: .atomic)
        }
    }

    public func entries() -> [NextTaskFeedback] {
        lock.lock()
        defer { lock.unlock() }
        guard let url, let data = try? Data(contentsOf: url) else { return [] }
        let decoder = JSONDecoder()
        return data.split(separator: 0x0A).compactMap { try? decoder.decode(NextTaskFeedback.self, from: Data($0)) }
    }

    static func trim(_ data: Data, to most: Int) -> Data {
        if data.count <= most { return data }
        let start = data.index(data.endIndex, offsetBy: -most)
        guard let cut = data[start...].firstIndex(of: 0x0A) else { return Data() }
        return Data(data[data.index(after: cut)...])
    }
}
