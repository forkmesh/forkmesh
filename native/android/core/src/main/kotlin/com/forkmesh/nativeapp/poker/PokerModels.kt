package com.forkmesh.nativeapp.poker

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class PokerCard(val rank: Char, val suit: Char) {
    val faceDown: Boolean get() = rank == '?'
    val red: Boolean get() = suit == 'h' || suit == 'd'
    val code: String get() = if (faceDown) "??" else "$rank$suit"
    val rankLabel: String get() = if (rank == 'T') "10" else rank.toString()
    val suitSymbol: String get() = PokerCards.symbol(suit)
    val label: String get() = if (faceDown) "face down" else "$rankLabel$suitSymbol"

    companion object {
        val BACK = PokerCard('?', '?')

        fun parse(raw: String?): PokerCard? = PokerCards.parse(raw)
    }
}

object PokerCards {
    const val RANKS = "23456789TJQKA"
    const val SUITS = "cdhs"

    fun parse(raw: String?): PokerCard? {
        val text = raw?.trim() ?: return null
        if (text == "??") return PokerCard.BACK
        if (text.length != 2) return null
        val rank = text[0].uppercaseChar()
        val suit = text[1].lowercaseChar()
        if (rank !in RANKS || suit !in SUITS) return null
        return PokerCard(rank, suit)
    }

    fun list(values: List<String>): List<PokerCard> = values.mapNotNull(::parse)

    fun symbol(suit: Char): String = when (suit) {
        'c' -> "♣"
        'd' -> "♦"
        'h' -> "♥"
        's' -> "♠"
        else -> ""
    }

    fun rankName(rank: Char): String = when (rank) {
        'A' -> "Ace"
        'K' -> "King"
        'Q' -> "Queen"
        'J' -> "Jack"
        'T' -> "Ten"
        else -> rank.toString()
    }

    fun suitName(suit: Char): String = when (suit) {
        'c' -> "clubs"
        'd' -> "diamonds"
        'h' -> "hearts"
        's' -> "spades"
        else -> ""
    }

    fun spoken(card: PokerCard): String = if (card.faceDown) "face-down card" else "${rankName(card.rank)} of ${suitName(card.suit)}"
}

data class PokerWallet(
    val chips: Long = 0,
    val onTables: Long = 0,
    val canClaim: Boolean = false,
    val claimAmount: Long = 2000,
    val nextClaimAt: Long = 0,
    val handsPlayed: Long = 0,
    val handsWon: Long = 0,
    val biggestPot: Long = 0,
    val bestHand: String = "",
) {
    val winRate: Int get() = if (handsPlayed > 0) ((handsWon * 100) / handsPlayed).toInt() else 0

    companion object {
        fun fromJson(json: JsonMap) = PokerWallet(
            chips = json.long("chips"),
            onTables = json.long("onTables"),
            canClaim = json.bool("canClaim"),
            claimAmount = json.long("claimAmount", 2000),
            nextClaimAt = json.long("nextClaimAt"),
            handsPlayed = json.long("handsPlayed"),
            handsWon = json.long("handsWon"),
            biggestPot = json.long("biggestPot"),
            bestHand = json.str("bestHand"),
        )
    }
}

data class PokerStake(
    val id: String,
    val label: String = "",
    val small: Long = 0,
    val big: Long = 0,
    val minBuyIn: Long = 0,
    val maxBuyIn: Long = 0,
) {
    val blinds: String get() = "${PokerChips.format(small)} / ${PokerChips.format(big)}"

    companion object {
        val DEFAULTS = listOf(
            PokerStake("starter", "Starter", 5, 10, 200, 1000),
            PokerStake("low", "Low", 25, 50, 1000, 5000),
            PokerStake("mid", "Mid", 100, 200, 4000, 20000),
            PokerStake("high", "High", 500, 1000, 20000, 100000),
        )

        fun fromJson(json: JsonMap): PokerStake? {
            val id = json.str("id")
            if (id.isEmpty()) return null
            return PokerStake(
                id,
                json.str("label").ifEmpty { id.replaceFirstChar { it.uppercase() } },
                json.long("small"),
                json.long("big"),
                json.long("minBuyIn"),
                json.long("maxBuyIn"),
            )
        }
    }
}

