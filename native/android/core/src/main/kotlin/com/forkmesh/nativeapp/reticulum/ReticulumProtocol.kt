package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.X25519
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object Rns {
    const val HASH_LENGTH = 16
    const val FULL_HASH_LENGTH = 32
    const val NAME_HASH_LENGTH = 10
    const val RANDOM_HASH_LENGTH = 10
    const val PUBLIC_KEY_LENGTH = 64
    const val PRIVATE_KEY_LENGTH = 64
    const val SIGNATURE_LENGTH = 64
    const val RATCHET_LENGTH = 32
    const val MTU = 500
    const val HEADER_MIN_SIZE = 2 + 1 + HASH_LENGTH
    const val HEADER_MAX_SIZE = 2 + 1 + HASH_LENGTH * 2
    const val STREAM_HARDWARE_MTU = 262144
    const val MAX_HOPS = 128
    const val STAMP_SIZE = 32
    const val DISCOVERY_EXPAND_ROUNDS = 20
    const val DISCOVERY_STAMP_VALUE = 16

    const val PACKET_DATA = 0
    const val PACKET_ANNOUNCE = 1
    const val PACKET_LINK_REQUEST = 2
    const val PACKET_PROOF = 3

    const val DESTINATION_SINGLE = 0
    const val DESTINATION_GROUP = 1
    const val DESTINATION_PLAIN = 2
    const val DESTINATION_LINK = 3

    const val HEADER_1 = 0
    const val HEADER_2 = 1
    const val TRANSPORT_BROADCAST = 0
    const val TRANSPORT_FORWARD = 1

    const val CONTEXT_NONE = 0x00
    const val CONTEXT_RESOURCE = 0x01
    const val CONTEXT_RESOURCE_ADVERTISEMENT = 0x02
    const val CONTEXT_RESOURCE_REQUEST = 0x03
    const val CONTEXT_RESOURCE_HASHMAP_UPDATE = 0x04
    const val CONTEXT_RESOURCE_PROOF = 0x05
    const val CONTEXT_RESOURCE_INITIATOR_CANCEL = 0x06
    const val CONTEXT_RESOURCE_RECEIVER_CANCEL = 0x07
    const val CONTEXT_CACHE_REQUEST = 0x08
    const val CONTEXT_REQUEST = 0x09
    const val CONTEXT_RESPONSE = 0x0A
    const val CONTEXT_PATH_RESPONSE = 0x0B
    const val CONTEXT_COMMAND = 0x0C
    const val CONTEXT_COMMAND_STATUS = 0x0D
    const val CONTEXT_CHANNEL = 0x0E
    const val CONTEXT_KEEPALIVE = 0xFA
    const val CONTEXT_LINK_IDENTIFY = 0xFB
    const val CONTEXT_LINK_CLOSE = 0xFC
    const val CONTEXT_LINK_PROOF = 0xFD
    const val CONTEXT_LINK_RTT = 0xFE
    const val CONTEXT_LINK_REQUEST_PROOF = 0xFF

    const val PATH_REQUEST_ASPECT = "rnstransport.path.request"

    private val random = SecureRandom()

    fun fullHash(vararg parts: ByteArray): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        for (part in parts) digest.update(part)
        return digest.digest()
    }

    fun truncatedHash(data: ByteArray): ByteArray = fullHash(data).copyOf(HASH_LENGTH)

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(if (key.isEmpty()) ByteArray(1) else key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    fun hkdf(length: Int, material: ByteArray, salt: ByteArray, context: ByteArray = ByteArray(0)): ByteArray {
        if (length < 1 || material.isEmpty()) return ByteArray(0)
        val key = hmacSha256(if (salt.isEmpty()) ByteArray(FULL_HASH_LENGTH) else salt, material)
        var block = ByteArray(0)
        val derived = java.io.ByteArrayOutputStream(length + FULL_HASH_LENGTH)
        var i = 0
        while (derived.size() < length) {
            block = hmacSha256(key, block + context + byteArrayOf(((i + 1) % 256).toByte()))
            derived.write(block)
            i++
        }
        return derived.toByteArray().copyOf(length)
    }

    fun randomBytes(length: Int): ByteArray = ByteArray(length).also { if (length > 0) random.nextBytes(it) }

    private val aspectPattern = Regex("^[^.\\s]+(\\.[^.\\s]+)*$")

    fun validAspectName(fullName: String): Boolean =
        fullName.isNotEmpty() && fullName.length <= 128 && aspectPattern.matches(fullName)

    fun nameHash(fullName: String): ByteArray = fullHash(fullName.toByteArray(Charsets.UTF_8)).copyOf(NAME_HASH_LENGTH)

    fun destinationHash(nameHash: ByteArray, identityHash: ByteArray = ByteArray(0)): ByteArray = truncatedHash(nameHash + identityHash)

    fun destinationHashForName(fullName: String, identityHash: ByteArray = ByteArray(0)): ByteArray =
        destinationHash(nameHash(fullName), identityHash)

    fun packedFlags(packet: Packet): Int =
        (if (packet.ifac) 0x80 else 0) or ((packet.headerType and 1) shl 6) or ((if (packet.contextFlag) 1 else 0) shl 5) or
            ((packet.transportType and 1) shl 4) or ((packet.destinationType and 3) shl 2) or (packet.packetType and 3)

    fun parsePacket(raw: ByteArray): Parsed<Packet> {
        if (raw.size <= HEADER_MIN_SIZE) return Parsed.failed("frame shorter than a Reticulum header")
        val flags = raw[0].toInt() and 0xff
        val ifac = flags and 0x80 != 0
        val headerType = (flags and 0x40) shr 6
        val hops = raw[1].toInt() and 0xff
        if (ifac) return Parsed.failed("packet carries an interface access code")
        if (hops >= MAX_HOPS) return Parsed.failed("hop count $hops is out of range")
        var offset = 2
        var transportId = ByteArray(0)
        if (headerType == HEADER_2) {
            if (raw.size <= HEADER_MAX_SIZE) return Parsed.failed("frame shorter than a transport header")
            transportId = raw.copyOfRange(offset, offset + HASH_LENGTH)
            offset += HASH_LENGTH
        }
        val destination = raw.copyOfRange(offset, offset + HASH_LENGTH)
        offset += HASH_LENGTH
        val context = raw[offset].toInt() and 0xff
        offset++
        val data = raw.copyOfRange(offset, raw.size)
        if (data.isEmpty()) return Parsed.failed("packet has no data")
        return Parsed.ok(
            Packet(
                ifac = false,
                headerType = headerType,
                contextFlag = flags and 0x20 != 0,
                transportType = (flags and 0x10) shr 4,
                destinationType = (flags and 0x0C) shr 2,
                packetType = flags and 0x03,
                hops = hops,
                transportId = transportId,
                destinationHash = destination,
                context = context,
                data = data,
            ),
        )
    }

    fun packPacket(packet: Packet): ByteArray {
        val out = java.io.ByteArrayOutputStream(HEADER_MAX_SIZE + packet.data.size)
        out.write(packedFlags(packet))
        out.write(packet.hops and 0xff)
        if (packet.headerType == HEADER_2) out.write(packet.transportId.copyOf(HASH_LENGTH))
        out.write(packet.destinationHash.copyOf(HASH_LENGTH))
        out.write(packet.context and 0xff)
        out.write(packet.data)
        return out.toByteArray()
    }

    fun packetHash(raw: ByteArray): ByteArray {
        if (raw.size < 3) return ByteArray(0)
        val header2 = raw[0].toInt() and 0x40 != 0
        val start = if (header2) HASH_LENGTH + 2 else 2
        val rest = if (start < raw.size) raw.copyOfRange(start, raw.size) else ByteArray(0)
        return fullHash(byteArrayOf((raw[0].toInt() and 0x0F).toByte()), rest)
    }

    fun hdlcFrame(data: ByteArray): ByteArray {
        val out = java.io.ByteArrayOutputStream(data.size + 8)
        out.write(HDLC_FLAG)
        for (byte in data) {
            val value = byte.toInt() and 0xff
            if (value == HDLC_ESCAPE || value == HDLC_FLAG) {
                out.write(HDLC_ESCAPE)
                out.write(value xor HDLC_MASK)
            } else {
                out.write(value)
            }
        }
        out.write(HDLC_FLAG)
        return out.toByteArray()
    }

    fun hdlcKeepalive(): ByteArray = byteArrayOf(HDLC_FLAG.toByte(), HDLC_FLAG.toByte())

    fun kissFrame(data: ByteArray): ByteArray {
        val out = java.io.ByteArrayOutputStream(data.size + 8)
        out.write(KISS_FEND)
        out.write(KISS_DATA)
        for (byte in data) {
            when (val value = byte.toInt() and 0xff) {
                KISS_FESC -> {
                    out.write(KISS_FESC)
                    out.write(KISS_TFESC)
                }
                KISS_FEND -> {
                    out.write(KISS_FESC)
                    out.write(KISS_TFEND)
                }
                else -> out.write(value)
            }
        }
        out.write(KISS_FEND)
        return out.toByteArray()
    }

    fun parseAnnounce(packet: Packet): Parsed<Announce> {
        if (packet.packetType != PACKET_ANNOUNCE) return Parsed.failed("not an announce")
        val ratchetLength = if (packet.contextFlag) RATCHET_LENGTH else 0
        val fixed = PUBLIC_KEY_LENGTH + NAME_HASH_LENGTH + RANDOM_HASH_LENGTH + ratchetLength + SIGNATURE_LENGTH
        val data = packet.data
        if (data.size < fixed) return Parsed.failed("announce is ${data.size} bytes, shorter than $fixed")
        var offset = 0
        fun take(count: Int): ByteArray = data.copyOfRange(offset, offset + count).also { offset += count }
        val publicKey = take(PUBLIC_KEY_LENGTH)
        val name = take(NAME_HASH_LENGTH)
        val randomHash = take(RANDOM_HASH_LENGTH)
        val ratchet = take(ratchetLength)
        val signature = take(SIGNATURE_LENGTH)
        val appData = data.copyOfRange(offset, data.size)
        var emitted = 0L
        for (i in 5 until RANDOM_HASH_LENGTH) emitted = (emitted shl 8) or (randomHash[i].toLong() and 0xff)
        return Parsed.ok(
            Announce(
                destinationHash = packet.destinationHash,
                publicKey = publicKey,
                nameHash = name,
                randomHash = randomHash,
                ratchet = ratchet,
                signature = signature,
                appData = appData,
                identityHash = truncatedHash(publicKey),
                emittedAt = emitted,
            ),
        )
    }

    fun validateAnnounce(announce: Announce): String? {
        val identity = RnsIdentity.fromPublicKey(announce.publicKey) ?: return "announce public key is not a valid key pair"
        val signed = announce.destinationHash + announce.publicKey + announce.nameHash + announce.randomHash + announce.ratchet + announce.appData
        if (!identity.verify(announce.signature, signed)) return "announce signature does not verify"
        if (!destinationHash(announce.nameHash, identity.hash).contentEquals(announce.destinationHash)) return "announce destination does not match its key"
        return null
    }

    fun buildAnnounce(
        identity: RnsIdentity,
        fullName: String,
        appData: ByteArray,
        pathResponse: Boolean,
        unixTime: Long,
        randomPrefix: ByteArray = ByteArray(0),
    ): ByteArray {
        if (!identity.hasPrivateKey || !validAspectName(fullName)) return ByteArray(0)
        val name = nameHash(fullName)
        val destination = destinationHash(name, identity.hash)
        val prefix = if (randomPrefix.size >= 5) randomPrefix.copyOf(5) else fullHash(randomBytes(32)).copyOf(5)
        val time = ByteArray(5) { ((unixTime ushr (32 - it * 8)) and 0xff).toByte() }
        val randomHash = prefix + time
        val signature = identity.sign(destination + identity.publicKey + name + randomHash + appData) ?: return ByteArray(0)
        return packPacket(
            Packet(
                packetType = PACKET_ANNOUNCE,
                destinationType = DESTINATION_SINGLE,
                destinationHash = destination,
                context = if (pathResponse) CONTEXT_PATH_RESPONSE else CONTEXT_NONE,
                data = identity.publicKey + name + randomHash + signature + appData,
            ),
        )
    }

    val pathRequestDestination: ByteArray by lazy { destinationHashForName(PATH_REQUEST_ASPECT) }

    fun buildPathRequest(destination: ByteArray, tag: ByteArray): ByteArray {
        if (destination.size != HASH_LENGTH || tag.isEmpty()) return ByteArray(0)
        return packPacket(
            Packet(
                packetType = PACKET_DATA,
                destinationType = DESTINATION_PLAIN,
                destinationHash = pathRequestDestination,
                data = destination + tag.copyOf(minOf(tag.size, HASH_LENGTH)),
            ),
        )
    }

    fun parsePathRequest(packet: Packet): PathRequest? {
        if (packet.packetType != PACKET_DATA || packet.destinationType != DESTINATION_PLAIN ||
            !packet.destinationHash.contentEquals(pathRequestDestination) || packet.data.size < HASH_LENGTH
        ) return null
        val data = packet.data
        val tag = when {
            data.size > HASH_LENGTH * 2 -> data.copyOfRange(HASH_LENGTH * 2, minOf(data.size, HASH_LENGTH * 3))
            data.size > HASH_LENGTH -> data.copyOfRange(HASH_LENGTH, minOf(data.size, HASH_LENGTH * 2))
            else -> ByteArray(0)
        }
        return PathRequest(data.copyOf(HASH_LENGTH), tag)
    }

    fun decodeMsgPack(bytes: ByteArray): Parsed<Any?> {
        if (bytes.isEmpty()) return Parsed.failed("empty msgpack")
        val reader = MsgPackReader(bytes)
        val value = reader.read(0)
        return if (reader.ok) Parsed.ok(value) else Parsed.failed("malformed msgpack")
    }

    val knownAspects: List<KnownAspect> by lazy {
        listOf(
            Triple("lxmf.delivery", "LXMF messaging", "peer"),
            Triple("lxmf.propagation", "LXMF propagation node", "propagation"),
            Triple("lxmf.propagation.control", "Propagation node control", "propagation"),
            Triple("nomadnetwork.node", "Nomad Network node", "node"),
            Triple("rnstransport.discovery.interface", "Interface discovery", "discovery"),
            Triple("rnstransport.remote.management", "Transport management", "transport"),
            Triple("rnstransport.info.blackhole", "Blackhole list", "transport"),
            Triple("rnstransport.probe", "Transport probe", "transport"),
            Triple("lxst.telephony", "LXST voice calls", "peer"),
            Triple("call.audio", "Voice calls", "peer"),
            Triple("rncp.receive", "File transfer (rncp)", "service"),
            Triple("rnx.execute", "Remote command (rnx)", "service"),
            Triple("forkmesh.node", "ForkMesh device", "forkmesh"),
        ).map { (name, label, kind) -> KnownAspect(name, label, kind, nameHash(name)) }
    }

    fun knownAspect(nameHash: ByteArray): KnownAspect? = knownAspects.firstOrNull { it.nameHash.contentEquals(nameHash) }

    fun displayNameFromAppData(kind: String, appData: ByteArray): String {
        if (appData.isEmpty()) return ""
        val first = appData[0].toInt() and 0xff
        if (kind == "propagation") {
            val list = decodeMsgPack(appData).value as? List<*> ?: return ""
            if (list.size < 7) return ""
            val metadata = list[6] as? Map<*, *> ?: emptyMap<String, Any?>()
            return cleanText(metadata["1"])
        }
        if (kind == "discovery") return ""
        if (first in 0x90..0x9f || first == 0xdc) {
            val parsed = decodeMsgPack(appData)
            val list = parsed.value as? List<*>
            if (!parsed.ok || list.isNullOrEmpty()) return ""
            return cleanText(list.first())
        }
        val text = String(appData, Charsets.UTF_8)
        if ('�' in text) return ""
        return cleanText(text)
    }

    fun stampValue(material: ByteArray, stamp: ByteArray, expandRounds: Int): Int {
        val workblock = java.io.ByteArrayOutputStream(expandRounds * 256)
        for (n in 0 until expandRounds) workblock.write(hkdf(256, material, fullHash(material, byteArrayOf(n.toByte()))))
        val result = fullHash(workblock.toByteArray(), stamp)
        var value = 0
        for (byte in result) {
            val bits = byte.toInt() and 0xff
            if (bits == 0) {
                value += 8
                continue
            }
            var bit = 7
            while (bit >= 0 && bits and (1 shl bit) == 0) {
                value++
                bit--
            }
            break
        }
        return value
    }

    fun parseDiscoveryAppData(appData: ByteArray): Parsed<DiscoveredInterface> {
        if (appData.size <= STAMP_SIZE + 1) return Parsed.failed("discovery data is too short")
        val flags = appData[0].toInt() and 0xff
        if (flags and 0x02 != 0) return Parsed.failed("discovery data is encrypted for its network")
        val payload = appData.copyOfRange(1, appData.size)
        val packed = payload.copyOf(payload.size - STAMP_SIZE)
        val stamp = payload.copyOfRange(payload.size - STAMP_SIZE, payload.size)
        val decoded = decodeMsgPack(packed)
        val info = decoded.value as? Map<*, *>
        if (!decoded.ok || info.isNullOrEmpty()) return Parsed.failed("discovery data is not a msgpack map")
        fun value(key: Int): Any? = info[key.toString()]
        val latitude = value(0x03)
        val longitude = value(0x04)
        var hasLocation = false
        var lat = 0.0
        var lon = 0.0
        var height = 0.0
        if (latitude != null && longitude != null) {
            lat = MsgPack.toDouble(latitude)
            lon = MsgPack.toDouble(longitude)
            height = MsgPack.toDouble(value(0x05))
            hasLocation = lat.isFinite() && lon.isFinite() && kotlin.math.abs(lat) <= 90 && kotlin.math.abs(lon) <= 180
        }
        var port = MsgPack.toInt(value(0x06))
        if (port < 0 || port > 65535) port = 0
        val stampBits = stampValue(fullHash(packed), stamp, DISCOVERY_EXPAND_ROUNDS)
        return Parsed.ok(
            DiscoveredInterface(
                name = cleanText(value(0xFF), 80),
                type = cleanText(value(0x00), 40),
                reachableOn = cleanText(value(0x02), 255),
                port = port,
                transport = MsgPack.toBool(value(0x01)),
                hasLocation = hasLocation,
                latitude = lat,
                longitude = lon,
                height = height,
                implementation = cleanText(value(0xFD), 40),
                version = cleanText(value(0xFC), 40),
                transportId = MsgPack.toBytes(value(0xFE)).let { it.copyOf(minOf(it.size, HASH_LENGTH)) },
                networkName = cleanText(value(0x07), 80),
                publishesPassphrase = value(0x08) != null,
                frequency = MsgPack.toDouble(value(0x09)),
                bandwidth = MsgPack.toDouble(value(0x0A)),
                modulation = cleanText(value(0x0D), 40),
                stampValue = stampBits,
                stampValid = stampBits >= DISCOVERY_STAMP_VALUE,
            ),
        )
    }

    fun cleanText(value: Any?, limit: Int = 120): String {
        val text = MsgPack.toText(value).replace("\u0000", "")
        val out = StringBuilder(minOf(text.length, limit))
        for (ch in text) {
            if (out.length >= limit) break
            out.append(if (printable(ch)) ch else ' ')
        }
        return simplified(out.toString())
    }

    private fun printable(ch: Char): Boolean = when (Character.getType(ch).toByte()) {
        Character.CONTROL, Character.FORMAT, Character.SURROGATE, Character.PRIVATE_USE, Character.UNASSIGNED -> false
        else -> true
    }

    private fun simplified(text: String): String {
        val out = StringBuilder(text.length)
        var pendingSpace = false
        for (ch in text) {
            if (ch.isWhitespace()) {
                pendingSpace = out.isNotEmpty()
            } else {
                if (pendingSpace) out.append(' ')
                pendingSpace = false
                out.append(ch)
            }
        }
        return out.toString()
    }

    fun hex(bytes: ByteArray): String {
        val chars = CharArray(bytes.size * 2)
        for (i in bytes.indices) {
            val value = bytes[i].toInt() and 0xff
            chars[i * 2] = HEX[value ushr 4]
            chars[i * 2 + 1] = HEX[value and 0x0f]
        }
        return String(chars)
    }

    fun fromHex(text: String): ByteArray? {
        if (text.length % 2 != 0) return null
        val out = ByteArray(text.length / 2)
        for (i in out.indices) {
            val high = Character.digit(text[i * 2], 16)
            val low = Character.digit(text[i * 2 + 1], 16)
            if (high < 0 || low < 0) return null
            out[i] = ((high shl 4) or low).toByte()
        }
        return out
    }

    fun prettyHash(hash: ByteArray): String = "<${hex(hash)}>"

    fun shortHash(hash: ByteArray): String {
        val text = hex(hash)
        return if (text.length > 12) "<${text.take(6)}…${text.takeLast(4)}>" else prettyHash(hash)
    }

    private val hashPattern = Regex("^[0-9a-fA-F]{32}$")

    fun parseHash(text: String): ByteArray? {
        val clean = text.trim().filter { it != '<' && it != '>' && it != ':' && it != ' ' }
        if (!hashPattern.matches(clean)) return null
        return fromHex(clean)
    }

    fun contextName(context: Int): String = when (context) {
        CONTEXT_NONE -> "none"
        in CONTEXT_RESOURCE..CONTEXT_RESOURCE_RECEIVER_CANCEL -> "resource"
        CONTEXT_CACHE_REQUEST -> "cache request"
        CONTEXT_REQUEST -> "request"
        CONTEXT_RESPONSE -> "response"
        CONTEXT_PATH_RESPONSE -> "path response"
        CONTEXT_COMMAND, CONTEXT_COMMAND_STATUS -> "command"
        CONTEXT_CHANNEL -> "channel"
        CONTEXT_KEEPALIVE -> "keepalive"
        in CONTEXT_LINK_IDENTIFY..CONTEXT_LINK_REQUEST_PROOF -> "link"
        else -> "0x%02x".format(context)
    }

    fun packetTypeName(type: Int): String = when (type) {
        PACKET_DATA -> "data"
        PACKET_ANNOUNCE -> "announce"
        PACKET_LINK_REQUEST -> "link request"
        PACKET_PROOF -> "proof"
        else -> "unknown"
    }

    fun destinationTypeName(type: Int): String = when (type) {
        DESTINATION_SINGLE -> "single"
        DESTINATION_GROUP -> "group"
        DESTINATION_PLAIN -> "plain"
        DESTINATION_LINK -> "link"
        else -> "unknown"
    }

    internal const val HDLC_FLAG = 0x7E
    internal const val HDLC_ESCAPE = 0x7D
    internal const val HDLC_MASK = 0x20
    internal const val KISS_FEND = 0xC0
    internal const val KISS_FESC = 0xDB
    internal const val KISS_TFEND = 0xDC
    internal const val KISS_TFESC = 0xDD
    internal const val KISS_DATA = 0x00

    private val HEX = "0123456789abcdef".toCharArray()
}

