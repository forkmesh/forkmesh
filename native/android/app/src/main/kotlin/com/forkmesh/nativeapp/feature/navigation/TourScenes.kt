package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.chat.RelayConnection
import com.forkmesh.nativeapp.shell.ShellBarShape
import com.forkmesh.nativeapp.shell.ShellBarStyle
import com.forkmesh.nativeapp.shell.ShellHomeIconStyle
import com.forkmesh.nativeapp.ui.kit.AvatarWithDot
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay

private val tourPurple = Color(0xFF8250DF)

@Composable
private fun TourSpinner() {
    FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
}

private class TourHomeIcon(val id: String, val title: String, val icon: String, val count: Int, val running: Boolean)

private val homeIcons = listOf(
    TourHomeIcon("agents", "Agents", "auto_awesome", 3, true), TourHomeIcon("inbox", "Inbox", "inbox", 12, false),
    TourHomeIcon("repos", "Repos", "library_books", 24, false), TourHomeIcon("code", "Code", "code", 0, false),
    TourHomeIcon("chat", "Chat", "forum", 4, false), TourHomeIcon("notes", "Notes", "description", 9, false),
    TourHomeIcon("tasks", "Tasks", "checklist", 5, false), TourHomeIcon("market", "Market", "shopping_bag", 0, false),
)

@Composable
internal fun TourHomeScene(phase: Int, act: (Int) -> Unit) {
    val focus = phase % homeIcons.size
    Column(Modifier.fillMaxSize()) {
        TourTopBar()
        TourSectionLabel("Go to")
        Column(Modifier.padding(start = 10.dp, end = 10.dp, top = 6.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            homeIcons.chunked(4).forEachIndexed { row, line ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    line.forEachIndexed { column, icon ->
                        val at = row * 4 + column
                        Box(Modifier.weight(1f)) {
                            ShellHomeTile(
                                icon.icon, icon.title, icon.count, selected = at == focus, running = icon.running, desktopOffline = false,
                                heat = if (at == focus) 0.8 else 0.0,
                            ) { act(at) }
                        }
                    }
                }
            }
        }
        Spacer(Modifier.weight(1f))
        TourBottomBar()
    }
}

@Composable
internal fun TourAgentsScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 3
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        TourTopBar(stars = if (step == 0) 1 else 2)
        TourTitle("Agents", if (step == 0) "1 needs you" else null)
        TourAppear(step == 0) {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                TourSectionLabel("Needs you")
                TourRow("back_hand", "Agent 754 · Fix login test", "Wants to run npm test", tint = c.warning, unread = true) {
                    TourPill("Allow", lit = true, modifier = Modifier.tourTap("Allow") { act(1) })
                }
            }
        }
        TourSectionLabel("Running")
        TourAppear(step >= 1) {
            TourRow("auto_awesome", "Agent 754 · Fix login test", if (step == 1) "Running npm test · 0:12" else "Writing the fix · 1:04") { TourSpinner() }
        }
        TourRow("auto_awesome", "Agent 751 · Dark mode", "Editing 4 files · 6:30") { TourSpinner() }
        TourSectionLabel("Finished")
        TourRow("check_circle", "Agent 748 · Update docs", "+42 −7 · ready to merge", tint = c.success) { TourChevron() }
    }
}

