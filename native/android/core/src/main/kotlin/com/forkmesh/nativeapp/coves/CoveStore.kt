package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import java.security.SecureRandom
import java.util.UUID

class CoveStoreException(message: String) : Exception(message) {
    companion object {
        val needsNameAndPassword get() = CoveStoreException("A cove needs a name and a password.")
        val needsNameAndAccount get() = CoveStoreException("A cove needs a name and a creator account.")
        val notAccountScoped get() = CoveStoreException("This cove is not account-scoped.")
        val accountScoped get() = CoveStoreException("Account-scoped coves are saved without a password.")
        val locked get() = CoveStoreException("Unlock the cove before saving it.")
        val missingParameters get() = CoveStoreException("The cove is missing its encryption parameters.")
        val grantsFailed get() = CoveStoreException("Could not seal the cove's access grants.")
    }
}

object CoveStore {
    const val COVES_DIR = ".forkmesh/coves"
    const val ENVELOPE_VERSION = 2
    const val CONTENT_KEY_BYTES = 32
    const val GRANT_SLOT_BLOCK = 4
    const val MAX_ACCESS_ENTRIES = 500

    fun normalizedAccount(account: String): String = account.trim().lowercase()

    fun normalizedAccounts(accounts: List<String>): List<String> {
        val out = ArrayList<String>()
        for (account in accounts) {
            val normalized = normalizedAccount(account)
            if (normalized.isNotEmpty() && normalized !in out) out.add(normalized)
        }
        return out.sortedBy { it.lowercase() }
    }

    fun cleanPath(path: String): String {
        val parts = ArrayList<String>()
        for (part in path.split('/').filter { it.isNotEmpty() }) {
            if (part == ".") continue
            if (part == "..") {
                if (parts.isNotEmpty() && parts.last() != "..") parts.removeAt(parts.size - 1)
                else if (!path.startsWith("/")) parts.add("..")
                continue
            }
            parts.add(part)
        }
        val joined = parts.joinToString("/")
        return if (path.startsWith("/")) "/$joined" else joined
    }

    fun relPath(slug: String): String = "$COVES_DIR/$slug.cove"

    fun slugFromPath(path: String): String {
        val file = path.split('/').lastOrNull { it.isNotEmpty() } ?: path
        return if (file.endsWith(".cove")) file.dropLast(5) else file
    }

    fun newId(): String = UUID.randomUUID().toString().lowercase()

    fun obscureSlug(): String = Bytes.hex(CoveCrypto.randomBytes(16))

    fun accountSecretV1(cove: Cove): String {
        if (!cove.accountScoped) return ""
        val invitees = normalizedAccounts(cove.invitedAccounts).toMutableList()
        val owner = normalizedAccount(cove.creatorAccount)
        if (owner.isNotEmpty() && owner !in invitees) invitees.add(owner)
        invitees.sortBy { it.lowercase() }
        val material = "forkmesh-account-cove-v1\n${cove.id}\n${cove.creator}\n$owner\n${invitees.joinToString("\n")}"
        return B64.encodeUrlNoPadding(Sha256.hash(material.toByteArray(Charsets.UTF_8)))
    }

    fun grantSecret(coveId: String, account: String): String = "forkmesh-cove-grant-v2\n$coveId\n${normalizedAccount(account)}"

