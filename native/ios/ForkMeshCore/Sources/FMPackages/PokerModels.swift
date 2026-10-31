import FMBase
import FMCrypto
import Foundation

struct PokerSkip: Decodable {
    init(from decoder: Decoder) throws {}
}

extension KeyedDecodingContainer {
    func pokerInt(_ key: Key, _ fallback: Int = 0) -> Int {
        if let value = try? decodeIfPresent(Int.self, forKey: key) { return value }
        if let value = try? decodeIfPresent(Double.self, forKey: key), value.isFinite, abs(value) < 9e15 { return Int(value) }
        if let text = try? decodeIfPresent(String.self, forKey: key), let value = Int(text) { return value }
        return fallback
    }

    func pokerInt64(_ key: Key, _ fallback: Int64 = 0) -> Int64 {
        if let value = try? decodeIfPresent(Int64.self, forKey: key) { return value }
        if let value = try? decodeIfPresent(Double.self, forKey: key), value.isFinite, abs(value) < 9e15 { return Int64(value) }
        return fallback
    }

    func pokerBool(_ key: Key, _ fallback: Bool = false) -> Bool {
        if let value = try? decodeIfPresent(Bool.self, forKey: key) { return value }
        if let value = try? decodeIfPresent(Int.self, forKey: key) { return value != 0 }
        return fallback
    }

    func pokerString(_ key: Key, _ fallback: String = "") -> String {
        if let value = try? decodeIfPresent(String.self, forKey: key) { return value }
        return fallback
    }

    func pokerList<T: Decodable>(_ key: Key) -> [T] {
        guard var list = try? nestedUnkeyedContainer(forKey: key) else { return [] }
        var out: [T] = []
        while !list.isAtEnd {
            if let value = try? list.decode(T.self) {
                out.append(value)
            } else if (try? list.decode(PokerSkip.self)) == nil {
                break
            }
        }
        return out
    }

    func pokerObject<T: Decodable>(_ key: Key) -> T? {
        guard (try? decodeNil(forKey: key)) == false else { return nil }
        return try? decode(T.self, forKey: key)
    }
}

public enum PokerJSON {
    public static func decode<T: Decodable>(_ type: T.Type, _ json: JSONObject) -> T? {
        try? JSONDecoder().decode(T.self, from: JSON.encodeData(.object(json)))
    }

    public static func decode<T: Decodable>(_ type: T.Type, data: Data) -> T? {
        try? JSONDecoder().decode(T.self, from: data)
    }
}

public struct PokerWallet: Codable, Equatable, Sendable {
    public var chips: Int = 0
    public var onTables: Int = 0
    public var canClaim: Bool = false
    public var claimAmount: Int = 2000
    public var nextClaimAt: Int64 = 0
    public var handsPlayed: Int = 0
    public var handsWon: Int = 0
    public var biggestPot: Int = 0
    public var bestHand: String = ""

    enum CodingKeys: String, CodingKey {
        case chips, onTables, canClaim, claimAmount, nextClaimAt, handsPlayed, handsWon, biggestPot, bestHand
    }

    public init(
        chips: Int = 0, onTables: Int = 0, canClaim: Bool = false, claimAmount: Int = 2000, nextClaimAt: Int64 = 0,
        handsPlayed: Int = 0, handsWon: Int = 0, biggestPot: Int = 0, bestHand: String = ""
    ) {
        self.chips = chips
        self.onTables = onTables
        self.canClaim = canClaim
        self.claimAmount = claimAmount
        self.nextClaimAt = nextClaimAt
        self.handsPlayed = handsPlayed
        self.handsWon = handsWon
        self.biggestPot = biggestPot
        self.bestHand = bestHand
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        chips = c.pokerInt(.chips)
        onTables = c.pokerInt(.onTables)
        canClaim = c.pokerBool(.canClaim)
        claimAmount = c.pokerInt(.claimAmount, 2000)
        nextClaimAt = c.pokerInt64(.nextClaimAt)
        handsPlayed = c.pokerInt(.handsPlayed)
        handsWon = c.pokerInt(.handsWon)
        biggestPot = c.pokerInt(.biggestPot)
        bestHand = c.pokerString(.bestHand)
    }
}

