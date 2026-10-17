package com.forkmesh.nativeapp.feature.tools

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.BugReportInboxAttachment
import com.forkmesh.nativeapp.api.BugReportInboxItem
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.tools.BugReportInboxCopy
import com.forkmesh.nativeapp.tools.BugReportInboxState
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

private const val ATTACHMENT_DIR = "bug-report-attachments"

internal suspend fun openBugReportAttachment(context: Context, reportId: String, attachment: BugReportInboxAttachment): String {
    val api = Services.get<ApiService>() ?: return BugReportInboxCopy.ATTACHMENTS_UNAVAILABLE
    val file = withContext(Dispatchers.IO) { api.bugReportAttachment(reportId, attachment.id) }
    if (!file.ok) return file.message
    val target = withContext(Dispatchers.IO) {
        runCatching {
            val folder = File(File(context.cacheDir, ATTACHMENT_DIR), attachment.id).apply { mkdirs() }
            File(folder, BugReportInboxCopy.fileName(attachment.name)).apply { writeBytes(file.data) }
        }.getOrNull()
    } ?: return BugReportInboxCopy.SAVE_FAILED
    val mime = file.contentType.substringBefore(';').trim().lowercase()
        .ifEmpty { attachment.contentType }.ifEmpty { "application/octet-stream" }
    return try {
        val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), target)
        val view = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, mime)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        view.clipData = ClipData.newRawUri(target.name, uri)
        context.startActivity(Intent.createChooser(view, attachment.name).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
        ""
    } catch (_: ActivityNotFoundException) {
        BugReportInboxCopy.NO_VIEWER
    } catch (_: IllegalArgumentException) {
        BugReportInboxCopy.SAVE_FAILED
    }
}

@Composable
internal fun BugReportInboxList(
    state: BugReportInboxState,
    onStatus: (String) -> Unit,
    onMore: () -> Unit,
    onSetStatus: (String, String) -> Unit,
    onOpen: (String, BugReportInboxAttachment) -> Unit,
    modifier: Modifier = Modifier,
) {
    val c = Fm.colors
    Column(modifier) {
        Row(
            Modifier
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            for (filter in BugReportInboxCopy.FILTERS) {
                val selected = filter.value == state.status
                FmToneButton(
                    state.label(filter),
                    onPressed = { onStatus(filter.value) },
                    tone = if (selected) FmTone.Accent else FmTone.Neutral,
                    filled = selected,
                )
            }
        }
        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            if (state.loading) {
                FmInlineStatusLine("Loading…", isError = false)
            }
            if (state.problem.isNotEmpty()) {
                FmInlineStatusLine(state.problem, isError = true)
            } else if (state.loaded && state.reports.isEmpty()) {
                Text(BugReportInboxCopy.EMPTY, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            }
            for (report in state.reports) {
                BugReportInboxRow(report, state, onSetStatus, onOpen)
            }
            if (state.nextOffset != null) {
                FmToneButton(BugReportInboxCopy.LOAD_MORE, onPressed = if (state.loading) null else onMore, tone = FmTone.Neutral)
            }
        }
    }
}

@Composable
private fun BugReportInboxRow(
    report: BugReportInboxItem,
    state: BugReportInboxState,
    onSetStatus: (String, String) -> Unit,
    onOpen: (String, BugReportInboxAttachment) -> Unit,
) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth()) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    BugReportInboxCopy.severityLabel(report.severity),
                    style = Fm.text.labelMedium.copy(
                        color = if (report.severity == "crash" || report.severity == "high") c.danger else c.textSecondary,
                    ),
                )
                Spacer(Modifier.weight(1f))
                Text(relativeTimeFrom(report.createdAt), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            Spacer(Modifier.height(FmSpace.x2))
            SelectionContainer {
                Text(report.message.ifEmpty { "(no message)" }, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            }
            Spacer(Modifier.height(FmSpace.x2))
            Text(BugReportInboxCopy.details(report), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            if (report.contact.isNotEmpty()) {
                Spacer(Modifier.height(2.dp))
                SelectionContainer {
                    Text("Reply to ${report.contact}", style = Fm.text.bodySmall.copy(color = c.textSecondary))
                }
            }
            if (report.attachments.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x2))
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    for (attachment in report.attachments) {
                        val opening = attachment.id in state.downloading
                        FmToneButton(
                            if (opening) BugReportInboxCopy.OPENING else "${attachment.name} · ${BugReportInboxCopy.sizeLabel(attachment.size)}",
                            onPressed = if (opening) null else ({ onOpen(report.id, attachment) }),
                            tone = FmTone.Neutral,
                        )
                    }
                }
            }
            Spacer(Modifier.height(FmSpace.x2))
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (choice in BugReportInboxCopy.STATUSES) {
                    val selected = choice.value == report.status
                    FmToneButton(
                        choice.label,
                        onPressed = if (report.id in state.updating) null else ({ onSetStatus(report.id, choice.value) }),
                        tone = if (selected) FmTone.Accent else FmTone.Neutral,
                        filled = selected,
                    )
                }
            }
        }
    }
}
