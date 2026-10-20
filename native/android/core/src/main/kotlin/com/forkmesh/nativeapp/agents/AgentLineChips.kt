package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.model.AgentSession

enum class AgentListView(val key: String, val caption: String) {
    Compact("compact", "Compact"),
    Zen("zen", "Zen"),
    Full("full", "Full"),
    Eagle("eagle", "Eagle"),
    ;

    val next: AgentListView
        get() = when (this) {
            Compact -> Zen
            Zen -> Full
            Full -> Eagle
            Eagle -> Compact
        }

    companion object {
        const val PREF_KEY = "agents/listView"
        const val EAGLE_DEFAULT_KEY = "agents/listViewEagleDefault"

        fun fromKey(key: String?): AgentListView = entries.firstOrNull { it.key == key } ?: Eagle

        fun stored(key: String?, eagleDefaulted: Boolean): AgentListView = if (eagleDefaulted) fromKey(key) else Eagle
    }
}

enum class AgentLineStatus(val word: String) {
    Running("Running"),
    Queued("Queued"),
    Waiting("Waiting"),
    Success("Success"),
    Failed("Failed"),
    Stopped("Stopped"),
    Merged("Merged"),
    Other("Idle"),
}

enum class AgentChipSlot(val widestWords: List<String>) {
    Status(AgentLineStatus.entries.map { it.word }),
    Provider(listOf("Claude", "Codex", "Llama")),
    Host(listOf("Desktop", "Cloud", "Runner", "Outside")),
    Age(listOf("now", "59m", "23h", "12mo")),
    Sync(listOf("Conflicts", "Behind")),
    Files(listOf("999+")),
    Churn(listOf("+9.9k−9.9k")),
    Branch(listOf("Worktree", "Branch")),
}

data class AgentLineChip(val slot: AgentChipSlot, val word: String)

data class AgentChipFacts(
    val status: AgentLineStatus,
    val provider: String = "",
    val cloud: Boolean = false,
    val outside: Boolean = false,
    val runner: Boolean = false,
    val runnerName: String = "",
    val updatedAtMs: Long = 0,
    val conflicted: Boolean = false,
    val behind: Int = -1,
    val filesChanged: Int = -1,
    val linesAdded: Int = -1,
    val linesRemoved: Int = -1,
    val branch: String = "",
    val hasWorktree: Boolean = false,
)

object AgentLineChips {
    fun status(raw: String, merged: Boolean, active: Boolean): AgentLineStatus {
        if (merged) return AgentLineStatus.Merged
        val value = raw.lowercase()
        fun has(vararg words: String) = words.any { value.contains(it) }
        return when {
            has("fail", "error", "reject") -> AgentLineStatus.Failed
            has("stop", "cancel", "interrupt") -> AgentLineStatus.Stopped
            has("wait", "input", "approval") -> AgentLineStatus.Waiting
            has("queue", "pending", "claim") -> AgentLineStatus.Queued
            has("run", "start", "progress") -> AgentLineStatus.Running
            has("done", "complete", "success", "succeed", "finish") -> AgentLineStatus.Success
            active -> AgentLineStatus.Running
            else -> AgentLineStatus.Other
        }
    }

    fun providerWord(provider: String): String {
        val value = provider.lowercase()
        return when {
            value.contains("claude") || value.contains("anthropic") -> "Claude"
            value.contains("codex") || value.contains("openai") || value.contains("gpt") -> "Codex"
            value.contains("llama") -> "Llama"
            else -> ""
        }
    }

    fun shortAge(ms: Long, nowMs: Long): String {
        if (ms <= 0) return ""
        val secs = (nowMs - ms) / 1000
        if (secs < 0) return "now"
        if (secs < 60) return "${secs}s"
        val mins = secs / 60
        if (mins < 60) return "${mins}m"
        val hours = mins / 60
        if (hours < 24) return "${hours}h"
        val days = hours / 24
        if (days < 7) return "${days}d"
        if (days < 30) return "${days / 7}w"
        val months = days / 30
        if (months < 12) return "${months}mo"
        return "${days / 365}y"
    }

