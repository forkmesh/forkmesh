package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.LocalContentColor
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonColors
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
fun fmSafeAreaInsets(top: Boolean = true, bottom: Boolean = true): WindowInsets {
    var sides = WindowInsetsSides.Horizontal
    if (top) sides += WindowInsetsSides.Top
    if (bottom) sides += WindowInsetsSides.Bottom
    return WindowInsets.systemBars.union(WindowInsets.displayCutout).only(sides)
}

@Composable
fun FmCard(
    modifier: Modifier = Modifier,
    onTap: (() -> Unit)? = null,
    color: Color? = null,
    padding: PaddingValues? = null,
    radius: Dp? = null,
    margin: PaddingValues? = null,
    content: @Composable () -> Unit = {},
) {
    val shape = RoundedCornerShape(radius ?: FmRadius.md)
    val haptics = FmHaptics.current
    Box(
        modifier
            .then(if (margin != null) Modifier.padding(margin) else Modifier)
            .clip(shape)
            .background(color ?: Fm.colors.bgRaised)
            .then(
                if (onTap != null) {
                    Modifier.clickable {
                        haptics.tap()
                        onTap()
                    }
                } else {
                    Modifier
                },
            )
            .padding(padding ?: PaddingValues(FmSpace.x4)),
    ) {
        content()
    }
}

@Composable
fun FmSectionHeader(
    title: String,
    modifier: Modifier = Modifier,
    icon: String? = null,
    trailing: (@Composable () -> Unit)? = null,
    count: Int? = null,
) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        if (icon != null) {
            FmIcon(icon, size = 14.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(
            title.uppercase(),
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelSmall,
        )
        if (trailing != null) {
            Spacer(Modifier.width(FmSpace.x3))
            trailing()
        } else if (count != null) {
            Spacer(Modifier.width(FmSpace.x3))
            Text("$count", style = Fm.text.bodySmall.copy(fontFeatureSettings = FmType.tabular))
        }
    }
}

@Composable
fun FmStatusBadge(
    label: String,
    color: Color,
    modifier: Modifier = Modifier,
    backgroundColor: Color? = null,
) {
    Text(
        label.uppercase(),
        modifier = modifier
            .background(backgroundColor ?: color.copy(alpha = 31f / 255f), RoundedCornerShape(FmRadius.full))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        style = Fm.text.labelSmall.copy(color = color),
    )
}

@Composable
fun FmEmptyState(
    icon: String,
    title: String,
    modifier: Modifier = Modifier,
    message: String? = null,
) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = FmSpace.x5, vertical = FmSpace.x7),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            FmIcon(icon, size = 40.dp, tint = Fm.colors.textTertiary)
            Spacer(Modifier.height(FmSpace.x4))
            Text(title, textAlign = TextAlign.Center, style = Fm.text.titleMedium)
            if (message != null) {
                Spacer(Modifier.height(FmSpace.x2))
                Text(
                    message,
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
                )
            }
            FmMessageTools(message?.let { "$title\n$it" } ?: title, Modifier.padding(top = FmSpace.x1))
        }
    }
}

@Composable
fun FmBottomActionBar(
    modifier: Modifier = Modifier,
    padding: PaddingValues? = null,
    content: @Composable () -> Unit,
) {
    Box(
        modifier
            .fillMaxWidth()
            .windowInsetsPadding(fmSafeAreaInsets(top = false))
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1)
            .fmGlass(RoundedCornerShape(FmRadius.lg * 2))
            .padding(padding ?: PaddingValues(FmSpace.x4)),
    ) {
        content()
    }
}

@Composable
fun FmPanelHeader(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    Row(
        modifier
            .fillMaxWidth()
            .height(56.dp)
            .padding(horizontal = FmSpace.x4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(FmSpace.x3))
        }
        Column(Modifier.weight(1f)) {
            Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleMedium)
            if (subtitle != null) {
                Spacer(Modifier.height(FmSpace.x1))
                Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall)
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(FmSpace.x3))
            trailing()
        }
    }
}

