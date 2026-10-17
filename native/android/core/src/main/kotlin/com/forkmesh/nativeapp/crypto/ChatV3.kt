package com.forkmesh.nativeapp.crypto

object ChatV3 {
    const val VERSION = 3L
    const val DAY_MS = 24L * 60 * 60 * 1000
    const val PREKEY_LIFE_MS = 8 * DAY_MS
    const val PREKEY_ROTATE_MS = DAY_MS
    const val MAX_PREKEYS = 10
    const val MAX_RECIPIENTS = 64
    const val REPLAY_WINDOW = 1024
    const val MAX_HELD = 64
    const val KID_CHARS = 16
    const val NONCE_BYTES = 12
    const val TAG_BYTES = 16
    const val KEY_BYTES = 32
    const val SIGNATURE_BYTES = 64
    const val SAFETY_ROUNDS = 5199

    const val SEALED_TYPE = "sealed"
    const val KEY_TYPE = "e2ee-key"

    private const val BUNDLE_LABEL = "forkmesh-chat-e2ee-bundle-v3"
    private const val CONTENT_LABEL = "forkmesh-chat-e2ee-content-v3"
    private const val WRAP_LABEL = "forkmesh-chat-e2ee-wrap-v3"
    private const val KEK_LABEL = "forkmesh-chat-e2ee-kek-v3"
    private const val SEALED_LABEL = "forkmesh-chat-e2ee-sealed-v3"
    private const val SALT_PREFIX = "ForkMesh chat e2ee salt v3:"
    private const val SAFETY_LABEL = "forkmesh-safety-v3"

    private val URL_CHARS = Regex("[A-Za-z0-9_-]*")
    private val STANDARD_CHARS = Regex("[A-Za-z0-9+/]*={0,2}")
    private val KID_PATTERN = Regex("[A-Za-z0-9_-]{16}")

    fun b64u(bytes: ByteArray): String = B64.encodeUrlNoPadding(bytes)

    fun b64(bytes: ByteArray): String = B64.encode(bytes)

    fun unb64u(text: Any?): ByteArray? {
        if (text !is String || !URL_CHARS.matches(text) || text.length % 4 == 1) return null
        val padded = text + "=".repeat((4 - text.length % 4) % 4)
        return B64.decodeOrNull(padded)
    }

    fun unb64(text: Any?): ByteArray? {
        if (text !is String || text.length % 4 != 0 || !STANDARD_CHARS.matches(text)) return null
        return B64.decodeOrNull(text)
    }

    fun id16(bytes: ByteArray): String = b64u(Sha256.hash(bytes)).substring(0, KID_CHARS)

    fun isKid(value: Any?): Boolean = value is String && KID_PATTERN.matches(value)

    fun integer(value: Any?): Long? = when (value) {
        is Long -> value
        is Int -> value.toLong()
        is Short -> value.toLong()
        is Byte -> value.toLong()
        else -> null
    }

    fun validAcct(acct: Any?): Boolean {
        if (acct !is String) return false
        val count = acct.codePointCount(0, acct.length)
        if (count < 1 || count > 128) return false
        var i = 0
        while (i < acct.length) {
            val cp = acct.codePointAt(i)
            if (!printable(cp)) return false
            i += Character.charCount(cp)
        }
        return true
    }

    private fun printable(cp: Int): Boolean {
        if (cp == ' '.code) return true
        return when (Character.getType(cp).toByte()) {
            Character.CONTROL, Character.FORMAT, Character.SURROGATE, Character.PRIVATE_USE, Character.UNASSIGNED,
            Character.LINE_SEPARATOR, Character.PARAGRAPH_SEPARATOR, Character.SPACE_SEPARATOR -> false
            else -> true
        }
    }

    fun x25519(privateKey: ByteArray, publicKey: ByteArray): ByteArray {
        require(publicKey.size == KEY_BYTES) { "X25519 public key must be 32 bytes" }
        val shared = X25519.sharedSecret(privateKey, publicKey)
        if (Bytes.isAllZero(shared)) throw IllegalArgumentException("all-zero X25519 shared secret")
        return shared
    }

    fun bundleMessage(room: String, kid: String, idk: String, acct: String, node: String, spk: Map<String, Any?>): ByteArray =
        Utf8.encode(
            listOf(
                BUNDLE_LABEL, room, kid, idk, acct, node, spk["id"] as String, spk["x"] as String, spk["pq"] as String,
                integer(spk["ts"]).toString(), integer(spk["exp"]).toString(),
            ).joinToString("\n"),
        )

    fun contentAad(room: String, sender: String, ctr: Long, ts: Long): ByteArray =
        Utf8.encode("$CONTENT_LABEL\n$room\n$sender\n$ctr\n$ts")

    fun wrapAad(room: String, sender: String, recipient: String, nonce: String, tag: String): ByteArray =
        Utf8.encode("$WRAP_LABEL\n$room\n$sender\n$recipient\n$nonce\n$tag")

