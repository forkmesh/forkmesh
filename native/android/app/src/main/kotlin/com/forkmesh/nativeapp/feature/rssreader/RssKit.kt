package com.forkmesh.nativeapp.feature.rssreader

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.rssreader.RssAppearance
import com.forkmesh.nativeapp.rssreader.RssFeed
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.ForkMeshTheme

internal object RssColors {
    val today = Color(0xFFFF9500)
    val unread = Color(0xFF2F7CF6)
    val star = Color(0xFFF5B800)
}

@Composable
internal fun RssThemed(content: @Composable () -> Unit) {
    val service = RssReaderFeature.service
    service.version.collectAsState().value
    val appearance = service.library?.settings()?.appearance ?: RssAppearance.AUTOMATIC
    val dark = Fm.dark
    when {
        appearance == RssAppearance.LIGHT && dark -> ForkMeshTheme(dark = false, messenger = false, content = content)
        appearance == RssAppearance.DARK && !dark -> ForkMeshTheme(dark = true, messenger = false, content = content)
        else -> content()
    }
}

@Composable
internal fun RssScaffold(
    title: String,
    navigator: Navigator,
    subtitle: String? = null,
    actions: @Composable RowScope.() -> Unit = {},
    bottomBar: (@Composable RowScope.() -> Unit)? = null,
    content: @Composable BoxScope.() -> Unit,
) {
    RssThemed {
        val c = Fm.colors
        Column(Modifier.fillMaxSize().background(c.bgBase)) {
            Column(Modifier.fillMaxWidth().background(c.bgRaised).windowInsetsPadding(fmSafeAreaInsets(bottom = false))) {
                Row(Modifier.fillMaxWidth().height(56.dp).padding(end = FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.width(56.dp), contentAlignment = Alignment.Center) {
                        FmIconButton("arrow_back", "Back", onPressed = { navigator.pop() }, color = c.textPrimary, iconSize = 24.dp)
                    }
                    Spacer(Modifier.width(FmSpace.x2))
                    Column(Modifier.weight(1f)) {
                        Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleLarge)
                        if (!subtitle.isNullOrEmpty()) {
                            Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                        }
                    }
                    actions()
                }
                HorizontalDivider(thickness = 1.dp, color = c.border)
            }
            Box(Modifier.weight(1f).fillMaxWidth(), content = content)
            if (bottomBar != null) {
                Column(Modifier.fillMaxWidth().background(c.bgRaised).windowInsetsPadding(fmSafeAreaInsets(top = false))) {
                    HorizontalDivider(thickness = 1.dp, color = c.border)
                    Row(
                        Modifier.fillMaxWidth().height(52.dp).padding(horizontal = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                        content = bottomBar,
                    )
                }
            }
        }
    }
}

@Composable
internal fun FeedIcon(feed: RssFeed?, size: Dp = 22.dp, radius: Dp = 5.dp) {
    val icons by RssReaderFeature.service.icons.collectAsState()
    val bitmap = feed?.let { icons[it.id] }
    val shape = RoundedCornerShape(radius)
    if (bitmap != null) {
        Image(bitmap, contentDescription = null, modifier = Modifier.size(size).clip(shape), contentScale = ContentScale.Crop)
    } else {
        val letter = feed?.displayTitle?.trim()?.firstOrNull()?.uppercaseChar()?.toString() ?: "?"
        Box(Modifier.size(size).clip(shape).background(Fm.colors.accent), contentAlignment = Alignment.Center) {
            Text(letter, style = Fm.text.labelMedium.copy(color = Color.White, fontWeight = FontWeight.Bold, fontSize = (size.value * 0.5f).sp))
        }
    }
}

@Composable
internal fun UnreadDot(visible: Boolean, size: Dp = 9.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(if (visible) RssColors.unread else Color.Transparent))
}

@Composable
internal fun SettingsHeader(title: String) {
    Text(
        title.uppercase(),
        modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x5, bottom = FmSpace.x2),
        style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary),
    )
}

@Composable
internal fun SettingsRow(
    title: String,
    value: String? = null,
    subtitle: String? = null,
    icon: String? = null,
    titleColor: Color? = null,
    onClick: (() -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgRaised)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .heightIn(min = 52.dp)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 20.dp, tint = c.textSecondary)
            Spacer(Modifier.width(FmSpace.x3))
        }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = Fm.text.bodyLarge.copy(color = titleColor ?: c.textPrimary))
            if (!subtitle.isNullOrEmpty()) Text(subtitle, style = Fm.text.bodySmall.copy(color = c.textTertiary))
        }
        if (value != null) {
            Spacer(Modifier.width(FmSpace.x2))
            Text(value, style = Fm.text.bodyMedium.copy(color = c.textTertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (trailing != null) {
            Spacer(Modifier.width(FmSpace.x2))
            trailing()
        }
    }
}

@Composable
internal fun SettingsSwitch(title: String, checked: Boolean, subtitle: String? = null, onChange: (Boolean) -> Unit) {
    SettingsRow(title, subtitle = subtitle, onClick = { onChange(!checked) }) { FmSwitch(checked, onCheckedChange = onChange) }
}

internal fun openInBrowser(context: Context, url: String) {
    if (!(url.startsWith("https://") || url.startsWith("http://") || url.startsWith("mailto:"))) return
    runCatching {
        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}

internal fun shareText(context: Context, subject: String, text: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_SUBJECT, subject).putExtra(Intent.EXTRA_TEXT, text)
    runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}
