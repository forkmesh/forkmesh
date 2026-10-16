package com.forkmesh.nativeapp.feature.rssreader

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.SwipeToDismissBox
import androidx.compose.material3.SwipeToDismissBoxValue
import androidx.compose.material3.Text
import androidx.compose.material3.rememberSwipeToDismissBoxState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.rssreader.RssArticle
import com.forkmesh.nativeapp.rssreader.RssFeed
import com.forkmesh.nativeapp.rssreader.RssFormat
import com.forkmesh.nativeapp.rssreader.RssScope
import com.forkmesh.nativeapp.rssreader.RssSettings
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal class RssTimelineScreen(private val scope: RssScope) : Screen() {
    override val title: String get() = RssReaderFeature.service.library?.let { scopeTitle(it, scope) } ?: "Articles"
    override val hidesShellChrome: Boolean get() = true
    private var searching by mutableStateOf(false)
    private var query by mutableStateOf("")
    private var keep by mutableStateOf<Set<String>?>(null)
    private var hidden by mutableStateOf<Set<String>>(emptySet())
    private var confirm by mutableStateOf(false)
    private var pulled by mutableStateOf(false)
    private var ids: List<String> = emptyList()

    fun currentIds(): List<String> = ids

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        val refreshing by service.refreshing.collectAsState()
        LaunchedEffect(Unit) { service.load() }
        LaunchedEffect(refreshing) { if (!refreshing) pulled = false }
        val library = service.library
        val now = remember(version) { System.currentTimeMillis() }
        LaunchedEffect(library) {
            if (library != null && keep == null && scope == RssScope.Unread) keep = library.articles(scope, System.currentTimeMillis()).map { it.id }.toSet()
        }
        val articles = remember(version, query, keep, hidden) {
            library?.timeline(scope, now, query, keep.orEmpty())?.filter { it.id !in hidden }.orEmpty()
        }
        ids = articles.map { it.id }
        val settings = library?.settings() ?: RssSettings()
        val feeds = remember(version) { library?.feeds().orEmpty().associateBy { it.id } }
        val unread = articles.count { !it.read }
        RssScaffold(
            library?.let { scopeTitle(it, scope) } ?: "Articles",
            navigator,
            subtitle = if (unread > 0) "$unread unread" else null,
            actions = {
                FmIconButton("search", "Search", onPressed = { searching = !searching; if (!searching) query = "" }, color = if (searching) Fm.colors.accent else Fm.colors.textSecondary)
                FmIconButton("done_all", "Mark All as Read", onPressed = if (unread > 0) ({ if (settings.confirmMarkAllRead) confirm = true else markAll(articles) }) else null)
            },
        ) {
            Column(Modifier.fillMaxSize()) {
                if (searching) {
                    Box(Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)) {
                        FmTextField(value = query, onValueChange = { query = it }, hint = "Search articles", autofocus = true, leading = { FmIcon("search", size = 18.dp) })
                    }
                }
                FmPullToRefreshBox(
                    isRefreshing = pulled && refreshing,
                    onRefresh = {
                        pulled = true
                        if (settings.refreshClearsRead) {
                            hidden = articles.filter { it.read }.map { it.id }.toSet()
                            if (scope == RssScope.Unread) keep = library?.articles(scope, System.currentTimeMillis())?.map { it.id }?.toSet()
                        }
                        service.refresh(force = true)
                    },
                    modifier = Modifier.fillMaxSize(),
                ) {
                    when {
                        library == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                        articles.isEmpty() -> FmEmptyState(
                            if (query.isNotEmpty()) "search" else "done_all",
                            if (query.isNotEmpty()) "No matching articles" else "No articles",
                            message = if (query.isNotEmpty()) null else "Pull down to check for new articles.",
                        )
                        else -> LazyColumn(Modifier.fillMaxSize()) {
                            val grouped = settings.groupByFeed && scope !is RssScope.Feed
                            var lastFeed = ""
                            for (article in articles) {
                                if (grouped && article.feedId != lastFeed) {
                                    lastFeed = article.feedId
                                    val feed = feeds[article.feedId]
                                    item(key = "group:${article.feedId}") {
                                        Row(Modifier.fillMaxWidth().background(Fm.colors.bgBase).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                                            FeedIcon(feed, 16.dp, 4.dp)
                                            Spacer(Modifier.width(FmSpace.x2))
                                            Text(feed?.displayTitle.orEmpty(), style = Fm.text.labelLarge.copy(color = Fm.colors.textSecondary))
                                        }
                                    }
                                }
                                item(key = article.id) {
                                    SwipeRow(article) {
                                        ArticleRow(
                                            article, feeds[article.feedId], settings, now,
                                            showFeedName = !grouped && scope !is RssScope.Feed,
                                        ) {
                                            navigator.push(RssArticleScreen(scope, article.id, ::currentIds))
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (confirm) {
                FmConfirmDialog(
                    title = "Mark All as Read?",
                    body = "Mark ${RssFormat.plural(unread, "article")} in “${library?.let { scopeTitle(it, scope) }.orEmpty()}” as read?",
                    confirmLabel = "Mark All as Read",
                    onDismissRequest = { confirm = false },
                    onConfirm = {
                        confirm = false
                        markAll(articles)
                    },
                )
            }
        }
    }

    private fun markAll(articles: List<RssArticle>) {
        RssReaderFeature.service.mutate { it.setRead(articles.filter { article -> !article.read }.map { article -> article.id }, true) }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SwipeRow(article: RssArticle, content: @Composable () -> Unit) {
    val service = RssReaderFeature.service
    val state = rememberSwipeToDismissBoxState()
    val current by rememberUpdatedState(article)
    LaunchedEffect(state.currentValue) {
        when (state.currentValue) {
            SwipeToDismissBoxValue.StartToEnd -> service.mutate { it.setRead(listOf(current.id), !current.read) }
            SwipeToDismissBoxValue.EndToStart -> service.mutate { it.setStarred(listOf(current.id), !current.starred) }
            SwipeToDismissBoxValue.Settled -> return@LaunchedEffect
        }
        state.snapTo(SwipeToDismissBoxValue.Settled)
    }
    SwipeToDismissBox(
        state = state,
        backgroundContent = {
            val toStart = state.dismissDirection == SwipeToDismissBoxValue.StartToEnd
            val tint = if (toStart) RssColors.unread else RssColors.star
            Row(
                Modifier.fillMaxSize().background(tint.copy(alpha = .9f)).padding(horizontal = FmSpace.x5),
                horizontalArrangement = if (toStart) Arrangement.Start else Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val label = if (toStart) (if (article.read) "Mark Unread" else "Mark Read") else (if (article.starred) "Unstar" else "Star")
                FmIcon(if (toStart) (if (article.read) "circle" else "radio_button_unchecked") else (if (article.starred) "star_border" else "star"), size = 22.dp, tint = Color.White)
                Spacer(Modifier.width(FmSpace.x2))
                Text(label, style = Fm.text.labelLarge.copy(color = Color.White))
            }
        },
        content = { content() },
    )
}

@Composable
private fun ArticleRow(
    article: RssArticle,
    feed: RssFeed?,
    settings: RssSettings,
    now: Long,
    showFeedName: Boolean,
    onTap: () -> Unit,
) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth().background(c.bgRaised).clickable(onClick = onTap)) {
        Row(Modifier.fillMaxWidth().padding(start = FmSpace.x1, end = FmSpace.x4, top = FmSpace.x3, bottom = FmSpace.x3)) {
            Column(Modifier.width(18.dp).padding(top = 5.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                UnreadDot(!article.read)
                if (article.starred) FmIcon("star", size = 12.dp, tint = RssColors.star)
            }
            Spacer(Modifier.width(FmSpace.x1))
            if (settings.showFeedIcons) {
                Box(Modifier.padding(top = 2.dp)) { FeedIcon(feed, 26.dp, 6.dp) }
                Spacer(Modifier.width(FmSpace.x3))
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(
                    article.title.ifEmpty { "Untitled" },
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleSmall.copy(fontWeight = FontWeight.SemiBold, color = if (article.read) c.textTertiary else c.textPrimary),
                )
                if (settings.timelineLines > 0 && article.summary.isNotEmpty()) {
                    Text(
                        article.summary,
                        maxLines = settings.timelineLines,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodyMedium.copy(color = if (article.read) c.textTertiary else c.textSecondary),
                    )
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    val byline = if (article.author.isEmpty() || article.author == feed?.displayTitle) {
                        feed?.displayTitle.orEmpty()
                    } else if (showFeedName) {
                        listOfNotNull(feed?.displayTitle, article.author).joinToString(" · ")
                    } else {
                        article.author
                    }
                    Text(byline, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(RssFormat.short(article.published, now), style = Fm.text.bodySmall.copy(color = c.textTertiary))
                }
            }
        }
        HorizontalDivider(Modifier.padding(start = if (settings.showFeedIcons) 60.dp else 22.dp), thickness = 1.dp, color = c.border)
    }
}
