package com.forkmesh.nativeapp.feature.instancefeed

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
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
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.FeedEntry
import com.forkmesh.nativeapp.market.FeedPost
import com.forkmesh.nativeapp.market.FeedProfile
import com.forkmesh.nativeapp.market.FeedSpan
import com.forkmesh.nativeapp.market.FeedThread
import com.forkmesh.nativeapp.market.InstanceFeed
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesException
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.appendLink
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

internal sealed class FeedView {
    data object Timeline : FeedView()
    data class Profile(val author: String) : FeedView()
    data class Thread(val id: String) : FeedView()
}

internal class FeedSession {
    val patched = mutableStateMapOf<String, FeedPost>()
    val removed = mutableStateListOf<String>()
    val revealed = mutableStateMapOf<String, Boolean>()
    var actor by mutableStateOf("")
    var maxLength by mutableIntStateOf(InstanceFeed.MAX_LENGTH)

    fun current(post: FeedPost): FeedPost = patched[post.id] ?: post

    fun accept(posts: List<FeedPost>) {
        posts.forEach { patched.remove(it.id) }
    }
}

internal class InstanceFeedScreen(
    post: String? = null,
    private val view: FeedView = FeedView.Timeline,
    private val session: FeedSession = FeedSession(),
) : Screen() {
    override val title: String
        get() = when (view) {
            FeedView.Timeline -> "Feed"
            is FeedView.Profile -> "@${view.author}"
            is FeedView.Thread -> "Post"
        }

    private var pendingPost = post?.trim()?.takeIf { it.isNotEmpty() }
    private var entries by mutableStateOf<List<FeedEntry>>(emptyList())
    private var thread by mutableStateOf<FeedThread?>(null)
    private var profile by mutableStateOf<FeedProfile?>(null)
    private var next by mutableStateOf("")
    private var loaded by mutableStateOf(false)
    private var loading by mutableStateOf(false)
    private var loadingMore by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var installed by mutableStateOf(true)
    private var missing by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var text by mutableStateOf("")
    private var spoiler by mutableStateOf("")
    private var warning by mutableStateOf(false)
    private var replyTo by mutableStateOf<FeedPost?>(null)
    private var posting by mutableStateOf(false)
    private var focusTick by mutableIntStateOf(0)
    private var confirmDelete by mutableStateOf<FeedPost?>(null)
    private val busy = mutableStateMapOf<String, Boolean>()
    private val status = FmStatus()

    init {
        pluginPackage = InstanceFeed.PACKAGE_ID
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val listState = rememberLazyListState()
        val openLink = rememberLinkOpener()
        LaunchedEffect(Unit) {
            val target = pendingPost
            if (target != null) {
                pendingPost = null
                navigator.push(InstanceFeedScreen(view = FeedView.Thread(target), session = session))
                return@LaunchedEffect
            }
            if (!loaded) load(scope)
        }
        val actions = FeedActions(
            openThread = { id -> if ((view as? FeedView.Thread)?.id != id) navigator.push(InstanceFeedScreen(view = FeedView.Thread(id), session = session)) },
            openAuthor = { name -> if ((view as? FeedView.Profile)?.author != name) navigator.push(InstanceFeedScreen(view = FeedView.Profile(name), session = session)) },
            openLink = openLink,
            reveal = { id -> session.revealed[id] = session.revealed[id] != true },
            reply = { post -> startReply(post, scope, listState) },
            boost = { post -> toggle(scope, post, boost = true) },
            favourite = { post -> toggle(scope, post, boost = false) },
            delete = { post -> confirmDelete = post },
        )
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton("refresh", "Refresh", onPressed = if (loading) null else ({ load(scope) }), color = Fm.colors.textPrimary)
            },
            status = status,
        ) {
            FmPullToRefreshBox(isRefreshing = refreshing, onRefresh = { load(scope, pulled = true) }, modifier = Modifier.fillMaxSize()) {
                LazyColumn(
                    Modifier.fillMaxSize(),
                    state = listState,
                    contentPadding = PaddingValues(bottom = FmSpace.x6),
                ) {
                    body(scope, navigator, actions)
                }
            }
        }
        confirmDelete?.let { post ->
            FmConfirmDialog(
                title = "Delete this post?",
                body = "Its favourites and boosts are removed with it. Replies stay.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    confirmDelete = null
                    delete(scope, navigator, post)
                },
                onDismissRequest = { confirmDelete = null },
            )
        }
    }

    private fun LazyListScope.body(scope: CoroutineScope, navigator: Navigator, actions: FeedActions) {
        if (!installed) {
            item(key = "install") { NotInstalled { navigator.push(MarketFeature.packageScreen(InstanceFeed.PACKAGE_ID)) } }
            return
        }
        if (view == FeedView.Timeline || replyTo != null) item(key = "compose") { Composer(scope, navigator) }
        if (error.isNotEmpty() && loaded) item(key = "error") { Notice(error) }
        val profileView = view as? FeedView.Profile
        profile?.takeIf { profileView != null }?.let { summary -> item(key = "profile") { ProfileHeader(summary) } }
        when {
            !loaded && loading -> item(key = "loading") { Loading() }
            !loaded && error.isNotEmpty() -> item(key = "failed") {
                FmEmptyState("cloud_off", "Couldn't load the feed", message = "$error Pull down or tap Refresh to try again.")
            }
            !loaded -> Unit
            view is FeedView.Thread -> threadItems(actions)
            else -> timelineItems(scope, actions, profileView)
        }
    }

    private fun LazyListScope.threadItems(actions: FeedActions) {
        val current = thread
        val focused = current?.post
        val posts = current?.posts.orEmpty().filter { it.id !in session.removed }
        if (missing || focused == null || posts.isEmpty()) {
            item(key = "missing") { FmEmptyState("forum", "Post not found", message = "It may have been deleted.") }
            return
        }
        items(posts, key = { "post-${it.id}" }) { post ->
            PostRow(
                session.current(post),
                booster = null,
                focused = post.id == focused.id,
                revealed = session.revealed[post.id] == true,
                busy = busy[post.id] == true,
                actions = actions,
            )
            HorizontalDivider(color = Fm.colors.border)
        }
    }

    private fun LazyListScope.timelineItems(scope: CoroutineScope, actions: FeedActions, profileView: FeedView.Profile?) {
        val visible = entries.filter { it.post.id !in session.removed && it.id !in session.removed }
        if (visible.isEmpty()) {
            item(key = "empty") {
                if (profileView != null) {
                    FmEmptyState("forum", "No posts yet", message = "@${profileView.author} has not posted to the feed.")
                } else {
                    FmEmptyState("forum", "No posts yet", message = "Be the first to post to this instance.")
                }
            }
            return
        }
        items(visible, key = { "entry-${it.id}" }) { entry ->
            PostRow(
                session.current(entry.post),
                booster = entry.takeIf { it.isBoost },
                focused = false,
                revealed = session.revealed[entry.post.id] == true,
                busy = busy[entry.post.id] == true,
                actions = actions,
            )
            HorizontalDivider(color = Fm.colors.border)
        }
        if (next.isNotEmpty()) {
            item(key = "more-$next") {
                LaunchedEffect(next) { if (!loading && error.isEmpty()) load(scope, append = true) }
                Box(Modifier.fillMaxWidth().padding(FmSpace.x4), contentAlignment = Alignment.Center) {
                    if (loadingMore) {
                        FmCircularProgressIndicator()
                    } else {
                        FmTextButton(onClick = { load(scope, append = true) }) { Text("Load older posts") }
                    }
                }
            }
        }
    }

    @Composable
    private fun Composer(scope: CoroutineScope, navigator: Navigator) {
        val c = Fm.colors
        val max = session.maxLength
        val over = text.length > max
        val canPost = !posting && text.isNotBlank() && text.trim().length <= max
        FmCard(Modifier.fillMaxWidth().padding(FmSpace.x4)) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                replyTo?.let { parent ->
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("reply", size = 16.dp, tint = c.textTertiary)
                        Spacer(Modifier.width(FmSpace.x2))
                        Text(
                            "Replying to @${parent.author}",
                            Modifier.weight(1f),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                        FmTextButton(onClick = { replyTo = null }) { Text("Cancel") }
                    }
                }
                if (warning) {
                    FmTextField(
                        spoiler,
                        { spoiler = it.take(InstanceFeed.MAX_SPOILER) },
                        hint = "Content warning",
                    )
                }
                key(focusTick) {
                    FmTextField(
                        text,
                        { text = it },
                        hint = "What's happening on the instance?",
                        minLines = 3,
                        maxLines = 8,
                        autofocus = focusTick > 0,
                        errorText = if (over) "Posts can be at most $max characters." else null,
                    )
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmToneButton(
                        "CW",
                        onPressed = {
                            warning = !warning
                            if (!warning) spoiler = ""
                        },
                        tone = if (warning) FmTone.Accent else FmTone.Neutral,
                        tooltip = if (warning) "Remove the content warning" else "Add a content warning",
                    )
                    Spacer(Modifier.weight(1f))
                    Text(
                        "${max - text.length}",
                        style = Fm.text.bodySmall.copy(color = if (over) c.danger else c.textTertiary, fontFeatureSettings = FmType.tabular),
                    )
                    Spacer(Modifier.width(FmSpace.x3))
                    FmToneButton(
                        if (posting) "Posting…" else "Post",
                        onPressed = if (canPost) ({ submit(scope, navigator) }) else null,
                        icon = "send",
                        tone = FmTone.Success,
                        filled = true,
                    )
                }
            }
        }
    }

    private fun client(): PackagesClient? {
        val client = MarketFeature.client()
        if (client == null) status.error(PackagesException.sentence("offline"))
        return client
    }

    private fun load(scope: CoroutineScope, append: Boolean = false, pulled: Boolean = false) {
        if (loading) return
        val client = MarketFeature.client()
        if (client == null) {
            error = PackagesException.sentence("offline")
            return
        }
        val cursor = if (append) next else null
        loading = true
        loadingMore = append
        refreshing = pulled
        scope.launch {
            try {
                when (view) {
                    is FeedView.Thread -> {
                        val result = io { InstanceFeed.thread(client, view.id) }
                        session.accept(result.posts)
                        thread = result
                        missing = result.post == null
                        installed = true
                    }
                    else -> {
                        val author = (view as? FeedView.Profile)?.author
                        val page = io { InstanceFeed.timeline(client, author, cursor) }
                        installed = page.installed
                        if (page.actor.isNotEmpty()) session.actor = page.actor
                        session.maxLength = page.maxLength
                        session.accept(page.entries.map { it.post })
                        entries = if (append) (entries + page.entries).distinctBy { it.id } else page.entries
                        next = page.next
                        if (!append || page.profile != null) profile = page.profile
                    }
                }
                error = ""
                loaded = true
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Exception) {
                val code = (failure as? PackagesException)?.code
                val httpStatus = (failure as? PackagesException)?.status ?: 0
                when {
                    code == "install_first" -> {
                        installed = false
                        loaded = true
                    }
                    view is FeedView.Thread && !append && (httpStatus == 404 || code == "not_found" || code == "post_not_found") -> {
                        thread = null
                        missing = true
                        loaded = true
                    }
                    else -> error = PackagesException.describe(failure)
                }
            } finally {
                loading = false
                loadingMore = false
                refreshing = false
            }
        }
    }

    private fun startReply(post: FeedPost, scope: CoroutineScope, listState: LazyListState) {
        replyTo = post
        val mention = "@${post.author}"
        if (!text.startsWith(mention)) text = "$mention $text".trimEnd() + " "
        focusTick++
        scope.launch { listState.animateScrollToItem(0) }
    }

    private fun submit(scope: CoroutineScope, navigator: Navigator) {
        val body = text.trim()
        if (body.isEmpty() || posting) return
        if (body.length > session.maxLength) {
            status.error("Posts can be at most ${session.maxLength} characters.")
            return
        }
        val client = client() ?: return
        val parent = replyTo
        val warningText = if (warning) spoiler.trim() else ""
        posting = true
        scope.launch {
            try {
                val created = io { InstanceFeed.post(client, body, warningText, parent?.id) }
                text = ""
                spoiler = ""
                warning = false
                replyTo = null
                status.clear()
                if (parent != null) {
                    val latest = session.current(parent)
                    session.patched[parent.id] = latest.copy(replies = latest.replies + 1)
                }
                when {
                    view is FeedView.Thread -> load(scope)
                    parent != null -> navigator.push(InstanceFeedScreen(view = FeedView.Thread(parent.id), session = session))
                    view == FeedView.Timeline || (view as? FeedView.Profile)?.author == created.author ->
                        entries = listOf(FeedEntry(created.id, created)) + entries
                }
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Exception) {
                status.error("Could not post: ${PackagesException.describe(failure)}")
            } finally {
                posting = false
            }
        }
    }

    private fun toggle(scope: CoroutineScope, post: FeedPost, boost: Boolean) {
        if (busy[post.id] == true) return
        val client = client() ?: return
        busy[post.id] = true
        scope.launch {
            try {
                val updated = io {
                    if (boost) InstanceFeed.boost(client, post.id, !post.boosted) else InstanceFeed.favourite(client, post.id, !post.favourited)
                }
                session.patched[updated.id] = updated
                if (boost && !updated.boosted && session.actor.isNotEmpty()) {
                    entries = entries.filterNot { it.isBoost && it.booster == session.actor && it.post.id == updated.id }
                }
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Exception) {
                status.error("Could not update the post: ${PackagesException.describe(failure)}")
            } finally {
                busy.remove(post.id)
            }
        }
    }

    private fun delete(scope: CoroutineScope, navigator: Navigator, post: FeedPost) {
        val client = client() ?: return
        busy[post.id] = true
        scope.launch {
            try {
                io { InstanceFeed.delete(client, post.id) }
                session.removed += post.id
                if (replyTo?.id == post.id) replyTo = null
                if (view is FeedView.Thread && thread?.post?.id == post.id && navigator.top === this@InstanceFeedScreen) navigator.pop()
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Exception) {
                status.error("Could not delete the post: ${PackagesException.describe(failure)}")
            } finally {
                busy.remove(post.id)
            }
        }
    }

    private suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }
}

