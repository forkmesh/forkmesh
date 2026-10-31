package com.forkmesh.nativeapp.feature.poker

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.poker.PokerActionKind
import com.forkmesh.nativeapp.poker.PokerApi
import com.forkmesh.nativeapp.poker.PokerClock
import com.forkmesh.nativeapp.poker.PokerEvent
import com.forkmesh.nativeapp.poker.PokerEvents
import com.forkmesh.nativeapp.poker.PokerLobby
import com.forkmesh.nativeapp.poker.PokerSession
import com.forkmesh.nativeapp.poker.PokerSound
import com.forkmesh.nativeapp.poker.PokerTable
import com.forkmesh.nativeapp.poker.PokerTableDraft
import com.forkmesh.nativeapp.poker.PokerUpdate
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal data class PokerFx(
    val id: Long,
    val deal: Boolean = false,
    val board: Set<Int> = emptySet(),
    val bets: Set<Int> = emptySet(),
    val sweep: Map<Int, Long> = emptyMap(),
    val payout: Map<Int, Long> = emptyMap(),
    val folds: Set<Int> = emptySet(),
)

internal sealed interface PokerSheet {
    data object Chat : PokerSheet
    data object Log : PokerSheet
    data object LastHand : PokerSheet
    data object Menu : PokerSheet
    data object Invite : PokerSheet
    data object Rebuy : PokerSheet
    data class Sit(val seat: Int) : PokerSheet
    data object ConfirmLeave : PokerSheet
    data object ConfirmClose : PokerSheet
}

internal class PokerModel(private val context: Context, private val scope: CoroutineScope) {
    val sounds = PokerSounds()
    private var session: PokerSession? = null

    var lobby by mutableStateOf<PokerLobby?>(null)
        private set
    var lobbyLoading by mutableStateOf(false)
        private set
    var lobbyProblem by mutableStateOf("")
        private set
    var lobbyReceivedAt by mutableLongStateOf(0L)
        private set
    var claiming by mutableStateOf(false)
        private set

    var table by mutableStateOf<PokerTable?>(null)
        private set
    var tableReceivedAt by mutableLongStateOf(0L)
        private set
    var openingId by mutableStateOf<String?>(null)
        private set
    var tableProblem by mutableStateOf("")
        private set
    var notice by mutableStateOf<String?>(null)
    var busy by mutableStateOf(false)
        private set
    var fx by mutableStateOf(PokerFx(0))
        private set
    var sheet by mutableStateOf<PokerSheet?>(null)
    var raising by mutableStateOf(false)
    var raiseAmount by mutableLongStateOf(0L)
    var muted by mutableStateOf(sounds.muted)
        private set
    var chatSeen by mutableIntStateOf(0)
    var closedTable by mutableStateOf(false)
        private set

    private var shownId: String? = null
    private var lobbyShown = false
    private var clockJob: Job? = null
    private var tickingJob: Job? = null
    private var soundJob: Job? = null
    private var refreshing = false
    private var refreshAgain = false
    private var fxCounter = 0L
    private var tickedWake = 0L
    private val soundQueue = ArrayDeque<PokerSound>()

    init {
        sounds.warm()
    }

    private fun session(): PokerSession? {
        session?.let { return it }
        val client = MarketFeature.client() ?: return null
        return PokerSession(PokerApi(client)).also { session = it }
    }

    fun toggleMute() {
        sounds.muted = !sounds.muted
        muted = sounds.muted
    }

    private fun describe(error: Throwable): String {
        val code = (error as? PackagesException)?.code ?: "network"
        return PokerApi.problem(code)
    }

    private fun code(error: Throwable): String = (error as? PackagesException)?.code ?: "network"

    fun lobbyAppeared() {
        lobbyShown = true
        loadLobby()
    }

    fun lobbyGone() {
        lobbyShown = false
    }

