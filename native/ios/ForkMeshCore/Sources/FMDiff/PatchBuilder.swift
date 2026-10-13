// FMDiff has no dependency on FMGit, so instead of a GitRepository and two
// commit ids, this takes the two flattened trees and closures that
// read a tree or a blob. FMCode adapts FMGit to them in a few lines; the
// diffing, hunking and header rules are all here.
//
// Lines are compared as UTF-8 bytes, never as Swift Strings: `==` on String is
// canonical equivalence, which would call a precomposed and a decomposed "é"
// the same line and lose the change.

import Foundation

/// A blob in a flattened tree: its full hex object id and its mode as git
/// writes it in a tree (`100644`, `100755`, `120000`, …).
public struct PatchBlobRef: Equatable, Hashable, Sendable {
    public let id: String
    public let mode: String

    public init(id: String, mode: String) {
        self.id = id
        self.mode = mode
    }
}

/// One entry of a tree object, as `PatchBuilder.flatten` needs it.
public struct PatchTreeEntry: Equatable, Sendable {
    public let name: String
    public let id: String
    public let mode: String
    public let isDirectory: Bool

    public init(name: String, id: String, mode: String, isDirectory: Bool) {
        self.name = name
        self.id = id
        self.mode = mode
        self.isDirectory = isDirectory
    }
}

/// A unified diff between two commits, plus what it could not represent.
public struct PatchBuildResult: Equatable, Sendable {
    /// The unified diff, ready for `git apply`. Empty when nothing changed.
    public let patch: String

    /// Paths the patch covers, in the order they appear.
    public let changedPaths: [String]

    /// Paths whose content is binary. This build deliberately does not encode
    /// binary blobs, so these are reported rather than silently dropped — a
    /// pull request that quietly omitted half a branch would be worse than one
    /// that refuses to be created.
    public let binaryPaths: [String]

    public init(patch: String, changedPaths: [String], binaryPaths: [String]) {
        self.patch = patch
        self.changedPaths = changedPaths
        self.binaryPaths = binaryPaths
    }

    public var isEmpty: Bool { U16.isBlank(patch.utf16) }
    public var hasBinaryChanges: Bool { !binaryPaths.isEmpty }
}

/// Builds `git apply`-compatible unified diffs.
///
/// Mobile needs this because a pull request is submitted as a patch. The
/// output targets real `git`, not a private format: the receiving node runs
/// `git apply --index --3way`, which is also why every file header carries an
/// `index <old>..<new> <mode>` line. Those blob ids are what let the receiver
/// fall back to a three-way merge when its copy has moved on — without them a
/// patch either applies exactly or fails outright.
///
/// Push the branch before submitting the pull request: three-way apply needs
/// the pre-image blobs present on the receiving side.
public enum PatchBuilder {
    /// Lines of unchanged context around each change, matching git's default.
    public static let defaultContext = 3

    /// A file whose first bytes contain NUL is treated as binary, as git does.
    static let binarySniffBytes = 8000

    /// Ceiling on Myers' edit distance per file.
    ///
    /// The algorithm's trace grows with the number of edits, and an unbounded
    /// one can allocate hundreds of megabytes on a rewritten file —
    /// unacceptable on a phone. Past this, the file is emitted as a whole-file
    /// replacement, which is a larger but equally valid patch. Common
    /// prefix/suffix trimming happens first, so a normal edit never
    /// approaches this.
    static let maxEditDistance = 600

    /// Guards against a pathological tree; a mobile repository will not
    /// approach either bound.
    static let maxTreeEntries = 20000
    static let maxTreeDepth = 64

    static let zeroId = "0000000000000000000000000000000000000000"

