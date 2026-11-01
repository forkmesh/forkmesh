package com.forkmesh.nativeapp.feature.tools

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import com.forkmesh.nativeapp.api.MyBugReport
import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.tools.MyBugReportTone
import com.forkmesh.nativeapp.tools.MyBugReportsCopy
import com.forkmesh.nativeapp.tools.MyBugReportsState
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
internal fun MyBugReportsList(
    state: MyBugReportsState,
    onRefresh: () -> Unit,
    onMore: () -> Unit,
    onConfirm: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val c = Fm.colors
    Column(modifier.testTag("bug-mine")) {
        Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
            FmToneButton(MyBugReportsCopy.REFRESH, onPressed = if (state.loading) null else onRefresh, tone = FmTone.Neutral)
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
                Text(MyBugReportsCopy.EMPTY, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            }
            for (report in state.reports) {
                MyBugReportRow(report, state, onConfirm)
            }
            if (state.nextOffset != null) {
                FmToneButton(MyBugReportsCopy.LOAD_MORE, onPressed = if (state.loading) null else onMore, tone = FmTone.Neutral)
            }
        }
    }
}

@Composable
private fun MyBugReportRow(report: MyBugReport, state: MyBugReportsState, onConfirm: (String) -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth().testTag("bug-mine-report-${report.id}")) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    MyBugReportsCopy.statusLabel(report),
                    style = Fm.text.labelMedium.copy(
                        color = when (MyBugReportsCopy.statusTone(report)) {
                            MyBugReportTone.Confirmed -> c.success
                            MyBugReportTone.InProgress -> c.accent
                            MyBugReportTone.Plain -> c.textSecondary
                        },
                    ),
                )
                Spacer(Modifier.weight(1f))
                Text(relativeTimeFrom(report.createdAt), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            Spacer(Modifier.height(FmSpace.x2))
            SelectionContainer {
                Text(report.message.ifEmpty { "(no message)" }, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            }
            val details = MyBugReportsCopy.details(report)
            if (details.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x2))
                Text(details, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            if (report.attachments.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x2))
                Text(MyBugReportsCopy.attachments(report), style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
            if (!report.confirmedFixed) {
                Spacer(Modifier.height(FmSpace.x2))
                val confirming = report.id in state.confirming
                FmToneButton(
                    if (confirming) MyBugReportsCopy.CONFIRMING else MyBugReportsCopy.CONFIRM,
                    onPressed = if (confirming) null else ({ onConfirm(report.id) }),
                    modifier = Modifier.testTag("bug-mine-confirm-${report.id}"),
                    tone = FmTone.Accent,
                    busy = confirming,
                )
            }
        }
    }
}
