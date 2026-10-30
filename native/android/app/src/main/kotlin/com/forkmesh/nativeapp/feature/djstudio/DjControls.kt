package com.forkmesh.nativeapp.feature.djstudio

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableLongState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjFocus
import com.forkmesh.nativeapp.djstudio.DjSampleStep
import com.forkmesh.nativeapp.djstudio.DjSmartKind
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjWaveBar
import com.forkmesh.nativeapp.ui.kit.FmActionMenu
import com.forkmesh.nativeapp.ui.kit.FmActionMenuButton
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

@Composable
internal fun DjText(text: String, size: Float, color: Color = DjTheme.ink, weight: FontWeight = FontWeight.Normal, align: TextAlign? = null, modifier: Modifier = Modifier, mono: Boolean = false) {
    Text(
        text, modifier = modifier, color = color, fontSize = size.sp, fontWeight = weight, maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = align,
        style = if (mono) androidx.compose.ui.text.TextStyle(fontFeatureSettings = "tnum") else androidx.compose.ui.text.TextStyle.Default,
    )
}

@Composable
internal fun DjBackButton(model: DjStudioModel, size: Dp) {
    DjRoundButton(size, "chevron_left", "Back, playing decks keep playing", onTap = { model.leave() })
}

@Composable
internal fun DjLibraryHandle(model: DjStudioModel, side: DjDeckSide, u: Float) {
    Column(
        Modifier.width((40 * u).dp).fillMaxHeight().padding(top = (4 * u).dp).djPress("Load Deck ${side.label}") { model.openLoad(side) },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy((6 * u).dp),
    ) {
        Box(Modifier.size((34 * u).dp, (9 * u).dp).djControl(RoundedCornerShape((2 * u).dp)))
        repeat(4) {
            Row(horizontalArrangement = Arrangement.spacedBy((4 * u).dp)) {
                repeat(2) { Box(Modifier.size((3.5f * u).dp).clip(CircleShape).background(Color.Black.copy(alpha = 0.55f))) }
            }
        }
    }
}

@Composable
internal fun DjDeckInfo(model: DjStudioModel, deck: DjDeck, mirrored: Boolean, u: Float, clock: MutableLongState, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((12 * u).dp)) {
        if (mirrored) {
            DjTitles(model, deck, true, u, Modifier.weight(1f, false))
            DjRepeatButton(model, deck, u)
            DjClock(deck, u, clock)
        } else {
            DjClock(deck, u, clock)
            DjRepeatButton(model, deck, u)
            DjTitles(model, deck, false, u, Modifier.weight(1f, false))
        }
    }
}

@Composable
private fun DjTitles(model: DjStudioModel, deck: DjDeck, mirrored: Boolean, u: Float, modifier: Modifier) {
    Column(
        modifier.widthIn(max = (220 * u).dp).djPress("Load Deck ${deck.side.label}") { model.openLoad(deck.side) },
        horizontalAlignment = if (mirrored) Alignment.End else Alignment.Start,
    ) {
        DjText(if (deck.loaded) deck.title.ifEmpty { "Loading…" } else "Deck ${deck.side.label}", 15 * u, DjTheme.ink.copy(alpha = 0.92f))
        val detail = when {
            !deck.loaded -> "Tap to load a video"
            deck.loading -> "Fetching the audio…"
            else -> deck.author.ifEmpty { "YouTube" }
        }
        DjText(detail, 13 * u, DjTheme.muted)
    }
}

@Composable
private fun DjClock(deck: DjDeck, u: Float, clock: MutableLongState) {
    Row(
        Modifier
            .djPress("Deck ${deck.side.label} time") { deck.showsRemaining = !deck.showsRemaining },
        verticalAlignment = Alignment.Bottom,
        horizontalArrangement = Arrangement.spacedBy((4 * u).dp),
    ) {
        clock.longValue
        deck.stoppedAt
        val position = deck.position()
        DjText(if (deck.showsRemaining) "-" + DjStudio.clock(max(0.0, deck.duration - position)) else DjStudio.clock(position), 30 * u, weight = FontWeight.Light, mono = true)
        DjText("/ " + if (deck.duration > 0) DjStudio.clock(deck.duration) else "--:--", 14 * u, DjTheme.muted, mono = true, modifier = Modifier.alpha(if (deck.loaded) 1f else 0.5f).padding(bottom = (4 * u).dp))
    }
}

