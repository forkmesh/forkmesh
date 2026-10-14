package com.forkmesh.nativeapp.feature.people

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.settings.SettingsArea
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.settings.PeopleModel
import com.forkmesh.nativeapp.settings.PeopleProfileModel
import com.forkmesh.nativeapp.settings.PeopleRanked
import com.forkmesh.nativeapp.settings.PeopleRing
import com.forkmesh.nativeapp.settings.PeopleRingLayout
import com.forkmesh.nativeapp.settings.PeopleRings
import com.forkmesh.nativeapp.settings.PeopleSection
import com.forkmesh.nativeapp.ui.kit.FmAvatar
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.min
import kotlin.math.sin

object PeopleFeature {
    private const val OWNER = "WS18"

    fun install() {
        NavigationFeature.register(Route.Kind.People, OWNER) { PeopleScreen() }
    }
}

object PeopleColors {
    val night = Color(0xFF120D14)
    val hearth = Color(0xFF3D1C0D)

    fun ring(ring: PeopleRing): Color = when (ring) {
        PeopleRing.BLAZING -> Color(0xFFFF6B2B)
        PeopleRing.GLOWING -> Color(0xFFFFA43A)
        PeopleRing.KINDLING -> Color(0xFFF4CF6B)
        PeopleRing.WARM -> Color(0xFF5FC28E)
        PeopleRing.EMBERS -> Color(0xFF7EA6C4)
    }

    fun icon(ring: PeopleRing): String = when (ring) {
        PeopleRing.BLAZING -> "local_fire_department_rounded"
        PeopleRing.GLOWING -> "light_mode_outlined"
        PeopleRing.KINDLING -> "auto_awesome_outlined"
        PeopleRing.WARM -> "favorite_outline"
        PeopleRing.EMBERS -> "bedtime_outlined"
    }
}

class PeopleScreen : Screen() {
    override val title: String get() = "People"

    private val model: PeopleModel? by lazy { SettingsArea.endpoints?.let { PeopleModel(it, { SettingsArea.account }) } }
    private var revision by mutableIntStateOf(0)
    private var query by mutableStateOf("")
    private var focus by mutableStateOf<PeopleRing?>(null)
    private var picked by mutableStateOf<PeopleRanked?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model
        val scope = rememberCoroutineScope()
        if (model != null) {
            LaunchedEffect(model) {
                withContext(Dispatchers.IO) { model.load() }
                revision++
            }
        }
        revision
        picked?.let { person -> PeopleProfileSheet(person, model?.viewerName.orEmpty(), onDismiss = { picked = null }) }
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("People", navigator)
            when {
                model == null -> FmEmptyState("people_outline", "People unavailable", message = "Sign in to a ForkMesh server to see people.")
                !model.loaded && model.error.isNotEmpty() -> Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
                    Box(Modifier.weight(1f)) { FmEmptyState("cloud_off_outlined", "Could not load people", message = model.error) }
                    FmToneButton("Retry", onPressed = {
                        scope.launch {
                            withContext(Dispatchers.IO) { model.load() }
                            revision++
                        }
                    }, icon = "refresh", tone = FmTone.Accent, height = 44.dp, modifier = Modifier.padding(bottom = FmSpace.x6))
                }
                !model.loaded -> FmSkeletonList(rows = 6, label = "Loading people")
                else -> Page(model)
            }
        }
    }

    @Composable
    private fun Page(model: PeopleModel) {
        val sections = remember(revision) { model.sections() }
        val visible = remember(revision, query, focus) { model.visible(query, focus) }
        val toggle: (PeopleRing) -> Unit = { ring -> focus = if (focus == ring) null else ring }
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x2, bottom = 88.dp)) {
            item(key = "fire") {
                PeopleCampfire(sections, focus, model.viewerName, onPick = { picked = it }, onRing = toggle)
            }
            item(key = "legend") {
                PeopleLegend(sections, focus, onRing = toggle, modifier = Modifier.padding(top = FmSpace.x3))
            }
            item(key = "search") {
                FmTextField(query, { query = it }, modifier = Modifier.fillMaxWidth().padding(top = FmSpace.x3), hint = "Find a person")
            }
            items(visible, key = { it.ring.id }) { section ->
                PeopleSectionCard(section, model.viewerName, onPick = { picked = it }, modifier = Modifier.padding(top = FmSpace.x4))
            }
            if (query.isNotBlank() && visible.all { it.people.isEmpty() }) {
                item(key = "none") {
                    Text("No one matches “$query”", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary, modifier = Modifier.padding(top = FmSpace.x4))
                }
            }
        }
    }
}

