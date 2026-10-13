package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.RepoDiscussion
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

interface CollaborationActions {
    val status: FmStatus

    fun newIssue()

    fun newPullRequest()

    fun newDiscussion()
}

object CollaborationScreens {
    @Composable
    fun IssuesTab(repo: Repository, navigator: Navigator, filters: IssueFilterState? = null) {
        val holder = filters ?: remember(repo, navigator) { IssueFilterState.forHost(navigator.top, repo) }
        IssuesTabContent(repo, navigator, holder)
    }

    @Composable
    fun PullsTab(repo: Repository, navigator: Navigator) {
        PullsTabContent(repo, navigator)
    }

    @Composable
    fun DiscussionsTab(repo: Repository, navigator: Navigator) {
        DiscussionsTabContent(repo, navigator)
    }

    fun issueDetailScreen(repo: Repository, number: Int, issue: Issue? = null): Screen =
        IssueDetailScreen(repo, number, issue?.takeIf { it.number == number })

    fun pullDetailScreen(repo: Repository, number: Int, pull: PublishedPull? = null): Screen =
        PullDetailScreen(repo, number, pull?.takeIf { it.number == number })

    fun newIssueScreen(repo: Repository): Screen = NewIssueScreen.forRoute(repo)

    fun discussionDetailScreen(repo: Repository, number: Int, discussion: RepoDiscussion? = null): Screen =
        DiscussionDetailScreen(repo, number, discussion?.takeIf { it.number == number })

    @Composable
    fun rememberActions(repo: Repository, navigator: Navigator): CollaborationActions {
        val env = rememberCollabEnv(navigator)
        return remember(repo, env) {
            object : CollaborationActions {
                override val status: FmStatus get() = env.status

                override fun newIssue() {
                    env.launch { openNewIssueComposer(repo) }
                }

                override fun newPullRequest() {
                    env.launch { showNewPullDialog(repo) }
                }

                override fun newDiscussion() {
                    env.launch { showNewDiscussionDialog(repo) }
                }
            }
        }
    }

    suspend fun openInitialTarget(
        repo: Repository,
        target: NotificationDeepLink,
        navigator: Navigator,
        host: Screen = navigator.top,
    ): Boolean {
        if (target.number <= 0) return false
        val api = Services.get<ApiService>() ?: return false
        return try {
            val screen: Screen = when (target.initialTab) {
                RepoDetailTab.Issues -> withContext(Dispatchers.IO) { api.publishedIssues(repo.owner, repo.name) }
                    .firstOrNull { it.number == target.number }
                    ?.let { IssueDetailScreen(repo, it.number, it) }
                RepoDetailTab.Pulls -> PullDetailScreen(repo, target.number, null)
                RepoDetailTab.Discussions -> withContext(Dispatchers.IO) { api.publishedDiscussions(repo.owner, repo.name) }
                    .firstOrNull { it.number == target.number }
                    ?.let { DiscussionDetailScreen(repo, it.number, it) }
                else -> null
            } ?: return false
            if (navigator.top !== host) return false
            navigator.push(screen)
            true
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            false
        }
    }
}
