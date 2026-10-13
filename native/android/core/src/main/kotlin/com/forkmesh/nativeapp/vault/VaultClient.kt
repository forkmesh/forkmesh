package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.crypto.dartTrim
import com.forkmesh.nativeapp.json.jsonText

const val VAULT_API = "/api/vault"

class VaultException(val code: String) : Exception(code) {
    override fun toString(): String = code.replace('_', ' ')
}

typealias VaultRequest = (method: String, path: String, body: Map<String, Any?>?) -> Map<String, Any?>

data class VaultShare(val kind: String, val name: String, val permission: String)

data class VaultItem(
    val id: String,
    val owner: String,
    val owned: Boolean,
    val permission: String,
    val revision: Long,
    val updatedAt: Long,
    val updatedBy: String,
    val viaKind: String = "",
    val viaName: String = "",
    val shares: List<VaultShare> = emptyList(),
    val fields: Map<String, Any?> = emptyMap(),
    val error: String = "",
) {
    val readable: Boolean get() = error.isEmpty()
    val canEdit: Boolean get() = readable && permission != "read"
    fun field(name: String): String = fields[name] as? String ?: ""
    val title: String get() = field("title")
    val username: String get() = field("username")
    val password: String get() = field("password")
    val url: String get() = field("url")
    val notes: String get() = field("notes")
    val host: String get() = VaultCrypto.loginHost(url)
}

class VaultRecipient(val name: String, val state: String, val fingerprint: String, val bundle: VaultBundle?)

data class VaultShareOutcome(val sealedTo: List<String>, val skipped: List<String>, val withoutVault: List<String>)

object VaultFields {
    val LIMITS = listOf("title" to 200, "username" to 320, "password" to 4096, "url" to 2048, "notes" to 8000)

    fun limit(name: String): Int = LIMITS.firstOrNull { it.first == name }?.second ?: 0

    fun clean(fields: Map<String, Any?>): Map<String, Any?> {
        val out = LinkedHashMap<String, Any?>()
        for ((name, limit) in LIMITS) {
            val text = fields[name] as? String ?: ""
            out[name] = if (text.length > limit) text.substring(0, limit) else text
        }
        out["title"] = dartTrim(out["title"] as String)
        out["url"] = dartTrim(out["url"] as String)
        if ((out["title"] as String).isEmpty()) throw VaultException("title_required")
        return out
    }
}

internal fun dartString(value: Any?): String = if (value == null) "null" else jsonText(value)

@Suppress("UNCHECKED_CAST")
private fun Any?.asMap(): Map<String, Any?>? = this as? Map<String, Any?>

private fun Map<String, Any?>.maps(key: String): List<Map<String, Any?>> =
    (this[key] as? List<*>)?.mapNotNull { it.asMap() } ?: emptyList()

private fun Map<String, Any?>.text(key: String): String = this[key] as? String ?: ""

private fun Any?.asLong(): Long? = when (this) {
    is Long -> this
    is Int -> toLong()
    is Double -> if (this == Math.floor(this)) toLong() else null
    else -> null
}

