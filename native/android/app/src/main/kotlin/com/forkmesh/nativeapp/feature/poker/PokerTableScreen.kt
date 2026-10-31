package com.forkmesh.nativeapp.feature.poker

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.imageResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.poker.PokerActionKind
import com.forkmesh.nativeapp.poker.PokerActions
import com.forkmesh.nativeapp.poker.PokerApi
import com.forkmesh.nativeapp.poker.PokerCard
import com.forkmesh.nativeapp.poker.PokerCards
import com.forkmesh.nativeapp.poker.PokerChips
import com.forkmesh.nativeapp.poker.PokerClock
import com.forkmesh.nativeapp.poker.PokerLayout
import com.forkmesh.nativeapp.poker.PokerPhase
import com.forkmesh.nativeapp.poker.PokerSeat
import com.forkmesh.nativeapp.poker.PokerTable
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.max
import kotlin.math.min

internal class PokerTableScreen(private val tableId: String) : Screen() {
    override val title: String get() = "Poker"
    override val hidesShellChrome: Boolean get() = true
    override val extendsUnderTabBar: Boolean get() = true

    private var model: PokerModel? = null

    init {
        pluginPackage = PokerApi.PACKAGE_ID
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val model = remember { PokerFeature.model(context) }
        this.model = model
        DisposableEffect(tableId) {
            model.tableAppeared(tableId)
            onDispose { model.tableGone(tableId) }
        }
        PokerTableView(model, tableId, onBack = { navigator.pop() })
        PokerSheetHost(model, onLeft = { navigator.pop() })
    }

    override fun onBack(): Boolean {
        val current = model ?: return false
        if (current.sheet != null) {
            current.sheet = null
            return true
        }
        if (current.raising) {
            current.raising = false
            return true
        }
        return false
    }
}

private data class TableGeometry(
    val width: Float,
    val height: Float,
    val landscape: Boolean,
    val table: Rect,
    val rail: Float,
    val avatar: Float,
    val holeCard: Float,
    val myCard: Float,
    val boardCard: Float,
) {
    val centre: Offset get() = table.center
    val dealer: Offset get() = Offset(table.center.x, table.top + rail * 0.3f)
    val pot: Offset get() = Offset(table.center.x, table.center.y - boardCard * 1.25f)

    fun seat(slot: Int, count: Int): Offset {
        val unit = PokerLayout.onRail(PokerLayout.position(slot, count, Math.toRadians(if (landscape) 56.0 else 70.0)))
        val rx = table.width / 2 - rail * 0.45f
        val ry = table.height / 2 - rail * 0.45f
        val lift = if (slot == 0) -rail * 0.2f else 0f
        return Offset(table.center.x + (unit.x * rx).toFloat(), table.center.y + (unit.y * ry).toFloat() + lift)
    }

    fun toward(from: Offset, amount: Float): Offset = from + (centre - from) * amount

    fun cards(slot: Int, count: Int): Offset {
        val at = seat(slot, count)
        return if (slot == 0) Offset(at.x, at.y - avatar * 0.5f - myCard * 0.7f) else toward(at, 0.3f)
    }

    fun bet(slot: Int, count: Int): Offset {
        val at = seat(slot, count)
        return if (slot == 0) Offset(at.x + avatar * 1.25f, at.y - avatar * 0.95f) else toward(at, 0.52f)
    }

    fun puck(slot: Int, count: Int): Offset {
        val at = seat(slot, count)
        val near = toward(at, 0.36f)
        val side = Offset(-(centre.y - at.y), centre.x - at.x)
        val length = kotlin.math.sqrt(side.x * side.x + side.y * side.y).coerceAtLeast(1f)
        val offset = if (slot == 0) Offset(-avatar * 1.2f, -avatar * 0.85f) else side * (avatar * 0.55f / length)
        return if (slot == 0) at + offset else near + offset
    }

    companion object {
        fun of(width: Float, height: Float, top: Float, bottom: Float): TableGeometry {
            val landscape = width > height
            val areaTop = top
            val areaHeight = max(160f, height - top - bottom)
            val tw: Float
            val th: Float
            if (landscape) {
                th = areaHeight * 0.80f
                tw = min(width * 0.70f, th * 2.1f)
            } else {
                tw = min(width * 0.80f, areaHeight * 0.64f)
                th = min(areaHeight * 0.80f, tw * 1.7f)
            }
            val cx = width / 2
            val cy = areaTop + areaHeight * (if (landscape) 0.52f else 0.50f)
            val table = Rect(cx - tw / 2, cy - th / 2, cx + tw / 2, cy + th / 2)
            val rail = min(tw, th) * 0.075f
            val avatar = if (landscape) (th * 0.15f).coerceIn(34f, 52f) else (tw * 0.16f).coerceIn(36f, 54f)
            val board = if (landscape) min(tw * 0.075f, 50f) else min(tw * 0.135f, 54f)
            return TableGeometry(width, height, landscape, table, rail, avatar, avatar * 0.5f, avatar * 0.92f, board)
        }
    }
}

