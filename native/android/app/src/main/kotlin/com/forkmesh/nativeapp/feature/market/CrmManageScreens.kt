package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmColumn
import com.forkmesh.nativeapp.market.CrmColumnCatalog
import com.forkmesh.nativeapp.market.CrmCsv
import com.forkmesh.nativeapp.market.CrmFlowDraft
import com.forkmesh.nativeapp.market.CrmFlowIcon
import com.forkmesh.nativeapp.market.CrmImportPreview
import com.forkmesh.nativeapp.market.CrmImportResult
import com.forkmesh.nativeapp.market.CrmListActivityEntry
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmPrompt
import com.forkmesh.nativeapp.market.CrmSettingsEdit
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmTabItem
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
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

class CrmNewListScreen(private val model: CrmModel) : Screen() {
    override val title: String get() = "New list"

    private var name by mutableStateOf("")
    private var flow by mutableStateOf("")
    private var onlyMe by mutableStateOf(false)
    private var flowMenu by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        val flows = model.data?.flows.orEmpty()
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("New list", navigator) {
                FmTextButton(onClick = {
                    runner.run({ createList(name, this@CrmNewListScreen.flow, if (onlyMe) "private" else "organization") }) { view -> if (view != null) navigator.pop() }
                }, enabled = name.isNotBlank() && runner.busy == 0) { Text("Create") }
            }
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                FmTextField(name, { name = it }, hint = "List name", autofocus = true)
                Box {
                    FmListRow("Shows", subtitle = flows.firstOrNull { it.id == flow }?.let { "Pipeline: ${it.name}" } ?: "Contacts I add to it", onTap = { flowMenu = true }, minHeight = 52.dp)
                    FmDropdownMenu(expanded = flowMenu, onDismissRequest = { flowMenu = false }) {
                        FmDropdownMenuItem("Contacts I add to it", onClick = { flowMenu = false; flow = "" })
                        flows.forEach { item -> FmDropdownMenuItem("Pipeline: ${item.name}", onClick = { flowMenu = false; flow = item.id }, icon = CrmFlowIcon.symbol(item.icon)) }
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Only me", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    FmSwitch(onlyMe, { onlyMe = it })
                }
                Text(
                    if (flow.isEmpty()) "A plain list holds the contacts you add to it." else "A pipeline list shows the pipeline's contacts, split by stage.",
                    style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
            }
        }
    }
}

class CrmPipelineEditorScreen(private val model: CrmModel, start: CrmFlowDraft) : Screen() {
    override val title: String get() = if (draft.isNew) "New pipeline" else draft.name

    private var draft by mutableStateOf(start)
    private var newStage by mutableStateOf("")
    private var iconMenu by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = { runner.run({ saveFlow(draft) }) { flow -> if (flow != null) navigator.pop() } }, enabled = draft.flow != null && model.canManage && runner.busy == 0) { Text("Save") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                FmTextField(draft.name, { draft = draft.copy(name = it) }, hint = "Pipeline name", autofocus = draft.isNew)
                Box {
                    FmListRow("Kind", subtitle = CrmFlowIcon.title(draft.icon), leading = { FmIcon(CrmFlowIcon.symbol(draft.icon)) }, onTap = { iconMenu = true }, minHeight = 52.dp)
                    FmDropdownMenu(expanded = iconMenu, onDismissRequest = { iconMenu = false }) {
                        CrmFlowIcon.ALL.forEach { icon -> FmDropdownMenuItem(CrmFlowIcon.title(icon), onClick = { iconMenu = false; draft = draft.copy(icon = icon) }, icon = CrmFlowIcon.symbol(icon)) }
                    }
                }
                FmSectionHeader("Stages", Modifier.padding(top = 8.dp))
                draft.stages.forEachIndexed { index, stage ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(9.dp).clip(CircleShape).background(CrmColors.stage(CrmText.slug(stage), draft.flow)))
                        Spacer(Modifier.width(8.dp))
                        FmTextField(stage, { value -> draft = draft.copy(stages = draft.stages.toMutableList().also { it[index] = value }) }, Modifier.weight(1f), hint = "Stage")
                        FmIconButton("arrow_upward", "Move up", onPressed = if (index > 0) ({ draft = draft.copy(stages = draft.stages.toMutableList().also { it.add(index - 1, it.removeAt(index)) }) }) else null)
                        FmIconButton("delete_outline", "Remove stage", onPressed = if (draft.stages.size > 1) ({ draft = draft.copy(stages = draft.stages.filterIndexed { i, _ -> i != index }) }) else null)
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(newStage, { newStage = it }, Modifier.weight(1f), hint = "Add a stage")
                    FmIconButton("add_circle", "Add stage", onPressed = if (newStage.isBlank()) null else ({ draft = draft.copy(stages = draft.stages + newStage.trim()); newStage = "" }))
                }
                Text("Contacts at a stage you rename move with it; contacts at a stage you remove go to the first stage.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                if (!draft.isNew && (model.data?.flows?.size ?: 0) > 1) {
                    FmToneButton("Delete pipeline", onPressed = { confirmDelete = true }, icon = "delete", tone = FmTone.Danger)
                }
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
            }
        }
        if (confirmDelete) {
            FmConfirmDialog(
                "Delete ${draft.name}?",
                onConfirm = { confirmDelete = false; runner.run({ deleteFlow(draft.id) }) { ok -> if (ok) navigator.pop() } },
                onDismissRequest = { confirmDelete = false },
                body = "Its lists are deleted too. Its contacts stay, under No pipeline, until you move them.",
                confirmLabel = "Delete",
                destructive = true,
            )
        }
    }
}