internal const val WAVE_WINDOW = 16.0

@Composable
internal fun DjWaveform(model: DjStudioModel, deck: DjDeck, clock: MutableLongState, modifier: Modifier) {
    var dragX by remember { mutableStateOf(0f) }
    var dragging by remember { mutableStateOf(false) }
    Box(modifier) {
        Canvas(
            Modifier
                .fillMaxSize()
                .semantics { contentDescription = "Deck ${deck.side.label} waveform" }
                .pointerInput(deck.side) {
                    detectDragGestures(
                        onDragStart = { dragging = true },
                        onDragEnd = {
                            if (deck.duration > 0 && size.width > 0) model.seek(deck.side, deck.position() - dragX / size.width * WAVE_WINDOW)
                            dragX = 0f
                            dragging = false
                        },
                        onDragCancel = {
                            dragX = 0f
                            dragging = false
                        },
                    ) { change, amount ->
                        change.consume()
                        if (deck.duration > 0) dragX += amount.x
                    }
                }
                .pointerInput(deck.side) { detectTapGestures { if (!deck.loaded) model.openLoad(deck.side) } },
        ) {
            clock.longValue
            deck.stoppedAt
            val time = deck.position() - (if (dragging) dragX / size.width else 0f) * WAVE_WINDOW
            drawWave(deck.waveform, time, deck.duration)
        }
        if (deck.loaded && deck.waveform.isEmpty() && (deck.reading || deck.loading)) {
            DjText(
                if (deck.loading) "Fetching the audio…" else "Reading the audio…", 10f, DjTheme.muted, FontWeight.Medium,
                modifier = Modifier.align(Alignment.Center).clip(CircleShape).background(DjTheme.panel.copy(alpha = 0.85f)).padding(horizontal = 6.dp),
            )
        }
    }
}

internal fun DrawScope.drawWave(bars: List<DjWaveBar>, time: Double, duration: Double) {
    val width = size.width
    val height = size.height
    drawRect(Color.White.copy(alpha = 0.22f), Offset(0f, height / 2 - 0.5f), Size(width, 1f))
    if (bars.isNotEmpty() && duration > 0 && width > 0) {
        val step = 3.dp.toPx()
        val perPoint = WAVE_WINDOW / width
        val first = time - WAVE_WINDOW / 2
        val offset = ((first / perPoint) % step).toFloat()
        var x = -offset
        while (x < width) {
            val moment = first + x * perPoint
            if (moment in 0.0..duration) {
                val bar = bars[min(bars.size - 1, (moment / duration * bars.size).toInt())]
                val h = max(1f, (bar.level * height * 0.92).toFloat())
                val bucket = min(DjTheme.waveColors.size - 1, max(0, (bar.hue * DjTheme.waveColors.size).toInt()))
                drawRect(DjTheme.waveColors[bucket], Offset(x, (height - h) / 2), Size(step * 0.7f, h))
            }
            x += step
        }
    }
    drawRect(DjTheme.playhead, Offset(width / 2 - 0.75.dp.toPx(), 0f), Size(1.5.dp.toPx(), height))
}

