import Foundation

public struct AgentVoiceCandidate: Hashable, Sendable, Identifiable {
    public var id: String
    public var number: Int
    public var title: String
    public var repo: String
    public var statusLabel: String
    public var isActive: Bool
    public var updatedAtMs: Int64

    public init(
        id: String, number: Int = 0, title: String = "", repo: String = "", statusLabel: String = "",
        isActive: Bool = false, updatedAtMs: Int64 = 0
    ) {
        self.id = id
        self.number = number
        self.title = title
        self.repo = repo
        self.statusLabel = statusLabel
        self.isActive = isActive
        self.updatedAtMs = updatedAtMs
    }

    public var name: String { AgentVoice.name(number: number, title: title) }

    public var detail: String {
        [title, statusLabel, repo].map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

public enum AgentVoice {
    public static let desktopPrefix = "desktop:"

    static let digitWords: [String: Character] = [
        "zero": "0", "oh": "0", "one": "1", "two": "2", "three": "3", "four": "4", "for": "4", "five": "5",
        "six": "6", "seven": "7", "eight": "8", "nine": "9",
    ]

    static let fillerWords: Set<String> = ["agent", "number", "no", "session", "run", "the", "#"]

    public static func name(number: Int, title: String = "") -> String {
        if number > 0 { return "Agent \(number)" }
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "Agent run" : trimmed
    }

    public static func rowId(number: Int) -> String { "\(desktopPrefix)\(number)" }

    public static func number(rowId: String) -> Int? {
        let raw = rowId.hasPrefix(desktopPrefix) ? String(rowId.dropFirst(desktopPrefix.count)) : rowId
        guard let value = Int(raw), value > 0 else { return nil }
        return value
    }

    public static func number(in spoken: String) -> Int? {
        let text = spoken.lowercased().replacingOccurrences(of: ",", with: "")
        var digits = ""
        for character in text {
            if character.isASCII, character.isNumber {
                digits.append(character)
            } else if !digits.isEmpty {
                break
            }
        }
        if let value = Int(digits), value > 0 { return value }
        let words = text.split(whereSeparator: { !$0.isLetter && $0 != "#" }).map(String.init)
            .filter { !fillerWords.contains($0) }
        guard !words.isEmpty, words.allSatisfy({ digitWords[$0] != nil }) else { return nil }
        let spelled = String(words.compactMap { digitWords[$0] })
        guard let value = Int(spelled), value > 0 else { return nil }
        return value
    }

    public static func ordered(_ candidates: [AgentVoiceCandidate]) -> [AgentVoiceCandidate] {
        var seen: Set<String> = []
        return candidates.filter { seen.insert($0.id).inserted }.sorted {
            if $0.isActive != $1.isActive { return $0.isActive }
            if $0.updatedAtMs != $1.updatedAtMs { return $0.updatedAtMs > $1.updatedAtMs }
            return $0.number > $1.number
        }
    }

    public static func match(_ candidates: [AgentVoiceCandidate], query: String) -> [AgentVoiceCandidate] {
        let pool = ordered(candidates)
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if needle.isEmpty { return pool }
        if let wanted = number(in: needle) {
            let exact = pool.filter { $0.number == wanted }
            if !exact.isEmpty { return exact }
        }
        let words = needle.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            .filter { !fillerWords.contains($0) }
        if words.isEmpty { return pool }
        return pool.filter { candidate in
            let haystack = "\(candidate.name) \(candidate.title) \(candidate.repo)".lowercased()
            return words.allSatisfy { haystack.contains($0) }
        }
    }

    public static func status(_ candidate: AgentVoiceCandidate) -> String {
        var text = candidate.name
        let title = candidate.title.trimmingCharacters(in: .whitespacesAndNewlines)
        if candidate.number > 0, !title.isEmpty { text += ", \(title)," }
        let state = candidate.statusLabel.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        text += state.isEmpty ? (candidate.isActive ? " is running" : " has finished") : " is \(state)"
        if !candidate.repo.isEmpty { text += " in \(candidate.repo)" }
        return text + "."
    }

    public static func changes(files: Int, added: Int, removed: Int, merged: Bool, base: String) -> String {
        if merged { return base.isEmpty ? "It is already merged." : "It is already merged into \(base)." }
        if files == 0 { return "It has no changes yet." }
        let noun = files == 1 ? "file" : "files"
        return "It changed \(files) \(noun), \(added) lines added and \(removed) removed."
    }

    public static func merged(_ name: String, branch: String, files: Int? = nil, added: Int = 0, removed: Int = 0) -> String {
        let head = branch.isEmpty ? "Merged \(name) and cleaned up." : "Merged \(name) from \(branch) and cleaned up."
        guard let files, files > 0 else { return head }
        return head + " " + changes(files: files, added: added, removed: removed, merged: false, base: "")
    }

    public static func stopped(_ names: [String]) -> String {
        switch names.count {
        case 0: return "No ForkMesh agents are running."
        case 1: return "Stopped \(names[0])."
        default: return "Stopped \(names.count) agents."
        }
    }

    static func agents(_ count: Int) -> String { count == 1 ? "1 agent" : "\(count) agents" }

    public static func deleteAllQuestion(count: Int, running: Int, unmerged: Int) -> String {
        if count <= 0 { return deletedAll(deleted: 0, failed: 0) }
        var text = count == 1
            ? "Delete the only agent on your desktop and its worktree?"
            : "Delete all \(count) agents on your desktop and their worktrees?"
        if running > 0 { text += running == 1 ? " 1 is running and will be stopped." : " \(running) are running and will be stopped." }
        if unmerged > 0 {
            text += unmerged == 1
                ? " 1 has an unmerged branch; that work is lost."
                : " \(unmerged) have unmerged branches; that work is lost."
        }
        return text
    }

    public static func deletedAll(deleted: Int, failed: Int) -> String {
        if deleted <= 0, failed <= 0 { return "There are no agents on your desktop to delete." }
        if deleted <= 0 { return failed == 1 ? "The agent could not be deleted." : "None of the \(failed) agents could be deleted." }
        let head = "Deleted \(agents(deleted))."
        if failed <= 0 { return head }
        return head + (failed == 1 ? " 1 could not be deleted." : " \(failed) could not be deleted.")
    }
}
