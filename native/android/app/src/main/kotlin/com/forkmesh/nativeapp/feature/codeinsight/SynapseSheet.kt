package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Insight
import com.forkmesh.nativeapp.codeinsight.Synapse
import com.forkmesh.nativeapp.codeinsight.SynapseColours
import com.forkmesh.nativeapp.codeinsight.SynapseScene
import com.forkmesh.nativeapp.codeinsight.SynapseShowMode
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.text.NumberFormat
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SynapseDetailsSheet(state: SynapsePageState, context: CodeInsightContext, onDismiss: () -> Unit) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    FmModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState, showDragHandle = true, isScrollControlled = true) {
        val selected = state.selected
        LazyColumn(Modifier.fillMaxWidth().fillMaxHeight(0.92f).navigationBarsPadding()) {
            if (state.sheet == SynapseSheet.File && selected >= 0) {
                fileContent(state, context, selected, onDismiss)
            } else {
                overviewContent(state, onDismiss)
            }
            item { Spacer(Modifier.height(FmSpace.x6)) }
        }
    }
}

private val numbers: NumberFormat get() = NumberFormat.getIntegerInstance()

private fun LazyListScope.section(title: String, count: Int? = null) {
    item(key = "section-$title") {
        FmSectionHeader(
            title,
            modifier = Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1).semantics { heading() },
            count = count,
        )
    }
}

@Composable
private fun SynapseFileRow(path: String, extra: String?, onClick: () -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .clickable(onClickLabel = "Select and fly to") { onClick() }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        verticalArrangement = Arrangement.Center,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                Synapse.fileName(path),
                style = Fm.text.bodyMedium,
                color = Fm.colors.accent,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f, fill = false),
            )
            if (extra != null) {
                Spacer(Modifier.width(FmSpace.x2))
                Text(extra, style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular), color = Fm.colors.textSecondary)
            }
        }
        Text(path, style = Fm.text.bodySmall, color = Fm.colors.textTertiary, maxLines = 1, overflow = TextOverflow.MiddleEllipsis)
    }
}

@Composable
private fun SynapseMore(count: Int) {
    Text(
        "and ${numbers.format(count)} more",
        style = Fm.text.bodySmall,
        color = Fm.colors.textSecondary,
        modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    )
}

@Composable
private fun SynapseNote(text: String, color: Color? = null) {
    Text(
        text,
        style = Fm.text.bodySmall,
        color = color ?: Fm.colors.textSecondary,
        modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
    )
}

private fun LazyListScope.fileList(key: String, state: SynapsePageState, files: List<Int>, cap: Int, extra: ((Int) -> String)?, onPick: (Int) -> Unit) {
    val shown = files.take(cap)
    items(shown, key = { "$key-$it" }) { file ->
        SynapseFileRow(state.snapshot.files[file].path, extra?.invoke(file)) { onPick(file) }
    }
    if (files.size > cap) item(key = "$key-more") { SynapseMore(files.size - cap) }
}

