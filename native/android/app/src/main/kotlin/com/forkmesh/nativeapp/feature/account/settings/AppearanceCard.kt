package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
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
import com.forkmesh.nativeapp.settings.ThemeModeChoice
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

@Composable
fun AppearanceCard() {
    val appearance by AppearanceStore.appearance.collectAsState()
    val c = Fm.colors
    val haptics = FmHaptics.current
    val label = Fm.text.labelMedium.copy(color = c.textSecondary)
    Column(Modifier.fillMaxWidth()) {
        Text("Theme", style = label)
        Box(Modifier.height(FmSpace.x2))
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.full)).background(c.bgOverlay).padding(3.dp),
        ) {
            for (mode in ThemeModeChoice.entries) {
                val selected = appearance.themeMode == mode
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
                        .testTag("appearance-theme-${mode.name}")
                        .semantics(mergeDescendants = true) {
                            this.selected = selected
                            contentDescription = "${mode.label} theme"
                        }
                        .clickable(role = Role.Button) {
                            if (!selected) {
                                haptics.selection()
                                AppearanceStore.setThemeMode(mode)
                            }
                        },
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmIcon(mode.icon(), size = 16.dp, tint = color)
                    Box(Modifier.width(FmSpace.x1))
                    Text(
                        mode.label,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.labelMedium.copy(color = color, fontWeight = if (selected) FmType.emphasis else FmType.medium),
                    )
                }
            }
        }
        Box(Modifier.height(FmSpace.x4))
        Text("Accent colour", style = label)
        Box(Modifier.height(FmSpace.x2))
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x5)) {
            for (accent in AccentChoice.entries) {
                val selected = appearance.accent == accent
                val swatch = Color(accent.accent(c.dark))
                Column(
                    Modifier
                        .testTag("appearance-accent-${accent.name}")
                        .semantics(mergeDescendants = true) {
                            this.selected = selected
                            contentDescription = "${accent.label} accent"
                        }
                        .clickable(role = Role.Button) {
                            if (!selected) {
                                haptics.selection()
                                AppearanceStore.setAccent(accent)
                            }
                        },
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Box(
                        Modifier
                            .size(36.dp)
                            .border(2.dp, if (selected) swatch else Color.Transparent, CircleShape)
                            .padding(4.dp)
                            .clip(CircleShape)
                            .background(swatch),
                        contentAlignment = Alignment.Center,
                    ) {
                        if (selected) FmIcon("check_rounded", size = 16.dp, tint = Color.White)
                    }
                    Box(Modifier.height(FmSpace.x1))
                    Text(
                        accent.label,
                        style = Fm.text.labelSmall.copy(
                            color = if (selected) c.textPrimary else c.textSecondary,
                            fontWeight = if (selected) FmType.emphasis else FmType.medium,
                        ),
                    )
                }
            }
        }
    }
}
