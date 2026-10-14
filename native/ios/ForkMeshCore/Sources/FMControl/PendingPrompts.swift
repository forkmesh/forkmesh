import Foundation

public struct PendingPrompts: Equatable, Sendable {
    private struct Entry: Equatable, Sendable {
        let id: Int
        let text: String
        let seen: Int
    }

    private var entries: [Entry] = []
    private var nextId = 0

    public init() {}

    public var isEmpty: Bool { entries.isEmpty }

    public var texts: [String] { entries.map(\.text) }

    public var blocks: [TranscriptBlock] { entries.map { .prompt($0.text) } }

    @discardableResult
    public mutating func add(_ text: String, shown: [TranscriptBlock]) -> Int {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        nextId += 1
        if trimmed.isEmpty { return nextId }
        entries.append(Entry(id: nextId, text: trimmed, seen: Self.count(trimmed, in: shown)))
        return nextId
    }

    public mutating func remove(_ id: Int) {
        entries.removeAll { $0.id == id }
    }

    public mutating func settle(_ shown: [TranscriptBlock]) {
        entries.removeAll { Self.count($0.text, in: shown) > $0.seen }
    }

    static func count(_ text: String, in blocks: [TranscriptBlock]) -> Int {
        blocks.reduce(0) { (total: Int, block: TranscriptBlock) -> Int in
            switch block {
            case .prompt(let said), .steer(_, let said):
                return said.contains(text) ? total + 1 : total
            default:
                return total
            }
        }
    }
}
