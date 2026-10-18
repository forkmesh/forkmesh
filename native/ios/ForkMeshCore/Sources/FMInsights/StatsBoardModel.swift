import FMBase
import FMControl
import Foundation
import Observation

public enum StatsTiles {
    public static func network(_ snapshot: NetworkSnapshot) -> [String: StatsTileData] {
        [
            "repos": StatsTileData(value: StatsFormat.compactNumber(Double(snapshot.repos)), note: "in the catalog",
                                   trend: .upIsGood, reading: Double(snapshot.repos)),
            "nodes": StatsTileData(value: StatsFormat.compactNumber(Double(snapshot.onlineNodes.count)), note: "connected now",
                                   status: snapshot.onlineNodes.isEmpty ? .critical : .good, trend: .upIsGood,
                                   reading: Double(snapshot.onlineNodes.count)),
            "hosts": StatsTileData(value: StatsFormat.compactNumber(Double(snapshot.hosts)), note: "serving repositories",
                                   status: snapshot.hosts == 0 ? .critical : snapshot.hosts == 1 ? .warning : .good,
                                   trend: .upIsGood, reading: Double(snapshot.hosts)),
        ]
    }

    public static func history(_ history: OnlineHistory) -> [String: StatsTileData] {
        let total = history.hours.reduce(0) { $0 + $1.nodeMinutes }
        return [
            "nodeHours": StatsTileData(
                value: StatsFormat.compactDuration(Int64(total) * 60_000), note: "node time online, 24 h",
                series: history.series, available: !history.hours.isEmpty,
                status: history.hours.isEmpty ? .none : total == 0 ? .critical : .good, trend: .upIsGood),
        ]
    }

    public static func status(_ report: StatusReport) -> [String: StatsTileData] {
        let down = report.down.count
        let uptime = report.websiteUptime24h
        var tiles: [String: StatsTileData] = [
            "uptime": StatsTileData(
                value: StatsFormat.percent(uptime, digits: 2), note: report.headline, available: uptime != nil,
                status: uptime.map { $0 >= 99.5 ? .good : $0 >= 95 ? .warning : .critical } ?? .none,
                trend: .upIsGood, reading: uptime),
            "incidents": StatsTileData(
                value: "\(down)", note: down == 0 ? "all core systems up" : report.down.prefix(2).map(\.label).joined(separator: ", "),
                status: down == 0 ? .good : .critical, trend: .downIsGood, reading: Double(down)),
        ]
        if let errors = report.current.errors24h {
            tiles["errors"] = StatsTileData(
                value: StatsFormat.compactNumber(Double(errors)), note: "logged by the server",
                status: errors == 0 ? .good : errors < 50 ? .warning : .critical, trend: .downIsGood,
                reading: Double(errors))
        } else {
            tiles["errors"] = .unavailable
        }
        return tiles
    }

    public static func desktop(_ health: DesktopHealth) -> [String: StatsTileData] {
        var tiles: [String: StatsTileData] = [
            "agents": StatsTileData(
                value: "\(health.agentsRunning)", note: health.agentsQueued > 0 ? "\(health.agentsQueued) queued" : "none queued",
                status: health.agentsQueued > 0 ? .warning : health.agentsRunning > 0 ? .good : .none, reading: Double(health.agentsRunning)),
            "stalls": StatsTileData(value: "\(health.stalls)", note: "recorded on the desktop",
                                    status: health.stalls == 0 ? .good : health.stalls < 10 ? .warning : .critical,
                                    trend: .downIsGood, reading: Double(health.stalls)),
        ]
        tiles["desktopLoad"] = health.cpuPercent >= 0
            ? StatsTileData(value: String(format: "%.0f%%", health.cpuPercent), note: health.level.isEmpty ? "CPU" : health.level,
                            status: health.tone, trend: .downIsGood, reading: health.cpuPercent)
            : .unavailable
        if let share = health.memoryUsedShare {
            tiles["desktopMemory"] = StatsTileData(
                value: String(format: "%.0f%%", share * 100),
                note: "\(StatsFormat.compactBytes(health.memoryTotal - health.memoryAvailable)) of \(StatsFormat.compactBytes(health.memoryTotal))",
                status: share > 0.9 ? .critical : share > 0.75 ? .warning : .good, trend: .downIsGood, reading: share * 100)
        } else {
            tiles["desktopMemory"] = .unavailable
        }
        return tiles
    }

    public static func codemagic(_ usage: CodemagicUsage, calendar: Calendar = .current) -> [String: StatsTileData] {
        var note = "of \(usage.limitMinutes) left"
        if usage.resetsAt > 0 {
            let reset = Date(timeIntervalSince1970: Double(usage.resetsAt) / 1000)
            let day = calendar.component(.day, from: reset)
            let month = calendar.shortMonthSymbols[calendar.component(.month, from: reset) - 1]
            note += " · resets \(month) \(day)"
        }
        return [
            "codemagic": StatsTileData(
                value: "\(usage.remainingMinutes) min", note: note, status: usage.tone, trend: .upIsGood,
                reading: Double(usage.remainingMinutes)),
        ]
    }

