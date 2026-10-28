package com.forkmesh.nativeapp.poker

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.market.PackagesTransport
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.security.MessageDigest

class PokerTest {
    private val tableId = "0123456789abcdef0123456789abcdef"

    private val lobbyJson = """
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
              "id": "$tableId", "name": "Friday night", "code": "QXRTMB", "private": false,
              "owner": "alice", "stakes": "starter", "small": 5, "big": 10,
              "minBuyIn": 200, "maxBuyIn": 1000, "seats": 6, "players": 3, "humans": 2,
              "bots": 1, "handNumber": 17, "turnSeconds": 30, "mine": true,
              "invited": false, "updatedAt": 1790000000000
            }
          ],
          "now": 1790000000000
        }
    """.trimIndent()

    private fun tableJson(version: Long = 211, events: String = """[{"seq": 801, "kind": "flop", "seat": -1, "amount": 0}]""", toAct: Int = 0, started: Long = 1789999990000) = """
        {
          "id": "$tableId", "name": "Friday night", "code": "QXRTMB", "private": false,
          "owner": "alice", "ownerIsMe": true, "stakes": "starter", "small": 5, "big": 10,
          "minBuyIn": 200, "maxBuyIn": 1000, "seatCount": 6, "turnSeconds": 30,
          "version": $version, "now": 1790000000000, "handNumber": 17,
          "phase": "flop",
          "board": ["Ah", "Kd", "7c"],
          "pots": [{"amount": 300, "eligible": [0, 2, 4]}],
          "potTotal": 380,
          "dealer": 2, "smallBlind": 4, "bigBlind": 0,
          "toAct": $toAct, "actStartedAt": $started, "actDeadline": 1790000020000,
          "currentBet": 40, "minRaiseTo": 80,
          "wakeAt": 1790000020000,
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
          "commitment": "${"c".repeat(64)}",
          "events": $events,
          "log": [{"at": 1789999990000, "seat": 2, "name": "river-rita", "kind": "bet", "amount": 40, "text": "River Rita bets 40"}],
          "chat": [{"at": 1789999980000, "name": "bob", "text": "nice hand"}],
          "lastHand": {
            "number": 16, "board": ["2c", "5d", "9h", "Js", "Qh"],
            "winners": [{"seat": 2, "name": "river-rita", "amount": 600, "handName": "Pair of Queens",
                         "cards": ["Qd", "3c"], "best": ["Qd", "Qh", "Js", "9h", "5d"]}],
            "shown": [{"seat": 0, "name": "alice", "cards": ["Tc", "Td"], "handName": "Pair of Tens"}],
            "commitment": "${"d".repeat(64)}", "reveal": {"hashes": [${List(52) { "\"" + "A".repeat(64) + "\"" }.joinToString(",")}], "cards": [{"index": 9, "card": "Qh", "salt": "${"e".repeat(16)}"}, {"index": 3, "card": "2c", "salt": "${"f".repeat(16)}"}, {"index": -1, "card": "Ks"}]}
          },
          "history": [{"number": 16, "winners": ["river-rita"], "amount": 600, "handName": "Pair of Queens", "at": 1789999900000}]
        }
    """.trimIndent()

    private fun table(version: Long = 211, events: String = """[{"seq": 801, "kind": "flop", "seat": -1, "amount": 0}]""", toAct: Int = 0, started: Long = 1789999990000): PokerTable =
        PokerTable.fromJson(Json.parseObject(tableJson(version, events, toAct, started)))!!

    private class Call(val method: String, val path: String, val body: Map<String, Any?>?)

