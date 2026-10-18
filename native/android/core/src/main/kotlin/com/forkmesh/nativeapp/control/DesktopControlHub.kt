package com.forkmesh.nativeapp.control

import java.util.concurrent.Callable
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorCompletionService
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

sealed class DesktopReach {
    data object NotPaired : DesktopReach()

    data class Offline(val reason: String) : DesktopReach()

    data class Connecting(val name: String) : DesktopReach()

    data class Connected(val transport: ControlTransport, val name: String) : DesktopReach()

    val isConnected: Boolean get() = this is Connected

    val label: String
        get() = when (this) {
            NotPaired -> "Sign in to ForkMesh on your desktop with this account"
            is Offline -> reason.ifEmpty { "Desktop not reachable" }
            is Connecting -> "Connecting to $name…"
            is Connected -> if (transport == ControlTransport.Lan) "$name · on this network" else "$name · through the relay"
        }
}

class LanControlLink(val service: LocalControlService) : ControlLink {
    override val transport: ControlTransport get() = ControlTransport.Lan
    override val isOpen: Boolean get() = service.connected
    override val desktopName: String get() = service.desktopName
    val desktopKey: String get() = service.desktopKey

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> =
        service.request(method, params, timeoutMs)

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = service.addEventTap(tap)

    override fun removeEventTap(token: Any) = service.removeEventTap(token)
}

fun interface RelayControlOpener {
    fun open(desktop: PairedDesktop): RelayControlConnection
}

