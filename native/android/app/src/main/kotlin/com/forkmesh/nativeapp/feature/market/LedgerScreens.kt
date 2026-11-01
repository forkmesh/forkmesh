package com.forkmesh.nativeapp.feature.market

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.account.projects.UserPresence
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.feature.tools.copyToClipboard
import com.forkmesh.nativeapp.market.Lamports
import com.forkmesh.nativeapp.market.LedgerEntry
import com.forkmesh.nativeapp.market.LedgerModel
import com.forkmesh.nativeapp.market.LedgerSection
import com.forkmesh.nativeapp.market.MoneyText
import com.forkmesh.nativeapp.market.PersonalRecord
import com.forkmesh.nativeapp.market.PersonalRecordKind
import com.forkmesh.nativeapp.market.WalletRequestModel
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class LedgerScreen(initial: LedgerSection) : Screen() {
    override val title: String get() = "Ledger & tips"

    private val client = MarketFeature.client()
    private val model = client?.let { c -> MarketFeature.ledgerStore()?.let { LedgerModel(c, it) } }
    private val wallet = client?.let { WalletRequestModel(MarketFeature.wallet(), it) }
    private var section by mutableStateOf(initial)
    private var loading by mutableStateOf(true)
    private var version by mutableIntStateOf(0)
    private var reload by mutableIntStateOf(1)
    private var adding by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val wallet = wallet ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(reload) {
            loading = true
            val own = Services.get<SettingsService>()?.solanaAddress?.value.orEmpty()
            withContext(Dispatchers.IO) { model.refresh(own) }
            loading = false
            version++
        }
        DisposableEffect(Unit) {
            scope.launch(Dispatchers.IO) {
                wallet.start { scope.launch(Dispatchers.IO) { wallet.refresh(); version++ } }
                version++
            }
            onDispose { wallet.stop() }
        }
        @Suppress("UNUSED_EXPRESSION") version
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Ledger & tips", navigator) { FmIconButton("refresh", "Refresh", onPressed = { reload++ }, color = Fm.colors.textPrimary) }
                val sections = LedgerSection.entries
                FmSegmentTabBar(sections.map { FmSegmentTab(it.title) }, sections.indexOf(section), onChanged = { section = sections[it] })
                ErrorLine(model.errorText)
                if (loading && model.entries.isEmpty() && section == LedgerSection.RECORDS) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                } else when (section) {
                    LedgerSection.RECORDS -> Records(model) { scope.launch { withContext(Dispatchers.IO) { model.loadMore() }; version++ } }
                    LedgerSection.REWARDS -> Rewards(model, wallet) { version++ }
                    LedgerSection.TIPS -> Send(wallet) { version++ }
                    LedgerSection.MINE -> Mine(model) { version++ }
                    LedgerSection.TREASURY -> Treasury(model)
                }
            }
            if (section == LedgerSection.MINE) {
                FmExtendedFloatingActionButton("Record", onClick = { adding = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), icon = "add")
            }
        }
        if (adding) RecordEditor(onDismiss = { adding = false }) { record ->
            model.add(record)
            adding = false
            version++
        }
    }
}

@Composable
private fun EntryRow(entry: LedgerEntry) {
    val context = LocalContext.current
    FmListRow(
        entry.title,
        subtitle = listOfNotNull(entry.amountText, shortDate(entry.occurredAt).ifEmpty { null }, entry.status, entry.note.ifEmpty { null }).joinToString(" · "),
        trailing = if (entry.verified) ({ FmStatusBadge("On chain", Fm.colors.success) }) else null,
        onTap = if (entry.explorerUrl.isNotEmpty()) ({ NavigationFeature.openInBrowser(context, entry.explorerUrl) }) else null,
        showChevron = entry.explorerUrl.isNotEmpty(),
    )
}

