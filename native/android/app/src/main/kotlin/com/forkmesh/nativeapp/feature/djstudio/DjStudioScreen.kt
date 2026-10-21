package com.forkmesh.nativeapp.feature.djstudio

import android.app.Activity
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.MutableLongState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlin.math.max
import kotlin.math.min

class DjStudioScreen : Screen() {
    override val title: String get() = "Video Mix DJ Studio"
    override val hidesShellChrome: Boolean get() = true
    override val extendsUnderTabBar: Boolean get() = true

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val model = DjStudioHub.studio(context)
        val activity = context as? Activity
        DisposableEffect(model) {
            model.onLeave = { navigator.pop() }
            model.appear(activity)
            onDispose {
                model.disappear(activity)
                model.onLeave = null
            }
        }
        DjStudioView(model)
    }

    override fun onBack(): Boolean {
        val model = DjStudioHub.current() ?: return false
        if (model.sheet != null) {
            model.sheet = null
            return true
        }
        return false
    }
}

@Composable
internal fun DjStudioView(model: DjStudioModel) {
    val moving = model.a.playing || model.b.playing || model.a.scratching || model.b.scratching || model.draggedId != null
    val clock = rememberFrameClock(moving)
    val insets = androidx.compose.foundation.layout.WindowInsets.systemBars.union(androidx.compose.foundation.layout.WindowInsets.displayCutout).asPaddingValues()
    Box(Modifier.fillMaxSize().background(DjTheme.backdrop)) {
        BoxWithConstraints(Modifier.fillMaxSize().padding(insets)) {
            val width = maxWidth.value
            val height = maxHeight.value
            if (width > height) DjLandscape(model, width, height, clock) else DjPortrait(model, width, height, clock)
        }
        DjReactionLayer(model, Modifier.fillMaxSize())
        AnimatedVisibility(
            model.reacting, Modifier.align(Alignment.BottomCenter).padding(insets).padding(bottom = 56.dp, start = 12.dp, end = 12.dp),
            enter = fadeIn() + slideInVertically { it }, exit = fadeOut() + slideOutVertically { it },
        ) { DjReactionBar(model, Modifier) }
        DjQueueGhost(model)
        DjNotice(model, Modifier.align(Alignment.TopCenter).padding(top = insets.calculateTopPadding()))
    }
    DjSheetHost(model)
    DjPromptHost(model)
    if (model.confirmingReset) {
        FmConfirmDialog(
            title = "Reset everything?",
            onConfirm = {
                model.confirmingReset = false
                model.resetAll()
            },
            onDismissRequest = { model.confirmingReset = false },
            body = "Stops and unloads both decks, and clears cues, loops, tempo, BPM, levels, the crossfader and sampler sections. Recently Played, playlists and tags are kept.",
            confirmLabel = "Reset All",
            destructive = true,
        )
    }
}

private val Divider = Color.Black.copy(alpha = 0.6f)

@Composable
private fun DjLandscape(model: DjStudioModel, width: Float, height: Float, clock: MutableLongState) {
    val u = min(1.7f, max(0.78f, min(height / 380f, width / 800f)))
    val top = 76 * u
    val bottom = 64 * u
    val footer = 34 * u
    val middle = max(120f, height - top - bottom - footer)
    val strip = 58 * u
    val level = 30 * u
    val narrowest = 96 * u
    val d = max(100f, min(middle - 10 * u, (width - 2 * strip - 2 * level - narrowest - 40 * u) / 2))
    val centre = max(narrowest, min(230 * u, width - 2 * strip - 2 * level - 2 * d - 40 * u))
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().height(top.dp).background(DjTheme.bar).padding(horizontal = (6 * u).dp).padding(top = (4 * u).dp),
            horizontalArrangement = Arrangement.spacedBy((8 * u).dp),
        ) {
            Box(Modifier.padding(top = (2 * u).dp)) { DjBackButton(model, (34 * u).dp) }
            DjLibraryHandle(model, com.forkmesh.nativeapp.djstudio.DjDeckSide.A, u)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy((4 * u).dp)) {
                Row(Modifier.fillMaxWidth().height((42 * u).dp), verticalAlignment = Alignment.CenterVertically) {
                    DjDeckInfo(model, model.a, false, u, clock, Modifier.weight(1f))
                    Spacer(Modifier.width((12 * u).dp))
                    DjDeckInfo(model, model.b, true, u, clock, Modifier.weight(1f))
                }
                Row(Modifier.fillMaxWidth().height((24 * u).dp), horizontalArrangement = Arrangement.spacedBy((10 * u).dp)) {
                    DjWaveform(model, model.a, clock, Modifier.weight(1f).fillMaxSize())
                    DjWaveform(model, model.b, clock, Modifier.weight(1f).fillMaxSize())
                }
            }
            DjLibraryHandle(model, com.forkmesh.nativeapp.djstudio.DjDeckSide.B, u)
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(Divider))
        Row(Modifier.fillMaxWidth().height(middle.dp).padding(horizontal = (8 * u).dp), verticalAlignment = Alignment.CenterVertically) {
            DjPitchStrip(model, model.a, u, Modifier.height((middle - 12 * u).dp))
            DjLevelFader(model, model.a, u, Modifier.width(level.dp).height((middle - 12 * u).dp))
            Spacer(Modifier.weight(1f))
            DjPlatter(model, model.a, d.dp)
            DjQueueView(model, u, true, Modifier.width(centre.dp).height((middle - 8 * u).dp).padding(horizontal = (6 * u).dp))
            DjPlatter(model, model.b, d.dp)
            Spacer(Modifier.weight(1f))
            DjLevelFader(model, model.b, u, Modifier.width(level.dp).height((middle - 12 * u).dp))
            DjPitchStrip(model, model.b, u, Modifier.height((middle - 12 * u).dp))
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(Divider))
        Column(Modifier.fillMaxWidth().weight(1f).background(DjTheme.bar).padding(horizontal = (10 * u).dp)) {
            Row(Modifier.fillMaxWidth().height(bottom.dp), verticalAlignment = Alignment.CenterVertically) {
                DjTransport(model, model.a, u, false, 40f, 40f)
                Spacer(Modifier.weight(1f))
                DjCueButtons(model, model.a, u, false, 42f)
                Spacer(Modifier.weight(1f))
                DjCrossfader(model, u, Modifier.width(max(120 * u, min(300 * u, width - 636 * u)).dp).height((bottom - 8 * u).dp))
                Spacer(Modifier.weight(1f))
                DjCueButtons(model, model.b, u, false, 42f)
                Spacer(Modifier.weight(1f))
                DjTransport(model, model.b, u, false, 40f, 40f)
            }
            DjUtilityRow(model, u, Modifier.fillMaxWidth().height(footer.dp))
        }
    }
}

