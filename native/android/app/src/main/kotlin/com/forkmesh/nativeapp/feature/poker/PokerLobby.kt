package com.forkmesh.nativeapp.feature.poker

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.poker.PokerApi
import com.forkmesh.nativeapp.poker.PokerChips
import com.forkmesh.nativeapp.poker.PokerClock
import com.forkmesh.nativeapp.poker.PokerLobby
import com.forkmesh.nativeapp.poker.PokerStake
import com.forkmesh.nativeapp.poker.PokerTableDraft
import com.forkmesh.nativeapp.poker.PokerTableSummary
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.fmGlass
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.delay
import kotlin.math.roundToLong

internal class PokerScreen(table: String?) : Screen() {
    override val title: String get() = "Poker"

    private var pendingTable = table?.takeIf(PokerApi::isTableId)
    private val status = FmStatus()

    init {
        pluginPackage = PokerApi.PACKAGE_ID
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val model = remember { PokerFeature.model(context) }
        DisposableEffect(model) {
            model.lobbyAppeared()
            onDispose { model.lobbyGone() }
        }
        LaunchedEffect(Unit) {
            val target = pendingTable ?: return@LaunchedEffect
            pendingTable = null
            navigator.push(PokerTableScreen(target))
        }
        LaunchedEffect(model.lobbyProblem) {
            if (model.lobbyProblem.isNotEmpty() && model.lobby != null) status.error(model.lobbyProblem)
        }
        LaunchedEffect(model.notice) {
            val text = model.notice ?: return@LaunchedEffect
            if (navigator.top === this@PokerScreen) {
                status.show(text)
                model.notice = null
            }
        }
        var creating by remember { mutableStateOf(false) }
        val open: (String) -> Unit = { id -> navigator.push(PokerTableScreen(id)) }
        DetailScaffold(
            title,
            navigator,
            actions = {
                FmIconButton(if (model.muted) "volume_off" else "volume_up", if (model.muted) "Turn sounds on" else "Mute sounds", onPressed = model::toggleMute, color = Fm.colors.textPrimary)
                FmIconButton("refresh", "Refresh", onPressed = if (model.lobbyLoading) null else ({ model.loadLobby() }), color = Fm.colors.textPrimary)
            },
            status = status,
        ) {
            val lobby = model.lobby
            when {
                lobby == null && model.lobbyProblem.isNotEmpty() -> LobbyProblem(model.lobbyProblem) { model.loadLobby() }
                lobby == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                else -> FmPullToRefreshBox(isRefreshing = model.lobbyLoading, onRefresh = { model.loadLobby() }, modifier = Modifier.fillMaxSize()) {
                    LobbyList(model, lobby, onCreate = { creating = true }, open = open)
                }
            }
        }
        val lobby = model.lobby
        if (creating && lobby != null) {
            CreateTableDialog(lobby, busy = model.busy, onDismiss = { creating = false }) { draft ->
                model.create(draft) { id ->
                    creating = false
                    open(id)
                }
            }
        }
    }
}

