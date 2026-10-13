package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
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
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmActivity
import com.forkmesh.nativeapp.market.CrmBulkAction
import com.forkmesh.nativeapp.market.CrmColumnCatalog
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmContactEdit
import com.forkmesh.nativeapp.market.CrmContactRef
import com.forkmesh.nativeapp.market.CrmDeckLink
import com.forkmesh.nativeapp.market.CrmEmailDraft
import com.forkmesh.nativeapp.market.CrmFlowIcon
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmPrompt
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.market.customFieldKeys
import com.forkmesh.nativeapp.market.decksFor
import com.forkmesh.nativeapp.market.duplicatesOf
import com.forkmesh.nativeapp.market.listViews
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

@Composable
internal fun ColumnScope.CrmSection(title: String, trailing: (@Composable () -> Unit)? = null, content: @Composable ColumnScope.() -> Unit) {
    FmSectionHeader(title, Modifier.padding(top = 10.dp, bottom = 2.dp), trailing = trailing)
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(vertical = 4.dp),
        content = content,
    )
}

@Composable
private fun DetailLine(label: String, value: String, icon: String, url: String? = null) {
    if (value.isEmpty()) return
    val context = LocalContext.current
    Row(
        Modifier.fillMaxWidth().combinedClickableCompat({ if (url != null) crmOpen(context, url) else crmCopy(context, value) }, { crmCopy(context, value) })
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 18.dp, tint = Fm.colors.textSecondary)
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
            Text(value, style = Fm.text.bodyMedium, color = if (url != null) Fm.colors.accent else Fm.colors.textPrimary, maxLines = 3, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun RowScopeAction(label: String, icon: String, tint: Color, enabled: Boolean, modifier: Modifier, onTap: () -> Unit) {
    Column(
        modifier.clip(RoundedCornerShape(12.dp)).background(tint.copy(alpha = if (enabled) 0.12f else 0.05f))
            .combinedClickableCompat({ if (enabled) onTap() }).padding(vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        FmIcon(icon, size = 20.dp, tint = if (enabled) tint else Fm.colors.textDisabled)
        Text(label, style = Fm.text.labelSmall, color = if (enabled) tint else Fm.colors.textDisabled, maxLines = 1)
    }
}

class ContactScreen(private val ref: CrmContactRef) : Screen() {
    override val title: String get() = "Contact"

    private val model = MarketFeature.crm()
    private var loading by mutableStateOf(false)
    private var menu by mutableStateOf(false)
    private var flowMenu by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        LaunchedEffect(Unit) {
            loading = true
            if (model.organization != ref.organization) model.organization = ref.organization
            withContext(Dispatchers.IO) { model.load() }
            loading = false
            CrmRevision.bump()
        }
        val top = navigator.top
        var seen by remember { mutableStateOf(false) }
        LaunchedEffect(top) {
            if (top === this@ContactScreen) {
                if (seen && model.data != null && model.contact(ref.contactId) == null) navigator.pop()
                seen = true
                CrmRevision.bump()
            }
        }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val data = model.data
        val contact = model.contact(ref.contactId)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(contact?.displayName ?: "Contact", navigator) {
                if (contact != null) {
                    FmTextButton(onClick = { navigator.push(ContactEditorScreen(model, contact)) }) { Text("Edit") }
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            val prompt = CrmPrompt.outreach(contact, model.activeTitle, data?.flow(contact.flow)?.name ?: CrmStageLabel.title(contact.flow))
                            FmDropdownMenuItem("Draft outreach with an agent", onClick = { menu = false; NavigationFeature.compose(ComposeTarget(text = prompt)) }, icon = "auto_awesome")
                            FmDropdownMenuItem("Copy prompt", onClick = { menu = false; crmCopy(context, prompt); runner.status.show("Copied a prompt about ${contact.displayName}.") }, icon = "content_copy")
                            FmDropdownMenuItem("Copy link", onClick = {
                                menu = false
                                crmCopy(context, CrmPrompt.contactLink(model.organization, contact.id))
                                runner.status.show("Copied a link to ${contact.displayName}. It opens for members of ${model.organization}.")
                            }, icon = "link")
                            FmDropdownMenuItem("Merge into…", onClick = { menu = false; navigator.push(CrmMergeScreen(model, contact)) }, icon = "merge")
                            FmDropdownMenuItem("Delete contact", onClick = { menu = false; confirmDelete = true }, icon = "delete", textColor = Fm.colors.danger)
                        }
                    }
                }
            }
            if (confirmDelete && contact != null) {
                FmConfirmDialog(
                    "Delete ${contact.displayName}?",
                    onConfirm = { confirmDelete = false; runner.run({ delete(contact) }) { ok -> if (ok) navigator.pop() } },
                    onDismissRequest = { confirmDelete = false },
                    body = "The contact and its pipeline history are removed for everyone in ${ref.organization}.",
                    confirmLabel = "Delete",
                    destructive = true,
                )
            }
            when {
                loading && contact == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                contact == null || data == null -> FmEmptyState("person_off", "Contact not found", message = model.errorText.ifEmpty { null })
                else -> Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    CrmRunnerStatus(runner, model)
                    val flow = data.flow(contact.flow)
                    val tint = CrmColors.stage(contact.stage, flow)
                    Column(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp))
                            .background(Brush.linearGradient(listOf(tint.copy(alpha = 0.2f), Fm.colors.bgRaised))).padding(14.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CrmAvatar(contact.name.ifEmpty { contact.email }, 60)
                            Spacer(Modifier.width(14.dp))
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                Text(contact.displayName, style = Fm.text.titleLarge.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary, maxLines = 2)
                                if (contact.subtitle.isNotEmpty()) Text(contact.subtitle, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary, maxLines = 2)
                                Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                                    CrmChip(flow.name, Fm.colors.textSecondary, icon = CrmFlowIcon.symbol(flow.icon))
                                    CrmChip(CrmStageLabel.title(contact.stage), tint)
                                    if (contact.value > 0) CrmChip(CrmText.money(contact.value), CrmColors.green, icon = "payments")
                                }
                            }
                        }
                        if (contact.stage in flow.stages) CrmStageDots(flow, contact.stage)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        RowScopeAction("Email", "mail", CrmColors.blue, contact.email.isNotEmpty(), Modifier.weight(1f)) { navigator.push(CrmComposeScreen(model, CrmEmailDraft.of(contact), false)) }
                        RowScopeAction("Call", "call", CrmColors.green, contact.phone.isNotEmpty(), Modifier.weight(1f)) { crmOpen(context, "tel:" + contact.phone.filter { !it.isWhitespace() }) }
                        RowScopeAction("Contacted", "mark_email_read", Color(0xFF0D9488), true, Modifier.weight(1f)) { runner.run({ markContacted(listOf(contact.id)) }) { ok -> if (ok) runner.status.show("Marked as contacted now.") } }
                        RowScopeAction("Deck", "slideshow", CrmColors.orange, true, Modifier.weight(1f)) { runner.run({ deckLink(contact) }) { link -> if (link != null) navigator.push(CrmDeckScreen(model, link)) } }
                    }
                    CrmSection("Pipeline") {
                        Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            (if (contact.stage in flow.stages) flow.stages else flow.stages + contact.stage).forEach { stage ->
                                val active = stage == contact.stage
                                CrmChip(CrmStageLabel.title(stage), CrmColors.stage(stage, flow), icon = if (active) "check" else null, filled = active, onTap = {
                                    if (!active) runner.run({ move(contact, stage) })
                                })
                            }
                        }
                        Box {
                            FmListRow("Pipeline", subtitle = flow.name, onTap = { flowMenu = true }, minHeight = 48.dp)
                            FmDropdownMenu(expanded = flowMenu, onDismissRequest = { flowMenu = false }) {
                                data.flows.forEach { target ->
                                    FmDropdownMenuItem(target.name, onClick = { flowMenu = false; runner.run({ moveFlow(contact, target.id) }) }, icon = if (target.id == contact.flow) "check" else CrmFlowIcon.symbol(target.icon))
                                }
                            }
                        }
                        DetailLine("Last contacted", if (contact.lastContactedAt > 0) relativeTime(contact.lastContactedAt) else "Never", "schedule")
                    }
                    if (data.listViews.isNotEmpty()) {
                        CrmSection("Lists") {
                            Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                data.listViews.forEach { view ->
                                    val member = view.id in contact.views
                                    CrmChip(view.name, Fm.colors.accent, icon = if (member) "check" else "add", filled = member, onTap = {
                                        runner.run({ bulk(if (member) CrmBulkAction.RemoveList(view.id) else CrmBulkAction.AddList(view.id), listOf(contact.id)) })
                                    })
                                }
                            }
                        }
                    }
                    CrmSection("Details") {
                        DetailLine("Email", contact.email, "mail", if (contact.email.isNotEmpty()) "mailto:${contact.email}" else null)
                        DetailLine("Phone", contact.phone, "call", "tel:" + contact.phone.filter { !it.isWhitespace() })
                        DetailLine("Website", contact.website, "public", CrmText.profileUrl("website", contact.website))
                        DetailLine("Location", contact.location, "place")
                        DetailLine("LinkedIn", contact.linkedin, "work", CrmText.profileUrl("linkedin", contact.linkedin))
                        DetailLine("GitHub", contact.github, "code", CrmText.profileUrl("github", contact.github))
                        DetailLine("Mastodon", contact.mastodon, "forum", CrmText.profileUrl("mastodon", contact.mastodon))
                        DetailLine("Bluesky", contact.bluesky, "cloud", CrmText.profileUrl("bluesky", contact.bluesky))
                        DetailLine("X", contact.twitter, "alternate_email", CrmText.profileUrl("twitter", contact.twitter))
                        DetailLine("Next step", contact.nextStep, "subdirectory_arrow_right")
                        DetailLine("Owner", contact.owner, "person")
                        DetailLine("Source", contact.source, "input")
                        if (contact.tags.isNotEmpty()) {
                            Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 14.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                contact.tags.forEach { CrmChip("#$it", CrmColors.purple) }
                            }
                        }
                    }
                    if (contact.fields.isNotEmpty()) {
                        CrmSection("More") {
                            contact.fields.keys.sorted().forEach { key -> DetailLine(CrmColumnCatalog.label(key), contact.fields[key].orEmpty(), "label") }
                        }
                    }
                    if (contact.notes.isNotEmpty()) {
                        CrmSection("Notes") { Text(contact.notes, Modifier.padding(14.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary) }
                    }
                    val emails = data.activityFor(contact.id)
                    val decks = data.decksFor(contact.id)
                    var logMenu by remember { mutableStateOf(false) }
                    CrmSection("Timeline", trailing = {
                        if (contact.email.isNotEmpty()) {
                            Box {
                                CrmChip("Log", Fm.colors.accent, icon = "add", onTap = { logMenu = true })
                                FmDropdownMenu(expanded = logMenu, onDismissRequest = { logMenu = false }) {
                                    FmDropdownMenuItem("Email I sent", onClick = { logMenu = false; runner.run({ logEmail(contact.email, "Email to ${contact.displayName}", contact.id) }) }, icon = "outbox")
                                    FmDropdownMenuItem("Email I received", onClick = { logMenu = false; runner.run({ logEmail(contact.email, "Email from ${contact.displayName}", contact.id, "inbound") }) }, icon = "inbox")
                                }
                            }
                        }
                    }) {
                        decks.forEach { link -> CrmDeckRowView(link, null) { navigator.push(CrmDeckScreen(model, link)) } }
                        emails.forEach { email -> CrmEmailRowView(email, null) { navigator.push(CrmEmailScreen(model, email)) } }
                        if (emails.isEmpty() && decks.isEmpty()) Text("No emails or deck views yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                    }
                    val duplicates = data.duplicatesOf(contact)
                    if (duplicates.isNotEmpty()) {
                        CrmSection("Possible duplicates") {
                            duplicates.forEach { other ->
                                Box(Modifier.combinedClickableCompat({ navigator.push(CrmMergeScreen(model, other, contact.id)) })) {
                                    CrmContactRowContent(other, data.flow(other.flow), showStage = true)
                                }
                            }
                        }
                    }
                    Text(
                        "Added ${shortDate(contact.createdAt)} by ${contact.createdBy.ifEmpty { "someone" }} · updated ${relativeTime(contact.updatedAt)} by ${contact.updatedBy.ifEmpty { contact.createdBy }}",
                        Modifier.padding(vertical = 8.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                    )
                    Spacer(Modifier.height(40.dp))
                }
            }
        }
    }
}

