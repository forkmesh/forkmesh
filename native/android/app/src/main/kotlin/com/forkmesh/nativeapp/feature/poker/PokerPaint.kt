package com.forkmesh.nativeapp.feature.poker

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.poker.PokerCard
import com.forkmesh.nativeapp.poker.PokerChips
import com.forkmesh.nativeapp.poker.PokerClockTone
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

internal object PokerColors {
    val room = Color(0xFF0B0C0F)
    val roomGlow = Color(0xFF1E2320)
    val feltCentre = Color(0xFF0D5C3A)
    val feltEdge = Color(0xFF083D27)
    val feltPrint = Color(0xFF0A4A2F)
    val gold = Color(0xFFC9A24A)
    val goldBright = Color(0xFFFFD54F)
    val walnutLight = Color(0xFF7A4A24)
    val walnut = Color(0xFF4E2C14)
    val walnutDark = Color(0xFF2B160A)
    val leather = Color(0xFF151515)
    val leatherEdge = Color(0xFF050505)
    val red = Color(0xFFC62828)
    val black = Color(0xFF1A1A1A)
    val back = Color(0xFF1E3A8A)
    val backLine = Color(0xFF3B5BC0)
    val clockGreen = Color(0xFF43A047)
    val clockAmber = Color(0xFFFFB300)
    val clockRed = Color(0xFFE53935)
    val plate = Color(0xCC101214)
    val tag = Color(0xE6262A2E)

    fun clock(tone: PokerClockTone): Color = when (tone) {
        PokerClockTone.GREEN -> clockGreen
        PokerClockTone.AMBER -> clockAmber
        PokerClockTone.RED -> clockRed
    }

    fun avatar(name: String): Brush {
        val palette = listOf(
            Color(0xFF5C6BC0) to Color(0xFF3949AB),
            Color(0xFF26A69A) to Color(0xFF00796B),
            Color(0xFFEF5350) to Color(0xFFC62828),
            Color(0xFFAB47BC) to Color(0xFF7B1FA2),
            Color(0xFFFFA726) to Color(0xFFEF6C00),
            Color(0xFF42A5F5) to Color(0xFF1565C0),
            Color(0xFF8D6E63) to Color(0xFF5D4037),
            Color(0xFF66BB6A) to Color(0xFF2E7D32),
        )
        val (a, b) = palette[(name.hashCode() and 0x7fffffff) % palette.size]
        return Brush.linearGradient(listOf(a, b))
    }
}

internal fun stadium(rect: Rect): RoundRect {
    val r = min(rect.width, rect.height) / 2
    return RoundRect(rect, CornerRadius(r, r))
}

internal fun DrawScope.drawRoom() {
    drawRect(Brush.radialGradient(listOf(PokerColors.roomGlow, PokerColors.room), center = Offset(size.width / 2, size.height * 0.45f), radius = size.maxDimension * 0.75f))
}