data class PokerTableSummary(
    val id: String,
    val name: String = "",
    val code: String = "",
    val private: Boolean = false,
    val owner: String = "",
    val stakes: String = "",
    val small: Long = 0,
    val big: Long = 0,
    val minBuyIn: Long = 0,
    val maxBuyIn: Long = 0,
    val seats: Int = 0,
    val players: Int = 0,
    val humans: Int = 0,
    val bots: Int = 0,
    val handNumber: Long = 0,
    val turnSeconds: Int = 30,
    val mine: Boolean = false,
    val invited: Boolean = false,
    val updatedAt: Long = 0,
) {
    val blinds: String get() = "${PokerChips.format(small)} / ${PokerChips.format(big)}"
    val full: Boolean get() = seats in 1..players

    companion object {
        fun fromJson(json: JsonMap): PokerTableSummary? {
            val id = json.str("id")
            if (!PokerApi.isTableId(id)) return null
            return PokerTableSummary(
                id = id,
                name = json.str("name"),
                code = json.str("code"),
                private = json.bool("private"),
                owner = json.str("owner"),
                stakes = json.str("stakes"),
                small = json.long("small"),
                big = json.long("big"),
                minBuyIn = json.long("minBuyIn"),
                maxBuyIn = json.long("maxBuyIn"),
                seats = json.int("seats"),
                players = json.int("players"),
                humans = json.int("humans"),
                bots = json.int("bots"),
                handNumber = json.long("handNumber"),
                turnSeconds = json.int("turnSeconds", 30),
                mine = json.bool("mine"),
                invited = json.bool("invited"),
                updatedAt = json.long("updatedAt"),
            )
        }
    }
}

data class PokerLobby(
    val actor: String = "",
    val wallet: PokerWallet = PokerWallet(),
    val stakes: List<PokerStake> = PokerStake.DEFAULTS,
    val tables: List<PokerTableSummary> = emptyList(),
    val now: Long = 0,
) {
    val myTables: List<PokerTableSummary> get() = tables.filter { it.mine }
    val invitedTables: List<PokerTableSummary> get() = tables.filter { it.invited && !it.mine }
    val openTables: List<PokerTableSummary> get() = tables.filter { !it.mine && !it.invited }

    fun stake(id: String): PokerStake? = stakes.firstOrNull { it.id == id }

    companion object {
        fun fromJson(json: JsonMap): PokerLobby {
            val stakes = json.maps("stakes").mapNotNull(PokerStake::fromJson)
            return PokerLobby(
                actor = json.str("actor"),
                wallet = PokerWallet.fromJson(json.obj("wallet")),
                stakes = stakes.ifEmpty { PokerStake.DEFAULTS },
                tables = json.maps("tables").mapNotNull(PokerTableSummary::fromJson),
                now = json.long("now"),
            )
        }
    }
}

data class PokerLastAction(val kind: String, val amount: Long = 0) {
    val label: String
        get() = when (kind) {
            "fold" -> "Fold"
            "check" -> "Check"
            "call" -> if (amount > 0) "Call ${PokerChips.format(amount)}" else "Call"
            "bet" -> "Bet ${PokerChips.format(amount)}"
            "raise" -> "Raise ${PokerChips.format(amount)}"
            "allin" -> "All-in"
            "small blind", "small", "sb" -> "SB ${PokerChips.format(amount)}"
            "big blind", "big", "bb" -> "BB ${PokerChips.format(amount)}"
            "blind" -> "Blind ${PokerChips.format(amount)}"
            "timeout" -> "Timed out"
            "win" -> "Won ${PokerChips.format(amount)}"
            else -> kind.replaceFirstChar { it.uppercase() }
        }

    companion object {
        fun fromJson(json: JsonMap): PokerLastAction? {
            val kind = json.str("kind")
            return if (kind.isEmpty()) null else PokerLastAction(kind, json.long("amount"))
        }
    }
}