private const val TIMELINE_MS = 2200f

private fun local(timeline: Float, start: Float, duration: Float): Float {
    val ms = timeline * TIMELINE_MS
    val t = ((ms - start) / duration).coerceIn(0f, 1f)
    return FastOutSlowInEasing.transform(t)
}

private fun lerp(a: Offset, b: Offset, t: Float): Offset = a + (b - a) * t

@Composable
private fun PokerTableView(model: PokerModel, tableId: String, onBack: () -> Unit) {
    val insets = WindowInsets.systemBars.union(WindowInsets.displayCutout).asPaddingValues()
    val logo: ImageBitmap = ImageBitmap.imageResource(R.drawable.fm_logo)
    Box(Modifier.fillMaxSize().background(PokerColors.room)) {
        BoxWithConstraints(Modifier.fillMaxSize().padding(insets)) {
            val w = maxWidth.value
            val h = maxHeight.value
            val landscape = w > h
            val geometry = remember(w, h) { TableGeometry.of(w, h, if (landscape) 48f else 58f, if (landscape) 76f else 150f) }
            val density = LocalDensity.current.density
            Canvas(Modifier.fillMaxSize()) {
                drawRoom()
                val r = geometry.table
                drawTable(Rect(r.left * density, r.top * density, r.right * density, r.bottom * density), geometry.rail * density, logo)
                drawDealer(Offset(geometry.dealer.x * density, geometry.dealer.y * density), density * (if (landscape) 0.7f else 0.82f))
            }
            val table = model.table
            if (table != null && table.id == tableId) {
                TableLayer(model, table, geometry)
            } else {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    if (model.tableProblem.isNotEmpty()) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(32.dp).clip(RoundedCornerShape(16.dp)).background(PokerColors.plate).padding(20.dp)) {
                            Text(model.tableProblem, color = Color.White, textAlign = TextAlign.Center)
                            Spacer(Modifier.height(12.dp))
                            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                if (!model.closedTable) PillButton("Try again", Color(0xFF1565C0)) { model.refresh() }
                                PillButton("Back to the lobby", Color(0xFF424242), onClick = onBack)
                            }
                        }
                    } else {
                        FmCircularProgressIndicator()
                    }
                }
            }
            TopBar(model, table?.takeIf { it.id == tableId }, onBack, Modifier.align(Alignment.TopCenter))
            if (table != null && table.id == tableId) {
                ActionBar(model, table, landscape, Modifier.align(Alignment.BottomCenter))
            }
            NoticeLine(model, Modifier.align(Alignment.TopCenter).padding(top = 56.dp))
        }
    }
}

