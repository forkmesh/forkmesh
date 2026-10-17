package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.BugReportAttachment
import com.forkmesh.nativeapp.api.BugReportInbox
import com.forkmesh.nativeapp.api.BugReportInboxItem
import com.forkmesh.nativeapp.api.BugReportStatusChange
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

fun sendBugReport(api: ApiService, draft: ReportBugDraft, screen: String, build: AppBuild, packageId: String? = null, attachments: List<BugReportAttachment> = emptyList()): BugReportResult =
    api.submitBugReport(
        message = draft.message.trim(),
        contact = draft.contact,
        screen = screen,
        severity = draft.severity,
        platform = build.platform,
        appVersion = build.version,
        osVersion = build.osVersion,
        packageId = packageId,
        attachments = attachments,
    )

data class BugReportStatusFilter(val value: String, val label: String)

object BugReportInboxCopy {
    val FILTERS = listOf(
        BugReportStatusFilter("new", "New"),
        BugReportStatusFilter("triaged", "Triaged"),
        BugReportStatusFilter("closed", "Closed"),
        BugReportStatusFilter("", "All"),
    )

    const val TAB_REPORT = "Report"
    const val TAB_REPORTED = "Reported"
    const val EMPTY = "No reported bugs here."
    const val LOAD_MORE = "Load more"
    const val UNAVAILABLE = "Reported bugs are unavailable in this build."
    const val OPENING = "Opening…"
    const val ATTACHMENTS_UNAVAILABLE = "Attachments are unavailable in this build."
    const val SAVE_FAILED = "The attachment could not be saved on this device."
    const val NO_VIEWER = "No app on this phone can open that attachment."

    val STATUSES = FILTERS.filter { it.value.isNotEmpty() }

    fun sizeLabel(bytes: Long): String = when {
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> "${Math.round(bytes / 1024.0)} KB"
        else -> String.format(java.util.Locale.ROOT, "%.1f MB", bytes / 1024.0 / 1024.0)
    }

    fun fileName(name: String): String {
        val text = name.map { if (it in "/\\:\u0000") '_' else it }.joinToString("").trim()
        return if (text.isEmpty() || text == "." || text == "..") "attachment" else text.take(160)
    }

    fun severityLabel(value: String): String =
        ReportBugCopy.SEVERITIES.firstOrNull { it.value == value }?.label ?: value.replaceFirstChar { it.uppercase() }

    fun details(report: BugReportInboxItem): String {
        val parts = mutableListOf(if (report.account.isEmpty()) "anonymous" else "@${report.account}")
        listOf(report.platform, report.osVersion).filter { it.isNotEmpty() }.joinToString(" ").takeIf { it.isNotEmpty() }?.let(parts::add)
        for (value in listOf(report.appVersion, report.screen, report.packageId, report.device)) {
            if (value.isNotEmpty()) parts.add(value)
        }
        return parts.joinToString(" · ")
    }
}

data class BugReportInboxState(
    val status: String = "new",
    val reports: List<BugReportInboxItem> = emptyList(),
    val counts: Map<String, Int> = emptyMap(),
    val nextOffset: Int? = null,
    val problem: String = "",
    val loading: Boolean = false,
    val loaded: Boolean = false,
    val updating: Set<String> = emptySet(),
    val downloading: Set<String> = emptySet(),
) {
    fun canSet(id: String, value: String): Boolean =
        id !in updating && reports.any { it.id == id && it.status != value }

    fun updatingStarted(id: String): BugReportInboxState = copy(updating = updating + id)

    fun statusChanged(id: String, change: BugReportStatusChange): BugReportInboxState {
        val done = copy(updating = updating - id)
        if (!change.ok) return done.copy(problem = change.message)
        if (status.isNotEmpty() && change.status != status) {
            return done.copy(
                problem = "",
                counts = change.counts,
                reports = reports.filterNot { it.id == id },
                nextOffset = nextOffset?.let { maxOf(it - 1, 0) },
            )
        }
        return done.copy(
            problem = "",
            counts = change.counts,
            reports = reports.map { if (it.id == id) it.copy(status = change.status) else it },
        )
    }

    fun downloadStarted(id: String): BugReportInboxState = copy(downloading = downloading + id)

    fun downloadFinished(id: String, problem: String = ""): BugReportInboxState =
        copy(downloading = downloading - id, problem = problem)

    fun label(filter: BugReportStatusFilter): String {
        if (filter.value.isEmpty()) return if (counts.isEmpty()) filter.label else "${filter.label} ${counts.values.sum()}"
        val count = counts[filter.value] ?: return filter.label
        return "${filter.label} $count"
    }

    fun requesting(status: String = this.status): BugReportInboxState = copy(status = status, loading = true)

    fun received(inbox: BugReportInbox, offset: Int): BugReportInboxState {
        if (!inbox.ok) {
            return copy(loading = false, loaded = true, problem = inbox.message,
                reports = if (offset == 0) emptyList() else reports, nextOffset = null)
        }
        return copy(
            loading = false,
            loaded = true,
            problem = "",
            counts = inbox.counts,
            reports = if (offset == 0) inbox.reports else reports + inbox.reports,
            nextOffset = inbox.nextOffset,
        )
    }
}

fun loadBugReportInbox(api: ApiService?, status: String, offset: Int = 0): BugReportInbox =
    api?.bugReportInbox(status, offset) ?: BugReportInbox.failure(BugReportInboxCopy.UNAVAILABLE)
