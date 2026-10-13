package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.dartNumEquals
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.SecretStore
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.time.Instant
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import jdk.net.ExtendedSocketOptions

const val MESH_CONVERSATION = "#mesh"
const val MESH_ROOM_NAME = "forkmesh-lan-mesh-v1"
const val MESH_MULTICAST_GROUP = "239.192.79.42"
const val MESH_DISCOVERY_PORT = 48732
const val MESH_BEACON_TAG = "forkmesh-lan-mesh"
const val MESH_PROTOCOL_VERSION = 1

private const val MAX_FRAME_BYTES = 24 * 1024 * 1024
private const val MAX_TEXT_CHARS = 16000
private const val MAX_NAME_CHARS = 32
private const val MAX_WAKE_TOKEN_CHARS = 200
private const val MAX_WAKE_TOKENS = 32
private const val BEACON_INTERVAL_MS = 15000L
private const val PEER_TIMEOUT_MS = 60000L
private const val SWEEP_INTERVAL_MS = 5000L
private const val SEEN_CACHE_LIMIT = 4096
private const val HISTORY_LIMIT = 200
private const val MAX_LINKS = 64

private const val HISTORY_SAVE_DELAY_MS = 1000L
private const val HISTORY_FILE_VERSION = 1L

private const val ADDRESS_SWEEP_EVERY_BEATS = 4

private const val ADDRESS_SWEEP_EVERY_BEATS_ALONE = 2

private const val SWEEP_BATCH = 16
private const val SWEEP_BATCH_GAP_MS = 500L

private const val DIAL_TIMEOUT_MS = 8000
private const val REDIAL_DELAY_MS = 1500L
private const val REDIAL_MIN_LINK_MS = 5000L
private const val BLUETOOTH_REDIAL_MS = 5000L
private const val LINK_REPLACE_AFTER_MS = 10000L
private const val HANDSHAKE_TIMEOUT_MS = 15000L
private const val WRITE_STALL_MS = 20000L
private const val STOP_DRAIN_MS = 1000L
private const val ANNOUNCE_DEBOUNCE_MS = 1000L
const val MESH_KEEPALIVE_IDLE_S = 10
const val MESH_KEEPALIVE_INTERVAL_S = 5
const val MESH_KEEPALIVE_PROBES = 3
const val MESH_UNACKED_TIMEOUT_MS = 30000

private val SUPERSEDED_REASONS = setOf("duplicate link", "replaced", "identity changed")

private val SKIPPED_INTERFACE_PREFIXES = listOf(
    "rmnet", "rev_rmnet", "ccmni", "clat", "v4-", "tun", "ppp", "pdp", "wwan", "dummy", "ifb", "sit",
    "ip6tnl", "ip6gre", "gre", "docker", "br-", "veth", "virbr", "lxc", "utun", "ipsec", "wg",
)

fun isMeshInterfaceName(name: String): Boolean = SKIPPED_INTERFACE_PREFIXES.none { name.startsWith(it) }

private fun addressKey(address: InetAddress): String {
    val raw = address.address
    if (address is Inet6Address && raw.size == 16 && raw.take(10).all { it.toInt() == 0 } &&
        raw[10] == 0xff.toByte() && raw[11] == 0xff.toByte()
    ) {
        return Inet4Address.getByAddress(raw.copyOfRange(12, 16)).hostAddress ?: ""
    }
    return (address.hostAddress ?: "").substringBefore('%')
}

fun isLocalNetworkAddress(address: InetAddress): Boolean {
    val raw = address.address.map { it.toInt() and 0xff }
    if (address is Inet4Address) {
        if (raw.size != 4) return false
        if (raw[0] == 127) return true
        if (raw[0] == 10) return true
        if (raw[0] == 172 && raw[1] in 16..31) return true
        if (raw[0] == 192 && raw[1] == 168) return true
        if (raw[0] == 169 && raw[1] == 254) return true
        return false
    }
    if (address !is Inet6Address || raw.size != 16) return false
    if (raw.take(15).all { it == 0 } && raw[15] == 1) return true
    if (raw.take(10).all { it == 0 } && raw[10] == 0xff && raw[11] == 0xff) {
        return isLocalNetworkAddress(Inet4Address.getByAddress(address.address.copyOfRange(12, 16)))
    }
    if ((raw[0] and 0xfe) == 0xfc) return true
    return raw[0] == 0xfe && (raw[1] and 0xc0) == 0x80
}

fun sweepAddresses(address: InetAddress): List<InetAddress> {
    if (address !is Inet4Address) return emptyList()
    val raw = address.address
    if (raw.size != 4 || !isLocalNetworkAddress(address)) return emptyList()
    val a = raw[0].toInt() and 0xff
    val b = raw[1].toInt() and 0xff
    if (a == 127) return emptyList()
    if (a == 169 && b == 254) return emptyList()
    val own = raw[3].toInt() and 0xff
    val out = ArrayList<InetAddress>(253)
    for (host in 1 until 255) {
        if (host == own) continue
        out.add(InetAddress.getByAddress(byteArrayOf(raw[0], raw[1], raw[2], host.toByte())))
    }
    return out
}

fun sweepBroadcastAddress(address: InetAddress): InetAddress? {
    if (sweepAddresses(address).isEmpty()) return null
    val raw = address.address
    return InetAddress.getByAddress(byteArrayOf(raw[0], raw[1], raw[2], 255.toByte()))
}

data class MeshControlHost(
    val nodeId: String,
    val address: InetAddress,
    val port: Int,
    val name: String = "",
    val nodeName: String = "",
    val ownerUser: String = "",
    val platform: String = "",
) {
    val label: String
        get() = nodeName.ifEmpty { name.ifEmpty { nodeId.take(8) } }
}

class MeshDiagnostics {
    var multicastSent = 0
    var broadcastSent = 0
    var unicastSent = 0
    var sweepSent = 0
    var sendErrors = 0
    var lastSendError = ""
    var datagramsIn = 0
    var ownBeaconsIn = 0
    var offNetworkIn = 0
    var undecryptableIn = 0
    var peerBeaconsIn = 0
    var lastBeaconFrom = ""
    var lastBeaconMs = 0L
    var dials = 0
    var dialsOk = 0
    var dialsFailed = 0
    var lastDialError = ""
    var inboundAccepted = 0
    var inboundRefused = 0
    var framesIn = 0
    var framesBad = 0
    var chatIn = 0
    var chatOut = 0
    var lastDropReason = ""
    var listenError = ""
    var discoveryError = ""
    var multicastJoinError = ""
    var startNote = ""
    var interfaces: List<String> = emptyList()
}

internal fun meshAgeText(now: Long, thenMs: Long): String {
    if (thenMs <= 0) return "never"
    val seconds = ((now - thenMs) / 1000).coerceAtLeast(0)
    if (seconds < 120) return "${seconds}s ago"
    if (seconds < 7200) return "${seconds / 60}m ago"
    return "${seconds / 3600}h ago"
}

private fun meshErrorText(error: Throwable): String = error.message ?: error.javaClass.simpleName

