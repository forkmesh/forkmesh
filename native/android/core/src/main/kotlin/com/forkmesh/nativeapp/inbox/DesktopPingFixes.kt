package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str

data class PingQuickFix(
    val id: String,
    val label: String,
    val detail: String = "",
    val icon: String = "",
    val score: Double = 0.0,
    val trusted: Boolean = false,
    val primary: Boolean = false,
) {
    val materialIcon: String
        get() = when (icon) {
            "pause" -> "pause_circle_outlined"
            "play" -> "play_circle_outlined"
            "rerun" -> "refresh_rounded"
            "agent" -> "auto_awesome_outlined"
            "approve" -> "check_circle_outlined"
            "reject" -> "block_outlined"
            else -> "build_outlined"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>): PingQuickFix = PingQuickFix(
            id = json.str("id"),
            label = json.str("label"),
            detail = json.str("detail"),
            icon = json.str("icon"),
            score = json.double("score"),
            trusted = json.bool("trusted"),
            primary = json.bool("primary"),
        )
    }
}

data class PingQuickFixes(
    val found: Boolean = false,
    val fixes: List<PingQuickFix> = emptyList(),
    val event: String = "",
    val cause: String = "",
    val message: String = "",
) {
    val primary: PingQuickFix? get() = fixes.firstOrNull { it.primary } ?: fixes.firstOrNull()

    companion object {
        fun fromJson(json: Map<String, Any?>): PingQuickFixes {
            val fixes = json.maps("fixes").map(PingQuickFix::fromJson).filter { it.id.isNotEmpty() && it.label.isNotEmpty() }
            return PingQuickFixes(
                found = json.bool("found") && fixes.isNotEmpty(),
                fixes = fixes,
                event = json.str("event"),
                cause = json.str("cause"),
                message = json.str("message"),
            )
        }
    }
}

class DesktopPingFixes(channel: ControlCalling) {
    private val family = ControlFamilyChannel(DesktopControlFamilyName.Pings, channel)

    fun fixes(item: InboxItem): PingQuickFixes {
        val params = params(item) ?: return PingQuickFixes()
        return PingQuickFixes.fromJson(family.call("fixes", params, TIMEOUT_MS))
    }

    fun apply(fix: PingQuickFix, item: InboxItem): PingQuickFixes {
        val params = params(item) ?: throw LocalControlException("unknown_ping")
        params["fix"] = fix.id
        return PingQuickFixes.fromJson(family.call("fix", params, TIMEOUT_MS))
    }

    companion object {
        const val TIMEOUT_MS = 8_000L

        fun params(item: InboxItem): LinkedHashMap<String, Any?>? {
            if (!item.fromDesktop) return null
            val params = linkedMapOf<String, Any?>("title" to item.title, "body" to item.body, "ts" to item.ts)
            if (item.desktopPingId.isNotEmpty()) params["pingId"] = item.desktopPingId
            return params
        }

        fun explain(error: Throwable): String = when ((error as? LocalControlException)?.code.orEmpty()) {
            "unknown_ping" -> "Your desktop no longer has this ping."
            "fix_unavailable" -> "That fix no longer applies."
            "unknown_repo" -> "Your desktop does not have this repository."
            "unknown_run" -> "The desktop no longer has that run."
            "unknown_agent" -> "The desktop no longer has that agent."
            "wrong_state" -> "That run has already moved on."
            "not_started" -> "The desktop could not start it."
            "unsupported", "unknown_method" -> "Update the desktop to use quick fixes."
            "not_owner" -> "The desktop is signed in to another account."
            "not_paired" -> "Pair this phone with your desktop first."
            "" -> "The desktop did not answer."
            else -> {
                val code = (error as LocalControlException).code
                code.replace('_', ' ').replaceFirstChar { it.uppercase() } + "."
            }
        }
    }
}
