package com.forkmesh.nativeapp.feature.code

import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PackageGate
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.code.ApiRepoOverviewClient
import com.forkmesh.nativeapp.code.RepoCatalog
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.code.RepoMetaItem
import com.forkmesh.nativeapp.code.RepoOverviewModel
import com.forkmesh.nativeapp.code.RepoOverviewTile
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.collab.CollaborationScreens
import com.forkmesh.nativeapp.feature.code.review.BranchesScreen
import com.forkmesh.nativeapp.feature.code.review.CodeMapScreen
import com.forkmesh.nativeapp.feature.code.review.IssueBoardScreen
import com.forkmesh.nativeapp.feature.code.review.CodeServices
import com.forkmesh.nativeapp.feature.code.review.MergeQueueScreen
import com.forkmesh.nativeapp.feature.code.review.ProviderMirrorScreen
import com.forkmesh.nativeapp.feature.code.review.RepoArchiveScreen
import com.forkmesh.nativeapp.feature.code.review.RepoSettingsScreen
import com.forkmesh.nativeapp.feature.code.review.ReprioritizeScreen
import com.forkmesh.nativeapp.feature.code.review.SecurityScreen
import com.forkmesh.nativeapp.feature.code.review.WebhooksScreen
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.feature.insights.InsightsFeature
import com.forkmesh.nativeapp.feature.localrepos.LocalCopy
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.planning.PlanningFeature
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class RepoDetailScreen(
    val repo: Repository,
    private val initialTab: RepoDetailTab = RepoDetailTab.Code,
    private val initialTarget: NotificationDeepLink? = null,
) : Screen() {
    private val overview = RepoOverviewModel(repo, Services.get<ApiService>()?.let(::ApiRepoOverviewClient))
    private val scope = MainScope()
    private var shown by mutableStateOf(repo)
    private var tiles by mutableStateOf(overview.tiles(hiddenSections()))
    private var loading by mutableStateOf(false)
    private var arrived = false
    private var counted = false

    val ref: RepoRef get() = RepoRef(repo.owner, repo.name)

    override val title: String get() = repo.fullName

    override fun onDisposed() {
        scope.cancel()
    }

    private fun loadCounts(force: Boolean = false) {
        if (loading || (counted && !force)) return
        loading = true
        scope.launch {
            try {
                if (shown.updatedMs == 0L) {
                    val catalog = withContext(Dispatchers.IO) { runCatching { repoApi.repositories() }.getOrNull() }
                    if (catalog != null && overview.adopt(catalog)) shown = overview.repo
                }
                tiles = overview.tiles(hiddenSections())
                RepoHubSection.counted
                    .filter { it !in hiddenSections() }
                    .map { section -> async(Dispatchers.IO) { section to overview.count(section) } }
                    .awaitAll()
                    .forEach { (section, count) -> overview.record(section, count) }
                tiles = overview.tiles(hiddenSections())
                counted = true
            } finally {
                loading = false
            }
        }
    }

    private fun hiddenSections(): Set<RepoHubSection> = PackageGateFeature.surfaces.value.repoSections

    private fun arrive(navigator: Navigator) {
        if (arrived) return
        arrived = true
        val tab = if (initialTab != RepoDetailTab.Code) initialTab else initialTarget?.initialTab ?: RepoDetailTab.Code
        if (tab == RepoDetailTab.Code || navigator.top !== this) return
        navigator.push(RepoSectionScreen(shown, RepoHubSection.of(tab), initialTarget))
    }

    private fun open(section: RepoHubSection, navigator: Navigator) {
        val repo = shown
        val screen: Screen = when (section) {
            RepoHubSection.Branches -> BranchesScreen(repo)
            RepoHubSection.Board -> IssueBoardScreen(repo)
            RepoHubSection.Backlog -> ReprioritizeScreen(repo)
            RepoHubSection.MergeQueue -> MergeQueueScreen(repo)
            RepoHubSection.CodeMap -> CodeMapScreen(repo)
            RepoHubSection.Insights -> InsightsFeature.repoInsightsScreen(repo)
            RepoHubSection.Ember -> NavigationFeature.registeredScreen(Route.Ember(RepoRef(repo.owner, repo.name)))
            RepoHubSection.Strata -> NavigationFeature.registeredScreen(Route.Strata(RepoRef(repo.owner, repo.name)))
            RepoHubSection.Synapse -> NavigationFeature.registeredScreen(Route.Synapse(RepoRef(repo.owner, repo.name)))
            RepoHubSection.Security -> SecurityScreen(repo)
            RepoHubSection.Webhooks -> WebhooksScreen(repo)
            RepoHubSection.Projects -> PlanningFeature.projectsScreen(RepoRef(repo.owner, repo.name))
            RepoHubSection.Milestones -> PlanningFeature.milestonesScreen(RepoRef(repo.owner, repo.name))
            RepoHubSection.Archive -> RepoArchiveScreen(repo)
            RepoHubSection.Settings -> RepoSettingsScreen(repo)
            else -> RepoSectionScreen(repo, section)
        }
        navigator.push(screen)
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        ComposerFeature.FollowRepository(repo.fullName)
        val c = Fm.colors
        val collaboration = CollaborationScreens.rememberActions(shown, navigator)
        val cloneToDevice = LocalReposFeature.rememberCloneToDevice(navigator, collaboration.status)
        val localCopy = LocalReposFeature.rememberLocalCopy(navigator, shown, collaboration.status)
        val surfaces by PackageGateFeature.surfaces.collectAsState()
        LaunchedEffect(surfaces) { tiles = overview.tiles(surfaces.repoSections) }
        LaunchedEffect(Unit) {
            CodeFeature.rememberCurrentRepo(shown)
            arrive(navigator)
            loadCounts()
        }
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(
                title = shown.name,
                navigator = navigator,
                titleStyle = Fm.text.titleSmall.copy(color = c.textPrimary),
                actions = {
                    if (CodeServices.account.equals(shown.owner, ignoreCase = true)) {
                        FmIconButton(
                            "sync_alt",
                            "Mirror to GitHub or GitLab",
                            onPressed = { navigator.push(ProviderMirrorScreen(shown)) },
                            color = c.textPrimary,
                            iconSize = 24.dp,
                        )
                    }
                    FmIconButton(
                        "refresh",
                        "Refresh counts",
                        onPressed = { loadCounts(force = true) },
                        color = c.textPrimary,
                        iconSize = 24.dp,
                    )
                },
            )
            FmPullToRefreshBox(
                isRefreshing = false,
                onRefresh = { loadCounts(force = true) },
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                Column(
                    Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(FmSpace.x4),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
                ) {
                    RepoHubHeader(shown)
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        if (surfaces.agents) {
                            RepoHubAction("Agent", "auto_awesome") {
                                NavigationFeature.compose(ComposeTarget(repo = RepoRef(shown.owner, shown.name)))
                            }
                        }
                        if (surfaces.shows(PackageGate.ISSUES)) RepoHubAction("Issue", "add_circle_outline") { collaboration.newIssue() }
                        RepoHubAction("Files", "folder_outlined") { open(RepoHubSection.Code, navigator) }
                        if (localCopy != null) {
                            RepoHubLocalCopy(localCopy)
                        } else if (cloneToDevice != null) {
                            RepoHubAction("Clone", "download_for_offline_outlined") { cloneToDevice(shown) }
                        }
                    }
                    FmInlineStatus(collaboration.status)
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        for (row in tiles.chunked(3)) {
                            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                                for (tile in row) RepoHubTile(tile) { open(tile.section, navigator) }
                                repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
                            }
                        }
                    }
                    RepoReadmeRow { navigator.push(RepoFileScreen(shown, "README.md")) }
                    Spacer(Modifier.height(navigationBarBottom()))
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RepoHubHeader(repo: Repository) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.Top) {
        FmIconTile(if (repo.isPrivate) "lock_outline" else "book_outlined", tint = c.accent, size = 40.dp)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    repo.fullName,
                    modifier = Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleMedium.copy(color = c.textPrimary),
                )
                Spacer(Modifier.width(FmSpace.x2))
                PresenceDot(RepoCatalog.isOnline(repo))
            }
            if (repo.description.isNotEmpty()) {
                Text(
                    repo.description,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
            Spacer(Modifier.height(FmSpace.x1))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                itemVerticalAlignment = Alignment.CenterVertically,
            ) {
                if (repo.defaultBranch.isNotEmpty()) RepoMeta(RepoMetaItem("call_split", repo.defaultBranch))
                for (item in RepoCatalog.metaItems(repo).filter { it.icon != "update" }) RepoMeta(item)
            }
        }
    }
}

