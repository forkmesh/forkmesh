package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.runners.RunnerAttentionItem
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

@Composable
private fun attentionColor(expired: Boolean): Color = if (expired) Fm.colors.danger else Fm.colors.warning

@Composable
internal fun RunnerAttentionBanner(navigator: Navigator, modifier: Modifier = Modifier) {
    val items by RunnerAttentionFeature.items.collectAsState()
    val uri = LocalUriHandler.current
    LaunchedEffect(Unit) { RunnerAttentionFeature.refresh() }
    if (items.isEmpty()) return
    val c = Fm.colors
    val tone = attentionColor(RunnerAttention.anyExpired(items))
    val shape = RoundedCornerShape(FmRadius.md)
    Column(
        modifier
            .fillMaxWidth()
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
            .clip(shape)
            .background(tone.copy(alpha = .12f))
            .border(1.dp, tone.copy(alpha = .5f), shape)
            .padding(FmSpace.x3)
            .testTag("runner-attention-banner"),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        if (items.size > 1) {
            Text(RunnerAttention.summaryTitle(items), style = Fm.text.titleSmall.copy(color = tone, fontWeight = FontWeight.Bold))
        }
        for (item in items) {
            val itemTone = attentionColor(item.expired)
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(if (item.expired) "error_outline" else "warning_amber_outlined", size = 20.dp, tint = itemTone)
                Spacer(Modifier.width(FmSpace.x2))
                Column(Modifier.weight(1f)) {
                    Text(RunnerAttention.title(item), style = Fm.text.bodyMedium.copy(color = itemTone, fontWeight = FontWeight.SemiBold))
                    Text(
                        RunnerAttention.detail(item, RunnerAttentionFeature.expiryText(item)),
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                    )
                }
                Spacer(Modifier.width(FmSpace.x2))
                FmToneButton(
                    RunnerAttention.buttonLabel(item),
                    onPressed = { RunnerAttentionFeature.open(navigator, item) { url -> runCatching { uri.openUri(url) } } },
                    icon = "login",
                    tone = if (item.expired) FmTone.Danger else FmTone.Warning,
                    filled = true,
                )
            }
        }
    }
}

@Composable
internal fun RunnerAttentionChip(onOpen: (List<RunnerAttentionItem>) -> Unit) {
    val items by RunnerAttentionFeature.items.collectAsState()
    if (items.isEmpty()) return
    val tone = attentionColor(RunnerAttention.anyExpired(items))
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(tone.copy(alpha = .14f))
            .clickable { onOpen(items) }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
            .testTag("runner-attention-chip"),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("warning_amber_outlined", size = 16.dp, tint = tone)
        Spacer(Modifier.width(FmSpace.x2))
        Text(
            RunnerAttention.summaryTitle(items),
            modifier = Modifier.weight(1f),
            style = Fm.text.bodyMedium.copy(color = tone, fontWeight = FontWeight.SemiBold),
            maxLines = 1,
        )
        Text(
            if (items.size == 1) RunnerAttention.buttonLabel(items[0]) else "Review",
            style = Fm.text.bodyMedium.copy(color = tone, fontWeight = FontWeight.Bold),
        )
    }
}
