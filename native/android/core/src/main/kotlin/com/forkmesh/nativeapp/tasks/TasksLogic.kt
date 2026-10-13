package com.forkmesh.nativeapp.tasks

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.jsonInt
import com.forkmesh.nativeapp.notes.jsonObject
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString

data class TaskBoard(
    val tasks: List<JsonObject>,
    val actor: String,
    val canManage: Boolean,
    val members: List<String>,
    val departments: List<String>,
    val nextOffset: Long?,
) {
    fun mine(task: JsonObject): Boolean =
        jsonString(task["assigneeKind"]) == "user" && jsonString(task["assignee"]).lowercase() == actor

    val mineOpen: List<JsonObject> get() = tasks.filter { it["status"] != "done" && mine(it) }
    val open: List<JsonObject> get() = tasks.filter { it["status"] != "done" }
    val done: List<JsonObject> get() = tasks.filter { it["status"] == "done" }

    val people: List<TaskPerson>
        get() {
            val names = LinkedHashSet<String>()
            for (name in members) {
                val key = name.trim().lowercase()
                if (key.isNotEmpty() && key != actor) names.add(key)
            }
            val ordered = names.sorted()
            val users = (if (actor.isEmpty()) ordered else listOf(actor) + ordered).map { name ->
                TaskPerson(TaskPerson.userKey(name), if (name == actor) "@$name (you)" else "@$name", name)
            }
            return listOf(TaskPerson(TaskPerson.EVERYONE, "Everyone")) + users +
                TaskPerson(TaskPerson.BOT, "Bot") + TaskPerson(TaskPerson.UNASSIGNED, "Unassigned")
        }

    fun personOpenCounts(): Map<String, Int> {
        val counts = HashMap<String, Int>()
        for (task in open) {
            counts.merge(TaskPerson.keyOf(task), 1, Int::plus)
            counts.merge(TaskPerson.EVERYONE, 1, Int::plus)
        }
        return counts
    }

    fun lists(person: String): List<List<JsonObject>> =
        listOf(mineOpen, open.filter { TaskPerson.matches(it, person) }, done.filter { TaskPerson.matches(it, person) })

    fun tabCounts(person: String): List<Int> = lists(person).map { it.size }

    fun merged(page: TaskBoard): TaskBoard {
        val seen = tasks.map { it["id"] }.toSet()
        return copy(tasks = tasks + page.tasks.filter { it["id"] !in seen }, nextOffset = page.nextOffset)
    }

    companion object {
        fun fromJson(data: JsonObject): TaskBoard = TaskBoard(
            tasks = jsonObjects(data["tasks"]),
            actor = jsonString(data["actor"]).lowercase(),
            canManage = data["canManage"] == true,
            members = strings(data["members"]),
            departments = strings(data["departments"]),
            nextOffset = if (data["hasMore"] == true) jsonInt(data["nextOffset"]) else null,
        )

        private fun strings(value: Any?): List<String> =
            (value as? List<*>)?.map { jsonString(it, "null") }?.filter { it.isNotEmpty() } ?: emptyList()
    }
}

data class TaskPerson(val key: String, val label: String, val name: String = "") {
    companion object {
        const val EVERYONE = "all"
        const val BOT = "agent"
        const val UNASSIGNED = "unassigned"
        private val BOT_KINDS = setOf("agent", "bot", "claude", "codex")

        fun userKey(name: String): String = "user:${name.trim().lowercase()}"

        fun keyOf(task: JsonObject): String {
            val kind = jsonString(task["assigneeKind"]).trim().lowercase()
            if (kind in BOT_KINDS) return BOT
            val name = jsonString(task["assignee"]).trim()
            return if (kind == UNASSIGNED || name.isEmpty()) UNASSIGNED else userKey(name)
        }

        fun matches(task: JsonObject, person: String): Boolean = person == EVERYONE || keyOf(task) == person
    }
}

data class TaskFilter(val tab: Int = 0, val person: String = TaskPerson.EVERYONE) {
    fun selectTab(index: Int): TaskFilter = if (index == 0) TaskFilter(0) else copy(tab = index)

    fun selectPerson(key: String): TaskFilter {
        val next = if (key == person) TaskPerson.EVERYONE else key
        return TaskFilter(if (tab == 0 && next != TaskPerson.EVERYONE) 1 else tab, next)
    }

    fun within(board: TaskBoard): TaskFilter =
        if (person == TaskPerson.EVERYONE || board.people.any { it.key == person }) this else copy(person = TaskPerson.EVERYONE)
}

class TasksApi(private val api: ApiService) {
    fun call(method: String, suffix: String = "", body: JsonObject? = null): JsonObject =
        api.accountApiCall(method, "/api/tasks$suffix", body)

    fun board(offset: Long? = null): TaskBoard = TaskBoard.fromJson(call("GET", if (offset == null) "" else "?offset=$offset"))

    fun task(id: String): JsonObject = jsonObject(call("GET", "/$id")["task"])

    fun post(id: String, suffix: String, body: JsonObject = emptyMap()): JsonObject? =
        call("POST", "/$id$suffix", body)["task"]?.let(::jsonObject)

    fun delete(id: String) {
        call("DELETE", "/$id", emptyMap())
    }

    fun create(title: String, details: String, department: String, assignee: String): JsonObject {
        val body = linkedMapOf<String, Any?>("title" to title.trim(), "details" to details.trim(), "department" to department)
        if (assignee.isEmpty()) {
            body["assigneeKind"] = "unassigned"
        } else {
            body["assigneeKind"] = "user"
            body["assignee"] = assignee
        }
        return jsonObject(call("POST", "", body)["task"])
    }

