package com.forkmesh.nativeapp.feature.code

import android.os.SystemClock
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.platform.testTag
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListLayoutInfo
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.DiffFileFilter
import com.forkmesh.nativeapp.code.DiffHealthMetric
import com.forkmesh.nativeapp.code.DiffHealthTone
import com.forkmesh.nativeapp.code.DiffLineFlag
import com.forkmesh.nativeapp.code.DiffLineMark
import com.forkmesh.nativeapp.code.DiffReviewModel
import com.forkmesh.nativeapp.code.DiffReviewRow
import com.forkmesh.nativeapp.code.DiffReviewText
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind
import com.forkmesh.nativeapp.code.diff.DiffSplitRow
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmChangeBadge
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmPalette
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch

private val findColor = Color(0xFFE3B341).copy(alpha = .45f)
private val findCurrentColor = Color(0xFFF78166).copy(alpha = .65f)

private class StreamMetrics(val fontSize: TextUnit, val charWidth: Dp, val numberWidth: Dp, val longest: Int) {
    val gutterPad = 4.dp
    val linePad = 1.dp
    val signWidth: Dp get() = charWidth * 1.6f
    val unifiedWidth: Dp get() = numberWidth * 2 + gutterPad * 3 + signWidth + charWidth * (longest + 2) + FmSpace.x3
    val sideWidth: Dp get() = numberWidth + gutterPad * 3 + charWidth * (longest + 2) + FmSpace.x2
}

private class Segment(val position: Int, val header: Boolean, val bodyStart: Int, val bodyEnd: Int)

private fun segmentsOf(rows: List<DiffReviewRow>): List<Segment> {
    val out = ArrayList<Segment>()
    var i = 0
    while (i < rows.size) {
        val row = rows[i]
        if (row is DiffReviewRow.Section) {
            out.add(Segment(i, false, i + 1, i + 1))
            i++
            continue
        }
        val start = i + 1
        var end = start
        while (end < rows.size && rows[end] !is DiffReviewRow.Header && rows[end] !is DiffReviewRow.Section) end++
        out.add(Segment(i, row is DiffReviewRow.Header, start, end))
        i = end
    }
    return out
}

