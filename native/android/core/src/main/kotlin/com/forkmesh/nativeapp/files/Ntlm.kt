package com.forkmesh.nativeapp.files

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

data class NtlmChallenge(val flags: Long, val serverChallenge: ByteArray, val targetInfo: ByteArray) {
    val timestamp: ByteArray?
        get() {
            val reader = ByteReader(targetInfo)
            while (reader.remaining >= 4) {
                val id = reader.u16()
                val length = reader.u16()
                if (id == 0 || length > reader.remaining) return null
                val value = reader.bytes(length)
                if (id == 7 && length == 8) return value
            }
            return null
        }
}

data class NtlmAuthentication(val message: ByteArray, val sessionKey: ByteArray)

data class SmbCredentials(val user: String, val password: String, val domain: String = "") {
    val anonymous: Boolean get() = user.isEmpty() && password.isEmpty()

    companion object {
        val guest = SmbCredentials("guest", "")
        val anonymous = SmbCredentials("", "")

        fun parse(user: String, password: String): SmbCredentials {
            val typed = user.trim()
            val slash = typed.indexOf('\\')
            return if (slash > 0) SmbCredentials(typed.substring(slash + 1), password, typed.substring(0, slash))
            else SmbCredentials(typed, password)
        }
    }
}

object Ntlm {
    const val UNICODE = 0x00000001L
    const val REQUEST_TARGET = 0x00000004L
    const val NTLM = 0x00000200L
    const val ANONYMOUS = 0x00000800L
    const val ALWAYS_SIGN = 0x00008000L
    const val EXTENDED_SESSIONSECURITY = 0x00080000L
    const val TARGET_INFO = 0x00800000L
    const val NEGOTIATE_128 = 0x20000000L
    const val NEGOTIATE_56 = 0x80000000L
    const val WORKSTATION = "FORKMESH"

    val signature = "NTLMSSP\u0000".toByteArray(Charsets.US_ASCII)

    val baseFlags = UNICODE or REQUEST_TARGET or NTLM or ALWAYS_SIGN or EXTENDED_SESSIONSECURITY or TARGET_INFO or NEGOTIATE_128 or NEGOTIATE_56

    fun hmacMd5(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacMD5")
        mac.init(SecretKeySpec(key, "HmacMD5"))
        parts.forEach(mac::update)
        return mac.doFinal()
    }

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    fun ntowfV1(password: String): ByteArray = Md4.digest(utf16(password))

    fun ntowfV2(password: String, user: String, domain: String): ByteArray =
        hmacMd5(ntowfV1(password), utf16(user.uppercase() + domain))

    fun negotiate(anonymous: Boolean): ByteArray = ByteWriter()
        .bytes(signature)
        .u32(1)
        .u32(baseFlags or (if (anonymous) ANONYMOUS else 0L))
        .u16(0).u16(0).u32(0)
        .u16(0).u16(0).u32(0)
        .toByteArray()

    fun findMessage(buffer: ByteArray): Int {
        outer@ for (start in 0..buffer.size - signature.size) {
            for (i in signature.indices) if (buffer[start + i] != signature[i]) continue@outer
            return start
        }
        return -1
    }

    fun parseChallenge(buffer: ByteArray): NtlmChallenge {
        val start = findMessage(buffer)
        if (start < 0) throw SmbException(SmbStatus.PROTOCOL_ERROR, SmbStatus.message(SmbStatus.PROTOCOL_ERROR))
        val reader = ByteReader(buffer.copyOfRange(start, buffer.size))
        if (reader.u32(8) != 2L) throw SmbException(SmbStatus.PROTOCOL_ERROR, SmbStatus.message(SmbStatus.PROTOCOL_ERROR))
        val flags = reader.u32(20)
        val challenge = reader.bytes(24, 8)
        val infoLength = reader.u16(40)
        val infoOffset = reader.u32(44).toInt()
        val info = if (infoLength > 0) reader.bytes(infoOffset, infoLength) else ByteArray(0)
        return NtlmChallenge(flags, challenge, info)
    }

    fun ntlmV2Response(
        responseKey: ByteArray,
        serverChallenge: ByteArray,
        clientChallenge: ByteArray,
        timestamp: ByteArray,
        targetInfo: ByteArray,
    ): Pair<ByteArray, ByteArray> {
        val blob = ByteWriter().u8(1).u8(1).zeros(6).bytes(timestamp).bytes(clientChallenge).zeros(4).bytes(targetInfo).zeros(4).toByteArray()
        val proof = hmacMd5(responseKey, serverChallenge, blob)
        return (proof + blob) to hmacMd5(responseKey, proof)
    }

    fun fileTime(millis: Long): ByteArray = ByteWriter().u64((millis + 11_644_473_600_000L) * 10_000L).toByteArray()

    fun authenticate(
        challenge: NtlmChallenge,
        credentials: SmbCredentials,
        clientChallenge: ByteArray,
        nowMillis: Long,
    ): NtlmAuthentication {
        val anonymous = credentials.anonymous
        val lm: ByteArray
        val nt: ByteArray
        val key: ByteArray
        if (anonymous) {
            lm = ByteArray(1)
            nt = ByteArray(0)
            key = ByteArray(16)
        } else {
            val responseKey = ntowfV2(credentials.password, credentials.user, credentials.domain)
            val stamp = challenge.timestamp ?: fileTime(nowMillis)
            val (response, sessionKey) = ntlmV2Response(responseKey, challenge.serverChallenge, clientChallenge, stamp, challenge.targetInfo)
            lm = ByteArray(24)
            nt = response
            key = sessionKey
        }
        val domain = utf16(credentials.domain)
        val user = utf16(credentials.user)
        val workstation = utf16(WORKSTATION)
        val flags = baseFlags or (if (anonymous) ANONYMOUS else 0L)
        val header = 64
        var offset = header
        val writer = ByteWriter().bytes(signature).u32(3)
        for (field in listOf(lm, nt, domain, user, workstation, ByteArray(0))) {
            writer.u16(field.size).u16(field.size).u32(offset)
            offset += field.size
        }
        writer.u32(flags)
        writer.bytes(lm).bytes(nt).bytes(domain).bytes(user).bytes(workstation)
        return NtlmAuthentication(writer.toByteArray(), key)
    }
}

object Spnego {
    private val spnegoOid = byteArrayOf(0x2b, 0x06, 0x01, 0x05, 0x05, 0x02)
    private val ntlmOid = byteArrayOf(0x2b, 0x06, 0x01, 0x04, 0x01, 0x82.toByte(), 0x37, 0x02, 0x02, 0x0a)

    fun tlv(tag: Int, content: ByteArray): ByteArray {
        val length = content.size
        val lengthBytes = when {
            length < 0x80 -> byteArrayOf(length.toByte())
            length < 0x100 -> byteArrayOf(0x81.toByte(), length.toByte())
            else -> byteArrayOf(0x82.toByte(), (length ushr 8).toByte(), length.toByte())
        }
        return byteArrayOf(tag.toByte()) + lengthBytes + content
    }

    fun initToken(ntlm: ByteArray): ByteArray {
        val mechTypes = tlv(0xa0, tlv(0x30, tlv(0x06, ntlmOid)))
        val mechToken = tlv(0xa2, tlv(0x04, ntlm))
        return tlv(0x60, tlv(0x06, spnegoOid) + tlv(0xa0, tlv(0x30, mechTypes + mechToken)))
    }

    fun responseToken(ntlm: ByteArray): ByteArray = tlv(0xa1, tlv(0x30, tlv(0xa2, tlv(0x04, ntlm))))
}
