package com.forkmesh.nativeapp.feature.poker

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.poker.PokerApi
import com.forkmesh.nativeapp.poker.PokerCard
import com.forkmesh.nativeapp.poker.PokerChips
import com.forkmesh.nativeapp.poker.PokerFair
import com.forkmesh.nativeapp.poker.PokerTable
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val clockFormat: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")

private fun time(at: Long): String = if (at <= 0) "" else clockFormat.format(Instant.ofEpochMilli(at).atZone(ZoneId.systemDefault()))

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
internal fun PokerSheetHost(model: PokerModel, onLeft: () -> Unit) {
    val table = model.table ?: return
    val close = { model.sheet = null }
    when (val sheet = model.sheet) {
        null -> Unit
        PokerSheet.Chat -> FmModalBottomSheet(onDismissRequest = close, isScrollControlled = true) { ChatSheet(model, table) }
        PokerSheet.Log -> FmModalBottomSheet(onDismissRequest = close, isScrollControlled = true) { LogSheet(table) }
        PokerSheet.LastHand -> FmModalBottomSheet(onDismissRequest = close, isScrollControlled = true) { LastHandSheet(table) }
        PokerSheet.Menu -> FmModalBottomSheet(onDismissRequest = close) { MenuSheet(model, table) }
        PokerSheet.Invite -> FmModalBottomSheet(onDismissRequest = close) { InviteSheet(model, table) }
        PokerSheet.Rebuy -> BuyInDialog(
            title = "Add chips",
            min = minOf(table.big.coerceAtLeast(1), maxRebuy(table)),
            max = maxRebuy(table),
            step = table.big,
            wallet = table.wallet,
            confirm = "Add",
            onDismiss = close,
            onConfirm = model::rebuy,
        )
        is PokerSheet.Sit -> BuyInDialog(
            title = "Sit down at seat ${sheet.seat + 1}",
            min = table.minBuyIn,
            max = if (table.wallet > 0) minOf(table.maxBuyIn, table.wallet) else table.maxBuyIn,
            step = table.big,
            wallet = table.wallet,
            confirm = "Sit down",
            onDismiss = close,
            onConfirm = { model.sit(sheet.seat, it) },
        )
        PokerSheet.ConfirmLeave -> FmConfirmDialog(
            title = "Leave the table?",
            body = if (table.me?.inHand == true) "You fold this hand. Your stack goes back to your wallet when the hand ends." else "Your stack goes back to your wallet.",
            confirmLabel = "Leave",
            destructive = true,
            onDismissRequest = close,
            onConfirm = { model.leave(onLeft) },
        )
        PokerSheet.ConfirmClose -> FmConfirmDialog(
            title = "Close this table?",
            body = "The hand stops and every stack goes back to its owner's wallet.",
            confirmLabel = "Close table",
            destructive = true,
            onDismissRequest = close,
            onConfirm = { model.close(onLeft) },
        )
    }
}

private fun maxRebuy(table: PokerTable): Long {
    val me = table.me ?: return 0
    val room = (table.maxBuyIn - me.stack - me.rebuy).coerceAtLeast(0)
    return if (table.wallet > 0) minOf(room, table.wallet) else room
}

@Composable
private fun BuyInDialog(title: String, min: Long, max: Long, step: Long, wallet: Long, confirm: String, onDismiss: () -> Unit, onConfirm: (Long) -> Unit) {
    var amount by remember(min, max) { mutableLongStateOf(if (max >= min) minOf(max, maxOf(min, (min + max) / 2 / step.coerceAtLeast(1) * step.coerceAtLeast(1))) else 0) }
    val possible = max >= min && max > 0
    FmConfirmDialog(
        title = title,
        body = if (possible) null else "You need at least ${PokerChips.format(min)} chips in your wallet. Collect free chips in the lobby.",
        confirmLabel = if (possible) "$confirm ${PokerChips.format(amount)}" else "OK",
        hideCancel = !possible,
        onDismissRequest = onDismiss,
        onConfirm = { if (possible) onConfirm(amount.coerceIn(min, max)) else onDismiss() },
        fields = if (!possible) null else ({
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    ChipIcon(if (amount >= 1000) 1000 else 100)
                    Spacer(Modifier.width(FmSpace.x2))
                    Text(PokerChips.format(amount), style = Fm.text.titleLarge, color = Fm.colors.textPrimary)
                }
                ChipSlider(amount, min, max, step, { amount = it })
                Text(
                    "${PokerChips.format(min)} – ${PokerChips.format(max)}" + if (wallet > 0) " · wallet ${PokerChips.format(wallet)}" else "",
                    style = Fm.text.labelSmall,
                    color = Fm.colors.textTertiary,
                )
            }
        }),
    )
}

