package com.forkmesh.nativeapp.feature.teammates

import com.forkmesh.nativeapp.feature.market.rememberScreenPluginMenu
import android.graphics.BitmapFactory
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridItemSpanScope
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.collab.ReferenceLinks
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.feature.agents.ChipRow
import com.forkmesh.nativeapp.feature.agents.opsIo
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.code.FmFilledButton
import com.forkmesh.nativeapp.feature.code.FmOutlinedButton
import com.forkmesh.nativeapp.teammates.BriefingEntry
import com.forkmesh.nativeapp.teammates.TeamBriefing
import com.forkmesh.nativeapp.teammates.TeamConfirmation
import com.forkmesh.nativeapp.teammates.TeamErrorText
import com.forkmesh.nativeapp.teammates.TeamRoomMessage
import com.forkmesh.nativeapp.teammates.TeamStats
import com.forkmesh.nativeapp.teammates.TeammateAction
import com.forkmesh.nativeapp.teammates.TeammateDetail
import com.forkmesh.nativeapp.teammates.TeammateRow
import com.forkmesh.nativeapp.teammates.TeammateStatus
import com.forkmesh.nativeapp.teammates.TeammatesStore
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmMessageActions
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.LinkedText
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.util.Locale
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.sin
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

internal data class PendingTeamConfirmation(
    val title: String,
    val confirmation: TeamConfirmation,
    val destructive: Boolean = false,
    val openedAtMs: Long = System.currentTimeMillis(),
)

@Composable
internal fun TeamConfirmDialog(
    pending: PendingTeamConfirmation,
    perform: (TeamConfirmation) -> String,
    onDone: (String) -> Unit,
    onError: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var running by remember(pending) { mutableStateOf(false) }
    FmConfirmDialog(
        title = pending.title,
        body = pending.confirmation.summary + "\n\nThe desktop carries this out and writes it to its log.",
        confirmLabel = pending.title,
        destructive = pending.destructive,
        busy = running,
        onConfirm = {
            if (System.currentTimeMillis() - pending.openedAtMs > pending.confirmation.expiresInMs) {
                onError(TeamErrorText.describe("confirmation_expired"))
                onDismiss()
                return@FmConfirmDialog
            }
            running = true
            scope.launch {
                try {
                    onDone(opsIo { perform(pending.confirmation) })
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    onError(TeamErrorText.describe(failure))
                } finally {
                    running = false
                    onDismiss()
                }
            }
        },
        onDismissRequest = onDismiss,
    )
}

private val fullRow: LazyGridItemSpanScope.() -> GridItemSpan = { GridItemSpan(maxLineSpan) }

private val teammatePortraits = setOf(
    "blogger", "ceo", "cfo", "code-steward", "coo", "cpa", "crawler", "fixer", "hearth", "herald", "marketing",
    "mobile-developer", "quality", "reaper", "reddit", "sage", "sales", "sentinel", "tempo", "ux", "yc-news",
)
private val modelFaces = setOf("opus", "sonnet", "haiku", "fable", "sol", "luna", "terra")
private val faceCache = HashMap<String, ImageBitmap?>()

@Composable
internal fun rememberTeammateFace(avatar: String): ImageBitmap? {
    val context = LocalContext.current
    val key = avatar.trim().lowercase(Locale.ROOT)
    return remember(key) {
        synchronized(faceCache) {
            faceCache.getOrPut(key) {
                val path = when (key) {
                    in teammatePortraits -> "teammate_faces/$key.jpg"
                    in modelFaces -> "agent_faces/$key.png"
                    else -> null
                }
                path?.let { asset -> runCatching { context.assets.open(asset).use { BitmapFactory.decodeStream(it) }?.asImageBitmap() }.getOrNull() }
            }
        }
    }
}

private val avatarPalette = listOf(
    Color(0xFF3B82F6), Color(0xFF8B5CF6), Color(0xFFF97316), Color(0xFF14B8A6),
    Color(0xFFEC4899), Color(0xFF6366F1), Color(0xFF22C55E), Color(0xFFEF4444),
)

