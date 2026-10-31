import Foundation

public enum PokerSuit: String, CaseIterable, Sendable {
    case clubs = "c"
    case diamonds = "d"
    case hearts = "h"
    case spades = "s"

    public var symbol: String {
        switch self {
        case .clubs: return "♣"
        case .diamonds: return "♦"
        case .hearts: return "♥"
        case .spades: return "♠"
        }
    }

    public var isRed: Bool { self == .hearts || self == .diamonds }
    public var colorHex: String { isRed ? "#c62828" : "#1a1a1a" }
}

public struct PokerCard: Equatable, Hashable, Sendable {
    public static let ranks: [Character] = Array("23456789TJQKA")

    public let code: String
    public let rank: Character?
    public let suit: PokerSuit?

    public init?(code: String) {
        let text = code.trimmingCharacters(in: .whitespaces)
        guard text.count == 2 else { return nil }
        if text == "??" {
            self.code = text
            rank = nil
            suit = nil
            return
        }
        let chars = Array(text)
        let rank = Character(chars[0].uppercased())
        guard PokerCard.ranks.contains(rank), let suit = PokerSuit(rawValue: chars[1].lowercased()) else { return nil }
        self.code = "\(rank)\(suit.rawValue)"
        self.rank = rank
        self.suit = suit
    }

    public static let back = PokerCard(code: "??")!

    public var faceDown: Bool { rank == nil }
    public var isRed: Bool { suit?.isRed ?? false }

    public var rankLabel: String {
        guard let rank else { return "" }
        return rank == "T" ? "10" : String(rank)
    }

    public var label: String { faceDown ? "" : rankLabel + (suit?.symbol ?? "") }

    public var spokenName: String {
        guard let rank, let suit else { return "Face-down card" }
        let ranks: [Character: String] = [
            "2": "Two", "3": "Three", "4": "Four", "5": "Five", "6": "Six", "7": "Seven", "8": "Eight", "9": "Nine",
            "T": "Ten", "J": "Jack", "Q": "Queen", "K": "King", "A": "Ace",
        ]
        let suits: [PokerSuit: String] = [.clubs: "Clubs", .diamonds: "Diamonds", .hearts: "Hearts", .spades: "Spades"]
        return "\(ranks[rank] ?? String(rank)) of \(suits[suit] ?? "")"
    }

    public static func parse(_ codes: [String]) -> [PokerCard] { codes.compactMap(PokerCard.init(code:)) }
}

public struct PokerChipCount: Equatable, Sendable {
    public let denomination: Int
    public let count: Int
}

public enum PokerChips {
    public static let denominations = [5000, 1000, 500, 100, 25, 5, 1]

    public static func colorHex(_ denomination: Int) -> String {
        switch denomination {
        case 5000: return "#ef6c00"
        case 1000: return "#f9a825"
        case 500: return "#6a1b9a"
        case 100: return "#212121"
        case 25: return "#2e7d32"
        case 5: return "#c62828"
        default: return "#f5f5f5"
        }
    }

    public static func breakdown(_ amount: Int) -> [PokerChipCount] {
        var left = max(0, amount)
        var out: [PokerChipCount] = []
        for value in denominations where left >= value {
            let count = left / value
            left -= count * value
            out.append(PokerChipCount(denomination: value, count: count))
        }
        return out
    }

    public static func stack(_ amount: Int, limit: Int = 10) -> [Int] {
        guard limit > 0 else { return [] }
        let chips = breakdown(amount).flatMap { [Int](repeating: $0.denomination, count: $0.count) }
        return Array(chips.prefix(limit))
    }

    public static func columns(_ amount: Int, maxColumns: Int = 3, perColumn: Int = 8) -> [[Int]] {
        let parts = breakdown(amount)
        guard !parts.isEmpty, maxColumns > 0, perColumn > 0 else { return [] }
        return parts.prefix(maxColumns).map { [Int](repeating: $0.denomination, count: min(perColumn, $0.count)) }
    }
}

public struct PokerPoint: Equatable, Sendable {
    public var x: Double
    public var y: Double

    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }
}

public enum PokerLayout {
    public static let topGap = 0.7

    public static func slots(seatCount: Int, mySeat: Int) -> [Int] {
        guard seatCount > 0 else { return [] }
        let base = (0..<seatCount).contains(mySeat) ? mySeat : 0
        return (0..<seatCount).map { (base + $0) % seatCount }
    }

