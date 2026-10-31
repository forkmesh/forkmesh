package com.forkmesh.nativeapp.feature.account.notifications

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.IconButtonDefaults
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.NOTIFICATIONS_DELETED_TEXT
import com.forkmesh.nativeapp.account.NOTIFICATIONS_DELETE_ALL_LABEL
import com.forkmesh.nativeapp.account.NOTIFICATIONS_LOAD_ERROR_TITLE
import com.forkmesh.nativeapp.account.NOTIFICATIONS_REFRESH_LABEL
import com.forkmesh.nativeapp.account.NOTIFICATION_FILTERS
import com.forkmesh.nativeapp.account.PING_SETTINGS_MENU_LABEL
import com.forkmesh.nativeapp.account.NotificationsBody
import com.forkmesh.nativeapp.account.canDeleteAllNotifications
import com.forkmesh.nativeapp.account.canMarkAllRead
import com.forkmesh.nativeapp.account.notificationContextLabel
import com.forkmesh.nativeapp.account.notificationIconForKind
import com.forkmesh.nativeapp.account.notificationPills
import com.forkmesh.nativeapp.account.notificationPreview
import com.forkmesh.nativeapp.account.notificationTimeLabel
import com.forkmesh.nativeapp.account.notificationsBody
import com.forkmesh.nativeapp.account.notificationsDeleteErrorText
import com.forkmesh.nativeapp.account.notificationsUnreadLabel
import com.forkmesh.nativeapp.account.notificationsUpdatesLabel
import com.forkmesh.nativeapp.account.pingKindSettingsLabel
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmSwap
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.launch

class NotificationsScreen : AccountScreen() {
    private var filter by mutableStateOf("All")
    private var started = false
    private val listStatus = FmStatus()

    override val title: String get() = "Notifications"

    @Composable
    override fun Content(navigator: Navigator) {
        val service = Services.require<NotificationService>()
        val c = Fm.colors
        val items by service.notifications.collectAsState()
        val unread by service.unreadCount.collectAsState()
        val loading by service.loading.collectAsState()
        val error by service.error.collectAsState()
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                scope.launch { service.refresh() }
            }
        }
        val refresh: () -> Unit = { scope.launch { service.refresh() } }
        val open: (ForkNotification) -> Unit = { notification -> navigator.push(PingDetailScreen(notification, listStatus)) }
        val openSettings: (String?) -> Unit = { kind -> navigator.push(PingSettingsScreen(kind)) }
        val markAllRead: () -> Unit = {
            scope.launch {
                try {
                    service.markAllRead()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                }
            }
        }
        val deleteAll: () -> Unit = {
            listStatus.clear()
            scope.launch {
                try {
                    service.deleteAll()
                    listStatus.show(NOTIFICATIONS_DELETED_TEXT)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    listStatus.error(notificationsDeleteErrorText(failure))
                }
            }
        }
        val body = notificationsBody(loading, error, items, filter)
        val bodies = remember { HashMap<String, NotificationsBody>() }
        bodies[body.stateKey] = body

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            NotificationsHeader(
                unread = unread,
                canPop = navigator.depth > 1,
                onBack = { navigator.pop() },
                onRefresh = refresh,
                onMarkAllRead = if (canMarkAllRead(unread)) markAllRead else null,
                onDeleteAll = if (canDeleteAllNotifications(items.size)) deleteAll else null,
                onOpenSettings = { openSettings(null) },
            )
            LazyRow(
                Modifier
                    .fillMaxWidth()
                    .height(56.dp),
                contentPadding = PaddingValues(horizontal = FmSpace.x4),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                items(NOTIFICATION_FILTERS) { option ->
                    NotificationFilterChip(label = option, selected = option == filter, onSelected = { filter = option })
                }
            }
            FmInlineStatus(listStatus, Modifier.padding(horizontal = FmSpace.x4))
            FmSwap(
                stateKey = body.stateKey,
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) { key ->
                NotificationsBodyView(
                    body = bodies[key] ?: body,
                    onRefresh = { service.refresh() },
                    onRetry = refresh,
                    onOpen = open,
                    onOpenSettings = openSettings,
                )
            }
        }
    }
}