data class PokerSeat(
    val index: Int,
    val empty: Boolean = true,
    val name: String = "",
    val label: String = "",
    val bot: Boolean = false,
    val stack: Long = 0,
    val bet: Long = 0,
    val totalBet: Long = 0,
    val status: String = "",
    val cards: List<PokerCard> = emptyList(),
    val lastAction: PokerLastAction? = null,
    val isTurn: Boolean = false,
    val away: Boolean = false,
    val handName: String = "",
    val winner: Boolean = false,
    val won: Long = 0,
    val rebuy: Long = 0,
) {
    val folded: Boolean get() = status == "folded"
    val allIn: Boolean get() = status == "allin"
    val inHand: Boolean get() = !empty && (status == "active" || status == "allin")
    val busted: Boolean get() = !empty && stack <= 0 && !inHand && !folded
    val displayName: String get() = label.ifEmpty { if (bot && !name.endsWith("(bot)")) "$name (bot)" else name }
    val initial: String get() = displayName.trim().firstOrNull()?.uppercase() ?: "?"

    companion object {
        fun fromJson(json: JsonMap, fallbackIndex: Int): PokerSeat {
            val index = json.int("index", fallbackIndex)
            if (json.bool("empty") || (json["name"] == null && json["empty"] != false)) return PokerSeat(index)
            return PokerSeat(
                index = index,
                empty = false,
                name = json.str("name"),
                label = json.str("displayName"),
                bot = json.bool("bot"),
                stack = json.long("stack"),
                bet = json.long("bet"),
                totalBet = json.long("totalBet"),
                status = json.str("status"),
                cards = PokerCards.list(json.strings("cards")),
                lastAction = PokerLastAction.fromJson(json.obj("lastAction")),
                isTurn = json.bool("isTurn"),
                away = json.bool("away"),
                handName = json.str("handName"),
                winner = json.bool("winner"),
                won = json.long("won"),
                rebuy = json.long("rebuy"),
            )
        }
    }
}

data class PokerPot(val amount: Long, val eligible: List<Int> = emptyList()) {
    companion object {
        fun fromJson(json: JsonMap) = PokerPot(json.long("amount"), json.list("eligible").mapNotNull { (it as? Number)?.toInt() })
    }
}

data class PokerRaise(val min: Long, val max: Long)

data class PokerLegal(
    val fold: Boolean = true,
    val check: Boolean = false,
    val call: Long = 0,
    val raise: PokerRaise? = null,
    val allIn: Long = 0,
) {
    companion object {
        fun fromJson(json: JsonMap): PokerLegal {
            val raise = json.obj("raise").takeIf { it.isNotEmpty() }?.let { PokerRaise(it.long("min"), it.long("max")) }
            return PokerLegal(
                fold = json["fold"] != false,
                check = json.bool("check"),
                call = json.long("call"),
                raise = raise?.takeIf { it.max > 0 && it.max >= it.min },
                allIn = json.long("allIn"),
            )
        }
    }
}

data class PokerMyHand(val name: String, val cards: List<PokerCard> = emptyList()) {
    companion object {
        fun fromJson(json: JsonMap): PokerMyHand? {
            val name = json.str("name")
            return if (name.isEmpty()) null else PokerMyHand(name, PokerCards.list(json.strings("cards")))
        }
    }
}

data class PokerEvent(val seq: Long, val kind: String, val seat: Int = -1, val amount: Long = 0) {
    companion object {
        fun fromJson(json: JsonMap): PokerEvent? {
            val seq = json.long("seq", -1)
            val kind = json.str("kind")
            if (seq < 0 || kind.isEmpty()) return null
            return PokerEvent(seq, kind, json.int("seat", -1), json.long("amount"))
        }
    }
}

data class PokerLogLine(val at: Long, val seat: Int, val name: String, val kind: String, val amount: Long, val text: String) {
    companion object {
        fun fromJson(json: JsonMap) =
            PokerLogLine(json.long("at"), json.int("seat", -1), json.str("name"), json.str("kind"), json.long("amount"), json.str("text"))
    }
}

data class PokerChatLine(val at: Long, val name: String, val text: String) {
    companion object {
        fun fromJson(json: JsonMap) = PokerChatLine(json.long("at"), json.str("name"), json.str("text"))
    }
}

