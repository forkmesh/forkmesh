package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderColors
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.settings.AppTheme
import com.forkmesh.nativeapp.settings.ResolvedTheme
import com.forkmesh.nativeapp.settings.ThemeColor
import com.forkmesh.nativeapp.settings.ThemeToken
import com.forkmesh.nativeapp.settings.ThemeTokenGroup
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDialogActions
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

class ThemeEditorScreen : Screen() {
    override val title: String get() = "Theme editor"

    @Composable
    override fun Content(navigator: Navigator) {
        val appearance by AppearanceStore.appearance.collectAsState()
        val theme = appearance.theme
        val haptics = FmHaptics.current
        val currentDark = Fm.dark
        var previewDark by rememberSaveable { mutableStateOf(currentDark) }
        var editing by remember { mutableStateOf<ThemeToken?>(null) }
        var pickingAccent by remember { mutableStateOf(false) }
        var confirmReset by remember { mutableStateOf(false) }
        val resolved = remember(theme, previewDark) { theme.resolve(previewDark) }
        val modeName = if (previewDark) "dark" else "light"
        SettingsPage("Theme editor", navigator) {
            item {
                SettingsCard("Preview") {
                    ThemeSegmentTrack(
                        listOf(false, true),
                        previewDark,
                        label = { if (it) "Dark" else "Light" },
                        icon = { if (it) "dark_mode_outlined" else "light_mode_outlined" },
                        tag = { "theme-preview-${if (it) "dark" else "light"}" },
                        description = { "Preview ${if (it) "dark" else "light"} mode" },
                    ) {
                        haptics.selection()
                        previewDark = it
                    }
                    Spacer(Modifier.height(FmSpace.x1))
                    ThemePreview(theme, previewDark)
                }
            }
            item {
                SettingsCard("Accent") {
                    ThemeAccentSwatches(
                        theme,
                        previewDark,
                        onPreset = {
                            haptics.selection()
                            AppearanceStore.setTheme(theme.chooseAccent(it))
                        },
                        onCustom = { pickingAccent = true },
                    )
                }
            }
            item {
                SettingsCard("Surfaces") {
                    ThemeSurfaceChips(theme, previewDark) {
                        haptics.selection()
                        AppearanceStore.setTheme(theme.chooseSurface(it))
                    }
                }
            }
            item {
                SettingsCard {
                    SettingsSwitch(
                        "Keep colours readable",
                        "Moves the accent, text and status colours until they reach 4.5:1 on the backgrounds.",
                        theme.readable,
                    ) {
                        AppearanceStore.setTheme(theme.copy(readable = it))
                    }
                    FmToneButton(
                        "Shuffle",
                        onPressed = { AppearanceStore.setTheme(AppTheme.shuffled()) },
                        modifier = Modifier.testTag("theme-shuffle"),
                        icon = "shuffle",
                    )
                }
            }
            item {
                Column(Modifier.fillMaxWidth().padding(top = FmSpace.x2)) {
                    Text("Every colour", style = Fm.text.titleMedium)
                    Spacer(Modifier.height(FmSpace.x1))
                    SettingsNote("The $modeName mode colours. Switch the preview to edit the other mode.")
                }
            }
            for (group in ThemeTokenGroup.entries) {
                item(key = "group-${group.name}") {
                    SettingsCard(group.label) {
                        for (token in group.tokens) {
                            ThemeTokenRow(
                                token,
                                resolved,
                                edited = theme.edits(previewDark).containsKey(token),
                                onEdit = { editing = token },
                                onReset = { AppearanceStore.setTheme(theme.setColor(null, token, previewDark)) },
                            )
                        }
                    }
                }
            }
            item {
                SettingsCard {
                    SettingsNote("Puts back the ForkMesh surfaces, the blue accent and readable colours, and drops every edited colour. Light and dark stay as they are.")
                    FmToneButton(
                        "Reset theme",
                        onPressed = if (theme.isStandard) null else ({ confirmReset = true }),
                        modifier = Modifier.testTag("theme-reset"),
                        icon = "restart_alt",
                        tone = FmTone.Danger,
                    )
                }
            }
        }
        val token = editing
        if (token != null) {
            ThemeColorPickerDialog(
                title = "${token.label} ($modeName)",
                initial = resolved.rgb(token),
                onDismiss = { editing = null },
                onPick = {
                    editing = null
                    AppearanceStore.setTheme(theme.setColor(it, token, previewDark))
                },
            )
        }
        if (pickingAccent) {
            ThemeColorPickerDialog(
                title = "Custom accent",
                initial = theme.customAccent ?: resolved.rgb(ThemeToken.accent),
                onDismiss = { pickingAccent = false },
                onPick = {
                    pickingAccent = false
                    AppearanceStore.setTheme(theme.chooseCustomAccent(it))
                },
            )
        }
        if (confirmReset) {
            FmConfirmDialog(
                title = "Reset the theme?",
                body = "Every edited colour is dropped and the ForkMesh surfaces and blue accent come back.",
                confirmLabel = "Reset",
                destructive = true,
                onDismissRequest = { confirmReset = false },
                onConfirm = {
                    confirmReset = false
                    AppearanceStore.setTheme(AppTheme.standard)
                },
            )
        }
    }
}

