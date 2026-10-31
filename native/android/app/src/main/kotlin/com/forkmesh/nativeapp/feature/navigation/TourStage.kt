package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.navigation.TourScene
import com.forkmesh.nativeapp.shell.ShellBarCell
import com.forkmesh.nativeapp.shell.ShellBarRows
import com.forkmesh.nativeapp.shell.ShellBarSlot
import com.forkmesh.nativeapp.shell.ShellBarStyle
import com.forkmesh.nativeapp.ui.kit.AvatarWithDot
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay
import kotlin.math.cos
import kotlin.math.sin

internal object TourBeat {
    const val BEAT_MS = 1900L
    const val QUIET_MS = 6000L
    val height = 330.dp
}

@Composable
internal fun TourStage(scene: TourScene, title: String, active: Boolean, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    var phase by remember { mutableIntStateOf(0) }
    var quietUntil by remember { mutableLongStateOf(0L) }
    LaunchedEffect(active) {
        if (!active) {
            phase = 0
            return@LaunchedEffect
        }
        while (true) {
            delay(TourBeat.BEAT_MS)
            if (System.currentTimeMillis() >= quietUntil) phase += 1
        }
    }
    val act: (Int) -> Unit = { next ->
        haptics.tap()
        quietUntil = System.currentTimeMillis() + TourBeat.QUIET_MS
        phase = next
    }
    val shape = RoundedCornerShape(30.dp)
    val shadow = Color.Black.copy(alpha = if (c.dark) 0.5f else 0.12f)
    Box(
        modifier
            .padding(horizontal = 8.dp)
            .fillMaxWidth()
            .height(TourBeat.height)
            .shadow(18.dp, shape, ambientColor = shadow, spotColor = shadow)
            .clip(shape)
            .background(c.bgBase)
            .border(1.dp, c.border, shape)
            .semantics { contentDescription = "Preview: $title" },
    ) {
        when (scene) {
            TourScene.HOME -> TourHomeScene(phase, act)
            TourScene.AGENTS -> TourAgentsScene(phase, act)
            TourScene.NEW_AGENT -> TourNewAgentScene(phase, act)
            TourScene.INBOX -> TourInboxScene(phase, act)
            TourScene.HUB -> TourHubScene(phase, act)
            TourScene.VOICE -> TourVoiceScene(phase, act)
            TourScene.CODE -> TourCodeScene(phase, act)
            TourScene.PALETTE -> TourPaletteScene(phase, act)
            TourScene.SEARCH -> TourSearchScene(phase, act)
            TourScene.BAR -> TourBarScene(phase, act)
            TourScene.CHAT -> TourChatScene(phase, act)
            TourScene.PROJECTS -> TourProjectsScene(phase, act)
            TourScene.DESKTOPS -> TourDesktopsScene(phase, act)
            TourScene.WORKSPACE -> TourWorkspaceScene(phase, act)
            TourScene.MARKET -> TourMarketScene(phase, act)
            TourScene.PERSONAL -> TourPersonalScene(phase, act)
            TourScene.SYSTEM -> TourSystemScene(phase, act)
            TourScene.LOOKS -> TourLooksScene(phase, act)
        }
    }
}

@Composable
internal fun rememberTourPulse(active: Boolean = true): Float {
    val reduced = Fm.reducedMotion
    if (!active || reduced) return 0f
    val transition = rememberInfiniteTransition(label = "tour-pulse")
    val pulse by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(850), RepeatMode.Reverse), label = "tour-pulse-value")
    return pulse
}

@Composable
private fun rememberTourPulseState(): State<Float>? {
    val reduced = Fm.reducedMotion
    if (reduced) return null
    val transition = rememberInfiniteTransition(label = "tour-ring")
    return transition.animateFloat(0f, 1f, infiniteRepeatable(tween(850), RepeatMode.Reverse), label = "tour-ring-value")
}

