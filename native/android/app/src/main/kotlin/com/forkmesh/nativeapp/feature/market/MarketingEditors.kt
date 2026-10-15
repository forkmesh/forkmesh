package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.market.MarketingAppStore
import com.forkmesh.nativeapp.market.MarketingDraft
import com.forkmesh.nativeapp.market.MarketingField
import com.forkmesh.nativeapp.market.MarketingFieldKind
import com.forkmesh.nativeapp.market.MarketingForms
import com.forkmesh.nativeapp.market.MarketingKeywordImport
import com.forkmesh.nativeapp.market.MarketingKind
import com.forkmesh.nativeapp.market.MarketingLink
import com.forkmesh.nativeapp.market.MarketingModel
import com.forkmesh.nativeapp.market.MarketingOverview
import com.forkmesh.nativeapp.market.MarketingText
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
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
import kotlinx.coroutines.withContext
import java.time.LocalDate
import java.time.ZoneOffset

class MarketingFormScreen(private val model: MarketingModel, draft: MarketingDraft) : Screen() {
    override val title: String get() = draft.title

    private var draft by mutableStateOf(draft)
    private var problem by mutableStateOf("")
    private var confirmDelete by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val confirm = when (draft.kind) {
            MarketingKind.X_POST -> "Post"
            MarketingKind.X_CONNECTION -> "Connect"
            MarketingKind.METRIC -> "Log"
            else -> "Save"
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(draft.title, navigator) {
                if (runner.busy > 0) FmCircularProgressIndicator()
                FmTextButton(onClick = {
                    problem = ""
                    runner.run({ save(draft) }) { error -> if (error == null) navigator.pop() else problem = error }
                }, enabled = runner.busy == 0) { Text(confirm) }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                footer(draft.kind)?.let { Text(it, style = Fm.text.bodySmall, color = Fm.colors.textSecondary) }
                draft.fields.forEach { field ->
                    MarketingFieldEditor(field, draft[field.key], model.overview) { draft = draft.with(field.key, it) }
                }
                if (draft.kind == MarketingKind.X_POST) Text("${draft["text"].length} characters · posts as @${model.overview?.x?.username.orEmpty()}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                if (problem.isNotEmpty()) Text(problem, color = Fm.colors.danger, style = Fm.text.bodySmall)
                MarketingRunnerStatus(runner, model)
                if (draft.kind.deletable && !draft.isNew) {
                    FmToneButton("Delete", onPressed = { confirmDelete = true }, icon = "delete", tone = FmTone.Danger)
                }
            }
        }
        if (confirmDelete) {
            FmConfirmDialog(
                title = "Delete this ${draft.kind.title.lowercase()}?",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { confirmDelete = false },
                onConfirm = {
                    confirmDelete = false
                    runner.run({ delete(draft.kind, draft.id) }) { ok -> if (ok) navigator.pop() }
                },
            )
        }
    }

