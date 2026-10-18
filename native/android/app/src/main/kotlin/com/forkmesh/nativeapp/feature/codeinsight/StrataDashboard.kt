package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataGateStatus
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataScore
import com.forkmesh.nativeapp.codeinsight.StrataSeverity
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

private data class StrataTile(val label: String, val value: String, val note: String)

private fun compact(value: Long): String = when {
    value >= 1_000_000 -> Strata.decimal(value / 1_000_000.0, 2) + "M"
    value >= 100_000 -> Strata.decimal(value / 1000.0, 0) + "k"
    else -> Strata.number(value)
}

@Composable
internal fun StrataSurface(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Box(
        modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x1 + 2.dp)
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(Fm.colors.bgRaised)
            .padding(FmSpace.x4),
    ) { content() }
}

@Composable
internal fun StrataCardTitle(title: String, trailing: String? = null, trailingColor: androidx.compose.ui.graphics.Color? = null) {
    Row(Modifier.fillMaxWidth().padding(bottom = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = Fm.text.titleSmall, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        if (trailing != null) {
            Text(trailing, style = Fm.text.labelMedium, color = trailingColor ?: Fm.colors.textSecondary, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
internal fun StrataRankedRow(name: String, score: StrataScore, maxDebt: Double, onTap: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val color = StrataColors.rating(score.rating)
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(role = Role.Button) {
                haptics.selection()
                onTap()
            }
            .semantics(mergeDescendants = true) {
                contentDescription = "$name, rating ${Strata.ratingLetter(score.rating)}, ${Strata.formatDebt(score.debt)} debt"
            }
            .padding(vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StrataRatingChip(score.rating)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(name, style = Fm.text.bodyMedium, color = c.textPrimary, maxLines = 1, overflow = TextOverflow.MiddleEllipsis)
            Spacer(Modifier.height(4.dp))
            Box(Modifier.fillMaxWidth().height(5.dp).clip(RoundedCornerShape(3.dp)).background(c.bgOverlay)) {
                val fraction = (score.debt / maxOf(1.0, maxDebt)).coerceIn(0.04, 1.0).toFloat()
                Box(Modifier.fillMaxWidth(fraction).fillMaxHeight().clip(RoundedCornerShape(3.dp)).background(color.copy(alpha = .9f)))
            }
        }
        Spacer(Modifier.width(FmSpace.x3))
        Text(Strata.formatDebt(score.debt), style = Fm.text.labelMedium, color = c.textSecondary)
    }
}

@Composable
internal fun StrataDashboard(model: StrataModel, state: StrataPageState) {
    val report = model.report
    val metrics = report.metrics
    val tiles = remember(model) {
        listOf(
            StrataTile("Lines of code", compact(metrics.codeLines.toLong()), if (metrics.codeLines >= 100000) Strata.number(metrics.codeLines) else ""),
            StrataTile("Files", Strata.number(metrics.files), if (metrics.testFiles != 0) "${Strata.number(metrics.testFiles)} tests" else ""),
            StrataTile(
                "Modules",
                Strata.number(metrics.modules),
                if (report.graph.cycles.isEmpty()) "no cycles" else "${report.graph.cycles.sumOf { it.size }} in cycles",
            ),
            StrataTile("Types", compact(metrics.types.toLong()), ""),
            StrataTile("Functions", compact(metrics.functions.toLong()), ""),
            StrataTile("Average complexity", Strata.decimal(metrics.averageComplexity, 1), "per function"),
            StrataTile("Max complexity", Strata.number(metrics.maxComplexity), metrics.maxComplexityFunction),
            StrataTile("Comments", "${Strata.decimal(metrics.commentPercent, 1)} %", ""),
            StrataTile("Duplicated", "${Strata.decimal(metrics.duplicatedPercent, 1)} %", ""),
            StrataTile("Rule violations", Strata.number(report.total.violations), "${report.rules.count { it.violations.isNotEmpty() }} rules"),
        )
    }
    val topRules = remember(model) {
        report.rules.indices.filter { report.rules[it].violations.isNotEmpty() }
            .sortedWith { a, b -> report.rules[b].debt.compareTo(report.rules[a].debt) }
            .take(6)
    }
    val modules = remember(model) {
        report.modules.indices.filter { report.modules[it].debt > 0 }
            .sortedWith { a, b -> report.modules[b].debt.compareTo(report.modules[a].debt) }
            .take(5)
    }
    val files = remember(model) {
        report.files.indices.filter { report.files[it].debt > 0 }
            .sortedWith { a, b -> report.files[b].debt.compareTo(report.files[a].debt) }
            .take(10)
    }
    val c = Fm.colors
    LazyColumn(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = FmSpace.x2)) {
        item(key = "rating") { RatingCard(model) }
        tileRows(tiles)
        item(key = "gates") {
            StrataSurface {
                Column {
                    val failing = report.gatesWith(StrataGateStatus.Fail)
                    val warning = report.gatesWith(StrataGateStatus.Warn)
                    StrataCardTitle(
                        "Quality gates",
                        when {
                            failing != 0 -> "$failing failing"
                            warning != 0 -> "$warning warning"
                            else -> "All pass"
                        },
                        when {
                            failing != 0 -> StrataColors.unhealthy
                            warning != 0 -> StrataColors.problematic
                            else -> StrataColors.healthy
                        },
                    )
                    for (gate in report.gates) {
                        val color = StrataColors.gate(gate.status)
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .padding(vertical = FmSpace.x2)
                                .semantics(mergeDescendants = true) {
                                    contentDescription = "${gate.name}: ${gate.status.label}, ${gate.measured}. ${gate.detail}"
                                },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Row(
                                Modifier
                                    .width(64.dp)
                                    .background(color.copy(alpha = .18f), RoundedCornerShape(FmRadius.full))
                                    .padding(horizontal = FmSpace.x2, vertical = 3.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                StrataDot(color, 7.dp)
                                Spacer(Modifier.width(5.dp))
                                Text(gate.status.label, style = Fm.text.labelSmall, color = color, fontWeight = FontWeight.Bold)
                            }
                            Spacer(Modifier.width(FmSpace.x3))
                            Column(Modifier.weight(1f)) {
                                Text(gate.name, style = Fm.text.bodyMedium, color = c.textPrimary)
                                Text(gate.measured, style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            }
                        }
                    }
                }
            }
        }
        item(key = "severity") {
            StrataSurface {
                Column {
                    StrataCardTitle("Rule violations by severity")
                    val total = maxOf(1, report.total.violations)
                    Row(Modifier.fillMaxWidth().height(12.dp).clip(RoundedCornerShape(6.dp)).background(c.bgOverlay)) {
                        for (s in 0 until 4) {
                            val count = report.bySeverity[s]
                            if (count > 0) {
                                Box(Modifier.weight(count.toFloat() / total).fillMaxHeight().background(StrataColors.severity(s)))
                            }
                        }
                        val rest = total - report.bySeverity.sum()
                        if (rest > 0) Spacer(Modifier.weight(rest.toFloat() / total))
                    }
                    Spacer(Modifier.height(FmSpace.x2))
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                        for (s in StrataSeverity.entries) {
                            StrataLegendKey(StrataColors.severity(s), "${s.label} ${Strata.number(report.bySeverity[s.ordinal])}")
                        }
                    }
                    if (topRules.isNotEmpty()) Spacer(Modifier.height(FmSpace.x2))
                    for (r in topRules) {
                        val rule = report.rules[r]
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .heightIn(min = 40.dp)
                                .clip(RoundedCornerShape(FmRadius.md))
                                .clickable(role = Role.Button) { state.showRule(r) }
                                .semantics(mergeDescendants = true) {
                                    contentDescription = "${rule.title}, ${rule.violations.size} violations, ${Strata.formatDebt(rule.debt)}. Opens the rule"
                                },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            StrataDot(StrataColors.severity(rule.worst), 8.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(rule.title, style = Fm.text.bodyMedium, color = c.textPrimary, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(
                                "${Strata.number(rule.violations.size)} · ${Strata.formatDebt(rule.debt)}",
                                style = Fm.text.labelMedium,
                                color = c.textSecondary,
                            )
                        }
                    }
                }
            }
        }
        item(key = "modules") {
            StrataSurface {
                Column {
                    StrataCardTitle("Modules with the most debt")
                    if (modules.isEmpty()) Text("No debt found.", style = Fm.text.bodySmall, color = c.textSecondary)
                    val max = modules.firstOrNull()?.let { report.modules[it].debt } ?: 1.0
                    for (m in modules) {
                        StrataRankedRow(report.graph.nodes[m].path.ifEmpty { "(root)" }, report.modules[m], max) { state.select(StrataFocus.Module(m)) }
                    }
                }
            }
        }
        item(key = "files") {
            StrataSurface {
                Column {
                    StrataCardTitle("Files with the most debt")
                    if (files.isEmpty()) Text("No debt found.", style = Fm.text.bodySmall, color = c.textSecondary)
                    val max = files.firstOrNull()?.let { report.files[it].debt } ?: 1.0
                    for (f in files) {
                        StrataRankedRow(model.snapshot.files[f].path, report.files[f], max) { state.select(StrataFocus.File(f)) }
                    }
                }
            }
        }
        item(key = "end") { Spacer(Modifier.height(FmSpace.x5)) }
    }
}

@Composable
private fun RatingCard(model: StrataModel) {
    val report = model.report
    val c = Fm.colors
    val color = StrataColors.rating(report.total.rating)
    val letter = Strata.ratingLetter(report.total.rating)
    StrataSurface(
        Modifier.semantics(mergeDescendants = true) {
            contentDescription = "Debt rating $letter. Technical debt ratio ${Strata.formatPercent(report.total.ratio)}, " +
                "total debt ${Strata.formatDebt(report.total.debt)}, " +
                if (report.history) "annual interest ${Strata.formatDebt(report.total.interest)} a year" else "annual interest needs git history"
        },
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(84.dp).background(color, RoundedCornerShape(18.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Text(letter.toString(), color = StrataColors.readableOn(color), fontSize = 48.sp, fontWeight = FontWeight.Bold)
            }
            Spacer(Modifier.width(FmSpace.x4))
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text("Technical debt ratio", style = Fm.text.labelSmall, color = c.textSecondary)
                Text(Strata.formatPercent(report.total.ratio), style = Fm.text.headlineSmall, color = c.textPrimary, fontWeight = FontWeight.Bold)
                Text("Total debt", style = Fm.text.labelSmall, color = c.textSecondary)
                Text(Strata.formatDebt(report.total.debt), style = Fm.text.titleSmall, color = c.textPrimary)
                Text("Annual interest", style = Fm.text.labelSmall, color = c.textSecondary)
                Text(
                    if (report.history) "${Strata.formatDebt(report.total.interest)} a year" else "Needs git history",
                    style = Fm.text.bodyMedium,
                    color = if (report.history) c.textPrimary else c.textTertiary,
                )
            }
        }
    }
}

private fun LazyListScope.tileRows(tiles: List<StrataTile>) {
    for (row in tiles.chunked(2)) {
        item(key = "tiles:" + row.first().label) {
            Row(
                Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                for (tile in row) {
                    Column(
                        Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(FmRadius.lg))
                            .background(Fm.colors.bgRaised)
                            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)
                            .semantics(mergeDescendants = true) {
                                contentDescription = "${tile.label}: ${tile.value}" + if (tile.note.isNotEmpty()) ", ${tile.note}" else ""
                            },
                    ) {
                        Text(tile.label, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(tile.value, style = Fm.text.titleLarge, color = Fm.colors.textPrimary, fontWeight = FontWeight.Bold, maxLines = 1)
                        Text(
                            tile.note.ifEmpty { " " },
                            style = Fm.text.labelSmall,
                            color = Fm.colors.textTertiary,
                            maxLines = 1,
                            overflow = TextOverflow.MiddleEllipsis,
                        )
                    }
                }
                if (row.size == 1) Spacer(Modifier.weight(1f))
            }
        }
    }
}