@OptIn(ExperimentalFoundationApi::class, ExperimentalMaterial3Api::class)
@Composable
internal fun DiffReviewStream(
    model: DiffReviewModel,
    extra: List<DiffMenuAction> = emptyList(),
    emptyMessage: String = "No files changed.",
    onRefresh: (() -> Unit)? = null,
    fileActions: (DiffFile) -> List<DiffMenuAction> = { emptyList() },
    header: (@Composable () -> Unit)? = null,
    comments: BranchComments? = null,
    status: FmStatus? = null,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val scope = rememberCoroutineScope()
    val ownStatus = rememberFmStatus()
    val notice = status ?: ownStatus
    val copy = rememberCopyText(notice)
    val tick = remember(model) { mutableIntStateOf(0) }
    DisposableEffect(model) {
        model.onChange = { tick.intValue++ }
        onDispose { model.onChange = null }
    }
    remember { DiffViewSettings.attach(Services.get<PrefStore>()) }
    LaunchedEffect(model, DiffViewSettings.split) { model.split = DiffViewSettings.split }
    val listState = rememberLazyListState()
    val highlights = remember(model) { HashMap<Int, DiffFileHighlights>() }
    var finding by remember { mutableStateOf(false) }
    var showFiles by remember { mutableStateOf(false) }
    var metric by remember { mutableStateOf<DiffHealthMetric?>(null) }
    val suppress = remember { mutableStateOf(false) }
    tick.intValue.let { }

    val headerPx = with(LocalDensity.current) { 38.dp.roundToPx() }
    val ratchetMutedUntil = remember { longArrayOf(0L) }

    fun muteRatchet() {
        ratchetMutedUntil[0] = SystemClock.uptimeMillis() + 350
    }

    fun jumpTo(position: Int?) {
        if (position == null) return
        haptics.selection()
        muteRatchet()
        suppress.value = true
        val offset = if (model.rows.getOrNull(position) is DiffReviewRow.Header) 0 else -headerPx
        scope.launch { listState.scrollToItem(position + 1, offset) }
    }

    fun topPosition(): Int = listState.firstVisibleItemIndex - 1

    fun toggleViewed(file: Int) {
        haptics.selection()
        muteRatchet()
        model.toggleViewed(file)?.let { next -> jumpTo(model.position(DiffReviewRow.headerId(next))) }
    }

    LaunchedEffect(model, listState) {
        var lastTop = topPosition()
        snapshotFlow { listState.firstVisibleItemIndex to listState.firstVisibleItemScrollOffset }
            .drop(1)
            .collectLatest { (index, _) ->
                val from = lastTop
                lastTop = index - 1
                if (suppress.value) {
                    suppress.value = false
                    return@collectLatest
                }
                if (!DiffViewSettings.autoViewed) return@collectLatest
                if (model.autoMarkPassed(from, lastTop).isNotEmpty()) haptics.selection()
                delay(400)
                val info = listState.layoutInfo
                for (item in info.visibleItemsInfo) {
                    val row = model.rows.getOrNull(item.index - 1) as? DiffReviewRow.End ?: continue
                    if (item.offset + item.size <= info.viewportEndOffset) model.autoMark(row.file)
                }
            }
    }

    LaunchedEffect(model, listState) {
        var lastRow: Long? = null
        var lastLayout: List<Any>? = null
        snapshotFlow {
            listOf(tick.intValue, DiffViewSettings.wrap, DiffViewSettings.split, DiffViewSettings.fontSize) to
                dividerRow(listState.layoutInfo, model.rows, headerPx)
        }.collect { (layout, row) ->
            val from = lastRow
            lastRow = row
            if (layout != lastLayout) {
                lastLayout = layout
                muteRatchet()
            }
            if (from == null || row == null || SystemClock.uptimeMillis() < ratchetMutedUntil[0]) return@collect
            val a = model.position(from) ?: return@collect
            val b = model.position(row) ?: return@collect
            val pass = model.scrollPass(a, b)
            if (pass.lines == 0) return@collect
            FmSoundCues.ratchet(pass.lines, pass.changed, pass.flagged)
            haptics.ratchet(crisp = pass.changed > 0 || pass.flagged > 0, caught = pass.flagged > 0)
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(c.editorSurface),
    ) {
        StreamToolbar(
            model = model,
            finding = finding,
            extra = extra,
            onFiles = { showFiles = true },
            onPreviousChange = { jumpTo(model.previousChange(topPosition())) },
            onNextChange = { jumpTo(model.nextChange(topPosition())) },
            onNextFile = {
                val current = model.fileAtPosition(maxOf(topPosition(), 0)) ?: -1
                model.nextUnviewed(current)?.let { jumpTo(model.position(DiffReviewRow.headerId(it))) }
            },
            onFind = {
                finding = !finding
                if (!finding) model.query = ""
            },
        )
        FmInlineStatus(
            notice,
            Modifier
                .background(c.bgRaised)
                .padding(horizontal = FmSpace.x3),
        )
        if (finding) {
            FindBar(
                model = model,
                onJump = { row -> jumpTo(model.position(row)) },
                onClose = {
                    finding = false
                    model.query = ""
                },
            )
        }
        if (model.files.isNotEmpty()) HealthStrip(model) { metric = it }
        val body = @Composable {
            StreamList(model, tick.intValue, listState, highlights, header, emptyMessage, comments, fileActions, copy, onToggleViewed = ::toggleViewed, onJumpToFile = { file ->
                jumpTo(model.position(DiffReviewRow.headerId(file)))
            })
        }
        Box(Modifier.weight(1f)) {
            if (onRefresh != null) {
                FmPullToRefreshBox(isRefreshing = false, onRefresh = onRefresh, modifier = Modifier.fillMaxSize()) { body() }
            } else {
                body()
            }
        }
        if (comments != null) BranchComposer(comments)
    }
    if (showFiles) {
        FilesSheet(model, tick.intValue, onDismiss = { showFiles = false }, onOpen = { file ->
            showFiles = false
            jumpTo(model.position(DiffReviewRow.headerId(file)))
        })
    }
    metric?.let { shown ->
        FmAlertDialog(
            onDismissRequest = { metric = null },
            title = shown.label,
            actions = { FmTextButton(onClick = { metric = null }) { Text("OK") } },
        ) {
            Text(shown.explanation, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
        }
    }
}

@Composable
private fun TinyButton(icon: String, label: String, active: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    FmTooltip(label) {
        Box(
            Modifier
                .size(32.dp)
                .background(if (active) c.accent.copy(alpha = .14f) else Color.Transparent, RoundedCornerShape(6.dp))
                .clickable(enabled = enabled) {
                    haptics.selection()
                    onClick()
                }
                .semantics { contentDescription = label },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(icon, size = 18.dp, tint = if (active) c.accent else if (enabled) c.textSecondary else c.textDisabled)
        }
    }
}

private object DiffTool {
    val side = 30.dp
    val gap = 4.dp
    val icon = 17.dp
    val shape = RoundedCornerShape(8.dp)
}

@Composable
private fun ToolSquare(icon: String, label: String, active: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    FmTooltip(label) {
        Box(
            Modifier
                .size(DiffTool.side)
                .fmGlass(DiffTool.shape, tint = if (active) c.accentSubtle else null, shadow = false)
                .clickable(enabled = enabled) {
                    haptics.selection()
                    onClick()
                }
                .semantics {
                    role = Role.Button
                    contentDescription = label
                    selected = active
                },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(icon, size = DiffTool.icon, tint = if (active) c.accent else if (enabled) c.textSecondary else c.textDisabled)
        }
    }
}

@Composable
private fun StreamToolbar(
    model: DiffReviewModel,
    finding: Boolean,
    extra: List<DiffMenuAction>,
    onFiles: () -> Unit,
    onPreviousChange: () -> Unit,
    onNextChange: () -> Unit,
    onNextFile: () -> Unit,
    onFind: () -> Unit,
) {
    val c = Fm.colors
    var more by remember { mutableStateOf(false) }
    val done = model.files.isNotEmpty() && model.seen == model.files.size
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1)
            .testTag("diff-toolbar"),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(DiffTool.gap),
    ) {
        FmTooltip("File list: ${model.viewedLabel}") {
            Row(
                Modifier
                    .heightIn(min = DiffTool.side)
                    .widthIn(min = DiffTool.side)
                    .fmGlass(DiffTool.shape, shadow = false)
                    .clickable(onClick = onFiles)
                    .semantics {
                        role = Role.Button
                        contentDescription = "${model.viewedLabel}. Files"
                    }
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                FmIcon("format_list_bulleted", size = 16.dp, tint = c.textSecondary)
                Spacer(Modifier.width(4.dp))
                Text(
                    "${model.seen}/${model.files.size}",
                    style = Fm.mono(size = 12.sp, weight = FmType.emphasis, color = if (done) c.success else c.textPrimary),
                )
            }
        }
        ToolSquare("keyboard_arrow_up", "Previous change", onClick = onPreviousChange)
        ToolSquare("keyboard_arrow_down", "Next change", onClick = onNextChange)
        ToolSquare("skip_next", "Next file to review", onClick = onNextFile)
        ToolSquare("search", "Find in diff", active = finding, onClick = onFind)
        ToolSquare(
            if (DiffViewSettings.split) "view_agenda_outlined" else "vertical_split_outlined",
            if (DiffViewSettings.split) "Unified view" else "Side-by-side view",
            active = DiffViewSettings.split,
            onClick = DiffViewSettings::toggleSplit,
        )
        ToolSquare("wrap_text", "Wrap lines", active = DiffViewSettings.wrap, onClick = DiffViewSettings::toggleWrap)
        ToolSquare("code", "Syntax highlighting", active = DiffViewSettings.syntax, onClick = DiffViewSettings::toggleSyntax)
        ToolSquare("flag_outlined", "Show findings on lines", active = DiffViewSettings.findings, onClick = DiffViewSettings::toggleFindings)
        ToolSquare(
            if (DiffViewSettings.autoViewed) "visibility_outlined" else "visibility_off_outlined",
            "Automatically mark files as viewed while scrolling",
            active = DiffViewSettings.autoViewed,
            onClick = DiffViewSettings::toggleAutoViewed,
        )
        ToolSquare("text_decrease", "Smaller diff text", enabled = DiffViewSettings.fontSize > DiffViewSettings.minFontSize) { DiffViewSettings.resize(-1f) }
        ToolSquare("text_increase", "Larger diff text", enabled = DiffViewSettings.fontSize < DiffViewSettings.maxFontSize) { DiffViewSettings.resize(1f) }
        if (extra.isNotEmpty()) {
            Box {
                ToolSquare("more_horiz", "More") { more = true }
                FmDropdownMenu(expanded = more, onDismissRequest = { more = false }) {
                    for (action in extra) {
                        FmDropdownMenuItem(text = action.label, icon = action.icon, onClick = { more = false; action.onClick() })
                    }
                }
            }
        }
    }
}