    private fun footer(kind: MarketingKind): String? = when (kind) {
        MarketingKind.AD_RESULTS -> "Copy these from the platform's ads manager. Saving stamps when they were read."
        MarketingKind.METRIC -> "Every figure is saved as a new reading; leave the ones you did not check as they are."
        MarketingKind.FUNNEL_WEEK -> "One row per week; logging the same week again updates it. Dates are YYYY-MM-DD."
        MarketingKind.CHANNEL -> "Access tokens are write-only: the server keeps them sealed and never sends them back."
        MarketingKind.X_CONNECTION -> "Create the keys in the X developer portal with read and write access. They stay sealed on the server."
        MarketingKind.REDDIT_THREAD -> "Marking a thread replied needs the rules check, the disclosure and your reply link."
        else -> null
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun MarketingFieldEditor(field: MarketingField, value: String, overview: MarketingOverview?, onChange: (String) -> Unit) {
    val hint = field.hint.ifEmpty { null }
    when (val kind = field.kind) {
        MarketingFieldKind.Text, MarketingFieldKind.Url, MarketingFieldKind.ListText -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: field.label, keyboardOptions = KeyboardOptions(keyboardType = if (kind == MarketingFieldKind.Url) KeyboardType.Uri else KeyboardType.Text))
        }
        MarketingFieldKind.Money, MarketingFieldKind.Decimal -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: "0.00", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
        }
        MarketingFieldKind.Count -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: "0", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
        }
        MarketingFieldKind.Secret -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: field.label, visualTransformation = PasswordVisualTransformation(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password))
        }
        MarketingFieldKind.Multiline, MarketingFieldKind.Lines, MarketingFieldKind.Tasks -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: field.label, minLines = 3, maxLines = 12)
        }
        MarketingFieldKind.Date -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: "YYYY-MM-DD", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
        }
        MarketingFieldKind.DateTime -> FmField(field.label) {
            FmTextField(value, onChange, hint = hint ?: "YYYY-MM-DD HH:MM")
        }
        MarketingFieldKind.Toggle -> Row(verticalAlignment = Alignment.CenterVertically) {
            Text(field.label, Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            FmSwitch(value == "true", { onChange(if (it) "true" else "") })
        }
        is MarketingFieldKind.Choice -> MarketingPicker(field.label, value, kind.options, { if (it.isEmpty()) "None" else MarketingText.title(it) }, onChange)
        is MarketingFieldKind.Channel -> MarketingPicker(field.label, value, kind.options, { if (it.isEmpty()) "Find from the link" else overview?.channelName(it) ?: it }, onChange)
        is MarketingFieldKind.Channels -> FmField(field.label) {
            val selected = MarketingForms.splitList(value)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                kind.options.forEach { option ->
                    CrmChip(overview?.channelName(option) ?: option, Fm.colors.accent, filled = option in selected, onTap = {
                        onChange((if (option in selected) selected - option else selected + option).joinToString(", "))
                    })
                }
            }
        }
    }
}

@Composable
private fun MarketingPicker(label: String, value: String, options: List<String>, title: (String) -> String, onChange: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmListRow(label, subtitle = title(value), onTap = { open = true }, minHeight = 52.dp)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            options.forEach { option -> FmDropdownMenuItem(title(option), onClick = { open = false; onChange(option) }) }
        }
    }
}

class MarketingCampaignScreen(private val model: MarketingModel, private val campaignId: String) : Screen() {
    override val title: String get() = "Campaign"