@Composable
internal fun CrmEmailRowView(email: CrmActivity, who: String?, onTap: () -> Unit) {
    val tint = when {
        email.failed -> Fm.colors.danger
        email.engaged -> CrmColors.green
        email.delivered -> CrmColors.blue
        else -> Fm.colors.textSecondary
    }
    Row(Modifier.fillMaxWidth().combinedClickableCompat(onTap).padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.clip(RoundedCornerShape(50)).background(tint.copy(alpha = 0.12f)).padding(6.dp)) {
            FmIcon(if (email.failed) "error" else if (email.incoming) "inbox" else if (email.engaged) "visibility" else "send", size = 14.dp, tint = tint)
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(email.subject.ifEmpty { "(no subject)" }, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.Medium), color = Fm.colors.textPrimary)
            Text(listOf(who ?: email.to, email.sender, relativeTime(email.at)).filter { it.isNotEmpty() }.joinToString(" · "), maxLines = 1, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        }
        CrmChip(email.status, tint)
    }
}

@Composable
internal fun CrmDeckRowView(link: CrmDeckLink, who: String?, onTap: () -> Unit) {
    Row(Modifier.fillMaxWidth().combinedClickableCompat(onTap).padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.clip(RoundedCornerShape(50)).background((if (link.active) CrmColors.orange else Fm.colors.textSecondary).copy(alpha = 0.12f)).padding(6.dp)) {
            FmIcon("slideshow", size = 14.dp, tint = if (link.active) CrmColors.orange else Fm.colors.textSecondary)
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(who ?: "Investor deck", maxLines = 1, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.Medium), color = Fm.colors.textPrimary)
                if (!link.active) CrmChip("Revoked", Fm.colors.textSecondary)
            }
            Text(
                if (link.views == 0) "Not opened yet" else "${link.views} view${if (link.views == 1) "" else "s"} · ${CrmText.duration(link.seconds)} · ${relativeTime(link.lastViewedAt)}",
                style = Fm.text.labelSmall, color = Fm.colors.textSecondary,
            )
            if (link.pageCount > 0) {
                Box(Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)).background(Fm.colors.border)) {
                    Box(Modifier.fillMaxWidth(link.pageShare.toFloat()).height(4.dp).background(CrmColors.orange))
                }
            }
        }
    }
}