@Composable
internal fun TourNewAgentScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 4
    val blur by animateDpAsState(if (step == 1) 1.5.dp else 0.dp, label = "tour-new-blur")
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.BottomCenter) {
        Column(Modifier.fillMaxSize().blur(blur), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            TourTopBar(stars = if (step >= 2) 3 else 2, buttonLit = step == 0, onButton = { act(1) })
            TourTitle("forkmesh/app")
            TourRow("error_outline", "#91 Login fails offline", "Opened 2 hours ago", tint = c.success)
            TourRow("call_merge", "#128 Fix flaky login test", "2 checks running")
        }
        AnimatedVisibility(step == 1, enter = if (Fm.reducedMotion) fadeIn(tween(0)) else slideInVertically { it } + fadeIn(), exit = if (Fm.reducedMotion) fadeOut(tween(0)) else slideOutVertically { it } + fadeOut()) {
            val shadow = Color.Black.copy(alpha = 0.15f)
            val shape = RoundedCornerShape(22.dp)
            Column(
                Modifier.padding(10.dp).fillMaxWidth().shadow(12.dp, shape, ambientColor = shadow, spotColor = shadow).clip(shape).background(c.bgElevated).padding(14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    TourPill("forkmesh/app", filled = false)
                    TourPill("Edit", filled = false)
                    TourPill("Opus", filled = false)
                }
                Text("Fix the flaky login test and open a pull request", color = c.textPrimary, fontSize = 15.sp)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmIcon("attach_file", size = 18.dp, tint = c.textSecondary)
                    FmIcon("mic", size = 18.dp, tint = c.textSecondary)
                    Spacer(Modifier.weight(1f))
                    Box(
                        Modifier.tourTarget(true, round = true).size(32.dp).background(c.accent, CircleShape).tourTap("Send") { act(2) },
                        contentAlignment = Alignment.Center,
                    ) {
                        FmIcon("arrow_upward", size = 18.dp, tint = Color.White)
                    }
                }
            }
        }
        AnimatedVisibility(step == 2, enter = tourPop(), exit = tourPopOut(), modifier = Modifier.padding(bottom = 20.dp)) {
            Row(
                Modifier.clip(CircleShape).background(c.bgRaised).border(0.5.dp, c.border, CircleShape).padding(horizontal = 14.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("auto_awesome", size = 15.dp, tint = c.accent)
                Text("Agent started", color = c.accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

private val inboxItems = listOf(
    Triple("back_hand", "Agent 754 needs approval", "Wants to run npm test"),
    Triple("call_merge", "Review requested: #128", "Fix flaky login test"),
    Triple("alternate_email", "You were mentioned in #mesh", "\"can you take a look?\""),
    Triple("error_outline", "Assigned: #91", "Login fails offline"),
)
private val inboxFilters = listOf("All", "Pings", "Reviews", "Assigned", "Mentions", "Approvals")

@Composable
internal fun TourInboxScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 6
    val shown = minOf(inboxItems.size, step + 1)
    val read = if (step >= 5) 1 else 0
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        TourTopBar()
        TourTitle("Inbox", "${maxOf(0, shown - read)} unread")
        Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            inboxFilters.forEachIndexed { at, filter -> TourPill(filter, filled = at == 0) }
        }
        inboxItems.forEachIndexed { at, item ->
            TourAppear(at < shown) {
                val fresh = at >= read
                TourRow(item.first, item.second, item.third, unread = fresh, modifier = Modifier.tourTap(item.second) { act(5) }) {
                    TourDot(if (fresh) c.accent else Color.Transparent)
                }
            }
        }
    }
}

@Composable
internal fun TourHubScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val open = phase % 2 == 1
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            TourTopBar(avatarLit = !open, onAvatar = { act(if (open) 0 else 1) })
            Spacer(Modifier.height(6.dp))
            TourTitle("Agents")
            Spacer(Modifier.weight(1f))
            TourBottomBar()
        }
        AnimatedVisibility(open, enter = fadeIn(), exit = fadeOut()) {
            Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.18f)).tourTap { act(0) })
        }
        AnimatedVisibility(open, enter = tourPop(), exit = tourPopOut(), modifier = Modifier.align(Alignment.TopEnd).padding(top = 54.dp, end = 10.dp)) {
            val shadow = Color.Black.copy(alpha = 0.2f)
            val shape = RoundedCornerShape(18.dp)
            Column(
                Modifier.width(230.dp).shadow(14.dp, shape, ambientColor = shadow, spotColor = shadow).clip(shape).background(c.bgElevated).padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    AvatarWithDot(RelayConnection.Connected, "You", size = 34.dp)
                    Column(Modifier.weight(1f)) {
                        Text("You 🌴", color = c.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                        Text("Away until Monday", color = c.textSecondary, fontSize = 11.sp)
                    }
                    TourPill("Set a status", filled = false)
                }
                HubToggle("Pocket mode", "smartphone", false)
                HubToggle("Suggestion mode", "lightbulb", true)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    HubButton("Settings", "settings", Modifier.weight(1f))
                    HubButton("Sign out", "logout", Modifier.weight(1f))
                }
                Text("Version 1.15 · What's new", color = c.textTertiary, fontSize = 11.sp)
            }
        }
    }
}