class Parsed<T> private constructor(val value: T?, val error: String, val ok: Boolean) {
    companion object {
        fun <T> ok(value: T): Parsed<T> = Parsed(value, "", true)
        fun <T> failed(error: String): Parsed<T> = Parsed(null, error, false)
    }
}

class Packet(
    val ifac: Boolean = false,
    val headerType: Int = Rns.HEADER_1,
    val contextFlag: Boolean = false,
    val transportType: Int = Rns.TRANSPORT_BROADCAST,
    val destinationType: Int = Rns.DESTINATION_SINGLE,
    val packetType: Int = Rns.PACKET_DATA,
    val hops: Int = 0,
    val transportId: ByteArray = ByteArray(0),
    val destinationHash: ByteArray = ByteArray(0),
    val context: Int = Rns.CONTEXT_NONE,
    val data: ByteArray = ByteArray(0),
)

class Announce(
    val destinationHash: ByteArray,
    val publicKey: ByteArray,
    val nameHash: ByteArray,
    val randomHash: ByteArray,
    val ratchet: ByteArray,
    val signature: ByteArray,
    val appData: ByteArray,
    val identityHash: ByteArray,
    val emittedAt: Long,
) {
    val hasAppData: Boolean get() = appData.isNotEmpty()
}

class PathRequest(val destination: ByteArray, val tag: ByteArray)

