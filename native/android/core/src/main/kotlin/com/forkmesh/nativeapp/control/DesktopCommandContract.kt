package com.forkmesh.nativeapp.control

object DesktopCommandContract {
    const val SIGNING_DOMAIN = "forkmesh-desktop-command-v1"
    const val UNSUPPORTED_MESSAGE = "This desktop request is not supported."
    const val RERUN_LABEL = "Request rerun on desktop"

    val pullCommands = listOf("pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close", "pull.reopen", "pull.delete")
    val runCommands = listOf("action.rerun")
    val releaseCommands = listOf("release.draft", "release.notes", "release.create", "release.publish")
    val commands: List<String> get() = pullCommands + runCommands + releaseCommands

    private val pullTargetPattern = Regex("^pull:[1-9][0-9]{0,8}$")
    private val runTargetPattern = Regex("^run:[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
    private val releaseTargetPattern = Regex("^release:[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$")

    fun canonical(owner: String, name: String, command: String, target: String, ts: String): String =
        listOf(SIGNING_DOMAIN, owner, name, command, target, ts).joinToString("\n")

    fun pullTarget(number: Int): String = "pull:$number"

    fun runTarget(runId: Long): String = "run:$runId"

    fun releaseTarget(tag: String): String = "release:$tag"

    fun isValid(command: String, target: String): Boolean {
        val pattern = when (command) {
            in pullCommands -> pullTargetPattern
            in runCommands -> runTargetPattern
            in releaseCommands -> releaseTargetPattern
            else -> return false
        }
        return pattern.matches(target)
    }

    fun path(owner: String, name: String): String = "/api/repo/$owner/$name/desktop-commands"

    fun routeAnswered(status: Int, code: String): Boolean? = when {
        status == 403 && code == "not_authorized" -> true
        status == 400 && (code == "invalid_json" || code == "unsupported_command") -> true
        status == 404 || status == 405 -> false
        else -> null
    }

    fun errorMessage(status: Int, code: String): String = when (code) {
        "not_authorized" ->
            "Only the repo owner or an organization member with write access can request desktop control for this repo."
        "unsupported_command" -> "The server does not accept that desktop request."
        "signature_required", "invalid_signature" ->
            "The server could not verify this device's signature. Pair this device with the owner key and try again."
        "stale_request" -> "This device's clock is more than five minutes off. Correct the time and try again."
        "replayed_request" -> "That request was already sent."
        "command_queue_full" -> "Fifty requests are already waiting for the desktop. Approve or decline them there first."
        else -> if (status == 404 || status == 405) "This server does not take desktop requests yet." else "Desktop request was not queued (HTTP $status)."
    }
}

class DesktopCommandAvailability(private val clock: () -> Long = System::currentTimeMillis) {
    private val lock = Any()
    private var checkedAtMs = 0L
    private var inFlight = false

    @Volatile
    var available = false
        private set

    fun refresh(probe: () -> Boolean?): Boolean {
        synchronized(lock) {
            if (available || inFlight) return available
            val now = clock()
            if (checkedAtMs > 0 && now - checkedAtMs < RECHECK_MS) return available
            inFlight = true
        }
        val answer = try {
            probe()
        } catch (_: Exception) {
            null
        }
        synchronized(lock) {
            inFlight = false
            if (answer == null) return available
            checkedAtMs = clock()
            available = answer
            return available
        }
    }

    fun reset() {
        synchronized(lock) {
            available = false
            checkedAtMs = 0
        }
    }

    companion object {
        const val RECHECK_MS = 15 * 60 * 1000L
        val shared = DesktopCommandAvailability()
    }
}
