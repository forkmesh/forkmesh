package com.forkmesh.nativeapp.feature.securedrive

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.ContextWrapper
import android.view.WindowManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.chat.ChatPrivacy
import com.forkmesh.nativeapp.feature.market.PluginMenuButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.text.DateFormat
import java.util.Date
import java.util.Locale

internal fun Context.findActivity(): Activity? {
    var current: Context? = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

@Composable
internal fun SdSecureWindow() {
    val activity = LocalContext.current.findActivity() ?: return
    DisposableEffect(activity) {
        activity.window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        onDispose { ChatPrivacy.applyWindow(activity, ChatPrivacy.hideInSwitcher.value) }
    }
}

@Composable
internal fun SdLockedGuard(navigator: Navigator) {
    val phase by SecureDriveFeature.service.phase.collectAsState()
    LaunchedEffect(phase) {
        if (phase != SdPhase.Unlocked) navigator.pop()
    }
}

@Composable
internal fun SdScaffold(
    title: String,
    navigator: Navigator,
    subtitle: String? = null,
    secure: Boolean = true,
    onBack: (() -> Unit)? = null,
    actions: @Composable RowScope.() -> Unit = {},
    content: @Composable BoxScope.() -> Unit,
) {
    val service = SecureDriveFeature.service
    val busy by service.busy.collectAsState()
    val phase by service.phase.collectAsState()
    if (secure) SdSecureWindow()
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxSize()
            .background(c.bgBase)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        awaitPointerEvent(PointerEventPass.Initial)
                        service.touch()
                    }
                }
            },
    ) {
        Column(Modifier.fillMaxWidth().background(c.bgRaised).windowInsetsPadding(fmSafeAreaInsets(bottom = false))) {
            Row(Modifier.fillMaxWidth().height(56.dp).padding(end = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.width(56.dp), contentAlignment = Alignment.Center) {
                    FmIconButton("arrow_back", "Back", onPressed = { if (onBack != null) onBack() else navigator.pop() }, color = c.textPrimary, iconSize = 24.dp)
                }
                Column(Modifier.weight(1f)) {
                    Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleLarge)
                    if (!subtitle.isNullOrEmpty()) {
                        Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    }
                }
                actions()
                if (phase == SdPhase.Unlocked) {
                    FmIconButton("lock", "Lock", onPressed = { service.lock() }, color = c.textPrimary)
                }
                PluginMenuButton()
            }
            HorizontalDivider(thickness = 1.dp, color = c.border)
        }
        val work = busy
        if (work != null) {
            FmLinearProgressIndicator(Modifier.fillMaxWidth())
            Text(work, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        Box(Modifier.weight(1f).fillMaxWidth(), content = content)
    }
}

@Composable
internal fun SdSafetyNumber(number: String, modifier: Modifier = Modifier) {
    Text(number, modifier, style = Fm.text.titleLarge.copy(fontFamily = FontFamily.Monospace, color = Fm.colors.textPrimary))
}

@Composable
internal fun SdBanner(lines: List<String>, danger: Boolean = true) {
    if (lines.isEmpty()) return
    val c = Fm.colors
    val tone = if (danger) c.danger else c.warning
    Column(
        Modifier
            .fillMaxWidth()
            .background(tone.copy(alpha = .1f))
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        Text(if (danger) "Problems found on the drive" else "Note", style = Fm.text.titleSmall.copy(color = tone))
        for (line in lines) Text(line, style = Fm.text.bodySmall.copy(color = c.textPrimary))
    }
}

@Composable
internal fun SdSpacer() = Spacer(Modifier.height(FmSpace.x3))

internal fun sdTime(ms: Long): String =
    if (ms <= 0) "" else DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(ms))

internal fun sdBytes(bytes: Long): String = when {
    bytes < 1024 -> "$bytes bytes"
    bytes < 1024 * 1024 -> String.format(Locale.getDefault(), "%.1f KB", bytes / 1024.0)
    bytes < 1024L * 1024 * 1024 -> String.format(Locale.getDefault(), "%.1f MB", bytes / (1024.0 * 1024))
    else -> String.format(Locale.getDefault(), "%.2f GB", bytes / (1024.0 * 1024 * 1024))
}

internal fun sdPlural(n: Int, word: String): String = "$n $word${if (n == 1) "" else "s"}"

internal fun sdCopy(context: Context, text: String) {
    val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText("Secure Drive", text))
}

internal fun sdKindIcon(kind: String): String = when (kind) {
    "document" -> "description"
    "spreadsheet" -> "grid_view"
    "drawing" -> "draw"
    "image" -> "image"
    "text" -> "notes"
    else -> "folder"
}
