package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.ripple.RippleAlpha
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LocalRippleConfiguration
import androidx.compose.material3.RippleConfiguration
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.account.projects.ProjectActions
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmColors
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Composable
fun FmIconTile(
    icon: String,
    modifier: Modifier = Modifier,
    tint: Color? = null,
    size: Dp = 38.dp,
    iconSize: Dp = 19.dp,
    radius: Dp = FmRadius.md,
) {
    val colors = Fm.colors
    val background = tint?.copy(alpha = if (colors.dark) .20f else .13f) ?: colors.bgOverlay
    Box(
        modifier
            .size(size)
            .background(background, RoundedCornerShape(radius)),
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = iconSize, tint = tint ?: colors.textSecondary)
    }
}

@Composable
fun FmListRow(
    title: String,
    modifier: Modifier = Modifier,
    titleContent: (@Composable () -> Unit)? = null,
    subtitle: String? = null,
    subtitleContent: (@Composable () -> Unit)? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    onTap: (() -> Unit)? = null,
    onLongPress: (() -> Unit)? = null,
    showChevron: Boolean = true,
    titleColor: Color? = null,
    minHeight: Dp = 60.dp,
    padding: PaddingValues = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x2),
) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val interactive = onTap != null || onLongPress != null
    val outerRipple = LocalRippleConfiguration.current
    val wash = colors.accentSubtle
    val rowRipple = remember(wash) {
        RippleConfiguration(color = wash.copy(alpha = 1f), rippleAlpha = RippleAlpha(wash.alpha, wash.alpha, wash.alpha, wash.alpha))
    }
    CompositionLocalProvider(LocalRippleConfiguration provides if (interactive) rowRipple else outerRipple) {
        Row(
            modifier
                .fillMaxWidth()
                .then(
                    if (interactive) {
                        Modifier.combinedClickable(
                            hapticFeedbackEnabled = false,
                            onLongClick = onLongPress?.let { longPress ->
                                {
                                    haptics.commit(SoundCue.OPEN)
                                    longPress()
                                }
                            },
                            onClick = {
                                if (onTap != null) {
                                    haptics.tap()
                                    onTap()
                                }
                            },
                        )
                    } else {
                        Modifier
                    },
                )
                .heightIn(min = minHeight)
                .padding(padding),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CompositionLocalProvider(LocalRippleConfiguration provides outerRipple) {
                if (leading != null) {
                    leading()
                    Spacer(Modifier.width(FmSpace.x3))
                }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
                    if (titleContent != null) {
                        titleContent()
                    } else {
                        Text(
                            title,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.titleSmall.let { if (titleColor != null) it.copy(color = titleColor) else it },
                        )
                    }
                    if (subtitleContent != null) {
                        Spacer(Modifier.height(3.dp))
                        subtitleContent()
                    } else if (!subtitle.isNullOrEmpty()) {
                        Spacer(Modifier.height(3.dp))
                        Text(
                            subtitle,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(
                                color = colors.textTertiary,
                                lineHeight = FmType.meta * FmType.leadingTight,
                            ),
                        )
                    }
                }
                if (trailing != null) {
                    Spacer(Modifier.width(FmSpace.x2))
                    trailing()
                }
                if (showChevron) {
                    Spacer(Modifier.width(FmSpace.x1))
                    FmIcon("chevron_right", size = 20.dp, tint = colors.textDisabled)
                }
            }
        }
    }
}

@Composable
fun FmRowDivider(modifier: Modifier = Modifier, indent: Dp = 66.dp) {
    HorizontalDivider(modifier.padding(start = indent), thickness = 1.dp, color = Fm.colors.border)
}

@Composable
fun FmCountPill(count: Int, modifier: Modifier = Modifier, active: Boolean = false) {
    val colors = Fm.colors
    Text(
        "$count",
        modifier = modifier
            .background(if (active) colors.accent else colors.bgElevated, RoundedCornerShape(FmRadius.full))
            .widthIn(min = 20.dp)
            .padding(horizontal = 7.dp, vertical = 2.dp),
        textAlign = TextAlign.Center,
        style = Fm.text.labelSmall.copy(
            color = if (active) Color.White else colors.textSecondary,
            letterSpacing = 0.sp,
            lineHeight = FmType.micro * FmType.leadingTight,
            fontFeatureSettings = FmType.tabular,
        ),
    )
}