class KnownAspect(val name: String, val label: String, val kind: String, val nameHash: ByteArray)

data class DiscoveredInterface(
    val name: String = "",
    val type: String = "",
    val reachableOn: String = "",
    val port: Int = 0,
    val transport: Boolean = false,
    val hasLocation: Boolean = false,
    val latitude: Double = 0.0,
    val longitude: Double = 0.0,
    val height: Double = 0.0,
    val implementation: String = "",
    val version: String = "",
    val transportId: ByteArray = ByteArray(0),
    val networkName: String = "",
    val publishesPassphrase: Boolean = false,
    val frequency: Double = 0.0,
    val bandwidth: Double = 0.0,
    val modulation: String = "",
    val encrypted: Boolean = false,
    val stampValue: Int = 0,
    val stampValid: Boolean = false,
) {
    val connectable: Boolean
        get() = reachableOn.isNotEmpty() && port > 0 && (type == "TCPServerInterface" || type == "BackboneInterface")
}

class FrameDecoder(framing: Framing = Framing.Hdlc, private val maximumFrame: Int = Rns.STREAM_HARDWARE_MTU) {
    enum class Framing { Hdlc, Kiss }

    var framing: Framing = framing
        private set
    var invalidFrames: Long = 0
        private set

    private var buffer = java.io.ByteArrayOutputStream()
    private var inFrame = false
    private var escape = false
    private var kissCommand = -1
    private var overflow = false

