package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.CommitFields
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.code.RepoHistoryRefSupport
import com.forkmesh.nativeapp.code.RepoHistoryText
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmBottomBar
import com.forkmesh.nativeapp.ui.kit.FmBranchLabel
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

internal enum class GitView { History, Branches }

@Stable
internal class CommitsTabState(val repo: Repository) {
    var view by mutableStateOf(GitView.History)
    val commits = AsyncValue<List<Map<String, Any?>>>()
    val branches = AsyncValue<List<RepoBranch>>()

    fun ensureLoaded() {
        if (!commits.started) loadCommits()
        if (!branches.started) loadBranches()
    }

    private fun loadCommits() {
        commits.reload { repoApi.commits(repo.owner, repo.name) }
    }

    private fun loadBranches() {
        branches.reload { repoApi.branches(repo.owner, repo.name) }
    }

    fun refresh() {
        repoApi.clearRepoCache(repo.owner, repo.name)
        loadCommits()
        loadBranches()
    }
}

@Composable
internal fun CommitsTab(state: CommitsTabState, navigator: Navigator) {
    state.commits.Bind()
    state.branches.Bind()
    val c = Fm.colors
    val copyStatus = rememberFmStatus()
    val copy = rememberCopyText(copyStatus)
    val repo = state.repo
    val defaultBranch = repo.defaultBranch.ifEmpty { "default branch" }
    val openCommit: (Map<String, Any?>) -> Unit = { navigator.push(CommitDetailScreen(repo, it)) }
    Column(
        Modifier
            .fillMaxSize()
            .background(c.listSurface),
    ) {
        FmSegmentTabBar(
            tabs = listOf(
                FmSegmentTab("History", state.commits.data?.size),
                FmSegmentTab("Branches", state.branches.data?.size),
            ),
            index = state.view.ordinal,
            onChanged = { state.view = GitView.entries[it] },
            trailing = {
                FmIconButton("refresh_rounded", "Refresh history", onPressed = state::refresh, color = c.textSecondary)
            },
        )
        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
        ) {
            when (state.view) {
                GitView.History -> HistoryView(state, openCommit)
                GitView.Branches -> BranchesView(state, openCommit, openBranch = { branch ->
                    if (RepoExplorer.isCurrentBranch(branch, repo.defaultBranch)) state.view = GitView.History else navigator.push(BranchHistoryScreen(repo, branch.name))
                })
            }
        }
        FmInlineStatus(copyStatus, Modifier.padding(horizontal = FmSpace.x4))
        FmBottomBar(
            leading = { FmBranchLabel(defaultBranch) },
            actions = {
                FmToneButton(
                    "Clone URL",
                    onPressed = { copy("Clone URL", repoApi.cloneUri(repo.owner, repo.name)) },
                    icon = "link_rounded",
                    tone = FmTone.Accent,
                    tooltip = "Copy clone URL",
                )
            },
        )
    }
}

@Composable
private fun HistoryView(state: CommitsTabState, openCommit: (Map<String, Any?>) -> Unit) {
    val value = state.commits
    val commits = value.data
    val error = value.error
    when {
        value.waiting || !value.started -> PaddedCardList(bottomInset = false) { LoadingCard("Loading commits...") }
        error != null -> PaddedCardList(bottomInset = false) {
            ErrorCard("Could not load commits: $error", onRetry = state::refresh)
        }
        commits.isNullOrEmpty() -> PaddedCardList(bottomInset = false) { EmptyCard("No commits.") }
        else -> LazyColumn(Modifier.fillMaxSize()) {
            itemsIndexed(commits) { index, commit ->
                CommitTimelineRow(
                    commit = commit,
                    isHead = index == 0,
                    isFirst = index == 0,
                    isLast = index == commits.lastIndex,
                    onTap = { openCommit(commit) },
                )
            }
            item { Spacer(Modifier.height(FmSpace.x4)) }
        }
    }
}

@Composable
private fun BranchesView(state: CommitsTabState, openCommit: (Map<String, Any?>) -> Unit, openBranch: (RepoBranch) -> Unit) {
    val c = Fm.colors
    val value = state.branches
    val branches = value.data
    val error = value.error
    val defaultBranch = state.repo.defaultBranch
    when {
        value.waiting || !value.started -> PaddedCardList(bottomInset = false) { LoadingCard("Loading branches...") }
        error != null -> PaddedCardList(bottomInset = false) {
            ErrorCard("Could not load branches: $error", onRetry = state::refresh)
        }
        branches.isNullOrEmpty() -> PaddedCardList(bottomInset = false) {
            EmptyCard("No branches reported for this repository.")
        }
        else -> LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x4)) {
            dividedRows(RepoExplorer.sortBranches(branches, defaultBranch)) { branch ->
                val current = RepoExplorer.isCurrentBranch(branch, defaultBranch)
                FmListRow(
                    title = branch.name,
                    titleColor = if (current) c.success else null,
                    subtitle = RepoExplorer.branchRowSubtitle(branch),
                    leading = { FmIconTile("account_tree_outlined", tint = if (current) c.success else null) },
                    trailing = if (current) ({ FmStatusBadge("Default", c.success) }) else null,
                    showChevron = branch.sha.isNotEmpty(),
                    onTap = if (branch.sha.isEmpty()) null else ({ openBranch(branch) }),
                )
            }
        }
    }
}