private class FeedActions(
    val openThread: (String) -> Unit,
    val openAuthor: (String) -> Unit,
    val openLink: (String) -> Unit,
    val reveal: (String) -> Unit,
    val reply: (FeedPost) -> Unit,
    val boost: (FeedPost) -> Unit,
    val favourite: (FeedPost) -> Unit,
    val delete: (FeedPost) -> Unit,
)

@Composable
private fun PostRow(
    post: FeedPost,
    booster: FeedEntry?,
    focused: Boolean,
    revealed: Boolean,
    busy: Boolean,
    actions: FeedActions,
) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .background(if (focused) c.bgRaised else Color.Transparent)
            .clickable(enabled = !focused, onClickLabel = "Open thread") { actions.openThread(post.id) }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
    ) {
        if (booster != null) {
            Row(
                Modifier
                    .padding(start = 52.dp, bottom = FmSpace.x2)
                    .clickable { actions.openAuthor(booster.booster) },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("repeat", size = 14.dp, tint = c.textTertiary)
                Spacer(Modifier.width(FmSpace.x1))
                Text(
                    "${booster.boosterName} boosted",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
            }
        }
        Row {
            Avatar(post.displayName, large = focused, onClick = { actions.openAuthor(post.author) })
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                AuthorLine(post, onClick = { actions.openAuthor(post.author) })
                if (post.replyTo.isNotEmpty()) ReplyingTo(post.replyToAuthor, actions.openAuthor)
                if (post.spoiler.isNotEmpty()) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(post.spoiler, Modifier.weight(1f), style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FontWeight.Medium))
                        FmToneButton(if (revealed) "Show less" else "Show more", onPressed = { actions.reveal(post.id) }, height = 30.dp)
                    }
                    if (revealed) PostBody(post.text, focused, actions)
                } else {
                    PostBody(post.text, focused, actions)
                }
                PostActions(post, busy, actions)
            }
        }
    }
}