    fun reset() {
        buffer.reset()
        inFrame = false
        escape = false
        kissCommand = -1
        overflow = false
    }

    fun changeFraming(value: Framing) {
        framing = value
        reset()
    }

    fun feed(bytes: ByteArray, count: Int = bytes.size): List<ByteArray> =
        if (framing == Framing.Kiss) feedKiss(bytes, count) else feedHdlc(bytes, count)

    private fun finishFrame(frames: MutableList<ByteArray>) {
        if (overflow) {
            invalidFrames++
        } else if (buffer.size() > 0) {
            if (buffer.size() <= Rns.HEADER_MIN_SIZE) invalidFrames++ else frames.add(buffer.toByteArray())
        }
        buffer.reset()
        escape = false
        overflow = false
    }

    private fun feedHdlc(bytes: ByteArray, count: Int): List<ByteArray> {
        val frames = ArrayList<ByteArray>()
        for (index in 0 until count) {
            val value = bytes[index].toInt() and 0xff
            if (value == Rns.HDLC_FLAG) {
                if (inFrame) finishFrame(frames)
                inFrame = true
                continue
            }
            if (!inFrame || overflow) continue
            if (escape) {
                escape = false
                buffer.write(value xor Rns.HDLC_MASK)
            } else if (value == Rns.HDLC_ESCAPE) {
                escape = true
                continue
            } else {
                buffer.write(value)
            }
            if (buffer.size() > maximumFrame) {
                overflow = true
                buffer.reset()
            }
        }
        return frames
    }

