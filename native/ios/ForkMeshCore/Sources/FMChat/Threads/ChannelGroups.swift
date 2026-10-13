import FMBase
import Foundation
import Observation

public enum ChannelGroups {
    public static let maxGroupNameLength = 48
    public static let maxGroups = 64

    public struct Group: Equatable, Sendable {
        public var name: String
        public var channels: [String] = []
        public var collapsed = false
        public var automatic = false

        public init(name: String, channels: [String] = [], collapsed: Bool = false, automatic: Bool = false) {
            self.name = name
            self.channels = channels
            self.collapsed = collapsed
            self.automatic = automatic
        }
    }

    public struct State: Equatable, Sendable {
        public var groups: [Group] = []
        public var ungrouped: [String] = []

        public init(groups: [Group] = [], ungrouped: [String] = []) {
            self.groups = groups
            self.ungrouped = ungrouped
        }

        public var isEmpty: Bool { groups.isEmpty && ungrouped.isEmpty }
    }

    public struct Row: Equatable, Sendable, Identifiable {
        public var group = ""
        public var channel = ""
        public var header = false
        public var collapsed = false
        public var automatic = false

        public var id: String { header ? "group:\(group)" : "channel:\(channel)" }
    }

    public static func normalizeName(_ name: String) -> String {
        let simplified = name.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
        return String(simplified.prefix(maxGroupNameLength))
    }

    public static func indexOfGroup(_ state: State, _ name: String) -> Int? {
        state.groups.firstIndex { $0.name == name }
    }

    public static func explicitGroupOf(_ state: State, _ channel: String) -> String {
        state.groups.first { $0.channels.contains(channel) }?.name ?? ""
    }

    static func detach(_ state: inout State, _ channel: String) {
        for index in state.groups.indices { state.groups[index].channels.removeAll { $0 == channel } }
        state.ungrouped.removeAll { $0 == channel }
    }

    @discardableResult
    public static func createGroup(_ state: inout State, _ rawName: String) -> Bool {
        let name = normalizeName(rawName)
        if name.isEmpty || indexOfGroup(state, name) != nil || state.groups.count >= maxGroups { return false }
        state.groups.append(Group(name: name))
        return true
    }

    static func materialize(_ state: inout State, _ name: String, _ members: [String], automatic: Bool) {
        var index = indexOfGroup(state, name)
        if index == nil {
            if name.isEmpty || state.groups.count >= maxGroups { return }
            state.groups.append(Group(name: name, automatic: automatic))
            index = state.groups.count - 1
        }
        guard let at = index else { return }
        for channel in members {
            for other in state.groups.indices where other != at { state.groups[other].channels.removeAll { $0 == channel } }
            state.ungrouped.removeAll { $0 == channel }
        }
        state.groups[at].channels = members
    }

    @discardableResult
    public static func renameGroup(_ state: inout State, from: String, to rawTo: String, members: [String]) -> Bool {
        let to = normalizeName(rawTo)
        if to.isEmpty || to == from || indexOfGroup(state, to) != nil { return false }
        materialize(&state, from, members, automatic: false)
        guard let index = indexOfGroup(state, from) else { return false }
        state.groups[index].name = to
        state.groups[index].automatic = false
        return true
    }

    public static func removeGroup(_ state: inout State, _ name: String, members: [String]) {
        if let index = indexOfGroup(state, name) { state.groups.remove(at: index) }
        for channel in members {
            detach(&state, channel)
            state.ungrouped.append(channel)
        }
    }

    public static func setCollapsed(_ state: inout State, _ name: String, collapsed: Bool, automatic: Bool) {
        var index = indexOfGroup(state, name)
        if index == nil {
            if name.isEmpty || state.groups.count >= maxGroups { return }
            state.groups.append(Group(name: name, automatic: automatic))
            index = state.groups.count - 1
        }
        if let at = index { state.groups[at].collapsed = collapsed }
    }

    @discardableResult
    public static func assign(
        _ state: inout State, channel: String, to group: String, index rawIndex: Int, members: [String], automaticGroup: Bool
    ) -> Bool {
        if channel.isEmpty { return false }
        if group.isEmpty {
            detach(&state, channel)
            state.ungrouped.append(channel)
            return true
        }
        var index = rawIndex
        if let old = members.firstIndex(of: channel), index > old { index -= 1 }
        materialize(&state, group, members.filter { $0 != channel }, automatic: automaticGroup)
        guard let groupIndex = indexOfGroup(state, group) else { return false }
        detach(&state, channel)
        let count = state.groups[groupIndex].channels.count
        if index < 0 || index > count { index = count }
        state.groups[groupIndex].channels.insert(channel, at: index)
        return true
    }

    @discardableResult
    public static func purge(
        _ state: inout State, channelMatches: (String) -> Bool, groupMatches: (String) -> Bool
    ) -> Bool {
        var changed = false
        for index in state.groups.indices.reversed() {
            let before = state.groups[index].channels.count
            state.groups[index].channels.removeAll(where: channelMatches)
            if state.groups[index].channels.count != before { changed = true }
            if state.groups[index].automatic && state.groups[index].channels.isEmpty && groupMatches(state.groups[index].name) {
                state.groups.remove(at: index)
                changed = true
            }
        }
        let before = state.ungrouped.count
        state.ungrouped.removeAll(where: channelMatches)
        if state.ungrouped.count != before { changed = true }
        return changed
    }

