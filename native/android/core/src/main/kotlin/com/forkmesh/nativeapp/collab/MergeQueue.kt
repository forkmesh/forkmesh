package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.modelInt

enum class MergeQueueEntryState(val raw: String, val title: String) {
    Queued("queued", "Queued"),
    Updating("updating", "Updating"),
    Merging("merging", "Merging"),
    Blocked("blocked", "Blocked"),
    Failed("failed", "Failed"),
    ;

    val isRunning: Boolean get() = this == Updating || this == Merging
    val needsAttention: Boolean get() = this == Blocked || this == Failed

    companion object {
        fun wire(value: String): MergeQueueEntryState = entries.firstOrNull { it.raw == value } ?: Queued
    }
}

data class MergeQueueEntry(
    val number: Int,
    val title: String = "",
    val state: MergeQueueEntryState = MergeQueueEntryState.Queued,
    val detail: String = "",
    val queuedAtMs: Long = 0,
    val stateAtMs: Long = 0,
) {
    companion object {
        fun fromJson(j: Map<String, Any?>): MergeQueueEntry? {
            val number = j.int("number")
            if (number <= 0) return null
            return MergeQueueEntry(
                number,
                j.str("title").take(200),
                MergeQueueEntryState.wire(j.str("state")),
                j.str("detail").take(200),
                modelInt(j["queuedAtMs"]),
                modelInt(j["stateAtMs"]),
            )
        }
    }
}

data class MergeQueueRepo(
    val owner: String,
    val name: String,
    val enabled: Boolean,
    val paused: Boolean = false,
    val isOpen: Boolean = false,
    val writable: Boolean = true,
    val entries: List<MergeQueueEntry> = emptyList(),
) {
    val id: String get() = "$owner/$name"

    val summary: String
        get() {
            if (!enabled) return "Off"
            if (entries.isEmpty()) return if (paused) "Empty · paused" else "Empty"
            var text = "${entries.size} queued"
            val running = entries.count { it.state.isRunning }
            val blocked = entries.count { it.state.needsAttention }
            if (running > 0) text += " · $running in progress"
            if (blocked > 0) text += " · $blocked blocked"
            if (paused) text += " · paused"
            return text
        }

    fun place(number: Int): Int? = entries.indexOfFirst { it.number == number }.takeIf { it >= 0 }?.plus(1)

    companion object {
        fun fromJson(j: Map<String, Any?>): MergeQueueRepo? {
            val owner = j.str("owner")
            val name = j.str("name")
            if (owner.isEmpty() || name.isEmpty()) return null
            return MergeQueueRepo(
                owner,
                name,
                j["enabled"] == true,
                j["paused"] == true,
                j["open"] == true,
                j.optBool("writable") ?: true,
                j.maps("entries").take(MergeQueueLimits.MAX_ENTRIES).mapNotNull(MergeQueueEntry::fromJson),
            )
        }
    }
}

enum class MergeQueueAction(val raw: String, val title: String) {
    Enqueue("enqueue", "Add to merge queue"),
    Remove("remove", "Remove from queue"),
    Move("move", "Move"),
    Pause("pause", "Pause queue"),
    Resume("resume", "Resume queue"),
    Enable("enable", "Turn on merge queue"),
    Disable("disable", "Turn off merge queue"),
    Clear("clear", "Clear queue"),
    Process("process", "Merge next now"),
    ;

    val needsNumber: Boolean get() = this == Enqueue || this == Remove || this == Move
    val isDestructive: Boolean get() = this == Clear || this == Disable
}

data class MergeQueueRequest(val owner: String, val name: String, val action: MergeQueueAction, val number: Int = 0, val delta: Int = 0) {
    val params: Map<String, Any?>
        get() {
            val out = linkedMapOf<String, Any?>("owner" to owner, "name" to name, "action" to action.raw)
            if (action.needsNumber) out["number"] = number.toLong()
            if (action == MergeQueueAction.Move) out["delta"] = delta.toLong()
            return out
        }
}

data class MergeQueueConfirmation(val token: String, val summary: String, val expiresInMs: Int) {
    companion object {
        fun fromJson(j: Map<String, Any?>) =
            MergeQueueConfirmation(j.str("token"), j.str("summary"), j.int("expiresInMs", 60_000))
    }
}