@Immutable
data class FmSegmentTab(val label: String, val count: Int? = null, val icon: String? = null)

@Composable
fun FmSegmentTabBar(
    tabs: List<FmSegmentTab>,
    index: Int,
    onChanged: (Int) -> Unit,
    modifier: Modifier = Modifier,
    trailing: (@Composable () -> Unit)? = null,
) {
    val colors = Fm.colors
    Row(
        modifier
            .fillMaxWidth()
            .height(48.dp)
            .background(colors.bgRaised)
            .drawBehind {
                val rule = 1.dp.toPx()
                drawRect(colors.border, size = Size(size.width, rule))
                drawRect(colors.border, topLeft = Offset(0f, size.height - rule), size = Size(size.width, rule))
            }
            .padding(vertical = 1.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(Modifier.width(FmSpace.x2))
        tabs.forEachIndexed { i, tab ->
            SegmentTabItem(tab = tab, selected = i == index, onTap = { onChanged(i) })
        }
        Spacer(Modifier.weight(1f))
        if (trailing != null) {
            trailing()
            Spacer(Modifier.width(FmSpace.x2))
        }
    }
}

@Composable
private fun SegmentTabItem(tab: FmSegmentTab, selected: Boolean, onTap: () -> Unit) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val color = if (selected) colors.textPrimary else colors.textTertiary
    Column(
        Modifier
            .fillMaxHeight()
            .selectable(
                selected = selected,
                role = Role.Button,
                onClick = {
                    if (!selected) haptics.selection()
                    onTap()
                },
            )
            .then(
                if (tab.icon == null) Modifier
                else Modifier.clearAndSetSemantics {
                    contentDescription = if (tab.count == null) tab.label else "${tab.label}, ${tab.count}"
                },
            )
            .padding(horizontal = FmSpace.x2)
            .width(IntrinsicSize.Max),
    ) {
        Box(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
            contentAlignment = Alignment.Center,
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (tab.icon != null) {
                    FmIcon(tab.icon, size = 20.dp, tint = color)
                } else {
                    Text(
                        tab.label,
                        style = Fm.text.titleSmall.copy(
                            color = color,
                            fontWeight = if (selected) FmType.emphasis else FmType.regular,
                        ),
                    )
                }
                if (tab.count != null) {
                    Spacer(Modifier.width(6.dp))
                    FmCountPill(tab.count, active = selected)
                }
            }
        }
        Box(
            Modifier
                .fillMaxWidth()
                .height(2.dp)
                .background(if (selected) colors.activeUnderline else Color.Transparent, RoundedCornerShape(2.dp)),
        )
    }
}

@Composable
fun FmGroupHeader(
    label: String,
    modifier: Modifier = Modifier,
    count: Int? = null,
    color: Color? = null,
    padding: PaddingValues = PaddingValues(
        start = FmSpace.x4,
        top = FmSpace.x3,
        end = FmSpace.x3,
        bottom = FmSpace.x2,
    ),
    actions: (@Composable RowScope.() -> Unit)? = null,
) {
    val text = if (count == null) label.uppercase() else "${label.uppercase()} · $count"
    Row(
        modifier
            .fillMaxWidth()
            .padding(padding),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelSmall.copy(color = color ?: Fm.colors.textTertiary),
        )
        if (actions != null) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
                content = actions,
            )
        }
    }
}

enum class FmChangeKind(val letter: String, val semanticLabel: String) {
    Added("A", "Added"),
    Modified("M", "Modified"),
    Deleted("D", "Deleted"),
    Renamed("R", "Renamed"),
}