public struct PokerStake: Codable, Equatable, Identifiable, Sendable {
    public var id: String = ""
    public var label: String = ""
    public var small: Int = 0
    public var big: Int = 0
    public var minBuyIn: Int = 0
    public var maxBuyIn: Int = 0

    enum CodingKeys: String, CodingKey {
        case id, label, small, big, minBuyIn, maxBuyIn
    }

    public init(id: String, label: String, small: Int, big: Int, minBuyIn: Int, maxBuyIn: Int) {
        self.id = id
        self.label = label
        self.small = small
        self.big = big
        self.minBuyIn = minBuyIn
        self.maxBuyIn = maxBuyIn
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.pokerString(.id)
        label = c.pokerString(.label, id.capitalized)
        small = c.pokerInt(.small)
        big = c.pokerInt(.big)
        minBuyIn = c.pokerInt(.minBuyIn)
        maxBuyIn = c.pokerInt(.maxBuyIn)
    }

    public static let defaults: [PokerStake] = [
        PokerStake(id: "starter", label: "Starter", small: 5, big: 10, minBuyIn: 200, maxBuyIn: 1000),
        PokerStake(id: "low", label: "Low", small: 25, big: 50, minBuyIn: 1000, maxBuyIn: 5000),
        PokerStake(id: "mid", label: "Mid", small: 100, big: 200, minBuyIn: 4000, maxBuyIn: 20000),
        PokerStake(id: "high", label: "High", small: 500, big: 1000, minBuyIn: 20000, maxBuyIn: 100000),
    ]

    public var blindsLine: String { "\(PokerText.chips(small)) / \(PokerText.chips(big))" }
    public var buyInLine: String { "\(PokerText.chips(minBuyIn)) – \(PokerText.chips(maxBuyIn))" }
}

public struct PokerTableSummary: Codable, Equatable, Identifiable, Sendable {
    public var id: String = ""
    public var name: String = ""
    public var code: String = ""
    public var isPrivate: Bool = false
    public var owner: String = ""
    public var stakes: String = ""
    public var small: Int = 0
    public var big: Int = 0
    public var minBuyIn: Int = 0
    public var maxBuyIn: Int = 0
    public var seats: Int = 0
    public var players: Int = 0
    public var humans: Int = 0
    public var bots: Int = 0
    public var handNumber: Int = 0
    public var turnSeconds: Int = 30
    public var mine: Bool = false
    public var invited: Bool = false
    public var updatedAt: Int64 = 0

    enum CodingKeys: String, CodingKey {
        case id, name, code, isPrivate = "private", owner, stakes, small, big, minBuyIn, maxBuyIn, seats, players, humans
        case bots, handNumber, turnSeconds, mine, invited, updatedAt
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.pokerString(.id)
        name = c.pokerString(.name)
        code = c.pokerString(.code)
        isPrivate = c.pokerBool(.isPrivate)
        owner = c.pokerString(.owner)
        stakes = c.pokerString(.stakes)
        small = c.pokerInt(.small)
        big = c.pokerInt(.big)
        minBuyIn = c.pokerInt(.minBuyIn)
        maxBuyIn = c.pokerInt(.maxBuyIn)
        seats = c.pokerInt(.seats)
        players = c.pokerInt(.players)
        humans = c.pokerInt(.humans)
        bots = c.pokerInt(.bots)
        handNumber = c.pokerInt(.handNumber)
        turnSeconds = c.pokerInt(.turnSeconds, 30)
        mine = c.pokerBool(.mine)
        invited = c.pokerBool(.invited)
        updatedAt = c.pokerInt64(.updatedAt)
    }

