package com.forkmesh.nativeapp.feature.tools

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.draw.clip
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.tools.ChangeNotifier
import com.forkmesh.nativeapp.tools.LogCategory
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm

private val mainHandler = Handler(Looper.getMainLooper())

@Composable
internal fun ChangeNotifier.observe(): Int {
    var ticks by remember(this) { mutableIntStateOf(0) }
    DisposableEffect(this) {
        val remove = addListener { mainHandler.post { ticks++ } }
        onDispose { remove() }
    }
    return ticks
}

internal fun LogCategory.iconName(): String = when (this) {
    LogCategory.Stall -> "hourglass_bottom_rounded"
    LogCategory.Error -> "error_outline_rounded"
    LogCategory.Net -> "cloud_outlined"
    LogCategory.Relay -> "cell_tower_rounded"
    LogCategory.Sync -> "sync_rounded"
    LogCategory.Auth -> "key_outlined"
    LogCategory.Push -> "notifications_none_rounded"
    LogCategory.Perf -> "speed_rounded"
    LogCategory.Info -> "info_outline_rounded"
}

@Composable
internal fun logCategoryColor(category: LogCategory, count: Int): Color {
    val c = Fm.colors
    if (count == 0) return c.textTertiary
    return when (category) {
        LogCategory.Error -> c.danger
        LogCategory.Stall -> c.warning
        else -> c.textPrimary
    }
}

@Composable
internal fun ToolsAppBar(title: String, navigator: Navigator, actions: @Composable RowScope.() -> Unit = {}) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .windowInsetsPadding(fmSafeAreaInsets(bottom = false))
            .height(56.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.width(56.dp), contentAlignment = Alignment.Center) {
            FmIconButton("arrow_back", "Back", onPressed = { navigator.pop() }, color = c.textPrimary, iconSize = 24.dp)
        }
        Spacer(Modifier.width(16.dp))
        Text(title, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleLarge)
        actions()
        Spacer(Modifier.width(4.dp))
    }
}

internal fun copyToClipboard(context: Context, text: String) {
    val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText("ForkMesh", text))
}

@Composable
internal fun ToolSheetRow(icon: String, label: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(onClick = onClick)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 28.dp, tint = Fm.colors.textTertiary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(
            label,
            style = MaterialTheme.typography.titleSmall,
            color = Fm.colors.textSecondary,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        FmIcon("chevron_right", size = 20.dp, tint = Fm.colors.textDisabled)
    }
}
