package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.Canvas
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.market.AdReviewDecision
import com.forkmesh.nativeapp.market.AdReviewQueue
import com.forkmesh.nativeapp.market.AdsModel
import com.forkmesh.nativeapp.market.AnalyticsOverview
import com.forkmesh.nativeapp.market.AnalyticsProject
import com.forkmesh.nativeapp.market.AnalyticsRange
import com.forkmesh.nativeapp.market.AnalyticsTopRow
import com.forkmesh.nativeapp.market.AnalyticsTotals
import com.forkmesh.nativeapp.market.Competitor
import com.forkmesh.nativeapp.market.CompetitorBoard
import com.forkmesh.nativeapp.market.CrmStageLabel
import com.forkmesh.nativeapp.market.OrgPackageModel
import com.forkmesh.nativeapp.market.OwnedAd
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale

private abstract class OrgPackageScreen<P : Any>(private val screenTitle: String, private val packageName: String) : Screen() {
    override val title: String get() = screenTitle

    protected abstract val model: OrgPackageModel<P>?
    protected var loading by mutableStateOf(true)
    protected var version by mutableIntStateOf(0)
    private var reload by mutableIntStateOf(1)
    private var loadOrg by mutableIntStateOf(0)

    @Composable
    protected abstract fun Body(payload: P, navigator: Navigator)

    @Composable
    protected open fun Actions() {}

    @Composable
    protected open fun Overlay(payload: P?, navigator: Navigator) {}

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        LaunchedEffect(reload) {
            loading = true
            withContext(Dispatchers.IO) { model.refresh() }
            loading = false
            version++
        }
        LaunchedEffect(loadOrg) {
            if (loadOrg == 0) return@LaunchedEffect
            loading = true
            withContext(Dispatchers.IO) { model.load() }
            loading = false
            version++
        }
        @Suppress("UNUSED_EXPRESSION") version
        val payload = model.payload
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar(screenTitle, navigator) {
                    Actions()
                    FmIconButton("refresh", "Refresh", onPressed = { reload++ }, color = Fm.colors.textPrimary)
                }
                OrganizationPicker(model.organizations, model.organization, packageName) { name ->
                    model.organization = name
                    loadOrg++
                }
                ErrorLine(model.errorText)
                when {
                    loading && payload == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    payload == null -> FmEmptyState("extension", "Nothing to show", message = "Install $packageName for an organization in the Market.")
                    else -> Body(payload, navigator)
                }
            }
            Overlay(payload, navigator)
        }
    }
}

private class AnalyticsScreenImpl : OrgPackageScreen<List<AnalyticsProject>>("Analytics", "Product analytics") {
    private val client = MarketFeature.client()
    override val model = client?.let { c -> OrgPackageModel("", { c.analyticsOrganizations() }) { org -> c.analyticsProjects(org) } }
    private var projectId by mutableStateOf("")
    private var range by mutableStateOf(AnalyticsRange.WEEK)
    private var overview by mutableStateOf<AnalyticsOverview?>(null)
    private var error by mutableStateOf("")