internal fun DrawScope.drawTable(rect: Rect, rail: Float, logo: ImageBitmap?) {
    val outer = stadium(rect)
    val shadow = Path().apply { addRoundRect(stadium(rect.translate(0f, rail * 0.35f).inflate(rail * 0.15f))) }
    drawPath(shadow, Color.Black.copy(alpha = 0.55f))
    val railPath = Path().apply { addRoundRect(outer) }
    drawPath(railPath, Brush.verticalGradient(listOf(PokerColors.walnutLight, PokerColors.walnut, PokerColors.walnutDark), rect.top, rect.bottom))
    clipPath(railPath) {
        val step = rail * 0.22f
        var y = rect.top
        var i = 0
        while (y < rect.bottom) {
            drawLine(Color.Black.copy(alpha = if (i % 3 == 0) 0.10f else 0.05f), Offset(rect.left, y), Offset(rect.right, y + step * 0.6f), strokeWidth = 1f)
            y += step
            i++
        }
    }
    drawPath(railPath, Color.White.copy(alpha = 0.10f), style = Stroke(width = rail * 0.08f))
    val padRect = rect.deflate(rail * 0.55f)
    val pad = Path().apply { addRoundRect(stadium(padRect)) }
    drawPath(pad, Brush.verticalGradient(listOf(Color(0xFF2A2A2A), PokerColors.leather, PokerColors.leatherEdge), padRect.top, padRect.bottom))
    drawPath(pad, Color.White.copy(alpha = 0.08f), style = Stroke(width = 1.2f))
    val feltRect = rect.deflate(rail * 1.05f)
    val felt = Path().apply { addRoundRect(stadium(feltRect)) }
    drawPath(felt, Brush.radialGradient(listOf(PokerColors.feltCentre, PokerColors.feltEdge), center = feltRect.center, radius = feltRect.maxDimension * 0.6f))
    clipPath(felt) {
        val weave = 5f
        var x = feltRect.left - feltRect.height
        while (x < feltRect.right) {
            drawLine(Color.White.copy(alpha = 0.025f), Offset(x, feltRect.top), Offset(x + feltRect.height, feltRect.bottom), strokeWidth = 1f)
            drawLine(Color.Black.copy(alpha = 0.04f), Offset(x + feltRect.height, feltRect.top), Offset(x, feltRect.bottom), strokeWidth = 1f)
            x += weave
        }
        drawRect(Brush.radialGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.35f)), center = feltRect.center, radius = feltRect.maxDimension * 0.62f), feltRect.topLeft, feltRect.size)
    }
    drawPath(felt, Color.Black.copy(alpha = 0.5f), style = Stroke(width = 3f))
    val line = Path().apply { addRoundRect(stadium(feltRect.deflate(rail * 0.42f))) }
    drawPath(line, PokerColors.gold.copy(alpha = 0.7f), style = Stroke(width = 1.6f))
    if (logo != null) {
        val side = min(feltRect.width, feltRect.height) * 0.26f
        val at = IntOffset((feltRect.center.x - side / 2).toInt(), (feltRect.center.y - side / 2).toInt())
        drawImage(logo, srcOffset = IntOffset.Zero, srcSize = IntSize(logo.width, logo.height), dstOffset = at, dstSize = IntSize(side.toInt(), side.toInt()), alpha = 0.22f, colorFilter = ColorFilter.tint(PokerColors.feltPrint, BlendMode.SrcIn))
    }
}

internal fun DrawScope.drawChipTop(centre: Offset, radius: Float, denomination: Long, alpha: Float = 1f) {
    val base = Color(PokerChips.color(denomination))
    drawCircle(Color.Black.copy(alpha = 0.35f * alpha), radius, centre + Offset(0f, radius * 0.12f))
    drawCircle(base.copy(alpha = alpha), radius, centre)
    val stripe = if (denomination == 1L) Color(0xFF1E3A8A) else Color.White
    for (i in 0 until 6) {
        val a = i * PI / 3
        val at = centre + Offset((cos(a) * radius * 0.8f).toFloat(), (sin(a) * radius * 0.8f).toFloat())
        rotate((a * 180 / PI).toFloat() + 90f, at) {
            drawRect(stripe.copy(alpha = alpha), at - Offset(radius * 0.13f, radius * 0.09f), Size(radius * 0.26f, radius * 0.18f))
        }
    }
    drawCircle(Color.Black.copy(alpha = 0.18f * alpha), radius * 0.6f, centre, style = Stroke(width = radius * 0.08f))
    drawCircle(base.copy(alpha = alpha), radius * 0.56f, centre)
    drawCircle(Color.White.copy(alpha = 0.18f * alpha), radius * 0.56f, centre - Offset(radius * 0.08f, radius * 0.1f), style = Stroke(width = radius * 0.05f))
}

internal fun DrawScope.drawChipEdge(centre: Offset, radius: Float, height: Float, denomination: Long, alpha: Float) {
    val base = Color(PokerChips.color(denomination))
    val squash = 0.42f
    val rect = Rect(centre.x - radius, centre.y - radius * squash, centre.x + radius, centre.y + radius * squash)
    drawOval(Color.Black.copy(alpha = 0.3f * alpha), rect.topLeft + Offset(0f, height), rect.size)
    drawRect(base.copy(alpha = alpha), Offset(rect.left, centre.y), Size(rect.width, height))
    drawOval(base.copy(alpha = alpha), Offset(rect.left, centre.y + height - rect.height / 2), rect.size)
    val stripe = if (denomination == 1L) Color(0xFF1E3A8A) else Color.White
    for (i in 0 until 4) {
        val x = rect.left + rect.width * (0.12f + i * 0.25f)
        drawRect(stripe.copy(alpha = 0.9f * alpha), Offset(x, centre.y), Size(rect.width * 0.08f, height))
    }
    drawOval(base.copy(alpha = alpha), rect.topLeft, rect.size)
    drawOval(Color.White.copy(alpha = 0.22f * alpha), rect.topLeft, rect.size, style = Stroke(width = 1f))
    drawOval(stripe.copy(alpha = 0.75f * alpha), rect.deflate(radius * 0.3f).topLeft, rect.deflate(radius * 0.3f).size, style = Stroke(width = radius * 0.12f))
}

