package com.forkmesh.nativeapp.feature.account.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.platform.WallpaperStore
import com.forkmesh.nativeapp.settings.Wallpaper
import com.forkmesh.nativeapp.shell.FmIosSlider
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

@Composable
fun AppearanceBackgroundRow() {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val haptics = FmHaptics.current
    val appearance by AppearanceStore.appearance.collectAsState()
    val wallpaper = appearance.wallpaper
    var saving by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(wallpaper.image) { WallpaperStore.load(context, wallpaper.image) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        error = null
        saving = true
        scope.launch {
            val saved = WallpaperStore.save(context, uri)
            saving = false
            if (saved == null) {
                error = "That image could not be read. Try a PNG, JPEG or HEIC."
                return@launch
            }
            val previous = AppearanceStore.appearance.value.wallpaper.image
            AppearanceStore.setWallpaperImage(saved)
            if (previous.isNotEmpty() && previous != saved) WallpaperStore.delete(context, previous)
            haptics.selection()
        }
    }
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
            val shape = RoundedCornerShape(FmRadius.md)
            Box(
                Modifier
                    .size(width = 48.dp, height = 84.dp)
                    .clip(shape)
                    .background(c.bgOverlay)
                    .border(1.dp, c.border, shape)
                    .semantics { contentDescription = "Background preview" },
                contentAlignment = Alignment.Center,
            ) {
                val bitmap = WallpaperStore.forName(wallpaper.image)
                when {
                    bitmap != null -> Image(bitmap, contentDescription = null, modifier = Modifier.size(width = 48.dp, height = 84.dp), contentScale = ContentScale.Crop)
                    saving -> FmCircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                    else -> FmIcon("image_outlined", size = 22.dp, tint = c.textTertiary)
                }
            }
            Column {
                BackgroundAction(
                    text = if (wallpaper.active) "Change image" else "Choose image…",
                    color = c.accent,
                    enabled = !saving,
                    tag = "appearance-background-choose",
                ) {
                    picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                }
                if (wallpaper.active) {
                    BackgroundAction(text = "Remove", color = c.textSecondary, enabled = !saving, tag = "appearance-background-remove") {
                        val previous = wallpaper.image
                        error = null
                        AppearanceStore.setWallpaperImage("")
                        WallpaperStore.delete(context, previous)
                        scope.launch { WallpaperStore.load(context, "") }
                        haptics.selection()
                    }
                }
            }
        }
        error?.let { Text(it, style = Fm.text.bodySmall.copy(color = c.danger)) }
        if (wallpaper.active) DimSlider(wallpaper.dim)
        SettingsNote("Shown behind every screen on this phone. Dim keeps text readable over a busy picture.")
    }
}

@Composable
private fun BackgroundAction(text: String, color: androidx.compose.ui.graphics.Color, enabled: Boolean, tag: String, onClick: () -> Unit) {
    Box(
        Modifier
            .heightIn(min = 44.dp)
            .testTag(tag)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(text, style = Fm.text.labelLarge.copy(color = if (enabled) color else color.copy(alpha = 0.45f)))
    }
}

@Composable
private fun DimSlider(dim: Double) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("Dim", style = Fm.text.bodyMedium, modifier = Modifier.weight(1f))
            Text("${Wallpaper.percent(dim)}%", style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        FmIosSlider(
            value = (dim / Wallpaper.MAX_DIM).toFloat(),
            onValueChange = { AppearanceStore.previewWallpaperDim(Wallpaper.snapDim(it * Wallpaper.MAX_DIM)) },
            onValueChangeFinished = {
                AppearanceStore.setWallpaperDim(AppearanceStore.appearance.value.wallpaper.dim)
                haptics.selection()
            },
            modifier = Modifier
                .fillMaxWidth()
                .testTag("appearance-background-dim")
                .semantics { contentDescription = "Background dim ${Wallpaper.percent(dim)} percent" },
        )
    }
}
