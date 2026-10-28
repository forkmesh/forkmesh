package com.forkmesh.nativeapp.feature.market

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.market.ContactInvoices
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmContactRef
import com.forkmesh.nativeapp.market.Invoice
import com.forkmesh.nativeapp.market.InvoiceContactLink
import com.forkmesh.nativeapp.market.InvoiceDates
import com.forkmesh.nativeapp.market.InvoiceDraft
import com.forkmesh.nativeapp.market.InvoiceErrors
import com.forkmesh.nativeapp.market.InvoiceEvent
import com.forkmesh.nativeapp.market.InvoiceFilter
import com.forkmesh.nativeapp.market.InvoiceLineDraft
import com.forkmesh.nativeapp.market.InvoiceMoney
import com.forkmesh.nativeapp.market.InvoicePaymentDraft
import com.forkmesh.nativeapp.market.InvoiceSendDraft
import com.forkmesh.nativeapp.market.InvoiceSettings
import com.forkmesh.nativeapp.market.InvoiceSummary
import com.forkmesh.nativeapp.market.InvoicesModel
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.contactInvoices
import com.forkmesh.nativeapp.market.deleteInvoice
import com.forkmesh.nativeapp.market.invoice
import com.forkmesh.nativeapp.market.invoiceAction
import com.forkmesh.nativeapp.market.invoiceSettings
import com.forkmesh.nativeapp.market.recordInvoicePayment
import com.forkmesh.nativeapp.market.saveInvoice
import com.forkmesh.nativeapp.market.saveInvoiceSettings
import com.forkmesh.nativeapp.market.sendInvoice
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmExtendedFloatingActionButton
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSegmentTab
import com.forkmesh.nativeapp.ui.kit.FmSegmentTabBar
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal object InvoiceRevision {
    var value by mutableIntStateOf(0)

    fun bump() {
        value++
    }
}

sealed class InvoiceEditorSeed {
    data class New(val contact: InvoiceContactLink? = null) : InvoiceEditorSeed()
    data class Edit(val invoice: Invoice) : InvoiceEditorSeed()
    data class Duplicate(val invoice: Invoice) : InvoiceEditorSeed()
}

@Composable
private fun invoiceTint(state: String): Color = when (state) {
    "sent" -> Fm.colors.accent
    "partial" -> Fm.colors.warning
    "overdue" -> Fm.colors.danger
    "paid" -> Fm.colors.success
    else -> Fm.colors.textTertiary
}

@Composable
private fun InvoiceBadge(invoice: Invoice) {
    FmStatusBadge(invoice.statusLabel, invoiceTint(invoice.state))
}

@Composable
private fun InvoiceNoticeLine(text: String) {
    if (text.isNotEmpty()) Text(text, Modifier.padding(horizontal = 16.dp, vertical = 4.dp), color = Fm.colors.success, style = Fm.text.bodySmall)
}