    public static func slot(of seat: Int, seatCount: Int, mySeat: Int) -> Int {
        guard seatCount > 0 else { return 0 }
        let base = (0..<seatCount).contains(mySeat) ? mySeat : 0
        return ((seat - base) % seatCount + seatCount) % seatCount
    }

    public static func angle(slot: Int, count: Int, gap: Double = topGap) -> Double {
        guard count > 1 else { return Double.pi / 2 }
        let path = 2 * Double.pi - gap
        let along = Double(slot % count) * path / Double(count)
        let angle = along <= Double.pi - gap / 2 + 1e-9 ? Double.pi / 2 + along : Double.pi / 2 + along + gap
        return angle.truncatingRemainder(dividingBy: 2 * Double.pi)
    }

    public static func point(angle: Double, center: PokerPoint, radiusX: Double, radiusY: Double) -> PokerPoint {
        PokerPoint(x: center.x + cos(angle) * radiusX, y: center.y + sin(angle) * radiusY)
    }

    public static func positions(
        seatCount: Int, mySeat: Int, center: PokerPoint, radiusX: Double, radiusY: Double, gap: Double = topGap
    ) -> [Int: PokerPoint] {
        var out: [Int: PokerPoint] = [:]
        for (slot, seat) in slots(seatCount: seatCount, mySeat: mySeat).enumerated() {
            out[seat] = point(angle: angle(slot: slot, count: seatCount, gap: gap), center: center, radiusX: radiusX, radiusY: radiusY)
        }
        return out
    }

    public static func toward(_ from: PokerPoint, _ to: PokerPoint, fraction: Double) -> PokerPoint {
        PokerPoint(x: from.x + (to.x - from.x) * fraction, y: from.y + (to.y - from.y) * fraction)
    }
}

public enum PokerActionKind: String, Sendable {
    case fold, check, call, bet, raise, allin
}

public struct PokerPreset: Equatable, Identifiable, Sendable {
    public let id: String
    public let label: String
    public let amount: Int
}

public enum PokerActions {
    public static func callLabel(_ legal: PokerLegal) -> String {
        legal.call > 0 ? "Call \(PokerText.chips(legal.call))" : "Check"
    }

    public static func callKind(_ legal: PokerLegal) -> PokerActionKind {
        legal.call > 0 ? .call : .check
    }

    public static func raiseKind(_ table: PokerTable) -> PokerActionKind {
        table.currentBet > 0 ? .raise : .bet
    }

    public static func raiseLabel(_ table: PokerTable, amount: Int) -> String {
        let verb = table.currentBet > 0 ? "Raise to" : "Bet"
        return "\(verb) \(PokerText.chips(amount))"
    }

    public static func allInLabel(_ legal: PokerLegal) -> String {
        "All-in \(PokerText.chips(legal.allIn))"
    }

    public static func canRaise(_ legal: PokerLegal) -> Bool {
        guard let raise = legal.raise else { return false }
        return raise.max > raise.min
    }

    public static func clamp(_ amount: Int, _ legal: PokerLegal) -> Int {
        guard let raise = legal.raise else { return 0 }
        return min(raise.max, max(raise.min, amount))
    }

    public static func potRaise(_ table: PokerTable, fraction: Double) -> Int {
        let call = table.legal?.call ?? 0
        let pot = max(0, table.potTotal) + call
        return table.currentBet + Int((Double(pot) * fraction).rounded())
    }

    public static func presets(_ table: PokerTable) -> [PokerPreset] {
        guard let legal = table.legal, let raise = legal.raise else { return [] }
        let options: [(String, String, Int)] = [
            ("half", "½ Pot", potRaise(table, fraction: 0.5)),
            ("threeQuarter", "¾ Pot", potRaise(table, fraction: 0.75)),
            ("pot", "Pot", potRaise(table, fraction: 1)),
            ("allIn", "All-in", raise.max),
        ]
        return options.map { PokerPreset(id: $0.0, label: $0.1, amount: clamp($0.2, legal)) }
    }

    public static func defaultRaise(_ table: PokerTable) -> Int {
        guard let legal = table.legal, let raise = legal.raise else { return 0 }
        return clamp(raise.min, legal)
    }

    public static func step(_ table: PokerTable) -> Int {
        max(1, table.small > 0 ? table.small : 1)
    }
}

public enum PokerClockTone: String, Sendable {
    case green, amber, red
}

