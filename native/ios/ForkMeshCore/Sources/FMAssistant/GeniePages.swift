import FMLocalAI
import Foundation

public enum GeniePageTurn: String, CaseIterable, Sendable {
    case next
    case previous
    case nextFile = "next_file"
    case previousFile = "previous_file"

    public var title: String {
        switch self {
        case .next: return "Next page"
        case .previous: return "Previous page"
        case .nextFile: return "Next file"
        case .previousFile: return "Previous file"
        }
    }

    public var symbol: String {
        switch self {
        case .next: return "arrow.down.circle"
        case .previous: return "arrow.up.circle"
        case .nextFile: return "arrow.down.to.line"
        case .previousFile: return "arrow.up.to.line"
        }
    }

    public static func parse(_ raw: String) -> GeniePageTurn? {
        let words = Set(GenieWords.words(raw.replacingOccurrences(of: "_", with: " ")))
        let back = !words.isDisjoint(with: ["previous", "prev", "back", "backward", "backwards", "up", "prior", "before", "earlier"])
        let file = !words.isDisjoint(with: ["file", "files"])
        if file { return back ? .previousFile : .nextFile }
        if back { return .previous }
        return words.isDisjoint(with: ["next", "forward", "forwards", "down", "on", "page", "more", "ahead"]) ? nil : .next
    }
}

public enum GeniePages {
    public nonisolated static let tool = "turn_page"
    public nonisolated static let page = "Diff"
    public nonisolated static let nothingOpen = "There's no diff on screen to page through."

    static let commands: [[String]: GeniePageTurn] = [
        ["next", "page"]: .next, ["page", "down"]: .next, ["down", "page"]: .next, ["page", "forward"]: .next,
        ["forward", "page"]: .next, ["page"]: .next, ["page", "ahead"]: .next,
        ["previous", "page"]: .previous, ["prev", "page"]: .previous, ["page", "up"]: .previous, ["up", "page"]: .previous,
        ["back", "page"]: .previous, ["page", "back"]: .previous, ["prior", "page"]: .previous,
        ["next", "file"]: .nextFile, ["file", "down"]: .nextFile, ["forward", "file"]: .nextFile,
        ["previous", "file"]: .previousFile, ["prev", "file"]: .previousFile, ["back", "file"]: .previousFile,
        ["prior", "file"]: .previousFile, ["file", "up"]: .previousFile,
    ]
    static let fillers: Set<String> = [
        "go", "turn", "flip", "skip", "move", "jump", "scroll", "show", "take", "let's", "lets", "me", "us", "to", "the", "a",
        "one", "on", "onto", "over", "by", "this", "of", "diff",
    ]
    static let tail: Set<String> = ["please", "now", "thanks", "thank", "you", "for", "me"]

    static func spot(_ tokens: [GenieSpotter.Token], from start: Int) -> GenieSpot? {
        guard start < tokens.count else { return nil }
        var last = tokens.count - 1
        while last >= start, tail.contains(tokens[last].word) || GenieSpotter.wakeWords.contains(tokens[last].word) { last -= 1 }
        guard last >= start else { return nil }
        let said = tokens[start...last].map(\.word)
        let kept = said.filter { !fillers.contains($0) }
        guard let turn = commands[kept] else { return nil }
        if kept == ["page"], !said.contains(where: { ["turn", "flip"].contains($0) }) { return nil }
        return GenieSpot(
            tool: tool, arguments: ["direction": turn.rawValue], title: turn.title, page: page, symbol: turn.symbol, route: nil,
            phrase: GenieSpotter.range(tokens, start, last), addressed: true)
    }
}
