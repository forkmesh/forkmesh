package com.forkmesh.nativeapp.feature.market

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmBulkAction
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmContactRef
import com.forkmesh.nativeapp.market.CrmEmailDraft
import com.forkmesh.nativeapp.market.CrmFlow
import com.forkmesh.nativeapp.market.CrmFlowDraft
import com.forkmesh.nativeapp.market.CrmFlowIcon
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.market.CrmSection as CrmArea
import com.forkmesh.nativeapp.market.CrmSort
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmStageStat
import com.forkmesh.nativeapp.market.CrmTab
import com.forkmesh.nativeapp.market.CrmTabItem
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.market.contactTabs
import com.forkmesh.nativeapp.market.listViews
import com.forkmesh.nativeapp.market.next
import com.forkmesh.nativeapp.market.owners
import com.forkmesh.nativeapp.market.stats
import com.forkmesh.nativeapp.market.tags
import com.forkmesh.nativeapp.market.workspaceTabs
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class CrmScreen(private val organization: String?, private val flow: String?) : Screen() {
    override val title: String get() = "CRM"

    private val model: CrmModel? = MarketFeature.crm()?.also { model ->
        if (!organization.isNullOrEmpty()) model.organization = organization
        if (flow != null) model.flowId = flow
    }
    private var loading by mutableStateOf(true)
    private var reload by mutableIntStateOf(1)
    private var openedFlow = false
    private var selecting by mutableStateOf(false)
    private var selection by mutableStateOf(setOf<String>())
    private var pendingDelete by mutableStateOf(listOf<String>())

    override fun onBack(): Boolean {
        if (!selecting) return false
        selecting = false
        selection = emptySet()
        return true
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        val top = navigator.top
        LaunchedEffect(top) {
            if (top !== this@CrmScreen) return@LaunchedEffect
            CrmRevision.bump()
            if (!loading && model.needsOrganization) reload++
        }
        LaunchedEffect(reload) {
            loading = true
            withContext(Dispatchers.IO) { model.refresh() }
            loading = false
            if (flow != null && !openedFlow) {
                openedFlow = true
                model.show(CrmArea.PIPELINE)
            }
            CrmRevision.bump()
        }
        val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            scope.launch {
                val text = withContext(Dispatchers.IO) {
                    runCatching { context.contentResolver.openInputStream(uri)?.use { String(it.readBytes()) } }.getOrNull().orEmpty()
                }
                val name = uri.lastPathSegment?.substringAfterLast('/')?.ifEmpty { null } ?: "contacts.csv"
                if (text.isNotBlank()) navigator.push(CrmImportScreen(model, text, name)) else runner.status.error("That file is empty.")
            }
        }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val data = model.data
        var orgMenu by remember { mutableStateOf(false) }
        var sortMenu by remember { mutableStateOf(false) }
        val launchImport = { importer.launch(arrayOf("text/*", "application/csv", "text/csv", "text/comma-separated-values")) }
        val newContact = {
            if (!model.activeTab.isContacts) model.select(model.defaultTab)
            navigator.push(ContactEditorScreen(model, null, model.activeView?.takeIf { !it.isPipeline }?.id))
        }
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                val plugin = rememberScreenPluginMenu()
                ToolsAppBar(if (selecting) "${selection.size} selected" else model.organization.ifEmpty { "CRM" }, navigator, pluginMenu = !selecting && data == null) {
                    plugin?.Host()
                    if (selecting) {
                        FmIconButton("close", "Done", onPressed = { selecting = false; selection = emptySet() }, color = Fm.colors.textPrimary)
                    } else {
                        if (model.organizations.size > 1) {
                            Box {
                                FmIconButton("corporate_fare", "Organization", onPressed = { orgMenu = true }, color = Fm.colors.textPrimary)
                                FmDropdownMenu(expanded = orgMenu, onDismissRequest = { orgMenu = false }) {
                                    model.organizations.forEach { org ->
                                        FmDropdownMenuItem(org.name + if (org.installed) "" else " (not installed)", onClick = {
                                            orgMenu = false
                                            model.organization = org.name
                                            model.tab = null
                                            runner.run({ load() })
                                        }, icon = if (org.name == model.organization) "check" else null)
                                    }
                                }
                            }
                        }
                        if (data != null) {
                            if (model.section == CrmArea.CONTACTS && model.activeTab.isContacts) {
                                Box {
                                    FmIconButton(
                                        if (model.filter.isActive) "filter_alt" else "filter_list", "Sort and filter",
                                        onPressed = { sortMenu = true }, color = Fm.colors.textPrimary,
                                    )
                                    FmDropdownMenu(expanded = sortMenu, onDismissRequest = { sortMenu = false }) {
                                        FmDropdownMenuItem("Select contacts", onClick = { sortMenu = false; selecting = true }, icon = "checklist")
                                        data.contactTabs.firstOrNull { it.tab == model.activeTab }?.takeIf { it.view != null || it.flow != null }?.let { item ->
                                            FmDropdownMenuItem("${item.title} settings", onClick = { sortMenu = false; navigator.push(CrmTabSettingsScreen(model, item)) }, icon = "tune")
                                        }
                                        if (model.filter.isActive) {
                                            FmDropdownMenuItem("Clear filters", onClick = {
                                                sortMenu = false
                                                model.filter = model.filter.copy(stage = "", owner = "", tag = "")
                                                CrmRevision.bump()
                                            }, icon = "close")
                                        }
                                    }
                                }
                            }
                            CrmModeMenu()
                            Box(Modifier.testTag("crm-new-contact")) {
                                FmIconButton("add", "New contact", onPressed = { newContact() }, color = Fm.colors.textPrimary)
                            }
                        }
                    }
                }
                ErrorLine(if (model.needsInstall || model.errorText == runner.status.message) "" else model.errorText)
                when {
                    loading && data == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    model.needsOrganization -> CrmNoOrganizationPanel { navigator.push(AccountFeature.orgsScreen()) }
                    model.needsInstall -> {
                        CrmRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                        CrmInstallPanel(model, runner.busy > 0) {
                            runner.run({ install() }) { ok -> if (ok) runner.status.show("CRM installed for ${model.organization}.") }
                        }
                    }
                    data == null -> FmEmptyState("contacts", "No CRM here yet", message = "Install the CRM package for an organization in the Market.")
                    else -> {
                        CrmHeader(model, data, selecting)
                        if (!selecting) CrmRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
                        val tab = model.activeTab
                        when (model.section) {
                            CrmArea.HOME -> CrmHomePane(model, data, runner, navigator)
                            CrmArea.PIPELINE -> CrmPipelinePane(model, data, runner, navigator)
                            CrmArea.MORE -> CrmMorePane(model, data, runner, navigator, plugin, onImport = { launchImport() }, onReload = { reload++ })
                            CrmArea.CONTACTS, CrmArea.ACTIVITY -> when {
                                tab.isContacts -> CrmContactList(
                                    model, data, runner, navigator, selecting, selection,
                                    onToggle = { id -> selection = if (id in selection) selection - id else selection + id },
                                    onDelete = { pendingDelete = it },
                                )
                                tab == CrmTab.Activity -> CrmActivityPane(model, data, navigator)
                                tab == CrmTab.Mail -> CrmMailPane(model, data, runner)
                                tab == CrmTab.Outreach -> CrmOutreachPane(model, data, runner, navigator)
                                tab == CrmTab.Team -> CrmTeamPane(data)
                                tab == CrmTab.Decks -> CrmDecksPane(model, data, runner, navigator)
                                tab == CrmTab.Shared -> CrmSharedPane(model, data, navigator)
                            }
                        }
                    }
                }
            }
            if (data != null && selecting) {
                CrmBulkBar(model, data, runner, navigator, selection, model.visibleContacts.map { it.id }, Modifier.align(Alignment.BottomCenter),
                    onSelect = { selection = it }, onDelete = { pendingDelete = it }, onDone = { selection = emptySet() })
            } else if (data != null && model.section == CrmArea.CONTACTS && model.activeTab.isContacts) {
                FmExtendedFloatingActionButton(
                    "Contact",
                    onClick = { newContact() },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                    icon = "person_add",
                )
            }
            if (pendingDelete.isNotEmpty()) {
                val ids = pendingDelete
                FmConfirmDialog(
                    if (ids.size == 1) "Delete ${model.contact(ids[0])?.displayName ?: "this contact"}?" else "Delete ${ids.size} contacts?",
                    onConfirm = {
                        pendingDelete = emptyList()
                        runner.run({ deleteAll(ids) }) { selection = selection - ids.toSet() }
                    },
                    onDismissRequest = { pendingDelete = emptyList() },
                    body = "Contacts and their pipeline history are removed for everyone in ${model.organization}.",
                    confirmLabel = "Delete",
                    destructive = true,
                )
            }
        }
    }
}

