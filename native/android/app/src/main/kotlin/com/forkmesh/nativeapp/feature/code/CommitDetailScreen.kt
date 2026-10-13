package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.code.ChangeKind
import com.forkmesh.nativeapp.code.CommitFields
import com.forkmesh.nativeapp.code.DiffReviewModel
import com.forkmesh.nativeapp.code.Patches
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.code.diff.parseUnifiedDiff
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.model.RepoCommitDetail
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmChangeBadge
import com.forkmesh.nativeapp.ui.kit.FmChangeKind
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

internal class CommitDetailScreen(val repo: Repository, val commit: Map<String, Any?>) : Screen() {
    private val hash = CommitFields.hash(commit)
    private val detail = AsyncValue<RepoCommitDetail>().apply {
        if (hash.isNotEmpty()) reload { repoApi.commitDetail(repo.owner, repo.name, hash, fallbackCommit = commit) }
    }
    private var reviewFor: RepoCommitDetail? = null
    private var review: DiffReviewModel? = null

    override val title: String get() = CommitFields.shortHash(hash).ifEmpty { "Commit" }

    private fun reviewOf(data: RepoCommitDetail): DiffReviewModel {
        val current = review
        if (current != null && reviewFor === data) return current
        return DiffReviewModel.of(parseUnifiedDiff(data.diff), "${repo.owner}/${repo.name}/commit/$hash", store = Services.get<PrefStore>()).also {
            review = it
            reviewFor = data
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        detail.Bind()
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        val data = detail.data
        val error = detail.error
        DetailScaffold(title = title, navigator = navigator) {
            if (hash.isNotEmpty() && data != null && error == null) {
                DiffReviewStream(
                    model = reviewOf(data),
                    extra = buildList {
                        add(DiffMenuAction("Copy commit id", "tag") { copy("Id", hash) })
                        if (data.diff.isNotEmpty()) add(DiffMenuAction("Copy patch", "content_copy") { copy("Patch", data.diff) })
                    },
                    emptyMessage = "This commit does not change any files.",
                    status = copyStatus,
                    onRefresh = { detail.reload { repoApi.commitDetail(repo.owner, repo.name, hash, fallbackCommit = commit) } },
                    fileActions = { file -> diffFileActions(repo, file, hash, navigator, editable = false) },
                    header = {
                        Column(Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x2)) {
                            CommitSummaryCard(repo, data)
                            if (data.truncated) {
                                Spacer(Modifier.height(FmSpace.x3))
                                Text(
                                    "Diff truncated by the desktop host to keep the mobile response bounded.",
                                    color = Fm.colors.warning,
                                    fontWeight = FmType.emphasis,
                                )
                            }
                        }
                    },
                )
            } else {
                val bottom = navigationBarBottom()
                LazyColumn(
                    Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(start = 20.dp, top = 20.dp, end = 20.dp, bottom = 20.dp + bottom),
                ) {
                    item { CommitSummaryCard(repo, data ?: RepoCommitDetail(commit = commit)) }
                    if (hash.isNotEmpty()) {
                        item { Spacer(Modifier.height(12.dp)) }
                        if (error != null) {
                            item { EmptyCard("Could not load commit diff: $error") }
                        } else {
                            item { LoadingCard("Loading commit diff...") }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun CommitSummaryCard(repo: Repository, detail: RepoCommitDetail) {
    val commit = detail.commit
    val hash = CommitFields.hash(commit)
    val author = CommitFields.author(commit)
    val body = CommitFields.body(commit)
    val source = RepoExplorer.sourceLabel(detail.source)
    InfoCard(CommitFields.subject(commit)) {
        ChipWrap {
            RepoChip("tag", hash.ifEmpty { "unknown" })
            if (author.isNotEmpty()) RepoChip("person_outline", author)
            RepoChip("folder_outlined", repo.fullName)
            if (source != null) RepoChip("dns_outlined", source)
        }
        if (body.isNotEmpty()) {
            Spacer(Modifier.height(16.dp))
            SelectionContainer {
                LinkedText(body, RepoRef(repo.owner, repo.name), style = LocalTextStyle.current.copy(lineHeight = FmType.small * 1.45f))
            }
        }
    }
}

private data class ChangedFile(val path: String, val kind: ChangeKind?, val adds: String, val dels: String)

private fun ChangeKind.toFm(): FmChangeKind = when (this) {
    ChangeKind.Added -> FmChangeKind.Added
    ChangeKind.Modified -> FmChangeKind.Modified
    ChangeKind.Deleted -> FmChangeKind.Deleted
    ChangeKind.Renamed -> FmChangeKind.Renamed
}

@Composable
internal fun PatchFilesSummary(patch: String) {
    val files = remember(patch) { Patches.parsePatchFiles(patch) }
    if (files.isEmpty()) return
    ChangedFilesCard(
        title = Patches.changedFilesTitle(files.size),
        totalAdds = files.sumOf { it.adds },
        totalDels = files.sumOf { it.dels },
        rows = files.map {
            ChangedFile(it.path, it.kind, if (it.adds > 0) "${it.adds}" else "", if (it.dels > 0) "${it.dels}" else "")
        },
    )
}

@Composable
private fun ChangedFilesCard(title: String, totalAdds: Int, totalDels: Int, rows: List<ChangedFile>) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
        Column(Modifier.fillMaxWidth()) {
            Row(
                Modifier.padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(title, Modifier.weight(1f), style = Fm.text.titleMedium.copy(color = c.textPrimary))
                if (rows.size > 1) DiffStat(totalAdds, totalDels)
            }
            HorizontalDivider(thickness = 1.dp, color = c.border)
            rows.forEachIndexed { index, row ->
                ChangedFileRow(row)
                if (index != rows.lastIndex) FmRowDivider(indent = 56.dp)
            }
            Spacer(Modifier.height(FmSpace.x1))
        }
    }
}

@Composable
private fun DiffStat(adds: Int, dels: Int) {
    if (adds == 0 && dels == 0) return
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("+$adds", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.success))
        Spacer(Modifier.width(6.dp))
        Text("-$dels", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.danger))
    }
}

@Composable
private fun ChangedFileRow(file: ChangedFile) {
    val c = Fm.colors
    val (dir, name) = Patches.splitPath(file.path)
    val adds = Patches.visibleCount(file.adds)
    val dels = Patches.visibleCount(file.dels)
    FmListRow(
        title = file.path,
        minHeight = 52.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        titleContent = {
            Text(
                buildAnnotatedString {
                    if (dir.isNotEmpty()) withStyle(SpanStyle(color = c.textTertiary, fontWeight = FmType.emphasis)) { append(dir) }
                    withStyle(SpanStyle(color = c.textPrimary, fontWeight = FmType.emphasis)) { append(name) }
                },
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(lineHeight = FmType.small * 1.25f),
            )
        },
        leading = {
            val kind = file.kind
            if (kind == null) {
                FmIconTile(RepoExplorer.fileGlyph(name), size = 26.dp, iconSize = 15.dp)
            } else {
                FmChangeBadge(kind.toFm(), size = 26.dp)
            }
        },
        showChevron = false,
        trailing = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (adds.isNotEmpty()) {
                    Text("+$adds", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.success))
                }
                if (adds.isNotEmpty() && dels.isNotEmpty()) Spacer(Modifier.width(6.dp))
                if (dels.isNotEmpty()) {
                    Text("-$dels", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.danger))
                }
            }
        },
    )
}