class DesktopControlHub(
    val local: LocalControlService?,
    private val openRelay: RelayControlOpener?,
    private val accountDesktopsLoader: (() -> List<PairedDesktop>)? = null,
    private val clock: () -> Long = { System.currentTimeMillis() },
    private val relayIdleProbeMs: Long = 45_000,
    private val pairedProvider: () -> List<PairedDesktop> = { local?.paired ?: emptyList() },
) {
    private val lock = Object()

    @Volatile
    var accountDesktops: List<PairedDesktop> = emptyList()
        private set

    @Volatile
    private var accountDesktopsLoadedAt = 0L

    @Volatile
    private var lastReachedKey = ""
    private val lanLink = local?.let { LanControlLink(it) }
    private var relay: RelayControlConnection? = null
    private var opening = false
    private val taps = CopyOnWriteArrayList<Pair<Any, (ControlEvent) -> Unit>>()
    private var attached: Pair<ControlLink, Any>? = null
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val linkListeners = CopyOnWriteArrayList<Pair<Any, () -> Unit>>()
    private var announcedLink: ControlLink? = null
    private val warmer = Executors.newSingleThreadScheduledExecutor { Thread(it, "DesktopControlWarm").apply { isDaemon = true } }
    private var warmRetry: ScheduledFuture<*>? = null

    @Volatile
    private var warmDelayMs = 2_000L

    @Volatile
    var lanGraceMs: Long = 1_500
    private val localListener: () -> Unit = { localChanged() }

    @Volatile
    var reach: DesktopReach = DesktopReach.NotPaired
        private set

    @Volatile
    var revision: Int = 0
        private set

    init {
        local?.addListener(localListener)
        refreshReach()
    }

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    val desktop: PairedDesktop? get() = candidates.firstOrNull()

    val candidates: List<PairedDesktop>
        get() {
            val paired = pairedProvider().sortedByDescending { it.pairedAtMs }
            val out = mutableListOf<PairedDesktop>()
            val service = local
            if (service != null && service.connected) {
                paired.firstOrNull { it.key == service.desktopKey }?.let { out.add(it) }
            }
            for (item in paired + accountDesktops) {
                val index = out.indexOfFirst { it.key == item.key }
                if (index < 0) out.add(item) else if (out[index].name.isEmpty()) out[index] = item
            }
            val reached = out.indexOfFirst { it.key == lastReachedKey }
            if (reached > 0) out.add(0, out.removeAt(reached))
            return out
        }

    fun refreshAccountDesktops(force: Boolean = false): List<PairedDesktop> {
        val loader = accountDesktopsLoader ?: return accountDesktops
        if (!force && accountDesktopsLoadedAt > 0 && clock() - accountDesktopsLoadedAt < 600_000) return accountDesktops
        val loaded = runCatching { loader() }.getOrNull() ?: return accountDesktops
        accountDesktops = loaded
        accountDesktopsLoadedAt = clock()
        local?.setAccountDesktops(loaded.map { it.key }.toSet())
        val before = reach
        refreshReach()
        if (before != reach) {
            revision++
            notifyListeners()
        }
        return accountDesktops
    }

    val currentLink: ControlLink?
        get() {
            lanLink?.takeIf { it.isOpen }?.let { return it }
            return synchronized(lock) { relay }?.takeIf { it.isOpen }
        }

    val transport: ControlTransport? get() = currentLink?.transport

    fun link(): ControlLink {
        currentLink?.let { current ->
            val relayed = current as? RelayControlConnection
            if (relayed == null || relayed.idleMs < relayIdleProbeMs || relayed.confirmAlive()) {
                attachTaps(current)
                return current
            }
        }
        lanLink?.let { lan ->
            if (awaitLan(lan)) {
                attachTaps(lan)
                return lan
            }
        }
        synchronized(lock) {
            while (opening) lock.wait()
        }
        currentLink?.let {
            attachTaps(it)
            return it
        }
        val opener = openRelay ?: throw LocalControlException("not_connected")
        synchronized(lock) { opening = true }
        try {
            refreshAccountDesktops()
            val targets = candidates.filter { it.name.isNotEmpty() }.take(4)
            if (targets.isEmpty()) {
                refreshReach()
                notifyListeners()
                throw LocalControlException("not_paired")
            }
            reach = DesktopReach.Connecting(targets.first().name)
            notifyListeners()
            val (connection, target) = race(targets, opener)
            lastReachedKey = target.key
            synchronized(lock) { relay = connection }
            connection.onClose { reason ->
                synchronized(lock) { if (relay === connection) relay = null }
                reach = DesktopReach.Offline(ControlErrorText.describe(reason))
                revision++
                notifyListeners()
                announceLink()
                if (keepWarm) scheduleWarm()
            }
            attachTaps(connection)
            refreshReach()
            revision++
            notifyListeners()
            announceLink()
            return connection
        } catch (error: Throwable) {
            if (!(error is LocalControlException && error.code == "not_paired")) {
                reach = DesktopReach.Offline(ControlErrorText.describe(error))
                notifyListeners()
            }
            throw error
        } finally {
            synchronized(lock) {
                opening = false
                lock.notifyAll()
            }
        }
    }

    private fun awaitLan(lan: LanControlLink): Boolean {
        val service = local ?: return lan.isOpen
        val deadline = System.nanoTime() + lanGraceMs * 1_000_000
        while (!lan.isOpen && service.lanPending && System.nanoTime() < deadline) {
            try {
                Thread.sleep(20)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                break
            }
        }
        return lan.isOpen
    }

    fun request(method: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?> =
        link().request(method, params, timeoutMs)

    @Volatile
    var keepWarm: Boolean = false
        set(value) {
            field = value
            if (value) {
                warm()
            } else {
                synchronized(lock) {
                    warmRetry?.cancel(false)
                    warmRetry = null
                }
            }
        }

    fun warm() {
        if (currentLink != null || openRelay == null || synchronized(lock) { opening }) return
        warmer.execute {
            try {
                link()
                warmDelayMs = 2_000
            } catch (refusal: LocalControlException) {
                if (refusal.code != "not_paired" && keepWarm) scheduleWarm()
            } catch (_: Throwable) {
                if (keepWarm) scheduleWarm()
            }
        }
    }

    private fun scheduleWarm() {
        synchronized(lock) {
            if (warmRetry != null) return
            val delay = warmDelayMs
            warmDelayMs = minOf(warmDelayMs * 2, 60_000)
            warmRetry = warmer.schedule({
                synchronized(lock) { warmRetry = null }
                if (keepWarm) warm()
            }, delay, TimeUnit.MILLISECONDS)
        }
    }

    fun addLinkListener(listener: () -> Unit): Any {
        val token = Any()
        linkListeners.add(token to listener)
        return token
    }

    fun removeLinkListener(token: Any) {
        linkListeners.removeAll { it.first === token }
    }

    private fun announceLink() {
        val now = currentLink
        synchronized(lock) {
            if (now === announcedLink) return
            announcedLink = now
        }
        for ((_, listener) in linkListeners) runCatching { listener() }
    }

    fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps.add(token to tap)
        currentLink?.let { attachTaps(it) }
        return token
    }

    fun removeEventTap(token: Any) {
        taps.removeAll { it.first === token }
    }

    fun disconnectRelay() {
        val current = synchronized(lock) { relay.also { relay = null } }
        current?.close()
        refreshReach()
        notifyListeners()
        announceLink()
    }

    private fun attachTaps(link: ControlLink) {
        synchronized(lock) {
            val existing = attached
            if (existing != null && existing.first === link) return
            if (existing != null) existing.first.removeEventTap(existing.second)
            val token = link.addEventTap { event -> for ((_, tap) in taps) runCatching { tap(event) } }
            attached = link to token
        }
    }

    private fun localChanged() {
        val before = reach
        refreshReach()
        val lan = lanLink
        if (lan != null && lan.isOpen) {
            attachTaps(lan)
            if (synchronized(lock) { relay } != null) {
                Thread({ disconnectRelay() }, "DesktopControlHub").apply { isDaemon = true }.start()
            }
        }
        if (before != reach) revision++
        notifyListeners()
        announceLink()
    }

    private fun refreshReach() {
        val lan = lanLink
        val relayNow = synchronized(lock) { relay }
        reach = when {
            lan != null && lan.isOpen -> DesktopReach.Connected(ControlTransport.Lan, lan.desktopName)
            relayNow != null && relayNow.isOpen ->
                DesktopReach.Connected(ControlTransport.Relay, relayNow.desktopName.ifEmpty { relayNow.node })
            pairedProvider().isEmpty() && accountDesktops.isEmpty() -> DesktopReach.NotPaired
            reach is DesktopReach.Connecting || reach is DesktopReach.Offline -> reach
            else -> DesktopReach.Offline("")
        }
    }

    fun dispose() {
        keepWarm = false
        local?.removeListener(localListener)
        disconnectRelay()
    }

    private fun race(targets: List<PairedDesktop>, opener: RelayControlOpener): Pair<RelayControlConnection, PairedDesktop> {
        if (targets.size == 1) return opener.open(targets[0]) to targets[0]
        val pool = Executors.newFixedThreadPool(targets.size) { Thread(it, "DesktopDial").apply { isDaemon = true } }
        val dials = ExecutorCompletionService<Pair<RelayControlConnection, PairedDesktop>>(pool)
        for (target in targets) dials.submit(Callable { opener.open(target) to target })
        pool.shutdown()
        var lastError: Throwable = LocalControlException("not_paired")
        for (finished in 1..targets.size) {
            try {
                val winner = dials.take().get()
                val stragglers = targets.size - finished
                if (stragglers > 0) {
                    Thread({
                        repeat(stragglers) { runCatching { dials.take().get().first.close() } }
                    }, "DesktopDial").apply { isDaemon = true }.start()
                }
                return winner
            } catch (failed: ExecutionException) {
                lastError = failed.cause ?: failed
            }
        }
        throw lastError
    }
}

