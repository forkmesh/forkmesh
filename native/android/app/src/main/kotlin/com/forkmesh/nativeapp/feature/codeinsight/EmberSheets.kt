package com.forkmesh.nativeapp.feature.codeinsight

import android.content.Context
import android.content.Intent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.RangeSlider
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateMap
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Ember
import com.forkmesh.nativeapp.codeinsight.EmberColoring
import com.forkmesh.nativeapp.codeinsight.EmberCouplingRef
import com.forkmesh.nativeapp.codeinsight.EmberFilters
import com.forkmesh.nativeapp.codeinsight.EmberPanel
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.InsightTrendPoint
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.copyMessageText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException

internal sealed interface EmberTrendState {
    data object Loading : EmberTrendState

    data class Ready(val points: List<InsightTrendPoint>) : EmberTrendState

    data class Failed(val message: String) : EmberTrendState
}

private fun shareFile(context: Context, repoTitle: String, branch: String, snapshot: InsightSnapshot, file: Int) {
    val f = snapshot.files[file]
    val lines = mutableListOf(
        f.path,
        listOf(repoTitle, branch).filter { it.isNotEmpty() }.joinToString(" · "),
        "Code health ${Ember.decimal(f.health)} (${com.forkmesh.nativeapp.codeinsight.Insight.healthBand(f.health).label.lowercase()}) · ${Ember.number(f.codeLines)} lines of code",
    )
    if (Ember.hasHistory(snapshot)) lines.add("${Ember.number(f.commits)} commits · ${Ember.number(f.fixCommits)} fix commits · hotspot ${Ember.decimal(f.hotspot, 2)}")
    lines.addAll(f.healthIssues.map { "• $it" })
    val send = Intent(Intent.ACTION_SEND).setType("text/plain")
        .putExtra(Intent.EXTRA_SUBJECT, Ember.fileNameOf(f.path))
        .putExtra(Intent.EXTRA_TEXT, lines.filter { it.isNotEmpty() }.joinToString("\n"))
    runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun EmberFileSheet(
    context: CodeInsightContext,
    snapshot: InsightSnapshot,
    file: Int,
    coloring: EmberColoring?,
    coupling: List<List<EmberCouplingRef>>,
    trends: SnapshotStateMap<String, EmberTrendState>,
    onSelectFile: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val android = LocalContext.current
    val status = rememberFmStatus()
    val f = snapshot.files[file]
    val rank = coloring?.files?.getOrNull(file)?.rank ?: 0
    val card = remember(snapshot, file, rank) { EmberPanel.card(snapshot, file, rank, System.currentTimeMillis() / 1000) }
    val refs = coupling.getOrNull(file).orEmpty()
    val history = Ember.hasHistory(snapshot)
    var tab by rememberSaveable { mutableIntStateOf(0) }
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    LaunchedEffect(f.path, tab) {
        if (tab != 1 || !history || trends[f.path] is EmberTrendState.Ready) return@LaunchedEffect
        trends[f.path] = EmberTrendState.Loading
        trends[f.path] = try {
            EmberTrendState.Ready(context.loadTrend(f.path))
        } catch (cancel: CancellationException) {
            trends.remove(f.path)
            throw cancel
        } catch (error: Exception) {
            EmberTrendState.Failed(error.message ?: "Could not read the history of ${card.name}.")
        }
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState, showDragHandle = true, isScrollControlled = true) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4)
                .padding(bottom = FmSpace.x6),
        ) {
            Text(card.crumb, style = Fm.text.bodySmall)
            Text(card.name, style = Fm.text.titleLarge)
            Spacer(Modifier.height(FmSpace.x2))
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                FmTextButton(onClick = { context.openFile(f.path, 0) }, modifier = Modifier.semantics { contentDescription = "Open ${card.name}" }) {
                    FmIcon("open_in_new", size = 18.dp)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text("Open file")
                }
                FmTextButton(onClick = { copyMessageText(android, f.path, status) }, modifier = Modifier.semantics { contentDescription = "Copy path ${f.path}" }) {
                    FmIcon("content_copy", size = 18.dp)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text("Copy path")
                }
                FmTextButton(
                    onClick = { shareFile(android, context.repoTitle, context.branch, snapshot, file) },
                    modifier = Modifier.semantics { contentDescription = "Share ${card.name}" },
                ) {
                    FmIcon("share", size = 18.dp)
                    Spacer(Modifier.width(FmSpace.x1))
                    Text("Share")
                }
            }
            FmInlineStatus(status)
            FmSegmentTabBar(
                tabs = listOf(FmSegmentTab("Metrics"), FmSegmentTab("Complexity trend"), FmSegmentTab(EmberPanel.couplingTabTitle(refs.size))),
                index = tab,
                onChanged = { tab = it },
            )
            Spacer(Modifier.height(FmSpace.x3))
            when (tab) {
                0 -> EmberMetrics(context, f.path, card)
                1 -> {
                    val state = trends[f.path]
                    when {
                        !history -> EmberMessage(EmberPanel.TREND_NO_HISTORY)
                        state is EmberTrendState.Ready && state.points.isEmpty() -> EmberMessage(EmberPanel.TREND_EMPTY)
                        state is EmberTrendState.Ready -> EmberTrendChart(state.points, Modifier.fillMaxWidth())
                        state is EmberTrendState.Failed -> EmberMessage(state.message)
                        else -> EmberMessage("Reading the history of ${card.name}…")
                    }
                }
                else -> {
                    Text(EmberPanel.COUPLING_INTRO, style = Fm.text.bodySmall)
                    Spacer(Modifier.height(FmSpace.x2))
                    if (refs.isEmpty()) {
                        EmberMessage(if (history) EmberPanel.COUPLING_EMPTY else EmberPanel.COUPLING_NO_HISTORY)
                    } else {
                        for (row in EmberPanel.couplingRows(snapshot, refs)) EmberRowItem(row, false, onTap = { onSelectFile(row.id) })
                    }
                }
            }
        }
    }
}