class CrmPipelinesScreen(private val model: CrmModel) : Screen() {
    override val title: String get() = "Pipelines"

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val flows = model.data?.flows.orEmpty()
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Pipelines", navigator) {
                if (model.canManage && flows.size < 12) FmIconButton("add", "New pipeline", onPressed = { navigator.push(CrmPipelineEditorScreen(model, CrmFlowDraft())) }, color = Fm.colors.textPrimary)
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(if (model.canManage) "Up to 12 pipelines. Tap one to rename it or edit its stages." else "Only owners and admins change pipelines.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                CrmRunnerStatus(runner, model)
                flows.forEachIndexed { index, flow ->
                    CrmCard(Modifier.fillMaxWidth().combinedClickableCompat({ if (model.canManage) navigator.push(CrmPipelineEditorScreen(model, CrmFlowDraft.of(flow))) })) {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                FmIcon(CrmFlowIcon.symbol(flow.icon), size = 20.dp, tint = Fm.colors.accent)
                                Spacer(Modifier.width(10.dp))
                                Text(flow.name, Modifier.weight(1f), style = Fm.text.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                                Text("${model.data?.contactsIn(flow)?.size ?: 0}", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                                if (model.canManage && index > 0) {
                                    FmIconButton("arrow_upward", "Move up", onPressed = {
                                        val order = flows.map { it.id }.toMutableList().also { it.add(index - 1, it.removeAt(index)) }
                                        runner.run({ reorderFlows(order) })
                                    })
                                }
                            }
                            CrmStageDots(flow, flow.stages.lastOrNull().orEmpty())
                            Text(flow.stages.joinToString(" → ") { CrmStageLabel.title(it) }, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2)
                        }
                    }
                }
                CrmErrorLine(model, runner)
            }
        }
    }
}

class CrmTabSettingsScreen(private val model: CrmModel, private val item: CrmTabItem) : Screen() {
    override val title: String get() = item.title