data class PokerWinner(
    val seat: Int,
    val name: String,
    val amount: Long,
    val handName: String = "",
    val cards: List<PokerCard> = emptyList(),
    val best: List<PokerCard> = emptyList(),
) {
    companion object {
        fun fromJson(json: JsonMap) = PokerWinner(
            json.int("seat", -1),
            json.str("name"),
            json.long("amount"),
            json.str("handName"),
            PokerCards.list(json.strings("cards")),
            PokerCards.list(json.strings("best")),
        )
    }
}

data class PokerShown(val seat: Int, val name: String, val cards: List<PokerCard>, val handName: String = "") {
    companion object {
        fun fromJson(json: JsonMap) =
            PokerShown(json.int("seat", -1), json.str("name"), PokerCards.list(json.strings("cards")), json.str("handName"))
    }
}

data class PokerRevealedCard(val index: Int, val card: String, val salt: String) {
    companion object {
        fun fromJson(json: JsonMap): PokerRevealedCard? {
            val index = json.int("index", -1)
            val card = json.str("card")
            if (index < 0 || card.isEmpty()) return null
            return PokerRevealedCard(index, card, json.str("salt"))
        }
    }
}

data class PokerLastHand(
    val number: Long,
    val board: List<PokerCard> = emptyList(),
    val winners: List<PokerWinner> = emptyList(),
    val shown: List<PokerShown> = emptyList(),
    val commitment: String = "",
    val hashes: List<String> = emptyList(),
    val cards: List<PokerRevealedCard> = emptyList(),
) {
    val revealed: Boolean get() = hashes.isNotEmpty()

    companion object {
        fun fromJson(json: JsonMap): PokerLastHand? {
            if (json.isEmpty()) return null
            val reveal = json.obj("reveal")
            return PokerLastHand(
                number = json.long("number"),
                board = PokerCards.list(json.strings("board")),
                winners = json.maps("winners").map(PokerWinner::fromJson),
                shown = json.maps("shown").map(PokerShown::fromJson),
                commitment = json.str("commitment"),
                hashes = reveal.strings("hashes").map { it.lowercase() },
                cards = reveal.maps("cards").mapNotNull(PokerRevealedCard::fromJson).sortedBy { it.index },
            )
        }
    }
}

data class PokerHistoryItem(val number: Long, val winners: List<String>, val amount: Long, val handName: String, val at: Long) {
    companion object {
        fun fromJson(json: JsonMap) =
            PokerHistoryItem(json.long("number"), json.strings("winners"), json.long("amount"), json.str("handName"), json.long("at"))
    }
}

