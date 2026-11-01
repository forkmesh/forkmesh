package com.forkmesh.nativeapp.feature.mastodon

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withLink
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.market.MarketImages
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.mastodon.MastodonAttachment
import com.forkmesh.nativeapp.mastodon.MastodonNotification
import com.forkmesh.nativeapp.mastodon.MastodonPoll
import com.forkmesh.nativeapp.mastodon.MastodonSegment
import com.forkmesh.nativeapp.mastodon.MastodonStatus
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.appendLink
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal class MastodonActions(
    val me: String,
    val openThread: (MastodonStatus) -> Unit,
    val openProfile: (String) -> Unit,
    val openTag: (String) -> Unit,
    val openLink: (String) -> Unit,
    val reply: (MastodonStatus) -> Unit,
    val boost: (MastodonStatus) -> Unit,
    val favourite: (MastodonStatus) -> Unit,
    val bookmark: (MastodonStatus) -> Unit,
    val delete: (MastodonStatus) -> Unit,
    val copyLink: (MastodonStatus) -> Unit,
)

@Composable
internal fun MastodonImage(url: String, modifier: Modifier, description: String?, maxWidth: Int = MarketImages.THUMBNAIL_WIDTH) {
    var image by remember(url, maxWidth) { mutableStateOf(if (url.isEmpty()) null else MarketImages.cached(url, maxWidth)) }
    LaunchedEffect(url, maxWidth) {
        if (image == null && url.startsWith("https://")) image = withContext(Dispatchers.IO) { MarketImages.load(url, maxWidth) }
    }
    Box(modifier.background(Fm.colors.bgOverlay), contentAlignment = Alignment.Center) {
        image?.let { Image(it, contentDescription = description, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop) }
    }
}

@Composable
internal fun MastodonAvatar(url: String, name: String, size: Dp = 40.dp, onClick: (() -> Unit)? = null) {
    val c = Fm.colors
    val pixels = with(LocalDensity.current) { size.roundToPx() }
    var image by remember(url) { mutableStateOf(if (url.isEmpty()) null else MarketImages.cached(url, pixels)) }
    LaunchedEffect(url) {
        if (image == null && url.startsWith("https://")) image = withContext(Dispatchers.IO) { MarketImages.load(url, pixels) }
    }
    Box(
        Modifier
            .size(size)
            .clip(CircleShape)
            .background(c.bgOverlay)
            .border(1.dp, c.border, CircleShape)
            .then(if (onClick != null) Modifier.clickable(onClickLabel = "Open profile", onClick = onClick) else Modifier),
        contentAlignment = Alignment.Center,
    ) {
        val bitmap = image
        if (bitmap != null) {
            Image(bitmap, contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        } else {
            Text(
                name.take(1).uppercase().ifEmpty { "?" },
                style = (if (size > 44.dp) Fm.text.titleMedium else Fm.text.titleSmall).copy(color = c.textSecondary, fontWeight = FontWeight.SemiBold),
            )
        }
    }
}

@Composable
internal fun MastodonPostRow(entry: MastodonStatus, focused: Boolean, actions: MastodonActions) {
    val c = Fm.colors
    val post = MastodonState.current(entry.shown)
    val booster = entry.takeIf { it.reblog != null }?.account
    val revealed = MastodonState.revealed[post.id] == true
    Column(
        Modifier
            .fillMaxWidth()
            .background(if (focused) c.bgRaised else Color.Transparent)
            .clickable(enabled = !focused, onClickLabel = "Open thread") { actions.openThread(post) }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
    ) {
        if (booster != null) {
            Row(
                Modifier
                    .padding(start = 52.dp, bottom = FmSpace.x2)
                    .clickable { actions.openProfile(booster.id) },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("repeat", size = 14.dp, tint = c.textTertiary)
                Spacer(Modifier.width(FmSpace.x1))
                Text("${booster.displayName} boosted", maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
        Row {
            MastodonAvatar(post.account.avatar, post.account.displayName, if (focused) 48.dp else 40.dp) { actions.openProfile(post.account.id) }
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                AuthorLine(post) { actions.openProfile(post.account.id) }
                if (post.spoilerText.isNotEmpty()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("warning_amber", size = 16.dp, tint = c.warning)
                        Spacer(Modifier.width(FmSpace.x2))
                        Text(post.spoilerText, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FontWeight.Medium))
                        FmToneButton(
                            if (revealed) "Show less" else "Show more",
                            onPressed = { MastodonState.revealed[post.id] = !revealed },
                            height = 30.dp,
                        )
                    }
                }
                if (post.spoilerText.isEmpty() || revealed) {
                    MastodonBody(post.segments, focused, actions)
                    if (post.media.isNotEmpty()) MastodonMedia(post, actions)
                    post.poll?.let { MastodonPollView(it) }
                }
                PostActions(post, actions)
            }
        }
    }
}

@Composable
private fun AuthorLine(post: MastodonStatus, onClick: () -> Unit) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Row(Modifier.weight(1f).clickable(onClickLabel = "Open profile", onClick = onClick), verticalAlignment = Alignment.CenterVertically) {
            Text(
                post.account.displayName,
                Modifier.weight(1f, fill = false),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold),
            )
            Spacer(Modifier.width(FmSpace.x1))
            Text(
                "@${post.account.acct}",
                Modifier.weight(1f, fill = false),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textTertiary),
            )
        }
        if (post.createdAt > 0) {
            Spacer(Modifier.width(FmSpace.x2))
            Text(relativeTime(post.createdAt), style = Fm.text.bodySmall.copy(color = c.textTertiary))
        }
    }
}

