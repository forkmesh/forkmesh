package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataRef
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmSpace

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun StrataDetailsSheet(model: StrataModel, state: StrataPageState, context: CodeInsightContext) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = false)
    var history by remember { mutableStateOf(listOf<StrataFocus>()) }
    val focus = state.selection ?: return
    val go: (StrataFocus) -> Unit = { next ->
        if (next != focus) {
            history = history + focus
            state.select(next)
        }
    }
    FmModalBottomSheet(
        onDismissRequest = { state.sheetOpen = false },
        sheetState = sheetState,
        showDragHandle = true,
        isScrollControlled = true,
    ) {
        LazyColumn(Modifier.fillMaxWidth().heightIn(max = 760.dp)) {
            if (history.isNotEmpty()) {
                item(key = "back") {
                    Row(Modifier.padding(horizontal = FmSpace.x4)) {
                        FmToneButton("Back", onPressed = {
                            val previous = history.last()
                            history = history.dropLast(1)
                            state.select(previous)
                        }, icon = "arrow_back", tooltip = "Back to the previous details", height = 30.dp)
                    }
                }
            }
            when (focus) {
                is StrataFocus.Module -> moduleDetails(model, focus.index, state, context, go)
                is StrataFocus.File -> fileDetails(model, focus.index, context, go)
                is StrataFocus.Cell -> cellDetails(model, focus.from, focus.to, context)
            }
            item(key = "end") { Spacer(Modifier.height(FmSpace.x6)) }
        }
    }
}

@Composable
private fun SheetTitle(title: String, subtitle: String?) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
        Text(title, style = Fm.text.titleMedium, color = c.textPrimary, modifier = Modifier.semantics { heading() })
        if (subtitle != null) Text(subtitle, style = Fm.text.bodySmall, color = c.textSecondary)
    }
}

@Composable
private fun ScoreLine(rating: Int, ratio: Double, debt: Double) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1).semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StrataRatingChip(rating, description = "Rating ${Strata.ratingLetter(rating)}")
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            "${Strata.formatPercent(ratio)} debt ratio · ${Strata.formatDebt(debt)} debt",
            style = Fm.text.bodyMedium,
            color = Fm.colors.textPrimary,
        )
    }
}

@Composable
private fun Fact(label: String, value: String, dot: Color? = null) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = 3.dp).semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = Fm.text.bodySmall, color = c.textSecondary, modifier = Modifier.width(128.dp))
        if (dot != null) {
            StrataDot(dot, 8.dp)
            Spacer(Modifier.width(FmSpace.x1))
        }
        Text(value, style = Fm.text.bodySmall, color = c.textPrimary)
    }
}

@Composable
private fun SectionTitle(title: String, count: Int? = null, color: Color? = null) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1)) {
        Text(
            title,
            style = Fm.text.titleSmall,
            color = color ?: c.textPrimary,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.semantics { heading() },
        )
        if (count != null) {
            Spacer(Modifier.width(FmSpace.x2))
            Text("$count", style = Fm.text.titleSmall, color = c.textTertiary)
        }
    }
}

@Composable
private fun LinkRow(
    title: String,
    subtitle: String? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    description: String,
    onTap: () -> Unit,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .clickable(role = Role.Button) {
                haptics.selection()
                onTap()
            }
            .semantics(mergeDescendants = true) { contentDescription = description }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(FmSpace.x2))
        }
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyMedium, color = c.accent, maxLines = 2, overflow = TextOverflow.MiddleEllipsis)
            if (subtitle != null) Text(subtitle, style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 3, overflow = TextOverflow.Ellipsis)
        }
        if (trailing != null) {
            Spacer(Modifier.width(FmSpace.x2))
            trailing()
        }
    }
}

