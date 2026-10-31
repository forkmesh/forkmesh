package com.forkmesh.nativeapp.feature.chat

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.animation.Crossfade
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.chat.ChatLinkKind
import com.forkmesh.nativeapp.chat.ChatLinkPreview
import com.forkmesh.nativeapp.chat.ChatLinkPreviewParsing
import com.forkmesh.nativeapp.chat.ChatLinkPreviewPolicy
import com.forkmesh.nativeapp.chat.ChatLinkPreviewService
import com.forkmesh.nativeapp.chat.ChatLinkPreviewState
import com.forkmesh.nativeapp.chat.ChatLinkSite
import com.forkmesh.nativeapp.chat.ChatLinkSites
import com.forkmesh.nativeapp.chat.ChatRow
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.net.URI
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ChatLinkPreviewUi {
    private const val MAX_BITMAPS = 120
    private const val LONGEST_SIDE = 1_200

    private val revisionState = MutableStateFlow(0)
    val revision: StateFlow<Int> = revisionState.asStateFlow()

    internal val bitmaps = LruCache<String, ImageBitmap>(MAX_BITMAPS)

    fun bind(service: ChatLinkPreviewService) {
        service.addListener { revisionState.value = revisionState.value + 1 }
    }

    fun clear() {
        bitmaps.evictAll()
        revisionState.value = revisionState.value + 1
    }

    internal fun decode(data: ByteArray): ImageBitmap? = runCatching {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(data, 0, data.size, bounds)
        val longest = maxOf(bounds.outWidth, bounds.outHeight)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return@runCatching null
        var sample = 1
        while (longest / (sample * 2) >= LONGEST_SIDE) sample *= 2
        val bitmap = BitmapFactory.decodeByteArray(data, 0, data.size, BitmapFactory.Options().apply { inSampleSize = sample })
            ?: return@runCatching null
        val side = maxOf(bitmap.width, bitmap.height)
        val fitted = if (side > LONGEST_SIDE) {
            val scale = LONGEST_SIDE.toFloat() / side
            Bitmap.createScaledBitmap(bitmap, maxOf(1, (bitmap.width * scale).toInt()), maxOf(1, (bitmap.height * scale).toInt()), true)
        } else {
            bitmap
        }
        fitted.asImageBitmap()
    }.getOrNull()
}

private val cardShape = RoundedCornerShape(14.dp)
private val cardIndent = FmSpace.x3 + 28.dp + FmSpace.x2

@Composable
fun ChatRowLinkPreviews(row: ChatRow) {
    if (row.text.isEmpty() || row.viewOnce != null) return
    val message = row.message
    val autoLoad = ChatLinkPreviewPolicy.autoLoads(message.conversation, message.self, ChatDepth.office?.room(message.conversation))
    ChatLinkPreviews(row.text, autoLoad, Modifier.padding(start = cardIndent, end = FmSpace.x3))
}

@Composable
fun ChatLinkPreviews(text: String, autoLoad: Boolean, modifier: Modifier = Modifier) {
    val service = ChatDepth.linkPreviews ?: return
    val revision by ChatLinkPreviewUi.revision.collectAsState()
    if (!service.enabled) return
    val urls = remember(text) { ChatLinkPreviewParsing.candidates(ChatLinkPreviewParsing.links(text)) }
    if (urls.isEmpty()) return
    Column(modifier, verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        for (url in urls) {
            key(url.toString()) { ChatLinkPreviewCard(url, service, autoLoad, revision) }
        }
    }
}

@Composable
fun ChatLinkPreviewCard(url: URI, service: ChatLinkPreviewService, autoLoad: Boolean, revision: Int) {
    val scope = rememberCoroutineScope()
    LaunchedEffect(url.toString(), autoLoad) {
        withContext(Dispatchers.IO) { if (autoLoad) service.load(url) else service.warm() }
    }
    val state = remember(url, revision) { service.state(url) }
    Crossfade(targetState = state, label = "chat-link-preview") { shown ->
        when {
            shown is ChatLinkPreviewState.Ready -> PreviewCard(shown.preview, url, service, Modifier.padding(top = FmSpace.x2))
            shown is ChatLinkPreviewState.Idle && !autoLoad -> LoadButton(url, Modifier.padding(top = FmSpace.x2)) {
                scope.launch(Dispatchers.IO) { service.load(url) }
            }
            shown is ChatLinkPreviewState.Loading && !autoLoad -> Text(
                "Loading preview…",
                modifier = Modifier.padding(top = FmSpace.x2),
                style = Fm.text.bodyMedium.copy(fontSize = 14.sp, color = Fm.colors.textSecondary),
            )
            else -> Spacer(Modifier.height(0.dp))
        }
    }
}

@Composable
private fun cardFill(): Color = Fm.colors.bgRaised.copy(alpha = if (Fm.colors.dark) 0.55f else 0.85f)

