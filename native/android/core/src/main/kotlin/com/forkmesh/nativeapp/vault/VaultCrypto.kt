package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.Hkdf
import com.forkmesh.nativeapp.crypto.Keccak
import com.forkmesh.nativeapp.crypto.MlKem768
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.crypto.dartTrim
import com.forkmesh.nativeapp.json.Json
import java.net.URI
import java.security.SecureRandom
import java.text.Normalizer

class VaultCryptoException(val code: String) : Exception(code) {
    override fun toString(): String = code.replace('_', ' ')
}

typealias VaultNormalizer = (String) -> String

object VaultCrypto {
    const val SUITE = "fmv1"
    val PERMISSIONS = listOf("read", "write")

    internal const val CONTEXT = "forkmesh-vault/fmv1/"
    internal const val MLKEM_PUBLIC_BYTES = 1184
    internal const val MLKEM_CIPHERTEXT_BYTES = 1088
    private val XWING_LABEL = byteArrayOf(0x5c, 0x2e, 0x2f, 0x2f, 0x5e, 0x5c)
    private val random = SecureRandom()

    val NFKC: VaultNormalizer = { Normalizer.normalize(it, Normalizer.Form.NFKC) }

    fun randomBytes(count: Int): ByteArray = Bytes.random(count)

    fun toBase64(bytes: ByteArray): String = B64.encodeUrlNoPadding(bytes)

    fun fromBase64(value: Any?): ByteArray {
        val text = value as? String ?: ""
        val bad = VaultCryptoException("invalid_encoding")
        for (c in text) if (!isBase64Url(c)) throw bad
        if (text.length % 4 == 1) throw bad
        val padded = text + "=".repeat((4 - text.length % 4) % 4)
        return B64.decodeOrNull(padded.replace('-', '+').replace('_', '/')) ?: throw bad
    }

    fun toHex(bytes: ByteArray): String = Bytes.hex(bytes)

    fun fromHex(hex: String): ByteArray? {
        if (hex.length % 2 != 0 || hex.any { Character.digit(it, 16) < 0 }) return null
        return Bytes.fromHex(hex)
    }

    fun wipe(bytes: ByteArray?) {
        bytes?.fill(0)
    }

    fun sameBytes(a: ByteArray, b: ByteArray): Boolean = Bytes.constantTimeEquals(a, b)

    private fun isBase64Url(c: Char): Boolean =
        c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '-' || c == '_'

    private fun utf8(text: String): ByteArray = text.toByteArray(Charsets.UTF_8)

    internal fun account(value: String): String {
        val name = dartTrim(value).lowercase()
        val ok = name.length in 1..63 && alnum(name[0]) && name.drop(1).all { alnum(it) || it == '-' }
        if (!ok) throw VaultCryptoException("invalid_account")
        return name
    }

    private fun alnum(c: Char): Boolean = c in 'a'..'z' || c in '0'..'9'

    internal fun itemIdChecked(value: String): String {
        if (value.length != 32 || !value.all { it in '0'..'9' || it in 'a'..'f' }) {
            throw VaultCryptoException("invalid_item_id")
        }
        return value
    }

    internal fun hkdf(secret: ByteArray, info: String): ByteArray =
        Hkdf.sha256(secret, ByteArray(32), utf8(CONTEXT + info), 32)

    internal fun seal(key: ByteArray, label: String, plaintext: ByteArray): String {
        if (key.size != 32) throw VaultCryptoException("invalid_key")
        val box = AesGcm.encrypt(plaintext, key, aad = utf8(CONTEXT + label))
        return toBase64(Bytes.concat(box.nonce, box.cipherText, box.mac))
    }

    internal fun open(key: ByteArray, label: String, blob: Any?): ByteArray {
        val bytes = fromBase64(blob)
        if (key.size != 32) throw VaultCryptoException("invalid_key")
        if (bytes.size < 12 + 16) throw VaultCryptoException("invalid_ciphertext")
        return try {
            AesGcm.decrypt(
                bytes.copyOfRange(12, bytes.size - 16),
                bytes.copyOfRange(0, 12),
                bytes.copyOfRange(bytes.size - 16, bytes.size),
                key,
                utf8(CONTEXT + label),
            )
        } catch (_: AesGcm.AuthenticationException) {
            throw VaultCryptoException("decryption_failed")
        }
    }