@Composable
private fun HubToggle(title: String, icon: String, on: Boolean) {
    val c = Fm.colors
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        FmIcon(icon, size = 16.dp, tint = c.textSecondary)
        Text(title, color = c.textPrimary, fontSize = 13.sp, modifier = Modifier.weight(1f))
        Box(Modifier.size(34.dp, 20.dp).background(if (on) c.accent else c.border, CircleShape).padding(2.dp), contentAlignment = if (on) Alignment.CenterEnd else Alignment.CenterStart) {
            Box(Modifier.size(16.dp).background(Color.White, CircleShape))
        }
    }
}

@Composable
private fun HubButton(title: String, icon: String, modifier: Modifier) {
    val c = Fm.colors
    Row(
        modifier.heightIn(min = 30.dp).clip(RoundedCornerShape(9.dp)).background(c.bgRaised),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 14.dp, tint = c.textPrimary)
        Spacer(Modifier.width(4.dp))
        Text(title, color = c.textPrimary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
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
    val dim = listening || sent
    val blur by animateDpAsState(if (dim) 6.dp else 0.dp, label = "tour-voice-blur")
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(Modifier.fillMaxSize().blur(blur)) { TourTopBar(stars = if (sent) 3 else 2) }
        AnimatedVisibility(dim, enter = fadeIn(), exit = fadeOut()) {
            Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.25f)))
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(14.dp)) {
            val orb by animateDpAsState(if (listening) 96.dp else 56.dp, label = "tour-voice-orb")
            Box(
                Modifier.size(150.dp)
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
            Text(
                voiceWords.take(said).joinToString(" "),
                color = if (dim) Color.White else c.textPrimary,
                fontSize = 17.sp,
                fontWeight = FontWeight.Medium,
                modifier = Modifier.height(24.dp),
            )
            when {
                sent -> Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("auto_awesome", size = 15.dp, tint = Color.White)
                    Text("Agent started", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                }
                listening -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TourPill("Cancel", filled = false, tint = Color.White)
                    TourPill("Type", filled = false, tint = Color.White)
                    TourPill("Commands", filled = false, tint = Color.White)
                }
                else -> Text("Touch and hold", color = c.textSecondary, fontSize = 13.sp)
            }
        }
    }
}

@Composable
internal fun TourCodeScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 3
    val merged = step == 2
    val fill by animateColorAsState(if (merged) tourPurple else c.success, label = "tour-merge")
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        TourTopBar()
        Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                TourPill(if (merged) "Merged" else "Open", tint = if (merged) tourPurple else c.success)
                Text("#128", color = c.textSecondary, fontSize = 13.sp, fontWeight = FontWeight.Medium)
            }
            Text("Fix flaky login test", color = c.textPrimary, fontSize = 19.sp, fontWeight = FontWeight.Bold)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("check_circle", size = 14.dp, tint = c.success)
                    Text("12 checks passed", color = c.success, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                }
                AnimatedVisibility(step >= 1, enter = tourPop(), exit = tourPopOut()) {
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                        FmIcon("verified", size = 14.dp, tint = c.accent)
                        Text("Approved", color = c.accent, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                    }
                }
            }
        }
        Column(Modifier.padding(horizontal = 12.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(c.bgRaised)) {
            Text("LoginTest.kt", color = c.textSecondary, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, fontFamily = FontFamily.Monospace, modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp))
            DiffLine(" ", "fun testSignIn() = runTest {", null)
            DiffLine("-", "    delay(1_000)", c.danger)
            DiffLine("+", "    session.awaitReady()", c.success)
            DiffLine(" ", "    assertTrue(session.signedIn)", null)
        }
        Spacer(Modifier.weight(1f))
        Row(
            Modifier.padding(start = 16.dp, end = 16.dp, bottom = 14.dp).fillMaxWidth().tourTarget(!merged).heightIn(min = 40.dp)
                .background(fill, CircleShape).tourTap(if (merged) "Merged" else "Merge pull request") { act(if (merged) 0 else 2) },
            horizontalArrangement = Arrangement.Center,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(if (merged) "check" else "merge", size = 17.dp, tint = Color.White)
            Spacer(Modifier.width(6.dp))
            Text(if (merged) "Merged" else "Merge pull request", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
private fun DiffLine(mark: String, code: String, tint: Color?) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().background((tint ?: Color.Transparent).copy(alpha = if (tint == null) 0f else 0.14f)).padding(horizontal = 10.dp, vertical = 3.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(mark, color = tint ?: c.textTertiary, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
        Text(code, color = c.textPrimary, fontSize = 11.sp, fontFamily = FontFamily.Monospace, maxLines = 1)
    }
}

private const val PALETTE_QUERY = "new is"
private val paletteCommands = listOf(
    "New issue" to "error_outline", "New agent" to "add_circle_outline", "Switch project" to "layers",
    "Open Inbox" to "inbox", "Search everywhere" to "search",
)

@Composable
internal fun TourPaletteScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val typed = PALETTE_QUERY.take(minOf(PALETTE_QUERY.length, (phase % 9) * 2))
    val words = typed.lowercase().split(" ").filter { it.isNotEmpty() }
    val results = if (words.isEmpty()) paletteCommands else paletteCommands.filter { (title, _) -> words.all { title.lowercase().contains(it) } }
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.BottomCenter) {
        Column(Modifier.fillMaxSize()) { TourTopBar() }
        Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.15f)))
        Column(
            Modifier.fillMaxWidth().height(230.dp).clip(RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp)).background(c.bgElevated).padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { Box(Modifier.size(36.dp, 5.dp).background(c.border, CircleShape)) }
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(c.bgRaised).padding(10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("keyboard_command_key", size = 17.dp, tint = c.textSecondary)
                Text(if (typed.isEmpty()) "Type a command or a place" else typed, color = if (typed.isEmpty()) c.textTertiary else c.textPrimary, fontSize = 15.sp)
                Box(Modifier.size(2.dp, 18.dp).background(c.accent))
            }
            Text(
                (if (typed.isEmpty()) "Suggested" else "Results").uppercase(),
                color = c.textTertiary, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.6.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
            results.forEachIndexed { at, (title, icon) ->
                val first = at == 0
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(if (first) c.accentSubtle else Color.Transparent)
                        .tourTap(title) { act(0) }.padding(horizontal = 10.dp, vertical = 7.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FmIcon(icon, size = 18.dp, tint = c.accent)
                    Text(title, color = c.textPrimary, fontSize = 14.sp, fontWeight = if (first) FontWeight.SemiBold else FontWeight.Normal, modifier = Modifier.weight(1f))
                    if (first) FmIcon("keyboard_return", size = 13.dp, tint = c.textTertiary)
                }
            }
        }
    }
}