@Composable
private fun MutedLine(text: String) {
    Text(text, style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
}

@Composable
private fun ShowMore(total: Int, shown: Int, onTap: () -> Unit) {
    Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1)) {
        FmToneButton("Show all $total", onPressed = onTap, icon = "expand_more", tooltip = "Show all $total, ${total - shown} more", height = 30.dp)
    }
}

private fun violationRows(
    scope: LazyListScope,
    key: String,
    model: StrataModel,
    refs: List<StrataRef>,
    fileNames: Boolean,
    context: CodeInsightContext,
    go: (StrataFocus) -> Unit,
) {
    val report = model.report
    scope.items(refs, key = { "$key-${it.rule}-${it.index}" }) { ref ->
        val rule = report.rules[ref.rule]
        val v = report.violation(ref)
        val files = model.snapshot.files
        val fileLink = !rule.moduleScope && v.file in files.indices
        val where = if (fileLink) Strata.fileName(files[v.file].path) + if (v.line > 0) ":${v.line}" else "" else ""
        val title = if (fileNames) rule.title else v.message
        val subtitle = if (fileNames) {
            listOf(where, v.message, Strata.formatDebt(v.debt)).filter { it.isNotEmpty() }.joinToString(" · ")
        } else {
            "${rule.title} · ${Strata.formatDebt(v.debt)}"
        }
        LinkRow(
            title = title,
            subtitle = subtitle,
            leading = { StrataDot(StrataColors.severity(v.severity), 8.dp) },
            description = "${v.severity.label}: $title, $subtitle. " + if (fileLink) "Opens the file" else "Shows the module",
            onTap = {
                if (fileLink) context.openFile(files[v.file].path, v.line) else if (v.module >= 0) go(StrataFocus.Module(v.module))
            },
        )
    }
}

