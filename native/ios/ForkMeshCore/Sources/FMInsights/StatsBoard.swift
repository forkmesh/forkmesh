import FMBase
import Foundation
import Observation

public enum StatsTileStatus: String, Equatable, Sendable {
    case none, good, warning, critical
}

public enum StatsTrendPolarity: Equatable, Sendable {
    case neutral, upIsGood, downIsGood
}

public struct StatsTileCatalogEntry: Equatable, Sendable, Identifiable {
    public let id: String
    public let title: String
    public let group: String
    public let hint: String
    public let symbol: String

    public init(id: String, title: String, group: String, hint: String, symbol: String) {
        self.id = id
        self.title = title
        self.group = group
        self.hint = hint
        self.symbol = symbol
    }
}

public struct StatsTileSeries: Equatable, Sendable {
    public var labels: [String]
    public var values: [Double]
    public var categorical: Bool

    public init(labels: [String] = [], values: [Double] = [], categorical: Bool = false) {
        self.labels = labels
        self.values = values
        self.categorical = categorical
    }

    public var isEmpty: Bool { values.isEmpty }
}

public struct StatsTileData: Equatable, Sendable {
    public var value: String
    public var note: String
    public var series: StatsTileSeries
    public var available: Bool
    public var status: StatsTileStatus
    public var trend: StatsTrendPolarity
    public var reading: Double?

    public init(value: String = "", note: String = "", series: StatsTileSeries = StatsTileSeries(),
                available: Bool = true, status: StatsTileStatus = .none, trend: StatsTrendPolarity = .neutral,
                reading: Double? = nil) {
        self.value = value
        self.note = note
        self.series = series
        self.available = available
        self.status = status
        self.trend = trend
        self.reading = reading
    }

    public static let unavailable = StatsTileData(available: false)

    public static func missing(_ note: String) -> StatsTileData { StatsTileData(note: note, available: false) }

    public var trendPercent: Double? { StatsFormat.trendPercent(series.values, categorical: series.categorical) }

    public var shownTrend: Double? {
        guard available, let percent = trendPercent, abs(percent) >= 1 else { return nil }
        return percent
    }

    public var trendTone: StatsTileStatus {
        guard let percent = shownTrend else { return .none }
        switch trend {
        case .neutral: return .none
        case .upIsGood: return percent > 0 ? .good : .critical
        case .downIsGood: return percent < 0 ? .good : .critical
        }
    }
}

public struct StatsTilePlacement: Equatable, Sendable, Identifiable {
    public static let maxSpan = 2

    public var id: String
    public var width: Int
    public var height: Int

    public init(id: String, width: Int = 1, height: Int = 1) {
        self.id = id
        self.width = StatsTilePlacement.clamp(width)
        self.height = StatsTilePlacement.clamp(height)
    }

    public static func clamp(_ span: Int) -> Int { min(max(span, 1), maxSpan) }
}

public struct StatsLayoutMerge: Equatable, Sendable {
    public var layout: [StatsTilePlacement]
    public var offered: [String]
    public var changed: Bool
}

public enum StatsLayout {
    public static func toJSON(_ layout: [StatsTilePlacement]) -> JSONValue {
        .array(layout.map { .object(["id": .string($0.id), "w": .int(Int64($0.width)), "h": .int(Int64($0.height))]) })
    }

    public static func fromJSON(_ value: JSONValue?) -> [StatsTilePlacement] {
        (value?.arrayValue ?? []).compactMap { item in
            let object = item.objectValue ?? JSONObject()
            let id = object.str("id")
            if id.isEmpty { return nil }
            return StatsTilePlacement(id: id, width: object.int("w", 1), height: object.int("h", 1))
        }
    }

    public static func merge(saved: [StatsTilePlacement], hasSaved: Bool, defaults: [StatsTilePlacement],
                             catalog: [StatsTileCatalogEntry], offered: [String]) -> StatsLayoutMerge {
        let catalogIds = Set(catalog.map(\.id))
        var alreadyOffered = Set(offered)
        var placed = Set<String>()
        var layout: [StatsTilePlacement] = []
        var changed = false
        for placement in hasSaved ? saved : defaults where !placement.id.isEmpty && !placed.contains(placement.id) {
            placed.insert(placement.id)
            layout.append(StatsTilePlacement(id: placement.id, width: placement.width, height: placement.height))
        }
        if hasSaved {
            for placement in defaults
            where !placement.id.isEmpty && !placed.contains(placement.id) && !alreadyOffered.contains(placement.id)
                && catalogIds.contains(placement.id)
            {
                placed.insert(placement.id)
                layout.append(placement)
                changed = true
            }
        } else {
            changed = true
        }
        var nextOffered = offered
        for entry in catalog where !alreadyOffered.contains(entry.id) {
            nextOffered.append(entry.id)
            alreadyOffered.insert(entry.id)
            changed = true
        }
        return StatsLayoutMerge(layout: layout, offered: nextOffered, changed: changed)
    }