    private fun feedKiss(bytes: ByteArray, count: Int): List<ByteArray> {
        val frames = ArrayList<ByteArray>()
        for (index in 0 until count) {
            val value = bytes[index].toInt() and 0xff
            if (value == Rns.KISS_FEND) {
                if (inFrame && kissCommand == Rns.KISS_DATA) finishFrame(frames)
                inFrame = true
                kissCommand = -1
                buffer.reset()
                escape = false
                overflow = false
                continue
            }
            if (!inFrame) continue
            if (kissCommand < 0) {
                kissCommand = value and 0x0F
                continue
            }
            if (kissCommand != Rns.KISS_DATA || overflow) continue
            if (value == Rns.KISS_FESC) {
                escape = true
                continue
            }
            var out = value
            if (escape) {
                if (value == Rns.KISS_TFEND) out = Rns.KISS_FEND else if (value == Rns.KISS_TFESC) out = Rns.KISS_FESC
                escape = false
            }
            buffer.write(out)
            if (buffer.size() > maximumFrame) {
                overflow = true
                buffer.reset()
            }
        }
        return frames
    }
}

class RnsIdentity private constructor(val publicKey: ByteArray, private val secret: ByteArray?) {
    val hasPrivateKey: Boolean get() = secret != null
    val privateKey: ByteArray get() = secret?.copyOf() ?: ByteArray(0)
    val hash: ByteArray = Rns.truncatedHash(publicKey)
    val encryptionPublicKey: ByteArray get() = publicKey.copyOfRange(0, 32)
    val signingPublicKey: ByteArray get() = publicKey.copyOfRange(32, 64)

