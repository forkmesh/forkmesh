import FMApi
import FMBase
import XCTest

@testable import FMPackages

private let tableId = "0123456789abcdef0123456789abcdef"

private let lobbyJSON = """
{
  "ok": true,
  "actor": "alice",
  "wallet": {
    "chips": 8750, "onTables": 1000, "canClaim": false, "claimAmount": 2000,
    "nextClaimAt": 1790000000000, "handsPlayed": 42, "handsWon": 11,
    "biggestPot": 4200, "bestHand": "Flush, Ace high"
  },
  "stakes": [
    {"id": "starter", "label": "Starter", "small": 5, "big": 10, "minBuyIn": 200, "maxBuyIn": 1000}
  ],
  "tables": [
    {
      "id": "\(tableId)", "name": "Friday night", "code": "QXRTMB", "private": false,
      "owner": "alice", "stakes": "starter", "small": 5, "big": 10,
      "minBuyIn": 200, "maxBuyIn": 1000, "seats": 6, "players": 3, "humans": 2,
      "bots": 1, "handNumber": 17, "turnSeconds": 30, "mine": true,
      "invited": false, "updatedAt": 1790000000000
    },
    {"id": "<32 hex>", "name": "broken"}
  ],
  "now": 1790000000000
}
"""

private func tableJSON(id: String = tableId, version: Int = 211, events: String = #"[{"seq": 801, "kind": "flop", "seat": -1, "amount": 0}]"#, wakeAt: Int64 = 1790000020000, now: Int64 = 1790000000000) -> String {
    """
    {
      "id": "\(id)", "name": "Friday night", "code": "QXRTMB", "private": false,
      "owner": "alice", "ownerIsMe": true, "stakes": "starter", "small": 5, "big": 10,
      "minBuyIn": 200, "maxBuyIn": 1000, "seatCount": 6, "turnSeconds": 30,
      "version": \(version), "now": \(now), "handNumber": 17,
      "phase": "flop",
      "board": ["Ah", "Kd", "7c"],
      "pots": [{"amount": 300, "eligible": [0, 2, 4]}],
      "potTotal": 380,
      "dealer": 2, "smallBlind": 4, "bigBlind": 0,
      "toAct": 0, "actStartedAt": 1789999990000, "actDeadline": 1790000020000,
      "currentBet": 40, "minRaiseTo": 80,
      "wakeAt": \(wakeAt),
      "mySeat": 0,
      "wallet": 8750,
      "seats": [
        {"index": 0, "empty": false, "name": "alice", "bot": false, "stack": 960,
         "bet": 0, "totalBet": 40, "status": "active", "cards": ["As", "Qs"],
         "lastAction": {"kind": "call", "amount": 40}, "isTurn": true,
         "away": false, "handName": "", "winner": false, "won": 0},
        {"index": 1, "empty": true},
        {"index": 2, "empty": false, "name": "river-rita", "bot": true, "stack": 1000,
         "bet": 40, "totalBet": 80, "status": "active", "cards": ["??", "??"],
         "lastAction": {"kind": "bet", "amount": 40}, "isTurn": false,
         "away": false, "handName": "", "winner": false, "won": 0}
      ],
      "legal": {"fold": true, "check": false, "call": 40,
                "raise": {"min": 80, "max": 960}, "allIn": 960},
      "myHand": {"name": "Pair of Aces", "cards": ["As", "Ah", "Kd", "Qs", "7c"]},
      "dealerLine": "Flop: A♥ K♦ 7♣",
      "commitment": "<64 hex>",
      "events": \(events),
      "log": [{"at": 1789999990000, "seat": 2, "name": "river-rita", "kind": "bet", "amount": 40, "text": "River Rita bets 40"}],
      "chat": [{"at": 1789999980000, "name": "bob", "text": "nice hand"}],
      "lastHand": {
        "number": 16, "board": ["2c", "5d", "9h", "Js", "Qh"],
        "winners": [{"seat": 2, "name": "river-rita", "amount": 600, "handName": "Pair of Queens",
                     "cards": ["Qd", "3c"], "best": ["Qd", "Qh", "Js", "9h", "5d"]}],
        "shown": [{"seat": 0, "name": "alice", "cards": ["Tc", "Td"], "handName": "Pair of Tens"}],
        "commitment": "<64 hex>", "reveal": {"hashes": ["<64 hex>"], "cards": [{"index": 4, "card": "Qh", "salt": "00112233445566ff"}, {"index": 1, "card": "2c", "salt": "aa"}]}
      },
      "history": [{"number": 16, "winners": ["river-rita"], "amount": 600, "handName": "Pair of Queens", "at": 1789999900000}]
    }
    """
}