    public var blindsLine: String { "\(PokerText.chips(small)) / \(PokerText.chips(big))" }
    public var seatsLine: String { "\(players) of \(seats) seated" }
    public var isFull: Bool { seats > 0 && players >= seats }
}

public struct PokerLobby: Codable, Equatable, Sendable {
    public var actor: String = ""
    public var wallet = PokerWallet()
    public var stakes: [PokerStake] = PokerStake.defaults
    public var tables: [PokerTableSummary] = []
    public var now: Int64 = 0

    enum CodingKeys: String, CodingKey {
        case actor, wallet, stakes, tables, now
    }

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        actor = c.pokerString(.actor)
        wallet = c.pokerObject(.wallet) ?? PokerWallet()
        let stakes: [PokerStake] = c.pokerList(.stakes).filter { !$0.id.isEmpty }
        self.stakes = stakes.isEmpty ? PokerStake.defaults : stakes
        tables = c.pokerList(.tables).filter { (table: PokerTableSummary) in PokerIds.isTableId(table.id) }
        now = c.pokerInt64(.now)
    }

    public func isMine(_ table: PokerTableSummary) -> Bool { table.mine || (!actor.isEmpty && table.owner == actor) }
    public var myTables: [PokerTableSummary] { tables.filter { isMine($0) } }
    public var invitedTables: [PokerTableSummary] { tables.filter { !isMine($0) && $0.invited } }
    public var openTables: [PokerTableSummary] { tables.filter { !isMine($0) && !$0.invited } }

    public func stake(_ id: String) -> PokerStake? { stakes.first { $0.id == id } }
}

public struct PokerLastAction: Codable, Equatable, Sendable {
    public var kind: String = ""
    public var amount: Int = 0

    enum CodingKeys: String, CodingKey {
        case kind, amount
    }

    public init(kind: String, amount: Int = 0) {
        self.kind = kind
        self.amount = amount
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = c.pokerString(.kind)
        amount = c.pokerInt(.amount)
    }

    public var label: String { PokerText.actionTag(kind, amount: amount) }
}

public struct PokerSeat: Codable, Equatable, Identifiable, Sendable {
    public var index: Int = 0
    public var empty: Bool = true
    public var name: String = ""
    public var bot: Bool = false
    public var stack: Int = 0
    public var bet: Int = 0
    public var totalBet: Int = 0
    public var status: String = "out"
    public var cards: [String] = []
    public var lastAction: PokerLastAction?
    public var isTurn: Bool = false
    public var away: Bool = false
    public var handName: String = ""
    public var winner: Bool = false
    public var won: Int = 0
    public var label: String = ""

    enum CodingKeys: String, CodingKey {
        case index, empty, name, bot, stack, bet, totalBet, status, cards, lastAction, isTurn, away, handName, winner, won
        case label = "displayName"
    }

    public init(index: Int, empty: Bool = true) {
        self.index = index
        self.empty = empty
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        index = c.pokerInt(.index)
        name = c.pokerString(.name)
        empty = c.pokerBool(.empty, name.isEmpty)
        bot = c.pokerBool(.bot)
        stack = c.pokerInt(.stack)
        bet = c.pokerInt(.bet)
        totalBet = c.pokerInt(.totalBet)
        status = c.pokerString(.status, "out")
        cards = c.pokerList(.cards)
        lastAction = c.pokerObject(.lastAction)
        isTurn = c.pokerBool(.isTurn)
        away = c.pokerBool(.away)
        handName = c.pokerString(.handName)
        winner = c.pokerBool(.winner)
        won = c.pokerInt(.won)
        label = c.pokerString(.label)
    }

    public var id: Int { index }
    public var displayName: String { label.isEmpty ? PokerText.displayName(name, bot: bot) : label }
    public var initial: String { PokerText.initial(name) }
    public var inHand: Bool { !empty && (status == "active" || status == "allin") }
    public var folded: Bool { status == "folded" }
    public var busted: Bool { !empty && stack == 0 && !inHand }
    public var parsedCards: [PokerCard] { cards.compactMap(PokerCard.init(code:)) }
}