@Composable
private fun Engagement(invoice: Invoice) {
    if (!invoice.wasSent) {
        Text("Not sent", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        return
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        listOf("drafts" to invoice.opens, "visibility" to invoice.views, "ads_click" to invoice.clicks).forEach { (icon, count) ->
            FmIcon(icon, size = 13.dp, tint = Fm.colors.textTertiary)
            Text(count.toString(), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
            Spacer(Modifier.width(4.dp))
        }
        if (invoice.lastActivityAt > 0) Text(relativeTime(invoice.lastActivityAt), style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
    }
}

@Composable
private fun InvoiceRow(invoice: Invoice, onTap: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().combinedClickableCompat(onTap).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(invoice.number, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary, maxLines = 1)
                InvoiceBadge(invoice)
            }
            Text(
                invoice.clientLabel + if (invoice.contactId.isNotEmpty()) " · CRM" else "",
                style = Fm.text.bodySmall, color = Fm.colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis,
            )
            Engagement(invoice)
        }
        Spacer(Modifier.width(10.dp))
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(InvoiceMoney.format(invoice.total, invoice.currency), style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
            if (invoice.balance > 0 && invoice.balance != invoice.total) {
                Text("${InvoiceMoney.format(invoice.balance, invoice.currency)} due", style = Fm.text.labelSmall, color = Fm.colors.textSecondary)
            }
            Text("Due ${InvoiceDates.label(invoice.dueAt)}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
    }
}

@Composable
private fun ActivityRow(event: InvoiceEvent, onTap: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().combinedClickableCompat(onTap).padding(horizontal = 16.dp, vertical = 9.dp),
        verticalAlignment = Alignment.Top,
    ) {
        FmIcon(event.icon, size = 18.dp, tint = Fm.colors.textTertiary)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text("${event.title} · ${event.number}", style = Fm.text.bodySmall, color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            val line = listOf(event.activityLine(), relativeTime(event.at)).filter { it.isNotEmpty() }.joinToString(" · ")
            Text(line, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun SummaryTile(label: String, value: String, detail: String, modifier: Modifier, tint: Color = Fm.colors.textPrimary) {
    FmCard(modifier, padding = androidx.compose.foundation.layout.PaddingValues(12.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 1)
            Text(value, style = Fm.text.titleMedium.copy(fontWeight = FontWeight.SemiBold), color = tint, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(detail, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun SummaryTiles(summary: InvoiceSummary, currency: String) {
    Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SummaryTile("Outstanding", InvoiceMoney.totals(summary.outstanding, currency), InvoiceSummary.plural(summary.count("sent"), "unpaid invoice"), Modifier.weight(1f))
            SummaryTile(
                "Overdue", InvoiceMoney.totals(summary.overdueAmount, currency), InvoiceSummary.plural(summary.count("overdue"), "overdue invoice"), Modifier.weight(1f),
                tint = if (summary.count("overdue") > 0) Fm.colors.danger else Fm.colors.textPrimary,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SummaryTile("Paid, last 30 days", InvoiceMoney.totals(summary.paidLast30Days, currency), InvoiceSummary.plural(summary.count("paid"), "paid invoice") + " in all", Modifier.weight(1f))
            SummaryTile("Open rate", summary.openRateText, summary.engagementText, Modifier.weight(1f))
        }
        SummaryTile("Average days to pay", summary.daysToPayText, summary.hoursToOpenText, Modifier.fillMaxWidth())
    }
}

private fun <T> CoroutineScope.invoiceWork(
    setBusy: (Boolean) -> Unit,
    onError: (String) -> Unit,
    work: () -> T,
    done: (T) -> Unit,
) {
    setBusy(true)
    launch {
        val result = withContext(Dispatchers.IO) { runCatching(work) }
        setBusy(false)
        result.onSuccess(done).onFailure { onError(InvoiceErrors.describe(it)) }
    }
}

class InvoicesScreen(private val requestedOrganization: String? = null, private val requestedInvoice: String? = null) : Screen() {
    override val title: String get() = "Invoices"

    private val model = MarketFeature.client()?.let { InvoicesModel(it) }
    private var loading by mutableStateOf(true)
    private var reload by mutableIntStateOf(1)
    private var firstLoad = true
    private var query by mutableStateOf("")
    private var filter by mutableStateOf(InvoiceFilter.ALL)
    private var tab by mutableIntStateOf(0)
    private var notice by mutableStateOf("")
    private var installing by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        val scope = rememberCoroutineScope()
        LaunchedEffect(reload) {
            loading = true
            val preferred = if (firstLoad) requestedOrganization else null
            withContext(Dispatchers.IO) { model.refresh(preferred) }
            loading = false
            InvoiceRevision.bump()
            val wanted = requestedInvoice
            if (firstLoad && wanted != null) {
                val found = withContext(Dispatchers.IO) { model.locate(wanted) }
                InvoiceRevision.bump()
                if (found != null) navigator.push(InvoiceDetailScreen(model, found.id)) else model.errorText = "That invoice was not found in your organizations."
            }
            firstLoad = false
        }
        @Suppress("UNUSED_EXPRESSION") InvoiceRevision.value
        val overview = model.overview
        Box(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            Column(Modifier.fillMaxSize()) {
                ToolsAppBar("Invoices", navigator) {
                    if (overview != null) FmIconButton("settings", "Invoicing settings", onPressed = { navigator.push(InvoiceSettingsScreen(model)) }, color = Fm.colors.textPrimary)
                    FmIconButton("refresh", "Refresh", onPressed = { notice = ""; reload++ }, color = Fm.colors.textPrimary)
                }
                if (model.organizations.isNotEmpty()) {
                    OrganizationPicker(model.organizations, model.organization, "Invoicing") { name ->
                        notice = ""
                        scope.launch {
                            loading = true
                            withContext(Dispatchers.IO) { model.choose(name) }
                            loading = false
                            InvoiceRevision.bump()
                        }
                    }
                }
                ErrorLine(model.errorText)
                InvoiceNoticeLine(notice)
                when {
                    loading && overview == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                    model.organizations.isEmpty() -> FmEmptyState("corporate_fare", "No organizations", message = "Join or create an organization before using Invoicing.")
                    model.needsInstall -> InstallPrompt(model, installing) {
                        installing = true
                        scope.launch {
                            val ok = withContext(Dispatchers.IO) { model.install() }
                            installing = false
                            if (ok) notice = "Invoicing installed."
                            InvoiceRevision.bump()
                        }
                    }
                    overview == null -> FmEmptyState("receipt_long", "Invoicing is unavailable", message = "Tap refresh to try again.")
                    else -> {
                        val shown = overview.filtered(query, filter)
                        LazyColumn(Modifier.fillMaxSize()) {
                            item(key = "summary") { SummaryTiles(overview.summary, overview.settings.currency) }
                            item(key = "tabs") {
                                FmSegmentTabBar(
                                    listOf(FmSegmentTab("Invoices", overview.invoices.size), FmSegmentTab("Activity", overview.activity.size)),
                                    tab, onChanged = { tab = it },
                                )
                            }
                            if (tab == 0) {
                                item(key = "search") { FmTextField(query, { query = it }, Modifier.padding(horizontal = 16.dp, vertical = 6.dp), hint = "Search number, client or email") }
                                item(key = "filters") {
                                    Row(
                                        Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 4.dp),
                                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                                    ) {
                                        InvoiceFilter.entries.forEach { option ->
                                            FmToneButton(
                                                "${option.title} ${overview.count(option)}",
                                                onPressed = { filter = option },
                                                tone = if (filter == option) FmTone.Accent else FmTone.Neutral,
                                                filled = filter == option,
                                            )
                                        }
                                    }
                                }
                                if (shown.isEmpty()) {
                                    item(key = "empty") {
                                        Text(
                                            if (overview.invoices.isEmpty()) "No invoices yet. Create one with the Invoice button." else "No invoices match.",
                                            Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary,
                                        )
                                    }
                                }
                                items(shown, key = { it.id }) { invoice -> InvoiceRow(invoice) { navigator.push(InvoiceDetailScreen(model, invoice.id)) } }
                            } else {
                                if (overview.activity.isEmpty()) {
                                    item(key = "activity-empty") {
                                        Text("Sends, opens, views and payments show up here.", Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                                    }
                                }
                                items(overview.activity.size, key = { "a$it" }) { index ->
                                    val event = overview.activity[index]
                                    ActivityRow(event) { if (event.invoiceId.isNotEmpty()) navigator.push(InvoiceDetailScreen(model, event.invoiceId)) }
                                }
                            }
                            item(key = "end") { Spacer(Modifier.height(88.dp)) }
                        }
                    }
                }
            }
            if (overview != null && !model.needsInstall) {
                FmExtendedFloatingActionButton(
                    "Invoice",
                    onClick = { navigator.push(InvoiceEditorScreen(model, InvoiceEditorSeed.New())) },
                    modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                    icon = "add",
                )
            }
        }
    }
}

@Composable
private fun InstallPrompt(model: InvoicesModel, installing: Boolean, onInstall: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        FmIcon("receipt_long", size = 40.dp, tint = Fm.colors.textTertiary)
        Text("Invoicing is not installed for ${model.organization}.", style = Fm.text.titleMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.Center)
        Text(
            "Create invoices for CRM contacts, email them with a view-and-pay link, and see when each one is opened, viewed and paid.",
            style = Fm.text.bodySmall, color = Fm.colors.textSecondary, textAlign = TextAlign.Center,
        )
        if (model.canInstall) {
            FmToneButton(if (installing) "Installing…" else "Install Invoicing", onPressed = if (installing) null else onInstall, tone = FmTone.Accent, filled = true, icon = "download")
        } else {
            Text("Ask an owner or admin of ${model.organization} to install it.", style = Fm.text.bodySmall, color = Fm.colors.textTertiary, textAlign = TextAlign.Center)
        }
    }
}

@Composable
private fun DetailMetric(label: String, value: String, hint: String, modifier: Modifier) {
    Column(modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        Text(value, style = Fm.text.bodyLarge.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (hint.isNotEmpty()) Text(hint, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2)
    }
}

@Composable
private fun TotalLine(label: String, value: String, strong: Boolean = false) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 3.dp)) {
        Text(label, Modifier.weight(1f), style = if (strong) Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold) else Fm.text.bodySmall, color = if (strong) Fm.colors.textPrimary else Fm.colors.textSecondary)
        Text(value, style = if (strong) Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold) else Fm.text.bodySmall, color = Fm.colors.textPrimary)
    }
}

class InvoiceDetailScreen(private val model: InvoicesModel, val invoiceId: String) : Screen() {
    override val title: String get() = "Invoice"

    private var invoice by mutableStateOf(model.overview?.invoice(invoiceId))
    private var loading by mutableStateOf(false)
    private var busy by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")
    private var menu by mutableStateOf(false)
    private var sending by mutableStateOf<InvoiceSendDraft?>(null)
    private var paying by mutableStateOf<InvoicePaymentDraft?>(null)
    private var confirm by mutableStateOf("")
    private var removed by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val organization = model.organization
        fun fetch() {
            scope.launch {
                loading = true
                val result = withContext(Dispatchers.IO) { runCatching { model.client.invoice(organization, invoiceId) } }
                loading = false
                result.onSuccess { invoice = it; error = "" }.onFailure {
                    if (InvoiceErrors.code(it) == "invoice_not_found") removed = true
                    error = InvoiceErrors.describe(it)
                }
            }
        }
        fun act(message: (Invoice) -> String, work: PackagesClient.() -> Invoice) {
            notice = ""
            scope.invoiceWork({ busy = it }, { error = it }, {
                val next = model.client.work()
                model.load()
                next
            }) { next ->
                invoice = next
                error = ""
                notice = message(next)
                InvoiceRevision.bump()
            }
        }
        val top = navigator.top
        LaunchedEffect(top) { if (top === this@InvoiceDetailScreen) fetch() }
        val current = invoice
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(current?.number ?: "Invoice", navigator) {
                if (current != null && current.editable) {
                    FmTextButton(onClick = { navigator.push(InvoiceEditorScreen(model, InvoiceEditorSeed.Edit(current))) }, enabled = !busy) { Text("Edit") }
                }
                if (current != null) {
                    Box {
                        FmIconButton("more_vert", "More", onPressed = { menu = true }, color = Fm.colors.textPrimary)
                        FmDropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            FmDropdownMenuItem("Duplicate", onClick = { menu = false; navigator.push(InvoiceEditorScreen(model, InvoiceEditorSeed.Duplicate(current))) }, icon = "content_copy")
                            if (current.publicUrl.isNotEmpty()) {
                                FmDropdownMenuItem("Copy link", onClick = { menu = false; crmCopy(context, current.publicUrl); notice = "Invoice link copied." }, icon = "link")
                                FmDropdownMenuItem("Share link", onClick = { menu = false; crmShare(context, current.publicUrl, "Invoice ${current.number}") }, icon = "share")
                            }
                            if (current.canReopen) FmDropdownMenuItem("Reopen", onClick = { menu = false; act({ "Invoice ${it.number} reopened." }) { invoiceAction(organization, invoiceId, "reopen") } }, icon = "undo")
                            if (current.canVoid) FmDropdownMenuItem("Void", onClick = { menu = false; confirm = "void" }, icon = "block", textColor = Fm.colors.danger)
                            if (current.deletable) FmDropdownMenuItem("Delete", onClick = { menu = false; confirm = "delete" }, icon = "delete", textColor = Fm.colors.danger)
                        }
                    }
                }
            }
            ErrorLine(error)
            InvoiceNoticeLine(notice)
            when {
                current == null && (loading || error.isEmpty()) -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                current == null || removed -> FmEmptyState("receipt_long", "Invoice not found", message = error.ifEmpty { null })
                else -> InvoiceDetailBody(current, organization, navigator, busy, onSend = { sending = InvoiceSendDraft.of(current) }, onPay = { paying = InvoicePaymentDraft.of(current, System.currentTimeMillis()) }, onAction = { action ->
                    when (action) {
                        "reopen" -> act({ "Invoice ${it.number} reopened." }) { invoiceAction(organization, invoiceId, "reopen") }
                        else -> confirm = action
                    }
                }, onCopy = { crmCopy(context, current.publicUrl); notice = "Invoice link copied." }, onShare = { crmShare(context, current.publicUrl, "Invoice ${current.number}") }, onPreview = { crmOpen(context, current.publicUrl) })
            }
        }
        val send = sending
        if (send != null && current != null) {
            val business = model.settings.businessName.ifEmpty { organization }
            FmConfirmDialog(
                title = if (current.wasSent) "Remind about ${current.number}" else "Email ${current.number}",
                confirmLabel = if (current.wasSent) "Send reminder" else "Send",
                busy = busy,
                onDismissRequest = { sending = null },
                onConfirm = {
                    if (send.to.isNotBlank()) {
                        sending = null
                        act({ "Invoice ${it.number} emailed to ${it.sentTo.ifEmpty { send.to.trim() }}." }) { sendInvoice(organization, invoiceId, send) }
                    }
                },
                fields = {
                    if (send.to.isBlank()) Text("Add the client's email address.", style = Fm.text.bodySmall, color = Fm.colors.danger)
                    FmTextField(send.to, { sending = send.copy(to = it) }, hint = "To")
                    FmTextField(send.subject, { sending = send.copy(subject = it) }, hint = InvoiceSendDraft.subjectHint(current, business))
                    FmTextField(send.message, { sending = send.copy(message = it) }, hint = "Message (leave empty for a short note with the amount and due date)", minLines = 3, maxLines = 8)
                    Text("The email has a View invoice link and a tracking image, so you can see when it is opened.", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                },
            )
        }
        val pay = paying
        if (pay != null && current != null) {
            FmConfirmDialog(
                title = if (current.canMarkPaid) "Mark ${current.number} paid" else "Record a payment",
                confirmLabel = "Record",
                busy = busy,
                onDismissRequest = { paying = null },
                onConfirm = {
                    val problem = pay.problem
                    if (problem != null) {
                        error = problem
                    } else {
                        paying = null
                        act({ if (it.status == "paid") "Invoice ${it.number} is paid." else "Payment recorded; ${InvoiceMoney.format(it.balance, it.currency)} still due." }) {
                            recordInvoicePayment(organization, invoiceId, pay)
                        }
                    }
                },
                fields = {
                    pay.problem?.let { Text(it, style = Fm.text.bodySmall, color = Fm.colors.danger) }
                    FmTextField(pay.amount, { paying = pay.copy(amount = it) }, hint = "Amount (${current.currency})")
                    FmTextField(pay.date, { paying = pay.copy(date = it) }, hint = "Date (YYYY-MM-DD)")
                    FmTextField(pay.method, { paying = pay.copy(method = it) }, hint = "Method, such as bank transfer")
                    FmTextField(pay.note, { paying = pay.copy(note = it) }, hint = "Note")
                },
            )
        }
        if (confirm.isNotEmpty() && current != null) {
            val deleting = confirm == "delete"
            FmConfirmDialog(
                title = if (deleting) "Delete invoice ${current.number}?" else "Void invoice ${current.number}?",
                body = if (deleting) "This cannot be undone." else "Its link will show it as void.",
                confirmLabel = if (deleting) "Delete" else "Void",
                destructive = true,
                onDismissRequest = { confirm = "" },
                onConfirm = {
                    confirm = ""
                    if (deleting) {
                        scope.invoiceWork({ busy = it }, { error = it }, { model.client.deleteInvoice(organization, invoiceId); model.load() }) {
                            InvoiceRevision.bump()
                            navigator.pop()
                        }
                    } else {
                        act({ "Invoice ${it.number} voided." }) { invoiceAction(organization, invoiceId, "void") }
                    }
                },
            )
        }
    }
}

