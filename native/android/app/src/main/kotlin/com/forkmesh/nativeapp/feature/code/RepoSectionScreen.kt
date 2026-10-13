package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.feature.code.collab.CollaborationScreens
import com.forkmesh.nativeapp.feature.code.collab.IssueFilterState
import com.forkmesh.nativeapp.feature.code.review.ActionsScreen
import com.forkmesh.nativeapp.feature.code.review.MirrorNodesScreen
import com.forkmesh.nativeapp.feature.code.review.ReleasesScreen
import com.forkmesh.nativeapp.feature.composer.ComposerFeature
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.RepoMirrorSet
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.coroutines.cancellation.CancellationException

internal class RepoSectionScreen(
    val repo: Repository,
    val section: RepoHubSection,
    private val initialTarget: NotificationDeepLink? = null,
) : Screen() {
    private val tab: RepoDetailTab = section.tab ?: RepoDetailTab.Code
    private var openedInitialTarget = false

    private val codeTab by lazy { CodeTabState(repo).also { it.ensureLoaded() } }
    private val commitsTab by lazy { CommitsTabState(repo).also { it.ensureLoaded() } }
    private val mirrors by lazy { AsyncValue<RepoMirrorSet>().apply { reload { repoApi.mirrorSet(repo.owner, repo.name) } } }
    private val releases by lazy { AsyncValue<List<RepoRelease>>().apply { reload { repoApi.releases(repo.owner, repo.name) } } }
    private val worktrees by lazy {
        AsyncValue<List<RepoBranch>>().apply {
            reload { RepoExplorer.worktreeBranches(repoApi.branches(repo.owner, repo.name)) }
        }
    }
    private val actionsTab by lazy { ActionsTabState(repo).also { it.load() } }
    private val agentsTab by lazy { AgentsTabState(repo).also { it.load() } }
    private val issueFilters = IssueFilterState()

    override val title: String get() = "${RepoHubSection.of(tab).title} · ${repo.name}"

    private suspend fun openInitialTarget(navigator: Navigator) {
        if (openedInitialTarget) return
        openedInitialTarget = true
        val target = initialTarget ?: return
        try {
            when (target.initialTab) {
                RepoDetailTab.Issues, RepoDetailTab.Pulls, RepoDetailTab.Discussions -> {
                    CollaborationScreens.openInitialTarget(repo, target, navigator, host = this)
                }
                RepoDetailTab.Commits -> {
                    if (target.isBranchCommits) {
                        CodeFeature.openBranchTarget(repo, target.branch, navigator, host = this)
                        return
                    }
                    val hash = target.reference.trim()
                    if (hash.isEmpty()) return
                    if (navigator.top !== this) return
                    navigator.push(CommitDetailScreen(repo, linkedMapOf("hash" to hash, "message" to "Commit $hash")))
                }
                else -> return
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (_: Exception) {
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        ComposerFeature.FollowRepository(repo.fullName)
        val c = Fm.colors
        val collaboration = CollaborationScreens.rememberActions(repo, navigator)
        LaunchedEffect(Unit) { openInitialTarget(navigator) }
        Box(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            Column(Modifier.fillMaxSize()) {
                CodeAppBar(
                    title = title,
                    navigator = navigator,
                    titleStyle = Fm.text.titleSmall.copy(color = c.textPrimary),
                    actions = {
                        val tool: Triple<String, String, () -> Screen>? = when (tab) {
                            RepoDetailTab.Actions -> Triple("tune", "Workflows and runs") { ActionsScreen(repo) }
                            RepoDetailTab.Releases -> Triple("inventory_2_outlined", "Releases and artifacts") { ReleasesScreen(repo) }
                            RepoDetailTab.Mirrors -> Triple("dns_outlined", "Mirror nodes") { MirrorNodesScreen(repo) }
                            else -> null
                        }
                        if (tool != null) {
                            FmIconButton(
                                tool.first,
                                tool.second,
                                onPressed = { navigator.push(tool.third()) },
                                color = c.textPrimary,
                                iconSize = 24.dp,
                            )
                        }
                    },
                )
                FmInlineStatus(
                    collaboration.status,
                    Modifier
                        .background(c.bgRaised)
                        .padding(horizontal = FmSpace.x4),
                )
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                ) {
                    Page(navigator)
                }
            }
            val fab: Pair<String, () -> Unit>? = when (tab) {
                RepoDetailTab.Issues -> "New issue" to collaboration::newIssue
                RepoDetailTab.Pulls -> "New PR" to collaboration::newPullRequest
                RepoDetailTab.Discussions -> "New discussion" to collaboration::newDiscussion
                else -> null
            }
            if (fab != null) {
                FmExtendedFloatingActionButton(
                    text = fab.first,
                    onClick = fab.second,
                    icon = "add",
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .windowInsetsPadding(WindowInsets.navigationBars)
                        .padding(FmSpace.x4),
                )
            }
        }
    }

    @Composable
    private fun Page(navigator: Navigator) {
        when (tab) {
            RepoDetailTab.Code -> CodeTab(codeTab, navigator)
            RepoDetailTab.Issues -> CollaborationScreens.IssuesTab(repo, navigator, filters = issueFilters)
            RepoDetailTab.Pulls -> CollaborationScreens.PullsTab(repo, navigator)
            RepoDetailTab.Discussions -> CollaborationScreens.DiscussionsTab(repo, navigator)
            RepoDetailTab.Commits -> CommitsTab(commitsTab, navigator)
            RepoDetailTab.Mirrors -> MirrorsTab(mirrors)
            RepoDetailTab.Releases -> ReleasesTab(repo, releases)
            RepoDetailTab.About -> AboutTab(repo)
            RepoDetailTab.Actions -> ActionsTab(actionsTab, navigator)
            RepoDetailTab.Agents -> AgentsTab(agentsTab, navigator)
            RepoDetailTab.Worktrees -> WorktreesTab(repo, worktrees, navigator)
        }
    }
}