@Composable
private fun FindBar(model: DiffReviewModel, onJump: (Long) -> Unit, onClose: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .padding(horizontal = 6.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmTextField(
            value = model.query,
            onValueChange = { value ->
                model.query = value
                model.currentMatch?.let { onJump(it.row) }
            },
            modifier = Modifier.weight(1f),
            hint = DiffReviewText.FIND_HINT,
            autofocus = true,
            leading = { FmIcon("search", size = 18.dp) },
        )
        Spacer(Modifier.width(4.dp))
        Text(if (model.query.isEmpty()) "" else model.findLabel, style = Fm.mono(size = 11.sp, color = c.textSecondary))
        TinyButton("keyboard_arrow_up", "Previous match", enabled = model.matches.isNotEmpty()) { model.stepMatch(false)?.let(onJump) }
        TinyButton("keyboard_arrow_down", "Next match", enabled = model.matches.isNotEmpty()) { model.stepMatch(true)?.let(onJump) }
        TinyButton("close", "Close find bar", onClick = onClose)
    }
    Box(
        Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(c.border),
    )
}

@Composable
private fun toneColor(tone: DiffHealthTone): Color {
    val c = Fm.colors
    return when (tone) {
        DiffHealthTone.Green -> c.success
        DiffHealthTone.Orange -> c.warning
        DiffHealthTone.Red -> c.danger
    }
}

private fun healthIcon(key: String): String = when (key) {
    "merge" -> "call_merge"
    "complexity" -> "account_tree"
    "quality" -> "verified"
    "security" -> "shield"
    "network" -> "public"
    "practices" -> "rule"
    else -> "circle"
}