    internal fun normalized(password: String, normalizer: VaultNormalizer?): String {
        if (password.all { it.code < 0x80 }) return password
        normalizer ?: throw VaultCryptoException("normalization_unavailable")
        return normalizer(password)
    }

    fun stretch(password: String, kdf: VaultKdf, normalizer: VaultNormalizer? = NFKC): ByteArray {
        VaultKdf.fromJson(kdf.toJson())
        val secret = utf8(normalized(password, normalizer))
        try {
            return Argon2id.deriveKey(
                password = secret,
                salt = kdf.salt,
                parallelism = kdf.parallelism,
                memoryKiB = kdf.memoryKiB,
                iterations = kdf.passes,
                hashLength = 32,
            )
        } catch (_: IllegalArgumentException) {
            throw VaultCryptoException("unsafe_kdf_parameters")
        } finally {
            wipe(secret)
        }
    }

    fun wrapKeyset(secret: ByteArray, purpose: String, account: String, keyset: VaultKeyset): String {
        val name = account(account)
        val kek = hkdf(secret, "keyset-kek|$purpose")
        try {
            return seal(kek, "keyset|$purpose|$name", utf8(Json.stringify(keyset.toJson())))
        } finally {
            wipe(kek)
        }
    }

    fun unwrapKeyset(secret: ByteArray, purpose: String, account: String, blob: Any?): VaultKeyset {
        val name = account(account)
        val kek = hkdf(secret, "keyset-kek|$purpose")
        try {
            val plain = open(kek, "keyset|$purpose|$name", blob)
            try {
                return VaultKeyset.fromJson(Json.parse(String(plain, Charsets.UTF_8)))
            } finally {
                wipe(plain)
            }
        } catch (error: VaultCryptoException) {
            val passthrough = setOf("unsupported_keyset", "invalid_keyset")
            throw VaultCryptoException(if (error.code in passthrough) error.code else "wrong_password")
        } catch (_: Exception) {
            throw VaultCryptoException("wrong_password")
        } finally {
            wipe(kek)
        }
    }

    private const val BASE32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

    fun newRecoveryCode(): String {
        val bytes = randomBytes(32)
        try {
            return recoveryCode(bytes)
        } finally {
            wipe(bytes)
        }
    }

    internal fun recoveryCode(bytes: ByteArray): String {
        val bits = IntArray(260)
        for (i in 0 until 256) bits[i] = (bytes[i / 8].toInt() ushr (7 - i % 8)) and 1
        val out = StringBuilder()
        for (i in 51 downTo 0) {
            val start = (51 - i) * 5
            var value = 0
            for (b in start until start + 5) value = (value shl 1) or bits[b]
            out.append(BASE32[value])
            if (i % 4 == 0 && i != 0) out.append('-')
        }
        return out.toString()
    }

    fun recoverySecret(code: String): ByteArray {
        val clean = StringBuilder()
        for (char in code.uppercase()) {
            when {
                char.isWhitespace() || char == '-' -> {}
                char == 'O' -> clean.append('0')
                char == 'I' || char == 'L' -> clean.append('1')
                else -> clean.append(char)
            }
        }
        val values = clean.map { BASE32.indexOf(it) }
        if (values.size != 52 || values.any { it < 0 }) throw VaultCryptoException("invalid_recovery_code")
        val out = ByteArray(32)
        var bit = 0
        for (value in values) {
            for (shift in 4 downTo 0) {
                if (bit < 256 && (value ushr shift) and 1 == 1) {
                    out[bit / 8] = (out[bit / 8].toInt() or (1 shl (7 - bit % 8))).toByte()
                }
                bit++
            }
        }
        return out
    }

    fun newItemId(): String = toHex(randomBytes(16))

    fun sealItem(itemKey: ByteArray, itemId: String, fields: Map<String, Any?>): String {
        val body = utf8(Json.stringify(fields))
        val size = (body.size + 1 + 255) / 256 * 256
        val padded = ByteArray(size) { if (it < body.size) body[it] else 0x20 }
        try {
            return seal(itemKey, "item|${itemIdChecked(itemId)}", padded)
        } finally {
            wipe(padded)
            wipe(body)
        }
    }

