package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.DiagnosticsRuntime
import com.forkmesh.nativeapp.market.MarketModel
import com.forkmesh.nativeapp.market.MarketUninstall
import com.forkmesh.nativeapp.market.PackageMenu
import com.forkmesh.nativeapp.market.PackageReviewsModel
import com.forkmesh.nativeapp.market.PluginFeedback
import com.forkmesh.nativeapp.market.ReviewDraft
import com.forkmesh.nativeapp.market.ReviewRules
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.tools.ReportBugDraft
import com.forkmesh.nativeapp.tools.sendBugReport
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.LocalNavigator
import com.forkmesh.nativeapp.ui.navigation.LocalScreen
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class PluginMenu(val packageId: String, private val navigator: Navigator, private val screen: Screen?) {
    val market: MarketModel? = currentMarketModel()
    private val reviews: PackageReviewsModel? = MarketFeature.client()?.let { PackageReviewsModel(it, packageId) }
    internal var uninstaller: PackageUninstaller? = null
    internal var signedIn = false
    private var version by mutableIntStateOf(0)
    private var loading by mutableStateOf(false)
    private var loaded = false
    private var sheet by mutableStateOf(false)
    private var problem by mutableStateOf<String?>(null)

    val name: String
        get() {
            @Suppress("UNUSED_EXPRESSION") version
            return PackageMenu.name(packageId, market?.pkg(packageId)?.name ?: StatusPageFeature.store?.catalog?.get(packageId)?.name)
        }

    private suspend fun load() {
        if (loaded) return
        loaded = true
        loading = true
        val market = market
        if (market != null) {
            if (market.pkg(packageId) == null) {
                val names = MarketFeature.organizationNamesOrNull()
                withContext(Dispatchers.IO) { market.refresh(names) }
                StatusPageFeature.rememberCatalog(market.packages)
            }
            if (market.pkg(packageId)?.installsPerOrganization == true && market.organizations.isEmpty()) {
                val names = MarketFeature.organizationNames()
                if (names.isNotEmpty()) withContext(Dispatchers.IO) { market.refresh(names) }
            }
        }
        reviews?.let { withContext(Dispatchers.IO) { it.refresh() } }
        loading = false
        version++
    }

    internal fun finished(outcome: UninstallOutcome) {
        version++
        if (outcome.problem != null) {
            problem = outcome.problem
            return
        }
        val exportPending = outcome.request.action is MarketUninstall.Organization &&
            market?.removed?.let { it.packageId == packageId && it.export != null } == true
        if (!exportPending) leave()
    }

    internal fun exportSaved(ok: Boolean) {
        if (ok) leave() else problem = "Could not save the export. Open ${name} in the Market to save it."
    }

    private fun leave() {
        if (screen != null && navigator.top === screen) navigator.pop()
    }

    @Composable
    fun Items(close: () -> Unit, divided: Boolean = false) {
        if (divided) MenuDivider()
        FmDropdownMenuItem(PackageMenu.OPTIONS, onClick = { close(); open() }, icon = "star")
    }

    fun open() {
        sheet = true
    }

    @Composable
    fun Host() {
        uninstaller?.Dialog()
        if (sheet) QuickSheet()
        problem?.let { message ->
            FmConfirmDialog(
                title = "Could not uninstall $name",
                body = message,
                onConfirm = { problem = null },
                onDismissRequest = { problem = null },
                hideCancel = true,
            )
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun QuickSheet() {
        LaunchedEffect(Unit) { load() }
        @Suppress("UNUSED_EXPRESSION") version
        val label = name
        val screenTitle = screen?.title ?: label
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        var feedback by remember { mutableStateOf(PluginFeedback()) }
        var busy by remember { mutableStateOf(false) }
        var notice by remember { mutableStateOf("") }
        var fieldIssues by remember { mutableStateOf(emptyMap<String, String>()) }
        var thanks by remember { mutableStateOf<String?>(null) }
        val rules = reviews?.rules ?: ReviewRules()
        val viewer = reviews?.page?.viewer
        fun edit(next: PluginFeedback) {
            feedback = next
            notice = ""
            fieldIssues = emptyMap()
        }
        fun close() {
            sheet = false
        }
        fun send() {
            if (!feedback.canSend) {
                notice = PluginFeedback.NOTHING_TO_SEND
                return
            }
            val model = reviews
            val review = model?.let { feedback.review(it.draft, rules) }
            if (review != null) {
                val issues = review.issues(rules)
                if (issues.isNotEmpty()) {
                    fieldIssues = issues
                    return
                }
            }
            val sent = feedback
            busy = true
            scope.launch {
                if (model != null && review != null) {
                    val ok = withContext(Dispatchers.IO) {
                        model.draft = review
                        model.submit()
                    }
                    version++
                    if (!ok) {
                        notice = model.errorText.ifEmpty { model.page?.viewer?.cannotRateReason.orEmpty() }
                        fieldIssues = model.fieldIssues
                        busy = false
                        return@launch
                    }
                }
                if (sent.reports) {
                    val api = Services.get<ApiService>()
                    val step = ReportBugDraft(message = sent.text).submit(apiAvailable = api != null)
                    if (!step.send || api == null) {
                        notice = step.draft.problem
                        busy = false
                        return@launch
                    }
                    val result = withContext(Dispatchers.IO) { sendBugReport(api, step.draft, screenTitle, DiagnosticsRuntime.build, packageId) }
                    if (!result.ok) {
                        notice = result.message
                        busy = false
                        return@launch
                    }
                }
                busy = false
                thanks = PluginFeedback.thanks(label, rated = review != null, reported = sent.reports)
            }
        }
        FmModalBottomSheet(onDismissRequest = { if (!busy) close() }, isScrollControlled = true, containerColor = c.bgRaised) {
            Column(
                Modifier
                    .imePadding()
                    .verticalScroll(rememberScrollState())
                    .padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(label, style = Fm.text.titleLarge.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
                    FmIconButton("close", "Close", onPressed = if (busy) null else ({ close() }), color = c.textPrimary)
                }
                val thanked = thanks
                if (thanked != null) {
                    FmInlineStatusLine(thanked, isError = false)
                    AccountFilledButton("Done", "check", onClick = { close() }, modifier = Modifier.fillMaxWidth().height(48.dp))
                    return@Column
                }
                Column {
                    Text(PackageMenu.rateLabel(label, viewer?.review != null), style = Fm.text.bodySmall, color = c.textTertiary)
                    val shown = if (feedback.rates) feedback.stars else viewer?.review?.stars ?: 0
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        (1..5).forEach { star ->
                            FmIconButton(
                                if (star <= shown) "star" else "star_border",
                                "Rate $star ${if (star == 1) "star" else "stars"}",
                                onPressed = if (busy || viewer?.canRate != true) null else ({ edit(feedback.copy(stars = star)) }),
                                color = c.warning,
                                iconSize = 28.dp,
                            )
                        }
                        if (loading) FmCircularProgressIndicator(Modifier.padding(start = FmSpace.x2).size(18.dp))
                    }
                    if (viewer != null && !viewer.canRate) {
                        Text(viewer.cannotRateReason, style = Fm.text.bodySmall, color = c.textTertiary)
                    }
                }
                FmTextField(
                    value = feedback.text,
                    onValueChange = { edit(feedback.copy(text = it)) },
                    hint = PluginFeedback.HINT,
                    errorText = fieldIssues["problem"]?.let(ReviewDraft::fieldMessage),
                    minLines = 3,
                    maxLines = 8,
                    enabled = !busy,
                )
                if (feedback.asksImprovement(rules)) {
                    FmTextField(
                        value = feedback.improvement,
                        onValueChange = { edit(feedback.copy(improvement = it)) },
                        hint = PluginFeedback.IMPROVEMENT_HINT,
                        errorText = fieldIssues["improvement"]?.let(ReviewDraft::fieldMessage),
                        minLines = 2,
                        maxLines = 6,
                        enabled = !busy,
                    )
                }
                if (notice.isNotEmpty()) FmInlineStatusLine(notice, isError = true)
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    FmToneButton(
                        PluginFeedback.AGENT,
                        onPressed = if (busy || !feedback.canAskAgent) null else ({
                            val prompt = feedback.agentPrompt(label, packageId, screenTitle)
                            close()
                            NavigationFeature.compose(ComposeTarget(text = prompt))
                        }),
                        icon = "auto_awesome",
                        tone = FmTone.Accent,
                        height = 48.dp,
                    )
                    AccountFilledButton(
                        if (busy) PluginFeedback.SENDING else PluginFeedback.SEND,
                        "send",
                        onClick = { send() },
                        enabled = !busy && feedback.canSend,
                        modifier = Modifier.weight(1f).height(48.dp),
                    )
                }
                val requests = market?.let { model ->
                    model.pkg(packageId)?.let { pkg -> model.uninstallAction(pkg, signedIn)?.let { PackageUninstall.requests(pkg, it) } }
                }.orEmpty()
                if (requests.isNotEmpty()) MenuDivider()
                requests.forEach { request ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable(enabled = !busy && uninstaller?.working == null) {
                                close()
                                uninstaller?.ask(request)
                            }
                            .padding(vertical = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    ) {
                        FmIcon("delete", size = 20.dp, tint = c.danger)
                        Text(PackageMenu.uninstallLabel(request.organization), style = Fm.text.bodyMedium, color = c.danger)
                    }
                }
            }
        }
    }
}

