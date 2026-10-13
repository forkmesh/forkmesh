package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.SecretStore

enum class AgentProviderKind(
    val storedName: String,
    val id: String,
    val label: String,
    val consoleUrl: String,
) {
    Anthropic("anthropic", "claude-api", "Claude", "https://console.anthropic.com/settings/keys"),
    OpenAi("openai", "openai", "OpenAI", "https://platform.openai.com/api-keys"),
    ;

    fun looksLikeKey(value: String): Boolean {
        val trimmed = value.trim()
        return when (this) {
            Anthropic -> trimmed.startsWith("sk-ant-") && trimmed.length > 20
            OpenAi -> trimmed.startsWith("sk-") && trimmed.length > 20
        }
    }

    val models: List<AgentModelOption>
        get() = when (this) {
            Anthropic -> listOf(
                AgentModelOption("claude-opus-5-5", "Claude Opus 5.5", "Most capable"),
                AgentModelOption("claude-opus-5", "Claude Opus 5", "Previous Opus"),
                AgentModelOption("claude-sonnet-5", "Claude Sonnet 5", "Balanced"),
                AgentModelOption("claude-haiku-4-5", "Claude Haiku 4.5", "Fastest and cheapest"),
            )
            OpenAi -> listOf(
                AgentModelOption("gpt-5.3-codex", "GPT-5.3 Codex", "Coding"),
                AgentModelOption("gpt-5.3", "GPT-5.3", "General"),
            )
        }

    companion object {
        fun fromStored(name: Any?): AgentProviderKind = entries.firstOrNull { it.storedName == name } ?: Anthropic
    }
}

data class AgentModelOption(val id: String, val label: String, val note: String)

class AgentCredentials(val kind: AgentProviderKind, val apiKey: String, val model: String) {
    val isUsable: Boolean get() = apiKey.trim().isNotEmpty() && model.trim().isNotEmpty()

    val maskedKey: String
        get() {
            val trimmed = apiKey.trim()
            if (trimmed.length <= 10) return "••••"
            return trimmed.substring(0, 6) + "…" + trimmed.substring(trimmed.length - 4)
        }

    fun build(): AgentProvider = when (kind) {
        AgentProviderKind.Anthropic -> AnthropicProvider(apiKey, model)
        AgentProviderKind.OpenAi -> OpenAiProvider(apiKey, model)
    }

    override fun toString(): String = "AgentCredentials(${kind.storedName}, $maskedKey, $model)"
}

class AgentCredentialStore(private val store: SecretStore) {
    companion object {
        const val STORAGE_KEY = "agent/credentials-v1"
    }

    fun read(): AgentCredentials? {
        val raw = store.read(STORAGE_KEY)
        if (raw.isNullOrEmpty()) return null
        return try {
            val data = Json.parse(raw) as? Map<*, *> ?: return null
            val kind = AgentProviderKind.fromStored(data["kind"])
            val model = jsonText(data["model"])
            val credentials = AgentCredentials(kind, jsonText(data["apiKey"]), model.ifEmpty { kind.models.first().id })
            if (credentials.isUsable) credentials else null
        } catch (_: Exception) {
            null
        }
    }

    fun write(credentials: AgentCredentials) = store.write(
        STORAGE_KEY,
        Json.stringify(
            linkedMapOf("kind" to credentials.kind.storedName, "apiKey" to credentials.apiKey.trim(), "model" to credentials.model),
        ),
    )

    fun clear() = store.delete(STORAGE_KEY)

    fun verify(credentials: AgentCredentials, transport: HttpTransport = HttpTransport.DEFAULT): String? = try {
        credentials.build().send(
            systemPrompt = "Reply with the single word: ok",
            history = listOf(linkedMapOf("role" to "user", "content" to "ok")),
            tools = emptyList(),
            transport = transport,
        )
        null
    } catch (error: AgentProviderException) {
        when (error.statusCode) {
            401, 403 -> "${credentials.kind.label} rejected this key. Check that you copied all of it and that it is still active."
            429 -> "The key is valid but the account is rate limited right now."
            else -> error.message
        }
    } catch (error: Exception) {
        "Could not reach ${credentials.kind.label}: ${error.message ?: error}"
    }
}
