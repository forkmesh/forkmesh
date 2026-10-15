package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.BugReportResult
import kotlin.math.max

data class BugButtonPoint(val x: Float, val y: Float)

data class BugButtonPlacement(val anchor: Double, val onLeft: Boolean)

class BugButtonGeometry(
    val layerWidth: Float,
    val layerHeight: Float,
    val safeTop: Float = 0f,
    val safeBottom: Float = 0f,
    val size: Float = SIZE_DP,
    val margin: Float = MARGIN_DP,
) {
    val travel: Float
        get() {
            val usable = layerHeight - safeTop - safeBottom - size - margin * 2
            return if (usable > 1) usable else 1f
        }

    fun restingAt(anchor: Double, onLeft: Boolean): BugButtonPoint = BugButtonPoint(
        x = if (onLeft) margin else layerWidth - size - margin,
        y = safeTop + margin + (anchor * travel).toFloat(),
    )

    fun dragged(from: BugButtonPoint, dx: Float, dy: Float): BugButtonPoint = BugButtonPoint(
        x = (from.x + dx).coerceIn(margin, max(margin, layerWidth - size - margin)),
        y = (from.y + dy).coerceIn(safeTop + margin, safeTop + margin + travel),
    )

    fun released(at: BugButtonPoint): BugButtonPlacement = BugButtonPlacement(
        anchor = ((at.y - safeTop - margin) / travel).toDouble().coerceIn(0.0, 1.0),
        onLeft = at.x + size / 2 < layerWidth / 2,
    )

    companion object {
        const val SIZE_DP = 42f

        const val MARGIN_DP = 12f

        const val IDLE_OPACITY = 0.62f
    }
}

data class BugSeverity(val value: String, val label: String)

object ReportBugCopy {
    val SEVERITIES = listOf(
        BugSeverity("low", "Minor"),
        BugSeverity("normal", "Normal"),
        BugSeverity("high", "Serious"),
        BugSeverity("crash", "Crash"),
    )

    const val DEFAULT_SEVERITY = "normal"
    const val TITLE = "Report a bug"
    const val SUBTITLE = "Goes straight to the ForkMesh team."
    const val MESSAGE_HINT = "What happened, and what did you expect instead?"
    const val SEVERITY_LABEL = "How bad is it?"
    const val EMPTY_PROBLEM = "Describe what went wrong."
    const val UNAVAILABLE_PROBLEM = "Reporting is unavailable in this build."
    const val SEND = "Send report"
    const val SENDING = "Sending…"
    const val CLOSE = "Close"
    const val THANKS = "Thanks — your report reached the ForkMesh team."
    const val BUTTON_LABEL = "Report a bug"
    const val BUTTON_HINT = "Double tap to describe a problem. Drag to move this button."

    const val FEEDBACK_HINT = "What works, what is missing, or what went wrong?"

    fun title(packageName: String?): String = packageName?.let { "Feedback on $it" } ?: TITLE

    fun subtitle(packageName: String?): String = packageName?.let { "Goes to the ForkMesh team, tagged with $it." } ?: SUBTITLE

    fun messageHint(packageName: String?): String = if (packageName != null) FEEDBACK_HINT else MESSAGE_HINT

    fun contactLabel(signedIn: Boolean): String = if (signedIn) "Reply to (optional)" else "Email or handle (optional)"

    fun contactHint(signedIn: Boolean): String =
        if (signedIn) "Only if you want the reply somewhere else" else "Only if you want a reply"

    fun attached(build: AppBuild, screen: String, signedIn: Boolean): String {
        val parts = mutableListOf("ForkMesh ${build.version}", build.platform)
        if (screen.isNotEmpty()) parts.add(screen)
        parts.add(if (signedIn) "your account" else "no account")
        return "Attached: ${parts.joinToString(" · ")}."
    }
}

data class ReportBugStep(val draft: ReportBugDraft, val send: Boolean)

data class ReportBugDraft(
    val message: String = "",
    val contact: String = "",
    val severity: String = ReportBugCopy.DEFAULT_SEVERITY,
    val problem: String = "",
    val busy: Boolean = false,
) {
    fun withMessage(text: String): ReportBugDraft =
        copy(message = text, problem = if (problem.isNotEmpty() && text.trim().isNotEmpty()) "" else problem)

    fun withContact(text: String): ReportBugDraft = copy(contact = text)

    fun withSeverity(value: String): ReportBugDraft = copy(severity = value)

    fun submit(apiAvailable: Boolean): ReportBugStep {
        if (busy) return ReportBugStep(this, send = false)
        if (message.trim().isEmpty()) return ReportBugStep(copy(problem = ReportBugCopy.EMPTY_PROBLEM), send = false)
        if (!apiAvailable) {
            return ReportBugStep(copy(busy = false, problem = ReportBugCopy.UNAVAILABLE_PROBLEM), send = false)
        }
        return ReportBugStep(copy(busy = true, problem = ""), send = true)
    }

    fun refused(message: String): ReportBugDraft = copy(busy = false, problem = message)
}

fun sendBugReport(api: ApiService, draft: ReportBugDraft, screen: String, build: AppBuild, packageId: String? = null): BugReportResult =
    api.submitBugReport(
        message = draft.message.trim(),
        contact = draft.contact,
        screen = screen,
        severity = draft.severity,
        platform = build.platform,
        appVersion = build.version,
        osVersion = build.osVersion,
        packageId = packageId,
    )