@Composable
private fun MenuDivider() {
    HorizontalDivider(thickness = Dp.Hairline, color = Fm.colors.border)
}

@Composable
internal fun rememberPluginMenu(packageId: String, navigator: Navigator, screen: Screen?): PluginMenu {
    val menu = remember(packageId, navigator, screen) { PluginMenu(packageId, navigator, screen) }
    val market = menu.market
    if (market != null) {
        val saveExport = rememberExportSaver(market) { ok -> menu.exportSaved(ok) }
        menu.uninstaller = rememberPackageUninstaller(market, saveExport) { outcome -> menu.finished(outcome) }
    }
    menu.signedIn = rememberPackagesSignedIn()
    return menu
}

@Composable
internal fun rememberScreenPluginMenu(): PluginMenu? {
    val screen = LocalScreen.current
    val packageId = screen?.pluginPackage ?: return null
    return rememberPluginMenu(packageId, LocalNavigator.current, screen)
}

@Composable
internal fun PluginMoreMenu(menu: PluginMenu) {
    FmIconButton("more_vert", PackageMenu.MORE, onPressed = { menu.open() }, color = Fm.colors.textPrimary)
    menu.Host()
}

@Composable
internal fun PluginMenuButton() {
    val menu = rememberScreenPluginMenu() ?: return
    PluginMoreMenu(menu)
}