class ContactEditorScreen(private val model: CrmModel, private val original: CrmContact?, private val addTo: String? = null) : Screen() {
    override val title: String get() = if (original == null) "New contact" else "Edit contact"

    private val startFlow = model.activeFlow ?: model.flow
    private var edit by mutableStateOf(
        original?.let(CrmContactEdit::of)
            ?: CrmContactEdit(flow = startFlow.id, stage = model.filter.stage.ifEmpty { startFlow.stages.firstOrNull() ?: "lead" }),
    )
    private var value by mutableStateOf(original?.value?.takeIf { it > 0 }?.toString() ?: "")
    private var newField by mutableStateOf("")
    private var newValue by mutableStateOf("")
    private var stageMenu by mutableStateOf(false)
    private var flowMenu by mutableStateOf(false)
    private var ownerMenu by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        val data = model.data
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = {
                    val next = edit.copy(value = value.trim().toLongOrNull() ?: 0)
                    runner.run({ save(next, original, addTo) }) { saved -> if (saved != null) navigator.pop() }
                }, enabled = runner.busy == 0 && edit.isValid) { Text(if (runner.busy > 0) "Saving…" else "Save") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                FmSectionHeader("Person")
                FmTextField(edit.name, { edit = edit.copy(name = it) }, hint = "Name", autofocus = original == null)
                FmTextField(edit.email, { edit = edit.copy(email = it) }, hint = "Email")
                FmTextField(edit.company, { edit = edit.copy(company = it) }, hint = "Company")
                FmTextField(edit.title, { edit = edit.copy(title = it) }, hint = "Title")
                FmTextField(edit.phone, { edit = edit.copy(phone = it) }, hint = "Phone")
                FmSectionHeader("Pipeline", Modifier.padding(top = 8.dp))
                val flows = data?.flows.orEmpty()
                val flow = data?.flow(edit.flow) ?: model.flow
                Box {
                    FmListRow("Pipeline", subtitle = flow.name, onTap = { flowMenu = true }, minHeight = 48.dp)
                    FmDropdownMenu(expanded = flowMenu, onDismissRequest = { flowMenu = false }) {
                        flows.forEach { target ->
                            FmDropdownMenuItem(target.name, onClick = {
                                flowMenu = false
                                edit = edit.copy(flow = target.id, stage = if (edit.stage in target.stages) edit.stage else target.stages.firstOrNull() ?: "lead")
                            })
                        }
                    }
                }
                Box {
                    FmListRow("Stage", subtitle = CrmStageLabel.title(edit.stage), onTap = { stageMenu = true }, minHeight = 48.dp)
                    FmDropdownMenu(expanded = stageMenu, onDismissRequest = { stageMenu = false }) {
                        flow.stages.forEach { stage -> FmDropdownMenuItem(CrmStageLabel.title(stage), onClick = { stageMenu = false; edit = edit.copy(stage = stage) }) }
                    }
                }
                FmTextField(value, { value = it.filter(Char::isDigit) }, hint = "Deal value")
                FmTextField(edit.nextStep, { edit = edit.copy(nextStep = it) }, hint = "Next step")
                Box {
                    FmTextField(edit.owner, { edit = edit.copy(owner = it) }, hint = "Owner", trailing = {
                        if (data?.members?.isNotEmpty() == true) FmIconButton("person_add", "Pick a member", onPressed = { ownerMenu = true })
                    })
                    FmDropdownMenu(expanded = ownerMenu, onDismissRequest = { ownerMenu = false }) {
                        data?.members.orEmpty().forEach { member -> FmDropdownMenuItem(member, onClick = { ownerMenu = false; edit = edit.copy(owner = member) }) }
                    }
                }
                FmTextField(edit.source, { edit = edit.copy(source = it) }, hint = "Source")
                FmTextField(edit.tags, { edit = edit.copy(tags = it) }, hint = "Tags, comma separated")
                CrmLastContactedField(edit.lastContactedAt) { edit = edit.copy(lastContactedAt = it) }
                FmSectionHeader("Online", Modifier.padding(top = 8.dp))
                FmTextField(edit.website, { edit = edit.copy(website = it) }, hint = "Website")
                FmTextField(edit.location, { edit = edit.copy(location = it) }, hint = "Location")
                FmTextField(edit.linkedin, { edit = edit.copy(linkedin = it) }, hint = "LinkedIn")
                FmTextField(edit.github, { edit = edit.copy(github = it) }, hint = "GitHub")
                FmTextField(edit.mastodon, { edit = edit.copy(mastodon = it) }, hint = "Mastodon (@name@server)")
                FmTextField(edit.bluesky, { edit = edit.copy(bluesky = it) }, hint = "Bluesky")
                FmTextField(edit.twitter, { edit = edit.copy(twitter = it) }, hint = "X / Twitter")
                FmSectionHeader("More fields", Modifier.padding(top = 8.dp))
                val known = data?.customFieldKeys().orEmpty()
                (known + edit.fields.keys.sorted().filter { it !in known }).forEach { key ->
                    FmTextField(edit.fields[key].orEmpty(), { edit = edit.copy(fields = edit.fields + (key to it)) }, hint = CrmColumnCatalog.label(key))
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(newField, { newField = it }, Modifier.weight(1f), hint = "New field")
                    FmTextField(newValue, { newValue = it }, Modifier.weight(1f), hint = "Value")
                    FmIconButton("add_circle", "Add field", onPressed = {
                        val key = CrmText.slug(newField).replace('-', '_')
                        if (key.isNotEmpty() && !CrmColumnCatalog.isBuiltIn(key)) {
                            edit = edit.copy(fields = edit.fields + (key to newValue))
                            newField = ""
                            newValue = ""
                        }
                    })
                }
                FmSectionHeader("Notes", Modifier.padding(top = 8.dp))
                FmTextField(edit.notes, { edit = edit.copy(notes = it) }, hint = "Notes", minLines = 3, maxLines = 12)
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
                Spacer(Modifier.height(40.dp))
            }
        }
    }
}

