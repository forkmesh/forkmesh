package com.forkmesh.nativeapp.feature.rssreader

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.rssreader.RssFeed
import com.forkmesh.nativeapp.rssreader.RssFolder
import com.forkmesh.nativeapp.rssreader.RssFormat
import com.forkmesh.nativeapp.rssreader.RssLibrary
import com.forkmesh.nativeapp.rssreader.RssScope
import com.forkmesh.nativeapp.rssreader.RssSidebarEntry
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPromptDialog
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

internal sealed class RssDialog {
    data object AddFeed : RssDialog()
    data object AddFolder : RssDialog()
    data class RenameFolder(val folder: RssFolder) : RssDialog()
    data class DeleteFolder(val folder: RssFolder) : RssDialog()
    data class Unsubscribe(val feed: RssFeed) : RssDialog()
    data class MarkAll(val scope: RssScope, val title: String, val count: Int) : RssDialog()
}

internal fun scopeTitle(library: RssLibrary, scope: RssScope): String = when (scope) {
    RssScope.Today -> "Today"
    RssScope.Unread -> "All Unread"
    RssScope.Starred -> "Starred"
    is RssScope.Feed -> library.feed(scope.id)?.displayTitle ?: "Feed"
    is RssScope.Folder -> library.folder(scope.id)?.name ?: "Folder"
}