public enum PokerClock {
    public static func tone(remainingMs: Int64) -> PokerClockTone {
        if remainingMs < 5000 { return .red }
        if remainingMs < 10000 { return .amber }
        return .green
    }

    public static func fraction(remainingMs: Int64, totalMs: Int64) -> Double {
        guard totalMs > 0 else { return 0 }
        return min(1, max(0, Double(remainingMs) / Double(totalMs)))
    }

    public static func wakeDelayMs(wakeAt: Int64, serverNow: Int64, lead: Int64 = 150) -> Int64? {
        guard wakeAt > 0 else { return nil }
        return max(0, wakeAt + lead - serverNow)
    }

    public static func tickTimes(deadline: Int64, serverNow: Int64, last: Int = 5) -> [Int64] {
        guard deadline > serverNow else { return [] }
        return (1...max(1, last)).reversed().map { deadline - Int64($0) * 1000 }.filter { $0 >= serverNow }
    }
}

public struct PokerServerClock: Equatable, Sendable {
    public private(set) var offsetMs: Int64 = 0

    public init(offsetMs: Int64 = 0) {
        self.offsetMs = offsetMs
    }

    public mutating func sync(serverNow: Int64, localNow: Int64) {
        guard serverNow > 0 else { return }
        offsetMs = serverNow - localNow
    }

    public func serverNow(_ localNow: Int64) -> Int64 { localNow + offsetMs }
    public func localTime(_ serverTime: Int64) -> Int64 { serverTime - offsetMs }
}

public struct PokerEventCursor: Equatable, Sendable {
    public private(set) var tableId = ""
    public private(set) var seen: Int?

    public init() {}

    public mutating func reset() {
        tableId = ""
        seen = nil
    }

    public mutating func take(_ table: PokerTable) -> [PokerEvent] {
        let top = table.maxSeq
        guard table.id == tableId, let last = seen else {
            tableId = table.id
            seen = top
            return []
        }
        if top < last {
            seen = top
            return []
        }
        seen = top
        return table.events.filter { $0.seq > last }.sorted { $0.seq < $1.seq }
    }
}

public enum PokerCue: String, CaseIterable, Sendable {
    case shuffle, deal, chip, check, fold, yourTurn, clock, allIn, win
}

public struct PokerCueStep: Equatable, Sendable {
    public let cue: PokerCue
    public let delayMs: Int
}

public enum PokerCues {
    public static func cues(for event: PokerEvent) -> [PokerCue] {
        switch event.kind {
        case "shuffle": return [.shuffle]
        case "deal": return [PokerCue](repeating: .deal, count: event.amount > 0 ? min(8, max(2, event.amount * 2)) : 4)
        case "flop": return [.deal, .deal, .deal]
        case "turn", "river": return [.deal]
        case "blinds", "call", "bet", "raise": return [.chip]
        case "check": return [.check]
        case "fold", "timeout": return [.fold]
        case "allin": return [.allIn]
        case "win": return [.win]
        default: return []
        }
    }

    public static func gapMs(after cue: PokerCue) -> Int {
        switch cue {
        case .shuffle: return 420
        case .deal: return 95
        case .chip: return 170
        case .check: return 260
        case .fold: return 210
        case .yourTurn: return 300
        case .clock: return 0
        case .allIn: return 450
        case .win: return 700
        }
    }

    public static func plan(_ events: [PokerEvent], maxEvents: Int = 8) -> [PokerCueStep] {
        var at = 0
        var out: [PokerCueStep] = []
        for event in events.suffix(maxEvents) {
            for cue in cues(for: event) {
                out.append(PokerCueStep(cue: cue, delayMs: at))
                at += gapMs(after: cue)
            }
        }
        return out
    }
}

public enum PokerText {
    public static func chips(_ value: Int) -> String {
        let negative = value < 0
        var digits = Array(String(abs(value)))
        var out: [Character] = []
        while digits.count > 3 {
            out.insert(contentsOf: [Character(",")] + Array(digits.suffix(3)), at: 0)
            digits.removeLast(3)
        }
        out.insert(contentsOf: digits, at: 0)
        return (negative ? "-" : "") + String(out)
    }

    public static func short(_ value: Int) -> String {
        let magnitude = abs(value)
        if magnitude >= 1_000_000 { return trimmed(Double(value) / 1_000_000) + "M" }
        if magnitude >= 100_000 { return trimmed(Double(value) / 1000) + "K" }
        return chips(value)
    }

