package com.forkmesh.nativeapp.reticulum

import java.nio.ByteBuffer
import java.util.UUID

enum class InterfaceKind(val key: String) { TcpClient("tcp"), SharedInstance("shared"), Udp("udp") }

enum class LinkState(val label: String) {
    Disabled("Off"),
    Connecting("Connecting"),
    Online("Online"),
    Waiting("Retrying"),
    Blocked("Blocked"),
    Failed("Failed"),
}

data class InterfaceConfig(
    val id: String = "",
    val name: String = "",
    val kind: InterfaceKind = InterfaceKind.TcpClient,
    val enabled: Boolean = true,
    val host: String = "",
    val port: Int = 4242,
    val kiss: Boolean = false,
    val instanceName: String = "default",
    val sharedOverTcp: Boolean = true,
    val listenAddress: String = "0.0.0.0",
    val listenPort: Int = 4242,
    val forwardAddress: String = "255.255.255.255",
    val forwardPort: Int = 4242,
    val networkName: String = "",
    val passphrase: String = "",
    val accessCodeSize: Int = 16,
) {
    val usesAccessCode: Boolean get() = networkName.isNotEmpty() || passphrase.isNotEmpty()

    fun toJson(includePassphrase: Boolean = true): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>("id" to id, "name" to name, "kind" to kind.key, "enabled" to enabled)
        if (usesAccessCode) {
            out["networkName"] = networkName
            if (includePassphrase) out["passphrase"] = passphrase
            out["accessCodeSize"] = accessCodeSize.toLong()
        }
        when (kind) {
            InterfaceKind.TcpClient -> {
                out["host"] = host
                out["port"] = port.toLong()
                out["kiss"] = kiss
            }
            InterfaceKind.SharedInstance -> {
                out["instanceName"] = instanceName
                out["sharedOverTcp"] = sharedOverTcp
                out["port"] = port.toLong()
            }
            InterfaceKind.Udp -> {
                out["listenAddress"] = listenAddress
                out["listenPort"] = listenPort.toLong()
                out["forwardAddress"] = forwardAddress
                out["forwardPort"] = forwardPort.toLong()
            }
        }
        return out
    }

    val endpoint: String
        get() = when (kind) {
            InterfaceKind.TcpClient -> "${if (':' in host) "[$host]" else host}:$port"
            InterfaceKind.SharedInstance -> "127.0.0.1:$port"
            InterfaceKind.Udp -> "$listenAddress:$listenPort → $forwardAddress:$forwardPort"
        }

    val kindLabel: String
        get() = when (kind) {
            InterfaceKind.TcpClient -> if (kiss) "TCP (KISS)" else "TCP"
            InterfaceKind.SharedInstance -> "Shared instance"
            InterfaceKind.Udp -> "UDP"
        }

    val validationError: String
        get() {
            if (name.isEmpty() || name.length > 80) return "Give the interface a name of up to 80 characters."
            fun badPort(value: Int) = value < 1 || value > 65535
            if (usesAccessCode && (accessCodeSize < 1 || accessCodeSize > 64)) return "The access code size must be between 1 and 64 bytes."
            if (networkName.length > 256 || passphrase.length > 256) return "The network name and passphrase may be up to 256 characters."
            when (kind) {
                InterfaceKind.TcpClient -> {
                    if (!ReticulumConfig.validHost(host)) return "Enter a host name or IP address."
                    if (badPort(port)) return "The port must be between 1 and 65535."
                }
                InterfaceKind.SharedInstance -> if (badPort(port)) return "The port must be between 1 and 65535."
                InterfaceKind.Udp -> {
                    if (!ReticulumConfig.isIpLiteral(listenAddress) || !ReticulumConfig.isIpLiteral(forwardAddress)) {
                        return "UDP needs a listen address and a forward address."
                    }
                    if (badPort(listenPort) || badPort(forwardPort)) return "The ports must be between 1 and 65535."
                }
            }
            return ""
        }

    fun sameTarget(other: InterfaceConfig): Boolean = kind == other.kind && endpoint.equals(other.endpoint, ignoreCase = true)

    companion object {
        const val SHARED_INSTANCE_PORT = 37428

        fun newId(): String = UUID.randomUUID().toString()

        fun fromJson(json: Map<String, Any?>): InterfaceConfig? {
            val kind = when (json["kind"]) {
                "shared" -> InterfaceKind.SharedInstance
                "udp" -> InterfaceKind.Udp
                "tcp" -> InterfaceKind.TcpClient
                else -> return null
            }
            fun text(key: String, fallback: String = "") = (json[key] as? String ?: fallback).trim()
            fun number(key: String, fallback: Int): Int = when (val value = json[key]) {
                is Long -> value.toInt()
                is Int -> value
                is Double -> if (value == kotlin.math.floor(value)) value.toInt() else fallback
                else -> fallback
            }
            fun flag(key: String, fallback: Boolean) = json[key] as? Boolean ?: fallback
            val config = InterfaceConfig(
                id = (json["id"] as? String).orEmpty().ifEmpty { newId() },
                name = text("name"),
                kind = kind,
                enabled = flag("enabled", true),
                host = text("host"),
                port = number("port", if (kind == InterfaceKind.SharedInstance) SHARED_INSTANCE_PORT else 4242),
                kiss = flag("kiss", false),
                instanceName = text("instanceName", "default"),
                sharedOverTcp = flag("sharedOverTcp", true),
                listenAddress = text("listenAddress", "0.0.0.0"),
                listenPort = number("listenPort", 4242),
                forwardAddress = text("forwardAddress", "255.255.255.255"),
                forwardPort = number("forwardPort", 4242),
                networkName = json["networkName"] as? String ?: "",
                passphrase = json["passphrase"] as? String ?: "",
                accessCodeSize = number("accessCodeSize", 16),
            )
            return if (config.validationError.isEmpty()) config else null
        }
    }
}

