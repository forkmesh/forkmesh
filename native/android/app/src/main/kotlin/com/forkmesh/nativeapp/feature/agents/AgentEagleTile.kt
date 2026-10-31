package com.forkmesh.nativeapp.feature.agents

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathMeasure
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.agents.AgentChipFacts
import com.forkmesh.nativeapp.agents.AgentLineBadge
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.AgentLineStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.PI
import kotlin.math.cos

private object EagleTile {
    val SIDE = 32.dp
    val RING = 2.dp
    val RADIUS = 8.dp
    val FILES_HEIGHT = 9.dp
    const val TASKS_SHOWN = 10
    const val TASKS_KEPT = 7
    const val BLINK_MS = 1400
    val PILL = Color.Black.copy(alpha = 0.45f)
    val MERGED = Color(0xFFA371F7)
    val BEHIND = Color(0xFFE3742F)
}

@Composable
internal fun AgentEagleTile(facts: AgentChipFacts, unread: Boolean) {
    val c = Fm.colors
    Box(Modifier.size(EagleTile.SIDE)) {
        Box(Modifier.fillMaxSize().background(c.bgOverlay, RoundedCornerShape(EagleTile.RADIUS)))
        AgentTokenRing(facts, EagleTile.RADIUS, Modifier.fillMaxSize())
        Column(
            Modifier.align(Alignment.Center).padding(EagleTile.RING + 1.dp),
            verticalArrangement = Arrangement.spacedBy(1.5.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (facts.filesChanged >= 0) AgentTileFiles(facts)
            AgentTileBadges(facts)
            if (facts.status == AgentLineStatus.Success && !facts.active) {
                AgentTileDone()
            } else if (facts.backgroundTasks > 0) {
                AgentTileTasks(facts.backgroundTasks, facts.active)
            }
        }
        AgentTileStatusDot(facts.status, Modifier.align(Alignment.TopEnd).offset(x = 2.dp, y = (-2).dp))
        if (unread) {
            Box(
                Modifier
                    .align(Alignment.TopStart)
                    .offset(x = (-2).dp, y = (-2).dp)
                    .size(8.dp)
                    .background(c.accent, CircleShape),
            )
        }
    }
}

@Composable
private fun AgentTileStatusDot(status: AgentLineStatus, modifier: Modifier) {
    val c = Fm.colors
    val tint = when (status) {
        AgentLineStatus.Running -> c.accent
        AgentLineStatus.Waiting -> c.warning
        AgentLineStatus.Success -> c.success
        AgentLineStatus.Merged -> EagleTile.MERGED
        AgentLineStatus.Failed -> c.danger
        AgentLineStatus.Queued, AgentLineStatus.Stopped, AgentLineStatus.Other -> c.textTertiary
    }
    Box(
        modifier
            .size(11.dp)
            .background(c.bgOverlay, CircleShape)
            .padding(1.5.dp)
            .background(tint, CircleShape),
    )
}

@Composable
private fun AgentTileFiles(facts: AgentChipFacts) {
    val share = AgentLineChips.churnShare(facts.linesAdded, facts.linesRemoved)
    val added = Fm.colors.success
    val removed = Fm.colors.danger
    val hairline = Fm.colors.border
    Box(
        Modifier
            .height(EagleTile.FILES_HEIGHT)
            .background(EagleTile.PILL, CircleShape)
            .drawWithContent {
                drawContent()
                drawChurnOutline(share, added, removed, hairline)
            }
            .padding(horizontal = 3.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            AgentLineChips.compactCount(facts.filesChanged),
            maxLines = 1,
            softWrap = false,
            style = TextStyle(fontSize = 7.sp, lineHeight = 8.sp, fontWeight = FontWeight.Bold, color = Color.White, fontFeatureSettings = "tnum"),
        )
    }
}

private fun DrawScope.drawChurnOutline(share: Float?, added: Color, removed: Color, hairline: Color) {
    if (share == null) return
    val width = 0.75.dp.toPx()
    val inset = width / 2
    val radius = size.height / 2 - inset
    val path = Path().apply { addRoundRect(RoundRect(inset, inset, size.width - inset, size.height - inset, CornerRadius(radius))) }
    val measure = PathMeasure().apply { setPath(path, false) }
    val length = measure.length
    if (length <= 0f) {
        drawPath(path, hairline, style = Stroke(width))
        return
    }
    val split = length * share
    if (split > 0f) {
        val green = Path()
        measure.getSegment(0f, split, green, true)
        drawPath(green, added, style = Stroke(width))
    }
    if (split < length) {
        val red = Path()
        measure.getSegment(split, length, red, true)
        drawPath(red, removed, style = Stroke(width))
    }
}

@Composable
private fun AgentTileBadges(facts: AgentChipFacts) {
    val c = Fm.colors
    val badges = AgentLineBadge.of(facts).take(AgentLineBadge.SHOWN)
    if (badges.isEmpty()) return
    Row(
        Modifier
            .height(7.dp)
            .background(EagleTile.PILL, CircleShape)
            .padding(horizontal = 1.5.dp),
        horizontalArrangement = Arrangement.spacedBy(1.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (badge in badges) {
            val tint = when (badge) {
                AgentLineBadge.Conflicted, AgentLineBadge.Behind -> EagleTile.BEHIND
                AgentLineBadge.Worktree, AgentLineBadge.Runner, AgentLineBadge.Cloud -> c.accent
                AgentLineBadge.Branch, AgentLineBadge.Ahead, AgentLineBadge.Pull, AgentLineBadge.Outside -> Color.White
            }
            FmIcon(badgeIcon(badge), size = 5.dp, tint = tint)
        }
    }
}

private fun badgeIcon(badge: AgentLineBadge): String = when (badge) {
    AgentLineBadge.Conflicted -> "warning"
    AgentLineBadge.Behind -> "arrow_downward"
    AgentLineBadge.Worktree -> "folder"
    AgentLineBadge.Branch -> "call_split"
    AgentLineBadge.Ahead -> "radio_button_checked"
    AgentLineBadge.Pull -> "call_merge"
    AgentLineBadge.Runner -> "memory"
    AgentLineBadge.Cloud -> "cloud"
    AgentLineBadge.Outside -> "terminal"
}

@Composable
private fun AgentTileDone() {
    Box(
        Modifier
            .height(7.dp)
            .background(Fm.colors.success, CircleShape)
            .padding(horizontal = 3.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            "done",
            maxLines = 1,
            softWrap = false,
            style = TextStyle(fontSize = 5.5.sp, lineHeight = 6.sp, fontWeight = FontWeight.ExtraBold, color = Color.White),
        )
    }
}

@Composable
private fun AgentTileTasks(count: Int, active: Boolean) {
    val blinking = active && !Fm.reducedMotion
    val glow = if (blinking) {
        val phase by rememberInfiniteTransition(label = "eagle-tasks").animateFloat(
            initialValue = 0f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(EagleTile.BLINK_MS, easing = LinearEasing), RepeatMode.Restart),
            label = "eagle-tasks-phase",
        )
        0.6f + 0.4f * cos(phase * 2 * PI.toFloat())
    } else {
        1f
    }
    val drawn = if (count > EagleTile.TASKS_SHOWN) EagleTile.TASKS_KEPT else count
    Row(
        Modifier
            .height(6.dp)
            .graphicsLayer { alpha = glow }
            .background(EagleTile.PILL, CircleShape)
            .padding(horizontal = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        repeat(drawn) {
            Box(Modifier.width(1.dp).height(4.dp).background(Color.White, CircleShape))
        }
        if (count > drawn) {
            Text(
                "+${count - drawn}",
                maxLines = 1,
                softWrap = false,
                style = TextStyle(fontSize = 5.5.sp, lineHeight = 6.sp, fontWeight = FontWeight.Bold, color = Color.White, fontFeatureSettings = "tnum"),
            )
        }
    }
}
