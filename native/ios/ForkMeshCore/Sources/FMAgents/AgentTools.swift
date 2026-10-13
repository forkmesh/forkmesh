import FMBase
import Foundation

/// One tool the model may call, and the code that runs it.
///
/// Tools are provider-neutral: `schema` is plain JSON Schema, which the
/// Anthropic, OpenAI and Gemma formats all accept (each wraps it differently;
/// see AgentProvider.swift).
public struct AgentTool {
    public let name: String
    public let description: String

    /// JSON Schema for the arguments object.
    public let schema: JSONObject

    /// Executes the call and returns text for the model. Throwing is fine:
    /// `AgentToolset.invoke` converts it into an error result the model can
    /// read and recover from, which is far more useful than ending the run.
    public let run: @MainActor (JSONObject) async throws -> String

    /// Whether a successful call can change the worktree. Drives the read-only
    /// mode gate.
    public let mutatesRepository: Bool

    public init(
        name: String,
        description: String,
        schema: JSONObject,
        mutatesRepository: Bool = false,
        run: @escaping @MainActor (JSONObject) async throws -> String
    ) {
        self.name = name
        self.description = description
        self.schema = schema
        self.mutatesRepository = mutatesRepository
        self.run = run
    }
}

/// Result of running one tool call.
public struct AgentToolResult: Equatable, Sendable {
    public let callId: String
    public let name: String
    public let output: String
    public let isError: Bool

    public init(callId: String, name: String, output: String, isError: Bool = false) {
        self.callId = callId
        self.name = name
        self.output = output
        self.isError = isError
    }
}

/// A bad tool argument.
struct AgentToolArgumentError: Error, CustomStringConvertible {
    let message: String
    var description: String { "Invalid argument(s): \(message)" }
}

/// The tools a phone can honestly offer.
///
/// There is no shell here, and that is the design rather than a gap: iOS
/// forbids executing code the app did not ship, so `execute_command`,
/// `run_tests` and `lint_project`, which the desktop agent has, cannot exist
/// on mobile. Everything below runs against the owned Git engine through an
/// `AgentWorkspace`, which enforces the path policy.
///
/// Tool names match `tools/forkmesh_coding_agent.py` so a transcript reads the
/// same whichever runtime produced it.
@MainActor
public final class AgentToolset {
    /// A tool result longer than this is truncated. Whole-file reads of a large
    /// file otherwise evict the rest of the conversation from the context
    /// window for little benefit.
    nonisolated public static let maxToolOutputChars = 24000

    /// Bounds for `grep`, so a search over a big repository stays quick and
    /// its result stays readable on a phone.
    nonisolated public static let maxGrepMatches = 80
    nonisolated public static let maxGrepFiles = 2000

    public let workspace: AgentWorkspace

    /// Plan mode: the model may look but not touch. Mutating calls are refused
    /// with an explanation rather than silently dropped, so the model can adapt.
    public let readOnly: Bool

    /// Commit identity. Set from the signed-in account before a run so commits
    /// are attributed to the person, not to a placeholder.
    public var author = AgentAuthor.placeholder

    /// Truncation bound for tool output, `maxToolOutputChars` unless a
    /// provider with a small context (the on-device model) lowers it.
    public var outputLimit = AgentToolset.maxToolOutputChars

    public init(_ workspace: AgentWorkspace, readOnly: Bool = false) {
        self.workspace = workspace
        self.readOnly = readOnly
    }

    public private(set) lazy var tools: [AgentTool] = makeTools()

