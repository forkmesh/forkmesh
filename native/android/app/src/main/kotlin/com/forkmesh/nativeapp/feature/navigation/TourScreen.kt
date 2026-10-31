package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.projects.ProjectDrawer
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.feature.market.PackageGateFeature
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.navigation.TourCatalog
import com.forkmesh.nativeapp.navigation.TourJump
import com.forkmesh.nativeapp.navigation.TourPlace
import com.forkmesh.nativeapp.navigation.TourProgress
import com.forkmesh.nativeapp.navigation.TourStop
import com.forkmesh.nativeapp.navigation.TourTrack
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

object ShellTour {
    private val leftState = MutableStateFlow(TourTrack.entries.size)

    val left: StateFlow<Int> = leftState.asStateFlow()

    fun progress(): TourProgress = TourProgress(Services.get<PrefStore>()).also { leftState.value = it.left }

    fun stops(track: TourTrack): List<TourStop> = TourCatalog.stops(track, PackageGateFeature::isInstalled)

    fun refresh() {
        progress()
    }
}

class TourScreen : Screen() {
    override val title: String get() = "Tour"
    override val hidesShellChrome: Boolean get() = true

    private val progress = ShellTour.progress()
    private var track by mutableStateOf<TourTrack?>(null)
    private var stops by mutableStateOf<List<TourStop>>(emptyList())
    private var index by mutableStateOf(0)
    private var done by mutableStateOf(progress.done)
    private var place by mutableStateOf(progress.place)
    private var finished by mutableStateOf<TourTrack?>(null)
    private var navigatorRef: Navigator? = null

    override fun onBack(): Boolean {
        if (track == null) {
            progress.markSeen()
            return false
        }
        track = null
        return true
    }

