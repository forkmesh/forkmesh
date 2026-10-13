package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import com.forkmesh.nativeapp.code.RepoActivity
import com.forkmesh.nativeapp.model.RepoMirrorSet
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
internal fun MirrorsTab(value: AsyncValue<RepoMirrorSet>) {
    value.Bind()
    val error = value.error
    Box(
        Modifier
            .fillMaxSize()
            .background(Fm.colors.listSurface),
    ) {
        when {
            value.waiting || !value.started -> PaddedCardList { LoadingCard("Loading…") }
            error != null -> PaddedCardList { EmptyCard("$error") }
            else -> {
                val mirrors = value.data?.mirrors.orEmpty()
                val sync = value.data?.syncStatus
                LazyColumn(
                    Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(bottom = FmSpace.x4 + navigationBarBottom()),
                ) {
                    if (sync != null) {
                        item {
                            Column {
                                SyncStatusCard(sync)
                                FmRowDivider()
                            }
                        }
                    }
                    if (mirrors.isEmpty()) {
                        item { EmptyCard("No mirrors reported for this repository yet.", Modifier.padding(FmSpace.x4)) }
                    }
                    itemsIndexed(mirrors) { index, mirror ->
                        Column {
                            if (index > 0) FmRowDivider()
                            val row = RepoActivity.mirrorRow(mirror)
                            MobileCard(row.icon, toneColor(row.tone), row.title, row.subtitle, badge = row.badge)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SyncStatusCard(status: Map<String, Any?>) {
    val row = RepoActivity.syncStatusRow(status)
    Box(Modifier.semantics { liveRegion = LiveRegionMode.Polite }) {
        MobileCard(row.icon, toneColor(row.tone), row.title, row.subtitle, badge = row.badge)
    }
}