class CrmMergeScreen(private val model: CrmModel, private val contact: CrmContact, private val keepId: String? = null) : Screen() {
    override val title: String get() = "Merge"

    private var query by mutableStateOf("")
    private var target by mutableStateOf<CrmContact?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        val data = model.data ?: return
        LaunchedEffect(keepId) { if (keepId != null) target = model.contact(keepId) }
        val duplicates = data.duplicatesOf(contact)
        val others = data.contacts.filter { it.id != contact.id && it !in duplicates && it.matches(query) }
        val candidates = if (query.isEmpty()) duplicates + others.take(40) else duplicates.filter { it.matches(query) } + others
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Merge ${contact.displayName}", navigator)
            Text(
                "Its details fill the blanks of the contact you keep, its email history and deck links move over, and then it is deleted.",
                Modifier.padding(horizontal = 16.dp, vertical = 8.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
            )
            FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp), hint = "Find the contact to keep")
            CrmRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            LazyColumn(Modifier.fillMaxSize()) {
                items(candidates, key = { it.id }) { other ->
                    Box(Modifier.background(Fm.colors.bgRaised).combinedClickableCompat({ target = other })) {
                        CrmContactRowContent(other, data.flow(other.flow), showStage = true)
                    }
                }
            }
        }
        target?.let { kept ->
            FmConfirmDialog(
                "Merge ${contact.displayName} into ${kept.displayName}?",
                onConfirm = { target = null; runner.run({ merge(contact, kept) }) { merged -> if (merged != null) navigator.pop() } },
                onDismissRequest = { target = null },
                confirmLabel = "Merge",
                destructive = true,
            )
        }
    }
}

