import FMApi
import FMBase
import Foundation

public struct ReviewEvent: Equatable, Hashable, Sendable {
    public var type: String
    public var id: String
    public var author: String
    public var authorName: String
    public var ts: Int
    public var state: String
    public var path: String
    public var side: String
    public var line: Int
    public var lineStart: Int
    public var lineEnd: Int
    public var threadId: String
    public var parentId: String
    public var suggestionPatch: String
    public var targetPath: String
    public var appliedCommit: String
    public var patchset: String
    public var anchorLines: String
    public var body: String
    public var sig: String

    public init(
        type: String, id: String = "", author: String = "", authorName: String = "", ts: Int = 0, state: String = "",
        path: String = "", side: String = "", line: Int = 0, lineStart: Int = 0, lineEnd: Int = 0,
        threadId: String = "", parentId: String = "", suggestionPatch: String = "", targetPath: String = "",
        appliedCommit: String = "", patchset: String = "", anchorLines: String = "", body: String = "", sig: String = ""
    ) {
        self.type = type
        self.id = id
        self.author = author
        self.authorName = authorName
        self.ts = ts
        self.state = state
        self.path = path
        self.side = side
        self.line = line
        self.lineStart = lineStart
        self.lineEnd = lineEnd
        self.threadId = threadId
        self.parentId = parentId
        self.suggestionPatch = suggestionPatch
        self.targetPath = targetPath
        self.appliedCommit = appliedCommit
        self.patchset = patchset
        self.anchorLines = anchorLines
        self.body = body
        self.sig = sig
    }

    public init(markdown md: String) {
        let meta = PublishedRecords.frontMatter(md)
        func text(_ key: String) -> String { meta[key] ?? "" }
        func number(_ key: String) -> Int { Int(text(key)) ?? 0 }
        self.init(
            type: meta["type"] ?? "comment", id: text("id"), author: text("author"), authorName: text("authorName"),
            ts: number("ts"), state: text("state"), path: text("path"), side: text("side"), line: number("line"),
            lineStart: number("lineStart"), lineEnd: number("lineEnd"), threadId: text("threadId"),
            parentId: text("parentId"), suggestionPatch: ReviewEvent.decodeBlob(text("suggestionPatchB64")),
            targetPath: text("targetPath"), appliedCommit: text("appliedCommit"), patchset: text("patchset"),
            anchorLines: ReviewEvent.decodeBlob(text("anchorLinesB64")),
            body: PublishedRecords.bodyWithoutFrontMatter(md), sig: text("sig"))
    }

    public init(json j: JSONObject) {
        self.init(
            type: j.str("type", "comment"), id: j.str("id"), author: j.str("author"), authorName: j.str("authorName"),
            ts: j.int("ts"), state: j.str("state"), path: j.str("path"), side: j.str("side"), line: j.int("line"),
            lineStart: j.int("lineStart"), lineEnd: j.int("lineEnd"), threadId: j.str("threadId"),
            parentId: j.str("parentId"), suggestionPatch: j.str("suggestionPatch"), targetPath: j.str("targetPath"),
            appliedCommit: j.str("appliedCommit"), patchset: j.str("patchset"), anchorLines: j.str("anchorLines"),
            body: j.str("body"), sig: j.str("sig"))
    }

    public static func decodeBlob(_ text: String) -> String {
        if text.isEmpty { return "" }
        guard let data = try? B64.decodeUnpadded(text) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    public static func encodeBlob(_ text: String) -> String {
        text.isEmpty ? "" : B64.encodeUrlNoPadding(Array(text.utf8))
    }

    public var displayAuthor: String { authorName.isEmpty ? author : authorName }

    public var reviewStanding: String {
        guard type == "review" else { return "" }
        switch state {
        case "approve", "approved": return "approved"
        case "request-changes", "changes-requested", "changes_requested", "requested_changes": return "changes_requested"
        default: return ""
        }
    }
}

public struct PullReviewRecord: Equatable, Sendable {
    public var number: Int
    public var title: String
    public var status: String
    public var author: String
    public var authorName: String
    public var base: String
    public var head: String
    public var branchBacked: Bool
    public var creationBaseOid: String
    public var creationHeadOid: String
    public var description: String
    public var patch: String
    public var events: [ReviewEvent]

    public init(
        number: Int, title: String = "", status: String = "open", author: String = "", authorName: String = "",
        base: String = "", head: String = "", branchBacked: Bool = false, creationBaseOid: String = "",
        creationHeadOid: String = "", description: String = "", patch: String = "", events: [ReviewEvent] = []
    ) {
        self.number = number
        self.title = title
        self.status = status
        self.author = author
        self.authorName = authorName
        self.base = base
        self.head = head
        self.branchBacked = branchBacked
        self.creationBaseOid = creationBaseOid
        self.creationHeadOid = creationHeadOid
        self.description = description
        self.patch = patch
        self.events = events
    }

