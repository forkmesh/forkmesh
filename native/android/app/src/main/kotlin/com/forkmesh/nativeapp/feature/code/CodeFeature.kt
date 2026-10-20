package com.forkmesh.nativeapp.feature.code

import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.CurrentRepoMemory
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object CodeFeature {
    private val currentRepoState = MutableStateFlow<RepoRef?>(null)
    private val records = ConcurrentHashMap<String, Repository>()

    val currentRepo: StateFlow<RepoRef?> = currentRepoState

    fun install(app: App) {
        records.clear()
        currentRepoState.value = CurrentRepoMemory(Services.get<PrefStore>()).ref
    }

    fun tab(): Screen = ReposScreen()

    fun rememberCurrentRepo(repo: Repository) {
        if (repo.owner.isEmpty() || repo.name.isEmpty()) return
        records[RepoRef(repo.owner, repo.name).slug.lowercase()] = repo
        CurrentRepoMemory(Services.get<PrefStore>()).remember(repo.owner, repo.name)
        currentRepoState.value = RepoRef(repo.owner, repo.name)
    }

    fun follow(ref: RepoRef) {
        if (ref.owner.isEmpty() || ref.name.isEmpty() || currentRepoState.value == ref) return
        CurrentRepoMemory(Services.get<PrefStore>()).remember(ref.owner, ref.name)
        currentRepoState.value = ref
    }

    fun currentRepository(): Repository? {
        val ref = currentRepoState.value ?: CurrentRepoMemory(Services.get<PrefStore>()).ref ?: return null
        return records[ref.slug.lowercase()] ?: Repository(ref.owner, ref.name)
    }

    fun repoOf(screen: Screen): RepoRef? = (screen as? RepoDetailScreen)?.ref

    fun repoDetailScreen(
        repo: Repository,
        initialTab: RepoDetailTab = RepoDetailTab.Code,
        initialTarget: NotificationDeepLink? = null,
    ): Screen = RepoDetailScreen(repo, initialTab, initialTarget)

    fun repoFileScreen(repo: Repository, path: String, ref: String = ""): Screen = RepoFileScreen(repo, path, ref)

    fun actionsScreen(repo: Repository, commit: String = ""): Screen = com.forkmesh.nativeapp.feature.code.review.ActionsScreen(repo, commit)

    fun actionRunScreen(repo: Repository, runId: Int): Screen = com.forkmesh.nativeapp.feature.code.review.standaloneActionRunScreen(repo, runId)

    fun releasesScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.ReleasesScreen(repo)

    fun securityScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.SecurityScreen(repo)

    fun providerMirrorScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.ProviderMirrorScreen(repo)

    fun webhooksScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.WebhooksScreen(repo)

    fun mirrorNodesScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.MirrorNodesScreen(repo)

    fun pullReviewScreen(repo: Repository, number: Int): Screen = com.forkmesh.nativeapp.feature.code.review.PullReviewScreen(repo, number)

    fun mergeQueueScreen(repo: Repository? = null): Screen = com.forkmesh.nativeapp.feature.code.review.MergeQueueScreen(repo)

    fun issueBoardScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.IssueBoardScreen(repo)

    fun branchesScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.BranchesScreen(repo)

    fun conflictsScreen(repo: Repository, branch: String): Screen = com.forkmesh.nativeapp.feature.code.review.ConflictsScreen(repo, branch)

    fun codeMapScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.CodeMapScreen(repo)

    fun repoSettingsScreen(repo: Repository): Screen = com.forkmesh.nativeapp.feature.code.review.RepoSettingsScreen(repo)

    fun fileToolsScreen(repo: Repository, path: String, ref: String = ""): Screen =
        com.forkmesh.nativeapp.feature.code.review.FileToolsScreen(repo, path, ref)

    fun branchDiffScreen(repo: Repository, head: String, base: String = "", title: String = ""): Screen =
        BranchDiffScreen(repo, head, base, title)

    fun patchFilesScreen(title: String, patch: String): Screen = PatchFilesScreen(title, patch)

    fun quickEditScreen(repo: Repository, path: String, ref: String = ""): Screen =
        QuickEditScreen(repo, path, ref)

    suspend fun openBranchTarget(repo: Repository, branch: String, navigator: Navigator, host: Screen) {
        if (branch.isEmpty()) return
        val branches = withContext(Dispatchers.IO) { repoApi.branches(repo.owner, repo.name) }
        if (navigator.top !== host) return
        val pushed = branches.firstOrNull { it.name == branch } ?: return
        if (pushed.isDefault) return
        val main = branches.firstOrNull { it.isDefault }
        navigator.push(BranchDiffScreen(repo, head = pushed.name, base = main?.name ?: ""))
    }

    @Composable
    fun PatchFilesCard(patch: String) {
        PatchFilesSummary(patch)
    }

    suspend fun showMirrorImportSheet(messenger: FmMessenger): String? = messenger.showMirrorImportSheet()

    @Composable
    fun rememberMirrorFromProvider(status: FmStatus? = null): () -> Unit {
        val requireAccount = AccountFeature.rememberRequireAccount()
        val messenger = LocalFmMessenger.current
        val scope = rememberCoroutineScope()
        return remember(requireAccount, messenger, scope, status) {
            {
                scope.launch {
                    if (!requireAccount("import a repository")) return@launch
                    val notice = messenger.showMirrorImportSheet() ?: return@launch
                    status?.show(notice)
                }
            }
        }
    }
}
