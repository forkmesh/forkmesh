import FMBase
import Foundation

public struct ShellBarFolder: Equatable, Codable, Sendable, Identifiable {
    public let id: String
    public var name: String
    public var items: [String]

    public init(id: String, name: String, items: [String] = []) {
        self.id = id
        self.name = name
        self.items = items
    }

    public var slotId: String { ShellBarLayout.folderPrefix + id }
}

public enum ShellBarSlot<Item> {
    case item(Item)
    case folder(ShellBarFolder, [Item])
}

public enum ShellBarStep: Sendable {
    case left
    case right
    case first
    case last
}

public struct ShellBarLayout: Equatable, Codable, Sendable {
    public static let key = "shell/bar-layout"
    public static let folderPrefix = "folder:"
    public static let nameLimit = 40
    public static let barLimit = 6

    public var hidden: [String]
    public var folders: [ShellBarFolder]
    public var order: [String]
    public var bar: [String]?

    public init(hidden: [String] = [], folders: [ShellBarFolder] = [], order: [String] = [], bar: [String]? = nil) {
        self.hidden = hidden
        self.folders = folders
        self.order = order
        self.bar = bar
    }

    public var isEmpty: Bool { hidden.isEmpty && folders.isEmpty && order.isEmpty && bar == nil }

    public static func load(_ store: KeyValueStore) -> ShellBarLayout {
        guard let text = store.string(forKey: key), let data = text.data(using: .utf8),
            let layout = try? JSONDecoder().decode(ShellBarLayout.self, from: data)
        else { return ShellBarLayout() }
        return layout
    }

    public func save(_ store: KeyValueStore) {
        guard !isEmpty, let data = try? JSONEncoder().encode(self), let text = String(data: data, encoding: .utf8) else {
            store.remove(ShellBarLayout.key)
            return
        }
        store.set(text, forKey: ShellBarLayout.key)
    }

    public func isHidden(_ id: String) -> Bool {
        if hidden.contains(id) { return true }
        guard let bar = bar else { return false }
        return !bar.contains(id)
    }

    public func folderOf(_ id: String) -> ShellBarFolder? {
        folders.first { $0.items.contains(id) }
    }

    public func folder(_ id: String) -> ShellBarFolder? {
        folders.first { $0.id == id }
    }

    public mutating func hide(_ id: String) {
        if !hidden.contains(id) {
            hidden.append(id)
        }
        bar?.removeAll { $0 == id }
    }

    public mutating func show(_ id: String) {
        hidden.removeAll { $0 == id }
        if let current = bar, !current.contains(id) {
            bar = current + [id]
        }
    }

    public mutating func showAll() {
        hidden = []
        bar = nil
    }

    public mutating func move(_ id: String, toFolder folderId: String?) {
        for index in folders.indices {
            folders[index].items.removeAll { $0 == id }
        }
        guard let folderId = folderId, let index = folders.firstIndex(where: { $0.id == folderId }) else { return }
        folders[index].items.append(id)
        show(id)
    }

    public static func cleanName(_ name: String) -> String {
        let trimmed: String = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "Folder" : String(trimmed.prefix(nameLimit))
    }

    @discardableResult
    public mutating func createFolder(_ name: String, with id: String? = nil) -> String {
        var number: Int = folders.count + 1
        while folders.contains(where: { $0.id == "f\(number)" }) {
            number += 1
        }
        let folderId: String = "f\(number)"
        folders.append(ShellBarFolder(id: folderId, name: ShellBarLayout.cleanName(name)))
        if let id = id {
            move(id, toFolder: folderId)
        }
        return folderId
    }

    public mutating func renameFolder(_ folderId: String, to name: String) {
        guard let index = folders.firstIndex(where: { $0.id == folderId }) else { return }
        folders[index].name = ShellBarLayout.cleanName(name)
    }

    public mutating func removeFolder(_ folderId: String) {
        folders.removeAll { $0.id == folderId }
    }

    public static func arranged<Item>(_ items: [Item], order: [String], id: (Item) -> String) -> [Item] {
        ShellBarOrder.ordered(items, recent: order, id: id)
    }

