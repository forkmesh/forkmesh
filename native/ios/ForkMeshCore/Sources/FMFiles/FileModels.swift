import FMPackages
import Foundation

public typealias FileProgress = SmbProgress

public struct FileEntry: Identifiable, Hashable, Sendable {
    public var name: String
    public var path: String
    public var isFolder: Bool
    public var size: Int64
    public var modified: Date?
    public var hidden: Bool

    public init(name: String, path: String, isFolder: Bool, size: Int64 = 0, modified: Date? = nil, hidden: Bool = false) {
        self.name = name
        self.path = path
        self.isFolder = isFolder
        self.size = size
        self.modified = modified
        self.hidden = hidden
    }

    public var id: String { path }
    public var kind: FileKind { isFolder ? .folder : FileKinds.kind(name: name) }

    public var localName: String? {
        let last = (name as NSString).lastPathComponent
        guard !last.isEmpty, last != ".", last != "..", !last.contains("/") else { return nil }
        return last
    }

    public func localTarget(in folder: URL) throws -> URL {
        let base = folder.standardizedFileURL
        guard let local = localName else { throw FileSourceError("That file name cannot be saved on this phone.") }
        let target = base.appendingPathComponent(local).standardizedFileURL
        guard target.deletingLastPathComponent().standardizedFileURL.path == base.path else {
            throw FileSourceError("That file name cannot be saved on this phone.")
        }
        return target
    }
}

public enum FileSort: String, CaseIterable, Sendable {
    case name, date, size

    public var title: String {
        switch self {
        case .name: return "Name"
        case .date: return "Date"
        case .size: return "Size"
        }
    }
}

public enum FileKind: String, Sendable {
    case folder, text, image, pdf, other
}

public enum FileKinds {
    public static let textLimit: Int64 = 5 << 20
    public static let previewLimit: Int64 = 20 << 20
    public static let sniffLength = 8192

    static let textExtensions: Set<String> = [
        "txt", "md", "markdown", "json", "yaml", "yml", "toml", "ini", "cfg", "conf", "xml", "html", "htm", "css", "js", "mjs",
        "ts", "tsx", "jsx", "swift", "kt", "kts", "java", "c", "h", "cc", "cpp", "hpp", "m", "mm", "py", "rb", "go", "rs",
        "sh", "bash", "zsh", "fish", "ps1", "bat", "sql", "csv", "tsv", "log", "env", "gitignore", "dockerfile", "make",
        "cmake", "gradle", "properties", "lua", "php", "pl", "r", "dart", "vue", "svelte", "tex", "srt", "vtt", "plist", "svg",
    ]
    static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "heic", "heif", "webp", "bmp", "tif", "tiff"]

    public static func pathExtension(_ name: String) -> String {
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else {
            return name.hasPrefix(".") ? String(name.dropFirst()).lowercased() : ""
        }
        return String(name[name.index(after: dot)...]).lowercased()
    }

    public static func kind(name: String) -> FileKind {
        let ext = pathExtension(name)
        if imageExtensions.contains(ext) { return .image }
        if ext == "pdf" { return .pdf }
        if textExtensions.contains(ext) { return .text }
        if ["makefile", "dockerfile", "readme", "license", "changelog"].contains(name.lowercased()) { return .text }
        return .other
    }

    public static func looksLikeText(_ bytes: [UInt8]) -> Bool {
        let head = Array(bytes.prefix(sniffLength))
        if head.isEmpty { return true }
        if head.contains(0) { return false }
        var trimmed = head
        for _ in 0..<4 {
            if String(bytes: trimmed, encoding: .utf8) != nil { return true }
            guard !trimmed.isEmpty, bytes.count > head.count else { return false }
            trimmed.removeLast()
        }
        return false
    }

    public static func symbol(_ entry: FileEntry) -> String {
        switch entry.kind {
        case .folder: return "folder"
        case .text: return "doc.text"
        case .image: return "photo"
        case .pdf: return "doc.richtext"
        case .other: return "doc"
        }
    }
}