    @Composable
    override fun Body(payload: List<AnalyticsProject>, navigator: Navigator) {
        val model = model ?: return
        val client = client ?: return
        if (payload.isEmpty()) {
            FmEmptyState("show_chart", "No analytics projects")
            return
        }
        if (payload.none { it.id == projectId }) projectId = (payload.firstOrNull { it.siteDefault } ?: payload.first()).id
        LaunchedEffect(model.organization, projectId, range) {
            error = ""
            val result = withContext(Dispatchers.IO) { runCatching { client.analyticsOverview(model.organization, projectId, range, System.currentTimeMillis()) } }
            result.onSuccess { overview = it }.onFailure { error = PackagesException.describe(it) }
        }
        var projectMenu by remember { mutableStateOf(false) }
        Column(Modifier.verticalScroll(rememberScrollState())) {
            Box {
                FmListRow(payload.first { it.id == projectId }.name, subtitle = "Project", onTap = { projectMenu = true }, minHeight = 48.dp)
                FmDropdownMenu(expanded = projectMenu, onDismissRequest = { projectMenu = false }) {
                    payload.forEach { project -> FmDropdownMenuItem(project.name, onClick = { projectMenu = false; projectId = project.id }) }
                }
            }
            val ranges = AnalyticsRange.entries
            FmSegmentTabBar(ranges.map { FmSegmentTab(it.wire) }, ranges.indexOf(range), onChanged = { range = ranges[it] })
            ErrorLine(error)
            val data = overview
            if (data == null) {
                Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                return@Column
            }
            Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Tile("Visitors", data.totals.visitors, data.previous.visitors, Modifier.weight(1f))
                Tile("Page views", data.totals.pageviews, data.previous.pageviews, Modifier.weight(1f))
                Tile("Sessions", data.totals.sessions, data.previous.sessions, Modifier.weight(1f))
            }
            data.series.firstOrNull()?.let { Sparkline(it.data) }
            Text(
                "Bounce ${String.format(Locale.US, "%.0f%%", data.totals.bounceRate * 100)} · average session ${data.totals.averageSessionMs / 1000}s · ${data.totals.events} events",
                Modifier.padding(horizontal = 16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary,
            )
            if (data.truncated) Text("Showing a sample: the range has more events than one query reads.", Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.warning)
            TopList("Pages", data.pages)
            TopList("Referrers", data.referrers)
            TopList("Countries", data.countries)
            TopList("Devices", data.devices)
            TopList("Events", data.events)
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun Tile(label: String, value: Long, previous: Long, modifier: Modifier) {
    FmCard(modifier) {
        Column(Modifier.padding(10.dp)) {
            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            Text(value.toString(), style = Fm.text.titleMedium)
            AnalyticsTotals.change(value, previous)?.let { change ->
                Text(
                    (if (change >= 0) "+" else "") + String.format(Locale.US, "%.0f%%", change * 100),
                    style = Fm.text.labelSmall,
                    color = if (change >= 0) Fm.colors.success else Fm.colors.danger,
                )
            }
        }
    }
}

@Composable
private fun Sparkline(points: List<Double>) {
    if (points.size < 2) return
    val color = Fm.colors.accent
    val max = points.maxOrNull()?.takeIf { it > 0 } ?: 1.0
    Canvas(Modifier.fillMaxWidth().height(80.dp).padding(horizontal = 16.dp)) {
        val step = size.width / (points.size - 1)
        val path = Path()
        points.forEachIndexed { index, value ->
            val point = Offset(index * step, size.height - (value / max * size.height).toFloat())
            if (index == 0) path.moveTo(point.x, point.y) else path.lineTo(point.x, point.y)
        }
        drawPath(path, color, style = Stroke(width = 2.dp.toPx()))
    }
}

@Composable
private fun TopList(title: String, rows: List<AnalyticsTopRow>) {
    if (rows.isEmpty()) return
    FmSectionHeader(title, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = rows.size)
    rows.forEach { row -> FmListRow(row.value.ifEmpty { "(none)" }, trailing = { Text(row.count.toString(), style = Fm.text.bodySmall) }, showChevron = false, minHeight = 40.dp) }
}

private class CompetitorsScreenImpl : OrgPackageScreen<CompetitorBoard>("Competitors", "Competitor watchlist") {
    private val client = MarketFeature.client()
    override val model = client?.let { c -> OrgPackageModel("", { c.competitorOrganizations() }) { org -> c.competitors(org) } }
    private var query by mutableStateOf("")
    private var editing by mutableStateOf<Competitor?>(null)

    @Composable
    override fun Body(payload: CompetitorBoard, navigator: Navigator) {
        val now = System.currentTimeMillis()
        FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp, vertical = 8.dp), hint = "Search competitors")
        val groups = payload.grouped(query)
        if (groups.isEmpty()) {
            FmEmptyState("travel_explore", "No competitors yet")
            return
        }
        LazyColumn(Modifier.fillMaxSize()) {
            groups.forEach { group ->
                item(key = "g-${group.status}") { FmSectionHeader(CrmStageLabel.title(group.status), Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = group.items.size) }
                items(group.items, key = { it.id }) { competitor ->
                    FmListRow(
                        competitor.name,
                        subtitle = listOfNotNull(competitor.category.ifEmpty { null }, competitor.positioning.take(80).ifEmpty { null }).joinToString(" · ").ifEmpty { null },
                        trailing = if (competitor.needsReview(now)) ({ FmStatusBadge("Review", Fm.colors.warning) }) else null,
                        onTap = { editing = competitor },
                    )
                }
            }
            item { Spacer(Modifier.height(88.dp)) }
        }
    }

    @Composable
    override fun Overlay(payload: CompetitorBoard?, navigator: Navigator) {
        if (payload != null) {
            Box(Modifier.fillMaxSize()) {
                FmExtendedFloatingActionButton("Competitor", onClick = { editing = Competitor() }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), icon = "add")
            }
        }
        val current = editing ?: return
        val model = model ?: return
        val client = client ?: return
        CompetitorEditor(
            current,
            onDismiss = { editing = null },
            onSave = { next ->
                val saved = client.saveCompetitor(model.organization, next)
                model.update { board -> board.copy(competitors = listOf(saved) + board.competitors.filter { it.id != saved.id }) }
                version++
            },
            onReviewed = if (current.id.isEmpty()) null else ({
                val saved = client.markCompetitorReviewed(model.organization, current, System.currentTimeMillis())
                model.update { board -> board.copy(competitors = board.competitors.map { if (it.id == saved.id) saved else it }) }
                version++
            }),
            onDelete = if (current.id.isEmpty()) null else ({
                client.deleteCompetitor(model.organization, current.id)
                model.update { board -> board.copy(competitors = board.competitors.filter { it.id != current.id }) }
                version++
            }),
        )
    }
}

