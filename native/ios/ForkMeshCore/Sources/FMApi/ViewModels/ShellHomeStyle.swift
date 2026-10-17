import FMBase
import Foundation

public enum ShellHomeSort: String, CaseIterable, Codable, Sendable {
    case custom
    case name
    case nameDescending
    case mostUsed
    case leastUsed
    case recent
    case attention
    case count

    public var title: String {
        switch self {
        case .custom: return "Custom order"
        case .name: return "Name A–Z"
        case .nameDescending: return "Name Z–A"
        case .mostUsed: return "Most used"
        case .leastUsed: return "Least used"
        case .recent: return "Recently used"
        case .attention: return "Needs attention"
        case .count: return "Biggest count"
        }
    }

    public var symbol: String {
        switch self {
        case .custom: return "hand.draw"
        case .name: return "textformat.abc"
        case .nameDescending: return "textformat.abc.dottedunderline"
        case .mostUsed: return "chart.bar.fill"
        case .leastUsed: return "chart.bar"
        case .recent: return "clock.arrow.circlepath"
        case .attention: return "bell.badge"
        case .count: return "number"
        }
    }
}

public enum ShellHomeGroupOrder: String, CaseIterable, Codable, Sendable {
    case usual
    case name
    case mostUsed
    case size

    public var title: String {
        switch self {
        case .usual: return "Usual order"
        case .name: return "Name A–Z"
        case .mostUsed: return "Most used first"
        case .size: return "Most icons first"
        }
    }

    public var symbol: String {
        switch self {
        case .usual: return "list.bullet"
        case .name: return "textformat.abc"
        case .mostUsed: return "chart.bar.fill"
        case .size: return "square.grid.3x2"
        }
    }
}

public enum ShellHomeIconStyle: String, CaseIterable, Codable, Sendable {
    case outline
    case filled
    case accent
    case tiles

    public var title: String {
        switch self {
        case .outline: return "Outline"
        case .filled: return "Filled"
        case .accent: return "Accent"
        case .tiles: return "Tiles"
        }
    }
}

public struct ShellHomeEntry: Equatable, Sendable {
    public let id: String
    public let title: String
    public let badge: Int
    public let count: Int?
    public let active: Bool

    public init(id: String, title: String, badge: Int = 0, count: Int? = nil, active: Bool = false) {
        self.id = id
        self.title = title
        self.badge = badge
        self.count = count
        self.active = active
    }
}

public struct ShellHomeSection: Equatable, Sendable {
    public let id: String
    public let title: String
    public let entries: [ShellHomeEntry]

    public init(id: String, title: String, entries: [ShellHomeEntry]) {
        self.id = id
        self.title = title
        self.entries = entries
    }
}

public struct ShellHomeArranged: Equatable, Sendable {
    public let id: String
    public let title: String
    public let ids: [String]

    public init(id: String, title: String, ids: [String]) {
        self.id = id
        self.title = title
        self.ids = ids
    }
}

public struct ShellHomeStyle: Equatable, Codable, Sendable {
    public static let key = "shell/home-style"
    public static let allId = "all"
    public static let acrossChoices: [Int] = [4, 5, 6]

    public var sort: ShellHomeSort = .custom
    public var grouped: Bool = true
    public var showTitles: Bool = true
    public var groupOrder: ShellHomeGroupOrder = .usual
    public var hidden: [String] = []
    public var icons: ShellHomeIconStyle = .outline
    public var across: Int = 5
    public var showCounts: Bool = true
    public var highlightFrequent: Bool = true
    public var hideBarIcons: Bool = true

    public init() {}

    private enum CodingKeys: String, CodingKey {
        case sort, grouped, showTitles, groupOrder, hidden, icons, across, showCounts, highlightFrequent, hideBarIcons
    }

    public init(from decoder: Decoder) throws {
        let values: KeyedDecodingContainer<CodingKeys> = try decoder.container(keyedBy: CodingKeys.self)
        let base: ShellHomeStyle = ShellHomeStyle()
        sort = (try? values.decodeIfPresent(ShellHomeSort.self, forKey: .sort)) ?? base.sort
        grouped = (try? values.decodeIfPresent(Bool.self, forKey: .grouped)) ?? base.grouped
        showTitles = (try? values.decodeIfPresent(Bool.self, forKey: .showTitles)) ?? base.showTitles
        groupOrder = (try? values.decodeIfPresent(ShellHomeGroupOrder.self, forKey: .groupOrder)) ?? base.groupOrder
        hidden = (try? values.decodeIfPresent([String].self, forKey: .hidden)) ?? base.hidden
        icons = (try? values.decodeIfPresent(ShellHomeIconStyle.self, forKey: .icons)) ?? base.icons
        across = ShellHomeStyle.clampedAcross((try? values.decodeIfPresent(Int.self, forKey: .across)) ?? base.across)
        showCounts = (try? values.decodeIfPresent(Bool.self, forKey: .showCounts)) ?? base.showCounts
        highlightFrequent =
            (try? values.decodeIfPresent(Bool.self, forKey: .highlightFrequent)) ?? base.highlightFrequent
        hideBarIcons = (try? values.decodeIfPresent(Bool.self, forKey: .hideBarIcons)) ?? base.hideBarIcons
    }

    public static func clampedAcross(_ value: Int) -> Int {
        min(acrossChoices.last ?? value, max(acrossChoices.first ?? value, value))
    }

    public static func load(_ store: KeyValueStore) -> ShellHomeStyle {
        guard let text = store.string(forKey: key), let data = text.data(using: .utf8),
            let style = try? JSONDecoder().decode(ShellHomeStyle.self, from: data)
        else { return ShellHomeStyle() }
        return style
    }

