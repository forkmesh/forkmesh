package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.code.RepoActivity
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.RepoReleaseAsset
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.launch

@Composable
internal fun ReleasesTab(repo: Repository, value: AsyncValue<List<RepoRelease>>) {
    value.Bind()
    val releases = value.data
    val error = value.error
    when {
        value.waiting || !value.started -> PaddedCardList { LoadingCard("Loading releases...") }
        error != null -> PaddedCardList { EmptyCard("$error") }
        releases.isNullOrEmpty() -> PaddedCardList {
            EmptyCard("No releases yet. Draft and publish from a paired desktop node.")
        }
        else -> LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                start = FmSpace.x4,
                top = FmSpace.x4,
                end = FmSpace.x4,
                bottom = FmSpace.x4 + navigationBarBottom(),
            ),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(releases) { release -> ReleaseCard(repo, release) }
        }
    }
}

@Composable
private fun ReleaseCard(repo: Repository, release: RepoRelease) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val commands = rememberDesktopCommands()
    val desktopRoute = rememberDesktopRoute(repo)
    InfoCard(release.tag) {
        ChipWrap {
            if (release.channel.isNotEmpty()) RepoChip("local_offer_outlined", release.channel)
            if (release.shortCommit.isNotEmpty()) RepoChip("commit_outlined", release.shortCommit)
            if (release.createdDate.isNotEmpty()) RepoChip("calendar_today_outlined", release.createdDate)
            RepoChip("inventory_2_outlined", release.assetSummary)
        }
        Spacer(Modifier.height(12.dp))
        Text(
            "Release artifacts are content-addressed and verified by sha256 integrity pins. Mobile can request desktop " +
                "release work; the paired Qt node builds, signs, mirrors, and publishes locally.",
            color = c.textSecondary,
        )
        Spacer(Modifier.height(12.dp))
        if (release.assets.isEmpty()) {
            Text("No artifacts are attached to this release yet.")
        } else {
            for (asset in release.assets) ReleaseAssetRow(asset)
        }
        Spacer(Modifier.height(12.dp))
        if (desktopRoute) ChipWrap {
            for (request in RepoActivity.releaseRequests) {
                val onClick = {
                    scope.launch {
                        commands.request(
                            repo = repo,
                            command = request.command,
                            target = RepoActivity.releaseTarget(release),
                            payload = RepoActivity.releasePayload(release),
                            successMessage = request.successMessage,
                        )
                    }
                    Unit
                }
                if (request.filled) {
                    FmFilledButton(request.label, onClick = onClick, icon = request.icon)
                } else {
                    FmOutlinedButton(request.label, onClick = onClick, icon = request.icon)
                }
            }
        }
        FmInlineStatus(commands.status, Modifier.padding(top = 8.dp))
    }
}

@Composable
private fun ReleaseAssetRow(asset: RepoReleaseAsset) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    Column(
        Modifier
            .padding(bottom = 8.dp)
            .fillMaxWidth()
            .background(c.bgBase, shape)
            .border(1.dp, c.border, shape)
            .padding(12.dp),
    ) {
        Text(asset.name.ifEmpty { asset.shortSha }, fontWeight = FmType.emphasis)
        Spacer(Modifier.height(6.dp))
        ChipWrap {
            RepoChip("devices_outlined", asset.platformLabel)
            RepoChip("storage_outlined", asset.sizeLabel)
            RepoChip("download_outlined", asset.downloadLabel)
        }
        if (asset.sha256.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            SelectionContainer {
                Text(
                    "sha256: ${asset.sha256}",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary, fontFamily = FmType.mono),
                )
            }
        }
    }
}
