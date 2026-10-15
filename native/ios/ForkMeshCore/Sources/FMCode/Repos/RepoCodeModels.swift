// The Code tab: the tree, branch switcher and breadcrumbs, the go-to-file
// sheet, and the file screen's read-only highlighted viewer with its status
// bar.

import FMApi
import FMBase
import FMDiff
import Foundation
import Observation

/// One `_Crumb` of the explorer footer.
public struct RepoCrumb: Equatable, Hashable, Sendable, Identifiable {
    public var label: String
    /// The folder the crumb opens ("" is the root).
    public var path: String
    public var current: Bool

    public var id: String { path }

    public init(label: String, path: String, current: Bool) {
        self.label = label
        self.path = path
        self.current = current
    }
}

/// `_CodeTabState`.
@MainActor
@Observable
public final class RepoCodeTabModel {
    public let repo: Repository
    public private(set) var path = ""
    public private(set) var selectedRef: String
    public let tree = RepoFuture<RepoTree>()
    public let branches = RepoFuture<[RepoBranch]>()

    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository) {
        self.client = client
        self.repo = repo
        self.selectedRef = repo.defaultBranch
    }

    public static let rootLabel = "Root"

    /// Loads the branch list and the root folder, once.
    public func start() async {
        async let branchLoad: Void = loadBranchesOnce()
        async let treeLoad: Void = loadTreeOnce()
        _ = await (branchLoad, treeLoad)
    }

    private func loadBranchesOnce() async {
        let owner = repo.owner
        let name = repo.name
        await branches.start { try await client.branches(owner, name) }
    }

    private func loadTreeOnce() async {
        let request = (repo.owner, repo.name, path, selectedRef)
        await tree.start { try await client.tree(request.0, request.1, path: request.2, ref: request.3) }
    }

    private func loadTree() async {
        let request = (repo.owner, repo.name, path, selectedRef)
        await tree.load { try await client.tree(request.0, request.1, path: request.2, ref: request.3) }
    }

    /// `_openDir`; `refresh` drops the cached reads first (Retry).
    public func openDir(_ path: String, refresh: Bool = false) async {
        if refresh { client.clearRepoCache(repo.owner, repo.name) }
        self.path = path
        await loadTree()
    }

    /// `_switchRef`: back to the root of the other branch.
    public func switchRef(_ ref: String) async {
        if ref == selectedRef { return }
        client.clearRepoCache(repo.owner, repo.name)
        selectedRef = ref
        path = ""
        await loadTree()
    }

    /// `_up`.
    public func up() async {
        if path.isEmpty { return }
        var parts = path.components(separatedBy: "/")
        parts.removeLast()
        await openDir(parts.joined(separator: "/"))
    }

    public var canGoUp: Bool { !path.isEmpty }

    /// The one-line description only decorates the repository root.
    public var showDescription: Bool { path.isEmpty }

    /// The node that served the tree, hidden while a folder loads.
    public var source: String { tree.loading ? "" : (tree.data?.source ?? "") }

    public var sourceLabel: String? { RepoDisplay.sourceLabel(source) }

    /// `_BranchSelector`'s menu: the selected ref, then the reported branches.
    public var branchNames: [String] {
        var names: [String] = []
        var seen = Set<String>()
        func add(_ name: String) {
            if name.isEmpty || seen.contains(name) { return }
            seen.insert(name)
            names.append(name)
        }
        add(selectedRef)
        for branch in branches.data ?? [] { add(branch.name) }
        return names
    }

    public var canSwitchBranch: Bool { !branchNames.isEmpty }

    /// `_BranchPill`'s text.
    public var branchPillLabel: String { "Branch: \(selectedRef.isEmpty ? "Default branch" : selectedRef)" }

    /// The pill is muted while it cannot switch, loads or failed.
    public var branchPillMuted: Bool { !canSwitchBranch || branches.loading || branches.error != nil }

    /// "Could not load files: <error>".
    public var treeErrorMessage: String? { tree.error.map { "Could not load files: \($0)" } }

    /// `_ExplorerPathBar`'s crumbs, root first.
    public var crumbs: [RepoCrumb] {
        let segments = path.components(separatedBy: "/").filter { !$0.isEmpty }
        var out = [RepoCrumb(label: RepoCodeTabModel.rootLabel, path: "", current: segments.isEmpty)]
        var prefix = ""
        for (i, segment) in segments.enumerated() {
            prefix = prefix.isEmpty ? segment : "\(prefix)/\(segment)"
            out.append(RepoCrumb(label: segment, path: prefix, current: i == segments.count - 1))
        }
        return out
    }

    /// `_FileRow`'s meta line: "Directory", a size or "File", then the age.
    public static func entryMeta(_ entry: RepoTreeEntry, now: Date = Date()) -> String {
        var meta: [String] = []
        if entry.isDirectory {
            meta.append("Directory")
        } else {
            meta.append(entry.size > 0 ? RepoDisplay.humanSize(entry.size) : "File")
        }
        if entry.updatedMs > 0 { meta.append(RepoDisplay.agoLabel(entry.updatedMs, now: now)) }
        return meta.joined(separator: " · ")
    }

    /// The tile glyph of a tree row.
    public static func entryGlyph(_ entry: RepoTreeEntry) -> String {
        entry.isDirectory ? "folder_outlined" : RepoDisplay.fileGlyph(entry.name)
    }
}

