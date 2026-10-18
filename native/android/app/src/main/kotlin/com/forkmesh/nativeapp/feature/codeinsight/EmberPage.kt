package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Ember
import com.forkmesh.nativeapp.codeinsight.EmberColoring
import com.forkmesh.nativeapp.codeinsight.EmberCouplingRef
import com.forkmesh.nativeapp.codeinsight.EmberFilters
import com.forkmesh.nativeapp.codeinsight.EmberLayout
import com.forkmesh.nativeapp.codeinsight.EmberMap
import com.forkmesh.nativeapp.codeinsight.EmberPanel
import com.forkmesh.nativeapp.codeinsight.EmberTab
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private class EmberPrepared(val snapshot: InsightSnapshot, val layout: EmberLayout, val coupling: List<List<EmberCouplingRef>>)

private fun listCaption(tab: EmberTab): String = when (tab) {
    EmberTab.Hotspots -> "ranked by hotspot score"
    EmberTab.RefactoringTargets -> "ranked by hotspot × (10 − health)"
    EmberTab.CodeHealth -> "least healthy first"
    EmberTab.ChangeCoupling -> "ranked by strongest coupling"
    EmberTab.Knowledge -> "ranked by share of code by former contributors"
    EmberTab.Defects -> "ranked by bug-fix commits"
    EmberTab.Languages -> "grouped by language, largest first"
}