@Composable
private fun InvoiceDetailBody(
    invoice: Invoice,
    organization: String,
    navigator: Navigator,
    busy: Boolean,
    onSend: () -> Unit,
    onPay: () -> Unit,
    onAction: (String) -> Unit,
    onCopy: () -> Unit,
    onShare: () -> Unit,
    onPreview: () -> Unit,
) {
    Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(18.dp)).background(Fm.colors.bgRaised).padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(invoice.number, style = Fm.text.titleLarge.copy(fontWeight = FontWeight.Bold), color = Fm.colors.textPrimary)
                InvoiceBadge(invoice)
            }
            val linked = invoice.contactId.isNotEmpty() && invoice.contact != null
            Text(
                invoice.clientLabel,
                Modifier.then(if (linked) Modifier.combinedClickableCompat({ navigator.push(ContactScreen(CrmContactRef(organization, invoice.contactId))) }) else Modifier),
                style = Fm.text.bodyLarge, color = if (linked) Fm.colors.accent else Fm.colors.textPrimary,
            )
            val sub = listOfNotNull(
                invoice.client.company.takeIf { it.isNotEmpty() && invoice.client.name.isNotEmpty() },
                invoice.client.email.takeIf { it.isNotEmpty() },
                if (invoice.contactRemoved) "CRM contact removed" else null,
            )
            if (sub.isNotEmpty()) Text(sub.joinToString(" · "), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            Text("Issued ${InvoiceDates.label(invoice.issuedAt)}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        }
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Fm.colors.bgRaised)) {
            Row {
                DetailMetric(
                    "Total", InvoiceMoney.format(invoice.total, invoice.currency),
                    if (invoice.paidAmount > 0) "${InvoiceMoney.format(invoice.paidAmount, invoice.currency)} paid" else "", Modifier.weight(1f),
                )
                DetailMetric(
                    "Due", InvoiceDates.label(invoice.dueAt),
                    when {
                        invoice.balance > 0 -> "${InvoiceMoney.format(invoice.balance, invoice.currency)} to pay"
                        invoice.status == "paid" -> "paid ${InvoiceDates.label(invoice.paidAt)}"
                        else -> ""
                    },
                    Modifier.weight(1f),
                )
            }
            Row {
                val openHint = if (invoice.firstOpenedAt > 0) "first ${relativeTime(invoice.firstOpenedAt)}" else if (invoice.wasSent) "not yet" else ""
                val viewHint = if (invoice.firstViewedAt > 0) "first ${relativeTime(invoice.firstViewedAt)}" else if (invoice.wasSent) "not yet" else ""
                DetailMetric("Email opens", if (invoice.wasSent) invoice.opens.toString() else "—", openHint, Modifier.weight(1f))
                DetailMetric(
                    "Page views", if (invoice.wasSent) invoice.views.toString() else "—",
                    if (invoice.clicks > 0) "$viewHint · ${invoice.clicks} pay ${if (invoice.clicks == 1) "click" else "clicks"}".trimStart(' ', '·') else viewHint,
                    Modifier.weight(1f),
                )
            }
        }
        if (invoice.wasSent) {
            Text(
                "Emailed to ${invoice.sentTo.ifEmpty { invoice.client.email.ifEmpty { "the client" } }} ${relativeTime(invoice.lastSentAt.takeIf { it > 0 } ?: invoice.sentAt)}" +
                    (if (invoice.sentBy.isNotEmpty()) " by ${invoice.sentBy}" else "") +
                    ". Opens count when the reader's mail app loads images; some apps load them for privacy without the reader opening it.",
                style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
            )
        }
        Column(Modifier.fillMaxWidth().padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (invoice.canSend) {
                FmToneButton(if (invoice.wasSent) "Send reminder" else "Email invoice", onPressed = if (busy) null else onSend, Modifier.fillMaxWidth(), icon = "send", tone = FmTone.Accent, filled = true, height = 42.dp)
            }
            if (invoice.canRecordPayment) FmToneButton("Record payment", onPressed = if (busy) null else onPay, Modifier.fillMaxWidth(), icon = "payments", tone = FmTone.Success, height = 42.dp)
            if (invoice.canMarkPaid) FmToneButton("Mark paid", onPressed = if (busy) null else onPay, Modifier.fillMaxWidth(), icon = "payments", tone = FmTone.Success, height = 42.dp)
            if (invoice.canReopen) FmToneButton("Reopen", onPressed = if (busy) null else ({ onAction("reopen") }), Modifier.fillMaxWidth(), icon = "undo", height = 42.dp)
        }
        Column {
            CrmSection("Items") {
                invoice.items.forEach { line ->
                    Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(line.description.ifEmpty { "Item" }, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                            Text("${InvoiceMoney.quantity(line.quantity)} × ${InvoiceMoney.format(line.unitAmount, invoice.currency)}", style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                        }
                        Text(InvoiceMoney.format(line.amount, invoice.currency), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    }
                }
                if (invoice.items.isEmpty()) Text("No line items yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                Spacer(Modifier.height(4.dp))
                TotalLine("Subtotal", InvoiceMoney.format(invoice.subtotal, invoice.currency))
                if (invoice.tax > 0) TotalLine("Tax (${InvoiceMoney.percent(invoice.taxRate)}%)", InvoiceMoney.format(invoice.tax, invoice.currency))
                if (invoice.paidAmount > 0) TotalLine("Paid", "−" + InvoiceMoney.format(invoice.paidAmount, invoice.currency))
                TotalLine("Total", InvoiceMoney.format(invoice.total, invoice.currency), strong = true)
                Spacer(Modifier.height(4.dp))
            }
            if (invoice.publicUrl.isNotEmpty()) {
                CrmSection("Client link") {
                    Text(invoice.publicUrl, Modifier.padding(horizontal = 14.dp, vertical = 6.dp), style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Row(Modifier.padding(horizontal = 8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        FmTextButton(onClick = onCopy) { Text("Copy link") }
                        FmTextButton(onClick = onShare) { Text("Share") }
                        FmTextButton(onClick = onPreview) { Text("Preview") }
                    }
                    Text("Opening the link yourself counts as a view.", Modifier.padding(horizontal = 14.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                }
            }
            if (invoice.paymentUrl.isNotEmpty()) {
                CrmSection("Pay link") { Text(invoice.paymentUrl, Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary) }
            }
            if (invoice.notes.isNotEmpty()) CrmSection("Notes") { Text(invoice.notes, Modifier.padding(14.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary) }
            if (invoice.terms.isNotEmpty()) CrmSection("Terms") { Text(invoice.terms, Modifier.padding(14.dp), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary) }
            CrmSection("History") {
                val events = invoice.events
                when {
                    events == null -> Text("Loading history…", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                    events.isEmpty() -> Text("Nothing has happened to this invoice yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                    else -> events.forEach { event ->
                        Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 7.dp), verticalAlignment = Alignment.Top) {
                            FmIcon(event.icon, size = 18.dp, tint = if (event.kind == "failed") Fm.colors.danger else Fm.colors.textTertiary)
                            Spacer(Modifier.width(10.dp))
                            Column(Modifier.weight(1f)) {
                                Text("${event.title} · ${relativeTime(event.at)}", style = Fm.text.bodySmall, color = Fm.colors.textPrimary)
                                val detail = event.detail(invoice.currency)
                                if (detail.isNotEmpty()) Text(detail, style = Fm.text.labelSmall, color = Fm.colors.textTertiary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            }
                        }
                    }
                }
            }
        }
        if (invoice.canVoid || invoice.deletable) {
            FmTextButton(onClick = { onAction(if (invoice.deletable) "delete" else "void") }, enabled = !busy, contentColor = Fm.colors.danger) {
                Text(if (invoice.deletable) "Delete invoice" else "Void invoice")
            }
        }
        Text(
            listOfNotNull(
                invoice.createdBy.takeIf { it.isNotEmpty() }?.let { "Created by $it" },
                invoice.updatedAt.takeIf { it > 0 }?.let { "updated ${relativeTime(it)}" + if (invoice.updatedBy.isNotEmpty()) " by ${invoice.updatedBy}" else "" },
            ).joinToString(" · "),
            Modifier.padding(vertical = 8.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
        )
        Spacer(Modifier.height(40.dp))
    }
}

class InvoiceEditorScreen(private val model: InvoicesModel, private val seed: InvoiceEditorSeed) : Screen() {
    override val title: String
        get() = when (seed) {
            is InvoiceEditorSeed.Edit -> "Edit ${seed.invoice.number}"
            else -> "New invoice"
        }

    private var draft by mutableStateOf<InvoiceDraft?>((seed as? InvoiceEditorSeed.Edit)?.invoice?.let(InvoiceDraft::of))
    private var contacts by mutableStateOf<List<InvoiceContactLink>>(emptyList())
    private var crmInstalled by mutableStateOf(false)
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var picking by mutableStateOf(false)
    private var contactQuery by mutableStateOf("")
    private var confirmDelete by mutableStateOf(false)

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        LaunchedEffect(Unit) {
            withContext(Dispatchers.IO) {
                model.ensureOverview()
                model.loadContacts()
            }
            crmInstalled = model.crmInstalled
            contacts = model.contacts.orEmpty()
            if (draft == null) {
                val now = System.currentTimeMillis()
                draft = when (seed) {
                    is InvoiceEditorSeed.New -> InvoiceDraft.blank(model.settings, now, seed.contact)
                    is InvoiceEditorSeed.Edit -> InvoiceDraft.of(seed.invoice)
                    is InvoiceEditorSeed.Duplicate -> InvoiceDraft.duplicate(seed.invoice, model.settings, now)
                }
            }
            if (model.overview == null && model.errorText.isNotEmpty()) error = model.errorText
        }
        val edit = draft
        val original = (seed as? InvoiceEditorSeed.Edit)?.invoice
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                FmTextButton(onClick = {
                    val current = draft ?: return@FmTextButton
                    val problem = current.problem
                    if (problem != null) {
                        error = problem
                        return@FmTextButton
                    }
                    error = ""
                    scope.invoiceWork({ saving = it }, { error = "Could not save the invoice: $it" }, {
                        val saved = model.client.saveInvoice(model.organization, current, crmInstalled)
                        model.load()
                        saved
                    }) { saved ->
                        InvoiceRevision.bump()
                        if (current.isNew) navigator.replace(InvoiceDetailScreen(model, saved.id)) else navigator.pop()
                    }
                }, enabled = !saving && edit != null) { Text(if (saving) "Saving…" else "Save") }
            }
            if (edit == null) {
                ErrorLine(error)
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                return@Column
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                if (crmInstalled) {
                    FmSectionHeader("CRM contact")
                    val chosen = contacts.firstOrNull { it.id == edit.contactId }
                    FmListRow(
                        chosen?.label ?: if (edit.contactId.isNotEmpty()) "Linked contact" else "Not linked to a CRM contact",
                        subtitle = if (chosen != null || edit.contactId.isNotEmpty()) "Tap to change. Choosing a contact fills the client's name, email and company." else "Tap to choose a contact",
                        leading = { FmIcon("contacts") },
                        onTap = { contactQuery = ""; picking = true },
                        minHeight = 52.dp,
                    )
                }
                FmSectionHeader("Client")
                FmTextField(edit.name, { draft = edit.copy(name = it) }, hint = "Name")
                FmTextField(edit.email, { draft = edit.copy(email = it) }, hint = "Email")
                FmTextField(edit.company, { draft = edit.copy(company = it) }, hint = "Company")
                FmTextField(edit.address, { draft = edit.copy(address = it) }, hint = "Address", minLines = 2, maxLines = 5)
                FmSectionHeader("Invoice", Modifier.padding(top = 6.dp))
                FmTextField(edit.number, { draft = edit.copy(number = it) }, hint = if (edit.isNew) "Number (automatic when empty)" else "Number")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(edit.issued, { draft = edit.copy(issued = it) }, Modifier.weight(1f), hint = "Issued (YYYY-MM-DD)")
                    FmTextField(edit.due, { draft = edit.copy(due = it) }, Modifier.weight(1f), hint = "Due (YYYY-MM-DD)")
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(edit.currency, { draft = edit.copy(currency = it.uppercase().take(3)) }, Modifier.weight(1f), hint = "Currency")
                    FmTextField(edit.tax, { draft = edit.copy(tax = it) }, Modifier.weight(1f), hint = "Tax %")
                }
                FmSectionHeader("Line items", Modifier.padding(top = 6.dp), trailing = {
                    CrmChip("Add line", Fm.colors.accent, icon = "add", onTap = { draft = edit.addLine() })
                })
                edit.lines.forEachIndexed { index, line -> LineEditor(index, line, edit.currencyCode, onChange = { next -> draft = edit.line(index) { next } }, onRemove = { draft = edit.removeLine(index) }) }
                Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Fm.colors.bgRaised).padding(vertical = 6.dp)) {
                    TotalLine("Subtotal", InvoiceMoney.format(edit.subtotal, edit.currencyCode))
                    TotalLine("Tax", InvoiceMoney.format(edit.taxAmount, edit.currencyCode))
                    TotalLine("Total", InvoiceMoney.format(edit.total, edit.currencyCode), strong = true)
                }
                FmSectionHeader("Payment and notes", Modifier.padding(top = 6.dp))
                FmTextField(edit.paymentUrl, { draft = edit.copy(paymentUrl = it) }, hint = "Pay link (https://)")
                FmTextField(edit.notes, { draft = edit.copy(notes = it) }, hint = "Notes for the client", minLines = 2, maxLines = 8)
                FmTextField(edit.terms, { draft = edit.copy(terms = it) }, hint = "Terms", minLines = 2, maxLines = 6)
                if (original != null && original.deletable) {
                    FmTextButton(onClick = { confirmDelete = true }, enabled = !saving, contentColor = Fm.colors.danger) { Text("Delete invoice") }
                }
                Spacer(Modifier.height(40.dp))
            }
        }
        if (picking) {
            FmModalBottomSheet(onDismissRequest = { picking = false }, isScrollControlled = true) {
                Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                    FmTextField(contactQuery, { contactQuery = it }, hint = "Search contacts")
                }
                val matches = contacts.filter { it.matches(contactQuery) }
                LazyColumn(Modifier.fillMaxWidth().heightIn(max = 480.dp)) {
                    item(key = "none") {
                        FmListRow("Not linked to a CRM contact", onTap = { draft = draft?.withContact(null); picking = false }, showChevron = false, minHeight = 48.dp)
                    }
                    items(matches.take(300), key = { it.id }) { contact ->
                        FmListRow(
                            contact.name.ifEmpty { contact.email.ifEmpty { "Unnamed contact" } },
                            subtitle = listOf(contact.company, contact.email).filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { null },
                            trailing = if (contact.id == draft?.contactId) ({ FmIcon("check", tint = Fm.colors.accent) }) else null,
                            onTap = { draft = draft?.withContact(contact); picking = false },
                            showChevron = false,
                            minHeight = 52.dp,
                        )
                    }
                    if (contacts.isEmpty()) item(key = "empty") { Text("No CRM contacts in this organization yet.", Modifier.padding(16.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary) }
                    item(key = "end") { Spacer(Modifier.height(24.dp)) }
                }
            }
        }
        if (confirmDelete && original != null) {
            FmConfirmDialog(
                "Delete invoice ${original.number}?",
                body = "This cannot be undone.",
                confirmLabel = "Delete",
                destructive = true,
                onDismissRequest = { confirmDelete = false },
                onConfirm = {
                    confirmDelete = false
                    scope.invoiceWork({ saving = it }, { error = "Could not delete the invoice: $it" }, { model.client.deleteInvoice(model.organization, original.id); model.load() }) {
                        InvoiceRevision.bump()
                        navigator.pop()
                        if ((navigator.top as? InvoiceDetailScreen)?.invoiceId == original.id) navigator.pop()
                    }
                },
            )
        }
    }
}