@Composable
private fun NotificationsHeader(
    unread: Int,
    canPop: Boolean,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
    onMarkAllRead: (() -> Unit)?,
    onDeleteAll: (() -> Unit)?,
    onOpenSettings: () -> Unit,
) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .height(72.dp)
            .padding(horizontal = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIconButton(
            icon = "arrow_back_ios_new_rounded",
            label = "Back",
            onPressed = if (canPop) onBack else null,
            colors = IconButtonDefaults.iconButtonColors(contentColor = c.textPrimary, disabledContentColor = c.textDisabled),
            busy = false,
        )
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
            Text(
                "Notifications",
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleLarge.copy(color = c.textPrimary, lineHeight = FmType.title * 1.1f),
            )
            VGap(FmSpace.x1)
            Text(
                notificationsUnreadLabel(unread),
                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textTertiary),
            )
        }
        if (onMarkAllRead != null) {
            FmTextButton(onClick = onMarkAllRead) { Text("Mark all read") }
        }
        FmIconButton(
            icon = "delete_sweep",
            label = NOTIFICATIONS_DELETE_ALL_LABEL,
            onPressed = onDeleteAll,
            modifier = Modifier.testTag("pings-delete-all"),
            colors = IconButtonDefaults.iconButtonColors(contentColor = c.danger, disabledContentColor = c.textDisabled),
            iconSize = 24.dp,
        )
        NotificationsMenu(onRefresh = onRefresh, onOpenSettings = onOpenSettings)
    }
}