@Composable
internal fun TeammateAvatar(name: String, avatar: String, size: Int = 40) {
    val seed = (avatar.ifEmpty { name }).sumOf { it.code }
    val initials = name.split(' ').filter { it.isNotEmpty() }.take(2).joinToString("") { it.first().uppercase() }.ifEmpty { "?" }
    val face = rememberTeammateFace(avatar)
    Box(
        Modifier.size(size.dp).clip(CircleShape).background(avatarPalette[kotlin.math.abs(seed) % avatarPalette.size]),
        contentAlignment = Alignment.Center,
    ) {
        if (face != null) {
            Image(face, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
        } else {
            Text(initials, style = Fm.text.labelLarge.copy(color = Color.White, fontSize = (size * 0.38).sp, fontWeight = FmType.emphasis))
        }
    }
}

@Composable
internal fun TeammateStatusBadge(status: TeammateStatus) {
    val c = Fm.colors
    val tint = when (status) {
        TeammateStatus.Working -> c.success
        TeammateStatus.Queued, TeammateStatus.Waiting -> c.warning
        TeammateStatus.Scheduled -> c.accent
        TeammateStatus.Paused -> c.textDisabled
        TeammateStatus.Idle -> c.textTertiary
    }
    FmStatusBadge(status.wire, tint)
}

@Composable
internal fun DesktopReachText(reach: DesktopReach) {
    val c = Fm.colors
    val color = when (reach) {
        is DesktopReach.Connected -> c.success
        is DesktopReach.Connecting -> c.warning
        is DesktopReach.Offline -> c.danger
        DesktopReach.NotPaired -> c.textDisabled
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).background(color, CircleShape))
        Spacer(Modifier.width(FmSpace.x2))
        Text(reach.label, style = Fm.text.bodySmall.copy(color = c.textSecondary))
    }
}

@Composable
internal fun OutdatedRow() {
    Text(
        "This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.",
        style = Fm.text.bodySmall.copy(color = Fm.colors.warning),
    )
}

@Composable
internal fun Banner(text: String, error: Boolean) {
    if (text.isEmpty()) return
    val c = Fm.colors
    Row(verticalAlignment = Alignment.CenterVertically) {
        FmIcon(if (error) "error_outline" else "check_circle_outline", size = 16.dp, tint = if (error) c.danger else c.success)
        Spacer(Modifier.width(FmSpace.x2))
        Text(text, style = Fm.text.bodySmall.copy(color = if (error) c.danger else c.success))
    }
}

