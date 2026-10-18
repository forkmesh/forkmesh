package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataCategory
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataRule
import com.forkmesh.nativeapp.codeinsight.StrataSeverity
import com.forkmesh.nativeapp.codeinsight.StrataViolation
import com.forkmesh.nativeapp.ui.kit.FmGroupHeader
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

private class StrataRuleRow(val index: Int, val rule: StrataRule, val violations: List<Int>, val debt: Double)

private class StrataRuleGroup(val category: StrataCategory, val rows: List<StrataRuleRow>)

private fun violationMatches(model: StrataModel, v: StrataViolation, text: String): Boolean {
    if (text.isEmpty()) return true
    if (v.message.contains(text, ignoreCase = true)) return true
    val files = model.snapshot.files
    if (v.file in files.indices && files[v.file].path.contains(text, ignoreCase = true)) return true
    val nodes = model.report.graph.nodes
    return v.module in nodes.indices && nodes[v.module].path.contains(text, ignoreCase = true)
}

private fun ruleGroups(model: StrataModel, query: String, severity: StrataSeverity?): List<StrataRuleGroup> {
    val text = query.trim()
    val rules = model.report.rules
    return StrataCategory.entries.mapNotNull { category ->
        val rows = rules.indices.filter { rules[it].category == category }.mapNotNull { r ->
            val rule = rules[r]
            val ruleMatch = text.isEmpty() || rule.title.contains(text, ignoreCase = true) || rule.id.contains(text, ignoreCase = true)
            val matching = rule.violations.indices.filter { i ->
                val v = rule.violations[i]
                (severity == null || v.severity == severity) && (ruleMatch || violationMatches(model, v, text))
            }
            if ((!ruleMatch || severity != null) && matching.isEmpty()) {
                null
            } else {
                StrataRuleRow(r, rule, matching, matching.sumOf { rule.violations[it].debt })
            }
        }
        if (rows.isEmpty()) null else StrataRuleGroup(category, rows)
    }
}

internal fun strataWhere(model: StrataModel, rule: StrataRule, v: StrataViolation): String {
    val files = model.snapshot.files
    val nodes = model.report.graph.nodes
    return when {
        rule.moduleScope && v.module in nodes.indices -> nodes[v.module].path.ifEmpty { nodes[v.module].name }
        v.file in files.indices -> files[v.file].path + if (v.line > 0) ":${v.line}" else ""
        v.module in nodes.indices -> nodes[v.module].path
        else -> ""
    }
}

internal fun strataOpenViolation(model: StrataModel, rule: StrataRule, v: StrataViolation, state: StrataPageState, context: CodeInsightContext) {
    val files = model.snapshot.files
    if (!rule.moduleScope && v.file in files.indices) {
        context.openFile(files[v.file].path, v.line)
    } else if (v.module >= 0) {
        state.select(StrataFocus.Module(v.module))
    }
}

