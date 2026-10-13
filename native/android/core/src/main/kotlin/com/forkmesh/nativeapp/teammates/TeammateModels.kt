package com.forkmesh.nativeapp.teammates

import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

enum class TeammateStatus(val wire: String) {
    Working("Working"),
    Queued("Queued"),
    Waiting("Waiting"),
    Scheduled("Scheduled"),
    Paused("Paused"),
    Idle("Idle"),
    ;

    val isWorking: Boolean get() = this == Working || this == Queued || this == Waiting

    val sortRank: Int
        get() = when (this) {
            Working -> 0
            Waiting -> 1
            Queued -> 2
            Scheduled -> 3
            Idle -> 4
            Paused -> 5
        }

    companion object {
        fun of(wire: String): TeammateStatus = entries.firstOrNull { it.wire == wire } ?: Idle
    }
}

data class TeammateSchedule(
    val mode: String = "on-demand",
    val time: String = "09:00",
    val days: List<Int> = emptyList(),
    val events: List<String> = emptyList(),
    val active: Boolean = false,
    val human: String = "",
    val next: String = "",
    val progress: Double = 0.0,
) {
    val isAutomatic: Boolean get() = active && mode != "on-demand"

    val wire: Map<String, Any?>
        get() = linkedMapOf(
            "mode" to mode,
            "time" to time,
            "days" to days.map(Int::toLong),
            "events" to events,
            "active" to active,
        )

    companion object {
        fun from(json: Map<String, Any?>): TeammateSchedule = TeammateSchedule(
            mode = json.str("mode", "on-demand"),
            time = json.str("time", "09:00"),
            days = json.list("days").map { jsonLong(it).toInt() }.filter { it in 1..7 },
            events = json.strings("events"),
            active = json.bool("active"),
            human = json.str("human"),
            next = json.str("next"),
            progress = json.double("progress"),
        )
    }
}

data class AgentChoiceRow(val taskType: String = "", val provider: String, val model: String)

data class TeammateMood(
    val key: String = "",
    val emoji: String = "",
    val label: String = "",
    val motion: String = "still",
    val attention: Boolean = false,
    val intensity: Double = 0.0,
    val reasons: List<String> = emptyList(),
) {
    val isEmpty: Boolean get() = emoji.isEmpty()

    val line: String get() = if (reasons.isEmpty()) label else "$label: ${reasons.joinToString(", ")}"

    companion object {
        fun from(json: Map<String, Any?>): TeammateMood = TeammateMood(
            key = json.str("key"),
            emoji = json.str("emoji"),
            label = json.str("label"),
            motion = json.str("motion", "still"),
            attention = json.bool("attention"),
            intensity = json.double("intensity"),
            reasons = json.strings("reasons"),
        )
    }
}

data class TeammateRow(
    val id: String,
    val name: String,
    val avatar: String = "",
    val specialty: String = "",
    val focus: String = "",
    val provider: String = "",
    val model: String = "",
    val status: TeammateStatus = TeammateStatus.Idle,
    val enabled: Boolean = true,
    val schedule: TeammateSchedule = TeammateSchedule(),
    val skills: List<String> = emptyList(),
    val monitors: Int = 0,
    val lastRunAtMs: Long = 0,
    val lastActive: String = "",
    val lastSessionId: Int = 0,
    val lastTrigger: String = "",
    val workflow: String = "",
    val improvementCycles: Int = 0,
    val mood: TeammateMood = TeammateMood(),
) {
    val initials: String
        get() {
            val letters = name.split(' ').filter { it.isNotEmpty() }.take(2).map { it.first().uppercase() }.joinToString("")
            return letters.ifEmpty { "?" }
        }

    val takesPrompts: Boolean get() = workflow.isEmpty()

    val modelLine: String get() = listOf(provider, model).filter { it.isNotEmpty() }.joinToString(" · ")

    companion object {
        fun from(json: Map<String, Any?>): TeammateRow = TeammateRow(
            id = json.str("id"),
            name = json.str("name"),
            avatar = json.str("avatar"),
            specialty = json.str("specialty"),
            focus = json.str("focus"),
            provider = json.str("provider"),
            model = json.str("model"),
            status = TeammateStatus.of(json.str("status")),
            enabled = json.optBool("enabled") ?: true,
            schedule = TeammateSchedule.from(json.obj("schedule")),
            skills = json.strings("skills"),
            monitors = json.int("monitors"),
            lastRunAtMs = json.long("lastRunAtMs"),
            lastActive = json.str("lastActive"),
            lastSessionId = json.int("lastSessionId"),
            lastTrigger = json.str("lastTrigger"),
            workflow = json.str("workflow"),
            improvementCycles = json.int("improvementCycles"),
            mood = TeammateMood.from(json.obj("mood")),
        )
    }
}

