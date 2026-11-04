import FMApi
import FMBase
import Foundation

public struct NoteReaction: Equatable, Identifiable, Sendable {
    public let id: String
    public let author: String
    public let emoji: String
    public let quote: String
    public let prefix: String
    public let suffix: String
    public let start: Int
    public let end: Int
    public let createdAt: Int64

    public init(
        id: String, author: String = "", emoji: String, quote: String = "", prefix: String = "", suffix: String = "",
        start: Int = 0, end: Int = 0, createdAt: Int64 = 0
    ) {
        self.id = id
        self.author = author
        self.emoji = emoji
        self.quote = quote
        self.prefix = prefix
        self.suffix = suffix
        self.start = start
        self.end = end
        self.createdAt = createdAt
    }

    public init(json: JSONObject) {
        self.init(
            id: ToolsJSON.text(json["id"], "").dartTrimmed, author: ToolsJSON.text(json["author"], "").dartTrimmed,
            emoji: ToolsJSON.text(json["emoji"], "").dartTrimmed, quote: ToolsJSON.text(json["quote"], ""),
            prefix: ToolsJSON.text(json["prefix"], ""), suffix: ToolsJSON.text(json["suffix"], ""),
            start: max(0, ToolsJSON.int(json["start"])), end: max(0, ToolsJSON.int(json["end"])),
            createdAt: ToolsJSON.epoch(json["createdAt"]))
    }

    public var whole: Bool { quote.isEmpty }

    public var anchor: NoteQuote? {
        whole ? nil : NoteQuote(quote: quote, prefix: prefix, suffix: suffix, start: start, end: end)
    }
}

public struct NoteReactionChoice: Equatable, Identifiable, Sendable {
    public let emoji: String
    public let label: String

    public init(_ emoji: String, _ label: String) {
        self.emoji = emoji
        self.label = label
    }

    public var id: String { emoji }
}

public struct NoteReactionChip: Equatable, Identifiable, Sendable {
    public let emoji: String
    public let people: [String]
    public let mine: [String]

    public init(emoji: String, people: [String], mine: [String]) {
        self.emoji = emoji
        self.people = people
        self.mine = mine
    }

    public var id: String { emoji }
    public var count: Int { people.count }
    public var reacted: Bool { !mine.isEmpty }
    public var label: String { "\(people.joined(separator: ", ")) reacted with \(emoji)" }
}

public struct NoteReactionGroup: Equatable, Identifiable, Sendable {
    public let range: Range<Int>?
    public let anchor: NoteQuote?
    public let chips: [NoteReactionChip]

    public init(range: Range<Int>?, anchor: NoteQuote?, chips: [NoteReactionChip]) {
        self.range = range
        self.anchor = anchor
        self.chips = chips
    }

    public var id: String { range.map { "\($0.lowerBound)-\($0.upperBound)" } ?? "whole" }
    public var whole: Bool { range == nil }
    public var quote: String { anchor?.quote ?? "" }
    public var summary: String { chips.map { "\($0.emoji) \($0.count)" }.joined(separator: "  ") }
    public var label: String { chips.map(\.label).joined(separator: ". ") }

    public func chip(_ emoji: String) -> NoteReactionChip? { chips.first { $0.emoji == emoji } }
}

public enum NoteReactionAction: Equatable, Sendable {
    case add(emoji: String, anchor: NoteQuote?)
    case remove([String])
}

public enum NoteReactions {
    public static let title = "Reactions"
    public static let wholeNote = "On the whole note"
    public static let react = "React"
    public static let addLabel = "Add a reaction"
    public static let quickSet: [NoteReactionChoice] = [
        NoteReactionChoice("👍", "Thumbs up"), NoteReactionChoice("❤️", "Heart"), NoteReactionChoice("😂", "Laughing"),
        NoteReactionChoice("🎉", "Celebrate"), NoteReactionChoice("😮", "Surprised"), NoteReactionChoice("👀", "Looking"),
    ]
    static let probeLength = 24

    public static func list(_ value: JSONValue?) -> [NoteReaction] {
        ToolsJSON.maps(value).map(NoteReaction.init(json:)).filter { !$0.id.isEmpty && !$0.emoji.isEmpty }
    }

    public static func body(emoji: String, anchor: NoteQuote?) -> JSONObject {
        var body: JSONObject = ["emoji": .string(emoji)]
        if let anchor {
            for (key, value) in anchor.body { body[key] = value }
        }
        return body
    }