    @Composable
    override fun Content(navigator: Navigator) {
        navigatorRef = navigator
        val haptics = FmHaptics.current
        val reduced = Fm.reducedMotion
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas).windowInsetsPadding(fmSafeAreaInsets())) {
            AnimatedContent(
                targetState = track,
                transitionSpec = {
                    if (reduced) {
                        fadeIn(tween(0)) togetherWith fadeOut(tween(0))
                    } else if (targetState != null) {
                        (slideInHorizontally { it } + fadeIn()) togetherWith (slideOutHorizontally { -it } + fadeOut())
                    } else {
                        (slideInHorizontally { -it } + fadeIn()) togetherWith (slideOutHorizontally { it } + fadeOut())
                    }
                },
                label = "tour-track",
            ) { shown ->
                if (shown == null) {
                    TourChooser(
                        done = done,
                        resume = resumable(),
                        finished = finished,
                        onPick = { haptics.tap(); start(it, 0) },
                        onResume = { at, i -> haptics.tap(); start(at.track, i) },
                        onSkip = { close(navigator) },
                    )
                } else {
                    Player(shown, navigator)
                }
            }
        }
    }

    private fun resumable(): Triple<TourPlace, Int, Int>? {
        val at = place ?: return null
        val list = ShellTour.stops(at.track)
        val i = TourCatalog.index(at, list) ?: return null
        if (i <= 0) return null
        return Triple(at, i, list.size)
    }

    private fun start(picked: TourTrack, at: Int) {
        finished = null
        stops = ShellTour.stops(picked)
        index = at.coerceIn(0, maxOf(0, stops.size - 1))
        track = picked
        remember()
    }

    private fun remember() {
        val current = track ?: return
        val stop = stops.getOrNull(index) ?: return
        val next = TourPlace(current, stop.id)
        progress.remember(next)
        place = next
    }

    private fun finish(navigator: Navigator) {
        val current = track
        if (current != null) {
            progress.markDone(current)
            done = progress.done
            ShellTour.refresh()
        }
        progress.remember(null)
        place = null
        if (current == null || progress.nextAfter(current) == null) {
            close(navigator)
            return
        }
        finished = current
        track = null
    }

    private fun close(navigator: Navigator) {
        progress.markSeen()
        if (navigator.top === this) navigator.pop()
    }

    private fun canJump(jump: TourJump): Boolean = !jump.needsShell || NavigationFeature.shell() != null

    private fun take(jump: TourJump, navigator: Navigator) {
        close(navigator)
        AppLifecycle.scope.launch(Dispatchers.Main) {
            delay(JUMP_DELAY_MS)
            val shell = NavigationFeature.shell()
            when (jump) {
                is TourJump.Tab -> shell?.select(jump.tab)
                TourJump.Home -> shell?.selectHome()
                TourJump.Hub -> shell?.openHub()
                TourJump.Composer -> shell?.openPrompt()
                TourJump.Palette -> shell?.openPalette()
                TourJump.Projects -> ProjectDrawer.open()
                is TourJump.Open -> NavigationFeature.open(jump.route)
            }
        }
    }

    @Composable
    private fun Player(shown: TourTrack, navigator: Navigator) {
        val haptics = FmHaptics.current
        val reduced = Fm.reducedMotion
        val pager = rememberPagerState(initialPage = index) { stops.size }
        LaunchedEffect(pager) {
            snapshotFlow { pager.currentPage }.collect { page ->
                if (page != index) {
                    index = page
                    haptics.selection()
                    remember()
                }
            }
        }
        LaunchedEffect(index) {
            if (pager.currentPage != index) {
                if (reduced) pager.scrollToPage(index) else pager.animateScrollToPage(index)
            }
        }
        val current = stops.getOrNull(index)
        val last = index >= stops.size - 1
        Column(Modifier.fillMaxSize()) {
            TourPlayerHeader(shown, index, stops.size, onTracks = { track = null }, onClose = { close(navigator) }, onSelect = { at ->
                if (at in stops.indices) index = at
            })
            HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth(), key = { stops[it].id }) { page ->
                TourStopPage(stops[page], active = page == pager.currentPage)
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (index > 0) {
                    FmIconButton("chevron_left", "Back", onPressed = { index -= 1 }, modifier = Modifier.testTag("tour-back"))
                }
                val jump = current?.jump
                if (current != null && jump != null && canJump(jump)) {
                    FmOutlinedButton(current.jumpLabel, onClick = { take(jump, navigator) }, modifier = Modifier.testTag("tour-show-me"))
                }
                Spacer(Modifier.weight(1f))
                FmFilledButton(if (last) "Finish" else "Next", onClick = {
                    if (last) {
                        haptics.success()
                        finish(navigator)
                    } else {
                        index += 1
                    }
                }, modifier = Modifier.testTag("tour-next"))
            }
        }
    }

    companion object {
        const val TOUR_KEY = TourProgress.SEEN_KEY
        private const val JUMP_DELAY_MS = 300L
    }
}