    private static func trimmed(_ value: Double) -> String {
        let rounded = (value * 10).rounded() / 10
        if rounded == rounded.rounded() { return String(Int(rounded)) }
        return String(format: "%.1f", rounded)
    }

    public static func displayName(_ name: String, bot: Bool) -> String {
        guard bot else { return name }
        if name.hasSuffix("(bot)") { return name }
        let words = name.split(whereSeparator: { $0 == "-" || $0 == "_" || $0 == " " }).map { $0.prefix(1).uppercased() + $0.dropFirst() }
        let pretty = words.isEmpty ? "Bot" : words.joined(separator: " ")
        return "\(pretty) (bot)"
    }

    public static func initial(_ name: String) -> String {
        guard let first = name.first(where: { $0.isLetter || $0.isNumber }) else { return "?" }
        return String(first).uppercased()
    }

    public static func actionTag(_ kind: String, amount: Int) -> String {
        let value = amount > 0 ? " \(chips(amount))" : ""
        switch kind {
        case "fold": return "Fold"
        case "check": return "Check"
        case "call": return "Call" + value
        case "bet": return "Bet" + value
        case "raise": return "Raise" + value
        case "allin", "all in", "all-in": return "All-in" + value
        case "sb", "small", "smallBlind", "small blind": return "SB" + value
        case "bb", "big", "bigBlind", "big blind": return "BB" + value
        case "blind", "blinds": return "Blind" + value
        case "timeout": return "Timed out"
        case "": return ""
        default: return kind.prefix(1).uppercased() + kind.dropFirst() + value
        }
    }

    public static func phase(_ phase: String) -> String {
        switch phase {
        case "waiting": return "Waiting for players"
        case "preflop": return "Pre-flop"
        case "flop": return "Flop"
        case "turn": return "Turn"
        case "river": return "River"
        case "showdown": return "Showdown"
        default: return phase.capitalized
        }
    }

    public static func status(_ seat: PokerSeat) -> String {
        if seat.empty { return "Open seat" }
        if seat.away { return "Sitting out" }
        if seat.status == "leaving" { return "Leaving" }
        if seat.busted { return "Busted" }
        if seat.status == "allin" { return "All-in" }
        if seat.status == "folded" { return "Folded" }
        if seat.status == "out" { return "Next hand" }
        return ""
    }

    public static func countdown(_ ms: Int64) -> String {
        let total = max(0, Int((ms + 999) / 1000))
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let seconds = total % 60
        if hours > 0 { return String(format: "%d:%02d:%02d", hours, minutes, seconds) }
        return String(format: "%d:%02d", minutes, seconds)
    }

    public static func winRate(_ wallet: PokerWallet) -> String {
        guard wallet.handsPlayed > 0 else { return "–" }
        return "\(Int((Double(wallet.handsWon) * 100 / Double(wallet.handsPlayed)).rounded()))%"
    }

    public static func error(_ code: String) -> String {
        switch code {
        case "invalid_session", "unauthorized", "sign_in_required": return "Sign in again to continue."
        case "plugin_not_installed", "plugin_not_installed_for_account": return "Install Poker from the Market first."
        case "claim_not_ready": return "Your free chips aren't ready yet."
        case "not_found", "table_not_found": return "That table was closed or doesn't exist."
        case "table_full": return "That table is full."
        case "seat_taken": return "Someone just took that seat. Pick another one."
        case "already_seated": return "You already have a seat at this table."
        case "not_seated": return "Take a seat first."
        case "not_enough_chips": return "You don't have enough chips in your wallet."
        case "invalid_buy_in": return "Pick a buy-in between the table's minimum and maximum."
        case "too_many_tables": return "You already own five open tables. Close one first."
        case "owner_only": return "Only the table's owner can do that."
        case "hand_in_progress": return "Wait for this hand to finish."
        case "invalid_stakes": return "Pick one of the stakes."
        case "invalid_text": return "That message can't be sent."
        case "hand_changed": return "The hand moved on before your action arrived."
        case "not_your_turn": return "It isn't your turn."
        case "invalid_amount": return "That amount isn't allowed right now."
        case "invalid_code": return "Table codes are six letters."
        case "": return "ForkMesh could not be reached. Check your connection."
        default:
            let words = code.replacingOccurrences(of: "_", with: " ")
            return words.prefix(1).uppercased() + words.dropFirst() + "."
        }
    }
}