@Composable
fun EmberPage(context: CodeInsightContext, @Suppress("UNUSED_PARAMETER") navigator: Navigator) {
    val snapshot = context.snapshot
    val c = Fm.colors
    val haptics = FmHaptics.current
    val scope = rememberCoroutineScope()
    val prepared by produceState<EmberPrepared?>(null, snapshot) {
        value = withContext(Dispatchers.Default) {
            EmberPrepared(snapshot, EmberMap.buildLayout(snapshot), Ember.buildCouplingIndex(snapshot))
        }
    }
    var chosenTab by rememberSaveable { mutableStateOf<String?>(null) }
    val tab = EmberPanel.defaultTab(snapshot, chosenTab?.let { name -> EmberTab.entries.firstOrNull { it.name == name } })
    var filters by remember { mutableStateOf(EmberFilters()) }
    var listMode by rememberSaveable { mutableStateOf(false) }
    var selectedPath by rememberSaveable { mutableStateOf<String?>(null) }
    var cardOpen by rememberSaveable { mutableStateOf(false) }
    var filtersOpen by remember { mutableStateOf(false) }
    var infoOpen by remember { mutableStateOf(false) }
    val trends = remember(snapshot) { mutableStateMapOf<String, EmberTrendState>() }
    val mapState = remember { EmberMapState(scope) }
    val ready = prepared?.takeIf { it.snapshot === snapshot }
    val coupling = ready?.coupling.orEmpty()
    val history = Ember.hasHistory(snapshot)
    val coloring by produceState<EmberColoring?>(null, ready, tab, filters) {
        val p = ready ?: return@produceState
        value = withContext(Dispatchers.Default) { Ember.colorize(p.snapshot, tab, filters, p.coupling) }
    }
    val currentColoring = coloring?.takeIf { it.files.size == snapshot.files.size }
    val selected = selectedPath?.let { snapshot.indexOf(it) } ?: -1

    LaunchedEffect(snapshot) {
        context.setSummary(Ember.summarize(snapshot).text)
        val author = filters.author
        if (author.isNotEmpty() && EmberPanel.authorOptions(snapshot).none { it.author == author }) filters = filters.copy(author = "")
    }
    LaunchedEffect(ready) {
        val p = ready ?: return@LaunchedEffect
        mapState.useLayout(p.layout)
        val restore = selectedPath?.let { snapshot.indexOf(it) } ?: -1
        if (restore >= 0) mapState.select(restore, true) else selectedPath = null
    }
    LaunchedEffect(mapState.ready) {
        if (mapState.ready && selected >= 0 && mapState.selected != selected) mapState.select(selected, true)
    }

    fun selectFile(file: Int, zoomTo: Boolean, openCard: Boolean) {
        if (file !in snapshot.files.indices) return
        haptics.selection()
        selectedPath = snapshot.files[file].path
        mapState.select(file, zoomTo)
        if (openCard) cardOpen = true
    }

    fun clearSelection() {
        selectedPath = null
        cardOpen = false
        mapState.select(-1, false)
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(c.bgBase),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x3),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            for (t in Ember.tabs) {
                val active = t == tab
                FilterChip(
                    selected = active,
                    onClick = {
                        if (!active) haptics.selection()
                        chosenTab = t.name
                    },
                    label = { Text(t.title) },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = c.accentSubtle,
                        selectedLabelColor = c.accent,
                        labelColor = if (t.needsHistory && !history) c.textTertiary else c.textSecondary,
                    ),
                    modifier = Modifier.semantics {
                        contentDescription = if (t.needsHistory && !history) "${t.title}, needs git history" else t.title
                    },
                )
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .padding(start = FmSpace.x3, end = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            EmberCrumbs(ready?.layout, mapState, selected, Modifier.weight(1f))
            Box {
                FmIconButton(
                    icon = "tune",
                    label = if (filters.active) "Filters and search, ${currentColoring?.matching ?: 0} files match" else "Filters and search",
                    onPressed = { filtersOpen = true },
                    color = if (filters.active) c.accent else null,
                )
                if (filters.active) {
                    Box(
                        Modifier
                            .align(Alignment.TopEnd)
                            .padding(top = 10.dp, end = 10.dp)
                            .size(8.dp)
                            .background(c.accent, CircleShape),
                    )
                }
            }
            EmberModeToggle(listMode, onChange = {
                if (it != listMode) haptics.selection()
                listMode = it
            })
        }
        HorizontalDivider(color = c.border)
        if (ready == null) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    FmCircularProgressIndicator(Modifier.size(28.dp))
                    Spacer(Modifier.height(FmSpace.x3))
                    Text("Laying out ${Ember.number(snapshot.files.size)} files…", style = Fm.text.bodySmall)
                }
            }
        } else if (snapshot.files.isEmpty()) {
            Text(
                snapshot.error.ifEmpty { "No source files found here. Pick another repository or folder." },
                modifier = Modifier.padding(FmSpace.x4),
                style = Fm.text.bodyMedium,
            )
        } else if (listMode) {
            EmberList(snapshot, currentColoring, coupling, selected, onSelect = { selectFile(it, true, true) })
        } else {
            Box(
                Modifier
                    .weight(1.35f)
                    .fillMaxWidth(),
            ) {
                val focusName = ready.layout.nodes.getOrNull(mapState.focus)?.name ?: ""
                val description = buildString {
                    append("Ember map, ${tab.title}. ")
                    append("${currentColoring?.matching ?: 0} of ${snapshot.files.size} files shown, showing $focusName. ")
                    if (selected >= 0) append("Selected ${Ember.fileNameOf(snapshot.files[selected].path)}. ")
                    append("Switch to List for a readable list of the same files.")
                }
                EmberMapCanvas(
                    state = mapState,
                    snapshot = snapshot,
                    coloring = currentColoring,
                    coupling = coupling,
                    description = description,
                    onTapFile = { selectFile(it, false, true) },
                    onClearSelection = { clearSelection() },
                    modifier = Modifier.fillMaxSize(),
                )
                if (currentColoring != null) {
                    EmberLegend(
                        currentColoring,
                        onInfo = { infoOpen = true },
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(FmSpace.x2)
                            .horizontalScroll(rememberScrollState()),
                    )
                    if (currentColoring.note.isNotEmpty()) {
                        Text(
                            currentColoring.note,
                            modifier = Modifier
                                .align(Alignment.TopCenter)
                                .padding(top = 52.dp, start = FmSpace.x5, end = FmSpace.x5)
                                .widthIn(max = 420.dp)
                                .clip(RoundedCornerShape(FmRadius.lg))
                                .background(if (c.dark) Color(22, 27, 34, 235) else Color(255, 255, 255, 240))
                                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                            textAlign = TextAlign.Center,
                            style = Fm.text.bodyMedium,
                        )
                    }
                }
                Column(
                    Modifier
                        .align(Alignment.BottomEnd)
                        .padding(FmSpace.x2)
                        .clip(RoundedCornerShape(FmRadius.lg))
                        .background(if (c.dark) Color(22, 27, 34, 225) else Color(255, 255, 255, 235)),
                ) {
                    FmIconButton("add", "Zoom in", onPressed = { mapState.zoomBy(1.6) })
                    FmIconButton("remove", "Zoom out", onPressed = { mapState.zoomBy(1 / 1.6) })
                    FmIconButton("adjust", "Show the whole repository", onPressed = { mapState.focusNode(0) })
                }
            }
            HorizontalDivider(color = c.border)
            EmberSidePanel(
                snapshot = snapshot,
                tab = tab,
                coloring = currentColoring,
                selected = selected,
                onSelect = { selectFile(it, true, true) },
                onBand = { band ->
                    haptics.selection()
                    filters = EmberPanel.toggleBand(filters, band)
                },
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            )
        }
    }

    if (cardOpen && selected >= 0 && ready != null) {
        EmberFileSheet(
            context = context,
            snapshot = snapshot,
            file = selected,
            coloring = currentColoring,
            coupling = coupling,
            trends = trends,
            onSelectFile = { selectFile(it, true, true) },
            onDismiss = { cardOpen = false },
        )
    }
    if (filtersOpen) {
        EmberFilterSheet(
            snapshot = snapshot,
            filters = filters,
            matching = currentColoring?.matching ?: 0,
            onChange = { filters = it },
            onSubmitSearch = {
                val best = EmberPanel.bestMatch(snapshot, filters.search)
                if (best >= 0) {
                    filtersOpen = false
                    selectFile(best, true, true)
                }
            },
            onDismiss = { filtersOpen = false },
        )
    }
    if (infoOpen) {
        FmAlertDialog(
            onDismissRequest = { infoOpen = false },
            title = EmberPanel.INFO_TITLE,
            actions = { FmTextButton(onClick = { infoOpen = false }) { Text("Close") } },
        ) {
            Text(EmberPanel.INFO_TEXT, style = Fm.text.bodyMedium)
        }
    }
}