public struct PokerPot: Codable, Equatable, Sendable {
    public var amount: Int = 0
    public var eligible: [Int] = []

    enum CodingKeys: String, CodingKey {
        case amount, eligible
    }

    public init(amount: Int, eligible: [Int] = []) {
        self.amount = amount
        self.eligible = eligible
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        amount = c.pokerInt(.amount)
        eligible = c.pokerList(.eligible)
    }
}

public struct PokerRaiseRange: Codable, Equatable, Sendable {
    public var min: Int = 0
    public var max: Int = 0

    enum CodingKeys: String, CodingKey {
        case min, max
    }

    public init(min: Int, max: Int) {
        self.min = min
        self.max = max
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        min = c.pokerInt(.min)
        max = c.pokerInt(.max)
    }
}

public struct PokerLegal: Codable, Equatable, Sendable {
    public var fold: Bool = true
    public var check: Bool = false
    public var call: Int = 0
    public var raise: PokerRaiseRange?
    public var allIn: Int = 0

    enum CodingKeys: String, CodingKey {
        case fold, check, call, raise, allIn
    }

    public init(fold: Bool = true, check: Bool = false, call: Int = 0, raise: PokerRaiseRange? = nil, allIn: Int = 0) {
        self.fold = fold
        self.check = check
        self.call = call
        self.raise = raise
        self.allIn = allIn
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        fold = c.pokerBool(.fold, true)
        call = c.pokerInt(.call)
        check = c.pokerBool(.check, call == 0)
        let range: PokerRaiseRange? = c.pokerObject(.raise)
        raise = range.flatMap { $0.max > 0 && $0.max >= $0.min ? $0 : nil }
        allIn = c.pokerInt(.allIn)
    }
}

public struct PokerHandInfo: Codable, Equatable, Sendable {
    public var name: String = ""
    public var cards: [String] = []

    enum CodingKeys: String, CodingKey {
        case name, cards
    }

    public init(name: String, cards: [String] = []) {
        self.name = name
        self.cards = cards
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = c.pokerString(.name)
        cards = c.pokerList(.cards)
    }
}

public struct PokerEvent: Codable, Equatable, Identifiable, Sendable {
    public var seq: Int = 0
    public var kind: String = ""
    public var seat: Int = -1
    public var amount: Int = 0

    enum CodingKeys: String, CodingKey {
        case seq, kind, seat, amount
    }

    public init(seq: Int, kind: String, seat: Int = -1, amount: Int = 0) {
        self.seq = seq
        self.kind = kind
        self.seat = seat
        self.amount = amount
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        seq = c.pokerInt(.seq)
        kind = c.pokerString(.kind)
        seat = c.pokerInt(.seat, -1)
        amount = c.pokerInt(.amount)
    }

    public var id: Int { seq }
}

public struct PokerLogLine: Codable, Equatable, Identifiable, Sendable {
    public var at: Int64 = 0
    public var seat: Int = -1
    public var name: String = ""
    public var kind: String = ""
    public var amount: Int = 0
    public var text: String = ""
    public var order: Int = 0

    enum CodingKeys: String, CodingKey {
        case at, seat, name, kind, amount, text
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        at = c.pokerInt64(.at)
        seat = c.pokerInt(.seat, -1)
        name = c.pokerString(.name)
        kind = c.pokerString(.kind)
        amount = c.pokerInt(.amount)
        text = c.pokerString(.text)
    }

    public var id: String { "\(at)-\(order)" }
}

public struct PokerChatLine: Codable, Equatable, Identifiable, Sendable {
    public var at: Int64 = 0
    public var name: String = ""
    public var text: String = ""
    public var order: Int = 0

    enum CodingKeys: String, CodingKey {
        case at, name, text
    }