    public static func toJSON(_ state: State) -> String {
        if state.isEmpty { return "" }
        var groups: [JSONValue] = []
        for group in state.groups {
            var entry: JSONObject = ["name": .string(group.name)]
            if !group.channels.isEmpty { entry["channels"] = JSONValue(group.channels) }
            if group.collapsed { entry["collapsed"] = true }
            if group.automatic { entry["automatic"] = true }
            groups.append(.object(entry))
        }
        var root: JSONObject = ["groups": .array(groups)]
        if !state.ungrouped.isEmpty { root["ungrouped"] = JSONValue(state.ungrouped) }
        return JSON.encode(root)
    }

    public static func fromJSON(_ json: String) -> State {
        var state = State()
        let root = JSON.parseObject(json)
        var seen = Set<String>()
        for entry in root["groups"]?.objects ?? [] {
            if state.groups.count >= maxGroups { break }
            let name = normalizeName(entry["name"]?.text ?? "")
            if name.isEmpty || indexOfGroup(state, name) != nil { continue }
            var group = Group(
                name: name, collapsed: entry["collapsed"]?.isTrue == true, automatic: entry["automatic"]?.isTrue == true)
            for channel in entry["channels"]?.strings ?? [] where !channel.isEmpty && !seen.contains(channel) {
                seen.insert(channel)
                group.channels.append(channel)
            }
            state.groups.append(group)
        }
        for channel in root["ungrouped"]?.strings ?? [] where !channel.isEmpty && !seen.contains(channel) {
            seen.insert(channel)
            state.ungrouped.append(channel)
        }
        return state
    }

    public static func layout(_ state: State, channels: [String], autoGroupFor: ((String) -> String)? = nil) -> [Row] {
        let present = Set(channels)
        struct Bucket {
            var name: String
            var members: [String] = []
            var collapsed = false
            var automatic = false
        }
        var buckets: [Bucket] = state.groups.map { group in
            Bucket(name: group.name, members: group.channels.filter { present.contains($0) }, collapsed: group.collapsed,
                   automatic: group.automatic)
        }
        var ungrouped: [String] = []
        for channel in channels {
            if !explicitGroupOf(state, channel).isEmpty { continue }
            let auto = (autoGroupFor != nil && !state.ungrouped.contains(channel)) ? normalizeName(autoGroupFor!(channel)) : ""
            if auto.isEmpty {
                ungrouped.append(channel)
                continue
            }
            if let index = buckets.firstIndex(where: { $0.name == auto }) {
                buckets[index].members.append(channel)
            } else {
                buckets.append(Bucket(name: auto, members: [channel], automatic: true))
            }
        }
        var rows: [Row] = ungrouped.map { Row(channel: $0) }
        for bucket in buckets {
            if bucket.automatic && bucket.members.isEmpty { continue }
            rows.append(Row(group: bucket.name, header: true, collapsed: bucket.collapsed, automatic: bucket.automatic))
            for channel in bucket.members {
                rows.append(Row(group: bucket.name, channel: channel, automatic: bucket.automatic))
            }
        }
        return rows
    }

    public static func renderedMembers(_ rows: [Row], _ group: String) -> [String] {
        if group.isEmpty { return [] }
        return rows.filter { !$0.header && $0.group == group }.map(\.channel)
    }

    public static func renderedGroupNames(_ rows: [Row]) -> [String] { rows.filter(\.header).map(\.group) }

    public static func renderedGroupIsAutomatic(_ rows: [Row], _ group: String) -> Bool {
        rows.first { $0.header && $0.group == group }?.automatic ?? false
    }

    public static func visibleRows(_ rows: [Row]) -> [Row] {
        var collapsed = Set<String>()
        for row in rows where row.header && row.collapsed { collapsed.insert(row.group) }
        return rows.filter { $0.header || $0.group.isEmpty || !collapsed.contains($0.group) }
    }
}

@MainActor
@Observable
public final class ChannelGroupsStore {
    public static let key = "chat/channelGroups"
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let scopedKey: String
    public private(set) var state: ChannelGroups.State

    public static func keyFor(_ scope: String) -> String { scope.isEmpty ? key : "\(key)/\(scope)" }

    public static func projectScope(projectId: String, room: String) -> String {
        "project-" + ChatHistoryCodec.scopeName(serverUrl: projectId, room: room)
    }

    public init(prefs: KeyValueStore, scope: String = "", formerScopes: [String] = []) {
        let key = ChannelGroupsStore.keyFor(scope)
        self.prefs = prefs
        self.scopedKey = key
        if prefs.string(forKey: key) == nil,
            let former = formerScopes.map(ChannelGroupsStore.keyFor).first(where: { $0 != key && prefs.string(forKey: $0) != nil }),
            let saved = prefs.string(forKey: former)
        {
            prefs.set(saved, forKey: key)
            prefs.remove(former)
        }
        state = ChannelGroups.fromJSON(prefs.string(forKey: key) ?? "")
    }

    public convenience init(prefs: KeyValueStore, projectId: String, serverUrl: String, room: String) {
        self.init(
            prefs: prefs, scope: ChannelGroupsStore.projectScope(projectId: projectId.isEmpty ? serverUrl : projectId, room: room),
            formerScopes: [ChatHistoryCodec.scopeName(serverUrl: serverUrl, room: room)])
    }

    public func update(_ change: (inout ChannelGroups.State) -> Void) {
        var next = state
        change(&next)
        if next == state { return }
        state = next
        let json = ChannelGroups.toJSON(next)
        if json.isEmpty {
            prefs.remove(scopedKey)
        } else {
            prefs.set(json, forKey: scopedKey)
        }
    }
}