@Composable
private fun HealthStrip(model: DiffReviewModel, onOpen: (DiffHealthMetric) -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("+${model.additions}", style = Fm.mono(size = 11.sp, weight = FmType.emphasis, color = c.success))
        Text("−${model.deletions}", style = Fm.mono(size = 11.sp, weight = FmType.emphasis, color = c.danger))
        for (shown in model.health) {
            val color = toneColor(shown.tone)
            Row(
                Modifier
                    .height(22.dp)
                    .background(color.copy(alpha = .12f), RoundedCornerShape(8.dp))
                    .border(1.dp, color.copy(alpha = .55f), RoundedCornerShape(8.dp))
                    .clickable { onOpen(shown) }
                    .semantics { contentDescription = shown.accessibility }
                    .padding(horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon(healthIcon(shown.key), size = 13.dp, tint = color)
                Spacer(Modifier.width(3.dp))
                Text("${shown.score}", style = Fm.mono(size = 11.sp, weight = FontWeight.Bold, color = color))
            }
        }
    }
    Box(
        Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(c.border),
    )
}

private fun dividerRow(info: LazyListLayoutInfo, rows: List<DiffReviewRow>, fallback: Int): Long? {
    val items = info.visibleItemsInfo
    val pinned = items.firstOrNull { it.contentType == "header" && it.offset <= 0 }
    val line = pinned?.let { it.offset + it.size } ?: fallback
    val item = items.firstOrNull { it !== pinned && it.offset <= line && line < it.offset + it.size } ?: return null
    return rows.getOrNull(item.index - 1)?.id
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun StreamList(
    model: DiffReviewModel,
    version: Int,
    listState: LazyListState,
    highlights: HashMap<Int, DiffFileHighlights>,
    header: (@Composable () -> Unit)?,
    emptyMessage: String,
    comments: BranchComments?,
    fileActions: (DiffFile) -> List<DiffMenuAction>,
    copy: (String, String) -> Unit,
    onToggleViewed: (Int) -> Unit,
    onJumpToFile: (Int) -> Unit,
) {
    val c = Fm.colors
    val rows = model.rows
    val segments = remember(rows) { segmentsOf(rows) }
    val wrap = DiffViewSettings.wrap
    val split = DiffViewSettings.split
    val size = DiffViewSettings.fontSize
    val charWidth = (size * 0.6f).dp
    val metrics = StreamMetrics(
        fontSize = size.sp,
        charWidth = charWidth,
        numberWidth = (size * 0.85f * 0.6f * model.gutterDigits).dp + 1.dp,
        longest = model.longestColumns,
    )
    val code = Fm.mono(size = size.sp, height = 1.35f, color = codeBaseColor(c))
    val bottom = navigationBarBottom()
    fun highlightsOf(file: Int): DiffFileHighlights = highlights.getOrPut(file) { DiffFileHighlights(model.files[file].file) }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val viewport = maxWidth
        val canvas = when {
            wrap -> viewport
            split -> maxOf(metrics.sideWidth * 2 + 1.dp, viewport)
            else -> maxOf(metrics.unifiedWidth, viewport)
        }
        val hScroll = rememberScrollState()
        val pinned = Modifier
            .width(viewport)
            .offset { IntOffset(hScroll.value, 0) }
        val list = @Composable { modifier: Modifier ->
            LazyColumn(modifier, state = listState, contentPadding = PaddingValues(bottom = bottom + 24.dp)) {
                item(key = "leading", contentType = "leading") {
                    Column(if (wrap) Modifier.fillMaxWidth() else pinned) {
                        header?.invoke()
                        if (model.files.isEmpty()) FmEmptyState("check_circle_outline", DiffReviewText.EMPTY_TITLE, message = emptyMessage)
                    }
                }
                for (segment in segments) {
                    val row = rows[segment.position]
                    if (segment.header) {
                        stickyHeader(key = row.id, contentType = "header") {
                            FileHeaderRow(model, version, row.file!!, if (wrap) Modifier.fillMaxWidth() else pinned, fileActions, copy, onToggleViewed, onJumpToFile)
                        }
                    } else if (row is DiffReviewRow.Section) {
                        item(key = row.id, contentType = "section") {
                            Text(
                                model.sections[row.section].title.uppercase(),
                                modifier = (if (wrap) Modifier.fillMaxWidth() else pinned)
                                    .background(c.bgBase)
                                    .padding(start = FmSpace.x3, top = FmSpace.x3, end = FmSpace.x3, bottom = FmSpace.x1),
                                style = Fm.text.labelSmall.copy(color = c.textSecondary, fontWeight = FmType.emphasis),
                            )
                        }
                    }
                    val start = segment.bodyStart
                    val count = segment.bodyEnd - segment.bodyStart
                    if (count > 0) {
                        items(count, key = { rows[start + it].id }, contentType = { rows[start + it]::class.simpleName }) { index ->
                            if (comments == null) {
                                StreamRow(model, version, rows[start + index], metrics, code, wrap, ::highlightsOf, pinned)
                            } else {
                                Column(Modifier.fillMaxWidth()) {
                                    val first = rows[start + index]
                                    if (index == 0 && segment.header && comments.version >= 0) {
                                        first.file?.let { file ->
                                            BranchThreads(comments, comments.session.fileThreads(model.files[file].file.path), if (wrap) Modifier.fillMaxWidth() else pinned, "File comments")
                                        }
                                    }
                                    StreamRow(model, version, first, metrics, code, wrap, ::highlightsOf, pinned, comments)
                                }
                            }
                        }
                    }
                }
            }
        }
        SelectionContainer {
            if (wrap) {
                list(Modifier.fillMaxSize())
            } else {
                Box(
                    Modifier
                        .fillMaxSize()
                        .horizontalScroll(hScroll),
                ) {
                    list(
                        Modifier
                            .width(canvas)
                            .fillMaxHeight(),
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun FileHeaderRow(
    model: DiffReviewModel,
    version: Int,
    index: Int,
    modifier: Modifier,
    fileActions: (DiffFile) -> List<DiffMenuAction>,
    copy: (String, String) -> Unit,
    onToggleViewed: (Int) -> Unit,
    onJumpToFile: (Int) -> Unit,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    var menu by remember { mutableStateOf(false) }
    val file = model.files[index].file
    val viewed = version >= 0 && model.isViewed(index)
    val folded = model.isFolded(index)
    val detail = DiffReviewText.detail(file)
    DisableSelection {
        Box(modifier) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 36.dp)
                    .background(if (viewed) c.bgOverlay else c.bgRaised)
                    .combinedClickable(
                        onClick = {
                            if (viewed) {
                                haptics.selection()
                                model.toggleFold(index)
                            } else {
                                onJumpToFile(index)
                            }
                        },
                        onLongClick = { menu = true },
                    )
                    .padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (viewed) {
                    FmIcon(if (folded) "chevron_right" else "expand_more", size = 18.dp, tint = c.textSecondary, contentDescription = DiffReviewText.foldHint(folded))
                    Spacer(Modifier.width(4.dp))
                }
                FmChangeBadge(diffChangeKind(file.status), size = 18.dp)
                Spacer(Modifier.width(6.dp))
                Text(
                    buildAnnotatedString {
                        withStyle(SpanStyle(fontWeight = FontWeight.SemiBold, color = if (viewed) c.textSecondary else c.textPrimary)) {
                            append(DiffReviewText.name(file))
                        }
                        if (detail.isNotEmpty()) withStyle(SpanStyle(color = c.textTertiary, fontSize = 11.sp)) { append("  $detail") }
                    },
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.MiddleEllipsis,
                    style = Fm.mono(size = 13.sp),
                )
                Spacer(Modifier.width(4.dp))
                if (file.binary) {
                    Text("BIN", style = Fm.mono(size = 11.sp, color = c.textTertiary))
                } else {
                    Row(Modifier.semantics(mergeDescendants = true) { contentDescription = DiffReviewText.countsTooltip(file) }) {
                        Text("+${file.additions}", style = Fm.mono(size = 11.sp, weight = FmType.emphasis, color = c.success))
                        Spacer(Modifier.width(3.dp))
                        Text("−${file.deletions}", style = Fm.mono(size = 11.sp, weight = FmType.emphasis, color = c.danger))
                    }
                }
                Spacer(Modifier.width(2.dp))
                Box(
                    Modifier
                        .size(36.dp)
                        .clickable { haptics.selection(); menu = true }
                        .semantics { contentDescription = "File actions" },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("more_horiz", size = 18.dp, tint = c.textSecondary)
                }
                Spacer(Modifier.width(2.dp))
                Row(
                    Modifier
                        .height(26.dp)
                        .border(1.dp, if (viewed) c.success.copy(alpha = .7f) else c.border, RoundedCornerShape(6.dp))
                        .clickable { onToggleViewed(index) }
                        .semantics { contentDescription = (if (viewed) "Viewed. " else "Not viewed. ") + DiffReviewText.viewedHint(viewed) }
                        .padding(horizontal = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmIcon(if (viewed) "check_box" else "check_box_outline_blank", size = 16.dp, tint = if (viewed) c.success else c.textTertiary)
                    Spacer(Modifier.width(3.dp))
                    Text("Viewed", style = Fm.text.labelSmall.copy(color = if (viewed) c.success else c.textSecondary, fontWeight = FmType.emphasis))
                }
            }
            Box(
                Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(c.border),
            )
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                for (action in fileActions(file)) {
                    FmDropdownMenuItem(text = action.label, icon = action.icon, onClick = { menu = false; action.onClick() })
                }
                FmDropdownMenuItem(
                    text = if (viewed) "Mark as not viewed" else "Mark as viewed",
                    icon = if (viewed) "check_box_outline_blank" else "check_box",
                    onClick = { menu = false; onToggleViewed(index) },
                )
                FmDropdownMenuItem(text = "Copy path", icon = "copy_all_outlined", onClick = { menu = false; copy("Path", file.path) })
                FmDropdownMenuItem(text = "Copy file diff", icon = "content_copy", onClick = { menu = false; copy("Diff", file.raw) })
            }
        }
    }
}

@Composable
private fun StreamRow(
    model: DiffReviewModel,
    version: Int,
    row: DiffReviewRow,
    metrics: StreamMetrics,
    code: androidx.compose.ui.text.TextStyle,
    wrap: Boolean,
    highlightsOf: (Int) -> DiffFileHighlights,
    pinned: Modifier,
    comments: BranchComments? = null,
) {
    val c = Fm.colors
    if (version < 0) return
    if (comments != null && comments.version >= 0) {
        val wide = if (wrap) Modifier.fillMaxWidth() else pinned
        val path = row.file?.let { model.files[it].file.path }.orEmpty()
        when (row) {
            is DiffReviewRow.Line -> {
                UnifiedLine(
                    model, row.file, row.id, row.line, highlightsOf(row.file), metrics, code, wrap,
                    selected = comments.isSelected(path, row.line),
                    onTap = if (comments.canComment) ({ comments.tap(path, row.line) }) else null,
                )
                BranchThreads(comments, comments.session.threadsUnder(path, row.line), wide)
                return
            }
            is DiffReviewRow.Pair -> {
                val left = row.row.left
                val right = row.row.right
                SplitLine(
                    model, row.file, row.id, row.row, highlightsOf(row.file), metrics, code, wrap,
                    leftSelected = comments.isSelected(path, left),
                    rightSelected = comments.isSelected(path, right),
                    onLeft = if (comments.canComment && left != null) ({ comments.tap(path, left) }) else null,
                    onRight = if (comments.canComment && right != null) ({ comments.tap(path, right) }) else null,
                )
                BranchThreads(comments, comments.session.threadsUnder(path, left, right), wide)
                return
            }
            is DiffReviewRow.End -> {
                BranchThreads(comments, comments.session.outdatedThreads(path), wide, "Outdated comments")
                Spacer(Modifier.height(10.dp))
                return
            }
            else -> Unit
        }
    }
    when (row) {
        is DiffReviewRow.Notice -> Text(
            row.text,
            modifier = (if (wrap) Modifier.fillMaxWidth() else pinned)
                .background(c.bgOverlay)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
            style = Fm.text.bodySmall.copy(color = c.textSecondary),
        )
        is DiffReviewRow.Hunk -> {
            val file = model.files[row.file].file
            val label = DiffFileFilter.hunkLabel(DiffFileFilter.hiddenBefore(file, row.index))
            Text(
                buildAnnotatedString {
                    if (label.isNotEmpty()) withStyle(SpanStyle(color = c.textTertiary)) { append("$label   ") }
                    withStyle(SpanStyle(color = c.accent)) { append(row.hunk.header) }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .background(c.accent.copy(alpha = .09f))
                    .padding(horizontal = FmSpace.x2, vertical = (metrics.fontSize.value * .3f).dp),
                maxLines = if (wrap) Int.MAX_VALUE else 1,
                softWrap = wrap,
                overflow = TextOverflow.Clip,
                style = code.copy(fontSize = metrics.fontSize * .92f),
            )
        }
        is DiffReviewRow.Line -> UnifiedLine(model, row.file, row.id, row.line, highlightsOf(row.file), metrics, code, wrap)
        is DiffReviewRow.Pair -> SplitLine(model, row.file, row.id, row.row, highlightsOf(row.file), metrics, code, wrap)
        is DiffReviewRow.End -> Spacer(Modifier.height(10.dp))
        else -> Unit
    }
}

private fun lineColor(colors: FmPalette, kind: DiffLineKind): Color = when (kind) {
    DiffLineKind.Added -> colors.success.copy(alpha = .12f)
    DiffLineKind.Removed -> colors.danger.copy(alpha = .11f)
    DiffLineKind.Context -> Color.Transparent
}

private fun gutterColor(colors: FmPalette, kind: DiffLineKind): Color = when (kind) {
    DiffLineKind.Added -> colors.success.copy(alpha = .20f)
    DiffLineKind.Removed -> colors.danger.copy(alpha = .18f)
    DiffLineKind.Context -> colors.editorGutter
}

private fun selectedLineColor(colors: FmPalette): Color = colors.accent.copy(alpha = .22f)

private fun selectedGutterColor(colors: FmPalette): Color = colors.accent.copy(alpha = .40f)

private fun lineTap(onTap: (() -> Unit)?): Modifier =
    if (onTap == null) Modifier else Modifier.clickable(onClickLabel = "Comment on this line", onClick = onTap)

private fun flagColor(colors: FmPalette, flags: List<DiffLineFlag>): Color = if (flags.any { it.severe }) colors.danger else colors.warning

@Composable
private fun LineMarks(marks: List<DiffLineMark>, metrics: StreamMetrics, backing: Color?, modifier: Modifier) {
    if (marks.isEmpty()) return
    val c = Fm.colors
    val shape = RoundedCornerShape(2.dp)
    Row(
        modifier
            .wrapContentSize(Alignment.TopStart, unbounded = true)
            .padding(top = metrics.charWidth * .2f)
            .then(if (backing == null) Modifier else Modifier.background(c.editorGutter, shape).background(backing, shape))
            .padding(horizontal = 1.dp),
        horizontalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        for (mark in marks) {
            FmIcon(healthIcon(mark.metric), size = metrics.charWidth * 1.05f, tint = if (mark.severe) c.danger else c.warning)
        }
    }
}

@Composable
private fun LineNumber(metrics: StreamMetrics, code: androidx.compose.ui.text.TextStyle, value: Int) {
    Text(
        if (value > 0) "$value" else "",
        modifier = Modifier.width(metrics.numberWidth),
        textAlign = TextAlign.End,
        maxLines = 1,
        style = code.copy(fontSize = metrics.fontSize * .85f, color = Fm.colors.textTertiary),
    )
}

@Composable
private fun CodeText(
    model: DiffReviewModel,
    rowId: Long,
    right: Boolean,
    line: DiffLine,
    highlights: DiffFileHighlights,
    flags: List<DiffLineFlag>,
    code: androidx.compose.ui.text.TextStyle,
    wrap: Boolean,
    modifier: Modifier,
) {
    val c = Fm.colors
    val spans = highlights.spans(line, c, DiffViewSettings.syntax)
    val found = if (model.query.isEmpty()) emptyList() else model.matches(rowId, right)
    val text = if (found.isEmpty() && !line.noNewlineAtEnd && flags.isEmpty()) {
        spans
    } else {
        buildAnnotatedString {
            append(spans)
            val current = model.currentMatch
            for (match in found) {
                val start = match.start.coerceIn(0, spans.length)
                val end = (match.start + match.length).coerceIn(start, spans.length)
                if (end > start) addStyle(SpanStyle(background = if (match == current) findCurrentColor else findColor), start, end)
            }
            if (line.noNewlineAtEnd) withStyle(SpanStyle(color = c.danger)) { append(" ⏎̸") }
            if (flags.isNotEmpty()) {
                withStyle(SpanStyle(color = flagColor(c, flags), fontWeight = FontWeight.Bold)) {
                    append("  ⚑ " + flags.joinToString(" · ") { it.label })
                }
            }
        }
    }
    Text(
        text,
        modifier = modifier,
        softWrap = wrap,
        maxLines = if (wrap) Int.MAX_VALUE else 1,
        overflow = TextOverflow.Clip,
        style = code,
    )
}

@Composable
private fun UnifiedLine(
    model: DiffReviewModel,
    file: Int,
    rowId: Long,
    line: DiffLine,
    highlights: DiffFileHighlights,
    metrics: StreamMetrics,
    code: androidx.compose.ui.text.TextStyle,
    wrap: Boolean,
    selected: Boolean = false,
    onTap: (() -> Unit)? = null,
) {
    val c = Fm.colors
    val flags = if (DiffViewSettings.findings) model.flags(file, line) else emptyList()
    val signColor = when (line.kind) {
        DiffLineKind.Added -> c.success
        DiffLineKind.Removed -> c.danger
        DiffLineKind.Context -> c.textTertiary
    }
    val sign = when (line.kind) {
        DiffLineKind.Added -> "+"
        DiffLineKind.Removed -> "−"
        DiffLineKind.Context -> " "
    }
    Row(
        Modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .background(if (selected) selectedLineColor(c) else lineColor(c, line.kind)),
    ) {
        DisableSelection {
            Row(
                Modifier
                    .fillMaxHeight()
                    .then(lineTap(onTap)),
            ) {
                Row(
                    Modifier
                        .fillMaxHeight()
                        .background(if (selected) selectedGutterColor(c) else if (flags.isEmpty()) gutterColor(c, line.kind) else flagColor(c, flags).copy(alpha = .35f))
                        .padding(horizontal = metrics.gutterPad, vertical = metrics.linePad),
                ) {
                    Box(Modifier.width(metrics.numberWidth)) {
                        LineNumber(metrics, code, line.oldNumber)
                        LineMarks(model.marks(file, line), metrics, null, Modifier.matchParentSize())
                    }
                    Spacer(Modifier.width(metrics.gutterPad))
                    LineNumber(metrics, code, line.newNumber)
                }
                Text(
                    sign,
                    modifier = Modifier
                        .width(metrics.signWidth)
                        .padding(top = metrics.linePad),
                    textAlign = TextAlign.Center,
                    style = code.copy(color = signColor),
                )
            }
        }
        CodeText(
            model, rowId, true, line, highlights, flags, code, wrap,
            Modifier
                .weight(1f)
                .padding(top = metrics.linePad, bottom = metrics.linePad, end = FmSpace.x2),
        )
    }
}

@Composable
private fun SplitLine(
    model: DiffReviewModel,
    file: Int,
    rowId: Long,
    row: DiffSplitRow,
    highlights: DiffFileHighlights,
    metrics: StreamMetrics,
    code: androidx.compose.ui.text.TextStyle,
    wrap: Boolean,
    leftSelected: Boolean = false,
    rightSelected: Boolean = false,
    onLeft: (() -> Unit)? = null,
    onRight: (() -> Unit)? = null,
) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min),
    ) {
        SplitSide(model, file, rowId, row.left, false, highlights, metrics, code, wrap, Modifier.weight(1f), leftSelected, onLeft)
        Box(
            Modifier
                .width(1.dp)
                .fillMaxHeight()
                .background(c.border),
        )
        SplitSide(model, file, rowId, row.right, true, highlights, metrics, code, wrap, Modifier.weight(1f), rightSelected, onRight)
    }
}

@Composable
private fun SplitSide(
    model: DiffReviewModel,
    file: Int,
    rowId: Long,
    line: DiffLine?,
    right: Boolean,
    highlights: DiffFileHighlights,
    metrics: StreamMetrics,
    code: androidx.compose.ui.text.TextStyle,
    wrap: Boolean,
    modifier: Modifier,
    selected: Boolean = false,
    onTap: (() -> Unit)? = null,
) {
    val c = Fm.colors
    if (line == null) {
        Box(
            modifier
                .fillMaxHeight()
                .background(c.bgOverlay.copy(alpha = .55f)),
        )
        return
    }
    val flags = if (DiffViewSettings.findings) model.flags(file, line) else emptyList()
    Row(
        modifier
            .fillMaxHeight()
            .background(if (selected) selectedLineColor(c) else lineColor(c, line.kind)),
    ) {
        DisableSelection {
            Box(
                Modifier
                    .fillMaxHeight()
                    .then(lineTap(onTap))
                    .background(if (selected) selectedGutterColor(c) else if (flags.isEmpty()) gutterColor(c, line.kind) else flagColor(c, flags).copy(alpha = .35f))
                    .padding(horizontal = metrics.gutterPad, vertical = metrics.linePad),
            ) {
                LineNumber(metrics, code, if (right) line.newNumber else line.oldNumber)
                LineMarks(model.marks(file, line), metrics, gutterColor(c, line.kind), Modifier.matchParentSize())
            }
        }
        Spacer(Modifier.width(metrics.gutterPad))
        CodeText(
            model, rowId, right, line, highlights, flags, code, wrap,
            Modifier
                .weight(1f)
                .padding(top = metrics.linePad, bottom = metrics.linePad, end = FmSpace.x1),
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FilesSheet(model: DiffReviewModel, version: Int, onDismiss: () -> Unit, onOpen: (Int) -> Unit) {
    val c = Fm.colors
    var filter by remember { mutableStateOf("") }
    val shown = remember(model, version, filter) {
        val wanted = DiffFileFilter.apply(model.allFiles, filter).toSet()
        model.files.filter { it.file in wanted }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true) {
        Column(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmLinearProgressIndicator(
                    Modifier
                        .weight(1f)
                        .height(4.dp),
                    progress = { model.progress },
                    color = c.success,
                )
                Spacer(Modifier.width(FmSpace.x3))
                Text(model.viewedLabel, style = Fm.text.labelMedium.copy(color = c.textSecondary))
            }
            Spacer(Modifier.height(FmSpace.x1))
            Text("${model.kindsLabel} · ${model.remainingLabel}", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            if (model.files.size > 5) {
                FmTextField(
                    value = filter,
                    onValueChange = { filter = it },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = FmSpace.x2),
                    hint = "Filter files",
                    leading = { FmIcon("search", size = 20.dp) },
                )
            }
        }
        LazyColumn(Modifier.fillMaxWidth()) {
            items(shown.size, key = { shown[it].index }) { i ->
                val entry = shown[i]
                val viewed = model.isViewed(entry.index)
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable { onOpen(entry.index) }
                        .padding(start = FmSpace.x4, end = FmSpace.x1, top = FmSpace.x2, bottom = FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmChangeBadge(diffChangeKind(entry.file.status), size = 20.dp)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(
                            DiffReviewText.name(entry.file),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(color = if (viewed) c.textTertiary else c.textPrimary, fontWeight = FmType.emphasis),
                        )
                        val detail = DiffReviewText.detail(entry.file)
                        if (detail.isNotEmpty()) Text(detail, maxLines = 1, overflow = TextOverflow.StartEllipsis, style = Fm.mono(color = c.textTertiary))
                    }
                    if (entry.file.binary) {
                        Text("BIN", style = Fm.mono(color = c.textTertiary))
                    } else {
                        DiffStatSummary(entry.file.additions, entry.file.deletions, showBar = false)
                    }
                    FmIconButton(
                        if (viewed) "check_circle" else "radio_button_unchecked",
                        if (viewed) "Mark as not viewed" else "Mark as viewed",
                        onPressed = { model.toggleViewed(entry.index) },
                        color = if (viewed) c.success else c.textTertiary,
                        iconSize = 20.dp,
                    )
                }
                FmRowDivider()
            }
            if (shown.isEmpty() && filter.isNotEmpty()) {
                item {
                    Text(
                        "No files match \"$filter\".",
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(FmSpace.x5),
                        textAlign = TextAlign.Center,
                        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                    )
                }
            }
        }
    }
}