private class TourHit(val bucket: String, val icon: String, val title: String, val detail: String)

private val searchScopes = listOf("All", "Code", "Issues & PRs", "Chat", "People")
private val searchResults = listOf(
    listOf(
        TourHit("Repositories", "library_books", "forkmesh/app", "Kotlin · updated today"),
        TourHit("Issues", "error_outline", "#91 Login fails offline", "forkmesh/app"),
        TourHit("Chat", "chat_bubble_outline", "\"login is fixed on main\"", "#mesh"),
    ),
    listOf(
        TourHit("Code", "article", "LoginScreen.kt", "fun signIn(token: String)"),
        TourHit("Code", "article", "LoginTest.kt", "fun testSignIn()"),
    ),
    listOf(
        TourHit("Pull requests", "call_merge", "#128 Fix flaky login test", "Open · 12 checks passed"),
        TourHit("Issues", "error_outline", "#91 Login fails offline", "Assigned to you"),
    ),
    listOf(
        TourHit("Chat", "chat_bubble_outline", "\"login is fixed on main\"", "#mesh · 2 hours ago"),
        TourHit("Chat", "chat_bubble_outline", "\"who owns login?\"", "Office room"),
    ),
    listOf(
        TourHit("People", "account_circle", "Alex", "Works on login"),
        TourHit("People", "account_circle", "Sam", "Reviewed #128"),
    ),
)

@Composable
internal fun TourSearchScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val scope = phase % searchScopes.size
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(
            Modifier.padding(start = 12.dp, end = 12.dp, top = 14.dp).fillMaxWidth().background(c.bgRaised, CircleShape).padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("search", size = 17.dp, tint = c.textSecondary)
            Text("login", color = c.textPrimary, fontSize = 15.sp, modifier = Modifier.weight(1f))
            FmIcon("cancel", size = 17.dp, tint = c.textTertiary)
        }
        Row(Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            searchScopes.forEachIndexed { at, name ->
                TourPill(name, filled = at == scope, modifier = Modifier.tourTap(name) { act(at) })
            }
        }
        val list = searchResults[scope]
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            list.forEachIndexed { at, hit ->
                if (at == 0 || list[at - 1].bucket != hit.bucket) TourSectionLabel(hit.bucket)
                TourRow(hit.icon, hit.title, hit.detail)
            }
        }
    }
}