@Composable
private fun AuthorLine(post: FeedPost, onClick: () -> Unit) {
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        Row(Modifier.weight(1f).clickable(onClickLabel = "Open profile", onClick = onClick), verticalAlignment = Alignment.CenterVertically) {
            Text(
                post.displayName,
                Modifier.weight(1f, fill = false),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold),
            )
            Spacer(Modifier.width(FmSpace.x1))
            Text(
                "@${post.author}",
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
private fun ReplyingTo(author: String, openAuthor: (String) -> Unit) {
    val c = Fm.colors
    if (author.isEmpty()) {
        Text("Replying to a removed post", style = Fm.text.bodySmall.copy(color = c.textTertiary))
        return
    }
    Row {
        Text("Replying to ", style = Fm.text.bodySmall.copy(color = c.textTertiary))
        Text("@$author", Modifier.clickable { openAuthor(author) }, style = Fm.text.bodySmall.copy(color = c.accent))
    }
}

@Composable
private fun PostBody(text: String, focused: Boolean, actions: FeedActions) {
    val accent = Fm.colors.accent
    val annotated = remember(text, accent) {
        AnnotatedString.Builder().apply {
            for (span in InstanceFeed.spans(text)) {
                when (span) {
                    is FeedSpan.Text -> append(span.text)
                    is FeedSpan.Link -> appendLink(span.url, span.url, accent, actions.openLink)
                    is FeedSpan.Mention -> withLink(
                        LinkAnnotation.Clickable("mention:${span.name}", TextLinkStyles(SpanStyle(color = accent))) { actions.openAuthor(span.name) },
                    ) { append(span.text) }
                }
            }
        }.toAnnotatedString()
    }
    val style = if (focused) Fm.text.bodyLarge else Fm.text.bodyMedium
    Text(annotated, style = style.copy(color = Fm.colors.textPrimary))
}

@Composable
private fun PostActions(post: FeedPost, busy: Boolean, actions: FeedActions) {
    val c = Fm.colors
    Row(Modifier.padding(top = FmSpace.x1).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        PostAction("reply", "Reply", post.replies, c.textTertiary, enabled = true) { actions.reply(post) }
        Spacer(Modifier.width(FmSpace.x3))
        PostAction(
            "repeat",
            if (post.boosted) "Undo boost" else "Boost",
            post.boosts,
            if (post.boosted) c.accent else c.textTertiary,
            enabled = !busy,
        ) { actions.boost(post) }
        Spacer(Modifier.width(FmSpace.x3))
        PostAction(
            if (post.favourited) "star" else "star_border",
            if (post.favourited) "Remove favourite" else "Favourite",
            post.favourites,
            if (post.favourited) c.warning else c.textTertiary,
            enabled = !busy,
        ) { actions.favourite(post) }
        Spacer(Modifier.weight(1f))
        if (post.canDelete) {
            FmIconButton("delete_outline", "Delete post", onPressed = if (busy) null else ({ actions.delete(post) }), color = c.textTertiary, iconSize = 18.dp)
        }
    }
}

@Composable
private fun PostAction(icon: String, label: String, count: Int, tint: Color, enabled: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .heightIn(min = 44.dp)
            .widthIn(min = 44.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .clickable(enabled = enabled, role = Role.Button, onClickLabel = label, onClick = onClick)
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
private fun Avatar(name: String, large: Boolean = false, onClick: (() -> Unit)? = null) {
    val c = Fm.colors
    val size = if (large) 48.dp else 40.dp
    Box(
        Modifier
            .size(size)
            .clip(CircleShape)
            .background(c.bgOverlay)
            .border(1.dp, c.border, CircleShape)
            .then(if (onClick != null) Modifier.clickable(onClickLabel = "Open profile", onClick = onClick) else Modifier),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            name.take(1).uppercase().ifEmpty { "?" },
            style = (if (large) Fm.text.titleMedium else Fm.text.titleSmall).copy(color = c.textSecondary, fontWeight = FontWeight.SemiBold),
        )
    }
}

private val SINCE_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("MMMM yyyy")

@Composable
private fun ProfileHeader(profile: FeedProfile) {
    val c = Fm.colors
    val facts = buildList {
        add("@${profile.name}")
        add(if (profile.posts == 1) "1 post" else "${profile.posts} posts")
        if (profile.since > 0) add("posting since ${SINCE_FORMAT.format(Instant.ofEpochMilli(profile.since).atZone(ZoneId.systemDefault()))}")
    }
    Column {
        Row(Modifier.fillMaxWidth().padding(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
            Avatar(profile.displayName, large = true)
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Text(profile.displayName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                Text(facts.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
        HorizontalDivider(color = c.border)
    }
}

@Composable
private fun NotInstalled(openMarket: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(FmSpace.x4), horizontalAlignment = Alignment.CenterHorizontally) {
        FmEmptyState(
            "storefront",
            "Feed is not installed",
            message = "Install Feed from the Market to read and post to the timeline everyone on your instance shares.",
        )
        FmToneButton("Open in Market", onPressed = openMarket, icon = "storefront", tone = FmTone.Accent, filled = true)
    }
}

@Composable
private fun Loading() {
    Box(Modifier.fillMaxWidth().padding(FmSpace.x6), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
}

@Composable
private fun Notice(text: String) {
    Text(
        text,
        Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        style = Fm.text.bodySmall.copy(color = Fm.colors.danger),
    )
}
