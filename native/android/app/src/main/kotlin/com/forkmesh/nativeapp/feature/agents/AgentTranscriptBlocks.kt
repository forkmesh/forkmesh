package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.control.AgentTranscriptFormat
import com.forkmesh.nativeapp.control.TranscriptAlign
import com.forkmesh.nativeapp.control.TranscriptBlock
import com.forkmesh.nativeapp.control.TranscriptInlineStyle
import com.forkmesh.nativeapp.control.TranscriptJson
import com.forkmesh.nativeapp.control.TranscriptTaskTone
import com.forkmesh.nativeapp.control.TranscriptTool
import com.forkmesh.nativeapp.ui.kit.LocalReferenceRepo
import com.forkmesh.nativeapp.ui.kit.appendLink
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

private const val TOOL_RUN_OPEN = 3
private const val JSON_PREVIEW_LINES = 8
private const val JSON_FOLD_LINES = 12

internal fun LazyListScope.transcriptBlocks(blocks: List<TranscriptBlock>, repo: RepoRef? = null) {
    items(blocks.size, contentType = { blocks[it]::class.simpleName }) { index ->
        CompositionLocalProvider(LocalReferenceRepo provides repo) {
            Box(Modifier.fillMaxWidth().padding(bottom = FmSpace.x2)) { TranscriptBlockView(blocks[index]) }
        }
    }
}

@Composable
internal fun TranscriptBlockView(block: TranscriptBlock) {
    val c = Fm.colors
    when (block) {
        is TranscriptBlock.Status -> Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            for (line in block.lines) {
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(4.dp).background(c.textTertiary, CircleShape))
                    Text(line, style = Fm.text.bodySmall.copy(color = c.textTertiary), maxLines = 3, overflow = TextOverflow.Ellipsis)
                }
            }
        }
        is TranscriptBlock.Error -> Row(
            Modifier.fillMaxWidth()
                .background(c.danger.copy(alpha = 0.08f), RoundedCornerShape(FmRadius.md))
                .padding(horizontal = 10.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            FmIcon("error_outline", size = 14.dp, tint = c.danger)
            SelectionContainer { Text(block.text, style = Fm.text.bodySmall.copy(color = c.danger)) }
        }
        is TranscriptBlock.Prompt -> YouBubble("You", block.text)
        is TranscriptBlock.Steer ->
            if (block.fromYou) YouBubble("You · ${block.from}", block.text) else PeerBubble(block.from, block.text)
        is TranscriptBlock.Tools -> ToolRun(block.calls)
        is TranscriptBlock.Heading -> Text(
            rememberInline(block.text),
            style = when {
                block.level <= 1 -> Fm.text.titleMedium.copy(color = c.textPrimary)
                block.level == 2 -> Fm.text.titleSmall.copy(color = c.textPrimary)
                else -> Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis)
            },
            modifier = Modifier.padding(top = 2.dp),
        )
        is TranscriptBlock.Bullet -> Row(
            Modifier.padding(start = (block.depth * 14).dp),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Text(block.marker, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            SelectionContainer { Text(rememberInline(block.text), style = Fm.text.bodyMedium.copy(color = c.textPrimary)) }
        }
        is TranscriptBlock.Code -> CodeBox(block.language, block.text)
        is TranscriptBlock.Json -> JsonBox(block.text)
        is TranscriptBlock.Table -> TableBox(block)
        is TranscriptBlock.Task -> TaskCard(block)
        TranscriptBlock.Rule -> HorizontalDivider(color = c.border)
        is TranscriptBlock.Text -> SelectionContainer {
            Text(rememberInline(block.text), style = Fm.text.bodyMedium.copy(color = c.textPrimary))
        }
    }
}

@Composable
private fun YouBubble(label: String, text: String) {
    val c = Fm.colors
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterEnd) {
        Column(
            Modifier.padding(start = FmSpace.x6)
                .background(c.accentSubtle, RoundedCornerShape(FmRadius.lg))
                .padding(horizontal = 10.dp, vertical = FmSpace.x2),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(label, style = Fm.text.labelSmall.copy(color = c.accent, fontWeight = FmType.emphasis))
            SelectionContainer { Text(rememberInline(text), style = Fm.text.bodyMedium.copy(color = c.textPrimary)) }
        }
    }
}

