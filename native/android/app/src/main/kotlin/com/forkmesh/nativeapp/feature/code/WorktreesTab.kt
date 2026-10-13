package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
internal fun WorktreesTab(repo: Repository, value: AsyncValue<List<RepoBranch>>, navigator: Navigator) {
    val c = Fm.colors
    AsyncList(value, empty = "No worktrees found for this repository.") { branch ->
        MobileCard(
            icon = "folder_special_outlined",
            iconColor = c.accent,
            title = branch.name,
            subtitle = branch.worktreePath,
            onTap = { navigator.push(WorktreeDetailScreen(repo, branch)) },
            chips = buildList {
                if (branch.isDefault) add("default")
                if (branch.sha.isNotEmpty()) add(RepoExplorer.worktreeShortSha(branch.sha))
            },
        )
    }
}

internal class WorktreeDetailScreen(val repo: Repository, val branch: RepoBranch) : Screen() {
    override val title: String get() = "Worktree: ${branch.name}"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        DetailScaffold(title = title, navigator = navigator) {
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 20.dp, top = 20.dp, end = 20.dp, bottom = 20.dp + navigationBarBottom()),
            ) {
                item {
                    InfoCard(branch.name) {
                        ChipWrap {
                            RepoChip("account_tree_outlined", branch.name)
                            if (branch.isDefault) RepoChip("flag_outlined", "default branch")
                            if (branch.sha.isNotEmpty()) RepoChip("commit_outlined", RepoExplorer.worktreeShortSha(branch.sha))
                            RepoChip("folder_outlined", repo.fullName)
                        }
                        Spacer(Modifier.height(16.dp))
                        Kv("Branch", branch.name)
                        Kv("Worktree path", branch.worktreePath)
                        if (branch.sha.isNotEmpty()) Kv("Commit", branch.sha)
                        Spacer(Modifier.height(16.dp))
                        Text(
                            "This worktree is managed by the desktop node. Mobile can view worktree state; operations like " +
                                "merge, update, and delete are desktop-controlled.",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary, lineHeight = FmType.meta * 1.35f),
                        )
                    }
                }
                item { Spacer(Modifier.height(12.dp)) }
                item {
                    InfoCard("Worktree actions") {
                        Text(
                            "Worktree operations are controlled from the ForkMesh desktop app. Use it to merge, update from " +
                                "main, or delete this worktree.",
                        )
                        Spacer(Modifier.height(12.dp))
                        FmOutlinedButton(
                            "Copy path",
                            onClick = { copy("Worktree path", branch.worktreePath) },
                            icon = "copy_all_outlined",
                        )
                        FmInlineStatus(copyStatus, Modifier.padding(top = 8.dp))
                    }
                }
            }
        }
    }
}