    private var newTask by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val context = LocalContext.current
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val overview = model.overview
        val campaign = overview?.campaign(campaignId)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(campaign?.title ?: "Campaign", navigator) {
                if (overview != null && campaign != null) FmTextButton(onClick = {
                    navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.CAMPAIGN, overview, campaign.json, campaign.id, model.zone)))
                }) { Text("Edit") }
            }
            if (overview == null || campaign == null) {
                Text("This campaign was removed.", Modifier.padding(16.dp), color = Fm.colors.textSecondary)
                return@Column
            }
            Column(Modifier.verticalScroll(rememberScrollState())) {
                FmKeyValueRow("Status", MarketingText.title(campaign.status), Modifier.padding(horizontal = 16.dp))
                MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
                MarketingNote(campaign.facts(overview).joinToString(" · "))
                campaign.dates.forEach { FmKeyValueRow(it.label, shortDate(it.ms), Modifier.padding(horizontal = 16.dp)) }
                if (campaign.json.str("audience").isNotEmpty()) FmKeyValueRow("Audience", campaign.json.str("audience"), Modifier.padding(horizontal = 16.dp))
                if (campaign.json.str("message").isNotEmpty()) Text(campaign.json.str("message"), Modifier.padding(16.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                val uncovered = campaign.json.obj("stats").strings("uncoveredChannels")
                if (uncovered.isNotEmpty()) Text("No content yet on " + uncovered.joinToString(", ") { overview.channelName(it) }, Modifier.padding(horizontal = 16.dp), style = Fm.text.labelSmall, color = MarketingTint.orange)
                if (campaign.link.isNotEmpty()) {
                    MarketingHeader("Tagged link")
                    FmListRow(campaign.link, trailing = { FmIcon("content_copy") }, onTap = { crmCopy(context, campaign.link); runner.status.show("Link copied.") }, showChevron = false)
                    FmToneButton("Share", onPressed = { crmShare(context, campaign.link, campaign.title) }, Modifier.padding(horizontal = 16.dp), icon = "share")
                }
                MarketingHeader("Tasks", campaign.tasks.size)
                campaign.tasks.forEachIndexed { index, task ->
                    FmListRow(
                        task.title, subtitle = task.owner.ifEmpty { null },
                        leading = { FmIcon(if (task.done) "check_circle" else "radio_button_unchecked", tint = if (task.done) MarketingTint.green else Fm.colors.textTertiary) },
                        onTap = { runner.run({ toggleTask(campaign, index, !task.done) }) }, showChevron = false,
                    )
                }
                Row(Modifier.padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(newTask, { newTask = it }, Modifier.weight(1f), hint = "Add a task")
                    FmIconButtonAdd { val title = newTask; newTask = ""; runner.run({ addTask(campaign, title) }) }
                }
                MarketingHeader("Content")
                overview.content.filter { it.campaign.isNotEmpty() && (it.campaign.equals(campaign.title, true) || it.campaign.equals(campaign.json.str("slug"), true)) }.forEach { item ->
                    FmListRow(item.title, subtitle = MarketingText.title(item.status), onTap = {
                        navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.CONTENT, overview, item.json, item.id, model.zone)))
                    })
                }
                FmToneButton("Plan content for this campaign", onPressed = {
                    navigator.push(MarketingFormScreen(model, MarketingForms.contentDraft(overview, campaign.title, campaign.json.strings("channels").firstOrNull().orEmpty(), zone = model.zone)))
                }, Modifier.padding(16.dp), icon = "add")
                if (overview.assistantAvailable) FmListRow("Ask the assistant about this campaign", leading = { FmIcon("auto_awesome") }, onTap = { navigator.push(MarketingAssistantScreen(model, campaign.id)) })
                if (campaign.json.str("brief").isNotEmpty()) {
                    MarketingHeader("Brief")
                    Text(campaign.json.str("brief"), Modifier.padding(horizontal = 16.dp, vertical = 8.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                }
                Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    MarketingKind.CAMPAIGN.statuses.filter { it != campaign.status }.take(4).forEach { status ->
                        FmToneButton(MarketingText.title(status), onPressed = { runner.run({ setStatus(MarketingKind.CAMPAIGN, campaign.id, status) }) })
                    }
                }
            }
        }
    }
}

@Composable
private fun FmIconButtonAdd(onPressed: () -> Unit) {
    com.forkmesh.nativeapp.ui.kit.FmIconButton("add", "Add task", onPressed = onPressed, color = Fm.colors.accent)
}

