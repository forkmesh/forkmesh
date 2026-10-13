package com.forkmesh.nativeapp.feature.agentwatch

import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agentwatch.AgentMotion
import com.forkmesh.nativeapp.agentwatch.AgentWatchController
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.min

private const val ROW_GLOW_SECONDS = 0.8
private const val ROW_HEIGHT = 42.0

internal fun listedMotions(controller: AgentWatchController): List<AgentMotion> =
    AgentMotion.entries.sortedWith(compareBy<AgentMotion>({ -(controller.landCount[it] ?: 0) }, { it.ordinal }))

internal fun soundGlow(controller: AgentWatchController, motion: AgentMotion, now: Double): Double {
    fun glow(at: Double?): Double {
        if (at == null) return 0.0
        val since = now - at
        return if (since < 0) 0.0 else exp(-since / ROW_GLOW_SECONDS)
    }
    return max(glow(controller.lastSounded[motion]), 0.18 * glow(controller.lastLanded[motion]))
}

private fun rowRect(width: Double) = box(0.0, 0.0, width, ROW_HEIGHT - 3)

private fun switchRect(row: RectF, sound: Boolean): RectF {
    val side = min(16.0, row.height() - 4.0)
    val top = row.top + min((row.height() - side) / 2, 4.0)
    val right = row.right - 4 - (if (sound) 0.0 else side + 6)
    return box(right - side, top, side, side)
}

