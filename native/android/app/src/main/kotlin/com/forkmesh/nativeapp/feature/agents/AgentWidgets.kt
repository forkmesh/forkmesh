package com.forkmesh.nativeapp.feature.agents

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentChipFacts
import com.forkmesh.nativeapp.agents.AgentChipSlot
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.AgentLineStatus
import com.forkmesh.nativeapp.agents.AgentListView
import com.forkmesh.nativeapp.agents.AgentReadState
import com.forkmesh.nativeapp.agents.AgentTone
import com.forkmesh.nativeapp.agents.AgentsList
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

object AgentBranchDiff {
    @Volatile
    var screen: ((repo: Repository, head: String, base: String, title: String) -> Screen)? = null

    fun open(navigator: Navigator, repo: Repository, head: String, base: String, title: String): Boolean {
        val factory = screen ?: return false
        navigator.push(factory(repo, head, base, title))
        return true
    }
}

@Composable
internal fun toneColor(tone: AgentTone): Color {
    val c = Fm.colors
    return when (tone) {
        AgentTone.Accent -> c.accent
        AgentTone.Success -> c.success
        AgentTone.Danger -> c.danger
        AgentTone.Muted -> c.textTertiary
        AgentTone.Secondary -> c.textSecondary
    }
}

@Composable
internal fun AgentChip(label: String) {
    FmStatusBadge(label = label, color = toneColor(AgentsList.badgeTone(label)))
}

@Composable
internal fun AgentStatusBadge(session: AgentSession) {
    FmStatusBadge(label = session.statusLabel, color = toneColor(AgentsList.statusTone(session)))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun ChipRow(content: @Composable () -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        content()
    }
}

@Composable
internal fun BranchChip(branch: String, onTap: (() -> Unit)?) {
    val c = Fm.colors
    Row(
        Modifier
            .clip(RoundedCornerShape(FmRadius.full))
            .background(c.accent.copy(alpha = 31f / 255f))
            .then(if (onTap != null) Modifier.clickable(onClick = onTap) else Modifier)
            .semantics {
                contentDescription = "View changes on $branch"
                role = Role.Button
            }
            .padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("account_tree_outlined", size = 12.dp, tint = c.accent)
        Spacer(Modifier.width(FmSpace.x1))
        Text(branch, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall.copy(color = c.accent))
        if (onTap != null) FmIcon("chevron_right", size = 12.dp, tint = c.accent)
    }
}

internal val LocalAgentListView = staticCompositionLocalOf { AgentListView.Compact }

@Composable
internal fun AgentTwoLineRow(
    icon: String,
    title: String,
    detail: String,
    status: @Composable () -> Unit,
    onTap: () -> Unit,
    trailing: (@Composable () -> Unit)? = null,
    facts: AgentChipFacts? = null,
    unread: Boolean? = null,
) {
    val c = Fm.colors
    val view = if (facts == null) AgentListView.Compact else LocalAgentListView.current
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .background(c.bgOverlay)
            .clickable(onClick = onTap)
            .semantics { if (unread == true) stateDescription = AgentReadState.UNREAD_LABEL }
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (unread != null) {
            AgentUnreadDot(unread)
            Spacer(Modifier.width(FmSpace.x2))
        }
        if (view == AgentListView.Compact) {
            FmIcon(icon, size = 20.dp, tint = c.accent)
            Spacer(Modifier.width(FmSpace.x3))
        }
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    title,
                    Modifier.weight(1f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                )
                if (view == AgentListView.Compact) {
                    Spacer(Modifier.width(FmSpace.x2))
                    status()
                }
            }
            if (view == AgentListView.Full && facts != null) {
                Spacer(Modifier.height(FmSpace.x1))
                AgentChipColumns(facts)
            } else if (view == AgentListView.Compact && detail.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x1))
                Text(detail, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(FmSpace.x1))
            trailing()
        }
    }
}

@Composable
internal fun AgentUnreadDot(unread: Boolean) {
    Box(Modifier.size(8.dp)) {
        if (unread) Box(Modifier.size(8.dp).background(Fm.colors.accent, CircleShape))
    }
}

@Composable
internal fun AgentChipColumns(facts: AgentChipFacts) {
    val c = Fm.colors
    val now = System.currentTimeMillis()
    val shown = AgentLineChips.chips(facts, now).associateBy { it.slot }
    val style = Fm.text.labelSmall.copy(color = c.textSecondary)
    FitRow(gap = 7.dp) {
        for (slot in AgentChipSlot.entries) {
            val chip = shown[slot]
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(11.dp), contentAlignment = Alignment.Center) {
                    if (chip != null) AgentChipGlyph(slot, facts)
                }
                Spacer(Modifier.width(2.dp))
                Box {
                    for (word in slot.widestWords) {
                        Text(word, Modifier.alpha(0f), maxLines = 1, style = style)
                    }
                    if (chip != null) Text(chip.word, maxLines = 1, style = style)
                }
            }
        }
    }
}