@Composable
private fun CrmNoOrganizationPanel(onCreate: () -> Unit) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(Modifier.padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("CRM needs an organization", style = Fm.text.titleMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
            Text("Create an organization, then come back here to install CRM for it. An invitation to someone else's organization works too.", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary, textAlign = TextAlign.Center)
            FmToneButton("Create an organization", onPressed = onCreate, tone = FmTone.Accent, filled = true, height = 40.dp)
        }
    }
}

@Composable
private fun CrmInstallPanel(model: CrmModel, installing: Boolean, onInstall: () -> Unit) {
    if (!model.canInstall) {
        FmEmptyState("contacts", "CRM is not installed for ${model.organization}", message = "Ask an owner or admin of the organization to install it from the Market.")
        return
    }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(Modifier.padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("CRM is not installed for ${model.organization}", style = Fm.text.titleMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
            Text("Install it to keep contacts, pipelines and email history with everyone in the organization.", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary, textAlign = TextAlign.Center)
            FmToneButton(if (installing) "Installing…" else "Install CRM", onPressed = if (installing) null else onInstall, tone = FmTone.Accent, filled = true, height = 40.dp)
        }
    }
}

@Composable
private fun CrmHeader(model: CrmModel, data: CrmOrganizationData, selecting: Boolean) {
    Column(Modifier.fillMaxWidth().padding(top = 4.dp, bottom = 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (!selecting) CrmSectionBar(model, data)
        if (model.section != CrmArea.MORE) {
            FmTextField(model.query, { query ->
                model.query = query
                if (query.isNotEmpty() && model.section == CrmArea.HOME) model.show(CrmArea.CONTACTS)
                CrmRevision.bump()
            }, Modifier.padding(horizontal = 16.dp), hint = if (model.section == CrmArea.ACTIVITY) "Search activity" else "Search contacts")
        }
        when (model.section) {
            CrmArea.CONTACTS -> CrmTabChips(model, data.contactTabs)
            CrmArea.ACTIVITY -> CrmTabChips(model, data.workspaceTabs)
            else -> Unit
        }
    }
}

@Composable
private fun CrmStageStrip(model: CrmModel, flow: CrmFlow, contacts: List<CrmContact>) {
    val stats = flow.stats(contacts)
    val total = maxOf(1, contacts.size)
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(Fm.colors.border)) {
            stats.filter { it.count > 0 }.forEach { stat ->
                Box(Modifier.weight(stat.count.toFloat() / total).fillMaxHeight().background(CrmColors.stage(stat.stage, flow)))
            }
            val rest = 1f - stats.sumOf { it.count }.toFloat() / total
            if (rest > 0.001f) Spacer(Modifier.weight(rest))
        }
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            stats.forEach { stat -> StagePill(model, flow, stat) }
        }
    }
}