@OptIn(ExperimentalLayoutApi::class)
private fun LazyListScope.fileContent(state: SynapsePageState, context: CodeInsightContext, selected: Int, onDismiss: () -> Unit) {
    val snapshot = state.snapshot
    val file = snapshot.files[selected]
    val pick = { target: Int -> state.select(target, fly = true) }
    item(key = "file-head") {
        val haptics = FmHaptics.current
        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Text(Synapse.fileName(file.path), style = Fm.text.titleMedium, modifier = Modifier.semantics { heading() })
            SelectionContainer { Text(file.path, style = Fm.mono(color = Fm.colors.textSecondary)) }
            Row(verticalAlignment = Alignment.CenterVertically) {
                val facts = buildList {
                    if (file.language.isNotEmpty()) add(file.language)
                    add("${numbers.format(file.codeLines)} lines")
                }
                Text(facts.joinToString(" · ") + " · ", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                Text(
                    "health ${String.format(Locale.getDefault(), "%.1f", file.health)}/10",
                    style = Fm.text.bodySmall,
                    color = Color(SynapseColours.health(file.health)),
                    modifier = Modifier.semantics { contentDescription = "Health ${String.format(Locale.getDefault(), "%.1f", file.health)} of 10, ${Insight.healthBand(file.health).label}" },
                )
                if (file.test) Text(" · test", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
            val graph = state.graph
            val node = graph?.nodeFor(selected) ?: -1
            if (graph != null && node >= 0) {
                Text("Module ${graph.lobes[graph.nodes[node].lobe].name}", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
            Spacer(Modifier.height(FmSpace.x1))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Open file", onPressed = { context.openFile(file.path, 1) }, icon = "open_in_new", tone = FmTone.Accent, filled = true)
                FmToneButton(
                    "Fly to",
                    onPressed = if (state.canFlyTo(selected)) ({
                        haptics.tap()
                        state.flyToFile(selected)
                        onDismiss()
                    }) else null,
                    icon = "navigation",
                )
                FmToneButton("Clear", onPressed = {
                    state.clearSelection()
                    onDismiss()
                }, icon = "close", tooltip = "Clear the selection")
            }
        }
    }
    if (file.cycle >= 0 && file.cycle < snapshot.cycles.size) {
        section("Circular dependency")
        item(key = "file-loop") {
            val members = snapshot.cycles[file.cycle]
            val loop = Synapse.cyclePath(snapshot, members)
            val red = Color(SynapseColours.CYCLE)
            Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                FlowRow(
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                    modifier = Modifier.semantics { contentDescription = "Loop: " + Synapse.cycleText(snapshot, loop) },
                ) {
                    Box(Modifier.padding(end = FmSpace.x1).size(8.dp).clip(CircleShape).background(red).align(Alignment.CenterVertically))
                    for ((step, member) in loop.withIndex()) {
                        if (step > 0) Text(" → ", style = Fm.text.bodyMedium, color = red)
                        Text(
                            Synapse.fileName(snapshot.files[member].path),
                            style = Fm.text.bodyMedium.copy(fontWeight = if (member == selected) FontWeight.SemiBold else FontWeight.Normal),
                            color = Fm.colors.accent,
                            modifier = Modifier.clickable(onClickLabel = "Select ${snapshot.files[member].path}") { pick(member) },
                        )
                    }
                }
                if (members.size > maxOf(0, loop.size - 1)) {
                    Text("${numbers.format(members.size)} files take part in this loop", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                val graph = state.graph
                val graphCycle = if (graph != null) SynapseScene.graphCycleOf(graph, file.cycle) else -1
                if (graphCycle >= 0) {
                    FmToneButton("Show only this loop", onPressed = {
                        state.isolateCycle(graphCycle)
                        onDismiss()
                    }, icon = "loop", tone = FmTone.Danger)
                }
            }
        }
    }
    val imports = file.dependsOn.distinct().sortedBy { snapshot.files[it].path }
    val importers = file.dependedOnBy.distinct().sortedBy { snapshot.files[it].path }
    section("Imports", imports.size)
    if (imports.isEmpty()) item(key = "imports-none") { SynapseNote("Imports no file of this repository.") }
    fileList("imports", state, imports, 60, null, pick)
    section("Imported by", importers.size)
    if (importers.isEmpty()) item(key = "importers-none") { SynapseNote("No file imports it.") }
    fileList("importers", state, importers, 60, null, pick)
    val packages = file.externalImports.distinct().sortedWith(String.CASE_INSENSITIVE_ORDER)
    if (packages.isNotEmpty()) {
        section("External packages", packages.size)
        items(packages.take(40), key = { "package-$it" }) { name ->
            Text(name, style = Fm.mono(color = Fm.colors.textSecondary), modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = 2.dp))
        }
        if (packages.size > 40) item(key = "packages-more") { SynapseMore(packages.size - 40) }
    }
}

@Composable
private fun SynapseStat(value: Int, label: String, color: Color?, modifier: Modifier) {
    Column(modifier.semantics(mergeDescendants = true) {}) {
        Text(numbers.format(value), style = Fm.text.titleLarge.copy(fontFeatureSettings = FmType.tabular), color = color ?: Fm.colors.textPrimary)
        Text(label, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
    }
}

private fun LazyListScope.overviewContent(state: SynapsePageState, onDismiss: () -> Unit) {
    val snapshot = state.snapshot
    val graph = state.graph
    val pick = { file: Int ->
        state.select(file, fly = true)
        state.sheet = SynapseSheet.File
    }
    item(key = "overview-head") {
        val connected = graph?.nodes?.count { it.fanIn + it.fanOut > 0 } ?: 0
        val red = Color(SynapseColours.CYCLE)
        Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Text("Overview", style = Fm.text.titleMedium, modifier = Modifier.semantics { heading() })
            Row(Modifier.fillMaxWidth()) {
                SynapseStat(connected, "connected files", null, Modifier.weight(1f))
                SynapseStat(state.importCount, "imports", null, Modifier.weight(1f))
                SynapseStat(snapshot.cycles.size, "circular dependencies", if (snapshot.cycles.isEmpty()) null else red, Modifier.weight(1f))
            }
            Row(Modifier.fillMaxWidth()) {
                SynapseStat(state.orphans.size, "orphans", null, Modifier.weight(1f))
                SynapseStat(state.leaves.size, "leaves", null, Modifier.weight(1f))
                SynapseStat(graph?.lobes?.size ?: 0, "modules", null, Modifier.weight(1f))
            }
            if (state.isolatedCycle >= 0 || state.showMode != SynapseShowMode.Everything) {
                FmToneButton("Show everything again", onPressed = {
                    state.showEverything()
                    onDismiss()
                }, icon = "hub", tone = FmTone.Accent)
            }
        }
    }
    if (graph != null && graph.cycles.isNotEmpty()) {
        section("Circular dependencies", graph.cycles.size)
        val order = SynapseScene.cycleOrder(graph)
        items(order.take(40), key = { "cycle-$it" }) { c ->
            val info = graph.cycles[c]
            val text = Synapse.cycleText(snapshot, info.path).ifEmpty { "${info.files.size} files" }
            val active = state.isolatedCycle == c
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 44.dp)
                    .clickable(onClickLabel = "Show only this loop") {
                        state.isolateCycle(c)
                        onDismiss()
                    }
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(Color(SynapseColours.CYCLE)))
                Spacer(Modifier.width(FmSpace.x2))
                Text(
                    text,
                    style = Fm.text.bodyMedium.copy(fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal),
                    color = Fm.colors.accent,
                    modifier = Modifier.weight(1f, fill = false).wrapContentHeight(),
                )
                if (info.files.size > maxOf(0, info.path.size - 1)) {
                    Spacer(Modifier.width(FmSpace.x2))
                    Text("${numbers.format(info.files.size)} files", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
            }
        }
        if (order.size > 40) item(key = "cycles-more") { SynapseMore(order.size - 40) }
    }
    val imported = Synapse.mostImported(snapshot, 10)
    if (imported.isNotEmpty()) {
        section("Most imported")
        fileList("imported", state, imported, 10, { "· ${numbers.format(snapshot.files[it].dependedOnBy.size)}" }, pick)
    }
    val importing = Synapse.mostImporting(snapshot, 10)
    if (importing.isNotEmpty()) {
        section("Most importing")
        fileList("importing", state, importing, 10, { "· ${numbers.format(snapshot.files[it].dependsOn.size)}" }, pick)
    }
    if (state.orphans.isNotEmpty()) {
        section("Orphans", state.orphans.size)
        item(key = "orphans-hint") {
            SynapseNote("Files no other file imports and that import nothing here. Show → Orphans places them in the brain.")
        }
        fileList("orphans", state, state.orphans, 30, null, pick)
    }
    if (snapshot.cycles.isEmpty() && state.importCount > 0) {
        section("Circular dependencies")
        item(key = "cycles-none") { SynapseNote("None. Every import points one way.", Color(SynapseColours.health(10.0))) }
    }
}
