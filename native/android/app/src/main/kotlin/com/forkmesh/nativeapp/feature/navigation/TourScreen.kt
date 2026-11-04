package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.foundation.text.InlineTextContent
import androidx.compose.foundation.text.appendInlineContent
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.Placeholder
import androidx.compose.ui.text.PlaceholderVerticalAlign
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.em
import com.forkmesh.nativeapp.navigation.TourScene
import com.forkmesh.nativeapp.navigation.TourShort
import com.forkmesh.nativeapp.navigation.short
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.fmGlass
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.animateColorAsState
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.theme.FmType
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

class TourScreen(private val startTrack: TourTrack? = null, private val startAt: Int = 0) : Screen() {
    override val title: String get() = "Tour"
    override val hidesShellChrome: Boolean get() = true

    private val progress = ShellTour.progress()
    private var track by mutableStateOf(startTrack)
    private var stops by mutableStateOf(startTrack?.let(ShellTour::stops).orEmpty())
    private var index by mutableIntStateOf(startAt.coerceIn(0, maxOf(0, stops.size - 1)))
    private var done by mutableStateOf(progress.done)
    private var place by mutableStateOf(progress.place)
    private val direct = startTrack != null

    override fun onBack(): Boolean {
        progress.markSeen()
        if (track == null || direct) return false
        leavePlayer()
        return true
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val haptics = FmHaptics.current
        val reduced = Fm.reducedMotion
        AnimatedContent(
            targetState = track,
            transitionSpec = { if (reduced) fadeIn(tween(0)) togetherWith fadeOut(tween(0)) else fadeIn() togetherWith fadeOut() },
            label = "tour-player",
        ) { shown ->
            if (shown == null) {
                Box(Modifier.fillMaxSize().background(Fm.colors.canvas).windowInsetsPadding(fmSafeAreaInsets())) {
                    TourChooser(
                        done = done,
                        resume = resumable(),
                        finished = null,
                        onPick = { haptics.tap(); start(it, 0) },
                        onResume = { at, i -> haptics.tap(); start(at.track, i) },
                        onSkip = { close(navigator) },
                    )
                }
            } else {
                key(shown) { Player(shown, navigator) }
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
        stops = ShellTour.stops(picked)
        index = at.coerceIn(0, maxOf(0, stops.size - 1))
        track = picked
    }

    private fun rememberPlace() {
        val current = track ?: return
        val stop = stops.getOrNull(index) ?: return
        val next = TourPlace(current, stop.id)
        progress.remember(next)
        place = next
    }

    private fun complete(finished: TourTrack) {
        progress.markDone(finished)
        progress.remember(null)
        done = progress.done
        place = null
        ShellTour.refresh()
    }

    private fun continueWith(next: TourTrack) {
        val list = ShellTour.stops(next)
        val seen = stops.map { it.id }.toSet()
        stops = list
        index = list.indexOfFirst { it.id !in seen }.takeIf { it >= 0 } ?: 0
        track = next
        rememberPlace()
    }

    private fun leavePlayer() {
        progress.markSeen()
        done = progress.done
        place = progress.place
        track = null
    }

    private fun closePlayer(navigator: Navigator) {
        if (direct) close(navigator) else leavePlayer()
    }

    private fun close(navigator: Navigator) {
        progress.markSeen()
        ShellTour.refresh()
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
        val c = Fm.colors
        val haptics = FmHaptics.current
        val reduced = Fm.reducedMotion
        val count = stops.size
        val pager = rememberPagerState(initialPage = index.coerceIn(0, count)) { count + 1 }
        var settled by remember { mutableIntStateOf(index) }
        LaunchedEffect(Unit) {
            progress.markSeen()
            ShellTour.refresh()
            rememberPlace()
        }
        LaunchedEffect(pager) {
            snapshotFlow { pager.settledPage }.collect { page -> if (page != index) index = page }
        }
        LaunchedEffect(index) {
            if (index != settled) {
                settled = index
                if (index >= count) {
                    haptics.success()
                    complete(shown)
                } else {
                    haptics.selection()
                    rememberPlace()
                }
            }
            if (pager.currentPage != index) {
                if (reduced) pager.scrollToPage(index) else pager.animateScrollToPage(index)
            }
        }
        val finished = index >= count
        val current = stops.getOrNull(index)
        val tint by animateColorAsState(
            when {
                finished -> c.success
                current?.scene == TourScene.CODE -> tourMerged
                else -> c.accent
            },
            tween(600),
            label = "tour-backdrop",
        )
        val glow = if (c.dark) 0.26f else 0.13f
        fun go(next: Int) {
            if (next in 0..count) index = next
        }
        Box(
            Modifier
                .fillMaxSize()
                .background(if (c.dark) Color.Black else c.bgBase)
                .drawBehind {
                    drawRect(
                        Brush.radialGradient(
                            listOf(tint.copy(alpha = glow), tint.copy(alpha = 0f)),
                            center = Offset(size.width / 2f, size.height * 0.34f),
                            radius = 320.dp.toPx(),
                        ),
                    )
                }
                .testTag("tour-player"),
        ) {
            Column(Modifier.fillMaxSize().windowInsetsPadding(fmSafeAreaInsets())) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4).padding(top = FmSpace.x1),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(
                        Modifier
                            .size(44.dp)
                            .fmGlass(CircleShape, interactive = true)
                            .clickable(role = Role.Button) { if (index == 0) closePlayer(navigator) else go(index - 1) }
                            .semantics { contentDescription = if (index == 0) "Close the tour" else "Back" }
                            .testTag("tour-back"),
                        contentAlignment = Alignment.Center,
                    ) {
                        FmIcon(if (index == 0) "close" else "arrow_back", size = 20.dp, tint = c.textPrimary)
                    }
                    TourDots(count, minOf(index, count - 1), ::go)
                    Spacer(Modifier.weight(1f))
                    if (!finished) {
                        FmTextButton(
                            onClick = { closePlayer(navigator) },
                            contentColor = c.textSecondary,
                            modifier = Modifier.testTag("tour-skip").semantics { stateDescription = "Closes the tour. Tour stays on Home." },
                        ) { Text("Skip", style = Fm.text.labelLarge) }
                    }
                }
                HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth(), key = { if (it < stops.size) stops[it].id else "done" }) { page ->
                    if (page < stops.size) {
                        TourPlayerPage(stops[page], active = page == pager.currentPage)
                    } else {
                        TourDonePage(shown, progress.nextAfter(shown), active = page == pager.currentPage) { next ->
                            haptics.tap()
                            continueWith(next)
                        }
                    }
                }
                Column(
                    Modifier.fillMaxWidth().padding(horizontal = FmSpace.x5).padding(bottom = FmSpace.x2),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    FmFilledButton(
                        when {
                            finished -> "Start using ForkMesh"
                            index == count - 1 -> "Finish"
                            else -> "Next"
                        },
                        onClick = { if (finished) closePlayer(navigator) else go(index + 1) },
                        modifier = Modifier.fillMaxWidth().height(54.dp).testTag("tour-next"),
                    )
                    Box(Modifier.fillMaxWidth().height(44.dp), contentAlignment = Alignment.Center) {
                        val jump = current?.jump
                        if (current != null && !finished && jump != null && canJump(jump)) {
                            FmTextButton(
                                onClick = { take(jump, navigator) },
                                contentColor = c.textSecondary,
                                modifier = Modifier.testTag("tour-show-me"),
                            ) { Text(current.jumpLabel, style = Fm.text.labelLarge) }
                        }
                    }
                }
            }
        }
    }

    companion object {
        const val TOUR_KEY = TourProgress.SEEN_KEY
        private const val JUMP_DELAY_MS = 300L

        fun firstRunStop(): Int {
            val at = ShellTour.progress().place ?: return 0
            if (at.track != TourTrack.ESSENTIALS) return 0
            return TourCatalog.index(at, ShellTour.stops(TourTrack.ESSENTIALS)) ?: 0
        }
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
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("Take the tour", style = Fm.text.displaySmall, color = c.textPrimary, modifier = Modifier.semantics { heading() })
            Text("Pick how deep to go. Each stop shows the real screen.", style = Fm.text.bodyLarge, color = c.textSecondary)
        }
        if (finished != null) {
            TourGroup {
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 60.dp).padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
                        .semantics(mergeDescendants = true) {}.testTag("tour-finished"),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text("${finished.title} done", style = Fm.text.titleSmall, color = c.textPrimary)
                        Text("Keep going, or close the tour and explore.", style = Fm.text.bodyMedium, color = c.textSecondary)
                    }
                    FmIcon("check", size = 18.dp, tint = c.success)
                }
            }
        } else if (resume != null) {
            TourGroup {
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 60.dp)
                        .clickable(role = Role.Button) { onResume(resume.first, resume.second) }
                        .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2).testTag("tour-resume"),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text("Pick up where you left off", style = Fm.text.titleSmall, color = c.accent)
                        Text("${resume.first.track.title} · stop ${resume.second + 1} of ${resume.third}", style = Fm.text.bodyMedium, color = c.textSecondary)
                    }
                    FmIcon("chevron_right", size = 18.dp, tint = c.textDisabled)
                }
            }
        }
        TourGroup {
            TourTrack.entries.forEachIndexed { index, option ->
                if (index > 0) FmRowDivider(indent = FmSpace.x4)
                TourTrackRow(option, ShellTour.stops(option).size, option in done, suggested == option) { onPick(option) }
            }
        }
        Text(
            if (finished == null) "Not now" else "Done for now",
            style = Fm.text.labelLarge,
            color = c.textSecondary,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().heightIn(min = 44.dp).clip(RoundedCornerShape(12.dp))
                .clickable(role = Role.Button, onClick = onSkip).padding(vertical = FmSpace.x3).testTag("tour-skip"),
        )
    }
}