@Composable
private fun StagePill(model: CrmModel, flow: CrmFlow, stat: CrmStageStat) {
    val active = model.filter.stage == stat.stage
    val tint = CrmColors.stage(stat.stage, flow)
    Column(
        Modifier.clip(RoundedCornerShape(10.dp)).background(if (active) tint.copy(alpha = 0.22f) else Fm.colors.bgElevated)
            .border(1.5.dp, if (active) tint else Color.Transparent, RoundedCornerShape(10.dp))
            .combinedClickableCompat({ model.filter = model.filter.copy(stage = if (active) "" else stat.stage); CrmRevision.bump() })
            .padding(horizontal = 10.dp, vertical = 6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(7.dp).clip(CircleShape).background(tint))
            Spacer(Modifier.width(4.dp))
            Text(CrmStageLabel.title(stat.stage), style = Fm.text.labelSmall, color = Fm.colors.textPrimary, maxLines = 1)
        }
        Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("${stat.count}", style = Fm.text.titleMedium.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary)
            if (stat.value > 0) Text(CrmText.money(stat.value), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        }
    }
}

@Composable
private fun CrmFilterRow(model: CrmModel, data: CrmOrganizationData) {
    var sortMenu by remember { mutableStateOf(false) }
    var ownerMenu by remember { mutableStateOf(false) }
    var tagMenu by remember { mutableStateOf(false) }
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
        Box {
            CrmChip(model.filter.sort.title, Fm.colors.accent, icon = "sort", onTap = { sortMenu = true })
            FmDropdownMenu(expanded = sortMenu, onDismissRequest = { sortMenu = false }) {
                CrmSort.entries.forEach { sort ->
                    FmDropdownMenuItem(sort.title, onClick = { sortMenu = false; model.filter = model.filter.copy(sort = sort); CrmRevision.bump() }, icon = if (sort == model.filter.sort) "check" else null)
                }
            }
        }
        Box {
            CrmChip(model.filter.owner.ifEmpty { "Any owner" }, if (model.filter.owner.isEmpty()) Fm.colors.textSecondary else Fm.colors.accent, icon = "person", filled = model.filter.owner.isNotEmpty(), onTap = { ownerMenu = true })
            FmDropdownMenu(expanded = ownerMenu, onDismissRequest = { ownerMenu = false }) {
                FmDropdownMenuItem("Anyone", onClick = { ownerMenu = false; model.filter = model.filter.copy(owner = ""); CrmRevision.bump() })
                data.owners.forEach { owner -> FmDropdownMenuItem(owner, onClick = { ownerMenu = false; model.filter = model.filter.copy(owner = owner); CrmRevision.bump() }) }
            }
        }
        if (data.tags.isNotEmpty()) {
            Box {
                CrmChip(if (model.filter.tag.isEmpty()) "Any tag" else "#${model.filter.tag}", if (model.filter.tag.isEmpty()) Fm.colors.textSecondary else CrmColors.purple, icon = "label", filled = model.filter.tag.isNotEmpty(), onTap = { tagMenu = true })
                FmDropdownMenu(expanded = tagMenu, onDismissRequest = { tagMenu = false }) {
                    FmDropdownMenuItem("Any tag", onClick = { tagMenu = false; model.filter = model.filter.copy(tag = ""); CrmRevision.bump() })
                    data.tags.forEach { tag -> FmDropdownMenuItem("#$tag", onClick = { tagMenu = false; model.filter = model.filter.copy(tag = tag); CrmRevision.bump() }) }
                }
            }
        }
        if (model.filter.isActive) {
            CrmChip("Clear", Fm.colors.danger, icon = "close", onTap = { model.filter = model.filter.copy(stage = "", owner = "", tag = ""); CrmRevision.bump() })
        }
    }
}