internal class RssFeedsScreen : Screen() {
    override val title: String get() = "Feeds"
    override val hidesShellChrome: Boolean get() = true
    private var dialog by mutableStateOf<RssDialog?>(null)
    private var pulled by mutableStateOf(false)
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        val service = RssReaderFeature.service
        val version by service.version.collectAsState()
        val refreshing by service.refreshing.collectAsState()
        LaunchedEffect(Unit) { service.open() }
        LaunchedEffect(refreshing) { if (!refreshing) pulled = false }
        val library = service.library
        val settings = library?.settings()
        val now = remember(version) { System.currentTimeMillis() }
        val counts = remember(version) { library?.counts(now) }
        val unreadOnly = settings?.feedsUnreadOnly == true
        val entries = remember(version, unreadOnly) { if (library != null && counts != null) library.sidebar(unreadOnly, counts) else emptyList() }
        var addMenu by remember { mutableStateOf(false) }
        val scope = rememberCoroutineScope()
        RssScaffold(
            "Feeds",
            navigator,
            subtitle = RssFormat.status(refreshing, library?.lastRefresh() ?: 0),
            actions = {
                FmIconButton(
                    if (unreadOnly) "filter_alt" else "filter_list",
                    if (unreadOnly) "Show all feeds" else "Show only feeds with unread articles",
                    onPressed = { service.mutate { it.updateSettings { s -> s.copy(feedsUnreadOnly = !s.feedsUnreadOnly) } } },
                    color = if (unreadOnly) Fm.colors.accent else Fm.colors.textSecondary,
                )
            },
            bottomBar = {
                FmIconButton("settings", "Settings", onPressed = { navigator.push(RssSettingsScreen()) }, color = Fm.colors.accent)
                Spacer(Modifier.weight(1f))
                Box {
                    FmIconButton("add", "Add", onPressed = { addMenu = true }, color = Fm.colors.accent, iconSize = 26.dp)
                    FmDropdownMenu(expanded = addMenu, onDismissRequest = { addMenu = false }) {
                        FmDropdownMenuItem("Add Feed", onClick = { addMenu = false; dialog = RssDialog.AddFeed }, icon = "feed")
                        FmDropdownMenuItem("Add Folder", onClick = { addMenu = false; dialog = RssDialog.AddFolder }, icon = "folder_outlined")
                    }
                }
            },
        ) {
            FmPullToRefreshBox(
                isRefreshing = pulled && refreshing,
                onRefresh = {
                    pulled = true
                    service.refresh(force = true)
                },
                modifier = Modifier.fillMaxSize(),
            ) {
                if (library == null || counts == null || settings == null) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                } else {
                    LazyColumn(Modifier.fillMaxSize()) {
                        item(key = "status") { FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4)) }
                        val smartOpen = "section:smart" !in settings.collapsed
                        item(key = "smart") { SectionHeader("Smart Feeds", smartOpen) { toggle("section:smart") } }
                        if (smartOpen) {
                            item(key = "today") { SmartRow("wb_sunny_outlined", RssColors.today, "Today", counts.today) { navigator.push(RssTimelineScreen(RssScope.Today)) } }
                            item(key = "unread") { SmartRow("circle", RssColors.unread, "All Unread", counts.unread) { navigator.push(RssTimelineScreen(RssScope.Unread)) } }
                            item(key = "starred") { SmartRow("star", RssColors.star, "Starred", counts.starred) { navigator.push(RssTimelineScreen(RssScope.Starred)) } }
                        }
                        val localOpen = "section:local" !in settings.collapsed
                        item(key = "local") { SectionHeader("On This Phone", localOpen) { toggle("section:local") } }
                        if (localOpen) {
                            if (library.feeds().isEmpty()) {
                                item(key = "empty") { EmptyFeeds(onAdd = { dialog = RssDialog.AddFeed }, onBlog = { addBlog(scope) }) }
                            } else if (entries.isEmpty()) {
                                item(key = "caught-up") {
                                    Text("No feeds with unread articles.", Modifier.padding(FmSpace.x4), style = Fm.text.bodyMedium.copy(color = Fm.colors.textTertiary))
                                }
                            }
                            entryItems(entries, settings.collapsed, counts.byFeed, counts.byFolder, navigator)
                        }
                        item(key = "end") { Spacer(Modifier.padding(FmSpace.x5)) }
                    }
                }
            }
            Dialogs(navigator)
        }
    }

    private fun toggle(key: String) {
        RssReaderFeature.service.mutate { library ->
            library.updateSettings { s -> s.copy(collapsed = if (key in s.collapsed) s.collapsed - key else s.collapsed + key) }
        }
    }

    private fun addBlog(scope: CoroutineScope) {
        val service = RssReaderFeature.service
        service.mutate { it.updateSettings { s -> s.copy(blogFeedOffered = true) } }
        status.show("Adding the ForkMesh Blog…")
        scope.launch {
            runCatching { service.addFeed(RssLibrary.FORKMESH_BLOG_FEED, "", "") }
                .onSuccess { status.show("Subscribed to ${it.displayTitle}.") }
                .onFailure { status.error(service.errorText(it)) }
        }
    }

    private fun LazyListScope.entryItems(
        entries: List<RssSidebarEntry>,
        collapsed: Set<String>,
        byFeed: Map<String, Int>,
        byFolder: Map<String, Int>,
        navigator: Navigator,
    ) {
        for (entry in entries) {
            when (entry) {
                is RssSidebarEntry.FeedEntry -> item(key = "feed:${entry.feed.id}") {
                    FeedRow(entry.feed, byFeed[entry.feed.id] ?: 0, false, navigator)
                }
                is RssSidebarEntry.FolderEntry -> {
                    val key = "folder:${entry.folder.id}"
                    val open = key !in collapsed
                    item(key = key) { FolderRow(entry.folder, byFolder[entry.folder.id] ?: 0, open, navigator) }
                    if (open) {
                        for (feed in entry.feeds) item(key = "$key/${feed.id}") { FeedRow(feed, byFeed[feed.id] ?: 0, true, navigator) }
                    }
                }
            }
        }
    }

    @Composable
    private fun SectionHeader(title: String, open: Boolean, onToggle: () -> Unit) {
        Row(
            Modifier.fillMaxWidth().clickable(onClick = onToggle).padding(start = FmSpace.x4, end = FmSpace.x3, top = FmSpace.x4, bottom = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(title, Modifier.weight(1f), style = Fm.text.titleSmall.copy(fontWeight = FontWeight.SemiBold, color = Fm.colors.textSecondary))
            FmIcon(if (open) "expand_more" else "chevron_right", size = 20.dp, tint = Fm.colors.textTertiary)
        }
    }

    @Composable
    private fun SmartRow(icon: String, tint: Color, title: String, count: Int, onTap: () -> Unit) {
        SourceRow(title, count, indent = false, onTap = onTap, onLongPress = null) { FmIcon(icon, size = 22.dp, tint = tint) }
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    private fun SourceRow(
        title: String,
        count: Int,
        indent: Boolean,
        onTap: () -> Unit,
        onLongPress: (() -> Unit)?,
        trailing: (@Composable () -> Unit)? = null,
        leading: @Composable () -> Unit,
    ) {
        val c = Fm.colors
        Column(Modifier.fillMaxWidth().background(c.bgRaised)) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .combinedClickable(onClick = onTap, onLongClick = onLongPress)
                    .heightIn(min = 48.dp)
                    .padding(start = if (indent) FmSpace.x7 else FmSpace.x4, end = FmSpace.x4),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.size(24.dp), contentAlignment = Alignment.Center) { leading() }
                Spacer(Modifier.width(FmSpace.x3))
                Text(title, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyLarge)
                if (count > 0) Text("$count", style = Fm.text.bodyMedium.copy(color = c.textTertiary))
                if (trailing != null) {
                    Spacer(Modifier.width(FmSpace.x1))
                    trailing()
                }
            }
            HorizontalDivider(Modifier.padding(start = if (indent) 84.dp else 52.dp), thickness = 1.dp, color = c.border)
        }
    }

    @Composable
    private fun FeedRow(feed: RssFeed, count: Int, indent: Boolean, navigator: Navigator) {
        var menu by remember { mutableStateOf(false) }
        val service = RssReaderFeature.service
        Box {
            SourceRow(
                feed.displayTitle,
                count,
                indent,
                onTap = { navigator.push(RssTimelineScreen(RssScope.Feed(feed.id))) },
                onLongPress = { menu = true },
                trailing = if (feed.errorCount > 0) ({ FmIcon("error_outline", size = 16.dp, tint = Fm.colors.warning) }) else null,
            ) { FeedIcon(feed) }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem("Mark All as Read", onClick = {
                    menu = false
                    markAll(RssScope.Feed(feed.id), feed.displayTitle, count)
                }, icon = "done_all", enabled = count > 0)
                FmDropdownMenuItem("Feed Info", onClick = { menu = false; navigator.push(RssFeedInfoScreen(feed.id)) }, icon = "info")
                FmDropdownMenuItem("Unsubscribe", onClick = { menu = false; dialog = RssDialog.Unsubscribe(feed) }, icon = "delete", textColor = Fm.colors.danger)
            }
        }
        service.version.collectAsState().value
    }

    @Composable
    private fun FolderRow(folder: RssFolder, count: Int, open: Boolean, navigator: Navigator) {
        var menu by remember { mutableStateOf(false) }
        Box {
            SourceRow(
                folder.name,
                count,
                indent = false,
                onTap = { navigator.push(RssTimelineScreen(RssScope.Folder(folder.id))) },
                onLongPress = { menu = true },
                trailing = {
                    FmIconButton(if (open) "expand_more" else "chevron_right", if (open) "Collapse" else "Expand", onPressed = { toggle("folder:${folder.id}") }, iconSize = 20.dp)
                },
            ) { FmIcon("folder", size = 22.dp, tint = RssColors.unread) }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem("Mark All as Read", onClick = { menu = false; markAll(RssScope.Folder(folder.id), folder.name, count) }, icon = "done_all", enabled = count > 0)
                FmDropdownMenuItem("Rename Folder", onClick = { menu = false; dialog = RssDialog.RenameFolder(folder) }, icon = "edit")
                FmDropdownMenuItem("Delete Folder", onClick = { menu = false; dialog = RssDialog.DeleteFolder(folder) }, icon = "delete", textColor = Fm.colors.danger)
            }
        }
    }

    private fun markAll(scope: RssScope, title: String, count: Int) {
        val service = RssReaderFeature.service
        val library = service.library ?: return
        if (count <= 0) return
        if (library.settings().confirmMarkAllRead) {
            dialog = RssDialog.MarkAll(scope, title, count)
        } else {
            markScopeRead(scope)
        }
    }

    @Composable
    private fun EmptyFeeds(onAdd: () -> Unit, onBlog: () -> Unit) {
        Column(
            Modifier.fillMaxWidth().background(Fm.colors.bgRaised).padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Text("No subscriptions yet", style = Fm.text.titleSmall)
            Text(
                "Add a site or feed address, or import an OPML file from Settings. Feeds are checked by this phone and stay on it.",
                style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmTextButton(onClick = onAdd) { Text("Add Feed") }
                FmTextButton(onClick = onBlog) { Text("Add the ForkMesh Blog") }
            }
        }
    }

    @Composable
    private fun Dialogs(navigator: Navigator) {
        val service = RssReaderFeature.service
        when (val current = dialog) {
            null -> {}
            RssDialog.AddFeed -> AddFeedDialog(onDone = { feed ->
                dialog = null
                if (feed != null) status.show("Subscribed to ${feed.displayTitle}.")
            })
            RssDialog.AddFolder -> FmPromptDialog(
                title = "Add Folder",
                hint = "Folder name",
                confirmLabel = "Add",
                onDismissRequest = { dialog = null },
                onConfirm = { name ->
                    dialog = null
                    runCatching { service.mutate { it.addFolder(name) } }.onFailure { status.error(service.errorText(it)) }
                },
            )
            is RssDialog.RenameFolder -> FmPromptDialog(
                title = "Rename Folder",
                initialValue = current.folder.name,
                confirmLabel = "Rename",
                onDismissRequest = { dialog = null },
                onConfirm = { name ->
                    dialog = null
                    runCatching { service.mutate { it.renameFolder(current.folder.id, name) } }.onFailure { status.error(service.errorText(it)) }
                },
            )
            is RssDialog.DeleteFolder -> FmConfirmDialog(
                title = "Delete “${current.folder.name}”?",
                body = "The folder is removed. Its feeds stay subscribed and move to the top level.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { dialog = null },
                onConfirm = {
                    dialog = null
                    service.mutate { it.deleteFolder(current.folder.id) }
                },
            )
            is RssDialog.Unsubscribe -> FmConfirmDialog(
                title = "Unsubscribe from “${current.feed.displayTitle}”?",
                body = "Its articles, including starred ones, are removed from this phone.",
                confirmLabel = "Unsubscribe",
                destructive = true,
                onDismissRequest = { dialog = null },
                onConfirm = {
                    dialog = null
                    service.mutate { it.unsubscribe(current.feed.id, System.currentTimeMillis()) }
                },
            )
            is RssDialog.MarkAll -> FmConfirmDialog(
                title = "Mark All as Read?",
                body = "Mark ${RssFormat.plural(current.count, "article")} in “${current.title}” as read?",
                confirmLabel = "Mark All as Read",
                onDismissRequest = { dialog = null },
                onConfirm = {
                    dialog = null
                    markScopeRead(current.scope)
                },
            )
        }
    }
}