@Composable
private fun NotificationsMenu(onRefresh: () -> Unit, onOpenSettings: () -> Unit) {
    val c = Fm.colors
    var expanded by remember { mutableStateOf(false) }
    Box {
        FmIconButton(
            icon = "more_vert",
            label = "More notification actions",
            onPressed = { expanded = true },
            color = c.textPrimary,
        )
        FmDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            FmDropdownMenuItem(
                text = PING_SETTINGS_MENU_LABEL,
                icon = "tune_rounded",
                modifier = Modifier.testTag("pings-open-settings"),
                onClick = {
                    expanded = false
                    onOpenSettings()
                },
            )
            FmDropdownMenuItem(
                text = NOTIFICATIONS_REFRESH_LABEL,
                onClick = {
                    expanded = false
                    onRefresh()
                },
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NotificationFilterChip(label: String, selected: Boolean, onSelected: () -> Unit) {
    val c = Fm.colors
    FilterChip(
        selected = selected,
        onClick = onSelected,
        label = {
            Text(
                label,
                style = TextStyle(fontFamily = FmType.ui, fontSize = FmType.small, fontWeight = if (selected) FmType.emphasis else FmType.regular),
            )
        },
        shape = RoundedCornerShape(FmRadius.sm),
        colors = FilterChipDefaults.filterChipColors(
            containerColor = c.bgRaised,
            labelColor = c.textSecondary,
            selectedContainerColor = c.accentSubtle,
            selectedLabelColor = c.textPrimary,
        ),
        border = FilterChipDefaults.filterChipBorder(
            enabled = true,
            selected = selected,
            borderColor = c.border,
            selectedBorderColor = c.accent.copy(alpha = 0.22f),
            borderWidth = 1.dp,
            selectedBorderWidth = 1.dp,
        ),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun NotificationsBodyView(
    body: NotificationsBody,
    onRefresh: suspend () -> Unit,
    onRetry: () -> Unit,
    onOpen: (ForkNotification) -> Unit,
    onOpenSettings: (String?) -> Unit,
) {
    when (body) {
        NotificationsBody.Loading -> FmSkeletonList(rows = 6, label = "Loading notifications")
        is NotificationsBody.Error -> NotificationsEmptyState(
            icon = "cloud_off_outlined",
            title = NOTIFICATIONS_LOAD_ERROR_TITLE,
            message = body.message,
        ) {
            FmTextButton(onClick = onRetry) {
                FmIcon("refresh", size = 18.dp)
                HGap(8.dp)
                Text("Retry")
            }
        }
        is NotificationsBody.Empty -> NotificationsEmptyState(icon = body.icon, title = body.title, message = body.message) {
            FmTextButton(onClick = { onOpenSettings(null) }) {
                FmIcon("tune_rounded", size = 18.dp)
                HGap(8.dp)
                Text(PING_SETTINGS_MENU_LABEL)
            }
        }
        is NotificationsBody.Items -> {
            val scope = rememberCoroutineScope()
            var refreshing by remember { mutableStateOf(false) }
            FmPullToRefreshBox(
                isRefreshing = refreshing,
                onRefresh = {
                    scope.launch {
                        refreshing = true
                        try {
                            onRefresh()
                        } finally {
                            refreshing = false
                        }
                    }
                },
                modifier = Modifier.fillMaxSize(),
            ) {
                LazyColumn(
                    Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(start = FmSpace.x4, top = FmSpace.x2, end = FmSpace.x4, bottom = FmSpace.x5),
                ) {
                    item {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .padding(bottom = FmSpace.x1),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                notificationsUpdatesLabel(body.items.size),
                                modifier = Modifier.weight(1f),
                                style = Fm.text.titleSmall.copy(color = Fm.colors.textPrimary),
                            )
                            FmTextButton(onClick = { onOpenSettings(null) }) {
                                FmIcon("tune_rounded", size = 18.dp)
                                HGap(FmSpace.x1)
                                Text("Settings")
                            }
                        }
                    }
                    items(body.items) { notification ->
                        NotificationRow(
                            notification,
                            onTap = { onOpen(notification) },
                            onOpenSettings = { onOpenSettings(notification.kind) },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun NotificationsEmptyState(
    icon: String,
    title: String,
    message: String,
    action: (@Composable () -> Unit)? = null,
) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(Modifier.padding(FmSpace.x5), horizontalAlignment = Alignment.CenterHorizontally) {
            FmEmptyState(icon = icon, title = title, modifier = Modifier.height(IntrinsicSize.Min), message = message)
            if (action != null) {
                VGap(FmSpace.x3)
                action()
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun NotificationRow(notification: ForkNotification, onTap: () -> Unit, onOpenSettings: () -> Unit) {
    val c = Fm.colors
    var menu by remember { mutableStateOf(false) }
    val time = notificationTimeLabel(notification.ts)
    val unread = notification.isUnread
    val accent = if (unread) c.accent else c.textTertiary
    val rule = c.border
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .combinedClickable(
                onClick = onTap,
                onLongClickLabel = pingKindSettingsLabel(notification.kindLabel),
                onLongClick = { menu = true },
            )
            .drawBehind {
                val stroke = 0.8.dp.toPx()
                val y = size.height - stroke / 2
                drawLine(rule, Offset(0f, y), Offset(size.width, y), stroke)
            }
            .padding(vertical = FmSpace.x3),
        verticalAlignment = Alignment.Top,
    ) {
        Box(
            Modifier
                .size(52.dp)
                .background(if (unread) c.accentSubtle else c.bgRaised, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(notificationIconForKind(notification.kind), size = 24.dp, tint = accent)
        }
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.Top) {
                Text(
                    notification.title,
                    modifier = Modifier.weight(1f),
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(
                        color = c.textPrimary,
                        fontSize = FmType.body,
                        fontWeight = if (unread) FmType.emphasis else FmType.regular,
                        lineHeight = FmType.body * 1.18f,
                    ),
                )
                HGap(FmSpace.x2)
                if (time.isNotEmpty()) {
                    Text(time, style = Fm.text.bodyMedium.copy(fontWeight = FmType.medium, color = c.textTertiary))
                }
            }
            VGap(FmSpace.x2)
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
            ) {
                notificationPills(notification).forEach { MiniPill(it) }
            }
            VGap(FmSpace.x2)
            Text(
                notificationPreview(notification),
                maxLines = 3,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(color = c.textSecondary, lineHeight = FmType.small * 1.45f),
            )
            notificationContextLabel(notification)?.let { label ->
                VGap(FmSpace.x2)
                Text(
                    label,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.accent),
                )
            }
            Box {
                FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    FmDropdownMenuItem(
                        text = pingKindSettingsLabel(notification.kindLabel),
                        icon = "tune_rounded",
                        onClick = {
                            menu = false
                            onOpenSettings()
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun MiniPill(label: String) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.sm)
    Text(
        label,
        modifier = Modifier
            .background(c.bgRaised, shape)
            .border(1.dp, c.border, shape)
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        style = Fm.text.bodySmall.copy(
            fontSize = FmType.micro,
            lineHeight = FmType.micro * FmType.leadingSnug,
            fontWeight = FmType.emphasis,
            color = c.textSecondary,
        ),
    )
}