class VaultClient(
    private val request: VaultRequest,
    val normalizer: VaultNormalizer? = VaultCrypto.NFKC,
) {
    var newMemoryKiB = 65536
    var newPasses = 3

    var account = ""
        private set
    var installed = false
        private set
    private var server: Map<String, Any?> = emptyMap()
    private var keyset: VaultKeyset? = null
    private var pins = LinkedHashMap<String, Any?>()
    var items: List<VaultItem> = emptyList()
        private set
    private val itemKeys = HashMap<String, ByteArray>()

    val unlocked: Boolean get() = keyset != null

    fun find(id: String): VaultItem? = items.firstOrNull { it.id == id }

    private fun keys(): VaultKeyset = keyset ?: throw VaultException("vault_locked")

    private fun call(method: String, path: String, body: Map<String, Any?>? = null): Map<String, Any?> =
        request(method, path, body)

    private inline fun <T> guarded(run: () -> T): T = try {
        run()
    } catch (error: VaultCryptoException) {
        throw VaultException(error.code)
    }

    private fun wipeItemKeys() {
        for (key in itemKeys.values) key.fill(0)
        itemKeys.clear()
    }

    @Synchronized
    fun lock() {
        wipeItemKeys()
        keyset?.wipe()
        keyset = null
        items = emptyList()
        pins = LinkedHashMap()
    }

    fun status() {
        val payload = call("GET", VAULT_API)
        account = payload.text("actor")
        installed = payload["installed"] == true
        server = if (installed) payload["account"].asMap() ?: emptyMap() else emptyMap()
    }

    private fun saveTrust() {
        val blob = VaultCrypto.sealPrivate(keys(), "trust", mapOf("pins" to LinkedHashMap(pins)))
        call("PUT", "$VAULT_API/trust", mapOf("trust" to blob))
        server = LinkedHashMap(server).apply { put("trust", blob) }
    }

    private fun adopt(opened: VaultKeyset) {
        val mine = VaultBundle.of(opened, account)
        val published = server["bundle"].asMap()
        if (published == null ||
            published["x25519"] != VaultCrypto.toBase64(mine.x25519) ||
            published["mlkem768"] != VaultCrypto.toBase64(mine.mlkem768) ||
            published["ed25519"] != VaultCrypto.toBase64(mine.ed25519)
        ) {
            opened.wipe()
            throw VaultException("keyset_mismatch")
        }
        keyset = opened
        pins = LinkedHashMap()
        val trust = server.text("trust")
        if (trust.isNotEmpty()) {
            val stored = VaultCrypto.openPrivate(opened, "trust", trust)
            stored["pins"].asMap()?.let { saved ->
                for ((name, fingerprint) in saved) pins[name] = dartString(fingerprint)
            }
        }
    }

    private fun newKdf(): VaultKdf = VaultKdf.fresh(newMemoryKiB, newPasses)

    fun setUp(masterPassword: String): String = guarded {
        status()
        if (installed) throw VaultException("vault_exists")
        val kdf = newKdf()
        val masterKey = VaultCrypto.stretch(masterPassword, kdf, normalizer)
        val fresh = VaultKeyset.generate()
        val recoveryCode = VaultCrypto.newRecoveryCode()
        val recoveryKey = VaultCrypto.recoverySecret(recoveryCode)
        try {
            val payload = call(
                "POST",
                "$VAULT_API/install",
                linkedMapOf(
                    "kdf" to kdf.toJson(),
                    "wrappedKeyset" to VaultCrypto.wrapKeyset(masterKey, "master", account, fresh),
                    "wrappedRecovery" to VaultCrypto.wrapKeyset(recoveryKey, "recovery", account, fresh),
                    "bundle" to VaultBundle.of(fresh, account).toJson(),
                ),
            )
            installed = true
            server = payload["account"].asMap() ?: emptyMap()
            keyset?.wipe()
            keyset = fresh
            pins = LinkedHashMap()
            items = emptyList()
            recoveryCode
        } finally {
            masterKey.fill(0)
            recoveryKey.fill(0)
        }
    }

    fun unlock(masterPassword: String) = guarded {
        status()
        if (!installed) throw VaultException("vault_not_set_up")
        val kdf = VaultKdf.fromJson(server["kdf"])
        val masterKey = VaultCrypto.stretch(masterPassword, kdf, normalizer)
        try {
            adopt(VaultCrypto.unwrapKeyset(masterKey, "master", account, server["wrappedKeyset"]))
        } finally {
            masterKey.fill(0)
        }
    }

    fun resume(savedKeyset: Map<String, Any?>) = guarded {
        status()
        if (!installed) throw VaultException("vault_not_set_up")
        adopt(VaultKeyset.fromJson(savedKeyset))
    }

    fun exportKeyset(): Map<String, Any?> = keys().toJson()

    private fun rewrap(masterPassword: String?, newRecovery: Boolean): String = guarded {
        val keyset = keys()
        val body = linkedMapOf<String, Any?>("revision" to (server["revision"] ?: 1L))
        var recoveryCode = ""
        if (masterPassword != null) {
            val kdf = newKdf()
            val masterKey = VaultCrypto.stretch(masterPassword, kdf, normalizer)
            try {
                body["kdf"] = kdf.toJson()
                body["wrappedKeyset"] = VaultCrypto.wrapKeyset(masterKey, "master", account, keyset)
            } finally {
                masterKey.fill(0)
            }
        }
        if (newRecovery) {
            recoveryCode = VaultCrypto.newRecoveryCode()
            val recoveryKey = VaultCrypto.recoverySecret(recoveryCode)
            try {
                body["wrappedRecovery"] = VaultCrypto.wrapKeyset(recoveryKey, "recovery", account, keyset)
            } finally {
                recoveryKey.fill(0)
            }
        }
        val payload = call("PUT", "$VAULT_API/keys", body)
        server = payload["account"].asMap() ?: emptyMap()
        recoveryCode
    }

    fun changePassword(masterPassword: String) {
        rewrap(masterPassword, newRecovery = false)
    }

    fun renewRecoveryCode(): String = rewrap(null, newRecovery = true)

    fun recover(recoveryCode: String, newMasterPassword: String): String = guarded {
        status()
        if (!installed) throw VaultException("vault_not_set_up")
        val recoveryKey = VaultCrypto.recoverySecret(recoveryCode)
        try {
            val blob = call("GET", "$VAULT_API/recovery")["wrappedRecovery"]
            try {
                adopt(VaultCrypto.unwrapKeyset(recoveryKey, "recovery", account, blob))
            } catch (error: VaultCryptoException) {
                throw VaultException(if (error.code == "wrong_password") "wrong_recovery_code" else error.code)
            }
        } finally {
            recoveryKey.fill(0)
        }
        rewrap(newMasterPassword, newRecovery = true)
    }

    internal class Listing(
        val items: MutableList<VaultItem>,
        val itemKeys: HashMap<String, ByteArray>,
        val pins: LinkedHashMap<String, Any?>,
        var pinsChanged: Boolean,
    )

    fun load(): List<VaultItem> = guarded {
        val keyset = keys()
        val payload = call("GET", "$VAULT_API/items")
        if (this.keyset !== keyset) throw VaultException("vault_locked")
        val listing = openListing(payload, keyset, account, pins)
        synchronized(this) {
            if (this.keyset !== keyset) {
                for (key in listing.itemKeys.values) key.fill(0)
                throw VaultException("vault_locked")
            }
            wipeItemKeys()
            itemKeys.putAll(listing.itemKeys)
            pins = listing.pins
            items = listing.items
        }
        if (listing.pinsChanged) saveTrust()
        items
    }

    fun matching(host: String): List<VaultItem> = items.filter { it.readable && VaultCrypto.matchesHost(it.url, host) }

    fun save(fields: Map<String, Any?>, id: String = ""): String = guarded {
        val keyset = keys()
        val clean = VaultFields.clean(fields)
        if (id.isEmpty()) {
            val itemId = VaultCrypto.newItemId()
            val itemKey = VaultCrypto.randomBytes(32)
            call(
                "POST",
                "$VAULT_API/items",
                linkedMapOf(
                    "id" to itemId,
                    "body" to VaultCrypto.sealItem(itemKey, itemId, clean),
                    "wrappedKey" to VaultCrypto.wrapItemKey(keyset, itemId, itemKey),
                ),
            )
            if (this.keyset === keyset) itemKeys[itemId] = itemKey else itemKey.fill(0)
            return@guarded itemId
        }
        val current = find(id)
        val itemKey = itemKeys[id]
        if (current == null || itemKey == null) throw VaultException("item_not_found")
        if (current.permission == "read") throw VaultException("read_only")
        call(
            "PUT",
            "$VAULT_API/items/$id",
            linkedMapOf("revision" to current.revision, "body" to VaultCrypto.sealItem(itemKey, id, clean)),
        )
        id
    }

    fun remove(id: String) {
        keys()
        call("DELETE", "$VAULT_API/items/$id")
        itemKeys.remove(id)?.fill(0)
    }

    private fun recipients(members: List<Map<String, Any?>>): List<VaultRecipient> {
        val out = ArrayList<VaultRecipient>()
        for (member in members) {
            val name = dartString(member["name"])
            if (member["bundle"].asMap() == null) {
                out.add(VaultRecipient(name, "no_vault", "", null))
                continue
            }
            try {
                val bundle = VaultBundle.verify(member["bundle"], name)
                val fingerprint = bundle.fingerprint()
                out.add(VaultRecipient(name, trustState(pins, name, fingerprint), fingerprint, bundle))
            } catch (_: Exception) {
                out.add(VaultRecipient(name, "invalid_keys", "", null))
            }
        }
        return out
    }

    fun previewShare(kind: String, name: String): List<VaultRecipient> {
        keys()
        val clean = dartTrim(name).lowercase()
        if (kind == "user") {
            val payload = call("GET", "$VAULT_API/users/$clean")
            return recipients(listOf(mapOf("name" to payload["name"], "bundle" to payload["bundle"])))
        }
        return recipients(call("GET", "$VAULT_API/organizations/$clean/members").maps("members"))
    }

    private fun grantsFor(
        id: String,
        itemKey: ByteArray,
        kind: String,
        name: String,
        permission: String,
        members: List<VaultRecipient>,
        accepted: List<String> = emptyList(),
    ): Pair<List<Map<String, Any?>>, List<String>> {
        val grants = ArrayList<Map<String, Any?>>()
        val skipped = ArrayList<String>()
        for (member in members) {
            if (member.name == account || member.state == "no_vault") continue
            val bundle = member.bundle
            if (bundle == null || (member.state == "changed" && member.name !in accepted)) {
                skipped.add(member.name)
                continue
            }
            grants.add(
                linkedMapOf(
                    "recipient" to member.name,
                    "via" to linkedMapOf("kind" to kind, "name" to name),
                    "grant" to VaultCrypto.sealGrant(keys(), account, bundle, id, itemKey, permission),
                ),
            )
            pins[member.name] = member.fingerprint
        }
        return grants to skipped
    }

    fun share(id: String, kind: String, name: String, permission: String = "read", accepted: List<String> = emptyList()): VaultShareOutcome = guarded {
        keys()
        val current = find(id)
        val itemKey = itemKeys[id]
        if (current == null || !current.owned || itemKey == null) throw VaultException("owner_required")
        val members = previewShare(kind, name)
        var target = dartTrim(name).lowercase()
        if (kind == "user") {
            val only = members.firstOrNull()
            if (only == null || only.state == "no_vault") throw VaultException("recipient_has_no_vault")
            if (only.state == "invalid_keys") throw VaultException("invalid_public_keys")
            if (only.state == "changed" && only.name !in accepted) throw VaultException("recipient_keys_changed")
            target = only.name
        }
        val (grants, skipped) = grantsFor(id, itemKey, kind, target, permission, members, accepted)
        call(
            "POST",
            "$VAULT_API/items/$id/shares",
            linkedMapOf(
                "target" to linkedMapOf("kind" to kind, "name" to target),
                "permission" to permission,
                "grants" to grants,
            ),
        )
        saveTrust()
        VaultShareOutcome(
            grants.map { dartString(it["recipient"]) },
            skipped,
            members.filter { it.state == "no_vault" }.map { it.name },
        )
    }

    private fun rotate(id: String) {
        val current = find(id)
        if (current == null || !current.owned || !current.readable) throw VaultException("owner_required")
        val listing = call("GET", "$VAULT_API/items")
        var fresh: Map<String, Any?>? = null
        for (row in listing.maps("items")) if (row["id"] == id) fresh = row
        val groups = call("GET", "$VAULT_API/items/$id/recipients").maps("recipients")
        val oldKey = itemKeys[id]
        val fields = if (fresh != null && oldKey != null) VaultCrypto.openItem(oldKey, id, fresh["body"]) else current.fields
        val itemKey = VaultCrypto.randomBytes(32)
        val grants = ArrayList<Map<String, Any?>>()
        for (group in groups) {
            grants += grantsFor(
                id,
                itemKey,
                dartString(group["kind"]),
                dartString(group["name"]),
                dartString(group["permission"]),
                recipients(group.maps("members")),
            ).first
        }
        call(
            "PUT",
            "$VAULT_API/items/$id",
            linkedMapOf(
                "revision" to (fresh?.get("revision").asLong() ?: current.revision),
                "body" to VaultCrypto.sealItem(itemKey, id, fields),
                "wrappedKey" to VaultCrypto.wrapItemKey(keys(), id, itemKey),
                "grants" to grants,
            ),
        )
        itemKeys[id]?.fill(0)
        itemKeys[id] = itemKey
    }

    fun unshare(id: String, kind: String, name: String) = guarded {
        keys()
        call("POST", "$VAULT_API/items/$id/unshare", linkedMapOf("kind" to kind, "name" to name))
        rotate(id)
    }

    fun syncPending(): Int = guarded {
        keys()
        val pending = call("GET", "$VAULT_API/pending").maps("pending")
        var sealedCount = 0
        for (entry in pending) {
            val id = dartString(entry["itemId"])
            val itemKey = itemKeys[id] ?: continue
            val (grants, _) = grantsFor(
                id,
                itemKey,
                "organization",
                dartString(entry["organization"]),
                dartString(entry["permission"]),
                recipients(entry.maps("members")),
            )
            if (grants.isEmpty()) continue
            call("POST", "$VAULT_API/items/$id/grants", mapOf("grants" to grants))
            sealedCount += grants.size
        }
        if (sealedCount > 0) saveTrust()
        sealedCount
    }

    fun organizations(): List<String> = call("GET", "$VAULT_API/organizations").maps("organizations").map { dartString(it["name"]) }

    fun myFingerprint(): String = guarded { VaultBundle.of(keys(), account).fingerprint() }

    fun exportData(): Map<String, Any?> = call("GET", "$VAULT_API/export")

    fun destroy(): Map<String, Any?> {
        val payload = call("POST", "$VAULT_API/uninstall", emptyMap())
        lock()
        installed = false
        server = emptyMap()
        return payload
    }

    companion object {
        internal fun trustState(pins: Map<String, Any?>, name: String, fingerprint: String): String {
            val pinned = pins[name] ?: return "new"
            return if (pinned == fingerprint) "pinned" else "changed"
        }

        internal fun item(row: Map<String, Any?>, owned: Boolean, fields: Map<String, Any?> = emptyMap(), error: String = ""): VaultItem {
            val via = row["via"].asMap()
            return VaultItem(
                id = dartString(row["id"]),
                owner = row.text("owner"),
                owned = owned,
                permission = row.text("permission"),
                revision = row["revision"].asLong() ?: 1L,
                updatedAt = row["updatedAt"].asLong() ?: 0L,
                updatedBy = row.text("updatedBy"),
                viaKind = via?.let { dartString(it["kind"]) } ?: "",
                viaName = via?.let { dartString(it["name"]) } ?: "",
                shares = row.maps("shares").map {
                    VaultShare(dartString(it["kind"]), dartString(it["name"]), dartString(it["permission"]))
                },
                fields = fields,
                error = error,
            )
        }

        internal fun openListing(payload: Map<String, Any?>, keyset: VaultKeyset, account: String, start: Map<String, Any?>): Listing {
            val listing = Listing(ArrayList(), HashMap(), LinkedHashMap(start), false)
            for (row in payload.maps("items")) {
                val id = dartString(row["id"])
                try {
                    val itemKey = VaultCrypto.unwrapItemKey(keyset, id, row["wrappedKey"])
                    val fields = VaultCrypto.openItem(itemKey, id, row["body"])
                    listing.itemKeys[id] = itemKey
                    listing.items.add(item(row, owned = true, fields = fields))
                } catch (error: Exception) {
                    listing.items.add(item(row, owned = true, error = (error as? VaultCryptoException)?.code ?: "invalid_item"))
                }
            }
            val bundles = payload["bundles"].asMap() ?: emptyMap()
            for (row in payload.maps("shared")) {
                val id = dartString(row["id"])
                val owner = dartString(row["owner"])
                try {
                    if (!bundles.containsKey(owner)) throw VaultCryptoException("sender_keys_missing")
                    val sender = VaultBundle.verify(bundles[owner], owner)
                    val fingerprint = sender.fingerprint()
                    val state = trustState(listing.pins, owner, fingerprint)
                    if (state == "changed") throw VaultCryptoException("sender_keys_changed")
                    val itemKey = VaultCrypto.openGrant(keyset, account, sender, id, row["grant"], dartString(row["permission"]))
                    val fields = VaultCrypto.openItem(itemKey, id, row["body"])
                    if (state == "new") {
                        listing.pins[owner] = fingerprint
                        listing.pinsChanged = true
                    }
                    listing.itemKeys[id] = itemKey
                    listing.items.add(item(row, owned = false, fields = fields))
                } catch (error: Exception) {
                    listing.items.add(item(row, owned = false, error = (error as? VaultCryptoException)?.code ?: "invalid_item"))
                }
            }
            listing.items.sortWith { a, b ->
                if (a.readable != b.readable) {
                    if (a.readable) -1 else 1
                } else {
                    a.title.lowercase().compareTo(b.title.lowercase())
                }
            }
            return listing
        }
    }
}
