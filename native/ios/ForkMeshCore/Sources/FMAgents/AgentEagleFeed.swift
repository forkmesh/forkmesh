import FMAccount
import FMControl
import Foundation
import Observation

@MainActor
@Observable
public final class AgentEagleText {
    public fileprivate(set) var text: String = ""
}

@MainActor
@Observable
public final class AgentEagleFeed {
    @ObservationIgnored public private(set) var dialogs: [String: String] = [:]
    @ObservationIgnored private var texts: [String: AgentEagleText] = [:]
    private var desktopRows: [String: String] = [:]
    @ObservationIgnored private var feeds: ControlWatchFeeds?
    @ObservationIgnored private var followed: Set<String> = []
    @ObservationIgnored private var launched: [String: String] = [:]
    @ObservationIgnored private var rows: [String: UnifiedAgentRow] = [:]
    @ObservationIgnored private var fetch: (@MainActor (UnifiedAgentRow) async -> String)?
    @ObservationIgnored private var asked: Set<String> = []
    @ObservationIgnored private var waiting: [String] = []
    @ObservationIgnored private var fetching = 0
    @ObservationIgnored private var read: [String: Int] = [:]
    @ObservationIgnored private let said: @MainActor (String) -> String

    public nonisolated static let fetchLimit = 3

    public init(dialog: @escaping @MainActor (String) -> String = { AgentLines.dialog($0) }) {
        said = dialog
    }

    public static func desktopKey(repo: String, localId: Int) -> String {
        "\(repo.lowercased())#\(localId)"
    }

    public func follow(
        _ rows: [UnifiedAgentRow], feeds make: @MainActor () -> ControlWatchFeeds,
        fetch: (@MainActor (UnifiedAgentRow) async -> String)? = nil
    ) {
        if let fetch { self.fetch = fetch }
        let live = rows.filter { $0.isActive && $0.isDesktop && $0.localId != 0 }
        let feeds = self.feeds ?? start(make())
        var keys = desktopRows
        for row in rows {
            self.rows[row.id] = row
            if row.isDesktop && row.localId != 0 && !row.outside {
                keys[Self.desktopKey(repo: row.repoName, localId: row.localId)] = row.id
            }
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
        rows = [:]
        fetch = nil
        asked = []
        waiting = []
        read = [:]
        dialogs = [:]
        for box in texts.values where !box.text.isEmpty { box.text = "" }
        texts = [:]
        if !desktopRows.isEmpty { desktopRows = [:] }
    }

    private func text(for id: String) -> String {
        if let box = texts[id] { return box.text }
        let box = AgentEagleText()
        box.text = dialogs[id] ?? ""
        texts[id] = box
        return box.text
    }

    private func rowId(_ line: AgentLine) -> String? {
        switch line.source {
        case .control(let row):
            return row.id
        case .entry(.desktop(let repo, let session)):
            return desktopRows[Self.desktopKey(repo: repo.name, localId: session.id)]
        case .entry(.launched):
            return nil
        }
    }

    public func dialog(_ line: AgentLine) -> String {
        switch line.source {
        case .control, .entry(.desktop):
            return rowId(line).map { text(for: $0) } ?? ""
        case .entry(.launched(_, let session, _, _)):
            let said = AgentLines.dialog(session)
            if said.isEmpty { return launched[line.id] ?? "" }
            launched[line.id] = said
            return said
        }
    }

    public func recall(_ line: AgentLine) {
        guard !line.isActive, fetch != nil, let id = rowId(line), (dialogs[id] ?? "").isEmpty, !asked.contains(id) else { return }
        if rows[id] == nil, case .control(let row) = line.source { rows[id] = row }
        guard rows[id] != nil else { return }
        asked.insert(id)
        waiting.append(id)
        drain()
    }

    private func drain() {
        while fetching < Self.fetchLimit, !waiting.isEmpty {
            let id = waiting.removeFirst()
            guard let fetch, let row = rows[id] else { continue }
            fetching += 1
            Task { @MainActor [weak self] in
                let text = await fetch(row) + "\n"
                guard let self else { return }
                let said = self.said(text)
                self.fetching -= 1
                if self.asked.contains(id) { self.recalled(id, said) }
                self.drain()
            }
        }
    }

    private func recalled(_ id: String, _ said: String) {
        guard !said.isEmpty, (dialogs[id] ?? "").isEmpty else { return }
        dialogs[id] = said
        texts[id]?.text = said
    }

    private func start(_ feeds: ControlWatchFeeds) -> ControlWatchFeeds {
        feeds.onChange = { [weak self] in self?.refresh() }
        self.feeds = feeds
        return feeds
    }

    private func refresh() {
        guard let feeds else { return }
        var next = dialogs
        PerformanceMonitorService.section("agents.eagle") {
            for id in followed {
                let revision = feeds.revision(id)
                if read[id] == revision { continue }
                read[id] = revision
                let line = said(feeds.text(id)?.text ?? "")
                if !line.isEmpty { next[id] = line }
            }
        }
        guard next != dialogs else { return }
        dialogs = next
        for (id, text) in next where texts[id]?.text != text {
            texts[id]?.text = text
        }
    }
}