@Composable
private fun CrmContactList(
    model: CrmModel,
    data: CrmOrganizationData,
    runner: CrmRunner,
    navigator: Navigator,
    selecting: Boolean,
    selection: Set<String>,
    onToggle: (String) -> Unit,
    onDelete: (List<String>) -> Unit,
) {
    val flow = model.activeFlow
    val rows = model.visibleContacts
    val columns = model.activeView?.summaryColumns.orEmpty()
    var menuFor by remember { mutableStateOf<String?>(null) }
    FmPullToRefreshBox(isRefreshing = runner.busy > 0, onRefresh = { runner.run({ load() }) }, modifier = Modifier.fillMaxSize()) {
        LazyColumn(Modifier.fillMaxSize()) {
            if (flow != null && !selecting) item(key = "strip") { CrmStageStrip(model, flow, model.tabContacts) }
            item(key = "filters") { CrmFilterRow(model, data) }
            if (rows.isEmpty()) {
                item(key = "empty") {
                    Text(
                        if (model.query.isEmpty() && !model.filter.isActive) "No contacts here yet." else "No contacts match.",
                        Modifier.fillMaxWidth().padding(32.dp), textAlign = TextAlign.Center, style = Fm.text.bodyMedium, color = Fm.colors.textTertiary,
                    )
                }
            }
            val grouped = flow != null && model.filter.sort == CrmSort.STAGE && model.filter.stage.isEmpty()
            val sections: List<Pair<CrmStageStat?, List<CrmContact>>> = if (grouped && flow != null) {
                flow.stats(rows).map { stat -> stat to rows.filter { if (stat.stage == "other") it.stage !in flow.stages else it.stage == stat.stage } }.filter { it.second.isNotEmpty() }
            } else {
                listOf(null to rows)
            }
            sections.forEach { (stat, group) ->
                if (stat != null && flow != null) {
                    item(key = "h-${stat.stage}") {
                        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(8.dp).clip(CircleShape).background(CrmColors.stage(stat.stage, flow)))
                            Spacer(Modifier.width(6.dp))
                            Text(CrmStageLabel.title(stat.stage).uppercase(), Modifier.weight(1f), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            Text("${stat.count}" + if (stat.value > 0) " · ${CrmText.money(stat.value)}" else "", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        }
                    }
                }
                items(group, key = { it.id }) { contact ->
                    Box {
                        Box(
                            Modifier.fillMaxWidth().background(Fm.colors.bgRaised).combinedClickableCompat(
                                onTap = { if (selecting) onToggle(contact.id) else navigator.push(ContactScreen(CrmContactRef(model.organization, contact.id))) },
                                onLongPress = { if (selecting) onToggle(contact.id) else menuFor = contact.id },
                            ),
                        ) {
                            CrmContactRowContent(contact, flow ?: data.flow(contact.flow), showStage = stat == null || stat.stage == "other", columns = columns, selected = if (selecting) contact.id in selection else null)
                        }
                        CrmContactMenu(model, data, runner, navigator, contact, expanded = menuFor == contact.id, onDismiss = { menuFor = null }, onDelete = { onDelete(listOf(it.id)) })
                    }
                }
            }
            item(key = "bottom") { Spacer(Modifier.height(if (selecting) 180.dp else 96.dp)) }
        }
    }
}

