package com.forkmesh.nativeapp.poker

data class PokerUpdate(
    val table: PokerTable?,
    val events: List<PokerEvent> = emptyList(),
    val turnStarted: Boolean = false,
    val wakeDelay: Long? = null,
    val adopted: Boolean = true,
)

class PokerSession(private val api: PokerApi, private val clock: () -> Long = System::currentTimeMillis) {
    var lobby: PokerLobby? = null
        private set
    var lobbyReceivedAt: Long = 0
        private set
    var table: PokerTable? = null
        private set
    var receivedAt: Long = 0
        private set

    private val tracker = PokerEventTracker()
    private var lastTurn: PokerTurnKey? = null

    @Synchronized
    fun loadLobby(): PokerLobby {
        val next = api.lobby()
        lobby = next
        lobbyReceivedAt = clock()
        return next
    }

    @Synchronized
    fun claim(): PokerWallet {
        val wallet = api.claim()
        lobby = (lobby ?: PokerLobby()).copy(wallet = wallet)
        return wallet
    }

    @Synchronized
    fun create(draft: PokerTableDraft): PokerUpdate = adopt(api.create(draft))

    @Synchronized
    fun joinByCode(code: String): PokerUpdate = adopt(api.open(api.code(code)))

    @Synchronized
    fun open(id: String): PokerUpdate = adopt(api.open(id))

    @Synchronized
    fun refresh(): PokerUpdate? {
        val id = table?.id ?: return null
        return adopt(api.open(id))
    }

    @Synchronized
    fun sit(seat: Int, buyIn: Long): PokerUpdate = adopt(api.sit(requireId(), seat, buyIn))

    @Synchronized
    fun act(kind: PokerActionKind, amount: Long = 0): PokerUpdate {
        val current = table ?: throw com.forkmesh.nativeapp.market.PackagesException("table_not_found", 404)
        return adopt(api.act(current.id, kind.wire, amount, current.handNumber))
    }

    @Synchronized
    fun tick(): PokerUpdate = adopt(api.tick(requireId()))

    @Synchronized
    fun away(on: Boolean): PokerUpdate = adopt(api.away(requireId(), on))

    @Synchronized
    fun rebuy(amount: Long): PokerUpdate = adopt(api.rebuy(requireId(), amount))

    @Synchronized
    fun addBot(): PokerUpdate = adopt(api.addBot(requireId()))

    @Synchronized
    fun removeBot(seat: Int): PokerUpdate = adopt(api.removeBot(requireId(), seat))

    @Synchronized
    fun chat(text: String): PokerUpdate = adopt(api.chat(requireId(), text))

    @Synchronized
    fun invite(names: List<String>): Pair<PokerInviteResult, PokerUpdate?> {
        val result = api.invite(requireId(), names)
        return result to result.table?.let(::adopt)
    }

    @Synchronized
    fun leave(): PokerUpdate? {
        val next = api.leave(requireId())
        return next?.let(::adopt)
    }

    @Synchronized
    fun close(): Boolean {
        val closed = api.close(requireId())
        forget()
        return closed
    }

    @Synchronized
    fun forget() {
        table = null
        receivedAt = 0
        tracker.reset()
        lastTurn = null
    }

    @Synchronized
    fun adopt(next: PokerTable, at: Long = clock()): PokerUpdate {
        val current = table
        if (current != null && current.id == next.id && next.version < current.version) {
            return PokerUpdate(current, wakeDelay = wakeDelay(at), adopted = false)
        }
        val first = current == null || current.id != next.id
        table = next
        receivedAt = at
        val events = tracker.fresh(next)
        val key = if (next.myTurn) PokerTurnKey(next.id, next.handNumber, next.actStartedAt, next.mySeat) else null
        val turnStarted = key != null && key != lastTurn && !first
        lastTurn = key
        return PokerUpdate(next, events, turnStarted, wakeDelay(at))
    }

    @Synchronized
    fun wakeDelay(localNow: Long = clock()): Long? {
        val current = table ?: return null
        return PokerClock.wakeDelay(current.wakeAt, current.now, receivedAt, localNow)
    }

    @Synchronized
    fun turnRemaining(localNow: Long = clock()): Long {
        val current = table ?: return 0
        return PokerClock.remaining(current.actDeadline, current.now, receivedAt, localNow)
    }

    @Synchronized
    fun serverNow(localNow: Long = clock()): Long {
        val current = table
        if (current != null && current.now > 0) return localNow + PokerClock.offset(current.now, receivedAt)
        val lobbyNow = lobby?.now ?: 0
        return if (lobbyNow > 0) localNow + PokerClock.offset(lobbyNow, lobbyReceivedAt) else localNow
    }

    @Synchronized
    fun claimRemaining(localNow: Long = clock()): Long {
        val current = lobby ?: return 0
        if (current.wallet.canClaim) return 0
        val now = if (current.now > 0) localNow + PokerClock.offset(current.now, lobbyReceivedAt) else localNow
        return (current.wallet.nextClaimAt - now).coerceAtLeast(0)
    }

    private fun requireId(): String = table?.id ?: throw com.forkmesh.nativeapp.market.PackagesException("table_not_found", 404)
}
