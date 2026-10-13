// The small pure helpers of the repository detail screen: sizes, file glyphs,
// source labels, badge colours, preview kinds and decoded text, the commit map
// readers, patch file parsing, the changed files title, and line splitting.

import FMApi
import FMBase
import Foundation

/// A palette colour, named: the core cannot hold SwiftUI colours, so the view
/// maps these to FmTheme.
public enum RepoBadgeTone: String, Sendable, Equatable {
    case success, danger, warning, accent, secondary, tertiary
}

/// Git change status of a file (FmChangeKind).
public enum RepoChangeKind: String, Sendable, Equatable, CaseIterable {
    case added, modified, deleted, renamed

    /// The badge letter.
    public var letter: String {
        switch self {
        case .added: return "A"
        case .modified: return "M"
        case .deleted: return "D"
        case .renamed: return "R"
        }
    }
}

/// `_PatchFile`: per-file summary parsed from a unified diff.
public struct RepoPatchFile: Equatable, Hashable, Sendable {
    public var path: String
    public var kind: RepoChangeKind
    public var adds: Int
    public var dels: Int

    public init(path: String, kind: RepoChangeKind, adds: Int = 0, dels: Int = 0) {
        self.path = path
        self.kind = kind
        self.adds = adds
        self.dels = dels
    }
}

/// `_PreviewKind`.
public enum RepoPreviewKind: String, Sendable, Equatable {
    case text, image, video
}

public enum RepoDisplay {
    // MARK: Sizes, glyphs, sources

    /// `_humanSize`: "12 B", "1.5 KB", "12 KB", "3.4 MB", "1.0 GB".
    public static func humanSize(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        let kb = Double(bytes) / 1024
        if kb < 1024 { return "\(DartCore.toStringAsFixed(kb, kb < 10 ? 1 : 0)) KB" }
        let mb = kb / 1024
        if mb < 1024 { return "\(DartCore.toStringAsFixed(mb, 1)) MB" }
        return "\(DartCore.toStringAsFixed(mb / 1024, 1)) GB"
    }