data class MergeQueueResult(val message: String, val repo: MergeQueueRepo?) {
    companion object {
        @Suppress("UNCHECKED_CAST")
        fun fromJson(j: Map<String, Any?>) =
            MergeQueueResult(j.str("message"), (j["queue"] as? Map<String, Any?>)?.let(MergeQueueRepo::fromJson))
    }
}

object MergeQueueLimits {
    const val MAX_REPOS = 100
    const val MAX_ENTRIES = 100
}

object MergeQueueRules {
    fun actions(repo: MergeQueueRepo): List<MergeQueueAction> {
        if (!repo.writable) return emptyList()
        if (!repo.enabled) return listOf(MergeQueueAction.Enable)
        val out = mutableListOf(if (repo.paused) MergeQueueAction.Resume else MergeQueueAction.Pause)
        if (repo.entries.isNotEmpty() && !repo.paused) out.add(MergeQueueAction.Process)
        if (repo.entries.isNotEmpty()) out.add(MergeQueueAction.Clear)
        out.add(MergeQueueAction.Disable)
        return out
    }

    fun entryActions(entry: MergeQueueEntry, repo: MergeQueueRepo): List<Pair<MergeQueueAction, Int>> {
        if (!repo.writable || !repo.enabled || entry.state.isRunning) return emptyList()
        val place = repo.place(entry.number) ?: return emptyList()
        val out = ArrayList<Pair<MergeQueueAction, Int>>()
        if (place > 1) out.add(MergeQueueAction.Move to -1)
        if (place < repo.entries.size) out.add(MergeQueueAction.Move to 1)
        out.add(MergeQueueAction.Remove to 0)
        return out
    }

    fun canEnqueue(pullStatus: String, repo: MergeQueueRepo?, number: Int): Boolean {
        if (repo == null || !repo.enabled || !repo.writable) return false
        return pullStatus.trim().lowercase() == "open" && repo.place(number) == null
    }

    fun errorMessage(code: String): String = when (code) {
        "unsupported", "unknown_method" -> "This desktop does not know the merge queue yet. Update ForkMesh on the desktop."
        "not_owner" -> "Only the desktop's owner can change its merge queue."
        "unknown_repo" -> "The desktop does not have that repository."
        "queue_off" -> "The merge queue is off for this repository."
        "read_only" -> "This repository is a read-only mirror on the desktop; the owning node merges it."
        "not_open_pull" -> "Only open pull requests can join the queue."
        "already_queued" -> "That pull request is already in the queue."
        "not_queued" -> "That pull request is not in the queue."
        "repo_not_open" -> "Open this repository on the desktop to run its queue now; it keeps running while open."
        "busy" -> "The desktop is merging right now. Try again in a moment."
        "paused" -> "The queue is paused. Resume it first."
        "queue_full" -> "The queue is full."
        "bad_request" -> "The desktop could not read that request."
        "missing", "expired", "mismatch", "confirmation_missing", "confirmation_expired", "confirmation_mismatch" ->
            "The confirmation expired. Try again."
        "" -> "The desktop refused."
        else -> code.replace('_', ' ')
    }
}

typealias DesktopFamilyCall = (suffix: String, params: Map<String, Any?>, timeoutMs: Long) -> Map<String, Any?>

class MergeQueueRemote(private val call: DesktopFamilyCall) {
    fun list(owner: String = "", name: String = ""): List<MergeQueueRepo> {
        val params = LinkedHashMap<String, Any?>()
        if (owner.isNotEmpty() && name.isNotEmpty()) {
            params["owner"] = owner
            params["name"] = name
        }
        return call("list", params, 20_000).maps("repos").take(MergeQueueLimits.MAX_REPOS).mapNotNull(MergeQueueRepo::fromJson)
    }

    fun prepare(request: MergeQueueRequest): MergeQueueConfirmation {
        val confirmation = MergeQueueConfirmation.fromJson(call("prepare", request.params, 20_000))
        if (confirmation.token.isEmpty()) throw LocalControlException("confirmation_missing")
        return confirmation
    }

    fun run(request: MergeQueueRequest, token: String): MergeQueueResult {
        val params = LinkedHashMap(request.params)
        params["token"] = token
        return MergeQueueResult.fromJson(call("run", params, 60_000))
    }

    fun perform(request: MergeQueueRequest): MergeQueueResult = run(request, prepare(request).token)

    companion object {
        fun message(error: Throwable): String =
            if (error is LocalControlException) MergeQueueRules.errorMessage(error.code) else (error.message ?: "$error")
    }
}
