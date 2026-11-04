package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.kit.LocalShellClearance
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.ExperimentalMaterial3Api
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.DesktopRepo
import com.forkmesh.nativeapp.code.DesktopReposModel
import com.forkmesh.nativeapp.code.RepoCatalog
import com.forkmesh.nativeapp.code.RepoFilter
import com.forkmesh.nativeapp.code.RepoMetaItem
import com.forkmesh.nativeapp.code.RepoSort
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.control.HubControlCalling
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEdgeFadeScroll
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmSliverFadeIn
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.feature.localrepos.LocalRepoActions
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import androidx.compose.material3.HorizontalDivider
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import com.forkmesh.nativeapp.ui.navigation.LocalShellPaneOpen
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.navigation.shellPaneSelected
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal class ReposScreen : Screen() {
    private var searchText by mutableStateOf("")
    private var query by mutableStateOf("")
    private var filter by mutableStateOf(RepoFilter.All)
    private var visibility by mutableStateOf<RepoFilter?>(null)
    private var sort by mutableStateOf(RepoSort.RecentlyUpdated)
    private val repos = AsyncValue<List<Repository>>().apply { reload { repoApi.repositories() } }
    private var desktop: DesktopReposModel? = null
    private var desktopHub: DesktopControlHub? = null
    private var desktopTick by mutableIntStateOf(1)
    private var desktopLoadedTick = 0
    private var desktopRows by mutableStateOf<List<DesktopRepo>>(emptyList())
    private var desktopProblem by mutableStateOf<String?>(null)
    private var desktopLoading by mutableStateOf(false)
    private var desktopTitle by mutableStateOf("Desktop")

    override val title: String get() = "Repos"

    override val extendsUnderTabBar: Boolean get() = true

    private fun reload() {
        desktopTick++
        repos.reload { repoApi.repositories() }
    }

    override fun onDisposed() {
        desktop?.stop()
    }

    private fun desktopModel(): DesktopReposModel? {
        val hub = runCatching { AgentsFeature.hub }.getOrNull()
        if (hub === desktopHub) return desktop
        desktop?.stop()
        desktopHub = hub
        desktopLoadedTick = 0
        desktop = hub?.let { current ->
            val name = (current.reach as? DesktopReach.Connected)?.name ?: current.desktop?.name.orEmpty()
            DesktopReposModel(HubControlCalling(current), name) {
                runCatching { AgentsFeature.control.desktopSnapshot?.repositories }.getOrNull().orEmpty()
            }.also { model -> model.start { desktopTick++ } }
        }
        syncDesktop()
        return desktop
    }

    private fun syncDesktop() {
        val model = desktop
        desktopRows = model?.repos.orEmpty()
        desktopProblem = model?.problem
        desktopTitle = model?.title ?: "Desktop"
    }

    @Composable
    override fun Content(navigator: Navigator) {
        repos.Bind()
        val desktopNow = desktopModel()
        val tick = desktopTick
        LaunchedEffect(desktopNow, tick) {
            val model = desktopNow ?: return@LaunchedEffect
            if (desktopLoadedTick == tick) return@LaunchedEffect
            desktopLoadedTick = tick
            desktopLoading = true
            try {
                withContext(Dispatchers.IO) { model.load() }
            } finally {
                desktopLoading = false
                syncDesktop()
            }
        }
        val local = LocalReposFeature.rememberActions(navigator)
        Column(Modifier.fillMaxSize()) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x1),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Repos", Modifier.weight(1f).semantics { heading() }, style = Fm.text.displaySmall)
                    RefreshButton()
                    if (local != null) {
                        Spacer(Modifier.width(FmSpace.x2))
                        AddMenu(local)
                    }
                }
                Spacer(Modifier.height(FmSpace.x2))
                SearchField(Modifier.fillMaxWidth())
            }
            CatalogControls()
            visibility?.let { shown -> ActiveFilterPill(shown.label) { visibility = null } }
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .background(Fm.colors.listSurface),
            ) {
                Catalog(navigator, local)
            }
        }
    }

    @Composable
    private fun SearchField(modifier: Modifier) {
        val c = Fm.colors
        val interaction = remember { MutableInteractionSource() }
        val focused by interaction.collectIsFocusedAsState()
        val shape = RoundedCornerShape(FmRadius.lg)
        BasicTextField(
            value = searchText,
            onValueChange = FmSafeInput.typing {
                searchText = it
                query = RepoCatalog.normalizeQuery(it)
            },
            modifier = modifier.height(40.dp),
            singleLine = true,
            textStyle = Fm.text.bodyLarge.copy(color = c.textPrimary),
            cursorBrush = SolidColor(c.accent),
            interactionSource = interaction,
            decorationBox = { inner ->
                Row(
                    Modifier
                        .fillMaxSize()
                        .background(c.bgOverlay, shape)
                        .then(if (focused) Modifier.border(1.2.dp, c.accent, shape) else Modifier),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.width(40.dp), contentAlignment = Alignment.Center) {
                        FmIcon("search", size = 18.dp, tint = c.textTertiary)
                    }
                    Box(
                        Modifier
                            .weight(1f)
                            .padding(end = FmSpace.x4),
                    ) {
                        if (searchText.isEmpty()) {
                            Text(
                                "Find a repository",
                                maxLines = 1,
                                style = Fm.text.bodyLarge.copy(color = c.textTertiary),
                            )
                        }
                        inner()
                    }
                }
            },
        )
    }

    @Composable
    private fun RefreshButton() {
        val c = Fm.colors
        val haptics = FmHaptics.current
        FmTooltip("Refresh repositories") {
            Box(
                Modifier
                    .size(44.dp)
                    .fmGlass(CircleShape, shadow = false, interactive = true)
                    .clickable {
                        haptics.tap()
                        reload()
                    }
                    .semantics {
                        contentDescription = "Refresh repositories"
                        role = Role.Button
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("refresh", size = 22.dp, tint = c.textPrimary)
            }
        }
    }

    @Composable
    private fun AddMenu(local: LocalRepoActions) {
        val c = Fm.colors
        var open by remember { mutableStateOf(false) }
        Box {
            FmTooltip("New repository") {
                Box(
                    Modifier
                        .size(44.dp)
                        .fmGlass(CircleShape, shadow = false, interactive = true)
                        .clickable(enabled = !local.busy) { open = true }
                        .semantics {
                            contentDescription = "New, import or mirror a repository"
                            role = Role.Button
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("add", size = 22.dp, tint = c.textPrimary)
                }
            }
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                FmDropdownMenuItem("New repository", icon = "add", onClick = {
                    open = false
                    local.create()
                })
                FmDropdownMenuItem("Import from Files", icon = "drive_folder_upload_outlined", onClick = {
                    open = false
                    local.importFiles()
                })
                FmDropdownMenuItem("Mirror a repository", icon = "cloud_sync_outlined", onClick = {
                    open = false
                    local.mirror()
                })
                if (local.canSync) {
                    FmDropdownMenuItem(local.syncLabel, icon = "sync", onClick = {
                        open = false
                        local.sync()
                    })
                }
            }
        }
    }

    @Composable
    private fun CatalogControls() {
        val haptics = FmHaptics.current
        val data = repos.data
        val base = data.orEmpty().filter { RepoCatalog.matchesQuery(it, query) && RepoCatalog.matchesVisibility(it, visibility) }
        val reach = RepoCatalog.reachFilters
        val tabs = reach.map { option -> FmSegmentTab(option.label, if (data == null) null else base.count { RepoCatalog.matchesFilter(it, option) }) }
        FmSegmentTabBar(
            tabs,
            reach.indexOf(filter).coerceAtLeast(0),
            onChanged = { index ->
                haptics.selection()
                filter = reach[index]
            },
            modifier = Modifier.padding(horizontal = FmSpace.x2),
            trailing = { FilterButton() },
        )
    }

    @Composable
    private fun FilterButton() {
        val c = Fm.colors
        val haptics = FmHaptics.current
        var open by remember { mutableStateOf(false) }
        val active = visibility != null || sort != RepoSort.RecentlyUpdated
        Box {
            FmTooltip("Filter and sort") {
                Box(
                    Modifier
                        .size(40.dp)
                        .fmGlass(CircleShape, shadow = false, interactive = true)
                        .clickable { open = true }
                        .semantics {
                            contentDescription = "Filter and sort repositories, ${visibility?.label ?: "any visibility"}, ${sort.buttonLabel}"
                            role = Role.Button
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("filter_list", size = 20.dp, tint = c.textPrimary)
                    if (active) {
                        Box(
                            Modifier
                                .align(Alignment.TopEnd)
                                .padding(top = 8.dp, end = 8.dp)
                                .size(7.dp)
                                .background(c.accent, CircleShape),
                        )
                    }
                }
            }
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                val check: @Composable () -> Unit = { FmIcon("check", size = 20.dp, tint = c.accent) }
                FmDropdownMenuItem("Any visibility", icon = "visibility_outlined", onClick = {
                    open = false
                    haptics.selection()
                    visibility = null
                }, trailing = if (visibility == null) check else null)
                for (option in RepoCatalog.visibilityFilters) {
                    FmDropdownMenuItem(option.label, icon = if (option == RepoFilter.Private) "lock_outline" else "public", onClick = {
                        open = false
                        haptics.selection()
                        visibility = option
                    }, trailing = if (visibility == option) check else null)
                }
                HorizontalDivider(Modifier.padding(vertical = FmSpace.x1), thickness = 0.5.dp, color = c.border)
                for (option in RepoSort.entries) {
                    FmDropdownMenuItem(option.menuLabel, onClick = {
                        open = false
                        haptics.selection()
                        sort = option
                    }, trailing = if (option == sort) check else null)
                }
            }
        }
    }

    @Composable
    private fun ActiveFilterPill(label: String, onClear: () -> Unit) {
        val c = Fm.colors
        Row(Modifier.padding(start = FmSpace.x4, bottom = FmSpace.x2)) {
            Row(
                Modifier
                    .height(28.dp)
                    .clip(RoundedCornerShape(FmRadius.full))
                    .background(c.bgOverlay)
                    .clickable(onClick = onClear)
                    .semantics {
                        contentDescription = "Showing $label only. Clear"
                        role = Role.Button
                    }
                    .padding(start = FmSpace.x3, end = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(label, style = Fm.text.labelMedium.copy(color = c.textPrimary))
                Spacer(Modifier.width(FmSpace.x1))
                FmIcon("close", size = 14.dp, tint = c.textSecondary)
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun Catalog(navigator: Navigator, local: LocalRepoActions?) {
        val waiting = repos.waiting
        val error = repos.error
        val visible = if (waiting || error != null) {
            emptyList()
        } else {
            RepoCatalog.filterAndSort(repos.data.orEmpty(), query, filter, sort).filter { RepoCatalog.matchesVisibility(it, visibility) }
        }
        val networkState = when {
            waiting -> "loading"
            error != null -> "error"
            else -> RepoCatalog.listStateKey(filter, sort, query, visible.size) + "-${visibility?.key.orEmpty()}"
        }
        val density = LocalDensity.current
        var sectionHeight by remember { mutableStateOf(0.dp) }
        val localRecord = LocalReposFeature.rememberLocalRecords()
        FmPullToRefreshBox(isRefreshing = false, onRefresh = { reload() }, modifier = Modifier.fillMaxSize()) {
            BoxWithConstraints(Modifier.fillMaxSize()) {
                val remaining = (maxHeight - sectionHeight).coerceAtLeast(0.dp)
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = LocalShellClearance.current)) {
                    item(key = "local-repos") {
                        Column(Modifier.onSizeChanged { sectionHeight = with(density) { it.height.toDp() } }) {
                            if (desktop != null && (desktopRows.isNotEmpty() || desktopProblem != null || desktopLoading)) {
                                DesktopReposSection(
                                    title = desktopTitle,
                                    count = desktopRows.size,
                                    rows = desktopRows.filter { it.matches(query) },
                                    problem = desktopProblem,
                                ) { row -> navigator.push(RepoDetailScreen(row.repository)) }
                            } else if (desktop == null && NavigationFeature.isRegistered(Route.Kind.Desktops)) {
                                PairDesktopRow { NavigationFeature.open(Route.Desktops) }
                            }
                            if (local != null) LocalReposFeature.LocalReposSection(local)
                            FmSectionHeader(
                                title = "Network",
                                count = if (waiting || error != null) null else visible.size,
                                modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
                            )
                        }
                    }
                    when {
                        waiting -> item {
                            FmSliverFadeIn(networkState) {
                                FmSkeletonList(rows = 6, label = "Loading repositories")
                            }
                        }
                        error != null -> item {
                            FmSliverFadeIn(networkState) {
                                Box(
                                    Modifier
                                        .fillMaxWidth()
                                        .heightIn(min = remaining),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    CatalogError(message = "$error", onRetry = ::reload)
                                }
                            }
                        }
                        visible.isEmpty() -> item {
                            FmSliverFadeIn(networkState) {
                                val active = RepoCatalog.hasActiveControls(query, filter) || visibility != null
                                Box(
                                    Modifier
                                        .fillMaxWidth()
                                        .heightIn(min = remaining),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    FmEmptyState(
                                        icon = "book_outlined",
                                        title = RepoCatalog.emptyTitle(active),
                                        message = RepoCatalog.emptyMessage(active),
                                    )
                                }
                            }
                        }
                        else -> {
                            itemsIndexed(visible) { index, repo ->
                                FmSliverFadeIn(networkState) {
                                    Column {
                                        if (index > 0) FmRowDivider()
                                        RepoTile(repo, localRecord(repo), selected = paneOpens(repo)) { navigator.push(RepoDetailScreen(repo)) }
                                    }
                                }
                            }
                            item { Spacer(Modifier.height(FmSpace.x5)) }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun CatalogError(message: String, onRetry: () -> Unit) {
    Box(Modifier.padding(FmSpace.x4)) {
        FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                FmEmptyState(icon = "cloud_off", title = "Could not load repositories", message = message)
                Spacer(Modifier.height(FmSpace.x3))
                FmOutlinedButton("Retry", onClick = onRetry)
            }
        }
    }
}

@Composable
private fun RepoTile(repo: Repository, local: LocalRepoRecord?, selected: Boolean = false, onTap: () -> Unit) {
    val c = Fm.colors
    val online = RepoCatalog.isOnline(repo)
    val owner = repo.displayOwner
    FmListRow(
        modifier = Modifier.shellPaneSelected(selected),
        title = repo.fullName,
        minHeight = 64.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        leading = {
            FmAvatar(owner.ifEmpty { "?" }, 36.dp, textStyle = Fm.text.labelMedium)
        },
        titleContent = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    buildAnnotatedString {
                        withStyle(SpanStyle(color = c.textSecondary, fontWeight = FmType.regular)) { append("$owner/") }
                        append(repo.name)
                    },
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall,
                )
                Spacer(Modifier.width(FmSpace.x2))
                PresenceDot(online)
            }
        },
        subtitleContent = {
            Column {
                if (repo.description.isNotEmpty()) {
                    Spacer(Modifier.height(1.dp))
                    Text(
                        repo.description,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                    )
                }
                Spacer(Modifier.height(FmSpace.x1))
                Text(
                    RepoCatalog.metaLine(repo),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textTertiary, fontFeatureSettings = FmType.tabular),
                )
            }
        },
        trailing = local?.let { record -> { FmStatusBadge(if (record.isCloned) "Cloned" else "On device", c.accent) } },
        onTap = onTap,
    )
}

@Composable
private fun PairDesktopRow(onTap: () -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(
            "Desktop",
            modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4, bottom = FmSpace.x2),
        )
        FmListRow(
            title = "Pair your desktop",
            subtitle = "See the repositories on your Mac or PC",
            leading = { FmIcon("desktop_windows_outlined", size = 22.dp, tint = c.textSecondary) },
            onTap = onTap,
        )
    }
}

@Composable
private fun DesktopReposSection(
    title: String,
    count: Int,
    rows: List<DesktopRepo>,
    problem: String?,
    onOpen: (DesktopRepo) -> Unit,
) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(
            title,
            modifier = Modifier.padding(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4, bottom = FmSpace.x2),
            count = count,
        )
        if (count == 0 && problem != null) {
            Text(
                problem,
                modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
                style = Fm.text.bodySmall.copy(color = c.textTertiary),
            )
        } else if (count == 0) {
            FmSkeletonList(rows = 2, label = "Loading desktop repositories")
        }
        rows.forEachIndexed { index, row ->
            if (index > 0) FmRowDivider()
            DesktopRepoRow(row, selected = paneOpens(row.repository)) { onOpen(row) }
        }
        FmRowDivider(indent = 0.dp)
    }
}

