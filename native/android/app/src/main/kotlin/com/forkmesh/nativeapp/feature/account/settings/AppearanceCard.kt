package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.settings.AccentChoice
import com.forkmesh.nativeapp.settings.AppTheme
import com.forkmesh.nativeapp.settings.ResolvedTheme
import com.forkmesh.nativeapp.settings.ThemeModeChoice
import com.forkmesh.nativeapp.settings.ThemeSurface
import com.forkmesh.nativeapp.settings.ThemeToken
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

private fun ThemeModeChoice.icon(): String = when (this) {
    ThemeModeChoice.system -> "brightness_auto_outlined"
    ThemeModeChoice.light -> "light_mode_outlined"
    ThemeModeChoice.dark -> "dark_mode_outlined"
}

private val customWheel = Brush.sweepGradient(
    listOf(0xFFFF3B30, 0xFFFFCC00, 0xFF34C759, 0xFF00C7BE, 0xFF007AFF, 0xFFAF52DE, 0xFFFF3B30).map(::Color),
)

@Composable
fun AppearanceCard(navigator: Navigator) {
    val appearance by AppearanceStore.appearance.collectAsState()
    val theme = appearance.theme
    val c = Fm.colors
    val haptics = FmHaptics.current
    val label = Fm.text.labelMedium.copy(color = c.textSecondary)
    var picking by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth()) {
        Text("Theme", style = label)
        Box(Modifier.height(FmSpace.x2))
        ThemeModeTrack(appearance.themeMode) {
            haptics.selection()
            AppearanceStore.setThemeMode(it)
        }
        Box(Modifier.height(FmSpace.x4))
        Text("Accent colour", style = label)
        Box(Modifier.height(FmSpace.x2))
        ThemeAccentSwatches(
            theme,
            c.dark,
            onPreset = {
                haptics.selection()
                AppearanceStore.setAccent(it)
            },
            onCustom = { picking = true },
        )
        Box(Modifier.height(FmSpace.x4))
        Text("Surfaces", style = label)
        Box(Modifier.height(FmSpace.x2))
        ThemeSurfaceChips(theme, c.dark) {
            haptics.selection()
            AppearanceStore.setTheme(theme.chooseSurface(it))
        }
        Box(Modifier.height(FmSpace.x3))
        FmListRow(
            "Theme editor",
            modifier = Modifier.testTag("appearance-theme-editor"),
            subtitle = if (theme.isStandard) "Every colour, shuffle and reset" else "Edited theme",
            leading = { FmIcon("palette", tint = c.textSecondary) },
            onTap = { navigator.push(ThemeEditorScreen()) },
            minHeight = 52.dp,
            padding = PaddingValues(vertical = FmSpace.x1),
        )
    }
    if (picking) {
        ThemeColorPickerDialog(
            title = "Custom accent",
            initial = theme.customAccent ?: theme.resolve(c.dark).rgb(ThemeToken.accent),
            onDismiss = { picking = false },
            onPick = {
                picking = false
                AppearanceStore.setTheme(theme.chooseCustomAccent(it))
            },
        )
    }
}

@Composable
internal fun ThemeModeTrack(selectedMode: ThemeModeChoice, onPick: (ThemeModeChoice) -> Unit) {
    ThemeSegmentTrack(
        ThemeModeChoice.entries,
        selectedMode,
        label = { it.label },
        icon = { it.icon() },
        tag = { "appearance-theme-${it.name}" },
        description = { "${it.label} theme" },
        onPick = onPick,
    )
}