    public func slots<Item>(_ items: [Item], ranking: [String], id: (Item) -> String) -> [ShellBarSlot<Item>] {
        let placed: [Item] = ShellBarOrder.ordered(
            ShellBarLayout.arranged(items, order: order, id: id), recent: ranking, id: id)
        let visible: [Item] = placed.filter { !isHidden(id($0)) }
        var byId: [String: Item] = [:]
        for item in visible where byId[id(item)] == nil {
            byId[id(item)] = item
        }
        var emitted: Set<String> = []
        var result: [ShellBarSlot<Item>] = []
        for item in visible {
            guard let folder = folderOf(id(item)) else {
                result.append(.item(item))
                continue
            }
            guard !emitted.contains(folder.id) else { continue }
            emitted.insert(folder.id)
            result.append(.folder(folder, folder.items.compactMap { byId[$0] }))
        }
        return result
    }

    public static func slotId<Item>(_ slot: ShellBarSlot<Item>, id: (Item) -> String) -> String {
        switch slot {
        case let .item(item): return id(item)
        case let .folder(folder, _): return folder.slotId
        }
    }

    public static func members<Item>(_ folderId: String, in slots: [ShellBarSlot<Item>], id: (Item) -> String) -> [String] {
        for slot in slots {
            if case let .folder(folder, members) = slot, folder.id == folderId {
                return members.map(id)
            }
        }
        return []
    }

    public static func target(_ index: Int, count: Int, step: ShellBarStep) -> Int {
        switch step {
        case .left: return max(0, index - 1)
        case .right: return min(count - 1, index + 1)
        case .first: return 0
        case .last: return count - 1
        }
    }

    public mutating func step<Item>(
        _ slotId: String, _ step: ShellBarStep, items: [Item], ranking: [String], id: (Item) -> String
    ) {
        let all: [ShellBarSlot<Item>] = slots(items, ranking: ranking, id: id)
        if let folderIndex = folders.firstIndex(where: { $0.items.contains(slotId) }) {
            var members: [String] = ShellBarLayout.members(folders[folderIndex].id, in: all, id: id)
            guard let from = members.firstIndex(of: slotId) else { return }
            members.remove(at: from)
            members.insert(slotId, at: ShellBarLayout.target(from, count: members.count + 1, step: step))
            folders[folderIndex].items = members + folders[folderIndex].items.filter { !members.contains($0) }
            return
        }
        var keys: [String] = all.map { ShellBarLayout.slotId($0, id: id) }
        guard let from = keys.firstIndex(of: slotId) else { return }
        keys.remove(at: from)
        keys.insert(slotId, at: ShellBarLayout.target(from, count: keys.count + 1, step: step))
        write(keys, items: items, ranking: ranking, id: id)
    }

    private mutating func write<Item>(_ keys: [String], items: [Item], ranking: [String], id: (Item) -> String) {
        var expanded: [String] = []
        for key in keys {
            if key.hasPrefix(ShellBarLayout.folderPrefix),
                let folder = folder(String(key.dropFirst(ShellBarLayout.folderPrefix.count)))
            {
                expanded.append(contentsOf: folder.items)
            } else {
                expanded.append(key)
            }
        }
        let rest: [String] = ShellBarOrder.ordered(
            ShellBarLayout.arranged(items, order: order, id: id), recent: ranking, id: id
        ).map(id).filter { !expanded.contains($0) }
        order = expanded + rest
    }

    public func canStep<Item>(
        _ slotId: String, _ step: ShellBarStep, items: [Item], ranking: [String], id: (Item) -> String
    ) -> Bool {
        let all: [ShellBarSlot<Item>] = slots(items, ranking: ranking, id: id)
        let keys: [String]
        if let folder = folderOf(slotId) {
            keys = ShellBarLayout.members(folder.id, in: all, id: id)
        } else {
            keys = all.map { ShellBarLayout.slotId($0, id: id) }
        }
        guard let index = keys.firstIndex(of: slotId) else { return false }
        switch step {
        case .left, .first: return index > 0
        case .right, .last: return index < keys.count - 1
        }
    }

    public static func holds<Item>(_ slot: ShellBarSlot<Item>, _ itemId: String, id: (Item) -> String) -> Bool {
        switch slot {
        case let .item(item): return id(item) == itemId
        case let .folder(_, members): return members.contains { id($0) == itemId }
        }
    }