@Composable
private fun PeerBubble(from: String, text: String) {
    val c = Fm.colors
    Column(
        Modifier.padding(end = FmSpace.x6)
            .background(c.bgOverlay, RoundedCornerShape(FmRadius.lg))
            .padding(horizontal = 10.dp, vertical = FmSpace.x2),
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
            FmIcon("forum_outlined", size = 12.dp, tint = c.textSecondary)
            Text("From $from", style = Fm.text.labelSmall.copy(color = c.textSecondary, fontWeight = FmType.emphasis))
        }
        SelectionContainer { Text(rememberInline(text), style = Fm.text.bodyMedium.copy(color = c.textPrimary)) }
    }
}

@Composable
private fun ToolRun(calls: List<TranscriptTool>) {
    val c = Fm.colors
    var open by rememberSaveable { mutableStateOf(false) }
    val folds = calls.size > TOOL_RUN_OPEN
    val shape = RoundedCornerShape(FmRadius.md)
    val line = c.border.copy(alpha = 0.5f)
    Column(
        Modifier.fillMaxWidth()
            .clip(shape)
            .background(c.bgOverlay.copy(alpha = 0.55f))
            .border(0.5.dp, line, shape),
    ) {
        if (folds) {
            val summary = remember(calls) {
                calls.groupingBy { it.name }.eachCount().entries.sortedByDescending { it.value }.take(3)
                    .joinToString(" · ") { if (it.value > 1) "${it.key} ×${it.value}" else it.key }
            }
            Row(
                Modifier.fillMaxWidth().clickable { open = !open }.padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon(if (open) "expand_less" else "expand_more", size = 14.dp, tint = c.textTertiary)
                Text(
                    "${calls.size} tool calls · $summary",
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            HorizontalDivider(thickness = 0.5.dp, color = line)
        }
        val shown = if (!folds || open) calls else calls.takeLast(1)
        shown.forEachIndexed { index, call ->
            if (index > 0) HorizontalDivider(Modifier.padding(start = 30.dp), thickness = 0.5.dp, color = line)
            ToolChip(call)
        }
    }
}

private fun toolIcon(name: String): String {
    val lower = name.lowercase()
    if (lower.startsWith("mcp__")) return "extension_outlined"
    return when (lower) {
        "bash", "shell", "exec", "exec_command", "local_shell", "bashoutput", "killshell" -> "terminal_rounded"
        "read", "view", "notebookread" -> "description_outlined"
        "edit", "multiedit", "write", "apply_patch", "notebookedit" -> "edit_outlined"
        "grep", "glob", "search", "ls" -> "search"
        "webfetch", "websearch", "web_search" -> "language"
        "agent", "task" -> "smart_toy_outlined"
        "todowrite", "update_plan" -> "task_alt"
        else -> "build_outlined"
    }
}

@Composable
private fun ToolChip(call: TranscriptTool) {
    val c = Fm.colors
    var open by rememberSaveable(call.name, call.detail) { mutableStateOf(false) }
    val multiLine = '\n' in call.detail
    val oneLine = remember(call.detail) {
        call.detail.lines().map { it.trim() }.filter { it.isNotEmpty() }.joinToString(" ⏎ ")
    }
    Column(
        Modifier.fillMaxWidth()
            .clickable(enabled = call.detail.isNotEmpty()) { open = !open }
            .padding(horizontal = FmSpace.x2, vertical = 6.dp),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(16.dp), contentAlignment = Alignment.Center) {
                FmIcon(toolIcon(call.name), size = 13.dp, tint = c.textTertiary)
            }
            Text(
                call.name,
                style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, fontWeight = FmType.emphasis, color = c.textSecondary),
                maxLines = 1,
            )
            Text(
                if (open) "" else oneLine,
                style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textTertiary),
                maxLines = 1,
                overflow = if (multiLine) TextOverflow.Ellipsis else TextOverflow.MiddleEllipsis,
                modifier = Modifier.weight(1f),
            )
            if (multiLine || open) FmIcon(if (open) "expand_less" else "expand_more", size = 14.dp, tint = c.textTertiary)
        }
        if (open) {
            val detail = remember(call.detail) { TranscriptJson.pretty(call.detail) ?: call.detail }
            SelectionContainer(Modifier.padding(start = 22.dp, top = 4.dp)) {
                Text(detail, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textSecondary))
            }
        }
    }
}