@Composable
private fun CompetitorEditor(
    initial: Competitor,
    onDismiss: () -> Unit,
    onSave: (Competitor) -> Unit,
    onReviewed: (() -> Unit)?,
    onDelete: (() -> Unit)?,
) {
    val scope = rememberCoroutineScope()
    var draft by remember(initial) { mutableStateOf(initial) }
    var problem by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    fun act(work: () -> Unit) {
        busy = true
        scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching(work) }
            busy = false
            result.onSuccess { onDismiss() }.onFailure { problem = PackagesException.describe(it) }
        }
    }
    FmConfirmDialog(
        title = if (initial.id.isEmpty()) "New competitor" else initial.name,
        confirmLabel = "Save",
        busy = busy,
        onDismissRequest = onDismiss,
        onConfirm = {
            when {
                draft.name.isBlank() -> problem = "Give the competitor a name."
                !draft.websiteIsValid -> problem = "Websites must start with https://."
                else -> act { onSave(draft) }
            }
        },
        fields = {
            problem?.let { Text(it, color = Fm.colors.danger, style = Fm.text.bodySmall) }
            FmTextField(draft.name, { draft = draft.copy(name = it) }, hint = "Name")
            FmTextField(draft.website, { draft = draft.copy(website = it) }, hint = "Website (https://)")
            FmTextField(draft.category, { draft = draft.copy(category = it) }, hint = "Category")
            FmTextField(draft.positioning, { draft = draft.copy(positioning = it) }, hint = "Positioning", maxLines = 3)
            FmTextField(draft.strengths, { draft = draft.copy(strengths = it) }, hint = "Strengths", maxLines = 3)
            FmTextField(draft.weaknesses, { draft = draft.copy(weaknesses = it) }, hint = "Weaknesses", maxLines = 3)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Competitor.STATUSES.forEach { status ->
                    FmToneButton(CrmStageLabel.title(status), onPressed = { draft = draft.copy(status = status) }, tone = if (draft.status == status) FmTone.Accent else FmTone.Neutral)
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                onReviewed?.let { FmTextButton(onClick = { act(it) }) { Text("Mark reviewed") } }
                onDelete?.let { FmTextButton(onClick = { act(it) }, contentColor = Fm.colors.danger) { Text("Delete") } }
            }
        },
    )
}

private class AdsScreenImpl : Screen() {
    override val title: String get() = "Ads"

