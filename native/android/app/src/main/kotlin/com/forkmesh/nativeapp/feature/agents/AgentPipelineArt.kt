package com.forkmesh.nativeapp.feature.agents

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathMeasure
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.account.openExternalUrl
import com.forkmesh.nativeapp.feature.account.projects.ProjectActions
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

internal object AgentPipeline {
    const val WIDTH = 340f
    const val HEIGHT = 132f
    const val DURATION_MS = 1600
    val easing = CubicBezierEasing(0.3f, 0.7f, 0.2f, 1f)

    class Stop(val x: Float, val y: Float, val radius: Float, val at: Float)

    class Label(val text: String, val x: Float, val y: Float)

    val stops = listOf(
        Stop(24f, 84f, 7f, 0f),
        Stop(92f, 84f, 6f, 0.2f),
        Stop(160f, 38f, 5f, 0.43f),
        Stop(196f, 38f, 5f, 0.53f),
        Stop(228f, 38f, 7f, 0.62f),
        Stop(280f, 84f, 7f, 0.86f),
    )

    val labels = listOf(
        Label("Prompt", 24f, 112f),
        Label("Mirror", 92f, 112f),
        Label("Branch", 178f, 20f),
        Label("Pull request", 240f, 20f),
        Label("Merged", 280f, 112f),
    )

    fun base(): Path = Path().apply {
        moveTo(24f, 84f)
        lineTo(316f, 84f)
        moveTo(92f, 84f)
        cubicTo(118f, 84f, 118f, 38f, 144f, 38f)
        lineTo(228f, 38f)
        cubicTo(254f, 38f, 254f, 84f, 280f, 84f)
    }

    fun route(): Path = Path().apply {
        moveTo(24f, 84f)
        lineTo(92f, 84f)
        cubicTo(118f, 84f, 118f, 38f, 144f, 38f)
        lineTo(228f, 38f)
        cubicTo(254f, 38f, 254f, 84f, 280f, 84f)
        lineTo(316f, 84f)
    }
}

@Composable
internal fun AgentPipelineArt(modifier: Modifier = Modifier) {
    val c = Fm.colors
    val reduced = Fm.reducedMotion
    val progress = remember { Animatable(if (reduced) 1f else 0f) }
    var plays by remember { mutableIntStateOf(0) }
    LaunchedEffect(plays, reduced) {
        if (reduced) {
            progress.snapTo(1f)
        } else {
            progress.snapTo(0f)
            progress.animateTo(1f, tween(AgentPipeline.DURATION_MS, easing = AgentPipeline.easing))
        }
    }
    val measurer = rememberTextMeasurer()
    val labelStyle = Fm.text.bodySmall.copy(color = c.textSecondary, textAlign = TextAlign.Center)
    val base = remember { AgentPipeline.base() }
    val route = remember { AgentPipeline.route() }
    val measure = remember { PathMeasure().apply { setPath(route, false) } }
    val track = if (c.dark) Color.White.copy(alpha = 0.18f) else Color(0xFFD6D7DB)
    val ring = if (c.dark) Color.White.copy(alpha = 0.28f) else Color(0xFFC9CACF)
    Canvas(
        modifier
            .widthIn(max = 340.dp)
            .fillMaxWidth()
            .aspectRatio(AgentPipeline.WIDTH / AgentPipeline.HEIGHT)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { plays++ }
            .semantics { contentDescription = "A prompt travels to a mirror, onto a branch, into a pull request and is merged" },
    ) {
        val unit = size.width / AgentPipeline.WIDTH
        val t = progress.value
        val length = measure.length
        val done = Path().also { measure.getSegment(0f, length * t, it, true) }
        val point = measure.getPosition(length * t)
        scale(unit, unit, pivot = Offset.Zero) {
            drawPath(base, track, style = Stroke(width = 3f, cap = StrokeCap.Round))
            drawPath(done, c.accent, style = Stroke(width = 3f, cap = StrokeCap.Round))
            AgentPipeline.stops.forEach { stop -> drawStop(stop, lit = t >= stop.at, fill = c.bgRaised, ring = ring, accent = c.accent) }
            drawCircle(c.accent.copy(alpha = 0.16f), radius = 13f, center = point)
            drawCircle(c.accent, radius = 6f, center = point)
        }
        AgentPipeline.labels.forEach { label ->
            val layout = measurer.measure(label.text, labelStyle)
            drawText(
                layout,
                topLeft = Offset(label.x * unit - layout.size.width / 2f, label.y * unit - layout.size.height * 0.75f),
            )
        }
    }
}

private fun DrawScope.drawStop(stop: AgentPipeline.Stop, lit: Boolean, fill: Color, ring: Color, accent: Color) {
    val center = Offset(stop.x, stop.y)
    drawCircle(fill, radius = stop.radius, center = center)
    drawCircle(if (lit) accent else ring, radius = stop.radius, center = center, style = Stroke(width = 2.5f))
}

@Composable
internal fun AgentsEmptyState(hasDesktopNode: Boolean, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val context = LocalContext.current
    Column(modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        FmEmptyState(
            icon = "smart_toy_outlined",
            title = "No agents yet",
            message = "Tap + and describe a change. Follow it here from branch to merge.",
            modifier = Modifier.fillMaxWidth().aspectRatio(1.05f, matchHeightConstraintsFirst = false),
            art = { AgentPipelineArt(Modifier.padding(horizontal = FmSpace.x2)) },
        )
        if (!hasDesktopNode) {
            Row(
                Modifier.padding(horizontal = FmSpace.x4),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "No desktop enrolled. Prompts run on a mirror, or on this phone.",
                    Modifier.weight(1f, fill = false),
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
                FmTextButton(
                    onClick = { openExternalUrl(context, ProjectActions.desktopUrl()) },
                    minHeight = 36.dp,
                    contentPadding = PaddingValues(horizontal = FmSpace.x2),
                ) { Text("How it runs", style = Fm.text.bodySmall.copy(color = c.accent)) }
            }
        }
    }
}