internal class TeammatesScreen : Screen() {
    override val title: String get() = "Special Agents"
    private var filter by mutableStateOf<TeammateStatus?>(null)
    private var pending by mutableStateOf<PendingTeamConfirmation?>(null)
    private var banner by mutableStateOf("")
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val tick = rememberOpsTick(store)
        val scope = rememberCoroutineScope()
        var menu by remember { mutableStateOf(false) }
        var repoMenu by remember { mutableStateOf(false) }
        LaunchedEffect(store) {
            opsIo {
                store.refresh()
                store.loadOptions()
            }
        }
        fun prepare(action: TeammateAction, destructive: Boolean = false) {
            scope.launch {
                try {
                    pending = PendingTeamConfirmation(action.title, opsIo { store.prepare(action) }, destructive)
                    error = ""
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                }
            }
        }
        val plugin = rememberScreenPluginMenu()
        DetailScaffold(title, navigator, pluginMenu = false, actions = {
            plugin?.Host()
            FmIconButton("newspaper", "Briefing", onPressed = { navigator.push(TeamBriefingScreen()) }, color = c.textPrimary)
            FmIconButton("bar_chart", "Stats", onPressed = { navigator.push(TeamStatsScreen()) }, color = c.textPrimary)
            FmIconButton("forum_outlined", "#team", onPressed = { navigator.push(TeamRoomScreen()) }, color = c.textPrimary)
            Box {
                FmIconButton("more_vert", "More", onPressed = { menu = true }, color = c.textPrimary)
                FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    FmDropdownMenuItem("Run the team now", icon = "play_arrow", onClick = { menu = false; prepare(TeammateAction.RunTeam) })
                    FmDropdownMenuItem("Generate briefing", icon = "newspaper", onClick = { menu = false; prepare(TeammateAction.Briefing(null)) })
                    FmDropdownMenuItem(
                        "Pause all schedules",
                        icon = "pause_circle_outline",
                        textColor = c.danger,
                        onClick = { menu = false; prepare(TeammateAction.DisableSchedules, destructive = true) },
                    )
                    FmDropdownMenuItem("Add special agent", icon = "person_add_alt", onClick = { menu = false; navigator.push(TeammateEditorScreen(null)) })
                    plugin?.Items(close = { menu = false }, divided = true)
                }
            }
        }) {
            tick.let { }
            val snapshot = store.snapshot
            LazyVerticalGrid(
                columns = GridCells.Adaptive(64.dp),
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                item(span = fullRow) {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                        DesktopReachText(TeammatesFeature.hub.reach)
                        if (store.unsupported) OutdatedRow()
                        if (snapshot != null) {
                            Box {
                                FmOutlinedButton(
                                    snapshot.repo.ifEmpty { "Choose a repository" },
                                    onClick = if (snapshot.repos.isEmpty()) null else ({ repoMenu = true }),
                                    icon = "folder_outlined",
                                )
                                FmDropdownMenu(expanded = repoMenu, onDismissRequest = { repoMenu = false }) {
                                    for (repo in snapshot.repos) {
                                        FmDropdownMenuItem(repo, icon = if (repo == snapshot.repo) "check" else null, onClick = {
                                            repoMenu = false
                                            scope.launch { opsIo { store.selectRepo(repo) } }
                                        })
                                    }
                                }
                            }
                            Text(snapshot.summary.line, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                        Banner(banner, false)
                        val message = error.ifEmpty { store.error }
                        if (!store.unsupported) Banner(message, true)
                    }
                }
                if (snapshot != null) {
                    item(span = fullRow) {
                        ChipRow {
                            FilterChip("All ${snapshot.summary.total}", filter == null) { filter = null }
                            FilterChip("Working ${snapshot.summary.working}", filter == TeammateStatus.Working) { filter = TeammateStatus.Working }
                            FilterChip("Scheduled ${snapshot.summary.scheduled}", filter == TeammateStatus.Scheduled) { filter = TeammateStatus.Scheduled }
                            FilterChip("Idle", filter == TeammateStatus.Idle) { filter = TeammateStatus.Idle }
                        }
                    }
                    val rows = snapshot.filtered(filter)
                    if (rows.isEmpty()) {
                        item(span = fullRow) {
                            Text(
                                if (snapshot.available) "No special agents here yet." else "Pick a repository with a working tree.",
                                style = Fm.text.bodyMedium.copy(color = c.textSecondary),
                            )
                        }
                    }
                    items(rows, key = { it.id }) { row ->
                        TeammateTile(row, onTap = { navigator.push(TeammateScreen(row.id)) }, onAction = { action, destructive -> prepare(action, destructive) })
                    }
                    for (line in snapshot.errors) item(span = fullRow) { Banner(line, true) }
                } else if (store.loading) {
                    item(span = fullRow) { Box(Modifier.fillMaxWidth().padding(FmSpace.x5), contentAlignment = Alignment.Center) { CircularProgressIndicator() } }
                } else if (store.error.isNotEmpty() || store.unsupported) {
                    item(span = fullRow) {
                        Box(Modifier.fillMaxWidth().height(240.dp)) {
                            FmEmptyState(icon = "groups_outlined", title = "Special agents are not available", message = "Sign in, then pair your desktop on the same network.")
                        }
                    }
                }
            }
            pending?.let { current ->
                TeamConfirmDialog(
                    current,
                    perform = { store.run(it) },
                    onDone = { banner = it.ifEmpty { "Done." } },
                    onError = { error = it },
                    onDismiss = { pending = null },
                )
            }
        }
    }
}

@Composable
internal fun FilterChip(label: String, selected: Boolean, onTap: () -> Unit) {
    val c = Fm.colors
    FmStatusBadge(label, if (selected) c.accent else c.textSecondary, Modifier.clip(RoundedCornerShape(FmRadius.full)).clickable(onClick = onTap))
}

private class TilePose(val scale: Float, val x: Float, val y: Float, val roll: Float)

