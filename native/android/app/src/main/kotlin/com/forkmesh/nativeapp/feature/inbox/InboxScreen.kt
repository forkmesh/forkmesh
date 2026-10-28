package com.forkmesh.nativeapp.feature.inbox

import com.forkmesh.nativeapp.ui.kit.LocalShellClearance
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3Api
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
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.notificationTimeLabel
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.navigation.RoutePage
import com.forkmesh.nativeapp.inbox.InboxItem
import com.forkmesh.nativeapp.inbox.InboxSyncState
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.emptyMessage
import com.forkmesh.nativeapp.navigation.label
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.account.projects.ProjectIcon
import com.forkmesh.nativeapp.inbox.InboxSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.ui.platform.testTag

class InboxScreen(private val pinnedFilter: InboxFilter?) : Screen() {
    override val extendsUnderTabBar: Boolean get() = true

    override val title: String get() = "Inbox"
    private var loading by mutableStateOf(false)
    private var unreadOnly by mutableStateOf(false)
    private val status = FmStatus()

    @Composable
    override fun Content(navigator: Navigator) {
        if (pinnedFilter != null) {
            RoutePage(pinnedFilter.label.let { if (pinnedFilter == InboxFilter.All) "Inbox" else it }, navigator) { Body(navigator) }
        } else {
            Body(navigator)
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun Body(navigator: Navigator) {
        val c = Fm.colors
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val tick by InboxFeature.changes.collectAsState()
        val actionProblem by InboxFeature.actionProblem.collectAsState()
        val shared by InboxFeature.filter.collectAsState()
        val filter = pinnedFilter ?: shared
        val model = InboxFeature.model
        fun reload() {
            if (loading) return
            scope.launch {
                loading = true
                try {
                    InboxFeature.refresh()
                } finally {
                    loading = false
                }
            }
        }
        LaunchedEffect(model) { reload() }
        tick.let { }
        if (model == null) {
            FmEmptyState("inbox", "Inbox", message = "Sign in to see what needs you.")
            return
        }
        model.filter = filter
        model.unreadOnly = unreadOnly
        val counts = model.unreadCounts
        val visible = model.visible

        fun open(item: InboxItem) {
            scope.launch {
                withContext(Dispatchers.IO) { model.markRead(item) }
                InboxFeature.publish()
            }
            val route = item.route
            val web = inboxWebLink(item)
            when {
                route != null -> NavigationFeature.open(route)
                web != null -> NavigationFeature.openUri(context, web)
            }
        }

        Column(Modifier.fillMaxSize().background(c.bgBase)) {
            InboxFilterBar(
                model,
                filter,
                unreadOnly,
                InboxBarActions(
                    onFilter = { if (pinnedFilter == null) InboxFeature.setFilter(it) },
                    onMarkAllRead = {
                        scope.launch {
                            withContext(Dispatchers.IO) { model.markAllRead() }
                            InboxFeature.publish()
                        }
                    },
                    onToggleUnread = { unreadOnly = !unreadOnly },
                    onPause = { choice ->
                        scope.launch {
                            withContext(Dispatchers.IO) { model.pause(choice) }?.let { status.error(it) }
                            InboxFeature.publish()
                        }
                    },
                    onDiscardFailed = {
                        model.syncQueue.discardFailed()
                        reload()
                    },
                    onSettings = { NavigationFeature.open(Route.PingSettings) },
                    onDeleteAll = {
                        scope.launch {
                            status.clear()
                            val failure = withContext(Dispatchers.IO) { model.deleteAllPings() }
                            if (failure != null) status.error(failure) else status.show("Pings deleted")
                            InboxFeature.publish()
                        }
                    },
                ),
            )
            if (model.isPaused) {
                Text(model.pauseLabel, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = c.warning))
            }
            model.errors.forEach {
                Text(it, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = c.danger))
            }
            FmInlineStatusLine(
                actionProblem,
                isError = true,
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                onDismiss = InboxFeature::clearActionProblem,
            )
            FmInlineStatus(status, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1))
            FmPullToRefreshBox(isRefreshing = loading && model.loaded, onRefresh = { reload() }, modifier = Modifier.weight(1f)) {
                if (visible.isEmpty() && model.loaded) {
                    FmEmptyState("inbox", filter.emptyMessage)
                } else {
                    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = FmSpace.x6 + LocalShellClearance.current)) {
                        items(visible, key = { it.id }) { item ->
                            SwipeRow(
                                item = item,
                                onShow = { navigator.push(InboxItemScreen(item)) },
                                onOpen = { open(item) },
                                onDelete = {
                                    scope.launch {
                                        withContext(Dispatchers.IO) { model.delete(item) }
                                        InboxFeature.publish()
                                    }
                                },
                                onPrompt = { NavigationFeature.compose(model.prompt(item)) },
                                onMarkRead = {
                                    scope.launch {
                                        withContext(Dispatchers.IO) { model.markRead(item) }
                                        InboxFeature.publish()
                                    }
                                },
                            )
                        }
                        if (model.pendingSyncCount > 0) {
                            item {
                                Text(
                                    "${model.pendingSyncCount} change${if (model.pendingSyncCount == 1) "" else "s"} waiting to sync",
                                    Modifier.padding(FmSpace.x4),
                                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
private fun SwipeRow(item: InboxItem, onShow: () -> Unit, onOpen: () -> Unit, onDelete: () -> Unit, onPrompt: () -> Unit, onMarkRead: () -> Unit) {
    val c = Fm.colors
    var menu by remember { mutableStateOf(false) }
    val activeProject = Services.get<ProjectsService>()?.active?.collectAsState()?.value
    val instance = if (item.source == InboxSource.Local) null else activeProject
    val state = rememberSwipeToDismissBoxState(confirmValueChange = { value ->
        if (value == SwipeToDismissBoxValue.EndToStart) {
            FmSoundCues.play(SoundCue.DELETE)
            onDelete()
            true
        } else {
            false
        }
    })
    SwipeToDismissBox(
        state = state,
        enableDismissFromStartToEnd = false,
        backgroundContent = {
            Box(Modifier.fillMaxSize().background(c.danger).padding(horizontal = FmSpace.x4), contentAlignment = Alignment.CenterEnd) {
                FmIcon("delete_outline", size = 22.dp, tint = c.bgBase)
            }
        },
    ) {
        Box {
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(c.bgBase)
                    .background(if (item.unread) c.accentSubtle else Color.Transparent)
                    .heightIn(min = 56.dp)
                    .combinedClickable(onClick = onShow, onLongClick = { menu = true })
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3)
                    .semantics(mergeDescendants = true) {
                        contentDescription = buildString {
                            append(if (item.unread) "Unread. " else "Read. ")
                            append("${item.kindLabel}. ${item.title}")
                            if (item.repo.isNotEmpty()) append(", ${item.repo}")
                            if (instance != null) append(". From ${instance.label}")
                        }
                    },
                verticalAlignment = Alignment.Top,
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    FmIcon(item.icon, size = 22.dp, tint = if (item.unread) c.accent else c.textTertiary)
                    if (instance != null) {
                        Box(Modifier.testTag("inbox-row-instance-logo")) { ProjectIcon(instance, size = 16.dp) }
                    }
                }
                Spacer(Modifier.width(FmSpace.x3))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            listOf(item.kindLabel, item.repo).filter { it.isNotEmpty() }.joinToString(" · "),
                            Modifier.weight(1f),
                            style = Fm.text.labelSmall.copy(color = c.textTertiary),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Text(notificationTimeLabel(item.ts), style = Fm.text.labelSmall.copy(color = if (item.unread) c.accent else c.textTertiary))
                    }
                    Text(
                        item.title,
                        style = Fm.text.bodyLarge.copy(
                            color = if (item.unread) c.textPrimary else c.textSecondary,
                            fontWeight = if (item.unread) FontWeight.SemiBold else FontWeight.Normal,
                        ),
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    if (item.body.isNotEmpty()) {
                        Text(
                            item.body,
                            style = Fm.text.bodySmall.copy(color = if (item.unread) c.textSecondary else c.textTertiary),
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    if (item.sync == InboxSyncState.Pending || item.sync == InboxSyncState.Failed) {
                        Text(
                            if (item.sync == InboxSyncState.Pending) "Waiting to sync" else "Could not sync",
                            style = Fm.text.labelSmall.copy(color = if (item.sync == InboxSyncState.Failed) c.danger else c.textTertiary),
                        )
                    }
                }
                if (item.unread) {
                    Spacer(Modifier.width(FmSpace.x2))
                    Box(Modifier.padding(top = 6.dp).size(10.dp).background(c.accent, CircleShape))
                }
            }
            FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                FmDropdownMenuItem(text = "Open", onClick = { menu = false; onOpen() })
                FmDropdownMenuItem(text = "Send to agent", onClick = { menu = false; onPrompt() })
                if (item.unread) FmDropdownMenuItem(text = "Mark read", onClick = { menu = false; onMarkRead() })
                FmDropdownMenuItem(text = "Delete", onClick = { menu = false; onDelete() })
            }
        }
    }
}
