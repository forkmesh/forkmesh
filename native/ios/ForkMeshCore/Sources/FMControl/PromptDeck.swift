import Foundation

public struct PromptDeck: Equatable, Sendable {
    public let prompts: [String]

    public init(said: [String]) {
        prompts = said.reversed().compactMap { (text: String) -> String? in
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }
    }

    public init(blocks: [TranscriptBlock]) {
        self.init(said: blocks.compactMap { (block: TranscriptBlock) -> String? in
            switch block {
            case .prompt(let text):
                return text
            case .steer(let from, let text):
                return TranscriptBlock.steerFromYou(from) ? text : nil
            default:
                return nil
            }
        })
    }

    public var isEmpty: Bool { prompts.isEmpty }

    public var count: Int { prompts.count }

    public func clamped(_ index: Int) -> Int {
        max(0, min(index, count - 1))
    }

    public func prompt(at index: Int) -> String {
        isEmpty ? "" : prompts[clamped(index)]
    }

    public func headline(at index: Int) -> String {
        prompt(at: index).split(whereSeparator: \.isWhitespace).joined(separator: " ")
    }

    public func older(than index: Int) -> Int { clamped(index + 1) }

    public func newer(than index: Int) -> Int { clamped(index - 1) }

    public func position(_ index: Int) -> String {
        count < 2 ? "" : "\(count - clamped(index))/\(count)"
    }

    public func behind(_ index: Int) -> Int {
        min(2, max(0, count - 1 - clamped(index)))
    }
}