    private var name by mutableStateOf(item.title)
    private var savedName by mutableStateOf(item.title)
    private var shareText by mutableStateOf(item.view?.sharedWith?.joinToString(", ").orEmpty())
    private var access by mutableStateOf(item.view?.shareAccess ?: "view")
    private var onlyMe by mutableStateOf(item.view?.isPrivate ?: false)
    private var columns by mutableStateOf(CrmColumnCatalog.editorColumns(item.view?.columns.orEmpty(), model.data?.settings?.removedColumns.orEmpty()))
    private var history by mutableStateOf<List<CrmListActivityEntry>>(emptyList())
    private var more by mutableStateOf(false)
    private var flowMenu by mutableStateOf(false)
    private var mergeMenu by mutableStateOf(false)
    private var memberMenu by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)
    private var pendingColumn by mutableStateOf<CrmColumn?>(null)
    private var statusAt by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val view = model.data?.views?.firstOrNull { it.id == item.view?.id } ?: item.view
        fun <T> go(section: String, work: CrmModel.() -> T, done: (T) -> Unit = {}) {
            statusAt = section
            runner.run(work, done)
        }
        LaunchedEffect(view?.id) {
            if (view != null) {
                val page = withContext(Dispatchers.IO) { model.listActivity(view) }
                history = page.first
                more = page.second
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(savedName, navigator)
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(name, { name = it }, Modifier.weight(1f), hint = "Name")
                    if (name.trim() != savedName && name.isNotBlank()) FmTextButton(onClick = { go("name", { renameTab(item, name) }) { ok -> if (ok) savedName = name.trim() } }) { Text("Rename") }
                }
                if (statusAt == "name" || statusAt == "") CrmRunnerStatus(runner, model)
                FmSectionHeader("Sharing", Modifier.padding(top = 8.dp))
                if (view == null || view.canManage) {
                    if (view != null) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("Only me and the people below", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                            FmSwitch(onlyMe, { onlyMe = it })
                        }
                    }
                    Box {
                        FmTextField(shareText, { shareText = it }, hint = "ForkMesh usernames, comma separated", trailing = {
                            if (model.data?.members?.isNotEmpty() == true) FmIconButton("person_add", "Add a teammate", onPressed = { memberMenu = true })
                        })
                        FmDropdownMenu(expanded = memberMenu, onDismissRequest = { memberMenu = false }) {
                            model.data?.members.orEmpty().forEach { member ->
                                FmDropdownMenuItem(member, onClick = {
                                    memberMenu = false
                                    val names = shareText.split(',').map { it.trim() }.filter { it.isNotEmpty() }
                                    shareText = (if (member in names) names else names + member).joinToString(", ")
                                })
                            }
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("They can", style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        CrmChip("View", Fm.colors.accent, filled = access == "view", onTap = { access = "view" })
                        CrmChip("Edit", Fm.colors.accent, filled = access == "edit", onTap = { access = "edit" })
                        Spacer(Modifier.weight(1f))
                        FmToneButton(if (view == null) "Share pipeline" else "Save sharing", onPressed = {
                            go("sharing", { share(item, shareText.split(','), access, if (onlyMe) "private" else "organization") })
                        }, tone = FmTone.Accent)
                    }
                    if (statusAt == "sharing") CrmRunnerStatus(runner, model)
                } else {
                    Text("Shared with ${view.sharedWith.ifEmpty { listOf("the organization") }.joinToString(", ")}.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                if (view != null) {
                    FmSectionHeader("List", Modifier.padding(top = 8.dp))
                    Box {
                        FmListRow("Shows", subtitle = model.data?.flows?.firstOrNull { it.id == view.flow }?.let { "Pipeline: ${it.name}" } ?: "Contacts added to it", onTap = { if (view.canManage) flowMenu = true }, minHeight = 52.dp)
                        FmDropdownMenu(expanded = flowMenu, onDismissRequest = { flowMenu = false }) {
                            FmDropdownMenuItem("Contacts added to it", onClick = { flowMenu = false; go("list", { updateList(view, mapOf("flow" to "")) }) })
                            model.data?.flows.orEmpty().forEach { flow -> FmDropdownMenuItem("Pipeline: ${flow.name}", onClick = { flowMenu = false; go("list", { updateList(view, mapOf("flow" to flow.id)) }) }) }
                        }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FmToneButton("Move left", onPressed = { go("list", { moveList(view, -1) }) }, icon = "arrow_back")
                        FmToneButton("Move right", onPressed = { go("list", { moveList(view, 1) }) }, icon = "arrow_forward")
                        FmToneButton("Share link", onPressed = { crmShare(context, CrmPrompt.listLink(model.organization, view.id), view.name) }, icon = "link")
                    }
                    if (statusAt == "list") CrmRunnerStatus(runner, model)
                    Text("Owner: ${view.owner.ifEmpty { "unknown" }}" + if (view.fileName.isEmpty()) "" else " · imported from ${view.fileName}", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    FmSectionHeader("Columns", Modifier.padding(top = 8.dp), trailing = {
                        FmTextButton(onClick = { go("columns", { setColumns(view, columns) }) }) { Text("Save") }
                    })
                    if (statusAt == "columns") CrmRunnerStatus(runner, model)
                    columns.forEachIndexed { index, column ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(column.label, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                                if (column.custom) Text("Custom", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            }
                            FmIconButton("arrow_upward", "Move up", onPressed = if (index > 0) ({ columns = columns.toMutableList().also { it.add(index - 1, it.removeAt(index)) } }) else null)
                            if (column.custom || column.field in CrmColumnCatalog.DELETABLE) FmIconButton("delete_outline", "Delete column", onPressed = { pendingColumn = column })
                            FmSwitch(!column.hidden, { shown -> columns = columns.toMutableList().also { it[index] = column.copy(hidden = !shown) } })
                        }
                    }
                    val removed = model.data?.settings?.removedColumns.orEmpty()
                    if (removed.isNotEmpty()) {
                        FmSectionHeader("Deleted columns", Modifier.padding(top = 8.dp))
                        removed.forEach { field ->
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(CrmColumnCatalog.label(field), Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                                if (model.canManage) FmTextButton(onClick = {
                                    go("removed", { restoreColumn(field) }) { ok ->
                                        if (ok) CrmColumnCatalog.BUILT_IN.firstOrNull { it.first == field }?.let { columns = columns + CrmColumn(it.first, it.second, false, false) }
                                    }
                                }) { Text("Restore") }
                            }
                        }
                        Text("Restoring brings the column back for every list; values erased when it was deleted stay empty.", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        if (statusAt == "removed") CrmRunnerStatus(runner, model)
                    }
                    val others = model.data?.views.orEmpty().filter { it.id != view.id }
                    if (others.isNotEmpty()) {
                        Box {
                            FmToneButton("Merge into another list", onPressed = { mergeMenu = true }, icon = "merge")
                            FmDropdownMenu(expanded = mergeMenu, onDismissRequest = { mergeMenu = false }) {
                                others.forEach { other -> FmDropdownMenuItem(other.name, onClick = { mergeMenu = false; go("merge", { mergeList(view, other) }) { ok -> if (ok) navigator.pop() } }) }
                            }
                        }
                        if (statusAt == "merge") CrmRunnerStatus(runner, model)
                    }
                    FmSectionHeader("History", Modifier.padding(top = 8.dp))
                    if (history.isEmpty()) Text("No changes recorded yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                    history.forEach { entry ->
                        Column(Modifier.padding(vertical = 4.dp)) {
                            Text(entry.text.ifEmpty { CrmStageLabel.title(entry.action.replace('.', '-')) }, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                            Text("${entry.actor} · ${relativeTime(entry.at)}", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        }
                    }
                    if (more && history.isNotEmpty()) {
                        FmTextButton(onClick = {
                            scope.launch {
                                val page = withContext(Dispatchers.IO) { model.listActivity(view, history.last().at) }
                                history = history + page.first
                                more = page.second
                            }
                        }) { Text("Show older") }
                    }
                    if (view.canManage) FmToneButton("Delete list", onPressed = { confirmDelete = true }, icon = "delete", tone = FmTone.Danger)
                    if (statusAt == "delete") CrmRunnerStatus(runner, model)
                } else if (item.flow != null && model.canManage) {
                    FmToneButton("Edit stages", onPressed = { navigator.push(CrmPipelineEditorScreen(model, CrmFlowDraft.of(item.flow!!))) }, icon = "account_tree")
                }
                CrmErrorLine(model, runner)
                Spacer(Modifier.height(40.dp))
            }
        }
        if (confirmDelete && view != null) {
            FmConfirmDialog(
                "Delete ${view.name}?",
                onConfirm = { confirmDelete = false; go("delete", { deleteList(view) }) { ok -> if (ok) navigator.pop() } },
                onDismissRequest = { confirmDelete = false },
                body = "The contacts stay in the CRM; only the list goes.",
                confirmLabel = "Delete list",
                destructive = true,
            )
        }
        pendingColumn?.let { column ->
            FmConfirmDialog(
                "Delete the ${column.label} column?",
                onConfirm = {
                    pendingColumn = null
                    go("columns", { deleteColumn(column.field) }) { ok -> if (ok) columns = columns.filter { it.field != column.field } }
                },
                onDismissRequest = { pendingColumn = null },
                body = "Its values are erased from every contact in the organization.",
                confirmLabel = "Delete column",
                destructive = true,
            )
        }
    }
}

class CrmSettingsScreen(private val model: CrmModel) : Screen() {
    override val title: String get() = "CRM settings"

    private var edit by mutableStateOf(model.data?.settings?.let(CrmSettingsEdit::of) ?: CrmSettingsEdit("", "", "zoho", "", "", "Inbox, Follow-up, Important, Archive"))

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        val settings = model.data?.settings
        val manage = model.canManage
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("CRM settings", navigator) {
                if (manage) FmTextButton(onClick = { runner.run({ saveSettings(edit) }) { ok -> if (ok) navigator.pop() } }, enabled = runner.busy == 0) { Text("Save") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (!manage) Text("Only owners and admins change CRM settings.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                FmSectionHeader("Sender")
                FmTextField(edit.fromName, { edit = edit.copy(fromName = it) }, hint = "From name", enabled = manage)
                FmTextField(edit.fromEmail, { edit = edit.copy(fromEmail = it) }, hint = "From email", enabled = manage)
                Text(
                    if (settings?.mailtrapConfigured == true) "Emails sent from the CRM go out from this address." else "This server has no mail sender configured, so the CRM opens your mailbox instead.",
                    style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                FmSectionHeader("Mailbox", Modifier.padding(top = 8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Email links open", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    CrmChip("Zoho Mail", Fm.colors.accent, filled = edit.mailProvider == "zoho", onTap = { if (manage) edit = edit.copy(mailProvider = "zoho") })
                    CrmChip("Gmail", Fm.colors.accent, filled = edit.mailProvider == "gmail", onTap = { if (manage) edit = edit.copy(mailProvider = "gmail") })
                }
                FmTextField(edit.mailBoxes, { edit = edit.copy(mailBoxes = it) }, hint = "Mail boxes, comma separated", enabled = manage)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIcon(if (settings?.zoho?.connected == true) "verified" else "error_outline", tint = if (settings?.zoho?.connected == true) CrmColors.green else CrmColors.orange)
                    Spacer(Modifier.width(8.dp))
                    Text(if (settings?.zoho?.connected == true) "Zoho Mail connected (${settings.zoho.region})" else "Zoho Mail not connected", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    if (settings?.zoho?.connected != true && settings?.zoho?.oneClick == true && manage) {
                        FmToneButton("Connect", onPressed = { runner.run({ connectZoho() }) { url -> if (url != null) crmOpen(context, url) } }, tone = FmTone.Accent)
                    }
                }
                if (settings?.zoho?.connected != true && manage) {
                    FmListRow("Connect with a Self Client code", subtitle = "This organization only, without a server change", leading = { FmIcon("key") }, onTap = { navigator.push(CrmZohoCodeScreen(model)) })
                }
                FmSectionHeader("Sent-mail links", Modifier.padding(top = 8.dp))
                FmTextField(edit.providerName, { edit = edit.copy(providerName = it) }, hint = "Provider name", enabled = manage)
                FmTextField(edit.providerUrlTemplate, { edit = edit.copy(providerUrlTemplate = it) }, hint = "Message link template (https://…{id})", enabled = manage)
                if (settings != null && settings.updatedAt > 0) Text("Last changed ${relativeTime(settings.updatedAt)} by ${settings.updatedBy}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
            }
        }
    }
}

class CrmImportScreen(private val model: CrmModel, private val csv: String, private val fileName: String) : Screen() {
    override val title: String get() = "Import CSV"

    private var preview by mutableStateOf<CrmImportPreview?>(null)
    private var fetched by mutableStateOf(false)
    private var sheetName by mutableStateOf(fileName.substringBeforeLast('.').ifEmpty { "Imported contacts" })
    private var target by mutableStateOf("")
    private var progress by mutableStateOf<Pair<Int, Int>?>(null)
    private var result by mutableStateOf<CrmImportResult?>(null)
    private var targetMenu by mutableStateOf(false)
    private var fieldMenu by mutableStateOf(-1)

    override fun onBack(): Boolean = progress != null && result == null

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            model.clearError()
            preview = withContext(Dispatchers.IO) { model.previewImport(csv) }
            fetched = true
        }
        val rows = remember(csv) { CrmCsv.split(csv).rows }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Import CSV", navigator) {
                if (result == null) {
                    FmTextButton(onClick = {
                        val mapping = preview?.columns?.map { it.field } ?: return@FmTextButton
                        progress = 0 to 1
                        scope.launch {
                            val done = withContext(Dispatchers.IO) {
                                model.importCsv(csv, fileName, sheetName, target, mapping) { part, total -> scope.launch { progress = part to total } }
                            }
                            CrmRevision.bump()
                            if (done != null) {
                                result = done
                                model.notice = ""
                            } else {
                                progress = null
                            }
                        }
                    }, enabled = preview != null && progress == null) { Text("Import") }
                }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                ErrorLine(model.errorText)
                val done = result
                if (done != null) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("check_circle", tint = CrmColors.green)
                        Spacer(Modifier.width(8.dp))
                        Text(done.sentence, style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                    }
                    done.errors.take(20).forEach { Text(it, style = Fm.text.labelSmall, color = Fm.colors.textSecondary) }
                    FmToneButton("Done", onPressed = { navigator.pop() }, tone = FmTone.Accent, filled = true)
                } else {
                    com.forkmesh.nativeapp.ui.kit.FmKeyValueRow("File", fileName)
                    com.forkmesh.nativeapp.ui.kit.FmKeyValueRow("Rows", "$rows")
                    Box {
                        FmListRow("Into", subtitle = model.data?.views?.firstOrNull { it.id == target }?.name ?: "A new list", onTap = { targetMenu = true }, minHeight = 52.dp)
                        FmDropdownMenu(expanded = targetMenu, onDismissRequest = { targetMenu = false }) {
                            FmDropdownMenuItem("A new list", onClick = { targetMenu = false; target = "" })
                            model.data?.views.orEmpty().forEach { view -> FmDropdownMenuItem(view.name, onClick = { targetMenu = false; target = view.id }) }
                        }
                    }
                    if (target.isEmpty()) FmTextField(sheetName, { sheetName = it }, hint = "List name")
                    progress?.let { (part, total) ->
                        Text("Importing part ${minOf(part + 1, total)} of $total", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                        FmLinearProgressIndicator(progress = { part.toFloat() / maxOf(1, total) }, modifier = Modifier.fillMaxWidth())
                    }
                    val loaded = preview
                    if (loaded == null) {
                        if (!fetched) FmCircularProgressIndicator()
                    } else {
                        FmSectionHeader("Columns", Modifier.padding(top = 8.dp))
                        loaded.columns.forEachIndexed { index, column ->
                            Box {
                                FmListRow(
                                    column.header.ifEmpty { "Column ${index + 1}" },
                                    subtitle = CrmColumnCatalog.IMPORT_TARGETS.firstOrNull { it.first == column.field }?.second.orEmpty() +
                                        if (column.samples.isEmpty()) "" else " · " + column.samples.joinToString(" · "),
                                    onTap = { fieldMenu = index }, minHeight = 52.dp,
                                )
                                FmDropdownMenu(expanded = fieldMenu == index, onDismissRequest = { fieldMenu = -1 }) {
                                    CrmColumnCatalog.IMPORT_TARGETS.forEach { (field, label) ->
                                        FmDropdownMenuItem(label, onClick = {
                                            fieldMenu = -1
                                            preview = loaded.copy(columns = loaded.columns.toMutableList().also { it[index] = column.copy(field = field) })
                                        }, icon = if (field == column.field) "check" else null)
                                    }
                                }
                            }
                        }
                        Text("Email is how the CRM spots people it already has; rows without one are always added as new contacts.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                    }
                }
                Spacer(Modifier.height(40.dp))
            }
        }
    }
}