    /// Diff two flattened trees (path → blob) as a unified patch.
    public static func between(
        baseTree: [String: PatchBlobRef],
        headTree: [String: PatchBlobRef],
        context: Int = defaultContext,
        readBlob: (String) async throws -> [UInt8]
    ) async rethrows -> PatchBuildResult {
        // Paths sort by UTF-16 code unit.
        let paths = Set(baseTree.keys).union(headTree.keys)
            .sorted { $0.utf16.lexicographicallyPrecedes($1.utf16) }
        var buffer: [UInt8] = []
        var changed: [String] = []
        var binary: [String] = []

        for path in paths {
            let before = baseTree[path]
            let after = headTree[path]
            if let before, let after, before.id == after.id {
                if before.mode == after.mode { continue }
                // Content identical, permissions changed: git records this on its own.
                buffer.append(contentsOf: Array("diff --git a/\(path) b/\(path)\n".utf8))
                buffer.append(contentsOf: Array("old mode \(before.mode)\n".utf8))
                buffer.append(contentsOf: Array("new mode \(after.mode)\n".utf8))
                changed.append(path)
                continue
            }

            var beforeBytes: [UInt8] = []
            var afterBytes: [UInt8] = []
            if let before { beforeBytes = try await readBlob(before.id) }
            if let after { afterBytes = try await readBlob(after.id) }

            if isBinary(beforeBytes) || isBinary(afterBytes) {
                binary.append(path)
                continue
            }

            let fileDiff = fileDiffBytes(
                path: path,
                before: before,
                after: after,
                beforeText: decode(beforeBytes),
                afterText: decode(afterBytes),
                context: context
            )
            if fileDiff.isEmpty { continue }
            buffer.append(contentsOf: fileDiff)
            changed.append(path)
        }

        return PatchBuildResult(
            patch: String(decoding: buffer, as: UTF8.self),
            changedPaths: changed,
            binaryPaths: binary
        )
    }

    // MARK: - Tree flattening

    /// Walks a commit's root tree into path → blob, within the entry and depth
    /// bounds. `readTree` returns the entries of the tree with the given id.
    public static func flatten(
        rootTree: String,
        readTree: (String) async throws -> [PatchTreeEntry]
    ) async rethrows -> [String: PatchBlobRef] {
        var out: [String: PatchBlobRef] = [:]
        try await walk(rootTree, prefix: "", depth: 0, into: &out, readTree: readTree)
        return out
    }

    private static func walk(
        _ treeId: String,
        prefix: String,
        depth: Int,
        into out: inout [String: PatchBlobRef],
        readTree: (String) async throws -> [PatchTreeEntry]
    ) async rethrows {
        if depth > maxTreeDepth || out.count >= maxTreeEntries { return }
        for entry in try await readTree(treeId) {
            let path = prefix.isEmpty ? entry.name : "\(prefix)/\(entry.name)"
            if entry.isDirectory {
                try await walk(entry.id, prefix: path, depth: depth + 1, into: &out, readTree: readTree)
            } else {
                if out.count >= maxTreeEntries { return }
                out[path] = PatchBlobRef(id: entry.id, mode: entry.mode)
            }
        }
    }

    // MARK: - Per-file diff

    /// The diff of one file, header included; empty when nothing changed. A
    /// nil side means the file does not exist there.
    public static func fileDiff(
        path: String,
        before: PatchBlobRef?,
        after: PatchBlobRef?,
        beforeText: String,
        afterText: String,
        context: Int = defaultContext
    ) -> String {
        String(decoding: fileDiffBytes(
            path: path,
            before: before,
            after: after,
            beforeText: Array(beforeText.utf8),
            afterText: Array(afterText.utf8),
            context: context
        ), as: UTF8.self)
    }