data class MeshBeacon(
    val nodeId: String,
    val port: Int,
    val name: String = "",
    val nodeName: String = "",
    val ownerUser: String = "",
    val accountKind: String = "",
    val platform: String = "",
    val version: String = "",
    val timestampMs: Long = 0,
    val reply: Boolean = false,
    val control: Boolean = false,
) {
    fun toJson(): LinkedHashMap<String, Any?> {
        val out = linkedMapOf<String, Any?>(
            "t" to MESH_BEACON_TAG,
            "v" to MESH_PROTOCOL_VERSION.toLong(),
            "id" to nodeId,
            "port" to port.toLong(),
            "ts" to timestampMs,
        )
        if (name.isNotEmpty()) out["name"] = name
        if (nodeName.isNotEmpty()) out["nodeName"] = nodeName
        if (ownerUser.isNotEmpty()) out["ownerUser"] = ownerUser
        if (accountKind.isNotEmpty()) out["accountKind"] = accountKind
        if (platform.isNotEmpty()) out["platform"] = platform
        if (version.isNotEmpty()) out["version"] = version
        if (reply) out["reply"] = true
        if (control) out["control"] = true
        return out
    }

    companion object {
        fun parse(obj: Map<String, Any?>): MeshBeacon? {
            if (obj["t"] != MESH_BEACON_TAG || !dartNumEquals(obj["v"], MESH_PROTOCOL_VERSION.toLong())) return null
            val id = dartTrimText(jsonText(obj["id"]))
            val port = obj["port"]
            val portValue = if (port is Long) port else dartIntTryParse(if (port == null) "null" else jsonText(port)) ?: 0L
            if (id.isEmpty() || portValue <= 0 || portValue > 65535) return null
            fun bounded(key: String, max: Int = MAX_NAME_CHARS): String {
                val value = jsonText(obj[key])
                return if (value.length > max) value.substring(0, max) else value
            }
            val ts = obj["ts"]
            return MeshBeacon(
                nodeId = if (id.length > 160) id.substring(0, 160) else id,
                port = portValue.toInt(),
                name = bounded("name"),
                nodeName = bounded("nodeName"),
                ownerUser = bounded("ownerUser"),
                accountKind = bounded("accountKind", 16),
                platform = bounded("platform", 16),
                version = bounded("version"),
                timestampMs = if (ts is Number) ts.toLong() else 0L,
                reply = obj["reply"] == true,
                control = obj["control"] == true,
            )
        }
    }
}

class MeshFrameException(message: String) : Exception(message)

fun frameBytes(payload: ByteArray): ByteArray {
    val out = ByteArray(payload.size + 4)
    val length = payload.size
    out[0] = (length shr 24).toByte()
    out[1] = (length shr 16).toByte()
    out[2] = (length shr 8).toByte()
    out[3] = length.toByte()
    payload.copyInto(out, 4)
    return out
}

fun takeFrame(buffer: BytesBuilderBuffer): ByteArray? {
    if (buffer.length < 4) return null
    val head = buffer.peek(4)
    val length = ((head[0].toLong() and 0xff) shl 24) or ((head[1].toLong() and 0xff) shl 16) or
        ((head[2].toLong() and 0xff) shl 8) or (head[3].toLong() and 0xff)
    if (length <= 0 || length > MAX_FRAME_BYTES) throw MeshFrameException("mesh frame length out of range")
    if (buffer.length < length + 4) return null
    buffer.discard(4)
    return buffer.take(length.toInt())
}

class BytesBuilderBuffer {
    private var bytes = ByteArray(0)
    private var start = 0
    private var end = 0

    val length: Int get() = end - start

    fun add(data: ByteArray) {
        if (data.isEmpty()) return
        val size = length
        if (end + data.size > bytes.size) {
            val needed = size + data.size
            if (needed > bytes.size) {
                val grown = ByteArray(if (needed > bytes.size * 2) needed else bytes.size * 2)
                bytes.copyInto(grown, 0, start, end)
                bytes = grown
            } else {
                bytes.copyInto(bytes, 0, start, end)
            }
            start = 0
            end = size
        }
        data.copyInto(bytes, end)
        end += data.size
    }

    fun peek(count: Int): ByteArray = bytes.copyOfRange(start, start + count)

    fun discard(count: Int) {
        start += count
        if (start >= end) {
            start = 0
            end = 0
        }
    }

    fun take(count: Int): ByteArray = peek(count).also { discard(count) }
}

fun meshWakeTokenExpiry(token: String): Long? {
    if (token.isEmpty() || token.length > MAX_WAKE_TOKEN_CHARS) return null
    val parts = token.split(".")
    if (parts.size != 4 || parts[0] != "mw1") return null
    return parts[2].toLongOrNull()?.takeIf { it > 0 }
}

internal fun dartIntTryParse(source: String): Long? {
    var text = dartTrimText(source)
    if (text.isEmpty()) return null
    var negative = false
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-'
        text = text.substring(1)
    }
    val radix = if (text.length > 2 && (text.startsWith("0x") || text.startsWith("0X"))) 16 else 10
    if (radix == 16) text = text.substring(2)
    if (text.isEmpty() || !text.all { it.code < 128 && Character.digit(it, radix) >= 0 }) return null
    return (if (negative) "-$text" else text).toLongOrNull(radix)
}

data class MeshSnapshot(
    val running: Boolean,
    val networkActive: Boolean,
    val bluetoothActive: Boolean,
    val unread: Int,
    val messages: List<ChatMessage>,
    val members: List<Member>,
    val peerCount: Int,
    val queued: Set<String> = emptySet(),
) {
    val hasUnread: Boolean get() = unread > 0
}

