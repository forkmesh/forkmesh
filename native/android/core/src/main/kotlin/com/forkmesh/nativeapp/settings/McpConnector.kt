package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.coves.CoveJson

data class McpTokenRow(
    val id: String,
    val provider: String,
    val label: String,
    val deviceName: String = "",
    val scopes: List<String>,
    val createdBy: String = "",
    val createdAt: Long = 0,
    val lastUsedAt: Long = 0,
    val expiresAt: Long = 0,
    val revokedAt: Long = 0,
) {
    fun isActive(nowMillis: Long): Boolean = revokedAt == 0L && (expiresAt == 0L || expiresAt > nowMillis)

    val title: String get() = label.ifEmpty { McpConnector.providerLabel(provider) }

    val tasksOnly: Boolean get() = scopes.isNotEmpty() && McpConnector.taskScopes.containsAll(scopes)

    fun status(nowMillis: Long): String {
        if (revokedAt > 0) return "Revoked"
        if (expiresAt in 1..nowMillis) return "Expired"
        if (expiresAt > 0) {
            val days = maxOf(0L, (expiresAt - nowMillis) / 86_400_000L)
            return if (days == 0L) "Expires today" else "Expires in $days day${if (days == 1L) "" else "s"}"
        }
        return "Active"
    }

    companion object {
        fun parse(map: Map<String, Any?>): McpTokenRow? {
            val id = map.text("id").lowercase()
            if (!McpConnector.isTokenId(id)) return null
            return McpTokenRow(
                id = id,
                provider = map.text("provider"),
                label = map.text("label").take(80),
                deviceName = map.text("deviceName").take(80),
                scopes = map.strings("scopes").take(32),
                createdBy = map.text("createdBy"),
                createdAt = map.long("createdAt"),
                lastUsedAt = map.long("lastUsedAt"),
                expiresAt = map.long("expiresAt"),
                revokedAt = map.long("revokedAt"),
            )
        }
    }
}

data class MintedMcpToken(val token: String, val tokenId: String, val label: String, val expiresAt: Long)

object McpConnector {
    val providers = listOf("claude-code", "codex")
    val taskScopes = listOf("organization.tasks.read", "organization.tasks.write")
    const val DEFAULT_DAYS = 90
    const val MAX_DAYS = 365

    fun providerLabel(provider: String): String = when (provider) {
        "claude-code" -> "Claude Code"
        "codex" -> "Codex"
        else -> provider.ifEmpty { "Bot" }
    }

    fun isTokenId(id: String): Boolean = id.length == 32 && id.all { it in '0'..'9' || it in 'a'..'f' }

    fun isToken(token: String): Boolean {
        val parts = token.split('_', limit = 3)
        if (parts.size != 3 || parts[0] != "fmbot" || !isTokenId(parts[1])) return false
        return parts[2].length == 43
    }

    fun path(org: String): String {
        val segment = org.lowercase().filter { it.isLetterOrDigit() || it == '-' || it == '_' || it == '.' }
        return "/api/orgs/$segment/bot-tokens"
    }

    fun serverUrl(origin: String): String {
        val trimmed = origin.removeSuffix("/")
        return if (trimmed.isEmpty()) "" else "$trimmed/mcp"
    }

    fun claudeCommand(serverUrl: String, token: String): String =
        "claude mcp add --transport http forkmesh $serverUrl --header \"Authorization: Bearer $token\""

    fun configJson(serverUrl: String, token: String): String = CoveJson.pretty(
        mapOf(
            "mcpServers" to mapOf(
                "forkmesh" to linkedMapOf(
                    "type" to "http",
                    "url" to serverUrl,
                    "headers" to mapOf("Authorization" to "Bearer $token"),
                ),
            ),
        ),
        indent = "  ",
    )

    fun mask(token: String): String = if (token.length <= 16) "•".repeat(token.length) else "${token.take(12)}…${token.takeLast(4)}"

    fun clampDays(days: Int): Int = days.coerceIn(1, MAX_DAYS)
}

class McpConnectorModel(val org: String, private val client: AccountEndpointCalling, private val clock: () -> Long = System::currentTimeMillis) {
    @Volatile var tokens: List<McpTokenRow> = emptyList()
        private set
    @Volatile var loading = false
        private set
    @Volatile var busy = false
        private set
    @Volatile var error = ""
        private set
    @Volatile var minted: MintedMcpToken? = null
        private set
    var provider = "claude-code"
    var label = ""
    var days = McpConnector.DEFAULT_DAYS

    val serverUrl: String get() = McpConnector.serverUrl(client.serverOrigin)

    val activeTokens: List<McpTokenRow> get() = tokens.filter { it.isActive(clock()) }

    fun load() {
        loading = true
        try {
            tokens = client.accountCall("GET", McpConnector.path(org), null).maps("tokens").mapNotNull(McpTokenRow::parse).take(64)
            error = ""
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load the tokens.")
        } finally {
            loading = false
        }
    }

    fun mint(): MintedMcpToken? {
        if (provider !in McpConnector.providers) return null
        busy = true
        try {
            val body = linkedMapOf<String, Any?>(
                "provider" to provider,
                "scopes" to McpConnector.taskScopes,
                "expiresDays" to McpConnector.clampDays(days).toLong(),
            )
            val trimmed = label.trim()
            if (trimmed.isNotEmpty()) body["label"] = trimmed.take(80)
            val map = client.accountCall("POST", McpConnector.path(org), body)
            val token = map.text("token")
            if (!McpConnector.isToken(token)) {
                error = "The server answered without a token."
                return null
            }
            val result = MintedMcpToken(token, map.text("tokenId"), map.text("label"), map.long("expiresAt"))
            minted = result
            label = ""
            error = ""
            load()
            return result
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not create a token.")
            return null
        } finally {
            busy = false
        }
    }

    fun revoke(id: String): Boolean {
        if (!McpConnector.isTokenId(id)) return false
        busy = true
        return try {
            client.accountCall("DELETE", McpConnector.path(org), mapOf("tokenId" to id))
            if (minted?.tokenId == id) minted = null
            error = ""
            load()
            true
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not revoke the token.")
            false
        } finally {
            busy = false
        }
    }

    fun rotate(row: McpTokenRow): MintedMcpToken? {
        if (row.provider in McpConnector.providers) provider = row.provider
        label = row.label
        val fresh = mint() ?: return null
        if (!revoke(row.id)) return fresh
        minted = fresh
        return fresh
    }

    fun dismissMinted() {
        minted = null
    }
}