    fun sign(data: ByteArray): ByteArray? {
        val key = secret ?: return null
        return Ed25519.sign(key.copyOfRange(32, 64), data, signingPublicKey)
    }

    fun verify(signature: ByteArray, data: ByteArray): Boolean {
        if (signature.size != Rns.SIGNATURE_LENGTH) return false
        return runCatching { Ed25519.verify(signingPublicKey, data, signature) }.getOrDefault(false)
    }

    companion object {
        fun generate(): RnsIdentity {
            val encryption = Rns.randomBytes(32)
            val signing = Ed25519.generateSeed()
            return fromPrivateKey(encryption + signing)!!
        }

        fun fromPrivateKey(key: ByteArray): RnsIdentity? {
            if (key.size != Rns.PRIVATE_KEY_LENGTH) return null
            return runCatching {
                val encryptionPublic = X25519.publicKey(key.copyOfRange(0, 32))
                val signingPublic = Ed25519.publicKey(key.copyOfRange(32, 64))
                RnsIdentity(encryptionPublic + signingPublic, key.copyOf())
            }.getOrNull()
        }

        fun fromPublicKey(key: ByteArray): RnsIdentity? {
            if (key.size != Rns.PUBLIC_KEY_LENGTH) return null
            return RnsIdentity(key.copyOf(), null)
        }
    }
}