data class TeammatePermission(val id: String, val label: String)

data class TeammateLogMonitor(val badge: String, val threshold: Int, val taskType: String, val active: Boolean) {
    val id: String get() = badge + taskType
}

data class TeammateDetail(
    val row: TeammateRow,
    val description: String = "",
    val instructions: String = "",
    val mode: String = "",
    val backupModels: List<AgentChoiceRow> = emptyList(),
    val taskAgents: List<AgentChoiceRow> = emptyList(),
    val logMonitors: List<TeammateLogMonitor> = emptyList(),
    val permissions: List<TeammatePermission> = emptyList(),
    val relayChannel: String = "",
    val selfImproving: Boolean = true,
    val improvementNotes: List<String> = emptyList(),
    val activity: List<String> = emptyList(),
    val shortcut: String = "",
    val mention: String = "",
    val latestOutput: String = "",
) {
    companion object {
        fun from(json: Map<String, Any?>): TeammateDetail = TeammateDetail(
            row = TeammateRow.from(json),
            description = json.str("description"),
            instructions = json.str("instructions"),
            mode = json.str("mode"),
            backupModels = json.maps("backupModels").map { AgentChoiceRow(provider = it.str("provider"), model = it.str("model")) },
            taskAgents = json.maps("taskAgents").map { AgentChoiceRow(it.str("taskType"), it.str("provider"), it.str("model")) },
            logMonitors = json.maps("logMonitors").map {
                TeammateLogMonitor(it.str("badge"), it.int("threshold"), it.str("taskType"), it.bool("active"))
            },
            permissions = json.maps("permissions").map { TeammatePermission(it.str("id"), it.str("label")) },
            relayChannel = json.str("relayChannel"),
            selfImproving = json.optBool("selfImproving") ?: true,
            improvementNotes = json.strings("improvementNotes"),
            activity = json.strings("activity"),
            shortcut = json.str("shortcut"),
            mention = json.str("mention"),
            latestOutput = json.str("latestOutput"),
        )
    }
}

data class TeamSummary(val total: Int = 0, val working: Int = 0, val scheduled: Int = 0, val idle: Int = 0) {
    val line: String get() = "$total special agent${if (total == 1) "" else "s"} · $working working · $scheduled scheduled · $idle idle"

    companion object {
        fun from(json: Map<String, Any?>): TeamSummary =
            TeamSummary(json.int("total"), json.int("working"), json.int("scheduled"), json.int("idle"))
    }
}