/// `_GoToFileSheetState`: code search on the selected ref.
@MainActor
@Observable
public final class RepoGoToFileModel {
    public let repo: Repository
    public let ref: String
    public var query = ""
    public private(set) var lastQuery = ""
    /// Nil until the first search (`_future == null`).
    public private(set) var results: RepoFuture<[RepoCodeSearchMatch]>?

    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository, ref: String) {
        self.client = client
        self.repo = repo
        self.ref = ref
    }

    /// "Searching owner/repo on main."
    public var scopeLine: String { "Searching \(repo.fullName) on \(ref)." }

    /// `_search`: at least two characters.
    public func search() async {
        let q = query.dartTrimmed
        if q.utf16.count < 2 { return }
        lastQuery = q
        let future = RepoFuture<[RepoCodeSearchMatch]>()
        results = future
        let owner = repo.owner
        let name = repo.name
        let ref = self.ref
        await future.load { try await client.searchRepo(owner, name, q, ref: ref).code }
    }

    /// "Could not search files: <error>".
    public var errorMessage: String? { results?.error.map { "Could not search files: \($0)" } }

    public var noMatchesMessage: String { "No code results for \"\(lastQuery)\"." }

    /// `_SearchFileRow`'s subtitle, or nil.
    public static func matchSubtitle(_ match: RepoCodeSearchMatch) -> String? {
        if match.text.isEmpty { return nil }
        return match.line > 0 ? "\(match.line): \(match.text)" : match.text
    }
}

/// What `_FilePreview` shows for a loaded blob.
public enum RepoFilePreview: Equatable, Sendable {
    case image
    /// Not previewed; `message` explains why.
    case unsupported(message: String)
    case text(String)
}

/// `RepoFileScreen`: one file of a remote repository.
@MainActor
@Observable
public final class RepoFileModel {
    public let repo: Repository
    public let path: String
    public let ref: String
    public let blob = RepoFuture<RepoBlob>()

    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository, path: String, ref: String = "") {
        self.client = client
        self.repo = repo
        self.path = path
        self.ref = ref
    }

    public func start() async {
        let request = (repo.owner, repo.name, path, ref)
        await blob.start { try await client.blob(request.0, request.1, request.2, ref: request.3) }
    }

    /// The app bar title.
    public var fileName: String { RepoDisplay.baseName(path) }

    /// Whether to offer editing: decided from the path alone.
    public var editable: Bool { RepoDisplay.previewKind(path) == .text }

    public var rawURL: String { client.rawURL(repo.owner, repo.name, path, ref: ref).absoluteString }

    /// The blob to preview (`snap.data ?? RepoBlob(path: path, content: '')`).
    public var loadedBlob: RepoBlob { blob.data ?? RepoBlob(path: path, content: "") }

    public static let videoMessage =
        "Video preview is not bundled in the mobile app yet. Use the raw file URL below to open or download it."
    public static let binaryMessage = "This looks like a binary file, so ForkMesh is not showing it as text."

    /// `_FilePreview.build`'s decision.
    public static func preview(for blob: RepoBlob) -> RepoFilePreview {
        switch RepoDisplay.previewKind(blob.path) {
        case .image:
            return .image
        case .video:
            return .unsupported(message: videoMessage)
        case .text:
            guard let decoded = RepoDisplay.decodedText(blob) else { return .unsupported(message: binaryMessage) }
            return .text(decoded.isEmpty ? "Empty file." : decoded)
        }
    }

    /// The image could not be drawn.
    public static func imageFailureMessage(_ error: String) -> String {
        "Could not render this image preview. \(error)"
    }
}

/// `_CodePreviewState`: lines, highlighting (built per line the first time it
/// is asked for), gutter width and the status bar.
@MainActor
public final class RepoCodePreview {
    public let path: String
    public let content: String
    public let lines: [String]
    public let highlighter: CodeHighlighter
    public let longestLine: Int
    public let byteCount: Int
    private let startsInBlockComment: [Bool]
    private var pieceCache: [Int: [(text: String, kind: TokenKind)]] = [:]

    public init(path: String, content: String) {
        self.path = path
        self.content = content
        let lines = RepoDisplay.dartLines(content)
        self.lines = lines
        let highlighter = CodeHighlighter(CodeLang.forPath(path))
        self.highlighter = highlighter
        self.startsInBlockComment = highlighter.blockCommentStarts(lines)
        self.longestLine = lines.reduce(0) { max($0, $1.utf16.count) }
        self.byteCount = content.utf8.count
    }

    public var lineCount: Int { lines.count }
    public var languageLabel: String { highlighter.lang.label }

    /// `(digits * 8.5 + 24).clamp(44, 84)`.
    public var gutterWidth: Double {
        let digits = String(lines.count).count
        return min(max(Double(digits) * 8.5 + 24, 44), 84)
    }

    /// `longestLine * 8.2 + gutterWidth + 40`: the scroll width.
    public var estimatedWidth: Double { Double(longestLine) * 8.2 + gutterWidth + 40 }

    /// The coloured pieces of line `index` (0-based).
    public func pieces(_ index: Int) -> [(text: String, kind: TokenKind)] {
        if let cached = pieceCache[index] { return cached }
        let built = highlighter.pieces(lines[index], startsInBlockComment: startsInBlockComment[index])
        pieceCache[index] = built
        return built
    }

    /// `_EditorStatusBar`'s text; `activeLine` is 1-based.
    public func statusLine(activeLine: Int) -> String {
        "Ln \(activeLine) · \(lineCount) \(lineCount == 1 ? "line" : "lines") · \(RepoDisplay.humanSize(byteCount))"
    }
}