private func object(_ text: String) -> JSONObject { JSON.parseObject(text) }

private func table(_ text: String = tableJSON()) -> PokerTable { PokerTable.from(object(text))! }

@MainActor
final class FakePokerApi {
    var calls: [(method: String, path: String, body: JSONObject?)] = []
    var replies: [String: [JSONObject]] = [:]
    var errors: [String: String] = [:]

    func reply(_ key: String, _ json: JSONObject) { replies[key, default: []].append(json) }

    func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        let key = "\(method) \(path)"
        if let code = errors[key] { throw OrgApiException(code, code) }
        guard var queue = replies[key], !queue.isEmpty else { return ["ok": true] }
        let next = queue.count > 1 ? queue.removeFirst() : queue[0]
        replies[key] = queue
        return next
    }

    func count(_ key: String) -> Int { calls.filter { "\($0.method) \($0.path)" == key }.count }
}

final class PokerTests: XCTestCase {
    func testLobbyDecodesTheDocSample() throws {
        let lobby = try XCTUnwrap(PokerJSON.decode(PokerLobby.self, object(lobbyJSON)))
        XCTAssertEqual(lobby.actor, "alice")
        XCTAssertEqual(lobby.wallet.chips, 8750)
        XCTAssertEqual(lobby.wallet.onTables, 1000)
        XCTAssertFalse(lobby.wallet.canClaim)
        XCTAssertEqual(lobby.wallet.nextClaimAt, 1_790_000_000_000)
        XCTAssertEqual(lobby.wallet.bestHand, "Flush, Ace high")
        XCTAssertEqual(lobby.stakes.map(\.id), ["starter"])
        XCTAssertEqual(lobby.stakes.first?.blindsLine, "5 / 10")
        XCTAssertEqual(lobby.tables.count, 1)
        let summary = try XCTUnwrap(lobby.tables.first)
        XCTAssertEqual(summary.code, "QXRTMB")
        XCTAssertFalse(summary.isPrivate)
        XCTAssertTrue(summary.mine)
        XCTAssertEqual(summary.seatsLine, "3 of 6 seated")
        XCTAssertEqual(lobby.myTables.count, 1)
        var owned = lobby
        owned.tables[0].mine = false
        XCTAssertEqual(owned.myTables.count, 1, "an owned table is yours even when you are not seated")
        owned.actor = "bob"
        XCTAssertEqual(owned.openTables.count, 1)
        XCTAssertEqual(lobby.now, 1_790_000_000_000)
    }