    public static func visible(_ layout: [StatsTilePlacement], catalog: [StatsTileCatalogEntry]) -> [StatsTilePlacement] {
        let ids = Set(catalog.map(\.id))
        return layout.filter { ids.contains($0.id) }
    }

    public static func move(_ layout: [StatsTilePlacement], id: String, to index: Int) -> [StatsTilePlacement] {
        guard let from = layout.firstIndex(where: { $0.id == id }) else { return layout }
        var next = layout
        let item = next.remove(at: from)
        next.insert(item, at: min(max(index, 0), next.count))
        return next
    }
}

public enum StatsCatalog {
    public static let entries: [StatsTileCatalogEntry] = [
        StatsTileCatalogEntry(id: "repos", title: "Repositories", group: "Network",
                              hint: "Public repositories in the network catalog", symbol: "shippingbox"),
        StatsTileCatalogEntry(id: "nodes", title: "Nodes online", group: "Network",
                              hint: "Nodes connected to the relay right now", symbol: "point.3.connected.trianglepath.dotted"),
        StatsTileCatalogEntry(id: "hosts", title: "Hosts online", group: "Network",
                              hint: "Nodes serving repositories right now", symbol: "server.rack"),
        StatsTileCatalogEntry(id: "nodeHours", title: "Node activity", group: "Network",
                              hint: "Node-minutes online per hour, last 24 hours", symbol: "chart.bar"),
        StatsTileCatalogEntry(id: "uptime", title: "Uptime (24 h)", group: "Status",
                              hint: "Share of the website's checks that passed in the last 24 hours", symbol: "checkmark.seal"),
        StatsTileCatalogEntry(id: "errors", title: "Errors (24 h)", group: "Status",
                              hint: "Server errors logged in the last 24 hours", symbol: "exclamationmark.triangle"),
        StatsTileCatalogEntry(id: "incidents", title: "Systems down", group: "Status",
                              hint: "Monitored systems failing their latest check", symbol: "bolt.horizontal"),
        StatsTileCatalogEntry(id: "desktopLoad", title: "Desktop load", group: "Desktop",
                              hint: "CPU of your paired desktop", symbol: "cpu"),
        StatsTileCatalogEntry(id: "desktopMemory", title: "Desktop memory", group: "Desktop",
                              hint: "Memory in use on your paired desktop", symbol: "memorychip"),
        StatsTileCatalogEntry(id: "agents", title: "Agents running", group: "Desktop",
                              hint: "Agents running and queued on your paired desktop", symbol: "sparkles"),
        StatsTileCatalogEntry(id: "stalls", title: "Desktop stalls", group: "Desktop",
                              hint: "UI stalls your desktop recorded", symbol: "hourglass"),
        StatsTileCatalogEntry(id: "codemagic", title: "Codemagic minutes", group: "Builds",
                              hint: "Free Codemagic build minutes left this month, read by your paired desktop",
                              symbol: "hammer"),
    ] + LiveReleases.catalog + DesktopTiles.catalog

    public static let defaults: [StatsTilePlacement] = [
        StatsTilePlacement(id: "nodes"), StatsTilePlacement(id: "repos"),
        StatsTilePlacement(id: "uptime"), StatsTilePlacement(id: "errors"),
        StatsTilePlacement(id: "nodeHours", width: 2), StatsTilePlacement(id: "agents"),
        StatsTilePlacement(id: "desktopLoad"), StatsTilePlacement(id: "hosts"),
        StatsTilePlacement(id: "incidents"), StatsTilePlacement(id: "codemagic"),
        StatsTilePlacement(id: "desktopMemory"), StatsTilePlacement(id: "stalls"),
    ] + LiveReleases.defaults + DesktopTiles.defaults

    public static func entry(_ id: String) -> StatsTileCatalogEntry? { entries.first { $0.id == id } }
}

