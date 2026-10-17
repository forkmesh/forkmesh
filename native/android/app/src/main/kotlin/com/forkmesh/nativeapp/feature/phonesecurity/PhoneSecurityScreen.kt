package com.forkmesh.nativeapp.feature.phonesecurity

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityCategory
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityFilter
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityFinding
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityScanner
import com.forkmesh.nativeapp.phonesecurity.PhoneSecurityStatus
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmCountPill
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.time.ZoneId
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class PhoneSecurityScreen : Screen() {
    override val title: String get() = "Phone Security"

    private var findings by mutableStateOf<List<PhoneSecurityFinding>?>(null)
    private var scanning by mutableStateOf(false)
    private var runs by mutableIntStateOf(0)
    private var filter by mutableStateOf(PhoneSecurityFilter.All)
    private var category by mutableStateOf<String?>(null)
    private var query by mutableStateOf("")
    private var selected by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val foreground by AppLifecycle.foreground.collectAsState()
        LaunchedEffect(runs, foreground) {
            if (!foreground && findings != null) return@LaunchedEffect
            scanning = true
            val app = context.applicationContext
            findings = withContext(Dispatchers.IO) {
                PhoneSecurityScanner.scan(PhoneSecurityProbe.read(app), System.currentTimeMillis(), ZoneId.systemDefault())
            }
            scanning = false
        }
        val all = findings.orEmpty()
        val visible = PhoneSecurityScanner.visible(all, filter, category, query)
        val counts = PhoneSecurityScanner.counts(all)
        all.firstOrNull { it.check.id == selected }?.let { finding ->
            FindingSheet(finding, onDismiss = { selected = null })
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Phone Security", navigator)
            LazyColumn(Modifier.fillMaxSize()) {
                item(key = "summary") {
                    SummaryCard(
                        findings = findings,
                        scanning = scanning,
                        summary = if (findings == null) "Checking this phone…" else PhoneSecurityScanner.filteredSummary(all, visible.size, query, filter, category),
                        onRunAgain = { runs++ },
                    )
                }
                item(key = "search") {
                    FmTextField(
                        value = query,
                        onValueChange = { query = it },
                        hint = "Search checks",
                        leading = { FmIcon("search", size = 18.dp, tint = Fm.colors.textTertiary) },
                        trailing = if (query.isEmpty()) null else {
                            { FmIcon("close", size = 18.dp, tint = Fm.colors.textTertiary, modifier = Modifier.clickable { query = "" }) }
                        },
                        modifier = Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                    )
                }
                item(key = "filters") {
                    ChipRow {
                        for (option in PhoneSecurityScanner.filters) {
                            Chip(option.displayName, counts[option] ?: 0, filter == option) { filter = option }
                        }
                    }
                }
                item(key = "categories") {
                    ChipRow {
                        Chip("All categories", null, category == null) { category = null }
                        for (name in PhoneSecurityCategory.all) {
                            Chip(name, null, category == name) { category = if (category == name) null else name }
                        }
                    }
                }
                if (findings != null && visible.isEmpty()) {
                    item(key = "empty") {
                        Text(
                            PhoneSecurityScanner.filteredSummary(all, 0, query, filter, category),
                            style = Fm.text.bodyMedium,
                            color = Fm.colors.textSecondary,
                            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                        )
                    }
                }
                items(visible, key = { it.check.id }) { finding ->
                    FindingRow(finding) { selected = finding.check.id }
                }
                item(key = "limits") {
                    Text(
                        "Phone Security reads settings, files and ForkMesh's own process on this phone. It sends nothing anywhere and changes nothing. " +
                            "An app sees only part of the system, so a well-hidden root kit can pass every check: a clean result is not proof.",
                        style = Fm.text.bodySmall,
                        color = Fm.colors.textTertiary,
                        modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x6),
                    )
                }
            }
        }
    }
}

@Composable
private fun statusColor(status: PhoneSecurityStatus): Color = when (status) {
    PhoneSecurityStatus.Critical -> Fm.colors.danger
    PhoneSecurityStatus.Warning -> Fm.colors.warning
    PhoneSecurityStatus.Review -> Fm.colors.accent
    PhoneSecurityStatus.Unavailable -> Fm.colors.textTertiary
    PhoneSecurityStatus.Pass -> Fm.colors.success
}