    public static func groups(_ reactions: [NoteReaction], markdown: String, me: String) -> [NoteReactionGroup] {
        let text = Array(markdown.utf16)
        let self_ = me.dartTrimmed.lowercased()
        var whole: [NoteReaction] = []
        var buckets: [Range<Int>: [NoteReaction]] = [:]
        for reaction in reactions where !reaction.id.isEmpty && !reaction.emoji.isEmpty {
            if reaction.whole {
                whole.append(reaction)
            } else if let range = NoteAnchor.locate(
                Array(reaction.quote.utf16), Array(reaction.prefix.utf16), Array(reaction.suffix.utf16), start: reaction.start, in: text) {
                buckets[range, default: []].append(reaction)
            }
        }
        let ordered = buckets.keys.sorted { $0.lowerBound != $1.lowerBound ? $0.lowerBound < $1.lowerBound : $0.upperBound < $1.upperBound }
        var out: [NoteReactionGroup] = []
        if !whole.isEmpty { out.append(group(nil, whole, me: self_)) }
        for range in ordered { out.append(group(range, buckets[range] ?? [], me: self_)) }
        return out
    }

    static func group(_ range: Range<Int>?, _ reactions: [NoteReaction], me: String) -> NoteReactionGroup {
        var emojis: [String] = []
        var people: [String: [String]] = [:]
        var mine: [String: [String]] = [:]
        for reaction in reactions {
            if people[reaction.emoji] == nil {
                emojis.append(reaction.emoji)
                people[reaction.emoji] = []
                mine[reaction.emoji] = []
            }
            let name = reaction.author.isEmpty ? "Someone" : reaction.author
            if !(people[reaction.emoji] ?? []).contains(where: { $0.lowercased() == name.lowercased() }) {
                people[reaction.emoji, default: []].append(name)
            }
            if !me.isEmpty && reaction.author.lowercased() == me {
                mine[reaction.emoji, default: []].append(reaction.id)
            }
        }
        let chips = emojis.map { NoteReactionChip(emoji: $0, people: people[$0] ?? [], mine: mine[$0] ?? []) }
        return NoteReactionGroup(range: range, anchor: reactions.first?.anchor, chips: chips)
    }

    public static func action(_ chip: NoteReactionChip, in group: NoteReactionGroup) -> NoteReactionAction {
        chip.reacted ? .remove(chip.mine) : .add(emoji: chip.emoji, anchor: group.anchor)
    }

    public static func action(adding emoji: String, to group: NoteReactionGroup) -> NoteReactionAction? {
        group.chip(emoji)?.reacted == true ? nil : .add(emoji: emoji, anchor: group.anchor)
    }

    public static func probe(_ block: DocsBlock) -> String? {
        let text: String
        switch block {
        case let .heading(_, value, _): text = value
        case let .paragraph(value): text = value
        case let .listItem(_, _, _, value): text = value
        case let .quote(value): text = value
        case let .code(_, value): text = value
        case let .table(header, _): text = header.first ?? ""
        case .rule: return nil
        }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    public static func blockStarts(_ probes: [String?], in markdown: String) -> [Int?] {
        let text = Array(markdown.utf16)
        var cursor = 0
        return probes.map { (probe: String?) -> Int? in
            guard let probe else { return nil }
            let units = Array(probe.utf16.prefix(probeLength))
            let word = Array(units.prefix { !isSpace($0) })
            for candidate in [units, word] where !candidate.isEmpty {
                if let found = find(candidate, in: text, from: cursor) {
                    cursor = found.upperBound
                    return found.lowerBound
                }
            }
            return nil
        }
    }

    public static func placed(_ groups: [NoteReactionGroup], starts: [Int?]) -> [[NoteReactionGroup]] {
        var out = Array(repeating: [NoteReactionGroup](), count: starts.count)
        let known = starts.enumerated().compactMap { (pair: (offset: Int, element: Int?)) -> (index: Int, start: Int)? in
            pair.element.map { (pair.offset, $0) }
        }
        guard let first = known.first else { return out }
        for group in groups {
            guard let range = group.range else { continue }
            let index = known.last { $0.start <= range.lowerBound }?.index ?? first.index
            out[index].append(group)
        }
        return out
    }

    static func isSpace(_ unit: UInt16) -> Bool { unit == 32 || unit == 9 || unit == 10 || unit == 13 }

    static func find(_ probe: [UInt16], in text: [UInt16], from: Int) -> Range<Int>? {
        var at = max(0, from)
        while at < text.count {
            if text[at] == probe[0], let end = match(probe, text, at: at) { return at..<end }
            at += 1
        }
        return nil
    }

    static func match(_ probe: [UInt16], _ text: [UInt16], at: Int) -> Int? {
        var i = 0
        var j = at
        while i < probe.count {
            if isSpace(probe[i]) {
                guard j < text.count, isSpace(text[j]) else { return nil }
                while i < probe.count, isSpace(probe[i]) { i += 1 }
                while j < text.count, isSpace(text[j]) { j += 1 }
                continue
            }
            guard j < text.count, text[j] == probe[i] else { return nil }
            i += 1
            j += 1
        }
        return j
    }
}
