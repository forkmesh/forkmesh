package com.forkmesh.nativeapp.feature.market

import android.content.ClipData
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.Survey
import com.forkmesh.nativeapp.market.SurveyAction
import com.forkmesh.nativeapp.market.SurveyDates
import com.forkmesh.nativeapp.market.SurveyDraft
import com.forkmesh.nativeapp.market.SurveyErrors
import com.forkmesh.nativeapp.market.SurveyExport
import com.forkmesh.nativeapp.market.SurveyFilter
import com.forkmesh.nativeapp.market.SurveyFormat
import com.forkmesh.nativeapp.market.SurveyLimits
import com.forkmesh.nativeapp.market.SurveyQuestion
import com.forkmesh.nativeapp.market.SurveyQuestionDraft
import com.forkmesh.nativeapp.market.SurveyQuestionType
import com.forkmesh.nativeapp.market.SurveyResponse
import com.forkmesh.nativeapp.market.SurveyResult
import com.forkmesh.nativeapp.market.SurveySummary
import com.forkmesh.nativeapp.market.SurveysModel
import com.forkmesh.nativeapp.market.deleteSurvey
import com.forkmesh.nativeapp.market.deleteSurveyResponse
import com.forkmesh.nativeapp.market.exportSurvey
import com.forkmesh.nativeapp.market.saveSurvey
import com.forkmesh.nativeapp.market.survey
import com.forkmesh.nativeapp.market.surveyAction
import com.forkmesh.nativeapp.market.surveyResponses
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object SurveyRevision {
    var value by mutableIntStateOf(0)

    fun bump() {
        value++
    }
}

@Composable
private fun surveyTint(state: String): Color = when (state) {
    "open" -> Fm.colors.success
    "scheduled" -> Fm.colors.warning
    "closed" -> Fm.colors.textSecondary
    else -> Fm.colors.textTertiary
}

@Composable
private fun SurveyBadge(survey: Survey) {
    FmStatusBadge(survey.statusLabel, surveyTint(survey.state))
}

@Composable
private fun SurveyNoticeLine(text: String) {
    if (text.isNotEmpty()) Text(text, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), color = Fm.colors.textSecondary, style = Fm.text.bodySmall)
}

private fun <T> CoroutineScope.surveyWork(
    setBusy: (Boolean) -> Unit,
    onError: (String) -> Unit,
    work: () -> T,
    done: (T) -> Unit,
) {
    setBusy(true)
    launch {
        val result = withContext(Dispatchers.IO) { runCatching(work) }
        setBusy(false)
        result.onSuccess(done).onFailure { onError(SurveyErrors.describe(it)) }
    }
}

private fun shareSurveyCsv(context: Context, export: SurveyExport): Boolean = runCatching {
    val folder = File(context.cacheDir, "surveys-share").apply { mkdirs() }
    val file = File(folder, export.safeFilename)
    file.writeText(export.csv)
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.composer.files", file)
    val send = Intent(Intent.ACTION_SEND).setType("text/csv").putExtra(Intent.EXTRA_STREAM, uri).putExtra(Intent.EXTRA_SUBJECT, file.name)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    send.clipData = ClipData.newRawUri(file.name, uri)
    context.startActivity(Intent.createChooser(send, file.name).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION))
    true
}.getOrDefault(false)