    fun kek(
        room: String,
        sender: String,
        recipient: String,
        spkId: String,
        ephPub: ByteArray,
        ct: ByteArray,
        ss1: ByteArray,
        ss2: ByteArray,
    ): ByteArray {
        val salt = Sha256.hash(Utf8.encode(SALT_PREFIX + room))
        val info = Utf8.encode("$KEK_LABEL\n$room\n$sender\n$recipient\n$spkId\n${b64u(ephPub)}\n${b64u(Sha256.hash(ct))}")
        val ikm = Bytes.concat(ss1, ss2)
        return try {
            Hkdf.sha256(ikm, salt, info, KEY_BYTES)
        } finally {
            Bytes.wipe(ikm)
        }
    }

    fun sealedMessage(room: String, frame: Map<String, Any?>): ByteArray {
        val body = unb64(frame["body"]) ?: throw IllegalArgumentException("bad body")
        val out = StringBuilder()
        out.append(SEALED_LABEL).append('\n').append(room).append('\n')
            .append(frame["kid"] as String).append('\n').append(frame["idk"] as String).append('\n')
            .append(integer(frame["ctr"])).append('\n').append(integer(frame["ts"])).append('\n')
            .append(frame["nonce"] as String).append('\n').append(frame["tag"] as String).append('\n')
            .append(b64u(Sha256.hash(body)))
        for (entry in frame["keys"] as List<*>) {
            val key = entry as Map<*, *>
            val ct = unb64(key["ct"]) ?: throw IllegalArgumentException("bad ct")
            out.append('\n')
                .append(key["kid"] as String).append('|')
                .append(key["spk"] as String).append('|')
                .append(key["eph"] as String).append('|')
                .append(b64u(Sha256.hash(ct))).append('|')
                .append(key["nonce"] as String).append('|')
                .append(key["tag"] as String).append('|')
                .append(key["key"] as String)
        }
        return Utf8.encode(out.toString())
    }

    fun publicPrekey(spk: Any?): Map<String, Any?>? {
        if (spk !is Map<*, *>) return null
        val id = spk["id"]
        val x = spk["x"]
        val pq = spk["pq"]
        val ts = integer(spk["ts"]) ?: return null
        val exp = integer(spk["exp"]) ?: return null
        if (!isKid(id) || x !is String || pq !is String) return null
        val xBytes = unb64u(x) ?: return null
        val pqBytes = unb64u(pq) ?: return null
        if (xBytes.size != KEY_BYTES || pqBytes.size != MlKem768.PUBLIC_KEY_BYTES) return null
        if (id != id16(Bytes.concat(xBytes, pqBytes))) return null
        return linkedMapOf("id" to id, "x" to x, "pq" to pq, "ts" to ts, "exp" to exp)
    }

    fun verifyBundle(bundle: Map<String, Any?>, room: String, now: Long): Boolean {
        return try {
            if (bundle["type"] != KEY_TYPE || integer(bundle["v"]) != VERSION) return false
            val kid = bundle["kid"]
            val idk = bundle["idk"]
            val acct = bundle["acct"]
            val node = bundle["node"]
            if (kid !is String || idk !is String || acct !is String || node !is String) return false
            val idkBytes = unb64u(idk) ?: return false
            if (idkBytes.size != Ed25519.PUBLIC_KEY_BYTES || kid != id16(idkBytes) || !validAcct(acct) || !validAcct(node)) return false
            val spk = publicPrekey(bundle["spk"]) ?: return false
            val ts = spk["ts"] as Long
            val exp = spk["exp"] as Long
            if (exp - ts <= 0 || exp - ts > 9 * DAY_MS) return false
            if (ts > now + DAY_MS || exp <= now) return false
            val sig = unb64u(bundle["sig"]) ?: return false
            if (sig.size != SIGNATURE_BYTES) return false
            Ed25519.verify(idkBytes, bundleMessage(room, kid, idk, acct, node, spk), sig)
        } catch (_: RuntimeException) {
            false
        }
    }

    class SafetyNumber(val digits: String, val qr: String) {
        val groups: List<String> get() = digits.chunked(5)
    }

    fun safetyNumber(scope: String, idks: Collection<ByteArray>): SafetyNumber {
        val sorted = idks.map { it.copyOf() }.sortedWith(::compareUnsigned)
        val keys = Bytes.concat(*sorted.toTypedArray())
        var h = Sha512.hash(Utf8.encode("$SAFETY_LABEL\n$scope\n"), keys)
        repeat(SAFETY_ROUNDS) { h = Sha512.hash(h, keys) }
        val digits = StringBuilder()
        for (offset in 0 until 60 step 5) {
            var value = 0L
            for (i in 0 until 5) value = (value shl 8) or (h[offset + i].toLong() and 0xff)
            digits.append("%05d".format(value % 100000))
        }
        return SafetyNumber(digits.toString(), "forkmesh-safety:3:$scope:${b64u(h.copyOfRange(0, 32))}")
    }