val LocalFmFieldLabel = compositionLocalOf<String?> { null }

@Composable
fun FmField(
    label: String,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Column(modifier) {
        Text(
            label,
            modifier = Modifier
                .fillMaxWidth()
                .clearAndSetSemantics {},
            style = Fm.text.bodySmall.copy(fontWeight = FmType.medium),
        )
        Spacer(Modifier.height(FmSpace.x2))
        CompositionLocalProvider(LocalFmFieldLabel provides label) {
            Box(Modifier.fillMaxWidth()) { content() }
        }
    }
}

@Composable
fun FmKeyValueRow(label: String, value: String, modifier: Modifier = Modifier) {
    Row(modifier.padding(vertical = FmSpace.x2)) {
        Text(
            label,
            modifier = Modifier.width(96.dp),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.mono(color = Fm.colors.textTertiary, weight = FmType.medium),
        )
        Spacer(Modifier.width(FmSpace.x4))
        Text(value, modifier = Modifier.weight(1f), style = Fm.text.bodyMedium)
    }
}

@Composable
fun FmIconButton(
    icon: String,
    label: String,
    onPressed: (() -> Unit)?,
    modifier: Modifier = Modifier,
    color: Color? = null,
    colors: IconButtonColors? = null,
    iconSize: Dp = 24.dp,
    busy: Boolean? = null,
) {
    FmIconButton(label = label, onPressed = onPressed, modifier = modifier, color = color, colors = colors, busy = busy) {
        FmIcon(icon, size = iconSize)
    }
}

@Composable
fun FmIconButton(
    label: String,
    onPressed: (() -> Unit)?,
    modifier: Modifier = Modifier,
    color: Color? = null,
    colors: IconButtonColors? = null,
    busy: Boolean? = null,
    icon: @Composable () -> Unit,
) {
    val haptics = FmHaptics.current
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val enabled = onPressed != null
    val tap = rememberFmBusyTap(enabled)
    val tint = when {
        colors != null -> if (enabled) colors.contentColor else colors.disabledContentColor
        else -> color ?: Fm.colors.textPrimary
    }
    FmTooltip(label) {
        Box(
            modifier
                .size(48.dp)
                .fmBusyRing(tap.busy(busy), inset = 7.dp)
                .alpha(if (enabled) 1f else 0.38f)
                .clickable(
                    interactionSource = interaction,
                    indication = null,
                    enabled = enabled,
                    role = Role.Button,
                ) {
                    tap.tapped()
                    haptics.tap(SoundCue.forLabel(label, SoundCue.CLICK))
                    onPressed?.invoke()
                }
                .semantics { contentDescription = label },
            contentAlignment = Alignment.Center,
        ) {
            if (pressed) Box(Modifier.size(40.dp).background(Fm.colors.textPrimary.copy(alpha = 0.08f), CircleShape))
            CompositionLocalProvider(LocalContentColor provides tint) { icon() }
        }
    }
}

@Composable
fun FmConnectionBanner(
    state: RelayConnection,
    modifier: Modifier = Modifier,
    onRetry: (() -> Unit)? = null,
) {
    if (state != RelayConnection.Offline) return
    val colors = Fm.colors
    Row(
        modifier
            .fillMaxWidth()
            .background(colors.danger.copy(alpha = 22f / 255f))
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("cloud_off_outlined", size = 18.dp, tint = colors.danger)
        Spacer(Modifier.width(FmSpace.x3))
        Text(
            "You are offline. Messages and live updates are paused.",
            modifier = Modifier.weight(1f),
            style = Fm.text.bodyMedium.copy(fontWeight = FmType.medium),
        )
        FmMessageTools("You are offline. Messages and live updates are paused.")
        if (onRetry != null) {
            FmTextButton(onClick = onRetry) { Text("Retry") }
        }
    }
}
