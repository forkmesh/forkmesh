package com.forkmesh.nativeapp.feature.mastodon

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.mastodon.Mastodon
import com.forkmesh.nativeapp.mastodon.MastodonAccount
import com.forkmesh.nativeapp.mastodon.MastodonHtml
import com.forkmesh.nativeapp.mastodon.MastodonRelationship
import com.forkmesh.nativeapp.mastodon.MastodonSearch
import com.forkmesh.nativeapp.mastodon.MastodonSource
import com.forkmesh.nativeapp.mastodon.MastodonStatus
import com.forkmesh.nativeapp.mastodon.MastodonThread
import com.forkmesh.nativeapp.mastodon.MastodonUnauthorized
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal enum class MastodonSection(val label: String, val empty: String) {
    Home("Home", "Your home timeline is quiet. Follow people to fill it."),
    Notifications("Notifications", "No notifications yet."),
    Local("Local", "No posts on this server yet."),
    Federated("Federated", "No posts reached this server yet."),
    Bookmarks("Bookmarks", "Posts you bookmark appear here."),
    Favourites("Favourites", "Posts you favourite appear here."),
    ;

    fun model(): MastodonFeedModel = when (this) {
        Notifications -> MastodonFeedModel("Nothing here", empty) { client, next -> MastodonFeedModel.notes(client.notifications(next)) }
        else -> MastodonFeedModel("Nothing here", empty) { client, next -> MastodonFeedModel.posts(client.timeline(source, next)) }
    }

    private val source: MastodonSource
        get() = when (this) {
            Home, Notifications -> MastodonSource.Home
            Local -> MastodonSource.Local
            Federated -> MastodonSource.Federated
            Bookmarks -> MastodonSource.Bookmarks
            Favourites -> MastodonSource.Favourites
        }
}

internal abstract class MastodonBaseScreen : Screen() {
    protected val status = FmStatus()
    protected val interactions = MastodonInteractions(status)

    init {
        pluginPackage = Mastodon.PACKAGE_ID
    }

    @Composable
    protected fun LeaveWhenSignedOut(navigator: Navigator) {
        val session = MastodonState.session
        LaunchedEffect(session) {
            if (session == null && navigator.top === this@MastodonBaseScreen) navigator.pop()
        }
    }
}

internal class MastodonScreen : Screen() {
    override val title: String get() = "Mastodon"

    private val status = FmStatus()
    private val interactions = MastodonInteractions(status)
    private var section by mutableStateOf(MastodonSection.Home)
    private val models = HashMap<MastodonSection, MastodonFeedModel>()
    private var generation = -1
    private var mode by mutableStateOf(SignInMode.Browser)
    private var code by mutableStateOf("")
    private var token by mutableStateOf("")

    private enum class SignInMode { Browser, Code, Token }

    init {
        pluginPackage = Mastodon.PACKAGE_ID
        interactions.onPosted = { created, parent ->
            if (parent == null && created.visibility != "direct") models[MastodonSection.Home]?.prepend(created)
        }
    }

