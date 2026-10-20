package com.forkmesh.nativeapp.feature.genie

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.drawscope.translate
import com.forkmesh.nativeapp.genie.GenieGait
import com.forkmesh.nativeapp.genie.GenieGravity
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

object GenieInk {
    val glow = Color(0.25f, 0.95f, 0.86f)
    val glowSoft = Color(0.42f, 0.86f, 0.94f)
    val glowDeep = Color(0.05f, 0.55f, 0.56f)
    val lilac = Color(0.56f, 0.40f, 0.82f)
    val violet = Color(0.40f, 0.24f, 0.66f)
    val plum = Color(0.21f, 0.13f, 0.38f)
    val shell = Color(0.10f, 0.10f, 0.20f)
    val shellLight = Color(0.22f, 0.22f, 0.36f)
    val pitch = Color(0.03f, 0.03f, 0.08f)
    val deepSea = Color(0.10f, 0.30f, 0.40f)
    val silver = Color(0.80f, 0.82f, 0.88f)
    val silverDark = Color(0.46f, 0.48f, 0.56f)
    val night = Color(0.06f, 0.05f, 0.14f)
    val dusk = Color(0.16f, 0.11f, 0.30f)
}

class GeniePainter(
    private val mood: GenieMood,
    private val level: Double,
    private val t: Double,
    private val reaction: GenieReaction?,
    private val reactionAge: Double,
    private val leanX: Float,
    private val leanY: Float,
    private val stride: GenieGait,
    private val vitality: Double = 0.7,
) {
    private class Leg(val shoulder: Offset, val knee: Offset, val foot: Offset)

    companion object {
        const val GROUND = 96.2f
        private val shoulders = listOf(Offset(35f, 76f), Offset(30f, 71f), Offset(28f, 65f), Offset(29f, 59f))
        private val knees = listOf(Offset(27f, 79f), Offset(20f, 72f), Offset(15f, 61f), Offset(13.5f, 51f))
        private val feet = listOf(Offset(31f, GROUND), Offset(17.5f, GROUND), Offset(8.5f, GROUND), Offset(2.5f, GROUND))
    }

    private val envelope: Double = reaction?.let { sin(PI * (reactionAge / it.duration).coerceIn(0.0, 1.0)) } ?: 0.0
    private val rise: Double = reaction?.let {
        (reactionAge / 0.22).coerceIn(0.0, 1.0) * ((it.duration - reactionAge) / 0.26).coerceIn(0.0, 1.0)
    } ?: 0.0
    private val walk = stride.walking.coerceIn(0.0, 1.0).toFloat()
    private val air = stride.air.coerceIn(0.0, 1.0).toFloat()
    private val landing = stride.land.coerceIn(0.0, 1.0).toFloat()
    private val surprise = max(if (reaction == GenieReaction.Surprise) envelope.toFloat() else 0f, air * 0.7f)
    private val nod = if (reaction == GenieReaction.Nod) (sin(reactionAge / GenieReaction.Nod.duration * PI * 2) * 1.8).toFloat() else 0f
    private val eureka = if (reaction == GenieReaction.Eureka) rise.toFloat() else 0f
    private val giggle = if (reaction == GenieReaction.Giggle) rise.toFloat() else 0f
    private val cheer = if (reaction == GenieReaction.Cheer) rise.toFloat() else 0f
    private val waving = if (reaction == GenieReaction.Wave) rise.toFloat() else eureka
    private val tiltX = leanX.coerceIn(-1f, 1f)
    private val tiltY = leanY.coerceIn(-1f, 1f)
    private val energy = 0.45 + 0.55 * vitality.coerceIn(0.0, 1.0)
    private val tint = GenieInk.glow

    private val bob: Float = (sin(t * 1.6 * energy) * 1.4).toFloat() - (abs(sin(stride.phase)) * 2.2).toFloat() * walk -
        giggle * abs(sin(reactionAge * 17)).toFloat() * 2.2f + nod

    private val sway: Float = (tiltX * 10 + sin(t * 0.8) * 1.2 + stride.facing * 7 * walk).toFloat()

    private val look: Offset
        get() {
            var x = tiltX * 0.9f + stride.facing.toFloat() * 1.1f * walk
            var y = tiltY * 0.6f
            if (mood == GenieMood.Thinking) {
                x += sin(t * 1.3).toFloat() * 0.8f
                y -= 1.1f
            } else if (mood == GenieMood.Idle && walk < 0.1f) {
                x += (sin(t * 0.37) * sin(t * 0.91)).toFloat() * 0.9f
                y += sin(t * 0.53).toFloat() * 0.4f
            }
            return Offset(x, y)
        }

    private val lid: Float
        get() {
            val cycle = t % 4.3
            val blink = if (cycle < 0.14) sin(cycle / 0.14 * PI).toFloat() else 0f
            return min(max(blink, giggle * 0.6f), 0.95f)
        }

    private val glowStrength: Float
        get() {
            val base = 0.35 + 0.65 * vitality.coerceIn(0.0, 1.0)
            val pulse = if (mood == GenieMood.Thinking) 0.2 * (0.5 + 0.5 * sin(t * 6)) else 0.0
            return min(base + pulse + eureka * 0.4, 1.0).toFloat()
        }

    private fun oval(x: Float, y: Float, rx: Float, ry: Float) = Path().apply { addOval(Rect(x - rx, y - ry, x + rx, y + ry)) }

    private fun mix(a: Offset, b: Offset, k: Float) = Offset(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k)

    private fun DrawScope.glow(x: Float, y: Float, r: Float, color: Color) {
        if (r <= 0f) return
        drawCircle(Brush.radialGradient(listOf(color, color.copy(alpha = 0f)), Offset(x, y), r), r, Offset(x, y))
    }

    fun paint(scope: DrawScope) = with(scope) {
        val side = min(size.width, size.height)
        if (side <= 0f) return@with
        val unit = side / 100f
        translate((size.width - side) / 2, (size.height - side) / 2) {
            scale(unit, unit, Offset.Zero) {
                translate(0f, 50f + GenieGravity.STAND.toFloat() * 100f - GROUND) {
                    aura()
                    withSquash { stage() }
                }
            }
        }
    }

    private fun DrawScope.withSquash(content: DrawScope.() -> Unit) {
        if (landing > 0.01f) {
            scale(1 + landing * 0.1f, 1 - landing * 0.14f, Offset(50f, 93f)) { content() }
        } else {
            content()
        }
    }

    private fun DrawScope.stage() {
        shadow()
        rotate(sway, Offset(50f, 90f)) {
            translate(0f, bob) {
                tail()
                legs(front = false)
                head()
                turban()
                eyes()
                mouth()
                legs(front = true)
                burst()
            }
        }
    }

    private fun DrawScope.aura() {
        val pulse = (0.5 + 0.5 * sin(t * 2.1)).toFloat()
        val boost = if (mood == GenieMood.Listening) level.coerceIn(0.0, 1.0).toFloat() * 8 else 0f
        val color = if (mood == GenieMood.Thinking) GenieInk.violet else tint
        glow(50f, 60f, 38 + pulse * 3 + boost, color.copy(alpha = 0.2f * glowStrength))
        if (mood != GenieMood.Listening) return
        for (ring in 0 until 3) {
            val phase = ((t * 0.8 + ring / 3.0) % 1).toFloat()
            val r = 26 + phase * 24
            drawCircle(tint.copy(alpha = (1 - phase) * 0.5f), r, Offset(50f, 60f), style = Stroke(0.8f))
        }
    }

    private fun DrawScope.shadow() {
        val grounded = 1 - air
        if (grounded <= 0.02f) return
        val y = GROUND + 0.6f
        scale(1f, 0.11f, Offset(50f, y)) {
            drawCircle(
                Brush.radialGradient(
                    0f to GenieInk.pitch.copy(alpha = 0.42f * grounded),
                    0.55f to GenieInk.pitch.copy(alpha = 0.18f * grounded),
                    1f to GenieInk.pitch.copy(alpha = 0f),
                    center = Offset(50f, y), radius = 36f,
                ),
                36f, Offset(50f, y),
            )
        }
    }

    private fun toBody(ground: Offset): Offset {
        val angle = sway * PI / 180
        val c = cos(angle).toFloat()
        val s = sin(angle).toFloat()
        val dx = ground.x - 50
        val dy = ground.y - 90
        return Offset(dx * c + dy * s + 50, -dx * s + dy * c + 90 - bob)
    }

    private fun DrawScope.tail() {
        val swish = sin(t * 1.9).toFloat() * 3 + stride.facing.toFloat() * -4 * walk
        val path = Path().apply {
            moveTo(45f, 79f)
            cubicTo(39f, 84f, 49 + swish, 87f, 42 + swish, 92f)
            cubicTo(35 + swish, 97.5f, 47 + swish, 101f, 53 + swish * 1.3f, 98.5f)
            cubicTo(49.5f + swish, 97f, 44.5f + swish, 96.2f, 45.8f + swish, 93f)
            cubicTo(58.5f + swish, 87f, 55f, 82f, 56f, 79f)
            close()
        }
        drawPath(path, Brush.linearGradient(listOf(GenieInk.shellLight, GenieInk.shell, GenieInk.pitch), Offset(42f, 80f), Offset(52f, 99f)))
        drawPath(path, tint.copy(alpha = 0.32f * glowStrength), style = Stroke(0.45f))
    }

    private fun DrawScope.legs(front: Boolean) {
        for (index in if (front) listOf(1, 0) else listOf(3, 2)) {
            for (side in listOf(-1f, 1f)) drawLeg(pose(index, side), back = !front)
        }
    }

    private fun pose(index: Int, side: Float): Leg {
        fun m(p: Offset) = if (side < 0) p else Offset(100 - p.x, p.y)
        val shoulder = m(shoulders[index])
        var knee = m(knees[index])
        var foot = m(feet[index])
        val group = if ((index % 2 == 0) == (side < 0)) 0.0 else PI
        val swing = stride.phase + group
        val lift = max(0.0, sin(swing)).toFloat() * 6 * walk
        val facing = if (stride.facing == 0.0) 1f else stride.facing.toFloat()
        val reach = cos(swing).toFloat() * 3 * walk * facing
        val breathe = sin(t * 1.6 * energy + index).toFloat() * 0.5f
        foot = toBody(Offset(foot.x + reach, foot.y - lift))
        knee = Offset(knee.x + reach * 0.5f, knee.y - lift * 0.55f - breathe + (1 - vitality.coerceIn(0.0, 1.0)).toFloat() * 3)
        if (air > 0.01f) {
            val dx = shoulder.x - 50
            val dy = shoulder.y - 62
            val length = max(hypot(dx, dy), 0.01f)
            val flail = sin(stride.phase + index * 1.3 + if (side < 0) 0.0 else 0.7).toFloat()
            knee = mix(knee, Offset(shoulder.x + dx / length * 13, shoulder.y + dy / length * 13 - 4 + flail * 2), air)
            foot = mix(foot, Offset(shoulder.x + dx / length * 27, shoulder.y + dy / length * 27 - 3 + flail * 6), air)
        }
        if (index == 0) {
            val raised = if (side < 0) max(waving, cheer) else cheer
            if (raised > 0.01f) {
                val wiggle = sin(reactionAge * 13 + if (side < 0) 0.0 else 1.4).toFloat() * 6 * raised
                foot = mix(foot, m(Offset(10 + wiggle * (if (side < 0) 1 else -1), 38f)), raised)
                knee = mix(knee, m(Offset(21f, 56f)), raised)
            }
        }
        return Leg(shoulder, knee, foot)
    }

    private fun DrawScope.drawLeg(leg: Leg, back: Boolean) {
        val shade = if (back) 0.85f else 1f
        val outward = if (leg.foot.x < 50) -1f else 1f
        drawCircle(
            Brush.radialGradient(listOf(GenieInk.shellLight.copy(alpha = shade), GenieInk.pitch.copy(alpha = shade)), leg.shoulder + Offset(-0.8f, -0.9f), 3.4f),
            3f, leg.shoulder,
        )
        val upper = Path().apply {
            moveTo(leg.shoulder.x, leg.shoulder.y)
            lineTo(leg.knee.x, leg.knee.y)
        }
        drawPath(upper, GenieInk.pitch.copy(alpha = shade), style = Stroke(6.2f, cap = StrokeCap.Round))
        drawPath(upper, GenieInk.shell.copy(alpha = shade), style = Stroke(4.6f, cap = StrokeCap.Round))
        val sheen = Path().apply {
            moveTo(leg.shoulder.x, leg.shoulder.y - 1.1f)
            lineTo(leg.knee.x, leg.knee.y - 1.1f)
        }
        drawPath(sheen, GenieInk.shellLight.copy(alpha = 0.75f * shade), style = Stroke(1.1f, cap = StrokeCap.Round))
        val dx = leg.foot.x - leg.knee.x
        val dy = leg.foot.y - leg.knee.y
        val length = max(sqrt(dx * dx + dy * dy), 0.01f)
        var nx = -dy / length
        var ny = dx / length
        if (nx * outward < 0 || (abs(nx) < 0.05f && ny > 0)) {
            nx = -nx
            ny = -ny
        }
        val mid = Offset(leg.knee.x + dx * 0.42f, leg.knee.y + dy * 0.42f)
        val bulge = min(length * 0.24f, 5.5f)
        val claw = Path().apply {
            moveTo(leg.knee.x + nx * 3.7f, leg.knee.y + ny * 3.7f)
            quadraticTo(mid.x + nx * (bulge + 3), mid.y + ny * (bulge + 3), leg.foot.x, leg.foot.y)
            quadraticTo(mid.x + nx * (bulge * 0.5f - 0.6f), mid.y + ny * (bulge * 0.5f - 0.6f), leg.knee.x - nx * 3.1f, leg.knee.y - ny * 3.1f)
            close()
        }
        drawPath(
            claw,
            Brush.linearGradient(
                0f to GenieInk.shellLight.copy(alpha = shade), 0.35f to GenieInk.shell.copy(alpha = shade),
                0.72f to GenieInk.pitch.copy(alpha = shade), 1f to tint.copy(alpha = glowStrength * shade),
                start = leg.knee, end = leg.foot,
            ),
        )
        val tip = mix(leg.knee, leg.foot, 0.82f)
        val rim = Path().apply {
            moveTo(leg.knee.x + nx * 2.4f, leg.knee.y + ny * 2.4f)
            quadraticTo(mid.x + nx * (bulge + 1.7f), mid.y + ny * (bulge + 1.7f), tip.x, tip.y)
        }
        drawPath(rim, GenieInk.silver.copy(alpha = 0.28f * shade), style = Stroke(0.7f, cap = StrokeCap.Round))
        val start = mix(leg.knee, leg.foot, 0.12f)
        val end = mix(leg.knee, leg.foot, 0.62f)
        val vein = Path().apply {
            moveTo(start.x, start.y)
            quadraticTo(mid.x + nx * bulge * 0.8f, mid.y + ny * bulge * 0.8f, end.x, end.y)
        }
        drawPath(vein, tint.copy(alpha = 0.7f * glowStrength * shade), style = Stroke(0.75f, cap = StrokeCap.Round))
        glow(leg.foot.x, leg.foot.y, 3.6f, tint.copy(alpha = 0.6f * glowStrength * shade))
        glow(leg.knee.x, leg.knee.y, 6f, tint.copy(alpha = 0.42f * glowStrength * shade))
        drawCircle(
            Brush.radialGradient(listOf(GenieInk.shellLight.copy(alpha = shade), GenieInk.pitch.copy(alpha = shade)), leg.knee + Offset(-1f, -1.2f), 4.4f),
            3.9f, leg.knee,
        )
        drawCircle(
            Brush.radialGradient(
                listOf(Color.White.copy(alpha = 0.9f * shade), tint.copy(alpha = glowStrength * shade), GenieInk.glowDeep.copy(alpha = shade)),
                leg.knee + Offset(-0.5f, -0.5f), 2.4f,
            ),
            2.2f, leg.knee,
        )
        drawCircle(GenieInk.silverDark.copy(alpha = 0.5f * shade), 3.9f, leg.knee, style = Stroke(0.35f))
    }

    private fun DrawScope.head() {
        val face = oval(50f, 61f, 25.5f, 25f)
        drawPath(face, Brush.radialGradient(listOf(GenieInk.shellLight, GenieInk.shell, GenieInk.pitch), Offset(41f, 54f), 34f))
        val rim = Path().apply { arcTo(Rect(Offset(50f, 61f), 24.6f), 15f, 65f, true) }
        drawPath(rim, tint.copy(alpha = 0.4f * glowStrength), style = Stroke(0.8f, cap = StrokeCap.Round))
        clipPath(face) { drawPath(oval(37f, 77f, 7f, 4f), Color.White.copy(alpha = 0.05f)) }
        drawPath(oval(38f, 73.5f, 4f, 2.2f), GenieInk.violet.copy(alpha = 0.24f))
        drawPath(oval(62f, 73.5f, 4f, 2.2f), GenieInk.violet.copy(alpha = 0.24f))
    }

    private fun DrawScope.feather() {
        val hunger = 1 - vitality.coerceIn(0.0, 1.0)
        rotate((sin(t * 1.1) * 4 + hunger * 38 + tiltX * -6).toFloat(), Offset(50f, 27f)) {
            val base = Offset(50f, 28f)
            val bend = Offset(44f, 13f)
            val tip = Offset(55f, 0.5f)
            fun spine(u: Float): Offset {
                val a = (1 - u) * (1 - u)
                val b = 2 * (1 - u) * u
                val c = u * u
                return Offset(a * base.x + b * bend.x + c * tip.x, a * base.y + b * bend.y + c * tip.y)
            }
            fun normal(u: Float): Offset {
                val dx = 2 * (1 - u) * (bend.x - base.x) + 2 * u * (tip.x - bend.x)
                val dy = 2 * (1 - u) * (bend.y - base.y) + 2 * u * (tip.y - bend.y)
                val length = max(hypot(dx, dy), 0.01f)
                return Offset(-dy / length, dx / length)
            }
            val steps = 14
            val left = mutableListOf<Offset>()
            val right = mutableListOf<Offset>()
            for (k in 0..steps) {
                val u = k.toFloat() / steps
                val notch = if (k % 2 == 1) 0.74f else 1f
                val width = 7.4f * sin(PI * min(u * 1.05f, 1f)).pow(0.75).toFloat() * (1 - u * 0.3f) * notch
                val p = spine(u)
                val n = normal(u)
                left.add(Offset(p.x + n.x * width, p.y + n.y * width))
                right.add(Offset(p.x - n.x * width * 0.92f, p.y - n.y * width * 0.92f))
            }
            val leaf = Path().apply {
                moveTo(base.x, base.y)
                left.forEach { lineTo(it.x, it.y) }
                right.reversed().forEach { lineTo(it.x, it.y) }
                close()
            }
            drawPath(leaf, Brush.linearGradient(listOf(GenieInk.shellLight, GenieInk.shell, GenieInk.pitch), Offset(42f, 8f), Offset(58f, 26f)))
            drawPath(leaf, GenieInk.pitch.copy(alpha = 0.6f), style = Stroke(0.35f))
            val vein = Path().apply {
                moveTo(base.x, base.y)
                val end = spine(0.94f)
                quadraticTo(bend.x, bend.y, end.x, end.y)
            }
            drawPath(vein, tint.copy(alpha = 0.3f * glowStrength), style = Stroke(2.4f, cap = StrokeCap.Round))
            drawPath(vein, tint.copy(alpha = 0.95f * glowStrength), style = Stroke(0.8f, cap = StrokeCap.Round))
        }
    }

    private fun DrawScope.silver(path: Path, width: Float) {
        drawPath(path, GenieInk.pitch.copy(alpha = 0.55f), style = Stroke(width + 1, cap = StrokeCap.Round, join = StrokeJoin.Round))
        drawPath(path, GenieInk.silverDark, style = Stroke(width, cap = StrokeCap.Round, join = StrokeJoin.Round))
        drawPath(path, GenieInk.silver, style = Stroke(width * 0.62f, cap = StrokeCap.Round, join = StrokeJoin.Round))
        translate(0f, -width * 0.18f) {
            drawPath(path, Color.White.copy(alpha = 0.7f), style = Stroke(width * 0.18f, cap = StrokeCap.Round, join = StrokeJoin.Round))
        }
    }

    private fun DrawScope.turban() {
        feather()
        val dome = Path().apply {
            moveTo(21.5f, 60f)
            cubicTo(15f, 39f, 29f, 23f, 50f, 23f)
            cubicTo(71f, 23f, 85f, 39f, 78.5f, 60f)
            cubicTo(75.5f, 51f, 64f, 44.5f, 50f, 44.5f)
            cubicTo(36f, 44.5f, 24.5f, 51f, 21.5f, 60f)
            close()
        }
        drawPath(dome, Brush.linearGradient(listOf(GenieInk.lilac, GenieInk.violet, GenieInk.plum, GenieInk.shell), Offset(36f, 22f), Offset(58f, 64f)))
        clipPath(dome) {
            for (k in 0 until 4) {
                val y = 58f - k * 8.5f
                val peak = 31f - k * 6f
                val fold = Path().apply {
                    moveTo(18f, y)
                    cubicTo(26f, peak, 70f, peak - 4, 82f, y - 3)
                }
                drawPath(fold, GenieInk.pitch.copy(alpha = 0.42f), style = Stroke(2.2f))
                translate(0f, 1.5f) { drawPath(fold, GenieInk.lilac.copy(alpha = 0.4f), style = Stroke(0.9f)) }
            }
            drawPath(oval(37f, 31f, 10f, 6f), Color.White.copy(alpha = 0.1f))
            val trim = Path().apply {
                moveTo(23f, 55f)
                quadraticTo(20.5f, 40f, 32f, 32f)
                moveTo(77f, 55f)
                quadraticTo(79.5f, 40f, 68f, 32f)
            }
            drawPath(trim, tint.copy(alpha = 0.32f * glowStrength), style = Stroke(2f, cap = StrokeCap.Round))
            drawPath(trim, tint.copy(alpha = 0.85f * glowStrength), style = Stroke(0.6f, cap = StrokeCap.Round))
        }
        silver(
            Path().apply {
                moveTo(22.4f, 59.6f)
                cubicTo(25.4f, 51f, 36f, 44.5f, 50f, 44.5f)
                cubicTo(64f, 44.5f, 74.6f, 51f, 77.6f, 59.6f)
            },
            3.4f,
        )
        silver(
            Path().apply {
                moveTo(33.5f, 28.5f)
                quadraticTo(43f, 31f, 50f, 40f)
                quadraticTo(57f, 31f, 66.5f, 28.5f)
            },
            2.6f,
        )
        val gem = Offset(50f, 40.5f)
        glow(gem.x, gem.y, 8 + eureka * 5, tint.copy(alpha = 0.5f * glowStrength))
        drawCircle(Brush.radialGradient(listOf(Color.White, GenieInk.silver, GenieInk.silverDark), gem + Offset(-1.8f, -2f), 7f), 5.4f, gem)
        drawCircle(GenieInk.pitch, 3.9f, gem)
        drawCircle(Brush.radialGradient(listOf(Color.White, tint, GenieInk.glowDeep), gem + Offset(-0.8f, -0.8f), 3.4f), 3f, gem)
        drawPath(oval(gem.x - 1, gem.y - 1.1f, 0.8f, 0.7f), Color.White.copy(alpha = 0.95f))
        val diamond = Path().apply {
            moveTo(50f, 46.3f)
            lineTo(52.6f, 49.8f)
            lineTo(50f, 54f)
            lineTo(47.4f, 49.8f)
            close()
        }
        drawPath(diamond, Brush.linearGradient(listOf(Color.White, GenieInk.silver, GenieInk.silverDark), Offset(48f, 46f), Offset(52f, 54f)))
        drawPath(diamond, GenieInk.pitch.copy(alpha = 0.5f), style = Stroke(0.3f))
        val inner = Path().apply {
            moveTo(50f, 48.2f)
            lineTo(51.2f, 49.9f)
            lineTo(50f, 51.8f)
            lineTo(48.8f, 49.9f)
            close()
        }
        drawPath(inner, tint.copy(alpha = glowStrength))
    }

    private fun DrawScope.eyes() {
        val wide = 1 + surprise * 0.2f
        val spots = listOf(
            Triple(43.6f, 66.8f, 6f), Triple(56.4f, 66.8f, 6f), Triple(33.4f, 60.4f, 4.4f),
            Triple(66.6f, 60.4f, 4.4f), Triple(44.2f, 55.6f, 3.1f), Triple(55.8f, 55.6f, 3.1f),
        )
        for (spot in spots) eye(Offset(spot.first, spot.second), spot.third * wide)
    }

    private fun DrawScope.eye(c: Offset, r: Float) {
        glow(c.x, c.y, r * 1.55f, tint.copy(alpha = 0.3f * glowStrength))
        drawCircle(GenieInk.pitch, r * 1.13f, c)
        drawCircle(tint.copy(alpha = 0.85f * glowStrength + 0.15f), r, c, style = Stroke(max(r * 0.11f, 0.35f)))
        clipPath(oval(c.x, c.y, r * 0.93f, r * 0.93f)) {
            val shift = Offset(c.x + look.x * r * 0.16f, c.y + look.y * r * 0.16f)
            drawCircle(Brush.radialGradient(listOf(GenieInk.deepSea, GenieInk.pitch), Offset(shift.x, shift.y + r * 0.45f), r * 1.05f), r, c)
            drawCircle(GenieInk.pitch.copy(alpha = 0.9f), r * 0.58f, Offset(shift.x, shift.y + r * 0.05f))
            val shine = Path().apply { arcTo(Rect(shift, r * 0.72f), 25f, 130f, true) }
            drawPath(shine, tint.copy(alpha = 0.55f * glowStrength + 0.1f), style = Stroke(r * 0.13f, cap = StrokeCap.Round))
            drawPath(oval(shift.x - r * 0.3f, shift.y - r * 0.33f, r * 0.27f, r * 0.23f), Color.White.copy(alpha = 0.96f))
            drawCircle(Color.White.copy(alpha = 0.75f), r * 0.11f, Offset(shift.x + r * 0.32f, shift.y + r * 0.22f))
        }
        val closed = lid
        if (closed > 0.02f) {
            clipPath(oval(c.x, c.y, r * 1.02f, r * 1.02f)) {
                val drop = r * 2 * closed
                drawRect(GenieInk.shell, Offset(c.x - r * 1.1f, c.y - r * 1.1f), Size(r * 2.2f, drop))
                drawLine(tint.copy(alpha = 0.4f * glowStrength), Offset(c.x - r, c.y - r + drop), Offset(c.x + r, c.y - r + drop), max(r * 0.1f, 0.3f))
            }
        }
    }

    private fun DrawScope.mouth() {
        val c = Offset(50f, 76.4f)
        if (mood == GenieMood.Speaking) {
            val open = ((0.5 + 0.5 * sin(t * 17)) * (0.65 + 0.35 * sin(t * 6.1))).toFloat()
            val hole = oval(c.x, c.y + 0.3f, 2.6f, 0.5f + 1.9f * open)
            drawPath(hole, GenieInk.pitch)
            drawPath(hole, tint.copy(alpha = 0.8f), style = Stroke(0.45f))
            return
        }
        if (surprise > 0.05f) {
            drawPath(oval(c.x, c.y, 1.2f + surprise, 1.2f + surprise * 1.3f), tint.copy(alpha = 0.9f), style = Stroke(0.55f))
            return
        }
        val grin = 1.6f + giggle * 1.2f + cheer + (if (mood == GenieMood.Listening) 0.4f else 0f) - (if (mood == GenieMood.Thinking) 1.2f else 0f)
        val arc = Path().apply {
            moveTo(c.x - 2.6f, c.y)
            quadraticTo(c.x, c.y + grin, c.x + 2.6f, c.y)
        }
        drawPath(arc, tint.copy(alpha = 0.95f), style = Stroke(0.6f, cap = StrokeCap.Round))
    }

    private fun DrawScope.burst() {
        if (eureka <= 0.05f) return
        val progress = (reactionAge / GenieReaction.Eureka.duration).coerceIn(0.0, 1.0).toFloat()
        for (index in 0 until 8) {
            val angle = index / 8.0 * PI * 2
            val inner = 7 + progress * 4
            val outer = inner + 4 * eureka
            drawLine(
                tint.copy(alpha = eureka * (1 - progress * 0.7f)),
                Offset(50 + cos(angle).toFloat() * inner, 41 + sin(angle).toFloat() * inner),
                Offset(50 + cos(angle).toFloat() * outer, 41 + sin(angle).toFloat() * outer),
                0.7f, cap = StrokeCap.Round,
            )
        }
    }
}
