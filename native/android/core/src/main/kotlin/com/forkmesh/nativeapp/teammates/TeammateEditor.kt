package com.forkmesh.nativeapp.teammates

data class TeammateDraft(
    val name: String = "",
    val avatar: String = "sol",
    val specialty: String = "",
    val focus: String = "",
    val description: String = "",
    val instructions: String = "",
    val provider: String = "codex",
    val model: String = "",
    val mode: String = "",
    val skills: List<String> = emptyList(),
    val backupModels: List<AgentChoiceRow> = emptyList(),
    val taskAgents: List<AgentChoiceRow> = emptyList(),
    val schedule: TeammateSchedule = TeammateSchedule(),
    val relayChannel: String = "",
    val enabled: Boolean = true,
    val selfImproving: Boolean = true,
) {
    companion object {
        fun from(detail: TeammateDetail): TeammateDraft = TeammateDraft(
            name = detail.row.name,
            avatar = detail.row.avatar.ifEmpty { "sol" },
            specialty = detail.row.specialty,
            focus = detail.row.focus,
            description = detail.description,
            instructions = detail.instructions,
            provider = detail.row.provider,
            model = detail.row.model,
            mode = detail.mode,
            skills = detail.row.skills,
            backupModels = detail.backupModels,
            taskAgents = detail.taskAgents,
            schedule = detail.row.schedule,
            relayChannel = detail.relayChannel,
            enabled = detail.row.enabled,
            selfImproving = detail.selfImproving,
        )
    }
}

sealed class TeammateDraftIssue(val message: String) {
    data object Name : TeammateDraftIssue("Give the special agent a name.")
    data object Avatar : TeammateDraftIssue("The avatar is a short lowercase word.")
    data object Provider : TeammateDraftIssue("Pick a provider.")
    data object Model : TeammateDraftIssue("Enter a model id.")
    data object Mode : TeammateDraftIssue("Pick Auto, Plan, Ask or the default.")
    data object Schedule : TeammateDraftIssue("Check the schedule's time and days.")
    data object RelayChannel : TeammateDraftIssue("A relay room starts with #, like #team.")
    data object Skills : TeammateDraftIssue("Skills are short names, up to 40.")
    data object Choices : TeammateDraftIssue("Every model choice needs a provider and a model.")
    data class TooLong(val field: String) : TeammateDraftIssue("$field is too long.")
}

object TeammateEditorLogic {
    private val avatarPattern = Regex("^[a-z0-9-]{1,32}$")
    private val providerPattern = Regex("^[a-z0-9][a-z0-9-]{0,31}$")
    private val modelPattern = Regex("^[A-Za-z0-9][A-Za-z0-9._:/@-]{0,99}$")
    private val timePattern = Regex("^([01][0-9]|2[0-3]):[0-5][0-9]$")
    private val relayPattern = Regex("^#[a-z0-9_-]{1,64}$")

    fun validate(draft: TeammateDraft): List<TeammateDraftIssue> {
        val issues = ArrayList<TeammateDraftIssue>()
        val name = draft.name.trim()
        if (name.isEmpty() || name.length > 80) issues += TeammateDraftIssue.Name
        if (!avatarPattern.matches(draft.avatar)) issues += TeammateDraftIssue.Avatar
        if (!providerPattern.matches(draft.provider)) issues += TeammateDraftIssue.Provider
        if (!modelPattern.matches(draft.model.trim())) issues += TeammateDraftIssue.Model
        if (draft.mode !in listOf("", "Auto", "Plan", "Ask")) issues += TeammateDraftIssue.Mode
        if (!timePattern.matches(draft.schedule.time) || draft.schedule.days.any { it !in 1..7 }) issues += TeammateDraftIssue.Schedule
        val relay = draft.relayChannel.trim()
        if (relay.isNotEmpty() && !relayPattern.matches(relay)) issues += TeammateDraftIssue.RelayChannel
        if (draft.skills.size > 40 || draft.skills.any { it.trim().isEmpty() || it.length > 100 }) issues += TeammateDraftIssue.Skills
        val choices = draft.backupModels + draft.taskAgents
        if (draft.backupModels.size > 10 || draft.taskAgents.size > 10 ||
            choices.any { it.provider.isEmpty() || it.model.isEmpty() } ||
            draft.taskAgents.any { it.taskType.isEmpty() }
        ) {
            issues += TeammateDraftIssue.Choices
        }
        if (draft.specialty.length > 200) issues += TeammateDraftIssue.TooLong("Specialty")
        if (draft.focus.length > 200) issues += TeammateDraftIssue.TooLong("Focus")
        if (draft.description.length > 2000) issues += TeammateDraftIssue.TooLong("Description")
        if (draft.instructions.length > 20000) issues += TeammateDraftIssue.TooLong("Instructions")
        return issues
    }