    private fun model(): MastodonFeedModel = models.getOrPut(section) { section.model() }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) { MastodonState.load() }
        val session = MastodonState.session
        if (MastodonState.generation != generation) {
            generation = MastodonState.generation
            models.clear()
        }
        LaunchedEffect(session?.token) { if (session != null) MastodonState.refreshLimits() }
        LaunchedEffect(MastodonState.notice) {
            val notice = MastodonState.notice
            if (notice.isNotEmpty() && session != null) {
                if (MastodonState.noticeIsError) status.error(notice) else status.show(notice)
                MastodonState.clearNotice()
            }
        }
        val actions = interactions.actions(navigator, scope)
        DetailScaffold(
            title,
            navigator,
            actions = {
                if (session != null) {
                    MastodonComposeIcon { interactions.compose() }
                    FmIconButton("search", "Search", onPressed = { navigator.push(MastodonSearchScreen()) }, color = Fm.colors.textPrimary)
                    AccountMenu(navigator, scope)
                }
            },
            status = status,
        ) {
            if (!MastodonState.loaded) {
                MastodonLoading()
            } else if (session == null) {
                SignIn(scope)
            } else {
                val model = model()
                LaunchedEffect(section, generation) { if (!model.loaded && !model.loading) model.load(scope) }
                Column(Modifier.fillMaxSize()) {
                    Sections()
                    FmPullToRefreshBox(isRefreshing = model.refreshing, onRefresh = { model.load(scope, pulled = true) }, modifier = Modifier.fillMaxSize()) {
                        LazyColumn(Modifier.fillMaxSize(), state = rememberLazyListState(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                            with(model) { render(scope, actions) }
                        }
                    }
                }
            }
        }
        interactions.Overlays(scope)
    }

    @Composable
    private fun Sections() {
        val c = Fm.colors
        Row(
            Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            for (entry in MastodonSection.entries) {
                val selected = entry == section
                Text(
                    entry.label,
                    Modifier
                        .clip(RoundedCornerShape(FmRadius.full))
                        .clickable(onClickLabel = entry.label) { section = entry }
                        .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                    style = Fm.text.labelLarge.copy(
                        color = if (selected) c.accent else c.textSecondary,
                        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                    ),
                )
            }
        }
        HorizontalDivider(color = c.border)
    }

    @Composable
    private fun AccountMenu(navigator: Navigator, scope: CoroutineScope) {
        val session = MastodonState.session ?: return
        var open by remember { mutableStateOf(false) }
        Box {
            Box(Modifier.padding(horizontal = FmSpace.x1).clickable(onClickLabel = "Account") { open = true }) {
                MastodonAvatar(session.avatar, session.displayName, 30.dp)
            }
            FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                FmDropdownMenuItem(session.handle, onClick = {}, icon = "account_circle", enabled = false)
                FmDropdownMenuItem("Your profile", onClick = {
                    open = false
                    navigator.push(MastodonProfileScreen(session.accountId))
                }, icon = "person")
                FmDropdownMenuItem("Sign out", onClick = {
                    open = false
                    scope.launch { MastodonState.signOut() }
                }, icon = "logout", textColor = Fm.colors.danger)
            }
        }
    }

    @Composable
    private fun SignIn(scope: CoroutineScope) {
        val c = Fm.colors
        val context = LocalContext.current
        val busy = MastodonState.busy
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4)) {
            item(key = "sign-in") {
                FmCard(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                        Text("Sign in to Mastodon", style = Fm.text.titleMedium.copy(color = c.textPrimary))
                        Text(
                            "Your server handles the sign-in. ForkMesh keeps the sign-in on this phone only.",
                            style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                        )
                        if (MastodonState.notice.isNotEmpty()) {
                            Text(
                                MastodonState.notice,
                                style = Fm.text.bodySmall.copy(color = if (MastodonState.noticeIsError) c.danger else c.textSecondary),
                            )
                        }
                        FmTextField(
                            MastodonState.server,
                            { MastodonState.server = it.trim() },
                            hint = "mastodon.social",
                            helperText = "Your server",
                            enabled = !busy,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go),
                            keyboardActions = KeyboardActions(onGo = { if (!busy) startBrowser(scope, context) }),
                        )
                        FmToneButton(
                            if (busy && mode == SignInMode.Browser) "Opening…" else "Sign in",
                            onPressed = if (busy || MastodonState.server.isBlank()) null else ({ startBrowser(scope, context) }),
                            icon = "login",
                            tone = FmTone.Accent,
                            filled = true,
                            busy = busy && mode == SignInMode.Browser,
                            modifier = Modifier.fillMaxWidth(),
                            height = 44.dp,
                        )
                        when (mode) {
                            SignInMode.Code -> {
                                FmTextField(code, { code = it }, hint = "Code", helperText = "Paste the code the server showed, or the whole address", enabled = !busy)
                                FmToneButton(
                                    "Finish sign-in",
                                    onPressed = if (busy || code.isBlank()) null else ({ scope.launch { MastodonState.finish(code) } }),
                                    tone = FmTone.Accent,
                                    busy = busy,
                                )
                            }
                            SignInMode.Token -> {
                                FmTextField(token, { token = it }, hint = "Access token", helperText = "Made under Preferences → Development on your server", enabled = !busy)
                                FmToneButton(
                                    "Sign in with token",
                                    onPressed = if (busy || token.isBlank() || MastodonState.server.isBlank()) null else ({ scope.launch { MastodonState.withToken(token) } }),
                                    tone = FmTone.Accent,
                                    busy = busy,
                                )
                            }
                            SignInMode.Browser -> Unit
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            if (mode != SignInMode.Code) {
                                FmTextButton(onClick = { startCode(scope, context) }, enabled = !busy && MastodonState.server.isNotBlank()) { Text("Paste a code instead") }
                            }
                            if (mode != SignInMode.Token) {
                                FmTextButton(onClick = { mode = SignInMode.Token }, enabled = !busy) { Text("Use an access token instead") }
                            }
                        }
                    }
                }
            }
        }
    }

    private fun startBrowser(scope: CoroutineScope, context: android.content.Context) {
        mode = SignInMode.Browser
        scope.launch { MastodonState.start()?.let { NavigationFeature.openInBrowser(context, it) } }
    }

    private fun startCode(scope: CoroutineScope, context: android.content.Context) {
        mode = SignInMode.Code
        scope.launch { MastodonState.startCode()?.let { NavigationFeature.openInBrowser(context, it) } }
    }
}