    fun update(id: String, title: String, details: String): JsonObject =
        jsonObject(call("PATCH", "/$id", linkedMapOf("title" to title.trim(), "details" to details.trim()))["task"])
}

object TasksLogic {
    private val MESSAGES = mapOf(
        "org_member_required" to "Tasks live on the organization task board. Ask an owner to add you as a member to see them.",
        "office_organization_unavailable" to "The organization task board is unavailable right now.",
        "active_task_exists" to "You already have a task running. Stop it before starting another.",
        "assignee_only" to "Only the assignee can do that.",
        "reminder_too_soon" to "The assignee was reminded less than 10 minutes ago. Try again later.",
        "assignee_or_manager_only" to "Only the assignee or a manager can do that.",
        "task_owner_or_reviewer_only" to "Only the creator, assignee, or reviewer can reopen this task.",
        "forbidden" to "You do not have permission to do that.",
    )

    val CHECKIN_STATES = listOf("going_well", "blocked", "needs_help")

    fun errorText(error: Throwable, fallback: String): String {
        if (error !is OrgApiException) return fallback
        return MESSAGES[error.code] ?: error.message
    }

    private fun cap(value: String): String = if (value.isEmpty()) value else value[0].uppercase() + value.substring(1)

    fun departmentLabel(value: String): String = cap(value.replace('-', ' '))

    fun elapsed(ms: Long): String {
        val minutes = ms / 60_000
        if (minutes < 1) return "under a minute"
        if (minutes < 60) return "${minutes}m"
        val hours = minutes / 60
        val rest = minutes % 60
        return if (rest == 0L) "${hours}h" else "${hours}h ${rest}m"
    }

    fun statusLabel(task: JsonObject): String = when (jsonString(task["status"], "null")) {
        "active" -> "Active"
        "done" -> "Done"
        else -> "Idle"
    }

    fun assigneeLabel(task: JsonObject): String {
        when (jsonString(task["assigneeKind"], "null")) {
            "agent" -> return "Agent"
            "unassigned" -> return "Unassigned"
        }
        val name = jsonString(task["assignee"])
        return if (name.isEmpty()) "Unassigned" else "@$name"
    }

    fun checkinLabel(state: String): String = when (state) {
        "going_well" -> "Going well"
        "blocked" -> "Blocked"
        "needs_help" -> "Needs help"
        else -> cap(state.replace('_', ' '))
    }

    fun rowSubtitle(task: JsonObject, now: Long = System.currentTimeMillis()): String {
        val replies = jsonInt(task["replyCount"])
        val parts = mutableListOf(
            assigneeLabel(task),
            departmentLabel(jsonString(task["department"], "general")),
            if (task["status"] == "done") "done ${relativeTime(jsonInt(task["completedAt"]), now)}" else "updated ${relativeTime(jsonInt(task["updatedAt"]), now)}",
        )
        if (replies > 0) parts.add("$replies ${if (replies == 1L) "reply" else "replies"}")
        return parts.joinToString(" · ")
    }

    fun emptyTitle(filter: Int, person: TaskPerson? = null): String {
        val who = when (person?.key) {
            null, TaskPerson.EVERYONE -> ""
            TaskPerson.BOT -> " for the bot"
            TaskPerson.UNASSIGNED -> " without an assignee"
            else -> " for @${person.name}"
        }
        return when (filter) {
            0 -> "Nothing assigned to you"
            1 -> "No open tasks$who"
            else -> "No finished tasks$who yet"
        }
    }

    data class Actions(val start: Boolean, val stop: Boolean, val checkin: Boolean, val finish: Boolean, val reopen: Boolean, val remind: Boolean = false)

    fun actions(task: JsonObject, actor: String, canManage: Boolean): Actions {
        val status = jsonString(task["status"], "null")
        val assigneeName = if (jsonString(task["assigneeKind"]) == "user") jsonString(task["assignee"]).lowercase() else ""
        val assignee = assigneeName.isNotEmpty() && assigneeName == actor
        val reviewer = jsonString(jsonObject(task["qa"])["reviewer"])
        val canReopen = canManage || assignee || jsonString(task["createdBy"], "null") == actor || reviewer == actor
        return Actions(
            start = assignee && status == "idle",
            stop = assignee && status == "active",
            checkin = assignee && status == "active",
            finish = status != "done" && (assignee || canManage),
            reopen = status == "done" && canReopen,
            remind = status != "done" && assigneeName.isNotEmpty() && !assignee,
        )
    }

    fun reminderStatus(reminder: JsonObject, now: Long = System.currentTimeMillis()): String {
        val to = jsonString(reminder["to"])
        val who = if (to.isNotEmpty()) "@$to" else "the assignee"
        val seenAt = jsonInt(reminder["seenAt"])
        if (seenAt > 0) {
            val how = if (jsonString(reminder["seenVia"]) == "ping") "opened the ping" else "opened the task"
            return "Seen: $who $how ${relativeTime(seenAt, now)}"
        }
        if (reminder["delivered"] == false) return "Not delivered: the ping could not be filed"
        val phones = jsonInt(reminder["phones"])
        val reach = if (phones > 0) "pinged and pushed to $phones phone${if (phones == 1L) "" else "s"}" else "pinged"
        return "Not seen yet: $who was $reach"
    }

    fun assignees(actor: String, canManage: Boolean, members: List<String>): List<String> {
        val out = LinkedHashSet<String>()
        if (actor.isNotEmpty()) out.add(actor)
        if (canManage) out.addAll(members)
        return out.toList()
    }
}