object MsgPack {
    fun toText(value: Any?): String = when (value) {
        null -> ""
        is String -> value
        is ByteArray -> String(value, Charsets.UTF_8)
        is Boolean -> value.toString()
        is Long -> value.toString()
        is Int -> value.toString()
        is Double -> if (value.isFinite() && value == kotlin.math.floor(value) && kotlin.math.abs(value) < 1e15) value.toLong().toString() else value.toString()
        else -> ""
    }

    fun toDouble(value: Any?): Double = when (value) {
        is Double -> value
        is Long -> value.toDouble()
        is Int -> value.toDouble()
        is Boolean -> if (value) 1.0 else 0.0
        is String -> value.trim().toDoubleOrNull() ?: 0.0
        is ByteArray -> String(value, Charsets.UTF_8).trim().toDoubleOrNull() ?: 0.0
        else -> 0.0
    }

    fun toInt(value: Any?): Int = when (value) {
        is Long -> value.toInt()
        is Int -> value
        is Double -> if (value.isFinite()) kotlin.math.round(value).toInt() else 0
        is Boolean -> if (value) 1 else 0
        is String -> value.trim().toIntOrNull() ?: 0
        is ByteArray -> String(value, Charsets.UTF_8).trim().toIntOrNull() ?: 0
        else -> 0
    }

