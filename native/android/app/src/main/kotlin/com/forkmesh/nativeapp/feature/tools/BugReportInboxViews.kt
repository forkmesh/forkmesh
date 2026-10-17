package com.forkmesh.nativeapp.feature.tools

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
import com.forkmesh.nativeapp.api.BugReportInboxItem
import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.tools.BugReportInboxCopy
import com.forkmesh.nativeapp.tools.BugReportInboxState
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
internal fun BugReportInboxList(
    state: BugReportInboxState,
    onStatus: (String) -> Unit,
    onMore: () -> Unit,
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
                BugReportInboxRow(report)
            }
            if (state.nextOffset != null) {
                FmToneButton(BugReportInboxCopy.LOAD_MORE, onPressed = if (state.loading) null else onMore, tone = FmTone.Neutral)
            }
        }
    }
}

@Composable
private fun BugReportInboxRow(report: BugReportInboxItem) {
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
                Spacer(Modifier.width(FmSpace.x2))
                Text(
                    report.status.replaceFirstChar { it.uppercase() },
                    style = Fm.text.labelMedium.copy(color = if (report.status == "new") c.accent else c.textTertiary),
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
                Spacer(Modifier.height(2.dp))
                Text(report.attachments.joinToString(" · ") { it.name }, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
    }
}
