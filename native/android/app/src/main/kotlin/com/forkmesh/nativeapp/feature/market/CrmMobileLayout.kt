package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.settings.LayoutModeTrack
import com.forkmesh.nativeapp.feature.account.settings.ThemeModeTrack
import com.forkmesh.nativeapp.feature.account.settings.ThemeSegmentTrack
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmContactRef
import com.forkmesh.nativeapp.market.CrmDashboard
import com.forkmesh.nativeapp.market.CrmEmailDraft
import com.forkmesh.nativeapp.market.CrmFlow
import com.forkmesh.nativeapp.market.CrmFlowDraft
import com.forkmesh.nativeapp.market.CrmFlowIcon
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmNextContact
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.market.CrmSection as CrmArea
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmTabItem
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.market.PackageMenu
import com.forkmesh.nativeapp.market.contactById
import com.forkmesh.nativeapp.market.contactTabs
import com.forkmesh.nativeapp.market.next
import com.forkmesh.nativeapp.market.stats
import com.forkmesh.nativeapp.market.tabForFlow
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.settings.LayoutModeChoice
import com.forkmesh.nativeapp.settings.ThemeModeChoice
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon

@Composable
internal fun crmDense(): Boolean = AppearanceStore.appearance.collectAsState().value.layoutMode.dense

private fun LayoutModeChoice.crmIcon(): String = if (this == LayoutModeChoice.controlTower) "grid_view" else "view_agenda"

private fun ThemeModeChoice.crmIcon(): String = when (this) {
    ThemeModeChoice.system -> "contrast"
    ThemeModeChoice.light -> "light_mode_outlined"
    ThemeModeChoice.dark -> "dark_mode_outlined"
}

@Composable
internal fun CrmSectionBar(model: CrmModel, data: CrmOrganizationData) {
    val haptics = FmHaptics.current
    Row(
        Modifier.padding(horizontal = 16.dp).fillMaxWidth().fmGlass(RoundedCornerShape(50), shadow = false).padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        CrmArea.entries.forEach { section ->
            val active = model.section == section
            val badge = if (section == CrmArea.CONTACTS && data.contacts.isNotEmpty()) data.contacts.size else null
            val tint = if (active) Color.White else Fm.colors.textPrimary.copy(alpha = 0.75f)
            Column(
                Modifier.weight(1f).heightIn(min = 40.dp).clip(RoundedCornerShape(50))
                    .background(if (active) Fm.colors.accent else Color.Transparent)
                    .testTag("crm-section-${section.name.lowercase()}")
                    .semantics(mergeDescendants = true) {
                        selected = active
                        contentDescription = badge?.let { "${section.title}, $it" } ?: section.title
                    }
                    .combinedClickableCompat({
                        haptics.selection()
                        model.show(section)
                        CrmRevision.bump()
                    })
                    .padding(vertical = 3.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                FmIcon(section.icon, size = 18.dp, tint = tint, outlined = !active)
                Text(
                    section.title, maxLines = 1,
                    style = Fm.text.labelSmall.copy(fontSize = 10.sp, fontWeight = if (active) FontWeight.SemiBold else FontWeight.Medium),
                    color = tint,
                )
            }
        }
    }
}

@Composable
internal fun CrmTabChips(model: CrmModel, items: List<CrmTabItem>) {
    val haptics = FmHaptics.current
    val state = rememberLazyListState()
    LaunchedEffect(items.size) {
        val index = items.indexOfFirst { it.tab == model.activeTab }
        if (index > 0) state.scrollToItem(index)
    }
    LazyRow(state = state, horizontalArrangement = Arrangement.spacedBy(6.dp), contentPadding = PaddingValues(horizontal = 16.dp)) {
        items(items, key = { it.tab.id }) { item ->
            val active = model.activeTab == item.tab
            val accent = Fm.colors.accent
            Row(
                Modifier.height(30.dp).clip(RoundedCornerShape(50))
                    .background(if (active) accent.copy(alpha = 0.16f) else Fm.colors.textSecondary.copy(alpha = 0.1f))
                    .semantics { selected = active }
                    .combinedClickableCompat({
                        haptics.selection()
                        model.select(item.tab)
                        CrmRevision.bump()
                        (item.tab as? com.forkmesh.nativeapp.market.CrmTab.Sheet)?.let { sheet -> Thread { model.recordListView(sheet.viewId) }.start() }
                    })
                    .padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                val color = if (active) accent else Fm.colors.textPrimary
                FmIcon(item.icon, size = 14.dp, tint = color)
                Text(item.title, maxLines = 1, style = Fm.text.bodySmall.copy(fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal), color = color)
                item.count?.let { count ->
                    Text("$count", style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold), color = color.copy(alpha = 0.75f))
                }
            }
        }
    }
}