@Composable
internal fun CrmContactMenu(
    model: CrmModel,
    data: CrmOrganizationData,
    runner: CrmRunner,
    navigator: Navigator,
    contact: CrmContact,
    expanded: Boolean,
    onDismiss: () -> Unit,
    onDelete: (CrmContact) -> Unit,
) {
    val context = LocalContext.current
    var level by remember(expanded) { mutableStateOf("") }
    FmDropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        val flow = data.flow(contact.flow)
        when (level) {
            "stage" -> flow.stages.filter { it != contact.stage }.forEach { stage ->
                FmDropdownMenuItem(CrmStageLabel.title(stage), onClick = { onDismiss(); runner.run({ bulk(CrmBulkAction.Stage(stage), listOf(contact.id)) }) })
            }
            "flow" -> data.flows.filter { it.id != contact.flow }.forEach { target ->
                FmDropdownMenuItem(target.name, onClick = { onDismiss(); runner.run({ bulk(CrmBulkAction.Flow(target.id), listOf(contact.id)) }) }, icon = CrmFlowIcon.symbol(target.icon))
            }
            "list" -> data.listViews.forEach { view ->
                val member = view.id in contact.views
                FmDropdownMenuItem(if (member) "Remove from ${view.name}" else "Add to ${view.name}", onClick = {
                    onDismiss()
                    runner.run({ bulk(if (member) CrmBulkAction.RemoveList(view.id) else CrmBulkAction.AddList(view.id), listOf(contact.id)) })
                }, icon = if (member) "playlist_remove" else "playlist_add")
            }
            else -> {
                if (contact.email.isNotEmpty()) FmDropdownMenuItem("Email", onClick = { onDismiss(); navigator.push(CrmComposeScreen(model, com.forkmesh.nativeapp.market.CrmEmailDraft.of(contact), false)) }, icon = "mail")
                if (contact.phone.isNotEmpty()) FmDropdownMenuItem("Call", onClick = { onDismiss(); crmOpen(context, "tel:" + contact.phone.filter { !it.isWhitespace() }) }, icon = "call")
                FmDropdownMenuItem("Move to stage…", onClick = { level = "stage" }, icon = "arrow_forward")
                FmDropdownMenuItem("Move to pipeline…", onClick = { level = "flow" }, icon = "account_tree")
                if (data.listViews.isNotEmpty()) FmDropdownMenuItem("Lists…", onClick = { level = "list" }, icon = "list")
                FmDropdownMenuItem("Mark contacted now", onClick = { onDismiss(); runner.run({ markContacted(listOf(contact.id)) }) }, icon = "mark_email_read")
                FmDropdownMenuItem("Edit", onClick = { onDismiss(); navigator.push(ContactEditorScreen(model, contact)) }, icon = "edit")
                FmDropdownMenuItem("Deck link", onClick = {
                    onDismiss()
                    runner.run({ deckLink(contact) }) { link -> if (link != null) { crmCopy(context, link.url); model.notice = ""; } }
                }, icon = "slideshow")
                FmDropdownMenuItem("Copy prompt", onClick = {
                    onDismiss()
                    crmCopy(context, com.forkmesh.nativeapp.market.CrmPrompt.outreach(contact, model.activeTitle, flow.name))
                }, icon = "auto_awesome")
                FmDropdownMenuItem("Merge into…", onClick = { onDismiss(); navigator.push(CrmMergeScreen(model, contact)) }, icon = "merge")
                FmDropdownMenuItem("Delete", onClick = { onDismiss(); onDelete(contact) }, icon = "delete", textColor = Fm.colors.danger)
            }
        }
    }
}