@Composable
private fun CodeBox(language: String, text: String) {
    val c = Fm.colors
    Column(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay),
    ) {
        if (language.isNotEmpty()) {
            Text(
                language,
                style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textTertiary),
                modifier = Modifier.padding(start = 10.dp, top = 6.dp, end = 10.dp),
            )
        }
        Box(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 10.dp, vertical = FmSpace.x2)) {
            SelectionContainer { Text(text, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textPrimary), softWrap = false) }
        }
    }
}

@Composable
private fun JsonBox(text: String) {
    val c = Fm.colors
    val lines = remember(text) { text.lines() }
    val folds = lines.size > JSON_FOLD_LINES
    var open by rememberSaveable(text) { mutableStateOf(false) }
    Column(
        Modifier.fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay),
    ) {
        Row(
            Modifier.fillMaxWidth()
                .clickable(enabled = folds) { open = !open }
                .padding(start = 10.dp, top = 6.dp, end = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("JSON", style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textTertiary), modifier = Modifier.weight(1f))
            if (folds) {
                Text(
                    if (open) "Collapse" else "Show all ${lines.size} lines",
                    style = Fm.text.labelSmall.copy(color = c.accent),
                )
                FmIcon(if (open) "expand_less" else "expand_more", size = 14.dp, tint = c.accent)
            }
        }
        val shown = if (!folds || open) text else lines.take(JSON_PREVIEW_LINES).joinToString("\n") + "\n  …"
        Box(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 10.dp, vertical = FmSpace.x2)) {
            SelectionContainer { Text(shown, style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textPrimary), softWrap = false) }
        }
    }
}

@Composable
private fun TableBox(table: TranscriptBlock.Table) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Box(
        Modifier.clip(shape)
            .border(1.dp, c.border, shape)
            .horizontalScroll(rememberScrollState()),
    ) {
        TranscriptGrid(table.header.size) {
            for ((column, cell) in table.header.withIndex()) {
                TableCell(cell, table.align.getOrElse(column) { TranscriptAlign.Start }, header = true)
            }
            for (row in table.rows) {
                for ((column, cell) in row.withIndex()) {
                    TableCell(cell, table.align.getOrElse(column) { TranscriptAlign.Start }, header = false)
                }
            }
        }
    }
}

@Composable
private fun TableCell(text: String, align: TranscriptAlign, header: Boolean) {
    val c = Fm.colors
    Box(
        Modifier.background(if (header) c.bgOverlay else Color.Transparent)
            .border(0.5.dp, c.border)
            .padding(horizontal = 10.dp, vertical = 6.dp),
        contentAlignment = when (align) {
            TranscriptAlign.Start -> Alignment.TopStart
            TranscriptAlign.Center -> Alignment.TopCenter
            TranscriptAlign.End -> Alignment.TopEnd
        },
    ) {
        Text(
            rememberInline(text),
            style = Fm.text.bodySmall.copy(
                color = c.textPrimary,
                fontWeight = if (header) FmType.emphasis else FmType.regular,
            ),
        )
    }
}

@Composable
private fun TranscriptGrid(columns: Int, content: @Composable () -> Unit) {
    val widest = with(LocalDensity.current) { 260.dp.roundToPx() }
    Layout(content) { measurables, _ ->
        if (columns <= 0 || measurables.isEmpty()) return@Layout layout(0, 0) {}
        val rows = (measurables.size + columns - 1) / columns
        val widths = IntArray(columns)
        measurables.forEachIndexed { index, cell ->
            val column = index % columns
            widths[column] = maxOf(widths[column], minOf(widest, cell.maxIntrinsicWidth(Constraints.Infinity)))
        }
        val heights = IntArray(rows)
        measurables.forEachIndexed { index, cell ->
            val row = index / columns
            heights[row] = maxOf(heights[row], cell.minIntrinsicHeight(widths[index % columns]))
        }
        val placed = measurables.mapIndexed { index, cell ->
            cell.measure(Constraints.fixed(widths[index % columns], heights[index / columns]))
        }
        layout(widths.sum(), heights.sum()) {
            var y = 0
            for (row in 0 until rows) {
                var x = 0
                for (column in 0 until columns) {
                    placed.getOrNull(row * columns + column)?.place(x, y)
                    x += widths[column]
                }
                y += heights[row]
            }
        }
    }
}