    public init(number: Int, markdown md: String, events: [ReviewEvent] = [], patch: String = "") {
        let meta = PublishedRecords.frontMatter(md)
        let body = PublishedRecords.bodyWithoutFrontMatter(md)
        let inlinePatch = meta["patch"] ?? meta["diff"] ?? PublishedRecords.extractPatch(body)
        self.init(
            number: Int(meta["number"] ?? "") ?? number,
            title: meta["title"] ?? PublishedRecords.firstHeading(md) ?? "Pull request #\(number)",
            status: meta["status"] ?? "open", author: meta["author"] ?? "", authorName: meta["authorName"] ?? "",
            base: meta["base"] ?? "", head: meta["head"] ?? "", branchBacked: meta["derive"] == "branch",
            creationBaseOid: ReviewRecordRules.commitOid(meta["creationBaseOid"] ?? ""),
            creationHeadOid: ReviewRecordRules.commitOid(meta["creationHeadOid"] ?? ""),
            description: body, patch: patch.isEmpty ? inlinePatch : patch, events: events)
    }

    public var currentPatchset: String { creationHeadOid }

    public var isOpen: Bool { status.dartTrimmed.lowercased() == "open" }
}

public enum ReviewRecordRules {
    public static func commitOid(_ text: String) -> String {
        let value = text.dartTrimmed.lowercased()
        guard value.count == 40 || value.count == 64 else { return "" }
        return value.allSatisfy { $0.isHexDigit } ? value : ""
    }
}

public struct ReviewThread: Equatable, Sendable, Identifiable {
    public var id: String
    public var path: String = ""
    public var side: DiffSide = .new
    public var lineStart = 0
    public var lineEnd = 0
    public var resolved = false
    public var hasSuggestion = false
    public var suggestionState = ""
    public var appliedCommit = ""
    public var patchset = ""
    public var anchorLines = ""
    public var placement: ReviewThreadPlacement = .legacy
    public var currentLineStart = 0
    public var currentLineEnd = 0
    public var events: [ReviewEvent] = []

    public init(id: String) { self.id = id }

    public var isOutdated: Bool { placement == .outdated }
    public var isMoved: Bool { placement == .moved }
    public var placementLabel: String { ReviewAnchoring.label(placement, lineStart: lineStart, lineEnd: lineEnd) }
    public var opening: ReviewEvent? { events.first }

    public var lastEventId: String { events.last(where: { !$0.id.isEmpty })?.id ?? id }

    public var paintedLine: Int? {
        if isOutdated || path.isEmpty || currentLineEnd <= 0 { return nil }
        return currentLineEnd
    }
}

public struct ReviewFileSummary: Equatable, Sendable {
    public var path: String
    public var totalThreads = 0
    public var unresolvedThreads = 0
    public var resolvedThreads = 0
    public var suggestions = 0
    public var outdatedThreads = 0

    public init(path: String) { self.path = path }
}

public struct ReviewSnapshot: Equatable, Sendable {
    public var reviewSummary = ""
    public var topLevelItems = 0
    public var threads: [ReviewThread] = []
    public var files: [String: ReviewFileSummary] = [:]

    public init() {}

    public var totalThreads: Int { threads.count }
    public var unresolvedThreads: Int { threads.filter { !$0.resolved }.count }
    public var resolvedThreads: Int { threads.filter(\.resolved).count }
    public var suggestions: Int { threads.filter(\.hasSuggestion).count }
    public var outdatedThreads: Int { threads.filter(\.isOutdated).count }

    public func thread(_ id: String) -> ReviewThread? { threads.first { $0.id == id } }