@Composable
private fun EmberCrumbs(layout: EmberLayout?, state: EmberMapState, selected: Int, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val crumbs = if (layout == null) emptyList() else EmberMap.crumbs(layout, state.focus, layout.fileNode(selected))
    Row(
        modifier.horizontalScroll(rememberScrollState(), reverseScrolling = true),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        crumbs.forEachIndexed { i, crumb ->
            if (i > 0) Text("  /  ", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            val tappable = crumb.node >= 0 && !crumb.current
            Text(
                crumb.name,
                modifier = Modifier
                    .then(
                        if (tappable) {
                            Modifier
                                .clip(RoundedCornerShape(FmRadius.sm))
                                .clickable {
                                    haptics.tap()
                                    state.focusNode(crumb.node)
                                }
                                .semantics { contentDescription = "Zoom to ${crumb.name}" }
                        } else {
                            Modifier
                        },
                    )
                    .padding(vertical = FmSpace.x2, horizontal = 2.dp),
                style = Fm.text.bodySmall.copy(
                    color = if (crumb.current) c.textPrimary else if (tappable) c.accent else c.textTertiary,
                    fontWeight = if (crumb.current) FontWeight.SemiBold else FontWeight.Normal,
                ),
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun EmberModeToggle(list: Boolean, onChange: (Boolean) -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .padding(horizontal = FmSpace.x1)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay),
    ) {
        for ((icon, isList, label) in listOf(Triple("map", false, "Map"), Triple("view_list", true, "List"))) {
            val active = list == isList
            Box(
                Modifier
                    .size(width = 40.dp, height = 32.dp)
                    .clip(RoundedCornerShape(FmRadius.md))
                    .background(if (active) c.accentSubtle else Color.Transparent)
                    .clickable { onChange(isList) }
                    .semantics {
                        role = Role.Tab
                        selected = active
                        contentDescription = "$label view"
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(icon, size = 18.dp, tint = if (active) c.accent else c.textSecondary)
            }
        }
    }
}

@Composable
private fun EmberSidePanel(
    snapshot: InsightSnapshot,
    tab: EmberTab,
    coloring: EmberColoring?,
    selected: Int,
    onSelect: (Int) -> Unit,
    onBand: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val history = Ember.hasHistory(snapshot)
    val bands = remember(snapshot) { EmberPanel.bandRows(snapshot) }
    val hotspots = remember(snapshot) { EmberPanel.hotspotRows(snapshot) }
    Column(
        modifier
            .verticalScroll(rememberScrollState())
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        if (tab == EmberTab.RefactoringTargets && history) {
            Text("Refactoring targets", style = Fm.text.titleSmall)
            Text(EmberPanel.TARGETS_INTRO, style = Fm.text.bodySmall)
            val rows = coloring?.let { EmberPanel.targetRows(snapshot, it.targets) }.orEmpty()
            if (rows.isEmpty()) Text("No refactoring targets.", style = Fm.text.bodyMedium)
            for (row in rows) EmberRowItem(row, row.id == selected, onTap = { onSelect(row.id) })
            Spacer(Modifier.height(FmSpace.x3))
        }
        Text("Code health", style = Fm.text.titleSmall)
        for (row in bands) EmberRowItem(row, false, onTap = { onBand(row.id) })
        Spacer(Modifier.height(FmSpace.x2))
        FmSectionHeader("Top hotspots")
        if (hotspots.isEmpty()) Text(EmberPanel.hotspotsEmptyText(history), style = Fm.text.bodyMedium)
        for (row in hotspots) EmberRowItem(row, row.id == selected, onTap = { onSelect(row.id) })
        Spacer(Modifier.height(FmSpace.x2))
        Text(
            "Tap a file to see its metrics. Tap a folder to zoom in, tap outside it to zoom out, pinch to zoom, drag to pan, double-tap to zoom in.",
            style = Fm.text.bodySmall,
        )
    }
}

@Composable
private fun EmberList(
    snapshot: InsightSnapshot,
    coloring: EmberColoring?,
    coupling: List<List<EmberCouplingRef>>,
    selected: Int,
    onSelect: (Int) -> Unit,
) {
    val rows = remember(snapshot, coloring, coupling) {
        if (coloring == null) emptyList() else EmberPanel.rankedFiles(snapshot, coloring, coupling)
    }
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(horizontal = FmSpace.x3, vertical = FmSpace.x3)) {
        item(key = "header") {
            Column(Modifier.padding(bottom = FmSpace.x2)) {
                if (coloring != null) {
                    Text(
                        "${Ember.number(rows.size)} of ${Ember.number(snapshot.files.size)} files · ${listCaption(coloring.tab)}",
                        style = Fm.text.bodySmall,
                    )
                    if (coloring.tab == EmberTab.RefactoringTargets && coloring.note.isEmpty()) {
                        Text(EmberPanel.TARGETS_INTRO, modifier = Modifier.padding(top = FmSpace.x1), style = Fm.text.bodySmall)
                    }
                    if (coloring.note.isNotEmpty()) {
                        Text(coloring.note, modifier = Modifier.padding(top = FmSpace.x2), style = Fm.text.bodyMedium)
                    }
                }
            }
        }
        items(rows, key = { it.id }) { row ->
            EmberRowItem(row, row.id == selected, onTap = { onSelect(row.id) })
        }
    }
}