data class TrafficSample(val rx: Long = 0, val tx: Long = 0, val announces: Int = 0)

private class TrafficCounter {
    var rx = 0L
    var tx = 0L
    var announces = 0

    fun sample() = TrafficSample(rx, tx, announces)
}

data class InterfaceStatus(
    val state: LinkState = LinkState.Disabled,
    val detail: String = "",
    val rxBytes: Long = 0,
    val txBytes: Long = 0,
    val rxPackets: Long = 0,
    val txPackets: Long = 0,
    val announces: Long = 0,
    val invalidFrames: Long = 0,
    val rejected: Long = 0,
    val connectMs: Long = -1,
    val attempts: Int = 0,
    val sessions: Int = 0,
    val onlineSince: Long = 0,
    val nextAttempt: Long = 0,
    val history: List<TrafficSample> = emptyList(),
)

data class DestinationRecord(
    val hash: ByteArray,
    val identityHash: ByteArray = ByteArray(0),
    val publicKey: ByteArray = ByteArray(0),
    val nameHash: ByteArray = ByteArray(0),
    val transportId: ByteArray = ByteArray(0),
    val appData: ByteArray = ByteArray(0),
    val aspect: String = "",
    val label: String = "",
    val kind: String = "",
    val displayName: String = "",
    val interfaceId: String = "",
    val hops: Int = 0,
    val announces: Int = 0,
    val ratchet: Boolean = false,
    val ratchetKey: ByteArray = ByteArray(0),
    val viaSharedInstance: Boolean = false,
    val emittedAt: Long = 0,
    val firstHeard: Long = 0,
    val lastHeard: Long = 0,
    val discovery: DiscoveredInterface? = null,
) {
    val hex: String = Rns.hex(hash)
}

data class NetworkEvent(val at: Long, val kind: String, val text: String, val destination: ByteArray = ByteArray(0))

data class PathLookup(
    val destination: ByteArray,
    val tag: ByteArray,
    val started: Long,
    val answered: Long = 0,
    val hops: Int = -1,
    val interfaceId: String = "",
    val timedOut: Boolean = false,
)

data class PacketCounters(
    var data: Long = 0,
    var announces: Long = 0,
    var linkRequests: Long = 0,
    var proofs: Long = 0,
    var duplicates: Long = 0,
    var invalid: Long = 0,
    var rejectedAnnounces: Long = 0,
    var pathRequestsSeen: Long = 0,
    var pathRequestsAnswered: Long = 0,
    var announcesSent: Long = 0,
)

data class InterfaceView(val config: InterfaceConfig, val status: InterfaceStatus)

data class ReticulumSnapshot(
    val running: Boolean = false,
    val startedAt: Long = 0,
    val identityHash: ByteArray = ByteArray(0),
    val destinationHash: ByteArray = ByteArray(0),
    val announceName: String = "",
    val answerPathRequests: Boolean = true,
    val lastAnnounce: Long = 0,
    val interfaces: List<InterfaceView> = emptyList(),
    val destinations: Map<String, DestinationRecord> = emptyMap(),
    val lookups: Map<String, PathLookup> = emptyMap(),
    val events: List<NetworkEvent> = emptyList(),
    val counters: PacketCounters = PacketCounters(),
    val history: List<TrafficSample> = emptyList(),
    val at: Long = 0,
) {
    val onlineCount: Int get() = interfaces.count { it.status.state == LinkState.Online }
    val enabledCount: Int get() = interfaces.count { it.config.enabled }

    fun interfaceName(id: String): String =
        interfaces.firstOrNull { it.config.id == id }?.config?.name ?: if (id.isEmpty()) "—" else "Removed interface"
}

private data class PacketKey(val a: Long, val b: Long, val c: Long, val d: Long) {
    companion object {
        fun of(hash: ByteArray): PacketKey {
            val buffer = ByteBuffer.wrap(hash.copyOf(32))
            return PacketKey(buffer.long, buffer.long, buffer.long, buffer.long)
        }
    }
}