public final class StatsLayoutStore: @unchecked Sendable {
    public static let layoutKey = "insights/stats/layout.v1"
    public static let offeredKey = "insights/stats/offered.v1"
    public static let desktopTilesKey = "insights/stats/desktop-tiles.v1"

    private let store: KeyValueStore
    private let scope: String

    public init(store: KeyValueStore, scope: String = "") {
        self.store = store
        self.scope = scope
    }

    private func key(_ base: String) -> String { scope.isEmpty ? base : "\(scope)/\(base)" }

    public func load() -> StatsLayoutMerge {
        let raw = store.string(forKey: key(StatsLayoutStore.layoutKey))
        let saved = raw.flatMap { try? JSON.parse($0) }
        let offered = store.stringList(forKey: key(StatsLayoutStore.offeredKey)) ?? []
        let merge = StatsLayout.merge(
            saved: StatsLayout.fromJSON(saved), hasSaved: saved != nil, defaults: StatsCatalog.defaults,
            catalog: StatsCatalog.entries, offered: offered)
        if merge.changed { save(merge.layout, offered: merge.offered) }
        return merge
    }

    public func save(_ layout: [StatsTilePlacement], offered: [String]) {
        store.set(JSON.encode(StatsLayout.toJSON(layout)), forKey: key(StatsLayoutStore.layoutKey))
        store.set(offered, forKey: key(StatsLayoutStore.offeredKey))
    }

    public func reset() {
        store.remove(key(StatsLayoutStore.layoutKey))
    }

    public func loadDesktopTiles() -> Set<String> {
        Set((store.stringList(forKey: key(StatsLayoutStore.desktopTilesKey)) ?? []).filter { DesktopTiles.ids.contains($0) })
    }

    public func saveDesktopTiles(_ ids: Set<String>) {
        store.set(ids.sorted(), forKey: key(StatsLayoutStore.desktopTilesKey))
    }
}

public struct StatsSample: Equatable, Sendable {
    public var at: Int64
    public var value: Double

    public init(at: Int64, value: Double) {
        self.at = at
        self.value = value
    }
}

public final class StatsHistoryStore: @unchecked Sendable {
    public static let historyKey = "insights/stats/history.v1"
    public static let maxSamples = 48
    public static let spacingMs: Int64 = 10 * 60 * 1000

    private let store: KeyValueStore
    private let scope: String

    public init(store: KeyValueStore, scope: String = "") {
        self.store = store
        self.scope = scope
    }

    private var key: String { scope.isEmpty ? StatsHistoryStore.historyKey : "\(scope)/\(StatsHistoryStore.historyKey)" }

    public func load() -> [String: [StatsSample]] {
        guard let raw = store.string(forKey: key), let parsed = try? JSON.parse(raw), let object = parsed.objectValue else {
            return [:]
        }
        var out: [String: [StatsSample]] = [:]
        for id in object.keys {
            let samples = (object[id]?.arrayValue ?? []).compactMap { item -> StatsSample? in
                guard let pair = item.arrayValue, pair.count == 2, let at = pair[0].int64Value,
                      let value = pair[1].doubleValue, value.isFinite else { return nil }
                return StatsSample(at: at, value: value)
            }
            if !samples.isEmpty { out[id] = Array(samples.suffix(StatsHistoryStore.maxSamples)) }
        }
        return out
    }

    public static func append(_ samples: [StatsSample], _ value: Double, at: Int64) -> [StatsSample] {
        var next = samples.filter { $0.at <= at }
        if let last = next.last, at - last.at < spacingMs {
            next[next.count - 1] = StatsSample(at: last.at, value: value)
        } else {
            next.append(StatsSample(at: at, value: value))
        }
        return Array(next.suffix(maxSamples))
    }

    @discardableResult
    public func record(_ readings: [String: Double], at: Int64) -> [String: [StatsSample]] {
        var history = load()
        for (id, value) in readings where value.isFinite {
            history[id] = StatsHistoryStore.append(history[id] ?? [], value, at: at)
        }
        let encoded = JSONObject(history.keys.sorted().map { id in
            (id, JSONValue.array((history[id] ?? []).map { .array([.int($0.at), .double($0.value)]) }))
        })
        store.set(JSON.encode(.object(encoded)), forKey: key)
        return history
    }

    public static func series(_ samples: [StatsSample]) -> StatsTileSeries {
        StatsTileSeries(labels: samples.map { String($0.at) }, values: samples.map(\.value))
    }
}