class LocalMeshRoom(
    private val settings: AccountSettings,
    private val identity: DeviceIdentity,
    private val secureStore: SecretStore,
    private val native: MeshNativeTransport = MeshNativeTransport.UNAVAILABLE,
    private val platform: String = "android",
    val loop: EventLoop = EventLoop("LocalMeshRoom"),
    private val historyFile: () -> File? = { null },
) {
    private class MeshLink(val socket: Socket?, val bluetoothId: Int?, val outbound: Boolean, val writer: EventLoop.SerialQueue) {
        val buffer = BytesBuilderBuffer()
        val pending = ArrayList<Map<String, Any?>>()
        var peerId = ""
        var helloMs = 0L
        var keyKnown = false
        var keyId = ""
        var closed = false
        var output: OutputStream? = null

        @Volatile
        var writeStartedMs = -1L
    }

    private class MeshPeer {
        var name = ""
        var nodeName = ""
        var ownerUser = ""
        var platform = ""
        var version = ""
        var address: InetAddress? = null
        var port = 0
        var lastSeenMs = 0L

        var unlinkedSinceMs = 0L
        var linked = false
        var control = false
    }

    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private var crypto: RoomCrypto? = null
    private var seal: RoomSeal? = null
    private var discovery: MulticastSocket? = null
    private var server: ServerSocket? = null
    private var beaconTimer: LoopTimer? = null
    private var sweepTimer: LoopTimer? = null
    private var bluetoothListener: ((MeshBluetoothEvent) -> Unit)? = null

    private val links = ArrayList<MeshLink>()
    private val peers = LinkedHashMap<String, MeshPeer>()
    private val history = ArrayList<Map<String, Any?>>()
    private val messageList = ArrayList<ChatMessage>()
    private val seenIds = HashSet<String>()
    private val seenOrder = ArrayDeque<String>()
    private val dialing = HashSet<String>()

    private val queued = LinkedHashSet<String>()
    private var historyTarget: File? = null
    private var historyLoaded = false
    private var historySaveTimer: LoopTimer? = null
    private var retention: ChatRetention? = null

    private var announceTimer: LoopTimer? = null
    private var addressSignature: String? = null
    private val joinedInterfaces = HashSet<String>()
    private val beatLock = Any()

    private var sweepPaceTimer: LoopTimer? = null
    private var sweepQueue: ArrayDeque<InetAddress> = ArrayDeque()
    private var sweepDatagram: ByteArray? = null
    private var sweepRun = 0
    private var diag = MeshDiagnostics()
    private var startedMs = 0L

    private var isRunning = false
    private var networkUp = false
    private var unreadCount = 0
    private var beaconCount = 0

    @Volatile
    var discoveryPort = MESH_DISCOVERY_PORT

    @Volatile
    var beaconIntervalMs = BEACON_INTERVAL_MS

    @Volatile
    var peerTimeoutMs = PEER_TIMEOUT_MS

    @Volatile
    var linkReplaceAfterMs = LINK_REPLACE_AFTER_MS

    @Volatile
    var handshakeTimeoutMs = HANDSHAKE_TIMEOUT_MS

    @Volatile
    var multicastEnabled = true

    @Volatile
    var addressSweepEnabled = true

    @Volatile
    var networkEnabled = true

    @Volatile
    var bluetoothEnabled = true

    @Volatile
    var listenAddress: InetAddress? = null

    val running: Boolean get() = loop.call { isRunning }
    val networkActive: Boolean get() = loop.call { networkUp }
    val bluetoothActive: Boolean get() = native.listening
    val unread: Int get() = loop.call { unreadCount }
    val hasUnread: Boolean get() = unread > 0
    val listenPort: Int get() = loop.call { server?.localPort ?: 0 }
    val messages: List<ChatMessage> get() = loop.call { ArrayList(messageList) }
    val peerCount: Int get() = loop.call { peers.values.count { it.linked } }
    val sealKeyId: String get() = loop.call { seal?.keyId ?: "" }

    fun isQueued(messageId: String): Boolean = loop.call { messageId in queued }

    val localNetworkBlocked: Boolean get() = false

    val displayName: String get() = name

    fun controlHosts(): List<MeshControlHost> = loop.call {
        val hosts = ArrayList<MeshControlHost>()
        for ((id, peer) in peers) {
            val address = peer.address ?: continue
            if (!peer.control || peer.port <= 0) continue
            hosts.add(
                MeshControlHost(
                    nodeId = id,
                    address = address,
                    port = peer.port,
                    name = peer.name,
                    nodeName = peer.nodeName,
                    ownerUser = peer.ownerUser,
                    platform = peer.platform,
                ),
            )
        }
        hosts
    }

    fun diagnosticsLine(): String = loop.call { diagnosticsLineNow() }

    private fun diagnosticsLineNow(): String {
        val d = diag
        if (!isRunning) {
            return if (d.startNote.isEmpty()) "mesh not running" else "mesh not running · ${d.startNote} · ${d.listenError}"
        }
        val now = System.currentTimeMillis()
        val keyed = links.count { it.keyKnown }
        val parts = arrayListOf(
            "udp $discoveryPort ${if (discovery != null) "bound" else "CLOSED"}" +
                if (d.multicastJoinError.isNotEmpty()) " no-mcast" else "",
            "tcp ${server?.localPort?.toString() ?: "CLOSED"}",
            if (d.interfaces.isEmpty()) "no interfaces seen" else d.interfaces.joinToString(", "),
            "tx mc ${d.multicastSent} bc ${d.broadcastSent} uc ${d.unicastSent} " +
                "sweep ${d.sweepSent}${if (d.sendErrors > 0) " ERR ${d.sendErrors}" else ""}",
            "rx ${d.datagramsIn}: own ${d.ownBeaconsIn} peer ${d.peerBeaconsIn} " +
                "bad ${d.undecryptableIn} off-net ${d.offNetworkIn}",
            if (d.lastBeaconMs > 0) "heard ${d.lastBeaconFrom} ${meshAgeText(now, d.lastBeaconMs)}" else "heard nobody",
            "dial ${d.dials} ok ${d.dialsOk} fail ${d.dialsFailed} · in ${d.inboundAccepted}",
            "links ${links.size} keyed $keyed · peers ${peers.size}",
            "frames ${d.framesIn} bad ${d.framesBad} · chat in ${d.chatIn} out ${d.chatOut} queued ${queued.size}",
            if (native.listening) "bt on" else "bt off",
        )
        if (!networkUp && !native.listening) parts.add(0, "waiting for a local network or Bluetooth")
        return parts.joinToString(" · ")
    }

    fun diagnosticsDetail(): String = loop.call {
        val d = diag
        val now = System.currentTimeMillis()
        val lines = arrayListOf(
            diagnosticsLineNow(),
            "node ${nodeId.take(12)} · $platform ${RelayRoom.CLIENT_VERSION} · up ${meshAgeText(now, startedMs)}",
            "sweep ${if (addressSweepEnabled) "on" else "off"}, queue ${sweepQueue.size}",
        )
        if (d.listenError.isNotEmpty()) lines.add("tcp error: ${d.listenError}")
        if (d.discoveryError.isNotEmpty()) lines.add("udp error: ${d.discoveryError}")
        if (d.multicastJoinError.isNotEmpty()) lines.add("multicast join: ${d.multicastJoinError}")
        if (d.lastSendError.isNotEmpty()) lines.add("last send error: ${d.lastSendError}")
        if (d.lastDialError.isNotEmpty()) lines.add("last dial error: ${d.lastDialError}")
        if (d.lastDropReason.isNotEmpty()) lines.add("last link drop: ${d.lastDropReason}")
        if (d.inboundRefused > 0) lines.add("inbound refused: ${d.inboundRefused}")
        if (peers.isEmpty()) lines.add("peers: none")
        for ((id, peer) in peers) {
            val state = when {
                peer.linked -> "linked"
                peer.unlinkedSinceMs > 0 -> "unlinked since ${meshAgeText(now, peer.unlinkedSinceMs)}"
                else -> "unlinked"
            }
            lines.add(
                "peer ${peer.name.ifEmpty { "?" }} (${id.take(8)}) " +
                    "${peer.address?.hostAddress ?: "-"}:${peer.port} ${peer.platform} " +
                    "${peer.version} · $state · seen ${meshAgeText(now, peer.lastSeenMs)}",
            )
        }
        lines.joinToString("\n")
    }

    fun snapshot(): MeshSnapshot = loop.call {
        MeshSnapshot(
            running = isRunning,
            networkActive = networkUp,
            bluetoothActive = native.listening,
            unread = unreadCount,
            messages = ArrayList(messageList),
            members = membersNow(),
            peerCount = peers.values.count { it.linked },
            queued = LinkedHashSet(queued),
        )
    }

    private val nodeId: String get() = identity.nodeId

    @Volatile
    var accountProvider: (() -> String)? = null

    @Volatile
    var wakeSender: ((List<String>) -> Unit)? = null

    @Volatile
    var onIncoming: ((ChatMessage, Int) -> Unit)? = null

    @Volatile
    var onDirect: ((String, String, Map<String, Any?>) -> Unit)? = null
    private val wakeTokens = HashMap<String, String>()
    private val wakeOrder = ArrayList<String>()

    fun wakeTargets(): List<String> = loop.call { wakeTargetsNow() }

    private fun wakeTargetsNow(): List<String> {
        val now = System.currentTimeMillis()
        val tokens = ArrayList<String>()
        for (id in wakeOrder.asReversed()) {
            val token = wakeTokens[id] ?: continue
            if ((meshWakeTokenExpiry(token) ?: 0L) > now && token !in tokens) tokens.add(token)
            if (tokens.size >= MAX_WAKE_TOKENS) break
        }
        return tokens
    }

    private fun rememberWake(senderId: String, plain: Map<String, Any?>) {
        if (senderId.isEmpty() || senderId == nodeId) return
        val token = plain["wake"] as? String ?: return
        val expires = meshWakeTokenExpiry(token) ?: return
        if (expires <= System.currentTimeMillis()) return
        val known = wakeTokens[senderId]?.let { meshWakeTokenExpiry(it) } ?: 0L
        if (known >= expires) return
        wakeTokens[senderId] = token
        wakeOrder.remove(senderId)
        wakeOrder.add(senderId)
        while (wakeOrder.size > MAX_WAKE_TOKENS) wakeTokens.remove(wakeOrder.removeAt(0))
    }

    val persona: ChatPersona
        get() = ChatPersona.resolve(identity.publicKeyB64url, accountProvider?.invoke() ?: "", settings.displayName)

    private val name: String
        get() {
            val chosen = persona.name
            return if (chosen.length > MAX_NAME_CHARS) chosen.substring(0, MAX_NAME_CHARS) else chosen
        }

    fun members(): List<Member> = loop.call { membersNow() }

    private fun membersNow(): List<Member> {
        val now = System.currentTimeMillis()
        val list = ArrayList<Member>()
        list.add(Member(id = nodeId, name = name, owner = persona.ownerUser, self = true, online = isRunning, platform = platform))
        for ((id, peer) in peers) {
            list.add(
                Member(
                    id = id,
                    name = peer.name.ifEmpty { peer.ownerUser.ifEmpty { id } },
                    online = peer.linked || now - peer.lastSeenMs <= peerTimeoutMs,
                    platform = peer.platform,
                    version = peer.version,
                    owner = peer.ownerUser,
                    nodeName = peer.nodeName,
                ),
            )
        }
        return list
    }

    fun setChatRetention(value: ChatRetention?) = loop.call {
        retention = value
    }

    fun pruneExpired(nowMs: Long = System.currentTimeMillis()): Boolean = loop.call {
        val rule = retention ?: return@call false
        val shown = messageList.removeAll { rule.isExpired(it.timestamp, MESH_CONVERSATION, nowMs) }
        val stored = history.removeAll { plain ->
            val ts = plain["ts"]
            val gone = ts is Number && rule.isExpired(ts.toLong(), MESH_CONVERSATION, nowMs)
            if (gone) queued.remove(jsonText(plain["id"]))
            gone
        }
        if (stored) scheduleHistorySave()
        if (shown) {
            unreadCount = minOf(unreadCount, messageList.count { !it.self })
            notifyListeners()
        }
        shown || stored
    }

    fun markRead() = loop.call {
        if (unreadCount == 0) return@call
        unreadCount = 0
        notifyListeners()
    }

    fun start(): Boolean {
        if (loop.call { isRunning }) return true
        val roomCrypto = RoomCrypto.shared(MESH_ROOM_NAME)
        val roomSeal = RoomSeal.loadOrCreate(MESH_ROOM_NAME, storageScope = "$MESH_ROOM_NAME\n$nodeId", secureStore = secureStore)
        val file = if (loop.call { historyLoaded }) null else historyFile()
        loop.call {
            crypto = roomCrypto
            seal = roomSeal
            if (!historyLoaded) loadHistory(file)
            diag = MeshDiagnostics()
            startedMs = System.currentTimeMillis()
            isRunning = true
            beaconCount = 0
            addressSignature = null
        }
        beat(sweepNow = true)
        loop.call {
            beaconTimer?.cancel()
            beaconTimer = loop.periodic(beaconIntervalMs) { loop.io { beat() } }
            sweepTimer?.cancel()
            sweepTimer = loop.periodic(SWEEP_INTERVAL_MS) { sweepPeers() }
            notifyListeners()
        }
        return true
    }

    fun announceNow() {
        loop.post {
            if (!isRunning) return@post
            announceTimer?.cancel()
            announceTimer = loop.timer(ANNOUNCE_DEBOUNCE_MS) {
                announceTimer = null
                if (isRunning) loop.io { beat(sweepNow = true) }
            }
        }
    }

    fun retryBluetooth() {
        loop.post {
            if (isRunning && bluetoothEnabled && !native.listening) loop.io { openBluetooth() }
        }
    }

    private fun beat(sweepNow: Boolean = false) {
        synchronized(beatLock) {
            if (!loop.call { isRunning }) return
            val interfaces = scanInterfaces()
            if (refreshNetwork(interfaces, sweepNow)) {
                sendBeacon(interfaces)
                sendUnicastBeacons(interfaces)
            }
            if (!native.listening) openBluetooth()
        }
    }

    private fun refreshNetwork(interfaces: List<MeshInterface>, sweepNow: Boolean): Boolean {
        val signature = interfaces.flatMap { mesh -> mesh.addresses.map { "${mesh.iface.name} ${it.hostAddress}" } }.sorted().joinToString(",")
        val available = networkEnabled && (listenAddress != null || interfaces.isNotEmpty())
        if (networkEnabled && !available) loop.call { diag.listenError = "no local IPv4 address" }
        val up = available && openListener()
        val moved = loop.call {
            val previous = addressSignature
            addressSignature = signature
            val changed = previous != null && previous != signature
            if (sweepNow || changed || (up && !networkUp)) beaconCount = 0
            if (up != networkUp) {
                networkUp = up
                notifyListeners()
            }
            changed
        }
        if (moved) dropStrandedLinks()
        if (!up) return false
        val socket = loop.call { discovery }
        if (socket == null) openDiscovery(interfaces) else joinGroups(socket, interfaces)
        return true
    }

    private fun dropStrandedLinks() {
        val present = HashSet<String>()
        try {
            for (iface in NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()) {
                for (address in iface.inetAddresses.toList()) present.add(addressKey(address))
            }
        } catch (_: Exception) {
            return
        }
        loop.call {
            for (link in ArrayList(links)) {
                val local = link.socket?.localAddress ?: continue
                if (local.isAnyLocalAddress) continue
                if (addressKey(local) !in present) closeLink(link, reason = "network changed")
            }
        }
    }

    fun stop() {
        var pendingHistory: Map<String, Any?>? = null
        val closing = loop.call {
            if (!isRunning) return@call null
            sendToAll(envelope("bye"))
            isRunning = false
            if (historySaveTimer?.isActive == true) {
                historySaveTimer?.cancel()
                historySaveTimer = null
                pendingHistory = historySnapshot()
            }
            beaconTimer?.cancel()
            sweepTimer?.cancel()
            announceTimer?.cancel()
            beaconTimer = null
            sweepTimer = null
            announceTimer = null
            sweepPaceTimer?.cancel()
            sweepPaceTimer = null
            sweepQueue = ArrayDeque()
            sweepDatagram = null
            val open = ArrayList(links)
            for (link in open) link.closed = true
            links.clear()
            dialing.clear()
            for (peer in peers.values) peer.linked = false
            discovery?.close()
            discovery = null
            addressSignature = null
            open
        } ?: return
        pendingHistory?.let(::saveHistory)
        val deadline = System.currentTimeMillis() + STOP_DRAIN_MS
        for (link in closing) {
            try {
                link.writer.submit {}.get((deadline - System.currentTimeMillis()).coerceAtLeast(1), TimeUnit.MILLISECONDS)
            } catch (_: Exception) {
            }
            releaseLink(link, duplicate = false)
        }
        native.releaseMulticastLock()
        loop.call {
            try {
                server?.close()
            } catch (_: IOException) {
            }
            server = null
            bluetoothListener?.let(native::removeEventListener)
            bluetoothListener = null
        }
        native.stopBluetooth()
        loop.call {
            networkUp = false
            notifyListeners()
        }
    }

    private fun openListener(): Boolean {
        if (loop.call { server != null }) return true
        return try {
            val bound = ServerSocket()
            bound.bind(InetSocketAddress(listenAddress ?: ANY_IPV4, 0))
            val installed = loop.call {
                if (server == null && isRunning) {
                    server = bound
                    diag.listenError = ""
                    true
                } else {
                    false
                }
            }
            if (!installed) {
                bound.close()
                return loop.call { server != null }
            }
            Thread({ acceptLoop(bound) }, "LocalMeshRoom-accept").apply { isDaemon = true }.start()
            true
        } catch (error: Exception) {
            loop.call { diag.listenError = meshErrorText(error) }
            false
        }
    }

    private fun acceptLoop(bound: ServerSocket) {
        while (!bound.isClosed) {
            val socket = try {
                bound.accept()
            } catch (_: IOException) {
                return
            }
            loop.post {
                if (!isRunning || links.size >= MAX_LINKS || !isLocalNetworkAddress(socket.inetAddress)) {
                    diag.inboundRefused++
                    closeQuietly(socket)
                    return@post
                }
                diag.inboundAccepted++
                attachSocket(socket, outbound = false)
            }
        }
    }

    private class MeshInterface(val iface: NetworkInterface, val addresses: List<Inet4Address>, val multicast: Boolean)

    private fun scanInterfaces(): List<MeshInterface> {
        val usable = ArrayList<MeshInterface>()
        val seen = ArrayList<String>()
        try {
            for (iface in NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()) {
                val v4 = iface.inetAddresses.toList().filterIsInstance<Inet4Address>()
                if (v4.isEmpty()) continue
                val up = try {
                    iface.isUp && !iface.isLoopback
                } catch (_: Exception) {
                    false
                }
                val local = v4.filter { isLocalNetworkAddress(it) && !it.isLoopbackAddress }
                val used = up && local.isNotEmpty() && isMeshInterfaceName(iface.name)
                seen.add("${iface.name} ${v4.joinToString("/") { it.hostAddress ?: "" }}${if (used) "*" else ""}")
                if (!used) continue
                val multicast = try {
                    iface.supportsMulticast()
                } catch (_: Exception) {
                    false
                }
                usable.add(MeshInterface(iface, local, multicast))
            }
            loop.call { diag.interfaces = seen }
        } catch (error: Exception) {
            loop.call { diag.interfaces = listOf("interfaces: ${meshErrorText(error)}") }
        }
        return usable
    }

    private fun openDiscovery(interfaces: List<MeshInterface>) {
        if (loop.call { discovery != null } || !multicastEnabled) return
        native.acquireMulticastLock()
        try {
            val socket = MulticastSocket(null)
            socket.reuseAddress = true
            socket.bind(InetSocketAddress(ANY_IPV4, discoveryPort))
            socket.timeToLive = 1
            @Suppress("DEPRECATION")
            socket.loopbackMode = false
            try {
                socket.broadcast = true
            } catch (_: Exception) {
            }
            val installed = loop.call {
                if (discovery == null && isRunning) {
                    diag.discoveryError = ""
                    discovery = socket
                    true
                } else {
                    false
                }
            }
            if (!installed) {
                socket.close()
                return
            }
            joinedInterfaces.clear()
            joinGroups(socket, interfaces)
            Thread({ receiveLoop(socket) }, "LocalMeshRoom-discovery").apply { isDaemon = true }.start()
        } catch (error: Exception) {
            loop.call {
                discovery = null
                diag.discoveryError = meshErrorText(error)
            }
        }
    }

    private fun joinGroups(socket: MulticastSocket, interfaces: List<MeshInterface>) {
        val group = InetSocketAddress(InetAddress.getByName(MESH_MULTICAST_GROUP), 0)
        val current = HashSet<String>()
        var error = ""
        for (mesh in interfaces) {
            if (!mesh.multicast) continue
            val key = "${mesh.iface.name}#${mesh.iface.index}"
            current.add(key)
            if (key in joinedInterfaces) continue
            try {
                socket.joinGroup(group, mesh.iface)
                joinedInterfaces.add(key)
            } catch (failure: Exception) {
                if (failure.message.orEmpty().contains("already", ignoreCase = true)) {
                    joinedInterfaces.add(key)
                } else {
                    error = "${mesh.iface.name}: ${meshErrorText(failure)}"
                }
            }
        }
        if (current.isEmpty()) {
            current.add(DEFAULT_GROUP_KEY)
            if (DEFAULT_GROUP_KEY !in joinedInterfaces) {
                try {
                    @Suppress("DEPRECATION")
                    socket.joinGroup(group.address)
                    joinedInterfaces.add(DEFAULT_GROUP_KEY)
                } catch (failure: Exception) {
                    error = meshErrorText(failure)
                }
            }
        }
        joinedInterfaces.retainAll(current)
        val joined = joinedInterfaces.isNotEmpty()
        loop.call { diag.multicastJoinError = if (joined) "" else error }
    }

    private fun receiveLoop(socket: MulticastSocket) {
        val buffer = ByteArray(65536)
        while (!socket.isClosed) {
            val packet = DatagramPacket(buffer, buffer.size)
            try {
                socket.receive(packet)
            } catch (error: IOException) {
                loop.post {
                    if (discovery === socket) {
                        discovery = null
                        if (!socket.isClosed) diag.discoveryError = meshErrorText(error)
                    }
                }
                return
            }
            val data = packet.data.copyOfRange(packet.offset, packet.offset + packet.length)
            val from = packet.address ?: continue
            val fromPort = packet.port
            loop.post { onDatagram(from, fromPort, data) }
        }
    }

    private fun beaconDatagram(reply: Boolean = false): ByteArray? {
        val roomCrypto = crypto ?: return null
        val bound = server ?: return null
        val self = persona
        val beacon = MeshBeacon(
            nodeId = nodeId,
            port = bound.localPort,
            name = name,
            ownerUser = self.ownerUser,
            accountKind = self.accountKind,
            platform = platform,
            version = RelayRoom.CLIENT_VERSION,
            timestampMs = System.currentTimeMillis(),
            reply = reply,
        )
        return Json.stringify(roomCrypto.encrypt(beacon.toJson())).toByteArray(StandardCharsets.UTF_8)
    }

    private fun sendDatagram(socket: MulticastSocket, datagram: ByteArray, to: InetAddress, port: Int, count: MeshDiagnostics.() -> Unit): Boolean =
        try {
            socket.send(DatagramPacket(datagram, datagram.size, to, port))
            loop.post { diag.count() }
            true
        } catch (error: Exception) {
            loop.post {
                diag.sendErrors++
                diag.lastSendError = "${to.hostAddress}: ${meshErrorText(error)}"
            }
            false
        }

    private fun sendBeacon(interfaces: List<MeshInterface>) {
        val prepared = loop.call {
            val socket = discovery
            if (!isRunning || socket == null) return@call null
            val datagram = beaconDatagram() ?: return@call null
            socket to datagram
        } ?: return
        val (socket, datagram) = prepared
        val group = InetAddress.getByName(MESH_MULTICAST_GROUP)
        val multicast = interfaces.filter { it.multicast }
        if (multicast.isEmpty()) {
            sendDatagram(socket, datagram, group, discoveryPort) { multicastSent++ }
        } else {
            for (mesh in multicast) {
                synchronized(socket) {
                    try {
                        socket.networkInterface = mesh.iface
                    } catch (_: Exception) {
                    }
                    sendDatagram(socket, datagram, group, discoveryPort) { multicastSent++ }
                }
            }
        }
        if (!addressSweepEnabled) return
        for (mesh in interfaces) {
            for (address in mesh.addresses) {
                val broadcast = sweepBroadcastAddress(address) ?: continue
                sendDatagram(socket, datagram, broadcast, discoveryPort) { broadcastSent++ }
            }
        }
    }

    private fun sendUnicastBeacons(interfaces: List<MeshInterface>) {
        val plan = loop.call {
            val socket = discovery
            if (!isRunning || socket == null) return@call null
            val sweepEvery = if (links.isEmpty()) ADDRESS_SWEEP_EVERY_BEATS_ALONE else ADDRESS_SWEEP_EVERY_BEATS
            val sweep = addressSweepEnabled && beaconCount++ % sweepEvery == 0
            val targets = LinkedHashMap<String, InetAddress>()
            for ((id, peer) in peers) {
                val address = peer.address ?: continue
                if (linkForPeer(id) != null) continue
                targets[address.hostAddress ?: continue] = address
            }
            Triple(socket, sweep, targets)
        } ?: return
        val (socket, sweep, targets) = plan
        val swept = LinkedHashMap<String, InetAddress>()
        if (sweep) {
            for (address in interfaces.flatMap { it.addresses }) {
                for (host in sweepAddresses(address)) {
                    val key = host.hostAddress ?: continue
                    if (key !in targets) swept[key] = host
                }
            }
        }
        if (targets.isEmpty() && swept.isEmpty()) return
        val datagram = loop.call { beaconDatagram() } ?: return
        for (target in targets.values) sendDatagram(socket, datagram, target, discoveryPort) { unicastSent++ }
        if (!sweep) return
        loop.call {
            sweepQueue = ArrayDeque(swept.values)
            sweepDatagram = datagram
            sweepRun++
            drainSweep()
        }
    }

    private fun drainSweep() {
        sweepPaceTimer?.cancel()
        sweepPaceTimer = null
        val datagram = sweepDatagram
        val socket = discovery
        if (!isRunning || socket == null || datagram == null) {
            sweepQueue = ArrayDeque()
            sweepDatagram = null
            return
        }
        val batch = ArrayList<InetAddress>()
        while (batch.size < SWEEP_BATCH && sweepQueue.isNotEmpty()) batch.add(sweepQueue.removeFirst())
        val run = sweepRun
        loop.io {
            for (host in batch) {
                if (loop.call { run != sweepRun || !isRunning }) return@io
                sendDatagram(socket, datagram, host, discoveryPort) { sweepSent++ }
            }
            loop.post {
                if (run != sweepRun || !isRunning) return@post
                if (sweepQueue.isEmpty()) {
                    sweepDatagram = null
                    return@post
                }
                sweepPaceTimer = loop.timer(SWEEP_BATCH_GAP_MS) { drainSweep() }
            }
        }
    }

    private fun onDatagram(from: InetAddress, fromPort: Int, data: ByteArray) {
        val roomCrypto = crypto
        if (!isRunning || roomCrypto == null) return
        diag.datagramsIn++
        if (!isLocalNetworkAddress(from)) {
            diag.offNetworkIn++
            return
        }
        @Suppress("UNCHECKED_CAST")
        val envelope = try {
            Json.parse(String(data, StandardCharsets.UTF_8)) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        }
        val plain = envelope?.let { roomCrypto.decrypt(it) }
        val beacon = plain?.let { MeshBeacon.parse(it) }
        if (beacon == null) {
            diag.undecryptableIn++
            return
        }
        if (beacon.nodeId == nodeId) {
            diag.ownBeaconsIn++
            return
        }
        diag.peerBeaconsIn++
        diag.lastBeaconMs = System.currentTimeMillis()
        val who = beacon.name.ifEmpty { beacon.nodeId.take(8) }
        diag.lastBeaconFrom = "$who ${from.hostAddress}:${beacon.port}${if (beacon.reply) " (reply)" else ""}"

        rememberPeer(
            beacon.nodeId,
            mapOf(
                "sender" to beacon.name,
                "nodeName" to beacon.nodeName,
                "ownerUser" to beacon.ownerUser,
                "platform" to beacon.platform,
                "version" to beacon.version,
                "control" to beacon.control,
            ),
            linked = false,
        )
        val peer = peers[beacon.nodeId] ?: return
        val moved = peer.address?.hostAddress != from.hostAddress || peer.port != beacon.port
        peer.address = from
        peer.port = beacon.port
        if (moved && peer.control) notifyListeners()
        if (linkForPeer(beacon.nodeId) != null) {
            peer.unlinkedSinceMs = 0
            return
        }
        if (!beacon.reply) {
            val socket = discovery
            val answer = beaconDatagram(reply = true)
            if (socket != null && answer != null && fromPort > 0) {
                loop.io { sendDatagram(socket, answer, from, fromPort) { unicastSent++ } }
            }
        }
        val now = System.currentTimeMillis()
        if (peer.unlinkedSinceMs == 0L) peer.unlinkedSinceMs = now
        if (nodeId < beacon.nodeId || now - peer.unlinkedSinceMs >= beaconIntervalMs / 2) {
            loop.io { connectToPeer(from, beacon.port) }
        }
    }

    fun connectToPeer(address: InetAddress, port: Int): Boolean {
        val key = "${address.hostAddress}:$port"
        val allowed = loop.call {
            if (!isRunning || port <= 0 || !isLocalNetworkAddress(address)) return@call false
            if (links.size >= MAX_LINKS) return@call false
            if (key in dialing) return@call false
            dialing.add(key)
            diag.dials++
            true
        }
        if (!allowed) return false
        return try {
            val socket = Socket()
            socket.connect(InetSocketAddress(address, port), DIAL_TIMEOUT_MS)
            loop.call {
                dialing.remove(key)
                diag.dialsOk++
                if (!isRunning) {
                    closeQuietly(socket)
                    return@call false
                }
                attachSocket(socket, outbound = true)
                true
            }
        } catch (error: IOException) {
            loop.call {
                dialing.remove(key)
                diag.dialsFailed++
                diag.lastDialError = "$key: ${meshErrorText(error)}"
            }
            false
        }
    }

    private fun attachSocket(socket: Socket, outbound: Boolean) {
        val output: OutputStream
        val input: InputStream
        try {
            socket.tcpNoDelay = true
            output = socket.getOutputStream()
            input = socket.getInputStream()
        } catch (_: IOException) {
            closeQuietly(socket)
            return
        }
        tuneKeepAlive(socket)
        val link = MeshLink(socket = socket, bluetoothId = null, outbound = outbound, writer = loop.SerialQueue("LocalMeshRoom-link"))
        link.output = output
        links.add(link)
        Thread({
            val chunk = ByteArray(8192)
            var reason = "closed by peer"
            while (true) {
                val read = try {
                    input.read(chunk)
                } catch (error: IOException) {
                    reason = meshErrorText(error)
                    -1
                }
                if (read <= 0) break
                val bytes = chunk.copyOf(read)
                loop.post { if (link in links) onLinkData(link, bytes) }
            }
            loop.post { closeLink(link, reason = reason) }
        }, "LocalMeshRoom-link").apply { isDaemon = true }.start()
        openLink(link)
    }

    private fun tuneKeepAlive(socket: Socket) {
        try {
            socket.keepAlive = true
        } catch (_: Exception) {
        }
        try {
            socket.setOption(ExtendedSocketOptions.TCP_KEEPIDLE, MESH_KEEPALIVE_IDLE_S)
            socket.setOption(ExtendedSocketOptions.TCP_KEEPINTERVAL, MESH_KEEPALIVE_INTERVAL_S)
            socket.setOption(ExtendedSocketOptions.TCP_KEEPCOUNT, MESH_KEEPALIVE_PROBES)
        } catch (_: Exception) {
        } catch (_: LinkageError) {
        }
        try {
            native.tuneLinkSocket(socket)
        } catch (_: Exception) {
        }
    }

    private fun openBluetooth(): Boolean {
        if (!bluetoothEnabled || !loop.call { isRunning }) return false
        if (native.listening) return true
        if (!native.startBluetooth()) return false
        loop.call {
            if (bluetoothListener == null) {
                val listener: (MeshBluetoothEvent) -> Unit = { event -> loop.post { onBluetoothEvent(event) } }
                bluetoothListener = listener
                native.addEventListener(listener)
            }
            notifyListeners()
        }
        return true
    }

    private fun onBluetoothEvent(event: MeshBluetoothEvent) {
        when (event.kind) {
            MeshBluetoothEventKind.LINK_OPENED -> {
                if (!isRunning) {
                    loop.io { native.closeBluetoothLink(event.linkId) }
                    return
                }
                val link = MeshLink(
                    socket = null,
                    bluetoothId = event.linkId,
                    outbound = event.outbound,
                    writer = loop.SerialQueue("LocalMeshRoom-bluetooth"),
                )
                links.add(link)
                openLink(link)
            }
            MeshBluetoothEventKind.DATA -> linkForBluetooth(event.linkId)?.let { onLinkData(it, event.bytes) }
            MeshBluetoothEventKind.LINK_CLOSED -> linkForBluetooth(event.linkId)?.let { closeLink(it, reason = "bluetooth closed") }
        }
    }

    private fun linkForBluetooth(id: Int): MeshLink? = links.firstOrNull { it.bluetoothId == id }

    private fun openLink(link: MeshLink) {
        sendKeyFrame(link)
        link.pending.add(envelope("hello"))
        loop.timer(handshakeTimeoutMs) {
            if (link in links && link.peerId.isEmpty()) closeLink(link, reason = "handshake timeout")
        }
    }

    private fun onLinkData(link: MeshLink, data: ByteArray) {
        link.buffer.add(data)
        while (true) {
            val payload = try {
                takeFrame(link.buffer)
            } catch (_: MeshFrameException) {
                closeLink(link, reason = "oversized frame")
                return
            } ?: return
            handleFrame(link, payload)
            if (link !in links) return
        }
    }

    private fun handleFrame(link: MeshLink, payload: ByteArray) {
        val roomCrypto = crypto ?: return
        val roomSeal = seal ?: return
        diag.framesIn++
        @Suppress("UNCHECKED_CAST")
        val envelope = try {
            Json.parse(String(payload, StandardCharsets.UTF_8)) as? Map<String, Any?>
        } catch (_: Exception) {
            null
        }
        val decrypted = envelope?.let { roomCrypto.decrypt(it) }
        if (decrypted == null) {
            diag.framesBad++
            return
        }
        if (RoomSeal.isKeyFrame(decrypted)) {
            val keyId = jsonText(decrypted["kid"])
            if (roomSeal.acceptKeyFrame(decrypted)) {
                sendKeyFrame(link)
            }
            if (keyId == roomSeal.keyId || keyId in roomSeal.peerKeyIds) link.keyId = keyId
            if (!link.keyKnown) {
                link.keyKnown = true
                val queued = ArrayList(link.pending)
                link.pending.clear()
                for (frame in queued) writePlain(link, frame)
            }
            return
        }
        val plain = roomSeal.unseal(decrypted)
        if (plain == null) {
            diag.framesBad++
            return
        }
        if (jsonText(plain["type"]) == "direct") {
            handleDirect(plain, RoomSeal.frameKeyId(decrypted), roomSeal.keyId)
            return
        }
        handlePlain(link, plain)
    }

    private fun handleDirect(plain: Map<String, Any?>, sealKeyId: String, ownKeyId: String) {
        val channel = jsonText(plain["channel"])
        if (sealKeyId.isEmpty() || sealKeyId == ownKeyId) return
        if (!channel.startsWith(DirectMessageFrames.PREFIX) || channel.length > 80) return
        val handler = onDirect ?: return
        loop.io { handler(channel, sealKeyId, plain) }
    }

    fun sendDirect(plain: Map<String, Any?>, keyIds: Collection<String>): Int = loop.call {
        val roomSeal = seal ?: return@call 0
        val channel = jsonText(plain["channel"])
        if (!isRunning || !channel.startsWith(DirectMessageFrames.PREFIX) || jsonText(plain["id"]).isEmpty() || keyIds.isEmpty()) {
            return@call 0
        }
        val frame = LinkedHashMap(plain)
        frame["type"] = "direct"
        frame["conversation"] = MESH_CONVERSATION
        val reached = HashSet<String>()
        for (link in ArrayList(links)) {
            val keyId = link.keyId
            if (!link.keyKnown || keyId.isEmpty() || keyId == roomSeal.keyId || keyId !in keyIds || keyId in reached) continue
            if (writePlain(link, frame, listOf(keyId))) reached.add(keyId)
        }
        reached.size
    }

    private fun handlePlain(link: MeshLink, plain: Map<String, Any?>) {
        val type = jsonText(plain["type"])
        val senderId = jsonText(plain["senderId"])
        if (senderId.isEmpty() || senderId == nodeId) return
        if (jsonText(plain["conversation"]) != MESH_CONVERSATION) return

        if (type == "hello") {
            if (link.peerId.isEmpty()) {
                val other = linkForPeer(senderId)
                val now = linkClock()
                link.peerId = senderId
                link.helloMs = now
                if (other != null) {
                    val bluetooth = link.bluetoothId != null
                    if (bluetooth != (other.bluetoothId != null)) {
                        if (bluetooth) {
                            closeLink(link, reason = "duplicate link")
                            return
                        }
                        closeLink(other, reason = "duplicate link")
                    } else if (now - other.helloMs >= linkReplaceAfterMs) {
                        closeLink(other, reason = "replaced")
                    } else {
                        val mine = if (link.outbound) nodeId else senderId
                        val theirs = if (other.outbound) nodeId else senderId
                        if (mine < theirs) {
                            closeLink(link, reason = "duplicate link")
                            return
                        }
                        closeLink(other, reason = "duplicate link")
                    }
                }
            } else if (link.peerId != senderId) {
                closeLink(link, reason = "identity changed")
                return
            }
        } else if ((type == "typing" || type == "bye") && link.peerId != senderId) {
            return
        }

        rememberPeer(senderId, plain, linked = link.peerId == senderId)
        rememberWake(senderId, plain)
        if (type == "hello" && link.peerId == senderId) {
            val peer = peers[senderId]
            val socket = link.socket
            if (peer != null && socket != null) {
                if (peer.address == null) peer.address = socket.inetAddress
                val port = plain["port"]
                if (peer.port == 0 && port is Number && port.toLong() in 1..65535) peer.port = port.toInt()
            }
        }
        if (type == "typing") return

        val id = jsonText(plain["id"])
        if (id.isEmpty() || !markSeen(id)) return

        if (type == "hello") {
            for (entry in ArrayList(history)) writePlain(link, entry)
            return
        }
        if (type == "bye") {
            if (peers.remove(senderId) != null) notifyListeners()
            return
        }
        if (type != "chat") return
        diag.chatIn++
        storeHistory(plain)
        val message = toMessage(plain)
        val added = appendMessage(message)
        for (other in ArrayList(links)) {
            if (other === link || !other.keyKnown) continue
            writePlain(other, plain)
        }
        notifyListeners()
        if (added && !message.self) announceIncoming(message)
    }

    private fun announceIncoming(message: ChatMessage) {
        val handler = onIncoming ?: return
        try {
            handler(message, unreadCount)
        } catch (failure: Exception) {
            loop.failures.add(failure)
        }
    }

    private fun sendKeyFrame(link: MeshLink) {
        val roomCrypto = crypto ?: return
        val roomSeal = seal ?: return
        val envelope = roomCrypto.encrypt(roomSeal.keyFrame())
        write(link, Json.stringify(envelope).toByteArray(StandardCharsets.UTF_8))
    }

    private fun writePlain(link: MeshLink, plain: Map<String, Any?>, onlyKeyIds: Collection<String>? = null): Boolean {
        val roomCrypto = crypto ?: return false
        val roomSeal = seal ?: return false
        val sealed = if (onlyKeyIds == null) roomSeal.seal(plain) else roomSeal.sealFor(plain, onlyKeyIds)
        val envelope = roomCrypto.encrypt(sealed)
        val written = write(link, Json.stringify(envelope).toByteArray(StandardCharsets.UTF_8))
        if (written && queued.remove(jsonText(plain["id"]))) {
            scheduleHistorySave()
            notifyListeners()
        }
        return written
    }

    private fun write(link: MeshLink, payload: ByteArray): Boolean {
        if (payload.size > MAX_FRAME_BYTES || link.closed) return false
        val framed = frameBytes(payload)
        val output = link.output
        val bluetoothId = link.bluetoothId
        if (output == null && bluetoothId == null) return false
        link.writer.execute {
            link.writeStartedMs = linkClock()
            try {
                if (output != null) {
                    output.write(framed)
                    output.flush()
                } else if (bluetoothId != null) {
                    native.sendBluetooth(bluetoothId, framed)
                }
            } catch (error: IOException) {
                loop.post { closeLink(link, reason = "write: ${meshErrorText(error)}") }
            } finally {
                link.writeStartedMs = -1L
            }
        }
        return true
    }

    private fun sendToAll(plain: Map<String, Any?>) {
        for (link in ArrayList(links)) {
            if (!link.keyKnown) {
                if (link.pending.size < 32) link.pending.add(plain)
                continue
            }
            writePlain(link, plain)
        }
    }

    private fun closeLink(link: MeshLink, notify: Boolean = true, reason: String = "") {
        if (link.closed) return
        link.closed = true
        val who = if (link.peerId.isEmpty()) "unnamed link" else link.peerId.take(8)
        diag.lastDropReason = "$who: ${reason.ifEmpty { "closed" }}"
        links.remove(link)
        val duplicate = reason == "duplicate link"
        loop.io { releaseLink(link, duplicate) }
        val peerId = link.peerId
        if (peerId.isEmpty() || linkForPeer(peerId) != null) return
        val peer = peers[peerId] ?: return
        peer.linked = false
        if (notify) notifyListeners()
        if (!isRunning || reason in SUPERSEDED_REASONS || linkClock() - link.helloMs < REDIAL_MIN_LINK_MS) return
        if (link.bluetoothId != null) {
            loop.timer(BLUETOOTH_REDIAL_MS) {
                if (isRunning && linkForPeer(peerId) == null) loop.io { native.rescanBluetooth() }
            }
        } else {
            loop.timer(REDIAL_DELAY_MS) { redialPeer(peerId) }
        }
    }

    private fun releaseLink(link: MeshLink, duplicate: Boolean) {
        link.socket?.let(::closeQuietly)
        link.bluetoothId?.let { native.closeBluetoothLink(it, duplicate) }
        link.writer.shutdown()
    }

    private fun redialPeer(peerId: String) {
        if (!isRunning || linkForPeer(peerId) != null) return
        val peer = peers[peerId] ?: return
        val address = peer.address ?: return
        val port = peer.port
        if (port <= 0) return
        loop.io { connectToPeer(address, port) }
    }

    private fun linkClock(): Long = if (loop.virtualTime) loop.virtualElapsedMs else System.currentTimeMillis()

    private fun linkForPeer(peerId: String): MeshLink? {
        if (peerId.isEmpty()) return null
        return links.firstOrNull { it.peerId == peerId }
    }

    private fun envelope(type: String): LinkedHashMap<String, Any?> = linkedMapOf<String, Any?>(
        "type" to type,
        "id" to messageId(),
        "senderId" to nodeId,
        "sender" to name,
        "conversation" to MESH_CONVERSATION,
        "ts" to System.currentTimeMillis(),
        "platform" to platform,
        "version" to RelayRoom.CLIENT_VERSION,
    ).also { persona.stamp(it) }

    fun sendMessage(text: String): String = loop.call {
        val trimmed = dartTrimText(text)
        if (!isRunning || trimmed.isEmpty()) return@call ""
        val body = if (trimmed.length > MAX_TEXT_CHARS) trimmed.substring(0, MAX_TEXT_CHARS) else trimmed
        val message = envelope("chat")
        message["text"] = body
        val id = message["id"] as String
        markSeen(id)
        storeHistory(message)
        queued.add(id)
        diag.chatOut++
        sendToAll(message)
        val wake = wakeTargetsNow()
        if (wake.isNotEmpty()) wakeSender?.invoke(wake)
        appendMessage(toMessage(message))
        notifyListeners()
        id
    }

    fun sendTyping(active: Boolean) = loop.call {
        if (!isRunning) return@call
        val typing = envelope("typing")
        typing["active"] = active
        sendToAll(typing)
    }

    private fun rememberPeer(peerId: String, plain: Map<String, Any?>, linked: Boolean) {
        if (peerId.isEmpty() || peerId == nodeId) return
        val existing = peers[peerId]
        val peer = existing ?: MeshPeer().also { peers[peerId] = it }
        fun field(key: String): String {
            val value = dartTrimText(jsonText(plain[key]))
            return if (value.length > MAX_NAME_CHARS) value.substring(0, MAX_NAME_CHARS) else value
        }
        field("sender").takeIf { it.isNotEmpty() }?.let { peer.name = it }
        field("nodeName").takeIf { it.isNotEmpty() }?.let { peer.nodeName = it }
        field("ownerUser").takeIf { it.isNotEmpty() }?.let { peer.ownerUser = it }
        field("platform").takeIf { it.isNotEmpty() }?.let { peer.platform = it }
        field("version").takeIf { it.isNotEmpty() }?.let { peer.version = it }
        var controlChanged = false
        if (plain.containsKey("control")) {
            val control = plain["control"] == true
            controlChanged = control != peer.control
            peer.control = control
        }
        if (linked) {
            peer.linked = true
            peer.unlinkedSinceMs = 0
        }
        peer.lastSeenMs = System.currentTimeMillis()
        if (existing == null || linked || controlChanged) notifyListeners()
    }

    private fun sweepPeers() {
        val clock = linkClock()
        for (link in ArrayList(links)) {
            val started = link.writeStartedMs
            if (started >= 0 && clock - started >= WRITE_STALL_MS) closeLink(link, reason = "write stalled")
        }
        val now = System.currentTimeMillis()
        var changed = false
        val iterator = peers.entries.iterator()
        while (iterator.hasNext()) {
            val (id, peer) = iterator.next()
            val keep = linkForPeer(id) != null || now - peer.lastSeenMs <= peerTimeoutMs
            if (!keep) {
                changed = true
                iterator.remove()
            }
        }
        if (changed) notifyListeners()
    }

    private fun storeHistory(plain: Map<String, Any?>) {
        history.add(plain)
        while (history.size > HISTORY_LIMIT) queued.remove(jsonText(history.removeAt(0)["id"]))
        scheduleHistorySave()
    }

    private fun loadHistory(file: File?) {
        historyLoaded = true
        historyTarget = file
        if (file == null) return
        try {
            if (!file.exists()) return
            @Suppress("UNCHECKED_CAST")
            val root = Json.parse(file.readText(StandardCharsets.UTF_8)) as? Map<String, Any?> ?: return
            if (!dartNumEquals(root["v"], HISTORY_FILE_VERSION)) return
            val queuedIds = (root["queued"] as? List<*>).orEmpty().map { jsonText(it) }.toSet()
            for (entry in (root["history"] as? List<*>).orEmpty()) {
                @Suppress("UNCHECKED_CAST")
                val plain = entry as? Map<String, Any?> ?: continue
                val id = jsonText(plain["id"])
                if (id.isEmpty() || plain["type"] != "chat" || plain["conversation"] != MESH_CONVERSATION || !markSeen(id)) continue
                history.add(plain)
                rememberWake(jsonText(plain["senderId"]), plain)
                if (id in queuedIds && plain["senderId"] == nodeId) queued.add(id)
                appendMessage(toMessage(plain), countUnread = false)
            }
            while (history.size > HISTORY_LIMIT) history.removeAt(0)
        } catch (_: Exception) {
        }
    }

    private fun historySnapshot(): Map<String, Any?> = linkedMapOf(
        "v" to HISTORY_FILE_VERSION,
        "history" to ArrayList(history),
        "queued" to ArrayList(queued),
    )

    private fun scheduleHistorySave() {
        if (historyTarget == null) return
        historySaveTimer?.cancel()
        historySaveTimer = loop.timer(HISTORY_SAVE_DELAY_MS) {
            historySaveTimer = null
            val snapshot = historySnapshot()
            loop.io { saveHistory(snapshot) }
        }
    }

    private fun saveHistory(snapshot: Map<String, Any?>) {
        val file = historyTarget ?: return
        synchronized(historyWriteLock) {
            try {
                file.parentFile?.mkdirs()
                val temp = File(file.path + ".tmp")
                temp.writeText(Json.stringify(snapshot), StandardCharsets.UTF_8)
                if (!temp.renameTo(file)) {
                    file.delete()
                    temp.renameTo(file)
                }
            } catch (_: Exception) {
            }
        }
    }

    private val historyWriteLock = Any()

    private fun markSeen(id: String): Boolean {
        if (id in seenIds) return false
        seenIds.add(id)
        seenOrder.addLast(id)
        while (seenOrder.size > SEEN_CACHE_LIMIT) seenIds.remove(seenOrder.removeFirst())
        return true
    }

    private fun toMessage(plain: Map<String, Any?>): ChatMessage {
        val senderId = jsonText(plain["senderId"])
        val ts = plain["ts"]
        return ChatMessage(
            id = jsonText(plain["id"]),
            conversation = MESH_CONVERSATION,
            senderId = senderId,
            senderName = jsonText(plain["sender"]),
            text = jsonText(plain["text"]),
            timestamp = if (ts is Number) ts.toLong() else System.currentTimeMillis(),
            self = senderId == nodeId,
        )
    }

    private fun appendMessage(message: ChatMessage, countUnread: Boolean = true): Boolean {
        if (retention?.isExpired(message.timestamp, MESH_CONVERSATION, System.currentTimeMillis()) == true) return false
        if (messageList.any { it.id == message.id }) return false
        messageList.add(message)
        messageList.sortWith(compareBy<ChatMessage>({ it.timestamp }, { it.id }))
        while (messageList.size > HISTORY_LIMIT) messageList.removeAt(0)
        if (!message.self && countUnread) unreadCount++
        return true
    }

    private fun messageId(): String {
        val next = seq.incrementAndGet()
        val now = Instant.now()
        return "$nodeId-${now.epochSecond * 1_000_000L + now.nano / 1_000}-$next"
    }

    private fun notifyListeners() {
        for (listener in listeners) {
            try {
                listener()
            } catch (failure: Exception) {
                loop.failures.add(failure)
            }
        }
    }

    private fun closeQuietly(socket: Socket) {
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }

    fun dispose() {
        loop.call {
            beaconTimer?.cancel()
            sweepTimer?.cancel()
            sweepPaceTimer?.cancel()
            announceTimer?.cancel()
            if (historySaveTimer?.isActive == true) {
                historySaveTimer?.cancel()
                saveHistory(historySnapshot())
            }
            bluetoothListener?.let(native::removeEventListener)
            bluetoothListener = null
            discovery?.close()
            discovery = null
            try {
                server?.close()
            } catch (_: IOException) {
            }
            server = null
            for (link in ArrayList(links)) {
                link.closed = true
                releaseLink(link, duplicate = false)
            }
            links.clear()
        }
        listeners.clear()
        loop.shutdown()
    }

    private companion object {
        val ANY_IPV4: InetAddress = Inet4Address.getByAddress(byteArrayOf(0, 0, 0, 0))
        const val DEFAULT_GROUP_KEY = "default"
        val seq = AtomicLong()
    }
}