class ReticulumNode(
    private val transport: ReticulumTransport = SocketTransport(),
    internal val timers: ReticulumTimers = ExecutorTimers(),
    internal val clock: () -> Long = System::currentTimeMillis,
    internal val maxDestinations: Int = MAX_DESTINATIONS,
) {
    internal val lock = Any()
    private val local = java.util.TreeMap<String, () -> ByteArray>()
    private val lastPathAnswers = HashMap<String, Long>()
    private val runtimes = ArrayList<InterfaceRuntime>()
    private var changePending = false
    private var tickTimer: ReticulumTimer? = null
    private var running = false
    private var startedAt = 0L
    private var identity: RnsIdentity? = null
    private var announceName = ""
    private var answerPathRequests = true
    private var lastAnnounce = 0L
    private val destinations = HashMap<String, DestinationRecord>()
    private val lookups = HashMap<String, PathLookup>()
    private val events = ArrayDeque<NetworkEvent>()
    private val seen = HashSet<PacketKey>()
    private val seenOrder = ArrayDeque<PacketKey>()
    private val counters = PacketCounters()
    private val history = ArrayDeque<TrafficSample>()
    private val current = TrafficCounter()

    @Volatile var authorizer: ((method: String, url: String) -> Boolean)? = null
    @Volatile var changedHandler: (() -> Unit)? = null
    @Volatile var announceHandler: ((ByteArray) -> Unit)? = null
    @Volatile var packetHandler: ((interfaceId: String, packet: Packet, raw: ByteArray, hops: Int) -> Unit)? = null

    init {
        addLocalDestination(ASPECT) { announceName.toByteArray(Charsets.UTF_8) }
    }

    fun addLocalDestination(aspect: String, appData: () -> ByteArray) = synchronized(lock) { local[aspect] = appData }

    fun localDestinationHash(aspect: String): ByteArray = synchronized(lock) {
        val own = identity
        if (own == null || aspect !in local) ByteArray(0) else Rns.destinationHashForName(aspect, own.hash)
    }

    fun localAspect(hash: ByteArray): String = synchronized(lock) {
        val own = identity ?: return@synchronized ""
        if (hash.size != Rns.HASH_LENGTH) return@synchronized ""
        local.keys.firstOrNull { Rns.destinationHashForName(it, own.hash).contentEquals(hash) }.orEmpty()
    }

    fun isLocalDestination(hash: ByteArray): Boolean = localAspect(hash).isNotEmpty()

    fun identityFor(destination: ByteArray): RnsIdentity? = synchronized(lock) {
        destinations[Rns.hex(destination)]?.let { RnsIdentity.fromPublicKey(it.publicKey) }
    }

    fun interfaceOnline(id: String): Boolean = synchronized(lock) { runtime(id)?.online == true }

    fun sendRaw(raw: ByteArray, onlyInterface: String = ""): ByteArray = synchronized(lock) {
        if (raw.isEmpty()) return@synchronized ByteArray(0)
        rememberPacket(Rns.packetHash(raw))
        transmit(raw, onlyInterface)
        notifyChanged()
        raw
    }

    fun sendPacket(packet: Packet, onlyInterface: String = ""): ByteArray = synchronized(lock) {
        val plain = Packet(
            headerType = Rns.HEADER_1,
            contextFlag = packet.contextFlag,
            transportType = Rns.TRANSPORT_BROADCAST,
            destinationType = packet.destinationType,
            packetType = packet.packetType,
            hops = packet.hops,
            destinationHash = packet.destinationHash,
            context = packet.context,
            data = packet.data,
        )
        val raw = Rns.packPacket(plain)
        rememberPacket(Rns.packetHash(raw))
        val found = destinations[Rns.hex(packet.destinationHash)]
        val routable = packet.packetType != Rns.PACKET_ANNOUNCE && packet.destinationType != Rns.DESTINATION_PLAIN &&
            packet.destinationType != Rns.DESTINATION_GROUP && found != null && onlyInterface.isEmpty() && runtime(found.interfaceId)?.online == true
        if (routable && found != null) {
            if (found.hops > 1 || (found.hops == 1 && found.viaSharedInstance)) {
                val routed = Packet(
                    headerType = Rns.HEADER_2,
                    contextFlag = plain.contextFlag,
                    transportType = Rns.TRANSPORT_FORWARD,
                    destinationType = plain.destinationType,
                    packetType = plain.packetType,
                    hops = plain.hops,
                    transportId = if (found.transportId.size == Rns.HASH_LENGTH) found.transportId else found.hash,
                    destinationHash = plain.destinationHash,
                    context = plain.context,
                    data = plain.data,
                )
                transmit(Rns.packPacket(routed), found.interfaceId)
            } else {
                transmit(raw, found.interfaceId)
            }
        } else {
            transmit(raw, onlyInterface)
        }
        notifyChanged()
        raw
    }

    val aspect: String get() = ASPECT

    fun interfaces(): List<InterfaceConfig> = synchronized(lock) { runtimes.map { it.config } }

    fun setInterfaces(configs: List<InterfaceConfig>) = synchronized(lock) {
        val keep = configs.map { it.id }.toSet()
        for (runtime in runtimes.toList()) if (runtime.config.id !in keep) removeInterface(runtime.config.id)
        for (config in configs) upsertInterface(config)
        val ordered = configs.mapNotNull { config -> runtimes.firstOrNull { it.config.id == config.id } }
        runtimes.clear()
        runtimes.addAll(ordered)
        notifyChanged()
    }

    fun upsertInterface(config: InterfaceConfig) = synchronized(lock) {
        val existing = runtime(config.id)
        if (existing != null) {
            existing.setConfig(config)
        } else {
            val created = InterfaceRuntime(config)
            runtimes.add(created)
            if (running && config.enabled) created.start()
        }
        updateTicking()
        notifyChanged()
    }

    fun removeInterface(id: String) = synchronized(lock) {
        val found = runtime(id) ?: return@synchronized
        runtimes.remove(found)
        found.stop()
        updateTicking()
        notifyChanged()
    }

    fun setInterfaceEnabled(id: String, enabled: Boolean) = synchronized(lock) {
        val found = runtime(id) ?: return@synchronized
        found.setConfig(found.config.copy(enabled = enabled))
        if (enabled && running) found.reconnectNow()
        updateTicking()
        notifyChanged()
    }

    fun reconnectInterface(id: String) = synchronized(lock) {
        runtime(id)?.reconnectNow()
        updateTicking()
    }

    fun status(id: String): InterfaceStatus = synchronized(lock) { runtime(id)?.snapshot() ?: InterfaceStatus() }

    fun onlineCount(): Int = synchronized(lock) { runtimes.count { it.online } }

    fun start() = synchronized(lock) {
        if (running) return@synchronized
        running = true
        startedAt = clock()
        for (runtime in runtimes) if (runtime.config.enabled) runtime.start()
        updateTicking()
        notifyChanged()
    }

    fun stop() = synchronized(lock) {
        running = false
        for (runtime in runtimes) runtime.stop()
        updateTicking()
        notifyChanged()
    }

    fun close() {
        changedHandler = null
        announceHandler = null
        stop()
        timers.shutdown()
    }

    val isRunning: Boolean get() = synchronized(lock) { running }

    fun setIdentity(value: RnsIdentity) = synchronized(lock) {
        identity = value
        notifyChanged()
    }

    fun identity(): RnsIdentity? = synchronized(lock) { identity }

    fun destinationHash(): ByteArray = localDestinationHash(ASPECT)

    fun setAnnounceName(name: String) = synchronized(lock) {
        announceName = name.trim().take(64)
    }

    fun announceName(): String = synchronized(lock) { announceName }

    fun setAnswerPathRequests(answer: Boolean) = synchronized(lock) { answerPathRequests = answer }

    fun announceDestination(destination: ByteArray, pathResponse: Boolean = false, onlyInterface: String = ""): Boolean = synchronized(lock) {
        val aspect = localAspect(destination)
        val own = identity
        if (aspect.isEmpty() || own == null || !own.hasPrivateKey || runtimes.none { it.online }) return@synchronized false
        val appData = local[aspect]?.invoke() ?: ByteArray(0)
        val raw = Rns.buildAnnounce(own, aspect, appData, pathResponse, clock() / 1000)
        if (raw.isEmpty()) return@synchronized false
        rememberPacket(Rns.packetHash(raw))
        transmit(raw, onlyInterface)
        counters.announcesSent++
        if (!pathResponse) {
            lastAnnounce = clock()
            addEvent("announce-sent", "Announced $aspect as ${Rns.prettyHash(destination)}", destination)
        }
        notifyChanged()
        true
    }

    fun announce(pathResponse: Boolean = false, onlyInterface: String = ""): Boolean = synchronized(lock) {
        var sent = false
        for (aspect in local.keys.toList()) sent = announceDestination(localDestinationHash(aspect), pathResponse, onlyInterface) || sent
        sent
    }

    fun requestPath(destination: ByteArray): ByteArray? = synchronized(lock) {
        if (destination.size != Rns.HASH_LENGTH || runtimes.none { it.online }) return@synchronized null
        val tag = Rns.truncatedHash(Rns.randomBytes(32))
        val raw = Rns.buildPathRequest(destination, tag)
        rememberPacket(Rns.packetHash(raw))
        transmit(raw)
        lookups[Rns.hex(destination)] = PathLookup(destination, tag, clock())
        addEvent("path-request", "Asked the network for a path to ${Rns.prettyHash(destination)}", destination)
        updateTicking()
        notifyChanged()
        tag
    }

    fun lookups(): Map<String, PathLookup> = synchronized(lock) { HashMap(lookups) }

    fun destinations(): Map<String, DestinationRecord> = synchronized(lock) { HashMap(destinations) }

    fun destination(hash: ByteArray): DestinationRecord? = synchronized(lock) { destinations[Rns.hex(hash)] }

    fun forgetDestination(hash: ByteArray) = synchronized(lock) {
        destinations.remove(Rns.hex(hash))
        lookups.remove(Rns.hex(hash))
        notifyChanged()
    }

    fun clearDestinations() = synchronized(lock) {
        destinations.clear()
        lookups.clear()
        notifyChanged()
    }

    fun events(): List<NetworkEvent> = synchronized(lock) { events.toList() }

    fun counters(): PacketCounters = synchronized(lock) { counters.copy() }

    fun totalHistory(): List<TrafficSample> = synchronized(lock) { history.toList() }

    fun snapshot(): ReticulumSnapshot = synchronized(lock) {
        ReticulumSnapshot(
            running = running,
            startedAt = startedAt,
            identityHash = identity?.hash ?: ByteArray(0),
            destinationHash = destinationHash(),
            announceName = announceName,
            answerPathRequests = answerPathRequests,
            lastAnnounce = lastAnnounce,
            interfaces = runtimes.map { InterfaceView(it.config, it.snapshot()) },
            destinations = HashMap(destinations),
            lookups = HashMap(lookups),
            events = events.toList(),
            counters = counters.copy(),
            history = history.toList(),
            at = clock(),
        )
    }

    fun ingest(interfaceId: String, raw: ByteArray, fromSharedInstance: Boolean = false) = synchronized(lock) {
        val parsed = Rns.parsePacket(raw)
        val source = runtime(interfaceId)
        val packet = parsed.value
        if (packet == null) {
            counters.invalid++
            source?.let { it.invalidFrames++ }
            return@synchronized
        }
        val hops = if (fromSharedInstance) packet.hops else packet.hops + 1
        val repeatable = packet.context in REPEATABLE_CONTEXTS
        val fresh = rememberPacket(Rns.packetHash(raw)) || (repeatable && packet.packetType != Rns.PACKET_ANNOUNCE)
        if (packet.packetType != Rns.PACKET_ANNOUNCE && packet.headerType == Rns.HEADER_2 && !fromSharedInstance) {
            counters.duplicates++
            return@synchronized
        }
        when (packet.packetType) {
            Rns.PACKET_ANNOUNCE -> {
                counters.announces++
                current.announces++
                source?.let {
                    it.announces++
                    it.current.announces++
                }
                if (!fresh) {
                    counters.duplicates++
                    val key = Rns.hex(packet.destinationHash)
                    val found = destinations[key]
                    if (found != null && hops < found.hops) {
                        destinations[key] = found.copy(hops = hops, interfaceId = interfaceId, transportId = packet.transportId)
                        notifyChanged()
                    }
                    return@synchronized
                }
                handleAnnounce(interfaceId, packet, hops)
            }
            Rns.PACKET_DATA -> {
                counters.data++
                if (!fresh) {
                    counters.duplicates++
                    return@synchronized
                }
                if (packet.destinationType == Rns.DESTINATION_PLAIN && packet.destinationHash.contentEquals(Rns.pathRequestDestination)) {
                    handlePathRequest(interfaceId, packet)
                } else {
                    packetHandler?.invoke(interfaceId, packet, raw, hops)
                }
            }
            Rns.PACKET_LINK_REQUEST -> {
                counters.linkRequests++
                if (fresh) packetHandler?.invoke(interfaceId, packet, raw, hops)
            }
            Rns.PACKET_PROOF -> {
                counters.proofs++
                if (fresh) packetHandler?.invoke(interfaceId, packet, raw, hops)
            }
        }
        notifyChanged()
    }

    fun tickForTest() = synchronized(lock) { tick() }

    private fun runtime(id: String): InterfaceRuntime? = runtimes.firstOrNull { it.config.id == id }

    private fun interfaceName(id: String): String = runtime(id)?.config?.name ?: id

    private fun transmit(raw: ByteArray, onlyInterface: String = "") {
        for (runtime in runtimes) {
            if (onlyInterface.isNotEmpty() && runtime.config.id != onlyInterface) continue
            runtime.send(raw)
        }
    }

    private fun notifyChanged() {
        if (changePending) return
        changePending = true
        timers.schedule(CHANGE_DELAY_MS) {
            synchronized(lock) { changePending = false }
            changedHandler?.invoke()
        }
    }

    fun addEvent(kind: String, text: String, destination: ByteArray = ByteArray(0)) {
        events.addFirst(NetworkEvent(clock(), kind, text, destination))
        while (events.size > MAX_EVENTS) events.removeLast()
        notifyChanged()
    }

    private fun rememberPacket(hash: ByteArray): Boolean {
        if (hash.isEmpty()) return false
        val key = PacketKey.of(hash)
        if (!seen.add(key)) return false
        seenOrder.addLast(key)
        while (seenOrder.size > MAX_SEEN_PACKETS) seen.remove(seenOrder.removeFirst())
        return true
    }

    private fun handleAnnounce(interfaceId: String, packet: Packet, hops: Int) {
        val parsed = Rns.parseAnnounce(packet)
        val announce = parsed.value
        val error = if (announce == null) parsed.error else Rns.validateAnnounce(announce)
        if (announce == null || error != null) {
            counters.rejectedAnnounces++
            runtime(interfaceId)?.let { it.rejected++ }
            if (counters.rejectedAnnounces <= 20 || counters.rejectedAnnounces % 100 == 0L) {
                addEvent(
                    "rejected",
                    "Dropped an announce for ${Rns.prettyHash(packet.destinationHash)} from ${interfaceName(interfaceId)}: $error",
                    packet.destinationHash,
                )
            }
            return
        }
        if (isLocalDestination(announce.destinationHash)) return
        val now = clock()
        val key = Rns.hex(announce.destinationHash)
        val found = destinations[key]
        val known = found != null
        if (found != null && !found.publicKey.contentEquals(announce.publicKey)) {
            counters.rejectedAnnounces++
            addEvent(
                "rejected",
                "Ignored an announce for ${Rns.prettyHash(announce.destinationHash)} with a different key than before",
                announce.destinationHash,
            )
            return
        }
        if (found != null && announce.emittedAt < found.emittedAt && packet.context != Rns.CONTEXT_PATH_RESPONSE) {
            counters.duplicates++
            return
        }
        val aspect = Rns.knownAspect(announce.nameHash)
        val kind = aspect?.kind ?: "unknown"
        val appData = announce.appData.copyOf(minOf(announce.appData.size, 1024))
        var discovery = found?.discovery
        if (kind == "discovery" && (found == null || !found.appData.contentEquals(appData))) {
            discovery = Rns.parseDiscoveryAppData(announce.appData).value
        }
        var displayName = Rns.displayNameFromAppData(kind, announce.appData)
        if (discovery != null && discovery.name.isNotEmpty()) displayName = discovery.name
        val record = DestinationRecord(
            hash = announce.destinationHash,
            identityHash = announce.identityHash,
            publicKey = announce.publicKey,
            nameHash = announce.nameHash,
            transportId = packet.transportId,
            appData = appData,
            aspect = aspect?.name.orEmpty(),
            label = aspect?.label ?: "Unknown application",
            kind = kind,
            displayName = displayName,
            interfaceId = interfaceId,
            hops = hops,
            announces = (found?.announces ?: 0) + 1,
            ratchet = announce.ratchet.isNotEmpty(),
            ratchetKey = if (announce.ratchet.isNotEmpty()) announce.ratchet else found?.ratchetKey ?: ByteArray(0),
            viaSharedInstance = runtime(interfaceId)?.config?.kind == InterfaceKind.SharedInstance,
            emittedAt = maxOf(found?.emittedAt ?: 0, announce.emittedAt),
            firstHeard = found?.firstHeard ?: now,
            lastHeard = now,
            discovery = discovery,
        )
        destinations[key] = record
        announceHandler?.invoke(record.hash)
        val lookup = lookups[key]
        if (lookup != null && lookup.answered == 0L) {
            lookups[key] = lookup.copy(answered = now, hops = hops, interfaceId = interfaceId, timedOut = false)
            val who = record.displayName.ifEmpty { Rns.prettyHash(record.hash) }
            addEvent(
                "path-found",
                "Found a path to $who: $hops hop${if (hops == 1) "" else "s"} via ${interfaceName(interfaceId)} in ${now - lookup.started} ms",
                record.hash,
            )
        } else if (!known) {
            val who = if (record.displayName.isEmpty()) Rns.prettyHash(record.hash) else "${record.displayName} ${Rns.prettyHash(record.hash)}"
            addEvent(
                "announce",
                "$who · ${record.label} · $hops hop${if (hops == 1) "" else "s"} via ${interfaceName(interfaceId)}",
                record.hash,
            )
        }
        if (destinations.size > maxDestinations) trimDestinations()
    }

    private fun handlePathRequest(interfaceId: String, packet: Packet) {
        val request = Rns.parsePathRequest(packet) ?: return
        counters.pathRequestsSeen++
        val own = identity
        if (!answerPathRequests || request.tag.isEmpty() || !isLocalDestination(request.destination) || own == null || !own.hasPrivateKey) return
        val now = clock()
        val key = Rns.hex(request.destination)
        val last = lastPathAnswers[key] ?: 0L
        if (last != 0L && (now - last) / 1000 < PATH_ANSWER_GAP_SECONDS) return
        lastPathAnswers[key] = now
        if (announceDestination(request.destination, true, interfaceId)) {
            counters.pathRequestsAnswered++
            addEvent("path-answered", "Answered a path request for ${localAspect(request.destination)} on ${interfaceName(interfaceId)}", request.destination)
        }
    }

    private fun trimDestinations() {
        val excess = destinations.size - maxDestinations + maxDestinations / 10
        destinations.values.sortedBy { it.lastHeard }.take(excess).forEach { destinations.remove(it.hex) }
    }

    private fun tick() {
        history.addLast(current.sample())
        while (history.size > HISTORY_SECONDS) history.removeFirst()
        current.rx = 0
        current.tx = 0
        current.announces = 0
        for (runtime in runtimes) runtime.rotateHistory()
        val now = clock()
        for ((key, lookup) in lookups.entries.toList()) {
            if (lookup.answered == 0L && !lookup.timedOut && (now - lookup.started) / 1000 >= PATH_TIMEOUT_SECONDS) {
                lookups[key] = lookup.copy(timedOut = true)
                addEvent("path-timeout", "No path to ${Rns.prettyHash(lookup.destination)} within $PATH_TIMEOUT_SECONDS s", lookup.destination)
            }
        }
        notifyChanged()
    }

    private fun updateTicking() {
        val needed = running && runtimes.any { it.config.enabled }
        if (needed && tickTimer == null) {
            tickTimer = timers.every(1000) { synchronized(lock) { if (tickTimer != null) tick() } }
        } else if (!needed && tickTimer != null) {
            tickTimer?.cancel()
            tickTimer = null
        }
    }

    private inner class InterfaceRuntime(initial: InterfaceConfig) {
        var config: InterfaceConfig = initial
            private set
        var state = LinkState.Disabled
        var detail = ""
        var rxBytes = 0L
        var txBytes = 0L
        var rxPackets = 0L
        var txPackets = 0L
        var announces = 0L
        var invalidFrames = 0L
        var rejected = 0L
        var connectMs = -1L
        var attempts = 0
        var sessions = 0
        var onlineSince = 0L
        var nextAttempt = 0L
        val current = TrafficCounter()
        private val history = ArrayDeque<TrafficSample>()
        private val decoder = FrameDecoder(if (initial.kiss) FrameDecoder.Framing.Kiss else FrameDecoder.Framing.Hdlc)
        private var accessCode = InterfaceAccessCode(initial.networkName, initial.passphrase, initial.accessCodeSize)
        private var connection: ReticulumConnection? = null
        private var generation = 0
        private var retryToken = 0
        private var retryTimer: ReticulumTimer? = null
        private var connectTimer: ReticulumTimer? = null
        private var keepaliveTimer: ReticulumTimer? = null
        private var connectStarted = 0L
        private var active = false
        private var authorized = false

        val online: Boolean get() = state == LinkState.Online

        private val shared: Boolean get() = config.kind == InterfaceKind.SharedInstance

        fun snapshot() = InterfaceStatus(
            state, detail, rxBytes, txBytes, rxPackets, txPackets, announces, invalidFrames, rejected, connectMs,
            attempts, sessions, onlineSince, nextAttempt, history.toList(),
        )

        fun setConfig(value: InterfaceConfig) {
            val changed = !config.sameTarget(value) || config.kiss != value.kiss || config.networkName != value.networkName ||
                config.passphrase != value.passphrase || config.accessCodeSize != value.accessCodeSize
            val wasActive = active
            config = value
            if (changed) {
                authorized = false
                decoder.changeFraming(if (value.kiss) FrameDecoder.Framing.Kiss else FrameDecoder.Framing.Hdlc)
                accessCode = InterfaceAccessCode(value.networkName, value.passphrase, value.accessCodeSize)
            }
            if (!value.enabled) {
                stop()
                return
            }
            if (changed && wasActive) {
                stop()
                start()
            } else if (!wasActive && running) {
                start()
            }
        }

        fun start() {
            if (!config.enabled) {
                setState(LinkState.Disabled)
                return
            }
            active = true
            cancelRetry()
            dropConnection()
            decoder.reset()
            attempts++
            connectStarted = clock()
            when (config.kind) {
                InterfaceKind.TcpClient, InterfaceKind.SharedInstance -> connectTcp()
                InterfaceKind.Udp -> bindUdp()
            }
        }

        fun stop() {
            active = false
            cancelRetry()
            keepaliveTimer?.cancel()
            keepaliveTimer = null
            dropConnection()
            nextAttempt = 0
            setState(LinkState.Disabled, if (config.enabled) "Stopped" else "")
        }

        fun reconnectNow() {
            authorized = false
            attempts = 0
            if (!config.enabled) return
            stop()
            start()
        }

        private fun authorize(method: String, url: String): Boolean {
            if (authorized) return true
            val check = authorizer
            if (check == null || check(method, url)) {
                authorized = true
                return true
            }
            return false
        }

        private fun connectTcp() {
            val host = if (shared) "127.0.0.1" else config.host
            val port = config.port
            if (!authorize("Reticulum", "tcp://${if (':' in host) "[$host]" else host}:$port")) {
                setState(LinkState.Blocked, "The ForkMesh firewall did not allow ${config.endpoint}")
                active = false
                return
            }
            setState(LinkState.Connecting, "Connecting to ${config.endpoint}")
            val attempt = ++generation
            connectTimer = timers.schedule(CONNECT_TIMEOUT_MS.toLong()) {
                synchronized(lock) {
                    if (attempt == generation && !online) lostConnection("No answer from ${config.endpoint} after ${CONNECT_TIMEOUT_MS / 1000} s")
                }
            }
            connection = transport.openTcp(host, port, CONNECT_TIMEOUT_MS, Events(attempt, datagrams = false))
        }

        private fun bindUdp() {
            if (!authorize("Reticulum UDP", "udp://${config.forwardAddress}:${config.forwardPort}")) {
                setState(LinkState.Blocked, "The ForkMesh firewall did not allow ${config.forwardAddress}")
                active = false
                return
            }
            val attempt = ++generation
            connection = try {
                transport.openUdp(config.listenAddress, config.listenPort, config.forwardAddress, config.forwardPort, UDP_MTU, Events(attempt, datagrams = true))
            } catch (problem: Exception) {
                scheduleRetry("Could not listen on ${config.listenAddress}:${config.listenPort}: ${problem.message ?: problem.javaClass.simpleName}")
                return
            }
            wentOnline()
        }

        private fun setState(value: LinkState, text: String = "") {
            if (value != LinkState.Online) onlineSince = 0
            state = value
            detail = text
            notifyChanged()
        }

        private fun wentOnline() {
            connectTimer?.cancel()
            connectTimer = null
            connectMs = clock() - connectStarted
            onlineSince = clock()
            attempts = 0
            nextAttempt = 0
            sessions++
            if (shared) {
                keepaliveTimer?.cancel()
                val attempt = generation
                keepaliveTimer = timers.every(KEEPALIVE_MS) {
                    synchronized(lock) { if (attempt == generation && online) connection?.write(Rns.hdlcKeepalive()) }
                }
            }
            setState(
                LinkState.Online,
                if (config.kind == InterfaceKind.Udp) "Listening on ${config.listenAddress}:${config.listenPort}" else "Connected in ${elapsedText(connectMs)}",
            )
            addEvent("interface", "${config.name} is online")
        }

        fun lostConnection(reason: String) {
            if (!active) return
            val wasOnline = online
            keepaliveTimer?.cancel()
            keepaliveTimer = null
            if (wasOnline) addEvent("interface", "${config.name} went offline: $reason")
            scheduleRetry(reason)
        }

        private fun scheduleRetry(reason: String) {
            dropConnection()
            val exponent = (attempts - 1).coerceIn(0, 5)
            val delay = minOf(RETRY_MAX_MS, RETRY_FIRST_MS shl exponent)
            nextAttempt = clock() + delay
            setState(LinkState.Waiting, reason)
            cancelRetry()
            val token = retryToken
            retryTimer = timers.schedule(delay.toLong()) {
                synchronized(lock) { if (token == retryToken && active && runtimes.contains(this)) start() }
            }
        }

        private fun cancelRetry() {
            retryToken++
            retryTimer?.cancel()
            retryTimer = null
        }

        private fun dropConnection() {
            generation++
            connectTimer?.cancel()
            connectTimer = null
            connection?.close()
            connection = null
        }

        fun receive(bytes: ByteArray, count: Int) {
            rxBytes += count
            current.rx += count
            this@ReticulumNode.current.rx += count
            val before = decoder.invalidFrames
            val frames = decoder.feed(bytes, count)
            invalidFrames += decoder.invalidFrames - before
            for (frame in frames) receiveFrame(frame)
        }

        fun receiveDatagram(bytes: ByteArray) {
            rxBytes += bytes.size
            current.rx += bytes.size
            this@ReticulumNode.current.rx += bytes.size
            if (bytes.size > UDP_MTU || bytes.size <= Rns.HEADER_MIN_SIZE) {
                invalidFrames++
                return
            }
            receiveFrame(bytes)
        }

        private fun receiveFrame(frame: ByteArray) {
            rxPackets++
            if (accessCode.enabled) {
                val inner = accessCode.remove(frame)
                if (inner == null) {
                    invalidFrames++
                    return
                }
                ingest(config.id, inner, shared)
                return
            }
            if (frame.isNotEmpty() && frame[0].toInt() and 0x80 != 0) {
                invalidFrames++
                return
            }
            ingest(config.id, frame, shared)
        }

        fun send(packet: ByteArray): Boolean {
            val link = connection
            if (!online || packet.isEmpty() || link == null) return false
            val raw = if (accessCode.enabled) accessCode.apply(packet) else packet
            val written = if (config.kind == InterfaceKind.Udp) {
                if (raw.size > UDP_MTU) return false
                if (link.write(raw)) raw.size else -1
            } else {
                val framed = if (config.kiss) Rns.kissFrame(raw) else Rns.hdlcFrame(raw)
                if (link.write(framed)) framed.size else -1
            }
            if (written <= 0) return false
            txPackets++
            txBytes += written
            current.tx += written
            this@ReticulumNode.current.tx += written
            return true
        }

        fun rotateHistory() {
            history.addLast(current.sample())
            while (history.size > HISTORY_SECONDS) history.removeFirst()
            current.rx = 0
            current.tx = 0
            current.announces = 0
        }

        private inner class Events(private val attempt: Int, private val datagrams: Boolean) : ReticulumConnectionEvents {
            override fun connected() = synchronized(lock) {
                if (attempt == generation) wentOnline()
            }

            override fun received(bytes: ByteArray, count: Int) = synchronized(lock) {
                if (attempt != generation) return@synchronized
                if (datagrams) receiveDatagram(bytes.copyOf(count)) else receive(bytes, count)
            }

            override fun failed(reason: String) = synchronized(lock) {
                if (attempt != generation) return@synchronized
                lostConnection(
                    if (shared && reason == SocketTransport.REFUSED) "No Reticulum app on this phone shares an instance on port ${config.port} (start Sideband or another RNS app with a shared instance)"
                    else reason,
                )
            }

            override fun closed() = synchronized(lock) {
                if (attempt == generation) lostConnection("${config.endpoint} closed the connection")
            }
        }
    }

    companion object {
        const val ASPECT = "forkmesh.node"
        const val HISTORY_SECONDS = 180
        const val MAX_DESTINATIONS = 20000
        const val MAX_EVENTS = 400
        const val MAX_SEEN_PACKETS = 60000
        const val PATH_TIMEOUT_SECONDS = 20
        const val PATH_ANSWER_GAP_SECONDS = 10
        const val RETRY_FIRST_MS = 5000
        const val RETRY_MAX_MS = 120000
        const val CONNECT_TIMEOUT_MS = 10000
        const val KEEPALIVE_MS = 60000L
        const val UDP_MTU = 1064
        const val CHANGE_DELAY_MS = 150L
        val REPEATABLE_CONTEXTS = setOf(
            Rns.CONTEXT_KEEPALIVE, Rns.CONTEXT_RESOURCE_REQUEST, Rns.CONTEXT_RESOURCE_PROOF, Rns.CONTEXT_RESOURCE,
            Rns.CONTEXT_CACHE_REQUEST, Rns.CONTEXT_CHANNEL,
        )

        fun elapsedText(ms: Long): String = if (ms < 1000) "$ms ms" else "%.1f s".format(java.util.Locale.ROOT, ms / 1000.0)
    }
}

