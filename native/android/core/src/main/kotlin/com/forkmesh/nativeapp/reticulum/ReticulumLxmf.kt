package com.forkmesh.nativeapp.reticulum

import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean

data class LxmfAttachment(val name: String, val data: ByteArray)

class LxmfMessage(
    val destinationHash: ByteArray,
    val sourceHash: ByteArray,
    val signature: ByteArray,
    val hash: ByteArray,
    val stamp: ByteArray,
    val signedPayload: ByteArray,
    val packed: ByteArray,
    val timestamp: Double,
    val title: String,
    val content: String,
    val attachments: List<LxmfAttachment>,
    val contentSize: Int,
)

class ResourceAdvertisement(
    val transferSize: Long = 0,
    val dataSize: Long = 0,
    val parts: Int = 0,
    val hash: ByteArray = ByteArray(0),
    val randomHash: ByteArray = ByteArray(0),
    val originalHash: ByteArray = ByteArray(0),
    val segmentIndex: Int = 1,
    val segments: Int = 1,
    val requestId: ByteArray = ByteArray(0),
    val flags: Int = 0,
    val hashmap: ByteArray = ByteArray(0),
) {
    val encrypted: Boolean get() = flags and 0x01 != 0
    val compressed: Boolean get() = flags and 0x02 != 0
    val split: Boolean get() = flags and 0x04 != 0
    val isRequest: Boolean get() = flags and 0x08 != 0
    val isResponse: Boolean get() = flags and 0x10 != 0
    val hasMetadata: Boolean get() = flags and 0x20 != 0
}

object Lxmf {
    const val OVERHEAD = 2 * Rns.HASH_LENGTH + Rns.SIGNATURE_LENGTH + 8 + 8
    const val ENCRYPTED_PACKET_MDU = 383
    const val LINK_MDU = 431
    const val OPPORTUNISTIC_MAX_CONTENT = ENCRYPTED_PACKET_MDU - OVERHEAD + Rns.HASH_LENGTH
    const val LINK_PACKET_MAX_CONTENT = LINK_MDU - OVERHEAD
    const val STAMP_ROUNDS = 3000
    const val MAX_STAMP_COST = 24
    const val RESOURCE_MAP_HASH_LENGTH = 4
    const val RESOURCE_RANDOM_LENGTH = 4
    const val RESOURCE_HASHMAP_MAX_LENGTH = (LINK_MDU - 134) / RESOURCE_MAP_HASH_LENGTH
    const val RESOURCE_MAX_SIZE = 1024 * 1024 - 1
    const val DELIVERY_ASPECT = "lxmf.delivery"

