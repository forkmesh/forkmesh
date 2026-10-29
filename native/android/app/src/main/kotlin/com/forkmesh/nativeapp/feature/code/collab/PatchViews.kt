package com.forkmesh.nativeapp.feature.code.collab

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.collab.DiffLineKind
import com.forkmesh.nativeapp.collab.PatchChangeKind
import com.forkmesh.nativeapp.collab.PatchFile
import com.forkmesh.nativeapp.collab.changedFilesTitle
import com.forkmesh.nativeapp.collab.dartLines
import com.forkmesh.nativeapp.collab.diffLineKind
import com.forkmesh.nativeapp.collab.parsePatchFiles
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmChangeBadge
import com.forkmesh.nativeapp.ui.kit.FmChangeKind
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.collab.PullDetail

@Composable
internal fun DiffViewer(diff: String) {
    val colors = Fm.colors
    val lines = remember(diff) { dartLines(diff) }
    LazyColumn(
        Modifier
            .fillMaxWidth()
            .heightIn(max = 420.dp)
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(colors.bgBase),
    ) {
        items(lines) { line ->
            val kind = diffLineKind(line)
            val background = when (kind) {
                DiffLineKind.Added -> colors.success.copy(alpha = .12f)
                DiffLineKind.Removed -> colors.danger.copy(alpha = .10f)
                DiffLineKind.Hunk -> colors.accent.copy(alpha = .10f)
                DiffLineKind.Context -> Color.Transparent
            }
            val textColor = when (kind) {
                DiffLineKind.Added -> colors.success
                DiffLineKind.Removed -> colors.danger
                DiffLineKind.Hunk -> colors.accent
                DiffLineKind.Context -> colors.textPrimary
            }
            SelectionContainer(
                Modifier
                    .fillMaxWidth()
                    .background(background)
                    .padding(horizontal = 12.dp, vertical = 3.dp),
            ) {
                Text(line, style = Fm.mono(size = FmType.meta, color = textColor, height = 1.35f))
            }
        }
    }
}

@Composable
internal fun PatchFilesCard(patch: String, commits: Int = 0) {
    val files = remember(patch) { parsePatchFiles(patch) }
    if (files.isEmpty()) return
    val colors = Fm.colors
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(0.dp)) {
        Column(Modifier.fillMaxWidth()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(start = FmSpace.x4, top = FmSpace.x4, end = FmSpace.x4, bottom = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(changedFilesTitle(files.size), style = Fm.text.titleMedium.copy(color = colors.textPrimary))
                    if (commits > 0) Text(PullDetail.commitsLine(commits), style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                }
                if (files.size > 1) DiffStat(files.sumOf { it.adds }, files.sumOf { it.dels })
            }
            HorizontalDivider(thickness = 1.dp, color = colors.border)
            files.forEachIndexed { index, file ->
                ChangedFileRow(file)
                if (index != files.lastIndex) FmRowDivider(indent = 56.dp)
            }
            Spacer(Modifier.height(FmSpace.x1))
        }
    }
}

@Composable
private fun DiffStat(adds: Int, dels: Int) {
    if (adds == 0 && dels == 0) return
    val colors = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("+$adds", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.success))
        Spacer(Modifier.width(6.dp))
        Text("-$dels", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.danger))
    }
}

private fun PatchChangeKind.toBadge(): FmChangeKind = when (this) {
    PatchChangeKind.Added -> FmChangeKind.Added
    PatchChangeKind.Modified -> FmChangeKind.Modified
    PatchChangeKind.Deleted -> FmChangeKind.Deleted
    PatchChangeKind.Renamed -> FmChangeKind.Renamed
}

@Composable
private fun ChangedFileRow(file: PatchFile) {
    val colors = Fm.colors
    val slash = file.path.lastIndexOf('/')
    val dir = if (slash < 0) "" else file.path.substring(0, slash + 1)
    val name = if (slash < 0) file.path else file.path.substring(slash + 1)
    val adds = if (file.adds > 0) "${file.adds}" else ""
    val dels = if (file.dels > 0) "${file.dels}" else ""
    FmListRow(
        title = file.path,
        minHeight = 52.dp,
        padding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        titleContent = {
            Text(
                buildAnnotatedString {
                    if (dir.isNotEmpty()) {
                        withStyle(SpanStyle(color = colors.textTertiary, fontWeight = FmType.emphasis)) { append(dir) }
                    }
                    withStyle(SpanStyle(color = colors.textPrimary, fontWeight = FmType.emphasis)) { append(name) }
                },
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.withHeight(1.25f),
            )
        },
        leading = { FmChangeBadge(kind = file.kind.toBadge(), size = 26.dp) },
        showChevron = false,
        trailing = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (adds.isNotEmpty()) {
                    Text("+$adds", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.success))
                }
                if (adds.isNotEmpty() && dels.isNotEmpty()) Spacer(Modifier.width(6.dp))
                if (dels.isNotEmpty()) {
                    Text("-$dels", style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = colors.danger))
                }
            }
        },
    )
}