@Composable
internal fun CrmModeMenu() {
    val appearance by AppearanceStore.appearance.collectAsState()
    val haptics = FmHaptics.current
    var open by remember { mutableStateOf(false) }
    Box(Modifier.testTag("crm-mode-menu")) {
        FmIconButton(appearance.layoutMode.crmIcon(), appearance.layoutMode.label, onPressed = { open = true }, color = Fm.colors.textPrimary)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            LayoutModeChoice.entries.forEach { mode ->
                FmDropdownMenuItem(mode.label, onClick = {
                    open = false
                    haptics.selection()
                    AppearanceStore.setLayoutMode(mode)
                }, icon = if (mode == appearance.layoutMode) "check" else mode.crmIcon())
            }
            ThemeModeChoice.entries.forEach { theme ->
                FmDropdownMenuItem(theme.label, onClick = {
                    open = false
                    haptics.selection()
                    AppearanceStore.setThemeMode(theme)
                }, icon = if (theme == appearance.themeMode) "check" else theme.crmIcon())
            }
        }
    }
}

@Composable
internal fun CrmModeSwitcher() {
    val appearance by AppearanceStore.appearance.collectAsState()
    val haptics = FmHaptics.current
    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(Modifier.testTag("crm-layout-mode")) {
            LayoutModeTrack(appearance.layoutMode) {
                haptics.selection()
                AppearanceStore.setLayoutMode(it)
            }
        }
        Text(appearance.layoutMode.note, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        Box(Modifier.testTag("crm-theme-mode")) {
            ThemeModeTrack(appearance.themeMode) {
                haptics.selection()
                AppearanceStore.setThemeMode(it)
            }
        }
    }
}

@Composable
private fun CrmKpiTile(value: String, label: String, icon: String, tint: Color, detail: String, dense: Boolean, modifier: Modifier, onTap: (() -> Unit)? = null) {
    val shape = RoundedCornerShape(14.dp)
    val fill = if (dense) {
        Modifier.background(Brush.verticalGradient(listOf(tint, tint.copy(alpha = 0.82f))), shape)
    } else {
        Modifier.background(Fm.colors.bgRaised, shape)
    }
    Column(
        modifier.clip(shape).then(fill).let { if (onTap != null) it.combinedClickableCompat(onTap) else it }
            .semantics(mergeDescendants = true) {}
            .padding(horizontal = 12.dp, vertical = if (dense) 9.dp else 14.dp),
        verticalArrangement = Arrangement.spacedBy(if (dense) 2.dp else 4.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            FmIcon(icon, size = 12.dp, tint = if (dense) Color.White.copy(alpha = 0.9f) else tint)
            Text(label, maxLines = 1, style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Medium), color = if (dense) Color.White.copy(alpha = 0.9f) else tint)
        }
        Text(
            value, maxLines = 1, overflow = TextOverflow.Ellipsis,
            style = (if (dense) Fm.text.titleMedium else Fm.text.titleLarge).copy(fontWeight = FontWeight.Bold),
            color = if (dense) Color.White else Fm.colors.textPrimary,
        )
        if (detail.isNotEmpty()) {
            Text(detail, maxLines = 1, style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Medium), color = if (dense) Color.White.copy(alpha = 0.85f) else Fm.colors.textSecondary)
        }
    }
}