    private fun leadingZeroBits(digest: ByteArray): Int {
        var value = 0
        for (byte in digest) {
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

    fun payload(timestamp: Double, title: String, content: String): ByteArray =
        MsgPackWriter().array(4).real(timestamp).bin(title.toByteArray(Charsets.UTF_8)).bin(content.toByteArray(Charsets.UTF_8)).map(0).bytes()

    fun messageId(destinationHash: ByteArray, sourceHash: ByteArray, payload: ByteArray): ByteArray =
        Rns.fullHash(destinationHash, sourceHash, payload)

    fun pack(
        source: RnsIdentity,
        sourceHash: ByteArray,
        destinationHash: ByteArray,
        timestamp: Double,
        title: String,
        content: String,
        stamp: ByteArray = ByteArray(0),
    ): LxmfMessage? {
        if (!source.hasPrivateKey || sourceHash.size != Rns.HASH_LENGTH || destinationHash.size != Rns.HASH_LENGTH) return null
        val payload = payload(timestamp, title, content)
        val hash = messageId(destinationHash, sourceHash, payload)
        val signature = source.sign(destinationHash + sourceHash + payload + hash) ?: return null
        var packedPayload = payload
        if (stamp.isNotEmpty()) {
            packedPayload = payload.copyOf()
            packedPayload[0] = 0x95.toByte()
            packedPayload += MsgPackWriter().bin(stamp).bytes()
        }
        val packed = destinationHash + sourceHash + signature + packedPayload
        return LxmfMessage(destinationHash, sourceHash, signature, hash, stamp, payload, packed, timestamp, title, content, emptyList(), packedPayload.size - 16)
    }

    fun unpack(packed: ByteArray): Parsed<LxmfMessage> {
        val header = 2 * Rns.HASH_LENGTH + Rns.SIGNATURE_LENGTH
        if (packed.size <= header) return Parsed.failed("message is too short")
        val destination = packed.copyOf(Rns.HASH_LENGTH)
        val source = packed.copyOfRange(Rns.HASH_LENGTH, 2 * Rns.HASH_LENGTH)
        val signature = packed.copyOfRange(2 * Rns.HASH_LENGTH, header)
        val payload = packed.copyOfRange(header, packed.size)
        val items = RnsCrypto.msgPackArrayItems(payload)
        if (items == null || items.size < 4) return Parsed.failed("message payload is not an LXMF array")
        fun item(index: Int) = payload.copyOfRange(items[index][0], items[index][1])
        val signedPayload = byteArrayOf(0x94.toByte()) + payload.copyOfRange(items[0][0], items[3][1])
        val stamp = if (items.size >= 5) MsgPack.toBytes(Rns.decodeMsgPack(item(4)).value).let { it.copyOf(minOf(it.size, 64)) } else ByteArray(0)
        val hash = messageId(destination, source, signedPayload)
        val timestamp = Rns.decodeMsgPack(item(0))
        if (!timestamp.ok) return Parsed.failed("message timestamp is unreadable")
        val title = MsgPack.toText(Rns.decodeMsgPack(item(1)).value).take(1024)
        val content = MsgPack.toText(Rns.decodeMsgPack(item(2)).value)
        val fields = Rns.decodeMsgPack(item(3)).value as? Map<*, *> ?: emptyMap<String, Any?>()
        val attachments = (fields["5"] as? List<*>).orEmpty().mapNotNull { entry ->
            val pair = entry as? List<*> ?: return@mapNotNull null
            if (pair.size < 2) null else LxmfAttachment(MsgPack.toText(pair[0]).take(255), MsgPack.toBytes(pair[1]))
        }
        return Parsed.ok(
            LxmfMessage(destination, source, signature, hash, stamp, signedPayload, packed, MsgPack.toDouble(timestamp.value), title, content, attachments, payload.size - 16),
        )
    }

    fun verify(message: LxmfMessage, sourcePublicKey: ByteArray): Boolean {
        if (sourcePublicKey.size != Rns.PUBLIC_KEY_LENGTH) return false
        return RnsCrypto.ed25519Verify(
            sourcePublicKey.copyOfRange(32, 64),
            message.signature,
            message.destinationHash + message.sourceHash + message.signedPayload + message.hash,
        )
    }

    fun announceData(displayName: String, stampCost: Int = 0): ByteArray {
        val writer = MsgPackWriter().array(3)
        if (displayName.isEmpty()) writer.nil() else writer.bin(displayName.toByteArray(Charsets.UTF_8))
        if (stampCost in 1..254) writer.integer(stampCost) else writer.nil()
        return writer.array(0).bytes()
    }

    fun stampCostFromAppData(appData: ByteArray): Int {
        if (appData.isEmpty()) return 0
        val first = appData[0].toInt() and 0xff
        if (!(first in 0x90..0x9f || first == 0xdc)) return 0
        val parsed = Rns.decodeMsgPack(appData)
        val list = parsed.value as? List<*>
        if (!parsed.ok || list == null || list.size < 2 || list[1] == null) return 0
        val cost = MsgPack.toInt(list[1])
        return if (cost in 1..254) cost else 0
    }

    fun stampWorkblock(material: ByteArray, rounds: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream(rounds * 256)
        for (n in 0 until rounds) out.write(Rns.hkdf(256, material, Rns.fullHash(material, MsgPackWriter().integer(n).bytes())))
        return out.toByteArray()
    }

    fun stampValueFromWorkblock(workblock: ByteArray, stamp: ByteArray): Int = leadingZeroBits(Rns.fullHash(workblock, stamp))

    class Stamp(val stamp: ByteArray, val value: Int)

    fun generateStamp(messageId: ByteArray, cost: Int, cancel: AtomicBoolean? = null): Stamp? {
        if (cost <= 0 || cost > MAX_STAMP_COST) return null
        val workblock = stampWorkblock(messageId, STAMP_ROUNDS)
        val base = MessageDigest.getInstance("SHA-256")
        base.update(workblock)
        val candidate = ByteArray(32)
        val random = java.security.SecureRandom()
        var attempt = 0L
        while (true) {
            if (cancel != null && attempt and 0xff == 0L && cancel.get()) return null
            random.nextBytes(candidate)
            val work = base.clone() as MessageDigest
            work.update(candidate)
            val bits = leadingZeroBits(work.digest())
            if (bits >= cost) return Stamp(candidate.copyOf(), bits)
            attempt++
        }
    }

    fun validateStamp(messageId: ByteArray, stamp: ByteArray, cost: Int): Boolean {
        if (cost <= 0) return true
        if (stamp.size != 32) return false
        return stampValueFromWorkblock(stampWorkblock(messageId, STAMP_ROUNDS), stamp) >= cost
    }

    fun packResourceAdvertisement(advertisement: ResourceAdvertisement, segment: Int = 0): ByteArray {
        val start = segment * RESOURCE_HASHMAP_MAX_LENGTH
        val end = minOf((segment + 1) * RESOURCE_HASHMAP_MAX_LENGTH, advertisement.parts)
        val writer = MsgPackWriter().map(11)
        writer.str("t").integer(advertisement.transferSize)
        writer.str("d").integer(advertisement.dataSize)
        writer.str("n").integer(advertisement.parts)
        writer.str("h").bin(advertisement.hash)
        writer.str("r").bin(advertisement.randomHash)
        writer.str("o").bin(advertisement.originalHash)
        writer.str("i").integer(advertisement.segmentIndex)
        writer.str("l").integer(advertisement.segments)
        writer.str("q")
        if (advertisement.requestId.isEmpty()) writer.nil() else writer.bin(advertisement.requestId)
        writer.str("f").integer(advertisement.flags)
        val from = minOf(start * RESOURCE_MAP_HASH_LENGTH, advertisement.hashmap.size)
        val to = minOf(maxOf(from, end * RESOURCE_MAP_HASH_LENGTH), advertisement.hashmap.size)
        writer.str("m").bin(advertisement.hashmap.copyOfRange(from, to))
        return writer.bytes()
    }

    fun unpackResourceAdvertisement(bytes: ByteArray): ResourceAdvertisement? {
        val parsed = Rns.decodeMsgPack(bytes)
        val map = parsed.value as? Map<*, *>
        if (!parsed.ok || map == null) return null
        if (listOf("t", "d", "n", "h", "r", "o", "i", "l", "f", "m").any { !map.containsKey(it) }) return null
        fun long(key: String): Long = when (val value = map[key]) {
            is Long -> value
            is Double -> value.toLong()
            else -> MsgPack.toInt(value).toLong()
        }
        val advertisement = ResourceAdvertisement(
            transferSize = long("t"),
            dataSize = long("d"),
            parts = MsgPack.toInt(map["n"]),
            hash = MsgPack.toBytes(map["h"]),
            randomHash = MsgPack.toBytes(map["r"]),
            originalHash = MsgPack.toBytes(map["o"]),
            segmentIndex = MsgPack.toInt(map["i"]),
            segments = MsgPack.toInt(map["l"]),
            requestId = MsgPack.toBytes(map["q"]),
            flags = MsgPack.toInt(map["f"]),
            hashmap = MsgPack.toBytes(map["m"]),
        )
        if (advertisement.transferSize <= 0 || advertisement.transferSize > 3L * RESOURCE_MAX_SIZE ||
            advertisement.parts <= 0 || advertisement.parts > 100000 || advertisement.hash.size != Rns.FULL_HASH_LENGTH ||
            advertisement.randomHash.size != RESOURCE_RANDOM_LENGTH || advertisement.hashmap.size % RESOURCE_MAP_HASH_LENGTH != 0
        ) return null
        return advertisement
    }

    fun resourceMapHash(part: ByteArray, randomHash: ByteArray): ByteArray = Rns.fullHash(part, randomHash).copyOf(RESOURCE_MAP_HASH_LENGTH)
}