private fun tilePose(motion: String, time: Float, phase: Float): TilePose {
    val t = time + phase * 60f
    fun wave(period: Float, shift: Float = 0f) = sin(2f * PI.toFloat() * (t / period + shift))
    val calm = if (motion == "still") 0.3f else 1f
    var scale = 1.1f + 0.025f * calm * wave(4.2f)
    var x = 0.012f * calm * wave(7.3f, phase)
    var y = 0.01f * calm * wave(5.9f, phase * 2)
    var roll = 0.8f * calm * wave(6.7f, phase)
    when (motion) {
        "wave" -> {
            val burst = max(0f, wave(3f))
            roll += 7f * burst * wave(0.9f)
            scale += 0.05f * burst
        }
        "jump" -> y -= 0.06f * max(0f, wave(1.2f))
        "bounce" -> y -= 0.03f * abs(wave(1f))
        "nod" -> y += 0.025f * max(0f, wave(1.4f))
        "pulse" -> scale += 0.06f * (0.5f + 0.5f * wave(1.1f))
        "wiggle" -> roll += 4f * wave(0.6f)
        "shake" -> x += 0.025f * wave(0.18f) * max(0f, wave(2.5f))
        "spin" -> roll += 10f * wave(1.6f)
        "doze" -> {
            y += 0.025f * (0.5f + 0.5f * wave(4f))
            roll += 3f * wave(8f)
        }
    }
    return TilePose(scale, x, y, roll)
}