    static func fileDiffBytes(
        path: String,
        before: PatchBlobRef?,
        after: PatchBlobRef?,
        beforeText: [UInt8],
        afterText: [UInt8],
        context: Int
    ) -> [UInt8] {
        if beforeText == afterText && before?.mode == after?.mode { return [] }

        // Diff over *keys*, where a final line lacking its newline is marked.
        // Two files whose only difference is the trailing newline have
        // identical lines otherwise, so without this the diff finds no change
        // and emits a header with no hunk — which git rejects as garbage.
        let beforeLines = keyLines(beforeText)
        let afterLines = keyLines(afterText)

        var header = "diff --git a/\(path) b/\(path)\n"
        if before == nil {
            header += "new file mode \(after?.mode ?? "")\n"
        } else if after == nil {
            header += "deleted file mode \(before?.mode ?? "")\n"
        } else if let before, let after, before.mode != after.mode {
            header += "old mode \(before.mode)\nnew mode \(after.mode)\n"
        }
        // `git apply --3way` resolves these ids to reconstruct the pre-image,
        // so they must be the real blob ids, not abbreviations of convenience.
        let oldId = before?.id ?? zeroId
        let newId = after?.id ?? zeroId
        var modeSuffix = ""
        if let before, let after {
            if before.mode == after.mode { modeSuffix = " \(before.mode)" }
        } else if before == nil, let after {
            modeSuffix = " \(after.mode)"
        }
        header += "index \(oldId)..\(newId)\(modeSuffix)\n"
        header += before == nil ? "--- /dev/null\n" : "--- a/\(path)\n"
        header += after == nil ? "+++ /dev/null\n" : "+++ b/\(path)\n"

        let rows = makeRows(diffLines(beforeLines, afterLines))
        let hunks = makeHunks(rows, context: context, oldTotal: beforeLines.count, newTotal: afterLines.count)
        var out = Array(header.utf8)
        // With no hunks this is a mode-only change: the header alone is the
        // whole diff.
        for hunk in hunks {
            out.append(contentsOf: Array(hunk.headerLine.utf8))
            out.append(0x0A)
            for row in hunk.rows {
                out.append(row.marker)
                out.append(contentsOf: row.displayText)
                out.append(0x0A)
                // Each row knows whether the line it carries ended without a
                // newline, so the note lands immediately after exactly that
                // line — which is what git does when only one side is missing
                // its final newline.
                if row.noEol { out.append(contentsOf: noNewlineNote) }
            }
        }
        return out
    }

    private static let noNewlineNote = Array("\\ No newline at end of file\n".utf8)

    // MARK: - Hunks

    enum Op { case keep, delete, insert }

    struct Edit {
        let op: Op
        let text: ArraySlice<UInt8>
    }

    struct Row {
        let op: Op
        let text: ArraySlice<UInt8>

        /// 1-based line number on the old side, or 0 for an inserted line.
        let oldNo: Int

        /// 1-based line number on the new side, or 0 for a deleted line.
        let newNo: Int

        /// Where this row sits on the old side even when it has no old line,
        /// so a pure-insertion hunk can still state a position.
        let oldAnchor: Int

        var newNoOrAnchor: Int { newNo == 0 ? 1 : newNo }

        /// Whether this line ended the file without a trailing newline.
        var noEol: Bool { text.last == noEolMark }

        /// The line as it belongs in the patch, without the internal marker.
        var displayText: ArraySlice<UInt8> { noEol ? text.dropLast() : text }

        var marker: UInt8 {
            switch op {
            case .keep: return 0x20
            case .delete: return 0x2D
            case .insert: return 0x2B
            }
        }
    }

    struct Hunk {
        let rows: ArraySlice<Row>
        let headerLine: String
    }

    static func makeRows(_ edits: [Edit]) -> [Row] {
        var rows: [Row] = []
        rows.reserveCapacity(edits.count)
        var oldNo = 1
        var newNo = 1
        for edit in edits {
            switch edit.op {
            case .keep:
                rows.append(Row(op: .keep, text: edit.text, oldNo: oldNo, newNo: newNo, oldAnchor: oldNo))
                oldNo += 1
                newNo += 1
            case .delete:
                rows.append(Row(op: .delete, text: edit.text, oldNo: oldNo, newNo: 0, oldAnchor: oldNo))
                oldNo += 1
            case .insert:
                rows.append(Row(op: .insert, text: edit.text, oldNo: 0, newNo: newNo, oldAnchor: oldNo))
                newNo += 1
            }
        }
        return rows
    }

