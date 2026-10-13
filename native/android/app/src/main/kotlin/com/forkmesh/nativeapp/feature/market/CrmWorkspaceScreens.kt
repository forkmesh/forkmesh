package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.CrmActivity
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmDeckDetail
import com.forkmesh.nativeapp.market.CrmDeckLink
import com.forkmesh.nativeapp.market.CrmEmailDraft
import com.forkmesh.nativeapp.market.CrmMailMessage
import com.forkmesh.nativeapp.market.CrmModel
import com.forkmesh.nativeapp.market.CrmOrganizationData
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.CrmTeamMember
import com.forkmesh.nativeapp.market.CrmText
import com.forkmesh.nativeapp.market.CrmView
import com.forkmesh.nativeapp.market.contactByEmail
import com.forkmesh.nativeapp.market.contactById
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private fun dayKey(ms: Long): String {
    if (ms <= 0) return "Earlier"
    val date = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).toLocalDate()
    val today = LocalDate.now()
    return when (date) {
        today -> "Today"
        today.minusDays(1) -> "Yesterday"
        else -> date.format(DateTimeFormatter.ofPattern("EEEE d MMM"))
    }
}

@Composable
private fun PaneColumn(content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp), content = content)
}

@Composable
private fun MailRow(message: CrmMailMessage, contact: CrmContact?, onTap: () -> Unit, onLongPress: (() -> Unit)? = null) {
    Row(Modifier.fillMaxWidth().combinedClickableCompat(onTap, onLongPress).padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.Top) {
        CrmAvatar(contact?.displayName ?: message.who, 30)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(contact?.displayName ?: message.who, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                if (message.hasAttachment) FmIcon("attach_file", size = 13.dp, tint = Fm.colors.textSecondary)
                Text(relativeTime(message.receivedAt), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            }
            Text(message.subject.ifEmpty { "(no subject)" }, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
            if (message.summary.isNotEmpty()) Text(message.summary, maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        }
    }
}

@Composable
internal fun CrmActivityPane(model: CrmModel, data: CrmOrganizationData, navigator: Navigator) {
    val context = LocalContext.current
    var filter by remember { mutableStateOf(0) }
    val filters = listOf("All", "Sent", "Received", "Failed", "Mailbox")
    val query = model.query
    val emails = if (filter == 4) emptyList() else data.activity.filter { email ->
        val who = data.contactById(email.contactId)?.displayName.orEmpty()
        com.forkmesh.nativeapp.market.PackagesText.matches(query, email.subject, email.to, email.sender, who) && when (filter) {
            1 -> !email.incoming
            2 -> email.incoming
            3 -> email.failed
            else -> true
        }
    }
    val mail = if (filter == 0 || filter == 2 || filter == 4) data.mail.filter { it.matches(query) } else emptyList()
    val feed: List<Pair<Long, Any>> = (emails.map { it.at to (it as Any) } + mail.map { it.receivedAt to (it as Any) }).sortedByDescending { it.first }
    Column(Modifier.fillMaxSize()) {
        FmSegmentTabBar(filters.map { FmSegmentTab(it) }, filter, onChanged = { filter = it })
        PaneColumn {
            if (feed.isEmpty()) Text("No activity yet.", Modifier.fillMaxWidth().padding(32.dp), textAlign = TextAlign.Center, style = Fm.text.bodyMedium, color = Fm.colors.textTertiary)
            feed.groupBy { dayKey(it.first) }.forEach { (day, items) ->
                CrmSection(day) {
                    items.forEach { (_, item) ->
                        when (item) {
                            is CrmActivity -> CrmEmailRowView(item, data.contactById(item.contactId)?.displayName) { navigator.push(CrmEmailScreen(model, item)) }
                            is CrmMailMessage -> MailRow(item, data.contactByEmail(item.address), { if (item.providerUrl.startsWith("https://")) crmOpen(context, item.providerUrl) })
                        }
                    }
                }
            }
            Spacer(Modifier.height(60.dp))
        }
    }
}

@Composable
internal fun CrmMailPane(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner) {
    val context = LocalContext.current
    val boxes = data.settings.mailBoxes
    var box by remember { mutableStateOf(boxes.firstOrNull() ?: "Inbox") }
    val current = if (box in boxes) box else boxes.firstOrNull() ?: "Inbox"
    val messages = data.mail.filter { it.box.equals(current, ignoreCase = true) && it.matches(model.query) }
    var menuFor by remember { mutableStateOf<String?>(null) }
    PaneColumn {
        Row(Modifier.horizontalScroll(rememberScrollState()).padding(vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            boxes.forEach { name ->
                val count = data.mail.count { it.box.equals(name, ignoreCase = true) }
                CrmChip("$name $count", Fm.colors.accent, filled = name == current, onTap = { box = name })
            }
        }
        CrmCard(Modifier.fillMaxWidth().padding(top = 6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(if (data.settings.zoho.connected) "verified" else "error_outline", tint = if (data.settings.zoho.connected) CrmColors.green else CrmColors.orange)
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(if (data.settings.zoho.connected) "Zoho Mail connected" else "Zoho Mail not connected", style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    if (data.settings.zoho.lastSyncAt > 0) Text("Synced ${relativeTime(data.settings.zoho.lastSyncAt)}", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                }
                FmToneButton(if (data.settings.zoho.connected) "Sync" else "Connect", onPressed = if (runner.busy > 0) null else ({
                    runner.run({ syncMail() }) { url -> if (url != null) crmOpen(context, url) }
                }), tone = FmTone.Accent)
            }
        }
        CrmSection(current) {
            if (messages.isEmpty()) Text(if (data.mail.isEmpty()) "No mail synced yet." else "Nothing in $current.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            messages.forEach { message ->
                Box {
                    MailRow(message, data.contactByEmail(message.address), { if (message.providerUrl.startsWith("https://")) crmOpen(context, message.providerUrl) }, { menuFor = message.id })
                    FmDropdownMenu(expanded = menuFor == message.id, onDismissRequest = { menuFor = null }) {
                        boxes.filter { it != message.box }.forEach { target ->
                            FmDropdownMenuItem("Move to $target", onClick = { menuFor = null; runner.run({ moveMail(message, target) }) }, icon = "move_to_inbox")
                        }
                        if (message.providerUrl.startsWith("https://")) FmDropdownMenuItem("Open in mailbox", onClick = { menuFor = null; crmOpen(context, message.providerUrl) }, icon = "open_in_new")
                        FmDropdownMenuItem("Delete", onClick = { menuFor = null; runner.run({ deleteMail(message) }) }, icon = "delete", textColor = Fm.colors.danger)
                    }
                }
            }
        }
        Spacer(Modifier.height(60.dp))
    }
}

@Composable
internal fun CrmOutreachPane(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner, navigator: Navigator) {
    val outreach = data.outreach
    var addMenu by remember { mutableStateOf(false) }
    if (!outreach.allowed) {
        Text("Outreach is for the outreach team. Owners and admins add members to it; ask one of them to add you.", Modifier.fillMaxWidth().padding(24.dp), textAlign = TextAlign.Center, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
        return
    }
    PaneColumn {
        CrmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("${outreach.remaining} sends left today", style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
                        Text(listOf(outreach.fromName, outreach.fromEmail).filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        if (!outreach.configured) Text("The server has no mail sender configured.", style = Fm.text.labelSmall, color = CrmColors.orange)
                    }
                    FmToneButton("New", onPressed = if (outreach.remaining == 0) null else ({ navigator.push(CrmComposeScreen(model, CrmEmailDraft(), true)) }), icon = "edit", tone = FmTone.Accent, filled = true)
                }
                val share = if (outreach.dailyLimit > 0) outreach.sentToday.toFloat() / outreach.dailyLimit else 0f
                Box(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(Fm.colors.border)) {
                    Box(Modifier.fillMaxWidth(share.coerceIn(0f, 1f)).fillMaxHeight().background(if (outreach.remaining > 5) CrmColors.green else CrmColors.orange))
                }
            }
        }
        if (outreach.templates.isNotEmpty()) {
            CrmSection("Templates") {
                outreach.templates.forEach { template ->
                    FmListRow(template.label, subtitle = template.subject, onTap = {
                        navigator.push(CrmComposeScreen(model, CrmEmailDraft(subject = template.subject, body = template.body, template = template.key), true))
                    }, minHeight = 52.dp)
                }
            }
        }
        CrmSection("Recent sends") {
            if (outreach.recent.isEmpty()) Text("Nothing sent yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            outreach.recent.forEach { send ->
                Row(Modifier.padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon(if (send.ok) "check_circle" else "cancel", size = 18.dp, tint = if (send.ok) CrmColors.green else Fm.colors.danger)
                    Spacer(Modifier.width(10.dp))
                    Column {
                        Text(send.subject, maxLines = 1, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        Text(listOf(send.to, send.sender, relativeTime(send.ts)).filter { it.isNotEmpty() }.joinToString(" · "), maxLines = 1, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        if (send.error.isNotEmpty()) Text(send.error, style = Fm.text.labelSmall, color = Fm.colors.danger)
                    }
                }
            }
        }
        if (outreach.canManage) {
            val onTeam = outreach.members.map { it.name.lowercase() }.toSet()
            val candidates = data.members.filter { it.lowercase() !in onTeam }
            CrmSection("Outreach team", trailing = {
                if (candidates.isNotEmpty()) {
                    Box {
                        CrmChip("Add", Fm.colors.accent, icon = "person_add", onTap = { addMenu = true })
                        FmDropdownMenu(expanded = addMenu, onDismissRequest = { addMenu = false }) {
                            candidates.forEach { name -> FmDropdownMenuItem(name, onClick = { addMenu = false; runner.run({ setOutreachMember(name, true) }) }) }
                        }
                    }
                }
            }) {
                if (outreach.members.isEmpty()) Text("Owners and admins can always send. Add members to let them send too.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                outreach.members.forEach { member ->
                    FmListRow(member.name, subtitle = if (member.addedBy.isEmpty()) null else "Added by ${member.addedBy}", leading = { CrmAvatar(member.name, 28) }, trailing = {
                        FmIconButton("person_remove", "Remove", onPressed = { runner.run({ setOutreachMember(member.name, false) }) })
                    }, showChevron = false, minHeight = 52.dp)
                }
            }
        }
        Spacer(Modifier.height(60.dp))
    }
}

@Composable
internal fun CrmTeamPane(data: CrmOrganizationData) {
    var days by remember { mutableStateOf(7) }
    var member by remember { mutableStateOf("") }
    var memberMenu by remember { mutableStateOf(false) }
    val count = data.team.days.size
    val range = maxOf(0, count - days) until count
    val people: List<CrmTeamMember> = data.team.members.filter { member.isEmpty() || it.name == member }
    val totals = people.fold(com.forkmesh.nativeapp.market.CrmDayCounts.ZERO) { sum, person -> sum + person.sum(range) }
    val ranked = people.map { it to it.sum(range) }.sortedByDescending { it.second.total }
    PaneColumn {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            CrmChip("7 days", Fm.colors.accent, filled = days == 7, onTap = { days = 7 })
            CrmChip("30 days", Fm.colors.accent, filled = days == 30, onTap = { days = 30 })
            Spacer(Modifier.weight(1f))
            if (data.team.members.size > 1) {
                Box {
                    CrmChip(member.ifEmpty { "Everyone" }, Fm.colors.textSecondary, icon = "person", onTap = { memberMenu = true })
                    FmDropdownMenu(expanded = memberMenu, onDismissRequest = { memberMenu = false }) {
                        FmDropdownMenuItem("Everyone", onClick = { memberMenu = false; member = "" })
                        data.team.members.forEach { person -> FmDropdownMenuItem(person.name, onClick = { memberMenu = false; member = person.name }) }
                    }
                }
            }
        }
        Row(Modifier.padding(vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CrmStatTile("${totals.emails}", "Emails", "mail", CrmColors.blue)
            CrmStatTile("${totals.outreach}", "Outreach", "send", CrmColors.orange)
            CrmStatTile("${totals.contacts}", "Contacts", "person_add", CrmColors.green)
            CrmStatTile("${totals.total}", "Touches", "touch_app", CrmColors.purple)
        }
        CrmSection("Daily") {
            val perDay = range.map { index -> people.fold(com.forkmesh.nativeapp.market.CrmDayCounts.ZERO) { sum, person -> sum + person.day(index) } }
            val top = maxOf(1, perDay.maxOfOrNull { it.total } ?: 1)
            Row(Modifier.fillMaxWidth().height(130.dp).padding(12.dp), verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(if (range.count() > 10) 2.dp else 6.dp)) {
                perDay.forEach { day ->
                    Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.Bottom) {
                        if (day.contacts > 0) Box(Modifier.fillMaxWidth().weight(day.contacts.toFloat(), fill = true).background(CrmColors.green))
                        if (day.outreach > 0) Box(Modifier.fillMaxWidth().weight(day.outreach.toFloat(), fill = true).background(CrmColors.orange))
                        if (day.emails > 0) Box(Modifier.fillMaxWidth().weight(day.emails.toFloat(), fill = true).background(CrmColors.blue))
                        val empty = top - day.total
                        if (empty > 0) Spacer(Modifier.weight(empty.toFloat()))
                    }
                }
            }
            Row(Modifier.padding(start = 12.dp, end = 12.dp, bottom = 10.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                listOf("Emails" to CrmColors.blue, "Outreach" to CrmColors.orange, "Contacts" to CrmColors.green).forEach { (label, color) ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(7.dp).clip(CircleShape).background(color))
                        Spacer(Modifier.width(4.dp))
                        Text(label, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    }
                }
            }
        }
        CrmSection("Leaderboard") {
            if (ranked.isEmpty()) Text("No activity yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            ranked.forEachIndexed { index, (person, part) ->
                Row(Modifier.padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("${index + 1}", Modifier.width(18.dp), style = Fm.text.labelSmall.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textSecondary)
                    CrmAvatar(person.name, 28)
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text(person.name, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                        Text("${part.emails} emails · ${part.outreach} outreach · ${part.contacts} contacts", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    }
                    val bars = range.map { person.day(it).total }
                    val peak = maxOf(1, bars.maxOrNull() ?: 1)
                    Row(Modifier.width(80.dp).height(22.dp), verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(1.dp)) {
                        bars.forEach { value ->
                            Box(Modifier.weight(1f).fillMaxHeight(maxOf(0.08f, value.toFloat() / peak)).background(if (value > 0) Fm.colors.accent else Fm.colors.border))
                        }
                    }
                }
            }
        }
        Spacer(Modifier.height(60.dp))
    }
}

@Composable
internal fun CrmDecksPane(model: CrmModel, data: CrmOrganizationData, runner: CrmRunner, navigator: Navigator) {
    val links = data.decks.filter { link -> model.query.isEmpty() || data.contactById(link.contactId)?.matches(model.query) == true }
    PaneColumn {
        Text("${data.decks.count { it.active }} active links · ${data.decks.sumOf { it.views }} views", Modifier.padding(vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
        CrmSection("Deck links") {
            if (links.isEmpty()) Text("No deck links yet. Make one from a contact.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            links.forEach { link -> CrmDeckRowView(link, data.contactById(link.contactId)?.displayName) { navigator.push(CrmDeckScreen(model, link)) } }
        }
        Spacer(Modifier.height(60.dp))
    }
}

class CrmDeckScreen(private val model: CrmModel, private val link: CrmDeckLink) : Screen() {
    override val title: String get() = "Deck link"

    private var detail by mutableStateOf<CrmDeckDetail?>(null)
    private var fetched by mutableStateOf(false)
    private var confirmRevoke by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val runner = remember { CrmRunner(scope, model) }
        LaunchedEffect(Unit) {
            detail = withContext(Dispatchers.IO) { model.deckDetail(link) }
            fetched = true
        }
        @Suppress("UNUSED_EXPRESSION") CrmRevision.value
        val current = model.data?.decks?.firstOrNull { it.token == link.token } ?: detail?.link ?: link
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Deck link", navigator) {
                FmIconButton("content_copy", "Copy link", onPressed = { crmCopy(context, current.url); runner.status.show("Link copied.") }, color = Fm.colors.textPrimary)
                FmIconButton("share", "Share link", onPressed = { crmShare(context, current.url) }, color = Fm.colors.textPrimary)
                if (current.active) FmTextButton(onClick = { confirmRevoke = true }) { Text("Revoke", color = Fm.colors.danger) }
            }
            PaneColumn {
                CrmRunnerStatus(runner, model)
                CrmCard(Modifier.fillMaxWidth()) { CrmDeckRowView(current, model.contact(current.contactId)?.displayName) {} }
                Text("Created ${shortDate(current.createdAt)} by ${current.createdBy}", Modifier.padding(vertical = 6.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                val loaded = detail
                if (loaded == null) {
                    if (!fetched) FmCircularProgressIndicator()
                } else {
                    CrmSection("Pages") {
                        loaded.pages.forEach { page ->
                            Row(Modifier.padding(horizontal = 14.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                                Text("${page.page}", Modifier.width(20.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                                Column(Modifier.weight(1f)) {
                                    Text(page.title, maxLines = 1, style = Fm.text.labelSmall, color = Fm.colors.textPrimary)
                                    Box(Modifier.fillMaxWidth().height(5.dp).clip(RoundedCornerShape(3.dp)).background(Fm.colors.border)) {
                                        Box(Modifier.fillMaxWidth(maxOf(0.02f, page.seconds.toFloat() / maxOf(1, loaded.longestPage))).fillMaxHeight().background(if (page.viewers > 0) CrmColors.orange else Fm.colors.border))
                                    }
                                }
                                Spacer(Modifier.width(8.dp))
                                Text(CrmText.duration(page.seconds), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                            }
                        }
                    }
                    if (loaded.sessions.isNotEmpty()) {
                        CrmSection("Sessions") {
                            loaded.sessions.forEach { session ->
                                FmListRow(
                                    relativeTime(session.startedAt),
                                    subtitle = listOf(CrmText.duration(session.seconds), "${session.pages} pages", session.country, session.agent).filter { it.isNotEmpty() }.joinToString(" · "),
                                    showChevron = false, minHeight = 52.dp,
                                )
                            }
                        }
                    }
                }
                Spacer(Modifier.height(40.dp))
            }
        }
        if (confirmRevoke) {
            FmConfirmDialog(
                "Revoke this deck link?",
                onConfirm = { confirmRevoke = false; runner.run({ revoke(current) }) },
                onDismissRequest = { confirmRevoke = false },
                body = "The link stops opening the deck. Its view history stays.",
                confirmLabel = "Revoke",
                destructive = true,
            )
        }
    }
}

@Composable
internal fun CrmSharedPane(model: CrmModel, data: CrmOrganizationData, navigator: Navigator) {
    PaneColumn {
        Text("Lists people in other organizations shared with you. Copy one to bring its contacts into ${model.organization}.", Modifier.padding(vertical = 6.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
        CrmSection("Shared with me") {
            data.sharedViews.forEach { view ->
                FmListRow(view.name, subtitle = "${view.organization} · shared by ${view.owner}", trailing = {
                    CrmChip(if (view.canEdit) "Can edit" else "View only", if (view.canEdit) CrmColors.green else Fm.colors.textSecondary)
                }, onTap = { navigator.push(CrmSharedViewScreen(model, view)) }, minHeight = 56.dp)
            }
        }
    }
}

class CrmSharedViewScreen(private val model: CrmModel, private val view: CrmView) : Screen() {
    override val title: String get() = view.name

    private var bundle by mutableStateOf<Pair<CrmView, List<CrmContact>>?>(null)
    private var fetched by mutableStateOf(false)
    private var stageMenu by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val runner = remember { CrmRunner(scope, model) }
        LaunchedEffect(Unit) {
            bundle = withContext(Dispatchers.IO) { model.sharedView(view) }
            fetched = true
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(view.name, navigator) {
                FmTextButton(onClick = { runner.run({ copySharedView(view) }) { result -> if (result != null) navigator.pop() } }, enabled = runner.busy == 0) { Text("Copy to ${model.organization}") }
            }
            CrmRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            val loaded = bundle
            when {
                loaded == null && !fetched -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                loaded == null -> ErrorLine(model.errorText.ifEmpty { "That list is no longer shared with you." })
                else -> PaneColumn {
                    val (shared, contacts) = loaded
                    Text("${contacts.size} contacts from ${shared.organization}", Modifier.padding(vertical = 6.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    CrmSection(shared.name) {
                        contacts.forEach { contact ->
                            Box {
                                Box(Modifier.combinedClickableCompat({ if (shared.canEdit && shared.stages.isNotEmpty()) stageMenu = contact.id })) {
                                    CrmContactRowContent(contact, null, showStage = shared.isPipeline, columns = shared.summaryColumns)
                                }
                                FmDropdownMenu(expanded = stageMenu == contact.id, onDismissRequest = { stageMenu = null }) {
                                    shared.stages.forEach { stage ->
                                        FmDropdownMenuItem(CrmStageLabel.title(stage), onClick = {
                                            stageMenu = null
                                            runner.run({ updateSharedContact(shared, contact, mapOf("stage" to stage)) }) { saved ->
                                                if (saved != null) bundle = shared to contacts.map { if (it.id == saved.id) saved else it }
                                            }
                                        }, icon = if (stage == contact.stage) "check" else null)
                                    }
                                }
                            }
                        }
                    }
                    Spacer(Modifier.height(40.dp))
                }
            }
        }
    }
}