@Composable
private fun NoticeLine(model: PokerModel, modifier: Modifier) {
    val text = model.notice
    LaunchedEffect(text) {
        if (text != null) {
            kotlinx.coroutines.delay(3500)
            if (model.notice == text) model.notice = null
        }
    }
    AnimatedVisibility(text != null, modifier, enter = fadeIn(), exit = fadeOut()) {
        Text(
            text ?: "",
            color = Color.White,
            fontSize = 13.sp,
            modifier = Modifier.padding(horizontal = 24.dp).clip(RoundedCornerShape(12.dp)).background(Color(0xE6202326)).clickable { model.notice = null }.padding(horizontal = 14.dp, vertical = 8.dp),
        )
    }
}

@Composable
private fun TableLayer(model: PokerModel, table: PokerTable, g: TableGeometry) {
    val fx = model.fx
    val timeline = remember { Animatable(1f) }
    LaunchedEffect(fx.id) {
        if (fx.id > 0) {
            timeline.snapTo(0f)
            timeline.animateTo(1f, tween(TIMELINE_MS.toInt(), easing = LinearEasing))
        }
    }
    val t = timeline.value
    val count = table.seats.size.coerceAtLeast(1)
    val order = PokerLayout.order(count, table.mySeat)
    val slotOf = { seat: Int -> PokerLayout.slot(seat, count, table.mySeat) }
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val clockRunning = table.inHand && table.toAct >= 0 && table.actDeadline > 0
    LaunchedEffect(clockRunning, table.actDeadline) {
        while (clockRunning) {
            withFrameMillis { }
            now = System.currentTimeMillis()
            kotlinx.coroutines.delay(50)
        }
    }
    val remaining = PokerClock.remaining(table.actDeadline, table.now, model.tableReceivedAt, now)
    val best = remember(table.lastHand, table.handNumber, table.phase) {
        val last = table.lastHand
        if (table.showdown && last != null && last.number == table.handNumber) last.winners.flatMap { it.best }.map { it.code }.toSet() else emptySet()
    }
    val dealIn = table.seats.filter { it.inHand || it.folded }.sortedBy { (slotOf(it.index) - slotOf(table.dealer.coerceAtLeast(0)) - 1 + count) % count }
    val freshBoard = fx.board.sorted()

    Box(Modifier.fillMaxSize()) {
        DealerLine(table, g)
        PotView(table, g, t, fx)
        BoardView(table, g, t, freshBoard, fx.deal, best)
        WinnerBanner(table, g)

        if (table.dealer >= 0 && table.seats.getOrNull(table.dealer)?.empty == false) {
            val target = g.puck(slotOf(table.dealer), count)
            val x by animateFloatAsState(target.x, tween(600), label = "puckX")
            val y by animateFloatAsState(target.y, tween(600), label = "puckY")
            val size = g.avatar * 0.5f
            DealerPuck(size.dp, Modifier.offset((x - size / 2).dp, (y - size / 2).dp))
        }

        order.forEachIndexed { slot, seatIndex ->
            val seat = table.seats[seatIndex]
            val at = g.seat(slot, count)
            if (seat.empty) {
                EmptySeat(at, g, enabled = !table.seated) { model.sheet = PokerSheet.Sit(seat.index) }
                return@forEachIndexed
            }
            val betAt = g.bet(slot, count)
            if (seat.bet > 0) {
                val p = if (seat.index in fx.bets) local(t, 0f, 280f) else 1f
                BetView(seat.bet, lerp(at, betAt, p))
            }
            val cardsAt = g.cards(slot, count)
            val dealOrder = dealIn.indexOfFirst { it.index == seat.index }
            HoleCards(seat, slot == 0, cardsAt, g, t, if (fx.deal && dealOrder >= 0) dealOrder else -1, dealIn.size, best)
            SeatView(seat, at, g, table, remaining)
        }

        fx.folds.forEach { seatIndex ->
            val slot = slotOf(seatIndex)
            val p = local(t, 0f, 340f)
            if (p < 1f && slot != 0) {
                val from = g.cards(slot, count)
                val at = lerp(from, g.dealer, p)
                val w = g.holeCard
                Row(Modifier.offset((at.x - w).dp, (at.y - w * 0.7f).dp).graphicsLayer { alpha = 1f - p; rotationZ = p * 40f }) {
                    PokerCardView(PokerCard.BACK, w.dp)
                    PokerCardView(PokerCard.BACK, w.dp)
                }
            }
        }

        fx.sweep.forEach { (seatIndex, amount) ->
            val p = local(t, 0f, 420f)
            if (p < 1f) {
                val slot = slotOf(seatIndex)
                val at = lerp(g.bet(slot, count), g.pot, p)
                ChipStackView(amount, Modifier.offset((at.x - 11f).dp, (at.y - 30f).dp), chip = 18.dp, alpha = 1f - p * 0.3f)
            }
        }

        fx.payout.forEach { (seatIndex, amount) ->
            val p = local(t, 450f, 650f)
            if (p > 0f && p < 1f) {
                val slot = slotOf(seatIndex)
                val at = lerp(g.pot, g.seat(slot, count), p)
                ChipStackView(amount, Modifier.offset((at.x - 13f).dp, (at.y - 36f).dp), chip = 22.dp)
            }
        }
    }
}