    fun loadLobby() {
        val current = session() ?: run {
            lobbyProblem = PokerApi.problem("invalid_session")
            return
        }
        if (lobbyLoading) return
        lobbyLoading = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.loadLobby() } }
            lobbyLoading = false
            result.onSuccess {
                lobby = it
                lobbyReceivedAt = System.currentTimeMillis()
                lobbyProblem = ""
            }.onFailure { if (it !is CancellationException) lobbyProblem = describe(it) }
        }
    }

    fun claim() {
        val current = session() ?: return
        if (claiming) return
        claiming = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.claim() } }
            claiming = false
            result.onSuccess { wallet ->
                lobby = (lobby ?: PokerLobby()).copy(wallet = wallet)
                notice = "Collected ${com.forkmesh.nativeapp.poker.PokerChips.format(wallet.claimAmount)} free chips."
                play(PokerSound.CHIP)
                loadLobby()
            }.onFailure {
                lobbyProblem = describe(it)
                loadLobby()
            }
        }
    }

    fun claimRemaining(localNow: Long): Long {
        val current = lobby ?: return 0
        if (current.wallet.canClaim) return 0
        val offset = PokerClock.offset(current.now, lobbyReceivedAt)
        return (current.wallet.nextClaimAt - (localNow + offset)).coerceAtLeast(0)
    }

    fun create(draft: PokerTableDraft, onOpen: (String) -> Unit) {
        perform({ it.create(draft) }) { update ->
            update.table?.id?.let(onOpen)
        }
    }

    fun joinByCode(code: String, onOpen: (String) -> Unit) {
        val normalized = PokerApi.normalizeCode(code)
        if (!PokerApi.isCode(normalized)) {
            lobbyProblem = PokerApi.problem("invalid_code")
            return
        }
        perform({ it.joinByCode(normalized) }) { update -> update.table?.id?.let(onOpen) }
    }

    fun tableAppeared(id: String) {
        shownId = id
        closedTable = false
        if (table?.id != id) {
            table = null
            tableProblem = ""
            chatSeen = 0
            session?.forget()
        }
        openingId = id
        refreshTable(id)
    }

    fun tableGone(id: String) {
        if (shownId != id) return
        shownId = null
        clockJob?.cancel()
        tickingJob?.cancel()
        raising = false
        sheet = null
        if (lobbyShown) loadLobby()
    }

    fun ring() {
        val id = shownId
        if (id != null) refreshTable(id) else if (lobbyShown) loadLobby()
    }

    private fun refreshTable(id: String) {
        val current = session() ?: run {
            tableProblem = PokerApi.problem("invalid_session")
            return
        }
        if (refreshing) {
            refreshAgain = true
            return
        }
        refreshing = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.open(id) } }
            refreshing = false
            openingId = null
            result.onSuccess { apply(it) }.onFailure { failed(it) }
            if (refreshAgain && shownId == id) {
                refreshAgain = false
                refreshTable(id)
            } else {
                refreshAgain = false
            }
        }
    }

    fun refresh() {
        val id = shownId
        if (id != null) refreshTable(id) else loadLobby()
    }

    fun act(kind: PokerActionKind, amount: Long = 0) {
        raising = false
        perform({ it.act(kind, amount) })
    }

    fun sit(seat: Int, buyIn: Long) {
        sheet = null
        perform({ it.sit(seat, buyIn) })
    }

    fun away(on: Boolean) = perform({ it.away(on) })

    fun rebuy(amount: Long) {
        sheet = null
        perform({ it.rebuy(amount) })
    }

    fun addBot() = perform({ it.addBot() })

    fun removeBot(seat: Int) = perform({ it.removeBot(seat) })

    fun chat(text: String) {
        val line = text.trim()
        if (line.isEmpty()) return
        perform({ it.chat(line) })
    }

    fun invite(raw: String) {
        val names = PokerApi.normalizeNames(raw)
        if (names.isEmpty()) {
            notice = "Type one or more account names."
            return
        }
        val current = session() ?: return
        busy = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.invite(names) } }
            busy = false
            result.onSuccess { (invite, update) ->
                update?.let(::apply)
                sheet = null
                notice = when {
                    invite.invited.isNotEmpty() && invite.skipped.isEmpty() -> "Invited ${invite.invited.joinToString(", ")}."
                    invite.invited.isNotEmpty() -> "Invited ${invite.invited.joinToString(", ")}. Skipped ${invite.skipped.joinToString(", ")}."
                    else -> "Nobody new to invite."
                }
            }.onFailure { failed(it) }
        }
    }

    fun leave(onLeft: () -> Unit) {
        sheet = null
        perform({ it.leave() ?: PokerUpdate(null) }) { onLeft() }
    }

    fun close(onClosed: () -> Unit) {
        sheet = null
        val current = session() ?: return
        busy = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.close() } }
            busy = false
            result.onSuccess {
                clockJob?.cancel()
                table = null
                notice = "Table closed. Every stack went back to its wallet."
                onClosed()
            }.onFailure { failed(it) }
        }
    }

    private fun tick() {
        val current = session() ?: return
        val id = shownId ?: return
        val showing = table ?: return
        if (showing.id != id) return
        tickedWake = showing.wakeAt
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { current.tick() } }
            result.onSuccess { apply(it) }.onFailure { failed(it) }
        }
    }

    private fun perform(block: (PokerSession) -> PokerUpdate, after: (PokerUpdate) -> Unit = {}) {
        val current = session() ?: run {
            tableProblem = PokerApi.problem("invalid_session")
            return
        }
        if (busy) return
        busy = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { block(current) } }
            busy = false
            result.onSuccess {
                apply(it)
                after(it)
            }.onFailure { failed(it) }
        }
    }

    private fun failed(error: Throwable) {
        if (error is CancellationException) return
        when (code(error)) {
            "table_not_found", "not_found" -> {
                clockJob?.cancel()
                session?.forget()
                table = null
                closedTable = true
                tableProblem = PokerApi.problem("table_not_found")
            }
            "hand_changed", "not_your_turn" -> {
                notice = describe(error)
                shownId?.let(::refreshTable)
            }
            else -> {
                if (table == null && shownId != null) tableProblem = describe(error) else notice = describe(error)
            }
        }
    }

    private fun apply(update: PokerUpdate) {
        val next = update.table ?: run {
            clockJob?.cancel()
            return
        }
        if (shownId != null && next.id != shownId) return
        val previous = table
        if (update.adopted) {
            table = next
            tableReceivedAt = session?.receivedAt ?: System.currentTimeMillis()
            tableProblem = ""
            if (chatSeen == 0 && previous == null) chatSeen = next.chat.size
            if (sheet == PokerSheet.Chat) chatSeen = next.chat.size
        }
        effects(previous, next, update.events)
        if (update.turnStarted) {
            PokerHaptics.turn(context)
            play(PokerSound.TURN)
            raiseAmount = next.legal?.raise?.min ?: 0
        }
        if (!next.myTurn) raising = false
        val stalled = next.wakeAt > 0 && next.wakeAt == tickedWake && next.now >= next.wakeAt
        schedule(if (stalled) null else update.wakeDelay)
        scheduleClockTicks(next)
    }

    private fun schedule(delayMs: Long?) {
        clockJob?.cancel()
        if (delayMs == null || shownId == null) return
        clockJob = scope.launch {
            delay(delayMs)
            tick()
        }
    }

    private fun scheduleClockTicks(next: PokerTable) {
        tickingJob?.cancel()
        if (!next.myTurn || next.actDeadline <= 0) return
        val receivedAt = tableReceivedAt
        tickingJob = scope.launch {
            while (true) {
                val left = PokerClock.remaining(next.actDeadline, next.now, receivedAt, System.currentTimeMillis())
                if (left <= 0) break
                if (left > PokerClock.RED_MS) {
                    delay(left - PokerClock.RED_MS)
                    continue
                }
                play(PokerSound.CLOCK)
                val wait = left % 1000
                delay(if (wait < 50) 1000 + wait else wait)
            }
        }
    }

    private fun effects(previous: PokerTable?, next: PokerTable, events: List<PokerEvent>) {
        if (events.isEmpty()) return
        val kinds = events.map { it.kind }.toSet()
        val streetEnded = kinds.any { it in setOf("flop", "turn", "river", "showdown", "win") }
        val board = buildSet {
            if ("flop" in kinds) addAll(0..2)
            if ("turn" in kinds) add(3)
            if ("river" in kinds) add(4)
        }.filter { it < next.board.size }.toSet()
        val sweep = if (streetEnded && previous != null && previous.id == next.id) {
            previous.seats.filter { it.bet > 0 }.associate { it.index to it.bet }
        } else {
            emptyMap()
        }
        val bets = events.filter { it.kind in setOf("call", "bet", "raise", "allin", "blinds") && it.seat >= 0 }.map { it.seat }.toMutableSet()
        if ("blinds" in kinds) {
            if (next.smallBlind >= 0) bets += next.smallBlind
            if (next.bigBlind >= 0) bets += next.bigBlind
        }
        val payout = events.filter { it.kind == "win" && it.seat >= 0 }.groupBy { it.seat }.mapValues { (_, list) -> list.sumOf { it.amount } }
        val folds = events.filter { it.kind == "fold" && it.seat >= 0 }.map { it.seat }.toSet()
        fxCounter += 1
        fx = PokerFx(fxCounter, deal = "deal" in kinds, board = board, bets = bets, sweep = sweep, payout = payout, folds = folds)
        events.mapNotNull { PokerEvents.sound(it.kind) }.forEach(::play)
    }

    private fun play(sound: PokerSound) {
        if (muted) return
        soundQueue.addLast(sound)
        if (soundJob?.isActive == true) return
        soundJob = scope.launch {
            while (soundQueue.isNotEmpty()) {
                val next = soundQueue.removeFirst()
                sounds.play(next)
                delay(
                    when (next) {
                        PokerSound.SHUFFLE -> 380
                        PokerSound.WIN -> 500
                        PokerSound.ALLIN -> 300
                        PokerSound.TURN -> 160
                        else -> 120
                    },
                )
            }
        }
    }

    fun dispose() {
        clockJob?.cancel()
        tickingJob?.cancel()
        soundJob?.cancel()
        sounds.release()
    }

    companion object {
        fun create(context: Context): PokerModel = PokerModel(context.applicationContext, ProjectLifecycle.scope)
    }
}