@Composable
private fun TeammateTile(row: TeammateRow, onTap: () -> Unit, onAction: (TeammateAction, Boolean) -> Unit) {
    val c = Fm.colors
    val face = rememberTeammateFace(row.avatar)
    val clock by rememberInfiniteTransition(label = "teammateFace").animateFloat(
        initialValue = 0f,
        targetValue = 600f,
        animationSpec = infiniteRepeatable(tween(600_000, easing = LinearEasing)),
        label = "teammateFaceClock",
    )
    val paused = row.status == TeammateStatus.Paused
    val motion = if (paused) "doze" else row.mood.motion.takeIf { !row.mood.isEmpty } ?: "float"
    val phase = remember(row.id) { (row.id.fold(0) { acc, ch -> (acc * 31 + ch.code) and 0xffff } % 1000) / 1000f }
    val playTint = when (row.status) {
        TeammateStatus.Working -> c.success
        TeammateStatus.Queued, TeammateStatus.Waiting -> c.warning
        TeammateStatus.Paused -> c.textDisabled
        TeammateStatus.Scheduled, TeammateStatus.Idle -> c.accent
    }
    val edge = when (row.status) {
        TeammateStatus.Working -> c.success
        TeammateStatus.Queued, TeammateStatus.Waiting -> c.warning
        TeammateStatus.Scheduled -> c.accent
        TeammateStatus.Paused -> c.textDisabled
        TeammateStatus.Idle -> Color.White.copy(alpha = 0.08f)
    }
    val shape = RoundedCornerShape(10.dp)
    var menu by remember { mutableStateOf(false) }
    val description = listOf(row.name, row.specialty, row.status.wire, row.mood.label, row.lastActive).filter { it.isNotEmpty() }.joinToString(", ")
    Box(
        Modifier
            .aspectRatio(1f)
            .clip(shape)
            .background(c.bgElevated)
            .clickable(onClick = onTap)
            .semantics { contentDescription = description }
            .drawWithContent {
                drawContent()
                val stroke = when (row.status) {
                    TeammateStatus.Working, TeammateStatus.Queued, TeammateStatus.Waiting ->
                        Stroke(2.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(5.dp.toPx(), 4.dp.toPx()), -(clock * 20f % 9f).dp.toPx()))
                    TeammateStatus.Paused -> Stroke(1.5.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(3.dp.toPx(), 3.dp.toPx())))
                    TeammateStatus.Scheduled -> Stroke(1.5.dp.toPx())
                    TeammateStatus.Idle -> Stroke(0.5.dp.toPx())
                }
                val inset = stroke.width / 2
                drawRoundRect(
                    edge,
                    topLeft = Offset(inset, inset),
                    size = Size(size.width - stroke.width, size.height - stroke.width),
                    cornerRadius = CornerRadius(10.dp.toPx() - inset),
                    style = stroke,
                )
            },
    ) {
        Box(
            Modifier.fillMaxSize().graphicsLayer {
                val pose = tilePose(motion, clock, phase)
                scaleX = pose.scale
                scaleY = pose.scale
                translationX = pose.x * size.width
                translationY = pose.y * size.height
                rotationZ = pose.roll
                alpha = if (paused) 0.45f else 1f
            },
            contentAlignment = Alignment.Center,
        ) {
            if (face != null) {
                Image(face, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            } else {
                val seed = (row.avatar.ifEmpty { row.name }).sumOf { it.code }
                Box(Modifier.fillMaxSize().background(avatarPalette[abs(seed) % avatarPalette.size]))
                Text(row.initials, style = Fm.text.labelLarge.copy(color = Color.White, fontSize = 22.sp, fontWeight = FmType.emphasis))
            }
        }
        Box(Modifier.fillMaxSize().background(Brush.radialGradient(0.45f to Color.Transparent, 1f to Color.Black.copy(alpha = 0.55f))))
        Column(Modifier.fillMaxSize()) {
            Row(Modifier.fillMaxWidth().padding(1.dp), verticalAlignment = Alignment.CenterVertically) {
                val running = row.status.isWorking
                HudGlyph(
                    if (running) "terminal" else "play_arrow",
                    playTint,
                    if (running) null else ({ onAction(TeammateAction.Run(row.id), false) }),
                )
                Spacer(Modifier.weight(1f))
                if (!row.mood.isEmpty) {
                    Box(
                        Modifier
                            .size(17.dp)
                            .background(Color.Black.copy(alpha = 0.45f), CircleShape)
                            .then(if (row.mood.attention) Modifier.border(1.2.dp, c.warning, CircleShape) else Modifier),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(row.mood.emoji, style = Fm.text.labelSmall.copy(fontSize = 10.sp))
                    }
                }
                Spacer(Modifier.weight(1f))
                Box {
                    HudGlyph("more_horiz", Color.Black.copy(alpha = 0.45f)) { menu = true }
                    FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        val header = listOf(row.specialty.ifEmpty { row.modelLine }, row.mood.line, row.lastActive).filter { it.isNotEmpty() }
                        Text(
                            row.name,
                            style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis),
                            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
                        )
                        for (line in header) {
                            Text(line, style = Fm.text.bodySmall.copy(color = c.textSecondary), modifier = Modifier.padding(horizontal = FmSpace.x4))
                        }
                        if (row.status.isWorking) {
                            FmDropdownMenuItem("Stop", icon = "stop", textColor = c.danger, onClick = { menu = false; onAction(TeammateAction.Stop(row.id), true) })
                        } else {
                            FmDropdownMenuItem("Run now", icon = "play_arrow", onClick = { menu = false; onAction(TeammateAction.Run(row.id), false) })
                        }
                        FmDropdownMenuItem("Generate briefing", icon = "newspaper", onClick = { menu = false; onAction(TeammateAction.Briefing(row.id), false) })
                    }
                }
            }
            Spacer(Modifier.weight(1f))
            Text(
                row.name,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                style = Fm.text.labelSmall.copy(color = Color.White, fontSize = 9.sp, fontWeight = FmType.emphasis),
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.7f))))
                    .padding(horizontal = 3.dp, vertical = 2.dp),
            )
        }
    }
}

@Composable
private fun HudGlyph(icon: String, tint: Color, onTap: (() -> Unit)?) {
    Box(
        Modifier.size(24.dp).then(if (onTap != null) Modifier.clip(CircleShape).clickable(onClick = onTap) else Modifier),
        contentAlignment = Alignment.Center,
    ) {
        Box(Modifier.size(17.dp).background(tint, CircleShape), contentAlignment = Alignment.Center) {
            FmIcon(icon, size = 11.dp, tint = Color.White)
        }
    }
}

internal class TeammateScreen(private val id: String) : Screen() {
    override val title: String get() = detail?.row?.name ?: "Special Agent"
    private var detail by mutableStateOf<TeammateDetail?>(null)
    private var error by mutableStateOf("")
    private var banner by mutableStateOf("")
    private var prompt by mutableStateOf("")
    private var sending by mutableStateOf(false)
    private var pending by mutableStateOf<PendingTeamConfirmation?>(null)