@Composable
internal fun TourBarScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val shape = ShellBarShape.entries[phase % ShellBarShape.entries.size]
    val menu = phase % 2 == 1
    val shown by animateFloatAsState(if (menu) 1f else 0f, label = "tour-bar-menu")
    Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(10.dp)) {
        TourTopBar()
        Spacer(Modifier.weight(1f))
        val shadow = Color.Black.copy(alpha = 0.15f)
        val box = RoundedCornerShape(14.dp)
        Column(
            Modifier.width(200.dp).graphicsLayer {
                alpha = shown
                val k = 0.85f + 0.15f * shown
                scaleX = k
                scaleY = k
                transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0.5f, 1f)
            }.shadow(10.dp, box, ambientColor = shadow, spotColor = shadow).clip(box).background(c.bgElevated),
        ) {
            listOf("Move" to "swap_horiz", "Move to folder" to "folder", "Hide" to "visibility_off", "Bottom bar settings" to "tune").forEach { (title, icon) ->
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 7.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(title, color = c.textPrimary, fontSize = 13.sp, modifier = Modifier.weight(1f))
                    FmIcon(icon, size = 15.dp, tint = c.textSecondary)
                }
            }
        }
        Text(shape.title, color = c.accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        Box(Modifier.tourTap("Change the bar's shape") { act(phase + 1) }) {
            TourBottomBar(ShellBarStyle(shape = shape))
        }
    }
}

private class TourMessage(val mine: Boolean, val who: String, val text: String)

private val chatMessages = listOf(
    TourMessage(false, "Alex", "login is fixed on main 🎉"),
    TourMessage(false, "Sam", "nice, can someone check the release notes?"),
    TourMessage(true, "You", "on it, starting an agent now"),
)

@Composable
internal fun TourChatScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 5
    val shown = minOf(chatMessages.size, step + 1)
    val reacted = step >= 3
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth().padding(start = 14.dp, end = 14.dp, top = 14.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("#", color = c.textSecondary, fontSize = 18.sp, fontWeight = FontWeight.Bold)
            Text("mesh", color = c.textPrimary, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            TourSparkleButton(stars = 2, size = 30.dp)
            AvatarWithDot(RelayConnection.Connected, "You", size = 28.dp)
        }
        Spacer(Modifier.weight(1f))
        chatMessages.forEachIndexed { at, message ->
            TourAppear(at < shown) { ChatBubble(message, at == 0 && reacted) { act(3) } }
        }
        Row(
            Modifier.padding(start = 12.dp, end = 12.dp, bottom = 12.dp).fillMaxWidth().background(c.bgRaised, CircleShape).padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("add_circle_outline", size = 17.dp, tint = c.textSecondary)
            Text("Message #mesh", color = c.textTertiary, fontSize = 14.sp, modifier = Modifier.weight(1f))
            FmIcon("mic", size = 17.dp, tint = c.textSecondary)
        }
    }
}

@Composable
private fun ChatBubble(message: TourMessage, reacted: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, bottom = if (reacted) 8.dp else 0.dp)) {
        if (message.mine) Spacer(Modifier.weight(1f))
        Column(
            Modifier.widthIn(max = 240.dp),
            horizontalAlignment = if (message.mine) Alignment.End else Alignment.Start,
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            if (!message.mine) Text(message.who, color = c.textSecondary, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
            Box {
                Text(
                    message.text,
                    color = if (message.mine) Color.White else c.textPrimary,
                    fontSize = 14.sp,
                    modifier = Modifier.clip(RoundedCornerShape(18.dp)).background(if (message.mine) c.accent else c.bgRaised)
                        .tourTap(message.text, onTap).padding(horizontal = 12.dp, vertical = 8.dp),
                )
                androidx.compose.animation.AnimatedVisibility(reacted, enter = tourPop(), exit = tourPopOut(), modifier = Modifier.align(Alignment.BottomEnd).offset(x = 10.dp, y = 12.dp)) {
                    Text(
                        "👍 2",
                        fontSize = 11.sp,
                        color = c.textPrimary,
                        modifier = Modifier.background(c.bgElevated, CircleShape).border(0.5.dp, c.border, CircleShape).padding(horizontal = 6.dp, vertical = 2.dp),
                    )
                }
            }
        }
        if (!message.mine) Spacer(Modifier.weight(1f))
    }
}