@Composable
private fun CrmPipelineBar(model: CrmModel, flow: CrmFlow, contacts: List<CrmContact>, onOpen: () -> Unit) {
    val haptics = FmHaptics.current
    val stats = flow.stats(contacts)
    val total = maxOf(1, contacts.size)
    CrmCard(Modifier.fillMaxWidth().combinedClickableCompat(onOpen)) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(CrmFlowIcon.symbol(flow.icon), size = 16.dp, tint = Fm.colors.textPrimary)
                Spacer(Modifier.width(6.dp))
                Text(flow.name, Modifier.weight(1f), maxLines = 1, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                Text(CrmText.money(stats.sumOf { it.value }), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
            }
            Row(Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(50)).background(Fm.colors.textSecondary.copy(alpha = 0.12f)), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                stats.filter { it.count > 0 }.forEach { stat ->
                    Box(Modifier.weight(stat.count.toFloat() / total).fillMaxHeight().background(CrmColors.stage(stat.stage, flow)))
                }
                val rest = 1f - stats.sumOf { it.count }.toFloat() / total
                if (rest > 0.001f) Spacer(Modifier.weight(rest))
            }
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                stats.forEach { stat ->
                    Row(
                        Modifier.clip(RoundedCornerShape(6.dp)).combinedClickableCompat({
                            haptics.selection()
                            model.data?.tabForFlow(flow.id)?.let { model.select(it) }
                            model.filter = model.filter.copy(stage = if (stat.stage == "other") "" else stat.stage)
                            model.show(CrmArea.CONTACTS)
                            CrmRevision.bump()
                        }).padding(vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Box(Modifier.size(7.dp).clip(CircleShape).background(CrmColors.stage(stat.stage, flow)))
                        Text(CrmStageLabel.title(stat.stage), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        Text("${stat.count}", style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary)
                    }
                }
            }
        }
    }
}

@Composable
private fun CrmCaption(title: String, trailing: (@Composable () -> Unit)? = null) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(title.uppercase(), Modifier.weight(1f), style = Fm.text.labelSmall.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textSecondary)
        trailing?.invoke()
    }
}

@Composable
internal fun CrmHomePane(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner, navigator: Navigator) {
    val dense = crmDense()
    @Suppress("UNUSED_EXPRESSION") CrmRevision.value
    val board = model.dashboard
    val name = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
    val hour = java.time.LocalTime.now().hour
    FmPullToRefreshBox(isRefreshing = runner.busy > 0, onRefresh = { runner.run({ load() }) }, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(if (dense) 10.dp else 16.dp),
        ) {
            Text(
                CrmDashboard.greeting(hour) + if (name.isEmpty()) "" else ", $name",
                maxLines = 1, overflow = TextOverflow.Ellipsis,
                style = (if (dense) Fm.text.titleMedium else Fm.text.titleLarge).copy(fontWeight = FontWeight.SemiBold),
                color = Fm.colors.textPrimary,
            )
            CrmZohoAlert(model, data)
            val pipeline: @Composable () -> Unit = {
                CrmDashboard.pipelineFlow(data, model.activeFlow)?.let { flow ->
                    CrmPipelineBar(model, flow, data.contacts.filter { it.flow == flow.id }) {
                        model.show(CrmArea.PIPELINE)
                        CrmRevision.bump()
                    }
                }
            }
            if (dense) {
                CrmKpis(model, board, dense)
                pipeline()
                CrmNextUp(model, runner, navigator, dense)
            } else {
                CrmNextUp(model, runner, navigator, dense)
                CrmKpis(model, board, dense)
                pipeline()
            }
            CrmRecent(model, data, navigator, dense)
            Spacer(Modifier.height(40.dp))
        }
    }
}

@Composable
private fun CrmKpis(model: CrmModel, board: CrmDashboard, dense: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CrmKpiTile(
                "${board.contacts}", "Contacts", "groups", CrmColors.blue,
                if (board.newThisWeek > 0) "+${board.newThisWeek} this week" else "", dense, Modifier.weight(1f),
            ) { model.show(CrmArea.CONTACTS); CrmRevision.bump() }
            CrmKpiTile(
                CrmText.money(board.openValue), "Open deals", "trending_up", Color(0xFF0D9488), "${board.openDeals} open", dense, Modifier.weight(1f),
            ) { model.show(CrmArea.PIPELINE); CrmRevision.bump() }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CrmKpiTile(
                "${board.toContact}", "To contact", "notifications_active", CrmColors.orange,
                "${board.neverContacted} new · ${board.quiet} quiet", dense, Modifier.weight(1f),
            )
            CrmKpiTile(CrmText.money(board.wonValue), "Won", "verified", Color(0xFF4F46E5), "${board.wonDeals} won", dense, Modifier.weight(1f))
        }
    }
}

@Composable
private fun CrmNextUp(model: CrmModel, runner: CrmRunner, navigator: Navigator, dense: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        CrmCaption("Up next")
        CrmCard(Modifier.fillMaxWidth()) {
            Column {
                CrmNextContact.Mode.entries.forEachIndexed { index, mode ->
                    if (index > 0) Box(Modifier.padding(start = 44.dp).fillMaxWidth().height(1.dp).background(Fm.colors.border))
                    CrmNextRow(model, mode, runner, navigator, dense)
                }
            }
        }
    }
}