@Composable
private fun DesktopRepoRow(repo: DesktopRepo, selected: Boolean = false, onTap: () -> Unit) {
    val c = Fm.colors
    FmListRow(
        modifier = Modifier.shellPaneSelected(selected),
        title = repo.slug,
        minHeight = 52.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        leading = {
            FmIcon(if (repo.isPrivate) "lock_outline" else "desktop_windows_outlined", size = 22.dp, tint = c.textSecondary)
        },
        titleContent = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    repo.slug,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall,
                )
                if (repo.runningAgents > 0) {
                    Spacer(Modifier.width(FmSpace.x2))
                    FmStatusBadge("${repo.runningAgents} running", c.success)
                }
            }
        },
        subtitle = repo.detail,
        onTap = onTap,
    )
}

@Composable
internal fun PresenceDot(online: Boolean) {
    val label = if (online) "Online" else "Offline"
    FmTooltip(label) {
        Box(
            Modifier
                .size(8.dp)
                .background(if (online) Fm.colors.success else FmColors.offline, CircleShape)
                .semantics { contentDescription = label },
        )
    }
}

@Composable
internal fun RepoMeta(item: RepoMetaItem) {
    val c = Fm.colors
    val row: @Composable () -> Unit = {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(
                item.icon,
                size = if (item.isLanguage) 9.dp else 14.dp,
                tint = if (item.isLanguage) FmColors.accentEdge else c.textTertiary,
            )
            Spacer(Modifier.width(FmSpace.x1))
            Text(item.text, style = Fm.text.bodySmall.copy(color = c.textTertiary, fontFeatureSettings = FmType.tabular))
        }
    }
    val tooltip = item.tooltip
    if (tooltip == null) row() else FmTooltip(tooltip) { row() }
}

@Composable
private fun paneOpens(repo: Repository): Boolean {
    val open = LocalShellPaneOpen.current as? RepoDetailScreen ?: return false
    return open.repo.owner.equals(repo.owner, ignoreCase = true) && open.repo.name.equals(repo.name, ignoreCase = true)
}
