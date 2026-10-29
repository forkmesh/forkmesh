package com.forkmesh.nativeapp.teammates

import com.forkmesh.nativeapp.agents.ops.OpsState
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str

data class TeamConfirmation(
    val token: String,
    val action: String,
    val subject: String,
    val summary: String,
    val expiresInMs: Int,
    val params: Map<String, Any?>,
) {
    val runParams: Map<String, Any?>
        get() {
            val out = LinkedHashMap(params)
            out["action"] = action
            out["token"] = token
            return out
        }

    companion object {
        fun from(json: Map<String, Any?>, params: Map<String, Any?>): TeamConfirmation? {
            val token = json.str("token")
            if (token.isEmpty()) return null
            return TeamConfirmation(
                token = token,
                action = json.str("action"),
                subject = json.str("subject"),
                summary = json.str("summary"),
                expiresInMs = json.int("expiresInMs", 60_000),
                params = params,
            )
        }
    }
}

sealed class TeammateAction {
    data object RunTeam : TeammateAction()
    data class Run(val id: String) : TeammateAction()
    data class Briefing(val id: String?) : TeammateAction()
    data class EnableSchedule(val id: String) : TeammateAction()
    data object DisableSchedules : TeammateAction()
    data class Grade(val session: Int, val grade: Int) : TeammateAction()
    data class Prompt(val id: String, val text: String) : TeammateAction()
    data class Stop(val id: String) : TeammateAction()
    data class ContinueSession(val id: String, val text: String) : TeammateAction()
    data class Edit(val id: String, val changes: Map<String, Any?>) : TeammateAction()
    data class Create(val changes: Map<String, Any?>) : TeammateAction()

    val params: Map<String, Any?>
        get() = when (this) {
            RunTeam -> linkedMapOf("action" to "runTeam")
            is Run -> linkedMapOf("action" to "run", "id" to id)
            is Briefing -> if (id == null) linkedMapOf("action" to "briefing") else linkedMapOf("action" to "briefing", "id" to id)
            is EnableSchedule -> linkedMapOf("action" to "enableSchedule", "id" to id)
            DisableSchedules -> linkedMapOf("action" to "disableSchedules")
            is Grade -> linkedMapOf("action" to "grade", "session" to session.toLong(), "grade" to grade.toLong())
            is Prompt -> linkedMapOf("action" to "prompt", "id" to id, "text" to text)
            is Stop -> linkedMapOf("action" to "stop", "id" to id)
            is ContinueSession -> linkedMapOf("action" to "continue", "id" to id, "text" to text)
            is Edit -> linkedMapOf("action" to "edit", "id" to id, "changes" to changes)
            is Create -> linkedMapOf("action" to "create", "changes" to changes)
        }

    val title: String
        get() = when (this) {
            RunTeam -> "Run the special agents now"
            is Run -> "Run now"
            is Briefing -> if (id == null) "Generate briefing" else "Refresh briefing"
            is EnableSchedule -> "Turn schedule on"
            DisableSchedules -> "Pause all schedules"
            is Grade -> "Grade task"
            is Prompt -> "Send prompt"
            is Stop -> "Stop"
            is ContinueSession -> "Continue"
            is Edit -> "Save changes"
            is Create -> "Add special agent"
        }

    val isQuiet: Boolean get() = this is Grade || this is Prompt
}

object TeamErrorText {
    fun describe(error: Throwable): String =
        if (error is LocalControlException) describe(error.code) else ControlErrorText.describe(error)