    public func barSlots<Item>(
        _ items: [Item], ranking: [String], keep: [String] = [], id: (Item) -> String
    ) -> [ShellBarSlot<Item>] {
        var shown: ShellBarLayout = self
        for kept in keep {
            shown.show(kept)
        }
        let all: [ShellBarSlot<Item>] = shown.slots(items, ranking: ranking, id: id)
        guard all.count > ShellBarLayout.barLimit else { return all }
        let pinned: [Bool] = all.map { (slot: ShellBarSlot<Item>) -> Bool in
            keep.contains { ShellBarLayout.holds(slot, $0, id: id) }
        }
        var room: Int = ShellBarLayout.barLimit - pinned.filter { $0 }.count
        var result: [ShellBarSlot<Item>] = []
        for (index, slot) in all.enumerated() {
            if pinned[index] {
                result.append(slot)
            } else if room > 0 {
                result.append(slot)
                room -= 1
            }
        }
        return result
    }

    public func inBar<Item>(
        _ itemId: String, items: [Item], ranking: [String], keep: [String] = [], id: (Item) -> String
    ) -> Bool {
        barSlots(items, ranking: ranking, keep: keep, id: id).contains { ShellBarLayout.holds($0, itemId, id: id) }
    }

    public func barIsFull<Item>(_ items: [Item], ranking: [String], keep: [String] = [], id: (Item) -> String) -> Bool {
        barSlots(items, ranking: ranking, keep: keep, id: id).count >= ShellBarLayout.barLimit
    }

    public mutating func fit<Item>(_ items: [Item], ranking: [String], keep: [String] = [], id: (Item) -> String) {
        var members: [String] = []
        for slot in barSlots(items, ranking: ranking, keep: keep, id: id) {
            switch slot {
            case let .item(item): members.append(id(item))
            case let .folder(_, inside): members.append(contentsOf: inside.map(id))
            }
        }
        let present: Set<String> = Set(items.map(id))
        bar = members + (bar ?? []).filter { !present.contains($0) && !members.contains($0) }
        for kept in keep {
            hidden.removeAll { $0 == kept }
        }
    }

    public mutating func removeFromBar<Item>(
        _ ids: [String], items: [Item], ranking: [String], keep: [String] = [], id: (Item) -> String
    ) {
        fit(items, ranking: ranking, keep: keep, id: id)
        for itemId in ids where !keep.contains(itemId) {
            bar?.removeAll { $0 == itemId }
        }
    }

    @discardableResult
    public mutating func putInBar<Item>(
        _ itemId: String, at target: String? = nil, items: [Item], ranking: [String], keep: [String] = [],
        id: (Item) -> String
    ) -> Bool {
        var next: ShellBarLayout = self
        next.fit(items, ranking: ranking, keep: keep, id: id)
        let before: [ShellBarSlot<Item>] = next.slots(items, ranking: ranking, id: id)
        let targetIndex: Int? = target.flatMap { (key: String) -> Int? in
            before.firstIndex { ShellBarLayout.slotId($0, id: id) == key }
        }
        if !before.contains(where: { ShellBarLayout.holds($0, itemId, id: id) }) {
            next.show(itemId)
            if next.slots(items, ranking: ranking, id: id).count > ShellBarLayout.barLimit {
                guard let targetIndex = targetIndex else { return false }
                let replaced: ShellBarSlot<Item> = before[targetIndex]
                guard !keep.contains(where: { ShellBarLayout.holds(replaced, $0, id: id) }) else { return false }
                switch replaced {
                case let .item(item): next.bar?.removeAll { $0 == id(item) }
                case let .folder(_, inside):
                    let gone: [String] = inside.map(id)
                    next.bar?.removeAll { gone.contains($0) }
                }
            }
        }
        if let targetIndex = targetIndex {
            let slotKey: String = next.folderOf(itemId)?.slotId ?? itemId
            var keys: [String] = next.slots(items, ranking: ranking, id: id).map { ShellBarLayout.slotId($0, id: id) }
            if let from = keys.firstIndex(of: slotKey) {
                keys.remove(at: from)
                keys.insert(slotKey, at: min(targetIndex, keys.count))
                next.write(keys, items: items, ranking: ranking, id: id)
            }
        }
        self = next
        return true
    }
}