    public init(at: Int64, name: String, text: String) {
        self.at = at
        self.name = name
        self.text = text
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        at = c.pokerInt64(.at)
        name = c.pokerString(.name)
        text = c.pokerString(.text)
    }

    public var id: String { "\(at)-\(order)" }
}

public struct PokerWinner: Codable, Equatable, Sendable {
    public var seat: Int = -1
    public var name: String = ""
    public var amount: Int = 0
    public var handName: String = ""
    public var cards: [String] = []
    public var best: [String] = []

    enum CodingKeys: String, CodingKey {
        case seat, name, amount, handName, cards, best
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        seat = c.pokerInt(.seat, -1)
        name = c.pokerString(.name)
        amount = c.pokerInt(.amount)
        handName = c.pokerString(.handName)
        cards = c.pokerList(.cards)
        best = c.pokerList(.best)
    }
}

public struct PokerShown: Codable, Equatable, Sendable {
    public var seat: Int = -1
    public var name: String = ""
    public var cards: [String] = []
    public var handName: String = ""

    enum CodingKeys: String, CodingKey {
        case seat, name, cards, handName
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        seat = c.pokerInt(.seat, -1)
        name = c.pokerString(.name)
        cards = c.pokerList(.cards)
        handName = c.pokerString(.handName)
    }
}

public struct PokerRevealedCard: Codable, Equatable, Identifiable, Sendable {
    public var index: Int = -1
    public var card: String = ""
    public var salt: String = ""

    enum CodingKeys: String, CodingKey {
        case index, card, salt
    }

    public init(index: Int, card: String, salt: String) {
        self.index = index
        self.card = card
        self.salt = salt
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        index = c.pokerInt(.index, -1)
        card = c.pokerString(.card)
        salt = c.pokerString(.salt)
    }

    public var id: Int { index }
    public var parsed: PokerCard? { PokerCard(code: card) }
}

public struct PokerReveal: Codable, Equatable, Sendable {
    public var hashes: [String] = []
    public var cards: [PokerRevealedCard] = []

    enum CodingKeys: String, CodingKey {
        case hashes, cards
    }

    public init(hashes: [String], cards: [PokerRevealedCard]) {
        self.hashes = hashes
        self.cards = cards
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        hashes = c.pokerList(.hashes)
        cards = c.pokerList(.cards).sorted { (a: PokerRevealedCard, b: PokerRevealedCard) in a.index < b.index }
    }
}

public struct PokerLastHand: Codable, Equatable, Sendable {
    public var number: Int = 0
    public var board: [String] = []
    public var winners: [PokerWinner] = []
    public var shown: [PokerShown] = []
    public var commitment: String = ""
    public var reveal: PokerReveal?

    enum CodingKeys: String, CodingKey {
        case number, board, winners, shown, commitment, reveal
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        number = c.pokerInt(.number)
        board = c.pokerList(.board)
        winners = c.pokerList(.winners)
        shown = c.pokerList(.shown)
        commitment = c.pokerString(.commitment)
        reveal = c.pokerObject(.reveal)
    }

    public var verified: Bool? {
        guard let reveal, !reveal.hashes.isEmpty, !commitment.isEmpty else { return nil }
        return PokerLastHand.verify(commitment: commitment, reveal: reveal)
    }

    public static let deckSize = 52

    public static func cardHash(salt: String, index: Int, card: String) -> String {
        Sha256.hex(Array("\(salt):\(index):\(card)".utf8))
    }

    public static func commitment(hashes: [String]) -> String {
        Sha256.hex(Array(hashes.joined(separator: ",").utf8))
    }

    public static func verify(commitment: String, reveal: PokerReveal) -> Bool {
        let hashes = reveal.hashes.map { $0.lowercased() }
        guard hashes.count == deckSize, Self.commitment(hashes: hashes) == commitment.lowercased() else { return false }
        return reveal.cards.allSatisfy { (shown: PokerRevealedCard) in
            hashes.indices.contains(shown.index) && cardHash(salt: shown.salt, index: shown.index, card: shown.card) == hashes[shown.index]
        }
    }
}