    fun parseEnvelope(bytes: ByteArray): Cove? {
        val parsed = try {
            Json.parse(String(bytes, Charsets.UTF_8))
        } catch (_: Exception) {
            return null
        }
        val obj = parsed as? Map<*, *> ?: return null
        if (obj["kind"] != "cove") return null
        val id = (obj["id"] as? String).orEmpty()
        val version = if (obj.containsKey("v")) jsonLong(obj["v"], 1).toInt() else 1
        val access = obj["access"] as? Map<*, *> ?: emptyMap<String, Any?>()
        val mode = (access["mode"] as? String) ?: (if (version >= 2) "" else CoveAccessMode.PASSWORD)
        val invited = ArrayList<String>()
        for (value in access["invitedAccounts"] as? List<*> ?: emptyList<Any?>()) {
            val account = normalizedAccount(value as? String ?: "")
            if (account.isNotEmpty() && account !in invited) invited.add(account)
        }
        val kdf = obj["kdf"] as? Map<*, *> ?: emptyMap<String, Any?>()
        val salt = CoveBase64.decode((kdf["salt"] as? String).orEmpty())
        val rounds = jsonLong(kdf["rounds"]).toInt()
        if (id.isEmpty() || salt.isEmpty() || rounds <= 0) return null
        return Cove(
            id = id,
            version = version,
            name = (obj["name"] as? String).orEmpty(),
            creator = (obj["creator"] as? String).orEmpty(),
            accessMode = mode,
            creatorAccount = normalizedAccount((access["creatorAccount"] as? String).orEmpty()),
            invitedAccounts = invited,
            createdAtMs = jsonLong(obj["createdAtMs"]),
            notifyOnOpen = obj["notifyOnOpen"] == true,
            grants = (obj["grants"] as? List<*> ?: emptyList<Any?>()).mapNotNull { CoveCipher.fromJson(it as? Map<*, *>) },
            salt = salt,
            rounds = rounds,
            cipher = CoveCipher.fromJson(obj["cipher"] as? Map<*, *>),
        )
    }

    fun parseEnvelope(bytes: ByteArray, path: String): Cove? =
        parseEnvelope(bytes)?.copy(relPath = path, slug = slugFromPath(path))

    fun envelope(cove: Cove): Map<String, Any?> = linkedMapOf(
        "cipher" to (cove.cipher?.toJson() ?: emptyMap<String, Any?>()),
        "grants" to cove.grants.map { it.toJson() },
        "id" to cove.id,
        "kdf" to linkedMapOf("algo" to "pbkdf2-sha256", "rounds" to cove.rounds.toLong(), "salt" to CoveBase64.encode(cove.salt)),
        "kind" to "cove",
        "v" to ENVELOPE_VERSION.toLong(),
    )

    fun envelopeText(cove: Cove): String = CoveJson.pretty(envelope(cove)) + "\n"

    fun payload(cove: Cove): ByteArray {
        val obj = LinkedHashMap<String, Any?>()
        obj["accessLog"] = cove.accessLog.map { it.toJson() }
        obj["createdAtMs"] = cove.createdAtMs
        obj["creator"] = cove.creator
        if (cove.accountScoped) obj["creatorAccount"] = normalizedAccount(cove.creatorAccount)
        obj["documents"] = cove.documents.map { it.toJson() }
        if (cove.accountScoped) obj["invitedAccounts"] = normalizedAccounts(cove.invitedAccounts)
        obj["mode"] = cove.accessMode
        obj["name"] = cove.name
        obj["notifyOnOpen"] = cove.notifyOnOpen
        return Json.stringify(obj).toByteArray(Charsets.UTF_8)
    }

    fun applyPayload(cove: Cove, plain: ByteArray): Cove? {
        val payload = try {
            Json.parse(String(plain, Charsets.UTF_8)) as? Map<*, *>
        } catch (_: Exception) {
            null
        } ?: return null
        var name = cove.name
        (payload["name"] as? String)?.let { name = it }
        val mode = (payload["mode"] as? String) ?: cove.accessMode.ifEmpty { CoveAccessMode.PASSWORD }
        val creator = (payload["creator"] as? String) ?: cove.creator
        val creatorAccount = normalizedAccount((payload["creatorAccount"] as? String) ?: cove.creatorAccount)
        var invited = cove.invitedAccounts
        if (payload.containsKey("invitedAccounts")) {
            val accounts = ArrayList<String>()
            for (value in payload["invitedAccounts"] as? List<*> ?: emptyList<Any?>()) {
                val account = normalizedAccount(value as? String ?: "")
                if (account.isNotEmpty() && account !in accounts) accounts.add(account)
            }
            invited = accounts
        }
        val notify = if (payload.containsKey("notifyOnOpen")) payload["notifyOnOpen"] == true else cove.notifyOnOpen
        val created = if (payload.containsKey("createdAtMs")) jsonLong(payload["createdAtMs"]) else cove.createdAtMs
        return cove.copy(
            name = name,
            accessMode = mode,
            creator = creator,
            creatorAccount = creatorAccount,
            invitedAccounts = invited,
            notifyOnOpen = notify,
            createdAtMs = created,
            documents = (payload["documents"] as? List<*> ?: emptyList<Any?>()).mapNotNull { (it as? Map<*, *>)?.let(CoveDocument::fromJson) },
            accessLog = (payload["accessLog"] as? List<*> ?: emptyList<Any?>()).mapNotNull { (it as? Map<*, *>)?.let(CoveAccessEntry::fromJson) },
            unlocked = true,
        )
    }