@Composable
private fun ThemePreview(theme: AppTheme, dark: Boolean) {
    ForkMeshTheme(dark = dark, theme = theme, messenger = false) {
        val c = Fm.colors
        val shape = RoundedCornerShape(FmRadius.lg)
        Column(
            Modifier
                .fillMaxWidth()
                .clip(shape)
                .background(c.bgBase)
                .border(1.dp, c.border, shape)
                .testTag("theme-preview"),
        ) {
            Row(
                Modifier.fillMaxWidth().background(c.bgRaised).padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("arrow_back", size = 18.dp, tint = c.textPrimary)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Projects", style = Fm.text.titleSmall, modifier = Modifier.weight(1f))
                Text("Edit", style = Fm.text.labelMedium.copy(color = c.accent))
            }
            Box(Modifier.fillMaxWidth().height(1.dp).background(c.border))
            Column(Modifier.padding(FmSpace.x3), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)).background(c.bgRaised)) {
                    PreviewRow("Field notes", "Updated a minute ago", selected = true)
                    Box(Modifier.fillMaxWidth().height(1.dp).background(c.border))
                    PreviewRow("Release plan", "3 open tasks", selected = false)
                }
                Text("Open the project page", style = Fm.text.bodyMedium.copy(color = c.accent))
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    PreviewChip("Done", c.success)
                    PreviewChip("Waiting", c.warning)
                    PreviewChip("Failed", c.danger)
                }
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Notifications", style = Fm.text.bodyMedium)
                        Text("Hints and captions", style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    }
                    FmSwitch(checked = true, onCheckedChange = {})
                }
                FmToneButton("Save", onPressed = {}, tone = FmTone.Accent, filled = true)
            }
        }
    }
}

@Composable
private fun PreviewRow(title: String, subtitle: String, selected: Boolean) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().background(if (selected) c.accentSubtle else Color.Transparent).padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyMedium.copy(fontWeight = FmType.medium))
            Text(subtitle, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
    }
}

@Composable
private fun PreviewChip(label: String, color: Color) {
    Text(
        label,
        modifier = Modifier
            .clip(RoundedCornerShape(FmRadius.full))
            .background(color.copy(alpha = if (Fm.dark) .20f else .13f))
            .padding(horizontal = FmSpace.x2, vertical = 2.dp),
        style = Fm.text.labelMedium.copy(color = color),
    )
}

@Composable
private fun ThemeTokenRow(token: ThemeToken, resolved: ResolvedTheme, edited: Boolean, onEdit: () -> Unit, onReset: () -> Unit) {
    val c = Fm.colors
    val argb = resolved[token]
    val opacity = ((argb shr 24) and 0xFF) * 100 / 255
    val hex = ThemeColor.hex(argb) + if (opacity < 100) " · $opacity%" else ""
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(role = Role.Button, onClick = onEdit)
            .semantics(mergeDescendants = true) { contentDescription = "${token.label}, $hex" }
            .testTag("theme-token-${token.name}")
            .padding(vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(32.dp)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(Color(argb))
                .border(1.dp, c.border, RoundedCornerShape(FmRadius.md)),
        )
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(token.label, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyLarge)
            Text(hex, style = Fm.mono(color = c.textSecondary))
        }
        if (edited) {
            FmIconButton("undo", "Reset ${token.label.lowercase()}", onPressed = onReset, color = c.accent)
        }
    }
}