    private lazy var byName: [String: AgentTool] = Dictionary(tools.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })

    private func makeTools() -> [AgentTool] {
        [
            AgentTool(
                name: "read_file",
                description: "Read a UTF-8 file from the repository. Returns the file with 1-based line numbers so you can refer to lines precisely.",
                schema: Self.object(["path": Self.string("Repository-relative path, e.g. lib/main.dart")], required: ["path"]),
                run: { [unowned self] args in try await self.readFile(Self.requireString(args, "path")) }
            ),
            AgentTool(
                name: "write_file",
                description: "Create a file, or replace one entirely. Prefer replace_text for edits to an existing file; a whole-file write of a large file is easy to get wrong.",
                schema: Self.object(
                    [
                        "path": Self.string("Repository-relative path"),
                        "content": Self.string("The complete new file contents"),
                    ],
                    required: ["path", "content"]
                ),
                mutatesRepository: true,
                run: { [unowned self] args in
                    try await self.writeFile(Self.requireString(args, "path"), Self.requireString(args, "content"))
                }
            ),
            AgentTool(
                name: "replace_text",
                description: "Replace an exact snippet in a file. The snippet must appear exactly once — include surrounding lines to make it unique. This is the preferred way to edit an existing file.",
                schema: Self.object(
                    [
                        "path": Self.string("Repository-relative path"),
                        "old": Self.string("Exact text to replace; must match once"),
                        "new": Self.string("Replacement text"),
                    ],
                    required: ["path", "old", "new"]
                ),
                mutatesRepository: true,
                run: { [unowned self] args in
                    try await self.replaceText(
                        Self.requireString(args, "path"),
                        Self.requireString(args, "old"),
                        Self.requireString(args, "new")
                    )
                }
            ),
            AgentTool(
                name: "list_directory",
                description: "List the direct children of a directory. Use \"\" or \".\" for the repository root.",
                schema: Self.object(["path": Self.string("Directory path, default the repository root")]),
                run: { [unowned self] args in try await self.listDirectory(Self.optionalString(args, "path") ?? "") }
            ),
            AgentTool(
                name: "grep",
                description: "Search file contents across the repository and return matching lines with their paths. Use this to locate code before reading whole files.",
                schema: Self.object(
                    [
                        "pattern": Self.string("Text or regular expression to find"),
                        "path": Self.string("Directory to search under, default the whole repo"),
                        "regex": .object([
                            "type": "boolean",
                            "description": "Treat pattern as a regular expression (default true)",
                        ]),
                    ],
                    required: ["pattern"]
                ),
                run: { [unowned self] args in
                    try await self.grep(
                        Self.requireString(args, "pattern"),
                        Self.optionalString(args, "path") ?? "",
                        args["regex"]?.boolValue ?? true
                    )
                }
            ),
            AgentTool(
                name: "git_status",
                description: "Show the current branch and which files are staged, modified, or untracked.",
                schema: Self.object([:]),
                run: { [unowned self] _ in try await self.gitStatus() }
            ),
            AgentTool(
                name: "git_commit",
                description: "Stage every change and commit it. Call this once the requested change is complete and you have reviewed it.",
                schema: Self.object(["message": Self.string("Commit message; a subject line, then why")], required: ["message"]),
                mutatesRepository: true,
                run: { [unowned self] args in try await self.gitCommit(Self.requireString(args, "message")) }
            ),
        ]
    }

    /// Run one call by name, converting every failure into readable text.
    ///
    /// A tool that throws is a normal event in an agent loop: a wrong path, a
    /// snippet that does not match. Returning the error to the model lets it
    /// correct itself; raising would end a run over something recoverable.
    public func invoke(_ callId: String, _ name: String, _ args: JSONObject) async -> AgentToolResult {
        guard let tool = byName[name] else {
            return AgentToolResult(
                callId: callId,
                name: name,
                output: "No tool named \"\(name)\". Available: \(tools.map(\.name).joined(separator: ", ")).",
                isError: true
            )
        }
        if readOnly && tool.mutatesRepository {
            return AgentToolResult(
                callId: callId,
                name: name,
                output: "This session is read-only, so \(name) did not run. Describe the change you would make instead.",
                isError: true
            )
        }
        do {
            let output = try await tool.run(args)
            return AgentToolResult(callId: callId, name: name, output: truncate(output))
        } catch let error as AgentWorkspaceError {
            return AgentToolResult(callId: callId, name: name, output: "\(name) failed: \(error.message)", isError: true)
        } catch {
            return AgentToolResult(callId: callId, name: name, output: "\(name) failed: \(error)", isError: true)
        }
    }

    // MARK: - Tool bodies

    private func readFile(_ path: String) async throws -> String {
        let file = try await workspace.readFile(path)
        if file.isBinary {
            return "\(path) is a binary file (\(file.bytes) bytes); it cannot be read as text."
        }
        if file.isTooLarge {
            return "\(path) is \(file.bytes) bytes, too large to read in full. Use grep to find the part you need."
        }
        let lines = AgentText.splitLines(file.text)
        if lines.isEmpty { return "\(path) is empty." }
        let width = String(lines.count).count
        var out = ""
        for (index, line) in lines.enumerated() {
            out += AgentText.padLeft(String(index + 1), width)
            out += "  "
            out += line
            out += "\n"
        }
        return out
    }

    private func writeFile(_ path: String, _ content: String) async throws -> String {
        try await workspace.writeFile(path, content)
        return "Wrote \(path) (\(AgentText.splitLines(content).count) lines)."
    }

    private func replaceText(_ path: String, _ oldText: String, _ newText: String) async throws -> String {
        if oldText.isEmpty {
            return "The \"old\" snippet is empty. Use write_file to create a file."
        }
        let file = try await workspace.readFile(path)
        if !file.isEditable {
            return "\(path) cannot be edited as text."
        }
        let occurrences = AgentText.occurrences(of: oldText, in: file.text)
        if occurrences == 0 {
            return "That snippet does not appear in \(path). Read the file and copy the exact text, including indentation."
        }
        if occurrences > 1 {
            return "That snippet appears \(occurrences) times in \(path). Include more surrounding lines so it matches exactly once."
        }
        try await workspace.writeFile(path, AgentText.replaceFirst(oldText, with: newText, in: file.text))
        return "Edited \(path)."
    }

    private func listDirectory(_ path: String) async throws -> String {
        let normalized = (path == "." ? "" : path).dartTrimmed
        let entries = try await workspace.listFiles(normalized)
        if entries.isEmpty {
            return normalized.isEmpty ? "The repository is empty." : "\(normalized) is empty."
        }
        let sorted = entries.sorted { a, b in
            if a.isDirectory != b.isDirectory { return a.isDirectory }
            return a.name.utf16.lexicographicallyPrecedes(b.name.utf16)
        }
        return sorted.map { entry in
            entry.isDirectory
                ? "\(entry.path)/"
                : "\(entry.path)  (\(entry.size) bytes\(entry.tracked ? "" : ", untracked"))"
        }.joined(separator: "\n")
    }

    private func grep(_ pattern: String, _ path: String, _ useRegex: Bool) async throws -> String {
        let matcher: NSRegularExpression
        do {
            matcher = try NSRegularExpression(pattern: useRegex ? pattern : NSRegularExpression.escapedPattern(for: pattern))
        } catch {
            return "That is not a valid regular expression: \(AgentText.regexProblem(error))"
        }

        let files = await walkFiles(path == "." ? "" : path)
        var hits: [String] = []
        var scanned = 0
        for file in files {
            if hits.count >= Self.maxGrepMatches { break }
            scanned += 1
            guard let content = try? await workspace.readFile(file) else {
                continue // unreadable file: skip rather than fail the whole search
            }
            if !content.isEditable { continue }
            let lines = AgentText.splitLines(content.text)
            for (index, line) in lines.enumerated() {
                let range = NSRange(line.startIndex..., in: line)
                if matcher.firstMatch(in: line, range: range) == nil { continue }
                hits.append("\(file):\(index + 1): \(line.dartTrimmed)")
                if hits.count >= Self.maxGrepMatches { break }
            }
        }
        if hits.isEmpty { return "No matches for \"\(pattern)\" in \(scanned) files." }
        let capped = hits.count >= Self.maxGrepMatches
            ? "\n… more matches were not listed; narrow the pattern or path."
            : ""
        return hits.joined(separator: "\n") + capped
    }

    /// Every file under `root`, breadth first, bounded by `maxGrepFiles`.
    private func walkFiles(_ root: String) async -> [String] {
        var found: [String] = []
        var queue: [String] = [root]
        while !queue.isEmpty && found.count < Self.maxGrepFiles {
            let directory = queue.removeFirst()
            guard let entries = try? await workspace.listFiles(directory) else { continue }
            for entry in entries {
                if entry.kind == .symlink { continue }
                if entry.isDirectory {
                    queue.append(entry.path)
                } else if found.count < Self.maxGrepFiles {
                    found.append(entry.path)
                }
            }
        }
        return found
    }

    private func gitStatus() async throws -> String {
        let status = try await workspace.status()
        let current = try await workspace.currentBranch()
        let head = current.isEmpty ? "no branch yet" : "on \(current)"
        if status.isClean { return "Clean worktree, \(head)." }
        var out = "Worktree \(head).\n"
        func section(_ label: String, _ changes: [AgentChange]) {
            if changes.isEmpty { return }
            out += "\(label):\n"
            for change in changes {
                out += "  \(change.kind.rawValue) \(change.path)\n"
            }
        }
        section("Staged", status.staged)
        section("Modified", status.unstaged)
        section("Untracked", status.untracked)
        return AgentText.trimRight(out)
    }

    private func gitCommit(_ message: String) async throws -> String {
        let trimmed = message.dartTrimmed
        if trimmed.isEmpty { return "A commit needs a message." }
        let status = try await workspace.status()
        if status.isClean {
            return "Nothing to commit; the worktree is clean."
        }
        try await workspace.stageAll()
        let id = try await workspace.commit(message: trimmed, author: author)
        let short = id.count >= 8 ? String(id.prefix(8)) : id
        let subject = trimmed.components(separatedBy: "\n").first ?? trimmed
        return "Committed \(short): \(subject)"
    }

    private func truncate(_ value: String) -> String {
        if value.utf16.count <= outputLimit { return value }
        return "\(AgentText.prefixUTF16(value, outputLimit))\n… output truncated at \(outputLimit) characters."
    }

    // MARK: - Small JSON Schema helpers

    static func object(_ properties: JSONObject, required: [String] = []) -> JSONObject {
        var schema: JSONObject = ["type": "object", "properties": .object(properties)]
        if !required.isEmpty { schema["required"] = .array(required.map { .string($0) }) }
        return schema
    }

    static func string(_ description: String) -> JSONValue {
        .object(["type": "string", "description": .string(description)])
    }

    static func requireString(_ args: JSONObject, _ key: String) throws -> String {
        guard let value = args[key]?.stringValue else {
            throw AgentToolArgumentError(message: "\"\(key)\" is required and must be a string.")
        }
        return value
    }

    static func optionalString(_ args: JSONObject, _ key: String) -> String? {
        args[key]?.stringValue
    }
}