@Composable
private fun LobbyProblem(message: String, retry: () -> Unit) {
    Column(Modifier.fillMaxSize().padding(FmSpace.x5), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
        FmIcon("casino", size = 44.dp, tint = Fm.colors.textTertiary)
        Spacer(Modifier.height(FmSpace.x3))
        Text(message, style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
        Spacer(Modifier.height(FmSpace.x3))
        FmToneButton("Try again", onPressed = retry, tone = FmTone.Accent)
    }
}

@Composable
private fun LobbyList(model: PokerModel, lobby: PokerLobby, onCreate: () -> Unit, open: (String) -> Unit) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        item("wallet") { WalletCard(model, lobby) }
        item("stats") { StatsRow(lobby) }
        item("actions") { LobbyActions(model, onCreate, open) }
        section("Your tables", "casino", lobby.myTables, open)
        section("Invitations", "person_add", lobby.invitedTables, open)
        section("Open tables", "group", lobby.openTables, open)
        if (lobby.tables.isEmpty()) {
            item("empty") {
                Column(Modifier.fillMaxWidth().padding(vertical = FmSpace.x6), horizontalAlignment = Alignment.CenterHorizontally) {
                    FmIcon("casino", size = 40.dp, tint = Fm.colors.textTertiary)
                    Spacer(Modifier.height(FmSpace.x2))
                    Text("No open tables right now", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                    Text("Start one and fill the empty seats with bots.", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
            }
        }
        item("note") {
            Text(
                "Chips are play money. They cannot be bought, sold, cashed out or moved between accounts.",
                style = Fm.text.bodySmall,
                color = Fm.colors.textTertiary,
                modifier = Modifier.padding(vertical = FmSpace.x3),
            )
        }
    }
}

private fun androidx.compose.foundation.lazy.LazyListScope.section(title: String, icon: String, tables: List<PokerTableSummary>, open: (String) -> Unit) {
    if (tables.isEmpty()) return
    item("h-$title") { FmSectionHeader(title, icon = icon, count = tables.size, modifier = Modifier.padding(top = FmSpace.x2)) }
    items(tables, key = { "$title-${it.id}" }) { table -> TableRow(table) { open(table.id) } }
}

@Composable
private fun WalletCard(model: PokerModel, lobby: PokerLobby) {
    val wallet = lobby.wallet
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val remaining = model.claimRemaining(now)
    LaunchedEffect(wallet.nextClaimAt, wallet.canClaim, remaining > 0) {
        while (model.claimRemaining(System.currentTimeMillis()) > 0) {
            delay(1000 - System.currentTimeMillis() % 1000)
            now = System.currentTimeMillis()
        }
        now = System.currentTimeMillis()
    }
    val shape = RoundedCornerShape(20.dp)
    Box(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(Brush.linearGradient(listOf(Color(0xFF0D5C3A), Color(0xFF083D27))))
            .border(1.dp, Color(0x55C9A24A), shape)
            .padding(FmSpace.x4),
    ) {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                ChipIcon(1000)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Wallet", style = Fm.text.labelLarge, color = Color.White.copy(alpha = 0.8f))
            }
            Spacer(Modifier.height(FmSpace.x2))
            Text("${PokerChips.format(wallet.chips)} chips", color = Color.White, fontSize = 30.sp, fontWeight = FontWeight.Bold)
            if (wallet.onTables > 0) {
                Text("${PokerChips.format(wallet.onTables)} more on tables", style = Fm.text.bodySmall, color = Color.White.copy(alpha = 0.75f))
            }
            Spacer(Modifier.height(FmSpace.x3))
            val ready = wallet.canClaim || remaining <= 0
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(44.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (ready) Color(0xFFF9A825) else Color.White.copy(alpha = 0.12f))
                    .clickable(enabled = ready && !model.claiming) { model.claim() },
                contentAlignment = Alignment.Center,
            ) {
                when {
                    model.claiming -> FmCircularProgressIndicator(Modifier.size(20.dp))
                    ready -> Text("Collect ${PokerChips.format(wallet.claimAmount)} free chips", color = Color(0xFF1A1A1A), fontWeight = FontWeight.SemiBold)
                    else -> Text("Next free chips in ${PokerClock.countdown(remaining)}", color = Color.White.copy(alpha = 0.85f), fontWeight = FontWeight.Medium)
                }
            }
        }
    }
}

@Composable
private fun StatsRow(lobby: PokerLobby) {
    val wallet = lobby.wallet
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        Stat("Hands", PokerChips.format(wallet.handsPlayed), Modifier.weight(1f))
        Stat("Won", if (wallet.handsPlayed > 0) "${PokerChips.format(wallet.handsWon)} (${wallet.winRate}%)" else "0", Modifier.weight(1f))
        Stat("Biggest pot", PokerChips.short(wallet.biggestPot), Modifier.weight(1f))
    }
    if (wallet.bestHand.isNotEmpty()) {
        Spacer(Modifier.height(FmSpace.x2))
        Stat("Best hand", wallet.bestHand, Modifier.fillMaxWidth())
    }
}