internal fun markScopeRead(scope: RssScope) {
    val service = RssReaderFeature.service
    service.mutate { library ->
        library.setRead(library.articles(scope, System.currentTimeMillis()).filter { !it.read }.map { it.id }, true)
    }
}

@Composable
internal fun AddFeedDialog(initial: String = "", onDone: (RssFeed?) -> Unit) {
    val service = RssReaderFeature.service
    val scope = rememberCoroutineScope()
    var address by remember { mutableStateOf(initial) }
    var name by remember { mutableStateOf("") }
    var folderId by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var folderMenu by remember { mutableStateOf(false) }
    val folders = service.library?.folders().orEmpty()
    FmConfirmDialog(
        title = "Add Feed",
        confirmLabel = "Add",
        busy = busy,
        onDismissRequest = { if (!busy) onDone(null) },
        onConfirm = {
            if (address.isBlank() || busy) return@FmConfirmDialog
            busy = true
            error = null
            scope.launch {
                runCatching { service.addFeed(address, folderId, name) }
                    .onSuccess { onDone(it) }
                    .onFailure { error = service.errorText(it) }
                busy = false
            }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            FmTextField(
                value = address,
                onValueChange = { address = it },
                hint = "Feed or site address",
                autofocus = true,
                errorText = error,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
            )
            FmTextField(value = name, onValueChange = { name = it }, hint = "Name (optional)")
            Box {
                SettingsRow(
                    "Folder",
                    value = folders.firstOrNull { it.id == folderId }?.name ?: "None",
                    onClick = { folderMenu = true },
                )
                FmDropdownMenu(expanded = folderMenu, onDismissRequest = { folderMenu = false }) {
                    FmDropdownMenuItem("None", onClick = { folderId = ""; folderMenu = false })
                    for (folder in folders) FmDropdownMenuItem(folder.name, onClick = { folderId = folder.id; folderMenu = false }, icon = "folder")
                }
            }
        }
    }
}