    fun facts(session: AgentSession): AgentChipFacts = AgentChipFacts(
        status = status(session.status, session.merged, session.isActive),
        provider = session.provider,
        updatedAtMs = maxOf(maxOf(session.createdAtMs, session.startedAtMs), maxOf(session.finishedAtMs, session.mergedAtMs)),
        conflicted = session.conflicted,
        behind = session.behind,
        filesChanged = session.filesChanged,
        branch = session.branchName,
    )

    fun facts(session: AgentLaunchSession, cloud: Boolean): AgentChipFacts {
        val stats = session.diffStats
        return AgentChipFacts(
            status = status(session.status, session.isMergedBranch, session.isActive),
            provider = session.provider,
            cloud = cloud,
            runner = session.isOnRunner,
            runnerName = session.runner,
            updatedAtMs = maxOf(maxOf(session.updatedAtMs, session.completedAtMs), session.createdAtMs),
            conflicted = stats.conflicted || session.merge.isConflict,
            behind = if (stats.known) stats.behind else -1,
            filesChanged = if (stats.known) stats.files else -1,
            linesAdded = if (stats.known) stats.added else -1,
            linesRemoved = if (stats.known) stats.removed else -1,
            branch = session.branchName,
        )
    }

    fun facts(row: UnifiedAgentRow): AgentChipFacts = AgentChipFacts(
        status = status(row.status, merged = row.merged, active = row.isActive),
        provider = row.provider,
        cloud = !row.isDesktop,
        outside = row.outside,
        runner = row.isRunner,
        runnerName = row.runner,
        updatedAtMs = row.updatedAtMs,
        conflicted = row.conflicted,
        behind = row.behind,
        filesChanged = row.filesChanged,
        linesAdded = row.linesAdded,
        linesRemoved = row.linesRemoved,
        branch = row.branchName,
    )

    fun runnerWord(name: String): String {
        val clean = name.trim()
        return when {
            clean.isEmpty() -> "Runner"
            clean.length > RUNNER_WORD_MAX -> clean.take(RUNNER_WORD_MAX - 1) + "…"
            else -> clean
        }
    }

    private const val RUNNER_WORD_MAX = 10

    fun count(value: Int): String = if (value > 999) "999+" else "$value"

    fun compactCount(value: Int): String = when {
        value >= 10_000 -> "${value / 1000}k"
        value >= 1000 -> "${value / 1000}.${value % 1000 / 100}k"
        else -> "${maxOf(value, 0)}"
    }

    fun chips(facts: AgentChipFacts, nowMs: Long): List<AgentLineChip> {
        val chips = mutableListOf(AgentLineChip(AgentChipSlot.Status, facts.status.word))
        providerWord(facts.provider).takeIf { it.isNotEmpty() }?.let { chips += AgentLineChip(AgentChipSlot.Provider, it) }
        chips += AgentLineChip(
            AgentChipSlot.Host,
            when {
                facts.runner -> runnerWord(facts.runnerName)
                facts.cloud -> "Cloud"
                facts.outside -> "Outside"
                else -> "Desktop"
            },
        )
        shortAge(facts.updatedAtMs, nowMs).takeIf { it.isNotEmpty() }?.let { chips += AgentLineChip(AgentChipSlot.Age, it) }
        if (facts.conflicted) {
            chips += AgentLineChip(AgentChipSlot.Sync, "Conflicts")
        } else if (facts.behind > 0) {
            chips += AgentLineChip(AgentChipSlot.Sync, "Behind")
        }
        if (facts.filesChanged >= 0) chips += AgentLineChip(AgentChipSlot.Files, count(facts.filesChanged))
        if (facts.linesAdded > 0 || facts.linesRemoved > 0) {
            chips += AgentLineChip(
                AgentChipSlot.Churn,
                "+${compactCount(facts.linesAdded)}−${compactCount(facts.linesRemoved)}",
            )
        }
        if (facts.branch.isNotEmpty()) {
            chips += AgentLineChip(AgentChipSlot.Branch, if (facts.hasWorktree) "Worktree" else "Branch")
        }
        return chips
    }
}