    fun openItem(itemKey: ByteArray, itemId: String, blob: Any?): Map<String, Any?> {
        val plain = open(itemKey, "item|${itemIdChecked(itemId)}", blob)
        try {
            val value = try {
                Json.parse(String(plain, Charsets.UTF_8))
            } catch (_: Exception) {
                throw VaultCryptoException("invalid_item")
            }
            @Suppress("UNCHECKED_CAST")
            return value as? Map<String, Any?> ?: throw VaultCryptoException("invalid_item")
        } finally {
            wipe(plain)
        }
    }

    fun wrapItemKey(keyset: VaultKeyset, itemId: String, itemKey: ByteArray): String =
        seal(keyset.vaultKey, "itemkey|${itemIdChecked(itemId)}", itemKey)

    fun unwrapItemKey(keyset: VaultKeyset, itemId: String, blob: Any?): ByteArray =
        open(keyset.vaultKey, "itemkey|${itemIdChecked(itemId)}", blob)

    fun sealPrivate(keyset: VaultKeyset, label: String, value: Map<String, Any?>): String =
        seal(keyset.vaultKey, "private|$label", utf8(Json.stringify(value)))

    fun openPrivate(keyset: VaultKeyset, label: String, blob: Any?): Map<String, Any?> {
        val plain = open(keyset.vaultKey, "private|$label", blob)
        @Suppress("UNCHECKED_CAST")
        return Json.parse(String(plain, Charsets.UTF_8)) as? Map<String, Any?> ?: emptyMap()
    }

    private fun grantContext(itemId: String, sender: String, recipient: String, permission: String): String =
        listOf("grant", itemIdChecked(itemId), account(sender), account(recipient), permission).joinToString("|")

    private fun grantMessage(label: String, recipientEd: ByteArray, ctX: ByteArray, ctM: ByteArray, wrapped: ByteArray): ByteArray {
        val digest = Sha256.hash(ctX, ctM, wrapped)
        return utf8(listOf(CONTEXT + label, toBase64(recipientEd), toBase64(digest)).joinToString("\n"))
    }

    private fun dh(privateKey: ByteArray, theirs: ByteArray, onZero: String): ByteArray {
        val shared = try {
            X25519.sharedSecret(privateKey, theirs)
        } catch (_: Exception) {
            throw VaultCryptoException(onZero)
        }
        if (Bytes.isAllZero(shared)) throw VaultCryptoException(onZero)
        return shared
    }

    fun sealGrant(
        keyset: VaultKeyset,
        sender: String,
        recipient: VaultBundle,
        itemId: String,
        itemKey: ByteArray,
        permission: String,
    ): Map<String, Any?> {
        if (permission !in PERMISSIONS) throw VaultCryptoException("invalid_permission")
        if (itemKey.size != 32) throw VaultCryptoException("invalid_share")
        val label = grantContext(itemId, sender, recipient.account, permission)
        val ephemeral = X25519.generatePrivateKey()
        val ctX = X25519.publicKey(ephemeral)
        val ssX = dh(ephemeral, recipient.x25519, "invalid_public_keys")
        val encapsulation = try {
            MlKem768.encapsulate(recipient.mlkem768)
        } catch (_: Exception) {
            throw VaultCryptoException("invalid_public_keys")
        }
        val ctM = encapsulation.ciphertext
        val ssM = encapsulation.sharedSecret
        val shared = Keccak.sha3_256(ssM, ssX, ctX, recipient.x25519, XWING_LABEL)
        val wrapKey = hkdf(shared, label)
        try {
            val wrapped = seal(wrapKey, label, itemKey)
            val signature = Ed25519.sign(
                keyset.ed25519,
                grantMessage(label, recipient.ed25519, ctX, ctM, fromBase64(wrapped)),
            )
            return linkedMapOf(
                "v" to 1L,
                "suite" to SUITE,
                "x25519" to toBase64(ctX),
                "mlkem768" to toBase64(ctM),
                "wrapped" to wrapped,
                "signature" to toBase64(signature),
            )
        } finally {
            wipe(ephemeral)
            wipe(ssX)
            wipe(ssM)
            wipe(shared)
            wipe(wrapKey)
        }
    }