public enum FileBrowsing {
    public static func sorted(_ entries: [FileEntry], by sort: FileSort, ascending: Bool = true) -> [FileEntry] {
        entries.sorted { a, b in
            if a.isFolder != b.isFolder { return a.isFolder }
            let order: ComparisonResult
            switch sort {
            case .name:
                order = a.name.localizedStandardCompare(b.name)
            case .date:
                let left = a.modified ?? .distantPast, right = b.modified ?? .distantPast
                order = left == right ? a.name.localizedStandardCompare(b.name) : (left < right ? .orderedAscending : .orderedDescending)
            case .size:
                order = a.size == b.size ? a.name.localizedStandardCompare(b.name) : (a.size < b.size ? .orderedAscending : .orderedDescending)
            }
            return ascending ? order == .orderedAscending : order == .orderedDescending
        }
    }

    public static func visible(_ entries: [FileEntry], showHidden: Bool, filter: String) -> [FileEntry] {
        let needle = filter.trimmingCharacters(in: .whitespaces)
        return entries.filter { entry in
            (showHidden || !entry.hidden) && (needle.isEmpty || entry.name.localizedCaseInsensitiveContains(needle))
        }
    }

    public static func clean(_ path: String) -> String {
        path.split(separator: "/", omittingEmptySubsequences: true).filter { $0 != "." }.joined(separator: "/")
    }

    public static func join(_ folder: String, _ name: String) -> String {
        let base = clean(folder)
        return base.isEmpty ? name : base + "/" + name
    }

    public static func parent(_ path: String) -> String {
        var parts = clean(path).split(separator: "/")
        if !parts.isEmpty { parts.removeLast() }
        return parts.joined(separator: "/")
    }

    public static func name(_ path: String) -> String {
        clean(path).split(separator: "/").last.map(String.init) ?? ""
    }

    public static func crumbs(_ path: String, root: String) -> [(title: String, path: String)] {
        var result: [(title: String, path: String)] = [(root, "")]
        var built = ""
        for part in clean(path).split(separator: "/") {
            built = built.isEmpty ? String(part) : built + "/" + part
            result.append((String(part), built))
        }
        return result
    }

    public static func isInside(_ path: String, folder: String) -> Bool {
        let child = clean(path), parent = clean(folder)
        if parent.isEmpty { return true }
        return child == parent || child.hasPrefix(parent + "/")
    }

    public static func nameProblem(_ name: String) -> String? {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return "Enter a name." }
        if trimmed == "." || trimmed == ".." { return "That name is not allowed." }
        if trimmed.contains(where: { "/\\:*?\"<>|".contains($0) }) { return "Names cannot contain / \\ : * ? \" < > or |." }
        if trimmed.utf8.count > 255 { return "That name is too long." }
        return nil
    }

    public static func uniqueName(_ name: String, existing: Set<String>) -> String {
        let lowered = Set(existing.map { $0.lowercased() })
        if !lowered.contains(name.lowercased()) { return name }
        let ext = FileKinds.pathExtension(name)
        let stem = ext.isEmpty || name.hasPrefix(".") && !name.dropFirst().contains(".") ? name : String(name.dropLast(ext.count + 1))
        let suffix = stem == name ? "" : "." + ext
        var index = 1
        while true {
            let candidate = stem + (index == 1 ? " copy" : " copy \(index)") + suffix
            if !lowered.contains(candidate.lowercased()) { return candidate }
            index += 1
        }
    }

    public static func formatSize(_ bytes: Int64) -> String { OpenWrtModel.formatBytes(bytes) }

    public static func deleteQuestion(_ entry: FileEntry) -> String {
        entry.isFolder ? "Delete \(entry.name) and everything in it?" : "Delete \(entry.name)?"
    }

    public static let emptyFolder = "This folder is empty."
}
