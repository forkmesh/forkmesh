package com.forkmesh.nativeapp.feature.agents

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.PromptDeck
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch
import kotlin.math.abs

private enum class DeckMotion { Back, Forward, Arrive }

@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun AgentPromptDeck(deck: PromptDeck, modifier: Modifier = Modifier, tag: String = "agent-prompt-deck") {
    if (deck.isEmpty) return
    val c = Fm.colors
    val context = LocalContext.current
    val density = LocalDensity.current
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val reduceMotion = Fm.reducedMotion
    var held by remember { mutableStateOf<Int?>(null) }
    var motion by remember { mutableStateOf(DeckMotion.Arrive) }
    var expanded by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    val drag = remember { Animatable(0f) }
    val index = deck.clamped(held?.let { deck.count - 1 - it } ?: 0)
    val shape = RoundedCornerShape(FmRadius.lg)

    fun show(next: Int, how: DeckMotion) {
        motion = how
        held = if (next == 0) null else deck.count - 1 - next
        expanded = false
        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
    }

    fun step(older: Boolean) {
        val next = if (older) deck.older(index) else deck.newer(index)
        if (next != index) show(next, if (older) DeckMotion.Back else DeckMotion.Forward)
    }

    LaunchedEffect(deck.count) {
        if (held != null) show(0, DeckMotion.Arrive) else motion = DeckMotion.Arrive
    }

    val behind = deck.behind(index)
    val bottom by animateDpAsState((behind * 3).dp, label = "prompt-deck-edges")
    val label = if (index == 0) "Your last prompt" else "Your earlier prompt"
    val value = listOf(deck.prompt(index), deck.position(index)).filter { it.isNotEmpty() }.joinToString(", ")
    Box(
        modifier
            .fillMaxWidth()
            .padding(bottom = bottom)
            .testTag(tag)
            .semantics(mergeDescendants = true) {
                contentDescription = label
                stateDescription = value
                customActions = listOf(
                    CustomAccessibilityAction("Earlier prompt") { step(older = true); true },
                    CustomAccessibilityAction("Later prompt") { step(older = false); true },
                )
            },
    ) {
        for (depth in 2 downTo 1) {
            if (depth <= behind) {
                Box(
                    Modifier
                        .matchParentSize()
                        .graphicsLayer {
                            scaleX = 1f - depth * 0.05f
                            translationY = depth * 3.dp.toPx()
                            transformOrigin = TransformOrigin(0.5f, 1f)
                        }
                        .background(c.bgRaised, shape)
                        .background(c.accentSubtle.copy(alpha = c.accentSubtle.alpha * 0.6f), shape)
                        .border(0.5.dp, c.border, shape),
                )
            }
        }
        AnimatedContent(
            targetState = deck.count - 1 - index,
            transitionSpec = {
                val spec = spring<androidx.compose.ui.unit.IntOffset>(dampingRatio = 0.72f, stiffness = Spring.StiffnessMediumLow)
                val shift = with(density) { 80.dp.roundToPx() }
                val drop = with(density) { 24.dp.roundToPx() }
                when {
                    reduceMotion -> fadeIn(tween(150)) togetherWith fadeOut(tween(150))
                    motion == DeckMotion.Back -> (slideInHorizontally(spec) { -shift } + fadeIn()) togetherWith (slideOutHorizontally(spec) { shift } + fadeOut())
                    motion == DeckMotion.Forward -> (slideInHorizontally(spec) { shift } + fadeIn()) togetherWith (slideOutHorizontally(spec) { -shift } + fadeOut())
                    else -> (slideInVertically(spec) { -drop } + fadeIn()) togetherWith (scaleOut(targetScale = 0.9f, transformOrigin = TransformOrigin(0.5f, 1f)) + fadeOut())
                }
            },
            label = "prompt-deck-card",
        ) { fromFirst ->
            val shown = deck.clamped(deck.count - 1 - fromFirst)
            Box {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .graphicsLayer {
                            translationX = drag.value
                            rotationZ = if (reduceMotion) 0f else drag.value / density.density * 0.04f
                            transformOrigin = TransformOrigin(0.5f, 1f)
                        }
                        .background(c.bgRaised, shape)
                        .background(c.accentSubtle, shape)
                        .border(0.5.dp, c.border, shape)
                        .pointerInput(deck.count, index) {
                            val tracker = VelocityTracker()
                            var total = 0f
                            detectHorizontalDragGestures(
                                onDragStart = {
                                    total = 0f
                                    tracker.resetTracking()
                                },
                                onDragEnd = {
                                    val speed = tracker.calculateVelocity().x
                                    val travel = (total + speed * 0.1f) / density.density
                                    if (travel > 50f) step(older = true) else if (travel < -50f) step(older = false)
                                    scope.launch { drag.animateTo(0f, spring(dampingRatio = 0.72f, stiffness = Spring.StiffnessMediumLow)) }
                                },
                                onDragCancel = { scope.launch { drag.animateTo(0f) } },
                            ) { change, amount ->
                                change.consume()
                                tracker.addPosition(change.uptimeMillis, change.position)
                                total += amount
                                val free = if (total > 0) deck.older(index) != index else deck.newer(index) != index
                                scope.launch { drag.snapTo(total * if (free) 0.6f else 0.15f) }
                            }
                        }
                        .combinedClickable(
                            onClick = { expanded = !expanded },
                            onLongClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                                menu = true
                            },
                        )
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = if (expanded) Alignment.Top else Alignment.CenterVertically,
                ) {
                    FmIcon(if (shown == 0) "format_quote" else "history", size = 12.dp, tint = c.accent)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(
                        if (expanded) deck.prompt(shown) else deck.headline(shown),
                        Modifier.weight(1f),
                        style = Fm.text.bodySmall.copy(color = c.textPrimary),
                        maxLines = if (expanded) 8 else 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    val position = deck.position(shown)
                    if (position.isNotEmpty()) {
                        Spacer(Modifier.width(FmSpace.x2))
                        Text(position, style = Fm.text.labelSmall.copy(color = c.textTertiary, fontFeatureSettings = "tnum"))
                    }
                }
                FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    FmDropdownMenuItem("Copy prompt", onClick = {
                        menu = false
                        copyToClipboard(context, deck.prompt(shown))
                    }, icon = "content_copy")
                    if (shown != 0) {
                        FmDropdownMenuItem("Back to the latest", onClick = {
                            menu = false
                            show(0, DeckMotion.Forward)
                        }, icon = "vertical_align_top")
                    }
                }
            }
        }
    }
}