@Composable
private fun RowScope.RepoHubLocalCopy(copy: LocalCopy) {
    var menu by remember { mutableStateOf(false) }
    Box(Modifier.weight(1f)) {
        Row(Modifier.fillMaxWidth()) { RepoHubAction("On device", "cloud_done_outlined") { menu = true } }
        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            FmDropdownMenuItem(
                text = "Open on this device",
                icon = "folder_open_outlined",
                onClick = {
                    menu = false
                    copy.open()
                },
            )
            FmDropdownMenuItem(
                text = "Sync from ForkMesh",
                icon = "sync",
                onClick = {
                    menu = false
                    copy.sync()
                },
            )
        }
    }
}

@Composable
private fun RowScope.RepoHubAction(title: String, icon: String, onClick: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Row(
        Modifier
            .weight(1f)
            .heightIn(min = 44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.accentSubtle)
            .clickable {
                haptics.tap()
                onClick()
            }
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = title
            }
            .padding(horizontal = FmSpace.x1),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 16.dp, tint = c.accent)
        Spacer(Modifier.width(6.dp))
        Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelMedium.copy(color = c.accent))
    }
}

@Composable
private fun RowScope.RepoHubTile(tile: RepoOverviewTile, onClick: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Column(
        Modifier
            .weight(1f)
            .heightIn(min = 60.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgRaised)
            .clickable {
                haptics.selection()
                onClick()
            }
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = tile.accessibilityLabel
            }
            .padding(vertical = FmSpace.x2, horizontal = FmSpace.x1),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box {
            FmIcon(tile.icon, size = 20.dp, tint = c.accent)
            val text = tile.countText
            if (text != null) {
                val active = (tile.count ?: 0) > 0
                Text(
                    text,
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .offset(x = 16.dp, y = (-6).dp)
                        .background(if (active) c.accent else c.bgBase, RoundedCornerShape(FmRadius.full))
                        .widthIn(min = 18.dp)
                        .padding(horizontal = 5.dp),
                    maxLines = 1,
                    style = Fm.text.labelSmall.copy(
                        color = if (active) Color.White else c.textSecondary,
                        fontFeatureSettings = FmType.tabular,
                    ),
                )
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(
            tile.title,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelSmall.copy(color = c.textPrimary),
        )
    }
}

@Composable
private fun RepoReadmeRow(onClick: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .height(44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgRaised)
            .clickable(onClick = onClick)
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = "Open README.md"
            }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("article_outlined", size = 18.dp, tint = c.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text("README.md", Modifier.weight(1f), style = Fm.text.labelLarge.copy(color = c.textPrimary))
        FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
    }
}