private fun LazyListScope.moduleDetails(
    model: StrataModel,
    module: Int,
    state: StrataPageState,
    context: CodeInsightContext,
    go: (StrataFocus) -> Unit,
) {
    val report = model.report
    val graph = report.graph
    val node = graph.nodes.getOrNull(module) ?: return
    val score = report.modules[module]
    val files = model.snapshot.files
    item(key = "module-head") {
        Column {
            SheetTitle(node.name, if (node.path != node.name) node.path.ifEmpty { "(root)" } else null)
            ScoreLine(score.rating, score.ratio, score.debt)
            if (node.cycle >= 0) {
                Text(
                    "In a dependency cycle with ${(graph.cycles.getOrNull(node.cycle)?.size ?: 1) - 1} other modules.",
                    style = Fm.text.bodySmall,
                    color = StrataColors.unhealthy,
                    modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                )
            }
            Spacer(Modifier.height(FmSpace.x2))
            Fact("Lines of code", Strata.number(node.codeLines))
            Fact("Files", Strata.number(node.files.size))
            Fact("Functions", Strata.number(node.functions))
            Fact("Complexity", Strata.number(node.complexity))
            Fact("Types", "${node.types} (${node.abstractTypes} abstract)")
            Fact("Health", Strata.fixed(node.health, 1), StrataColors.health(node.health))
            Fact("Afferent Ca", node.afferent.toString())
            Fact("Efferent Ce", node.efferent.toString())
            Fact("Instability I", Strata.fixed(node.instability, 2))
            Fact("Abstractness A", Strata.fixed(node.abstractness, 2))
            Fact("Distance D", Strata.fixed(node.distance, 2), StrataColors.scale(node.distance))
            Fact("Cohesion", Strata.fixed(node.cohesion, 2))
            if (node.commits > 0) Fact("Commits", Strata.number(node.commits))
            if (report.history) Fact("Interest", "${Strata.formatDebt(score.interest)} a year")
            Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton(
                    "Show its files in the graph",
                    onPressed = {
                        state.drillInto(module)
                        state.select(StrataFocus.Module(module), open = false)
                    },
                    icon = "account_tree",
                    tone = FmTone.Accent,
                )
            }
        }
    }
    val worst = report.worstForModule(module, 6)
    if (worst.isNotEmpty()) {
        item(key = "module-worst") { SectionTitle("Worst rules") }
        violationRows(this, "worst", model, worst, true, context, go)
    }
    val uses = graph.edges.filter { it.from == module && it.to != module }.sortedByDescending { it.weight }
    val usedBy = graph.edges.filter { it.to == module && it.from != module }.sortedByDescending { it.weight }
    for ((title, list, outgoing) in listOf(Triple("Uses", uses, true), Triple("Used by", usedBy, false))) {
        item(key = "module-$title") {
            SectionTitle(title, list.size, if (outgoing) Fm.colors.accent else strataDependent())
            if (list.isEmpty()) MutedLine("None")
        }
        items(list, key = { "module-$title-${it.from}-${it.to}" }) { edge ->
            val other = if (outgoing) edge.to else edge.from
            val otherNode = graph.nodes[other]
            val cycle = otherNode.cycle >= 0 && otherNode.cycle == node.cycle
            LinkRow(
                title = otherNode.name,
                subtitle = "${edge.weight} imports" + if (cycle) " · cycle" else "",
                trailing = {
                    FmToneButton("Imports", onPressed = { go(StrataFocus.Cell(edge.from, edge.to)) }, tooltip = "List the file imports from ${graph.nodes[edge.from].name} to ${graph.nodes[edge.to].name}", height = 30.dp)
                },
                description = "${otherNode.name}, ${edge.weight} imports" + if (cycle) ", in the same cycle" else "",
                onTap = { go(StrataFocus.Module(other)) },
            )
        }
    }
    val ranked = node.files.sortedWith { a, b -> report.files[b].debt.compareTo(report.files[a].debt) }
    item(key = "module-files") {
        SectionTitle("Files", ranked.size)
    }
    item(key = "module-files-list") {
        var all by remember(module) { mutableStateOf(false) }
        val shown = if (all) ranked else ranked.take(12)
        Column {
            for (f in shown) {
                val fileScore = report.files[f]
                LinkRow(
                    title = Strata.fileName(files[f].path),
                    subtitle = "${Strata.number(files[f].codeLines)} LOC" + if (fileScore.debt > 0) " · ${Strata.formatDebt(fileScore.debt)} debt" else "",
                    leading = { StrataRatingChip(fileScore.rating, 20.dp) },
                    description = "${files[f].path}, rating ${Strata.ratingLetter(fileScore.rating)}, ${Strata.formatDebt(fileScore.debt)} debt",
                    onTap = { go(StrataFocus.File(f)) },
                )
            }
            if (!all && ranked.size > shown.size) ShowMore(ranked.size, shown.size) { all = true }
        }
    }
}

