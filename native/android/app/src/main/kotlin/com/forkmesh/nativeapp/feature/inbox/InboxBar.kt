package com.forkmesh.nativeapp.feature.inbox

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.inbox.InboxModel
import com.forkmesh.nativeapp.inbox.PingPauseChoice
import com.forkmesh.nativeapp.inbox.PingSyncQueue
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.label
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

internal val InboxFilter.icon: String
    get() = when (this) {
        InboxFilter.All -> "inbox"
        InboxFilter.Pings -> "notifications_none_rounded"
        InboxFilter.Reviews -> "call_merge"
        InboxFilter.Assigned -> "assignment_ind_outlined"
        InboxFilter.Mentions -> "alternate_email"
        InboxFilter.Approvals -> "verified_user"
    }

internal val InboxFilter.menuLabel: String
    get() = if (this == InboxFilter.All) "All activity" else label

internal object InboxMenuOrder {
    val specific = listOf(InboxFilter.Approvals, InboxFilter.Reviews, InboxFilter.Assigned, InboxFilter.Mentions, InboxFilter.Pings)
}

internal fun inboxSubtitle(filter: InboxFilter, unread: Int): String =
    if (unread > 0) "${filter.menuLabel} · $unread unread" else filter.menuLabel

internal class InboxBarActions(
    val onFilter: (InboxFilter) -> Unit,
    val onMarkAllRead: () -> Unit,
    val onToggleUnread: () -> Unit,
    val onPause: (PingPauseChoice?) -> Unit,
    val onDiscardFailed: () -> Unit,
    val onSettings: () -> Unit,
    val onDeleteAll: () -> Unit,
)

@Composable
internal fun InboxTitleBar(model: InboxModel, filter: InboxFilter, unreadOnly: Boolean, pinned: Boolean, actions: InboxBarActions) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val counts = model.unreadCounts
    var filterMenu by remember { mutableStateOf(false) }
    val subtitle = inboxSubtitle(filter, counts[filter] ?: 0)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = FmSpace.x4, end = FmSpace.x3, top = FmSpace.x1, bottom = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.weight(1f)) {
            if (pinned) {
                Text(subtitle, style = Fm.text.bodyLarge.copy(color = c.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            } else {
                Column(
                    Modifier
                        .clip(RoundedCornerShape(FmSpace.x2))
                        .clickable {
                            haptics.tap()
                            filterMenu = true
                        }
                        .semantics(mergeDescendants = true) {
                            role = Role.Button
                            contentDescription = "Inbox, showing ${filter.menuLabel}"
                            stateDescription = subtitle
                        },
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Inbox", Modifier.semantics { heading() }, style = Fm.text.displaySmall)
                        Spacer(Modifier.width(FmSpace.x1))
                        FmIcon("expand_more", size = 24.dp, tint = c.textSecondary)
                    }
                    Text(subtitle, style = Fm.text.bodyMedium.copy(color = c.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                FmDropdownMenu(expanded = filterMenu, onDismissRequest = { filterMenu = false }) {
                    InboxFilterMenuItem(InboxFilter.All, filter, counts[InboxFilter.All] ?: 0) {
                        filterMenu = false
                        haptics.selection()
                        actions.onFilter(it)
                    }
                    HorizontalDivider(Modifier.padding(vertical = FmSpace.x1), thickness = 0.5.dp, color = c.border)
                    InboxMenuOrder.specific.forEach { option ->
                        InboxFilterMenuItem(option, filter, counts[option] ?: 0) {
                            filterMenu = false
                            haptics.selection()
                            actions.onFilter(it)
                        }
                    }
                }
            }
        }
        Spacer(Modifier.width(FmSpace.x2))
        InboxMoreMenu(model, unreadOnly, actions)
    }
}

@Composable
private fun InboxFilterMenuItem(option: InboxFilter, current: InboxFilter, count: Int, onPick: (InboxFilter) -> Unit) {
    val c = Fm.colors
    val selected = option == current
    FmDropdownMenuItem(
        text = option.menuLabel,
        icon = option.icon,
        modifier = Modifier.semantics { if (selected) stateDescription = "Selected" },
        onClick = { onPick(option) },
        trailing = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (count > 0) {
                    Text(
                        if (count > 99) "99+" else "$count",
                        style = Fm.text.bodyLarge.copy(color = c.textTertiary, fontFeatureSettings = FmType.tabular),
                    )
                }
                Box(Modifier.padding(start = FmSpace.x2).size(20.dp), contentAlignment = Alignment.Center) {
                    if (selected) FmIcon("check", size = 20.dp, tint = c.accent)
                }
            }
        },
    )
}

@Composable
private fun InboxMoreMenu(model: InboxModel, unreadOnly: Boolean, actions: InboxBarActions) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    var pauseOpen by remember { mutableStateOf(false) }
    Box {
        FmTooltip("More inbox actions") {
            Box(
                Modifier
                    .size(40.dp)
                    .fmGlass(CircleShape, shadow = false, interactive = true)
                    .clickable { open = true }
                    .semantics {
                        role = Role.Button
                        contentDescription = "More inbox actions"
                    },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("more_horiz", size = 22.dp, tint = c.textPrimary)
                if (model.isPaused) {
                    Box(Modifier.align(Alignment.TopEnd).padding(7.dp).size(7.dp).clip(CircleShape).background(c.warning))
                }
            }
        }
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            FmDropdownMenuItem("Mark all read", icon = "drafts", onClick = {
                open = false
                actions.onMarkAllRead()
            })
            FmDropdownMenuItem("Unread only", icon = "filter_list", onClick = {
                open = false
                actions.onToggleUnread()
            }, trailing = if (unreadOnly) ({ FmIcon("check", size = 20.dp, tint = c.accent) }) else null)
            FmDropdownMenuItem(
                if (model.isPaused) "Pings paused" else "Pause pings",
                icon = if (model.isPaused) "notifications_off_outlined" else "bedtime",
                textColor = if (model.isPaused) c.warning else null,
                onClick = {
                    open = false
                    pauseOpen = true
                },
            )
            if (model.syncQueue.operations.any { it.attempts >= PingSyncQueue.MAX_ATTEMPTS }) {
                FmDropdownMenuItem("Discard changes that could not sync", icon = "error_outline", textColor = c.danger, onClick = {
                    open = false
                    actions.onDiscardFailed()
                })
            }
            FmDropdownMenuItem("Ping settings", icon = "settings", onClick = {
                open = false
                actions.onSettings()
            })
            FmDropdownMenuItem(
                "Delete all pings",
                icon = "delete_outline",
                textColor = c.danger,
                enabled = model.canDeleteAllPings,
                modifier = Modifier.testTag("inbox-delete-all"),
                onClick = {
                    open = false
                    actions.onDeleteAll()
                },
            )
        }
        FmDropdownMenu(expanded = pauseOpen, onDismissRequest = { pauseOpen = false }) {
            PingPauseChoice.entries.forEach { choice ->
                FmDropdownMenuItem(choice.label, onClick = {
                    pauseOpen = false
                    actions.onPause(choice)
                })
            }
            if (model.isPaused) FmDropdownMenuItem("Resume pings", onClick = {
                pauseOpen = false
                actions.onPause(null)
            })
        }
    }
}