@Composable
internal fun WatchSoundList(
    controller: AgentWatchController,
    frame: Int,
    hidden: Set<AgentMotion>,
    onToggle: (AgentMotion, Boolean) -> Unit,
    modifier: Modifier,
) {
    val density = LocalDensity.current.density
    val text = remember(density) { WatchText(density) }
    frame.let { }
    val motions = listedMotions(controller)
    LazyColumn(
        modifier
            .padding(start = 10.dp, end = 10.dp, top = 4.dp, bottom = 10.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(Color(argb(170, 8, 11, 20)))
            .padding(6.dp),
    ) {
        items(motions, key = { it.name }) { motion ->
            val shown = motion !in hidden
            val heard = motion !in controller.muted
            Canvas(
                Modifier
                    .fillMaxWidth()
                    .height(ROW_HEIGHT.dp)
                    .semantics {
                        contentDescription = "${motion.label}: ${motion.description}. ♪ ${motion.sound}. ${controller.landCount[motion] ?: 0} so far"
                        customActions = listOf(
                            CustomAccessibilityAction(if (heard) "Mute its sound" else "Hear its sound") { onToggle(motion, true); true },
                            CustomAccessibilityAction(if (shown) "Hide its figure" else "Show its figure") { onToggle(motion, false); true },
                        )
                    }
                    .pointerInput(motion) {
                        detectTapGestures { at ->
                            val row = rowRect(size.width / density.toDouble())
                            val x = at.x / density
                            if (x >= switchRect(row, true).left - 6) onToggle(motion, true)
                            else if (x >= switchRect(row, false).left - 12) onToggle(motion, false)
                        }
                    },
            ) {
                frame.let { }
                drawIntoCanvas { c ->
                    val canvas = c.nativeCanvas
                    canvas.save()
                    canvas.scale(density, density)
                    paintSoundRow(canvas, text, controller, motion, controller.now, shown, heard, size.width / density.toDouble())
                    canvas.restore()
                }
            }
        }
    }
}

private fun paintSoundRow(
    canvas: Canvas,
    text: WatchText,
    controller: AgentWatchController,
    motion: AgentMotion,
    now: Double,
    shown: Boolean,
    heard: Boolean,
    width: Double,
) {
    val row = rowRect(width)
    val glow = soundGlow(controller, motion, now)
    val hue = motion.hue
    val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    if (glow > 0.01) {
        fill.shader = LinearGradient(
            row.left, row.centerY(), row.right, row.centerY(),
            watchInk(hue, 0.9, 0.55, 0.55 * glow), watchInk(hue, 0.9, 0.55, 0.08 * glow), Shader.TileMode.CLAMP,
        )
        canvas.drawRoundRect(row, 6f, 6f, fill)
        fill.shader = null
    }
    fill.color = watchInk(hue, 0.95, 0.66, 0.35 + 0.65 * glow)
    canvas.drawRoundRect(RectF(row.left, row.top + 3, row.left + 3, row.bottom - 3), 1.5f, 1.5f, fill)
    val painter = QtPainter(canvas)
    val iconSize = min(9.0, row.height() * 0.32) * (1.0 + 0.35 * glow)
    val iconAt = O(row.left + 20.0, row.top + min(row.height() / 2.0, 13.0))
    if (glow > 0.05) {
        val lastAt = max(controller.lastSounded[motion] ?: -1e9, controller.lastLanded[motion] ?: -1e9)
        val since = now - lastAt
        val t = (since / max(0.4, motion.seconds)) % 1.0
        canvas.save()
        canvas.clipRect(row)
        paintWatchFigure(painter, motion, iconAt, 5.0, t.coerceIn(0.0, 1.0), 0.1, glow)
        canvas.restore()
    }
    paintWatchIcon(painter, motion, iconAt, iconSize, (0.55 + 0.45 * glow) * (if (shown) 1.0 else 0.35))
    paintSwitch(canvas, switchRect(row, false), false, shown, hue)
    paintSwitch(canvas, switchRect(row, true), true, heard, hue)

    val left = row.left + 38.0
    val textWidth = max(20.0, switchRect(row, false).left - left - 6)
    val opacity = if (shown || heard) 1.0 else 0.45
    val nameSize = (row.height() - 3.0).coerceIn(8.0, 12.0)
    val smallSize = (row.height() - 5.0).coerceIn(7.0, 10.0)
    val nameInk = if (glow > 0.05) watchInk(hue, 0.6, 0.92, (0.7 + 0.3 * glow) * opacity) else withAlpha(WATCH_MUTED, opacity)
    val namePaint = text.paint(nameSize, true, nameInk)
    val top = 3.0
    text.drawTop(canvas, namePaint, text.fit(namePaint, motion.label, textWidth), left, row.top + top)
    val lineHeight = text.lineHeight(namePaint)
    val count = controller.landCount[motion] ?: 0
    if (count > 0) {
        val tallyPaint = text.paint(smallSize, false, withAlpha(WATCH_MUTED, (0.6 + 0.4 * glow) * opacity))
        val tally = text.fit(tallyPaint, "×$count", textWidth)
        val tallyWidth = tallyPaint.measureText(tally)
        text.drawTop(canvas, tallyPaint, tally, left + textWidth - tallyWidth, row.top + top + (lineHeight - text.lineHeight(tallyPaint)) / 2)
    }
    val live = controller.lastText[motion] ?: ""
    val liveAge = now - (controller.lastTextSeconds[motion] ?: -1e9)
    val fresh = exp(-max(0.0, liveAge) / 2.5)
    val secondPaint = if (live.isNotEmpty()) {
        text.paint(smallSize, false, watchInk(hue, 0.55, 0.9, (0.5 + 0.5 * fresh) * opacity))
    } else {
        text.paint(smallSize, false, withAlpha(WATCH_FOREGROUND, (0.55 + 0.4 * glow) * opacity))
    }
    text.drawTop(canvas, secondPaint, text.fit(secondPaint, live.ifEmpty { motion.description }, textWidth), left, row.top + top + lineHeight)
}

private fun paintSwitch(canvas: Canvas, rect: RectF, sound: Boolean, on: Boolean, hue: Double) {
    val colour = if (on) watchInk(hue, 0.7, 0.82, 0.95) else WATCH_MUTED
    val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    if (on) {
        fill.color = watchInk(hue, 0.8, 0.5, 0.22)
        canvas.drawRoundRect(rect, 3f, 3f, fill)
    }
    val cx = rect.centerX()
    val cy = rect.centerY()
    val r = rect.width() / 2 - 2.5f
    val ink = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.2f
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        color = colour
    }
    if (sound) {
        val cone = Path().apply {
            moveTo(cx - r, cy - r * 0.35f)
            lineTo(cx - r * 0.4f, cy - r * 0.35f)
            lineTo(cx + r * 0.15f, cy - r * 0.85f)
            lineTo(cx + r * 0.15f, cy + r * 0.85f)
            lineTo(cx - r * 0.4f, cy + r * 0.35f)
            lineTo(cx - r, cy + r * 0.35f)
            close()
        }
        canvas.drawPath(cone, ink)
        if (on) canvas.drawArc(RectF(cx - r * 0.2f, cy - r * 0.7f, cx + r, cy + r * 0.7f), 50f, -100f, false, ink)
    } else {
        val lids = Path().apply {
            moveTo(cx - r, cy)
            quadTo(cx, cy - r * 1.1f, cx + r, cy)
            quadTo(cx, cy + r * 1.1f, cx - r, cy)
        }
        canvas.drawPath(lids, ink)
        fill.color = colour
        canvas.drawCircle(cx, cy, r * 0.3f, fill)
    }
    if (!on) canvas.drawLine(cx - r, cy + r, cx + r, cy - r, ink)
}
