package com.forkmesh.nativeapp.feature.aquarium

import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.PathMeasure
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Density
import com.forkmesh.nativeapp.aquarium.AquariumBit
import com.forkmesh.nativeapp.aquarium.AquariumColor
import com.forkmesh.nativeapp.aquarium.AquariumFish
import com.forkmesh.nativeapp.aquarium.AquariumMark
import com.forkmesh.nativeapp.aquarium.AquariumMarkKind
import com.forkmesh.nativeapp.aquarium.AquariumParticle
import com.forkmesh.nativeapp.aquarium.AquariumRect
import com.forkmesh.nativeapp.aquarium.AquariumSpecies
import com.forkmesh.nativeapp.aquarium.AquariumTank
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal class AquariumPainter(
    private val tank: AquariumTank,
    private val measurer: TextMeasurer,
    private val layouts: HashMap<String, TextLayoutResult>,
) {
    private var density: Density = Density(1f)

    fun paint(scope: DrawScope) {
        density = scope
        with(scope) {
            drawRect(WATER.fade(0.035), Offset.Zero, Size(tank.screen.width.toFloat(), tank.screen.height.toFloat()))
            for (mark in tank.marks) drawMark(mark)
            for (particle in tank.particles) drawParticle(particle)
            for (fish in tank.fish) drawFish(fish)
            for (fish in tank.fish) drawSpeech(fish)
        }
    }

    private fun Color.fade(alpha: Double): Color = copy(alpha = (this.alpha * alpha).toFloat().coerceIn(0f, 1f))

    private fun AquariumColor.color(): Color = Color(red.toFloat(), green.toFloat(), blue.toFloat())

    private fun AquariumRect.rect(): Rect = Rect(minX.toFloat(), minY.toFloat(), maxX.toFloat(), maxY.toFloat())

    private fun pt(x: Double, y: Double): Offset = Offset(x.toFloat(), y.toFloat())

    private fun fade(progress: Double): Double = if (progress > 0.8) max(0.0, (1 - progress) / 0.2) else 1.0

    private fun roundedPath(rect: AquariumRect, radius: Double): Path =
        Path().apply { addRoundRect(RoundRect(rect.rect(), CornerRadius(radius.toFloat()))) }

    private fun ovalPath(cx: Double, cy: Double, rx: Double, ry: Double): Path =
        Path().apply { addOval(Rect((cx - rx).toFloat(), (cy - ry).toFloat(), (cx + rx).toFloat(), (cy + ry).toFloat())) }

    private fun dots(width: Double, on: Float, off: Float, round: Boolean = true): Stroke =
        Stroke(
            width = width.toFloat(),
            cap = if (round) StrokeCap.Round else StrokeCap.Butt,
            pathEffect = PathEffect.dashPathEffect(floatArrayOf(on, off)),
        )

    private fun DrawScope.drawCircle(color: Color, cx: Double, cy: Double, radius: Double) {
        drawCircle(color, radius.toFloat(), pt(cx, cy))
    }

    private fun DrawScope.strokeCircle(color: Color, cx: Double, cy: Double, radius: Double, width: Double) {
        drawCircle(color, radius.toFloat(), pt(cx, cy), style = Stroke(width.toFloat()))
    }

    private fun DrawScope.drawMark(mark: AquariumMark) {
        val tint = mark.tint.color()
        val alpha = fade(mark.progress)
        val rect = mark.rect
        when (val kind = mark.kind) {
            AquariumMarkKind.Ring -> {
                val ring = roundedPath(rect.insetBy(-4.0, -4.0), 8.0)
                drawPath(ring, tint.fade(0.8 * (1 - mark.progress)), style = Stroke(2f))
            }
            AquariumMarkKind.Underline -> {
                val share = min(mark.progress * 2, 1.0)
                val line = Path().apply {
                    moveTo(rect.minX.toFloat(), (rect.maxY + 3).toFloat())
                    lineTo((rect.minX + rect.width * share).toFloat(), (rect.maxY + 3).toFloat())
                }
                drawPath(line, tint.fade(0.75 * alpha), style = dots(2.5, 0.5f, 5f))
            }
            AquariumMarkKind.Highlight -> {
                val share = min(mark.progress * 2.5, 1.0)
                val band = AquariumRect(rect.minX - 2, rect.minY, (rect.width + 4) * share, rect.height)
                drawPath(roundedPath(band, 4.0), tint.fade(0.18 * alpha))
            }
            AquariumMarkKind.Trace -> {
                val outline = roundedPath(rect.insetBy(-5.0, -5.0), 9.0)
                val measure = PathMeasure().apply { setPath(outline, false) }
                val part = Path()
                measure.getSegment(0f, measure.length * min(mark.progress * 1.4, 1.0).toFloat(), part, true)
                drawPath(part, tint.fade(0.7 * alpha), style = dots(2.5, 0.5f, 6f))
            }
            is AquariumMarkKind.Tape -> {
                val y = rect.minY - 6
                val share = min(mark.progress * 2, 1.0)
                val tape = Path().apply {
                    moveTo(rect.minX.toFloat(), y.toFloat())
                    lineTo((rect.minX + rect.width * share).toFloat(), y.toFloat())
                    moveTo(rect.minX.toFloat(), (y - 4).toFloat())
                    lineTo(rect.minX.toFloat(), (y + 4).toFloat())
                    if (share >= 1) {
                        moveTo(rect.maxX.toFloat(), (y - 4).toFloat())
                        lineTo(rect.maxX.toFloat(), (y + 4).toFloat())
                    }
                }
                drawPath(tape, tint.fade(0.85 * alpha), style = dots(1.5, 4f, 3f, round = false))
                if (share >= 1) text(kind.label, 9.0, FontWeight.Bold, tint.fade(alpha), rect.midX, y - 8)
            }
        }
    }

    private fun DrawScope.drawParticle(particle: AquariumParticle) {
        val p = particle.point
        val size = particle.size
        val alpha = 1 - particle.progress * 0.7
        when (val bit = particle.bit) {
            AquariumBit.Bubble, AquariumBit.Splash -> {
                drawCircle(WATER.fade(0.12 * alpha), p.x, p.y, size)
                strokeCircle(Color.White.fade(0.75 * alpha), p.x, p.y, size, 0.8)
            }
            AquariumBit.Sparkle -> {
                val star = Path().apply {
                    moveTo(p.x.toFloat(), (p.y - size).toFloat())
                    quadraticTo(p.x.toFloat(), p.y.toFloat(), (p.x + size).toFloat(), p.y.toFloat())
                    quadraticTo(p.x.toFloat(), p.y.toFloat(), p.x.toFloat(), (p.y + size).toFloat())
                    quadraticTo(p.x.toFloat(), p.y.toFloat(), (p.x - size).toFloat(), p.y.toFloat())
                    quadraticTo(p.x.toFloat(), p.y.toFloat(), p.x.toFloat(), (p.y - size).toFloat())
                }
                drawPath(star, AquariumColor.GOLD.color().fade(alpha))
            }
            AquariumBit.Heart -> glyph("♥", p.x, p.y, size, Color(1f, 0.35f, 0.6f), alpha)
            AquariumBit.Star -> glyph("★", p.x, p.y, size, AquariumColor.GOLD.color(), alpha)
            AquariumBit.Note -> glyph("♪", p.x, p.y, size, Color(0.45f, 0.3f, 0.85f), alpha)
            AquariumBit.Zzz -> glyph("z", p.x, p.y, size, Color(0.4f, 0.55f, 0.85f), alpha)
            AquariumBit.Flash -> drawCircle(Color.White.fade(0.5 * (1 - particle.progress)), p.x, p.y, size * 0.6)
            AquariumBit.Photo -> {
                val card = AquariumRect(p.x - size / 2, p.y - size * 0.6, size, size * 1.2)
                drawPath(roundedPath(card, 2.0), Color.White.fade(0.92 * alpha))
                val inner = card.insetBy(2.5, 2.5).offsetBy(0.0, -2.0).insetBy(0.0, 1.5)
                drawRect(VIOLET.fade(0.8 * alpha), Offset(inner.minX.toFloat(), inner.minY.toFloat()), Size(inner.width.toFloat(), inner.height.toFloat()))
            }
            AquariumBit.Speed -> {
                val line = Path().apply {
                    moveTo(p.x.toFloat(), p.y.toFloat())
                    lineTo((p.x - particle.velocity.width * 0.3).toFloat(), (p.y - particle.velocity.height * 0.3).toFloat())
                }
                drawPath(line, Color.White.fade(0.6 * alpha), style = Stroke(1f))
            }
            is AquariumBit.Glyph -> {
                val radius = size * 0.8
                drawCircle(WATER.fade(0.14 * alpha), p.x, p.y, radius)
                strokeCircle(Color.White.fade(0.7 * alpha), p.x, p.y, radius, 0.8)
                glyph(bit.text, p.x, p.y, size * 0.9, INK, alpha)
            }
        }
    }

    private fun DrawScope.glyph(content: String, x: Double, y: Double, size: Double, color: Color, alpha: Double) {
        text(content, size, FontWeight.Bold, color.fade(alpha), x, y)
    }

    private fun layout(content: String, points: Double, weight: FontWeight): TextLayoutResult {
        if (layouts.size > 400) layouts.clear()
        return layouts.getOrPut("${weight.weight}|$points|$content") {
            measurer.measure(
                content,
                TextStyle(fontSize = with(density) { points.toFloat().toSp() }, fontWeight = weight),
                overflow = TextOverflow.Clip,
                softWrap = false,
                maxLines = 1,
            )
        }
    }

    private fun DrawScope.text(content: String, points: Double, weight: FontWeight, color: Color, cx: Double, cy: Double) {
        val result = layout(content, points, weight)
        drawText(result, color, Offset((cx - result.size.width / 2.0).toFloat(), (cy - result.size.height / 2.0).toFloat()))
    }

    private fun DrawScope.drawFish(fish: AquariumFish) {
        val at = fish.drawn
        val bob = sin(fish.swim * 0.7) * 1.2
        val alpha = fish.opacity * fish.appear
        withTransform({
            translate(at.x.toFloat(), (at.y + bob).toFloat())
            if (fish.facingLeft) scale(-1f, 1f, Offset.Zero)
        }) {
            fishShape(fish, alpha)
        }
        val below = at.y + fish.height / 2 + 9
        val name = layout(fish.label, 9.0, FontWeight.SemiBold)
        val w = name.size.width.toDouble()
        val h = name.size.height.toDouble()
        val pill = AquariumRect(at.x - w / 2 - 4, below - h / 2 - 1, w + 8, h + 2)
        drawPath(roundedPath(pill, pill.height / 2), INK.fade(0.45 * alpha))
        text(fish.label, 9.0, FontWeight.SemiBold, Color.White.fade(alpha), at.x, below)
        if (fish.waiting) {
            val pulse = 1 + sin(tank.time * 4) * 0.12
            val radius = 7 * pulse
            val cx = at.x
            val cy = at.y - fish.height / 2 - 10
            drawCircle(Color(1f, 0.6f, 0.15f).fade(0.95 * alpha), cx, cy, radius)
            text("?", 10.0, FontWeight.ExtraBold, Color.White.fade(alpha), cx, cy)
        }
    }

    private fun DrawScope.fishShape(fish: AquariumFish, alpha: Double) {
        val look = fish.look
        val length = fish.length
        val height = fish.height
        val wag = sin(fish.swim * 6) * height * 0.18
        val bodyColor = look.body.color()
        val finColor = look.fin.color()
        val outline = INK.fade(0.55 * alpha)
        val tailSize = if (look.species == AquariumSpecies.BETTA || look.species == AquariumSpecies.GOLDFISH) 0.75 else 0.5
        val tail = Path().apply {
            moveTo((-length * 0.32).toFloat(), 0f)
            quadraticTo((-length * 0.45).toFloat(), (-height * 0.1).toFloat(), (-length * 0.62).toFloat(), (-height * tailSize + wag).toFloat())
            quadraticTo((-length * 0.52).toFloat(), wag.toFloat(), (-length * 0.62).toFloat(), (height * tailSize + wag).toFloat())
            close()
        }
        drawPath(tail, finColor.fade(0.9 * alpha))
        val fin = if (look.species == AquariumSpecies.ANGELFISH) 0.9 else if (look.species == AquariumSpecies.BETTA) 0.6 else 0.3
        val dorsal = Path().apply {
            moveTo((-length * 0.15).toFloat(), (-height * 0.4).toFloat())
            quadraticTo((-length * 0.12).toFloat(), (-height * (0.5 + fin)).toFloat(), (length * 0.12).toFloat(), (-height * 0.42).toFloat())
            close()
        }
        drawPath(dorsal, finColor.fade(0.85 * alpha))
        if (look.species == AquariumSpecies.ANGELFISH || look.species == AquariumSpecies.BETTA) {
            val anal = Path().apply {
                moveTo((-length * 0.15).toFloat(), (height * 0.4).toFloat())
                quadraticTo((-length * 0.12).toFloat(), (height * (0.5 + fin)).toFloat(), (length * 0.1).toFloat(), (height * 0.42).toFloat())
                close()
            }
            drawPath(anal, finColor.fade(0.85 * alpha))
        }
        val hull = if (look.species == AquariumSpecies.PUFFER) AquariumRect(-height / 2, -height / 2, height, height)
        else AquariumRect(-length * 0.4, -height / 2, length * 0.8, height)
        val skin = ovalPath(hull.midX, hull.midY, hull.width / 2, hull.height / 2)
        drawPath(
            skin,
            Brush.verticalGradient(
                listOf(bodyColor.fade(alpha), bodyColor.fade(0.78 * alpha)),
                startY = hull.minY.toFloat(),
                endY = hull.maxY.toFloat(),
            ),
        )
        clipPath(skin) {
            when (look.species) {
                AquariumSpecies.CLOWNFISH -> for (x in listOf(-0.08, 0.16)) {
                    val band = Rect((length * x - length * 0.05).toFloat(), hull.minY.toFloat(), (length * x + length * 0.05).toFloat(), hull.maxY.toFloat())
                    drawRect(Color.White.fade(0.95 * alpha), band.topLeft, band.size)
                    drawRect(INK.fade(0.6 * alpha), band.topLeft, band.size, style = Stroke(0.8f))
                }
                AquariumSpecies.ANGELFISH -> for (x in listOf(-0.2, 0.0, 0.18)) {
                    drawRect(
                        look.accent.color().fade(0.7 * alpha),
                        pt(length * x, hull.minY),
                        Size((length * 0.05).toFloat(), height.toFloat()),
                    )
                }
                AquariumSpecies.TETRA -> {
                    drawRect(look.accent.color().fade(0.95 * alpha), pt(hull.minX, -height * 0.12), Size(hull.width.toFloat(), (height * 0.18).toFloat()))
                    drawRect(look.fin.color().fade(0.85 * alpha), pt(hull.minX, height * 0.08), Size((hull.width * 0.45).toFloat(), (height * 0.35).toFloat()))
                }
                AquariumSpecies.PUFFER -> for (index in 0 until 6) {
                    val angle = index * 1.05
                    val sx = cos(angle) * height * 0.25 - height * 0.05
                    val sy = sin(angle) * height * 0.22 - height * 0.1
                    drawCircle(look.accent.color().fade(0.7 * alpha), sx, sy, 1.6)
                }
                AquariumSpecies.GOLDFISH, AquariumSpecies.BETTA -> {
                    val shine = ovalPath(hull.minX + hull.width * 0.15 + hull.width * 0.3, hull.minY + 1 + height * 0.15, hull.width * 0.3, height * 0.15)
                    drawPath(shine, Color.White.fade(0.2 * alpha))
                }
            }
        }
        drawPath(skin, outline, style = Stroke(0.9f))
        val eyeRadius = max(2.0, height * 0.13)
        val eyeX = if (look.species == AquariumSpecies.PUFFER) height * 0.22 else length * 0.22
        val eyeY = -height * 0.1
        drawCircle(Color.White.fade(alpha), eyeX, eyeY, eyeRadius)
        val pupil = eyeRadius * 0.55
        drawCircle(INK.fade(alpha), eyeX + eyeRadius * 0.2, eyeY, pupil)
    }

    private fun DrawScope.drawSpeech(fish: AquariumFish) {
        val content = fish.speech ?: return
        val box = fish.speechBox ?: return
        val appear = fish.appear
        val shape = roundedPath(box, 10.0)
        drawPath(shape, Color.White.fade(0.94 * appear))
        drawPath(shape, fish.look.body.color().fade(0.9 * appear), style = Stroke(1.2f))
        text(content, 11.0, FontWeight.SemiBold, INK.fade(appear), box.midX, box.midY)
    }

    companion object {
        val WATER = Color(0.2f, 0.55f, 0.85f)
        val INK = Color(0.08f, 0.1f, 0.16f)
        val VIOLET = Color(0.40f, 0.24f, 0.66f)
    }
}