data class TeammatesSnapshot(
    val repo: String = "",
    val repos: List<String> = emptyList(),
    val available: Boolean = false,
    val teammates: List<TeammateRow> = emptyList(),
    val errors: List<String> = emptyList(),
    val summary: TeamSummary = TeamSummary(),
) {
    val sorted: List<TeammateRow>
        get() = teammates.sortedWith(compareBy<TeammateRow> { it.status.sortRank }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    fun filtered(status: TeammateStatus?): List<TeammateRow> = when {
        status == null -> sorted
        status == TeammateStatus.Working -> sorted.filter { it.status.isWorking }
        else -> sorted.filter { it.status == status }
    }

    val automaticSchedules: Int get() = teammates.count { it.schedule.isAutomatic }

    companion object {
        fun from(json: Map<String, Any?>): TeammatesSnapshot = TeammatesSnapshot(
            repo = json.str("repo"),
            repos = json.strings("repos"),
            available = json.bool("available"),
            teammates = json.maps("teammates").map(TeammateRow::from),
            errors = json.strings("errors"),
            summary = TeamSummary.from(json.obj("summary")),
        )
    }
}

data class TaskTypeOption(val id: String, val label: String)

data class TeammateOptions(
    val providers: List<String>,
    val modes: List<String>,
    val scheduleModes: List<String>,
    val taskTypes: List<TaskTypeOption> = emptyList(),
    val editable: List<String> = emptyList(),
) {
    companion object {
        fun from(json: Map<String, Any?>): TeammateOptions = TeammateOptions(
            providers = json.strings("providers"),
            modes = json.strings("modes"),
            scheduleModes = json.strings("scheduleModes"),
            taskTypes = json.maps("taskTypes").map { TaskTypeOption(it.str("id"), it.str("label")) },
            editable = json.strings("editable"),
        )

        val fallback = TeammateOptions(
            providers = listOf("codex", "claude-code", "openai", "claude-api"),
            modes = listOf("", "Auto", "Plan", "Ask"),
            scheduleModes = listOf("on-demand", "continuous", "hourly", "daily", "weekly", "weekdays", "custom", "events"),
        )
    }
}

data class BriefingEntry(
    val id: String,
    val name: String,
    val avatar: String,
    val status: TeammateStatus,
    val sessionId: Int,
    val sessionStatus: String,
    val updatedAtMs: Long,
    val output: String,
) {
    val hasUpdate: Boolean get() = sessionId > 0

    companion object {
        fun from(json: Map<String, Any?>): BriefingEntry = BriefingEntry(
            id = json.str("id"),
            name = json.str("name"),
            avatar = json.str("avatar"),
            status = TeammateStatus.of(json.str("status")),
            sessionId = json.int("sessionId"),
            sessionStatus = json.str("sessionStatus"),
            updatedAtMs = json.long("updatedAtMs"),
            output = json.str("output"),
        )
    }
}

data class TeamBriefing(
    val repo: String = "",
    val date: String = "",
    val updates: Int = 0,
    val completed: Int = 0,
    val inProgress: Int = 0,
    val entries: List<BriefingEntry> = emptyList(),
) {
    val headline: String
        get() {
            if (updates == 0) return "No updates yet today"
            return "$updates update${if (updates == 1) "" else "s"} · $completed done · $inProgress in progress"
        }

    companion object {
        fun from(json: Map<String, Any?>): TeamBriefing = TeamBriefing(
            repo = json.str("repo"),
            date = json.str("date"),
            updates = json.int("updates"),
            completed = json.int("completed"),
            inProgress = json.int("inProgress"),
            entries = json.maps("entries").map(BriefingEntry::from),
        )
    }
}

data class TeammateStatsRow(
    val id: String,
    val name: String,
    val avatar: String,
    val runs: Int,
    val succeeded: Int,
    val failed: Int,
    val graded: Int,
    val averageGrade: Double,
    val costUsd: Double,
) {
    val successRate: Double get() = if (runs > 0) succeeded.toDouble() / runs else 0.0

    companion object {
        fun from(json: Map<String, Any?>): TeammateStatsRow = TeammateStatsRow(
            id = json.str("id"),
            name = json.str("name"),
            avatar = json.str("avatar"),
            runs = json.int("runs"),
            succeeded = json.int("succeeded"),
            failed = json.int("failed"),
            graded = json.int("graded"),
            averageGrade = json.double("averageGrade"),
            costUsd = json.double("costUsd"),
        )
    }
}

data class TeamTask(
    val session: Int,
    val teammates: List<String>,
    val title: String,
    val status: String,
    val grade: Int,
    val gradable: Boolean,
    val finishedAtMs: Long,
    val costUsd: Double,
) {
    companion object {
        fun from(json: Map<String, Any?>): TeamTask = TeamTask(
            session = json.int("session"),
            teammates = json.strings("teammates"),
            title = json.str("title"),
            status = json.str("status"),
            grade = json.int("grade"),
            gradable = json.bool("gradable"),
            finishedAtMs = json.long("finishedAtMs"),
            costUsd = json.double("costUsd"),
        )
    }
}

data class TeamStats(
    val repo: String = "",
    val teammates: List<TeammateStatsRow> = emptyList(),
    val tasks: List<TeamTask> = emptyList(),
    val totalTasks: Int = 0,
) {
    val totalCost: Double get() = teammates.sumOf { it.costUsd }

    companion object {
        fun from(json: Map<String, Any?>): TeamStats = TeamStats(
            repo = json.str("repo"),
            teammates = json.maps("teammates").map(TeammateStatsRow::from),
            tasks = json.maps("tasks").map(TeamTask::from),
            totalTasks = json.int("totalTasks"),
        )
    }
}

data class TeamRoomMessage(
    val id: String,
    val sender: String,
    val text: String,
    val timestampMs: Long,
    val isSelf: Boolean,
    val threadRootId: String,
) {
    companion object {
        fun from(json: Map<String, Any?>): TeamRoomMessage = TeamRoomMessage(
            id = json.str("id"),
            sender = json.str("sender"),
            text = json.str("text"),
            timestampMs = json.long("t"),
            isSelf = json.bool("self"),
            threadRootId = json.str("threadRootId"),
        )
    }
}
