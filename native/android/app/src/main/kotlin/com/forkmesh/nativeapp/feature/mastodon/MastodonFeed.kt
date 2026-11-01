package com.forkmesh.nativeapp.feature.mastodon

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.mastodon.MastodonClient
import com.forkmesh.nativeapp.mastodon.MastodonNotification
import com.forkmesh.nativeapp.mastodon.MastodonPage
import com.forkmesh.nativeapp.mastodon.MastodonStatus
import com.forkmesh.nativeapp.mastodon.MastodonUnauthorized
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal sealed class MastodonItem {
    abstract val key: String

    data class Post(val status: MastodonStatus) : MastodonItem() {
        override val key: String get() = "post-${status.id}"
    }

    data class Note(val notification: MastodonNotification) : MastodonItem() {
        override val key: String get() = "note-${notification.id}"
    }
}

internal class MastodonFeedModel(
    private val emptyTitle: String,
    private val emptyMessage: String,
    private val fetch: (MastodonClient, String?) -> MastodonPage<MastodonItem>,
) {
    var items by mutableStateOf<List<MastodonItem>>(emptyList())
    var next by mutableStateOf<String?>(null)
    var loaded by mutableStateOf(false)
    var loading by mutableStateOf(false)
    var loadingMore by mutableStateOf(false)
    var refreshing by mutableStateOf(false)
    var error by mutableStateOf("")

    fun load(scope: CoroutineScope, append: Boolean = false, pulled: Boolean = false) {
        if (loading) return
        val client = MastodonState.client() ?: return
        val cursor = if (append) next else null
        if (append && cursor == null) return
        loading = true
        loadingMore = append
        refreshing = pulled
        scope.launch {
            try {
                val page = withContext(Dispatchers.IO) { fetch(client, cursor) }
                page.items.forEach { item -> if (item is MastodonItem.Post) MastodonState.patched.remove(item.status.shown.id) }
                items = if (append) (items + page.items).distinctBy { it.key } else page.items
                next = page.next
                error = ""
                loaded = true
            } catch (failure: CancellationException) {
                throw failure
            } catch (_: MastodonUnauthorized) {
                MastodonState.revoked()
            } catch (failure: Exception) {
                error = MastodonState.describe(failure)
            } finally {
                loading = false
                loadingMore = false
                refreshing = false
            }
        }
    }

    fun prepend(status: MastodonStatus) {
        items = listOf<MastodonItem>(MastodonItem.Post(status)) + items
    }

    fun LazyListScope.render(scope: CoroutineScope, actions: MastodonActions) {
        val visible = items.filter { item -> item !is MastodonItem.Post || item.status.shown.id !in MastodonState.removed }
        when {
            !loaded && error.isNotEmpty() -> item(key = "failed") {
                FmEmptyState("cloud_off", "Couldn't load Mastodon", message = "$error Pull down to try again.")
            }
            !loaded -> item(key = "loading") { MastodonLoading() }
            visible.isEmpty() -> item(key = "empty") { FmEmptyState("forum", emptyTitle, message = emptyMessage) }
            else -> {
                if (error.isNotEmpty()) item(key = "error") { MastodonErrorLine(error) }
                items(visible, key = { it.key }) { item ->
                    when (item) {
                        is MastodonItem.Post -> MastodonPostRow(item.status, focused = false, actions = actions)
                        is MastodonItem.Note -> MastodonNotificationRow(item.notification, actions)
                    }
                    HorizontalDivider(color = Fm.colors.border)
                }
                val cursor = next
                if (cursor != null) {
                    item(key = "more-$cursor") {
                        LaunchedEffect(cursor) { if (!loading && error.isEmpty()) load(scope, append = true) }
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
        }
    }

    companion object {
        fun posts(page: MastodonPage<MastodonStatus>) = MastodonPage(page.items.map { MastodonItem.Post(it) }, page.next)

        fun notes(page: MastodonPage<MastodonNotification>) = MastodonPage(page.items.map { MastodonItem.Note(it) }, page.next)
    }
}

internal class MastodonInteractions(private val status: FmStatus) {
    var composer by mutableStateOf<MastodonComposer?>(null)
    var confirmDelete by mutableStateOf<MastodonStatus?>(null)
    var onPosted: (MastodonStatus, MastodonStatus?) -> Unit = { _, _ -> }

    fun compose(replyTo: MastodonStatus? = null) {
        composer = MastodonComposer(replyTo, MastodonState.session?.acct.orEmpty())
    }

    @Composable
    fun actions(navigator: Navigator, scope: CoroutineScope): MastodonActions {
        val context = LocalContext.current
        return MastodonActions(
            me = MastodonState.session?.accountId.orEmpty(),
            openThread = { post -> navigator.push(MastodonThreadScreen(post.id)) },
            openProfile = { id -> navigator.push(MastodonProfileScreen(id)) },
            openTag = { name -> navigator.push(MastodonTagScreen(name)) },
            openLink = { url -> NavigationFeature.openInBrowser(context, url) },
            reply = { post -> compose(post) },
            boost = { post -> toggle(scope, post, MastodonToggle.Boost) },
            favourite = { post -> toggle(scope, post, MastodonToggle.Favourite) },
            bookmark = { post -> toggle(scope, post, MastodonToggle.Bookmark) },
            delete = { post -> confirmDelete = post },
            copyLink = { post -> copyLink(context, post.url) },
        )
    }

    private fun toggle(scope: CoroutineScope, post: MastodonStatus, kind: MastodonToggle) {
        scope.launch {
            MastodonState.toggle(post, kind)?.let { status.error(it) }
        }
    }

    @Composable
    fun Overlays(scope: CoroutineScope, onDeleted: (MastodonStatus) -> Unit = {}) {
        confirmDelete?.let { post ->
            FmConfirmDialog(
                title = "Delete this post?",
                body = "It is removed from your server and the servers it reached.",
                confirmLabel = "Delete",
                destructive = true,
                onConfirm = {
                    confirmDelete = null
                    scope.launch {
                        val error = MastodonState.delete(post)
                        if (error == null) onDeleted(post) else status.error(error)
                    }
                },
                onDismissRequest = { confirmDelete = null },
            )
        }
        composer?.let { current ->
            MastodonComposeSheet(
                current,
                onDismiss = { composer = null },
                onPosted = { created ->
                    composer = null
                    current.replyTo?.let { parent ->
                        val latest = MastodonState.current(parent)
                        MastodonState.patched[parent.id] = latest.copy(repliesCount = latest.repliesCount + 1)
                    }
                    status.show("Posted.")
                    onPosted(created, current.replyTo)
                },
            )
        }
    }

    private fun copyLink(context: Context, url: String) {
        if (url.isEmpty()) return
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        clipboard?.setPrimaryClip(ClipData.newPlainText("Mastodon post", url))
        if (clipboard == null) status.error("Could not reach the clipboard.") else status.show("Link copied")
    }
}

@Composable
internal fun MastodonLoading() {
    Box(Modifier.fillMaxWidth().padding(FmSpace.x6), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
}

@Composable
internal fun MastodonErrorLine(text: String) {
    Text(text, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), style = Fm.text.bodySmall.copy(color = Fm.colors.danger))
}