data class PokerTable(
    val id: String,
    val name: String = "",
    val code: String = "",
    val private: Boolean = false,
    val owner: String = "",
    val ownerIsMe: Boolean = false,
    val stakes: String = "",
    val small: Long = 0,
    val big: Long = 0,
    val minBuyIn: Long = 0,
    val maxBuyIn: Long = 0,
    val seatCount: Int = 0,
    val turnSeconds: Int = 30,
    val version: Long = 0,
    val now: Long = 0,
    val handNumber: Long = 0,
    val phase: String = "waiting",
    val board: List<PokerCard> = emptyList(),
    val pots: List<PokerPot> = emptyList(),
    val potTotal: Long = 0,
    val dealer: Int = -1,
    val smallBlind: Int = -1,
    val bigBlind: Int = -1,
    val toAct: Int = -1,
    val actStartedAt: Long = 0,
    val actDeadline: Long = 0,
    val currentBet: Long = 0,
    val minRaiseTo: Long = 0,
    val wakeAt: Long = 0,
    val mySeat: Int = -1,
    val wallet: Long = 0,
    val seats: List<PokerSeat> = emptyList(),
    val legal: PokerLegal? = null,
    val myHand: PokerMyHand? = null,
    val dealerLine: String = "",
    val commitment: String = "",
    val events: List<PokerEvent> = emptyList(),
    val log: List<PokerLogLine> = emptyList(),
    val chat: List<PokerChatLine> = emptyList(),
    val lastHand: PokerLastHand? = null,
    val history: List<PokerHistoryItem> = emptyList(),
) {
    val seated: Boolean get() = mySeat in seats.indices && !seats[mySeat].empty
    val me: PokerSeat? get() = seats.getOrNull(mySeat)?.takeIf { !it.empty }
    val myTurn: Boolean get() = legal != null && toAct == mySeat && mySeat >= 0
    val inHand: Boolean get() = phase in PokerPhase.BETTING
    val showdown: Boolean get() = phase == "showdown"
    val betsOnStreet: Long get() = seats.sumOf { it.bet }
    val pottedChips: Long get() = (potTotal - betsOnStreet).coerceAtLeast(0)
    val occupied: Int get() = seats.count { !it.empty }
    val bots: Int get() = seats.count { !it.empty && it.bot }
    val hasEmptySeat: Boolean get() = seats.any { it.empty }
    val blinds: String get() = "${PokerChips.format(small)} / ${PokerChips.format(big)}"
    val myHoleCards: List<PokerCard> get() = me?.cards?.filter { !it.faceDown } ?: emptyList()

    companion object {
        fun fromJson(json: JsonMap): PokerTable? {
            val id = json.str("id")
            if (!PokerApi.isTableId(id)) return null
            val rawSeats = jsonMaps(json["seats"])
            val seatCount = json.int("seatCount").takeIf { it > 0 } ?: rawSeats.size
            val parsed = rawSeats.mapIndexed { i, seat -> PokerSeat.fromJson(seat, i) }.associateBy { it.index }
            val seats = (0 until maxOf(seatCount, (parsed.keys.maxOrNull() ?: -1) + 1)).map { parsed[it] ?: PokerSeat(it) }
            return PokerTable(
                id = id,
                name = json.str("name"),
                code = json.str("code"),
                private = json.bool("private"),
                owner = json.str("owner"),
                ownerIsMe = json.bool("ownerIsMe"),
                stakes = json.str("stakes"),
                small = json.long("small"),
                big = json.long("big"),
                minBuyIn = json.long("minBuyIn"),
                maxBuyIn = json.long("maxBuyIn"),
                seatCount = seats.size,
                turnSeconds = json.int("turnSeconds", 30),
                version = json.long("version"),
                now = json.long("now"),
                handNumber = json.long("handNumber"),
                phase = json.str("phase").ifEmpty { "waiting" },
                board = PokerCards.list(json.strings("board")),
                pots = json.maps("pots").map(PokerPot::fromJson),
                potTotal = json.long("potTotal"),
                dealer = json.int("dealer", -1),
                smallBlind = json.int("smallBlind", -1),
                bigBlind = json.int("bigBlind", -1),
                toAct = json.int("toAct", -1),
                actStartedAt = json.long("actStartedAt"),
                actDeadline = json.long("actDeadline"),
                currentBet = json.long("currentBet"),
                minRaiseTo = json.long("minRaiseTo"),
                wakeAt = json.long("wakeAt"),
                mySeat = json.int("mySeat", -1),
                wallet = json.long("wallet"),
                seats = seats,
                legal = json.obj("legal").takeIf { it.isNotEmpty() }?.let(PokerLegal::fromJson),
                myHand = PokerMyHand.fromJson(json.obj("myHand")),
                dealerLine = json.str("dealerLine"),
                commitment = json.str("commitment"),
                events = json.maps("events").mapNotNull(PokerEvent::fromJson).sortedBy { it.seq },
                log = json.maps("log").map(PokerLogLine::fromJson),
                chat = json.maps("chat").map(PokerChatLine::fromJson),
                lastHand = PokerLastHand.fromJson(json.obj("lastHand")),
                history = json.maps("history").map(PokerHistoryItem::fromJson),
            )
        }
    }
}

object PokerPhase {
    val BETTING = setOf("preflop", "flop", "turn", "river")

    fun label(phase: String): String = when (phase) {
        "waiting" -> "Waiting for players"
        "preflop" -> "Preflop"
        "flop" -> "Flop"
        "turn" -> "Turn"
        "river" -> "River"
        "showdown" -> "Showdown"
        else -> phase.replaceFirstChar { it.uppercase() }
    }
}