class MarketingChannelScreen(private val model: MarketingModel, private val channel: String) : Screen() {
    override val title: String get() = "Channel"

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val context = LocalContext.current
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val overview = model.overview ?: return
        val stats = overview.stats(channel)
        val setting = overview.setting(channel)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(overview.channelName(channel), navigator)
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (stats != null) {
                    MarketingChannelRow(overview, stats, model.isWorking("refresh:$channel")) {}
                    stats.checks.forEach { check ->
                        FmListRow(check.label, leading = { FmIcon(if (check.ok) "check_circle" else "radio_button_unchecked", tint = if (check.ok) MarketingTint.green else MarketingTint.orange) }, showChevron = false)
                    }
                }
                if (channel == MarketingAppStore.CHANNEL) MarketingAppStoreSection(model, runner)
                MarketingHeader("Update")
                if (channel in overview.refreshable) FmListRow("Refresh now", leading = { FmIcon("refresh") }, onTap = { runner.run({ refreshChannel(channel) }) })
                MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
                FmListRow("Log numbers by hand", leading = { FmIcon("edit_note") }, onTap = { navigator.push(MarketingFormScreen(model, MarketingForms.metricDraft(overview, channel, model.zone))) })
                if (stats?.canCapture == true) FmListRow("Update with the browser extension", leading = { FmIcon("extension") }, onTap = { crmOpen(context, stats.captureUrl(model.organization)) })
                if (setting?.profileUrl?.startsWith("https://") == true) FmListRow("Open the channel page", leading = { FmIcon("open_in_new") }, onTap = { crmOpen(context, setting.profileUrl) })
                if (overview.canManageSettings) FmListRow("Channel settings", leading = { FmIcon("settings") }, onTap = { navigator.push(MarketingFormScreen(model, MarketingForms.channelDraft(overview, channel, model.zone))) })
                setting?.lastError?.takeIf { it.isNotEmpty() }?.let { MarketingNote("Last refresh failed: " + PackagesException.sentence(it)) }
                overview.playbook(channel)?.let { (goal, kpis, tips) ->
                    MarketingHeader("Playbook")
                    MarketingNote(goal)
                    if (kpis.isNotEmpty()) FmKeyValueRow("Watch", kpis.joinToString(", "), Modifier.padding(horizontal = 16.dp))
                    tips.forEach { tip -> FmListRow(tip, leading = { FmIcon("lightbulb") }, showChevron = false) }
                }
                val content = overview.content.filter { it.channel == channel }
                if (content.isNotEmpty()) {
                    MarketingHeader("Content", content.size)
                    content.take(20).forEach { item -> FmListRow(item.title, subtitle = MarketingText.title(item.status), showChevron = false) }
                }
                MarketingHeader("Posted links")
                overview.posts(channel).forEach { post ->
                    MarketingRecordRow(post, overview, onTap = { navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.POST, overview, post.json, post.id, model.zone))) })
                }
                FmToneButton("Track a posted link", onPressed = {
                    navigator.push(MarketingFormScreen(model, MarketingDraft.of(MarketingKind.POST, overview, mapOf("channel" to channel), zone = model.zone)))
                }, Modifier.padding(16.dp), icon = "add")
            }
        }
    }
}

class MarketingAssistantScreen(private val model: MarketingModel, campaignId: String) : Screen() {
    override val title: String get() = "Marketing assistant"

    private var campaign by mutableStateOf(campaignId)
    private var question by mutableStateOf("")
    private var campaignMenu by mutableStateOf(false)

    override fun onBack(): Boolean {
        model.clearAdvice()
        return false
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val overview = model.overview
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Marketing assistant", navigator)
            Column(Modifier.verticalScroll(rememberScrollState()).padding(vertical = 8.dp)) {
                Box {
                    FmListRow(overview?.campaign(campaign)?.title ?: "All marketing", subtitle = "About", onTap = { campaignMenu = true })
                    FmDropdownMenu(expanded = campaignMenu, onDismissRequest = { campaignMenu = false }) {
                        FmDropdownMenuItem("All marketing", onClick = { campaignMenu = false; campaign = "" })
                        overview?.campaigns?.forEach { item -> FmDropdownMenuItem(item.title, onClick = { campaignMenu = false; campaign = item.id }) }
                    }
                }
                FmTextField(question, { question = it }, Modifier.padding(horizontal = 16.dp), hint = "Ask a question (optional)", maxLines = 4)
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmToneButton("Suggest next steps", onPressed = { runner.run({ ask(campaign, question) }) }, icon = "auto_awesome", tone = FmTone.Accent, filled = true)
                    if (model.isWorking("assistant")) FmCircularProgressIndicator()
                }
                MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
                MarketingNote("The assistant reads this organization's campaigns, channels and keywords and suggests up to eight steps.")
                val advice = model.advice
                if (advice != null) {
                    MarketingHeader("Summary")
                    Text(advice.summary, Modifier.padding(horizontal = 16.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    MarketingHeader("Suggestions", advice.suggestions.size)
                    advice.suggestions.forEach { idea ->
                        FmListRow(
                            idea.title,
                            subtitleContent = {
                                Column {
                                    if (idea.detail.isNotEmpty()) Text(idea.detail, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                                    if (idea.copy.isNotEmpty()) Text(idea.copy, style = Fm.text.labelSmall, color = Fm.colors.textPrimary)
                                    Text(
                                        listOf(MarketingText.title(idea.kind), if (idea.channel.isEmpty()) "" else overview?.channelName(idea.channel).orEmpty(), if (idea.inDays > 0) "in ${idea.inDays} days" else "today")
                                            .filter { it.isNotEmpty() }.joinToString(" · "),
                                        style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                                    )
                                }
                            },
                            trailing = if (idea.actionTitle.isEmpty()) null else ({
                                FmToneButton(idea.actionTitle, onPressed = { runner.run({ apply(idea) }) { draft -> if (draft != null) navigator.push(MarketingFormScreen(model, draft)) } })
                            }),
                            showChevron = false,
                        )
                    }
                }
            }
        }
    }
}

class MarketingReportScreen(private val model: MarketingModel) : Screen() {
    override val title: String get() = "Weekly report"

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val context = LocalContext.current
        LaunchedEffect(Unit) { runner.run({ loadReport() }) }
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val report = model.report
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Weekly report", navigator) {
                if (report != null) FmTextButton(onClick = { crmShare(context, report.markdown, "Marketing report") }) { Text("Share") }
            }
            MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
            if (report == null) {
                Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
            } else {
                Text(report.markdown, Modifier.verticalScroll(rememberScrollState()).padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
            }
        }
    }
}