    static func makeHunks(_ rows: [Row], context: Int, oldTotal: Int, newTotal: Int) -> [Hunk] {
        let changed = rows.indices.filter { rows[$0].op != .keep }
        if changed.isEmpty { return [] }
        func clamp(_ value: Int, _ upper: Int) -> Int { min(max(value, 0), upper) }

        var hunks: [Hunk] = []
        var index = 0
        while index < changed.count {
            let start = clamp(changed[index] - context, rows.count)
            var end = changed[index]
            // Absorb later changes whose context windows touch this one.
            while index + 1 < changed.count, changed[index + 1] - end <= context * 2 {
                index += 1
                end = changed[index]
            }
            end = clamp(end + context + 1, rows.count)
            index += 1

            let slice = rows[start..<end]
            let oldCount = slice.reduce(0) { $0 + ($1.op != .insert ? 1 : 0) }
            let newCount = slice.reduce(0) { $0 + ($1.op != .delete ? 1 : 0) }
            // A hunk with no old lines sits *after* the anchor line, which is
            // how git expresses a pure insertion (e.g. `@@ -0,0 +1,3 @@` for a
            // new file).
            let oldStart = oldCount == 0
                ? clamp(slice[start].oldAnchor - 1, oldTotal)
                : slice.first(where: { $0.op != .insert })!.oldNo
            let newStart = newCount == 0
                ? clamp(slice[start].newNoOrAnchor - 1, newTotal)
                : slice.first(where: { $0.op != .delete })!.newNo

            hunks.append(Hunk(
                rows: slice,
                headerLine: "@@ -\(range(oldStart, oldCount)) +\(range(newStart, newCount)) @@"
            ))
        }
        return hunks
    }

    /// git omits the count when it is exactly one line.
    static func range(_ start: Int, _ count: Int) -> String {
        count == 1 ? "\(start)" : "\(start),\(count)"
    }

    // MARK: - Myers diff

    /// Line diff via Myers' O(ND) algorithm, with common prefix and suffix
    /// trimmed first so the search space matches the size of the actual edit.
    static func diffLines(_ a: [ArraySlice<UInt8>], _ b: [ArraySlice<UInt8>]) -> [Edit] {
        // Lines become small integers once, so every later comparison is O(1)
        // instead of a byte-by-byte walk.
        var ids: [ArraySlice<UInt8>: Int32] = [:]
        func intern(_ lines: [ArraySlice<UInt8>]) -> [Int32] {
            lines.map { line in
                if let id = ids[line] { return id }
                let id = Int32(ids.count)
                ids[line] = id
                return id
            }
        }
        let ka = intern(a)
        let kb = intern(b)

        var prefix = 0
        while prefix < ka.count, prefix < kb.count, ka[prefix] == kb[prefix] { prefix += 1 }
        var suffix = 0
        while suffix < ka.count - prefix, suffix < kb.count - prefix,
              ka[ka.count - 1 - suffix] == kb[kb.count - 1 - suffix] {
            suffix += 1
        }

        var edits: [Edit] = []
        edits.reserveCapacity(max(a.count, b.count))
        for i in 0..<prefix { edits.append(Edit(op: .keep, text: a[i])) }
        let aEnd = a.count - suffix
        let bEnd = b.count - suffix
        for step in myers(Array(ka[prefix..<aEnd]), Array(kb[prefix..<bEnd])) {
            switch step.op {
            case .keep, .delete: edits.append(Edit(op: step.op, text: a[prefix + step.index]))
            case .insert: edits.append(Edit(op: .insert, text: b[prefix + step.index]))
            }
        }
        for i in aEnd..<a.count { edits.append(Edit(op: .keep, text: a[i])) }
        return edits
    }

