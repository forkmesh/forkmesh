import FMApi
import FMBase
import FMDiff
import Foundation
import Observation

@MainActor
@Observable
public final class PullReviewSession {
    public let owner: String
    public let name: String
    public let number: Int

    public private(set) var record: PullReviewRecord?
    public private(set) var snapshot = ReviewSnapshot()
    public private(set) var files: [DiffFile] = []
    public private(set) var loading = false
    public private(set) var loadError: String?
    public private(set) var pending: [ReviewEvent] = []
    public private(set) var busy = false
    public var selection: DiffLineSelection?
    public var drafts = ReviewDraftBook()
    public var viewed: Set<String> = []

    @ObservationIgnored let ledger: ReviewLedger
    @ObservationIgnored let writer: CollabRecordWriter
    @ObservationIgnored let clock: Clock
    @ObservationIgnored var counter = 0

    public init(owner: String, name: String, number: Int, reader: ReviewLedgerReading, writer: CollabRecordWriter,
                clock: Clock = SystemClock()) {
        self.owner = owner
        self.name = name
        self.number = number
        self.ledger = ReviewLedger(reader: reader, owner: owner, name: name)
        self.writer = writer
        self.clock = clock
    }

    public var canWrite: Bool { writer.signer != nil }
    public var patchset: String { record?.currentPatchset ?? "" }
    public var patch: String { record?.patch ?? "" }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            let loaded = try await ledger.load(number)
            record = loaded
            files = UnifiedDiff.parse(loaded.patch)
            loadError = nil
            rebuild()
        } catch {
            loadError = "\(error)"
        }
    }

    func rebuild() {
        guard let record else { return }
        snapshot = ReviewSnapshotBuilder.build(events: record.events + pending, patch: record.patch, currentPatchset: record.currentPatchset)
    }

    public func file(_ path: String) -> DiffFile? { files.first { $0.path == path } }

    public func threadCount(_ path: String) -> Int { snapshot.files[path]?.totalThreads ?? 0 }

    public func tap(path: String, line: DiffLine) {
        selection = DiffLineSelection.tapped(selection, path: path, line: line)
    }

    public func clearSelection() { selection = nil }

    public func toggleViewed(_ path: String) {
        if viewed.contains(path) { viewed.remove(path) } else { viewed.insert(path) }
    }

    public var viewedSummary: String { "\(viewed.intersection(files.map(\.path)).count) of \(files.count) viewed" }

    func nextId(_ type: String) -> String {
        counter += 1
        return ReviewComposer.eventId(type: type, ts: clock.nowMillis, random: "\(counter)-\(Hex.encode(Bytes.random(4)))")
    }

    public func addDraft(body: String, suggestion: String = "") -> String? {
        guard let selection else { return "Pick a line first." }
        let problem = drafts.add(selection, body: body, id: nextId("draft"), suggestion: suggestion)
        if problem == nil { self.selection = nil }
        return problem
    }

    func send(_ fields: [ReviewEventFields], ok: String) async -> CollabNotice {
        if busy { return CollabNotice("Still sending.", isError: true) }
        busy = true
        defer { busy = false }
        do {
            for item in fields {
                let event = try await writer.sendPull(number, item)
                pending.append(ReviewEvent(json: event))
            }
            rebuild()
            return CollabNotice(ok)
        } catch {
            rebuild()
            return CollabNotice(CollabCopy.runFailure(error), isError: true)
        }
    }

    public func commentNow(body: String, suggestion: String = "") async -> CollabNotice {
        if let problem = ReviewComposer.validate(body) { return CollabNotice(problem, isError: true) }
        guard let selection else { return CollabNotice("Pick a line first.", isError: true) }
        let fields = ReviewComposer.inlineComment(
            selection, body: body, patch: patch, patchset: patchset, suggestion: suggestion, newId: { nextId("line") })
        let notice = await send([fields], ok: "Comment sent. It appears for everyone once the owner's node applies it.")
        if !notice.isError { self.selection = nil }
        return notice
    }

    public func reply(_ thread: ReviewThread, body: String) async -> CollabNotice {
        if let problem = ReviewComposer.validate(body) { return CollabNotice(problem, isError: true) }
        return await send([ReviewComposer.reply(to: thread, body: body)], ok: "Reply sent.")
    }

    public func setResolved(_ thread: ReviewThread, _ resolved: Bool) async -> CollabNotice {
        await send([ReviewComposer.setResolved(thread, resolved: resolved)], ok: resolved ? "Thread resolved." : "Thread reopened.")
    }

    public func submit(_ verdict: ReviewVerdict, body: String) async -> CollabNotice {
        if let problem = ReviewComposer.validateReview(verdict, body: body, drafts: drafts.drafts) {
            return CollabNotice(problem, isError: true)
        }
        let events = ReviewComposer.submission(
            verdict: verdict, body: body, drafts: drafts.drafts, patch: patch, patchset: patchset, newId: { nextId("thread") })
        let before = pending.count
        let notice = await send(events, ok: verdict == .comment ? "Review sent." : "\(verdict.title) sent.")
        let sentDrafts = min(max(0, pending.count - before), drafts.count)
        for draft in drafts.drafts.prefix(sentDrafts) { drafts.remove(draft.id) }
        return notice
    }

    public func isPending(_ event: ReviewEvent) -> Bool { pending.contains(event) }
}

@MainActor
@Observable
public final class IssueBoardSession {
    public let owner: String
    public let name: String
    public private(set) var issues: [Issue] = []
    public private(set) var columns: [String]
    public private(set) var loading = false
    public private(set) var loadError: String?
    public private(set) var movingNumber: Int?
    public var query = ""

    @ObservationIgnored let load: () async throws -> [Issue]
    @ObservationIgnored let writer: CollabRecordWriter
    @ObservationIgnored let store: KeyValueStore

    public init(owner: String, name: String, store: KeyValueStore, writer: CollabRecordWriter,
                load: @escaping () async throws -> [Issue]) {
        self.owner = owner
        self.name = name
        self.store = store
        self.writer = writer
        self.load = load
        self.columns = IssueBoard.load(store, owner: owner, name: name)
    }

    public var layout: [IssueBoardColumn] {
        let needle = query.dartTrimmed.lowercased()
        return IssueBoard.layout(issues, columns: columns) { issue in
            needle.isEmpty || issue.title.lowercased().contains(needle) || "#\(issue.number)" == needle
                || issue.labels.contains { $0.lowercased().contains(needle) }
        }
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            issues = try await load()
            loadError = nil
        } catch {
            loadError = dartErrorText(error)
        }
    }

    public func saveColumns(_ names: [String]) -> String? {
        if let problem = IssueBoard.validate(names) { return problem }
        IssueBoard.save(store, owner: owner, name: name, columns: names)
        columns = IssueBoard.load(store, owner: owner, name: name)
        return nil
    }

    public func move(_ issue: Issue, to column: String) async -> CollabNotice? {
        guard let move = IssueBoard.move(issue, to: column, columns: columns) else { return nil }
        guard movingNumber == nil else { return CollabNotice("Still moving another card.", isError: true) }
        movingNumber = issue.number
        defer { movingNumber = nil }
        do {
            try await writer.applyIssue(issue.number, move.edits)
            if let index = issues.firstIndex(where: { $0.number == issue.number }) {
                for edit in move.edits {
                    if edit.type == "labels" { issues[index].labels = edit.fields.strings("labels") }
                    if edit.type == "status" { issues[index].status = edit.fields.str("status") }
                }
            }
            return CollabNotice(move.summary)
        } catch {
            return CollabNotice(CollabCopy.runFailure(error), isError: true)
        }
    }
}
