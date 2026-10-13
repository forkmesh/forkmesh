import FMBase
import Foundation

public struct ShellMenuLayout: Equatable, Codable, Sendable {
    public static let key = "shell/menu-layout"

    public var groups: [String: [String]]

    public init(groups: [String: [String]] = [:]) {
        self.groups = groups
    }

    public var isEmpty: Bool { groups.isEmpty }

    public static func load(_ store: KeyValueStore) -> ShellMenuLayout {
        guard let text = store.string(forKey: key), let data = text.data(using: .utf8),
            let layout = try? JSONDecoder().decode(ShellMenuLayout.self, from: data)
        else { return ShellMenuLayout() }
        return layout
    }

    public func save(_ store: KeyValueStore) {
        guard !isEmpty, let data = try? JSONEncoder().encode(self), let text = String(data: data, encoding: .utf8) else {
            store.remove(ShellMenuLayout.key)
            return
        }
        store.set(text, forKey: ShellMenuLayout.key)
    }

    public func arranged<Group, Item>(
        _ defaults: [Group], group: (Group) -> String, items: (Group) -> [Item], id: (Item) -> String
    ) -> [[Item]] {
        let present: Set<String> = Set(defaults.map(group))
        var home: [String: String] = [:]
        for (key, ids) in groups where present.contains(key) {
            for itemId in ids where home[itemId] == nil {
                home[itemId] = key
            }
        }
        var byId: [String: Item] = [:]
        var origin: [String: String] = [:]
        var loose: [String: [Item]] = [:]
        for entry in defaults {
            let key: String = group(entry)
            for item in items(entry) where byId[id(item)] == nil {
                byId[id(item)] = item
                origin[id(item)] = key
                if home[id(item)] == nil {
                    loose[key, default: []].append(item)
                }
            }
        }
        return defaults.map { (entry: Group) -> [Item] in
            let key: String = group(entry)
            var emitted: Set<String> = []
            var placed: [Item] = []
            for itemId in groups[key] ?? [] where home[itemId] == key && !emitted.contains(itemId) {
                guard let item = byId[itemId] else { continue }
                emitted.insert(itemId)
                placed.append(item)
            }
            return placed + (loose[key] ?? [])
        }
    }

    public mutating func move(_ itemId: String, to group: String, near target: String?, current: [(String, [String])]) {
        var lists: [(String, [String])] = current
        var from: (group: Int, index: Int)?
        for (groupIndex, entry) in lists.enumerated() {
            if let index = entry.1.firstIndex(of: itemId) {
                from = (groupIndex, index)
                lists[groupIndex].1.remove(at: index)
            }
        }
        guard let into = lists.firstIndex(where: { $0.0 == group }) else { return }
        var insert: Int = lists[into].1.count
        if let target = target, target != itemId, let at = lists[into].1.firstIndex(of: target) {
            let forward: Bool = from.map { $0.group == into && $0.index <= at } ?? false
            insert = forward ? at + 1 : at
        }
        lists[into].1.insert(itemId, at: insert)
        let shown: Set<String> = Set(lists.flatMap(\.1))
        var next: [String: [String]] = [:]
        for (key, ids) in lists {
            next[key] = ids
        }
        for (key, ids) in groups {
            var list: [String] = next[key] ?? []
            for (index, kept) in ids.enumerated() where !shown.contains(kept) && !list.contains(kept) {
                let after: Int? = ids[..<index].reversed().lazy.compactMap { list.firstIndex(of: $0) }.first
                list.insert(kept, at: after.map { $0 + 1 } ?? 0)
            }
            if !list.isEmpty {
                next[key] = list
            }
        }
        groups = next.filter { !$0.value.isEmpty }
    }
}