    fun unlock(cove: Cove, password: String): Cove? {
        val cipher = cove.cipher ?: return null
        val crypto = try {
            CoveCrypto.withPassword(password, cove.salt, cove.rounds)
        } catch (_: CoveCryptoException) {
            return null
        }
        val plain = crypto.decrypt(cipher) ?: return null
        return applyPayload(cove, plain)
    }

    fun accountCanAccess(cove: Cove, account: String): Boolean {
        if (!cove.accountScoped) return false
        val me = normalizedAccount(account)
        if (me.isEmpty()) return false
        if (normalizedAccount(cove.creatorAccount) == me) return true
        return me in normalizedAccounts(cove.invitedAccounts)
    }

    fun unlockForAccount(cove: Cove, account: String): Cove? {
        val me = normalizedAccount(account)
        if (me.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0) return null
        val cipher = cove.cipher ?: return null
        if (cove.grants.isNotEmpty()) {
            val kek = try {
                CoveCrypto.withPassword(grantSecret(cove.id, me), cove.salt, cove.rounds)
            } catch (_: CoveCryptoException) {
                return null
            }
            for (grant in cove.grants) {
                val key = kek.decrypt(grant) ?: continue
                if (key.size != CONTENT_KEY_BYTES) continue
                val plain = CoveCrypto.withKey(key).decrypt(cipher) ?: return null
                return applyPayload(cove, plain)?.copy(contentKey = key)
            }
            return null
        }
        if (!accountCanAccess(cove, me)) return null
        val crypto = try {
            CoveCrypto.withPassword(accountSecretV1(cove), cove.salt, cove.rounds)
        } catch (_: CoveCryptoException) {
            return null
        }
        val plain = crypto.decrypt(cipher) ?: return null
        return applyPayload(cove, plain)
    }

    fun appendAccess(cove: Cove, entry: CoveAccessEntry): Cove {
        val log = (cove.accessLog + entry).takeLast(MAX_ACCESS_ENTRIES)
        return cove.copy(accessLog = log)
    }

    fun decoyGrant(): CoveCipher = CoveCipher(
        nonce = CoveBase64.encode(CoveCrypto.randomBytes(CoveCrypto.NONCE_BYTES)),
        tag = CoveBase64.encode(CoveCrypto.randomBytes(CoveCrypto.TAG_BYTES)),
        body = CoveBase64.encode(CoveCrypto.randomBytes(CONTENT_KEY_BYTES)),
    )

    fun buildGrants(cove: Cove, contentKey: ByteArray): List<CoveCipher> {
        val slots = ArrayList<CoveCipher>()
        if (cove.accountScoped) {
            for (account in cove.members) {
                try {
                    val kek = CoveCrypto.withPassword(grantSecret(cove.id, account), cove.salt, cove.rounds)
                    slots.add(kek.encrypt(contentKey))
                } catch (_: Exception) {
                    throw CoveStoreException.grantsFailed
                }
            }
        }
        var target = maxOf(slots.size, 1)
        target = ((target + GRANT_SLOT_BLOCK - 1) / GRANT_SLOT_BLOCK) * GRANT_SLOT_BLOCK
        while (slots.size < target) slots.add(decoyGrant())
        slots.shuffle(SecureRandom())
        return slots
    }