@Composable
internal fun MastodonBody(segments: List<MastodonSegment>, focused: Boolean, actions: MastodonActions) {
    if (segments.isEmpty()) return
    val accent = Fm.colors.accent
    val annotated = remember(segments, accent) {
        AnnotatedString.Builder().apply {
            for (segment in segments) {
                when (segment) {
                    is MastodonSegment.Text -> append(segment.text)
                    is MastodonSegment.Link -> appendLink(segment.label, segment.url, accent, actions.openLink)
                    is MastodonSegment.Mention -> withLink(
                        LinkAnnotation.Clickable("mention:${segment.accountId}", TextLinkStyles(SpanStyle(color = accent))) { actions.openProfile(segment.accountId) },
                    ) { append(segment.label) }
                    is MastodonSegment.Hashtag -> withLink(
                        LinkAnnotation.Clickable("tag:${segment.name}", TextLinkStyles(SpanStyle(color = accent))) { actions.openTag(segment.name) },
                    ) { append(segment.label) }
                }
            }
        }.toAnnotatedString()
    }
    val style = if (focused) Fm.text.bodyLarge else Fm.text.bodyMedium
    Text(annotated, style = style.copy(color = Fm.colors.textPrimary))
}

@Composable
private fun MastodonMedia(post: MastodonStatus, actions: MastodonActions) {
    val c = Fm.colors
    val key = "media:${post.id}"
    val hidden = post.sensitive && MastodonState.revealed[key] != true
    if (hidden) {
        Box(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 56.dp)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(c.bgOverlay)
                .clickable(onClickLabel = "Show sensitive media") { MastodonState.revealed[key] = true },
            contentAlignment = Alignment.Center,
        ) {
            Text("Sensitive media · tap to show", style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        return
    }
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        for (item in post.media) MediaTile(item, single = post.media.size == 1, open = actions.openLink)
    }
}

@Composable
private fun MediaTile(item: MastodonAttachment, single: Boolean, open: (String) -> Unit) {
    val target = item.url.ifEmpty { item.previewUrl }
    val modifier = Modifier
        .then(if (single) Modifier.fillMaxWidth().aspectRatio(16f / 9f) else Modifier.size(140.dp))
        .clip(RoundedCornerShape(FmRadius.md))
        .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.md))
        .clickable(enabled = target.isNotEmpty(), onClickLabel = "Open media") { open(target) }
    Box {
        MastodonImage(item.previewUrl.ifEmpty { item.url }, modifier, item.description.ifEmpty { "Attachment" }, if (single) MarketImages.FULL_WIDTH else MarketImages.THUMBNAIL_WIDTH)
        if (item.type == "video" || item.type == "gifv" || item.type == "audio") {
            FmIcon("play_circle", size = 32.dp, tint = Color.White, modifier = Modifier.align(Alignment.Center))
        }
    }
}

@Composable
private fun MastodonPollView(poll: MastodonPoll) {
    val c = Fm.colors
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        for (option in poll.options) {
            val percent = poll.percent(option)
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(32.dp)
                    .clip(RoundedCornerShape(FmRadius.sm))
                    .background(c.bgOverlay),
            ) {
                Box(Modifier.fillMaxWidth(percent / 100f).height(32.dp).background(c.accent.copy(alpha = 0.22f)))
                Row(Modifier.fillMaxSize().padding(horizontal = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    Text(option.title, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textPrimary))
                    Text("$percent%", style = Fm.text.bodySmall.copy(color = c.textSecondary, fontFeatureSettings = FmType.tabular))
                }
            }
        }
        Text(
            (if (poll.votesCount == 1) "1 vote" else "${poll.votesCount} votes") + if (poll.expired) " · Closed" else "",
            style = Fm.text.bodySmall.copy(color = c.textTertiary),
        )
    }
}

