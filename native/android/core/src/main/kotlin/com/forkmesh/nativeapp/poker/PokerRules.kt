package com.forkmesh.nativeapp.poker

import java.text.NumberFormat
import java.util.Locale
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.roundToLong
import kotlin.math.sin

data class PokerChipStack(val denomination: Long, val count: Int)

object PokerChips {
    val DENOMINATIONS = listOf(5000L, 1000L, 500L, 100L, 25L, 5L, 1L)

    val COLORS: Map<Long, Long> = mapOf(
        1L to 0xFFF5F5F5,
        5L to 0xFFC62828,
        25L to 0xFF2E7D32,
        100L to 0xFF212121,
        500L to 0xFF6A1B9A,
        1000L to 0xFFF9A825,
        5000L to 0xFFEF6C00,
    )

    fun color(denomination: Long): Long = COLORS[denomination] ?: 0xFFF5F5F5

    fun breakdown(amount: Long): List<PokerChipStack> {
        var left = amount.coerceAtLeast(0)
        val out = ArrayList<PokerChipStack>()
        for (value in DENOMINATIONS) {
            val count = left / value
            if (count > 0) {
                out += PokerChipStack(value, count.toInt())
                left -= count * value
            }
        }
        return out
    }

    fun visual(amount: Long, maxChips: Int = 12): List<Long> {
        val chips = ArrayList<Long>()
        for (stack in breakdown(amount)) repeat(stack.count) { chips += stack.denomination }
        if (chips.size <= maxChips) return chips.reversed()
        return chips.take(maxChips).reversed()
    }

    fun format(amount: Long): String = NumberFormat.getIntegerInstance(Locale.US).format(amount)

    fun short(amount: Long): String = when {
        amount >= 1_000_000 -> trim(amount / 1_000_000.0) + "M"
        amount >= 10_000 -> trim(amount / 1_000.0) + "K"
        else -> format(amount)
    }

    private fun trim(value: Double): String {
        val rounded = (value * 10).roundToLong() / 10.0
        return if (rounded % 1.0 == 0.0) rounded.toLong().toString() else rounded.toString()
    }
}

data class PokerPoint(val x: Double, val y: Double)

object PokerLayout {
    fun order(seatCount: Int, mySeat: Int): List<Int> {
        if (seatCount <= 0) return emptyList()
        val base = if (mySeat in 0 until seatCount) mySeat else 0
        return (0 until seatCount).map { (base + it) % seatCount }
    }

    fun slot(seat: Int, seatCount: Int, mySeat: Int): Int {
        if (seatCount <= 0) return 0
        val base = if (mySeat in 0 until seatCount) mySeat else 0
        return ((seat - base) % seatCount + seatCount) % seatCount
    }

    fun angle(slot: Int, count: Int, gap: Double = 0.0): Double {
        val n = count.coerceAtLeast(1)
        val open = gap.coerceIn(0.0, PI)
        val travel = slot * (2 * PI - open) / n
        return PI / 2 + if (travel <= PI - open / 2 + 1e-9) travel else travel + open
    }

    fun position(slot: Int, count: Int, gap: Double = 0.0): PokerPoint {
        val a = angle(slot, count, gap)
        return PokerPoint(clean(cos(a)), clean(sin(a)))
    }

    fun positions(count: Int, gap: Double = 0.0): List<PokerPoint> = (0 until count.coerceAtLeast(0)).map { position(it, count, gap) }

    fun onRail(point: PokerPoint, squareness: Double = 4.0): PokerPoint {
        val d = Math.pow(Math.pow(kotlin.math.abs(point.x), squareness) + Math.pow(kotlin.math.abs(point.y), squareness), 1 / squareness)
        if (d <= 0) return point
        return PokerPoint(point.x / d, point.y / d)
    }

    fun towardCentre(point: PokerPoint, amount: Double): PokerPoint = PokerPoint(point.x * (1 - amount), point.y * (1 - amount))

    private fun clean(value: Double): Double = if (kotlin.math.abs(value) < 1e-9) 0.0 else value
}

enum class PokerActionKind(val wire: String) { FOLD("fold"), CHECK("check"), CALL("call"), BET("bet"), RAISE("raise"), ALLIN("allin") }

data class PokerButton(val kind: PokerActionKind, val label: String, val amount: Long = 0)

data class PokerPreset(val label: String, val amount: Long)

object PokerActions {
    fun buttons(table: PokerTable): List<PokerButton> {
        val legal = table.legal ?: return emptyList()
        val me = table.me
        val out = ArrayList<PokerButton>()
        val stack = me?.stack ?: legal.allIn
        if (legal.fold && !legal.check) out += PokerButton(PokerActionKind.FOLD, "Fold")
        if (legal.check) {
            out += PokerButton(PokerActionKind.CHECK, "Check")
        } else if (legal.call > 0) {
            val allInCall = stack in 1..legal.call
            out += PokerButton(PokerActionKind.CALL, if (allInCall) "Call ${PokerChips.format(stack)} (all-in)" else "Call ${PokerChips.format(legal.call)}", legal.call)
        }
        val raise = legal.raise
        if (raise != null) {
            val kind = if (table.currentBet <= 0) PokerActionKind.BET else PokerActionKind.RAISE
            out += PokerButton(kind, if (kind == PokerActionKind.BET) "Bet" else "Raise to", raise.min)
        }
        if (legal.allIn > 0 && legal.allIn > legal.call) out += PokerButton(PokerActionKind.ALLIN, "All-in ${PokerChips.format(legal.allIn)}", legal.allIn)
        return out
    }