@Composable
fun FmChangeBadge(kind: FmChangeKind, modifier: Modifier = Modifier, size: Dp = 24.dp) {
    val colors = Fm.colors
    val tone = when (kind) {
        FmChangeKind.Added -> colors.success
        FmChangeKind.Modified -> colors.warning
        FmChangeKind.Deleted -> colors.danger
        FmChangeKind.Renamed -> colors.accent
    }
    Box(
        modifier
            .clearAndSetSemantics { contentDescription = kind.semanticLabel }
            .size(size)
            .background(tone.copy(alpha = if (colors.dark) .22f else .15f), RoundedCornerShape(FmRadius.sm + 2.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Text(kind.letter, style = Fm.mono(size = (size.value * 0.5f).sp, weight = FmType.emphasis, color = tone, height = 1f))
    }
}

enum class FmTone { Accent, Success, Warning, Danger, Neutral }

@Composable
fun FmToneButton(
    label: String,
    onPressed: (() -> Unit)?,
    modifier: Modifier = Modifier,
    icon: String? = null,
    tone: FmTone = FmTone.Neutral,
    filled: Boolean = false,
    tooltip: String? = null,
    height: Dp = 34.dp,
) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val enabled = onPressed != null
    val color = when (tone) {
        FmTone.Accent -> colors.accent
        FmTone.Success -> colors.success
        FmTone.Warning -> colors.warning
        FmTone.Danger -> colors.danger
        FmTone.Neutral -> colors.textSecondary
    }
    val background = when {
        filled -> color
        tone == FmTone.Neutral -> colors.bgOverlay
        else -> color.copy(alpha = if (colors.dark) .20f else .13f)
    }
    val foreground = if (filled) (if (tone == FmTone.Warning) FmColors.darkBgBase else Color.White) else color
    val shape = RoundedCornerShape(FmRadius.md)
    val button: @Composable () -> Unit = {
        Row(
            modifier
                .height(height)
                .clip(shape)
                .clickable(enabled = enabled, role = Role.Button) {
                    haptics.tap(SoundCue.forLabel(label, SoundCue.CLICK))
                    onPressed?.invoke()
                }
                .clearAndSetSemantics { contentDescription = tooltip ?: label }
                .alpha(if (enabled) 1f else .45f)
                .background(background)
                .padding(horizontal = if (icon == null) 12.dp else 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (icon != null) {
                FmIcon(icon, size = 16.dp, tint = foreground)
                Spacer(Modifier.width(6.dp))
            }
            Text(
                label,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.labelMedium.copy(color = foreground, lineHeight = FmType.small),
            )
        }
    }
    if (tooltip == null) button() else FmTooltip(tooltip) { button() }
}

@Composable
fun FmBottomBar(
    leading: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    height: Dp = 50.dp,
    actions: (@Composable RowScope.() -> Unit)? = null,
) {
    val colors = Fm.colors
    Box(
        modifier
            .fillMaxWidth()
            .background(colors.bgRaised)
            .windowInsetsPadding(fmSafeAreaInsets(top = false)),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .height(height)
                .drawBehind { drawRect(colors.border, size = Size(size.width, 1.dp.toPx())) }
                .padding(start = FmSpace.x3, top = 1.dp, end = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.weight(1f)) { leading() }
            if (actions != null) {
                Row(
                    Modifier.padding(start = FmSpace.x2),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                    content = actions,
                )
            }
        }
    }
}

@Composable
fun FmBranchLabel(
    name: String,
    modifier: Modifier = Modifier,
    color: Color? = null,
    onTap: (() -> Unit)? = null,
    trailingIcon: String? = null,
) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val tone = color ?: colors.success
    val tapModifier = if (onTap == null) {
        Modifier
    } else {
        Modifier
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable {
                haptics.tap()
                onTap()
            }
            .padding(FmSpace.x2)
    }
    Row(modifier.then(tapModifier), verticalAlignment = Alignment.CenterVertically) {
        FmIcon("account_tree_outlined", size = 16.dp, tint = tone)
        Spacer(Modifier.width(6.dp))
        Text(
            name,
            modifier = Modifier.weight(1f, fill = false),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelMedium.copy(color = tone),
        )
        if (trailingIcon != null) {
            Spacer(Modifier.width(2.dp))
            FmIcon(trailingIcon, size = 18.dp, tint = colors.textTertiary)
        }
    }
}