    private val model = MarketFeature.client()?.let(::AdsModel)
    private var loading by mutableStateOf(true)
    private var version by mutableIntStateOf(0)
    private var reload by mutableIntStateOf(1)
    private var tab by mutableIntStateOf(0)
    private var deciding by mutableStateOf<Pair<OwnedAd, String>?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(reload) {
            loading = true
            withContext(Dispatchers.IO) { model.refresh() }
            loading = false
            version++
        }
        @Suppress("UNUSED_EXPRESSION") version
        val account = model.account
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Ads", navigator) { FmIconButton("refresh", "Refresh", onPressed = { reload++ }, color = Fm.colors.textPrimary) }
            if (model.isReviewer) FmSegmentTabBar(listOf(FmSegmentTab("My ads", account?.ads?.size), FmSegmentTab("Review", model.queue?.ads?.size)), tab, onChanged = { tab = it })
            ErrorLine(model.errorText)
            when {
                loading && account == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                account == null -> FmEmptyState("ads_click", "Ads unavailable")
                tab == 1 && model.isReviewer -> ReviewQueue(model, onFilter = { filter ->
                    model.reviewFilter = filter
                    scope.launch { withContext(Dispatchers.IO) { model.loadQueue() }; version++ }
                }, onDecide = { ad, action -> deciding = ad to action })
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    item {
                        FmCard(Modifier.fillMaxWidth().padding(16.dp)) {
                            Column {
                                FmKeyValueRow("Plan", account.tierLabel.ifEmpty { "Free" })
                                FmKeyValueRow("Live ads", "${account.running} running of ${account.liveAds}")
                                FmKeyValueRow("Waiting", "${account.pending} of ${account.pendingLimit}")
                                FmKeyValueRow("All ads", "${account.total} of ${account.totalLimit}")
                            }
                        }
                    }
                    if (account.ads.isEmpty()) item { Text("Create ads on the web; they show up here to manage.", Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary) }
                    items(account.ads, key = { it.id }) { ad ->
                        AdCard(ad) {
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                ad.ownerActions.forEach { action ->
                                    FmToneButton(action.replaceFirstChar { it.uppercase() }, onPressed = {
                                        scope.launch { withContext(Dispatchers.IO) { model.perform(action, ad) }; version++ }
                                    }, tone = if (action == "withdraw") FmTone.Danger else FmTone.Neutral)
                                }
                            }
                        }
                    }
                }
            }
        }
        val decision = deciding
        if (decision != null) {
            val (ad, action) = decision
            var reason by remember(decision) { mutableStateOf("") }
            val codes = remember(decision) { mutableStateOf(setOf<String>()) }
            val needsReason = AdReviewDecision(action).needsReason
            FmConfirmDialog(
                title = "${action.replaceFirstChar { it.uppercase() }} “${ad.headline}”?",
                confirmLabel = action.replaceFirstChar { it.uppercase() },
                destructive = needsReason,
                onDismissRequest = { deciding = null },
                onConfirm = {
                    val chosen = AdReviewDecision(action, reason, codes.value.toList())
                    if (!chosen.isComplete) return@FmConfirmDialog
                    deciding = null
                    scope.launch { withContext(Dispatchers.IO) { model.review(ad, chosen) }; version++ }
                },
                fields = if (!needsReason) null else ({
                    FmTextField(reason, { reason = it }, hint = "Reason for the advertiser", maxLines = 4)
                    model.queue?.prohibited?.forEach { item ->
                        val on = item.id in codes.value
                        FmToneButton(item.label, onPressed = { codes.value = if (on) codes.value - item.id else codes.value + item.id }, tone = if (on) FmTone.Danger else FmTone.Neutral)
                    }
                }),
            )
        }
    }
}

@Composable
private fun AdCard(ad: OwnedAd, actions: @Composable () -> Unit) {
    FmCard(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(ad.headline.ifEmpty { "(No headline)" }, Modifier.weight(1f), style = Fm.text.bodyMedium)
                FmStatusBadge(ad.deliveryText, if (ad.delivery == "running") Fm.colors.success else Fm.colors.textTertiary)
            }
            if (ad.body.isNotEmpty()) Text(ad.body, style = Fm.text.bodySmall)
            Text(
                listOfNotNull(ad.sponsor.ifEmpty { null }, ad.category.ifEmpty { null }, "${ad.clicks} clicks in ${ad.clickWindowDays} days", if (ad.openReports > 0) "${ad.openReports} reports" else null, ad.owner.ifEmpty { null }?.let { "@$it" }).joinToString(" · "),
                style = Fm.text.labelSmall,
                color = Fm.colors.textTertiary,
            )
            if (ad.reviewReason.isNotEmpty()) Text("Review: ${ad.reviewReason}", style = Fm.text.bodySmall, color = Fm.colors.warning)
            if (ad.screenFlags.isNotEmpty()) Text("Flags: ${ad.screenFlags.joinToString(", ")}", style = Fm.text.labelSmall, color = Fm.colors.danger)
            actions()
        }
    }
}

@Composable
private fun ReviewQueue(model: AdsModel, onFilter: (String) -> Unit, onDecide: (OwnedAd, String) -> Unit) {
    val queue = model.queue
    val filters = AdReviewQueue.FILTERS
    FmSegmentTabBar(filters.map { FmSegmentTab(CrmStageLabel.title(it), queue?.counts?.get(it)) }, filters.indexOf(model.reviewFilter).coerceAtLeast(0), onChanged = { onFilter(filters[it]) })
    val ads = queue?.ads.orEmpty()
    if (ads.isEmpty()) {
        FmEmptyState("done_all", "Nothing to review")
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        items(ads, key = { it.id }) { ad ->
            AdCard(ad) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    ad.reviewActions.forEach { action ->
                        FmToneButton(action.replaceFirstChar { it.uppercase() }, onPressed = { onDecide(ad, action) }, tone = if (action == "approve" || action == "reinstate") FmTone.Success else FmTone.Danger)
                    }
                }
            }
        }
    }
}

@Suppress("FunctionName")
fun AnalyticsScreen(): Screen = AnalyticsScreenImpl()

@Suppress("FunctionName")
fun CompetitorsScreen(): Screen = CompetitorsScreenImpl()

@Suppress("FunctionName")
fun AdsScreen(): Screen = AdsScreenImpl()