@Composable
private fun SurveyRow(survey: Survey, onTap: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().combinedClickableCompat(onTap).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    survey.displayTitle, Modifier.weight(1f, fill = false), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold),
                    color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
                SurveyBadge(survey)
            }
            Text(survey.countsLine, style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 1)
            val activity = if (survey.lastResponseAt > 0) "Last response ${relativeTime(survey.lastResponseAt)}" else if (survey.isDraft) "Not published" else "No responses yet"
            Text(activity, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
        }
        Spacer(Modifier.width(10.dp))
        Column(horizontalAlignment = Alignment.End) {
            Text(survey.responseCount.toString(), style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
            Text(if (survey.responseCount == 1) "response" else "responses", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
    }
}

@Composable
private fun SurveyTile(label: String, value: String, detail: String, modifier: Modifier) {
    FmCard(modifier, padding = PaddingValues(12.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
            Text(value, style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary, maxLines = 1)
            Text(detail, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun SurveyTotals(summary: SurveySummary) {
    Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        SurveyTile("Surveys", summary.surveys.toString(), summary.surveysDetail, Modifier.weight(1f))
        SurveyTile("Responses", summary.responses.toString(), summary.weekDetail, Modifier.weight(1f))
    }
}

class SurveysScreen(private val requestedOrganization: String? = null, private val requestedSurvey: String? = null) : Screen() {
    override val title: String get() = "Surveys"

    private val model = MarketFeature.client()?.let { SurveysModel(it) }
    private var loading by mutableStateOf(true)
    private var reload by mutableIntStateOf(1)
    private var firstLoad = true
    private var query by mutableStateOf("")
    private var filter by mutableStateOf(SurveyFilter.ALL)
    private var notice by mutableStateOf("")
    private var installing by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(reload) {
            loading = true
            val preferred = if (firstLoad) requestedOrganization else null
            withContext(Dispatchers.IO) { model.refresh(preferred) }
            loading = false
            SurveyRevision.bump()
            val wanted = requestedSurvey
            if (firstLoad && wanted != null) {
                val found = withContext(Dispatchers.IO) { model.locate(wanted) }
                SurveyRevision.bump()
                if (found != null) navigator.push(SurveyDetailScreen(model, found.id)) else model.errorText = "That survey was not found in your organizations."
            }
            firstLoad = false
        }
        val top = navigator.top
        LaunchedEffect(top) {
            if (top === this@SurveysScreen && !firstLoad && model.overview != null) {
                withContext(Dispatchers.IO) { model.load() }
                SurveyRevision.bump()
            }
        }
        @Suppress("UNUSED_EXPRESSION") SurveyRevision.value
        val overview = model.overview
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Surveys", navigator) {
                    FmIconButton("refresh", "Refresh", onPressed = { notice = ""; reload++ }, color = Fm.colors.textPrimary)
                }
                if (model.organizations.isNotEmpty()) {
                    OrganizationPicker(model.organizations, model.organization, "Surveys") { name ->
                        notice = ""
                        scope.launch {
                            loading = true
                            withContext(Dispatchers.IO) { model.choose(name) }
                            loading = false
                            SurveyRevision.bump()
                        }
                    }
                }
                ErrorLine(model.errorText)
                SurveyNoticeLine(notice)
                when {
                    loading && overview == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    model.organizations.isEmpty() -> FmEmptyState("corporate_fare", "No organizations", message = "Join or create an organization before using Surveys.")
                    model.needsInstall -> SurveyInstallPrompt(model, installing) {
                        installing = true
                        scope.launch {
                            val ok = withContext(Dispatchers.IO) { model.install() }
                            installing = false
                            if (ok) notice = "Surveys installed."
                            SurveyRevision.bump()
                        }
                    }
                    overview == null -> FmEmptyState("assignment", "Surveys is unavailable", message = "Tap refresh to try again.")
                    else -> {
                        val shown = overview.filtered(query, filter)
                        LazyColumn(Modifier.fillMaxSize()) {
                            item(key = "totals") { SurveyTotals(overview.summary) }
                            item(key = "search") { FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp, vertical = 6.dp), hint = "Search surveys") }
                            item(key = "filters") {
                                Row(
                                    Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp),
                                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                                ) {
                                    SurveyFilter.entries.forEach { option ->
                                        FmToneButton(
                                            "${option.title} ${overview.count(option)}",
                                            onPressed = { filter = option },
                                            tone = if (filter == option) FmTone.Accent else FmTone.Neutral,
                                            filled = filter == option,
                                        )
                                    }
                                }
                            }
                            if (shown.isEmpty()) {
                                item(key = "empty") {
                                    Text(
                                        if (overview.surveys.isEmpty()) "No surveys yet. Create one with the Survey button." else "No surveys match.",
                                        Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary,
                                    )
                                }
                            }
                            items(shown, key = { it.id }) { survey -> SurveyRow(survey) { navigator.push(SurveyDetailScreen(model, survey.id)) } }
                            item(key = "end") { Spacer(Modifier.height(88.dp)) }
                        }
                    }
                }
            }
            if (overview != null && !model.needsInstall) {
                FmExtendedFloatingActionButton(
                    "Survey",
                    onClick = { navigator.push(SurveyEditorScreen(model, null)) },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                    icon = "add",
                )
            }
        }
    }
}