private fun statusIcon(status: PhoneSecurityStatus): String = when (status) {
    PhoneSecurityStatus.Critical -> "error"
    PhoneSecurityStatus.Warning -> "warning_amber"
    PhoneSecurityStatus.Review -> "info_outline"
    PhoneSecurityStatus.Unavailable -> "help_outline"
    PhoneSecurityStatus.Pass -> "check_circle"
}

@Composable
private fun SummaryCard(findings: List<PhoneSecurityFinding>?, scanning: Boolean, summary: String, onRunAgain: () -> Unit) {
    val worst = findings?.minByOrNull { it.status.rank }?.status
    val headline = when {
        findings == null -> "Checking this phone"
        worst == PhoneSecurityStatus.Critical -> "Needs attention now"
        worst == PhoneSecurityStatus.Warning -> "Some risks found"
        worst == PhoneSecurityStatus.Review -> "Worth a look"
        else -> "No problems found"
    }
    FmCard(
        Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        color = Fm.colors.bgRaised,
        padding = PaddingValues(FmSpace.x3),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            FmIcon("security", tint = if (worst == null || worst == PhoneSecurityStatus.Pass || worst == PhoneSecurityStatus.Unavailable) Fm.colors.textTertiary else statusColor(worst))
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f)) {
                Text(headline, style = Fm.text.titleMedium)
                Text(summary, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
            Spacer(Modifier.width(FmSpace.x2))
            if (scanning) {
                FmCircularProgressIndicator(Modifier.padding(FmSpace.x2))
            } else {
                FmToneButton("Run again", onPressed = onRunAgain, icon = "refresh", tooltip = "Check this phone again")
            }
        }
    }
}

@Composable
private fun ChipRow(content: @Composable () -> Unit) {
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        content()
    }
}

@Composable
private fun Chip(label: String, count: Int?, selected: Boolean, onSelected: () -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        Modifier
            .height(30.dp)
            .clip(shape)
            .background(if (selected) colors.accentSubtle else colors.bgRaised)
            .border(1.dp, if (selected) colors.accent else colors.border, shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onSelected)
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        Text(
            label,
            maxLines = 1,
            style = Fm.text.bodyMedium.copy(
                color = if (selected) colors.accent else colors.textSecondary,
                fontSize = FmType.meta,
                fontWeight = FmType.emphasis,
            ),
        )
        if (count != null) FmCountPill(count, active = selected)
    }
}

@Composable
private fun FindingRow(finding: PhoneSecurityFinding, onTap: () -> Unit) {
    FmListRow(
        finding.check.title,
        subtitle = "${finding.check.category} · ${finding.detail}",
        leading = { FmIcon(statusIcon(finding.status), tint = statusColor(finding.status)) },
        trailing = { FmStatusBadge(finding.status.displayName, statusColor(finding.status)) },
        onTap = onTap,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FindingSheet(finding: PhoneSecurityFinding, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val status = rememberFmStatus()
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x5),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(statusIcon(finding.status), tint = statusColor(finding.status))
                Spacer(Modifier.width(FmSpace.x2))
                Column(Modifier.weight(1f)) {
                    Text(finding.check.title, style = Fm.text.titleMedium)
                    Text(finding.check.category, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                FmStatusBadge(finding.status.displayName, statusColor(finding.status))
            }
            Text(finding.detail, style = Fm.text.bodyMedium)
            FmSectionHeader("What this is")
            Text(finding.check.explanation, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
            finding.usage?.let { usage ->
                FmSectionHeader("Do you need it?")
                Text(usage, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
            }
            finding.fix?.let { fix ->
                FmSectionHeader("Fix")
                Text(fix.description, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                FmToneButton(
                    fix.label,
                    onPressed = {
                        if (!PhoneSecurityFeature.open(context, fix)) status.error("This phone has no Settings page to open.")
                    },
                    icon = "open_in_new",
                    tone = FmTone.Accent,
                    filled = true,
                )
                FmInlineStatus(status)
            }
        }
    }
}