class MarketingLinkScreen(private val model: MarketingModel) : Screen() {
    override val title: String get() = "Tagged link"

    private var draft by mutableStateOf(model.newDraft(MarketingKind.LINK))
    private var link by mutableStateOf<MarketingLink?>(null)
    private var problem by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val context = LocalContext.current
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Tagged link", navigator)
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                val current = draft ?: return@Column
                current.fields.forEach { field -> MarketingFieldEditor(field, current[field.key], model.overview) { draft = current.with(field.key, it) } }
                FmToneButton("Build link", onPressed = {
                    runner.run({ buildLink(current) }) { (built, error) -> link = built; problem = error.orEmpty() }
                }, icon = "link", tone = FmTone.Accent, filled = true)
                Text("Tags follow the organization's allowlist so visits are counted under the right campaign.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                if (problem.isNotEmpty()) Text(problem, color = Fm.colors.danger, style = Fm.text.bodySmall)
                link?.let { built ->
                    FmListRow(built.url, trailing = { FmIcon("content_copy") }, onTap = { crmCopy(context, built.url); problem = ""; runner.status.show("Link copied.") }, showChevron = false)
                    built.checks.forEach { check -> Text(check, style = Fm.text.labelSmall, color = MarketingTint.orange) }
                }
                MarketingRunnerStatus(runner, model)
            }
        }
    }
}

class MarketingKeywordImportScreen(private val model: MarketingModel) : Screen() {
    override val title: String get() = "Import keywords"

    private var text by mutableStateOf("")
    private var source by mutableStateOf("Search Console")
    private var day by mutableStateOf(LocalDate.now().toString())
    private var limit by mutableIntStateOf(100)
    private var problem by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val rows = remember(text) { MarketingKeywordImport.rows(text) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Import keywords", navigator) {
                FmTextButton(onClick = {
                    val observed = runCatching { LocalDate.parse(day.trim()).atStartOfDay().toInstant(ZoneOffset.UTC).toEpochMilli() + 12 * 3_600_000L }.getOrNull()
                    if (observed == null) {
                        problem = "Write the reading date as YYYY-MM-DD."
                    } else {
                        runner.run({ importKeywords(text, source, limit, observed) }) { error -> if (error == null) navigator.pop() else problem = error }
                    }
                }, enabled = rows.isNotEmpty() && runner.busy == 0) { Text("Import") }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                FmTextField(text, { text = it }, hint = "Top queries,Clicks,Impressions,CTR,Position", minLines = 6, maxLines = 16)
                Text(
                    if (rows.isEmpty()) "Paste Search Console's Queries export, Bing's keyword report, or one keyword per line."
                    else MarketingText.plural(rows.size, "keyword", "keywords") + " found" + if (rows.size > limit) "; the top $limit by clicks will be imported." else ".",
                    style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                )
                FmField("Reading date") { FmTextField(day, { day = it }, hint = "YYYY-MM-DD") }
                FmField("Source") { FmTextField(source, { source = it }) }
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(25, 100, 250, 500).forEach { option -> CrmChip("Top $option", Fm.colors.accent, filled = limit == option, onTap = { limit = option }) }
                }
                if (problem.isNotEmpty()) Text(problem, color = Fm.colors.danger, style = Fm.text.bodySmall)
            }
        }
    }
}

