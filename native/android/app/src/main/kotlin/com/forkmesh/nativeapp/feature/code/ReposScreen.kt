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
        Column(Modifier.fillMaxSize()) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(FmSpace.x4),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    SearchField(Modifier.weight(1f))
                    Spacer(Modifier.width(FmSpace.x2))
                    RefreshButton()
                }
                Spacer(Modifier.height(FmSpace.x3))
                CatalogControls()
            }
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .background(Fm.colors.listSurface),
            ) {
                Catalog(navigator)
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
            modifier = modifier.height(48.dp),
            singleLine = true,
            textStyle = Fm.text.bodyLarge.copy(color = c.textPrimary),
            cursorBrush = SolidColor(c.accent),
            interactionSource = interaction,
            decorationBox = { inner ->
                Row(
                    Modifier
                        .fillMaxSize()
                        .background(c.bgRaised, shape)
                        .then(if (focused) Modifier.border(1.2.dp, c.accent, shape) else Modifier),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.width(48.dp), contentAlignment = Alignment.Center) {
                        FmIcon("search", size = 18.dp, tint = c.textTertiary)
                    }
                    Box(
                        Modifier
                            .weight(1f)
                            .padding(end = FmSpace.x4),
                    ) {
                        if (searchText.isEmpty()) {
                            Text(
                                "Find a repository...",
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
                    .size(48.dp)
                    .clip(RoundedCornerShape(FmRadius.lg))
                    .background(c.bgRaised)
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
                FmIcon("refresh", size = 24.dp, tint = c.textSecondary)
            }
        }
    }

    @Composable
    private fun CatalogControls() {
        val c = Fm.colors
        val haptics = FmHaptics.current
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmEdgeFadeScroll(
                Modifier
                    .weight(1f)
                    .height(36.dp),
            ) { state ->
                Row(
                    Modifier
                        .fillMaxHeight()
                        .horizontalScroll(state),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    for (option in RepoFilter.entries) {
                        val selected = option == filter
                        val shape = RoundedCornerShape(FmRadius.md)
                        Box(
                            Modifier
                                .height(32.dp)
                                .clip(shape)
                                .background(if (selected) c.accentSubtle else c.bgRaised)
                                .border(1.dp, if (selected) c.accent else c.border, shape)
                                .selectable(selected = selected, role = Role.RadioButton) {
                                    haptics.selection()
                                    filter = option
                                }
                                .padding(horizontal = 10.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                option.label,
                                style = Fm.text.labelMedium.copy(color = if (selected) c.accent else c.textSecondary),
                            )
                        }
                    }
                }
            }
            Spacer(Modifier.width(FmSpace.x2))
            SortButton()
        }
    }

    @Composable
    private fun SortButton() {
        val c = Fm.colors
        var open by remember { mutableStateOf(false) }
        val shape = RoundedCornerShape(FmRadius.full)
        Box {
            FmTooltip("Sort repositories") {
                Row(
                    Modifier
                        .height(36.dp)
                        .clip(shape)
                        .background(c.bgRaised)
                        .border(1.dp, c.border, shape)
                        .clickable { open = true }
                        .semantics {
                            contentDescription = "Sort repositories, ${sort.buttonLabel}"
                            role = Role.Button
                        }
                        .width(36.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                ) {
                    FmIcon("sort", size = 16.dp, tint = c.textSecondary)
                }
            }
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                for (option in RepoSort.entries) {
                    FmDropdownMenuItem(
                        text = option.menuLabel,
                        onClick = {
                            open = false
                            sort = option
                        },
                        modifier = if (option == sort) Modifier.background(c.textPrimary.copy(alpha = .08f)) else Modifier,
                    )
                }
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun Catalog(navigator: Navigator) {
        val waiting = repos.waiting
        val error = repos.error
        val visible = if (waiting || error != null) {
            emptyList()
        } else {
            RepoCatalog.filterAndSort(repos.data.orEmpty(), query, filter, sort)
        }
        val networkState = when {
            waiting -> "loading"
            error != null -> "error"
            else -> RepoCatalog.listStateKey(filter, sort, query, visible.size)
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
                            }
                            LocalReposFeature.LocalReposSection(navigator)
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
                                val active = RepoCatalog.hasActiveControls(query, filter)
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

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RepoTile(repo: Repository, local: LocalRepoRecord?, selected: Boolean = false, onTap: () -> Unit) {
    val c = Fm.colors
    val online = RepoCatalog.isOnline(repo)
    FmListRow(
        modifier = Modifier.shellPaneSelected(selected),
        title = repo.fullName,
        minHeight = 64.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        leading = {
            FmIconTile(if (repo.isPrivate) "lock_outline" else "book_outlined", tint = c.accent, size = 40.dp)
        },
        titleContent = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    repo.fullName,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 2,
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
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                    )
                }
                Spacer(Modifier.height(FmSpace.x2))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                    itemVerticalAlignment = Alignment.CenterVertically,
                ) {
                    for (item in RepoCatalog.metaItems(repo)) RepoMeta(item)
                }
            }
        },
        trailing = local?.let { record -> { FmStatusBadge(if (record.isCloned) "Cloned" else "On device", c.accent) } },
        onTap = onTap,
    )
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
            icon = "desktop_windows_outlined",
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
            FmIconTile(if (repo.isPrivate) "lock_outline" else "desktop_windows_outlined", tint = c.accent, size = 36.dp)
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