internal class MastodonThreadScreen(private val id: String) : MastodonBaseScreen() {
    override val title: String get() = "Post"

    private var thread by mutableStateOf<MastodonThread?>(null)
    private var loading by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var error by mutableStateOf("")

    init {
        interactions.onPosted = { _, _ -> }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LeaveWhenSignedOut(navigator)
        LaunchedEffect(Unit) { if (thread == null) load(scope) }
        interactions.onPosted = { _, _ -> load(scope) }
        val actions = interactions.actions(navigator, scope)
        DetailScaffold(title, navigator, status = status) {
            FmPullToRefreshBox(isRefreshing = refreshing, onRefresh = { load(scope, pulled = true) }, modifier = Modifier.fillMaxSize()) {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                    val current = thread
                    when {
                        current == null && error.isNotEmpty() -> item(key = "failed") { FmEmptyState("forum", "Post not found", message = error) }
                        current == null -> item(key = "loading") { MastodonLoading() }
                        current.status.id in MastodonState.removed -> item(key = "gone") { FmEmptyState("forum", "Post not found", message = "It was deleted.") }
                        else -> {
                            if (error.isNotEmpty()) item(key = "error") { MastodonErrorLine(error) }
                            items(current.all.filter { it.id !in MastodonState.removed }, key = { "post-${it.id}" }) { post ->
                                MastodonPostRow(post, focused = post.id == current.status.id, actions = actions)
                                HorizontalDivider(color = Fm.colors.border)
                            }
                        }
                    }
                }
            }
        }
        interactions.Overlays(scope) { deleted -> if (deleted.id == id && navigator.top === this) navigator.pop() }
    }

    private fun load(scope: CoroutineScope, pulled: Boolean = false) {
        val client = MastodonState.client() ?: return
        if (loading) return
        loading = true
        refreshing = pulled
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) { client.thread(id) }
                result.all.forEach { MastodonState.patched.remove(it.id) }
                thread = result
                error = ""
            } catch (failure: CancellationException) {
                throw failure
            } catch (_: MastodonUnauthorized) {
                MastodonState.revoked()
            } catch (failure: Exception) {
                error = MastodonState.describe(failure)
            } finally {
                loading = false
                refreshing = false
            }
        }
    }
}

internal class MastodonProfileScreen(private val accountId: String) : MastodonBaseScreen() {
    private var account by mutableStateOf<MastodonAccount?>(null)
    private var relationship by mutableStateOf<MastodonRelationship?>(null)
    private var following by mutableStateOf(false)
    private var accountError by mutableStateOf("")
    private val posts = MastodonFeedModel("No posts yet", "Nothing posted here yet.") { client, next ->
        MastodonFeedModel.posts(client.timeline(MastodonSource.Account(accountId), next))
    }