@Composable
private fun CrmNextRow(model: CrmModel, mode: CrmNextContact.Mode, runner: CrmRunner, navigator: Navigator, dense: Boolean) {
    val context = LocalContext.current
    val haptics = FmHaptics.current
    @Suppress("UNUSED_EXPRESSION") CrmRevision.value
    val next = model.nextContact(mode, everyone = true)
    val contact = next.contact
    if (contact == null) {
        Row(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(34.dp), contentAlignment = Alignment.Center) {
                FmIcon(if (mode == CrmNextContact.Mode.REACH_OUT) "person_add" else "reply", size = 18.dp, tint = Fm.colors.textSecondary)
            }
            Spacer(Modifier.width(10.dp))
            Text(
                if (mode == CrmNextContact.Mode.REACH_OUT) "No new leads to reach out to." else "Nobody due for a follow-up.",
                Modifier.weight(1f), style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
            )
            if (model.nextSkipped.isNotEmpty()) {
                Text("Show skipped", Modifier.clip(RoundedCornerShape(6.dp)).combinedClickableCompat({ model.nextSkipped.clear(); CrmRevision.bump() }).padding(4.dp),
                    style = Fm.text.labelSmall, color = Fm.colors.accent)
            }
        }
        return
    }
    var menu by remember { mutableStateOf(false) }
    Box {
        Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(
                Modifier.weight(1f).combinedClickableCompat(
                    { navigator.push(ContactScreen(CrmContactRef(model.organization, contact.id))) },
                    { menu = true },
                ),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                CrmAvatar(contact.name.ifEmpty { contact.email }, if (dense) 34 else 40)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(1.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                        Text(contact.displayName, Modifier.weight(1f, fill = false), maxLines = 1, overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                        CrmChip(mode.title, if (mode == CrmNextContact.Mode.REACH_OUT) CrmColors.blue else CrmColors.orange)
                    }
                    Text(next.reason, maxLines = if (dense) 1 else 2, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                }
            }
            val mastodon = CrmText.profileUrl("mastodon", contact.mastodon) ?: CrmText.profileUrl("twitter", contact.twitter)
            when {
                '@' in contact.email -> CrmRoundAction("mail", "Email ${contact.displayName}", Fm.colors.accent) {
                    navigator.push(CrmComposeScreen(model, CrmEmailDraft.of(contact), false))
                }
                mastodon != null -> CrmRoundAction("chat", "Message ${contact.displayName}", Fm.colors.accent) { crmOpen(context, mastodon) }
            }
            CrmRoundAction("check", "Mark ${contact.displayName} contacted", CrmColors.green) {
                runner.run({ markContacted(listOf(contact.id)) }) { ok -> if (ok) haptics.commit() }
            }
        }
        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            FmDropdownMenuItem("Skip for now", onClick = { menu = false; model.nextSkipped.add(contact.id); CrmRevision.bump() }, icon = "skip_next")
        }
    }
}

@Composable
private fun CrmRoundAction(icon: String, label: String, tint: Color, onTap: () -> Unit) {
    Box(
        Modifier.size(32.dp).clip(CircleShape).background(tint.copy(alpha = 0.15f)).semantics { contentDescription = label }.combinedClickableCompat(onTap),
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 16.dp, tint = tint)
    }
}

@Composable
private fun CrmRecent(model: CrmModel, data: CrmOrganizationData, navigator: Navigator, dense: Boolean) {
    val items = data.activity.sortedByDescending { it.at }.take(if (dense) 5 else 3)
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        CrmCaption("Recent activity") {
            Text("All", Modifier.clip(RoundedCornerShape(6.dp)).combinedClickableCompat({ model.show(CrmArea.ACTIVITY); CrmRevision.bump() }).padding(4.dp),
                style = Fm.text.bodySmall, color = Fm.colors.accent)
        }
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(vertical = 4.dp)) {
            if (items.isEmpty()) Text("No emails yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            items.forEach { item ->
                CrmEmailRowView(item, data.contactById(item.contactId)?.displayName) { navigator.push(CrmEmailScreen(model, item)) }
            }
        }
    }
}

@Composable
internal fun CrmZohoAlert(model: CrmModel, data: CrmOrganizationData) {
    val zoho = data.settings.zoho
    if (zoho.connected && zoho.lastSyncError.isEmpty()) return
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(CrmColors.orange.copy(alpha = 0.1f))
            .combinedClickableCompat({ model.show(CrmArea.MORE); CrmRevision.bump() })
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        FmIcon("mail", size = 16.dp, tint = CrmColors.orange)
        Text(if (zoho.connected) "Zoho Mail sync failed" else "Zoho Mail is not connected", Modifier.weight(1f),
            style = Fm.text.bodySmall.copy(fontWeight = FontWeight.Medium), color = Fm.colors.textPrimary)
        FmIcon("chevron_right", size = 14.dp, tint = Fm.colors.textTertiary)
    }
}