    private suspend fun load(store: TeammatesStore) {
        try {
            detail = opsIo { store.detail(id) }
            error = ""
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Exception) {
            error = TeamErrorText.describe(failure)
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val scope = rememberCoroutineScope()
        LaunchedEffect(id) { load(store) }
        fun prepare(action: TeammateAction, destructive: Boolean = false) {
            scope.launch {
                try {
                    pending = PendingTeamConfirmation(action.title, opsIo { store.prepare(action) }, destructive)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                }
            }
        }
        fun send() {
            scope.launch {
                sending = true
                try {
                    banner = opsIo { store.perform(TeammateAction.Prompt(id, prompt)) }
                    prompt = ""
                    error = ""
                    load(store)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    error = TeamErrorText.describe(failure)
                } finally {
                    sending = false
                }
            }
        }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("edit_outlined", "Edit", onPressed = { navigator.push(TeammateEditorScreen(id)) }, color = c.textPrimary)
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load(store) } }, color = c.textPrimary)
        }) {
            val current = detail
            LazyColumn(
                Modifier.fillMaxSize(),
                contentPadding = PaddingValues(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                if (current == null) {
                    item {
                        if (error.isEmpty()) {
                            Box(Modifier.fillMaxWidth().padding(FmSpace.x5), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                        } else {
                            Banner(error, true)
                        }
                    }
                    return@LazyColumn
                }
                val row = current.row
                item {
                    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                TeammateAvatar(row.name, row.avatar, 52)
                                Spacer(Modifier.width(FmSpace.x3))
                                Column(Modifier.weight(1f)) {
                                    Text(row.name, style = Fm.text.titleMedium.copy(color = c.textPrimary))
                                    if (row.specialty.isNotEmpty()) Text(row.specialty, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                                }
                                TeammateStatusBadge(row.status)
                            }
                            if (row.focus.isNotEmpty()) Labeled("Focus", row.focus)
                            Labeled("Model", row.modelLine)
                            Labeled("Schedule", row.schedule.human.ifEmpty { "On demand" })
                            if (row.schedule.isAutomatic && row.schedule.next.isNotEmpty()) Labeled("Next", row.schedule.next)
                            if (current.mention.isNotEmpty()) Labeled("Mention", current.mention)
                        }
                    }
                }
                item { FmSectionHeader("Work") }
                item {
                    ChipRow {
                        FmOutlinedButton(TeammateAction.Run(id).title, onClick = { prepare(TeammateAction.Run(id)) }, icon = "play_arrow")
                        if (row.status.isWorking) {
                            FmOutlinedButton("Stop", onClick = { prepare(TeammateAction.Stop(id), destructive = true) }, icon = "stop")
                        }
                        if (row.lastSessionId > 0 && !row.status.isWorking) {
                            FmOutlinedButton("Continue", onClick = { prepare(TeammateAction.ContinueSession(id, "")) }, icon = "refresh")
                        }
                        if (!row.schedule.isAutomatic && row.schedule.mode != "on-demand") {
                            FmOutlinedButton("Turn schedule on", onClick = { prepare(TeammateAction.EnableSchedule(id)) }, icon = "schedule")
                        }
                        FmOutlinedButton("Refresh briefing", onClick = { prepare(TeammateAction.Briefing(id)) }, icon = "newspaper")
                    }
                }
                if (row.takesPrompts) {
                    item { FmSectionHeader("Prompt") }
                    item {
                        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                            FmTextField(prompt, { prompt = it }, hint = "Ask ${row.name} to…", minLines = 2, maxLines = 6)
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                FmFilledButton(
                                    if (row.status.isWorking) "Steer the live run" else "Send",
                                    onClick = if (sending || prompt.isBlank()) null else ({ send() }),
                                    icon = "send",
                                    busy = sending,
                                )
                                if (sending) CircularProgressIndicator(Modifier.padding(start = FmSpace.x2).size(20.dp), strokeWidth = 2.dp)
                            }
                        }
                    }
                }
                item { Banner(banner, false) }
                item { Banner(error, true) }
                if (current.latestOutput.isNotEmpty()) {
                    item { FmSectionHeader("Latest output") }
                    item { FmCard(Modifier.fillMaxWidth()) { SelectionContainer { Text(current.latestOutput, style = Fm.text.bodySmall.copy(color = c.textPrimary)) } } }
                }
                if (current.activity.isNotEmpty()) {
                    item { FmSectionHeader("Activity") }
                    items(current.activity.reversed()) { line -> Text(line, style = Fm.text.labelSmall.copy(color = c.textSecondary)) }
                }
                if (row.skills.isNotEmpty()) {
                    item { FmSectionHeader("Skills") }
                    item { Text(row.skills.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textPrimary)) }
                }
                if (current.improvementNotes.isNotEmpty()) {
                    item { FmSectionHeader("What it has learned") }
                    items(current.improvementNotes) { note -> Text(note, style = Fm.text.labelSmall.copy(color = c.textSecondary)) }
                }
                if (current.logMonitors.isNotEmpty() || current.permissions.isNotEmpty()) {
                    item { FmSectionHeader("Monitors and permissions") }
                    items(current.logMonitors, key = { it.id }) { monitor -> Labeled(monitor.badge, "${monitor.threshold}+ · ${if (monitor.active) "on" else "off"}") }
                    items(current.permissions, key = { it.id }) { permission -> Text(permission.label, style = Fm.text.bodySmall.copy(color = c.textSecondary)) }
                    item { Text("These are changed on the desktop.", style = Fm.text.labelSmall.copy(color = c.textTertiary)) }
                }
            }
            pending?.let { confirm ->
                TeamConfirmDialog(
                    confirm,
                    perform = { store.run(it) },
                    onDone = {
                        banner = it.ifEmpty { "Done." }
                        scope.launch { load(store) }
                    },
                    onError = { error = it },
                    onDismiss = { pending = null },
                )
            }
        }
    }
}