    fun raiseLabel(table: PokerTable, amount: Long): String {
        val bet = table.currentBet <= 0
        val max = table.legal?.raise?.max ?: 0
        return when {
            max in 1..amount -> "All-in ${PokerChips.format(amount)}"
            bet -> "Bet ${PokerChips.format(amount)}"
            else -> "Raise to ${PokerChips.format(amount)}"
        }
    }

    fun wireFor(table: PokerTable, amount: Long): PokerActionKind {
        val max = table.legal?.raise?.max ?: 0
        return when {
            max in 1..amount -> PokerActionKind.ALLIN
            table.currentBet <= 0 -> PokerActionKind.BET
            else -> PokerActionKind.RAISE
        }
    }

    fun presets(table: PokerTable): List<PokerPreset> {
        val raise = table.legal?.raise ?: return emptyList()
        val myBet = table.me?.bet ?: 0
        val toCall = (table.currentBet - myBet).coerceAtLeast(0)
        val potAfterCall = table.potTotal + toCall
        val unit = table.small.coerceAtLeast(1)
        fun sized(fraction: Double): Long {
            val target = table.currentBet + fraction * potAfterCall
            val rounded = ((target / unit).roundToLong()) * unit
            return clamp(rounded, raise.min, raise.max)
        }
        return listOf(
            PokerPreset("½ pot", sized(0.5)),
            PokerPreset("¾ pot", sized(0.75)),
            PokerPreset("Pot", sized(1.0)),
            PokerPreset("All-in", raise.max),
        )
    }

    fun clamp(amount: Long, min: Long, max: Long): Long = if (max < min) min else amount.coerceIn(min, max)

    fun step(table: PokerTable): Long = table.small.coerceAtLeast(1)
}

object PokerClock {
    const val WAKE_SLACK_MS = 150L
    const val AMBER_MS = 10_000L
    const val RED_MS = 5_000L

    fun offset(serverNow: Long, receivedAt: Long): Long = if (serverNow > 0) serverNow - receivedAt else 0

    fun wakeDelay(wakeAt: Long, serverNow: Long, receivedAt: Long, localNow: Long): Long? {
        if (wakeAt <= 0) return null
        val local = wakeAt - offset(serverNow, receivedAt) + WAKE_SLACK_MS
        return (local - localNow).coerceAtLeast(0)
    }

    fun remaining(deadline: Long, serverNow: Long, receivedAt: Long, localNow: Long): Long {
        if (deadline <= 0) return 0
        return (deadline - offset(serverNow, receivedAt) - localNow).coerceAtLeast(0)
    }

    fun fraction(started: Long, deadline: Long, remaining: Long): Double {
        val span = deadline - started
        if (span <= 0) return 0.0
        return (remaining.toDouble() / span).coerceIn(0.0, 1.0)
    }

    fun tone(remaining: Long): PokerClockTone = when {
        remaining < RED_MS -> PokerClockTone.RED
        remaining < AMBER_MS -> PokerClockTone.AMBER
        else -> PokerClockTone.GREEN
    }

    fun countdown(ms: Long): String {
        val total = (ms.coerceAtLeast(0) + 999) / 1000
        val h = total / 3600
        val m = (total % 3600) / 60
        val s = total % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
    }
}

enum class PokerClockTone { GREEN, AMBER, RED }

enum class PokerSound { SHUFFLE, DEAL, CHIP, CHECK, FOLD, TURN, CLOCK, ALLIN, WIN }

object PokerEvents {
    fun sound(kind: String): PokerSound? = when (kind) {
        "shuffle" -> PokerSound.SHUFFLE
        "deal", "flop", "turn", "river", "showdown" -> PokerSound.DEAL
        "blinds", "call", "bet", "raise" -> PokerSound.CHIP
        "check" -> PokerSound.CHECK
        "fold", "timeout" -> PokerSound.FOLD
        "allin" -> PokerSound.ALLIN
        "win" -> PokerSound.WIN
        else -> null
    }
}

class PokerEventTracker {
    var tableId: String = ""
        private set
    var highest: Long = -1
        private set

    fun fresh(table: PokerTable): List<PokerEvent> {
        val top = table.events.maxOfOrNull { it.seq } ?: 0
        if (table.id != tableId || highest < 0) {
            tableId = table.id
            highest = top
            return emptyList()
        }
        val out = table.events.filter { it.seq > highest }.sortedBy { it.seq }
        if (top > highest) highest = top
        return out
    }

    fun reset() {
        tableId = ""
        highest = -1
    }
}

data class PokerTurnKey(val tableId: String, val hand: Long, val started: Long, val seat: Int)

object PokerFair {
    const val DECK_SIZE = 52

    private val sha: (ByteArray) -> ByteArray = { java.security.MessageDigest.getInstance("SHA-256").digest(it) }

    fun cardHash(salt: String, index: Int, card: String, sha256: (ByteArray) -> ByteArray = sha): String =
        hex(sha256("$salt:$index:$card".toByteArray(Charsets.UTF_8)))

    fun commitment(hashes: List<String>, sha256: (ByteArray) -> ByteArray = sha): String =
        hex(sha256(hashes.joinToString(",").toByteArray(Charsets.UTF_8)))

    fun verify(lastHand: PokerLastHand, sha256: (ByteArray) -> ByteArray = sha): Boolean? {
        if (!lastHand.revealed || lastHand.commitment.isEmpty()) return null
        val hashes = lastHand.hashes
        if (hashes.size != DECK_SIZE) return false
        if (commitment(hashes, sha256) != lastHand.commitment.lowercase()) return false
        return lastHand.cards.all { it.index in hashes.indices && cardHash(it.salt, it.index, it.card, sha256) == hashes[it.index] }
    }

    private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
}