    private fun compareUnsigned(a: ByteArray, b: ByteArray): Int {
        val n = minOf(a.size, b.size)
        for (i in 0 until n) {
            val x = a[i].toInt() and 0xff
            val y = b[i].toInt() and 0xff
            if (x != y) return x - y
        }
        return a.size - b.size
    }
}

object MeshLinkCrypto {
    const val HELLO_TYPE = "fm-link-hello"
    const val AUTH_TYPE = "fm-link-auth"
    const val NONCE_BYTES = 16
    private const val TRANSCRIPT_LABEL = "forkmesh-mesh-link-v3"
    private const val AUTH_LABEL = "forkmesh-mesh-link-auth-v3"
    private val AAD = Utf8.encode("forkmesh-mesh-link-v3")
    private val I2R = Utf8.encode("forkmesh-mesh-link-v3 i2r")
    private val R2I = Utf8.encode("forkmesh-mesh-link-v3 r2i")

    class Hello(val idk: String, val eph: String, val nonce: String, val idkBytes: ByteArray, val ephBytes: ByteArray) {
        val kid: String get() = ChatV3.id16(idkBytes)

        fun toJson(): Map<String, Any?> = linkedMapOf("t" to HELLO_TYPE, "v" to ChatV3.VERSION, "idk" to idk, "eph" to eph, "nonce" to nonce)
    }

    fun hello(idk: ByteArray, ephPub: ByteArray, nonce: ByteArray): Hello =
        Hello(ChatV3.b64u(idk), ChatV3.b64u(ephPub), ChatV3.b64u(nonce), idk.copyOf(), ephPub.copyOf())

    fun parseHello(frame: Map<String, Any?>): Hello? {
        if (frame["t"] != HELLO_TYPE || ChatV3.integer(frame["v"]) != ChatV3.VERSION) return null
        val idk = frame["idk"] as? String ?: return null
        val eph = frame["eph"] as? String ?: return null
        val nonce = frame["nonce"] as? String ?: return null
        val idkBytes = ChatV3.unb64u(idk) ?: return null
        val ephBytes = ChatV3.unb64u(eph) ?: return null
        val nonceBytes = ChatV3.unb64u(nonce) ?: return null
        if (idkBytes.size != Ed25519.PUBLIC_KEY_BYTES || ephBytes.size != X25519.KEY_BYTES || nonceBytes.size != NONCE_BYTES) return null
        return Hello(idk, eph, nonce, idkBytes, ephBytes)
    }

    fun transcript(initiator: Hello, responder: Hello): ByteArray = Sha256.hash(
        Utf8.encode(
            listOf(TRANSCRIPT_LABEL, initiator.idk, initiator.eph, initiator.nonce, responder.idk, responder.eph, responder.nonce)
                .joinToString("\n"),
        ),
    )

    fun authMessage(initiator: Boolean, transcript: ByteArray): ByteArray =
        Utf8.encode("$AUTH_LABEL\n${if (initiator) "i" else "r"}\n${ChatV3.b64u(transcript)}")

    fun authFrame(signature: ByteArray): Map<String, Any?> = linkedMapOf("t" to AUTH_TYPE, "sig" to ChatV3.b64u(signature))

    fun authSignature(frame: Map<String, Any?>): ByteArray? {
        if (frame["t"] != AUTH_TYPE) return null
        val sig = ChatV3.unb64u(frame["sig"]) ?: return null
        return if (sig.size == ChatV3.SIGNATURE_BYTES) sig else null
    }

    class Keys(val i2r: ByteArray, val r2i: ByteArray)

    fun keys(ownEphPrivate: ByteArray, peerEphPublic: ByteArray, transcript: ByteArray): Keys {
        val shared = ChatV3.x25519(ownEphPrivate, peerEphPublic)
        try {
            return Keys(Hkdf.sha256(shared, transcript, I2R, 32), Hkdf.sha256(shared, transcript, R2I, 32))
        } finally {
            Bytes.wipe(shared)
        }
    }

    fun nonce(counter: Long): ByteArray {
        val out = ByteArray(12)
        for (i in 0 until 8) out[4 + i] = (counter ushr (56 - 8 * i)).toByte()
        return out
    }

    fun seal(key: ByteArray, counter: Long, plain: ByteArray): ByteArray {
        val box = AesGcm.encrypt(plain, key, nonce(counter), AAD)
        return Bytes.concat(box.cipherText, box.mac)
    }

    fun open(key: ByteArray, counter: Long, data: ByteArray): ByteArray? {
        if (data.size < AesGcm.TAG_BYTES) return null
        val split = data.size - AesGcm.TAG_BYTES
        return AesGcm.decryptOrNull(data.copyOfRange(0, split), nonce(counter), data.copyOfRange(split, data.size), key, AAD)
    }
}