@Composable
private fun PeopleCampfire(sections: List<PeopleSection>, focus: PeopleRing?, viewer: String, onPick: (PeopleRanked) -> Unit, onRing: (PeopleRing) -> Unit) {
    BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        val side: Dp = min(maxWidth.value, 460f).dp
        val sizePx = side.value.toDouble()
        val seat: Dp = (sizePx * 0.066).coerceIn(22.0, 36.0).dp
        val label: Dp = (sizePx * 0.2).dp
        val seats = remember(sections, sizePx) { PeopleRingLayout.seats(sections, sizePx, seat.value.toDouble(), label.value.toDouble()) }
        Box(
            Modifier.size(side).clip(RoundedCornerShape(24.dp))
                .background(Brush.radialGradient(listOf(PeopleColors.hearth, Color(0xFF1F1214), PeopleColors.night)))
                .semantics { contentDescription = "Campfire of people, from the most active in the centre to the quietest at the edge" },
        ) {
            Canvas(Modifier.fillMaxSize()) {
                val center = Offset(size.width / 2, size.height / 2)
                val unit = size.width / side.value
                PeopleRing.entries.forEach { ring ->
                    val r = PeopleRingLayout.radius(ring.ordinal, sizePx).toFloat() * unit
                    val color = PeopleColors.ring(ring)
                    val dim = focus != null && focus != ring
                    drawCircle(color.copy(alpha = if (dim) .02f else .08f), r, center, style = Stroke(size.width * 0.06f))
                    drawCircle(
                        color.copy(alpha = if (dim) .12f else .55f), r, center,
                        style = Stroke(
                            width = (if (focus == ring) 3f else 1.5f) * unit,
                            pathEffect = if (ring == PeopleRing.EMBERS) PathEffect.dashPathEffect(floatArrayOf(4f * unit, 5f * unit)) else null,
                        ),
                    )
                }
            }
            PeopleFire(Modifier.size(side * 0.2f).align(Alignment.Center))
            seats.forEach { seatSpot ->
                val color = PeopleColors.ring(seatSpot.ring)
                val dim = focus != null && focus != seatSpot.ring
                val person = seatSpot.person
                val spot = Modifier.offset((seatSpot.x.dp - seat / 2), (seatSpot.y.dp - seat / 2)).size(seat)
                if (person != null) {
                    Box(
                        spot.shadow(if (seatSpot.ring == PeopleRing.BLAZING) 6.dp else 0.dp, CircleShape, ambientColor = color, spotColor = color)
                            .clip(CircleShape)
                            .clickable(role = Role.Button, onClickLabel = "Open profile") { onPick(person) }
                            .semantics { contentDescription = "@${person.member.name}, ${seatSpot.ring.title}" },
                    ) {
                        FmAvatar(person.member.name, seat, avatarPng = person.member.avatar, textStyle = Fm.text.labelMedium)
                        Box(Modifier.matchParentSize().border(2.dp, if (person.member.id == viewer) Color.White else color, CircleShape))
                        if (dim) Box(Modifier.matchParentSize().background(PeopleColors.night.copy(alpha = .7f), CircleShape))
                    }
                } else {
                    Box(
                        spot.clip(CircleShape).background(color.copy(alpha = if (dim) .25f else 1f))
                            .clickable(role = Role.Button) { onRing(seatSpot.ring) }
                            .semantics { contentDescription = "${seatSpot.overflow} more in ${seatSpot.ring.title}" },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("+${seatSpot.overflow}", color = PeopleColors.night, fontSize = (seat.value * 0.32f).sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
            PeopleRing.entries.forEach { ring ->
                val r = PeopleRingLayout.radius(ring.ordinal, sizePx)
                val count = sections.firstOrNull { it.ring == ring }?.people?.size ?: 0
                val color = PeopleColors.ring(ring)
                val on = focus == ring
                Row(
                    Modifier.align(Alignment.TopCenter)
                        .offset(y = (sizePx / 2 - r).dp - 9.dp)
                        .widthIn(max = label)
                        .height(18.dp)
                        .clip(RoundedCornerShape(50))
                        .background(if (on) color else PeopleColors.night.copy(alpha = .88f))
                        .border(BorderStroke(1.dp, color.copy(alpha = .7f)), RoundedCornerShape(50))
                        .clickable(role = Role.Button) { onRing(ring) }
                        .semantics { contentDescription = "${ring.title} ring, $count ${if (count == 1) "person" else "people"}"; selected = on }
                        .padding(horizontal = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(3.dp),
                ) {
                    Text(ring.title, color = if (on) PeopleColors.night else color, fontSize = 10.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
                    Text("$count", color = (if (on) PeopleColors.night else color).copy(alpha = .8f), fontSize = 10.sp, maxLines = 1)
                }
            }
        }
    }
}

@Composable
private fun PeopleFire(modifier: Modifier) {
    val motion = rememberInfiniteTransition(label = "fire")
    val t by motion.animateFloat(0f, 1000f, infiniteRepeatable(tween(1_000_000, easing = LinearEasing), RepeatMode.Restart), label = "t")
    Canvas(modifier) {
        val s = size.width
        val seconds = t.toDouble()
        drawCircle(
            Brush.radialGradient(listOf(Color(0xFFFF8A2B).copy(alpha = .55f), Color.Transparent), center = center, radius = s * 0.7f),
            radius = s * 0.7f * (1.3f + 0.06f * sin(seconds * 2.1).toFloat()) / 1.3f,
        )
        val flame = Path().apply {
            moveTo(s * .5f, s * .05f)
            cubicTo(s * .66f, s * .3f, s * .9f, s * .42f, s * .84f, s * .7f)
            cubicTo(s * .8f, s * .88f, s * .66f, s * .95f, s * .5f, s * .95f)
            cubicTo(s * .32f, s * .95f, s * .16f, s * .86f, s * .16f, s * .68f)
            cubicTo(s * .16f, s * .52f, s * .28f, s * .44f, s * .34f, s * .3f)
            cubicTo(s * .37f, s * .44f, s * .43f, s * .47f, s * .5f, s * .5f)
            cubicTo(s * .5f, s * .32f, s * .44f, s * .18f, s * .5f, s * .05f)
            close()
        }
        val core = Path().apply {
            moveTo(s * .5f, s * .45f)
            cubicTo(s * .6f, s * .6f, s * .72f, s * .68f, s * .69f, s * .8f)
            cubicTo(s * .67f, s * .9f, s * .6f, s * .94f, s * .5f, s * .94f)
            cubicTo(s * .4f, s * .94f, s * .32f, s * .88f, s * .32f, s * .8f)
            cubicTo(s * .32f, s * .68f, s * .42f, s * .62f, s * .5f, s * .45f)
            close()
        }
        scale(1f + 0.04f * sin(seconds * 5.3).toFloat(), 1f + 0.07f * sin(seconds * 7.1).toFloat(), pivot = Offset(s / 2, s * .95f)) {
            drawPath(flame, Brush.verticalGradient(listOf(Color(0xFFFFD85A), Color(0xFFFF7A1F), Color(0xFFE2430F))))
        }
        scale(1f, 1f + 0.1f * sin(seconds * 9.7).toFloat(), pivot = Offset(s / 2, s * .95f)) {
            drawPath(core, Color(0xFFFFF3B0))
        }
    }
}

@Composable
private fun PeopleLegend(sections: List<PeopleSection>, focus: PeopleRing?, onRing: (PeopleRing) -> Unit, modifier: Modifier = Modifier) {
    val c = Fm.colors
    Row(modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        sections.forEach { section ->
            val color = PeopleColors.ring(section.ring)
            val on = focus == section.ring
            Row(
                Modifier.heightIn(min = 44.dp).clip(RoundedCornerShape(50))
                    .background(if (on) color else c.bgRaised)
                    .border(1.dp, if (on) color else c.border, RoundedCornerShape(50))
                    .clickable(role = Role.Button) { onRing(section.ring) }
                    .semantics { selected = on }
                    .padding(horizontal = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
            ) {
                Box(Modifier.size(10.dp).background(if (on) PeopleColors.night else color, CircleShape))
                Text(section.ring.title, style = Fm.text.labelMedium, color = if (on) PeopleColors.night else c.textPrimary)
                Text("${section.people.size}", style = Fm.text.bodySmall, color = if (on) PeopleColors.night else c.textSecondary)
            }
        }
    }
}

@Composable
private fun PeopleSectionCard(section: PeopleSection, viewer: String, onPick: (PeopleRanked) -> Unit, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val color = PeopleColors.ring(section.ring)
    val shape = RoundedCornerShape(12.dp)
    Row(modifier.fillMaxWidth().height(IntrinsicSize.Min).clip(shape).background(c.bgRaised).border(1.dp, c.border, shape)) {
        Box(Modifier.width(4.dp).fillMaxHeight().background(color))
        Column(Modifier.weight(1f)) {
            Row(
                Modifier.fillMaxWidth().background(Brush.horizontalGradient(listOf(color.copy(alpha = .14f), Color.Transparent))).padding(FmSpace.x3).semantics(mergeDescendants = true) { heading() },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.size(34.dp).background(color.copy(alpha = .18f), CircleShape), contentAlignment = Alignment.Center) {
                    FmIcon(PeopleColors.icon(section.ring), tint = color, size = 18.dp)
                }
                Column(Modifier.weight(1f).padding(horizontal = FmSpace.x3)) {
                    Text(section.ring.title, style = Fm.text.titleMedium, color = c.textPrimary)
                    Text(section.ring.blurb, style = Fm.text.bodySmall, color = c.textSecondary)
                }
                Text(
                    "${section.people.size}", style = Fm.text.labelMedium, color = color,
                    modifier = Modifier.background(color.copy(alpha = .15f), RoundedCornerShape(50)).padding(horizontal = FmSpace.x2, vertical = 2.dp),
                )
            }
            if (section.people.isEmpty()) {
                Hairline()
                Text("No one in this ring right now.", style = Fm.text.bodySmall, color = c.textTertiary, modifier = Modifier.padding(FmSpace.x3))
            }
            section.people.forEach { person ->
                Hairline()
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 56.dp).clickable(role = Role.Button, onClickLabel = "Open profile") { onPick(person) }
                        .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box {
                        FmAvatar(person.member.name, 40.dp, avatarPng = person.member.avatar)
                        Box(Modifier.matchParentSize().border(2.dp, color, CircleShape))
                    }
                    Column(Modifier.weight(1f).padding(horizontal = FmSpace.x3)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("@${person.member.name}", style = Fm.text.titleSmall, color = c.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            if (person.member.id == viewer) Text("  You", style = Fm.text.labelSmall, color = c.accent)
                        }
                        Text(subtitle(person), style = Fm.text.bodySmall, color = c.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    FmIcon("chevron_right", tint = c.textTertiary, size = 18.dp)
                }
            }
        }
    }
}

@Composable
private fun Hairline() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(Fm.colors.border))
}

private fun subtitle(person: PeopleRanked): String {
    val status = listOf(person.member.statusEmoji, person.member.statusText).filter { it.isNotEmpty() }.joinToString(" ")
    return status.ifEmpty { PeopleRings.lastActive(person.member.activityBucket) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PeopleProfileSheet(person: PeopleRanked, viewer: String, onDismiss: () -> Unit) {
    val c = Fm.colors
    val color = PeopleColors.ring(person.ring)
    val model = remember(person.member.id) { SettingsArea.endpoints?.let { PeopleProfileModel(person.member.name, viewer, it) } }
    var revision by remember { mutableIntStateOf(0) }
    val scope = rememberCoroutineScope()
    val requireAccount = AccountFeature.rememberRequireAccount()
    LaunchedEffect(model) {
        if (model != null) {
            withContext(Dispatchers.IO) { model.load() }
            revision++
        }
    }
    revision
    val open: (Route) -> Unit = { route ->
        onDismiss()
        NavigationFeature.open(route)
    }
    FmModalBottomSheet(onDismissRequest = onDismiss, showDragHandle = true, isScrollControlled = true) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x5)) {
            val profile = model?.profile
            val avatar = profile?.avatar?.takeIf { it.isNotEmpty() } ?: person.member.avatar
            Column(
                Modifier.fillMaxWidth().background(Brush.verticalGradient(listOf(color.copy(alpha = .55f), PeopleColors.hearth, PeopleColors.night))).padding(vertical = FmSpace.x5),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                Box(Modifier.shadow(12.dp, CircleShape, ambientColor = color, spotColor = color)) {
                    FmAvatar(person.member.name, 88.dp, avatarPng = avatar, textStyle = Fm.text.displaySmall)
                    Box(Modifier.matchParentSize().border(3.dp, color, CircleShape))
                }
                Text("@${person.member.name}", style = Fm.text.titleLarge, color = Color.White)
                Row(
                    Modifier.background(color, RoundedCornerShape(50)).padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
                ) {
                    FmIcon(PeopleColors.icon(person.ring), tint = PeopleColors.night, size = 14.dp)
                    Text("${person.ring.title} ring", style = Fm.text.labelMedium, color = PeopleColors.night)
                }
                Text(PeopleRings.lastActive(person.member.activityBucket), style = Fm.text.bodySmall, color = Color.White.copy(alpha = .75f))
            }
            Column(Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x4)) {
                if (model?.isSelf == true) {
                    Text("This is you.", style = Fm.text.bodyMedium, color = c.textSecondary)
                } else {
                    Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                        val following = profile?.isFollowing == true
                        val ready = model != null && profile != null && !model.busy
                        FmToneButton(
                            if (model?.busy == true) "…" else if (following) "Following" else "Follow",
                            onPressed = if (ready) {
                                {
                                    scope.launch {
                                        if (!requireAccount("follow people")) return@launch
                                        withContext(Dispatchers.IO) { model?.toggleFollow() }
                                        revision++
                                    }
                                }
                            } else {
                                null
                            },
                            icon = if (following) "check" else "person_add",
                            tone = FmTone.Accent,
                            filled = !following,
                            height = 44.dp,
                            modifier = Modifier.weight(1f).semantics { contentDescription = "${if (following) "Unfollow" else "Follow"} @${person.member.name}" },
                        )
                        FmToneButton(
                            "Message",
                            onPressed = {
                                scope.launch {
                                    if (requireAccount("send a message")) open(Route.Dm(person.member.name))
                                }
                            },
                            icon = "chat_bubble_outline",
                            tone = FmTone.Neutral,
                            height = 44.dp,
                            modifier = Modifier.weight(1f).semantics { contentDescription = "Message @${person.member.name}" },
                        )
                    }
                }
                if (!model?.actionError.isNullOrEmpty()) Text(model?.actionError.orEmpty(), style = Fm.text.bodySmall, color = c.danger)
                when {
                    profile?.isPrivate == true -> Text("This profile is private.", style = Fm.text.bodyMedium, color = c.textSecondary)
                    profile != null -> {
                        Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(c.bgRaised).border(1.dp, c.border, RoundedCornerShape(12.dp)).padding(vertical = FmSpace.x3)) {
                            Stat("${profile.followers}", "Followers", Modifier.weight(1f))
                            Stat("${profile.following}", "Following", Modifier.weight(1f))
                            Stat(
                                if (profile.createdAt > 0) DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(profile.createdAt)) else "—",
                                "Joined", Modifier.weight(1f),
                            )
                        }
                        val status = listOf(profile.statusEmoji, profile.statusText).filter { it.isNotEmpty() }.joinToString(" ")
                        if (status.isNotEmpty()) Text(status, style = Fm.text.bodyMedium, color = c.textPrimary)
                        if (profile.bio.isNotEmpty()) Text(profile.bio, style = Fm.text.bodyLarge, color = c.textPrimary)
                        if (profile.location.isNotEmpty()) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                FmIcon("location_on", tint = c.textSecondary, size = 16.dp)
                                Text(profile.location, style = Fm.text.bodySmall, color = c.textSecondary)
                            }
                        }
                    }
                    !model?.error.isNullOrEmpty() -> Text(model?.error.orEmpty(), style = Fm.text.bodyMedium, color = c.textSecondary)
                    else -> Text("Loading profile…", style = Fm.text.bodyMedium, color = c.textSecondary)
                }
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 44.dp).clickable(role = Role.Button) { open(Route.Profile(person.member.name)) },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("View full profile", style = Fm.text.labelLarge, color = c.accent, modifier = Modifier.weight(1f))
                    FmIcon("chevron_right", tint = c.accent, size = 18.dp)
                }
            }
        }
    }
}

@Composable
private fun Stat(value: String, label: String, modifier: Modifier) {
    Column(modifier.semantics(mergeDescendants = true) {}, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(value, style = Fm.text.titleMedium, color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(label, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
    }
}