/// String helpers the tools depend on, counted in UTF-16 code units.
enum AgentText {
    /// Splits on \n, \r\n and \r; a trailing terminator does not start
    /// another line.
    static func splitLines(_ text: String) -> [String] {
        var lines: [String] = []
        var current: [UInt16] = []
        var previousWasCR = false
        for unit in text.utf16 {
            if unit == 0x0A {
                if previousWasCR {
                    previousWasCR = false
                    continue
                }
                lines.append(String(decoding: current, as: UTF16.self))
                current.removeAll(keepingCapacity: true)
            } else if unit == 0x0D {
                lines.append(String(decoding: current, as: UTF16.self))
                current.removeAll(keepingCapacity: true)
                previousWasCR = true
                continue
            } else {
                current.append(unit)
            }
            previousWasCR = false
        }
        if !current.isEmpty { lines.append(String(decoding: current, as: UTF16.self)) }
        return lines
    }

    /// How many non-overlapping occurrences of `pattern` are in `text`.
    static func occurrences(of pattern: String, in text: String) -> Int {
        let needle = Array(pattern.utf16)
        let haystack = Array(text.utf16)
        guard !needle.isEmpty, needle.count <= haystack.count else { return 0 }
        var count = 0
        var index = 0
        while index + needle.count <= haystack.count {
            if haystack[index] == needle[0] && Array(haystack[index..<index + needle.count]) == needle {
                count += 1
                index += needle.count
            } else {
                index += 1
            }
        }
        return count
    }