public struct PokerHistoryEntry: Codable, Equatable, Identifiable, Sendable {
    public var number: Int = 0
    public var winners: [String] = []
    public var amount: Int = 0
    public var handName: String = ""
    public var at: Int64 = 0

    enum CodingKeys: String, CodingKey {
        case number, winners, amount, handName, at
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        number = c.pokerInt(.number)
        winners = c.pokerList(.winners)
        amount = c.pokerInt(.amount)
        handName = c.pokerString(.handName)
        at = c.pokerInt64(.at)
    }

    public var id: Int { number }
}

public struct PokerTable: Codable, Equatable, Identifiable, Sendable {
    public var id: String = ""
    public var name: String = ""
    public var code: String = ""
    public var isPrivate: Bool = false
    public var owner: String = ""
    public var ownerIsMe: Bool = false
    public var stakes: String = ""
    public var small: Int = 0
    public var big: Int = 0
    public var minBuyIn: Int = 0
    public var maxBuyIn: Int = 0
    public var seatCount: Int = 0
    public var turnSeconds: Int = 30
    public var version: Int = 0
    public var now: Int64 = 0
    public var handNumber: Int = 0
    public var phase: String = "waiting"
    public var board: [String] = []
    public var pots: [PokerPot] = []
    public var potTotal: Int = 0
    public var dealer: Int = -1
    public var smallBlind: Int = -1
    public var bigBlind: Int = -1
    public var toAct: Int = -1
    public var actStartedAt: Int64 = 0
    public var actDeadline: Int64 = 0
    public var currentBet: Int = 0
    public var minRaiseTo: Int = 0
    public var wakeAt: Int64 = 0
    public var mySeat: Int = -1
    public var wallet: Int = 0
    public var seats: [PokerSeat] = []
    public var legal: PokerLegal?
    public var myHand: PokerHandInfo?
    public var dealerLine: String = ""
    public var commitment: String = ""
    public var events: [PokerEvent] = []
    public var log: [PokerLogLine] = []
    public var chat: [PokerChatLine] = []
    public var lastHand: PokerLastHand?
    public var history: [PokerHistoryEntry] = []