@Composable
private fun EmberMessage(text: String) {
    Text(text, modifier = Modifier.padding(vertical = FmSpace.x4), style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary))
}

@Composable
private fun EmberMetrics(context: CodeInsightContext, path: String, card: com.forkmesh.nativeapp.codeinsight.EmberCard) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        EmberHealthRing(card.health)
        Spacer(Modifier.width(FmSpace.x4))
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Text("Code health", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis))
            FmStatusBadge(card.band.label, Ember.bandColor(card.band).compose())
            if (card.note.isNotEmpty()) Text(card.note, style = Fm.text.bodySmall)
        }
    }
    Spacer(Modifier.height(FmSpace.x3))
    for (metric in card.metrics) {
        val link = metric.line > 0
        Row(
            Modifier
                .fillMaxWidth()
                .then(
                    if (link) {
                        Modifier
                            .clickable { context.openFile(path, metric.line) }
                            .semantics {
                                role = Role.Button
                                contentDescription = "${metric.label}: ${metric.value}. Opens line ${metric.line}"
                            }
                    } else {
                        Modifier
                    },
                )
                .padding(vertical = 6.dp),
        ) {
            Text(metric.label, modifier = Modifier.width(96.dp), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            Spacer(Modifier.width(FmSpace.x2))
            Text(metric.value, modifier = Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = if (link) c.accent else c.textPrimary))
        }
    }
    Spacer(Modifier.height(FmSpace.x3))
    FmSectionHeader("Health issues")
    Spacer(Modifier.height(FmSpace.x2))
    if (card.issues.isEmpty()) {
        Text("No issues found.", style = Fm.text.bodyMedium)
    } else {
        for (issue in card.issues) Text("• $issue", modifier = Modifier.padding(vertical = 2.dp), style = Fm.text.bodyMedium)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun EmberFilterSheet(
    snapshot: InsightSnapshot,
    filters: EmberFilters,
    matching: Int,
    onChange: (EmberFilters) -> Unit,
    onSubmitSearch: () -> Unit,
    onDismiss: () -> Unit,
) {
    val c = Fm.colors
    val history = Ember.hasHistory(snapshot)
    val authors = remember(snapshot) { EmberPanel.authorOptions(snapshot) }
    val commitMax = remember(snapshot) { EmberPanel.commitMax(snapshot) }
    var authorMenu by remember { mutableStateOf(false) }
    val sliderColors = SliderDefaults.colors(
        thumbColor = c.accent,
        activeTrackColor = c.accent,
        inactiveTrackColor = c.border,
        activeTickColor = c.bgBase.copy(alpha = 0.6f),
        inactiveTickColor = c.textTertiary.copy(alpha = 0.4f),
    )
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true, isScrollControlled = true) {
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4)
                .padding(bottom = FmSpace.x6),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Filters", style = Fm.text.titleMedium)
                    Text("$matching of ${Ember.number(snapshot.files.size)} files match", style = Fm.text.bodySmall)
                }
                FmTextButton(onClick = { onChange(EmberFilters()) }, enabled = filters.active, busy = false) { Text("Reset") }
            }
            FmField("Search by filename") {
                FmTextField(
                    value = filters.search,
                    onValueChange = { onChange(filters.copy(search = it)) },
                    hint = "Search by filename",
                    leading = { FmIcon("search", size = 20.dp, tint = c.textTertiary) },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = { onSubmitSearch() }),
                )
            }
            FmField("Filter by authors") {
                val current = authors.firstOrNull { it.author == filters.author }?.label ?: "All authors"
                val enabled = history && authors.size > 1
                Box {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable(enabled = enabled) { authorMenu = true }
                            .semantics {
                                role = Role.DropdownList
                                contentDescription = "Filter by authors: $current"
                            }
                            .padding(vertical = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(current, modifier = Modifier.weight(1f), style = Fm.text.bodyLarge.copy(color = if (enabled) c.textPrimary else c.textDisabled))
                        FmIcon("expand_more", size = 20.dp, tint = c.textTertiary)
                    }
                    FmDropdownMenu(expanded = authorMenu, onDismissRequest = { authorMenu = false }) {
                        for (option in authors) {
                            FmDropdownMenuItem(option.label, onClick = {
                                authorMenu = false
                                onChange(filters.copy(author = option.author))
                            })
                        }
                    }
                }
            }
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Code health range", modifier = Modifier.weight(1f), style = Fm.text.bodySmall.copy(fontWeight = FmType.medium))
                    Text("${Ember.decimal(filters.minHealth)} – ${Ember.decimal(filters.maxHealth)}", style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular))
                }
                RangeSlider(
                    value = filters.minHealth.toFloat()..filters.maxHealth.toFloat(),
                    onValueChange = { range ->
                        val low = Math.round(range.start * 2) / 2.0
                        val high = Math.round(range.endInclusive * 2) / 2.0
                        if (low != filters.minHealth || high != filters.maxHealth) onChange(filters.copy(minHealth = low, maxHealth = high))
                    },
                    valueRange = 1f..10f,
                    steps = 17,
                    colors = sliderColors,
                    modifier = Modifier.semantics { contentDescription = "Code health range" },
                )
                EmberBandStrip(Modifier.padding(horizontal = 10.dp))
            }
            Column {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Commit threshold", modifier = Modifier.weight(1f), style = Fm.text.bodySmall.copy(fontWeight = FmType.medium))
                    Text(EmberPanel.commitThresholdText(filters.minCommits), style = Fm.text.bodySmall)
                }
                Slider(
                    value = filters.minCommits.coerceAtMost(commitMax).toFloat(),
                    onValueChange = { v ->
                        val value = Math.round(v)
                        if (value != filters.minCommits) onChange(filters.copy(minCommits = value))
                    },
                    valueRange = 0f..commitMax.toFloat(),
                    steps = if (commitMax > 1 && commitMax <= 60) commitMax - 1 else 0,
                    enabled = history,
                    colors = sliderColors,
                    modifier = Modifier.semantics { contentDescription = "Minimum commits" },
                )
                if (!history) Text("Needs git history", style = Fm.text.bodySmall)
            }
        }
    }
}