    /// `text` with its first occurrence of `pattern` replaced.
    static func replaceFirst(_ pattern: String, with replacement: String, in text: String) -> String {
        let needle = Array(pattern.utf16)
        let haystack = Array(text.utf16)
        guard !needle.isEmpty, needle.count <= haystack.count else { return text }
        var index = 0
        while index + needle.count <= haystack.count {
            if haystack[index] == needle[0] && Array(haystack[index..<index + needle.count]) == needle {
                let result = Array(haystack[..<index]) + Array(replacement.utf16) + Array(haystack[(index + needle.count)...])
                return String(decoding: result, as: UTF16.self)
            }
            index += 1
        }
        return text
    }

    /// The first `end` UTF-16 code units of `text`.
    static func prefixUTF16(_ text: String, _ end: Int) -> String {
        String(decoding: Array(text.utf16.prefix(end)), as: UTF16.self)
    }

    /// `value` padded on the left with spaces to `width` characters.
    static func padLeft(_ value: String, _ width: Int) -> String {
        let pad = width - value.count
        return pad > 0 ? String(repeating: " ", count: pad) + value : value
    }

    /// `text` with trailing whitespace removed.
    static func trimRight(_ text: String) -> String {
        var scalars = Substring(text)
        while let last = scalars.last, last.isWhitespace { scalars.removeLast() }
        return String(scalars)
    }

    static func regexProblem(_ error: Error) -> String {
        let nsError = error as NSError
        if let detail = nsError.userInfo["NSInvalidValue"] as? String {
            return "invalid pattern \(detail)"
        }
        return nsError.localizedDescription
    }
}
