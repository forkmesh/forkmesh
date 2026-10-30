package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.platform.WallpaperStore
import com.forkmesh.nativeapp.ui.theme.Fm

@Composable
fun FmWallpaperLayer(modifier: Modifier = Modifier.fillMaxSize()) {
    val appearance by AppearanceStore.appearance.collectAsState()
    val wallpaper = appearance.wallpaper
    if (!wallpaper.active) return
    val context = LocalContext.current
    LaunchedEffect(wallpaper.image) { WallpaperStore.load(context, wallpaper.image) }
    Box(modifier.background(Fm.colors.bgBase)) {
        val bitmap = WallpaperStore.forName(wallpaper.image)
        if (bitmap != null) {
            Image(bitmap, contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            Box(Modifier.fillMaxSize().background(Fm.colors.bgBase.copy(alpha = wallpaper.dim.toFloat())))
        }
    }
}