@Composable
private fun CommitTimelineRow(commit: Map<String, Any?>, isHead: Boolean, isFirst: Boolean, isLast: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    val subject = CommitFields.subject(commit)
    val author = CommitFields.author(commit)
    val short = CommitFields.short(commit)
    val time = CommitFields.relativeTime(commit)
    Row(
        Modifier
            .fillMaxWidth()
            .height(82.dp)
            .clickable(onClick = onTap),
    ) {
        Box(
            Modifier
                .width(54.dp)
                .fillMaxHeight(),
        ) {
            CommitRailNode(active = isHead, isFirst = isFirst, isLast = isLast)
        }
        Column(
            Modifier
                .weight(1f)
                .fillMaxHeight()
                .padding(start = FmSpace.x1, top = FmSpace.x3, end = FmSpace.x2, bottom = FmSpace.x3),
            verticalArrangement = Arrangement.Center,
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (isHead) {
                    HeadBadge()
                    Spacer(Modifier.width(FmSpace.x2))
                }
                Text(
                    subject,
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall.copy(color = c.textPrimary, lineHeight = FmType.body * 1.15f),
                )
                if (time.isNotEmpty()) {
                    Spacer(Modifier.width(FmSpace.x1))
                    Text(time, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textTertiary))
                }
            }
            Spacer(Modifier.height(FmSpace.x2))
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (short.isNotEmpty()) {
                    HashBadge(short)
                    Spacer(Modifier.width(FmSpace.x2))
                }
                Text(
                    author.ifEmpty { "Unknown author" },
                    modifier = Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textSecondary),
                )
            }
        }
        Box(
            Modifier
                .fillMaxHeight()
                .padding(end = FmSpace.x3),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("chevron_right", size = 20.dp, tint = c.textTertiary)
        }
    }
}

@Composable
private fun CommitRailNode(active: Boolean, isFirst: Boolean, isLast: Boolean) {
    val c = Fm.colors
    val lineColor = if (c.dark) Color(0xFF4A4A4D) else Color(0xFFC6C6C8)
    val nodeColor = if (active) c.success else lineColor
    val surface = c.listSurface
    Canvas(Modifier.fillMaxSize()) {
        val centerX = size.width / 2
        val top = if (isFirst) 41.dp.toPx() else 0f
        val bottom = size.height - if (isLast) 41.dp.toPx() else 0f
        val line = 2.dp.toPx()
        if (bottom > top) drawRect(lineColor, topLeft = Offset(centerX - line / 2, top), size = Size(line, bottom - top))
        val radius = 9.dp.toPx()
        val center = Offset(centerX, size.height / 2)
        if (active) {
            drawCircle(nodeColor, radius, center)
        } else {
            drawCircle(surface, radius, center)
            drawCircle(nodeColor, radius - line / 2, center, style = Stroke(line))
        }
    }
}

@Composable
private fun HeadBadge() {
    val c = Fm.colors
    Text(
        "HEAD",
        modifier = Modifier
            .background(c.success.copy(alpha = .18f), RoundedCornerShape(FmRadius.sm))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(
            fontSize = FmType.micro,
            fontWeight = FmType.emphasis,
            color = c.success,
            lineHeight = FmType.micro,
        ),
    )
}

@Composable
private fun HashBadge(hash: String) {
    val blue = Color(0xFF58A6FF)
    Text(
        hash,
        modifier = Modifier
            .background(blue.copy(alpha = if (Fm.colors.dark) .18f else .12f), RoundedCornerShape(FmRadius.sm))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(
            fontWeight = FmType.emphasis,
            color = blue,
            fontFamily = FmType.mono,
            lineHeight = FmType.meta,
        ),
    )
}

internal class BranchHistoryScreen(private val repo: Repository, private val ref: String) : Screen() {
    private val commits = AsyncValue<List<Map<String, Any?>>>()
    private var support by mutableStateOf<RepoHistoryRefSupport?>(null)

    override val title: String get() = RepoHistoryText.title(ref)

    private fun load() {
        commits.reload {
            val branches = runCatching { repoApi.branches(repo.owner, repo.name) }.getOrDefault(emptyList())
            val read = repoApi.history(repo.owner, repo.name, ref)
            support = RepoHistoryRefSupport.detect(ref, read, branches, repo.defaultBranch)
            read.commits
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        commits.Bind()
        LaunchedEffect(Unit) { if (!commits.started) load() }
        val c = Fm.colors
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh_rounded", "Refresh history", onPressed = { load() }, color = c.textSecondary)
        }) {
            val list = commits.data
            val error = commits.error
            Column(Modifier.fillMaxSize().background(c.listSurface)) {
                RepoHistoryText.notice(ref, support, repo.defaultBranch)?.let { notice ->
                    Text(notice, Modifier.fillMaxWidth().padding(FmSpace.x3), style = Fm.text.bodySmall.copy(color = c.warning))
                }
                when {
                    commits.waiting || !commits.started -> PaddedCardList(bottomInset = false) { LoadingCard("Loading commits...") }
                    error != null -> PaddedCardList(bottomInset = false) { ErrorCard("Could not load commits: $error", onRetry = { load() }) }
                    list.isNullOrEmpty() -> PaddedCardList(bottomInset = false) { EmptyCard("No commits.") }
                    else -> LazyColumn(Modifier.fillMaxSize()) {
                        itemsIndexed(list) { index, commit ->
                            CommitTimelineRow(
                                commit = commit,
                                isHead = index == 0,
                                isFirst = index == 0,
                                isLast = index == list.lastIndex,
                                onTap = { navigator.push(CommitDetailScreen(repo, commit)) },
                            )
                        }
                        item { Spacer(Modifier.height(FmSpace.x4)) }
                    }
                }
            }
        }
    }
}