object ReticulumConfig {
    private val hostPattern = Regex("^(?=.{1,253}$)([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?$")
    private val ipv4Pattern = Regex("^(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)(\\.(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)){3}$")
    private val ipv6Chars = Regex("^[0-9A-Fa-f:.]+(%[A-Za-z0-9_.-]+)?$")

    fun isIpLiteral(text: String): Boolean {
        if (ipv4Pattern.matches(text)) return true
        if (':' !in text || !ipv6Chars.matches(text)) return false
        return runCatching { java.net.InetAddress.getByName(text) is java.net.Inet6Address }.getOrDefault(false)
    }

    fun validHost(host: String): Boolean = isIpLiteral(host) || hostPattern.matches(host)

    fun publicEntryPoints(): List<InterfaceConfig> {
        val first = InterfaceConfig(id = "public-rmap-1", name = "RMAP Backbone 1", host = "rmap.world", port = 4242)
        return listOf(first, first.copy(id = "public-rmap-2", name = "RMAP Backbone 2", port = 4343))
    }

    fun defaultInterfaces(): List<InterfaceConfig> = listOf(
        InterfaceConfig(
            id = "shared-instance",
            name = "Reticulum on this phone",
            kind = InterfaceKind.SharedInstance,
            port = InterfaceConfig.SHARED_INSTANCE_PORT,
            sharedOverTcp = true,
        ),
    )