enum class DesktopControlFamilyName(val prefix: String) {
    AgentOps("agentops."),
    MergeQueue("mergequeue."),
    Pulls("pulls."),
    Fleet("fleet."),
    FilesBrowse("files.browse."),
    Diagnostics("diag."),
    Teammates("teammates."),
    Insights("insights."),
    Discord("discord."),
    Wallet("wallet."),
    Update("update."),
    Repos("repos."),
    Pings("pings."),
    ;

    companion object {
        fun owning(method: String): DesktopControlFamilyName? =
            entries.sortedByDescending { it.prefix.length }.firstOrNull { method.startsWith(it.prefix) }
    }
}

class DesktopControlFamily(val name: DesktopControlFamilyName, private val hub: DesktopControlHub) {
    val prefix: String get() = name.prefix

    fun method(suffix: String): String = if (suffix.startsWith(prefix)) suffix else prefix + suffix

    fun call(suffix: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?> =
        hub.request(method(suffix), params, timeoutMs)

    fun addEventTap(tap: (String, Map<String, Any?>) -> Unit): Any = hub.addEventTap { event ->
        if (event.event.startsWith(prefix)) tap(event.event.removePrefix(prefix), event.data)
    }

    fun removeEventTap(token: Any) = hub.removeEventTap(token)

    companion object {
        fun isUnsupported(error: Throwable): Boolean =
            error is LocalControlException && (error.code == "unsupported" || error.code == "unknown_method")
    }
}