@Composable
private fun TourGroup(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(22.dp)).background(Fm.colors.bgRaised)) { content() }
}

@Composable
private fun TourTrackRow(track: TourTrack, count: Int, done: Boolean, suggested: Boolean, onPick: () -> Unit) {
    val c = Fm.colors
    val meta = "$count stops · about ${TourCatalog.minutes(count)} min"
    Row(
        Modifier.fillMaxWidth().heightIn(min = 60.dp)
            .clickable(onClick = onPick)
            .clearAndSetSemantics {
                role = Role.Button
                contentDescription = "${track.title}. $meta. ${track.summary}${if (done) " Done." else ""}"
            }
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
            .testTag("tour-track-${track.raw}"),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                Text(track.title, style = Fm.text.titleSmall, color = c.textPrimary)
                if (suggested) {
                    Text(
                        if (track == TourTrack.ESSENTIALS) "Start here" else "Up next",
                        style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis),
                        color = c.accent,
                    )
                }
            }
            Text(meta, style = Fm.text.bodyMedium.copy(fontFeatureSettings = "tnum"), color = c.textSecondary)
        }
        if (done) FmIcon("check", size = 18.dp, tint = c.success) else FmIcon("chevron_right", size = 18.dp, tint = c.textDisabled)
    }
}

@Composable
private fun TourDots(count: Int, index: Int, onSelect: (Int) -> Unit) {
    val c = Fm.colors
    val dot = if (count > 12) 5.dp else 6.dp
    Row(
        Modifier.clearAndSetSemantics {
            contentDescription = "Stop ${index + 1} of $count"
            setProgress { target ->
                onSelect(target.toInt() - 1)
                true
            }
        },
        horizontalArrangement = Arrangement.spacedBy(if (count > 12) 4.dp else 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (at in 0 until count) {
            val width by animateDpAsState(if (at == index) dot * 4 else dot, label = "tour-dot")
            Box(Modifier.height(44.dp).tourTap { onSelect(at) }, contentAlignment = Alignment.Center) {
                Box(Modifier.size(width, dot).background(if (at == index) c.textPrimary else c.textTertiary.copy(alpha = 0.5f), CircleShape))
            }
        }
    }
}

@Composable
private fun TourPlayerPage(stop: TourStop, active: Boolean) {
    val c = Fm.colors
    val short = stop.short
    val line = if (stop.fromMarket) "Not installed yet. Add it from the Market." else short.line
    Column(Modifier.fillMaxSize()) {
        TourStage(stop.scene, short.headline, active, Modifier.weight(1f).fillMaxWidth())
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = FmSpace.x6)
                .padding(top = FmSpace.x3, bottom = FmSpace.x4)
                .clearAndSetSemantics {
                    heading()
                    contentDescription = "${short.headline}. $line ${stop.message}"
                },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            TourHeadline(short)
            Text(line, style = Fm.text.bodyLarge, color = c.textSecondary, textAlign = TextAlign.Center)
        }
    }
}

