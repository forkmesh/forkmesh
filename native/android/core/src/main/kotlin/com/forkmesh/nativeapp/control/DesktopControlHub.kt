package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.api.PrefStore
import java.util.concurrent.Callable
import java.util.concurrent.ConcurrentHashMap
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

enum class DesktopPresence(val label: String) {
    Unknown("Not checked yet"),
    Checking("Checking…"),
    Online("Running"),
    Offline("Not running or not reachable"),
}

data class DesktopChoice(val id: String, val name: String, val presence: DesktopPresence, val active: Boolean) {
    val label: String get() = "Desktop $name, ${presence.label.lowercase()}" + if (active) ", in use" else ""
}

sealed class ShellLocalDesktop {
    data object Hidden : ShellLocalDesktop()

    data object InReach : ShellLocalDesktop()

    data class Connecting(val name: String) : ShellLocalDesktop()

    data class AwaitingApproval(val name: String) : ShellLocalDesktop()

    data class Lan(val name: String) : ShellLocalDesktop()

    data class Relay(val name: String) : ShellLocalDesktop()

    val isConnected: Boolean get() = this is Lan || this is Relay

    val label: String
        get() = when (this) {
            Hidden -> "No desktop connected"
            InReach -> "A desktop is on this network"
            is Connecting -> "Connecting to $name"
            is AwaitingApproval -> "Approve this phone on $name"
            is Lan -> "Connected to $name on this network"
            is Relay -> "Connected to $name through the relay"
        }
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
    private val prefs: PrefStore? = null,
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

    @Volatile
    var preferredKey: String = prefs?.getString(PREFERRED_KEY).orEmpty()
        private set
    private val presence = ConcurrentHashMap<String, DesktopPresence>()
    private val retired = java.util.Collections.newSetFromMap(java.util.WeakHashMap<RelayControlConnection, Boolean>())

    @Volatile
    private var checkedAt = 0L

    @Volatile
    var checkIntervalMs: Long = 300_000
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
    private val keyedTaps = DesktopKeyedTaps()
    private val sideLinks = DesktopSideLinks(openRelay, relayIdleProbeMs, keyedTaps) {
        notifyListeners()
        for ((_, listener) in linkListeners) runCatching { listener() }
    }

    @Volatile
    var reach: DesktopReach = DesktopReach.NotPaired
        private set

    @Volatile
    var revision: Int = 0
        private set

    init {
        local?.addListener(localListener)
        lanLink?.let { lan -> lan.addEventTap { event -> keyedTaps.deliver(lan.desktopKey, event) } }
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
            val first = if (out.any { it.key == preferredKey }) preferredKey else lastReachedKey
            val reached = out.indexOfFirst { it.key == first }
            if (reached > 0) out.add(0, out.removeAt(reached))
            return out
        }

    val choices: List<DesktopChoice>
        get() {
            val named = candidates.filter { it.name.isNotEmpty() }
            val active = currentLink?.let(::linkKey) ?: named.firstOrNull()?.key.orEmpty()
            return named.map { item ->
                val twins = named.count { it.name.equals(item.name, ignoreCase = true) } > 1
                DesktopChoice(
                    id = item.key,
                    name = if (twins) "${item.name} · ${item.key.take(6)}" else item.name,
                    presence = presenceOf(item.key),
                    active = item.key == active,
                )
            }
        }

    val localDesktop: ShellLocalDesktop
        get() {
            val service = local
            val lan = when (service?.status) {
                LocalControlStatus.Connected -> ShellLocalDesktop.Lan(service.desktopName)
                LocalControlStatus.Connecting -> ShellLocalDesktop.Connecting(service.desktopName)
                LocalControlStatus.AwaitingApproval -> ShellLocalDesktop.AwaitingApproval(service.desktopName)
                else -> if (service != null && service.hosts.isNotEmpty()) ShellLocalDesktop.InReach else ShellLocalDesktop.Hidden
            }
            val now = reach
            val onLan = currentLink is LanControlLink
            if (now is DesktopReach.Connected && now.transport == ControlTransport.Relay && !(lan.isConnected && onLan)) {
                return ShellLocalDesktop.Relay(now.name)
            }
            if (lan.isConnected && !onLan) {
                return if (now is DesktopReach.Connecting) ShellLocalDesktop.Connecting(now.name) else ShellLocalDesktop.Hidden
            }
            return lan
        }