    override val title: String get() = account?.let { "@${it.acct}" } ?: "Profile"

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        LeaveWhenSignedOut(navigator)
        LaunchedEffect(Unit) {
            if (account == null) loadAccount(scope)
            if (!posts.loaded) posts.load(scope)
        }
        val actions = interactions.actions(navigator, scope)
        DetailScaffold(
            title,
            navigator,
            actions = {
                account?.url?.takeIf { it.isNotEmpty() }?.let { url ->
                    FmIconButton("public", "Open in browser", onPressed = { NavigationFeature.openInBrowser(context, url) }, color = Fm.colors.textPrimary)
                }
            },
            status = status,
        ) {
            FmPullToRefreshBox(
                isRefreshing = posts.refreshing,
                onRefresh = {
                    loadAccount(scope)
                    posts.load(scope, pulled = true)
                },
                modifier = Modifier.fillMaxSize(),
            ) {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                    header(scope, actions)
                    with(posts) { render(scope, actions) }
                }
            }
        }
        interactions.Overlays(scope)
    }

    private fun LazyListScope.header(scope: CoroutineScope, actions: MastodonActions) {
        val current = account
        if (current == null) {
            if (accountError.isNotEmpty()) item(key = "account-error") { MastodonErrorLine(accountError) }
            return
        }
        item(key = "profile") {
            val c = Fm.colors
            Column(Modifier.fillMaxWidth().padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    MastodonAvatar(current.avatar, current.displayName, 64.dp)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(current.displayName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                        Text("@${current.acct}", maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                    if (current.id != MastodonState.session?.accountId) FollowButton(scope)
                }
                val bio = remember(current.note) { MastodonHtml.segments(current.note) }
                MastodonBody(bio, focused = false, actions = actions)
                Text(
                    listOf(
                        count(current.statusesCount, "post", "posts"),
                        count(current.followingCount, "following", "following"),
                        count(current.followersCount, "follower", "followers"),
                    ).joinToString(" · "),
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
            HorizontalDivider(color = c.border)
        }
    }

    private fun count(value: Int, one: String, many: String) = "$value ${if (value == 1) one else many}"

    @Composable
    private fun FollowButton(scope: CoroutineScope) {
        val current = relationship ?: return
        val label = when {
            current.following -> "Unfollow"
            current.requested -> "Requested"
            else -> "Follow"
        }
        FmToneButton(
            label,
            onPressed = if (following) null else ({ follow(scope, !(current.following || current.requested)) }),
            tone = if (current.following || current.requested) FmTone.Neutral else FmTone.Accent,
            filled = !(current.following || current.requested),
            busy = following,
        )
    }

    private fun loadAccount(scope: CoroutineScope) {
        val client = MastodonState.client() ?: return
        scope.launch {
            try {
                val (loaded, relation) = withContext(Dispatchers.IO) { client.account(accountId) to runCatching { client.relationship(accountId) }.getOrNull() }
                account = loaded
                relationship = relation
                accountError = ""
            } catch (failure: CancellationException) {
                throw failure
            } catch (_: MastodonUnauthorized) {
                MastodonState.revoked()
            } catch (failure: Exception) {
                accountError = MastodonState.describe(failure)
            }
        }
    }

    private fun follow(scope: CoroutineScope, on: Boolean) {
        val client = MastodonState.client() ?: return
        following = true
        scope.launch {
            try {
                relationship = withContext(Dispatchers.IO) { client.follow(accountId, on) }
            } catch (failure: CancellationException) {
                throw failure
            } catch (_: MastodonUnauthorized) {
                MastodonState.revoked()
            } catch (failure: Exception) {
                status.error("Could not update the follow: ${MastodonState.describe(failure)}")
            } finally {
                following = false
            }
        }
    }
}

internal class MastodonTagScreen(private val name: String) : MastodonBaseScreen() {
    override val title: String get() = "#$name"

    private val posts = MastodonFeedModel("No posts yet", "Nobody on this server has used #$name yet.") { client, next ->
        MastodonFeedModel.posts(client.timeline(MastodonSource.Tag(name), next))
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LeaveWhenSignedOut(navigator)
        LaunchedEffect(Unit) { if (!posts.loaded) posts.load(scope) }
        val actions = interactions.actions(navigator, scope)
        DetailScaffold(title, navigator, status = status) {
            FmPullToRefreshBox(isRefreshing = posts.refreshing, onRefresh = { posts.load(scope, pulled = true) }, modifier = Modifier.fillMaxSize()) {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                    with(posts) { render(scope, actions) }
                }
            }
        }
        interactions.Overlays(scope)
    }
}

internal class MastodonSearchScreen : MastodonBaseScreen() {
    override val title: String get() = "Search"

    private var query by mutableStateOf("")
    private var results by mutableStateOf<MastodonSearch?>(null)
    private var searching by mutableStateOf(false)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LeaveWhenSignedOut(navigator)
        val actions = interactions.actions(navigator, scope)
        DetailScaffold(title, navigator, status = status) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
                item(key = "field") {
                    Row(Modifier.fillMaxWidth().padding(FmSpace.x4), verticalAlignment = Alignment.CenterVertically) {
                        FmTextField(
                            query,
                            { query = it },
                            Modifier.weight(1f),
                            hint = "People, hashtags or posts",
                            autofocus = results == null,
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                            keyboardActions = KeyboardActions(onSearch = { search(scope) }),
                        )
                        Spacer(Modifier.width(FmSpace.x2))
                        FmToneButton("Search", onPressed = if (query.isBlank() || searching) null else ({ search(scope) }), tone = FmTone.Accent, busy = searching)
                    }
                }
                if (error.isNotEmpty()) item(key = "error") { MastodonErrorLine(error) }
                val found = results
                if (found != null) results(found, navigator, actions)
            }
        }
        interactions.Overlays(scope)
    }

    private fun LazyListScope.results(found: MastodonSearch, navigator: Navigator, actions: MastodonActions) {
        if (found.accounts.isEmpty() && found.hashtags.isEmpty() && found.statuses.isEmpty()) {
            item(key = "none") { FmEmptyState("search", "Nothing found", message = "Try a full address such as @name@server.") }
            return
        }
        if (found.accounts.isNotEmpty()) {
            item(key = "people") { SectionTitle("People") }
            items(found.accounts, key = { "account-${it.id}" }) { account ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable { navigator.push(MastodonProfileScreen(account.id)) }
                        .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    MastodonAvatar(account.avatar, account.displayName)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(account.displayName, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary, fontWeight = FontWeight.SemiBold))
                        Text("@${account.acct}", maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
                    }
                }
            }
        }
        if (found.hashtags.isNotEmpty()) {
            item(key = "tags") { SectionTitle("Hashtags") }
            items(found.hashtags, key = { "tag-${it.name}" }) { tag ->
                Text(
                    "#${tag.name}",
                    Modifier
                        .fillMaxWidth()
                        .clickable { navigator.push(MastodonTagScreen(tag.name.lowercase())) }
                        .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                    style = Fm.text.bodyMedium.copy(color = Fm.colors.accent),
                )
            }
        }
        if (found.statuses.isNotEmpty()) {
            item(key = "posts") { SectionTitle("Posts") }
            items(found.statuses, key = { "post-${it.id}" }) { post ->
                MastodonPostRow(post, focused = false, actions = actions)
                HorizontalDivider(color = Fm.colors.border)
            }
        }
    }

    @Composable
    private fun SectionTitle(text: String) {
        Text(
            text,
            Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1),
            style = Fm.text.labelLarge.copy(color = Fm.colors.textSecondary, fontWeight = FontWeight.SemiBold),
        )
    }

    private fun search(scope: CoroutineScope) {
        val text = query.trim()
        val client = MastodonState.client() ?: return
        if (text.isEmpty() || searching) return
        searching = true
        scope.launch {
            try {
                results = withContext(Dispatchers.IO) { client.search(text) }
                error = ""
            } catch (failure: CancellationException) {
                throw failure
            } catch (_: MastodonUnauthorized) {
                MastodonState.revoked()
            } catch (failure: Exception) {
                error = MastodonState.describe(failure)
            } finally {
                searching = false
            }
        }
    }
}
