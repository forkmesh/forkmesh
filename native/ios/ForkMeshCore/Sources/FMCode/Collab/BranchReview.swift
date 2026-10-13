import FMApi
import FMBase
import FMCrypto
import FMDiff
import Foundation
import Observation

@MainActor
public protocol ReviewThreadActing: AnyObject {
    var canWrite: Bool { get }
    var busy: Bool { get }
    func isPending(_ event: ReviewEvent) -> Bool
    func reply(_ thread: ReviewThread, body: String) async -> CollabNotice
    func setResolved(_ thread: ReviewThread, _ resolved: Bool) async -> CollabNotice
}

extension PullReviewSession: ReviewThreadActing {}

public enum BranchReviewPaths {
    public static let root = ".forkmesh/branch-reviews"
    public static let keyLength = 24

    public static func key(_ branch: String) -> String {
        String(Sha256.hex(Array(branch.utf8)).lowercased().prefix(keyLength))
    }

    public static func folder(_ branch: String) -> String { "\(root)/\(key(branch))" }

    public static func isEventFile(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        guard bytes.count > 9, bytes[4] == UInt8(ascii: "-"), name.hasSuffix(".md") else { return false }
        guard bytes[0..<4].allSatisfy({ $0 >= UInt8(ascii: "0") && $0 <= UInt8(ascii: "9") }) else { return false }
        let kind = bytes[5..<(bytes.count - 3)]
        return !kind.isEmpty && kind.allSatisfy { ($0 >= UInt8(ascii: "a") && $0 <= UInt8(ascii: "z")) || $0 == UInt8(ascii: "-") }
    }

    public static func eventFiles(_ entries: [RepoTreeEntry], limit: Int) -> [RepoTreeEntry] {
        entries.filter { !$0.isDirectory && isEventFile($0.name) }
            .sorted { $0.name < $1.name }
            .suffix(limit)
            .map { $0 }
    }
}

@MainActor
public final class BranchReviewLedger {
    let reader: ReviewLedgerReading
    let owner: String
    let name: String

    public init(reader: ReviewLedgerReading, owner: String, name: String) {
        self.reader = reader
        self.owner = owner
        self.name = name
    }

    public func load(_ branch: String) async -> [ReviewEvent] {
        let folder = BranchReviewPaths.folder(branch)
        guard let listing = try? await reader.tree(owner, name, path: folder, ref: ReviewLedger.ref) else { return [] }
        var events: [ReviewEvent] = []
        for file in BranchReviewPaths.eventFiles(listing.entries, limit: ReviewLedger.maxEvents) {
            let path = file.path.isEmpty ? "\(folder)/\(file.name)" : file.path
            if let blob = try? await reader.blob(owner, name, path, ref: ReviewLedger.ref) {
                let event = ReviewEvent(markdown: blob.content)
                if CollabSigner.branchEventTypes.contains(event.type) { events.append(event) }
            }
        }
        return events
    }
}

public enum BranchReviewText {
    public static let commentHint = "Tap a line number to comment. Tap another line on the same side to select a range."
    public static let noSigningKey = "This device has no signing key, so it can read comments but not write them."
    public static let sent = "Comment sent. It appears for everyone once the owner's node applies it."
    public static let pickLine = "Pick a line first."
    public static let noBranch = "Comments need a branch that differs from its base."

    public static func threadSummary(open: Int, resolved: Int) -> String {
        if open == 0 && resolved == 0 { return "" }
        var parts: [String] = []
        if open > 0 { parts.append("\(open) open \(open == 1 ? "thread" : "threads")") }
        if resolved > 0 { parts.append("\(resolved) resolved") }
        return parts.joined(separator: " · ")
    }
}

@MainActor
@Observable
public final class BranchReviewSession: ReviewThreadActing {
    public let owner: String
    public let name: String
    public let branch: String

    public private(set) var events: [ReviewEvent] = []
    public private(set) var pending: [ReviewEvent] = []
    public private(set) var snapshot = ReviewSnapshot()
    public private(set) var patch = ""
    public private(set) var headOid = ""
    public private(set) var baseOid = ""
    public private(set) var baseName = ""
    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var busy = false
    public var selection: DiffLineSelection?

    @ObservationIgnored let ledger: BranchReviewLedger
    @ObservationIgnored let writer: CollabRecordWriter
    @ObservationIgnored let clock: Clock
    @ObservationIgnored var counter = 0
    @ObservationIgnored var byLine: [String: [DiffSide: [Int: [ReviewThread]]]] = [:]
    @ObservationIgnored var atTop: [String: [ReviewThread]] = [:]

    public init(owner: String, name: String, branch: String, reader: ReviewLedgerReading, writer: CollabRecordWriter,
                clock: Clock = SystemClock()) {
        self.owner = owner
        self.name = name
        self.branch = branch
        self.ledger = BranchReviewLedger(reader: reader, owner: owner, name: name)
        self.writer = writer
        self.clock = clock
    }

    public var canWrite: Bool { writer.signer != nil }

    public var hasBranchHead: Bool {
        !branch.isEmpty && !headOid.isEmpty && headOid.lowercased() != baseOid.lowercased() && branch != baseName
    }

    public var canComment: Bool { canWrite && hasBranchHead }

    public var summary: String {
        BranchReviewText.threadSummary(open: snapshot.unresolvedThreads, resolved: snapshot.resolvedThreads)
    }