@Composable
internal fun ThemeColorPickerDialog(title: String, initial: Long, onDismiss: () -> Unit, onPick: (Long) -> Unit) {
    val c = Fm.colors
    val start = remember(initial) { ThemeColor.hsl(initial) }
    var hue by remember(initial) { mutableFloatStateOf(start.hue.toFloat()) }
    var saturation by remember(initial) { mutableFloatStateOf(start.saturation.toFloat()) }
    var lightness by remember(initial) { mutableFloatStateOf(start.lightness.toFloat()) }
    var hexText by remember(initial) { mutableStateOf(ThemeColor.hex(initial)) }
    val typed = ThemeColor.parseHex(hexText)
    val rgb = typed ?: ThemeColor.fromHsl(hue.toDouble(), saturation.toDouble(), lightness.toDouble())
    fun fromSliders(h: Float, s: Float, l: Float) {
        hue = h
        saturation = s
        lightness = l
        hexText = ThemeColor.hex(ThemeColor.fromHsl(h.toDouble(), s.toDouble(), l.toDouble()))
    }
    val sliderColors = SliderDefaults.colors(thumbColor = c.accent, activeTrackColor = c.accent, inactiveTrackColor = c.bgElevated)
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = title,
        actions = {
            FmDialogActions(onCancel = onDismiss, onConfirm = if (typed != null) ({ onPick(rgb) }) else null, confirmLabel = "Use colour")
        },
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(56.dp)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(Color(0xFF000000 or rgb))
                .border(1.dp, c.border, RoundedCornerShape(FmRadius.md))
                .testTag("theme-picker-swatch"),
        )
        Spacer(Modifier.height(FmSpace.x3))
        PickerSlider("Hue", "${hue.toInt()}°", hue, 0f..360f, sliderColors) { fromSliders(it, saturation, lightness) }
        PickerSlider("Saturation", "${(saturation * 100).toInt()}%", saturation, 0f..1f, sliderColors) { fromSliders(hue, it, lightness) }
        PickerSlider("Lightness", "${(lightness * 100).toInt()}%", lightness, 0f..1f, sliderColors) { fromSliders(hue, saturation, it) }
        Spacer(Modifier.height(FmSpace.x2))
        FmTextField(
            hexText,
            { text ->
                hexText = text
                ThemeColor.parseHex(text)?.let { parsed ->
                    val hsl = ThemeColor.hsl(parsed)
                    if (hsl.saturation > 0) hue = hsl.hue.toFloat()
                    saturation = hsl.saturation.toFloat()
                    lightness = hsl.lightness.toFloat()
                }
            },
            Modifier.fillMaxWidth().testTag("theme-picker-hex"),
            hint = "#rrggbb",
            errorText = if (typed == null) "Use six hex digits, like #0066cc." else null,
            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, keyboardType = KeyboardType.Ascii, autoCorrectEnabled = false),
            textStyle = Fm.mono(size = FmType.body),
        )
    }
}

@Composable
private fun PickerSlider(
    label: String,
    value: String,
    current: Float,
    range: ClosedFloatingPointRange<Float>,
    colors: SliderColors,
    onChange: (Float) -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth()) {
            Text(label, style = Fm.text.labelMedium.copy(color = Fm.colors.textSecondary), modifier = Modifier.weight(1f))
            Text(value, style = Fm.mono(color = Fm.colors.textSecondary))
        }
        Slider(
            value = current.coerceIn(range.start, range.endInclusive),
            onValueChange = onChange,
            valueRange = range,
            colors = colors,
            modifier = Modifier.fillMaxWidth().testTag("theme-picker-${label.lowercase()}"),
        )
    }
}