@Composable
private fun LineEditor(index: Int, line: InvoiceLineDraft, currency: String, onChange: (InvoiceLineDraft) -> Unit, onRemove: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Fm.colors.bgRaised).padding(10.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FmTextField(line.description, { onChange(line.copy(description = it)) }, Modifier.weight(1f), hint = "Item ${index + 1} description", maxLines = 3)
            FmIconButton("delete", "Remove line", onPressed = onRemove, color = Fm.colors.textTertiary)
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FmTextField(line.quantity, { onChange(line.copy(quantity = it)) }, Modifier.weight(1f), hint = "Quantity")
            FmTextField(line.unit, { onChange(line.copy(unit = it)) }, Modifier.weight(1.4f), hint = "Unit price")
            Text(InvoiceMoney.format(line.amount, currency), Modifier.weight(1.2f), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary, textAlign = TextAlign.End)
        }
    }
}

class InvoiceSettingsScreen(private val model: InvoicesModel) : Screen() {
    override val title: String get() = "Invoicing settings"

    private var settings by mutableStateOf(model.settings)
    private var dueDays by mutableStateOf(model.settings.dueDays.toString())
    private var nextNumber by mutableStateOf(model.settings.nextNumber.toString())
    private var tax by mutableStateOf(InvoiceMoney.percent(model.settings.taxRate))
    private var saving by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var touched = false

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val editable = model.canManage
        LaunchedEffect(Unit) {
            val fresh = withContext(Dispatchers.IO) { runCatching { model.client.invoiceSettings(model.organization) } }
            fresh.onSuccess { latest ->
                model.adopt(latest)
                if (!touched) {
                    settings = latest
                    dueDays = latest.dueDays.toString()
                    nextNumber = latest.nextNumber.toString()
                    tax = InvoiceMoney.percent(latest.taxRate)
                }
            }.onFailure { error = InvoiceErrors.describe(it) }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                if (editable) {
                    FmTextButton(onClick = {
                        val next = settings.copy(
                            dueDays = dueDays.trim().toIntOrNull()?.coerceIn(0, 365) ?: settings.dueDays,
                            nextNumber = nextNumber.trim().toLongOrNull()?.coerceAtLeast(1L) ?: settings.nextNumber,
                            taxRate = InvoiceMoney.basisPoints(tax),
                        )
                        val problem = next.problem
                        if (problem != null) {
                            error = problem
                            return@FmTextButton
                        }
                        scope.invoiceWork({ saving = it }, { error = "Settings were not saved: $it" }, { model.client.saveInvoiceSettings(model.organization, next) }) { saved ->
                            model.adopt(saved)
                            InvoiceRevision.bump()
                            navigator.pop()
                        }
                    }, enabled = !saving) { Text(if (saving) "Saving…" else "Save") }
                }
            }
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(
                    if (editable) "Shown on every invoice and invoice email." else "Only an organization owner or admin can change these.",
                    style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                if (error.isNotEmpty()) Text(error, style = Fm.text.bodySmall, color = Fm.colors.danger)
                FmSectionHeader("Business")
                FmTextField(settings.businessName, { touched = true; settings = settings.copy(businessName = it) }, hint = "Business name", enabled = editable)
                FmTextField(settings.businessAddress, { touched = true; settings = settings.copy(businessAddress = it) }, hint = "Business address", enabled = editable, minLines = 2, maxLines = 5)
                FmTextField(settings.fromName, { touched = true; settings = settings.copy(fromName = it) }, hint = "From name", enabled = editable)
                FmTextField(settings.fromEmail, { touched = true; settings = settings.copy(fromEmail = it) }, hint = "From email (uses the CRM sender when empty)", enabled = editable)
                FmSectionHeader("Defaults", Modifier.padding(top = 6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(settings.currency, { touched = true; settings = settings.copy(currency = it.uppercase().take(3)) }, Modifier.weight(1f), hint = "Default currency", enabled = editable)
                    FmTextField(tax, { touched = true; tax = it }, Modifier.weight(1f), hint = "Default tax rate (%)", enabled = editable)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FmTextField(settings.prefix, { touched = true; settings = settings.copy(prefix = it) }, Modifier.weight(1f), hint = "Number prefix", enabled = editable)
                    FmTextField(nextNumber, { touched = true; nextNumber = it.filter(Char::isDigit) }, Modifier.weight(1f), hint = "Next number", enabled = editable)
                }
                FmTextField(dueDays, { touched = true; dueDays = it.filter(Char::isDigit) }, hint = "Days until due", enabled = editable)
                FmTextField(settings.paymentUrl, { touched = true; settings = settings.copy(paymentUrl = it) }, hint = "Default pay link (https)", enabled = editable)
                FmSectionHeader("On the invoice", Modifier.padding(top = 6.dp))
                FmTextField(settings.paymentInstructions, { touched = true; settings = settings.copy(paymentInstructions = it) }, hint = "Payment instructions (bank details or how to pay)", enabled = editable, minLines = 3, maxLines = 8)
                FmTextField(settings.footer, { touched = true; settings = settings.copy(footer = it) }, hint = "Footer", enabled = editable, minLines = 2, maxLines = 5)
                Spacer(Modifier.height(40.dp))
            }
        }
    }
}