    fun openGrant(
        keyset: VaultKeyset,
        recipient: String,
        sender: VaultBundle,
        itemId: String,
        grant: Any?,
        permission: String,
    ): ByteArray {
        val map = grant as? Map<*, *> ?: emptyMap<String, Any?>()
        val bad = VaultCryptoException("invalid_share")
        if (!isOne(map["v"]) || map["suite"] != SUITE || permission !in PERMISSIONS) throw bad
        val ctX: ByteArray
        val ctM: ByteArray
        val wrapped: ByteArray
        val signature: ByteArray
        try {
            ctX = fromBase64(map["x25519"])
            ctM = fromBase64(map["mlkem768"])
            wrapped = fromBase64(map["wrapped"])
            signature = fromBase64(map["signature"])
        } catch (_: VaultCryptoException) {
            throw bad
        }
        if (ctX.size != 32 || ctM.size != MLKEM_CIPHERTEXT_BYTES || signature.size != 64) throw bad
        val label = grantContext(itemId, sender.account, recipient, permission)
        val myEd = Ed25519.publicKey(keyset.ed25519)
        val signed = Ed25519.verify(sender.ed25519, grantMessage(label, myEd, ctX, ctM, wrapped), signature)
        if (!signed) throw VaultCryptoException("invalid_share_signature")
        val myX = X25519.publicKey(keyset.x25519)
        val ssX = dh(keyset.x25519, ctX, "invalid_share")
        val ssM = try {
            MlKem768.decapsulate(keyset.mlkem.secretKey, ctM)
        } catch (_: Exception) {
            throw bad
        }
        val shared = Keccak.sha3_256(ssM, ssX, ctX, myX, XWING_LABEL)
        val wrapKey = hkdf(shared, label)
        try {
            val itemKey = open(wrapKey, label, map["wrapped"])
            if (itemKey.size != 32) throw bad
            return itemKey
        } finally {
            wipe(ssX)
            wipe(ssM)
            wipe(shared)
            wipe(wrapKey)
        }
    }

    internal fun isOne(value: Any?): Boolean = numEquals(value, 1.0)

    internal fun numEquals(value: Any?, expected: Double): Boolean = when (value) {
        is Long -> value.toDouble() == expected
        is Int -> value.toDouble() == expected
        is Double -> value == expected
        else -> false
    }

    private val CLASSES = listOf(
        "abcdefghijkmnopqrstuvwxyz",
        "ABCDEFGHJKLMNPQRSTUVWXYZ",
        "23456789",
        "!@#$%^&*-_=+?",
    )

    fun generatePassword(
        length: Int = 24,
        lower: Boolean = true,
        upper: Boolean = true,
        digits: Boolean = true,
        symbols: Boolean = true,
    ): String {
        val wanted = listOf(lower, upper, digits, symbols)
        val chosen = CLASSES.filterIndexed { i, _ -> wanted[i] }
        if (chosen.isEmpty()) throw VaultCryptoException("no_character_classes")
        val alphabet = chosen.joinToString("")
        val size = length.coerceIn(8, 128)
        while (true) {
            val out = String(CharArray(size) { alphabet[random.nextInt(alphabet.length)] })
            if (chosen.all { set -> out.any { it in set } }) return out
        }
    }

    private val SCHEME = Regex("^[a-z][a-z0-9+.-]*://", RegexOption.IGNORE_CASE)

    private fun loginUri(url: String): URI? {
        val text = dartTrim(url)
        if (text.isEmpty()) return null
        val parsed = try {
            URI(if (SCHEME.containsMatchIn(text)) text else "https://$text")
        } catch (_: Exception) {
            return null
        }
        var host = parsed.host?.lowercase() ?: return null
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length - 1)
        if (host.isEmpty()) return null
        val scheme = parsed.scheme?.lowercase() ?: return null
        val local = host in setOf("localhost", "127.0.0.1", "::1")
        if (scheme != "https" && !(scheme == "http" && local)) return null
        return try {
            URI(scheme, parsed.rawUserInfo, parsed.host.lowercase(), parsed.port, null, null, null).let { base ->
                URI(
                    base.toString() + (parsed.rawPath ?: "") +
                        (parsed.rawQuery?.let { "?$it" } ?: "") +
                        (parsed.rawFragment?.let { "#$it" } ?: ""),
                )
            }
        } catch (_: Exception) {
            null
        }
    }

    private fun hostOf(uri: URI): String {
        var host = uri.host?.lowercase() ?: ""
        if (host.startsWith("[") && host.endsWith("]")) host = host.substring(1, host.length - 1)
        return host
    }

    fun loginHost(url: String): String {
        val host = loginUri(url)?.let(::hostOf) ?: ""
        return if (host.startsWith("www.")) host.substring(4) else host
    }

    fun loginHref(url: String): String = loginUri(url)?.toString() ?: ""

    fun matchesHost(savedUrl: String, pageHost: String): Boolean {
        val saved = loginHost(savedUrl)
        var page = dartTrim(pageHost).lowercase()
        if (page.startsWith("www.")) page = page.substring(4)
        return saved.isNotEmpty() && saved == page
    }
}