    /// An edit naming the line by index: into `a` for keep and delete, into
    /// `b` for insert.
    struct Step {
        let op: Op
        let index: Int
    }

    static func myers(_ a: [Int32], _ b: [Int32]) -> [Step] {
        if a.isEmpty && b.isEmpty { return [] }
        if a.isEmpty { return b.indices.map { Step(op: .insert, index: $0) } }
        if b.isEmpty { return a.indices.map { Step(op: .delete, index: $0) } }

        let n = a.count
        let m = b.count
        let maxD = min(n + m, maxEditDistance)
        let offset = maxD
        let size = 2 * maxD + 3
        var v = [Int](repeating: 0, count: size)
        var trace: [[Int]] = []

        for d in 0...maxD {
            trace.append(v)
            var k = -d
            while k <= d {
                defer { k += 2 }
                let ki = k + offset
                if ki <= 0 || ki >= size - 1 { continue }
                var x: Int
                if k == -d || (k != d && v[ki - 1] < v[ki + 1]) {
                    x = v[ki + 1]
                } else {
                    x = v[ki - 1] + 1
                }
                var y = x - k
                while x < n, y < m, a[x] == b[y] {
                    x += 1
                    y += 1
                }
                v[ki] = x
                if x >= n && y >= m { return backtrack(trace, n: n, m: m, offset: offset, size: size) }
            }
        }

        // Beyond the budget: a correct, if verbose, whole-file replacement.
        return a.indices.map { Step(op: .delete, index: $0) } + b.indices.map { Step(op: .insert, index: $0) }
    }

    private static func backtrack(_ trace: [[Int]], n: Int, m: Int, offset: Int, size: Int) -> [Step] {
        var steps: [Step] = []
        var x = n
        var y = m
        var d = trace.count - 1
        while d >= 0 && (x > 0 || y > 0) {
            let v = trace[d]
            let k = x - y
            let ki = k + offset
            let prevK: Int
            if k == -d || (k != d && ki - 1 > 0 && ki + 1 < size && v[ki - 1] < v[ki + 1]) {
                prevK = k + 1
            } else {
                prevK = k - 1
            }
            let prevKi = prevK + offset
            let prevX = (prevKi >= 0 && prevKi < size) ? v[prevKi] : 0
            let prevY = prevX - prevK

            while x > prevX && y > prevY {
                steps.append(Step(op: .keep, index: x - 1))
                x -= 1
                y -= 1
            }
            if d == 0 { break }
            if x > prevX {
                steps.append(Step(op: .delete, index: x - 1))
                x -= 1
            } else if y > prevY {
                steps.append(Step(op: .insert, index: y - 1))
                y -= 1
            }
            d -= 1
        }
        return steps.reversed()
    }

    // MARK: - Helpers

    static func isBinary(_ bytes: [UInt8]) -> Bool {
        bytes.prefix(binarySniffBytes).contains(0)
    }

    /// Lenient UTF-8 decode, back as bytes: malformed sequences become
    /// U+FFFD.
    static func decode(_ bytes: [UInt8]) -> [UInt8] {
        Array(String(decoding: bytes, as: UTF8.self).utf8)
    }

    /// Marks a final line that has no newline after it, so the diff can see
    /// the difference between `x` and `x\n`.
    ///
    /// NUL is safe as the marker precisely because binary content is filtered
    /// out before any of this runs — a NUL byte is exactly what defines binary.
    static let noEolMark: UInt8 = 0

    /// Split into lines without inventing a trailing empty one, marking the
    /// final line when the text did not end with a newline.
    static func keyLines(_ text: [UInt8]) -> [ArraySlice<UInt8>] {
        if text.isEmpty { return [] }
        var lines: [ArraySlice<UInt8>] = []
        var start = 0
        for i in text.indices where text[i] == 0x0A {
            lines.append(text[start..<i])
            start = i + 1
        }
        if start < text.count {
            lines.append(ArraySlice(text[start...] + [noEolMark]))
        }
        return lines
    }
}
