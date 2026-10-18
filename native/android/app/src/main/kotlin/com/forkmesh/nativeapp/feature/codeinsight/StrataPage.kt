package com.forkmesh.nativeapp.feature.codeinsight

import android.content.ClipData
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataDrillGraph
import com.forkmesh.nativeapp.codeinsight.StrataLayout
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataReportHtml
import com.forkmesh.nativeapp.codeinsight.StrataSeverity
import com.forkmesh.nativeapp.codeinsight.StrataTreemapSize
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

enum class StrataView(val label: String, val icon: String) {
    Dashboard("Dashboard", "dashboard"),
    Graph("Graph", "account_tree"),
    Matrix("Matrix", "grid_view"),
    Scatter("Scatter", "query_stats"),
    Treemap("Treemap", "dataset"),
    Rules("Rules", "rule"),
}

enum class StrataGraphColor(val label: String, val legend: String) {
    Rating("Debt rating", "Debt rating"),
    Health("Health", "Health"),
    Instability("Instability", "Instability"),
    Complexity("Complexity", "Complexity"),
}

enum class StrataTreemapColor(val label: String, val low: String) {
    Health("Health", "Healthy"),
    MaxComplexity("Max complexity", "≤ 4"),
    Debt("Debt ratio", "0 %"),
    ChangeFrequency("Change frequency", "0"),
}

sealed interface StrataFocus {
    data class Module(val index: Int) : StrataFocus
    data class File(val index: Int) : StrataFocus
    data class Cell(val from: Int, val to: Int) : StrataFocus
}

class StrataPageState(val snapshot: InsightSnapshot) {
    var view by mutableStateOf(StrataView.Dashboard)
    var depth by mutableIntStateOf(0)
    var model by mutableStateOf<StrataModel?>(null)
    var building by mutableStateOf(false)
    var selection by mutableStateOf<StrataFocus?>(null)
    var sheetOpen by mutableStateOf(false)
    var drilled by mutableIntStateOf(-1)
    var drill by mutableStateOf<StrataDrillGraph?>(null)
    var graphColor by mutableStateOf(StrataGraphColor.Rating)
    var graphList by mutableStateOf(false)
    var fitRequest by mutableIntStateOf(0)
    var treemapSize by mutableStateOf(StrataTreemapSize.Lines)
    var treemapColor by mutableStateOf(StrataTreemapColor.Health)
    var ruleQuery by mutableStateOf("")
    var ruleSeverity by mutableStateOf<StrataSeverity?>(null)
    var expandedRules by mutableStateOf(setOf<Int>())
    var searchOpen by mutableStateOf(false)
    var status by mutableStateOf<String?>(null)

    fun select(focus: StrataFocus?, open: Boolean = true) {
        selection = focus
        sheetOpen = open && focus != null
    }

    fun drillInto(module: Int) {
        drilled = module
        drill = null
        view = StrataView.Graph
    }

    fun drillOut() {
        drilled = -1
        drill = null
    }

    fun showRule(index: Int) {
        ruleQuery = ""
        ruleSeverity = null
        expandedRules = setOf(index)
        view = StrataView.Rules
        sheetOpen = false
    }
}

@Composable
fun StrataPage(context: CodeInsightContext, @Suppress("UNUSED_PARAMETER") navigator: Navigator) {
    val state = remember(context.snapshot) { StrataPageState(context.snapshot) }
    val depth = state.depth
    LaunchedEffect(context.snapshot, depth) {
        state.building = true
        val built = withContext(Dispatchers.Default) { Strata.buildModel(context.snapshot, depth) }
        state.selection = null
        state.sheetOpen = false
        state.drillOut()
        state.model = built
        state.building = false
        context.setSummary(Strata.summaryLine(built.report))
    }
    val model = state.model
    val drilled = state.drilled
    LaunchedEffect(model, drilled) {
        if (model == null || drilled < 0) return@LaunchedEffect
        state.drill = withContext(Dispatchers.Default) { StrataLayout.drillGraph(model.snapshot, model.report.graph, drilled) }
    }
    val c = Fm.colors
    Column(Modifier.fillMaxSize().background(c.bgBase)) {
        StrataViewChips(state)
        StrataToolbar(state, context)
        state.status?.let {
            FmInlineStatusLine(message = it, isError = false, modifier = Modifier.padding(horizontal = FmSpace.x4))
        }
        if (state.building) FmLinearProgressIndicator(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4))
        Box(Modifier.weight(1f).fillMaxWidth()) {
            if (model == null) {
                if (!state.building) FmEmptyState("layers", "No modules", message = "Strata found no source files to group into modules.")
            } else if (model.report.graph.nodes.isEmpty()) {
                FmEmptyState("layers", "No modules", message = "Strata found no source files to group into modules.")
            } else {
                when (state.view) {
                    StrataView.Dashboard -> StrataDashboard(model, state)
                    StrataView.Graph -> StrataGraphView(model, state, context)
                    StrataView.Matrix -> StrataMatrixView(model, state)
                    StrataView.Scatter -> StrataScatterView(model, state)
                    StrataView.Treemap -> StrataTreemapView(model, state)
                    StrataView.Rules -> StrataRulesView(model, state, context)
                }
            }
        }
    }
    if (model != null && state.sheetOpen && state.selection != null) {
        StrataDetailsSheet(model, state, context)
    }
    if (model != null && state.searchOpen) {
        StrataSearchSheet(model, state)
    }
}