@Composable
internal fun Modifier.tourTarget(lit: Boolean, round: Boolean = false): Modifier {
    if (!lit) return this
    val accent = Fm.colors.accent
    val pulse = rememberTourPulseState()
    return drawWithContent {
        drawContent()
        val p = pulse?.value ?: 0f
        val grow = 1f + 0.12f * p
        val outset = 5.dp.toPx()
        val w = (size.width + outset * 2) * grow
        val h = (size.height + outset * 2) * grow
        val topLeft = Offset((size.width - w) / 2, (size.height - h) / 2)
        val color = accent.copy(alpha = 0.95f - 0.7f * p)
        val stroke = Stroke(width = 2.5.dp.toPx())
        if (round) {
            drawOval(color, topLeft, Size(w, h), style = stroke)
        } else {
            drawRoundRect(color, topLeft, Size(w, h), CornerRadius(h / 2), style = stroke)
        }
    }
}

internal fun Modifier.tourTap(label: String? = null, onClick: () -> Unit): Modifier =
    clickable(interactionSource = null, indication = null, role = Role.Button, onClickLabel = label, onClick = onClick)
        .then(if (label != null) Modifier.semantics { contentDescription = label } else Modifier)

@Composable
internal fun tourEnter(): EnterTransition = if (Fm.reducedMotion) EnterTransition.None else expandVertically() + fadeIn()

@Composable
internal fun tourExit(): ExitTransition = if (Fm.reducedMotion) ExitTransition.None else shrinkVertically() + fadeOut()

@Composable
internal fun tourPop(): EnterTransition = if (Fm.reducedMotion) EnterTransition.None else scaleIn(initialScale = 0.3f) + fadeIn()

@Composable
internal fun tourPopOut(): ExitTransition = if (Fm.reducedMotion) ExitTransition.None else scaleOut(targetScale = 0.3f) + fadeOut()

@Composable
internal fun ColumnScope.TourAppear(visible: Boolean, content: @Composable AnimatedVisibilityScope.() -> Unit) {
    AnimatedVisibility(visible, enter = tourEnter(), exit = tourExit(), content = content)
}

private fun DrawScope.sparkle(center: Offset, radius: Float, color: Color) {
    val waist = radius * 0.22f
    val path = Path().apply {
        moveTo(center.x, center.y - radius)
        quadraticTo(center.x + waist, center.y - waist, center.x + radius, center.y)
        quadraticTo(center.x + waist, center.y + waist, center.x, center.y + radius)
        quadraticTo(center.x - waist, center.y + waist, center.x - radius, center.y)
        quadraticTo(center.x - waist, center.y - waist, center.x, center.y - radius)
        close()
    }
    drawPath(path, color)
}

@Composable
internal fun TourSparkleButton(stars: Int, size: Dp = 36.dp, lit: Boolean = false) {
    val accent = Fm.colors.accent
    val reduced = Fm.reducedMotion
    val spin = if (reduced) {
        0f
    } else {
        val transition = rememberInfiniteTransition(label = "tour-sparkles")
        val turn by transition.animateFloat(0f, 6.2832f, infiniteRepeatable(tween(9000)), label = "tour-sparkles-turn")
        turn
    }
    val glow = accent.copy(alpha = 0.45f)
    Box(
        Modifier
            .tourTarget(lit, round = true)
            .size(size)
            .shadow(8.dp, CircleShape, ambientColor = glow, spotColor = glow)
            .clip(CircleShape)
            .background(Brush.verticalGradient(listOf(accent.copy(alpha = 0.95f), accent, accent.copy(alpha = 0.7f))))
            .drawBehind {
                val count = minOf(stars, 12)
                for (i in 0 until count) {
                    val angle = i * 2.399963f + spin
                    val r = this.size.minDimension * (0.2f + 0.06f * (i % 3))
                    val point = Offset(this.size.width / 2 + r * cos(angle), this.size.height / 2 + r * sin(angle))
                    sparkle(point, this.size.minDimension * 0.07f, Color.White.copy(alpha = 0.85f))
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon("add", size = size * 0.55f, tint = Color.White)
    }
}

@Composable
internal fun TourTopBar(
    project: String = "Personal",
    stars: Int = 2,
    buttonLit: Boolean = false,
    avatarLit: Boolean = false,
    projectLit: Boolean = false,
    onButton: (() -> Unit)? = null,
    onAvatar: (() -> Unit)? = null,
    onProject: (() -> Unit)? = null,
) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            (if (onProject != null) Modifier.tourTap("Project $project", onProject) else Modifier.semantics { contentDescription = "Project $project" }),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.tourTarget(projectLit).size(26.dp).clip(RoundedCornerShape(7.dp)).background(c.accent.copy(alpha = 0.85f)),
                contentAlignment = Alignment.Center,
            ) {
                Text(project.take(1), color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            }
            Text(project, color = c.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        }
        Spacer(Modifier.weight(1f))
        FmIcon("search", size = 18.dp, tint = c.textPrimary)
        Box(if (onButton != null) Modifier.tourTap("New agent", onButton) else Modifier.clearAndSetSemantics {}) {
            TourSparkleButton(stars, lit = buttonLit)
        }
        Box(
            (if (onAvatar != null) Modifier.tourTap("Your hub", onAvatar) else Modifier.clearAndSetSemantics {}).tourTarget(avatarLit, round = true),
        ) {
            AvatarWithDot(RelayConnection.Connected, "You", size = 30.dp)
        }
    }
}