class MarketingAddChannelsScreen(private val model: MarketingModel) : Screen() {
    override val title: String get() = "Add channels"

    private var selected by mutableStateOf(emptySet<String>())
    private var query by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        val channels = model.overview?.addableChannels.orEmpty().filter { com.forkmesh.nativeapp.market.PackagesText.matches(query, it.name, it.category) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Add channels", navigator) {
                FmTextButton(onClick = { runner.run({ addChannels(selected.sorted()) }) { navigator.pop() } }, enabled = selected.isNotEmpty() && runner.busy == 0) { Text("Add ${selected.size}") }
            }
            FmTextField(query, { query = it }, Modifier.padding(16.dp), hint = "Search 100 channels")
            Column(Modifier.verticalScroll(rememberScrollState())) {
                channels.groupBy { it.category.ifEmpty { "Other" } }.toSortedMap().forEach { (category, items) ->
                    MarketingHeader(category, items.size)
                    items.forEach { channel ->
                        FmListRow(
                            channel.name,
                            trailing = { FmSwitch(channel.id in selected, { on -> selected = if (on) selected + channel.id else selected - channel.id }) },
                            onTap = { selected = if (channel.id in selected) selected - channel.id else selected + channel.id },
                            showChevron = false,
                        )
                    }
                }
            }
        }
    }
}

class MarketingSettingsScreen(private val model: MarketingModel) : Screen() {
    override val title: String get() = "Marketing settings"

    @Composable
    override fun Content(navigator: Navigator) {
        val runner = rememberMarketingRunner(model)
        @Suppress("UNUSED_EXPRESSION") MarketingRevision.value
        val overview = model.overview ?: return
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Marketing settings", navigator)
            Column(Modifier.verticalScroll(rememberScrollState())) {
                MarketingHeader("X")
                if (overview.x.connected) {
                    FmKeyValueRow("Connected", "@" + overview.x.username, Modifier.padding(horizontal = 16.dp))
                    FmListRow("Post to X", leading = { FmIcon("send") }, onTap = { navigator.push(MarketingFormScreen(model, MarketingForms.xPostDraft(overview))) })
                    FmListRow("Disconnect X", leading = { FmIcon("link_off") }, titleColor = Fm.colors.danger, onTap = { runner.run({ disconnectX() }) })
                    MarketingRunnerStatus(runner, model, Modifier.padding(horizontal = 16.dp))
                } else {
                    model.newDraft(MarketingKind.X_CONNECTION)?.let { draft -> FmListRow("Connect an X account", leading = { FmIcon("add_link") }, onTap = { navigator.push(MarketingFormScreen(model, draft)) }) }
                }
                MarketingNote("A connected account lets Marketing post X content straight from the plan.")
                MarketingHeader("Measurement")
                listOf(MarketingKind.ATTRIBUTION, MarketingKind.TARGETS, MarketingKind.GOALS, MarketingKind.REDDIT_WATCH).forEach { kind ->
                    model.newDraft(kind)?.let { draft -> FmListRow(kind.title, onTap = { navigator.push(MarketingFormScreen(model, draft)) }) }
                }
                MarketingHeader("Channels", overview.channelSettings.size)
                FmListRow("Add channels", leading = { FmIcon("add") }, onTap = { navigator.push(MarketingAddChannelsScreen(model)) })
                overview.channelSettings.forEach { setting ->
                    FmListRow(
                        setting.displayName.ifEmpty { overview.channelName(setting.id) },
                        subtitle = if (!setting.enabled) "Off" else if (setting.profileUrl.isEmpty()) "No link" else setting.profileUrl,
                        onTap = { navigator.push(MarketingFormScreen(model, MarketingForms.channelDraft(overview, setting.id, model.zone))) },
                    )
                }
            }
        }
    }
}