@Composable
private fun SurveyInstallPrompt(model: SurveysModel, installing: Boolean, onInstall: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        FmIcon("assignment", size = 40.dp, tint = Fm.colors.textTertiary)
        Text("Surveys is not installed for ${model.organization}.", style = Fm.text.titleMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
        Text(
            "Build surveys with choice, text and rating questions, share a private link anyone can answer, and read the results here.",
            style = Fm.text.bodySmall, color = Fm.colors.textSecondary, textAlign = TextAlign.Center,
        )
        if (model.canInstall) {
            FmToneButton(if (installing) "Installing…" else "Install Surveys", onPressed = if (installing) null else onInstall, tone = FmTone.Accent, filled = true, icon = "download")
        } else {
            Text("Ask an owner or admin of ${model.organization} to install it.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary, textAlign = TextAlign.Center)
        }
    }
}

class SurveyDetailScreen(private val model: SurveysModel, val surveyId: String) : Screen() {
    override val title: String get() = "Survey"

    private var survey by mutableStateOf(model.overview?.survey(surveyId))
    private var responses by mutableStateOf<List<SurveyResponse>?>(null)
    private var loading by mutableStateOf(false)
    private var loadingResponses by mutableStateOf(false)
    private var busy by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")
    private var menu by mutableStateOf(false)
    private var tab by mutableIntStateOf(0)
    private var confirmDelete by mutableStateOf(false)
    private var confirmClose by mutableStateOf(false)
    private var deletingResponse by mutableStateOf<SurveyResponse?>(null)
    private var removed by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val organization = model.organization
        fun fetch() {
            scope.launch {
                loading = true
                val result = withContext(Dispatchers.IO) { runCatching { model.client.survey(organization, surveyId) } }
                loading = false
                result.onSuccess {
                    survey = it
                    error = ""
                    model.adopt(it)
                }.onFailure {
                    if (SurveyErrors.code(it) == "survey_not_found" || SurveyErrors.status(it) == 404) removed = true
                    error = SurveyErrors.describe(it)
                }
            }
        }
        fun fetchResponses() {
            scope.launch {
                loadingResponses = true
                val result = withContext(Dispatchers.IO) { runCatching { model.client.surveyResponses(organization, surveyId) } }
                loadingResponses = false
                result.onSuccess { responses = it }.onFailure { error = SurveyErrors.describe(it) }
            }
        }
        fun act(action: SurveyAction) {
            notice = ""
            scope.surveyWork({ busy = it }, { error = it }, { model.client.surveyAction(organization, surveyId, action) }) { next ->
                error = ""
                model.adopt(next)
                SurveyRevision.bump()
                if (action == SurveyAction.DUPLICATE) {
                    notice = "Draft copy created."
                    navigator.push(SurveyDetailScreen(model, next.id))
                } else {
                    notice = when (action) {
                        SurveyAction.CLOSE -> "Survey closed. The link no longer takes answers."
                        SurveyAction.REOPEN -> "Survey reopened."
                        else -> "Survey open. Share the link to collect answers."
                    }
                    survey = next.copy(questions = next.questions ?: survey?.questions, results = next.results ?: survey?.results, activity = survey?.activity ?: next.activity)
                    fetch()
                }
            }
        }
        fun export() {
            notice = ""
            scope.surveyWork({ busy = it }, { error = it }, { model.client.exportSurvey(organization, surveyId) }) { file ->
                if (!shareSurveyCsv(context, file)) error = "Could not share the CSV file."
            }
        }
        val top = navigator.top
        LaunchedEffect(top) {
            if (top === this@SurveyDetailScreen) {
                fetch()
                if (responses != null || tab == 1) fetchResponses()
            }
        }
        val current = survey
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(current?.displayTitle ?: "Survey", navigator) {
                if (current != null) {
                    FmTextButton(onClick = { navigator.push(SurveyEditorScreen(model, current)) }, enabled = !busy) { Text("Edit") }
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Duplicate", onClick = { menu = false; act(SurveyAction.DUPLICATE) }, icon = "content_copy")
                            if (current.canShare) {
                                FmDropdownMenuItem("Copy link", onClick = { menu = false; crmCopy(context, current.publicUrl); notice = "Survey link copied." }, icon = "link")
                                FmDropdownMenuItem("Share link", onClick = { menu = false; crmShare(context, current.publicUrl, current.displayTitle) }, icon = "share")
                            }
                            if (current.canExport) FmDropdownMenuItem("Export CSV", onClick = { menu = false; export() }, icon = "table_view")
                            if (current.canDelete) FmDropdownMenuItem("Delete", onClick = { menu = false; confirmDelete = true }, icon = "delete", textColor = Fm.colors.danger)
                        }
                    }
                }
            }
            ErrorLine(error)
            SurveyNoticeLine(notice)
            when {
                current == null && (loading || error.isEmpty()) -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                current == null || removed -> FmEmptyState("assignment", "Survey not found", message = error.ifEmpty { null })
                else -> Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    SurveyHeader(current)
                    SurveyStatusActions(current, busy, onAction = { action ->
                        when (action) {
                            SurveyAction.CLOSE -> confirmClose = true
                            SurveyAction.PUBLISH -> {
                                val problem = current.publishProblem
                                if (problem != null) error = problem else act(action)
                            }
                            else -> act(action)
                        }
                    }, onExport = { export() })
                    if (current.questions != null) PageActivityCard(current.activity)
                    if (current.canShare) {
                        Column {
                            CrmSection("Share link") {
                                Text(current.publicUrl, Modifier.padding(horizontal = 14.dp, vertical = 6.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                                Row(Modifier.padding(horizontal = 8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    FmTextButton(onClick = { crmCopy(context, current.publicUrl); notice = "Survey link copied." }) { Text("Copy link") }
                                    FmTextButton(onClick = { crmShare(context, current.publicUrl, current.displayTitle) }) { Text("Share") }
                                    FmTextButton(onClick = { crmOpen(context, current.publicUrl) }) { Text("Open") }
                                }
                                Text(
                                    if (current.acceptsAnswers) "Anyone with the link can answer, no account needed." else "The link shows that the survey is not taking answers.",
                                    Modifier.padding(horizontal = 14.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                                )
                            }
                        }
                    }
                    FmSegmentTabBar(
                        listOf(FmSegmentTab("Results"), FmSegmentTab("Responses", current.responseCount)),
                        tab,
                        onChanged = {
                            tab = it
                            if (it == 1 && responses == null && !loadingResponses) fetchResponses()
                        },
                    )
                    if (tab == 0) {
                        SurveyResults(current)
                    } else {
                        SurveyResponses(current, responses, loadingResponses, busy) { deletingResponse = it }
                    }
                    Text(
                        listOfNotNull(
                            current.createdBy.takeIf { it.isNotEmpty() }?.let { "Created by $it" },
                            current.publishedAt.takeIf { it > 0 }?.let { "published ${relativeTime(it)}" },
                            current.updatedAt.takeIf { it > 0 }?.let { "updated ${relativeTime(it)}" + if (current.updatedBy.isNotEmpty()) " by ${current.updatedBy}" else "" },
                        ).joinToString(" · "),
                        Modifier.padding(vertical = 8.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                    )
                    if (current.canDelete) {
                        FmTextButton(onClick = { confirmDelete = true }, enabled = !busy, contentColor = Fm.colors.danger) { Text("Delete survey") }
                    }
                    Spacer(Modifier.height(40.dp))
                }
            }
        }
        if (confirmClose && current != null) {
            FmConfirmDialog(
                title = "Close ${current.displayTitle}?",
                body = "The link stops taking answers. You can reopen the survey later.",
                confirmLabel = "Close survey",
                onDismissRequest = { confirmClose = false },
                onConfirm = {
                    confirmClose = false
                    act(SurveyAction.CLOSE)
                },
            )
        }
        if (confirmDelete && current != null) {
            FmConfirmDialog(
                title = "Delete ${current.displayTitle}?",
                body = "The survey and its ${SurveyFormat.plural(current.responseCount, "response")} are deleted. This cannot be undone.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { confirmDelete = false },
                onConfirm = {
                    confirmDelete = false
                    scope.surveyWork({ busy = it }, { error = it }, { model.client.deleteSurvey(organization, surveyId); model.forget(surveyId); model.load() }) {
                        SurveyRevision.bump()
                        navigator.pop()
                    }
                },
            )
        }
        val doomed = deletingResponse
        if (doomed != null && current != null) {
            FmConfirmDialog(
                title = "Delete this response?",
                body = "Submitted ${SurveyDates.label(doomed.submittedAt)}. Its answers leave the results. This cannot be undone.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { deletingResponse = null },
                onConfirm = {
                    deletingResponse = null
                    scope.surveyWork({ busy = it }, { error = it }, { model.client.deleteSurveyResponse(organization, surveyId, doomed.id) }) { next ->
                        responses = responses?.filter { it.id != doomed.id }
                        if (next != null) {
                            model.adopt(next)
                            survey = survey?.copy(responseCount = next.responseCount, lastResponseAt = next.lastResponseAt, displayStatus = next.displayStatus)
                        }
                        notice = "Response deleted."
                        SurveyRevision.bump()
                        fetch()
                    }
                },
            )
        }
    }
}

@Composable
private fun SurveyHeader(survey: Survey) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(Fm.colors.bgRaised).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(survey.displayTitle, Modifier.weight(1f, fill = false), style = Fm.text.titleLarge.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary)
            SurveyBadge(survey)
        }
        if (survey.description.isNotEmpty()) Text(survey.description, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
        Text(
            survey.countsLine + if (survey.lastResponseAt > 0) " · last ${relativeTime(survey.lastResponseAt)}" else "",
            style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
        )
        if (survey.scheduleLine.isNotEmpty()) Text(survey.scheduleLine, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        Text(survey.settingsLine, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
    }
}

@Composable
private fun SurveyStatusActions(survey: Survey, busy: Boolean, onAction: (SurveyAction) -> Unit, onExport: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (survey.canPublish) {
            FmToneButton("Open survey", onPressed = if (busy) null else ({ onAction(SurveyAction.PUBLISH) }), Modifier.fillMaxWidth(), icon = "play_arrow", tone = FmTone.Accent, filled = true, height = 42.dp)
            survey.publishProblem?.let { Text(it, style = Fm.text.labelSmall, color = Fm.colors.textTertiary) }
        }
        if (survey.canReopen) {
            FmToneButton("Reopen survey", onPressed = if (busy) null else ({ onAction(SurveyAction.REOPEN) }), Modifier.fillMaxWidth(), icon = "replay", tone = FmTone.Accent, filled = true, height = 42.dp)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (survey.canClose) {
                FmToneButton("Close survey", onPressed = if (busy) null else ({ onAction(SurveyAction.CLOSE) }), Modifier.weight(1f), icon = "stop", height = 42.dp)
            }
            if (survey.canExport) {
                FmToneButton("Export CSV", onPressed = if (busy) null else onExport, Modifier.weight(1f), icon = "table_view", height = 42.dp)
            }
        }
    }
}