    fun describe(code: String): String {
        if (code.startsWith("bad_field:")) return "The desktop refused the ${code.removePrefix("bad_field:")} field."
        if (code.startsWith("field_not_editable:")) return "${code.removePrefix("field_not_editable:")} is edited on the desktop."
        return when (code) {
            "unsupported", "unknown_method" -> "This desktop's ForkMesh is too old for that. Update it."
            "unknown_teammate" -> "That special agent is gone."
            "no_repository" -> "Pick a repository with a working tree on the desktop first."
            "unknown_repo" -> "The desktop has no such repository."
            "not_started" -> "The special agent could not start. The desktop says why on its Special Agents page."
            "not_gradable" -> "Only a finished special agent task can be graded."
            "native_workflow" -> "This special agent runs a built-in workflow and takes no prompt."
            "no_session" -> "There is no session to continue."
            "teammate_exists" -> "A special agent with that name already exists."
            "empty_prompt" -> "Type a prompt first."
            "prompt_too_long" -> "That prompt is too long."
            "no_changes" -> "Nothing changed."
            "bad_family" -> "The desktop does not know that tool family."
            "unknown_shortcut" -> "That shortcut is gone."
            "script_on_desktop" -> "Script shortcuts run on the desktop itself."
            "shortcut_running" -> "Another shortcut is already running on the desktop."
            "shortcut_not_started" -> "The desktop could not start that shortcut."
            "approval_required" -> "This desktop's ForkMesh still asks you to allow the file there first. Update ForkMesh on the desktop so runs from this phone count as your approval."
            "empty_shortcut" -> "That shortcut has no prompt."
            "unsupported_agent", "model_mismatch" -> "The shortcut names an agent or model the desktop cannot run."
            "agent_not_started" -> "The desktop could not start an agent."
            else -> ControlErrorText.describe(code)
        }
    }

    fun isUnsupported(error: Throwable): Boolean =
        error is LocalControlException && (error.code == "unsupported" || error.code == "unknown_method")
}

class TeammatesStore(channel: ControlCalling, private val launch: (() -> Unit) -> Unit = { it() }) : OpsState() {
    val family = ControlFamilyChannel(DesktopControlFamilyName.Teammates, channel)
    private val lock = Any()
    private var tap: Any? = family.addEventTap { suffix, _ ->
        if (suffix == "changed") {
            changesSeen++
            launch { refresh() }
        }
    }
    private var refreshing = false
    private var again = false

    @Volatile
    var changesSeen = 0
        private set

    @Volatile
    var snapshot: TeammatesSnapshot? = null
        private set

    @Volatile
    var options: TeammateOptions = TeammateOptions.fallback
        private set

    @Volatile
    var loading = false
        private set

    @Volatile
    var error = ""
        private set

    @Volatile
    var unsupported = false
        private set

    fun refresh() {
        synchronized(lock) {
            if (refreshing) {
                again = true
                return
            }
            refreshing = true
        }
        loading = true
        changed()
        try {
            do {
                synchronized(lock) { again = false }
                try {
                    snapshot = TeammatesSnapshot.from(family.call("list"))
                    error = ""
                    unsupported = false
                } catch (failure: Exception) {
                    unsupported = TeamErrorText.isUnsupported(failure)
                    error = TeamErrorText.describe(failure)
                }
            } while (synchronized(lock) { again })
        } finally {
            synchronized(lock) { refreshing = false }
            loading = false
            changed()
        }
    }

    fun loadOptions() {
        val reply = runCatching { family.call("options") }.getOrNull() ?: return
        val parsed = TeammateOptions.from(reply)
        if (parsed.providers.isNotEmpty()) {
            options = parsed
            changed()
        }
    }

    fun detail(id: String): TeammateDetail = TeammateDetail.from(family.call("get", linkedMapOf("id" to id)))

    fun briefing(): TeamBriefing = TeamBriefing.from(family.call("briefing"))

    fun stats(): TeamStats = TeamStats.from(family.call("stats"))

    fun room(limit: Int = 100): List<TeamRoomMessage> =
        family.call("room", linkedMapOf("limit" to limit.toLong())).maps("messages").map(TeamRoomMessage::from)

    fun selectRepo(repo: String) {
        try {
            snapshot = TeammatesSnapshot.from(family.call("selectRepo", linkedMapOf("repo" to repo)))
            error = ""
        } catch (failure: Exception) {
            error = TeamErrorText.describe(failure)
        }
        changed()
    }

    fun prepare(action: TeammateAction): TeamConfirmation {
        val params = action.params
        return TeamConfirmation.from(family.call("prepare", params), params) ?: throw LocalControlException("bad_answer")
    }

    fun run(confirmation: TeamConfirmation): String {
        val reply = family.call("run", confirmation.runParams, 45_000)
        refresh()
        return reply.str("message")
    }

    fun perform(action: TeammateAction): String = run(prepare(action))

    fun teammate(id: String): TeammateRow? = snapshot?.teammates?.firstOrNull { it.id == id }

    fun stop() {
        val token = synchronized(lock) { tap.also { tap = null } } ?: return
        family.removeEventTap(token)
    }
}