class VaultKdf(
    val salt: ByteArray,
    val memoryKiB: Int = 65536,
    val passes: Int = 3,
    val parallelism: Int = 1,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "algorithm" to "argon2id",
        "version" to 19L,
        "memoryKiB" to memoryKiB.toLong(),
        "passes" to passes.toLong(),
        "parallelism" to parallelism.toLong(),
        "salt" to VaultCrypto.toBase64(salt),
    )

    companion object {
        const val FLOOR_MEMORY_KIB = 19456
        const val FLOOR_PASSES = 2

        fun fresh(memoryKiB: Int = 65536, passes: Int = 3): VaultKdf =
            VaultKdf(VaultCrypto.randomBytes(16), memoryKiB, passes)

        fun fromJson(json: Any?): VaultKdf {
            val map = json as? Map<*, *> ?: emptyMap<String, Any?>()
            val unsafe = VaultCryptoException("unsafe_kdf_parameters")
            val salt = try {
                VaultCrypto.fromBase64(map["salt"])
            } catch (_: VaultCryptoException) {
                throw unsafe
            }
            fun integer(key: String): Int? = when (val v = map[key]) {
                is Long -> if (v in Int.MIN_VALUE..Int.MAX_VALUE) v.toInt() else null
                is Int -> v
                else -> null
            }
            val memory = integer("memoryKiB") ?: throw unsafe
            val passes = integer("passes") ?: throw unsafe
            val lanes = integer("parallelism") ?: throw unsafe
            if (map["algorithm"] != "argon2id" || !VaultCrypto.numEquals(map["version"], 19.0) ||
                salt.size < 16 || salt.size > 64 ||
                memory < FLOOR_MEMORY_KIB || memory > 1_048_576 ||
                passes < FLOOR_PASSES || passes > 16 ||
                lanes < 1 || lanes > 16
            ) {
                throw unsafe
            }
            return VaultKdf(salt, memory, passes, lanes)
        }
    }
}

class VaultKeyset(
    vaultKey: ByteArray,
    x25519: ByteArray,
    mlkemSeed: ByteArray,
    ed25519: ByteArray,
) {
    val vaultKey: ByteArray = vaultKey
    val x25519: ByteArray = x25519
    val mlkemSeed: ByteArray = mlkemSeed
    val ed25519: ByteArray = ed25519
    private var expanded: MlKem768.KeyPair? = null

    init {
        if (vaultKey.size != 32 || x25519.size != 32 || mlkemSeed.size != 64 || ed25519.size != 32) {
            throw VaultCryptoException("invalid_keyset")
        }
    }

    val mlkem: MlKem768.KeyPair
        @Synchronized get() = expanded ?: MlKem768.generateKeyPair(mlkemSeed).also { expanded = it }

    @Synchronized
    fun copy(): VaultKeyset =
        VaultKeyset(vaultKey.copyOf(), x25519.copyOf(), mlkemSeed.copyOf(), ed25519.copyOf()).also { it.expanded = expanded }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "v" to 1L,
        "vaultKey" to VaultCrypto.toBase64(vaultKey),
        "x25519" to VaultCrypto.toBase64(x25519),
        "mlkemSeed" to VaultCrypto.toBase64(mlkemSeed),
        "ed25519" to VaultCrypto.toBase64(ed25519),
    )

    @Synchronized
    fun wipe() {
        vaultKey.fill(0)
        x25519.fill(0)
        mlkemSeed.fill(0)
        ed25519.fill(0)
        expanded = null
    }

    companion object {
        fun generate(): VaultKeyset = VaultKeyset(
            VaultCrypto.randomBytes(32),
            VaultCrypto.randomBytes(32),
            VaultCrypto.randomBytes(64),
            VaultCrypto.randomBytes(32),
        )

        fun fromJson(json: Any?): VaultKeyset {
            val map = json as? Map<*, *> ?: emptyMap<String, Any?>()
            if (!VaultCrypto.isOne(map["v"])) throw VaultCryptoException("unsupported_keyset")
            return try {
                VaultKeyset(
                    VaultCrypto.fromBase64(map["vaultKey"]),
                    VaultCrypto.fromBase64(map["x25519"]),
                    VaultCrypto.fromBase64(map["mlkemSeed"]),
                    VaultCrypto.fromBase64(map["ed25519"]),
                )
            } catch (_: VaultCryptoException) {
                throw VaultCryptoException("invalid_keyset")
            }
        }
    }
}

