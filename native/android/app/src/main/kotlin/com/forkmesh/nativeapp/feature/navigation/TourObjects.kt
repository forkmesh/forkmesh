package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.delay
import kotlin.math.sin

internal val tourMerged = Color(0xFF7B4DFF)

@Composable
internal fun Modifier.tourCard(radius: Dp = 22.dp): Modifier {
    val c = Fm.colors
    val shape = RoundedCornerShape(radius)
    val shadow = Color.Black.copy(alpha = if (c.dark) 0.45f else 0.1f)
    return this
        .shadow(12.dp, shape, ambientColor = shadow, spotColor = shadow)
        .background(c.bgRaised, shape)
        .border(0.5.dp, Color.White.copy(alpha = if (c.dark) 0.06f else 0f), shape)
}

@Composable
private fun TourInitial(initial: String, color: Color, size: Dp = 34.dp, textSize: Int = 14) {
    Box(Modifier.size(size).background(color, CircleShape), contentAlignment = Alignment.Center) {
        Text(initial, color = Color.White, fontSize = textSize.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun TourAgentLine(initial: String, color: Color, title: String, detail: String) {
    val c = Fm.colors
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
        TourInitial(initial, color)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = c.textPrimary, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            AnimatedContent(detail, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "tour-agent-detail") { shown ->
                Text(shown, color = c.textSecondary, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Composable
internal fun TourAgentsScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 3
    val haptics = FmHaptics.current
    LaunchedEffect(step) { if (step == 1) haptics.success() }
    val fill by animateColorAsState(if (step == 0) c.textPrimary else c.success, tween(450), label = "tour-agents-button")
    Box(Modifier.width(320.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier.fillMaxWidth().offset(y = (-86).dp).graphicsLayer {
                scaleX = 0.84f
                scaleY = 0.84f
                alpha = 0.45f
            }.tourCard().padding(16.dp),
        ) { TourAgentLine("D", Color(0xFF6B5A46), "docs-refresh", "Finished · 2 commits") }
        Box(
            Modifier.fillMaxWidth().offset(y = (-44).dp).graphicsLayer {
                scaleX = 0.92f
                scaleY = 0.92f
                alpha = 0.75f
            }.tourCard().padding(16.dp),
        ) { TourAgentLine("R", Color(0xFF2F5D50), "fix-inbox-badges", "Running on your desktop · 4:12") }
        Column(
            Modifier.fillMaxWidth().offset(y = 30.dp).tourCard(26.dp).padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            TourAgentLine(
                "A",
                c.accent,
                if (step == 2) "Pushed to main" else "Agent wants to push to main",
                when (step) {
                    0 -> "forkmesh/forkmesh · 3 commits"
                    1 -> "Running checks · 0:12"
                    else -> "+214 −96 · ready to merge"
                },
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                FmIcon("call_merge", size = 15.dp, tint = c.textSecondary)
                Text("Phone header avatar: fill the status ring", color = c.textSecondary, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                Box(
                    Modifier.tourTarget(step == 0).height(36.dp).background(fill, CircleShape)
                        .tourTap(if (step == 0) "Allow" else null) { act(1) }
                        .padding(horizontal = 18.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    AnimatedContent(step, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "tour-agents-label") { shown ->
                        Text(
                            when (shown) {
                                0 -> "Allow"
                                1 -> "Running"
                                else -> "Done"
                            },
                            color = if (shown == 0) c.bgBase else Color.White,
                            fontSize = 15.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
            }
        }
    }
}

private val newAgentPrompt = listOf("Fix", "the", "failing", "inbox", "tests")

@Composable
internal fun TourNewAgentScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val reduced = Fm.reducedMotion
    val step = phase % 4
    val typed = when (step) {
        1 -> newAgentPrompt.take(3).joinToString(" ")
        2, 3 -> newAgentPrompt.joinToString(" ")
        else -> ""
    }
    val breathe = if (reduced) {
        1f
    } else {
        val transition = rememberInfiniteTransition(label = "tour-new-agent")
        val grow by transition.animateFloat(1f, 1.03f, infiniteRepeatable(tween(1600), RepeatMode.Reverse), label = "tour-new-agent-grow")
        grow
    }
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(34.dp)) {
        Box(
            Modifier.graphicsLayer {
                scaleX = breathe
                scaleY = breathe
            }.tourTap("New agent") { act(1) },
        ) {
            TourSparkleButton(stars = if (step >= 3) 6 else 3, size = 156.dp, lit = step == 0)
        }
        Row(
            Modifier.width(300.dp).height(52.dp).tourCard(26.dp).padding(horizontal = 18.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(if (step >= 3) "check_circle" else "auto_awesome", size = 17.dp, tint = if (step >= 3) c.success else c.accent)
            Text(
                typed.ifEmpty { "Describe a change" },
                Modifier.weight(1f),
                color = if (typed.isEmpty()) c.textTertiary else c.textPrimary,
                fontSize = 16.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            AnimatedVisibility(step >= 3, enter = fadeIn() + scaleIn(initialScale = 0.8f), exit = fadeOut()) {
                Text("Started", color = c.success, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

private class TourInboxFilter(val title: String, val icon: String, val count: Int)

private val tourInboxFilters = listOf(
    TourInboxFilter("Approvals", "verified_user", 1),
    TourInboxFilter("Reviews", "call_merge", 2),
    TourInboxFilter("Mentions", "alternate_email", 2),
    TourInboxFilter("Pings", "notifications_none_rounded", 3),
)

@Composable
internal fun TourInboxScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 4
    val open = step == 1 || step == 2
    val picked = if (step >= 2) 1 else null
    Box(contentAlignment = Alignment.TopStart) {
        Column(Modifier.width(310.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(
                Modifier.tourTarget(step == 0).tourTap("Inbox filters") { act(1) },
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Inbox", color = c.textPrimary, fontSize = 28.sp, fontWeight = FontWeight.SemiBold)
                    FmIcon("expand_more", size = 18.dp, tint = c.textSecondary)
                }
                Text(if (picked == null) "All activity · 14 unread" else "Reviews · 2 unread", color = c.textSecondary, fontSize = 14.sp)
            }
            Column(Modifier.fillMaxWidth().tourCard()) {
                TourInboxRow("RT", Color(0xFF3D8BFF), "Rahel asked for your review", "#1482 · 12m")
                if (picked == null) {
                    TourInboxRow("IB", Color(0xFF6B5A46), "Ivo mentioned you in #mesh", "1h")
                    TourInboxRow("MR", Color(0xFF2F5D50), "Mirror fmnext-ams-2 is behind", "Tue")
                } else {
                    TourInboxRow("KA", tourMerged, "Kaif asked for your review", "#1479 · 3h")
                }
            }
        }
        AnimatedVisibility(
            open,
            modifier = Modifier.offset(y = 62.dp).zIndex(1f),
            enter = fadeIn() + scaleIn(initialScale = 0.3f, transformOrigin = TransformOrigin(0f, 0f)),
            exit = fadeOut() + scaleOut(targetScale = 0.3f, transformOrigin = TransformOrigin(0f, 0f)),
        ) {
            val shadow = Color.Black.copy(alpha = if (c.dark) 0.4f else 0.12f)
            val shape = RoundedCornerShape(22.dp)
            Column(
                Modifier.width(230.dp).shadow(14.dp, shape, ambientColor = shadow, spotColor = shadow)
                    .background(c.bgRaised, shape).padding(vertical = 6.dp),
            ) {
                TourMenuRow("All activity", if (picked == null) "check" else "inbox", 14)
                HorizontalDivider(Modifier.padding(vertical = 2.dp), thickness = 0.5.dp, color = c.border)
                tourInboxFilters.forEachIndexed { at, filter ->
                    TourMenuRow(filter.title, if (picked == at) "check" else filter.icon, filter.count)
                }
            }
        }
    }
}

@Composable
private fun TourInboxRow(initials: String, color: Color, title: String, detail: String) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().height(58.dp).padding(horizontal = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TourInitial(initials, color, 32.dp, 12)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = c.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(detail, color = c.textSecondary, fontSize = 12.sp)
        }
        Box(Modifier.size(7.dp).background(c.accent, CircleShape))
    }
}

@Composable
private fun TourMenuRow(title: String, icon: String, count: Int) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().height(38.dp).padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 16.dp, tint = c.textPrimary)
        Text(title, Modifier.weight(1f), color = c.textPrimary, fontSize = 15.sp)
        Text("$count", color = c.textTertiary, fontSize = 14.sp, style = Fm.text.bodyLarge.copy(fontFeatureSettings = FmType.tabular))
    }
}

private val voiceWords = listOf("Add", "a", "dark", "mode", "to", "settings")

@Composable
internal fun TourVoiceScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val reduced = Fm.reducedMotion
    var holding by remember { mutableStateOf(false) }
    var heard by remember { mutableIntStateOf(0) }
    val step = phase % 5
    val listening = holding || step == 1 || step == 2
    val sent = !holding && step == 3
    val said = if (holding) heard else when (step) {
        1 -> 3
        2, 3 -> voiceWords.size
        else -> 0
    }
    val latestAct by rememberUpdatedState(act)
    LaunchedEffect(holding) {
        if (!holding) return@LaunchedEffect
        while (heard < voiceWords.size) {
            delay(380)
            heard += 1
        }
    }
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(18.dp)) {
        val orb by animateDpAsState(if (listening) 132.dp else 112.dp, label = "tour-voice-orb")
        Box(
            Modifier.size(220.dp)
                .pointerInput(Unit) {
                    detectTapGestures(onPress = {
                        haptics.palette()
                        heard = 0
                        latestAct(1)
                        holding = true
                        tryAwaitRelease()
                        if (holding) {
                            holding = false
                            latestAct(3)
                            haptics.success()
                        }
                    })
                }
                .semantics {
                    role = Role.Button
                    contentDescription = "New agent, hold to talk"
                    onClick { latestAct(2); true }
                },
            contentAlignment = Alignment.Center,
        ) {
            if (listening && !reduced) {
                val transition = rememberInfiniteTransition(label = "tour-voice-rings")
                for (ring in 0 until 3) {
                    val grow by transition.animateFloat(
                        0f, 1f,
                        infiniteRepeatable(tween(1400, delayMillis = ring * 450), RepeatMode.Restart),
                        label = "tour-voice-ring-$ring",
                    )
                    Box(
                        Modifier.size(orb).graphicsLayer {
                            val k = 1f + 0.7f * grow
                            scaleX = k
                            scaleY = k
                            alpha = 0.8f * (1f - grow)
                        }.border(2.dp, c.accent.copy(alpha = 0.5f), CircleShape),
                    )
                }
            }
            TourSparkleButton(stars = if (sent) 9 else 4, size = orb, lit = step == 0 && !holding)
        }
        TourWave(live = listening, modifier = Modifier.height(44.dp).graphicsLayer { alpha = if (listening) 1f else 0.25f })
        Text(
            voiceWords.take(said).joinToString(" "),
            Modifier.height(30.dp),
            color = c.textPrimary,
            fontSize = 22.sp,
            fontWeight = FontWeight.Medium,
        )
        Box(Modifier.height(22.dp), contentAlignment = Alignment.Center) {
            when {
                sent -> Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("auto_awesome", size = 15.dp, tint = c.success)
                    Text("Agent started", color = c.success, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                }
                listening -> Text("Listening", color = c.accent, fontSize = 14.sp, fontWeight = FontWeight.Medium)
                else -> Text("Touch and hold", color = c.textSecondary, fontSize = 14.sp)
            }
        }
    }
}

private val waveHeights = listOf(0.35f, 0.7f, 0.5f, 1f, 0.6f, 0.85f, 0.4f, 0.95f, 0.55f, 0.75f, 0.3f, 0.65f)

@Composable
internal fun TourWave(live: Boolean, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val moving = live && !Fm.reducedMotion
    val time = if (moving) {
        val transition = rememberInfiniteTransition(label = "tour-wave")
        val t by transition.animateFloat(0f, 6.2832f, infiniteRepeatable(tween(900)), label = "tour-wave-time")
        t
    } else {
        0f
    }
    Row(modifier.clearAndSetSemantics {}, horizontalArrangement = Arrangement.spacedBy(5.dp), verticalAlignment = Alignment.CenterVertically) {
        waveHeights.forEachIndexed { at, height ->
            val wobble = if (moving) (sin(time + at * 0.9f) + 1f) / 2f else 0.4f
            Box(Modifier.width(6.dp).height(maxOf(6.dp, 44.dp * height * (0.35f + wobble * 0.65f))).background(c.accent, CircleShape))
        }
    }
}

@Composable
internal fun TourCodeScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val step = phase % 3
    LaunchedEffect(step) { if (step == 2) haptics.success() }
    val fill by animateColorAsState(
        when (step) {
            0 -> c.textPrimary
            1 -> c.success
            else -> tourMerged
        },
        tween(400),
        label = "tour-merge",
    )
    Column(Modifier.width(320.dp).tourCard(26.dp).padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Fill the status ring", Modifier.weight(1f), color = c.textPrimary, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
            Text("#1482", color = c.textSecondary, fontSize = 13.sp)
        }
        Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
            TourDiffLine("- ring.trim(from: 0, to: 0.8)", -1)
            TourDiffLine("+ ring.trim(from: 0, to: 1)", 1)
            TourDiffLine("  .stroke(status.color)", 0)
            TourDiffLine("+ .animation(.smooth)", 1)
        }
        Row(
            Modifier.fillMaxWidth().heightIn(min = 44.dp).tourTarget(step == 0).background(fill, CircleShape)
                .tourTap(if (step == 0) "Merge" else null) { act(2) },
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val ink = if (step == 0) c.bgBase else Color.White
            FmIcon(if (step == 2) "call_merge" else "check", size = 17.dp, tint = ink)
            AnimatedContent(step, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "tour-merge-label") { shown ->
                Text(
                    when (shown) {
                        0 -> "Merge"
                        1 -> "Checks passed"
                        else -> "Merged"
                    },
                    color = ink,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }
    }
}

@Composable
private fun TourDiffLine(text: String, kind: Int) {
    val c = Fm.colors
    val add = if (c.dark) Color(0xFF4FD38A) else Color(0xFF1D8A4D)
    val del = if (c.dark) Color(0xFFFF7A7A) else Color(0xFFC43C3C)
    val ink = when {
        kind > 0 -> add
        kind < 0 -> del
        else -> c.textSecondary
    }
    Text(
        text,
        Modifier.fillMaxWidth().background(ink.copy(alpha = if (kind == 0) 0f else 0.12f), RoundedCornerShape(6.dp)).padding(horizontal = 8.dp, vertical = 2.dp),
        color = ink,
        fontSize = 13.sp,
        fontFamily = FontFamily.Monospace,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

private class TourPlugin(val title: String, val detail: String, val icon: String, val color: Color)

private val tourPlugins = listOf(
    TourPlugin("NEXUS", "Network inspection", "lan", Color(0xFF3D8BFF)),
    TourPlugin("Notes", "Workspace", "description", Color(0xFF2F5D50)),
    TourPlugin("Vault", "Personal", "shield", Color(0xFF6B5A46)),
)

@Composable
internal fun TourMarketScene(phase: Int, act: (Int) -> Unit) {
    val haptics = FmHaptics.current
    var added by remember { mutableStateOf(emptySet<Int>()) }
    val center = phase % tourPlugins.size
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        tourPlugins.forEachIndexed { at, plugin ->
            val raw = (at - center + tourPlugins.size) % tourPlugins.size
            val place = if (raw == 2) -1 else raw
            TourPluginCard(plugin, place, at in added) {
                haptics.tap()
                added = added + at
                act(phase)
            }
        }
    }
}

@Composable
private fun TourPluginCard(plugin: TourPlugin, place: Int, added: Boolean, onGet: () -> Unit) {
    val c = Fm.colors
    val centered = place == 0
    val motion = spring<Float>(dampingRatio = 0.8f, stiffness = 300f)
    val slide by animateFloatAsState(place.toFloat(), motion, label = "tour-market-slide")
    val grow by animateFloatAsState(if (centered) 1f else 0.82f, motion, label = "tour-market-scale")
    val fade by animateFloatAsState(if (centered) 1f else 0.5f, motion, label = "tour-market-fade")
    val drop by animateFloatAsState(if (centered) 0f else 14f, motion, label = "tour-market-drop")
    Column(
        Modifier
            .zIndex(if (centered) 2f else 1f)
            .graphicsLayer {
                translationX = slide * 168.dp.toPx()
                translationY = drop * density
                scaleX = grow
                scaleY = grow
                rotationZ = slide * 6f
                alpha = fade
            }
            .size(210.dp, 290.dp)
            .tourCard(32.dp)
            .then(if (centered) Modifier else Modifier.clearAndSetSemantics {}),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            Modifier.padding(top = 24.dp).size(96.dp).clip(RoundedCornerShape(28.dp)).background(plugin.color),
            contentAlignment = Alignment.Center,
        ) { FmIcon(plugin.icon, size = 42.dp, tint = Color.White) }
        Spacer(Modifier.weight(1f))
        Text(plugin.title, color = c.textPrimary, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
        Text(plugin.detail, Modifier.padding(top = 2.dp), color = c.textSecondary, fontSize = 13.sp)
        Box(
            Modifier.padding(top = 12.dp, bottom = 20.dp).size(120.dp, 34.dp)
                .graphicsLayer { alpha = if (centered) 1f else 0f }
                .background(c.textPrimary, CircleShape)
                .then(if (centered) Modifier.tourTap(if (added) "Open" else "Get", onGet) else Modifier),
            contentAlignment = Alignment.Center,
        ) {
            Text(if (added) "Open" else "Get", color = c.bgBase, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}