    fun presenceOf(key: String): DesktopPresence {
        if (key.isEmpty()) return DesktopPresence.Unknown
        val lan = lanLink
        if (lan != null && lan.isOpen && lan.desktopKey == key) return DesktopPresence.Online
        val relayNow = synchronized(lock) { relay }
        if (relayNow != null && relayNow.isOpen && relayNow.desktopKey == key) return DesktopPresence.Online
        if (sideLinks.open(key) != null) return DesktopPresence.Online
        return presence[key] ?: DesktopPresence.Unknown
    }

    fun choose(key: String) {
        if (key == preferredKey) return
        preferredKey = key
        if (key.isEmpty()) prefs?.remove(PREFERRED_KEY) else prefs?.putString(PREFERRED_KEY, key)
        val current = synchronized(lock) { relay }
        val dropped = if (current != null && !wanted(current.desktopKey)) {
            synchronized(lock) {
                if (relay === current) {
                    relay = null
                    retired.add(current)
                    current
                } else {
                    null
                }
            }
        } else {
            null
        }
        if (dropped != null) {
            synchronized(lock) {
                attached?.takeIf { it.first === dropped }?.let {
                    it.first.removeEventTap(it.second)
                    attached = null
                }
            }
            if (!sideLinks.adopt(dropped)) Thread({ dropped.close() }, "DesktopControlHub").apply { isDaemon = true }.start()
        }
        refreshReach()
        revision++
        notifyListeners()
        announceLink()
        if (keepWarm && key.isNotEmpty() && currentLink == null) warm()
    }

    fun chooseNamed(name: String) {
        choices.firstOrNull { it.name == name }?.let { choose(it.id) }
    }

    fun checkDesktops(force: Boolean = false) {
        val opener = openRelay ?: return
        if (!force && checkedAt > 0 && clock() - checkedAt < checkIntervalMs) return
        checkedAt = clock()
        refreshAccountDesktops()
        val others = candidates.filter { it.name.isNotEmpty() && presenceOf(it.key) != DesktopPresence.Online }
        if (others.isEmpty()) return
        for (item in others) presence[item.key] = DesktopPresence.Checking
        revision++
        notifyListeners()
        val pool = Executors.newFixedThreadPool(others.size) { Thread(it, "DesktopCheck").apply { isDaemon = true } }
        val probes = others.map { item ->
            pool.submit(
                Callable {
                    item.key to runCatching {
                        opener.open(item).close()
                        true
                    }.getOrDefault(false)
                },
            )
        }
        pool.shutdown()
        for (probe in probes) {
            val (key, reached) = runCatching { probe.get() }.getOrNull() ?: continue
            if (presenceOf(key) != DesktopPresence.Online) presence[key] = if (reached) DesktopPresence.Online else DesktopPresence.Offline
        }
        revision++
        notifyListeners()
    }