class CrmOwnerScreen(private val model: CrmModel, private val ids: List<String>) : Screen() {
    override val title: String get() = "Set owner"

    private var owner by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Owner of ${ids.size} contact${if (ids.size == 1) "" else "s"}", navigator) {
                FmTextButton(onClick = { runner.run({ bulk(CrmBulkAction.Owner(owner.trim()), ids) }) { ok -> if (ok) navigator.pop() } }) { Text("Apply") }
            }
            FmTextField(owner, { owner = it }, Modifier.padding(16.dp), hint = "Owner (empty clears it)")
            CrmRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
            LazyColumn {
                items(model.data?.members.orEmpty()) { member ->
                    FmListRow(member, leading = { CrmAvatar(member, 28) }, trailing = { if (owner == member) FmIcon("check", tint = Fm.colors.accent) }, showChevron = false, onTap = { owner = member })
                }
            }
        }
    }
}

class CrmEmailScreen(private val model: CrmModel, private val email: CrmActivity) : Screen() {
    override val title: String get() = "Email"

    private var detail by mutableStateOf<CrmActivity?>(null)
    private var fetched by mutableStateOf(false)
    private var statusMenu by mutableStateOf(false)
    private var confirmDelete by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        LaunchedEffect(Unit) {
            detail = withContext(Dispatchers.IO) { model.emailDetail(email.id) }
            fetched = true
        }
        val shown = detail ?: email
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Email", navigator) {
                if (shown.providerUrl.startsWith("https://")) FmIconButton("open_in_new", "Open in mailbox", onPressed = { crmOpen(context, shown.providerUrl) }, color = Fm.colors.textPrimary)
                Box {
                    FmIconButton("flag", "Set status", onPressed = { statusMenu = true }, color = Fm.colors.textPrimary)
                    FmDropdownMenu(expanded = statusMenu, onDismissRequest = { statusMenu = false }) {
                        CrmActivity.STATUSES.forEach { status ->
                            FmDropdownMenuItem(CrmStageLabel.title(status), onClick = { statusMenu = false; runner.run({ setEmailStatus(shown, status) }) { updated -> if (updated != null) detail = updated } })
                        }
                    }
                }
                FmIconButton("delete", "Delete record", onPressed = { confirmDelete = true }, color = Fm.colors.textPrimary)
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                CrmRunnerStatus(runner, model)
                Text(shown.subject.ifEmpty { "(no subject)" }, style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    CrmChip(shown.status, if (shown.failed) Fm.colors.danger else CrmColors.blue)
                    CrmChip(if (shown.incoming) "Received" else "Sent", Fm.colors.textSecondary)
                    if (shown.provider.isNotEmpty()) CrmChip(shown.provider, Fm.colors.textSecondary)
                }
                CrmCard(Modifier.fillMaxWidth()) {
                    Column {
                        listOf("To" to shown.to, "From" to listOf(shown.fromName, shown.fromEmail).filter { it.isNotEmpty() }.joinToString(" "), "By" to shown.sender, "When" to relativeTime(shown.at))
                            .filter { it.second.isNotEmpty() }.forEach { (label, text) -> com.forkmesh.nativeapp.ui.kit.FmKeyValueRow(label, text) }
                    }
                }
                if (shown.error.isNotEmpty()) Text(shown.error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                when {
                    shown.plainBody.isNotEmpty() -> Text(shown.plainBody, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    !fetched -> FmCircularProgressIndicator()
                }
            }
        }
        if (confirmDelete) {
            FmConfirmDialog(
                "Delete this email record?",
                onConfirm = { confirmDelete = false; runner.run({ deleteEmail(email) }) { ok -> if (ok) navigator.pop() } },
                onDismissRequest = { confirmDelete = false },
                body = "Only the CRM's record goes; the message stays in the mailbox.",
                confirmLabel = "Delete",
                destructive = true,
            )
        }
    }
}