internal fun DrawScope.drawStack(bottomCentre: Offset, radius: Float, chips: List<Long>, alpha: Float = 1f) {
    val thickness = radius * 0.24f
    chips.forEachIndexed { i, denomination ->
        drawChipEdge(bottomCentre - Offset(0f, i * thickness + thickness), radius, thickness, denomination, alpha)
    }
}

@Composable
internal fun ChipStackView(amount: Long, modifier: Modifier = Modifier, chip: Dp = 18.dp, maxChips: Int = 8, alpha: Float = 1f) {
    val chips = remember(amount, maxChips) { PokerChips.visual(amount, maxChips) }
    Canvas(modifier.size(chip * 1.2f, chip * (0.6f + 0.24f * maxChips))) {
        val r = size.width / 2.4f
        drawStack(Offset(size.width / 2, size.height - r * 0.5f), r, chips, alpha)
    }
}

internal fun DrawScope.drawLattice(rect: Rect) {
    val step = rect.width / 7f
    var x = rect.left - rect.height
    while (x < rect.right + rect.height) {
        drawLine(PokerColors.backLine, Offset(x, rect.top), Offset(x + rect.height, rect.bottom), strokeWidth = 1f)
        drawLine(PokerColors.backLine, Offset(x + rect.height, rect.top), Offset(x, rect.bottom), strokeWidth = 1f)
        x += step
    }
}

@Composable
internal fun PokerCardView(card: PokerCard?, width: Dp, modifier: Modifier = Modifier, faceUp: Float = 1f, lifted: Boolean = false, dim: Boolean = false) {
    val height = width * 1.4f
    val shape = RoundedCornerShape(width * 0.1f)
    val face = card?.takeIf { !it.faceDown && faceUp >= 0.5f }
    val showFace = face != null
    val turn = if (card != null && !card.faceDown) (1f - faceUp) * 180f else 0f
    Box(
        modifier
            .size(width, height)
            .graphicsLayer {
                rotationY = if (turn > 90f) turn - 180f else turn
                cameraDistance = 12f * density
                translationY = if (lifted) -width.toPx() * 0.22f else 0f
                alpha = if (dim) 0.45f else 1f
            }
            .shadow(if (lifted) 8.dp else 3.dp, shape)
            .clip(shape)
            .background(if (showFace) Color.White else PokerColors.back)
            .then(if (lifted) Modifier.border(2.dp, PokerColors.goldBright, shape) else Modifier),
    ) {
        if (face != null) {
            val ink = if (face.red) PokerColors.red else PokerColors.black
            val corner = (width.value * 0.3f).sp
            Column(Modifier.align(Alignment.TopStart).padding(start = width * 0.07f, top = width * 0.04f), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(face.rankLabel, color = ink, fontSize = corner, fontWeight = FontWeight.Bold, lineHeight = corner)
                Text(face.suitSymbol, color = ink, fontSize = corner * 0.85f, lineHeight = corner * 0.85f)
            }
            Column(Modifier.align(Alignment.BottomEnd).padding(end = width * 0.07f, bottom = width * 0.04f).graphicsLayer { rotationZ = 180f }, horizontalAlignment = Alignment.CenterHorizontally) {
                Text(face.rankLabel, color = ink, fontSize = corner, fontWeight = FontWeight.Bold, lineHeight = corner)
                Text(face.suitSymbol, color = ink, fontSize = corner * 0.85f, lineHeight = corner * 0.85f)
            }
            Text(face.suitSymbol, color = ink, fontSize = (width.value * 0.62f).sp, modifier = Modifier.align(Alignment.Center).padding(top = width * 0.08f))
        } else {
            Canvas(Modifier.fillMaxSize()) {
                val inset = size.width * 0.09f
                val inner = Rect(inset, inset, size.width - inset, size.height - inset)
                drawRect(PokerColors.back)
                clipPath(Path().apply { addRoundRect(RoundRect(inner, CornerRadius(inset, inset))) }) {
                    drawRect(Color(0xFF172E70), inner.topLeft, inner.size)
                    drawLattice(inner)
                }
                drawRoundRect(Color.White, inner.topLeft, inner.size, CornerRadius(inset, inset), style = Stroke(width = size.width * 0.045f))
            }
        }
    }
}

