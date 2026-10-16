package com.forkmesh.nativeapp.feature.rssreader

import android.content.Context
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
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
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.rssreader.RssArticlePage
import com.forkmesh.nativeapp.rssreader.RssPageStyle
import com.forkmesh.nativeapp.rssreader.RssScope
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmMotion
import kotlinx.coroutines.launch

internal class RssArticleScreen(
    initialScope: RssScope,
    initialId: String,
    private val source: () -> List<String>,
) : Screen() {
    override val title: String get() = "Article"
    override val hidesShellChrome: Boolean get() = true
    private var scope by mutableStateOf(initialScope)
    private var currentId by mutableStateOf(initialId)
    private var list by mutableStateOf<List<String>?>(null)
    private var reader by mutableStateOf(false)
    private var readerBusy by mutableStateOf(false)
    private var readerFailed by mutableStateOf<String?>(null)
    private var pageDirection by mutableStateOf(1)
    private val status = FmStatus()

    private fun ids(): List<String> = list ?: source().also { list = it }

    private fun open(id: String, direction: Int = 1) {
        pageDirection = direction
        currentId = id
        reader = false
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        val context = LocalContext.current
        val coroutines = rememberCoroutineScope()
        LaunchedEffect(Unit) { service.load() }
        LaunchedEffect(currentId) {
            service.load()
            status.clear()
            if (service.library?.article(currentId)?.read == false) service.mutate { it.setRead(listOf(currentId), true) }
        }
        val library = service.library
        val article = remember(version, currentId) { library?.article(currentId) }
        val feed = remember(version, article?.feedId) { article?.let { library?.feed(it.feedId) } }
        val order = ids()
        val index = order.indexOf(currentId)
        RssScaffold(
            feed?.displayTitle ?: "",
            navigator,
            actions = {
                FmIconButton("keyboard_arrow_up", "Previous Article", onPressed = if (index > 0) ({ open(order[index - 1], -1) }) else null, iconSize = 28.dp)
                FmIconButton("keyboard_arrow_down", "Next Article", onPressed = if (index >= 0 && index < order.size - 1) ({ open(order[index + 1]) }) else null, iconSize = 28.dp)
            },
            bottomBar = {
                val accent = Fm.colors.accent
                FmIconButton(
                    if (article?.read == false) "circle" else "radio_button_unchecked",
                    if (article?.read == false) "Mark as Read" else "Mark as Unread",
                    onPressed = article?.let { a -> { service.mutate { it.setRead(listOf(a.id), !a.read) } } },
                    color = accent,
                )
                FmIconButton(
                    if (article?.starred == true) "star" else "star_border",
                    if (article?.starred == true) "Unstar" else "Star",
                    onPressed = article?.let { a -> { service.mutate { it.setStarred(listOf(a.id), !a.starred) } } },
                    color = if (article?.starred == true) RssColors.star else accent,
                )
                FmIconButton(
                    if (article?.readLater == true) "bookmark" else "bookmark_border",
                    if (article?.readLater == true) "Remove from Read Later" else "Read Later",
                    onPressed = article?.let { a -> { service.mutate { it.setReadLater(listOf(a.id), !a.readLater) } } },
                    color = accent,
                )
                Spacer(Modifier.weight(1f))
                FmTextButton(onClick = { nextUnread() }) { Text("Next Unread") }
                Spacer(Modifier.weight(1f))
                Box(contentAlignment = Alignment.Center) {
                    if (readerBusy) {
                        FmCircularProgressIndicator(Modifier.size(22.dp))
                    } else {
                        FmIconButton(
                            "notes",
                            if (reader) "Show Feed Content" else "Reader View",
                            onPressed = article?.let { { toggleReader(coroutines) } },
                            color = if (reader) accent else Fm.colors.textSecondary,
                        )
                    }
                }
                FmIconButton("share", "Share", onPressed = article?.let { a ->
                    { shareText(context, a.title, listOf(a.title, a.link).filter { it.isNotEmpty() }.joinToString("\n")) }
                }, color = accent)
            },
        ) {
            when {
                library == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                article == null -> FmEmptyState("article", "This article is no longer available")
                else -> {
                    AnimatedContent(
                        targetState = article.id,
                        transitionSpec = {
                            val duration = FmMotion.normal
                            if (pageDirection > 0) {
                                (slideInHorizontally(tween(duration)) { it } + fadeIn(tween(duration))) togetherWith
                                    (slideOutHorizontally(tween(duration)) { -it } + fadeOut(tween(duration)))
                            } else {
                                (slideInHorizontally(tween(duration)) { -it } + fadeIn(tween(duration))) togetherWith
                                    (slideOutHorizontally(tween(duration)) { it } + fadeOut(tween(duration)))
                            }
                        },
                        label = "rss-article",
                    ) { id ->
                        val displayed = library.article(id) ?: return@AnimatedContent
                        val displayedFeed = library.feed(displayed.feedId)
                        val settings = library.settings()
                        val style = RssPageStyle(
                            settings.theme,
                            settings.font,
                            settings.textSize,
                            Fm.dark,
                            hex(Fm.colors.accent.toArgb()),
                            settings.loadRemoteImages,
                        )
                        val html = remember(displayed, displayedFeed?.displayTitle, displayedFeed?.language, style, reader) {
                            RssArticlePage.html(
                                displayed,
                                displayedFeed?.displayTitle.orEmpty(),
                                style,
                                service.iconDataUri(displayed.feedId),
                                reader,
                                displayedFeed?.language.orEmpty(),
                            )
                        }
                        val background = Fm.colors.bgBase.toArgb()
                        var swipeDistance by remember(id) { mutableStateOf(0f) }
                        AndroidView(
                            factory = { ctx -> articleWebView(ctx) },
                            update = { view ->
                                view.settings.blockNetworkImage = !settings.loadRemoteImages
                                view.setBackgroundColor(background)
                                if (view.tag != html) {
                                    view.tag = html
                                    view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
                                }
                            },
                            onRelease = { it.destroy() },
                            modifier = Modifier
                                .fillMaxSize()
                                .pointerInput(order, index) {
                                    detectHorizontalDragGestures(
                                        onHorizontalDrag = { _, distance -> swipeDistance += distance },
                                        onDragEnd = {
                                            when {
                                                swipeDistance <= -72 && index >= 0 && index < order.size - 1 -> open(order[index + 1], 1)
                                                swipeDistance >= 72 && index > 0 -> open(order[index - 1], -1)
                                            }
                                            swipeDistance = 0f
                                        },
                                        onDragCancel = { swipeDistance = 0f },
                                    )
                                },
                        )
                    }
                }
            }
            FmInlineStatus(status)
            readerFailed?.let { message ->
                FmConfirmDialog(
                    title = "Reader View Unavailable",
                    body = "$message You can open the original page in your browser instead.",
                    confirmLabel = "Open Original",
                    onDismissRequest = { readerFailed = null },
                    onConfirm = {
                        readerFailed = null
                        article?.link?.let { openInBrowser(context, it) }
                    },
                )
            }
        }
    }

    private fun toggleReader(coroutines: kotlinx.coroutines.CoroutineScope) {
        val service = RssReaderFeature.service
        val article = service.library?.article(currentId) ?: return
        if (reader) {
            reader = false
            return
        }
        if (article.readerHtml.isNotEmpty()) {
            reader = true
            return
        }
        if (article.link.isEmpty()) {
            readerFailed = "This article has no web page."
            return
        }
        readerBusy = true
        val id = currentId
        coroutines.launch {
            runCatching { service.readerView(id) }
                .onSuccess { readable ->
                    if (readable.contentHtml.isEmpty()) readerFailed = "The main text of this page couldn't be found."
                    else if (currentId == id) reader = true
                }
                .onFailure { readerFailed = service.errorText(it) }
            readerBusy = false
        }
    }

    private fun nextUnread() {
        val service = RssReaderFeature.service
        val library = service.library ?: return
        val now = System.currentTimeMillis()
        val current = ids().mapNotNull { library.article(it) }
        val next = library.nextUnread(scope, current, currentId, now)
        if (next == null) {
            status.show("You're all caught up.")
            return
        }
        if (next.first != scope) {
            scope = next.first
            list = library.timeline(next.first, now).map { it.id }
        }
        open(next.second)
    }

    private fun hex(argb: Int): String = String.format("#%06X", argb and 0xFFFFFF)
}

private fun articleWebView(context: Context): WebView = WebView(context).apply {
    settings.javaScriptEnabled = false
    settings.javaScriptCanOpenWindowsAutomatically = false
    settings.allowFileAccess = false
    settings.allowContentAccess = false
    settings.domStorageEnabled = false
    settings.setGeolocationEnabled(false)
    settings.setSupportMultipleWindows(false)
    settings.mediaPlaybackRequiresUserGesture = true
    settings.mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
    settings.cacheMode = WebSettings.LOAD_DEFAULT
    isVerticalScrollBarEnabled = true
    webViewClient = object : WebViewClient() {
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            openInBrowser(view.context, request.url.toString())
            return true
        }
    }
}