@Composable
internal fun CrmPipelinePane(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner, navigator: Navigator) {
    val dense = crmDense()
    @Suppress("UNUSED_EXPRESSION") CrmRevision.value
    var board by remember { mutableStateOf<Boolean?>(null) }
    var flowMenu by remember { mutableStateOf(false) }
    val pipelineTabs = data.contactTabs.filter { it.flow != null }
    val flow = model.activeFlow ?: pipelineTabs.firstOrNull()?.flow
    val showBoard = board ?: dense
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.weight(1f)) {
                Row(
                    Modifier.clip(RoundedCornerShape(8.dp)).combinedClickableCompat({ flowMenu = true }).padding(vertical = 6.dp, horizontal = 2.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Text(
                        if (model.activeFlow == null) pipelineTabs.firstOrNull()?.title ?: "Pipeline" else model.activeTitle,
                        maxLines = 1, overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.accent,
                    )
                    FmIcon("expand_more", size = 16.dp, tint = Fm.colors.accent)
                }
                FmDropdownMenu(expanded = flowMenu, onDismissRequest = { flowMenu = false }) {
                    pipelineTabs.forEach { item ->
                        FmDropdownMenuItem("${item.title} (${item.count ?: 0})", onClick = {
                            flowMenu = false
                            model.select(item.tab)
                            CrmRevision.bump()
                        }, icon = if (item.tab == model.activeTab) "check" else item.icon)
                    }
                }
            }
            Box(Modifier.width(176.dp)) {
                ThemeSegmentTrack(
                    listOf(true, false), showBoard,
                    label = { if (it) "Board" else "Stages" },
                    icon = { if (it) "view_week" else "view_list" },
                    tag = { if (it) "crm-pipeline-board" else "crm-pipeline-stages" },
                    description = { if (it) "Board" else "Stages" },
                    onPick = { board = it },
                )
            }
        }
        when {
            flow == null -> FmEmptyState("view_week", "No pipelines yet")
            showBoard -> CrmBoard(model, data, flow, runner, navigator)
            else -> CrmStageList(model, data, flow, runner, navigator)
        }
    }
}