    fun sealPassword(cove: Cove, password: String): Cove {
        if (cove.accountScoped) throw CoveStoreException.accountScoped
        if (cove.relPath.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0) throw CoveStoreException.missingParameters
        val crypto = try {
            CoveCrypto.withPassword(password, cove.salt, cove.rounds)
        } catch (error: CoveCryptoException) {
            throw CoveStoreException(error.message ?: "")
        }
        var sealed = cove
        if (sealed.accessMode.isEmpty()) sealed = sealed.copy(accessMode = CoveAccessMode.PASSWORD)
        val cipher = try {
            crypto.encrypt(payload(sealed))
        } catch (error: CoveCryptoException) {
            throw CoveStoreException(error.message ?: "")
        }
        sealed = sealed.copy(cipher = cipher)
        return sealed.copy(grants = buildGrants(sealed, ByteArray(0)), version = ENVELOPE_VERSION)
    }

    fun sealAccount(cove: Cove): Cove {
        if (!cove.accountScoped) throw CoveStoreException.notAccountScoped
        if (!cove.unlocked) throw CoveStoreException.locked
        if (cove.relPath.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0) throw CoveStoreException.missingParameters
        var sealed = cove.copy(
            invitedAccounts = normalizedAccounts(cove.invitedAccounts),
            creatorAccount = normalizedAccount(cove.creatorAccount),
        )
        if (sealed.contentKey.size != CONTENT_KEY_BYTES) sealed = sealed.copy(contentKey = CoveCrypto.randomKey())
        val cipher = try {
            CoveCrypto.withKey(sealed.contentKey).encrypt(payload(sealed))
        } catch (error: CoveCryptoException) {
            throw CoveStoreException(error.message ?: "")
        }
        sealed = sealed.copy(cipher = cipher)
        return sealed.copy(grants = buildGrants(sealed, sealed.contentKey), version = ENVELOPE_VERSION)
    }

    fun newPasswordCove(
        name: String,
        password: String,
        creator: String,
        notifyOnOpen: Boolean,
        documents: List<CoveDocument> = emptyList(),
        nowMs: Long,
        rounds: Int = CoveCrypto.DEFAULT_ROUNDS,
    ): Cove {
        val trimmed = name.trim()
        if (trimmed.isEmpty() || password.isEmpty()) throw CoveStoreException.needsNameAndPassword
        val slug = obscureSlug()
        val cove = Cove(
            id = newId(),
            name = trimmed,
            slug = slug,
            relPath = relPath(slug),
            creator = creator,
            createdAtMs = nowMs,
            notifyOnOpen = notifyOnOpen,
            salt = CoveCrypto.randomSalt(),
            rounds = rounds,
            documents = documents,
            unlocked = true,
        )
        return sealPassword(cove, password)
    }

    fun newAccountCove(
        name: String,
        creatorAccount: String,
        invitedAccounts: List<String> = emptyList(),
        creator: String,
        notifyOnOpen: Boolean,
        documents: List<CoveDocument> = emptyList(),
        nowMs: Long,
        rounds: Int = CoveCrypto.DEFAULT_ROUNDS,
    ): Cove {
        val trimmed = name.trim()
        val owner = normalizedAccount(creatorAccount)
        if (trimmed.isEmpty() || owner.isEmpty()) throw CoveStoreException.needsNameAndAccount
        val invited = normalizedAccounts(invitedAccounts).toMutableList()
        if (owner !in invited) invited.add(owner)
        val slug = obscureSlug()
        val cove = Cove(
            id = newId(),
            name = trimmed,
            slug = slug,
            relPath = relPath(slug),
            creator = creator,
            accessMode = CoveAccessMode.ACCOUNT,
            creatorAccount = owner,
            invitedAccounts = invited.sortedBy { it.lowercase() },
            createdAtMs = nowMs,
            notifyOnOpen = notifyOnOpen,
            salt = CoveCrypto.randomSalt(),
            rounds = rounds,
            contentKey = CoveCrypto.randomKey(),
            documents = documents,
            unlocked = true,
        )
        return sealAccount(cove)
    }

    fun openCanonical(coveId: String, creatorKey: String, openerKey: String, ts: Long): ByteArray =
        "forkmesh-cove-open-v1\n$coveId\n$creatorKey\n$openerKey\n$ts".toByteArray(Charsets.UTF_8)
}