    private fun wanted(key: String): Boolean {
        val preferred = preferredKey
        if (preferred.isEmpty() || key == preferred) return true
        return candidates.none { it.key == preferred && it.name.isNotEmpty() }
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
            lanLink?.takeIf { it.isOpen && wanted(it.desktopKey) }?.let { return it }
            return synchronized(lock) { relay }?.takeIf { it.isOpen && wanted(it.desktopKey) }
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
            if (awaitLan(lan) && wanted(lan.desktopKey)) {
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
            val named = candidates.filter { it.name.isNotEmpty() }
            val targets = named.firstOrNull { it.key == preferredKey }?.let { listOf(it) } ?: named.take(4)
            if (targets.isEmpty()) {
                refreshReach()
                notifyListeners()
                throw LocalControlException("not_paired")
            }
            reach = DesktopReach.Connecting(targets.first().name)
            for (target in targets) if (presenceOf(target.key) != DesktopPresence.Online) presence[target.key] = DesktopPresence.Checking
            notifyListeners()
            val kept = targets.firstNotNullOfOrNull { target -> sideLinks.take(target.key)?.let { it to target } }
            if (kept != null) synchronized(lock) { retired.remove(kept.first) }
            val (connection, target) = kept ?: race(targets, opener) { key, reached ->
                presence[key] = if (reached) DesktopPresence.Online else DesktopPresence.Offline
            }
            if (!wanted(target.key)) {
                connection.close()
                throw LocalControlException("not_connected")
            }
            lastReachedKey = target.key
            val previous = synchronized(lock) {
                relay.also {
                    relay = connection
                    if (it != null && it !== connection) retired.add(it)
                }
            }
            if (previous != null && previous !== connection) Thread({ previous.close() }, "DesktopControlHub").apply { isDaemon = true }.start()
            connection.onClose { reason ->
                val gone = synchronized(lock) {
                    if (relay === connection) relay = null
                    retired.remove(connection)
                }
                presence.remove(connection.desktopKey)
                if (gone) return@onClose
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
                if (choices.size > 1) checkDesktops()
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
        if (taps.removeAll { it.first === token }) return
        keyedTaps.remove(token)
    }

    fun addEventTap(desktopKey: String, tap: (ControlEvent) -> Unit): Any {
        if (desktopKey.isEmpty()) return addEventTap(tap)
        val token = keyedTaps.add(desktopKey, tap)
        currentLink?.let { attachTaps(it) }
        return token
    }

    fun linkKey(link: ControlLink): String = when (link) {
        is LanControlLink -> link.desktopKey
        is RelayControlConnection -> link.desktopKey
        else -> ""
    }

    val activeKey: String
        get() = currentLink?.let(::linkKey)?.takeIf { it.isNotEmpty() } ?: candidates.firstOrNull()?.key.orEmpty()

    val otherDesktops: List<PairedDesktop>
        get() {
            val active = activeKey
            return candidates.filter { it.name.isNotEmpty() && it.key != active }
        }

    fun link(desktopKey: String): ControlLink {
        if (desktopKey.isEmpty() || desktopKey == activeKey) return link()
        lanLink?.takeIf { it.isOpen && it.desktopKey == desktopKey }?.let { return it }
        synchronized(lock) { relay }?.takeIf { it.isOpen && it.desktopKey == desktopKey }?.let { return it }
        val target = candidates.firstOrNull { it.key == desktopKey && it.name.isNotEmpty() }
            ?: throw LocalControlException("not_paired")
        return sideLinks.link(target)
    }

    fun openLink(desktopKey: String): ControlLink? {
        if (desktopKey.isEmpty() || desktopKey == activeKey) return currentLink
        lanLink?.takeIf { it.isOpen && it.desktopKey == desktopKey }?.let { return it }
        synchronized(lock) { relay }?.takeIf { it.isOpen && it.desktopKey == desktopKey }?.let { return it }
        return sideLinks.open(desktopKey)
    }

    fun request(method: String, params: Map<String, Any?>, timeoutMs: Long, desktopKey: String): Map<String, Any?> =
        link(desktopKey).request(method, params, timeoutMs)

    fun channel(desktopKey: String): ControlCalling =
        if (desktopKey.isEmpty()) HubControlCalling(this) else DesktopScopedCalling(this, desktopKey)

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
            val token = link.addEventTap { event ->
                for ((_, tap) in taps) runCatching { tap(event) }
                if (link !is LanControlLink) keyedTaps.deliver(linkKey(link), event)
            }
            attached = link to token
        }
    }

    private fun localChanged() {
        val before = reach
        refreshReach()
        val lan = lanLink
        if (lan != null && lan.isOpen && wanted(lan.desktopKey)) {
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
            lan != null && lan.isOpen && wanted(lan.desktopKey) -> DesktopReach.Connected(ControlTransport.Lan, lan.desktopName)
            relayNow != null && relayNow.isOpen && wanted(relayNow.desktopKey) ->
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
        sideLinks.closeAll()
    }

    companion object {
        const val PREFERRED_KEY = "desktop/preferred-key"
    }

    private fun race(
        targets: List<PairedDesktop>,
        opener: RelayControlOpener,
        heard: (String, Boolean) -> Unit,
    ): Pair<RelayControlConnection, PairedDesktop> {
        fun dial(target: PairedDesktop): RelayControlConnection = try {
            opener.open(target).also { heard(target.key, true) }
        } catch (error: Throwable) {
            heard(target.key, false)
            throw error
        }
        if (targets.size == 1) return dial(targets[0]) to targets[0]
        val pool = Executors.newFixedThreadPool(targets.size) { Thread(it, "DesktopDial").apply { isDaemon = true } }
        val dials = ExecutorCompletionService<Pair<RelayControlConnection, PairedDesktop>>(pool)
        for (target in targets) dials.submit(Callable { dial(target) to target })
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
    Workflows("workflows."),
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
