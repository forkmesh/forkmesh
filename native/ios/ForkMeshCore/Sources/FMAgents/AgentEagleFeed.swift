import FMControl
import Foundation
import Observation

@MainActor
@Observable
public final class AgentEagleFeed {
    public private(set) var dialogs: [String: String] = [:]
    private var desktopRows: [String: String] = [:]
    @ObservationIgnored private var feeds: ControlWatchFeeds?
    @ObservationIgnored private var followed: Set<String> = []
    @ObservationIgnored private var launched: [String: String] = [:]

    public init() {}

    public static func desktopKey(repo: String, localId: Int) -> String {
        "\(repo.lowercased())#\(localId)"
    }

    public func follow(_ rows: [UnifiedAgentRow], feeds make: @MainActor () -> ControlWatchFeeds) {
        let live = rows.filter { $0.isActive && $0.isDesktop && $0.localId != 0 }
        let feeds = self.feeds ?? start(make())
        var keys = desktopRows
        for row in live where !row.outside {
            keys[Self.desktopKey(repo: row.repoName, localId: row.localId)] = row.id
        }
        if keys != desktopRows { desktopRows = keys }
        followed.formUnion(live.map(\.id))
        feeds.follow(live)
        refresh()
    }

    public func stop() {
        feeds?.stop()
        feeds = nil
        followed = []
        launched = [:]
        if !dialogs.isEmpty { dialogs = [:] }
        if !desktopRows.isEmpty { desktopRows = [:] }
    }

    public func dialog(_ line: AgentLine) -> String {
        guard line.isActive else { return "" }
        switch line.source {
        case .control(let row):
            return dialogs[row.id] ?? ""
        case .entry(.desktop(let repo, let session)):
            return desktopRows[Self.desktopKey(repo: repo.name, localId: session.id)].flatMap { dialogs[$0] } ?? ""
        case .entry(.launched(_, let session, _, _)):
            let said = AgentLines.dialog(session)
            if said.isEmpty { return launched[line.id] ?? "" }
            launched[line.id] = said
            return said
        }
    }

    private func start(_ feeds: ControlWatchFeeds) -> ControlWatchFeeds {
        feeds.onChange = { [weak self] in self?.refresh() }
        self.feeds = feeds
        return feeds
    }

    private func refresh() {
        guard let feeds else { return }
        var next = dialogs
        for id in followed {
            let said = AgentLines.dialog(feeds.text(id)?.text ?? "")
            if !said.isEmpty { next[id] = said }
        }
        if next != dialogs { dialogs = next }
    }
}