    fun toBool(value: Any?): Boolean = when (value) {
        is Boolean -> value
        is Long -> value != 0L
        is Int -> value != 0
        is Double -> value != 0.0
        is String -> value.isNotEmpty() && value != "0" && !value.equals("false", ignoreCase = true)
        is ByteArray -> toBool(String(value, Charsets.UTF_8))
        else -> false
    }

    fun toBytes(value: Any?): ByteArray = when (value) {
        is ByteArray -> value
        is String -> value.toByteArray(Charsets.UTF_8)
        else -> ByteArray(0)
    }
}

internal class MsgPackReader(private val bytes: ByteArray) {
    var ok = true
        private set
    private var offset = 0
    private var items = 0

    val position: Int get() = offset
    val atEnd: Boolean get() = offset == bytes.size

    fun read(depth: Int): Any? {
        if (!ok) return null
        if (depth > MAX_DEPTH || offset >= bytes.size || ++items > MAX_ITEMS) return fail()
        val type = bytes[offset++].toInt() and 0xff
        if (type <= 0x7f) return type.toLong()
        if (type >= 0xe0) return type.toByte().toLong()
        if (type and 0xf0 == 0x80) return readMap((type and 0x0f).toLong(), depth)
        if (type and 0xf0 == 0x90) return readArray((type and 0x0f).toLong(), depth)
        if (type and 0xe0 == 0xa0) return readString((type and 0x1f).toLong())
        return when (type) {
            0xc0 -> null
            0xc2 -> false
            0xc3 -> true
            0xc4 -> readBytes(readUnsigned(1))
            0xc5 -> readBytes(readUnsigned(2))
            0xc6 -> readBytes(readUnsigned(4))
            0xca -> java.lang.Float.intBitsToFloat(readUnsigned(4).toInt()).toDouble()
            0xcb -> java.lang.Double.longBitsToDouble(readUnsigned(8))
            0xcc -> readUnsigned(1)
            0xcd -> readUnsigned(2)
            0xce -> readUnsigned(4)
            0xcf -> readUnsigned(8)
            0xd0 -> readUnsigned(1).toByte().toLong()
            0xd1 -> readUnsigned(2).toShort().toLong()
            0xd2 -> readUnsigned(4).toInt().toLong()
            0xd3 -> readUnsigned(8)
            0xd9 -> readString(readUnsigned(1))
            0xda -> readString(readUnsigned(2))
            0xdb -> readString(readUnsigned(4))
            0xdc -> readArray(readUnsigned(2), depth)
            0xdd -> readArray(readUnsigned(4), depth)
            0xde -> readMap(readUnsigned(2), depth)
            0xdf -> readMap(readUnsigned(4), depth)
            else -> fail()
        }
    }

    private fun fail(): Any? {
        ok = false
        return null
    }

    private fun remaining(): Long = (bytes.size - offset).toLong()

    private fun readUnsigned(width: Int): Long {
        if (offset + width > bytes.size) {
            ok = false
            return 0
        }
        var value = 0L
        repeat(width) { value = (value shl 8) or (bytes[offset++].toLong() and 0xff) }
        return value
    }

    private fun readBytes(length: Long): ByteArray? {
        if (!ok || length < 0 || length > remaining()) {
            ok = false
            return null
        }
        val out = bytes.copyOfRange(offset, offset + length.toInt())
        offset += length.toInt()
        return out
    }

    private fun readString(length: Long): String? = readBytes(length)?.let { String(it, Charsets.UTF_8) }

    private fun readArray(count: Long, depth: Int): List<Any?>? {
        if (!ok || count < 0 || count > remaining()) {
            ok = false
            return null
        }
        val list = ArrayList<Any?>(count.toInt())
        var i = 0L
        while (i < count && ok) {
            list.add(read(depth + 1))
            i++
        }
        return list
    }

    private fun readMap(count: Long, depth: Int): Map<String, Any?>? {
        if (!ok || count < 0 || count * 2 > remaining()) {
            ok = false
            return null
        }
        val map = LinkedHashMap<String, Any?>()
        var i = 0L
        while (i < count && ok) {
            val key = read(depth + 1)
            val value = read(depth + 1)
            map[MsgPack.toText(key)] = value
            i++
        }
        return map
    }

    private companion object {
        const val MAX_DEPTH = 8
        const val MAX_ITEMS = 4096
    }
}
