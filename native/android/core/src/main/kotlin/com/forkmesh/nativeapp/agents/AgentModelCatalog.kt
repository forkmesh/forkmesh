package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.PrefStore

data class AgentModelChoice(
    val provider: String,
    val alias: String,
    val id: String,
    val label: String,
    val note: String = "",
)

object AgentModelCatalog {
    val DEFAULT_NODE_MODEL = AgentModelChoice("claude-code", "opus", "claude-opus-5-5", "Opus 5.5", "Default · most capable Opus")

    val NODE_MODELS: List<AgentModelChoice> = listOf(
        DEFAULT_NODE_MODEL,
        AgentModelChoice("claude-code", "opus-5", "claude-opus-5", "Opus 5", "Previous Opus"),
        AgentModelChoice("claude-code", "sonnet", "claude-sonnet-5", "Sonnet 5", "Balanced"),
        AgentModelChoice("claude-code", "haiku", "claude-haiku-4-5", "Haiku 4.5", "Fastest"),
        AgentModelChoice("claude-code", "fable", "claude-fable-5", "Fable 5", "Long, hard tasks"),
        AgentModelChoice("codex", "terra", "gpt-5.6-terra", "GPT-5.6 Terra"),
        AgentModelChoice("codex", "sol", "gpt-5.6-sol", "GPT-5.6 Sol"),
        AgentModelChoice("codex", "luna", "gpt-5.6-luna", "GPT-5.6 Luna"),
        AgentModelChoice("codex", "spark", "gpt-5.3-codex-spark", "Codex Spark"),
    )

    private val SHORT_LABELS = mapOf(
        "claude-opus-5-5" to "Opus 5.5",
        "claude-opus-5" to "Opus 5",
        "claude-sonnet-5" to "Sonnet 5",
        "claude-haiku-4-5" to "Haiku 4.5",
        "claude-fable-5" to "Fable 5",
        "claude-fable-5-1" to "Fable 5.1",
        "claude-opus-4-8" to "Opus 4.8",
        "claude-sonnet-4-6" to "Sonnet 4.6",
        "gpt-5.3-codex" to "GPT-5.3 Codex",
        "gpt-5.3" to "GPT-5.3",
    )

    fun shortLabel(idOrAlias: String): String {
        val key = idOrAlias.trim()
        if (key.isEmpty()) return ""
        SHORT_LABELS[key.lowercase()]?.let { return it }
        val lower = key.lowercase()
        return NODE_MODELS.firstOrNull { it.alias == lower || it.id == lower }?.label ?: key
    }

    fun nodeChoices(offered: Map<String, List<String>>): List<AgentModelChoice> {
        if (offered.isEmpty()) return NODE_MODELS
        val choices = NODE_MODELS.filter { offered[it.provider]?.contains(it.alias) == true }
        return choices.ifEmpty { NODE_MODELS }
    }
}

class NodeModelPreference(private val prefs: PrefStore) {
    fun read(): AgentModelChoice {
        val alias = prefs.getString(KEY) ?: ""
        return AgentModelCatalog.NODE_MODELS.firstOrNull { it.alias == alias } ?: AgentModelCatalog.DEFAULT_NODE_MODEL
    }

    fun write(choice: AgentModelChoice) = prefs.putString(KEY, choice.alias)

    companion object {
        const val KEY = "agent/node-model-alias"
    }
}