@Composable
private fun CrmStageList(model: CrmModel, data: CrmOrganizationData, flow: CrmFlow, runner: CrmRunner, navigator: Navigator) {
    val haptics = FmHaptics.current
    val rows = model.visibleContacts
    var open by remember(flow.id) { mutableStateOf(setOf<String>()) }
    var menuFor by remember { mutableStateOf<String?>(null) }
    var pendingDelete by remember { mutableStateOf<CrmContact?>(null) }
    FmPullToRefreshBox(isRefreshing = runner.busy > 0, onRefresh = { runner.run({ load() }) }, modifier = Modifier.fillMaxSize()) {
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            flow.stats(rows).forEach { stat ->
                val group = rows.filter { if (stat.stage == "other") it.stage !in flow.stages else it.stage == stat.stage }
                val expanded = stat.stage in open
                item(key = "stage-${stat.stage}") {
                    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised)) {
                        Row(
                            Modifier.fillMaxWidth().combinedClickableCompat({
                                if (group.isNotEmpty()) {
                                    haptics.selection()
                                    open = if (expanded) open - stat.stage else open + stat.stage
                                }
                            }).padding(horizontal = 14.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Box(Modifier.size(10.dp).clip(CircleShape).background(CrmColors.stage(stat.stage, flow)))
                            Text(CrmStageLabel.title(stat.stage), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                            Text("${stat.count}", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                            if (stat.value > 0) Text(CrmText.money(stat.value), style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                            FmIcon(if (expanded) "expand_less" else "expand_more", size = 16.dp, tint = if (group.isEmpty()) Fm.colors.textDisabled else Fm.colors.textTertiary)
                        }
                        if (expanded) {
                            group.forEach { contact ->
                                Box {
                                    Row(
                                        Modifier.fillMaxWidth().combinedClickableCompat(
                                            { navigator.push(ContactScreen(CrmContactRef(model.organization, contact.id))) },
                                            { menuFor = contact.id },
                                        ),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        Box(Modifier.weight(1f)) { CrmContactRowContent(contact, flow, showStage = false, dense = false) }
                                        flow.next(contact.stage)?.let { next ->
                                            CrmChip("→ ${CrmStageLabel.title(next)}", CrmColors.stage(next, flow), Modifier.padding(end = 12.dp), onTap = {
                                                runner.run({ move(contact, next) }) { ok -> if (ok) haptics.selection() }
                                            })
                                        }
                                    }
                                    CrmContactMenu(model, data, runner, navigator, contact, expanded = menuFor == contact.id, onDismiss = { menuFor = null }, onDelete = { pendingDelete = it })
                                }
                            }
                        }
                    }
                }
            }
            item(key = "bottom") { Spacer(Modifier.height(60.dp)) }
        }
    }
    pendingDelete?.let { contact ->
        com.forkmesh.nativeapp.ui.kit.FmConfirmDialog(
            "Delete ${contact.displayName}?",
            onConfirm = { pendingDelete = null; runner.run({ delete(contact) }) },
            onDismissRequest = { pendingDelete = null },
            body = "The contact and its pipeline history are removed for everyone in ${model.organization}.",
            confirmLabel = "Delete",
            destructive = true,
        )
    }
}

@Composable
private fun ColumnScope.CrmMoreGroup(title: String, footer: String? = null, content: @Composable ColumnScope.() -> Unit) {
    CrmSection(title, content = content)
    if (footer != null) Text(footer, Modifier.padding(horizontal = 14.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
}

@Composable
private fun CrmMoreRow(title: String, icon: String, onTap: () -> Unit) {
    FmListRow(
        title,
        leading = { FmIcon(icon, size = 20.dp, tint = Fm.colors.accent) },
        onTap = onTap,
        showChevron = false,
        titleColor = Fm.colors.accent,
        minHeight = 44.dp,
        padding = PaddingValues(horizontal = 14.dp, vertical = 4.dp),
    )
}

@Composable
internal fun CrmMorePane(
    model: CrmModel,
    data: CrmOrganizationData,
    runner: CrmRunner,
    navigator: Navigator,
    plugin: PluginMenu?,
    onImport: () -> Unit,
    onReload: () -> Unit,
) {
    val context = LocalContext.current
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp)) {
        CrmMoreGroup("Settings", footer = "Layout and theme apply across the app.") {
            CrmMoreRow("CRM settings", "settings") { navigator.push(CrmSettingsScreen(model)) }
            CrmModeSwitcher()
        }
        CrmMoreGroup("Mail") {
            CrmZohoBar(model, data, runner, Modifier.padding(horizontal = 8.dp, vertical = 4.dp))
            CrmMoreRow("Sync mail", "sync") { runner.run({ syncMail() }) { url -> if (url != null) crmOpen(context, url) } }
            CrmMoreRow(if (data.settings.mailProvider == "gmail") "Open Gmail" else "Open Zoho Mail", "mail") { crmOpen(context, data.settings.composeUrl("")) }
            CrmMoreRow(model.noteDrafts.size.let { if (it == 0) "Unsent drafts" else "Unsent drafts ($it)" }, "edit_note") { navigator.push(CrmNoteDraftsScreen(model)) }
        }
        CrmMoreGroup("Organize") {
            CrmMoreRow("Pipelines", "account_tree") { navigator.push(CrmPipelinesScreen(model)) }
            CrmMoreRow("New list", "playlist_add") { navigator.push(CrmNewListScreen(model)) }
            if (model.canManage) CrmMoreRow("New pipeline", "add_circle") { navigator.push(CrmPipelineEditorScreen(model, CrmFlowDraft())) }
        }
        CrmMoreGroup("Data") {
            CrmMoreRow("Import CSV", "upload") { onImport() }
            CrmMoreRow("Export CSV", "download") { runner.run({ exportCsv() }) { file -> if (file != null) crmShare(context, file.second, file.first) } }
            CrmMoreRow("Refresh", "refresh") { onReload() }
        }
        if (plugin != null) {
            Spacer(Modifier.height(10.dp))
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(vertical = 4.dp)) {
                CrmMoreRow(PackageMenu.OPTIONS, "star_border") { plugin.open() }
            }
        }
        Spacer(Modifier.height(60.dp))
    }
}

@Composable
internal fun CrmNoContactsHint(text: String) {
    Text(text, Modifier.fillMaxWidth().padding(32.dp), textAlign = TextAlign.Center, style = Fm.text.bodyMedium, color = Fm.colors.textTertiary)
}