private val tourProjects = listOf("Personal" to "Your own repositories", "Team" to "Shared with 6 people", "Open source" to "Public work")

@Composable
internal fun TourProjectsScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val open = phase % 4 != 0
    val current = (phase / 4) % tourProjects.size
    val name = tourProjects[current].first
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            TourTopBar(project = name, projectLit = !open, onProject = { act(if (open) phase - phase % 4 + 4 else phase + 1) })
            Spacer(Modifier.height(6.dp))
            TourTitle("Agents")
            Spacer(Modifier.weight(1f))
            TourBottomBar()
        }
        AnimatedVisibility(open, enter = tourPop(), exit = tourPopOut(), modifier = Modifier.padding(top = 52.dp, start = 10.dp)) {
            val shadow = Color.Black.copy(alpha = 0.2f)
            val shape = RoundedCornerShape(18.dp)
            Column(
                Modifier.width(250.dp).shadow(14.dp, shape, ambientColor = shadow, spotColor = shadow).clip(shape).background(c.bgElevated).padding(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                tourProjects.forEachIndexed { at, (project, detail) ->
                    val next = at == (current + 1) % tourProjects.size && phase % 4 == 3
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(if (next) c.accentSubtle else Color.Transparent)
                            .tourTap(project) { act(at * 4) }.padding(8.dp),
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Box(Modifier.size(30.dp).clip(RoundedCornerShape(8.dp)).background(c.accent.copy(alpha = 0.85f - at * 0.2f)), contentAlignment = Alignment.Center) {
                            Text(project.take(1), color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold)
                        }
                        Column(Modifier.weight(1f)) {
                            Text(project, color = c.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                            Text(detail, color = c.textSecondary, fontSize = 11.sp)
                        }
                        if (at == current) FmIcon("check", size = 15.dp, tint = c.accent)
                    }
                }
                Row(Modifier.padding(8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("person_add", size = 15.dp, tint = c.accent)
                    Text("Invite people to $name", color = c.accent, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                }
            }
        }
    }
}

@Composable
internal fun TourDesktopsScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val step = phase % 4
    val progress by animateFloatAsState(
        when (step) {
            1 -> 0.35f
            2 -> 0.8f
            3 -> 1f
            else -> 0f
        },
        label = "tour-desktop-progress",
    )
    val status = when (step) {
        0 -> "Build the app"
        3 -> "Build the app · done"
        else -> "Build the app · running"
    }
    val button = when (step) {
        0 -> "Run"
        3 -> "Again"
        else -> "Stop"
    }
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        TourTopBar()
        TourTitle("Desktops")
        Column(
            Modifier.padding(horizontal = 12.dp).fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(c.bgRaised).padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                FmIcon("desktop_windows", size = 20.dp, tint = c.accent)
                Column(Modifier.weight(1f)) {
                    Text("Studio desktop", color = c.textPrimary, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    Text("Online · 3 agents running", color = c.textSecondary, fontSize = 11.sp)
                }
                TourDot(c.success)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                FmIcon("construction", size = 17.dp, tint = c.textSecondary)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(status, color = c.textPrimary, fontSize = 13.sp)
                    FmLinearProgressIndicator(
                        Modifier.fillMaxWidth().graphicsLayer { alpha = if (step == 0) 0f else 1f },
                        progress = { progress },
                        color = if (step == 3) c.success else c.accent,
                    )
                }
                TourPill(button, filled = step == 0, lit = step == 0, modifier = Modifier.tourTap(button) { act(if (step == 0) 1 else 0) })
            }
        }
        TourRow("laptop", "Laptop", "Offline since 9:12", tint = c.textSecondary) { TourDot(c.warning) }
        TourRow("dns", "Hosts & Network", "2 hosts · 3 relays healthy") { TourChevron() }
    }
}

private val workspaceTasks = listOf("Write the release notes", "Review #128", "Book the design review")
private val workspaceDays = listOf("M", "T", "W", "T", "F", "S", "S")
private const val WORKSPACE_TODAY = 4