@Composable
private fun TourHeadline(short: TourShort) {
    val c = Fm.colors
    val style = Fm.text.displaySmall
    val icon = with(LocalDensity.current) { style.fontSize.toDp() }
    val emblem = short.emblem
    val text = buildAnnotatedString {
        short.headline.split(" ").forEachIndexed { at, word ->
            if (at > 0) append(" ")
            if (emblem != null && word == short.emblemBefore) {
                appendInlineContent(EMBLEM, " ")
                append(" ")
            }
            append(word)
        }
        withStyle(SpanStyle(color = c.accent)) { append(".") }
    }
    val inline = if (emblem == null) {
        emptyMap()
    } else {
        mapOf(EMBLEM to InlineTextContent(Placeholder(1.em, 1.em, PlaceholderVerticalAlign.TextCenter)) { FmIcon(emblem, size = icon, tint = c.accent) })
    }
    Text(text, style = style, color = c.textPrimary, textAlign = TextAlign.Center, inlineContent = inline)
}

private const val EMBLEM = "emblem"

@Composable
private fun TourDonePage(track: TourTrack, next: TourTrack?, active: Boolean, onNext: (TourTrack) -> Unit) {
    val c = Fm.colors
    val reduced = Fm.reducedMotion
    var drawn by remember { mutableStateOf(false) }
    LaunchedEffect(active) { drawn = active }
    val pop by animateFloatAsState(
        if (drawn || reduced) 1f else 0f,
        if (reduced) snap<Float>() else spring<Float>(dampingRatio = 0.5f, stiffness = 300f),
        label = "tour-done-pop",
    )
    val shape = RoundedCornerShape(44.dp)
    val shadow = Color.Black.copy(alpha = if (c.dark) 0.4f else 0.1f)
    Column(Modifier.fillMaxSize().padding(bottom = FmSpace.x4), horizontalAlignment = Alignment.CenterHorizontally) {
        Spacer(Modifier.weight(1f))
        Box(
            Modifier
                .graphicsLayer {
                    val k = 0.7f + 0.3f * pop
                    scaleX = k
                    scaleY = k
                    alpha = pop.coerceIn(0f, 1f)
                }
                .size(140.dp)
                .shadow(16.dp, shape, ambientColor = shadow, spotColor = shadow)
                .background(c.bgRaised, shape)
                .clearAndSetSemantics {},
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("check", size = 64.dp, tint = c.success)
        }
        Spacer(Modifier.weight(1f))
        Column(
            Modifier.padding(horizontal = FmSpace.x6),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Text(
                buildAnnotatedString {
                    append("You're set")
                    withStyle(SpanStyle(color = c.accent)) { append(".") }
                },
                style = Fm.text.displaySmall,
                color = c.textPrimary,
                modifier = Modifier.semantics { heading() },
            )
            Text("${track.title} done. Tour stays on Home for later.", style = Fm.text.bodyLarge, color = c.textSecondary, textAlign = TextAlign.Center)
        }
        if (next != null) {
            val added = ShellTour.stops(next).size - ShellTour.stops(track).size
            Row(
                Modifier
                    .padding(horizontal = FmSpace.x5)
                    .padding(top = FmSpace.x5)
                    .fillMaxWidth()
                    .heightIn(min = 64.dp)
                    .clip(RoundedCornerShape(22.dp))
                    .background(c.bgRaised)
                    .clickable(role = Role.Button) { onNext(next) }
                    .padding(horizontal = FmSpace.x4)
                    .testTag("tour-next-track"),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("auto_awesome", size = 20.dp, tint = c.accent)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("Keep going: ${next.title}", style = Fm.text.titleSmall, color = c.textPrimary)
                    Text(
                        "$added more stops · about ${TourCatalog.minutes(added)} min",
                        style = Fm.text.bodyMedium.copy(fontFeatureSettings = FmType.tabular),
                        color = c.textSecondary,
                    )
                }
                FmIcon("chevron_right", size = 18.dp, tint = c.textDisabled)
            }
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
        navigator.push(TourScreen(TourTrack.ESSENTIALS, TourScreen.firstRunStop()))
    }
}