    private class Fake(val reply: (Call) -> ApiJsonResult) : PackagesTransport {
        val calls = ArrayList<Call>()
        override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
            val call = Call(method, path, body)
            calls += call
            return reply(call)
        }
    }

    fun testDecodesLobbyFixture() {
        val lobby = PokerLobby.fromJson(Json.parseObject(lobbyJson))
        assertEq("alice", lobby.actor)
        assertEq(8750L, lobby.wallet.chips)
        assertEq(1000L, lobby.wallet.onTables)
        assertEq(false, lobby.wallet.canClaim)
        assertEq(1790000000000L, lobby.wallet.nextClaimAt)
        assertEq(26, lobby.wallet.winRate)
        assertEq("Flush, Ace high", lobby.wallet.bestHand)
        assertEq(listOf(PokerStake("starter", "Starter", 5, 10, 200, 1000)), lobby.stakes)
        val summary = lobby.tables.single()
        assertEq(tableId, summary.id)
        assertEq("QXRTMB", summary.code)
        assertEq(6, summary.seats)
        assertEq(3, summary.players)
        assertEq(true, summary.mine)
        assertEq("5 / 10", summary.blinds)
        assertEq(listOf(summary), lobby.myTables)
        assertEq(emptyList<PokerTableSummary>(), lobby.openTables)
        assertEq(1790000000000L, lobby.now)
    }

    fun testDecodesTableFixture() {
        val t = table()
        assertEq(tableId, t.id)
        assertEq(true, t.ownerIsMe)
        assertEq(6, t.seatCount)
        assertEq(6, t.seats.size)
        assertEq((0..5).toList(), t.seats.map { it.index })
        assertEq(true, t.seats[1].empty)
        assertEq(true, t.seats[5].empty)
        assertEq("flop", t.phase)
        assertEq(listOf("Ah", "Kd", "7c"), t.board.map { it.code })
        assertEq(listOf(PokerPot(300, listOf(0, 2, 4))), t.pots)
        assertEq(380L, t.potTotal)
        assertEq(40L, t.betsOnStreet)
        assertEq(340L, t.pottedChips)
        assertEq(2, t.dealer)
        assertEq(0, t.mySeat)
        assertEq(true, t.seated)
        assertEq(true, t.myTurn)
        assertEq(listOf("As", "Qs"), t.me!!.cards.map { it.code })
        assertEq(listOf(true, true), t.seats[2].cards.map { it.faceDown })
        assertEq("river-rita (bot)", t.seats[2].displayName)
        assertEq("River Rita (bot)", PokerSeat.fromJson(mapOf("name" to "river-rita", "bot" to true, "displayName" to "River Rita (bot)"), 0).displayName)
        assertEq(PokerLastAction("bet", 40), t.seats[2].lastAction)
        assertEq("Call 40", t.seats[0].lastAction!!.label)
        assertEq(PokerLegal(true, false, 40, PokerRaise(80, 960), 960), t.legal)
        assertEq("Pair of Aces", t.myHand?.name)
        assertEq(5, t.myHand?.cards?.size)
        assertEq("Flop: A♥ K♦ 7♣", t.dealerLine)
        assertEq(listOf(PokerEvent(801, "flop", -1, 0)), t.events)
        assertEq("River Rita bets 40", t.log.single().text)
        assertEq(PokerChatLine(1789999980000, "bob", "nice hand"), t.chat.single())
        val last = t.lastHand!!
        assertEq(16L, last.number)
        assertEq("Pair of Queens", last.winners.single().handName)
        assertEq(5, last.winners.single().best.size)
        assertEq(listOf("Tc", "Td"), last.shown.single().cards.map { it.code })
        assertEq(true, last.revealed)
        assertEq(52, last.hashes.size)
        assertEq("a".repeat(64), last.hashes.first())
        assertEq(listOf(PokerRevealedCard(3, "2c", "f".repeat(16)), PokerRevealedCard(9, "Qh", "e".repeat(16))), last.cards)
        assertEq(false, PokerFair.verify(last))
        assertEq(listOf("river-rita"), t.history.single().winners)
    }

    fun testToleratesMissingFields() {
        val bare = PokerTable.fromJson(mapOf("id" to tableId))!!
        assertEq("waiting", bare.phase)
        assertEq(-1, bare.mySeat)
        assertEq(-1, bare.dealer)
        assertEq(false, bare.seated)
        assertEq(false, bare.myTurn)
        assertEq(null, bare.legal)
        assertEq(null, bare.lastHand)
        assertEq(null, bare.myHand)
        assertEq(emptyList<PokerSeat>(), bare.seats)
        assertEq(null, PokerTable.fromJson(mapOf("id" to "nope")))
        val sparse = PokerTable.fromJson(mapOf("id" to tableId, "seatCount" to 3, "mySeat" to null, "seats" to listOf(mapOf("index" to 1, "name" to "bob", "stack" to 5))))!!
        assertEq(3, sparse.seats.size)
        assertEq(false, sparse.seats[1].empty)
        assertEq("bob", sparse.seats[1].name)
        assertEq(true, sparse.seats[0].empty)
        assertEq(-1, sparse.mySeat)
        val noRaise = PokerLegal.fromJson(mapOf("check" to true, "raise" to null, "allIn" to 50))
        assertEq(null, noRaise.raise)
        assertEq(true, noRaise.check)
        val lobby = PokerLobby.fromJson(emptyMap())
        assertEq(PokerStake.DEFAULTS, lobby.stakes)
        assertEq(0L, lobby.wallet.chips)
        assertEq(2000L, lobby.wallet.claimAmount)
        assertEq(emptyList<PokerTableSummary>(), PokerLobby.fromJson(mapOf("tables" to listOf(mapOf("id" to "x")))).tables)
    }

    fun testParsesCards() {
        assertEq(PokerCard('T', 'd'), PokerCards.parse("Td"))
        assertEq("10", PokerCards.parse("Td")!!.rankLabel)
        assertEq(true, PokerCards.parse("Td")!!.red)
        assertEq(false, PokerCards.parse("As")!!.red)
        assertEq(PokerCard('A', 'h'), PokerCards.parse("ah"))
        assertEq(true, PokerCards.parse("??")!!.faceDown)
        assertEq(null, PokerCards.parse("Xx"))
        assertEq(null, PokerCards.parse("A"))
        assertEq(null, PokerCards.parse(null))
        assertEq("Q♠", PokerCards.parse("Qs")!!.label)
        assertEq("Ace of hearts", PokerCards.spoken(PokerCards.parse("Ah")!!))
        assertEq(listOf("2c", "Kh"), PokerCards.list(listOf("2c", "zz", "Kh")).map { it.code })
    }

    fun testRotatesTheUserToTheBottom() {
        assertEq(listOf(2, 3, 4, 5, 0, 1), PokerLayout.order(6, 2))
        assertEq(listOf(0, 1, 2), PokerLayout.order(3, -1))
        assertEq(listOf(0, 1, 2), PokerLayout.order(3, 7))
        assertEq(0, PokerLayout.slot(2, 6, 2))
        assertEq(4, PokerLayout.slot(0, 6, 2))
        assertEq(5, PokerLayout.slot(1, 6, 2))
        assertEq(emptyList<Int>(), PokerLayout.order(0, 0))
        for (n in 2..9) for (me in 0 until n) for (seat in 0 until n) {
            assertEq(seat, PokerLayout.order(n, me)[PokerLayout.slot(seat, n, me)])
        }
    }

    fun testSpreadsSeatsAroundTheOval() {
        val six = PokerLayout.positions(6)
        assertEq(PokerPoint(0.0, 1.0), six[0])
        assertEq(PokerPoint(0.0, -1.0), six[3])
        expect(six[1].x < 0 && six[1].y > 0, "the next seat sits to the bottom left")
        for (n in 2..9) {
            val points = PokerLayout.positions(n)
            assertEq(n, points.size)
            assertEq(PokerPoint(0.0, 1.0), points[0])
            for (k in 1 until n) {
                val a = points[k]
                val b = points[n - k]
                expect(kotlin.math.abs(a.x + b.x) < 1e-9 && kotlin.math.abs(a.y - b.y) < 1e-9, "seats mirror left and right for $n")
                expect(kotlin.math.abs(a.x * a.x + a.y * a.y - 1) < 1e-9)
            }
        }
        assertEq(PokerPoint(0.0, 0.5), PokerLayout.towardCentre(PokerPoint(0.0, 1.0), 0.5))
        val gap = Math.toRadians(70.0)
        for (n in 2..9) {
            val points = PokerLayout.positions(n, gap)
            assertEq(PokerPoint(0.0, 1.0), points[0])
            for (p in points) {
                val fromTop = Math.toDegrees(kotlin.math.acos((-p.y).coerceIn(-1.0, 1.0)))
                expect(fromTop >= 35.0 - 1e-6, "no seat in front of the dealer for $n")
            }
            if (n % 2 == 1) {
                for (k in 1 until n) expect(kotlin.math.abs(points[k].x + points[n - k].x) < 1e-9, "odd tables stay symmetric")
            }
        }
        val corner = PokerLayout.onRail(PokerPoint(Math.sqrt(0.5), Math.sqrt(0.5)))
        expect(corner.x > 0.8 && kotlin.math.abs(corner.x - corner.y) < 1e-9, "rail points reach into the corners")
        assertEq(PokerPoint(0.0, 1.0), PokerLayout.onRail(PokerPoint(0.0, 1.0)))
    }

    fun testBreaksChipsIntoDenominations() {
        assertEq(
            listOf(PokerChipStack(1000, 1), PokerChipStack(500, 1), PokerChipStack(100, 1), PokerChipStack(25, 1), PokerChipStack(5, 1)),
            PokerChips.breakdown(1630),
        )
        assertEq(listOf(PokerChipStack(5000, 2), PokerChipStack(25, 1), PokerChipStack(5, 1), PokerChipStack(1, 3)), PokerChips.breakdown(10033))
        assertEq(emptyList<PokerChipStack>(), PokerChips.breakdown(0))
        assertEq(emptyList<PokerChipStack>(), PokerChips.breakdown(-5))
        assertEq(listOf(5L, 25L, 100L, 500L, 1000L), PokerChips.visual(1630))
        assertEq(11, PokerChips.visual(99).size)
        assertEq(8, PokerChips.visual(99, 8).size)
        assertEq(25L, PokerChips.visual(99, 8).last())
        assertEq(1L, PokerChips.visual(99, 8).first())
        assertEq(0xFFC62828, PokerChips.color(5))
        assertEq(0xFFEF6C00, PokerChips.color(5000))
        assertEq("12,500", PokerChips.format(12500))
        assertEq("9,999", PokerChips.short(9999))
        assertEq("12.5K", PokerChips.short(12500))
        assertEq("100K", PokerChips.short(100000))
        assertEq("1.2M", PokerChips.short(1_200_000))
    }

    fun testRaisePresetsClampToTheLegalRange() {
        val t = table()
        assertEq(
            listOf(PokerPreset("½ pot", 250), PokerPreset("¾ pot", 355), PokerPreset("Pot", 460), PokerPreset("All-in", 960)),
            PokerActions.presets(t),
        )
        val tight = t.copy(legal = t.legal!!.copy(raise = PokerRaise(80, 100)))
        assertEq(listOf(100L, 100L, 100L, 100L), PokerActions.presets(tight).map { it.amount })
        val wide = t.copy(potTotal = 0, currentBet = 0, legal = PokerLegal(true, true, 0, PokerRaise(80, 960), 960))
        assertEq(listOf(80L, 80L, 80L, 960L), PokerActions.presets(wide).map { it.amount })
        assertEq(emptyList<PokerPreset>(), PokerActions.presets(t.copy(legal = null)))
        assertEq(80L, PokerActions.clamp(10, 80, 960))
        assertEq(960L, PokerActions.clamp(5000, 80, 960))
        assertEq(80L, PokerActions.clamp(5000, 80, 40))
    }

    fun testLegalActionLabels() {
        val t = table()
        assertEq(
            listOf(
                PokerButton(PokerActionKind.FOLD, "Fold"),
                PokerButton(PokerActionKind.CALL, "Call 40", 40),
                PokerButton(PokerActionKind.RAISE, "Raise to", 80),
                PokerButton(PokerActionKind.ALLIN, "All-in 960", 960),
            ),
            PokerActions.buttons(t),
        )
        val check = t.copy(currentBet = 0, legal = PokerLegal(true, true, 0, PokerRaise(10, 960), 960))
        assertEq(listOf("Check", "Bet", "All-in 960"), PokerActions.buttons(check).map { it.label })
        val short = t.copy(legal = PokerLegal(true, false, 960, null, 960))
        assertEq(listOf("Fold", "Call 960 (all-in)"), PokerActions.buttons(short).map { it.label })
        assertEq(emptyList<PokerButton>(), PokerActions.buttons(t.copy(legal = null)))
        assertEq("Raise to 200", PokerActions.raiseLabel(t, 200))
        assertEq("All-in 960", PokerActions.raiseLabel(t, 960))
        assertEq("Bet 200", PokerActions.raiseLabel(check, 200))
        assertEq(PokerActionKind.RAISE, PokerActions.wireFor(t, 200))
        assertEq(PokerActionKind.ALLIN, PokerActions.wireFor(t, 960))
        assertEq(PokerActionKind.BET, PokerActions.wireFor(check, 200))
    }

    fun testEventsPlayOnlyWhenNewerThanTheHighestSeen() {
        val tracker = PokerEventTracker()
        val first = table(events = """[{"seq": 799, "kind": "deal"}, {"seq": 801, "kind": "flop"}]""")
        assertEq(emptyList<PokerEvent>(), tracker.fresh(first))
        assertEq(801L, tracker.highest)
        assertEq(emptyList<PokerEvent>(), tracker.fresh(first))
        val next = table(events = """[{"seq": 803, "kind": "call", "seat": 2, "amount": 40}, {"seq": 801, "kind": "flop"}, {"seq": 802, "kind": "check", "seat": 0}]""")
        assertEq(listOf(802L, 803L), tracker.fresh(next).map { it.seq })
        assertEq(803L, tracker.highest)
        assertEq(emptyList<PokerEvent>(), tracker.fresh(first))
        assertEq(803L, tracker.highest)
        val other = table().copy(id = "f".repeat(32), events = listOf(PokerEvent(5, "shuffle")))
        assertEq(emptyList<PokerEvent>(), tracker.fresh(other))
        assertEq(5L, tracker.highest)
        assertEq(PokerSound.SHUFFLE, PokerEvents.sound("shuffle"))
        assertEq(PokerSound.CHIP, PokerEvents.sound("raise"))
        assertEq(PokerSound.WIN, PokerEvents.sound("win"))
        assertEq(null, PokerEvents.sound("join"))
    }

    fun testGameClockFiresOnceAfterWakeAt() {
        assertEq(5150L, PokerClock.wakeDelay(1_000_005_000, 1_000_000_000, 50, 50))
        assertEq(4150L, PokerClock.wakeDelay(1_000_005_000, 1_000_000_000, 50, 1050))
        assertEq(0L, PokerClock.wakeDelay(1_000_005_000, 1_000_000_000, 50, 99_999))
        assertEq(null, PokerClock.wakeDelay(0, 1_000_000_000, 50, 50))
        assertEq(3000L, PokerClock.remaining(1_000_003_000, 1_000_000_000, 0, 0))
        assertEq(0.5, PokerClock.fraction(0, 30_000, 15_000))
        assertEq(PokerClockTone.GREEN, PokerClock.tone(12_000))
        assertEq(PokerClockTone.AMBER, PokerClock.tone(9_999))
        assertEq(PokerClockTone.RED, PokerClock.tone(4_000))
        assertEq("3:59:01", PokerClock.countdown(14_340_100))
        assertEq("0:05", PokerClock.countdown(4_200))
    }

    fun testSessionDiffsEventsAndIgnoresStaleReplies() {
        var reply = tableJson()
        val fake = Fake { ApiJsonResult(200, mapOf("ok" to true, "table" to Json.parseObject(reply))) }
        var now = 10_000L
        val session = PokerSession(PokerApi(PackagesClient(fake)), clock = { now })
        val opened = session.open(tableId)
        assertEq(emptyList<PokerEvent>(), opened.events)
        assertEq(false, opened.turnStarted)
        assertEq(20_000L + PokerClock.WAKE_SLACK_MS, opened.wakeDelay)
        now = 11_000L
        reply = tableJson(version = 212, events = """[{"seq": 801, "kind": "flop"}, {"seq": 802, "kind": "call", "seat": 2, "amount": 40}]""", started = 1790000000500)
        val ticked = session.tick()
        assertEq(listOf(802L), ticked.events.map { it.seq })
        assertEq(true, ticked.turnStarted)
        assertEq("POST", fake.calls.last().method)
        assertEq("/api/poker/tables/$tableId/tick", fake.calls.last().path)
        reply = tableJson(version = 200, events = """[{"seq": 900, "kind": "win"}]""")
        val stale = session.refresh()!!
        assertEq(false, stale.adopted)
        assertEq(212L, session.table!!.version)
        reply = tableJson(version = 212, events = """[{"seq": 802, "kind": "call"}]""", started = 1790000000500)
        assertEq(false, session.refresh()!!.turnStarted)
        session.forget()
        assertEq(null, session.table)
        assertEq(null, session.wakeDelay())
    }

    fun testApiSendsTheDocumentedBodies() {
        val fake = Fake { call ->
            when {
                call.path.startsWith("/api/poker/codes/") -> ApiJsonResult(200, mapOf("ok" to true, "tableId" to tableId))
                call.method == "DELETE" -> ApiJsonResult(200, mapOf("ok" to true, "closed" to true))
                call.path.endsWith("/invite") -> ApiJsonResult(200, mapOf("ok" to true, "invited" to listOf("bob"), "skipped" to listOf("zed"), "table" to Json.parseObject(tableJson())))
                call.path.endsWith("/act") && call.body?.get("hand") != 17L -> ApiJsonResult(409, mapOf("ok" to false, "error" to "hand_changed"))
                else -> ApiJsonResult(200, mapOf("ok" to true, "table" to Json.parseObject(tableJson()), "wallet" to mapOf("chips" to 10750, "canClaim" to false)))
            }
        }
        val api = PokerApi(PackagesClient(fake))
        assertEq(tableId, api.code(" qx-rtmb "))
        assertEq("/api/poker/codes/QXRTMB", fake.calls.last().path)
        assertEq("invalid_code", assertFails<PackagesException> { api.code("abc") }.code)
        assertEq(10750L, api.claim().chips)
        api.act(tableId, "raise", 200, 17)
        assertEq(mapOf<String, Any?>("action" to "raise", "amount" to 200L, "hand" to 17L), fake.calls.last().body)
        assertEq("hand_changed", assertFails<PackagesException> { api.act(tableId, "call", 0, 16) }.code)
        assertFails<PackagesException> { api.act(tableId, "dance", 0, 17) }
        assertFails<PackagesException> { api.open("../x") }
        api.sit(tableId, 3, 500)
        assertEq(mapOf<String, Any?>("seat" to 3, "buyIn" to 500L), fake.calls.last().body)
        api.removeBot(tableId, 4)
        assertEq(mapOf("remove" to 4), fake.calls.last().body)
        val invite = api.invite(tableId, PokerApi.normalizeNames("@Bob, zed  bad name!"))
        assertEq(listOf("bob", "zed", "bad"), fake.calls.last().body!!["names"])
        assertEq(listOf("bob"), invite.invited)
        assertEq(listOf("zed"), invite.skipped)
        assertEq(true, api.close(tableId))
        assertEq("DELETE", fake.calls.last().method)
        assertFails<PackagesException> { api.chat(tableId, "   ") }
        val draft = PokerTableDraft(" Friday night ", "starter", 12, true, 20, 9, 500).body
        assertEq(mapOf<String, Any?>("name" to "Friday night", "stakes" to "starter", "seats" to 9, "private" to true, "turnSeconds" to 30, "bots" to 8, "buyIn" to 500L), draft)
    }

    private fun sha(text: String): String =
        MessageDigest.getInstance("SHA-256").digest(text.toByteArray()).joinToString("") { "%02x".format(it.toInt() and 0xff) }

    fun testProvablyFairRevealMatchesTheCommitment() {
        val deck = PokerCards.RANKS.flatMap { rank -> PokerCards.SUITS.map { suit -> "$rank$suit" } }.shuffled(java.util.Random(4))
        val salts = deck.indices.map { "%016x".format(it * 7919L + 12345L) }
        val hashes = deck.indices.map { sha("${salts[it]}:$it:${deck[it]}") }
        val commitment = sha(hashes.joinToString(",")).uppercase()
        val shown = listOf(0, 5, 17, 30, 51).map { PokerRevealedCard(it, deck[it], salts[it]) }
        val hand = PokerLastHand(16, commitment = commitment, hashes = hashes, cards = shown)
        assertEq(52, deck.toSet().size)
        assertEq(true, PokerFair.verify(hand))
        assertEq(true, PokerFair.verify(hand.copy(cards = emptyList())))
        assertEq(hashes[5], PokerFair.cardHash(salts[5], 5, deck[5]))
        assertEq(commitment.lowercase(), PokerFair.commitment(hashes))
        val swapped = shown.toMutableList().also { it[1] = PokerRevealedCard(5, deck[6], salts[5]) }
        assertEq(false, PokerFair.verify(hand.copy(cards = swapped)))
        assertEq(false, PokerFair.verify(hand.copy(cards = shown + PokerRevealedCard(52, deck[0], salts[0]))))
        assertEq(false, PokerFair.verify(hand.copy(hashes = hashes.drop(1), commitment = sha(hashes.drop(1).joinToString(",")))))
        assertEq(false, PokerFair.verify(hand.copy(commitment = sha("tampered"))))
        assertEq(null, PokerFair.verify(PokerLastHand(16, commitment = commitment)))
        assertEq(null, PokerFair.verify(hand.copy(commitment = "")))
        val parsed = PokerLastHand.fromJson(
            mapOf(
                "number" to 16,
                "commitment" to commitment,
                "reveal" to mapOf("hashes" to hashes, "cards" to shown.map { mapOf("index" to it.index, "card" to it.card, "salt" to it.salt) }),
            ),
        )!!
        assertEq(true, PokerFair.verify(parsed))
    }

    fun testSynthesizesEverySound() {
        val synth = PokerSynth()
        for (sound in PokerSound.entries) {
            val pcm = synth.render(sound)
            expect(pcm.isNotEmpty(), "$sound has samples")
            val peak = pcm.maxOf { kotlin.math.abs(it.toInt()) }
            expect(peak in 1000..32767, "$sound peak $peak")
        }
        val ms = { sound: PokerSound -> PokerSynth.durationMs(synth.render(sound).size, synth.rate) }
        expect(ms(PokerSound.SHUFFLE) in 300L..450L, "shuffle length")
        assertEq(45L, ms(PokerSound.DEAL))
        assertEq(20L, ms(PokerSound.CLOCK))
        expect(ms(PokerSound.TURN) in 280L..300L, "turn length")
        expect(ms(PokerSound.WIN) >= 440L, "win length")
    }
}
