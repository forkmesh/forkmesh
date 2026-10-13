package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.code.BranchComparison
import com.forkmesh.nativeapp.code.BranchDiff
import com.forkmesh.nativeapp.code.DiffReviewModel
import com.forkmesh.nativeapp.code.PreviewKind
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffFileStatus
import com.forkmesh.nativeapp.code.diff.PatchCommit
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.collab.BranchReviewLedger
import com.forkmesh.nativeapp.collab.BranchReviewSession
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.code.review.CodeServices
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmBranchLabel
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPagerSounds
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

internal class BranchDiffScreen(
    val repo: Repository,
    val head: String,
    base: String = "",
    private val subtitleTitle: String = "",
) : Screen() {
    private val base = BranchDiff.base(base, repo.defaultBranch)
    private val comparison = AsyncValue<BranchComparison>().apply { reload(::load) }
    private var tab by mutableIntStateOf(0)
    private var reviewFor: BranchComparison? = null
    private var review: DiffReviewModel? = null
    private val comments: BranchComments? = if (BranchReviewLedger.canComment(head, this.base)) {
        BranchComments(BranchReviewSession(repo.owner, repo.name, head.trim(), CodeServices.ledgerReader(), CodeServices.writer(repo)))
    } else {
        null
    }

    private fun reviewOf(data: BranchComparison): DiffReviewModel {
        val current = review
        if (current != null && reviewFor === data) return current
        return DiffReviewModel.of(data.files, "${repo.owner}/${repo.name}/branch/$head", store = Services.get<PrefStore>()).also {
            review = it
            reviewFor = data
        }
    }

    override val title: String get() = head

    private fun load(): BranchComparison = BranchComparison(repoApi.compare(repo.owner, repo.name, base = base, head = head))

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        comparison.Bind()
        val c = Fm.colors
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        val data = comparison.data
        val error = comparison.error
        val loading = comparison.waiting
        val scope = rememberCoroutineScope()
        val pager = rememberPagerState(initialPage = tab) { 2 }
        LaunchedEffect(data) {
            if (data != null) comments?.reload(data.raw.patch, data.raw.headOid)
        }
        FmPagerSounds(pager)
        LaunchedEffect(pager) { snapshotFlow { pager.currentPage }.collect { tab = it } }
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(
                title = head,
                navigator = navigator,
                titleStyle = Fm.text.titleSmall.copy(color = c.textPrimary),
                actions = {
                    FmIconButton("refresh", "Refresh", onPressed = if (loading) null else ({ comparison.reload(::load) }), color = c.textPrimary, iconSize = 24.dp)
                    DiffViewMenu(
                        if (data != null && data.raw.patch.isNotEmpty()) {
                            listOf(DiffMenuAction("Copy patch", "content_copy") { copy("Patch", data.raw.patch) })
                        } else {
                            emptyList()
                        },
                    )
                },
            )
            Text(
                repo.fullName,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(c.bgRaised)
                    .padding(start = 72.dp, end = FmSpace.x4, bottom = FmSpace.x2),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(color = c.textTertiary),
            )
            FmInlineStatus(
                copyStatus,
                Modifier
                    .background(c.bgRaised)
                    .padding(start = 72.dp, end = FmSpace.x4),
            )
            when {
                data == null && error == null -> Box(Modifier.padding(FmSpace.x4)) { FmSkeletonList(label = "Loading diff") }
                error != null -> FmPullToRefreshBox(isRefreshing = false, onRefresh = { comparison.reload(::load) }, modifier = Modifier.fillMaxSize()) {
                    LazyColumn(Modifier.fillMaxSize()) {
                        item {
                            FmEmptyState("difference_outlined", "Could not load the diff", message = BranchDiff.describeError(error, head, base))
                        }
                        item {
                            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                                FmOutlinedButton("Try again", onClick = { comparison.reload(::load) }, icon = "refresh")
                            }
                        }
                    }
                }
                data != null -> {
                    FmSegmentTabBar(
                        tabs = listOf(
                            FmSegmentTab("Files", data.files.size),
                            FmSegmentTab("Commits", data.commitTabCount),
                        ),
                        index = tab,
                        onChanged = { scope.launch { pager.animateScrollToPage(it) } },
                    )
                    HorizontalPager(
                        state = pager,
                        modifier = Modifier
                            .weight(1f)
                            .fillMaxWidth(),
                    ) { page ->
                        if (page == 0) {
                            DiffReviewStream(
                                model = reviewOf(data),
                                emptyMessage = BranchDiff.emptyMessage(data.raw.commitCount, head, base),
                                onRefresh = { comparison.reload(::load) },
                                fileActions = { file -> diffFileActions(repo, file, head, navigator, editable = true) },
                                header = { ComparisonHeader(data) },
                                comments = comments,
                            )
                        } else {
                            FmPullToRefreshBox(isRefreshing = false, onRefresh = { comparison.reload(::load) }, modifier = Modifier.fillMaxSize()) {
                                CommitList(data, navigator)
                            }
                        }
                    }
                }
            }
        }
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun ComparisonHeader(data: BranchComparison) {
        val c = Fm.colors
        val (additions, deletions) = BranchDiff.totals(data.files)
        FmCard(
            Modifier
                .fillMaxWidth()
                .padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
            radius = FmRadius.lg,
        ) {
            Column {
                if (subtitleTitle.isNotEmpty()) {
                    Text(subtitleTitle, maxLines = 3, overflow = TextOverflow.Ellipsis, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                    Spacer(Modifier.height(FmSpace.x3))
                }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    FmBranchLabel(head)
                    FmIcon("arrow_forward", size = 14.dp, tint = c.textTertiary, modifier = Modifier.align(Alignment.CenterVertically))
                    FmBranchLabel(base, color = c.textSecondary)
                }
                Spacer(Modifier.height(FmSpace.x3))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    Text(
                        BranchDiff.summary(data.raw.commitCount, data.files.size),
                        style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                        modifier = Modifier.align(Alignment.CenterVertically),
                    )
                    DiffStatSummary(additions, deletions)
                }
                if (data.raw.headOid.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Text(BranchDiff.headLine(data.raw), style = Fm.mono(color = c.textTertiary))
                }
                val notes = comments
                if (notes != null && notes.version >= 0 && notes.session.loaded && data.files.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Text(notes.summary, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                }
            }
        }
    }

    @Composable
    private fun CommitList(data: BranchComparison, navigator: Navigator) {
        val commits = remember(data) { data.commits.reversed() }
        if (commits.isEmpty()) {
            val count = data.raw.commitCount
            LazyColumn(Modifier.fillMaxSize()) {
                item { FmEmptyState("commit", BranchDiff.noCommitsTitle(count), message = BranchDiff.noCommitsMessage(count)) }
            }
            return
        }
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(top = FmSpace.x2, bottom = navigationBarBottom() + FmSpace.x5),
        ) {
            items(commits.size, key = { "branch-commit-${commits[it].hash}" }) { index ->
                val commit = commits[index]
                val files = data.filesOf(commit)
                PatchCommitRow(commit, files) { navigator.push(PatchCommitScreen(repo, commit, files)) }
                if (index != commits.lastIndex) FmRowDivider()
            }
        }
    }
}