private val tourBarCells = listOf(
    ShellBarCell("tour-home", "Home", "grid_view", 0, false),
    ShellBarCell("tour-agents", "Agents", "auto_awesome", 0, true),
    ShellBarCell("tour-inbox", "Inbox", "inbox", 4, false),
    ShellBarCell("tour-chat", "Chat", "forum", 0, false),
    ShellBarCell("tour-repos", "Repos", "library_books", 0, false),
)

@Composable
internal fun TourBottomBar(style: ShellBarStyle = ShellBarStyle()) {
    BoxWithConstraints(Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, bottom = 8.dp).clearAndSetSemantics {}) {
        ShellBarRows(tourBarCells.map { ShellBarSlot.Single(it) }, style, maxWidth, interactive = false)
    }
}

@Composable
internal fun TourTitle(text: String, trailing: String? = null) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(text, color = c.textPrimary, fontSize = 24.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (trailing != null) {
            Text(
                trailing,
                color = Color.White,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.background(c.accent, CircleShape).padding(horizontal = 7.dp, vertical = 2.dp),
            )
        }
    }
}

@Composable
internal fun TourSectionLabel(text: String) {
    Text(
        text.uppercase(),
        color = Fm.colors.textTertiary,
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        letterSpacing = 0.6.sp,
        modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 6.dp),
    )
}

@Composable
internal fun TourRow(
    icon: String,
    title: String,
    detail: String,
    tint: Color? = null,
    unread: Boolean = false,
    modifier: Modifier = Modifier,
    trailing: @Composable RowScope.() -> Unit = {},
) {
    val c = Fm.colors
    val ink = tint ?: c.accent
    Row(
        modifier.fillMaxWidth().padding(horizontal = 12.dp).clip(RoundedCornerShape(14.dp)).background(c.bgRaised).padding(horizontal = 12.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(30.dp).clip(RoundedCornerShape(9.dp)).background(ink.copy(alpha = 0.14f)), contentAlignment = Alignment.Center) {
            FmIcon(icon, size = 16.dp, tint = ink)
        }
        Column(Modifier.weight(1f)) {
            Text(title, color = c.textPrimary, fontSize = 14.sp, fontWeight = if (unread) FontWeight.SemiBold else FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(detail, color = c.textSecondary, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        trailing()
    }
}

@Composable
internal fun TourPill(text: String, filled: Boolean = true, tint: Color? = null, lit: Boolean = false, modifier: Modifier = Modifier) {
    val base = tint ?: Fm.colors.accent
    val ink = if (filled) Color.White else base
    val fill = if (filled) base else base.copy(alpha = 0.14f)
    Text(
        text,
        color = ink,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        maxLines = 1,
        modifier = modifier.tourTarget(lit).background(fill, CircleShape).padding(horizontal = 10.dp, vertical = 5.dp),
    )
}

@Composable
internal fun TourChevron() {
    FmIcon("chevron_right", size = 14.dp, tint = Fm.colors.textTertiary)
}

@Composable
internal fun TourDot(color: Color) {
    Box(Modifier.size(8.dp).background(color, CircleShape))
}

@Composable
internal fun TourGap(height: Dp) {
    Spacer(Modifier.height(height))
}

@Composable
internal fun TourHSpace(width: Dp) {
    Spacer(Modifier.width(width))
}
