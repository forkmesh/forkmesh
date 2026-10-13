package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.LegalLinks
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.model.SponsoredAd
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun SponsoredAdCard(modifier: Modifier = Modifier, topGap: Boolean = false) {
    val api = Services.get<ApiService>() ?: return
    val settings = Services.get<SettingsService>()
    val enabled = settings?.showSponsoredMessages?.collectAsState()?.value ?: true
    val hiddenUntilMs = settings?.sponsoredHiddenUntilMs?.collectAsState()?.value ?: 0L
    var hiddenLocally by remember { mutableStateOf(false) }
    val hidden = hiddenLocally || hiddenUntilMs > System.currentTimeMillis()
    val allowed = enabled && !hidden
    var ad by remember { mutableStateOf<SponsoredAd?>(null) }

    LaunchedEffect(allowed) {
        if (allowed && ad == null) {
            ad = withContext(Dispatchers.IO) { runCatching { api.sponsoredAd() }.getOrNull() }
        }
    }

    val shown = ad
    if (!allowed || shown == null) return

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val c = Fm.colors
    val caption = Fm.text.labelSmall.copy(color = c.textTertiary)

    Column(modifier.fillMaxWidth()) {
        if (topGap) VGap(FmSpace.x5)
        FmCard(
            modifier = Modifier
                .fillMaxWidth()
                .testTag("sponsored-ad-card")
                .semantics { contentDescription = "Sponsored message" },
            radius = FmRadius.lg,
        ) {
            Column(Modifier.fillMaxWidth()) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("Sponsored", style = caption)
                    if (shown.sponsor.isNotEmpty()) {
                        Text("  ·  ", style = caption)
                        Text(
                            shown.sponsor,
                            modifier = Modifier.weight(1f),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = caption,
                        )
                    }
                }
                VGap(FmSpace.x2)
                Text(
                    shown.headline,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                )
                if (shown.body.isNotEmpty()) {
                    VGap(FmSpace.x1)
                    Text(
                        shown.body,
                        maxLines = 4,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodySmall.copy(
                            color = c.textSecondary,
                            lineHeight = FmType.meta * FmType.leadingSnug,
                        ),
                    )
                }
                VGap(FmSpace.x2)
                Text(shown.host, maxLines = 1, overflow = TextOverflow.Ellipsis, style = caption)
                VGap(FmSpace.x3)
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    FmToneButton(
                        label = shown.cta,
                        onPressed = {
                            openExternalUrl(context, shown.url)
                            scope.launch(Dispatchers.IO) { runCatching { api.sponsoredAdClicked(shown.id) } }
                        },
                        modifier = Modifier
                            .align(Alignment.CenterVertically)
                            .testTag("sponsored-ad-cta"),
                        tone = FmTone.Accent,
                    )
                    FmTextButton(
                        onClick = { openExternalUrl(context, LegalLinks.AD_NETWORK) },
                        modifier = Modifier
                            .align(Alignment.CenterVertically)
                            .testTag("sponsored-ad-why"),
                        contentColor = c.textTertiary,
                        minHeight = 34.dp,
                        contentPadding = PaddingValues(horizontal = FmSpace.x2),
                        textStyle = Fm.text.labelSmall,
                    ) { Text("Why this ad?") }
                    FmTextButton(
                        onClick = {
                            if (settings != null) settings.hideSponsoredMessages() else hiddenLocally = true
                        },
                        modifier = Modifier
                            .align(Alignment.CenterVertically)
                            .testTag("sponsored-ad-hide"),
                        contentColor = c.textTertiary,
                        minHeight = 34.dp,
                        contentPadding = PaddingValues(horizontal = FmSpace.x2),
                        textStyle = Fm.text.labelSmall,
                    ) { Text("Hide") }
                }
            }
        }
    }
}