@Composable
private fun SheetTitle(title: String, trailing: @Composable () -> Unit = {}) {
    Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = Fm.text.titleMedium, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
        trailing()
    }
}

@Composable
private fun ChatSheet(model: PokerModel, table: PokerTable) {
    var text by remember { mutableStateOf("") }
    val list = rememberLazyListState()
    LaunchedEffect(table.chat.size) {
        model.chatSeen = table.chat.size
        if (table.chat.isNotEmpty()) list.animateScrollToItem(table.chat.size - 1)
    }
    val send = {
        val line = text.trim()
        if (line.isNotEmpty()) {
            model.chat(line)
            text = ""
        }
    }
    Column(Modifier.fillMaxWidth().padding(bottom = FmSpace.x3)) {
        SheetTitle("Table chat")
        LazyColumn(Modifier.fillMaxWidth().heightIn(min = 120.dp, max = 360.dp), state = list, contentPadding = PaddingValues(horizontal = FmSpace.x4)) {
            if (table.chat.isEmpty()) {
                item { Text("No messages yet. Say hello to the table.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(vertical = FmSpace.x3)) }
            }
            items(table.chat) { line ->
                Row(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                    Text(line.name, style = Fm.text.labelLarge, color = Fm.colors.accent, modifier = Modifier.padding(end = FmSpace.x2))
                    Text(line.text, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, modifier = Modifier.weight(1f))
                    Text(time(line.at), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                }
            }
        }
        Row(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
            FmTextField(
                text,
                { text = it.take(PokerApi.CHAT_MAX) },
                Modifier.weight(1f),
                hint = "Message the table",
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = { send() }),
            )
            Spacer(Modifier.width(FmSpace.x2))
            FmToneButton("Send", onPressed = if (text.isNotBlank() && !model.busy) send else null, tone = FmTone.Accent, filled = true, height = 44.dp, busy = model.busy)
        }
    }
}

@Composable
private fun LogSheet(table: PokerTable) {
    Column(Modifier.fillMaxWidth().padding(bottom = FmSpace.x4)) {
        SheetTitle("Hand log")
        LazyColumn(Modifier.fillMaxWidth().heightIn(max = 460.dp), contentPadding = PaddingValues(horizontal = FmSpace.x4)) {
            if (table.log.isEmpty()) item { Text("Nothing has happened at this table yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary) }
            items(table.log.reversed()) { line ->
                Row(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
                    Text(time(line.at), style = Fm.text.labelSmall, color = Fm.colors.textTertiary, modifier = Modifier.width(44.dp))
                    Text(line.text, style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                }
            }
            if (table.history.isNotEmpty()) {
                item {
                    Spacer(Modifier.height(FmSpace.x3))
                    Text("Recent hands", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                    Spacer(Modifier.height(FmSpace.x1))
                }
                items(table.history.reversed()) { hand ->
                    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                        Text("Hand #${hand.number} · ${hand.winners.joinToString(", ")} won ${PokerChips.format(hand.amount)}", style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                        if (hand.handName.isNotEmpty()) Text(hand.handName, style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
                    }
                    HorizontalDivider(color = Fm.colors.textPrimary.copy(alpha = 0.08f))
                }
            }
        }
    }
}

@Composable
private fun CardRow(cards: List<PokerCard>, width: androidx.compose.ui.unit.Dp = 30.dp, lift: Set<String> = emptySet()) {
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.padding(top = 6.dp)) {
        cards.forEach { PokerCardView(it, width, lifted = it.code in lift) }
    }
}

@Composable
private fun LastHandSheet(table: PokerTable) {
    val last = table.lastHand
    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4).padding(bottom = FmSpace.x5)) {
        SheetTitle("Last hand")
        if (last == null) {
            Text("No hand has finished at this table yet.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
        } else {
            Text("Hand #${last.number}", style = Fm.text.labelLarge, color = Fm.colors.textSecondary)
            if (last.board.isNotEmpty()) CardRow(last.board, 34.dp, last.winners.flatMap { it.best }.map { it.code }.toSet())
            Spacer(Modifier.height(FmSpace.x3))
            last.winners.forEach { winner ->
                Text("${winner.name} won ${PokerChips.format(winner.amount)}" + if (winner.handName.isNotEmpty()) " with ${winner.handName}" else "", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                if (winner.cards.isNotEmpty()) CardRow(winner.cards, 30.dp, winner.best.map { it.code }.toSet())
                Spacer(Modifier.height(FmSpace.x2))
            }
            last.shown.filter { shown -> last.winners.none { it.seat == shown.seat } }.forEach { shown ->
                Text("${shown.name} showed" + if (shown.handName.isNotEmpty()) " ${shown.handName}" else "", style = Fm.text.bodyMedium, color = Fm.colors.textSecondary)
                CardRow(shown.cards, 26.dp)
                Spacer(Modifier.height(FmSpace.x2))
            }
            Spacer(Modifier.height(FmSpace.x2))
            HorizontalDivider(color = Fm.colors.textPrimary.copy(alpha = 0.08f))
            Spacer(Modifier.height(FmSpace.x2))
            Text("Provably fair", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
            Text(
                "Before the deal the table sealed each of the 52 shuffled positions with its own salt and published one SHA-256 over all of them. After the hand it reveals the salts of the cards that were shown, never folded cards, so you can check each one sat where the deck said.",
                style = Fm.text.bodySmall,
                color = Fm.colors.textSecondary,
            )
            val verified = PokerFair.verify(last)
            Spacer(Modifier.height(FmSpace.x2))
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(if (verified == true) "verified" else "shield", size = 18.dp, tint = if (verified == false) Fm.colors.danger else Fm.colors.success)
                Spacer(Modifier.width(FmSpace.x2))
                Text(
                    when (verified) {
                        true -> "Checked on this phone: all 52 seals match the commitment and every shown card matches its seal."
                        false -> "The revealed cards do not match the commitment."
                        null -> "The shown cards are revealed when the hand ends."
                    },
                    style = Fm.text.bodySmall,
                    color = Fm.colors.textPrimary,
                )
            }
            Mono("Commitment", last.commitment)
            if (last.cards.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x2))
                Text("Revealed cards", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                last.cards.forEach { revealed ->
                    val card = PokerCard.parse(revealed.card)
                    val sealed = last.hashes.getOrNull(revealed.index)?.let { it == PokerFair.cardHash(revealed.salt, revealed.index, revealed.card) } == true
                    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
                        if (card != null) PokerCardView(card, 22.dp) else Text(revealed.card, style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                        Spacer(Modifier.width(FmSpace.x2))
                        Text("#${revealed.index}", style = Fm.text.labelLarge, color = Fm.colors.textPrimary, modifier = Modifier.width(36.dp))
                        Text(revealed.salt, style = Fm.text.bodySmall.copy(fontFamily = FontFamily.Monospace), color = Fm.colors.textSecondary, modifier = Modifier.weight(1f))
                        FmIcon(if (sealed) "verified" else "shield", size = 16.dp, tint = if (sealed) Fm.colors.success else Fm.colors.danger)
                    }
                }
            }
            if (last.hashes.isNotEmpty()) Mono("Seals for the 52 positions", last.hashes.mapIndexed { i, h -> "$i $h" }.joinToString("\n"))
        }
        if (table.commitment.isNotEmpty() && table.inHand) {
            Spacer(Modifier.height(FmSpace.x3))
            Mono("This hand's commitment", table.commitment)
        }
    }
}

@Composable
private fun Mono(label: String, value: String) {
    if (value.isEmpty()) return
    Spacer(Modifier.height(FmSpace.x2))
    Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
    Text(
        value,
        style = Fm.text.bodySmall.copy(fontFamily = FontFamily.Monospace),
        color = Fm.colors.textPrimary,
        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Fm.colors.bgOverlay).padding(FmSpace.x2),
    )
}

@Composable
private fun MenuRow(icon: String, label: String, destructive: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = FmSpace.x4, vertical = 13.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val tint = when {
            !enabled -> Fm.colors.textTertiary
            destructive -> Fm.colors.danger
            else -> Fm.colors.textPrimary
        }
        FmIcon(icon, size = 20.dp, tint = if (destructive) tint else Fm.colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(label, style = Fm.text.bodyLarge, color = tint)
    }
}

private fun copy(context: Context, label: String, text: String) {
    val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
    clipboard.setPrimaryClip(ClipData.newPlainText(label, text))
}

@Composable
private fun MenuSheet(model: PokerModel, table: PokerTable) {
    val context = LocalContext.current
    val me = table.me
    val origin = remember { Services.get<ApiService>()?.gitOrigin?.trimEnd('/')?.ifEmpty { null } ?: "https://app.forkmesh.com" }
    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = FmSpace.x4)) {
        SheetTitle(table.name.ifEmpty { "Table" }) {
            Text("Code ${table.code}", style = Fm.text.labelLarge, color = Fm.colors.textSecondary)
        }
        MenuRow(if (model.muted) "volume_off" else "volume_up", if (model.muted) "Turn sounds on" else "Mute sounds") { model.toggleMute() }
        MenuRow("receipt_long", "Hand log") { model.sheet = PokerSheet.Log }
        MenuRow("history", "Last hand and fairness") { model.sheet = PokerSheet.LastHand }
        MenuRow("content_copy", "Copy table code") {
            copy(context, "Poker table code", table.code)
            model.notice = "Copied the table code ${table.code}."
            model.sheet = null
        }
        MenuRow("link", "Copy table link") {
            copy(context, "Poker table link", "$origin/dashboard/poker?table=${table.id}")
            model.notice = "Copied a link to this table."
            model.sheet = null
        }
        MenuRow("person_add", "Invite players") { model.sheet = PokerSheet.Invite }
        if (me != null) {
            HorizontalDivider(color = Fm.colors.textPrimary.copy(alpha = 0.08f))
            if (me.away) {
                MenuRow("pause", "I'm back") {
                    model.away(false)
                    model.sheet = null
                }
            } else {
                MenuRow("pause", "Sit out next hands") {
                    model.away(true)
                    model.sheet = null
                }
            }
            if (maxRebuy(table) > 0) MenuRow("savings", "Add chips from your wallet") { model.sheet = PokerSheet.Rebuy }
            MenuRow("logout", "Leave the table", destructive = true) { model.sheet = PokerSheet.ConfirmLeave }
        }
        if (table.ownerIsMe) {
            HorizontalDivider(color = Fm.colors.textPrimary.copy(alpha = 0.08f))
            Text("Owner", style = Fm.text.labelSmall, color = Fm.colors.textTertiary, modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2))
            MenuRow("smart_toy", "Add a bot", enabled = table.hasEmptySeat && !model.busy) {
                model.addBot()
                model.sheet = null
            }
            table.seats.filter { !it.empty && it.bot }.forEach { bot ->
                MenuRow("remove", "Remove ${bot.displayName}", enabled = !model.busy) {
                    model.removeBot(bot.index)
                    model.sheet = null
                }
            }
            MenuRow("delete", "Close the table", destructive = true) { model.sheet = PokerSheet.ConfirmClose }
        }
    }
}

@Composable
private fun InviteSheet(model: PokerModel, table: PokerTable) {
    var names by remember { mutableStateOf("") }
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4).padding(bottom = FmSpace.x5), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        SheetTitle("Invite players")
        Text(
            if (table.private) "Invited players can find this private table in their lobby and get a ping." else "Invited players get a ping with a link to this table.",
            style = Fm.text.bodySmall,
            color = Fm.colors.textSecondary,
        )
        FmTextField(names, { names = it }, Modifier.fillMaxWidth(), hint = "Account names, separated by commas", autofocus = true)
        Text("Or share the code ${table.code}.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            FmTextButton(onClick = { model.sheet = null }) { Text("Cancel") }
            FmToneButton("Invite", onPressed = if (PokerApi.normalizeNames(names).isNotEmpty() && !model.busy) ({ model.invite(names) }) else null, tone = FmTone.Accent, filled = true, height = 40.dp, busy = model.busy)
        }
    }
}