@Composable
private fun TaskCard(task: TranscriptBlock.Task) {
    val c = Fm.colors
    val tone = when (task.tone) {
        TranscriptTaskTone.Done -> c.success
        TranscriptTaskTone.Failed -> c.danger
        TranscriptTaskTone.Running -> c.warning
    }
    val icon = when (task.tone) {
        TranscriptTaskTone.Done -> "check_circle_outline"
        TranscriptTaskTone.Failed -> "error_outline"
        TranscriptTaskTone.Running -> "timelapse"
    }
    val shape = RoundedCornerShape(FmRadius.lg)
    var open by rememberSaveable(task.summary) { mutableStateOf(false) }
    Column(
        Modifier.fillMaxWidth()
            .clip(shape)
            .background(tone.copy(alpha = 0.07f))
            .border(1.dp, tone.copy(alpha = 0.35f), shape)
            .padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmIcon(icon, size = 18.dp, tint = tone)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    "Task " + task.status.replace('_', ' '),
                    style = Fm.text.labelSmall.copy(color = tone, fontWeight = FmType.emphasis),
                )
                SelectionContainer {
                    Text(rememberInline(task.summary), style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                }
            }
        }
        if (task.body.isNotEmpty()) {
            Row(
                Modifier.clip(RoundedCornerShape(FmRadius.md)).clickable { open = !open }.padding(vertical = 2.dp),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon(if (open) "expand_less" else "expand_more", size = 16.dp, tint = c.accent)
                Text(if (open) "Hide result" else "Show result", style = Fm.text.labelMedium.copy(color = c.accent))
            }
            if (open) {
                val body = remember(task.body) { AgentTranscriptFormat.blocks(task.body) }
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    for (inner in body) TranscriptBlockView(inner)
                }
            }
        }
    }
}

@Composable
private fun rememberInline(text: String): AnnotatedString {
    val c = Fm.colors
    val repo = LocalReferenceRepo.current
    val open = rememberLinkOpener()
    return remember(text, c.accent, c.bgOverlay, repo, open) {
        buildAnnotatedString {
            for (span in AgentTranscriptFormat.inline(text)) {
                when (span.style) {
                    TranscriptInlineStyle.Plain -> for (run in ReferenceLinks.runs(span.text, repo, commits = repo != null)) {
                        val url = run.url
                        if (url == null) append(run.text) else appendLink(run.text, url, c.accent, open)
                    }
                    TranscriptInlineStyle.Bold -> withStyle(SpanStyle(fontWeight = FmType.emphasis)) { append(span.text) }
                    TranscriptInlineStyle.Italic -> withStyle(SpanStyle(fontStyle = FontStyle.Italic)) { append(span.text) }
                    TranscriptInlineStyle.Code -> withStyle(SpanStyle(fontFamily = FmType.mono, background = c.bgOverlay)) { append(span.text) }
                    TranscriptInlineStyle.Link -> withLink(LinkAnnotation.Url(span.url, TextLinkStyles(style = SpanStyle(color = c.accent)))) {
                        append(span.text)
                    }
                }
            }
        }
    }
}

@Composable
internal fun AgentBranchDiffRow(branch: String, base: String, onClick: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth()
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .clickable(onClick = onClick)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("difference_outlined", size = 18.dp, tint = c.accent)
        Column(Modifier.weight(1f)) {
            Text("View branch diff", style = Fm.text.labelMedium.copy(color = c.accent))
            if (branch.isNotEmpty()) {
                Text(
                    if (base.isEmpty()) branch else "$branch → $base",
                    style = Fm.text.bodySmall.copy(fontFamily = FmType.mono, color = c.textSecondary),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
    }
}
