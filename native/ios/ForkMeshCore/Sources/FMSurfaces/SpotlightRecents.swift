import FMApi
import FMBase
import Foundation

public enum SurfaceEntityKind: String, Codable, Hashable, Sendable, CaseIterable {
    case repo
    case issue
    case pull
    case note
    case agent

    public var label: String {
        switch self {
        case .repo: return "Repository"
        case .issue: return "Issue"
        case .pull: return "Pull request"
        case .note: return "Note"
        case .agent: return "Agent run"
        }
    }

    public var systemImage: String {
        switch self {
        case .repo: return "folder"
        case .issue: return "exclamationmark.circle"
        case .pull: return "arrow.triangle.pull"
        case .note: return "note.text"
        case .agent: return "cpu"
        }
    }
}

public struct SurfaceEntityRef: Codable, Hashable, Sendable {
    public var kind: SurfaceEntityKind
    public var id: String

    public init(kind: SurfaceEntityKind, id: String) {
        self.kind = kind
        self.id = id
    }

    public static func of(_ route: Route) -> SurfaceEntityRef? {
        switch route {
        case let .repo(repo, _):
            return SurfaceEntityRef(kind: .repo, id: repo.slug)
        case let .issue(repo, number) where number > 0:
            return SurfaceEntityRef(kind: .issue, id: SurfaceEntityRef.numbered(repo, number))
        case let .pull(repo, number, _) where number > 0:
            return SurfaceEntityRef(kind: .pull, id: SurfaceEntityRef.numbered(repo, number))
        case let .note(id, _) where !id.isEmpty:
            return SurfaceEntityRef(kind: .note, id: id)
        case let .agent(id) where !id.isEmpty:
            return SurfaceEntityRef(kind: .agent, id: id)
        default:
            return nil
        }
    }

    public static func numbered(_ repo: RepoRef, _ number: Int) -> String { "\(repo.slug)#\(number)" }

    public static func parseNumbered(_ id: String) -> (RepoRef, Int)? {
        guard let hash = id.lastIndex(of: "#"), let number = Int(id[id.index(after: hash)...]), number > 0,
              let repo = RepoRef(slug: String(id[..<hash]))
        else { return nil }
        return (repo, number)
    }

    public var route: Route? {
        switch kind {
        case .repo:
            return RepoRef(slug: id).map { .repo($0, section: nil) }
        case .issue:
            return SurfaceEntityRef.parseNumbered(id).map { .issue($0.0, number: $0.1) }
        case .pull:
            return SurfaceEntityRef.parseNumbered(id).map { .pull($0.0, number: $0.1, tab: .conversation) }
        case .note:
            return id.isEmpty ? nil : .note(id: id)
        case .agent:
            return id.isEmpty ? nil : .agent(id: id)
        }
    }

    public var spotlightIdentifier: String { "\(kind.rawValue):\(id)" }

    public static func fromSpotlightIdentifier(_ identifier: String) -> SurfaceEntityRef? {
        guard let colon = identifier.firstIndex(of: ":"),
              let kind = SurfaceEntityKind(rawValue: String(identifier[..<colon]))
        else { return nil }
        let id = String(identifier[identifier.index(after: colon)...])
        let ref = SurfaceEntityRef(kind: kind, id: id)
        return ref.route == nil ? nil : ref
    }

    public var defaultTitle: String {
        switch kind {
        case .repo: return id
        case .issue, .pull:
            if let (repo, number) = SurfaceEntityRef.parseNumbered(id) { return "\(repo.slug) #\(number)" }
            return id
        case .note: return "Note"
        case .agent: return "Agent run"
        }
    }
}

public struct SurfaceRecent: Codable, Hashable, Sendable, Identifiable {
    public var ref: SurfaceEntityRef
    public var title: String
    public var detail: String
    public var openedAtMs: Int64

    public init(ref: SurfaceEntityRef, title: String, detail: String, openedAtMs: Int64) {
        self.ref = ref
        self.title = title
        self.detail = detail
        self.openedAtMs = openedAtMs
    }

    public var id: String { ref.spotlightIdentifier }
}

public final class SurfaceRecentsStore: @unchecked Sendable {
    public static let fileName = "recents.json"
    public static let maxItems = 200
    static let maxBytes = 256 * 1_024

    public let directory: URL
    private let clock: Clock
    private let lock = NSLock()

    public init(directory: URL, clock: Clock = SystemClock()) {
        self.directory = directory
        self.clock = clock
    }

    public static func shared() -> SurfaceRecentsStore? {
        (SurfaceContainer.groupDirectory() ?? SurfaceContainer.appDirectory()).map { SurfaceRecentsStore(directory: $0) }
    }

    private var url: URL { directory.appendingPathComponent(SurfaceRecentsStore.fileName) }

    public func all() -> [SurfaceRecent] {
        lock.lock()
        defer { lock.unlock() }
        return load()
    }

    public func all(of kind: SurfaceEntityKind) -> [SurfaceRecent] {
        all().filter { $0.ref.kind == kind }
    }

    public func find(_ ref: SurfaceEntityRef) -> SurfaceRecent? {
        all().first { $0.ref == ref }
    }

    private func load() -> [SurfaceRecent] {
        SurfaceFiles.read([SurfaceRecent].self, at: url, maxBytes: SurfaceRecentsStore.maxBytes) ?? []
    }

    @discardableResult
    public func record(_ route: Route, title: String = "", detail: String = "") -> SurfaceRecent? {
        guard let ref = SurfaceEntityRef.of(route) else { return nil }
        lock.lock()
        defer { lock.unlock() }
        var list = load()
        let previous = list.first { $0.ref == ref }
        let cleanTitle = SurfaceText.clip(title, 120)
        let cleanDetail = SurfaceText.clip(detail, 200)
        let recent = SurfaceRecent(
            ref: ref,
            title: cleanTitle.isEmpty ? (previous?.title ?? ref.defaultTitle) : cleanTitle,
            detail: cleanDetail.isEmpty ? (previous?.detail ?? ref.kind.label) : cleanDetail,
            openedAtMs: clock.nowMillis)
        list.removeAll { $0.ref == ref }
        list.insert(recent, at: 0)
        if list.count > SurfaceRecentsStore.maxItems { list = Array(list.prefix(SurfaceRecentsStore.maxItems)) }
        return SurfaceFiles.write(list, to: url, maxBytes: SurfaceRecentsStore.maxBytes) ? recent : nil
    }

    public func removeAll() {
        lock.lock()
        defer { lock.unlock() }
        SurfaceFiles.remove(url)
    }

    public static func matching(_ recents: [SurfaceRecent], query: String) -> [SurfaceRecent] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if needle.isEmpty { return recents }
        return recents.filter {
            $0.title.lowercased().contains(needle) || $0.ref.id.lowercased().contains(needle)
                || $0.detail.lowercased().contains(needle)
        }
    }
}
