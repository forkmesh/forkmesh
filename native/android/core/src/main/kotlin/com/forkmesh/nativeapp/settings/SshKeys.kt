package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.crypto.B64

data class SshKeyRow(
    val id: String,
    val label: String,
    val keyType: String,
    val fingerprint: String,
    val createdAt: Long,
    val lastUsedAt: Long,
) {
    val title: String get() = label.ifEmpty { SshKeys.typeLabel(keyType) }

    companion object {
        fun parse(map: Map<String, Any?>): SshKeyRow? {
            val id = map.text("id")
            if (!SshKeys.isKeyId(id)) return null
            return SshKeyRow(id, map.text("label").take(SshKeys.MAX_LABEL), map.text("keyType"), map.text("fingerprint"), map.long("createdAt"), map.long("lastUsedAt"))
        }
    }
}

data class SshKeysPage(
    val keys: List<SshKeyRow>,
    val maximumKeys: Int,
    val gatewayConfigured: Boolean,
    val gatewayHost: String,
    val gatewayPort: Int,
    val notice: String,
) {
    val atLimit: Boolean get() = maximumKeys > 0 && keys.size >= maximumKeys

    val gatewayLine: String
        get() {
            if (!gatewayConfigured || gatewayHost.isEmpty()) return "This server has no SSH gateway yet. Keys are kept for when it does."
            return if (gatewayPort == 22) "git@$gatewayHost" else "ssh://git@$gatewayHost:$gatewayPort"
        }

    companion object {
        fun parse(map: Map<String, Any?>): SshKeysPage = SshKeysPage(
            keys = map.maps("keys").mapNotNull(SshKeyRow::parse).take(SshKeys.MAX_ROWS),
            maximumKeys = maxOf(0, map.int("maximumKeys", SshKeys.DEFAULT_MAXIMUM)),
            gatewayConfigured = map.flag("gatewayConfigured"),
            gatewayHost = map.text("gatewayHost"),
            gatewayPort = map.int("gatewayPort", 22),
            notice = map.text("notice"),
        )
    }
}

sealed class SshKeyCheck {
    data class Ok(val keyType: String, val line: String, val comment: String) : SshKeyCheck()
    data object Empty : SshKeyCheck()
    data object PrivateKey : SshKeyCheck()
    data object MultipleKeys : SshKeyCheck()
    data class UnsupportedType(val type: String) : SshKeyCheck()
    data object Malformed : SshKeyCheck()
}

object SshKeys {
    const val PATH = "/api/accounts/ssh-keys"
    const val MAX_LABEL = 80
    const val MAX_ROWS = 64
    const val DEFAULT_MAXIMUM = 20
    const val MAX_LINE_BYTES = 16 * 1024
    val supportedTypes = listOf(
        "ssh-ed25519",
        "sk-ssh-ed25519@openssh.com",
        "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521",
        "sk-ecdsa-sha2-nistp256@openssh.com",
        "ssh-rsa",
    )

    fun isKeyId(id: String): Boolean {
        if (!id.startsWith("sk_")) return false
        val rest = id.substring(3)
        if (rest.length !in 16..80) return false
        return rest.all { (it.isLetterOrDigit() && it.code < 128) || it == '_' || it == '-' }
    }

    fun typeLabel(keyType: String): String = when (keyType) {
        "ssh-ed25519" -> "Ed25519 key"
        "sk-ssh-ed25519@openssh.com" -> "Ed25519 security key"
        "sk-ecdsa-sha2-nistp256@openssh.com" -> "ECDSA security key"
        "ssh-rsa" -> "RSA key"
        else -> if (keyType.startsWith("ecdsa-")) "ECDSA key" else keyType.ifEmpty { "SSH key" }
    }