@Composable
private fun SurveyBar(fraction: Float) {
    FmLinearProgressIndicator(Modifier.height(8.dp).clip(RoundedCornerShape(4.dp)), progress = { fraction })
}

@Composable
private fun SurveyResults(survey: Survey) {
    val results = survey.results
    val questions = survey.questions.orEmpty()
    when {
        results == null -> Text("Loading results…", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        results.isEmpty() -> Text("Add questions to see results here.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        else -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            results.forEachIndexed { index, result -> SurveyResultCard(index + 1, result, questions.firstOrNull { it.id == result.id }) }
        }
    }
}

@Composable
private fun SurveyResultCard(number: Int, result: SurveyResult, question: SurveyQuestion?) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            FmIcon(result.type.icon, size = 16.dp, tint = Fm.colors.textTertiary)
            Text("$number. ${result.title.ifBlank { question?.title ?: "Untitled question" }}", Modifier.weight(1f), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
        }
        Text(result.answeredText, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        when {
            result.type.isChoice -> {
                if (result.options.isEmpty()) Text("No options.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                result.options.forEach { option ->
                    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                        Row {
                            Text(option.label, Modifier.weight(1f), style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                            Text(result.optionLine(option), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                        }
                        SurveyBar(result.share(option.count))
                    }
                }
            }
            result.type == SurveyQuestionType.RATING -> {
                Text("Average ${result.averageText}", style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                result.ratingRows.forEach { (rating, count) ->
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(rating.toString(), Modifier.width(22.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary, textAlign = TextAlign.End)
                        Box(Modifier.weight(1f)) { SurveyBar(result.distributionShare(count)) }
                        Text(count.toString(), Modifier.width(32.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    }
                }
            }
            else -> {
                if (result.answers.isEmpty()) Text("No written answers yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                result.answers.forEach { answer ->
                    Text(
                        answer, Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(Fm.colors.canvas).padding(horizontal = 10.dp, vertical = 8.dp),
                        style = Fm.text.bodySmall, color = Fm.colors.textPrimary, maxLines = 8, overflow = TextOverflow.Ellipsis,
                    )
                }
                if (result.answers.size >= SurveyLimits.RECENT_ANSWERS) {
                    Text("The ${SurveyLimits.RECENT_ANSWERS} newest answers. Responses and Export CSV have them all.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                }
            }
        }
    }
}

@Composable
private fun SurveyResponses(survey: Survey, responses: List<SurveyResponse>?, loading: Boolean, busy: Boolean, onDelete: (SurveyResponse) -> Unit) {
    val questions = survey.questions.orEmpty()
    when {
        responses == null -> Text(if (loading) "Loading responses…" else "Responses are not loaded.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        responses.isEmpty() -> Text(
            if (survey.isDraft) "Open the survey and share its link to collect responses." else "No responses yet.",
            Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary,
        )
        else -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            responses.forEach { response ->
                Column(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(start = 14.dp, end = 4.dp, top = 6.dp, bottom = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(SurveyDates.label(response.submittedAt), style = Fm.text.bodySmall.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                            Text(relativeTime(response.submittedAt), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                        }
                        FmIconButton("delete_outline", "Delete response", onPressed = if (busy) null else ({ onDelete(response) }), color = Fm.colors.textTertiary)
                    }
                    response.lines(questions).forEach { (title, answer) ->
                        Column(Modifier.padding(end = 10.dp)) {
                            Text(title, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                            Text(answer.ifEmpty { "No answer" }, style = Fm.text.bodySmall, color = if (answer.isEmpty()) Fm.colors.textTertiary else Fm.colors.textPrimary)
                        }
                    }
                }
            }
            if (responses.size >= 2000) Text("Showing the 2,000 newest responses. Export CSV has them all.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
    }
}

class SurveyEditorScreen(private val model: SurveysModel, private val original: Survey?) : Screen() {
    override val title: String get() = if (original == null) "New survey" else "Edit survey"

    private var draft by mutableStateOf<SurveyDraft?>(if (original == null) SurveyDraft.blank() else original.takeIf { it.questions != null }?.let { SurveyDraft.of(it) })
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            if (draft == null && original != null) {
                val result = withContext(Dispatchers.IO) { runCatching { model.client.survey(model.organization, original.id) } }
                result.onSuccess { draft = SurveyDraft.of(it) }.onFailure { error = SurveyErrors.describe(it) }
            }
        }
        val edit = draft
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = {
                    val current = draft ?: return@FmTextButton
                    val problem = current.problem()
                    if (problem != null) {
                        error = problem
                        return@FmTextButton
                    }
                    error = ""
                    scope.surveyWork({ saving = it }, { error = "Could not save the survey: $it" }, { model.client.saveSurvey(model.organization, current) }) { saved ->
                        model.adopt(saved)
                        SurveyRevision.bump()
                        if (current.isNew) navigator.replace(SurveyDetailScreen(model, saved.id)) else navigator.pop()
                    }
                }, enabled = !saving && edit != null, busy = saving) { Text(if (saving) "Saving…" else "Save") }
            }
            if (edit == null) {
                ErrorLine(error)
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                return@Column
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                if (original != null && original.responseCount > 0) {
                    Text(
                        "This survey has ${SurveyFormat.plural(original.responseCount, "response")}. Stored answers are kept; results only count answers that still fit each question.",
                        style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                    )
                }
                FmSectionHeader("Survey")
                FmTextField(edit.title, { draft = edit.copy(title = it) }, hint = "Title")
                FmTextField(edit.description, { draft = edit.copy(description = it) }, hint = "Description (shown above the questions)", minLines = 2, maxLines = 6)
                FmTextField(edit.thankYou, { draft = edit.copy(thankYou = it) }, hint = "Thank-you message (shown after answering)", minLines = 2, maxLines = 5)
                FmSectionHeader("Questions", Modifier.padding(top = 6.dp), count = edit.questions.size)
                edit.questions.forEachIndexed { index, question ->
                    SurveyQuestionEditor(
                        index, question, edit.questions.size,
                        onChange = { next -> draft = edit.question(index) { next } },
                        onMove = { delta -> draft = edit.moveQuestion(index, delta) },
                        onRemove = { draft = edit.removeQuestion(index) },
                    )
                }
                if (edit.questions.isEmpty()) Text("No questions yet. A survey opens once it has at least one.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                SurveyAddQuestion(edit.canAddQuestion) { type -> draft = edit.addQuestion(type) }
                FmSectionHeader("Settings", Modifier.padding(top = 6.dp))
                SurveySwitchRow("One response per person", "A second answer from the same connection is refused.", edit.oneResponsePerPerson) { draft = edit.copy(oneResponsePerPerson = it) }
                SurveySwitchRow("Ping me on new responses", "At most one Ping per survey an hour.", edit.notifyOnResponse) { draft = edit.copy(notifyOnResponse = it) }
                SurveySwitchRow("Ping me when someone opens or starts it", "At most 10 visit Pings per survey an hour.", edit.notifyOnVisit) { draft = edit.copy(notifyOnVisit = it) }
                SurveySwitchRow("Shuffle choice options", "Each respondent sees the options in a random order.", edit.shuffleOptions) { draft = edit.copy(shuffleOptions = it) }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(edit.opens, { draft = edit.copy(opens = it) }, Modifier.weight(1f), hint = "Opens (YYYY-MM-DD HH:MM)")
                    FmTextField(edit.closes, { draft = edit.copy(closes = it) }, Modifier.weight(1f), hint = "Closes (YYYY-MM-DD HH:MM)")
                }
                FmTextField(edit.maxResponses, { draft = edit.copy(maxResponses = it.filter(Char::isDigit).take(5)) }, hint = "Response limit (empty for no limit, up to 10,000)")
                Text("Leave the times empty to take answers as soon as the survey is open and until you close it.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                Spacer(Modifier.height(40.dp))
            }
        }
    }
}

@Composable
private fun SurveySwitchRow(title: String, detail: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Fm.colors.bgRaised).combinedClickableCompat({ onChange(!checked) }).padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            Text(detail, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
        Spacer(Modifier.width(10.dp))
        FmSwitch(checked, onCheckedChange = onChange)
    }
}

@Composable
private fun SurveyAddQuestion(enabled: Boolean, onAdd: (SurveyQuestionType) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmToneButton(
            if (enabled) "Add question" else "${SurveyLimits.QUESTIONS} questions is the most a survey holds",
            onPressed = if (enabled) ({ open = true }) else null,
            Modifier.fillMaxWidth(), icon = "add", tone = FmTone.Accent, height = 42.dp,
        )
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            SurveyQuestionType.entries.forEach { type ->
                FmDropdownMenuItem(type.title, onClick = { open = false; onAdd(type) }, icon = type.icon)
            }
        }
    }
}

@Composable
private fun SurveyQuestionEditor(
    index: Int,
    question: SurveyQuestionDraft,
    total: Int,
    onChange: (SurveyQuestionDraft) -> Unit,
    onMove: (Int) -> Unit,
    onRemove: () -> Unit,
) {
    var typeMenu by remember { mutableStateOf(false) }
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Question ${index + 1}", Modifier.weight(1f).padding(start = 4.dp), style = Fm.text.labelSmall.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textSecondary)
            FmIconButton("arrow_upward", "Move up", onPressed = if (index > 0) ({ onMove(-1) }) else null, color = Fm.colors.textTertiary, iconSize = 20.dp)
            FmIconButton("arrow_downward", "Move down", onPressed = if (index < total - 1) ({ onMove(1) }) else null, color = Fm.colors.textTertiary, iconSize = 20.dp)
            FmIconButton("delete_outline", "Remove question", onPressed = onRemove, color = Fm.colors.textTertiary, iconSize = 20.dp)
        }
        Box {
            FmListRow(
                question.type.title,
                subtitle = "Question type",
                leading = { FmIcon(question.type.icon) },
                trailing = { FmIcon("arrow_drop_down", tint = Fm.colors.textTertiary) },
                onTap = { typeMenu = true },
                showChevron = false,
                minHeight = 48.dp,
            )
            FmDropdownMenu(expanded = typeMenu, onDismissRequest = { typeMenu = false }) {
                SurveyQuestionType.entries.forEach { type ->
                    FmDropdownMenuItem(type.title, onClick = { typeMenu = false; onChange(question.withType(type)) }, icon = type.icon)
                }
            }
        }
        FmTextField(question.title, { onChange(question.copy(title = it.take(SurveyLimits.TITLE))) }, hint = "Question", maxLines = 3)
        FmTextField(question.help, { onChange(question.copy(help = it.take(SurveyLimits.HELP))) }, hint = "Help text (optional)", maxLines = 4)
        Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Required", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            FmSwitch(question.required, onCheckedChange = { onChange(question.copy(required = it)) })
        }
        when {
            question.type.isChoice -> {
                question.options.forEachIndexed { at, option ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon(if (question.type == SurveyQuestionType.SINGLE) "radio_button_unchecked" else "check_box_outline_blank", size = 18.dp, tint = Fm.colors.textTertiary)
                        Spacer(Modifier.width(6.dp))
                        FmTextField(option, { onChange(question.option(at, it.take(SurveyLimits.OPTION))) }, Modifier.weight(1f), hint = "Option ${at + 1}")
                        FmIconButton("close", "Remove option", onPressed = { onChange(question.removeOption(at)) }, color = Fm.colors.textTertiary, iconSize = 18.dp)
                    }
                }
                if (question.options.size < SurveyLimits.MAX_OPTIONS) {
                    FmTextButton(onClick = { onChange(question.addOption()) }) { Text("Add option") }
                }
                if (question.title.isNotBlank() && question.cleanOptions.size < SurveyLimits.MIN_OPTIONS) {
                    Text("Give at least ${SurveyLimits.MIN_OPTIONS} different options.", Modifier.padding(horizontal = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.danger)
                }
            }
            question.type == SurveyQuestionType.RATING -> {
                Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Scale 1 to ${question.scale}", Modifier.weight(1f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    FmIconButton("remove", "Smaller scale", onPressed = if (question.scale > SurveyLimits.MIN_SCALE) ({ onChange(question.stepScale(-1)) }) else null, color = Fm.colors.textPrimary)
                    FmIconButton("add", "Larger scale", onPressed = if (question.scale < SurveyLimits.MAX_SCALE) ({ onChange(question.stepScale(1)) }) else null, color = Fm.colors.textPrimary)
                }
            }
            else -> Text(
                if (question.type == SurveyQuestionType.TEXT) "One line, up to 500 characters." else "Long text, up to 5,000 characters.",
                Modifier.padding(horizontal = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
            )
        }
    }
}
