package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str

data class RemoteConfirmation(
    val family: DesktopControlFamilyName,
    val token: String,
    val action: String,
    val subject: String = "",
    val summary: String = "",
    val typeToConfirm: String = "",
    val expiresInMs: Int = 60_000,
    val params: Map<String, Any?> = emptyMap(),
) {
    val needsTypedConfirmation: Boolean get() = typeToConfirm.isNotEmpty()

    val needsDeviceAuthentication: Boolean get() = needsTypedConfirmation

    fun accepts(typed: String): Boolean = typeToConfirm.isEmpty() || typed.trim() == typeToConfirm

    fun runParams(typed: String): Map<String, Any?> {
        val out = LinkedHashMap(params)
        out["action"] = action
        out["token"] = token
        if (needsTypedConfirmation) out["confirmText"] = typed.trim()
        return out
    }

    companion object {
        fun fromJson(family: DesktopControlFamilyName, json: Map<String, Any?>, params: Map<String, Any?>): RemoteConfirmation? {
            val token = json.str("token")
            if (token.isEmpty()) return null
            return RemoteConfirmation(
                family = family,
                token = token,
                action = json.str("action"),
                subject = json.str("subject"),
                summary = json.str("summary"),
                typeToConfirm = json.str("typeToConfirm"),
                expiresInMs = json.int("expiresInMs", 60_000),
                params = params,
            )
        }
    }
}

data class RemoteActionOutcome(
    val action: String,
    val subject: String,
    val ok: Boolean,
    val message: String,
    val atMs: Long,
) {
    val id: String get() = "$action:$subject:$atMs"

    val headline: String get() = if (subject.isEmpty()) action else "$action · $subject"

    companion object {
        fun fromJson(json: Map<String, Any?>, atMs: Long) = RemoteActionOutcome(
            action = json.str("action"),
            subject = json.firstStr("subject", "root"),
            ok = json.bool("ok"),
            message = json.str("message"),
            atMs = atMs,
        )
    }
}

data class RemoteStarted(val action: String, val subject: String, val message: String) {
    companion object {
        fun fromJson(json: Map<String, Any?>) = RemoteStarted(json.str("action"), json.str("subject"), json.str("message"))
    }
}

fun ControlFamilyChannel.prepare(params: Map<String, Any?>): RemoteConfirmation {
    val reply = call("prepare", params, 20_000)
    return RemoteConfirmation.fromJson(name, reply, params) ?: throw LocalControlException("bad_answer")
}

fun ControlFamilyChannel.run(confirmation: RemoteConfirmation, typed: String = ""): RemoteStarted {
    if (!confirmation.accepts(typed)) throw LocalControlException("name_mismatch")
    return RemoteStarted.fromJson(call("run", confirmation.runParams(typed), 45_000))
}

fun isUnsupportedControl(error: Throwable): Boolean =
    error is LocalControlException && (error.code == "unsupported" || error.code == "unknown_method")

object RemoteErrorText {
    fun describe(error: Throwable): String =
        if (error is LocalControlException) describe(error.code) else ControlErrorText.describe(error)

    fun describe(code: String): String = when (code) {
        "unsupported", "unknown_method" -> "This desktop's ForkMesh is too old for that. Update it."
        "unknown_root" -> "The desktop no longer has that repository or worktree."
        "outside_root" -> "That link points outside the repository, so it is not shown."
        "not_allowed" -> "Git's own files are not shown."
        "sensitive_file" -> "That file looks like a key or credential, so it stays on the desktop."
        "not_found" -> "The file is gone."
        "not_a_file" -> "That is a folder."
        "not_a_directory" -> "That is not a folder."
        "bad_path" -> "That path is not valid."
        "root_unavailable" -> "The repository folder is missing on the desktop."
        "git_status_failed" -> "Git could not read the working tree."
        "git_unavailable" -> "Git is not available on the desktop."
        "host_exists" -> "The desktop already has a host with that name."
        "bad_host" -> "That host name is not valid."
        "bad_address" -> "Enter an IP address or a host name."
        "bad_user" -> "Enter a valid SSH user name."
        "bad_code" -> "The link code has six digits."
        "bad_mode" -> "Pick what to deploy."
        "bad_repo", "unknown_repo" -> "The desktop does not have that repository."
        "bad_params" -> "That setting is out of range."
        "not_admin" -> "Only a platform admin can delete a node."
        "protected_node" -> "The main node cannot be deleted."
        "cannot_sign" -> "Sign in to the account on the desktop first."
        "nothing_to_resume" -> "There is no paused deployment."
        "fleet_target_off" -> "Turn on the fleet target first."
        "no_mirror_targets" -> "That repository has no mirror remotes or managed hosts."
        "host_unreachable" -> "The desktop could not reach that host."
        "not_started" -> "The desktop could not start it."
        "not_running" -> "That agent is no longer running."
        "nothing_to_send" -> "There are no stalls to send, or no checkout to run an agent in."
        "unknown_stall" -> "That stall record is gone."
        "agent_not_started" -> "The desktop could not start an agent."
        "ps_failed", "ps_unavailable" -> "The desktop could not list its processes."
        "bad_pid" -> "That process is not valid."
        "process_exited" -> "That process has already exited."
        "process_changed" -> "That process ended and its number was reused. Refresh the list."
        "protected_process" -> "The desktop does not stop that process."
        "process_stop_failed" -> "The desktop could not stop that process."
        "bad_password" -> "That password cannot be used."
        "process_details_unavailable" -> "Process details from the phone work only with a Linux desktop."
        "bad_session" -> "That agent run is not valid."
        "already_running" -> "The desktop is already updating or restarting."
        "unknown_action" -> "The desktop does not know that action."
        else -> ControlErrorText.describe(code)
    }
}

object RemoteFormat {
    fun fixed(value: Double, digits: Int): String {
        if (value.isNaN() || value.isInfinite()) return value.toString()
        val text = java.math.BigDecimal(value).setScale(digits, java.math.RoundingMode.HALF_EVEN).toPlainString()
        return if (text.startsWith("-") && text.trimStart('-').all { it == '0' || it == '.' }) text.substring(1) else text
    }

    fun bytes(value: Double): String {
        if (value < 0) return "—"
        val units = listOf("B", "KB", "MB", "GB", "TB")
        var amount = value
        var index = 0
        while (amount >= 1024 && index < units.size - 1) {
            amount /= 1024
            index++
        }
        if (index == 0) return "${amount.toLong()} B"
        return fixed(amount, if (amount >= 100) 0 else 1) + " " + units[index]
    }

    fun money(value: Double): String = "$" + fixed(value, 2)

    fun duration(ms: Long): String {
        val seconds = maxOf(0L, ms / 1000)
        if (seconds < 60) return "${seconds}s"
        val minutes = seconds / 60
        if (minutes < 60) return "${minutes}m ${seconds % 60}s"
        val hours = minutes / 60
        if (hours < 48) return "${hours}h ${minutes % 60}m"
        return "${hours / 24}d ${hours % 24}h"
    }

    fun percent(value: Double): String = if (value < 0) "—" else fixed(value, 0) + "%"
}