@Composable
internal fun CardSlot(width: Dp, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(width * 0.1f)
    Box(modifier.size(width, width * 1.4f).border(1.dp, Color.White.copy(alpha = 0.12f), shape).background(Color.Black.copy(alpha = 0.08f), shape))
}

@Composable
internal fun DealerPuck(size: Dp, modifier: Modifier = Modifier) {
    Box(
        modifier
            .size(size)
            .shadow(4.dp, CircleShape)
            .clip(CircleShape)
            .background(Brush.radialGradient(listOf(Color.White, Color(0xFFE0E0E0))))
            .border(1.dp, Color(0xFFBDBDBD), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text("DEALER", color = Color(0xFF1A1A1A), fontSize = (size.value * 0.2f).sp, fontWeight = FontWeight.Black, letterSpacing = 0.sp, maxLines = 1)
    }
}

internal fun DrawScope.drawDealer(centre: Offset, scale: Float) {
    val tray = Rect(centre.x - 46 * scale, centre.y + 16 * scale, centre.x + 46 * scale, centre.y + 34 * scale)
    drawRoundRect(Color(0xFF111111), tray.topLeft, tray.size, CornerRadius(5 * scale, 5 * scale))
    val colors = listOf(5000L, 1000L, 500L, 100L, 25L, 5L, 1L)
    val slot = tray.width / colors.size
    colors.forEachIndexed { i, value ->
        val x = tray.left + slot * i + slot * 0.12f
        drawRoundRect(Color(PokerChips.color(value)), Offset(x, tray.top + 3 * scale), Size(slot * 0.76f, tray.height - 6 * scale), CornerRadius(2 * scale, 2 * scale))
        drawLine(Color.Black.copy(alpha = 0.35f), Offset(x, tray.center.y), Offset(x + slot * 0.76f, tray.center.y), strokeWidth = 1f)
    }
    drawRoundRect(Color.White.copy(alpha = 0.15f), tray.topLeft, tray.size, CornerRadius(5 * scale, 5 * scale), style = Stroke(width = 1f))
    val shoulders = Path().apply {
        moveTo(centre.x - 30 * scale, centre.y + 16 * scale)
        cubicTo(centre.x - 28 * scale, centre.y - 2 * scale, centre.x - 14 * scale, centre.y - 6 * scale, centre.x, centre.y - 6 * scale)
        cubicTo(centre.x + 14 * scale, centre.y - 6 * scale, centre.x + 28 * scale, centre.y - 2 * scale, centre.x + 30 * scale, centre.y + 16 * scale)
        close()
    }
    drawPath(shoulders, Brush.verticalGradient(listOf(Color(0xFF2B2B2B), Color(0xFF101010)), centre.y - 6 * scale, centre.y + 16 * scale))
    val shirt = Path().apply {
        moveTo(centre.x - 7 * scale, centre.y - 6 * scale)
        lineTo(centre.x + 7 * scale, centre.y - 6 * scale)
        lineTo(centre.x, centre.y + 12 * scale)
        close()
    }
    drawPath(shirt, Color.White)
    val bow = Path().apply {
        moveTo(centre.x, centre.y - 2 * scale)
        lineTo(centre.x - 6 * scale, centre.y - 5.5f * scale)
        lineTo(centre.x - 6 * scale, centre.y + 1.5f * scale)
        close()
        moveTo(centre.x, centre.y - 2 * scale)
        lineTo(centre.x + 6 * scale, centre.y - 5.5f * scale)
        lineTo(centre.x + 6 * scale, centre.y + 1.5f * scale)
        close()
    }
    drawPath(bow, Color(0xFFB71C1C))
    drawCircle(Color(0xFFE0B58C), 10 * scale, Offset(centre.x, centre.y - 17 * scale))
    drawArc(Color(0xFF3E2723), 180f, 180f, true, Offset(centre.x - 10.5f * scale, centre.y - 28 * scale), Size(21 * scale, 16 * scale))
    drawCircle(Color.Black.copy(alpha = 0.18f), 10 * scale, Offset(centre.x, centre.y - 17 * scale), style = Stroke(width = 1f))
}

internal fun DrawScope.drawTurnRing(fraction: Float, tone: PokerClockTone, width: Float) {
    val inset = width / 2
    drawArc(Color.Black.copy(alpha = 0.45f), 0f, 360f, false, Offset(inset, inset), Size(size.width - width, size.height - width), style = Stroke(width = width))
    drawArc(PokerColors.clock(tone), -90f, 360f * fraction, false, Offset(inset, inset), Size(size.width - width, size.height - width), style = Stroke(width = width, cap = StrokeCap.Round))
}