    private static let codeExtensions: Set<String> = [
        "dart", "ts", "tsx", "js", "jsx", "mjs", "cjs", "py", "go", "rs", "c", "cc", "cpp", "h", "hpp", "java",
        "kt", "kts", "swift", "rb", "php", "cs", "sh", "bash", "zsh", "sql", "lua", "scala", "m", "mm", "html",
        "css", "scss", "vue", "svelte",
    ]
    private static let dataExtensions: Set<String> = ["json", "yaml", "yml", "toml", "xml", "lock", "csv", "ini"]
    private static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "ico"]
    private static let docExtensions: Set<String> = ["md", "mdx", "txt", "rst", "adoc"]

    /// `_fileGlyph`: the Material icon name for a file name.
    public static func fileGlyph(_ name: String) -> String {
        let lower = name.lowercased()
        let ext: String
        if let dot = lower.lastIndex(of: ".") {
            ext = String(lower[lower.index(after: dot)...])
        } else {
            ext = ""
        }
        if codeExtensions.contains(ext) { return "code_rounded" }
        if dataExtensions.contains(ext) { return "data_object_rounded" }
        if imageExtensions.contains(ext) { return "image_outlined" }
        if docExtensions.contains(ext) { return "article_outlined" }
        if lower == "dockerfile" || lower == "makefile" || lower == "license" { return "terminal_rounded" }
        return "description_outlined"
    }

    /// `_sourceLabel`: how the serving node is named on a chip, or nil.
    public static func sourceLabel(_ source: String) -> String? {
        let value = source.dartTrimmed
        if value.isEmpty { return nil }
        let lower = value.lowercased()
        if lower == "live" || lower == "live_source" || lower == "live-source" { return "live source" }
        if lower == "live mirror" || lower == "live_mirror" || lower == "live-mirror" { return "live mirror" }
        if lower.hasPrefix("served by ") || lower.hasPrefix("live ") { return value }
        return "served by \(value)"
    }

    /// `_badgeColor`: the colour of a small status badge, from its text.
    public static func badgeTone(_ text: String) -> RepoBadgeTone {
        let normalized = text.lowercased()
        if normalized.contains("open") || normalized.contains("live") || normalized.contains("signed") {
            return .success
        }
        if normalized.contains("closed") || normalized.contains("offline") || normalized.contains("request") {
            return .danger
        }
        if normalized.contains("merged") { return .accent }
        return .secondary
    }

    /// `_ActionStatusBadge`.
    public static func actionStatusTone(_ statusLabel: String) -> RepoBadgeTone {
        switch statusLabel {
        case "Success": return .success
        case "Failed", "Rejected", "Cancelled": return .danger
        case "Running", "Queued", "Awaiting approval": return .accent
        default: return .tertiary
        }
    }

    /// `_AgentStatusBadge`.
    public static func agentStatusTone(_ statusLabel: String) -> RepoBadgeTone {
        switch statusLabel {
        case "Merged", "Done": return .success
        case "Failed", "Stopped": return .danger
        case "Running", "Queued", "Waiting": return .accent
        default: return .tertiary
        }
    }

    /// The last path segment (`path.split('/').last`).
    public static func baseName(_ path: String) -> String {
        path.components(separatedBy: "/").last ?? path
    }

    // MARK: File previews

    /// `_previewKind`.
    public static func previewKind(_ path: String) -> RepoPreviewKind {
        let lower = path.lowercased()
        for ext in [".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"] where lower.hasSuffix(ext) {
            return .image
        }
        for ext in [".mp4", ".mov", ".webm", ".mkv"] where lower.hasSuffix(ext) {
            return .video
        }
        return .text
    }

    /// `_FilePreview._decodedText`: the blob as text, or nil when it looks
    /// binary (a NUL in the first 512 bytes / characters) or is bad base64.
    public static func decodedText(_ blob: RepoBlob) -> String? {
        let content = blob.content
        if blob.encoding.lowercased() == "base64" {
            let compact = String(String.UnicodeScalarView(content.unicodeScalars.filter { !isDartRegexSpace($0) }))
            guard let bytes = B64.decodeOrNil(compact) else { return nil }
            if bytes.prefix(512).contains(0) { return nil }
            return String(decoding: bytes, as: UTF8.self)
        }
        if content.unicodeScalars.prefix(512).contains(where: { $0.value == 0 }) { return nil }
        return content
    }

    /// ECMAScript RegExp `\s`: white space and line terminators.
    static func isDartRegexSpace(_ c: Unicode.Scalar) -> Bool {
        switch c.value {
        case 0x09...0x0D, 0x20, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
            return true
        default:
            return false
        }
    }

    /// Splits on \n, \r\n and \r; a final terminator does not start another
    /// line.
    public static func dartLines(_ text: String) -> [String] {
        var lines: [String] = []
        var current: [UInt16] = []
        let units = Array(text.utf16)
        var i = 0
        while i < units.count {
            let unit = units[i]
            if unit == 0x0A || unit == 0x0D {
                lines.append(String(decoding: current, as: UTF16.self))
                current.removeAll(keepingCapacity: true)
                if unit == 0x0D, i + 1 < units.count, units[i + 1] == 0x0A { i += 1 }
            } else {
                current.append(unit)
            }
            i += 1
        }
        if !current.isEmpty { lines.append(String(decoding: current, as: UTF16.self)) }
        return lines
    }

    // MARK: Commits

    /// `_commitHash`.
    public static func commitHash(_ commit: JSONObject) -> String {
        DartCore.toString(ModelJSON.coalesce(commit["hash"], commit["sha"], commit["id"]) ?? .string(""))
    }

    /// `_commitShort`: the first seven characters.
    public static func commitShort(_ commit: JSONObject) -> String {
        shortHash(commitHash(commit))
    }

    /// `hash.length < 7 ? hash : hash.substring(0, 7)`.
    public static func shortHash(_ hash: String) -> String {
        let units = Array(hash.utf16)
        if units.count < 7 { return hash }
        return String(decoding: units[0..<7], as: UTF16.self)
    }

    /// `_commitSubject`: the first line of the subject or message.
    public static func commitSubject(_ commit: JSONObject) -> String {
        let raw = DartCore.toString(ModelJSON.coalesce(commit["subject"], commit["message"]) ?? .string("Commit"))
        let firstLine = (raw.components(separatedBy: "\n").first ?? "").dartTrimmed
        return firstLine.isEmpty ? "Commit" : firstLine
    }

    /// `_commitAuthor`.
    public static func commitAuthor(_ commit: JSONObject) -> String {
        if case .object(let author)? = commit["author"] {
            return DartCore.toString(ModelJSON.coalesce(author["name"], author["email"]) ?? .string(""))
        }
        return DartCore.toString(
            ModelJSON.coalesce(commit["author"], commit["authorName"], commit["committer"]) ?? .string(""))
    }

    /// `(commit['body'] ?? commit['description'] ?? '').toString()`.
    public static func commitBody(_ commit: JSONObject) -> String {
        DartCore.toString(ModelJSON.coalesce(commit["body"], commit["description"]) ?? .string(""))
    }

    private static func epochFromNumber(_ n: Int) -> Int { n > 100_000_000_000 ? n : n * 1000 }

    /// `_commitDate`, as epoch milliseconds.
    public static func commitDateMs(_ commit: JSONObject, timeZone: TimeZone = .current) -> Int? {
        for key in ["date", "time", "timestamp", "createdAt", "committedDate", "authorDate", "ts"] {
            switch commit[key] {
            case .int(let n)?:
                return epochFromNumber(Int(truncatingIfNeeded: n))
            case .double(let d)? where d.isFinite:
                return epochFromNumber(Int(d.rounded(.toNearestOrAwayFromZero)))
            case .string(let s)? where !s.dartTrimmed.isEmpty:
                if let n = DartCore.intTryParse(s) { return epochFromNumber(n) }
                if let ms = DartCore.dateTimeTryParseMs(s, timeZone: timeZone) { return ms }
            default:
                continue
            }
        }
        return nil
    }

    /// `_relativeCommitTime`: "" when the commit carries no date.
    public static func relativeCommitTime(_ commit: JSONObject, now: Date = Date(), timeZone: TimeZone = .current) -> String {
        guard let ms = commitDateMs(commit, timeZone: timeZone) else { return "" }
        return relativeAge(ms, now: now)
    }

    /// `_relativeTime`: compact "now" / "5m" / "2h" / "3d" / "5mo" / "2y".
    public static func relativeAge(_ epochMs: Int, now: Date = Date()) -> String {
        let diffMs = Int(now.epochMillis) - epochMs
        let minutes = diffMs / 60_000
        let hours = diffMs / 3_600_000
        let days = diffMs / 86_400_000
        if minutes < 1 { return "now" }
        if hours < 1 { return "\(minutes)m" }
        if days < 1 { return "\(hours)h" }
        if days < 30 { return "\(days)d" }
        if days < 365 { return "\(days / 30)mo" }
        return "\(days / 365)y"
    }

    /// `_agoLabel`: "just now" or "2h ago".
    public static func agoLabel(_ epochMs: Int, now: Date = Date()) -> String {
        let rel = relativeAge(epochMs, now: now)
        return rel == "now" ? "just now" : "\(rel) ago"
    }

    // MARK: Diffs

    /// `_changedFilesTitle`.
    public static func changedFilesTitle(_ count: Int) -> String {
        "\(count) changed \(count == 1 ? "file" : "files")"
    }

    /// `_parsePatchFiles`: per-file kind and +/- counts of a unified diff.
    public static func parsePatchFiles(_ diff: String) -> [RepoPatchFile] {
        if diff.dartTrimmed.isEmpty { return [] }
        var files: [RepoPatchFile] = []
        var path: String?
        var kind = RepoChangeKind.modified
        var adds = 0
        var dels = 0
        func flush() {
            guard let current = path, !current.isEmpty else { return }
            files.append(RepoPatchFile(path: current, kind: kind, adds: adds, dels: dels))
        }
        for line in dartLines(diff) {
            if line.hasPrefix("diff --git ") {
                flush()
                path = gitHeaderPath(line).dartTrimmed
                kind = .modified
                adds = 0
                dels = 0
            } else if line.hasPrefix("new file mode") || line.hasPrefix("--- /dev/null") {
                kind = .added
            } else if line.hasPrefix("deleted file mode") || line.hasPrefix("+++ /dev/null") {
                kind = .deleted
            } else if line.hasPrefix("rename from ") || line.hasPrefix("similarity index") {
                kind = .renamed
            } else if line.hasPrefix("rename to ") {
                path = String(line.dropFirst("rename to ".count)).dartTrimmed
            } else if line.hasPrefix("+++ b/") {
                if path == nil { path = String(line.dropFirst("+++ b/".count)).dartTrimmed }
            } else if line.hasPrefix("+++ ") || line.hasPrefix("--- ") {
                continue
            } else if line.hasPrefix("+") {
                adds += 1
            } else if line.hasPrefix("-") {
                dels += 1
            }
        }
        flush()
        return files
    }

    /// `RegExp(r'^diff --git a/(.*?) b/(.*)$')` group 2, or the rest of the
    /// line when it does not match. The lazy first group makes group 2 start
    /// after the first " b/" that follows "a/".
    static func gitHeaderPath(_ line: String) -> String {
        let rest = String(line.dropFirst("diff --git ".count))
        if rest.hasPrefix("a/") {
            let afterA = rest.dropFirst(2)
            if let range = afterA.range(of: " b/") {
                return String(afterA[range.upperBound...])
            }
        }
        return rest
    }

    /// `_DiffViewer` line colouring: which band a unified diff line sits in.
    public static func diffLineTone(_ line: String) -> RepoBadgeTone? {
        if line.hasPrefix("+") && !line.hasPrefix("+++") { return .success }
        if line.hasPrefix("-") && !line.hasPrefix("---") { return .danger }
        if line.hasPrefix("@@") { return .accent }
        return nil
    }
}