private sealed class ContactInvoicesState {
    data object Loading : ContactInvoicesState()
    data object Hidden : ContactInvoicesState()
    data object NotInstalled : ContactInvoicesState()
    data class Failed(val text: String) : ContactInvoicesState()
    data class Loaded(val data: ContactInvoices) : ContactInvoicesState()
}

@Composable
internal fun ColumnScope.CrmContactInvoices(organization: String, contact: CrmContact, navigator: Navigator) {
    val model = remember(organization) { MarketFeature.client()?.let { InvoicesModel(it, organization) } } ?: return
    var state by remember(organization, contact.id) { mutableStateOf<ContactInvoicesState>(ContactInvoicesState.Loading) }
    LaunchedEffect(organization, contact.id, InvoiceRevision.value) {
        val result = withContext(Dispatchers.IO) { runCatching { model.client.contactInvoices(organization, contact.id) } }
        state = result.fold({ ContactInvoicesState.Loaded(it) }) { thrown ->
            when {
                InvoiceErrors.status(thrown) == 404 -> ContactInvoicesState.Hidden
                InvoiceErrors.code(thrown) == "plugin_not_installed" -> ContactInvoicesState.NotInstalled
                InvoiceErrors.code(thrown) == "crm_not_installed" -> ContactInvoicesState.Hidden
                else -> ContactInvoicesState.Failed(InvoiceErrors.describe(thrown))
            }
        }
    }
    val current = state
    if (current is ContactInvoicesState.Hidden) return
    CrmSection("Invoices", trailing = {
        if (current is ContactInvoicesState.Loaded) {
            CrmChip("New invoice", Fm.colors.accent, icon = "add", onTap = {
                navigator.push(InvoiceEditorScreen(model, InvoiceEditorSeed.New(InvoiceContactLink.of(contact))))
            })
        }
    }) {
        when (current) {
            ContactInvoicesState.Loading -> Text("Loading invoices…", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            ContactInvoicesState.NotInstalled -> {
                Text(
                    "Invoicing is not installed for $organization. An owner or admin can install it to bill this contact.",
                    Modifier.padding(horizontal = 14.dp, vertical = 8.dp), style = Fm.text.bodySmall, color = Fm.colors.textSecondary,
                )
                Row(Modifier.padding(horizontal = 8.dp)) {
                    FmTextButton(onClick = { navigator.push(MarketFeature.packageScreen(InvoiceErrors.PACKAGE_ID)) }) { Text("Open in the Market") }
                }
            }
            is ContactInvoicesState.Failed -> Text(current.text, Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.danger)
            is ContactInvoicesState.Loaded -> {
                val data = current.data
                val currency = data.invoices.firstOrNull()?.currency ?: "USD"
                if (data.invoices.isNotEmpty()) {
                    Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp)) {
                        DetailMetric("Billed", InvoiceMoney.totals(data.billed, currency), "", Modifier.weight(1f))
                        DetailMetric("Paid", InvoiceMoney.totals(data.paid, currency), "", Modifier.weight(1f))
                        DetailMetric("Outstanding", InvoiceMoney.totals(data.outstanding, currency), "", Modifier.weight(1f))
                    }
                }
                data.invoices.forEach { invoice ->
                    Row(
                        Modifier.fillMaxWidth().combinedClickableCompat({ navigator.push(InvoiceDetailScreen(model, invoice.id)) }).padding(horizontal = 14.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                Text(invoice.number, style = Fm.text.bodyMedium.copy(fontWeight = FontWeight.SemiBold), color = Fm.colors.textPrimary)
                                InvoiceBadge(invoice)
                            }
                            Text(
                                if (invoice.wasSent) "${invoice.opens} ${if (invoice.opens == 1) "open" else "opens"} · ${invoice.views} ${if (invoice.views == 1) "view" else "views"} · due ${InvoiceDates.label(invoice.dueAt)}" else "Not sent",
                                style = Fm.text.labelSmall, color = Fm.colors.textTertiary,
                            )
                        }
                        Text(InvoiceMoney.format(invoice.total, invoice.currency), style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
                    }
                }
                if (data.invoices.isEmpty()) Text("No invoices for this contact yet.", Modifier.padding(14.dp), style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
            }
            ContactInvoicesState.Hidden -> Unit
        }
    }
}