@Composable
private fun PatchCommitRow(commit: PatchCommit, files: List<DiffFile>, onTap: () -> Unit) {
    val c = Fm.colors
    val (additions, deletions) = BranchDiff.totals(files)
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("commit", tint = c.accent)
        Spacer(Modifier.width(FmSpace.x4))
        Column(Modifier.weight(1f)) {
            Text(commit.subject, maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyLarge)
            Text(BranchDiff.commitMeta(commit), style = Fm.mono(color = c.textTertiary))
        }
        Spacer(Modifier.width(FmSpace.x2))
        DiffStatSummary(additions, deletions, showBar = false)
    }
}

internal fun diffFileActions(repo: Repository, file: DiffFile, ref: String, navigator: Navigator, editable: Boolean): List<DiffMenuAction> {
    if (file.status == DiffFileStatus.Deleted || ref.isEmpty()) return emptyList()
    return buildList {
        if (editable && !file.binary && RepoExplorer.previewKind(file.path) == PreviewKind.Text) {
            add(DiffMenuAction("Edit file", "edit_outlined") { navigator.push(QuickEditScreen(repo, file.path, ref)) })
        }
        add(DiffMenuAction("View file", "description_outlined") { navigator.push(RepoFileScreen(repo, file.path, ref)) })
    }
}

internal class PatchCommitScreen(
    val repo: Repository,
    private val commit: PatchCommit,
    files: List<DiffFile>? = null,
) : Screen() {
    private val review = DiffReviewModel.of(
        files ?: parseUnifiedDiff(commit.patch),
        "${repo.owner}/${repo.name}/commit/${commit.hash}",
        store = Services.get<PrefStore>(),
    )

    override val title: String get() = commit.shortHash

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(title = commit.shortHash, navigator = navigator)
            Box(Modifier.weight(1f)) {
                DiffReviewStream(
                    model = review,
                    extra = buildList {
                        add(DiffMenuAction("Copy commit id", "tag") { copy("Id", commit.hash) })
                        if (commit.patch.isNotEmpty()) add(DiffMenuAction("Copy patch", "content_copy") { copy("Patch", commit.patch) })
                    },
                    emptyMessage = "This commit does not change any files.",
                    status = copyStatus,
                    fileActions = { file -> diffFileActions(repo, file, commit.hash, navigator, editable = false) },
                    header = {
                        FmCard(
                            Modifier
                                .fillMaxWidth()
                                .padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2),
                            radius = FmRadius.lg,
                        ) {
                            SelectionContainer {
                                Column {
                                    LinkedText(commit.subject, RepoRef(repo.owner, repo.name), style = Fm.text.titleSmall.copy(color = c.textPrimary))
                                    if (commit.body.isNotEmpty()) {
                                        Spacer(Modifier.height(FmSpace.x3))
                                        LinkedText(commit.body, RepoRef(repo.owner, repo.name), style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                                    }
                                    Spacer(Modifier.height(FmSpace.x3))
                                    Text(BranchDiff.commitMeta(commit), style = Fm.mono(color = c.textTertiary))
                                }
                            }
                        }
                    },
                )
            }
        }
    }
}

internal class PatchFilesScreen(private val screenTitle: String, private val patch: String) : Screen() {
    private val review = DiffReviewModel.of(parseUnifiedDiff(patch), DiffReviewModel.patchScope(patch), store = Services.get<PrefStore>())

    override val title: String get() = screenTitle

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase),
        ) {
            CodeAppBar(title = screenTitle, navigator = navigator)
            Box(Modifier.weight(1f)) {
                DiffReviewStream(
                    model = review,
                    extra = listOf(DiffMenuAction("Copy patch", "content_copy") { copy("Patch", patch) }),
                    status = copyStatus,
                )
            }
        }
    }
}