    fun check(pasted: String): SshKeyCheck {
        val raw = pasted.trim()
        if (raw.isEmpty()) return SshKeyCheck.Empty
        if (raw.contains("PRIVATE KEY") || raw.startsWith("-----BEGIN") || raw.startsWith("PuTTY-User-Key-File")) return SshKeyCheck.PrivateKey
        if (raw.contains('\n') || raw.contains('\r')) return SshKeyCheck.MultipleKeys
        if (raw.toByteArray().size > MAX_LINE_BYTES) return SshKeyCheck.Malformed
        val parts = raw.split(' ').filter { it.isNotEmpty() }.let { tokens ->
            if (tokens.size <= 2) tokens else listOf(tokens[0], tokens[1], raw.substringAfter(tokens[1]).trim())
        }
        if (parts.size < 2) return SshKeyCheck.Malformed
        if (parts[0] !in supportedTypes) return SshKeyCheck.UnsupportedType(parts[0])
        val blob = strictBase64(parts[1]) ?: return SshKeyCheck.Malformed
        if (blob.isEmpty()) return SshKeyCheck.Malformed
        if (embeddedType(blob) != parts[0]) return SshKeyCheck.Malformed
        val comment = if (parts.size == 3) parts[2].trim() else ""
        return SshKeyCheck.Ok(parts[0], "${parts[0]} ${parts[1]}", comment)
    }

    private fun strictBase64(text: String): ByteArray? {
        if (text.isEmpty() || text.length % 4 != 0) return null
        if (!text.all { it.isLetterOrDigit() && it.code < 128 || it == '+' || it == '/' || it == '=' }) return null
        return try {
            java.util.Base64.getDecoder().decode(text)
        } catch (_: IllegalArgumentException) {
            null
        }
    }

    internal fun embeddedType(blob: ByteArray): String? {
        if (blob.size < 4) return null
        val length = ((blob[0].toInt() and 0xff) shl 24) or ((blob[1].toInt() and 0xff) shl 16) or
            ((blob[2].toInt() and 0xff) shl 8) or (blob[3].toInt() and 0xff)
        if (length <= 0 || length > 64 || blob.size < 4 + length) return null
        return String(blob, 4, length, Charsets.UTF_8)
    }

    fun message(check: SshKeyCheck): String = when (check) {
        is SshKeyCheck.Ok -> ""
        SshKeyCheck.Empty -> "Paste the contents of your .pub file."
        SshKeyCheck.PrivateKey -> "That is a private key. Never paste a private key anywhere; paste the .pub file instead."
        SshKeyCheck.MultipleKeys -> "Paste one key at a time."
        is SshKeyCheck.UnsupportedType -> "${check.type} keys are not supported."
        SshKeyCheck.Malformed -> "That is not an OpenSSH public key line."
    }

    fun label(typed: String, comment: String): String {
        val chosen = typed.trim().ifEmpty { comment }
        return chosen.filter { it != '\n' && it != '\r' }.take(MAX_LABEL)
    }
}

class SshKeysModel(private val client: AccountEndpointCalling) {
    @Volatile var page: SshKeysPage? = null
        private set
    @Volatile var loading = false
        private set
    @Volatile var busy = false
        private set
    @Volatile var error = ""
        private set
    var draftKey = ""
    var draftLabel = ""

    val draftCheck: SshKeyCheck get() = SshKeys.check(draftKey)

    val canAdd: Boolean get() = !busy && page?.atLimit != true && draftCheck is SshKeyCheck.Ok

    fun load() {
        loading = true
        try {
            page = SshKeysPage.parse(client.accountCall("GET", SshKeys.PATH, null))
            error = ""
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load your SSH keys.")
        } finally {
            loading = false
        }
    }

    fun add(): Boolean {
        val check = draftCheck
        if (check !is SshKeyCheck.Ok) {
            error = SshKeys.message(check)
            return false
        }
        busy = true
        return try {
            client.accountCall("POST", SshKeys.PATH, linkedMapOf("publicKey" to check.line, "label" to SshKeys.label(draftLabel, check.comment)))
            draftKey = ""
            draftLabel = ""
            error = ""
            load()
            true
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not add the key.")
            false
        } finally {
            busy = false
        }
    }

    fun remove(id: String): Boolean {
        if (!SshKeys.isKeyId(id)) return false
        busy = true
        return try {
            client.accountCall("DELETE", "${SshKeys.PATH}/$id", null)
            page = page?.let { it.copy(keys = it.keys.filter { key -> key.id != id }) }
            error = ""
            true
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not remove the key.")
            false
        } finally {
            busy = false
        }
    }
}