@Composable
private fun Stat(label: String, value: String, modifier: Modifier) {
    Column(modifier.fmGlass(RoundedCornerShape(FmRadius.lg), shadow = false).padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)) {
        Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
        Text(value, style = Fm.text.titleSmall, color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun LobbyActions(model: PokerModel, onCreate: () -> Unit, open: (String) -> Unit) {
    var code by remember { mutableStateOf("") }
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        FmToneButton("New table", onPressed = onCreate, icon = "add", tone = FmTone.Success, filled = true, height = 44.dp, modifier = Modifier.fillMaxWidth())
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmTextField(
                code,
                { code = PokerApi.normalizeCode(it).take(6) },
                Modifier.weight(1f),
                hint = "Table code",
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters, imeAction = ImeAction.Go),
                keyboardActions = KeyboardActions(onGo = { model.joinByCode(code, open) }),
            )
            FmToneButton("Join", onPressed = if (PokerApi.isCode(code) && !model.busy) ({ model.joinByCode(code, open) }) else null, tone = FmTone.Accent, height = 44.dp, busy = model.busy)
        }
    }
}

@Composable
private fun TableRow(table: PokerTableSummary, onTap: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .fmGlass(RoundedCornerShape(FmRadius.lg), shadow = false, interactive = true)
            .clickable(onClick = onTap)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(40.dp).clip(CircleShape).background(Brush.radialGradient(listOf(Color(0xFF0D5C3A), Color(0xFF083D27)))), contentAlignment = Alignment.Center) {
            FmIcon("casino", size = 20.dp, tint = Color(0xFFE8D18A))
        }
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(table.name.ifEmpty { "Table ${table.code}" }, style = Fm.text.titleSmall, color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                if (table.private) {
                    Spacer(Modifier.width(4.dp))
                    FmIcon("lock", size = 14.dp, tint = Fm.colors.textTertiary)
                }
            }
            val bots = if (table.bots > 0) " · ${table.bots} bot${if (table.bots == 1) "" else "s"}" else ""
            Text(
                "Blinds ${table.blinds} · ${table.players}/${table.seats} seated$bots · ${table.turnSeconds}s",
                style = Fm.text.bodySmall,
                color = Fm.colors.textSecondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text("Buy-in ${PokerChips.format(table.minBuyIn)} – ${PokerChips.format(table.maxBuyIn)} · ${table.owner}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
        }
        if (table.invited && !table.mine) {
            Text("Invited", style = Fm.text.labelSmall, color = Fm.colors.accent)
        } else if (table.full) {
            Text("Full", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
    }
}

@Composable
internal fun ChipIcon(denomination: Long, size: androidx.compose.ui.unit.Dp = 22.dp) {
    androidx.compose.foundation.Canvas(Modifier.size(size)) { drawChipTop(center, this.size.minDimension / 2, denomination) }
}

@Composable
internal fun Stepper(label: String, value: Int, range: IntRange, onChange: (Int) -> Unit, format: (Int) -> String = { it.toString() }) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        FmIconButton("remove", "Fewer", onPressed = if (value > range.first) ({ onChange(value - 1) }) else null, busy = false)
        Text(format(value), style = Fm.text.titleSmall, color = Fm.colors.textPrimary, modifier = Modifier.width(44.dp), textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        FmIconButton("add", "More", onPressed = if (value < range.last) ({ onChange(value + 1) }) else null, busy = false)
    }
}

@Composable
internal fun Choices(options: List<Pair<String, String>>, selected: String, onSelect: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        options.forEach { (id, label) ->
            val on = id == selected
            Box(
                Modifier
                    .clip(RoundedCornerShape(FmRadius.full))
                    .background(if (on) Fm.colors.accent else Fm.colors.bgOverlay)
                    .clickable { onSelect(id) }
                    .padding(horizontal = FmSpace.x3, vertical = 7.dp),
            ) {
                Text(label, style = Fm.text.labelLarge, color = if (on) Color.White else Fm.colors.textPrimary)
            }
        }
    }
}