@Composable
private fun TourChooser(
    done: Set<TourTrack>,
    resume: Triple<TourPlace, Int, Int>?,
    finished: TourTrack?,
    onPick: (TourTrack) -> Unit,
    onResume: (TourPlace, Int) -> Unit,
    onSkip: () -> Unit,
) {
    val c = Fm.colors
    val suggested = TourTrack.entries.firstOrNull { it !in done }
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4).padding(bottom = FmSpace.x5).testTag("tour-chooser"),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
    ) {
        Row(Modifier.fillMaxWidth().padding(top = FmSpace.x1), horizontalArrangement = Arrangement.End) {
            FmIconButton("close", "Close the tour", onPressed = onSkip, color = c.textSecondary, iconSize = 20.dp)
        }
        TourChooserHero()
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("Take the tour", style = Fm.text.titleLarge, color = c.textPrimary, modifier = Modifier.semantics { heading() })
            Text("Pick how deep to go. Each stop shows the real screen, and you can try it right there.", style = Fm.text.bodyLarge, color = c.textSecondary)
        }
        if (finished != null) {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(c.bgRaised).padding(FmSpace.x3)
                    .semantics(mergeDescendants = true) {}.testTag("tour-finished"),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("verified", size = 28.dp, tint = c.success)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("${finished.title} done", style = Fm.text.titleSmall, color = c.textPrimary)
                    Text("Keep going with the next track, or close the tour and explore.", style = Fm.text.bodySmall, color = c.textSecondary)
                }
            }
        } else if (resume != null) {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(c.accentSubtle)
                    .clickable(role = Role.Button) { onResume(resume.first, resume.second) }
                    .padding(FmSpace.x3).testTag("tour-resume"),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("play_circle", size = 28.dp, tint = c.accent)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("Pick up where you left off", style = Fm.text.titleSmall, color = c.textPrimary)
                    Text("${resume.first.track.title}, stop ${resume.second + 1} of ${resume.third}", style = Fm.text.bodySmall, color = c.textSecondary)
                }
                FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
            }
        }
        TourTrack.entries.forEach { option ->
            TourTrackCard(option, ShellTour.stops(option).size, option in done, suggested == option) { onPick(option) }
        }
        Text(
            if (finished == null) "Not now" else "Done for now",
            style = Fm.text.labelLarge,
            color = c.textSecondary,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().heightIn(min = 44.dp).clip(RoundedCornerShape(12.dp))
                .clickable(role = Role.Button, onClick = onSkip).padding(vertical = FmSpace.x3).testTag("tour-skip"),
        )
        Text(
            "Tour stays on Home, so you can come back for the next track.",
            style = Fm.text.bodySmall,
            color = c.textTertiary,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun TourChooserHero() {
    val c = Fm.colors
    Row(Modifier.clearAndSetSemantics {}, horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
        TourSparkleButton(stars = 6, size = 52.dp)
        listOf("grid_view", "auto_awesome", "inbox", "library_books").forEach { icon ->
            Box(Modifier.size(40.dp).clip(RoundedCornerShape(12.dp)).background(c.bgRaised), contentAlignment = Alignment.Center) {
                FmIcon(icon, size = 20.dp, tint = c.textSecondary)
            }
        }
    }
}

@Composable
private fun TourTrackCard(track: TourTrack, count: Int, done: Boolean, suggested: Boolean, onPick: () -> Unit) {
    val c = Fm.colors
    val shape = RoundedCornerShape(22.dp)
    val meta = "$count stops, about ${TourCatalog.minutes(count)} min"
    Column(
        Modifier.fillMaxWidth().clip(shape).background(c.bgRaised)
            .border(BorderStroke(if (suggested) 1.5.dp else 0.5.dp, if (suggested) c.accent.copy(alpha = 0.6f) else c.border), shape)
            .clickable(onClick = onPick)
            .clearAndSetSemantics {
                role = Role.Button
                contentDescription = "${track.title}. $meta. ${track.summary}${if (done) " Done." else ""}"
            }
            .padding(FmSpace.x4)
            .testTag("tour-track-${track.raw}"),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.Top) {
            Box(Modifier.size(40.dp).background(c.accent, CircleShape), contentAlignment = Alignment.Center) {
                FmIcon(track.icon, size = 20.dp, tint = c.onAccent)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                    Text(track.title, style = Fm.text.titleMedium, color = c.textPrimary)
                    if (suggested) {
                        Text(
                            if (track == TourTrack.ESSENTIALS) "Start here" else "Up next",
                            style = Fm.text.labelSmall,
                            color = c.accent,
                            modifier = Modifier.background(c.accentSubtle, CircleShape).padding(horizontal = 6.dp, vertical = 2.dp),
                        )
                    }
                }
                Text(meta, style = Fm.text.bodySmall, color = c.textSecondary)
            }
            if (done) FmIcon("verified", size = 20.dp, tint = c.success) else FmIcon("chevron_right", size = 16.dp, tint = c.textTertiary)
        }
        Text(track.summary, style = Fm.text.bodyMedium, color = c.textSecondary)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
            if (track != TourTrack.ESSENTIALS) FmIcon("add", size = 12.dp, tint = c.textTertiary)
            TourCatalog.symbols(track).forEach { icon ->
                Box(Modifier.size(24.dp).clip(RoundedCornerShape(7.dp)).background(c.accentSubtle), contentAlignment = Alignment.Center) {
                    FmIcon(icon, size = 13.dp, tint = c.accent)
                }
            }
        }
    }
}