@Composable
fun FmDialogActions(
    onCancel: (() -> Unit)?,
    onConfirm: (() -> Unit)?,
    modifier: Modifier = Modifier,
    confirmLabel: String = "Create",
    cancelLabel: String = "Cancel",
    destructive: Boolean = false,
    busy: Boolean = false,
) {
    val colors = Fm.colors
    val haptics = FmHaptics.current
    val tone = if (destructive) colors.danger else colors.accent
    Row(
        modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.End,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TextButton(
            onClick = {
                FmSoundCues.play(SoundCue.CLOSE)
                onCancel?.invoke()
            },
            modifier = Modifier.defaultMinSize(minWidth = 1.dp, minHeight = 44.dp),
            enabled = !busy && onCancel != null,
            colors = ButtonDefaults.textButtonColors(contentColor = colors.textPrimary),
            contentPadding = PaddingValues(horizontal = FmSpace.x4),
        ) {
            Text(cancelLabel)
        }
        Spacer(Modifier.width(FmSpace.x2))
        Button(
            onClick = {
                if (destructive) haptics.failure(SoundCue.DELETE) else haptics.commit(SoundCue.forLabel(confirmLabel, SoundCue.CLICK))
                onConfirm?.invoke()
            },
            modifier = Modifier.defaultMinSize(minWidth = 112.dp, minHeight = 44.dp),
            enabled = !busy && onConfirm != null,
            shape = RoundedCornerShape(FmRadius.md),
            colors = ButtonDefaults.buttonColors(
                containerColor = tone,
                contentColor = Color.White,
                disabledContainerColor = tone.copy(alpha = .45f),
                disabledContentColor = Color(0xB3FFFFFF),
            ),
            contentPadding = PaddingValues(horizontal = FmSpace.x5),
        ) {
            if (busy) {
                FmCircularProgressIndicator(Modifier.size(18.dp), color = Color.White, strokeWidth = 2.dp)
            } else {
                Text(confirmLabel)
            }
        }
    }
}

object FmNeedsDesktopNote {
    const val headline = "Needs a desktop or server node on this account"
    const val action = "Get the desktop app"
}

@Composable
fun FmNeedsDesktopNote(
    modifier: Modifier = Modifier,
    detail: String = "",
    icon: String = "desktop_access_disabled_outlined",
) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        modifier
            .fillMaxWidth()
            .background(colors.bgOverlay, shape)
            .border(1.dp, colors.border, shape)
            .padding(FmSpace.x3),
    ) {
        FmIcon(icon, size = 18.dp, tint = colors.warning)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f)) {
            Text(FmNeedsDesktopNote.headline, style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis))
            Spacer(Modifier.height(FmSpace.x1))
            Text(
                detail.ifEmpty {
                    "This phone publishes and signs as the owner, but it never hosts or mirrors, so it cannot run this work."
                },
                style = Fm.text.bodySmall.copy(lineHeight = FmType.meta * FmType.leadingRelaxed),
            )
            val context = LocalContext.current
            FmTextButton(
                onClick = { openExternalUrl(context, ProjectActions.desktopUrl()) },
                minHeight = 36.dp,
                contentPadding = PaddingValues(0.dp),
            ) {
                Text(FmNeedsDesktopNote.action)
            }
        }
    }
}

@Composable
fun FmInboxPendingNote(modifier: Modifier = Modifier) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    Row(
        modifier
            .background(colors.bgBase, shape)
            .border(1.dp, colors.border, shape)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("verified_user_outlined", size = 14.dp, tint = colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x1))
        Text(
            "Signed inbox action - pending the repo owner applying it.",
            modifier = Modifier.weight(1f, fill = false),
            style = Fm.text.bodySmall,
        )
    }
}