@Composable
private fun PostActions(post: MastodonStatus, actions: MastodonActions) {
    val c = Fm.colors
    Row(Modifier.padding(top = FmSpace.x1).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        PostAction("reply", "Reply", post.repliesCount, c.textTertiary) { actions.reply(post) }
        Spacer(Modifier.width(FmSpace.x2))
        PostAction("repeat", if (post.reblogged) "Undo boost" else "Boost", post.reblogsCount, if (post.reblogged) c.accent else c.textTertiary) { actions.boost(post) }
        Spacer(Modifier.width(FmSpace.x2))
        PostAction(
            if (post.favourited) "star" else "star_border",
            if (post.favourited) "Remove favourite" else "Favourite",
            post.favouritesCount,
            if (post.favourited) c.warning else c.textTertiary,
        ) { actions.favourite(post) }
        Spacer(Modifier.width(FmSpace.x2))
        PostAction(
            if (post.bookmarked) "bookmark" else "bookmark_border",
            if (post.bookmarked) "Remove bookmark" else "Bookmark",
            0,
            if (post.bookmarked) c.accent else c.textTertiary,
        ) { actions.bookmark(post) }
        Spacer(Modifier.weight(1f))
        PostMenu(post, actions)
    }
}

@Composable
private fun PostMenu(post: MastodonStatus, actions: MastodonActions) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmIconButton("more_horiz", "More", onPressed = { open = true }, color = Fm.colors.textTertiary, iconSize = 18.dp)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (post.url.isNotEmpty()) {
                FmDropdownMenuItem("Open in browser", onClick = { open = false; actions.openLink(post.url) }, icon = "public")
                FmDropdownMenuItem("Copy link", onClick = { open = false; actions.copyLink(post) }, icon = "link")
            }
            if (post.account.id == actions.me) {
                FmDropdownMenuItem("Delete", onClick = { open = false; actions.delete(post) }, icon = "delete_outline", textColor = Fm.colors.danger)
            }
        }
    }
}

@Composable
private fun PostAction(icon: String, label: String, count: Int, tint: Color, onClick: () -> Unit) {
    Row(
        Modifier
            .heightIn(min = 44.dp)
            .widthIn(min = 44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(role = Role.Button, onClickLabel = label, onClick = onClick)
            .clearAndSetSemantics { contentDescription = if (count > 0) "$label, $count" else label }
            .padding(horizontal = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 18.dp, tint = tint)
        if (count > 0) {
            Spacer(Modifier.width(FmSpace.x1))
            Text("$count", style = Fm.text.bodySmall.copy(color = tint, fontFeatureSettings = FmType.tabular))
        }
    }
}

@Composable
internal fun MastodonNotificationRow(note: MastodonNotification, actions: MastodonActions) {
    val c = Fm.colors
    val status = note.status?.let { MastodonState.current(it) }
    val icon = when (note.type) {
        "favourite" -> "star"
        "reblog" -> "repeat"
        "follow", "follow_request" -> "person_add"
        "mention" -> "alternate_email"
        "poll" -> "poll"
        "update" -> "edit"
        else -> "notifications"
    }
    Column(
        Modifier
            .fillMaxWidth()
            .clickable(onClickLabel = if (status != null) "Open thread" else "Open profile") {
                when {
                    status != null -> actions.openThread(status)
                    else -> note.account?.let { actions.openProfile(it.id) }
                }
            }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmIcon(icon, size = 18.dp, tint = if (note.type == "favourite") c.warning else c.accent)
            Spacer(Modifier.width(FmSpace.x3))
            note.account?.let { MastodonAvatar(it.avatar, it.displayName, 28.dp) { actions.openProfile(it.id) } }
            Spacer(Modifier.width(FmSpace.x2))
            Text(note.sentence, Modifier.weight(1f), maxLines = 2, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
            if (note.createdAt > 0) {
                Spacer(Modifier.width(FmSpace.x2))
                Text(relativeTime(note.createdAt), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
        if (status != null) {
            Box(Modifier.padding(start = 30.dp)) {
                Text(
                    status.spoilerText.ifEmpty { status.segments.joinToString("") { it.label } },
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
        }
    }
}