@Composable
private fun TourPlayerHeader(track: TourTrack, index: Int, count: Int, onTracks: () -> Unit, onClose: () -> Unit, onSelect: (Int) -> Unit) {
    val c = Fm.colors
    val shown = minOf(index + 1, count)
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4).padding(top = FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            Row(
                Modifier.heightIn(min = 44.dp).clip(RoundedCornerShape(12.dp))
                    .clickable(role = Role.Button, onClick = onTracks)
                    .semantics(mergeDescendants = true) { contentDescription = "Choose another track" }
                    .padding(end = FmSpace.x2).testTag("tour-tracks"),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                FmIcon("chevron_left", size = 18.dp, tint = c.accent)
                Text(track.title, style = Fm.text.labelLarge, color = c.accent)
            }
            Spacer(Modifier.weight(1f))
            Text("$shown of $count", style = Fm.text.labelMedium.copy(fontFeatureSettings = "tnum"), color = c.textSecondary)
            FmIconButton("close", "Close the tour", onPressed = onClose, color = c.textSecondary, iconSize = 20.dp, modifier = Modifier.testTag("tour-close"))
        }
        Row(
            Modifier.fillMaxWidth().semantics(mergeDescendants = true) {
                contentDescription = "Stop $shown of $count"
                setProgress { target ->
                    onSelect(target.toInt() - 1)
                    true
                }
            },
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            for (at in 0 until count) {
                val height by animateDpAsState(if (at == index) 6.dp else 4.dp, label = "tour-segment")
                Box(
                    Modifier.weight(1f).height(16.dp).clickable(onClickLabel = "Go to stop ${at + 1}") { onSelect(at) },
                    contentAlignment = Alignment.Center,
                ) {
                    Box(Modifier.fillMaxWidth().height(height).background(if (at <= index) c.accent else c.border, CircleShape))
                }
            }
        }
    }
}

@Composable
private fun TourStopPage(stop: TourStop, active: Boolean) {
    val c = Fm.colors
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
    ) {
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            TourStage(stop.scene, stop.title, active, Modifier.widthIn(max = 420.dp))
        }
        TourHint(stop.tryIt, active)
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                FmIcon(stop.icon, size = 20.dp, tint = c.accent)
                Text(stop.title, style = Fm.text.titleLarge, color = c.textPrimary, modifier = Modifier.semantics { heading() })
            }
            Text(stop.message, style = Fm.text.bodyLarge, color = c.textSecondary)
        }
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            stop.tips.forEach { tip ->
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.Top) {
                    FmIcon("check_circle", size = 15.dp, tint = c.accent, modifier = Modifier.padding(top = 2.dp))
                    Text(tip, style = Fm.text.bodyMedium, color = c.textSecondary)
                }
            }
        }
        if (stop.fromMarket) {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(c.bgRaised).padding(FmSpace.x3).semantics(mergeDescendants = true) {},
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("shopping_bag", size = 18.dp, tint = c.accent)
                Text("Not installed yet. Add it from the Market to use it.", style = Fm.text.bodySmall, color = c.textSecondary)
            }
        }
    }
}

@Composable
private fun TourHint(text: String, active: Boolean) {
    val c = Fm.colors
    val beat = rememberTourPulse(active)
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Row(
            Modifier.background(c.accentSubtle, CircleShape).padding(horizontal = FmSpace.x3, vertical = 6.dp)
                .clearAndSetSemantics { contentDescription = "Try it: $text" },
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("touch_app", size = 16.dp, tint = c.accent, modifier = Modifier.graphicsLayer { alpha = 0.55f + 0.45f * beat })
            Text(text, style = Fm.text.labelMedium, color = c.accent)
        }
    }
}

@Composable
fun ShellFirstRun(navigator: Navigator) {
    LaunchedEffect(Unit) {
        val progress = ShellTour.progress()
        if (progress.seen) return@LaunchedEffect
        if (!PackageGateFeature.isInstalled(PackageGate.TOUR)) return@LaunchedEffect
        progress.markSeen()
        navigator.push(TourScreen())
    }
}