@Composable
private fun FitRow(gap: Dp, content: @Composable () -> Unit) {
    Layout(content) { measurables, constraints ->
        val gapPx = gap.roundToPx()
        val placeables = measurables.map { it.measure(Constraints()) }
        val natural = placeables.sumOf { it.width } + gapPx * (placeables.size - 1).coerceAtLeast(0)
        val width = if (constraints.hasBoundedWidth) constraints.maxWidth else natural
        val height = placeables.maxOfOrNull { it.height } ?: 0
        layout(width, height) {
            var x = 0
            for (placeable in placeables) {
                if (x + placeable.width > width) break
                placeable.placeRelative(x, (height - placeable.height) / 2)
                x += placeable.width + gapPx
            }
        }
    }
}

@Composable
private fun AgentChipGlyph(slot: AgentChipSlot, facts: AgentChipFacts) {
    val c = Fm.colors
    val amber = Color(0xFFE3742F)
    when (slot) {
        AgentChipSlot.Status -> AgentStatusGlyph(facts.status)
        AgentChipSlot.Provider -> FmIcon("terminal_outlined", size = 10.dp, tint = c.textSecondary)
        AgentChipSlot.Host -> FmIcon(
            when {
                facts.runner -> "memory_outlined"
                facts.cloud -> "cloud_outlined"
                facts.outside -> "terminal_outlined"
                else -> "desktop_windows_outlined"
            },
            size = 10.dp,
            tint = if (facts.cloud || facts.runner) c.accent else c.textSecondary,
        )
        AgentChipSlot.Age -> FmIcon("history", size = 10.dp, tint = c.textSecondary)
        AgentChipSlot.Sync -> FmIcon(if (facts.conflicted) "warning_amber_rounded" else "south", size = 10.dp, tint = amber)
        AgentChipSlot.Files -> FmIcon("description_outlined", size = 10.dp, tint = Color(0xFF58A6FF))
        AgentChipSlot.Churn -> FmIcon("difference_outlined", size = 10.dp, tint = c.textSecondary)
        AgentChipSlot.Branch -> FmIcon(if (facts.hasWorktree) "folder_rounded" else "call_split", size = 10.dp, tint = c.success)
    }
}

internal val AgentFlamePurple = Color(0xFFBC8CFF)

@Composable
internal fun AgentStatusGlyph(status: AgentLineStatus, size: Dp = 11.dp) {
    val c = Fm.colors
    if (status == AgentLineStatus.Running) {
        val pulse by rememberInfiniteTransition(label = "agent-flame").animateFloat(
            initialValue = 1f,
            targetValue = 0.45f,
            animationSpec = infiniteRepeatable(tween(700), RepeatMode.Reverse),
            label = "agent-flame-alpha",
        )
        FmIcon("local_fire_department_rounded", Modifier.alpha(pulse), size = size, tint = AgentFlamePurple)
        return
    }
    val (name, tint) = when (status) {
        AgentLineStatus.Queued -> "schedule" to Color(0xFFD29922)
        AgentLineStatus.Waiting -> "pan_tool_rounded" to Color(0xFFE3742F)
        AgentLineStatus.Success -> "check_circle_rounded" to c.success
        AgentLineStatus.Failed -> "cancel_rounded" to c.danger
        AgentLineStatus.Stopped -> "stop_circle_rounded" to c.danger
        AgentLineStatus.Merged -> "merge_type" to Color(0xFFA371F7)
        else -> "circle" to c.textTertiary
    }
    FmIcon(name, size = size, tint = tint)
}

@Composable
internal fun SealedSessionsNotice(count: Int) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgOverlay, shape)
            .border(1.dp, c.border, shape)
            .padding(FmSpace.x3),
    ) {
        FmIcon("lock_outline", size = 18.dp, tint = c.textTertiary)
        Spacer(Modifier.width(FmSpace.x2))
        Column {
            Text(AgentsList.sealedLabel(count), style = Fm.text.labelMedium.copy(color = c.textPrimary))
            Spacer(Modifier.height(FmSpace.x1))
            Text(
                "Agent transcripts are readable only on the owner devices they were sealed to. Sign in on this device " +
                    "and let it enroll, then the desktop seals new sessions to it too.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
        }
    }
}

@Composable
internal fun NoteLine(text: String, color: Color, icon: String) {
    Row(verticalAlignment = Alignment.Top) {
        FmIcon(icon, size = 16.dp, tint = color)
        Spacer(Modifier.width(FmSpace.x2))
        Text(text, style = Fm.text.bodySmall.copy(color = color))
    }
}