class CrmComposeScreen(private val model: CrmModel, start: CrmEmailDraft, outreach: Boolean) : Screen() {
    override val title: String get() = "Email"

    private var draft by mutableStateOf(start)
    private val modes: List<String> = buildList {
        if (model.data?.settings?.mailtrapConfigured == true) add("crm")
        if (model.data?.outreach?.allowed == true) add("outreach")
        add("mailbox")
    }
    private var mode by mutableStateOf(
        when {
            outreach && "outreach" in modes -> "outreach"
            "crm" in modes && model.data?.settings?.fromEmail?.isNotEmpty() == true -> "crm"
            "outreach" in modes -> "outreach"
            else -> "mailbox"
        },
    )
    private var templateMenu by mutableStateOf(false)
    private var logIt by mutableStateOf(true)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        val data = model.data
        val outreach = data?.outreach
        val contact = model.contact(draft.contactId)
        val canSend = when (mode) {
            "mailbox" -> '@' in draft.to
            "outreach" -> draft.isValid && (outreach?.remaining ?: 0) > 0
            else -> draft.isValid
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(contact?.let { "Email ${it.displayName}" } ?: "New email", navigator) {
                FmTextButton(onClick = {
                    when (mode) {
                        "crm" -> runner.run({ sendEmail(draft) }) { sent -> if (sent != null) navigator.pop() }
                        "outreach" -> runner.run({ sendOutreach(draft) }) { ok -> if (ok) navigator.pop() }
                        else -> {
                            crmOpen(context, data?.settings?.composeUrl(draft.to, draft.subject, draft.body) ?: "mailto:${draft.to}")
                            if (logIt && draft.subject.isNotBlank()) runner.run({ logEmail(draft.to, draft.subject, draft.contactId) }) { navigator.pop() } else navigator.pop()
                        }
                    }
                }, enabled = canSend && runner.busy == 0) { Text(if (mode == "mailbox") "Open" else "Send") }
            }
            if (modes.size > 1) {
                val labels = modes.map { when (it) { "crm" -> "CRM send"; "outreach" -> "Outreach"; else -> "Mailbox" } }
                FmSegmentTabBar(labels.map { FmSegmentTab(it) }, modes.indexOf(mode).coerceAtLeast(0), onChanged = { mode = modes[it] })
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    when (mode) {
                        "crm" -> data?.settings?.fromEmail?.takeIf { it.isNotEmpty() }?.let { "Sent from $it and tracked on the contact." } ?: "Set a sender address in CRM settings first."
                        "outreach" -> "From ${outreach?.fromEmail.orEmpty()} · ${outreach?.sentToday ?: 0} of ${outreach?.dailyLimit ?: 50} sent today."
                        else -> if (data?.settings?.mailProvider == "gmail") "Opens Gmail with the message filled in." else "Opens Zoho Mail with the message filled in."
                    },
                    style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                FmTextField(draft.to, { draft = draft.copy(to = it) }, hint = "To")
                if (mode == "outreach" && outreach != null && outreach.templates.isNotEmpty()) {
                    Box {
                        FmListRow("Template", subtitle = outreach.templates.firstOrNull { it.key == draft.template }?.label ?: "Blank", onTap = { templateMenu = true }, minHeight = 48.dp)
                        FmDropdownMenu(expanded = templateMenu, onDismissRequest = { templateMenu = false }) {
                            FmDropdownMenuItem("Blank", onClick = { templateMenu = false; draft = draft.copy(template = "blank") })
                            outreach.templates.forEach { template ->
                                FmDropdownMenuItem(template.label, onClick = {
                                    templateMenu = false
                                    val (subject, body) = template.filled(contact)
                                    draft = draft.copy(template = template.key, subject = subject, body = body)
                                })
                            }
                        }
                    }
                }
                FmTextField(draft.subject, { draft = draft.copy(subject = it) }, hint = "Subject")
                FmTextField(draft.body, { draft = draft.copy(body = it) }, hint = "Message", minLines = 8, maxLines = 24)
                if (mode == "mailbox") {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Log it on the contact", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        FmSwitch(logIt, { logIt = it })
                    }
                }
                CrmErrorLine(model, runner)
                CrmRunnerStatus(runner, model)
            }
        }
    }
}