    func testLobbyToleratesMissingFields() throws {
        let lobby = try XCTUnwrap(PokerJSON.decode(PokerLobby.self, object(#"{"ok": true, "wallet": {"chips": "12"}}"#)))
        XCTAssertEqual(lobby.wallet.chips, 12)
        XCTAssertEqual(lobby.wallet.claimAmount, 2000)
        XCTAssertEqual(lobby.stakes, PokerStake.defaults)
        XCTAssertEqual(lobby.tables, [])
    }

    func testTableDecodesTheDocSample() throws {
        let raw = try XCTUnwrap(PokerJSON.decode(PokerTable.self, object(tableJSON(id: "<32 hex>"))))
        XCTAssertEqual(raw.name, "Friday night")
        XCTAssertNil(PokerTable.from(object(tableJSON(id: "<32 hex>"))))
        let t = table()
        XCTAssertEqual(t.seatCount, 6)
        XCTAssertEqual(t.seats.count, 6)
        XCTAssertTrue(t.seats[1].empty)
        XCTAssertTrue(t.seats[3].empty)
        XCTAssertEqual(t.seats[0].cards, ["As", "Qs"])
        XCTAssertEqual(t.seats[0].lastAction, PokerLastAction(kind: "call", amount: 40))
        XCTAssertEqual(t.seats[2].displayName, "River Rita (bot)")
        XCTAssertEqual(t.seats[0].displayName, "alice")
        XCTAssertTrue(t.seats[2].parsedCards.allSatisfy(\.faceDown))
        XCTAssertEqual(t.phase, "flop")
        XCTAssertEqual(t.boardCards.map(\.label), ["A♥", "K♦", "7♣"])
        XCTAssertEqual(t.pots, [PokerPot(amount: 300, eligible: [0, 2, 4])])
        XCTAssertEqual(t.potTotal, 380)
        XCTAssertEqual(t.betsOnFelt, 40)
        XCTAssertEqual(t.legal, PokerLegal(fold: true, check: false, call: 40, raise: PokerRaiseRange(min: 80, max: 960), allIn: 960))
        XCTAssertTrue(t.myTurn)
        XCTAssertTrue(t.seated)
        XCTAssertEqual(t.me?.name, "alice")
        XCTAssertEqual(t.myHand?.name, "Pair of Aces")
        XCTAssertEqual(t.events, [PokerEvent(seq: 801, kind: "flop")])
        XCTAssertEqual(t.log.first?.text, "River Rita bets 40")
        XCTAssertEqual(t.chat.first?.text, "nice hand")
        XCTAssertEqual(t.lastHand?.winners.first?.best.count, 5)
        XCTAssertEqual(t.lastHand?.shown.first?.handName, "Pair of Tens")
        XCTAssertEqual(t.lastHand?.reveal?.cards.map(\.index), [1, 4])
        XCTAssertEqual(t.lastHand?.reveal?.cards.last?.salt, "00112233445566ff")
        XCTAssertEqual(t.lastHand?.verified, false)
        XCTAssertEqual(t.history.first?.winners, ["river-rita"])
        XCTAssertEqual(t.wakeAt, 1_790_000_020_000)
        XCTAssertEqual(t.openSeats, [1, 3, 4, 5])
    }

    func testTableToleratesNullsAndWatchers() throws {
        let json = #"{"id": "\#(tableId)", "seatCount": 2, "mySeat": null, "legal": null, "lastHand": null, "myHand": null, "seats": [{"index": 1, "name": "bob", "stack": 5}, 7, {"index": 9, "name": "x"}]}"#
        let t = try XCTUnwrap(PokerTable.from(object(json)))
        XCTAssertEqual(t.mySeat, -1)
        XCTAssertFalse(t.seated)
        XCTAssertFalse(t.myTurn)
        XCTAssertNil(t.legal)
        XCTAssertNil(t.lastHand)
        XCTAssertEqual(t.phase, "waiting")
        XCTAssertEqual(t.seats.count, 2)
        XCTAssertTrue(t.seats[0].empty)
        XCTAssertFalse(t.seats[1].empty)
        XCTAssertEqual(t.seats[1].status, "out")
    }

    func testProvablyFairReveal() throws {
        let deck = (0..<52).map { PokerCard.ranks[$0 % 13].description + ["c", "d", "h", "s"][$0 / 13] }
        let salts = (0..<52).map { String(format: "%016x", $0 * 7919 + 13) }
        let hashes = (0..<52).map { PokerLastHand.cardHash(salt: salts[$0], index: $0, card: deck[$0]) }
        XCTAssertEqual(hashes[0], PokerLastHand.cardHash(salt: salts[0], index: 0, card: "2c"))
        XCTAssertEqual(hashes[0].count, 64)
        let worker = (0..<2).map { PokerLastHand.cardHash(salt: "00ff", index: $0, card: "Ah") }
        XCTAssertEqual(worker[0], "b3989d8187e49d69f4c5b9912d672c223e404cfa4f6feb030b15e79770af4c2b")
        XCTAssertEqual(PokerLastHand.commitment(hashes: worker), "5d9080c900c6f18296ad8b1850aa22c5f96e160ffac72774a0cb03fcef4d826c")
        let commitment = PokerLastHand.commitment(hashes: hashes)
        func hand(_ commitment: String, _ hashes: [String], _ shown: [(Int, String, String)]) throws -> PokerLastHand {
            let cards = shown.map { #"{"index": \#($0.0), "card": "\#($0.1)", "salt": "\#($0.2)"}"# }.joined(separator: ",")
            let list = hashes.map { "\"\($0)\"" }.joined(separator: ",")
            let json = #"{"number": 3, "commitment": "\#(commitment)", "reveal": {"hashes": [\#(list)], "cards": [\#(cards)]}}"#
            return try XCTUnwrap(PokerJSON.decode(PokerLastHand.self, object(json)))
        }
        let good = try hand(commitment.uppercased(), hashes, [(5, deck[5], salts[5]), (40, deck[40], salts[40])])
        XCTAssertEqual(good.reveal?.cards.map(\.index), [5, 40])
        XCTAssertEqual(good.verified, true)
        XCTAssertEqual(try hand(commitment, hashes, []).verified, true, "nothing shown still checks the commitment")
        XCTAssertEqual(try hand(commitment, hashes, [(5, deck[6], salts[5])]).verified, false, "a card that was not at that position")
        XCTAssertEqual(try hand(commitment, hashes, [(5, deck[5], salts[6])]).verified, false, "a wrong salt")
        XCTAssertEqual(try hand(commitment, hashes, [(60, deck[5], salts[5])]).verified, false, "an index outside the deck")
        var swapped = hashes
        swapped.swapAt(0, 1)
        XCTAssertEqual(try hand(commitment, swapped, []).verified, false, "hashes that do not match the commitment")
        XCTAssertEqual(try hand(PokerLastHand.commitment(hashes: Array(hashes.prefix(51))), Array(hashes.prefix(51)), []).verified, false)
        XCTAssertNil(try XCTUnwrap(PokerJSON.decode(PokerLastHand.self, object(#"{"number": 3}"#))).verified)
    }

    func testSeatDisplayNameFromTheServer() throws {
        let json = #"{"id": "\#(tableId)", "seatCount": 1, "seats": [{"index": 0, "name": "river-rita", "bot": true, "displayName": "Rita R. (bot)"}]}"#
        let t = try XCTUnwrap(PokerTable.from(object(json)))
        XCTAssertEqual(t.seats[0].displayName, "Rita R. (bot)")
        XCTAssertEqual(t.seats[0].name, "river-rita")
    }

    func testCards() throws {
        let ace = try XCTUnwrap(PokerCard(code: "Ah"))
        XCTAssertEqual(ace.rankLabel, "A")
        XCTAssertEqual(ace.suit, .hearts)
        XCTAssertTrue(ace.isRed)
        XCTAssertEqual(ace.suit?.colorHex, "#c62828")
        let ten = try XCTUnwrap(PokerCard(code: "ts"))
        XCTAssertEqual(ten.code, "Ts")
        XCTAssertEqual(ten.label, "10♠")
        XCTAssertFalse(ten.isRed)
        XCTAssertEqual(ten.suit?.colorHex, "#1a1a1a")
        XCTAssertEqual(ten.spokenName, "Ten of Spades")
        XCTAssertTrue(try XCTUnwrap(PokerCard(code: "??")).faceDown)
        XCTAssertNil(PokerCard(code: "1h"))
        XCTAssertNil(PokerCard(code: "Ax"))
        XCTAssertNil(PokerCard(code: "Ahh"))
        XCTAssertEqual(PokerCard.parse(["2c", "bad", "Qd"]).map(\.code), ["2c", "Qd"])
    }

    func testSeatRotationPutsMeAtTheBottom() {
        XCTAssertEqual(PokerLayout.slots(seatCount: 6, mySeat: 4), [4, 5, 0, 1, 2, 3])
        XCTAssertEqual(PokerLayout.slots(seatCount: 6, mySeat: -1), [0, 1, 2, 3, 4, 5])
        XCTAssertEqual(PokerLayout.slot(of: 1, seatCount: 6, mySeat: 4), 3)
        XCTAssertEqual(PokerLayout.slot(of: 4, seatCount: 6, mySeat: 4), 0)
        let center = PokerPoint(x: 100, y: 100)
        for count in 2...9 {
            let positions = PokerLayout.positions(seatCount: count, mySeat: count - 1, center: center, radiusX: 80, radiusY: 50)
            XCTAssertEqual(positions.count, count)
            let me = positions[count - 1]!
            XCTAssertEqual(me.x, 100, accuracy: 1e-9)
            XCTAssertEqual(me.y, 150, accuracy: 1e-9)
            for slot in 0..<count {
                let angle = PokerLayout.angle(slot: slot, count: count)
                let fromTop = abs(angle - 3 * Double.pi / 2)
                XCTAssertGreaterThanOrEqual(fromTop, PokerLayout.topGap / 2 - 1e-6, "\(count) seats, slot \(slot)")
            }
        }
        let next = PokerLayout.positions(seatCount: 6, mySeat: 0, center: center, radiusX: 80, radiusY: 50)[1]!
        XCTAssertLessThan(next.x, 100)
        XCTAssertGreaterThan(next.y, 100)
        let odd = PokerLayout.positions(seatCount: 5, mySeat: 0, center: center, radiusX: 80, radiusY: 50)
        XCTAssertEqual(odd[1]!.x - 100, 100 - odd[4]!.x, accuracy: 1e-9)
        XCTAssertEqual(odd[1]!.y, odd[4]!.y, accuracy: 1e-9)
        XCTAssertEqual(odd[2]!.y, odd[3]!.y, accuracy: 1e-9)
    }

    func testChipBreakdown() {
        XCTAssertEqual(
            PokerChips.breakdown(6631).map { [$0.denomination, $0.count] },
            [[5000, 1], [1000, 1], [500, 1], [100, 1], [25, 1], [5, 1], [1, 1]])
        XCTAssertEqual(PokerChips.breakdown(960).map { [$0.denomination, $0.count] }, [[500, 1], [100, 4], [25, 2], [5, 2]])
        XCTAssertEqual(PokerChips.breakdown(0), [])
        XCTAssertEqual(PokerChips.breakdown(-5), [])
        XCTAssertEqual(PokerChips.stack(130, limit: 3), [100, 25, 5])
        XCTAssertEqual(PokerChips.stack(40), [25, 5, 5, 5])
        XCTAssertEqual(PokerChips.columns(12000, maxColumns: 2, perColumn: 1), [[5000], [1000]])
        XCTAssertEqual(PokerChips.colorHex(25), "#2e7d32")
        XCTAssertEqual(PokerChips.colorHex(5000), "#ef6c00")
        XCTAssertEqual(PokerChips.colorHex(1), "#f5f5f5")
    }

    func testActionLabelsAndRaisePresets() {
        let t = table()
        let legal = t.legal!
        XCTAssertEqual(PokerActions.callLabel(legal), "Call 40")
        XCTAssertEqual(PokerActions.callKind(legal), .call)
        XCTAssertEqual(PokerActions.raiseKind(t), .raise)
        XCTAssertEqual(PokerActions.raiseLabel(t, amount: 80), "Raise to 80")
        XCTAssertEqual(PokerActions.allInLabel(legal), "All-in 960")
        XCTAssertEqual(PokerActions.presets(t).map(\.label), ["½ Pot", "¾ Pot", "Pot", "All-in"])
        XCTAssertEqual(PokerActions.presets(t).map(\.amount), [250, 355, 460, 960])
        XCTAssertEqual(PokerActions.clamp(10, legal), 80)
        XCTAssertEqual(PokerActions.clamp(5000, legal), 960)
        XCTAssertEqual(PokerActions.defaultRaise(t), 80)
        var check = t
        check.currentBet = 0
        check.potTotal = 20
        check.legal = PokerLegal(fold: true, check: true, call: 0, raise: PokerRaiseRange(min: 10, max: 300), allIn: 300)
        XCTAssertEqual(PokerActions.callLabel(check.legal!), "Check")
        XCTAssertEqual(PokerActions.raiseKind(check), .bet)
        XCTAssertEqual(PokerActions.raiseLabel(check, amount: 20), "Bet 20")
        XCTAssertEqual(PokerActions.presets(check).map(\.amount), [10, 15, 20, 300])
        var short = t
        short.legal = PokerLegal(fold: true, check: false, call: 40, raise: nil, allIn: 30)
        XCTAssertEqual(PokerActions.presets(short), [])
        XCTAssertFalse(PokerActions.canRaise(short.legal!))
    }

    func testEventCursorPlaysOnlyNewerEvents() {
        var cursor = PokerEventCursor()
        let first = table()
        XCTAssertEqual(cursor.take(first), [], "first load plays nothing")
        XCTAssertEqual(cursor.seen, 801)
        let events = #"[{"seq": 803, "kind": "call", "seat": 2, "amount": 40}, {"seq": 801, "kind": "flop"}, {"seq": 802, "kind": "bet", "seat": 0, "amount": 40}]"#
        let next = table(tableJSON(version: 212, events: events))
        XCTAssertEqual(cursor.take(next).map(\.seq), [802, 803])
        XCTAssertEqual(cursor.take(next), [])
        let other = table(tableJSON(id: "ffffffffffffffffffffffffffffffff", events: events))
        XCTAssertEqual(cursor.take(other), [], "another table starts fresh")
        let reset = table(tableJSON(id: "ffffffffffffffffffffffffffffffff", events: #"[{"seq": 3, "kind": "deal"}]"#))
        XCTAssertEqual(cursor.take(reset), [])
        XCTAssertEqual(cursor.seen, 3)
    }

    func testCuePlan() {
        let plan = PokerCues.plan([
            PokerEvent(seq: 1, kind: "shuffle"), PokerEvent(seq: 2, kind: "flop"), PokerEvent(seq: 3, kind: "chat"),
            PokerEvent(seq: 4, kind: "win", seat: 2, amount: 600),
        ])
        XCTAssertEqual(plan.map(\.cue), [.shuffle, .deal, .deal, .deal, .win])
        XCTAssertEqual(plan.map(\.delayMs), [0, 420, 515, 610, 705])
        XCTAssertEqual(PokerCues.plan([PokerEvent(seq: 1, kind: "deal", amount: 3)]).count, 6)
        XCTAssertEqual(PokerCues.plan([PokerEvent(seq: 1, kind: "deal", amount: 9)]).count, 8)
        XCTAssertEqual(PokerCues.plan((1...20).map { PokerEvent(seq: $0, kind: "check") }, maxEvents: 3).count, 3)
    }

    func testClock() {
        XCTAssertEqual(PokerClock.tone(remainingMs: 12000), .green)
        XCTAssertEqual(PokerClock.tone(remainingMs: 9999), .amber)
        XCTAssertEqual(PokerClock.tone(remainingMs: 4999), .red)
        XCTAssertEqual(PokerClock.fraction(remainingMs: 15000, totalMs: 30000), 0.5)
        XCTAssertEqual(PokerClock.wakeDelayMs(wakeAt: 0, serverNow: 100), nil)
        XCTAssertEqual(PokerClock.wakeDelayMs(wakeAt: 2000, serverNow: 1000), 1150)
        XCTAssertEqual(PokerClock.wakeDelayMs(wakeAt: 500, serverNow: 1000), 0)
        XCTAssertEqual(PokerClock.tickTimes(deadline: 30000, serverNow: 27500), [28000, 29000])
        XCTAssertEqual(PokerClock.tickTimes(deadline: 30000, serverNow: 1000).count, 5)
        var clock = PokerServerClock()
        clock.sync(serverNow: 10_000, localNow: 7_000)
        XCTAssertEqual(clock.serverNow(8_000), 11_000)
        XCTAssertEqual(clock.localTime(11_000), 8_000)
    }

    func testText() {
        XCTAssertEqual(PokerText.chips(0), "0")
        XCTAssertEqual(PokerText.chips(999), "999")
        XCTAssertEqual(PokerText.chips(1000), "1,000")
        XCTAssertEqual(PokerText.chips(1_234_567), "1,234,567")
        XCTAssertEqual(PokerText.chips(-2500), "-2,500")
        XCTAssertEqual(PokerText.short(150_000), "150K")
        XCTAssertEqual(PokerText.short(1_250_000), "1.3M")
        XCTAssertEqual(PokerText.displayName("river-rita", bot: true), "River Rita (bot)")
        XCTAssertEqual(PokerText.displayName("Ace (bot)", bot: true), "Ace (bot)")
        XCTAssertEqual(PokerText.displayName("alice", bot: false), "alice")
        XCTAssertEqual(PokerText.initial("alice"), "A")
        XCTAssertEqual(PokerText.actionTag("raise", amount: 80), "Raise 80")
        XCTAssertEqual(PokerText.actionTag("check", amount: 0), "Check")
        XCTAssertEqual(PokerText.actionTag("all in", amount: 960), "All-in 960")
        XCTAssertEqual(PokerText.actionTag("big blind", amount: 10), "BB 10")
        XCTAssertEqual(PokerText.countdown(3_599_001), "1:00:00")
        XCTAssertEqual(PokerText.countdown(65_000), "1:05")
        XCTAssertEqual(PokerText.error("seat_taken"), "Someone just took that seat. Pick another one.")
        XCTAssertEqual(PokerText.error("weird_code"), "Weird code.")
        XCTAssertEqual(PokerIds.normalizedCode("qxr-tmb"), "QXRTMB")
        XCTAssertNil(PokerIds.normalizedCode("QXRTM1"))
    }

    func testBuyInAndDraft() {
        XCTAssertEqual(PokerBuyIn.range(min: 200, max: 1000, wallet: 8750), 200...1000)
        XCTAssertEqual(PokerBuyIn.range(min: 200, max: 1000, wallet: 500), 200...500)
        XCTAssertNil(PokerBuyIn.range(min: 200, max: 1000, wallet: 100))
        XCTAssertEqual(PokerBuyIn.suggested(big: 10, range: 200...1000), 1000)
        XCTAssertEqual(PokerBuyIn.suggested(big: 1, range: 200...1000), 200)
        var mine = table()
        mine.seats[0].stack = 300
        XCTAssertEqual(PokerBuyIn.rebuyRange(mine), 10...700)
        var draft = PokerTableDraft()
        draft.name = "  Friday night  "
        draft.seats = 12
        draft.bots = 20
        draft.buyIn = 50_000
        draft.turnSeconds = 7
        draft.fit(PokerStake.defaults[0], wallet: 600)
        XCTAssertEqual(draft.seats, 9)
        XCTAssertEqual(draft.bots, 8)
        XCTAssertEqual(draft.buyIn, 600)
        XCTAssertEqual(draft.turnSeconds, 30)
        XCTAssertEqual(draft.body.str("name"), "Friday night")
        XCTAssertEqual(draft.body.bool("private"), false)
        XCTAssertEqual(PokerModel.inviteNames("@Bob, carol bob\n dave"), ["bob", "carol", "dave"])
        XCTAssertEqual(PokerModel.inviteSummary(invited: ["bob"], skipped: ["zed"]), "Invited bob. Skipped zed.")
    }

    func testSynthesizedSoundsAreBoundedAndTimed() {
        for cue in PokerCue.allCases {
            let samples = PokerSynth.render(cue, rate: 22050)
            XCTAssertFalse(samples.isEmpty, "\(cue)")
            let peak = samples.reduce(Float(0)) { max($0, abs($1)) }
            XCTAssertGreaterThan(peak, 0.1, "\(cue)")
            XCTAssertLessThanOrEqual(peak, 1, "\(cue)")
            XCTAssertTrue(samples.allSatisfy(\.isFinite), "\(cue)")
        }
        XCTAssertEqual(PokerSynth.render(.deal, rate: 20000).count, 900)
        XCTAssertEqual(PokerSynth.render(.fold, rate: 20000).count, 3600)
        XCTAssertEqual(PokerSynth.render(.clock, rate: 20000).count, 400)
        XCTAssertEqual(PokerSynth.render(.yourTurn, rate: 20000).count, 5600)
        XCTAssertEqual(PokerSynth.render(.chip, rate: 22050), PokerSynth.render(.chip, rate: 22050))
    }

    func testPokerRoutesAndGate() {
        let poker = MarketPackage(id: "poker", name: "Poker", webRoute: "/dashboard/poker")
        XCTAssertEqual(poker.phoneSurface, .native("poker"))
        XCTAssertEqual(PackageRoutes.route(for: poker), .poker(table: nil))
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "poker"), "poker")
        XCTAssertEqual(PackageGate.packageId(for: Route.poker(table: tableId)), "poker")
        XCTAssertEqual(PackageGate.packageId(forItem: "poker"), "poker")
        XCTAssertFalse(PackageGate.shows(Route.poker(table: nil), installed: ["water"]))
        XCTAssertTrue(PackageGate.shows(Route.poker(table: nil), installed: ["poker"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .poker(table: nil) })
        XCTAssertEqual(RouteCatalog.title(.poker), "Poker")
        XCTAssertEqual(PluginMenu.packageId(for: .poker(table: nil)), "poker")
    }
}

@MainActor
final class PokerModelTests: XCTestCase {
    private func wait(_ condition: () -> Bool) async {
        for _ in 0..<400 {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 2_000_000)
        }
    }

    private func model(_ fake: FakePokerApi, sleeps: SleepLog = SleepLog(), now: Int64 = 1_789_999_990_000) -> PokerModel {
        PokerModel(
            api: PokerAPI(call: { method, path, body in try await fake.call(method, path, body) }),
            localNow: { now },
            sleeper: { (ms: Int64) in await sleeps.record(ms) })
    }

    func testLobbyLoadAndClaim() async {
        let fake = FakePokerApi()
        fake.reply("GET ", object(lobbyJSON))
        fake.reply("POST /claim", object(#"{"ok": true, "wallet": {"chips": 10750, "canClaim": false, "nextClaimAt": 1790014400000}}"#))
        let sleeps = SleepLog()
        let poker = model(fake, sleeps: sleeps)
        await poker.loadLobby()
        XCTAssertEqual(poker.actor, "alice")
        XCTAssertEqual(poker.wallet.chips, 8750)
        XCTAssertEqual(poker.claimRemainingMs, 0)
        await poker.claim()
        XCTAssertTrue(poker.notice.contains("2,000"))
        XCTAssertEqual(fake.count("POST /claim"), 1)
    }

    func testNotInstalledShowsInstall() async {
        let fake = FakePokerApi()
        fake.errors["GET "] = "plugin_not_installed_for_account"
        let poker = model(fake)
        await poker.loadLobby()
        XCTAssertTrue(poker.notInstalled)
        XCTAssertEqual(poker.lobbyError, "Install Poker from the Market first.")
    }

    func testEventsPlayOnlyAfterFirstLoadAndTheGameClockTicksOnce() async {
        let fake = FakePokerApi()
        fake.reply("GET /tables/\(tableId)", object(#"{"ok": true, "table": \#(tableJSON())}"#))
        fake.reply(
            "POST /tables/\(tableId)/tick",
            object(#"{"ok": true, "table": \#(tableJSON(version: 212, events: #"[{"seq": 801, "kind": "flop"}, {"seq": 802, "kind": "timeout", "seat": 0}, {"seq": 803, "kind": "fold", "seat": 0}]"#, wakeAt: 0))}"#))
        let sleeps = SleepLog()
        let poker = model(fake, sleeps: sleeps)
        var played: [[Int]] = []
        var turns = 0
        poker.onEvents = { events, _ in played.append(events.map(\.seq)) }
        poker.onMyTurn = { turns += 1 }
        await poker.openTable(tableId)
        XCTAssertEqual(poker.table?.version, 211)
        XCTAssertEqual(played, [])
        XCTAssertEqual(turns, 0)
        XCTAssertEqual(poker.raiseTo, 80)
        await wait { fake.count("POST /tables/\(tableId)/tick") == 1 && poker.table?.version == 212 }
        XCTAssertEqual(fake.count("POST /tables/\(tableId)/tick"), 1)
        XCTAssertEqual(played, [[802, 803]])
        let delays = await sleeps.values
        XCTAssertTrue(delays.contains(1_790_000_020_000 + 150 - 1_790_000_000_000), "\(delays)")
        await poker.reloadTable()
        try? await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(fake.count("POST /tables/\(tableId)/tick"), 1, "no wake scheduled once wakeAt is 0")
        poker.closeTable()
        XCTAssertNil(poker.table)
    }

    func testActSendsTheHandAndRaiseAmount() async {
        let fake = FakePokerApi()
        let quiet = tableJSON(wakeAt: 0)
        fake.reply("GET /tables/\(tableId)", object(#"{"ok": true, "table": \#(quiet)}"#))
        fake.reply("POST /tables/\(tableId)/act", object(#"{"ok": true, "table": \#(quiet)}"#))
        let poker = model(fake)
        await poker.openTable(tableId)
        poker.raiseTo = 355
        await poker.raise()
        let act = fake.calls.last { $0.path.hasSuffix("/act") }
        XCTAssertEqual(act?.body?.str("action"), "raise")
        XCTAssertEqual(act?.body?.int("amount"), 355)
        XCTAssertEqual(act?.body?.int("hand"), 17)
        await poker.checkOrCall()
        XCTAssertEqual(fake.calls.last?.body?.str("action"), "call")
        XCTAssertNil(fake.calls.last?.body?["amount"])
        fake.errors["POST /tables/\(tableId)/act"] = "hand_changed"
        await poker.fold()
        XCTAssertEqual(poker.notice, "The hand moved on before your action arrived.")
        await wait { fake.count("GET /tables/\(tableId)") >= 2 }
        XCTAssertGreaterThanOrEqual(fake.count("GET /tables/\(tableId)"), 2)
    }

    func testQuickSitTakesTheFirstOpenSeat() async {
        let fake = FakePokerApi()
        let watching = tableJSON(wakeAt: 0).replacingOccurrences(of: #""mySeat": 0"#, with: #""mySeat": -1"#)
        fake.reply("GET /tables/\(tableId)", object(#"{"ok": true, "table": \#(watching)}"#))
        fake.reply("POST /tables/\(tableId)/sit", object(#"{"ok": true, "table": \#(tableJSON(version: 212, wakeAt: 0))}"#))
        let poker = model(fake)
        let seated = await poker.quickSit(tableId, buyIn: 500)
        XCTAssertTrue(seated)
        let sit = fake.calls.last { $0.path.hasSuffix("/sit") }
        XCTAssertEqual(sit?.body?.int("seat"), 1)
        XCTAssertEqual(sit?.body?.int("buyIn"), 500)
        XCTAssertTrue(poker.table?.seated == true)
    }

    func testCreateAndJoinByCode() async {
        let fake = FakePokerApi()
        let quiet = tableJSON(wakeAt: 0)
        fake.reply("POST /tables", object(#"{"ok": true, "table": \#(quiet)}"#))
        fake.reply("GET /codes/QXRTMB", object(#"{"ok": true, "tableId": "\#(tableId)"}"#))
        fake.reply("GET /tables/\(tableId)", object(#"{"ok": true, "table": \#(quiet)}"#))
        let poker = model(fake)
        var draft = PokerTableDraft()
        draft.name = "Friday night"
        draft.buyIn = 1000
        let created = await poker.createTable(draft)
        XCTAssertTrue(created)
        XCTAssertEqual(poker.tableId, tableId)
        XCTAssertEqual(fake.calls.first?.body?.int("seats"), 6)
        poker.closeTable()
        let joined = await poker.join(code: "qxrtmb")
        XCTAssertTrue(joined)
        XCTAssertEqual(poker.table?.code, "QXRTMB")
        let bad = await poker.join(code: "nope")
        XCTAssertFalse(bad)
        XCTAssertEqual(poker.notice, "Table codes are six letters.")
    }
}

actor SleepLog {
    private(set) var values: [Int64] = []

    func record(_ ms: Int64) {
        values.append(ms)
    }
}
