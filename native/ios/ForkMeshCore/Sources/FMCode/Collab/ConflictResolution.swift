import FMBase
import Foundation

public enum ConflictChoice: String, CaseIterable, Sendable, Codable {
    case ours, theirs, both, bothTheirsFirst

    public var title: String {
        switch self {
        case .ours: return "Keep ours"
        case .theirs: return "Keep theirs"
        case .both: return "Keep both"
        case .bothTheirsFirst: return "Both, theirs first"
        }
    }
}

public struct ConflictHunk: Equatable, Sendable, Identifiable {
    public let id: Int
    public let ours: [String]
    public let base: [String]?
    public let theirs: [String]
    public let oursLabel: String
    public let theirsLabel: String
    public let startLine: Int
}

public enum ConflictSegment: Equatable, Sendable {
    case text([String])
    case conflict(ConflictHunk)
}

public struct ConflictFile: Equatable, Sendable {
    public let path: String
    public let segments: [ConflictSegment]
    public let trailingNewline: Bool

    public var hunks: [ConflictHunk] {
        segments.compactMap { if case .conflict(let hunk) = $0 { return hunk } else { return nil } }
    }

    public var hasConflicts: Bool { !hunks.isEmpty }
}

public enum ConflictResolution {
    public static let malformed = "The conflict markers in this file are incomplete, so it cannot be resolved here."

    public static func parse(path: String, text: String) -> Result<ConflictFile, CollabSignError> {
        var lines = text.components(separatedBy: "\n")
        let trailing = text.hasSuffix("\n")
        if trailing { lines.removeLast() }
        var segments: [ConflictSegment] = []
        var plain: [String] = []
        var index = 0
        var hunkId = 0
        while index < lines.count {
            let line = lines[index]
            guard line.hasPrefix("<<<<<<<") else {
                plain.append(line)
                index += 1
                continue
            }
            if !plain.isEmpty { segments.append(.text(plain)); plain = [] }
            let start = index + 1
            let oursLabel = String(line.dropFirst(7)).dartTrimmed
            var ours: [String] = []
            var base: [String]?
            var theirs: [String] = []
            var theirsLabel = ""
            var stage = 0
            index += 1
            var closed = false
            while index < lines.count {
                let current = lines[index]
                if current.hasPrefix("<<<<<<<") { return .failure(CollabSignError(malformed)) }
                if stage == 0 && current.hasPrefix("|||||||") {
                    stage = 1
                    base = []
                } else if stage < 2 && current.hasPrefix("=======") && current.dartTrimmed == "=======" {
                    stage = 2
                } else if stage == 2 && current.hasPrefix(">>>>>>>") {
                    theirsLabel = String(current.dropFirst(7)).dartTrimmed
                    closed = true
                    index += 1
                    break
                } else if stage == 0 {
                    ours.append(current)
                } else if stage == 1 {
                    base?.append(current)
                } else {
                    theirs.append(current)
                }
                index += 1
            }
            if !closed { return .failure(CollabSignError(malformed)) }
            segments.append(.conflict(ConflictHunk(
                id: hunkId, ours: ours, base: base, theirs: theirs, oursLabel: oursLabel, theirsLabel: theirsLabel,
                startLine: start)))
            hunkId += 1
        }
        if !plain.isEmpty { segments.append(.text(plain)) }
        return .success(ConflictFile(path: path, segments: segments, trailingNewline: trailing))
    }

    public static func lines(_ hunk: ConflictHunk, _ choice: ConflictChoice) -> [String] {
        switch choice {
        case .ours: return hunk.ours
        case .theirs: return hunk.theirs
        case .both: return hunk.ours + hunk.theirs
        case .bothTheirsFirst: return hunk.theirs + hunk.ours
        }
    }

    public static func unresolved(_ file: ConflictFile, _ choices: [Int: ConflictChoice]) -> [Int] {
        file.hunks.map(\.id).filter { choices[$0] == nil }
    }

    public static func resolve(_ file: ConflictFile, _ choices: [Int: ConflictChoice]) -> String? {
        guard unresolved(file, choices).isEmpty else { return nil }
        var out: [String] = []
        for segment in file.segments {
            switch segment {
            case .text(let lines): out += lines
            case .conflict(let hunk): out += lines(hunk, choices[hunk.id]!)
            }
        }
        let body = out.joined(separator: "\n")
        return file.trailingNewline ? body + "\n" : body
    }

    public static func agentPrompt(branch: String, base: String, files: [String]) -> String {
        let list = files.prefix(40).map { "- \($0)" }.joined(separator: "\n")
        return "Merge \(base) into \(branch) and resolve every conflict, keeping the intent of both sides. Run the tests, then push the branch.\n\nConflicting files:\n\(list)"
    }

    public static let fixSent = "Asked the agent to merge the base in and resolve the conflicts."

    public static func sessionPrompt(base: String, files: [String] = []) -> String {
        let into = base.isEmpty ? "the base branch" : "`\(base)`"
        let head = "Merge \(into) into your branch and resolve all merge conflicts. Make sure the build and tests still pass, then commit."
        if files.isEmpty { return head }
        return head + "\n\nConflicting files:\n" + files.prefix(40).map { "- \($0)" }.joined(separator: "\n")
    }
}
