package com.forkmesh.nativeapp.feature.chat

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.ClipOp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlin.math.PI
import kotlin.math.min

@Composable
fun ChatReceiptMark(
    receipt: ChatReceipt,
    modifier: Modifier = Modifier,
    mutedColor: Color? = null,
    accentColor: Color? = null,
    backgroundColor: Color? = null,
) {
    val colors = Fm.colors
    val muted = mutedColor ?: colors.textTertiary
    val accent = accentColor ?: colors.accent
    val background = backgroundColor ?: colors.bgBase
    Canvas(modifier.size(17.dp, 11.dp).semantics { contentDescription = receipt.label }) {
        val s = min(size.width / 22f, size.height / 14f)
        val ox = (size.width - 22f * s) / 2f
        val oy = (size.height - 14f * s) / 2f
        fun at(x: Float, y: Float) = Offset(ox + x * s, oy + y * s)
        fun polyline(points: List<Offset>) = Path().apply {
            moveTo(points[0].x, points[0].y)
            for (point in points.drop(1)) lineTo(point.x, point.y)
        }
        val radius = 5.2f * s
        val width = 1.4f * s
        val front = at(14f, 7f)
        val back = at(8.5f, 7f)
        val stroke = Stroke(width = width, cap = StrokeCap.Round, join = StrokeJoin.Round)
        val frontTick = polyline(listOf(at(11.7f, 7.2f), at(13.4f, 8.9f), at(16.4f, 5.6f)))
        val backTick = polyline(listOf(at(6.2f, 7.2f), at(7.9f, 8.9f), at(9.3f, 7.4f)))
        fun outsideFront(margin: Float, block: DrawScope.() -> Unit) {
            val hole = Path().apply { addOval(Rect(center = front, radius = radius + margin)) }
            clipPath(hole, ClipOp.Difference, block)
        }
        when (receipt) {
            ChatReceipt.SENDING -> {
                val gap = (2f * PI.toFloat() * radius) / 12f
                drawCircle(
                    muted.copy(alpha = 0.7f),
                    radius,
                    front,
                    style = Stroke(width = width, cap = StrokeCap.Round, pathEffect = PathEffect.dashPathEffect(floatArrayOf(0.01f, gap))),
                )
            }
            ChatReceipt.SENT -> {
                drawCircle(muted, radius, front, style = stroke)
                drawPath(frontTick, muted, style = stroke)
            }
            ChatReceipt.DELIVERED -> {
                outsideFront(width / 2f) {
                    drawCircle(muted, radius, back, style = stroke)
                    drawPath(backTick, muted, style = stroke)
                }
                drawCircle(muted, radius, front, style = stroke)
                drawPath(frontTick, muted, style = stroke)
            }
            ChatReceipt.READ -> {
                val disc = radius + width / 2f
                outsideFront(width / 2f + 0.9f * s) {
                    drawCircle(accent, disc, back)
                    drawPath(backTick, background, style = stroke)
                }
                drawCircle(accent, disc, front)
                drawPath(frontTick, background, style = stroke)
            }
        }
    }
}