    public func attach(_ comparison: RepoComparison, base: String) {
        patch = comparison.patch
        headOid = comparison.headOid
        baseOid = comparison.baseOid
        baseName = base
        if let current = selection, !hasBranchHead || !DiffSideIndexes(patch: patch).contains(current) {
            selection = nil
        }
        rebuild()
    }

    public func load() async {
        loading = true
        defer { loading = false }
        let read = await ledger.load(branch)
        events = read
        let stored = Set(read.map(\.id).filter { !$0.isEmpty })
        pending.removeAll { stored.contains($0.id) }
        loaded = true
        rebuild()
    }

    func rebuild() {
        snapshot = ReviewSnapshotBuilder.build(events: events + pending, patch: patch, currentPatchset: "")
        var lines: [String: [DiffSide: [Int: [ReviewThread]]]] = [:]
        var top: [String: [ReviewThread]] = [:]
        for thread in snapshot.threads where !thread.path.isEmpty {
            if let line = thread.paintedLine {
                lines[thread.path, default: [:]][thread.side, default: [:]][line, default: []].append(thread)
            } else if thread.isOutdated || thread.lineStart <= 0 {
                top[thread.path, default: []].append(thread)
            }
        }
        byLine = lines
        atTop = top
    }

    public func threads(path: String, at anchors: [(DiffSide, Int)]) -> [ReviewThread] {
        guard let sides = byLine[path] else { return [] }
        var seen: Set<String> = []
        var out: [ReviewThread] = []
        for (side, line) in anchors where line > 0 {
            for thread in sides[side]?[line] ?? [] where seen.insert(thread.id).inserted { out.append(thread) }
        }
        return out
    }

    public func topThreads(path: String) -> [ReviewThread] { atTop[path] ?? [] }

    public func threadCount(_ path: String) -> Int { snapshot.files[path]?.totalThreads ?? 0 }

    public func isSelected(path: String, line: DiffLine) -> Bool {
        guard let selection, selection.path == path else { return false }
        return selection.contains(line)
    }

    public func tap(path: String, line: DiffLine) {
        guard canComment else { return }
        selection = DiffLineSelection.tapped(selection, path: path, line: line)
    }

    public func clearSelection() { selection = nil }

    public var selectionPreview: String {
        guard let selection else { return "" }
        return ReviewAnchoring.anchorLines(
            patch: patch, path: selection.path, side: selection.side, lineStart: selection.lineStart,
            lineEnd: selection.lineEnd)
    }

    func nextId(_ type: String) -> String {
        counter += 1
        return ReviewComposer.eventId(type: type, ts: clock.nowMillis, random: "\(counter)-\(Hex.encode(Bytes.random(4)))")
    }

    func send(_ fields: ReviewEventFields, ok: String) async -> CollabNotice {
        if busy { return CollabNotice("Still sending.", isError: true) }
        busy = true
        defer { busy = false }
        do {
            let event = try await writer.sendBranch(branch, fields)
            pending.append(ReviewEvent(json: event))
            rebuild()
            return CollabNotice(ok)
        } catch {
            return CollabNotice(CollabCopy.runFailure(error), isError: true)
        }
    }

    public func comment(body: String) async -> CollabNotice {
        if let problem = ReviewComposer.validate(body) { return CollabNotice(problem, isError: true) }
        guard let selection else { return CollabNotice(BranchReviewText.pickLine, isError: true) }
        guard hasBranchHead else { return CollabNotice(BranchReviewText.noBranch, isError: true) }
        let fields = ReviewComposer.branchComment(selection, body: body, patch: patch, newId: { nextId("thread") })
        let notice = await send(fields, ok: BranchReviewText.sent)
        if !notice.isError { self.selection = nil }
        return notice
    }

    public func reply(_ thread: ReviewThread, body: String) async -> CollabNotice {
        if let problem = ReviewComposer.validate(body) { return CollabNotice(problem, isError: true) }
        return await send(ReviewComposer.reply(to: thread, body: body), ok: "Reply sent.")
    }

    public func setResolved(_ thread: ReviewThread, _ resolved: Bool) async -> CollabNotice {
        await send(
            ReviewComposer.setResolved(thread, resolved: resolved, reopened: "unresolved"),
            ok: resolved ? "Thread resolved." : "Thread reopened.")
    }

    public func isPending(_ event: ReviewEvent) -> Bool { pending.contains(event) }
}

public enum DiffThreadAnchors {
    public static func unified(_ line: DiffLine) -> [(DiffSide, Int)] {
        switch line.kind {
        case .removed: return [(.old, line.oldNumber)]
        case .added: return [(.new, line.newNumber)]
        case .context: return [(.new, line.newNumber), (.old, line.oldNumber)]
        }
    }

    public static func split(_ row: DiffSplitRow) -> [(DiffSide, Int)] {
        var out: [(DiffSide, Int)] = []
        if let left = row.left, left.kind != .added { out.append((.old, left.oldNumber)) }
        if let right = row.right, right.kind != .removed { out.append((.new, right.newNumber)) }
        return out
    }
}

extension DiffSideIndexes {
    func contains(_ selection: DiffLineSelection) -> Bool {
        side(selection.path, selection.side)?.contains(line: selection.lineStart) ?? false
    }
}