@Composable
internal fun <T> ThemeSegmentTrack(
    options: List<T>,
    selectedOption: T,
    label: (T) -> String,
    icon: (T) -> String?,
    tag: (T) -> String,
    description: (T) -> String,
    onPick: (T) -> Unit,
) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.full)).background(c.bgOverlay).padding(3.dp),
    ) {
        for (option in options) {
            val selected = selectedOption == option
            val color = if (selected) c.textPrimary else c.textSecondary
            val fill = when {
                !selected -> Color.Transparent
                c.dark -> Color.White.copy(alpha = 0.12f)
                else -> c.bgRaised
            }
            Row(
                Modifier
                    .weight(1f)
                    .height(38.dp)
                    .clip(RoundedCornerShape(FmRadius.full))
                    .background(fill)
                    .testTag(tag(option))
                    .semantics(mergeDescendants = true) {
                        this.selected = selected
                        contentDescription = description(option)
                    }
                    .clickable(role = Role.Button) {
                        if (!selected) onPick(option)
                    },
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val iconName = icon(option)
                if (iconName != null) {
                    FmIcon(iconName, size = 16.dp, tint = color)
                    Box(Modifier.width(FmSpace.x1))
                }
                Text(
                    label(option),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.labelMedium.copy(color = color, fontWeight = if (selected) FmType.emphasis else FmType.medium),
                )
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun ThemeAccentSwatches(theme: AppTheme, dark: Boolean, onPreset: (AccentChoice) -> Unit, onCustom: () -> Unit) {
    val presets: List<Pair<AccentChoice, ResolvedTheme>> = remember(theme.surface, theme.readable, dark) {
        AccentChoice.entries.map { it to AppTheme(surface = theme.surface, accent = it, readable = theme.readable).resolve(dark) }
    }
    val current = remember(theme, dark) { theme.resolve(dark) }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        for ((accent, resolved) in presets) {
            val selected = theme.customAccent == null && theme.accent == accent
            ThemeSwatch(
                label = accent.label,
                tag = "appearance-accent-${accent.name}",
                description = "${accent.label} accent",
                selected = selected,
                fill = Color(resolved[ThemeToken.accent]),
                check = Color(resolved[ThemeToken.onAccent]),
            ) {
                if (!selected) onPreset(accent)
            }
        }
        val custom = theme.customAccent != null
        ThemeSwatch(
            label = "Custom",
            tag = "appearance-accent-custom",
            description = "Custom accent",
            selected = custom,
            fill = if (custom) Color(current[ThemeToken.accent]) else null,
            check = Color(current[ThemeToken.onAccent]),
            onTap = onCustom,
        )
    }
}

@Composable
private fun ThemeSwatch(
    label: String,
    tag: String,
    description: String,
    selected: Boolean,
    fill: Color?,
    check: Color,
    onTap: () -> Unit,
) {
    val c = Fm.colors
    val ring = fill ?: c.accent
    Column(
        Modifier
            .width(60.dp)
            .testTag(tag)
            .semantics(mergeDescendants = true) {
                this.selected = selected
                contentDescription = description
            }
            .clickable(role = Role.Button, onClick = onTap),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            Modifier
                .size(36.dp)
                .border(2.dp, if (selected) ring else Color.Transparent, CircleShape)
                .padding(4.dp)
                .clip(CircleShape)
                .then(if (fill != null) Modifier.background(fill) else Modifier.background(customWheel)),
            contentAlignment = Alignment.Center,
        ) {
            if (selected) FmIcon("check_rounded", size = 16.dp, tint = check)
            if (fill == null) FmIcon("add", size = 16.dp, tint = Color.White)
        }
        Box(Modifier.height(FmSpace.x1))
        Text(
            label,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.labelSmall.copy(
                color = if (selected) c.textPrimary else c.textSecondary,
                fontWeight = if (selected) FmType.emphasis else FmType.medium,
            ),
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun ThemeSurfaceChips(theme: AppTheme, dark: Boolean, onPick: (ThemeSurface) -> Unit) {
    val c = Fm.colors
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        for (surface in ThemeSurface.entries) {
            val selected = theme.surface == surface
            val colors = surface.colors(dark)
            val shape = RoundedCornerShape(FmRadius.full)
            Row(
                Modifier
                    .height(34.dp)
                    .clip(shape)
                    .background(if (selected) c.accentSubtle else c.bgOverlay)
                    .border(1.dp, if (selected) c.accent else Color.Transparent, shape)
                    .testTag("appearance-surface-${surface.name}")
                    .semantics(mergeDescendants = true) {
                        this.selected = selected
                        contentDescription = "${surface.label} surfaces"
                    }
                    .clickable(role = Role.Button) {
                        if (!selected) onPick(surface)
                    }
                    .padding(start = FmSpace.x2, end = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    Modifier
                        .size(18.dp)
                        .clip(CircleShape)
                        .background(Color(0xFF000000 or (colors[ThemeToken.bgBase] ?: 0L)))
                        .border(1.dp, c.border, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Box(Modifier.size(6.dp).clip(CircleShape).background(Color(0xFF000000 or (colors[ThemeToken.textPrimary] ?: 0L))))
                }
                Box(Modifier.width(6.dp))
                Text(
                    surface.label,
                    maxLines = 1,
                    style = Fm.text.labelMedium.copy(
                        color = if (selected) c.accent else c.textPrimary,
                        fontWeight = if (selected) FmType.emphasis else FmType.medium,
                    ),
                )
            }
        }
    }
}