@Composable
internal fun DjTransport(model: DjStudioModel, deck: DjDeck, u: Float, vertical: Boolean, cellWidth: Float = 58f, cellHeight: Float = 32f) {
    val atStart = !deck.playing && deck.stoppedAt < 0.05
    val items = listOf(
        Triple("START", "play_arrow", DjTheme.play) to deck.playing,
        Triple("PAUSE", "pause", DjTheme.ink) to (deck.loaded && !deck.playing && !atStart),
        Triple("STOP", "stop", DjTheme.record) to (deck.loaded && !deck.playing && atStart),
        Triple("RESTART", "skip_previous", DjTheme.active) to false,
    )
    val actions = listOf<() -> Unit>({ model.start(deck.side) }, { model.pauseDeck(deck.side) }, { model.stop(deck.side) }, { model.restart(deck.side) })
    val labels = listOf("Start", "Pause", "Stop and go back to the beginning of", "Start over")
    val content: @Composable () -> Unit = {
        items.forEachIndexed { index, (look, lit) ->
            if (index > 0) Box(if (vertical) Modifier.width((cellWidth * u).dp).height(1.dp).background(Color.Black.copy(alpha = 0.55f)) else Modifier.width(1.dp).height((cellHeight * u).dp).background(Color.Black.copy(alpha = 0.55f)))
            val (title, icon, tint) = look
            Column(
                Modifier
                    .size((cellWidth * u).dp, (cellHeight * u).dp)
                    .background(tint.copy(alpha = if (lit) 0.16f else 0f))
                    .djPress("${labels[index]} Deck ${deck.side.label}", onTap = actions[index]),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                FmIcon(icon, size = (14 * u).dp, tint = tint.copy(alpha = if (deck.loaded) 1f else 0.45f))
                DjText(title, 8 * u, DjTheme.ink.copy(alpha = if (deck.loaded) 0.9f else 0.5f), FontWeight.Bold)
            }
        }
    }
    val shape = RoundedCornerShape((4 * u).dp)
    if (vertical) Column(Modifier.djControl(shape)) { content() } else Row(Modifier.djControl(shape)) { content() }
}

@Composable
internal fun DjCueButtons(model: DjStudioModel, deck: DjDeck, u: Float, vertical: Boolean, segment: Float = 58f) {
    val (title, tint, lit, label) = when (deck.sample) {
        DjSampleStep.Idle -> listOf("SAMPLE", DjTheme.muted, false, "Start sampling Deck ${deck.side.label}")
        is DjSampleStep.Recording -> listOf("REC", DjTheme.record, true, "Stop sampling Deck ${deck.side.label}")
        is DjSampleStep.Armed -> listOf("ARMED", DjTheme.active, true, "Play Deck ${deck.side.label}'s sample")
    }
    title as String
    tint as Color
    lit as Boolean
    label as String
    val ink = if (deck.loaded) DjTheme.ink else DjTheme.muted
    val divider: @Composable () -> Unit = {
        Box(if (vertical) Modifier.width((segment * u).dp).height(1.dp).background(Color.Black.copy(alpha = 0.55f)) else Modifier.width(1.dp).height((34 * u).dp).background(Color.Black.copy(alpha = 0.55f)))
    }
    val cell = Modifier.size((segment * u).dp, (34 * u).dp)
    val content: @Composable () -> Unit = {
        Row(cell.djPress("Set cue on Deck ${deck.side.label}") { model.setCue(deck.side) }, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
            DjText("SET", 12 * u, ink, FontWeight.Bold)
            Spacer(Modifier.width((4 * u).dp))
            Box(Modifier.size((8 * u).dp).clip(CircleShape).background(ink))
        }
        divider()
        Box(cell.djPress("Jump to Deck ${deck.side.label} cue") { model.jumpToCue(deck.side) }, contentAlignment = Alignment.Center) {
            FmIcon("replay", size = (18 * u).dp, tint = ink)
        }
        divider()
        Row(
            cell.background(tint.copy(alpha = if (lit) 0.3f else 0f)).djPress(label) { model.sample(deck.side) },
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center,
        ) {
            Box(Modifier.size((7 * u).dp).clip(CircleShape).background(tint.copy(alpha = if (lit) 1f else 0.35f)))
            Spacer(Modifier.width((3 * u).dp))
            DjText(title, 10 * u, ink, FontWeight.Bold)
        }
    }
    val shape = RoundedCornerShape((4 * u).dp)
    if (vertical) Column(Modifier.djControl(shape)) { content() } else Row(Modifier.djControl(shape)) { content() }
}