    private fun truthy(value: String?): Boolean = value?.trim()?.lowercase() in setOf("yes", "true", "1", "on")

    private val subsection = Regex("^\\[\\[\\s*(.+?)\\s*\\]\\]$")
    private val top = Regex("^\\[\\s*([^\\[\\]]+?)\\s*\\]$")
    private val pair = Regex("^([A-Za-z0-9_]+)\\s*=\\s*(.*)$")

    fun parse(text: String, skipped: MutableList<String>? = null): List<InterfaceConfig> {
        val out = ArrayList<InterfaceConfig>()
        var section = ""
        var current = ""
        val values = HashMap<String, String>()
        val reticulum = HashMap<String, String>()
        fun number(value: String?): Int = value?.toIntOrNull() ?: 0
        fun flush() {
            if (current.isEmpty()) return
            val type = values["type"].orEmpty()
            var config = InterfaceConfig(
                id = InterfaceConfig.newId(),
                name = current.take(80),
                enabled = truthy(values["enabled"]) || truthy(values["interface_enabled"]),
            )
            var accepted = false
            when {
                type == "TCPClientInterface" -> {
                    config = config.copy(
                        kind = InterfaceKind.TcpClient,
                        host = values["target_host"].orEmpty(),
                        port = number(values["target_port"]),
                        kiss = truthy(values["kiss_framing"]),
                    )
                    accepted = true
                }
                type == "BackboneInterface" && "remote" in values -> {
                    config = config.copy(
                        kind = InterfaceKind.TcpClient,
                        host = values["remote"].orEmpty(),
                        port = number(values["target_port"] ?: values["port"]),
                    )
                    accepted = true
                }
                type == "UDPInterface" -> {
                    val both = number(values["port"])
                    config = config.copy(
                        kind = InterfaceKind.Udp,
                        listenAddress = values["listen_ip"] ?: "0.0.0.0",
                        forwardAddress = values["forward_ip"] ?: "255.255.255.255",
                        listenPort = if (both > 0) both else number(values["listen_port"]),
                        forwardPort = if (both > 0) both else number(values["forward_port"]),
                    )
                    accepted = true
                }
            }
            config = config.copy(
                networkName = values["networkname"] ?: values["network_name"] ?: "",
                passphrase = values["passphrase"] ?: values["pass_phrase"] ?: "",
            )
            values["ifac_size"]?.let { config = config.copy(accessCodeSize = maxOf(1, number(it) / 8)) }
            if (accepted && config.validationError.isEmpty()) {
                out.add(config)
            } else {
                skipped?.add("$current (${type.ifEmpty { "no type" }})")
            }
            current = ""
            values.clear()
        }
        for (rawLine in text.split('\n')) {
            var line = rawLine
            val hash = line.indexOf('#')
            if (hash >= 0) line = line.substring(0, hash)
            line = line.trim()
            if (line.isEmpty()) continue
            val inner = subsection.matchEntire(line)
            if (inner != null) {
                flush()
                if (section == "interfaces") current = inner.groupValues[1]
                continue
            }
            val heading = top.matchEntire(line)
            if (heading != null) {
                flush()
                section = heading.groupValues[1].lowercase()
                continue
            }
            val match = pair.matchEntire(line) ?: continue
            var value = match.groupValues[2].trim()
            if (value.length >= 2 && (value.startsWith('"') || value.startsWith('\'')) && value.endsWith(value[0])) {
                value = value.substring(1, value.length - 1)
            }
            val key = match.groupValues[1].lowercase()
            if (current.isNotEmpty()) values[key] = value else if (section == "reticulum") reticulum[key] = value
        }
        flush()
        if ("share_instance" !in reticulum || truthy(reticulum["share_instance"])) {
            var shared = defaultInterfaces().first().copy(id = InterfaceConfig.newId())
            reticulum["instance_name"]?.let { shared = shared.copy(instanceName = it) }
            if (reticulum["shared_instance_type"]?.lowercase() == "tcp") shared = shared.copy(sharedOverTcp = true)
            reticulum["shared_instance_port"]?.let { shared = shared.copy(port = number(it)) }
            if (shared.validationError.isEmpty()) out.add(0, shared)
        }
        return out
    }
}