@Composable
internal fun Labeled(label: String, value: String) {
    val c = Fm.colors
    Row {
        Text(label, style = Fm.text.bodySmall.copy(color = c.textTertiary), modifier = Modifier.width(96.dp))
        Text(value, style = Fm.text.bodySmall.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
    }
}

internal class TeamBriefingScreen : Screen() {
    override val title: String get() = "Briefing"
    private var briefing by mutableStateOf<TeamBriefing?>(null)
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val scope = rememberCoroutineScope()
        suspend fun load() {
            try {
                briefing = opsIo { store.briefing() }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = TeamErrorText.describe(failure)
            }
        }
        LaunchedEffect(Unit) { load() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            val current = briefing
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item { Banner(error, true) }
                if (current != null) {
                    item {
                        Column {
                            Text(current.headline, style = Fm.text.titleSmall.copy(color = c.textPrimary))
                            Text(listOf(current.repo, current.date).filter { it.isNotEmpty() }.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                        }
                    }
                    items(current.entries, key = { it.id }) { entry -> BriefingCard(navigator, entry) }
                }
            }
        }
    }
}

@Composable
private fun BriefingCard(navigator: Navigator, entry: BriefingEntry) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), onTap = { navigator.push(TeammateScreen(entry.id)) }) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TeammateAvatar(entry.name, entry.avatar, 28)
                Spacer(Modifier.width(FmSpace.x2))
                Text(entry.name, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis), modifier = Modifier.weight(1f))
                TeammateStatusBadge(entry.status)
            }
            if (entry.hasUpdate && entry.output.isNotEmpty()) {
                Text(entry.output, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 8, overflow = TextOverflow.Ellipsis)
            } else if (!entry.hasUpdate) {
                Text("No update today.", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
    }
}