@Composable
private fun CrmBulkBar(
    model: CrmModel,
    data: CrmOrganizationData,
    runner: CrmRunner,
    navigator: Navigator,
    selection: Set<String>,
    all: List<String>,
    modifier: Modifier,
    onSelect: (Set<String>) -> Unit,
    onDelete: (List<String>) -> Unit,
    onDone: () -> Unit,
) {
    val ids = selection.toList()
    val flow = model.activeFlow ?: ids.firstOrNull()?.let { model.contact(it) }?.let { data.flow(it.flow) }
    var menu by remember { mutableStateOf("") }
    fun go(work: CrmModel.() -> Boolean) {
        menu = ""
        runner.run(work) { ok -> if (ok) onDone() }
    }
    Column(
        modifier.fillMaxWidth().padding(12.dp).clip(RoundedCornerShape(18.dp)).background(Fm.colors.bgOverlay).border(1.dp, Fm.colors.border, RoundedCornerShape(18.dp)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        CrmRunnerStatus(runner, model)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(if (ids.isEmpty()) "Select contacts" else "${ids.size} selected", Modifier.weight(1f), style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
            CrmChip(if (selection.size == all.size && all.isNotEmpty()) "Select none" else "Select all", Fm.colors.accent, onTap = {
                onSelect(if (selection.size == all.size) emptySet() else all.toSet())
            })
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("stage" to ("Stage" to "arrow_forward"), "flow" to ("Pipeline" to "account_tree"), "list" to ("Lists" to "list"), "more" to ("More" to "more_horiz")).forEach { (key, label) ->
                Box(Modifier.weight(1f)) {
                    Column(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Fm.colors.accentSubtle)
                            .combinedClickableCompat({ if (ids.isNotEmpty()) menu = key }).padding(vertical = 8.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        FmIcon(label.second, size = 18.dp, tint = if (ids.isEmpty()) Fm.colors.textDisabled else Fm.colors.accent)
                        Text(label.first, style = Fm.text.labelSmall, color = if (ids.isEmpty()) Fm.colors.textDisabled else Fm.colors.textPrimary)
                    }
                    FmDropdownMenu(expanded = menu == key, onDismissRequest = { menu = "" }) {
                        when (key) {
                            "stage" -> flow?.stages.orEmpty().forEach { stage -> FmDropdownMenuItem(CrmStageLabel.title(stage), onClick = { go { bulk(CrmBulkAction.Stage(stage), ids) } }) }
                            "flow" -> {
                                data.views.filter { it.isPipeline }.forEach { view -> FmDropdownMenuItem("List: ${view.name}", onClick = { go { moveToPipelineList(ids, view) } }, icon = "playlist_add") }
                                data.flows.forEach { target -> FmDropdownMenuItem(target.name, onClick = { go { bulk(CrmBulkAction.Flow(target.id), ids) } }, icon = CrmFlowIcon.symbol(target.icon)) }
                            }
                            "list" -> data.listViews.forEach { view ->
                                FmDropdownMenuItem("Add to ${view.name}", onClick = { go { bulk(CrmBulkAction.AddList(view.id), ids) } }, icon = "playlist_add")
                                FmDropdownMenuItem("Only in ${view.name}", onClick = { go { setOnlyList(ids, view.id) } }, icon = "swap_horiz")
                                FmDropdownMenuItem("Remove from ${view.name}", onClick = { go { bulk(CrmBulkAction.RemoveList(view.id), ids) } }, icon = "playlist_remove")
                            }
                            else -> {
                                FmDropdownMenuItem("Mark contacted now", onClick = { go { markContacted(ids) } }, icon = "mark_email_read")
                                FmDropdownMenuItem("Set owner…", onClick = { menu = ""; navigator.push(CrmOwnerScreen(model, ids)) }, icon = "person")
                                if (ids.size == 1) model.contact(ids[0])?.let { contact -> FmDropdownMenuItem("Merge into…", onClick = { menu = ""; navigator.push(CrmMergeScreen(model, contact)) }, icon = "merge") }
                                FmDropdownMenuItem("Delete", onClick = { menu = ""; onDelete(ids) }, icon = "delete", textColor = Fm.colors.danger)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
internal fun CrmBoard(model: CrmModel, data: CrmOrganizationData, flow: CrmFlow, runner: CrmRunner, navigator: Navigator) {
    val rows = model.visibleContacts
    var menuFor by remember { mutableStateOf<String?>(null) }
    var pendingDelete by remember { mutableStateOf<CrmContact?>(null) }
    LazyRow(Modifier.fillMaxSize(), contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        items(flow.stats(rows), key = { it.stage }) { stat ->
            val tint = CrmColors.stage(stat.stage, flow)
            val cards = rows.filter { if (stat.stage == "other") it.stage !in flow.stages else it.stage == stat.stage }
            Column(
                Modifier.width(260.dp).fillMaxHeight().clip(RoundedCornerShape(16.dp)).background(tint.copy(alpha = 0.07f)).padding(8.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(Modifier.padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(9.dp).clip(CircleShape).background(tint))
                    Spacer(Modifier.width(6.dp))
                    Text(CrmStageLabel.title(stat.stage), Modifier.weight(1f), style = Fm.text.titleSmall, color = Fm.colors.textPrimary, maxLines = 1)
                    Text("${stat.count}" + if (stat.value > 0) " · ${CrmText.money(stat.value)}" else "", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                }
                if (cards.isEmpty()) Text("Nobody here yet", Modifier.fillMaxWidth().padding(vertical = 20.dp), textAlign = TextAlign.Center, style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                cards.forEach { contact ->
                    Box {
                        Column(
                            Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Fm.colors.bgRaised)
                                .combinedClickableCompat({ navigator.push(ContactScreen(CrmContactRef(model.organization, contact.id))) }, { menuFor = contact.id })
                                .padding(10.dp),
                            verticalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                CrmAvatar(contact.name.ifEmpty { contact.email }, 28)
                                Spacer(Modifier.width(8.dp))
                                Column(Modifier.weight(1f)) {
                                    Text(contact.displayName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                                    if (contact.subtitle.isNotEmpty()) Text(contact.subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                                }
                                if (contact.value > 0) Text(CrmText.money(contact.value), style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold), color = CrmColors.green)
                            }
                            if (contact.nextStep.isNotEmpty()) Text("↳ " + contact.nextStep, maxLines = 2, style = Fm.text.labelSmall, color = Fm.colors.textPrimary)
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                flow.next(contact.stage)?.let { next ->
                                    CrmChip("→ ${CrmStageLabel.title(next)}", CrmColors.stage(next, flow), onTap = { runner.run({ move(contact, next) }) })
                                }
                                Spacer(Modifier.weight(1f))
                                if (contact.lastContactedAt > 0) Text(relativeTime(contact.lastContactedAt), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                            }
                        }
                        CrmContactMenu(model, data, runner, navigator, contact, expanded = menuFor == contact.id, onDismiss = { menuFor = null }, onDelete = { pendingDelete = it })
                    }
                }
            }
        }
    }
    pendingDelete?.let { contact ->
        FmConfirmDialog(
            "Delete ${contact.displayName}?",
            onConfirm = { pendingDelete = null; runner.run({ delete(contact) }) },
            onDismissRequest = { pendingDelete = null },
            body = "The contact and its pipeline history are removed for everyone in ${model.organization}.",
            confirmLabel = "Delete",
            destructive = true,
        )
    }
}