@Composable
private fun DjPortrait(model: DjStudioModel, width: Float, height: Float, clock: MutableLongState) {
    val u = min(1.6f, max(0.74f, min(width / 390f, height / 780f)))
    val header = 40 * u
    val wave = 24 * u
    val people = if (model.inJam) 34 * u else 0f
    val mixer = 122 * u + people
    val footer = 42 * u
    val deckBody = max(110f, (height - mixer - footer - 2 * (header + wave) - 32 * u) / 2)
    val d = max(100f, min(deckBody - 6 * u, width - 198 * u))
    Column(Modifier.fillMaxSize()) {
        DjPortraitDeck(model, model.a, deckBody, d, header, wave, u, clock)
        Box(Modifier.fillMaxWidth().height(1.dp).background(Divider))
        Column(
            Modifier.fillMaxWidth().height(mixer.dp).background(DjTheme.bar).padding(vertical = (4 * u).dp),
            verticalArrangement = Arrangement.spacedBy((4 * u).dp),
        ) {
            DjQueueView(model, u, false, Modifier.fillMaxWidth().height((58 * u).dp).padding(horizontal = (10 * u).dp))
            if (people > 0) DjPeopleStrip(model, u, Modifier.fillMaxWidth().height((people - 4 * u).dp).padding(horizontal = (10 * u).dp))
            DjCrossfader(model, u, Modifier.fillMaxWidth().height((46 * u).dp).padding(horizontal = (24 * u).dp))
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(Divider))
        DjPortraitDeck(model, model.b, deckBody, d, header, wave, u, clock)
        Box(Modifier.fillMaxWidth().height(1.dp).background(Divider))
        DjUtilityRow(model, u, Modifier.fillMaxWidth().weight(1f).background(DjTheme.bar))
    }
}

@Composable
private fun DjPortraitDeck(model: DjStudioModel, deck: DjDeck, deckBody: Float, d: Float, header: Float, wave: Float, u: Float, clock: MutableLongState) {
    Column(Modifier.fillMaxWidth().padding(horizontal = (10 * u).dp, vertical = (4 * u).dp), verticalArrangement = Arrangement.spacedBy((4 * u).dp)) {
        Row(Modifier.fillMaxWidth().height(header.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((8 * u).dp)) {
            if (deck.side == com.forkmesh.nativeapp.djstudio.DjDeckSide.A) DjBackButton(model, (32 * u).dp)
            DjDeckInfo(model, deck, false, u * 0.82f, clock, Modifier.weight(1f))
            Box(Modifier.size((32 * u).dp, header.dp)) { DjLibraryHandle(model, deck.side, u * 0.8f) }
        }
        DjWaveform(model, deck, clock, Modifier.fillMaxWidth().height(wave.dp))
        Row(Modifier.fillMaxWidth().height(deckBody.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy((6 * u).dp)) {
            Column(Modifier.width((64 * u).dp), verticalArrangement = Arrangement.spacedBy((6 * u).dp)) {
                DjTransport(model, deck, u, true, 64f, 26f)
                DjCueButtons(model, deck, u, true, 64f)
            }
            Spacer(Modifier.weight(1f))
            DjPlatter(model, deck, d.dp)
            Spacer(Modifier.weight(1f))
            DjLevelFader(model, deck, u * 0.92f, Modifier.width((26 * u).dp).height((deckBody - 4 * u).dp))
            DjPitchStrip(model, deck, u * 0.92f, Modifier.height((deckBody - 4 * u).dp))
        }
    }
}