internal class TeamStatsScreen : Screen() {
    override val title: String get() = "Special agent stats"
    private var stats by mutableStateOf<TeamStats?>(null)
    private var error by mutableStateOf("")
    private var banner by mutableStateOf("")
    private var pending by mutableStateOf<PendingTeamConfirmation?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val scope = rememberCoroutineScope()
        suspend fun load() {
            try {
                stats = opsIo { store.stats() }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = TeamErrorText.describe(failure)
            }
        }
        LaunchedEffect(Unit) { load() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            val current = stats
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item { Banner(banner, false) }
                item { Banner(error, true) }
                if (current != null) {
                    item {
                        Text(
                            "${current.totalTasks} tasks · ${String.format(Locale.US, "$%.2f", current.totalCost)}",
                            style = Fm.text.titleSmall.copy(color = c.textPrimary),
                        )
                    }
                    item { FmSectionHeader("Special Agents") }
                    items(current.teammates, key = { it.id }) { row ->
                        FmCard(Modifier.fillMaxWidth()) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                TeammateAvatar(row.name, row.avatar, 28)
                                Spacer(Modifier.width(FmSpace.x2))
                                Column(Modifier.weight(1f)) {
                                    Text(row.name, style = Fm.text.bodyMedium.copy(color = c.textPrimary, fontWeight = FmType.emphasis))
                                    Text(
                                        "${row.runs} runs · ${(row.successRate * 100).toInt()}% ok · ${row.failed} failed" +
                                            (if (row.graded > 0) " · grade ${String.format(Locale.US, "%.1f", row.averageGrade)}" else ""),
                                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                                    )
                                }
                                Text(String.format(Locale.US, "$%.2f", row.costUsd), style = Fm.text.labelSmall.copy(color = c.textSecondary))
                            }
                        }
                    }
                    item { FmSectionHeader("Tasks") }
                    items(current.tasks, key = { it.session }) { task ->
                        FmCard(Modifier.fillMaxWidth()) {
                            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                Text(task.title.ifEmpty { "Session #${task.session}" }, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                                Text(
                                    listOf(task.teammates.joinToString(", "), task.status).filter { it.isNotEmpty() }.joinToString(" · "),
                                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                                )
                                if (task.gradable) {
                                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalAlignment = Alignment.CenterVertically) {
                                        for (grade in 1..5) {
                                            FmIconButton(
                                                if (grade <= task.grade) "star" else "star_border",
                                                "Grade $grade",
                                                onPressed = {
                                                    scope.launch {
                                                        try {
                                                            pending = PendingTeamConfirmation("Grade task", opsIo { store.prepare(TeammateAction.Grade(task.session, grade)) })
                                                        } catch (cancelled: CancellationException) {
                                                            throw cancelled
                                                        } catch (failure: Exception) {
                                                            error = TeamErrorText.describe(failure)
                                                        }
                                                    }
                                                },
                                                color = c.warning,
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            pending?.let { confirm ->
                TeamConfirmDialog(
                    confirm,
                    perform = { store.run(it) },
                    onDone = {
                        banner = it.ifEmpty { "Graded." }
                        scope.launch { load() }
                    },
                    onError = { error = it },
                    onDismiss = { pending = null },
                )
            }
        }
    }
}

internal class TeamRoomScreen : Screen() {
    override val title: String get() = "#team"
    private var messages by mutableStateOf<List<TeamRoomMessage>>(emptyList())
    private var error by mutableStateOf("")

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        val store = remember { TeammatesFeature.teammates }
        val tick = rememberOpsTick(store)
        val scope = rememberCoroutineScope()
        suspend fun load() {
            try {
                messages = opsIo { store.room() }
                error = ""
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                error = TeamErrorText.describe(failure)
            }
        }
        LaunchedEffect(tick) { load() }
        DetailScaffold(title, navigator, actions = {
            FmIconButton("refresh", "Refresh", onPressed = { scope.launch { load() } }, color = c.textPrimary)
        }) {
            LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                item { Banner(error, true) }
                if (messages.isEmpty() && error.isEmpty()) {
                    item { Text("The desktop's #team room is quiet.", style = Fm.text.bodyMedium.copy(color = c.textSecondary)) }
                }
                items(messages, key = { it.id }) { message ->
                    Column(Modifier.fillMaxWidth(), horizontalAlignment = if (message.isSelf) Alignment.End else Alignment.Start) {
                        Text(message.sender, style = Fm.text.labelSmall.copy(color = c.textTertiary))
                        FmMessageActions(message.text) {
                            FmCard(color = if (message.isSelf) c.accent.copy(alpha = .15f) else null) {
                                LinkedText(message.text, ReferenceLinks.CHAT_REPO, style = Fm.text.bodySmall.copy(color = c.textPrimary), commits = false)
                            }
                        }
                    }
                }
            }
        }
    }
}