    public func threads(inFile path: String) -> (byLine: [DiffSide: [Int: [ReviewThread]]], outdated: [ReviewThread]) {
        var byLine: [DiffSide: [Int: [ReviewThread]]] = [:]
        var outdated: [ReviewThread] = []
        for thread in threads where thread.path == path {
            if let line = thread.paintedLine {
                byLine[thread.side, default: [:]][line, default: []].append(thread)
            } else if thread.isOutdated {
                outdated.append(thread)
            }
        }
        return (byLine, outdated)
    }
}

public enum ReviewSnapshotBuilder {
    public static func build(events: [ReviewEvent], patch: String, currentPatchset: String) -> ReviewSnapshot {
        var snapshot = ReviewSnapshot()
        snapshot.reviewSummary = ReviewGate.summary(events)
        var indexById: [String: Int] = [:]

        func ensure(_ id: String) -> Int {
            let key = id.isEmpty ? "thread:\(snapshot.threads.count + 1)" : id
            if let found = indexById[key] { return found }
            snapshot.threads.append(ReviewThread(id: key))
            indexById[key] = snapshot.threads.count - 1
            return snapshot.threads.count - 1
        }

        func copyAnchor(_ index: Int, _ ev: ReviewEvent) {
            var thread = snapshot.threads[index]
            if !ev.path.isEmpty { thread.path = ev.path }
            if !ev.side.isEmpty { thread.side = DiffSide(wire: ev.side) }
            if ev.lineStart > 0 { thread.lineStart = ev.lineStart }
            if ev.lineEnd > 0 { thread.lineEnd = ev.lineEnd }
            if thread.lineStart == 0 && ev.line > 0 { thread.lineStart = ev.line }
            if thread.lineEnd == 0 && ev.line > 0 { thread.lineEnd = ev.line }
            if !ev.patchset.isEmpty { thread.patchset = ev.patchset }
            if !ev.anchorLines.isEmpty { thread.anchorLines = ev.anchorLines }
            snapshot.threads[index] = thread
        }

        for (i, ev) in events.enumerated() {
            switch ev.type {
            case "comment", "review":
                snapshot.topLevelItems += 1
            case "line-comment":
                let id = ev.id.isEmpty ? "legacy:\(ev.path):\(ev.side):\(ev.line):\(i)" : ev.id
                let index = ensure(id)
                copyAnchor(index, ev)
                snapshot.threads[index].events.append(ev)
            case "thread-comment":
                let index = ensure(ev.threadId)
                copyAnchor(index, ev)
                if !ev.suggestionPatch.isEmpty { snapshot.threads[index].hasSuggestion = true }
                snapshot.threads[index].events.append(ev)
            case "thread-reply":
                snapshot.threads[ensure(ev.threadId)].events.append(ev)
            case "thread-state":
                let index = ensure(ev.threadId)
                snapshot.threads[index].resolved = ev.state == "resolved"
                snapshot.threads[index].events.append(ev)
            case "suggestion-state":
                let index = ensure(ev.threadId)
                snapshot.threads[index].suggestionState = ev.state
                snapshot.threads[index].appliedCommit = ev.appliedCommit
                snapshot.threads[index].events.append(ev)
            default:
                continue
            }
        }

        var indexes: DiffSideIndexes?
        for i in snapshot.threads.indices {
            var thread = snapshot.threads[i]
            if thread.path.isEmpty || thread.lineStart <= 0 {
                thread.placement = .legacy
                thread.currentLineStart = 0
                thread.currentLineEnd = 0
                snapshot.threads[i] = thread
                continue
            }
            if indexes == nil { indexes = DiffSideIndexes(patch: patch) }
            let anchor = ReviewAnchoring.Anchor(
                path: thread.path, side: thread.side, lineStart: thread.lineStart, lineEnd: thread.lineEnd,
                patchset: thread.patchset, anchorLines: thread.anchorLines)
            let placed = ReviewAnchoring.place(
                anchor, side: indexes?.side(thread.path, thread.side), currentPatchset: currentPatchset)
            thread.placement = placed.placement
            thread.currentLineStart = placed.currentLineStart
            thread.currentLineEnd = placed.currentLineEnd
            snapshot.threads[i] = thread
        }

        for thread in snapshot.threads where !thread.path.isEmpty {
            var summary = snapshot.files[thread.path] ?? ReviewFileSummary(path: thread.path)
            summary.totalThreads += 1
            if thread.resolved { summary.resolvedThreads += 1 } else { summary.unresolvedThreads += 1 }
            if thread.hasSuggestion { summary.suggestions += 1 }
            if thread.isOutdated { summary.outdatedThreads += 1 }
            snapshot.files[thread.path] = summary
        }
        return snapshot
    }

    public static func build(_ record: PullReviewRecord) -> ReviewSnapshot {
        build(events: record.events, patch: record.patch, currentPatchset: record.currentPatchset)
    }
}

public enum ReviewGate {
    public static func standings(_ events: [ReviewEvent], excluding author: String? = nil) -> [(String, String)] {
        var order: [String] = []
        var latest: [String: String] = [:]
        for ev in events where ev.type == "review" {
            if let author, ev.author.isEmpty || ev.author == author { continue }
            let standing = ev.reviewStanding
            if standing.isEmpty {
                latest.removeValue(forKey: ev.author)
            } else {
                if latest[ev.author] == nil && !order.contains(ev.author) { order.append(ev.author) }
                latest[ev.author] = standing
            }
        }
        return order.compactMap { who in latest[who].map { (who, $0) } }
    }

    public static func summary(_ events: [ReviewEvent]) -> String {
        let all = standings(events)
        if all.contains(where: { $0.1 == "changes_requested" }) { return "changes_requested" }
        return all.contains(where: { $0.1 == "approved" }) ? "approved" : ""
    }

    public static func changesRequestedBy(_ events: [ReviewEvent], author: String) -> [String] {
        let names = Dictionary(events.map { ($0.author, $0.displayAuthor) }, uniquingKeysWith: { _, last in last })
        return standings(events, excluding: author).filter { $0.1 == "changes_requested" }.map { names[$0.0] ?? $0.0 }
    }

    public static func independentApprovals(_ events: [ReviewEvent], author: String) -> Int {
        standings(events, excluding: author).filter { $0.1 == "approved" }.count
    }

    public static func satisfied(_ events: [ReviewEvent], author: String) -> Bool {
        changesRequestedBy(events, author: author).isEmpty && independentApprovals(events, author: author) > 0
    }
}
