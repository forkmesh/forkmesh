package com.forkmesh.nativeapp.feature.hosts

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
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
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.hosts.FileEntry
import com.forkmesh.nativeapp.hosts.FileListing
import com.forkmesh.nativeapp.hosts.FileRoot
import com.forkmesh.nativeapp.hosts.FileSlice
import com.forkmesh.nativeapp.hosts.GitChangeKind
import com.forkmesh.nativeapp.hosts.GitRemoteAction
import com.forkmesh.nativeapp.hosts.GitWorkingStatus
import com.forkmesh.nativeapp.hosts.RemoteConfirmation
import com.forkmesh.nativeapp.hosts.RemoteFormat
import com.forkmesh.nativeapp.hosts.RemotePath
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

internal class FileRootsScreen : Screen() {
    override val title: String get() = "Files"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { DesktopRemoteFeature.files }
        val tick = rememberRemoteTick(store)
        LaunchedEffect(store) { offMain { store.loadRoots() } }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { Thread { store.loadRoots() }.start() }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                if (store.unsupported) item { OutdatedNote() }
                else if (store.error.isNotEmpty()) item { Text(store.error, style = Fm.text.bodySmall.copy(color = c.danger)) }
                val groups = store.groups
                if (groups.isEmpty() && store.loadingRoots) item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                if (groups.isEmpty() && !store.loadingRoots && store.error.isEmpty()) {
                    item { FmEmptyState(icon = "folder_outlined", title = "No repositories", message = "The desktop has no local repositories to browse.") }
                }
                for (group in groups) {
                    item(key = "g-" + group.repo.id) { RootRow(group.repo, store.changedRoots.contains(group.repo.id), navigator) }
                    items(group.worktrees, key = { "w-" + it.id }) { root ->
                        Row {
                            Spacer(Modifier.width(FmSpace.x5))
                            Column(Modifier.weight(1f)) { RootRow(root, store.changedRoots.contains(root.id), navigator) }
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun RootRow(root: FileRoot, changed: Boolean, navigator: Navigator) {
        val c = Fm.colors
        FmListRow(
            root.label,
            subtitle = listOf(if (root.branch.isEmpty()) "" else root.branch, if (changed) "changed" else "").filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { null },
            leading = { FmIcon(if (root.kind.wire == "worktree") "account_tree_outlined" else "folder_outlined", size = 20.dp, tint = c.textSecondary) },
            trailing = { FmIconButton("difference_outlined", "Changes", onPressed = { navigator.push(WorkingStatusScreen(root.id, root.label)) }, color = c.textSecondary) },
            onTap = { navigator.push(FileListingScreen(root.id, root.label, "")) },
            minHeight = 52.dp,
        )
    }
}

internal class FileListingScreen(private val root: String, private val rootLabel: String, private val path: String) : Screen() {
    override val title: String get() = if (path.isEmpty()) rootLabel else path.substringAfterLast('/')
    private var listing by mutableStateOf<FileListing?>(null)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.files }
        suspend fun load() {
            try {
                listing = offMain { store.list(root, path) }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            }
        }
        LaunchedEffect(root, path) {
            listing = store.cached(root, path)
            load()
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                if (path.isNotEmpty()) {
                    item {
                        Text(
                            (listOf(rootLabel) + RemotePath.crumbs(path).map { it.first }).joinToString(" / "),
                            style = Fm.text.labelSmall.copy(color = c.textTertiary),
                        )
                    }
                }
                val current = listing
                if (current == null && error.isEmpty()) item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                if (current != null) {
                    val sorted = current.entries.sortedWith(compareBy<FileEntry> { !it.kind.isFolder }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name })
                    items(sorted, key = { it.name }) { entry -> EntryRow(entry, navigator) }
                    if (current.truncated) item { Text("Showing ${current.entries.size} of ${current.total} entries.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    if (current.entries.isEmpty()) item { Text("This folder is empty.", style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
    }

    @Composable
    private fun EntryRow(entry: FileEntry, navigator: Navigator) {
        val c = Fm.colors
        val child = RemotePath.join(path, entry.name)
        val note = when {
            entry.sensitive -> "Kept on the desktop"
            entry.escapes -> "Points outside the repository"
            else -> entry.sizeLabel
        }
        FmListRow(
            entry.name,
            subtitle = note.ifEmpty { null },
            leading = { FmIcon(if (entry.kind.isFolder) "folder_outlined" else if (entry.sensitive) "lock_outline" else "description_outlined", size = 20.dp, tint = c.textSecondary) },
            onTap = if (!entry.canOpen) null else ({
                if (entry.kind.isFolder) navigator.push(FileListingScreen(root, rootLabel, child)) else navigator.push(RemoteFileScreen(root, child))
            }),
            showChevron = entry.canOpen,
            minHeight = 48.dp,
        )
    }
}

internal class RemoteFileScreen(private val root: String, private val path: String) : Screen() {
    override val title: String get() = path.substringAfterLast('/')
    private var slices by mutableStateOf<List<FileSlice>>(emptyList())
    private var error by mutableStateOf("")
    private var loading by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.files }
        fun loadMore() {
            if (loading) return
            val offset = slices.lastOrNull()?.nextOffset ?: 0.0
            loading = true
            scope.launch {
                try {
                    val next = offMain { store.read(root, path, offset) }
                    slices = slices + next
                    error = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = describeRemote(failure)
                } finally {
                    loading = false
                }
            }
        }
        LaunchedEffect(root, path) { if (slices.isEmpty()) loadMore() }
        DetailScaffold(title, navigator) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val first = slices.firstOrNull()
                if (first != null) {
                    item { Text("${RemoteFormat.bytes(first.size)} · $path", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    if (first.binary) {
                        val bytes = first.data
                        val image = if (bytes != null && RemotePath.isImage(path)) runCatching { BitmapFactory.decodeByteArray(bytes, 0, bytes.size) }.getOrNull() else null
                        item {
                            if (image != null) {
                                Image(image.asImageBitmap(), contentDescription = title, modifier = Modifier.fillMaxWidth())
                            } else {
                                Text("This is a binary file. Open it on the desktop.", style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                            }
                        }
                    } else {
                        items(slices.size) { index ->
                            SelectionContainer {
                                Text(
                                    slices[index].text,
                                    style = Fm.text.labelSmall.copy(fontFamily = FmType.mono, color = c.textPrimary),
                                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                                )
                            }
                        }
                        val last = slices.last()
                        if (!last.eof) {
                            item { FmToneButton("Load more", onPressed = if (loading) null else ({ loadMore() })) }
                        }
                    }
                } else if (error.isEmpty()) {
                    item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
    }
}

internal class WorkingStatusScreen(private val root: String, private val label: String) : Screen() {
    override val title: String get() = label
    private var status by mutableStateOf<GitWorkingStatus?>(null)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")
    private var pending by mutableStateOf<RemoteConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val store = remember { DesktopRemoteFeature.files }
        val tick = rememberRemoteTick(store)
        suspend fun load() {
            try {
                status = offMain { store.status(root) }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = describeRemote(failure)
            }
        }
        LaunchedEffect(root) {
            status = store.cachedStatus(root)
            load()
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            tick.let { }
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                val current = status
                if (current != null) {
                    item {
                        FmCard(Modifier.fillMaxWidth()) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    FmIcon("call_split", size = 18.dp, tint = c.textSecondary)
                                    Spacer(Modifier.width(FmSpace.x2))
                                    Text(current.branch.ifEmpty { "Detached" }, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                                }
                                Text(current.syncLine, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                Text(current.changesLine, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                if (root in store.changedRoots) Text("Changed on the desktop since this was read.", style = Fm.text.labelSmall.copy(color = c.warning))
                            }
                        }
                    }
                    item {
                        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            for (action in GitRemoteAction.entries) {
                                val enabled = action == GitRemoteAction.Fetch || current.canFastForward
                                FmToneButton(action.title, onPressed = if (!enabled) null else ({
                                    scope.launch {
                                        try {
                                            pending = offMain { store.prepare(action, root) }
                                        } catch (cancelled: CancellationException) {
                                            throw cancelled
                                        } catch (failure: Exception) {
                                            error = describeRemote(failure)
                                        }
                                    }
                                }))
                            }
                        }
                    }
                    if (notice.isNotEmpty()) item { FmInlineStatusLine(notice, isError = false, onDismiss = { notice = "" }) }
                    if (current.files.isNotEmpty()) {
                        item { FmSectionHeader("Changes", count = current.total) }
                        items(current.files, key = { it.path }) { change ->
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                val tint = when (change.kind) {
                                    GitChangeKind.Added, GitChangeKind.Untracked -> c.success
                                    GitChangeKind.Deleted, GitChangeKind.Conflicted -> c.danger
                                    else -> c.warning
                                }
                                Text(change.kind.letter, style = Fm.text.labelMedium.copy(color = tint, fontFamily = FmType.mono), modifier = Modifier.width(20.dp))
                                Column(Modifier.weight(1f)) {
                                    Text(change.path, style = Fm.text.bodySmall.copy(color = c.textPrimary))
                                    if (change.from.isNotEmpty()) Text("from ${change.from}", style = Fm.text.labelSmall.copy(color = c.textTertiary))
                                }
                                if (change.isStaged) Text("staged", style = Fm.text.labelSmall.copy(color = c.textTertiary))
                            }
                        }
                        if (current.truncated) item { Text("Only the first changes are shown.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                    }
                    store.outcomes.filter { it.subject == root }.take(5).forEach { outcome ->
                        item(key = outcome.id) { RemoteOutcomeCard(outcome) }
                    }
                } else if (error.isEmpty()) {
                    item { CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp) }
                }
                if (error.isNotEmpty()) item { Text(error, style = Fm.text.bodySmall.copy(color = c.danger)) }
            }
        }
        pending?.let { confirmation ->
            RemoteConfirmDialog(
                if (confirmation.action == GitRemoteAction.Pull.wire) GitRemoteAction.Pull.title else GitRemoteAction.Fetch.title,
                confirmation,
                perform = { conf, _ -> store.run(conf) },
                onDone = {
                    notice = it
                    scope.launch { load() }
                },
            ) { pending = null }
        }
    }
}
