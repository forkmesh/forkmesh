import FMDiff
import FMWatchWire
import Foundation

public enum AgentWatchDiff {
    public static func make(key: String, diff: AgentDiff, maxBytes: Int = WatchWire.maxBodyBytes) -> WatchDiff {
        var files = UnifiedDiff.parse(diff.committed).map { file(from: $0, group: .committed) }
        files += UnifiedDiff.parse(diff.uncommitted).map { file(from: $0, group: .uncommitted) }
        files += diff.untracked.filter { !$0.isEmpty }.map { WatchDiffFile(path: $0, status: "A", group: .untracked) }
        let out = WatchDiff(
            key: key, branch: diff.branch, base: diff.base, merged: diff.merged, files: files, truncated: diff.truncated)
        return WatchWire.fitted(out, maxBytes: maxBytes)
    }

    public static func file(from parsed: DiffFile, group: WatchDiffFile.Group) -> WatchDiffFile {
        var lines: [WatchDiffLine] = []
        for hunk in parsed.hunks {
            lines.append(WatchDiffLine(.hunk, hunk.section.isEmpty ? hunk.header : hunk.section, number: hunk.newStart))
            for line in hunk.lines {
                switch line.kind {
                case .added: lines.append(WatchDiffLine(.added, line.text, number: line.newNumber))
                case .removed: lines.append(WatchDiffLine(.removed, line.text, number: line.oldNumber))
                case .context: lines.append(WatchDiffLine(.context, line.text, number: line.newNumber))
                }
            }
        }
        return WatchDiffFile(
            path: parsed.path, oldPath: parsed.status == .renamed || parsed.status == .copied ? parsed.oldPath : "",
            status: letter(parsed.status), group: group, added: parsed.additions, removed: parsed.deletions,
            binary: parsed.binary, lines: lines)
    }

    public static func letter(_ status: DiffFileStatus) -> String {
        switch status {
        case .added: return "A"
        case .modified: return "M"
        case .deleted: return "D"
        case .renamed: return "R"
        case .copied: return "C"
        }
    }
}
