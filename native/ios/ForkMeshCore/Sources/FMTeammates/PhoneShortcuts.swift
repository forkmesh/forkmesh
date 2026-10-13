import Foundation

public struct ShortcutMeta: Equatable, Sendable {
    public var name: String
    public var description = ""
    public var agent = ""
    public var model = ""
}

public enum PhoneShortcuts {
    public static let maxBytes = 64 * 1024
    public static let desktopOnly = "(only on the desktop)"

    static let folders: [(scope: String, path: String)] = [
        ("shortcut", ".forkmesh/shortcuts"),
        ("repository", ".forkmesh/skills"),
    ]

    static let metaKeys = ["# name:", "# description:", "# agent:", "# model:"]

    public static func validFile(_ name: String) -> Bool {
        name.range(of: "^[A-Za-z0-9_][A-Za-z0-9._ -]{0,127}$", options: .regularExpression) != nil
            && !name.contains("..")
    }

    public static func kind(file: String) -> String {
        switch (file as NSString).pathExtension.lowercased() {
        case "sh", "bash": return "script"
        case "skill": return "skill"
        case "md", "txt", "prompt": return "prompt"
        default: return "file"
        }
    }

    public static func meta(file: String, text: String) -> ShortcutMeta {
        var meta = ShortcutMeta(name: file)
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false).prefix(20) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("#!") { continue }
            if line.hasPrefix("# name:") {
                meta.name = String(line.dropFirst(7)).trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("# description:") {
                meta.description = String(line.dropFirst(14)).trimmingCharacters(in: .whitespaces)
            } else if line.hasPrefix("# agent:") {
                meta.agent = String(line.dropFirst(8)).trimmingCharacters(in: .whitespaces).lowercased()
            } else if line.hasPrefix("# model:") {
                meta.model = String(line.dropFirst(8)).trimmingCharacters(in: .whitespaces)
            } else if meta.description.isEmpty && line.hasPrefix("#") {
                meta.description = String(line.dropFirst()).trimmingCharacters(in: .whitespaces)
            }
        }
        return meta
    }

    public static func prompt(_ text: String) -> String {
        text.split(separator: "\n", omittingEmptySubsequences: false)
            .filter { line in
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                return !metaKeys.contains { trimmed.hasPrefix($0) }
            }
            .joined(separator: "\n")
            .replacingOccurrences(of: "{{appLog}}", with: desktopOnly)
            .replacingOccurrences(of: "{{stallLog}}", with: desktopOnly)
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public static func runsOnPhone(_ entry: ShortcutEntry) -> Bool {
        entry.kind == "prompt" || entry.kind == "skill"
    }

    public static func read(_ url: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let data = try? handle.read(upToCount: maxBytes) else { return nil }
        return String(decoding: data, as: UTF8.self).replacingOccurrences(of: "\r", with: "")
    }

    public static func file(worktree: URL, scope: String, file: String) -> URL? {
        guard validFile(file), let folder = folders.first(where: { $0.scope == scope }) else { return nil }
        return worktree.appendingPathComponent(folder.path, isDirectory: true).appendingPathComponent(file)
    }

    public static func text(worktree: URL, entry: ShortcutEntry) -> String? {
        file(worktree: worktree, scope: entry.scope, file: entry.file).flatMap(read)
    }

    public static func entries(worktree: URL) -> [ShortcutEntry] {
        let manager = FileManager.default
        var found: [ShortcutEntry] = []
        for folder in folders {
            let dir = worktree.appendingPathComponent(folder.path, isDirectory: true)
            guard let names = try? manager.contentsOfDirectory(atPath: dir.path) else { continue }
            for name in names.sorted() where validFile(name) {
                let url = dir.appendingPathComponent(name)
                var isDir: ObjCBool = false
                guard manager.fileExists(atPath: url.path, isDirectory: &isDir), !isDir.boolValue,
                    let text = read(url)
                else { continue }
                let header = Self.meta(file: name, text: text)
                var entry = ShortcutEntry(
                    file: name, scope: folder.scope, name: header.name, description: header.description,
                    kind: Self.kind(file: name), agent: header.agent, model: header.model)
                entry.onPhone = true
                entry.runnable = runsOnPhone(entry) && !prompt(text).isEmpty
                found.append(entry)
            }
        }
        return found
    }

    public static func catalog(repo: String, repos: [String], worktree: URL?) -> ShortcutCatalog {
        ShortcutCatalog(repo: repo, repos: repos, entries: worktree.map(entries(worktree:)) ?? [], onPhone: true)
    }
}
