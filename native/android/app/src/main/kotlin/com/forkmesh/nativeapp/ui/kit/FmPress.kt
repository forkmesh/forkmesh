package com.forkmesh.nativeapp.ui.kit

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.drawOutline
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.node.DelegatableNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.invalidateDraw
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import kotlinx.coroutines.launch

class FmPressIndication(private val color: Color, private val shape: Shape = RectangleShape) : IndicationNodeFactory {
    override fun create(interactionSource: InteractionSource): DelegatableNode = PressWashNode(interactionSource, color, shape)

    override fun equals(other: Any?): Boolean = other is FmPressIndication && other.color == color && other.shape == shape

    override fun hashCode(): Int = 31 * color.hashCode() + shape.hashCode()
}

private class PressWashNode(
    private val source: InteractionSource,
    private val color: Color,
    private val shape: Shape,
) : Modifier.Node(), DrawModifierNode {
    private var presses = 0

    override fun onAttach() {
        coroutineScope.launch {
            source.interactions.collect { interaction ->
                when (interaction) {
                    is PressInteraction.Press -> presses++
                    is PressInteraction.Release, is PressInteraction.Cancel -> presses = (presses - 1).coerceAtLeast(0)
                }
                invalidateDraw()
            }
        }
    }

    override fun ContentDrawScope.draw() {
        drawContent()
        if (presses > 0) drawOutline(shape.createOutline(size, layoutDirection, this), color)
    }
}

object FmPress {
    const val DIM = 0.35f
}

@Composable
fun FmSwitch(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val interaction = remember { MutableInteractionSource() }
    val motion = spring<androidx.compose.ui.unit.Dp>(dampingRatio = 0.75f, stiffness = Spring.StiffnessMediumLow)
    val offset = animateDpAsState(if (checked) 22.dp else 2.dp, motion, label = "fmSwitchThumb")
    val off = if (c.dark) Color(0x52787880) else Color(0x29787880)
    val track = animateColorAsState(if (checked) c.accent else off, spring(stiffness = Spring.StiffnessMediumLow), label = "fmSwitchTrack")
    Box(
        modifier
            .size(51.dp, 31.dp)
            .alpha(if (enabled) 1f else 0.5f)
            .clip(RoundedCornerShape(15.5.dp))
            .background(track.value)
            .toggleable(
                value = checked,
                interactionSource = interaction,
                indication = null,
                enabled = enabled && onCheckedChange != null,
                role = Role.Switch,
            ) { on ->
                haptics.selection(SoundCue.toggle(on))
                onCheckedChange?.invoke(on)
            },
        contentAlignment = Alignment.CenterStart,
    ) {
        Box(
            Modifier
                .offset(x = offset.value)
                .padding(vertical = 2.dp)
                .size(27.dp)
                .shadow(2.dp, CircleShape, clip = false, ambientColor = Color.Black.copy(alpha = 0.15f), spotColor = Color.Black.copy(alpha = 0.15f))
                .background(Color.White, CircleShape),
        )
    }
}