@Composable
internal fun TourWorkspaceScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val checked = phase % (workspaceTasks.size + 1)
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        TourTopBar()
        TourTitle("Tasks", "${workspaceTasks.size - checked} left")
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp)) {
            workspaceDays.forEachIndexed { at, day ->
                val today = at == WORKSPACE_TODAY
                val busy = at == 1 || today
                Column(Modifier.weight(1f), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text(day, color = c.textTertiary, fontSize = 10.sp, fontWeight = FontWeight.Medium)
                    Box(Modifier.size(26.dp).background(if (today) c.accent else Color.Transparent, CircleShape), contentAlignment = Alignment.Center) {
                        Text("${20 + at}", color = if (today) Color.White else c.textPrimary, fontSize = 13.sp, fontWeight = if (today) FontWeight.Bold else FontWeight.Normal)
                    }
                    Box(Modifier.size(4.dp).background(if (busy) c.accent else Color.Transparent, CircleShape))
                }
            }
        }
        workspaceTasks.forEachIndexed { at, task ->
            val finished = at < checked
            val bounce by animateFloatAsState(if (finished) 1f else 0f, label = "tour-task-$at")
            Row(
                Modifier.padding(horizontal = 12.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(c.bgRaised)
                    .tourTap(task) { act(if (finished) at else at + 1) }.padding(horizontal = 12.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.tourTarget(at == checked, round = true).scale(1f + 0.15f * bounce * (1f - bounce) * 4f)) {
                    FmIcon(if (finished) "check_circle" else "radio_button_unchecked", size = 20.dp, tint = if (finished) c.accent else c.textTertiary)
                }
                Text(
                    task,
                    color = if (finished) c.textTertiary else c.textPrimary,
                    fontSize = 14.sp,
                    textDecoration = if (finished) TextDecoration.LineThrough else null,
                )
            }
        }
    }
}

private val marketPackages = listOf(Triple("Notes", "description", "Markdown notes"), Triple("Water", "water_drop", "Count your glasses"), Triple("Vault", "shield", "Passwords and secrets"))

@Composable
internal fun TourMarketScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val installed = phase % (marketPackages.size + 1)
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        TourTopBar()
        Box(
            Modifier.padding(horizontal = 12.dp).fillMaxWidth().height(70.dp).clip(RoundedCornerShape(14.dp)).background(c.bgRaised.copy(alpha = 0.5f)).padding(horizontal = 12.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            if (installed == 0) Text("Home", color = c.textTertiary, fontSize = 12.sp, modifier = Modifier.align(Alignment.Center))
            Row {
                marketPackages.forEachIndexed { at, (title, icon, _) ->
                    AnimatedVisibility(at < installed, enter = tourPop(), exit = tourPopOut()) {
                        Box(Modifier.width(64.dp)) { ShellHomeTile(icon, title, 0, selected = false, running = false, desktopOffline = false) {} }
                    }
                }
            }
        }
        TourTitle("Market")
        marketPackages.forEachIndexed { at, (title, icon, detail) ->
            val got = at < installed
            TourRow(icon, title, detail) {
                TourPill(
                    if (got) "Installed" else "Get",
                    filled = !got,
                    tint = if (got) c.success else null,
                    lit = at == installed,
                    modifier = Modifier.tourTap(if (got) "Installed" else "Get $title") { act(if (got) at else at + 1) },
                )
            }
        }
    }
}

@Composable
internal fun TourPersonalScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val glasses = phase % 9
    val level by animateFloatAsState(glasses / 8f, label = "tour-water")
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        TourTopBar()
        Row(Modifier.padding(horizontal = 20.dp), horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
            val glass = RoundedCornerShape(bottomStart = 14.dp, bottomEnd = 14.dp)
            Box(
                Modifier.tourTarget(glasses < 8).size(64.dp, 100.dp).clip(glass).background(c.bgRaised).border(2.dp, c.accent, glass)
                    .tourTap("Add a glass of water") { act(glasses + 1) },
                contentAlignment = Alignment.BottomCenter,
            ) {
                Box(Modifier.fillMaxWidth().height(100.dp * level).background(c.accent.copy(alpha = 0.75f)))
            }
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Water", color = c.textSecondary, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Text("$glasses of 8", color = c.textPrimary, fontSize = 28.sp, fontWeight = FontWeight.Bold)
                Text(if (glasses >= 8) "Done for today" else "glasses today", color = c.textSecondary, fontSize = 12.sp)
            }
        }
        TourRow("shield", "github.com", "••••••••••••") { FmIcon("content_copy", size = 15.dp, tint = c.textSecondary) }
        TourRow("directions_walk", "Steps", "6,240 of 8,000", tint = c.success) {
            FmLinearProgressIndicator(Modifier.width(60.dp), progress = { 0.78f }, color = c.success)
        }
    }
}

