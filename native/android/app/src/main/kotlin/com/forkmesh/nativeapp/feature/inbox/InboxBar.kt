package com.forkmesh.nativeapp.feature.inbox

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.inbox.InboxModel
import com.forkmesh.nativeapp.inbox.PingPauseChoice
import com.forkmesh.nativeapp.inbox.PingSyncQueue
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.label
import com.forkmesh.nativeapp.settings.AccentChoice
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmTooltip
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal object InboxSquare {
    val side = 28.dp
    val spacing = 4.dp
    val shape = RoundedCornerShape(8.dp)
}

internal val InboxFilter.icon: String
    get() = when (this) {
        InboxFilter.All -> "inbox"
        InboxFilter.Pings -> "notifications_none_rounded"
        InboxFilter.Reviews -> "call_merge"
        InboxFilter.Assigned -> "assignment_ind_outlined"
        InboxFilter.Mentions -> "alternate_email"
        InboxFilter.Approvals -> "verified_user"
    }

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
internal fun InboxFilterBar(model: InboxModel, filter: InboxFilter, unreadOnly: Boolean, actions: InboxBarActions) {
    val c = Fm.colors
    val counts = model.unreadCounts
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Row(
            Modifier
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1 + 2.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(InboxSquare.spacing)) {
                InboxFilter.entries.forEach { option ->
                    val selected = option == filter
                    val count = counts[option] ?: 0
                    Box {
                        InboxSquareButton(
                            option.icon,
                            option.label,
                            color = if (selected) c.accent else c.textPrimary,
                            tinted = selected,
                            selected = selected,
                            value = if (count > 0) "$count unread" else null,
                            onClick = { actions.onFilter(option) },
                        )
                        if (count > 0) InboxCountBadge(count, Modifier.align(Alignment.TopEnd).offset(x = 5.dp, y = (-5).dp))
                    }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(InboxSquare.spacing)) {
                InboxSquareButton("drafts", "Mark all read", color = c.textPrimary, onClick = actions.onMarkAllRead)
                InboxSquareButton(
                    "filter_list",
                    "Unread only",
                    color = if (unreadOnly) c.accent else c.textPrimary,
                    tinted = unreadOnly,
                    selected = unreadOnly,
                    onClick = actions.onToggleUnread,
                )
                InboxPauseMenu(model, actions.onPause)
                if (model.syncQueue.operations.any { it.attempts >= PingSyncQueue.MAX_ATTEMPTS }) {
                    InboxSquareButton("error_outline", "Discard changes that could not sync", color = c.danger, onClick = actions.onDiscardFailed)
                }
                InboxSquareButton("settings", "Ping settings", color = c.textPrimary, onClick = actions.onSettings)
                InboxSquareButton(
                    "delete_outline",
                    "Delete all pings",
                    color = if (model.canDeleteAllPings) c.danger else c.textDisabled,
                    enabled = model.canDeleteAllPings,
                    modifier = Modifier.testTag("inbox-delete-all"),
                    onClick = actions.onDeleteAll,
                )
            }
        }
    }
}

@Composable
private fun InboxPauseMenu(model: InboxModel, onPause: (PingPauseChoice?) -> Unit) {
    val c = Fm.colors
    var open by remember { mutableStateOf(false) }
    Box {
        InboxSquareButton(
            if (model.isPaused) "notifications_off_outlined" else "bedtime",
            if (model.isPaused) "Pings paused" else "Pause pings",
            color = if (model.isPaused) c.warning else c.textPrimary,
            onClick = { open = true },
        )
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            PingPauseChoice.entries.forEach { choice ->
                FmDropdownMenuItem(choice.label, onClick = { open = false; onPause(choice) })
            }
            if (model.isPaused) FmDropdownMenuItem("Resume pings", onClick = { open = false; onPause(null) })
        }
    }
}

@Composable
private fun InboxSquareButton(
    icon: String,
    label: String,
    color: Color,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    tinted: Boolean = false,
    selected: Boolean = false,
    enabled: Boolean = true,
    value: String? = null,
) {
    val c = Fm.colors
    FmTooltip(label) {
        Box(
            modifier
                .size(InboxSquare.side)
                .fmGlass(InboxSquare.shape, tint = if (tinted) c.accentSubtle else null, shadow = false)
                .clickable(enabled = enabled) {
                    FmSoundCues.play(SoundCue.CLICK)
                    onClick()
                }
                .semantics {
                    role = Role.Button
                    contentDescription = label
                    this.selected = selected
                    if (value != null) stateDescription = value
                },
            contentAlignment = Alignment.Center,
        ) { FmIcon(icon, size = 16.dp, tint = color) }
    }
}

@Composable
private fun InboxCountBadge(count: Int, modifier: Modifier) {
    val c = Fm.colors
    val blue = Color(if (c.dark) AccentChoice.blue.onDark else AccentChoice.blue.light)
    Box(
        modifier
            .defaultMinSize(minWidth = 14.dp, minHeight = 14.dp)
            .background(blue, RoundedCornerShape(50))
            .padding(horizontal = 3.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            if (count > 99) "99+" else "$count",
            style = Fm.text.labelSmall.copy(fontSize = 9.sp, lineHeight = 10.sp, fontWeight = FontWeight.Bold, color = Color.White),
        )
    }
}