@Composable
private fun StrataViewChips(state: StrataPageState) {
    val haptics = FmHaptics.current
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x3),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (view in StrataView.entries) {
            StrataChip(
                label = view.label,
                selected = state.view == view,
                icon = view.icon,
                description = if (state.view == view) "${view.label}, selected" else "Show ${view.label}",
                onClick = {
                    if (state.view != view) haptics.selection()
                    state.view = view
                },
            )
        }
    }
}

@Composable
internal fun <T> StrataMenuButton(
    label: String,
    icon: String,
    options: List<T>,
    selected: T,
    optionLabel: (T) -> String,
    description: String,
    onSelect: (T) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmToneButton(label = label, onPressed = { open = true }, icon = icon, tooltip = description)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for (option in options) {
                FmDropdownMenuItem(
                    text = optionLabel(option),
                    icon = if (option == selected) "check" else null,
                    onClick = {
                        open = false
                        onSelect(option)
                    },
                )
            }
        }
    }
}

private fun depthLabel(depth: Int): String = if (depth == 0) "Auto" else "$depth ${if (depth == 1) "level" else "levels"}"

@Composable
private fun StrataToolbar(state: StrataPageState, context: CodeInsightContext) {
    val scope = rememberCoroutineScope()
    val android = LocalContext.current
    val c = Fm.colors
    val model = state.model
    Row(
        Modifier.fillMaxWidth().padding(start = FmSpace.x3, end = FmSpace.x1, top = FmSpace.x1, bottom = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier.weight(1f).horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            StrataMenuButton(
                label = "Modules: ${depthLabel(state.depth)}",
                icon = "layers",
                options = listOf(0, 1, 2, 3, 4),
                selected = state.depth,
                optionLabel = { "Modules: ${depthLabel(it)}" },
                description = "Module depth: how many folder levels make a module, now ${depthLabel(state.depth)}",
                onSelect = { state.depth = it },
            )
            when (state.view) {
                StrataView.Graph -> {
                    StrataMenuButton(
                        label = state.graphColor.label,
                        icon = "palette",
                        options = StrataGraphColor.entries,
                        selected = state.graphColor,
                        optionLabel = { "Colour: ${it.label}" },
                        description = "Colour modules by ${state.graphColor.label}",
                        onSelect = { state.graphColor = it },
                    )
                    FmToneButton(
                        label = if (state.graphList) "Canvas" else "List",
                        onPressed = { state.graphList = !state.graphList },
                        icon = if (state.graphList) "account_tree" else "list",
                        tooltip = if (state.graphList) "Show the graph" else "Show the graph as a list",
                    )
                }
                StrataView.Treemap -> {
                    StrataMenuButton(
                        label = state.treemapSize.label,
                        icon = "dataset",
                        options = StrataTreemapSize.entries,
                        selected = state.treemapSize,
                        optionLabel = { "Size: ${it.label}" },
                        description = "Size files by ${state.treemapSize.label}",
                        onSelect = { state.treemapSize = it },
                    )
                    StrataMenuButton(
                        label = state.treemapColor.label,
                        icon = "palette",
                        options = StrataTreemapColor.entries,
                        selected = state.treemapColor,
                        optionLabel = { "Colour: ${it.label}" },
                        description = "Colour files by ${state.treemapColor.label}",
                        onSelect = { state.treemapColor = it },
                    )
                }
                else -> Unit
            }
        }
        if (state.view == StrataView.Graph && !state.graphList) {
            FmIconButton("fullscreen", "Fit the graph", onPressed = { state.fitRequest++ }, color = c.textSecondary)
        }
        FmIconButton("search", "Find a module or file", onPressed = if (model != null) ({ state.searchOpen = true }) else null, color = c.textSecondary)
        FmIconButton(
            "ios_share",
            "Export report",
            onPressed = if (model != null && model.snapshot.files.isNotEmpty()) {
                {
                    scope.launch {
                        state.status = shareReport(android, model, context.repoTitle.ifEmpty { model.snapshot.name })
                    }
                }
            } else {
                null
            },
            color = c.textSecondary,
        )
    }
}