@Composable
private fun Records(model: LedgerModel, onMore: () -> Unit) {
    if (model.entries.isEmpty()) {
        FmEmptyState("receipt_long", "No public records yet")
        return
    }
    LazyColumn(Modifier.fillMaxSize()) {
        if (model.solUsd.isNotEmpty()) item { Text("1 SOL = $${model.solUsd}", Modifier.padding(16.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary) }
        items(model.entries, key = { it.id }) { EntryRow(it) }
        if (model.next != null) item { FmToneButton("Load more", onPressed = onMore, modifier = Modifier.padding(16.dp)) }
    }
}

@Composable
private fun Rewards(model: LedgerModel, wallet: WalletRequestModel, onChanged: () -> Unit) {
    val scope = rememberCoroutineScope()
    val sendStatus = rememberFmStatus()
    LazyColumn(Modifier.fillMaxSize()) {
        item(key = "send-status") { FmInlineStatus(sendStatus, Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) }
        if (wallet.rewards.isNotEmpty()) {
            item { FmSectionHeader("Payable from your desktop", Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = wallet.rewards.size) }
            items(wallet.rewards, key = { "w-" + it.rewardId }) { reward ->
                FmListRow(
                    "@${reward.recipient} · ${Lamports.sol(reward.lamports)}",
                    subtitle = "${reward.repository} #${reward.number} ${reward.title}",
                    trailing = { FmToneButton("Pay", onPressed = { scope.launch(Dispatchers.IO) { wallet.prepareReward(reward); onChanged() } }, tone = FmTone.Accent) },
                    showChevron = false,
                )
            }
        }
        item { FmSectionHeader("Pending", Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = model.pendingRewards.size) }
        items(model.pendingRewards, key = { "p-" + it.rewardId }) { reward ->
            FmListRow("@${reward.recipient} · ${Lamports.sol(reward.amountLamports)}", subtitle = "${reward.subjectLabel} ${reward.subjectTitle}", showChevron = false)
        }
        item { FmSectionHeader("Paid", Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = model.paidRewards.size) }
        items(model.paidRewards, key = { "d-" + it.rewardId }) { reward ->
            FmListRow("@${reward.recipient} · ${Lamports.sol(reward.amountLamports)}", subtitle = reward.subjectLabel, showChevron = false)
        }
        item { ConfirmBar(wallet, sendStatus, onChanged) }
    }
}

@Composable
private fun ConfirmBar(wallet: WalletRequestModel, status: FmStatus, onChanged: () -> Unit) {
    val confirmation = wallet.confirmation ?: return
    val context = LocalContext.current
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    FmConfirmDialog(
        title = "Send this request to your desktop?",
        body = confirmation.summary + "\n\nYour desktop asks you to approve and signs the payment there.",
        confirmLabel = "Send request",
        onDismissRequest = { wallet.cancel(); onChanged() },
        onConfirm = {
            status.clear()
            scope.launch {
                if (!UserPresence.confirm(context, messenger, "Confirm the payment request")) {
                    wallet.cancel()
                    onChanged()
                    return@launch
                }
                val ok = withContext(Dispatchers.IO) { wallet.send() }
                onChanged()
                status.report(if (ok) "Sent. Approve it on your desktop." else wallet.errorText, !ok)
            }
        },
    )
}

@Composable
private fun Send(wallet: WalletRequestModel, onChanged: () -> Unit) {
    val scope = rememberCoroutineScope()
    val sendStatus = rememberFmStatus()
    val status = wallet.status
    Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        when {
            wallet.unavailable -> Text(wallet.errorText.ifEmpty { "Connect to your desktop to send tips and rewards." }, style = Fm.text.bodyMedium, color = Fm.colors.textTertiary)
            status == null -> FmCircularProgressIndicator()
            !status.linked -> Text("No tip wallet is linked on your desktop. Link one there first.", style = Fm.text.bodyMedium)
            else -> {
                FmCard(Modifier.fillMaxWidth()) {
                    Column {
                        FmKeyValueRow("From", status.address.take(6) + "…" + status.address.takeLast(4))
                        if (status.minLamports > 0 || status.maxLamports > 0) FmKeyValueRow("Limits", "${Lamports.sol(status.minLamports)} – ${Lamports.sol(status.maxLamports)}")
                        status.awaitingOwner?.let { FmKeyValueRow("Waiting", it.summary.ifEmpty { it.status }) }
                    }
                }
                var recipient by remember { mutableStateOf(wallet.recipient) }
                var address by remember { mutableStateOf(wallet.address) }
                var amount by remember { mutableStateOf(wallet.amount) }
                wallet.recipient = recipient
                wallet.address = address
                wallet.amount = amount
                FmTextField(recipient, { recipient = it }, hint = "Recipient account")
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmTextField(address, { address = it }, Modifier.weight(1f), hint = "Payout address")
                    FmToneButton("Look up", onPressed = {
                        scope.launch {
                            withContext(Dispatchers.IO) { wallet.lookUpAddress() }
                            address = wallet.address
                            onChanged()
                        }
                    })
                }
                FmTextField(amount, { amount = it }, hint = "Amount in SOL", errorText = if (amount.isNotBlank()) wallet.amountProblem else null)
                FmToneButton("Prepare tip", onPressed = if (wallet.canPrepareTip) ({ scope.launch(Dispatchers.IO) { wallet.prepareTip(); onChanged() } }) else null, tone = FmTone.Accent, filled = true, busy = false)
                wallet.lastRequest?.let { Text("Last request: ${it.summary.ifEmpty { it.action }} · ${it.status.replace('_', ' ')}", style = Fm.text.bodySmall, color = Fm.colors.textTertiary) }
            }
        }
        FmInlineStatus(sendStatus)
        ErrorLine(if (wallet.errorText == sendStatus.message) "" else wallet.errorText)
        ConfirmBar(wallet, sendStatus, onChanged)
    }
}