@Composable
private fun DealerLine(table: PokerTable, g: TableGeometry) {
    val line = table.dealerLine.ifEmpty { if (table.phase == "waiting") "Waiting for players" else "" }
    if (line.isEmpty()) return
    val width = g.table.width * 0.8f
    Box(Modifier.offset((g.centre.x - width / 2).dp, (g.table.top + g.rail * 1.1f + 24f).dp).width(width.dp), contentAlignment = Alignment.Center) {
        Text(
            line,
            color = Color(0xFFF1E4B8),
            fontSize = 12.sp,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
            modifier = Modifier.clip(RoundedCornerShape(10.dp)).background(Color.Black.copy(alpha = 0.32f)).padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

@Composable
private fun PotView(table: PokerTable, g: TableGeometry, t: Float, fx: PokerFx) {
    val shown = if (fx.sweep.isNotEmpty() && local(t, 0f, 420f) < 1f) table.pottedChips - fx.sweep.values.sum() else table.pottedChips
    val amount = if (table.showdown && fx.payout.isNotEmpty() && local(t, 450f, 650f) >= 1f) 0 else shown.coerceAtLeast(0)
    val total = table.potTotal
    if (total <= 0 && amount <= 0) return
    val at = g.pot
    Column(Modifier.offset((at.x - 60f).dp, (at.y - 46f).dp).width(120.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        if (amount > 0) ChipStackView(amount, chip = 20.dp, maxChips = 9)
        Text(
            "Pot ${PokerChips.format(total)}",
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.clip(RoundedCornerShape(10.dp)).background(Color.Black.copy(alpha = 0.4f)).padding(horizontal = 10.dp, vertical = 2.dp),
        )
        if (table.pots.size > 1) {
            Text(table.pots.drop(1).joinToString("  ") { "Side ${PokerChips.short(it.amount)}" }, color = Color.White.copy(alpha = 0.75f), fontSize = 10.sp)
        }
    }
}

@Composable
private fun BoardView(table: PokerTable, g: TableGeometry, t: Float, fresh: List<Int>, dealt: Boolean, best: Set<String>) {
    val w = g.boardCard
    val gap = w * 0.12f
    val total = w * 5 + gap * 4
    val left = g.centre.x - total / 2
    val top = g.centre.y - w * 0.7f
    for (i in 0 until 5) {
        val x = left + i * (w + gap)
        val card = table.board.getOrNull(i)
        if (card == null) {
            CardSlot(w.dp, Modifier.offset(x.dp, top.dp))
            continue
        }
        val order = fresh.indexOf(i)
        val start = (if (dealt) 600f else 0f) + order * 160f
        val p = if (order >= 0) local(t, start, 300f) else 1f
        val flip = if (order >= 0) local(t, start + 200f, 260f) else 1f
        val from = Offset(g.dealer.x - w / 2, g.dealer.y + 30f)
        val at = lerp(from, Offset(x, top), p)
        PokerCardView(card, w.dp, Modifier.offset(at.x.dp, at.y.dp).semantics { contentDescription = PokerCards.spoken(card) }, faceUp = flip, lifted = card.code in best)
    }
}

@Composable
private fun WinnerBanner(table: PokerTable, g: TableGeometry) {
    val winners = if (table.showdown) table.seats.filter { it.winner } else emptyList()
    val text = winners.joinToString("\n") { seat ->
        val hand = seat.handName.ifEmpty { table.lastHand?.winners?.firstOrNull { it.seat == seat.index }?.handName.orEmpty() }
        "${seat.displayName} wins ${PokerChips.format(seat.won)}" + if (hand.isNotEmpty()) " · $hand" else ""
    }
    val width = g.table.width * 0.9f
    AnimatedVisibility(
        winners.isNotEmpty(),
        Modifier.offset((g.centre.x - width / 2).dp, (g.centre.y + g.boardCard * 0.95f).dp).width(width.dp),
        enter = fadeIn() + scaleIn(initialScale = 0.7f),
        exit = fadeOut() + scaleOut(targetScale = 0.9f),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text,
                color = PokerColors.goldBright,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color.Black.copy(alpha = 0.55f))
                    .border(1.dp, PokerColors.gold, RoundedCornerShape(12.dp))
                    .padding(horizontal = 12.dp, vertical = 6.dp),
            )
        }
    }
}

@Composable
private fun EmptySeat(at: Offset, g: TableGeometry, enabled: Boolean, onSit: () -> Unit) {
    val size = g.avatar
    Box(
        Modifier
            .offset((at.x - size / 2).dp, (at.y - size / 2).dp)
            .size(size.dp)
            .clip(CircleShape)
            .background(Color.Black.copy(alpha = 0.35f))
            .border(1.5.dp, Color.White.copy(alpha = if (enabled) 0.45f else 0.18f), CircleShape)
            .clickable(enabled = enabled, onClick = onSit),
        contentAlignment = Alignment.Center,
    ) {
        Text(if (enabled) "Sit" else "Open", color = Color.White.copy(alpha = if (enabled) 0.85f else 0.4f), fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
private fun BetView(amount: Long, at: Offset) {
    Row(Modifier.offset((at.x - 12f).dp, (at.y - 22f).dp), verticalAlignment = Alignment.Bottom) {
        ChipStackView(amount, chip = 16.dp, maxChips = 6)
        Text(
            PokerChips.short(amount),
            color = Color.White,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(start = 2.dp, bottom = 2.dp).clip(RoundedCornerShape(6.dp)).background(Color.Black.copy(alpha = 0.45f)).padding(horizontal = 4.dp),
        )
    }
}

@Composable
private fun HoleCards(seat: PokerSeat, mine: Boolean, at: Offset, g: TableGeometry, t: Float, dealOrder: Int, dealing: Int, best: Set<String>) {
    if (seat.cards.isEmpty()) return
    val w = if (mine) g.myCard else g.holeCard
    val overlap = if (mine) w * 0.08f else -w * 0.35f
    val total = w * seat.cards.size + overlap * (seat.cards.size - 1)
    seat.cards.forEachIndexed { i, card ->
        val start = if (dealOrder >= 0) (i * dealing + dealOrder) * 90f else 0f
        val p = if (dealOrder >= 0) local(t, start, 260f) else 1f
        val flip = if (dealOrder >= 0 && mine) local(t, start + 240f, 240f) else 1f
        val final = Offset(at.x - total / 2 + i * (w + overlap), at.y - w * 0.7f)
        val pos = lerp(Offset(g.dealer.x - w / 2, g.dealer.y + 30f), final, p)
        val tilt = if (mine) (i - (seat.cards.size - 1) / 2f) * 6f else (i - (seat.cards.size - 1) / 2f) * 10f
        PokerCardView(
            card,
            w.dp,
            Modifier.offset(pos.x.dp, pos.y.dp).graphicsLayer { rotationZ = tilt * p }.semantics { contentDescription = PokerCards.spoken(card) },
            faceUp = flip,
            lifted = card.code in best && seat.winner,
            dim = seat.folded,
        )
    }
}

@Composable
private fun SeatView(seat: PokerSeat, at: Offset, g: TableGeometry, table: PokerTable, remaining: Long) {
    val size = g.avatar
    val plateWidth = max(size * 1.75f, 74f)
    val glow = if (seat.winner && table.showdown) {
        val pulse = rememberInfiniteTransition(label = "glow")
        pulse.animateFloat(0.45f, 1f, infiniteRepeatable(tween(700), RepeatMode.Reverse), label = "glowAlpha").value
    } else {
        0f
    }
    val ringFraction = if (seat.isTurn && table.inHand) PokerClock.fraction(table.actStartedAt, table.actDeadline, remaining).toFloat() else 0f
    val ringTone = PokerClock.tone(remaining)
    Box(Modifier.offset((at.x - plateWidth / 2).dp, (at.y - size / 2 - 18f).dp).width(plateWidth.dp)) {
        Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
            Box(Modifier.height(16.dp), contentAlignment = Alignment.Center) {
                val tag = when {
                    seat.away -> "Sitting out"
                    seat.status == "leaving" -> "Leaving"
                    seat.busted -> "Busted"
                    else -> seat.lastAction?.label.orEmpty()
                }
                if (tag.isNotEmpty()) {
                    Text(tag, color = Color.White, fontSize = 9.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, modifier = Modifier.clip(RoundedCornerShape(6.dp)).background(tagColor(seat)).padding(horizontal = 6.dp, vertical = 1.dp))
                }
            }
            Spacer(Modifier.height(2.dp))
            Box(
                Modifier
                    .size((size + 8f).dp)
                    .drawBehind {
                        if (glow > 0f) {
                            drawCircle(Brush.radialGradient(listOf(PokerColors.goldBright.copy(alpha = 0.85f * glow), Color.Transparent), radius = this.size.minDimension * 0.85f), radius = this.size.minDimension * 0.85f)
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .size(size.dp)
                        .clip(CircleShape)
                        .background(PokerColors.avatar(seat.name))
                        .border(if (seat.winner && table.showdown) 2.dp else 1.dp, if (seat.winner && table.showdown) PokerColors.goldBright else Color.White.copy(alpha = 0.6f), CircleShape)
                        .graphicsLayer { alpha = if (seat.folded || seat.away) 0.5f else 1f },
                    contentAlignment = Alignment.Center,
                ) {
                    Text(seat.initial, color = Color.White, fontSize = (size * 0.42f).sp, fontWeight = FontWeight.Bold)
                }
                if (ringFraction > 0f) {
                    Canvas(Modifier.size((size + 8f).dp)) { drawTurnRing(ringFraction, ringTone, 3.5f * density) }
                }
                if (seat.bot) {
                    Box(Modifier.align(Alignment.BottomEnd).size(16.dp).clip(CircleShape).background(Color(0xFF263238)).border(1.dp, Color.White.copy(alpha = 0.6f), CircleShape), contentAlignment = Alignment.Center) {
                        FmIcon("smart_toy", size = 11.dp, tint = Color.White)
                    }
                }
            }
            Column(
                Modifier
                    .offset(y = (-6).dp)
                    .widthIn(min = (size * 1.2f).dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(PokerColors.plate)
                    .border(1.dp, if (seat.isTurn && table.inHand) PokerColors.clock(ringTone) else Color.White.copy(alpha = 0.12f), RoundedCornerShape(8.dp))
                    .padding(horizontal = 6.dp, vertical = 1.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(seat.displayName, color = Color.White, fontSize = 10.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(PokerChips.format(seat.stack), color = Color(0xFFFFE082), fontSize = 11.sp, fontWeight = FontWeight.Bold, maxLines = 1)
            }
        }
    }
}

private fun tagColor(seat: PokerSeat): Color = when (seat.lastAction?.kind) {
    "fold" -> Color(0xE6616161)
    "allin" -> Color(0xE6C62828)
    "raise", "bet" -> Color(0xE6EF6C00)
    "call" -> Color(0xE61565C0)
    "check" -> Color(0xE62E7D32)
    else -> PokerColors.tag
}

@Composable
private fun TopBar(model: PokerModel, table: PokerTable?, onBack: () -> Unit, modifier: Modifier) {
    Row(modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        RoundIcon("arrow_back", "Back to the lobby", onBack)
        Column(Modifier.weight(1f).padding(horizontal = 10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Text(table?.name?.ifEmpty { "Poker" } ?: "Poker", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (table != null) {
                val hand = if (table.handNumber > 0) " · Hand #${table.handNumber}" else ""
                Text("Blinds ${table.blinds}$hand · ${PokerPhase.label(table.phase)}", color = Color.White.copy(alpha = 0.65f), fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (table != null) {
            val unread = (table.chat.size - model.chatSeen).coerceAtLeast(0)
            Box {
                RoundIcon("chat", "Table chat") {
                    model.chatSeen = table.chat.size
                    model.sheet = PokerSheet.Chat
                }
                if (unread > 0) {
                    Box(Modifier.align(Alignment.TopEnd).size(16.dp).clip(CircleShape).background(Color(0xFFE53935)), contentAlignment = Alignment.Center) {
                        Text(if (unread > 9) "9+" else unread.toString(), color = Color.White, fontSize = 9.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
            Spacer(Modifier.width(6.dp))
            RoundIcon("more_horiz", "Table menu") { model.sheet = PokerSheet.Menu }
        }
    }
}

@Composable
internal fun RoundIcon(icon: String, label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .size(38.dp)
            .clip(CircleShape)
            .background(Color.White.copy(alpha = 0.12f))
            .border(0.5.dp, Color.White.copy(alpha = 0.25f), CircleShape)
            .clickable(onClick = onClick)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = 20.dp, tint = Color.White)
    }
}

@Composable
internal fun PillButton(label: String, color: Color, enabled: Boolean = true, modifier: Modifier = Modifier, height: Dp = 40.dp, onClick: () -> Unit) {
    Box(
        modifier
            .height(height)
            .clip(RoundedCornerShape(height / 2))
            .background(Brush.verticalGradient(listOf(color.copy(alpha = if (enabled) 1f else 0.35f), color.copy(alpha = if (enabled) 0.78f else 0.3f))))
            .border(0.5.dp, Color.White.copy(alpha = 0.25f), RoundedCornerShape(height / 2))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = Color.White.copy(alpha = if (enabled) 1f else 0.6f), fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun ActionBar(model: PokerModel, table: PokerTable, landscape: Boolean, modifier: Modifier) {
    val shape = RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp)
    Column(
        modifier
            .fillMaxWidth()
            .then(if (landscape) Modifier.padding(horizontal = 48.dp) else Modifier)
            .clip(shape)
            .background(Brush.verticalGradient(listOf(Color(0xE61A1D21), Color(0xF20E1013))))
            .border(0.5.dp, Color.White.copy(alpha = 0.14f), shape)
            .padding(horizontal = 12.dp, vertical = if (landscape) 6.dp else 10.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        val me = table.me
        val hand = table.myHand?.name.orEmpty()
        if (hand.isNotEmpty() || (!landscape && table.seated)) {
            Text(
                if (hand.isNotEmpty()) hand else if (table.inHand) "" else PokerPhase.label(table.phase),
                color = Color(0xFFFFE082),
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center,
                maxLines = 1,
            )
        }
        when {
            table.myTurn && model.raising -> RaisePanel(model, table)
            table.myTurn -> TurnButtons(model, table)
            me != null -> SeatedBar(model, table, me)
            else -> WatcherBar(table)
        }
    }
}

@Composable
private fun TurnButtons(model: PokerModel, table: PokerTable) {
    val buttons = PokerActions.buttons(table)
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        buttons.forEach { button ->
            val color = when (button.kind) {
                PokerActionKind.FOLD -> Color(0xFF8E2424)
                PokerActionKind.CHECK, PokerActionKind.CALL -> Color(0xFF1565C0)
                PokerActionKind.BET, PokerActionKind.RAISE -> Color(0xFF2E7D32)
                PokerActionKind.ALLIN -> Color(0xFFB26A00)
            }
            PillButton(button.label, color, enabled = !model.busy, modifier = Modifier.weight(1f), height = 46.dp) {
                when (button.kind) {
                    PokerActionKind.BET, PokerActionKind.RAISE -> {
                        val raise = table.legal?.raise
                        model.raiseAmount = PokerActions.clamp(model.raiseAmount, raise?.min ?: 0, raise?.max ?: 0)
                        model.raising = true
                    }
                    else -> model.act(button.kind, button.amount)
                }
            }
        }
    }
}

@Composable
private fun RaisePanel(model: PokerModel, table: PokerTable) {
    val raise = table.legal?.raise ?: return
    val amount = PokerActions.clamp(model.raiseAmount, raise.min, raise.max)
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            PokerActions.presets(table).forEach { preset ->
                val on = preset.amount == amount
                Box(
                    Modifier
                        .weight(1f)
                        .height(32.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(if (on) Color(0xFFF9A825) else Color.White.copy(alpha = 0.1f))
                        .clickable { model.raiseAmount = preset.amount },
                    contentAlignment = Alignment.Center,
                ) {
                    Text(preset.label, color = if (on) Color(0xFF1A1A1A) else Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }
        ChipSlider(amount, raise.min, raise.max, PokerActions.step(table)) { model.raiseAmount = it }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PillButton("Cancel", Color(0xFF424242), modifier = Modifier.weight(1f), height = 44.dp) { model.raising = false }
            PillButton(PokerActions.raiseLabel(table, amount), Color(0xFF2E7D32), enabled = !model.busy, modifier = Modifier.weight(2f), height = 44.dp) {
                val kind = PokerActions.wireFor(table, amount)
                model.act(kind, if (kind == PokerActionKind.ALLIN) 0 else amount)
            }
        }
    }
}

@Composable
private fun SeatedBar(model: PokerModel, table: PokerTable, me: PokerSeat) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        val status = when {
            me.away -> "You are sitting out."
            me.busted -> "You are out of chips."
            me.folded -> "You folded. Next hand soon."
            me.inHand -> "Waiting for ${table.seats.getOrNull(table.toAct)?.displayName ?: "the table"}…"
            table.inHand -> "You join the next hand."
            table.occupied < 2 -> "Waiting for another player. The owner can add bots."
            else -> "The next hand starts soon."
        }
        Text(status, color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, modifier = Modifier.weight(1f), maxLines = 2)
        if (me.busted || (me.stack + me.rebuy < table.maxBuyIn && table.wallet > 0 && me.stack < table.minBuyIn)) {
            PillButton("Rebuy", Color(0xFFB26A00), enabled = !model.busy) { model.sheet = PokerSheet.Rebuy }
        }
        if (me.away) {
            PillButton("I'm back", Color(0xFF2E7D32), enabled = !model.busy) { model.away(false) }
        } else {
            PillButton("Sit out", Color(0xFF424242), enabled = !model.busy) { model.away(true) }
        }
    }
}

@Composable
private fun WatcherBar(table: PokerTable) {
    Row(Modifier.fillMaxWidth().height(40.dp), verticalAlignment = Alignment.CenterVertically) {
        val text = if (table.hasEmptySeat) {
            "Watching. Tap an open seat to sit down (buy-in ${PokerChips.format(table.minBuyIn)} – ${PokerChips.format(table.maxBuyIn)})."
        } else {
            "Watching. The table is full."
        }
        Text(text, color = Color.White.copy(alpha = 0.85f), fontSize = 13.sp, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
    }
}