    private fun choiceArray(rows: List<AgentChoiceRow>, withTask: Boolean): List<Map<String, Any?>> = rows.map { row ->
        val item = linkedMapOf<String, Any?>("provider" to row.provider.trim(), "model" to row.model.trim())
        if (withTask) item["taskType"] = row.taskType
        item
    }

    fun changes(original: TeammateDraft?, draft: TeammateDraft): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>()
        val base = original
        fun text(key: String, value: String, old: String?) {
            val trimmed = value.trim()
            val changed = if (base == null) trimmed.isNotEmpty() else trimmed != old?.trim()
            if (changed) out[key] = trimmed
        }
        text("name", draft.name, base?.name)
        if (base == null || draft.avatar != base.avatar) out["avatar"] = draft.avatar
        text("specialty", draft.specialty, base?.specialty)
        text("focus", draft.focus, base?.focus)
        text("description", draft.description, base?.description)
        text("instructions", draft.instructions, base?.instructions)
        if (base == null || draft.provider != base.provider) out["provider"] = draft.provider
        text("model", draft.model, base?.model)
        if (if (base == null) draft.mode.isNotEmpty() else draft.mode != base.mode) out["mode"] = draft.mode
        val skills = draft.skills.map { it.trim() }.filter { it.isNotEmpty() }
        if (if (base == null) skills.isNotEmpty() else skills != base.skills) out["skills"] = skills
        if (if (base == null) draft.backupModels.isNotEmpty() else draft.backupModels != base.backupModels) {
            out["backupModels"] = choiceArray(draft.backupModels, false)
        }
        if (if (base == null) draft.taskAgents.isNotEmpty() else draft.taskAgents != base.taskAgents) {
            out["taskAgents"] = choiceArray(draft.taskAgents, true)
        }
        if (base == null || !sameSchedule(draft.schedule, base.schedule)) out["schedule"] = draft.schedule.wire
        text("relayChannel", draft.relayChannel, base?.relayChannel)
        if (if (base == null) !draft.enabled else draft.enabled != base.enabled) out["enabled"] = draft.enabled
        if (if (base == null) !draft.selfImproving else draft.selfImproving != base.selfImproving) out["selfImproving"] = draft.selfImproving
        return out
    }

    fun sameSchedule(a: TeammateSchedule, b: TeammateSchedule): Boolean =
        a.mode == b.mode && a.time == b.time && a.days.sorted() == b.days.sorted() && a.events == b.events && a.active == b.active

    val weekdays: List<Pair<Int, String>> = listOf(1 to "Mon", 2 to "Tue", 3 to "Wed", 4 to "Thu", 5 to "Fri", 6 to "Sat", 7 to "Sun")

    fun scheduleModeLabel(mode: String): String = when (mode) {
        "on-demand" -> "On demand"
        "continuous" -> "Continuously"
        "hourly" -> "Every hour"
        "daily" -> "Every day"
        "weekly" -> "Every week"
        "weekdays" -> "Weekdays"
        "custom" -> "Chosen days"
        "events" -> "On events"
        else -> mode
    }

    fun usesTime(mode: String): Boolean = mode in listOf("daily", "weekly", "weekdays", "custom")

    fun usesDays(mode: String): Boolean = mode in listOf("weekly", "custom")

    fun usesEvents(mode: String): Boolean = mode == "events"
}