@Composable
private fun Mine(model: LedgerModel, onChanged: () -> Unit) {
    LazyColumn(Modifier.fillMaxSize()) {
        item { Text("Only on this phone, encrypted in its keystore.", Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary) }
        model.ownBalance?.lamports?.let { item { FmListRow("Your payout wallet", subtitle = Lamports.sol(it), showChevron = false) } }
        items(model.totals, key = { "t-" + it.currency }) { total ->
            FmListRow(total.currency, subtitle = "In ${MoneyText.cents(total.income, "")} · out ${MoneyText.cents(total.expense, "")}", showChevron = false)
        }
        if (model.records.isEmpty()) item { FmEmptyState("savings", "No records yet") }
        items(model.records, key = { it.id }) { record ->
            FmListRow(
                record.title,
                subtitle = listOf(
                    (if (record.kind == PersonalRecordKind.INCOME) "+" else "−") + record.amountText,
                    shortDate(record.occurredAt),
                    if (record.settled) "Settled" else "Open",
                ).joinToString(" · "),
                trailing = {
                    Row {
                        FmIconButton(if (record.settled) "check_circle" else "radio_button_unchecked", "Settled", onPressed = { model.toggleSettled(record.id); onChanged() })
                        FmIconButton("delete", "Delete", onPressed = { model.remove(record.id); onChanged() }, color = Fm.colors.danger)
                    }
                },
                showChevron = false,
            )
        }
        item { Spacer(Modifier.height(88.dp)) }
    }
}

@Composable
private fun Treasury(model: LedgerModel) {
    val context = LocalContext.current
    val notice = rememberFmStatus()
    val treasury = model.treasury
    if (treasury == null) {
        FmEmptyState("account_balance", "No treasury address published")
        return
    }
    Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(treasury.label.ifEmpty { "Treasury" }, style = Fm.text.titleMedium)
        if (treasury.purpose.isNotEmpty()) Text(treasury.purpose, style = Fm.text.bodyMedium)
        if (treasury.donationUri.isNotEmpty()) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                com.forkmesh.nativeapp.ui.kit.QrImage(treasury.donationUri, size = 200.dp, description = "Treasury address QR code")
            }
        }
        FmCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp)) {
                Text(treasury.address, style = Fm.mono())
                if (treasury.balanceKnown) Text(Lamports.sol(treasury.balanceLamports), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmToneButton("Copy address", onPressed = {
                copyToClipboard(context, treasury.address)
                notice.show("Address copied")
            })
            FmToneButton("Open in a wallet app", onPressed = {
                runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(treasury.donationUri)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
                    .onFailure { notice.error("No Solana wallet app is installed.") }
            }, tone = FmTone.Accent)
            if (treasury.explorerUrl.isNotEmpty()) FmToneButton("Explorer", onPressed = { NavigationFeature.openInBrowser(context, treasury.explorerUrl) })
        }
        FmInlineStatus(notice)
        FmSectionHeader("Other wallets", count = model.wallets.size - 1)
        model.wallets.filter { it.key != treasury.key }.forEach { wallet ->
            FmListRow(wallet.label.ifEmpty { wallet.key }, subtitle = wallet.address, showChevron = false)
        }
    }
}

@Composable
private fun RecordEditor(onDismiss: () -> Unit, onSave: (PersonalRecord) -> Unit) {
    var kind by remember { mutableStateOf(PersonalRecordKind.EXPENSE) }
    var title by remember { mutableStateOf("") }
    var amount by remember { mutableStateOf("") }
    var currency by remember { mutableStateOf("USD") }
    var note by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    FmConfirmDialog(
        title = "New record",
        confirmLabel = "Save",
        onDismissRequest = onDismiss,
        onConfirm = {
            val cents = amount.trim().toBigDecimalOrNull()?.movePointRight(2)?.toLong()
            when {
                title.isBlank() -> problem = "Give the record a title."
                cents == null || cents <= 0 -> problem = "Enter an amount such as 12.50."
                else -> onSave(PersonalRecord.make(kind, title.trim(), cents, currency.trim().uppercase().ifEmpty { "USD" }, System.currentTimeMillis(), note))
            }
        },
        fields = {
            problem?.let { Text(it, color = Fm.colors.danger, style = Fm.text.bodySmall) }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                PersonalRecordKind.entries.forEach { value ->
                    FmToneButton(value.wire.replaceFirstChar { it.uppercase() }, onPressed = { kind = value }, tone = if (kind == value) FmTone.Accent else FmTone.Neutral)
                }
            }
            FmTextField(title, { title = it }, hint = "Title")
            FmTextField(amount, { amount = it }, hint = "Amount")
            FmTextField(currency, { currency = it }, hint = "Currency")
            FmTextField(note, { note = it }, hint = "Note", maxLines = 3)
        },
    )
}
