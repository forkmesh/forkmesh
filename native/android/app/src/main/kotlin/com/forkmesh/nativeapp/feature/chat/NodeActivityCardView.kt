package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import com.forkmesh.nativeapp.chat.NodeActivityCard
import com.forkmesh.nativeapp.chat.NodeActivityTone
import com.forkmesh.nativeapp.chat.nodeActivityCard
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmNeedsDesktopNote
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun NodeActivityCardView() {
    val node by ChatFeature.nodeSnapshot.collectAsState()
    val applier by ChatFeature.applierSummary.collectAsState()
    val session = Services.get<AuthService>()?.session?.collectAsState()?.value
    val card = nodeActivityCard(node, noDesktop = session?.desktopDevices?.isEmpty() ?: true, applier = applier) ?: return
    val colors = Fm.colors
    val badgeColor = when (card.badgeTone) {
        NodeActivityTone.TERTIARY -> colors.textTertiary
        NodeActivityTone.SUCCESS -> colors.success
        NodeActivityTone.WARNING -> colors.warning
    }
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(
            title = NodeActivityCard.TITLE,
            icon = "hub_outlined",
            trailing = { FmStatusBadge(label = card.badge, color = badgeColor) },
        )
        Spacer(Modifier.height(FmSpace.x2))
        FmCard(radius = FmRadius.lg, color = colors.bgRaised) {
            Column(Modifier.fillMaxWidth()) {
                Text(card.headline, style = Fm.text.titleSmall.copy(color = colors.textPrimary))
                Spacer(Modifier.height(FmSpace.x1))
                Text(card.body, style = Fm.text.bodySmall.copy(color = colors.textSecondary, lineHeight = Fm.text.bodySmall.fontSize * FmType.leadingSnug))
                Spacer(Modifier.height(FmSpace.x3))
                TokenRow(card.tokens)
                if (card.lastError.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x2))
                    Text(card.lastError, style = Fm.text.bodySmall.copy(color = colors.danger))
                }
                card.needsDesktopDetail?.let {
                    Spacer(Modifier.height(FmSpace.x3))
                    FmNeedsDesktopNote(detail = it)
                }
                if (card.applierTokens.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    TokenRow(card.applierTokens)
                    card.appliedNote?.let {
                        Spacer(Modifier.height(FmSpace.x2))
                        Text(it, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                    }
                    for (reason in card.leftReasons) {
                        Spacer(Modifier.height(FmSpace.x1))
                        Text(reason, style = Fm.text.bodySmall.copy(color = colors.textTertiary))
                    }
                }
                Spacer(Modifier.height(FmSpace.x3))
                FmToneButton(
                    label = card.syncLabel,
                    icon = "sync",
                    tooltip = NodeActivityCard.SYNC_TOOLTIP,
                    onPressed = if (card.syncing) {
                        null
                    } else {
                        { AppLifecycle.scope.launch(Dispatchers.IO) { ChatFeature.nodeSync?.syncNow() } }
                    },
                )
            }
        }
        Spacer(Modifier.height(FmSpace.x5))
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TokenRow(tokens: List<String>) {
    val colors = Fm.colors
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        for (token in tokens) {
            Text(
                token,
                style = Fm.text.labelSmall.copy(color = colors.textSecondary),
                modifier = Modifier
                    .background(colors.bgOverlay, RoundedCornerShape(FmRadius.full))
                    .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
            )
        }
    }
}