@Composable
internal fun DjPitchStrip(model: DjStudioModel, deck: DjDeck, u: Float, modifier: Modifier) {
    var bpmMenu by remember { mutableStateOf(false) }
    Column(modifier.width((58 * u).dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy((6 * u).dp)) {
        DjRoundButton((30 * u).dp, "tune", "Deck ${deck.side.label} settings") { model.sheet = DjSheet.Deck(deck.side) }
        Column(
            Modifier.weight(1f).fillMaxWidth().djPanel(RoundedCornerShape((5 * u).dp)).padding((6 * u).dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy((6 * u).dp),
        ) {
            val changed = abs(deck.effectiveRate - 1) > 0.0005
            val text = if (deck.bpm > 0) "%.1f".format(deck.shownBpm) else if (changed) "%+.1f%%".format((deck.effectiveRate - 1) * 100) else "TAP"
            Box {
                Box(
                    Modifier.fillMaxWidth().heightIn(min = (28 * u).dp).clip(RoundedCornerShape((3 * u).dp)).background(Color.Black.copy(alpha = 0.35f))
                        .djPress(if (deck.bpm > 0) "$text BPM, tap to retap" else "Tap tempo", onLongPress = { bpmMenu = true }) { model.tapTempo(deck.side) },
                    contentAlignment = Alignment.Center,
                ) {
                    DjText(text, (if (deck.bpm > 0) 15 else 13) * u, if (deck.bpm > 0 || changed) DjTheme.ink else DjTheme.muted, mono = true)
                }
                FmActionMenu(bpmMenu, { bpmMenu = false }) {
                    FmActionMenuButton("Detect BPM", { bpmMenu = false; model.detectBpm(deck.side) })
                    FmActionMenuButton("Clear BPM", { bpmMenu = false; model.clearBpm(deck.side) })
                }
            }
            DjSmallButton("SYNC", u, true, "Sync Deck ${deck.side.label} to the other deck") { model.sync(deck.side) }
            DjPitchFader(model, deck, u, Modifier.weight(1f).fillMaxWidth())
            DjSmallButton("RESET", u, abs(deck.rate - 1) > 0.0005, "Reset Deck ${deck.side.label} tempo to the middle") { model.setRate(deck.side, 1.0) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy((4 * u).dp)) {
            DjHoldButton("remove", u, "Slow Deck ${deck.side.label} while held") { model.bend(deck.side, if (it) -1.0 else 0.0) }
            DjHoldButton("add", u, "Speed up Deck ${deck.side.label} while held") { model.bend(deck.side, if (it) 1.0 else 0.0) }
        }
    }
}

@Composable
private fun DjSmallButton(title: String, u: Float, bright: Boolean, label: String, onTap: () -> Unit) {
    Box(
        Modifier.fillMaxWidth().heightIn(min = (22 * u).dp).djControl(RoundedCornerShape((3 * u).dp)).djPress(label, onTap = onTap),
        contentAlignment = Alignment.Center,
    ) {
        DjText(title, 10.5f * u, if (bright) DjTheme.ink.copy(alpha = 0.9f) else DjTheme.muted, FontWeight.Bold)
    }
}

@Composable
private fun DjHoldButton(icon: String, u: Float, label: String, changed: (Boolean) -> Unit) {
    var held by remember { mutableStateOf(false) }
    Box(
        Modifier.size((26 * u).dp).semantics { contentDescription = label }.pointerInput(Unit) {
            awaitEachGesture {
                awaitFirstDown()
                held = true
                changed(true)
                do {
                    val event = awaitPointerEvent()
                } while (event.changes.any { it.pressed })
                held = false
                changed(false)
            }
        },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = (18 * u).dp, tint = if (held) DjTheme.active else DjTheme.muted)
    }
}

internal fun DrawScope.drawKnob(topLeft: Offset, knob: Size, horizontal: Boolean) {
    val radius = min(knob.width, knob.height) * 0.22f
    drawRoundRect(Color.Black.copy(alpha = 0.35f), topLeft + Offset(0f, 2.dp.toPx()), knob, CornerRadius(radius))
    drawRoundRect(
        if (horizontal) Brush.horizontalGradient(listOf(DjTheme.knobTop, DjTheme.knobBottom), topLeft.x, topLeft.x + knob.width)
        else Brush.verticalGradient(listOf(DjTheme.knobTop, DjTheme.knobBottom), topLeft.y, topLeft.y + knob.height),
        topLeft, knob, CornerRadius(radius),
    )
    if (horizontal) drawRect(DjTheme.knobLine, Offset(topLeft.x + knob.width / 2 - 1f, topLeft.y + knob.height * 0.04f), Size(2f, knob.height * 0.92f))
    else drawRect(DjTheme.knobLine, Offset(topLeft.x + knob.width * 0.04f, topLeft.y + knob.height / 2 - 1f), Size(knob.width * 0.92f, 2f))
}

@Composable
private fun DjPitchFader(model: DjStudioModel, deck: DjDeck, u: Float, modifier: Modifier) {
    Canvas(
        modifier
            .semantics {
                contentDescription = "Deck ${deck.side.label} tempo"
                stateDescription = "%+.1f percent".format((deck.rate - 1) * 100)
            }
            .pointerInput(deck.side) { detectTapGestures(onDoubleTap = { model.setRate(deck.side, 1.0) }) }
            .pointerInput(deck.side) {
                var start = 1.0
                var moved = 0f
                detectDragGestures(onDragStart = {
                    start = deck.rate
                    moved = 0f
                }) { change, amount ->
                    change.consume()
                    moved += amount.y
                    val knob = 22 * u * density
                    val travel = max(1f, size.height - knob)
                    model.setRate(deck.side, start - moved / travel * 2 * DjStudio.PITCH_RANGE)
                }
            },
    ) {
        val knob = Size(min(size.width, 40 * u * density), 22 * u * density)
        val travel = max(1f, size.height - knob.height)
        val fraction = ((deck.rate - (1 - DjStudio.PITCH_RANGE)) / (2 * DjStudio.PITCH_RANGE)).toFloat()
        val centre = size.width / 2
        for (index in 0..8) {
            val width = (if (index == 4) 26 else 18) * u * density
            drawRect(Color.White.copy(alpha = if (index == 4) 0.35f else 0.12f), Offset(centre - width / 2, knob.height / 2 + travel * index / 8), Size(width, 1f))
        }
        drawRoundRect(Color.Black.copy(alpha = 0.7f), Offset(centre - 3 * u * density, knob.height / 2), Size(6 * u * density, travel), CornerRadius(3 * u * density))
        drawCircle(DjTheme.play, 1.5f * u * density, Offset(centre - 17 * u * density, knob.height / 2 + travel / 2))
        val y = knob.height / 2 + (1 - fraction) * travel
        drawKnob(Offset(centre - knob.width / 2, y - knob.height / 2), knob, false)
    }
}

@Composable
internal fun DjLevelFader(model: DjStudioModel, deck: DjDeck, u: Float, modifier: Modifier) {
    Column(modifier.djPanel(RoundedCornerShape((5 * u).dp)).padding(vertical = (4 * u).dp), horizontalAlignment = Alignment.CenterHorizontally) {
        FmIcon(if (deck.level < 0.01) "volume_off" else if (deck.level < 0.5) "volume_down" else "volume_up", size = (13 * u).dp, tint = DjTheme.muted)
        Canvas(
            Modifier.weight(1f).fillMaxWidth().padding(vertical = (4 * u).dp)
                .semantics {
                    contentDescription = "Deck ${deck.side.label} volume"
                    stateDescription = "${(deck.level * 100).roundToInt()} percent"
                }
                .pointerInput(deck.side) { detectTapGestures(onDoubleTap = { model.setLevel(deck.side, 1.0) }) }
                .pointerInput(deck.side) {
                    var start = 1.0
                    var moved = 0f
                    detectDragGestures(onDragStart = {
                        start = deck.level
                        moved = 0f
                    }) { change, amount ->
                        change.consume()
                        moved += amount.y
                        val travel = max(1f, size.height - 14 * u * density)
                        model.setLevel(deck.side, start - moved / travel)
                    }
                },
        ) {
            val knob = Size(min(size.width, 24 * u * density), 14 * u * density)
            val travel = max(1f, size.height - knob.height)
            val y = knob.height / 2 + (1 - deck.level.toFloat()) * travel
            val centre = size.width / 2
            val track = 5 * u * density
            drawRoundRect(Color.Black.copy(alpha = 0.7f), Offset(centre - track / 2, knob.height / 2), Size(track, travel), CornerRadius(track / 2))
            drawRoundRect(DjTheme.play.copy(alpha = 0.85f), Offset(centre - track / 2, y), Size(track, max(0f, knob.height / 2 + travel - y)), CornerRadius(track / 2))
            drawKnob(Offset(centre - knob.width / 2, y - knob.height / 2), knob, false)
        }
        DjText("${(deck.level * 100).roundToInt()}", 10 * u, DjTheme.muted, FontWeight.SemiBold, mono = true)
    }
}

@Composable
internal fun DjCrossfader(model: DjStudioModel, u: Float, modifier: Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((10 * u).dp)) {
        DjSlamArrow(model, DjDeckSide.A, u)
        Canvas(
            Modifier.weight(1f).fillMaxHeight()
                .semantics {
                    contentDescription = "Crossfader"
                    stateDescription = if (model.crossfader == 0.0) "Centre" else if (model.crossfader < 0) "${(-model.crossfader * 100).toInt()} percent Deck A" else "${(model.crossfader * 100).toInt()} percent Deck B"
                }
                .pointerInput(Unit) { detectTapGestures(onDoubleTap = { model.setCrossfader(0.0) }) }
                .pointerInput(Unit) {
                    var start = 0.0
                    var moved = 0f
                    detectDragGestures(onDragStart = {
                        start = model.crossfader
                        moved = 0f
                    }) { change, amount ->
                        change.consume()
                        moved += amount.x
                        val travel = max(1f, size.width - 34 * u * density)
                        model.setCrossfader(start + moved / travel * 2)
                    }
                },
        ) {
            val knob = Size(34 * u * density, min(size.height, 54 * u * density))
            val travel = max(1f, size.width - knob.width)
            val track = 12 * u * density
            val inset = knob.width / 2 - 6 * u * density
            drawRoundRect(
                Brush.verticalGradient(listOf(Color(0xFF8F949B), Color(0xFF5B5F66)), size.height / 2 - track / 2, size.height / 2 + track / 2),
                Offset(inset, size.height / 2 - track / 2), Size(size.width - 2 * inset, track), CornerRadius(track / 2),
            )
            drawRect(Color.White.copy(alpha = 0.18f), Offset(size.width / 2, size.height / 2 - knob.height * 0.45f), Size(1f, knob.height * 0.9f))
            val x = size.width / 2 + model.crossfader.toFloat() * travel / 2
            drawKnob(Offset(x - knob.width / 2, size.height / 2 - knob.height / 2), knob, true)
        }
        DjSlamArrow(model, DjDeckSide.B, u)
    }
}

@Composable
private fun DjSlamArrow(model: DjStudioModel, side: DjDeckSide, u: Float) {
    Box(Modifier.size((24 * u).dp, (40 * u).dp).djPress("Crossfade fully to Deck ${side.label}") { model.slam(side) }, contentAlignment = Alignment.Center) {
        FmIcon(if (side == DjDeckSide.A) "arrow_left" else "arrow_right", size = (22 * u).dp, tint = Color.Black.copy(alpha = 0.45f))
    }
}

@Composable
internal fun DjAutomixButton(model: DjStudioModel, u: Float) {
    var menu by remember { mutableStateOf(false) }
    var focusMenu by remember { mutableStateOf(false) }
    val on = model.automixOn
    Box {
        Row(
            Modifier
                .heightIn(min = 44.dp)
                .djPress(
                    "Automix ${if (on) "on" else "off"}" + if (model.crate.focus == DjFocus.Everything) "" else ", playing ${model.crate.label(model.crate.focus)}",
                    onLongPress = { menu = true },
                ) { model.automix() },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Row(
                Modifier
                    .height((28 * u).dp)
                    .shadow(if (on) 8.dp else 2.dp, CircleShape, ambientColor = DjTheme.active, spotColor = if (on) DjTheme.active else Color.Black)
                    .clip(CircleShape)
                    .background(if (on) Brush.linearGradient(listOf(DjTheme.active, DjTheme.active)) else DjTheme.control)
                    .border(1.dp, if (on) Color.White.copy(alpha = 0.35f) else DjTheme.active.copy(alpha = 0.55f), CircleShape)
                    .padding(horizontal = (14 * u).dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy((6 * u).dp),
            ) {
                val ink = if (on) Color(0xFF08172A) else DjTheme.ink
                FmIcon(if (model.automixing) "compare_arrows" else if (model.crate.focus != DjFocus.Everything) "gps_fixed" else "shuffle", size = (14 * u).dp, tint = ink)
                DjText("AUTOMIX", 12 * u, ink, FontWeight.Bold)
            }
        }
        FmActionMenu(menu, { menu = false }) {
            FmActionMenuButton(if (model.automixing) "Stop the Fade" else "Mix to the Other Deck Now", { menu = false; model.mixNow() })
            FmActionMenuButton("Plays ${model.crate.label(model.crate.focus)}…", { menu = false; focusMenu = true })
            FmActionMenuButton("Playlists…", { menu = false; model.sheet = DjSheet.Crate })
        }
        FmActionMenu(focusMenu, { focusMenu = false }) {
            val pick = { focus: DjFocus -> focusMenu = false; model.focusAutomix(focus, false) }
            FmActionMenuButton("Everything", { pick(DjFocus.Everything) })
            model.crate.playlists.forEach { list -> FmActionMenuButton(list.name, { pick(DjFocus.Playlist(list.id)) }) }
            model.crate.tagCounts.forEach { tag -> FmActionMenuButton("#${tag.name}", { pick(DjFocus.Tag(tag.name)) }) }
            DjSmartKind.entries.forEach { kind ->
                model.crate.smart.filter { it.kind == kind }.forEach { list -> FmActionMenuButton(list.name, { pick(DjFocus.Smart(list.id)) }) }
            }
        }
    }
}

@Composable
internal fun DjUtilityRow(model: DjStudioModel, u: Float, modifier: Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((12 * u).dp, Alignment.CenterHorizontally)) {
        Box(Modifier.size((28 * u).dp).djPress("Track info") { model.sheet = DjSheet.Info }, contentAlignment = Alignment.Center) {
            Box(Modifier.size((17 * u).dp).clip(CircleShape).background(DjTheme.muted), contentAlignment = Alignment.Center) {
                DjText("i", 12 * u, Color(0xFF23252A), FontWeight.Black)
            }
        }
        DjRoundButton((26 * u).dp, "grid_view", "Sampler pads") { model.sheet = DjSheet.Pads }
        DjRoundButton((26 * u).dp, "queue_music", "Playlists", lit = model.crate.focus != DjFocus.Everything) { model.sheet = DjSheet.Crate }
        DjAutomixButton(model, u)
        DjAutoDjButton(model, u)
        DjRoundButton((26 * u).dp, "mood", "Reactions", lit = model.reacting) { model.reacting = !model.reacting }
        Box(Modifier.size((28 * u).dp).djPress("Reset all") { model.confirmingReset = true }, contentAlignment = Alignment.Center) {
            FmIcon("restart_alt", size = (18 * u).dp, tint = DjTheme.muted)
        }
    }
}

@Composable
internal fun DjNotice(model: DjStudioModel, modifier: Modifier) {
    val text = model.notice
    var shown by remember { mutableStateOf("") }
    if (text != null) shown = text
    AnimatedVisibility(text != null, modifier, enter = fadeIn() + slideInVertically { -it }, exit = fadeOut() + slideOutVertically { -it }) {
        Text(
            shown, color = DjTheme.ink, fontSize = 13.sp, fontWeight = FontWeight.Medium, textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 6.dp, start = 16.dp, end = 16.dp).clip(RoundedCornerShape(20.dp)).background(Color(0xE6202226))
                .border(0.5.dp, Color.White.copy(alpha = 0.12f), RoundedCornerShape(20.dp)).padding(horizontal = 14.dp, vertical = 8.dp),
        )
    }
}

internal fun IntOffset.Companion.of(offset: Offset): IntOffset = IntOffset(offset.x.roundToInt(), offset.y.roundToInt())