    public func save(_ store: KeyValueStore) {
        guard self != ShellHomeStyle(), let data = try? JSONEncoder().encode(self),
            let text = String(data: data, encoding: .utf8)
        else {
            store.remove(ShellHomeStyle.key)
            return
        }
        store.set(text, forKey: ShellHomeStyle.key)
    }

    public var isDefault: Bool { self == ShellHomeStyle() }

    public var reorders: Bool { grouped }

    public var headed: Bool { grouped && showTitles }

    public func shows(_ group: String) -> Bool {
        !hidden.contains(group)
    }

    public mutating func setShown(_ group: String, _ shown: Bool) {
        hidden.removeAll { $0 == group }
        if !shown {
            hidden.append(group)
        }
    }

    public func arranged(
        _ sections: [ShellHomeSection], uses: [String: Int], recent: [String], docked: Set<String> = [],
        searching: Bool = false
    ) -> [ShellHomeArranged] {
        let shown: [ShellHomeSection] = searching ? sections : undocked(sections.filter { shows($0.id) }, docked)
        guard grouped else {
            let entries: [ShellHomeEntry] = ShellHomeStyle.ordered(shown, groupOrder, uses: uses).flatMap(\.entries)
            let ids: [String] = ShellHomeStyle.sorted(entries, sort, uses: uses, recent: recent).map(\.id)
            return ids.isEmpty ? [] : [ShellHomeArranged(id: ShellHomeStyle.allId, title: "", ids: ids)]
        }
        return ShellHomeStyle.ordered(shown, groupOrder, uses: uses).compactMap {
            (section: ShellHomeSection) -> ShellHomeArranged? in
            let ids: [String] = ShellHomeStyle.sorted(section.entries, sort, uses: uses, recent: recent).map(\.id)
            return ids.isEmpty ? nil : ShellHomeArranged(id: section.id, title: section.title, ids: ids)
        }
    }

    private func undocked(_ sections: [ShellHomeSection], _ docked: Set<String>) -> [ShellHomeSection] {
        guard hideBarIcons, !docked.isEmpty else { return sections }
        return sections.map {
            ShellHomeSection(id: $0.id, title: $0.title, entries: $0.entries.filter { !docked.contains($0.id) })
        }
    }

    public static func ordered(
        _ sections: [ShellHomeSection], _ order: ShellHomeGroupOrder, uses: [String: Int]
    ) -> [ShellHomeSection] {
        let indexed: [(offset: Int, element: ShellHomeSection)] = Array(sections.enumerated())
        func total(_ section: ShellHomeSection) -> Int {
            section.entries.reduce(0) { $0 + (uses[$1.id] ?? 0) }
        }
        let ranked: [(offset: Int, element: ShellHomeSection)]
        switch order {
        case .usual:
            return sections
        case .name:
            ranked = indexed.sorted { (lhs, rhs) in
                let order: ComparisonResult = lhs.element.title.localizedStandardCompare(rhs.element.title)
                return order == .orderedSame ? lhs.offset < rhs.offset : order == .orderedAscending
            }
        case .mostUsed:
            ranked = indexed.sorted { (lhs, rhs) in
                let left: Int = total(lhs.element)
                let right: Int = total(rhs.element)
                return left == right ? lhs.offset < rhs.offset : left > right
            }
        case .size:
            ranked = indexed.sorted { (lhs, rhs) in
                let left: Int = lhs.element.entries.count
                let right: Int = rhs.element.entries.count
                return left == right ? lhs.offset < rhs.offset : left > right
            }
        }
        return ranked.map(\.element)
    }

    public static func sorted(
        _ entries: [ShellHomeEntry], _ sort: ShellHomeSort, uses: [String: Int], recent: [String]
    ) -> [ShellHomeEntry] {
        let indexed: [(offset: Int, element: ShellHomeEntry)] = Array(entries.enumerated())
        func by(_ key: (ShellHomeEntry) -> Int, descending: Bool) -> [ShellHomeEntry] {
            indexed.sorted { (lhs, rhs) in
                let left: Int = key(lhs.element)
                let right: Int = key(rhs.element)
                if left == right { return lhs.offset < rhs.offset }
                return descending ? left > right : left < right
            }
            .map(\.element)
        }
        func byName(ascending: Bool) -> [ShellHomeEntry] {
            indexed.sorted { (lhs, rhs) in
                let order: ComparisonResult = lhs.element.title.localizedStandardCompare(rhs.element.title)
                if order == .orderedSame { return lhs.offset < rhs.offset }
                return ascending ? order == .orderedAscending : order == .orderedDescending
            }
            .map(\.element)
        }
        switch sort {
        case .custom:
            return entries
        case .name:
            return byName(ascending: true)
        case .nameDescending:
            return byName(ascending: false)
        case .mostUsed:
            return by({ uses[$0.id] ?? 0 }, descending: true)
        case .leastUsed:
            return by({ uses[$0.id] ?? 0 }, descending: false)
        case .recent:
            var place: [String: Int] = [:]
            for (index, id) in recent.enumerated() where place[id] == nil {
                place[id] = index
            }
            return by({ place[$0.id] ?? Int.max }, descending: false)
        case .attention:
            return by({ attention($0) }, descending: true)
        case .count:
            return by({ $0.count.map { max($0, 0) } ?? (($0.badge > 0) ? $0.badge : -1) }, descending: true)
        }
    }

    public static func attention(_ entry: ShellHomeEntry) -> Int {
        if entry.badge > 0 { return 2_000_000 + min(entry.badge, 999_999) }
        if entry.active { return 1_000_000 }
        return 0
    }

    public static func rows(_ sections: [ShellHomeArranged], across: Int) -> Int {
        let width: Int = max(1, across)
        return sections.reduce(0) { $0 + ($1.ids.count + width - 1) / width }
    }
}