@Composable
internal fun StrataRulesView(model: StrataModel, state: StrataPageState, context: CodeInsightContext) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val query = state.ruleQuery
    val severity = state.ruleSeverity
    val groups = remember(model, query, severity) { ruleGroups(model, query, severity) }
    val counts = model.report.bySeverity
    LazyColumn(Modifier.fillMaxSize()) {
        item(key = "filter") {
            Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                FmTextField(
                    value = query,
                    onValueChange = { state.ruleQuery = it },
                    hint = "Filter rules, files or messages",
                    leading = { FmIcon("search", size = 18.dp, tint = c.textTertiary) },
                    trailing = if (query.isNotEmpty()) {
                        { FmIconButton("close", "Clear the filter", onPressed = { state.ruleQuery = "" }, color = c.textTertiary) }
                    } else {
                        null
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(FmSpace.x2))
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    StrataChip(
                        "All ${Strata.number(model.report.total.violations)}",
                        severity == null,
                        { state.ruleSeverity = null },
                        description = "All severities",
                    )
                    for (s in StrataSeverity.entries) {
                        StrataChip(
                            "${s.label} ${Strata.number(counts[s.ordinal])}",
                            severity == s,
                            { state.ruleSeverity = if (severity == s) null else s },
                            description = "${s.label} violations only, ${counts[s.ordinal]}",
                        )
                    }
                }
            }
        }
        if (groups.isEmpty()) {
            item(key = "none") {
                Text(
                    "No rule or violation matches.",
                    style = Fm.text.bodySmall,
                    color = c.textSecondary,
                    modifier = Modifier.padding(FmSpace.x4),
                )
            }
        }
        for (group in groups) {
            item(key = "group-${group.category.name}") {
                FmGroupHeader(
                    "${group.category.label} · ${Strata.formatDebt(group.rows.sumOf { it.debt })}",
                    count = group.rows.sumOf { it.violations.size },
                )
            }
            for (row in group.rows) {
                val expanded = row.index in state.expandedRules
                item(key = "rule-${row.index}") {
                    val rule = row.rule
                    val color = if (rule.violations.isEmpty()) StrataColors.healthy else StrataColors.severity(rule.worst)
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 52.dp)
                            .clickable(role = Role.Button) {
                                haptics.selection()
                                state.expandedRules = if (expanded) state.expandedRules - row.index else state.expandedRules + row.index
                            }
                            .semantics(mergeDescendants = true) {
                                stateDescription = if (expanded) "Expanded" else "Collapsed"
                                contentDescription = "${rule.id} ${rule.title}, ${row.violations.size} violations, ${Strata.formatDebt(row.debt)}" +
                                    if (rule.violations.isEmpty()) ", no violations" else ", worst ${StrataSeverity.entries[rule.worst].label}"
                            }
                            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        FmIcon(if (expanded) "expand_more" else "chevron_right", size = 18.dp, tint = c.textTertiary)
                        Spacer(Modifier.width(FmSpace.x1))
                        StrataDot(color)
                        Spacer(Modifier.width(FmSpace.x2))
                        Column(Modifier.weight(1f)) {
                            Text(rule.title, style = Fm.text.bodyMedium, color = c.textPrimary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            Text(rule.id, style = Fm.text.labelSmall, color = c.textTertiary)
                        }
                        Spacer(Modifier.width(FmSpace.x2))
                        Column(horizontalAlignment = Alignment.End) {
                            Text(Strata.number(row.violations.size), style = Fm.text.labelMedium, color = c.textPrimary, fontWeight = FontWeight.SemiBold)
                            Text(Strata.formatDebt(row.debt), style = Fm.text.labelSmall, color = c.textSecondary)
                        }
                    }
                }
                if (expanded) {
                    item(key = "about-${row.index}") { StrataRuleAbout(row.rule) }
                    val shown = row.violations.take(500)
                    items(shown, key = { "v-${row.index}-$it" }) { i ->
                        val v = row.rule.violations[i]
                        val where = strataWhere(model, row.rule, v)
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .heightIn(min = 48.dp)
                                .clickable(role = Role.Button) {
                                    haptics.tap()
                                    strataOpenViolation(model, row.rule, v, state, context)
                                }
                                .semantics(mergeDescendants = true) {
                                    contentDescription = "${v.severity.label}: $where, ${v.message}, ${Strata.formatDebt(v.debt)}. " +
                                        if (!row.rule.moduleScope && v.file >= 0) "Opens the file" else "Shows the module"
                                }
                                .padding(start = FmSpace.x7, end = FmSpace.x4, top = FmSpace.x1, bottom = FmSpace.x1),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            StrataDot(StrataColors.severity(v.severity), 8.dp)
                            Spacer(Modifier.width(FmSpace.x2))
                            Column(Modifier.weight(1f)) {
                                Text(where, style = Fm.mono(color = c.accent), maxLines = 1, overflow = TextOverflow.MiddleEllipsis)
                                Text(v.message, style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            }
                            Spacer(Modifier.width(FmSpace.x2))
                            Text(Strata.formatDebt(v.debt), style = Fm.text.labelSmall, color = c.textTertiary)
                        }
                    }
                    if (row.violations.size > shown.size) {
                        item(key = "more-${row.index}") {
                            Text(
                                "and ${row.violations.size - shown.size} more — export the report for the full list",
                                style = Fm.text.bodySmall,
                                color = c.textTertiary,
                                modifier = Modifier.padding(start = FmSpace.x7, end = FmSpace.x4, top = FmSpace.x1, bottom = FmSpace.x2),
                            )
                        }
                    }
                }
            }
        }
        item(key = "end") { Spacer(Modifier.height(FmSpace.x5)) }
    }
}

@Composable
private fun StrataRuleAbout(rule: StrataRule) {
    val c = Fm.colors
    val color = if (rule.violations.isEmpty()) StrataColors.healthy else StrataColors.severity(rule.worst)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(start = FmSpace.x7, end = FmSpace.x4, bottom = FmSpace.x2)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgRaised)
            .padding(FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "${rule.id} · ${rule.category.label} · default severity ",
                style = Fm.text.labelSmall,
                color = c.textSecondary,
            )
            Text(rule.severity.label, style = Fm.text.labelSmall, color = color, fontWeight = FontWeight.SemiBold)
            Text(" · ${rule.violations.size} violations · ${Strata.formatDebt(rule.debt)}", style = Fm.text.labelSmall, color = c.textSecondary)
        }
        Text(rule.description, style = Fm.text.bodySmall, color = c.textPrimary)
        Text("How to fix. ${rule.howToFix}", style = Fm.text.bodySmall, color = c.textSecondary)
    }
}