    public static let desktopIds: Set<String> = ["agents", "stalls", "desktopLoad", "desktopMemory", "codemagic"]
    public static let networkIds: Set<String> = ["repos", "nodes", "hosts"]
    public static let historyIds: Set<String> = ["nodeHours"]
    public static let statusIds: Set<String> = ["uptime", "incidents", "errors"]
    public static var releaseIds: Set<String> { LiveReleases.homepageIds.union([LiveReleases.iosId]) }
    public static var serverIds: Set<String> { networkIds.union(historyIds).union(statusIds).union(releaseIds) }
    public static var allIds: Set<String> { serverIds.union(desktopIds).union(DesktopTiles.ids) }

    public static func withHistory(_ tiles: [String: StatsTileData], _ history: [String: [StatsSample]]) -> [String: StatsTileData] {
        var out = tiles
        for (id, data) in tiles where data.available && data.series.isEmpty && data.reading != nil {
            let samples = history[id] ?? []
            if samples.count >= 2 { out[id]?.series = StatsHistoryStore.series(samples) }
        }
        return out
    }
}

@MainActor
@Observable
public final class StatsBoardModel {
    @ObservationIgnored private let transport: InsightsRequesting
    @ObservationIgnored private let layoutStore: StatsLayoutStore
    @ObservationIgnored private let desktop: (@MainActor () async throws -> DesktopHealth)?
    @ObservationIgnored private let codemagic: (@MainActor () async throws -> CodemagicUsage)?
    @ObservationIgnored private let desktopTiles: (@MainActor () async throws -> DesktopTilesReply)?
    @ObservationIgnored private let history: StatsHistoryStore?
    @ObservationIgnored private let now: () -> Date

    public private(set) var layout: [StatsTilePlacement]
    public private(set) var offered: [String]
    public private(set) var tiles: [String: StatsTileData] = [:]
    public private(set) var loading = false
    public private(set) var pending: Set<String> = StatsTiles.allIds
    public private(set) var failures: [String] = []
    public private(set) var desktopNote = ""
    public private(set) var desktopTilesNote = ""
    public private(set) var desktopOffered: Set<String> = []
    public private(set) var updatedAt: Date?

    public init(transport: InsightsRequesting, layoutStore: StatsLayoutStore,
                desktop: (@MainActor () async throws -> DesktopHealth)? = nil,
                codemagic: (@MainActor () async throws -> CodemagicUsage)? = nil,
                desktopTiles: (@MainActor () async throws -> DesktopTilesReply)? = nil,
                history: StatsHistoryStore? = nil, now: @escaping () -> Date = Date.init) {
        self.transport = transport
        self.layoutStore = layoutStore
        self.desktop = desktop
        self.codemagic = codemagic
        self.desktopTiles = desktopTiles
        self.history = history
        self.now = now
        let merge = layoutStore.load()
        layout = merge.layout
        offered = merge.offered
        desktopOffered = layoutStore.loadDesktopTiles()
    }

    public func isShown(_ id: String) -> Bool { !DesktopTiles.isDesktopTile(id) || desktopOffered.contains(id) }

    public var visible: [StatsTilePlacement] {
        StatsLayout.visible(layout, catalog: StatsCatalog.entries).filter { isShown($0.id) }
    }

    public var addable: [StatsTileCatalogEntry] {
        let placed = Set(layout.map(\.id))
        return StatsCatalog.entries.filter { !placed.contains($0.id) && isShown($0.id) }
    }

    public func data(_ id: String) -> StatsTileData? { tiles[id] }

    public func isLoading(_ id: String) -> Bool { pending.contains(id) }

    public func reload() async {
        loading = true
        defer { loading = false }
        pending = StatsTiles.allIds
        var failed: [String] = []
        async let network: String? = load(StatsTiles.networkIds, failure: "network") {
            StatsTiles.network(NetworkSnapshot(json: try await self.transport.call("GET", "/api/network/stats", nil)))
        }
        async let activity: String? = load(StatsTiles.historyIds, failure: "activity") {
            StatsTiles.history(OnlineHistory(json: try await self.transport.call("GET", "/api/network/online-history", nil)))
        }
        async let status: String? = load(StatsTiles.statusIds, failure: "status") {
            StatsTiles.status(StatusReport(json: try await self.transport.call("GET", "/api/status", nil)))
        }
        async let releases: String? = load(LiveReleases.homepageIds, failure: "releases") {
            LiveReleases.tiles(try await LiveReleases.load(self.transport))
        }
        async let appStoreDone: Void = loadAppStore()
        async let desktopDone: Void = loadDesktop()
        async let codemagicDone: Void = loadCodemagic()
        async let desktopTilesDone: Void = loadDesktopTiles()
        for name in await [network, activity, status, releases] { if let name { failed.append(name) } }
        _ = await (desktopDone, codemagicDone, appStoreDone, desktopTilesDone)
        pending = []
        failures = failed
        updatedAt = now()
    }