private suspend fun shareReport(context: Context, model: StrataModel, title: String): String? {
    val file = withContext(Dispatchers.IO) {
        runCatching {
            val folder = File(context.cacheDir, "code-insight-share")
            folder.mkdirs()
            val safe = title.replace(Regex("[^A-Za-z0-9._-]+"), "-").trim('-').ifEmpty { "repository" }
            val target = File(folder, "$safe-strata-report.html")
            target.writeText(StrataReportHtml.render(model.snapshot, model.report, title), Charsets.UTF_8)
            target
        }.getOrNull()
    } ?: return "The report could not be written."
    val started = runCatching {
        val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), file)
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/html")
            .putExtra(Intent.EXTRA_STREAM, uri)
            .putExtra(Intent.EXTRA_SUBJECT, "Strata report · $title")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        send.clipData = ClipData.newRawUri(file.name, uri)
        context.startActivity(Intent.createChooser(send, "Share the Strata report").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
    return if (started.isFailure) "No app on this phone can take the report." else null
}

private data class StrataHit(val focus: StrataFocus, val title: String, val subtitle: String, val icon: String)

private fun strataHits(model: StrataModel, query: String, limit: Int): List<StrataHit> {
    val needle = query.trim()
    if (needle.isEmpty()) return emptyList()
    val report = model.report
    val hits = ArrayList<StrataHit>()
    for ((m, node) in report.graph.nodes.withIndex()) {
        if (hits.size >= limit) break
        if (!node.path.contains(needle, ignoreCase = true) && !node.name.contains(needle, ignoreCase = true)) continue
        val score = report.modules[m]
        hits.add(
            StrataHit(
                StrataFocus.Module(m),
                node.path.ifEmpty { node.name },
                "Module · ${Strata.number(node.codeLines)} lines · rating ${Strata.ratingLetter(score.rating)} · ${Strata.formatDebt(score.debt)} debt",
                "folder",
            ),
        )
    }
    for ((f, file) in model.snapshot.files.withIndex()) {
        if (hits.size >= limit) break
        if (!file.path.contains(needle, ignoreCase = true)) continue
        val score = report.files[f]
        hits.add(
            StrataHit(
                StrataFocus.File(f),
                file.path,
                "File · rating ${Strata.ratingLetter(score.rating)} · ${Strata.formatDebt(score.debt)} debt",
                "description",
            ),
        )
    }
    return hits
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun StrataSearchSheet(model: StrataModel, state: StrataPageState) {
    var query by remember { mutableStateOf("") }
    val hits = remember(model, query) { strataHits(model, query, 80) }
    val c = Fm.colors
    FmModalBottomSheet(onDismissRequest = { state.searchOpen = false }, showDragHandle = true, isScrollControlled = true) {
        Text(
            "Find a module or file",
            style = Fm.text.titleMedium,
            color = c.textPrimary,
            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        )
        FmTextField(
            value = query,
            onValueChange = { query = it },
            hint = "Path or name",
            autofocus = true,
            leading = { FmIcon("search", size = 18.dp, tint = c.textTertiary) },
            modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4),
        )
        LazyColumn(Modifier.fillMaxWidth().heightIn(max = 480.dp)) {
            if (query.isNotBlank() && hits.isEmpty()) {
                item(key = "none") {
                    Text(
                        "Nothing matches.",
                        style = Fm.text.bodySmall,
                        color = c.textSecondary,
                        modifier = Modifier.padding(FmSpace.x4),
                    )
                }
            }
            items(hits, key = { it.focus.toString() }) { hit ->
                FmListRow(
                    title = hit.title,
                    subtitle = hit.subtitle,
                    leading = { FmIcon(hit.icon, size = 20.dp, tint = c.textTertiary) },
                    onTap = {
                        state.searchOpen = false
                        val focus = hit.focus
                        if (focus is StrataFocus.File && state.view == StrataView.Graph) {
                            val module = model.report.graph.moduleOfFile.getOrElse(focus.index) { -1 }
                            if (module >= 0 && state.drilled != module) state.drillInto(module)
                        } else if (focus is StrataFocus.Module && state.view == StrataView.Graph && state.drilled >= 0) {
                            state.drillOut()
                        }
                        state.select(focus)
                    },
                )
            }
            item(key = "bottom") { Spacer(Modifier.padding(bottom = FmSpace.x5)) }
        }
    }
}

@Composable
internal fun StrataLegendRow(content: @Composable () -> Unit) {
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) { content() }
}

@Composable
internal fun StrataLegendKey(color: androidx.compose.ui.graphics.Color, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.semantics(mergeDescendants = true) {}) {
        StrataDot(color)
        Spacer(Modifier.width(FmSpace.x1))
        Text(label, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
internal fun StrataGradientKey(low: String, high: String, colors: List<androidx.compose.ui.graphics.Color>, title: String) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.semantics(mergeDescendants = true) { contentDescription = "$title, from $low to $high" },
    ) {
        Text(title, style = Fm.text.labelSmall, color = Fm.colors.textPrimary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(low, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x1))
        Box(
            Modifier
                .width(72.dp)
                .padding(vertical = 2.dp)
                .background(androidx.compose.ui.graphics.Brush.horizontalGradient(colors), androidx.compose.foundation.shape.RoundedCornerShape(4.dp))
                .heightIn(min = 8.dp),
        )
        Spacer(Modifier.width(FmSpace.x1))
        Text(high, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
    }
}