private fun LazyListScope.fileDetails(model: StrataModel, file: Int, context: CodeInsightContext, go: (StrataFocus) -> Unit) {
    val report = model.report
    val files = model.snapshot.files
    val info = files.getOrNull(file) ?: return
    val score = report.files[file]
    val module = report.graph.moduleOfFile.getOrElse(file) { -1 }
    item(key = "file-head") {
        Column {
            SheetTitle(Strata.fileName(info.path), info.path)
            ScoreLine(score.rating, score.ratio, score.debt)
            Row(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                FmToneButton("Open the file", onPressed = { context.openFile(info.path, 0) }, icon = "open_in_new", tone = FmTone.Accent)
            }
            if (module >= 0) {
                LinkRow(
                    title = report.graph.nodes[module].name,
                    subtitle = "Module",
                    description = "Module ${report.graph.nodes[module].name}",
                    onTap = { go(StrataFocus.Module(module)) },
                )
            }
            Fact("Language", info.language)
            Fact("Lines of code", "${Strata.number(info.codeLines)} (${Strata.number(info.commentLines)} comment lines)")
            Fact("Functions", Strata.number(info.functions.size))
            Fact("Complexity", "${info.complexity} · max ${info.maxComplexity}")
            Fact("Max nesting", info.maxNesting.toString())
            Fact("Health", Strata.fixed(info.health, 1), StrataColors.health(info.health))
            Fact("Imports", "${info.dependsOn.size} files · ${info.externalImports.size} packages")
            Fact("Imported by", "${info.dependedOnBy.size} files")
            if (info.duplicatedLines > 0) Fact("Duplicated", "${info.duplicatedLines} lines")
            if (info.commits > 0) {
                Fact("Commits", "${info.commits} (${info.recentCommits} recent, ${info.fixCommits} fixes)")
                Fact("Authors", "${info.authors} · main ${info.mainAuthor} (${Strata.formatPercent(info.mainAuthorShare)})")
                Fact("Interest", "${Strata.formatDebt(score.interest)} a year")
            }
            if (info.healthIssues.isNotEmpty()) {
                SectionTitle("Health")
                for (issue in info.healthIssues) MutedLine("• $issue")
            }
        }
    }
    val refs = report.worstForFile(file)
    if (refs.isNotEmpty()) {
        item(key = "file-violations") { SectionTitle("Rule violations", refs.size) }
        violationRows(this, "file", model, refs, false, context, go)
    }
    for ((title, list, outgoing) in listOf(Triple("Uses", info.dependsOn, true), Triple("Used by", info.dependedOnBy, false))) {
        if (list.isEmpty()) continue
        item(key = "file-$title") {
            SectionTitle(title, list.size, if (outgoing) Fm.colors.accent else strataDependent())
            var all by remember(file) { mutableStateOf(false) }
            val shown = if (all) list else list.take(15)
            Column {
                for (f in shown) {
                    LinkRow(title = files[f].path, description = files[f].path, onTap = { go(StrataFocus.File(f)) })
                }
                if (!all && list.size > shown.size) ShowMore(list.size, shown.size) { all = true }
            }
        }
    }
}

private fun LazyListScope.cellDetails(model: StrataModel, from: Int, to: Int, context: CodeInsightContext) {
    val graph = model.report.graph
    val files = model.snapshot.files
    if (from !in graph.nodes.indices || to !in graph.nodes.indices) return
    item(key = "cell-head") { SheetTitle("Dependencies", null) }
    if (from == to) {
        item(key = "cell-self") {
            Text(
                "${graph.nodes[from].name} has ${graph.nodes[from].internalEdges} imports between its own files.",
                style = Fm.text.bodyMedium,
                color = Fm.colors.textPrimary,
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            )
        }
        return
    }
    for ((a, b) in listOf(from to to, to to from)) {
        val edge = graph.edges.getOrNull(graph.edgeIndex(a, b))
        val forward = a == from
        item(key = "cell-$a-$b") {
            SectionTitle(
                "${graph.nodes[a].name} → ${graph.nodes[b].name}",
                null,
                if (forward) Fm.colors.accent else strataDependent(),
            )
            MutedLine(if (edge == null) "No imports in this direction." else "${edge.weight} imports")
        }
        if (edge != null) {
            val pairs = edge.fileEdges.filter { it.first in files.indices && it.second in files.indices }
            items(pairs.take(60), key = { "cell-$a-$b-${it.first}-${it.second}" }) { (fa, fb) ->
                Column(Modifier.fillMaxWidth()) {
                    LinkRow(
                        title = files[fa].path,
                        subtitle = "imports ${files[fb].path}",
                        trailing = {
                            FmToneButton("Open", onPressed = { context.openFile(files[fb].path, 0) }, tooltip = "Open ${files[fb].path}", height = 30.dp)
                        },
                        description = "${files[fa].path} imports ${files[fb].path}. Opens ${files[fa].path}",
                        onTap = { context.openFile(files[fa].path, 0) },
                    )
                }
            }
            if (pairs.size > 60) item(key = "cell-more-$a-$b") { MutedLine("and ${pairs.size - 60} more") }
        }
    }
}