    private func load(_ ids: Set<String>, failure: String,
                      _ fetch: @MainActor () async throws -> [String: StatsTileData]) async -> String? {
        defer { pending.subtract(ids) }
        do {
            publish(try await fetch())
            return nil
        } catch {
            return failure
        }
    }

    private func loadDesktop() async {
        defer { pending.subtract(StatsTiles.desktopIds.subtracting(["codemagic"])) }
        guard let desktop else {
            desktopNote = "Pair a desktop to see its load and agents."
            for id in StatsTiles.desktopIds where id != "codemagic" { tiles[id] = .unavailable }
            return
        }
        do {
            publish(StatsTiles.desktop(try await desktop()))
            desktopNote = ""
        } catch {
            desktopNote = DesktopStatsClient.explain(error)
            for id in StatsTiles.desktopIds where tiles[id] == nil && id != "codemagic" { tiles[id] = .unavailable }
        }
    }

    private func loadCodemagic() async {
        defer { pending.remove("codemagic") }
        guard let codemagic else {
            tiles["codemagic"] = .missing("Pair a desktop to read the build minutes.")
            return
        }
        do {
            publish(StatsTiles.codemagic(try await codemagic()))
        } catch {
            let why = DesktopStatsClient.explain(error)
            if tiles["codemagic"]?.available != true { tiles["codemagic"] = .missing(why) }
        }
    }

    private func loadAppStore() async {
        defer { pending.remove(LiveReleases.iosId) }
        do {
            publish([LiveReleases.iosId: LiveReleases.appStore(try await transport.call("GET", LiveReleases.appStoreStatus, nil))])
        } catch {
            if tiles[LiveReleases.iosId]?.available != true {
                tiles[LiveReleases.iosId] = .missing("App Store status unavailable: \(InsightsError.from(error).message)")
            }
        }
    }

    private func loadDesktopTiles() async {
        defer { pending.subtract(DesktopTiles.ids) }
        guard let desktopTiles else {
            offerDesktopTiles([])
            desktopTilesNote = ""
            return
        }
        do {
            let reply = try await desktopTiles()
            offerDesktopTiles(reply.offered.intersection(DesktopTiles.ids))
            publish(reply.tiles.filter { DesktopTiles.ids.contains($0.key) })
            desktopTilesNote = ""
        } catch {
            if DesktopControlFamily.isUnsupported(error) {
                offerDesktopTiles([])
                desktopTilesNote = "Update ForkMesh on your desktop to see all of its stats here."
            } else {
                desktopTilesNote = ""
                let why = DesktopStatsClient.explain(error)
                for id in desktopOffered where tiles[id]?.available != true { tiles[id] = .missing(why) }
            }
        }
    }

    private func offerDesktopTiles(_ ids: Set<String>) {
        guard ids != desktopOffered else { return }
        desktopOffered = ids
        layoutStore.saveDesktopTiles(ids)
    }

    private func publish(_ fresh: [String: StatsTileData]) {
        var fresh = fresh
        if let history {
            let readings = fresh.compactMapValues { $0.available && $0.series.isEmpty ? $0.reading : nil }
            let samples = history.record(readings, at: Int64(now().timeIntervalSince1970 * 1000))
            fresh = StatsTiles.withHistory(fresh, samples)
        }
        tiles.merge(fresh) { $1 }
    }

    public func move(_ id: String, to index: Int) {
        layout = StatsLayout.move(layout, id: id, to: index)
        persist()
    }

    public func move(fromOffsets source: IndexSet, toOffset destination: Int) {
        let ids = visible.map(\.id)
        let picked = source.filter { $0 < ids.count }.sorted()
        if picked.isEmpty { return }
        let items = picked.map { ids[$0] }
        var remaining = ids.enumerated().filter { !picked.contains($0.offset) }.map(\.element)
        let before = picked.filter { $0 < destination }.count
        remaining.insert(contentsOf: items, at: min(max(destination - before, 0), remaining.count))
        var byId: [String: StatsTilePlacement] = [:]
        for placement in layout where byId[placement.id] == nil { byId[placement.id] = placement }
        let shown = Set(ids)
        layout = remaining.compactMap { byId[$0] } + layout.filter { !shown.contains($0.id) }
        persist()
    }

    public func remove(_ id: String) {
        layout.removeAll { $0.id == id }
        persist()
    }

    public func add(_ id: String) {
        guard StatsCatalog.entry(id) != nil, isShown(id), !layout.contains(where: { $0.id == id }) else { return }
        layout.append(StatsTilePlacement(id: id))
        persist()
    }

    public func toggleWide(_ id: String) {
        guard let index = layout.firstIndex(where: { $0.id == id }) else { return }
        layout[index].width = layout[index].width == 1 ? 2 : 1
        persist()
    }

    public func reset() {
        layoutStore.reset()
        let merge = layoutStore.load()
        layout = merge.layout
        offered = merge.offered
    }

    private func persist() {
        layoutStore.save(layout, offered: offered)
    }
}