class VaultBundle(
    val account: String,
    val x25519: ByteArray,
    val mlkem768: ByteArray,
    val ed25519: ByteArray,
    val signature: ByteArray,
) {
    fun toJson(): Map<String, Any?> = linkedMapOf(
        "v" to 1L,
        "suite" to VaultCrypto.SUITE,
        "account" to account,
        "x25519" to VaultCrypto.toBase64(x25519),
        "mlkem768" to VaultCrypto.toBase64(mlkem768),
        "ed25519" to VaultCrypto.toBase64(ed25519),
        "signature" to VaultCrypto.toBase64(signature),
    )

    fun fingerprint(): String {
        val digest = Sha256.hash(
            "${VaultCrypto.CONTEXT}fingerprint\n$account\n".toByteArray(Charsets.UTF_8),
            x25519,
            mlkem768,
            ed25519,
        )
        return VaultCrypto.toHex(digest).chunked(4).joinToString(" ")
    }

    override fun equals(other: Any?): Boolean =
        other is VaultBundle && other.account == account && other.x25519.contentEquals(x25519) &&
            other.mlkem768.contentEquals(mlkem768) && other.ed25519.contentEquals(ed25519) &&
            other.signature.contentEquals(signature)

    override fun hashCode(): Int = account.hashCode() * 31 + ed25519.contentHashCode()

    companion object {
        private fun message(account: String, x: ByteArray, m: ByteArray, ed: ByteArray): ByteArray =
            listOf(
                VaultCrypto.CONTEXT + "identity",
                account,
                VaultCrypto.toBase64(x),
                VaultCrypto.toBase64(m),
                VaultCrypto.toBase64(ed),
            ).joinToString("\n").toByteArray(Charsets.UTF_8)

        fun of(keyset: VaultKeyset, account: String): VaultBundle {
            val name = VaultCrypto.account(account)
            val x = X25519.publicKey(keyset.x25519)
            val ed = Ed25519.publicKey(keyset.ed25519)
            val m = keyset.mlkem.publicKey
            val signature = Ed25519.sign(keyset.ed25519, message(name, x, m, ed), ed)
            return VaultBundle(name, x, m, ed, signature)
        }

        fun verify(json: Any?, account: String): VaultBundle {
            val map = json as? Map<*, *> ?: emptyMap<String, Any?>()
            val bad = VaultCryptoException("invalid_public_keys")
            val name: String
            val x: ByteArray
            val m: ByteArray
            val ed: ByteArray
            val signature: ByteArray
            try {
                name = VaultCrypto.account(account)
                x = VaultCrypto.fromBase64(map["x25519"])
                m = VaultCrypto.fromBase64(map["mlkem768"])
                ed = VaultCrypto.fromBase64(map["ed25519"])
                signature = VaultCrypto.fromBase64(map["signature"])
            } catch (_: VaultCryptoException) {
                throw bad
            }
            if (!VaultCrypto.isOne(map["v"]) || map["suite"] != VaultCrypto.SUITE || map["account"] != name ||
                x.size != 32 || ed.size != 32 || signature.size != 64 ||
                m.size != VaultCrypto.MLKEM_PUBLIC_BYTES || !mlkemKeyReduced(m)
            ) {
                throw bad
            }
            if (!Ed25519.verify(ed, message(name, x, m, ed), signature)) throw bad
            return VaultBundle(name, x, m, ed, signature)
        }

        internal fun mlkemKeyReduced(key: ByteArray): Boolean {
            var i = 0
            while (i + 2 < 1152) {
                val b0 = key[i].toInt() and 0xff
                val b1 = key[i + 1].toInt() and 0xff
                val b2 = key[i + 2].toInt() and 0xff
                val a = b0 or ((b1 and 0x0f) shl 8)
                val b = (b1 ushr 4) or (b2 shl 4)
                if (a >= 3329 || b >= 3329) return false
                i += 3
            }
            return true
        }
    }
}