@Composable
internal fun TourSystemScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val approved = phase % 3 == 2
    val shown = phase % 3 != 0
    Box(
        Modifier.fillMaxSize().background(
            Brush.linearGradient(listOf(c.accent.copy(alpha = 0.85f), Color(0xFF4B3FB5).copy(alpha = 0.9f), Color.Black.copy(alpha = 0.85f))),
        ),
    ) {
        Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("9:41", color = Color.White.copy(alpha = 0.92f), fontSize = 52.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 14.dp))
            Row(
                Modifier.padding(horizontal = 30.dp).fillMaxWidth().background(Color.Black.copy(alpha = 0.55f), CircleShape).padding(horizontal = 14.dp, vertical = 9.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("auto_awesome", size = 14.dp, tint = Color.White)
                Text(if (approved) "3 running" else "2 running · 1 needs you", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Text("6:30", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, style = androidx.compose.ui.text.TextStyle(fontFeatureSettings = "tnum"))
            }
            AnimatedVisibility(
                shown,
                enter = if (Fm.reducedMotion) fadeIn(tween(0)) else slideInVertically { -it } + fadeIn(),
                exit = if (Fm.reducedMotion) fadeOut(tween(0)) else slideOutVertically { -it } + fadeOut(),
            ) {
                Column(
                    Modifier.padding(horizontal = 14.dp).fillMaxWidth().clip(RoundedCornerShape(20.dp)).background(Color.White.copy(alpha = 0.18f)).padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(22.dp).clip(RoundedCornerShape(6.dp)).background(c.accent), contentAlignment = Alignment.Center) {
                            FmIcon("add", size = 14.dp, tint = Color.White)
                        }
                        Text("ForkMesh", color = Color.White.copy(alpha = 0.7f), fontSize = 11.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                        Text("now", color = Color.White.copy(alpha = 0.6f), fontSize = 11.sp)
                    }
                    Text(if (approved) "Approved: Agent 754 runs npm test" else "Agent 754 wants to run npm test", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    if (!approved) {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Box(
                                Modifier.weight(1f).tourTarget(true).heightIn(min = 32.dp).background(Color.White.copy(alpha = 0.25f), CircleShape).tourTap("Approve") { act(2) },
                                contentAlignment = Alignment.Center,
                            ) { Text("Approve", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold) }
                            Box(
                                Modifier.weight(1f).heightIn(min = 32.dp).background(Color.White.copy(alpha = 0.12f), CircleShape),
                                contentAlignment = Alignment.Center,
                            ) { Text("Deny", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold) }
                        }
                    }
                }
            }
        }
    }
}

private val looksSamples = listOf(Triple("Agents", "auto_awesome", 3), Triple("Inbox", "inbox", 12), Triple("Repos", "library_books", 0), Triple("Notes", "description", 0))

@Composable
internal fun TourLooksScene(phase: Int, act: (Int) -> Unit) {
    val c = Fm.colors
    val styles = ShellHomeIconStyle.entries
    val style = styles[phase % styles.size]
    Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        TourTopBar()
        Spacer(Modifier.weight(1f))
        Row(
            Modifier.padding(horizontal = 12.dp).fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(c.bgRaised.copy(alpha = 0.5f))
                .tourTap("Change the icon style") { act(phase + 1) }.padding(horizontal = 8.dp, vertical = 8.dp),
        ) {
            looksSamples.forEachIndexed { index, (title, icon, count) ->
                Box(Modifier.weight(1f)) {
                    ShellHomeTile(icon, title, count, selected = false, running = index == 0, desktopOffline = false, iconStyle = style, heat = if (phase % 2 == 1 && index == 1) 0.9 else 0.0) { act(phase + 1) }
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            styles.forEachIndexed { at, option ->
                TourPill(option.title, filled = option == style, modifier = Modifier.tourTap(option.title) { act(at) })
            }
        }
        Spacer(Modifier.weight(1f))
        TourBottomBar(ShellBarStyle(icons = style))
    }
}