    enum CodingKeys: String, CodingKey {
        case id, name, code, isPrivate = "private", owner, ownerIsMe, stakes, small, big, minBuyIn, maxBuyIn, seatCount
        case turnSeconds, version, now, handNumber, phase, board, pots, potTotal, dealer, smallBlind, bigBlind, toAct
        case actStartedAt, actDeadline, currentBet, minRaiseTo, wakeAt, mySeat, wallet, seats, legal, myHand, dealerLine
        case commitment, events, log, chat, lastHand, history
    }

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.pokerString(.id)
        name = c.pokerString(.name)
        code = c.pokerString(.code)
        isPrivate = c.pokerBool(.isPrivate)
        owner = c.pokerString(.owner)
        ownerIsMe = c.pokerBool(.ownerIsMe)
        stakes = c.pokerString(.stakes)
        small = c.pokerInt(.small)
        big = c.pokerInt(.big)
        minBuyIn = c.pokerInt(.minBuyIn)
        maxBuyIn = c.pokerInt(.maxBuyIn)
        turnSeconds = c.pokerInt(.turnSeconds, 30)
        version = c.pokerInt(.version)
        now = c.pokerInt64(.now)
        handNumber = c.pokerInt(.handNumber)
        phase = c.pokerString(.phase, "waiting")
        board = c.pokerList(.board)
        pots = c.pokerList(.pots)
        potTotal = c.pokerInt(.potTotal)
        dealer = c.pokerInt(.dealer, -1)
        smallBlind = c.pokerInt(.smallBlind, -1)
        bigBlind = c.pokerInt(.bigBlind, -1)
        toAct = c.pokerInt(.toAct, -1)
        actStartedAt = c.pokerInt64(.actStartedAt)
        actDeadline = c.pokerInt64(.actDeadline)
        currentBet = c.pokerInt(.currentBet)
        minRaiseTo = c.pokerInt(.minRaiseTo)
        wakeAt = c.pokerInt64(.wakeAt)
        mySeat = c.pokerInt(.mySeat, -1)
        wallet = c.pokerInt(.wallet)
        let decoded: [PokerSeat] = c.pokerList(.seats)
        let count = max(0, min(9, c.pokerInt(.seatCount, decoded.count)))
        seatCount = count
        seats = PokerTable.normalized(decoded, count: count)
        legal = c.pokerObject(.legal)
        myHand = c.pokerObject(.myHand)
        dealerLine = c.pokerString(.dealerLine)
        commitment = c.pokerString(.commitment)
        events = c.pokerList(.events).sorted { (a: PokerEvent, b: PokerEvent) in a.seq < b.seq }
        var log: [PokerLogLine] = c.pokerList(.log)
        for index in log.indices { log[index].order = index }
        self.log = log
        var chat: [PokerChatLine] = c.pokerList(.chat)
        for index in chat.indices { chat[index].order = index }
        self.chat = chat
        lastHand = c.pokerObject(.lastHand)
        history = c.pokerList(.history)
    }

    static func normalized(_ seats: [PokerSeat], count: Int) -> [PokerSeat] {
        var out = (0..<count).map { PokerSeat(index: $0) }
        for seat in seats where seat.index >= 0 && seat.index < count {
            out[seat.index] = seat
        }
        return out
    }

    public static func from(_ json: JSONObject) -> PokerTable? {
        guard let table = PokerJSON.decode(PokerTable.self, json), PokerIds.isTableId(table.id) else { return nil }
        return table
    }

    public var seated: Bool { mySeat >= 0 && mySeat < seats.count && !seats[mySeat].empty }
    public var me: PokerSeat? { seated ? seats[mySeat] : nil }
    public var myTurn: Bool { seated && toAct == mySeat && legal != nil }
    public var inHand: Bool { ["preflop", "flop", "turn", "river"].contains(phase) }
    public var showdown: Bool { phase == "showdown" }
    public var waiting: Bool { phase == "waiting" }
    public var openSeats: [Int] { seats.filter(\.empty).map(\.index) }
    public var winners: [PokerSeat] { seats.filter { !$0.empty && $0.winner } }
    public var maxSeq: Int { events.map(\.seq).max() ?? 0 }
    public var boardCards: [PokerCard] { board.compactMap(PokerCard.init(code:)) }
    public var winningCards: Set<String> {
        guard showdown, let hand = lastHand, hand.number == handNumber else { return [] }
        return Set(hand.winners.flatMap(\.best))
    }
    public var stake: PokerStake {
        PokerStake(id: stakes, label: stakes.capitalized, small: small, big: big, minBuyIn: minBuyIn, maxBuyIn: maxBuyIn)
    }
    public var blindsLine: String { "\(PokerText.chips(small)) / \(PokerText.chips(big))" }
    public var betsOnFelt: Int { seats.reduce(0) { $0 + $1.bet } }
    public var potInMiddle: Int { max(0, potTotal - betsOnFelt) }
    public var canRebuy: Bool {
        guard let me else { return false }
        return !me.inHand && me.stack < maxBuyIn && wallet > 0
    }
}

public enum PokerIds {
    public static func isTableId(_ value: String) -> Bool {
        value.count == 32 && value.allSatisfy { $0.isHexDigit && !$0.isUppercase }
    }

    public static func normalizedCode(_ value: String) -> String? {
        let letters = value.uppercased().filter { !$0.isWhitespace && $0 != "-" }
        guard letters.count == 6, letters.allSatisfy({ $0.isASCII && $0.isLetter }) else { return nil }
        return letters
    }
}
