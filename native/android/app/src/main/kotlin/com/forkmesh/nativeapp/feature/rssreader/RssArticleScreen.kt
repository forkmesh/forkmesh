package com.forkmesh.nativeapp.feature.rssreader

import android.annotation.SuppressLint
import android.content.Context
import android.os.SystemClock
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.ViewConfiguration
import android.view.animation.DecelerateInterpolator
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
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.chat.ChatSendResult
import com.forkmesh.nativeapp.feature.chat.ChatShare
import com.forkmesh.nativeapp.feature.chat.composeChatShare
import com.forkmesh.nativeapp.feature.chat.pickChatShareTarget
import com.forkmesh.nativeapp.rssreader.RssArticlePage
import com.forkmesh.nativeapp.rssreader.RssFormat
import com.forkmesh.nativeapp.rssreader.RssPageStyle
import com.forkmesh.nativeapp.rssreader.RssScope
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmMotion
import kotlinx.coroutines.launch
import kotlin.math.abs

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
        val messenger = LocalFmMessenger.current
        val requireAccount = AccountFeature.rememberRequireAccount()
        var shareMenu by remember { mutableStateOf(false) }
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
                Box {
                    FmIconButton("share", "Share", onPressed = article?.let { { shareMenu = true } }, color = accent)
                    FmDropdownMenu(expanded = shareMenu, onDismissRequest = { shareMenu = false }) {
                        FmDropdownMenuItem("Send in Chat", icon = "send_outlined", onClick = {
                            shareMenu = false
                            article?.let { a ->
                                coroutines.launch {
                                    val text = RssFormat.shareText(a)
                                    val target = pickChatShareTarget(messenger, "Send Article to") ?: return@launch
                                    val message = composeChatShare(messenger, target, text) ?: return@launch
                                    when (val result = ChatShare.send(target, message)) {
                                        ChatSendResult.Sent -> status.show("Sent to ${target.title}.")
                                        ChatSendResult.Empty -> {}
                                        is ChatSendResult.NeedsAccount -> requireAccount(result.action)
                                        is ChatSendResult.Failed -> status.error(result.message)
                                    }
                                }
                            }
                        })
                        FmDropdownMenuItem("Share to Other Apps", icon = "share", onClick = {
                            shareMenu = false
                            article?.let { a -> shareText(context, a.title, RssFormat.shareText(a)) }
                        })
                    }
                }
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
                        val canPage by rememberUpdatedState { direction: Int ->
                            if (direction > 0) index >= 0 && index < order.size - 1 else index > 0
                        }
                        val pageTo by rememberUpdatedState { direction: Int ->
                            if (direction > 0 && index >= 0 && index < order.size - 1) open(order[index + 1], 1)
                            else if (direction < 0 && index > 0) open(order[index - 1], -1)
                        }
                        AndroidView(
                            factory = { ctx ->
                                articleWebView(ctx).also { view ->
                                    view.canPage = { canPage(it) }
                                    view.onPage = { pageTo(it) }
                                }
                            },
                            update = { view ->
                                view.settings.blockNetworkImage = !settings.loadRemoteImages
                                view.setBackgroundColor(background)
                                if (view.tag != html) {
                                    view.tag = html
                                    view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
                                }
                            },
                            onRelease = { it.destroy() },
                            modifier = Modifier.fillMaxSize(),
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

internal class RssArticleWebView(context: Context) : WebView(context) {
    var canPage: (Int) -> Boolean = { false }
    var onPage: (Int) -> Unit = {}
    private val density = resources.displayMetrics.density
    private val lockDistance = 4f * density
    private val pageDistance = 72f * density
    private val flingSpeed = 500f * density
    private var downX = 0f
    private var downY = 0f
    private var downRawX = 0f
    private var downRawY = 0f
    private var axis = AXIS_NONE
    private var velocity: VelocityTracker? = null
    private val tapSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var scrolledAt = 0L
    private var scrollingAtDown = false
    private var linkOpened = false
    private var pendingTap: Runnable? = null

    fun openedLink() {
        linkOpened = true
        pendingTap?.let { removeCallbacks(it) }
        pendingTap = null
    }

    override fun onScrollChanged(l: Int, t: Int, oldl: Int, oldt: Int) {
        super.onScrollChanged(l, t, oldl, oldt)
        scrolledAt = SystemClock.uptimeMillis()
    }

    private fun onControl(): Boolean = when (hitTestResult.type) {
        WebView.HitTestResult.SRC_ANCHOR_TYPE,
        WebView.HitTestResult.SRC_IMAGE_ANCHOR_TYPE,
        WebView.HitTestResult.EMAIL_TYPE,
        WebView.HitTestResult.PHONE_TYPE,
        WebView.HitTestResult.GEO_TYPE,
        WebView.HitTestResult.EDIT_TEXT_TYPE -> true
        else -> false
    }

    private fun tapped(event: MotionEvent) {
        if (scrollingAtDown) return
        if (abs(event.rawX - downRawX) > tapSlop || abs(event.rawY - downRawY) > tapSlop) return
        if (event.eventTime - event.downTime >= ViewConfiguration.getLongPressTimeout()) return
        val direction = if (event.x < width / 2f) -1 else 1
        if (!canPage(direction)) return
        linkOpened = false
        val check = Runnable {
            pendingTap = null
            if (!linkOpened && !onControl()) onPage(direction)
        }
        pendingTap = check
        postDelayed(check, TAP_DELAY_MS)
    }

    private fun track(event: MotionEvent) {
        val raw = MotionEvent.obtain(event)
        raw.setLocation(event.rawX, event.rawY)
        velocity?.addMovement(raw)
        raw.recycle()
    }

    private fun pass(event: MotionEvent): Boolean {
        val locked = MotionEvent.obtain(event)
        locked.setLocation(downX, event.y)
        val handled = super.onTouchEvent(locked)
        locked.recycle()
        return handled
    }

    private fun cancelContent(event: MotionEvent) {
        val cancel = MotionEvent.obtain(event)
        cancel.action = MotionEvent.ACTION_CANCEL
        cancel.setLocation(downX, downY)
        super.onTouchEvent(cancel)
        cancel.recycle()
    }

    private fun offset(dx: Float): Float = if (canPage(if (dx < 0) 1 else -1)) dx else dx * 0.25f

    private fun settle() {
        animate().translationX(0f).setDuration(180).setInterpolator(DecelerateInterpolator()).start()
    }

    private fun release(event: MotionEvent) {
        track(event)
        velocity?.computeCurrentVelocity(1000)
        val speed = velocity?.xVelocity ?: 0f
        val dx = event.rawX - downRawX
        val direction = if (dx < 0) 1 else -1
        val far = abs(dx) >= pageDistance || (abs(speed) >= flingSpeed && (speed < 0) == (dx < 0) && abs(dx) >= lockDistance * 3)
        if (far && canPage(direction)) onPage(direction) else settle()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                animate().cancel()
                translationX = 0f
                downX = event.x
                downY = event.y
                downRawX = event.rawX
                downRawY = event.rawY
                scrollingAtDown = SystemClock.uptimeMillis() - scrolledAt < 120
                axis = AXIS_NONE
                velocity?.recycle()
                velocity = VelocityTracker.obtain()
                track(event)
                parent?.requestDisallowInterceptTouchEvent(true)
                return pass(event)
            }
            MotionEvent.ACTION_MOVE -> {
                track(event)
                val dx = event.rawX - downRawX
                val dy = event.rawY - downRawY
                if (axis == AXIS_FREE) return super.onTouchEvent(event)
                if (axis == AXIS_NONE && event.eventTime - event.downTime >= ViewConfiguration.getLongPressTimeout()) {
                    axis = AXIS_FREE
                    return super.onTouchEvent(event)
                }
                if (axis == AXIS_NONE && (abs(dx) >= lockDistance || abs(dy) >= lockDistance)) {
                    axis = if (abs(dx) > abs(dy)) AXIS_HORIZONTAL else AXIS_VERTICAL
                    if (axis == AXIS_HORIZONTAL) cancelContent(event)
                }
                if (axis == AXIS_HORIZONTAL) {
                    translationX = offset(dx)
                    return true
                }
                return pass(event)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val was = axis
                axis = AXIS_NONE
                return when {
                    was == AXIS_HORIZONTAL -> {
                        if (event.actionMasked == MotionEvent.ACTION_UP) release(event) else settle()
                        true
                    }
                    was == AXIS_FREE -> super.onTouchEvent(event)
                    else -> pass(event).also {
                        if (was == AXIS_NONE && event.actionMasked == MotionEvent.ACTION_UP) tapped(event)
                    }
                }
            }
            else -> return when (axis) {
                AXIS_HORIZONTAL -> true
                AXIS_FREE -> super.onTouchEvent(event)
                else -> pass(event)
            }
        }
    }

    override fun onDetachedFromWindow() {
        pendingTap?.let { removeCallbacks(it) }
        pendingTap = null
        velocity?.recycle()
        velocity = null
        super.onDetachedFromWindow()
    }

    private companion object {
        const val AXIS_NONE = 0
        const val AXIS_HORIZONTAL = 1
        const val AXIS_VERTICAL = 2
        const val AXIS_FREE = 3
        const val TAP_DELAY_MS = 200L
    }
}

private fun articleWebView(context: Context): RssArticleWebView = RssArticleWebView(context).apply {
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
    settings.setSupportZoom(false)
    settings.builtInZoomControls = false
    settings.displayZoomControls = false
    settings.useWideViewPort = false
    settings.loadWithOverviewMode = false
    isVerticalScrollBarEnabled = true
    isHorizontalScrollBarEnabled = false
    isHorizontalFadingEdgeEnabled = false
    webViewClient = object : WebViewClient() {
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            (view as? RssArticleWebView)?.openedLink()
            openInBrowser(view.context, request.url.toString())
            return true
        }
    }
}