@Composable
private fun LoadButton(url: URI, modifier: Modifier, onLoad: () -> Unit) {
    val colors = Fm.colors
    Row(
        modifier
            .widthIn(max = 520.dp)
            .fillMaxWidth()
            .clip(cardShape)
            .background(cardFill())
            .border(1.dp, colors.border, cardShape)
            .clickable(onClickLabel = "Ask the site for a preview. The site sees this phone's address.", onClick = onLoad)
            .testTag("chat-link-preview-load")
            .padding(horizontal = 14.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ChatLinkSiteBadge(ChatLinkSites.site(url))
        Text(
            "Show preview of ${ChatLinkSites.bareHost(url.host.orEmpty())}",
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodyMedium.copy(fontSize = 14.sp, color = colors.accent),
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun PreviewCard(preview: ChatLinkPreview, url: URI, service: ChatLinkPreviewService, modifier: Modifier) {
    val colors = Fm.colors
    val context = LocalContext.current
    val open = rememberLinkOpener()
    var menu by remember { mutableStateOf(false) }
    val link = url.toString()
    Box(modifier) {
        Column(
            Modifier
                .widthIn(max = 520.dp)
                .fillMaxWidth()
                .clip(cardShape)
                .background(cardFill())
                .border(1.dp, colors.border, cardShape)
                .combinedClickable(onClickLabel = "Opens the link", onClick = { open(link) }, onLongClick = { menu = true })
                .semantics(mergeDescendants = true) { contentDescription = "${preview.siteName}: ${preview.title}" }
                .testTag("chat-link-preview"),
        ) {
            if (preview.kind != ChatLinkKind.Image || preview.imageUrl.isEmpty() || preview.title.isNotEmpty()) {
                Column(
                    Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    if (preview.title.isNotEmpty()) {
                        Text(
                            preview.title,
                            maxLines = 3,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyLarge.copy(fontSize = 16.sp, lineHeight = 21.sp, fontWeight = FontWeight.SemiBold, color = colors.textPrimary),
                        )
                    }
                    if (preview.author.isNotEmpty()) {
                        Text(
                            preview.author,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(fontSize = 15.sp, color = colors.textSecondary),
                        )
                    }
                    if (preview.detail.isNotEmpty() && (preview.author.isEmpty() || preview.imageUrl.isEmpty()) && !preview.isVideo) {
                        Text(
                            preview.detail,
                            maxLines = 3,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(fontSize = 14.sp, lineHeight = 19.sp, color = colors.textSecondary),
                        )
                    }
                    Row(
                        Modifier.padding(top = 2.dp),
                        horizontalArrangement = Arrangement.spacedBy(7.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        ChatLinkSiteBadge(preview.site)
                        Text(
                            preview.siteName,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodyMedium.copy(fontSize = 14.sp, color = colors.textSecondary),
                        )
                    }
                }
            }
            if (preview.imageUrl.isNotEmpty()) PreviewImage(preview.imageUrl, preview.isVideo, service)
        }
        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            FmDropdownMenuItem("Open link", icon = "open_in_new", onClick = {
                menu = false
                open(link)
            })
            FmDropdownMenuItem("Copy link", icon = "content_copy", onClick = {
                menu = false
                copyLink(context, link)
            })
            FmDropdownMenuItem("Share link", icon = "share", onClick = {
                menu = false
                shareLink(context, link)
            })
        }
    }
}

private fun copyLink(context: Context, link: String) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText("Link", link))
}

private fun shareLink(context: Context, link: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, link)
    runCatching { context.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
}

@Composable
fun ChatLinkSiteBadge(site: ChatLinkSite?) {
    val badge = RoundedCornerShape(5.dp)
    if (site != null) {
        Box(
            Modifier
                .size(width = 24.dp, height = 17.dp)
                .clip(badge)
                .background(Color(0xFF000000L or site.color))
                .border(0.5.dp, Color.White.copy(alpha = 0.18f), badge),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(site.icon, size = 12.dp, tint = Color.White)
        }
    } else {
        Box(Modifier.size(width = 24.dp, height = 17.dp), contentAlignment = Alignment.Center) {
            FmIcon("public", size = 14.dp, tint = Fm.colors.textSecondary)
        }
    }
}

@Composable
private fun PreviewImage(source: String, video: Boolean, service: ChatLinkPreviewService) {
    val colors = Fm.colors
    var image by remember(source) { mutableStateOf(ChatLinkPreviewUi.bitmaps.get(source)) }
    var failed by remember(source) { mutableStateOf(false) }
    LaunchedEffect(source) {
        if (image != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.IO) { service.imageData(source)?.let(ChatLinkPreviewUi::decode) }
        if (decoded == null) {
            failed = true
        } else {
            ChatLinkPreviewUi.bitmaps.put(source, decoded)
            image = decoded
        }
    }
    val bitmap = image
    when {
        failed -> Unit
        bitmap != null && video -> Box(Modifier.fillMaxWidth().aspectRatio(16f / 9f).clipToBounds()) {
            Image(bitmap, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            Box(
                Modifier.align(Alignment.Center).size(58.dp).background(Color.Black.copy(alpha = 0.55f), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("play_arrow", size = 30.dp, tint = Color.White)
            }
        }
        bitmap != null -> Box(Modifier.fillMaxWidth().background(colors.bgOverlay), contentAlignment = Alignment.Center) {
            Image(
                bitmap,
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.heightIn(max = 300.dp).aspectRatio(bitmap.width.toFloat() / maxOf(1, bitmap.height)),
            )
        }
        else -> Box(Modifier.fillMaxWidth().aspectRatio(if (video) 16f / 9f else 1.91f).background(colors.bgOverlay))
    }
}

@Composable
fun ChatLinkPreviewSettingRow() {
    val service = ChatDepth.linkPreviews ?: return
    val revision by ChatLinkPreviewUi.revision.collectAsState()
    val enabled = remember(revision) { service.enabled }
    HorizontalDivider(thickness = 1.dp, color = Fm.colors.border)
    FmListRow(
        title = "Link previews",
        modifier = Modifier.testTag("settings-chat-link-previews"),
        subtitle = "Fetches a title and picture for links in every chat, straight from the site.",
        leading = { FmIcon("link", tint = Fm.colors.textSecondary) },
        trailing = { FmSwitch(checked = enabled, onCheckedChange = { service.setEnabled(it) }) },
        onTap = { service.setEnabled(!enabled) },
        showChevron = false,
    )
}