@Composable
internal fun ChipSlider(value: Long, min: Long, max: Long, step: Long, onChange: (Long) -> Unit) {
    if (max <= min) {
        Text(PokerChips.format(max.coerceAtLeast(min)), style = Fm.text.titleMedium, color = Fm.colors.textPrimary)
        return
    }
    val span = (max - min).toFloat()
    Slider(
        value = ((value - min) / span).coerceIn(0f, 1f),
        onValueChange = { fraction ->
            val raw = min + (fraction * span).toDouble()
            val unit = step.coerceAtLeast(1)
            val snapped = if (fraction >= 0.999f) max else ((raw / unit).roundToLong() * unit).coerceIn(min, max)
            onChange(snapped)
        },
        colors = SliderDefaults.colors(thumbColor = Color(0xFFF9A825), activeTrackColor = Color(0xFFF9A825)),
    )
}

@Composable
private fun CreateTableDialog(lobby: PokerLobby, busy: Boolean, onDismiss: () -> Unit, onCreate: (PokerTableDraft) -> Unit) {
    var name by remember { mutableStateOf("${lobby.actor.ifEmpty { "My" }}'s table".replaceFirstChar { it.uppercase() }) }
    var stakeId by remember { mutableStateOf(lobby.stakes.firstOrNull { it.minBuyIn <= lobby.wallet.chips }?.id ?: lobby.stakes.first().id) }
    var seats by remember { mutableIntStateOf(6) }
    var turn by remember { mutableIntStateOf(30) }
    var private by remember { mutableStateOf(false) }
    var bots by remember { mutableIntStateOf(2) }
    var watch by remember { mutableStateOf(false) }
    val stake: PokerStake = lobby.stake(stakeId) ?: lobby.stakes.first()
    val top = minOf(stake.maxBuyIn, lobby.wallet.chips)
    val canSit = top >= stake.minBuyIn
    var buyIn by remember(stakeId) { mutableLongStateOf(minOf(stake.maxBuyIn, maxOf(stake.minBuyIn, top / 2 / stake.big.coerceAtLeast(1) * stake.big.coerceAtLeast(1)))) }
    if (bots > seats - 1) bots = seats - 1
    FmAlertDialog(
        onDismissRequest = onDismiss,
        title = "New table",
        actions = {
            FmTextButton(onClick = onDismiss) { Text("Cancel") }
            FmTextButton(
                onClick = {
                    onCreate(PokerTableDraft(name, stake.id, seats, private, turn, bots, if (watch || !canSit) 0 else buyIn.coerceIn(stake.minBuyIn, top)))
                },
                enabled = !busy && name.isNotBlank(),
                busy = busy,
            ) { Text(if (busy) "Creating…" else "Create") }
        },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2), modifier = Modifier.verticalScroll(rememberScrollState())) {
            FmTextField(name, { name = it.take(PokerApi.NAME_MAX) }, Modifier.fillMaxWidth(), hint = "Table name")
            Text("Stakes", style = Fm.text.labelLarge, color = Fm.colors.textSecondary)
            Choices(lobby.stakes.map { it.id to "${it.label} ${it.blinds}" }, stake.id) { stakeId = it }
            Stepper("Seats", seats, PokerApi.MIN_SEATS..PokerApi.MAX_SEATS, { seats = it })
            Stepper("Bots", bots, 0..(seats - 1), { bots = it })
            Text("Turn clock", style = Fm.text.labelLarge, color = Fm.colors.textSecondary)
            Choices(PokerApi.TURN_SECONDS.map { it.toString() to "${it}s" }, turn.toString()) { turn = it.toInt() }
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmCheckbox(private, { private = it })
                Text("Private (by invite, link or code)", style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmCheckbox(watch || !canSit, if (canSit) ({ watch = it }) else null)
                Text("Watch only", style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            }
            if (canSit && !watch) {
                Text("Buy-in ${PokerChips.format(buyIn)}", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                ChipSlider(buyIn, stake.minBuyIn, top, stake.big, { buyIn = it })
                Text("${PokerChips.format(stake.minBuyIn)} – ${PokerChips.format(stake.maxBuyIn)} · wallet ${PokerChips.format(lobby.wallet.chips)}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            } else if (!canSit) {
                Text("You need ${PokerChips.format(stake.minBuyIn)} chips to sit at these stakes.", style = Fm.text.bodySmall, color = Fm.colors.warning)
            }
        }
    }
}
