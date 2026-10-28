package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.math.BigDecimal
import java.math.RoundingMode
import java.text.NumberFormat
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Currency
import java.util.Locale
import kotlin.math.roundToLong

object InvoiceMoney {
    fun format(minor: Long, currency: String, locale: Locale = Locale.getDefault()): String {
        val code = currency.trim().uppercase()
        val value = BigDecimal.valueOf(minor).movePointLeft(2)
        return try {
            val format = NumberFormat.getCurrencyInstance(locale)
            format.currency = Currency.getInstance(code)
            format.minimumFractionDigits = 2
            format.maximumFractionDigits = 2
            format.format(value)
        } catch (_: Exception) {
            "$code ${value.setScale(2, RoundingMode.HALF_UP).toPlainString()}".trim()
        }
    }

    fun totals(amounts: Map<String, Long>, fallbackCurrency: String, locale: Locale = Locale.getDefault()): String {
        val entries = amounts.filterValues { it != 0L }
        if (entries.isEmpty()) return format(0, fallbackCurrency.ifEmpty { "USD" }, locale)
        return entries.entries.joinToString(" · ") { format(it.value, it.key, locale) }
    }

    fun minor(text: String): Long {
        val cleaned = text.filter { it.isDigit() || it == '.' || it == '-' }
        val number = cleaned.toBigDecimalOrNull() ?: return 0
        return maxOf(0L, number.movePointRight(2).setScale(0, RoundingMode.HALF_UP).toLong())
    }

    fun input(minor: Long): String = BigDecimal.valueOf(minor).movePointLeft(2).setScale(2, RoundingMode.HALF_UP).toPlainString()

    fun basisPoints(percent: String): Long {
        val number = percent.trim().toBigDecimalOrNull() ?: return 0
        return number.movePointRight(2).setScale(0, RoundingMode.HALF_UP).toLong().coerceIn(0L, 100_000L)
    }

    fun percent(basisPoints: Long): String = BigDecimal.valueOf(basisPoints).movePointLeft(2).stripTrailingZeros().toPlainString()

    fun quantity(value: Double): String =
        if (value == Math.floor(value) && !value.isInfinite()) value.toLong().toString() else BigDecimal.valueOf(value).stripTrailingZeros().toPlainString()

    fun lineAmount(quantity: Double, unitAmount: Long): Long = (quantity * unitAmount).roundToLong()

    fun tax(subtotal: Long, basisPoints: Long): Long = (subtotal * basisPoints / 10_000.0).roundToLong()
}

object InvoiceDates {
    const val DAY_MS = 86_400_000L
    private val INPUT: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE
    private val LABEL: DateTimeFormatter = DateTimeFormatter.ofPattern("d MMM yyyy", Locale.ENGLISH)

    fun input(ms: Long): String = if (ms <= 0) "" else Instant.ofEpochMilli(ms).atZone(ZoneOffset.UTC).toLocalDate().format(INPUT)

    fun parse(text: String): Long? {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return null
        return try {
            LocalDate.parse(trimmed, INPUT).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
        } catch (_: Exception) {
            null
        }
    }

    fun today(nowMs: Long): Long = nowMs - Math.floorMod(nowMs, DAY_MS)

    fun label(ms: Long): String = if (ms <= 0) "—" else Instant.ofEpochMilli(ms).atZone(ZoneOffset.UTC).toLocalDate().format(LABEL)
}

data class InvoiceClient(val name: String = "", val email: String = "", val company: String = "", val address: String = "") {
    val label: String get() = name.ifEmpty { company.ifEmpty { email.ifEmpty { "No client" } } }

    companion object {
        fun fromJson(json: JsonMap) = InvoiceClient(json.str("name"), json.str("email"), json.str("company"), json.str("address"))
    }
}

data class InvoiceLine(val description: String = "", val quantity: Double = 1.0, val unitAmount: Long = 0) {
    val amount: Long get() = InvoiceMoney.lineAmount(quantity, unitAmount)

    companion object {
        fun fromJson(json: JsonMap) = InvoiceLine(json.str("description"), json.double("quantity", 1.0), maxOf(0L, json.long("unitAmount")))
    }
}

data class InvoiceContactLink(val id: String, val name: String = "", val email: String = "", val company: String = "", val stage: String = "") {
    val label: String get() = listOf(name.ifEmpty { email }, company).filter { it.isNotEmpty() }.joinToString(" · ").ifEmpty { "Unnamed contact" }

    fun matches(query: String): Boolean = PackagesText.matches(query, name, email, company)

    companion object {
        fun fromJson(json: JsonMap) = InvoiceContactLink(json.str("id"), json.str("name"), json.str("email"), json.str("company"), json.str("stage"))

        fun of(contact: CrmContact) = InvoiceContactLink(contact.id, contact.name, contact.email, contact.company, contact.stage)
    }
}

data class InvoiceEvent(
    val kind: String,
    val at: Long,
    val actor: String = "",
    val client: String = "",
    val to: String = "",
    val amount: Long = 0,
    val method: String = "",
    val error: String = "",
    val invoiceId: String = "",
    val number: String = "",
    val clientName: String = "",
    val currency: String = "",
) {
    val title: String get() = LABELS[kind] ?: CrmStageLabel.title(kind)

    val icon: String get() = ICONS[kind] ?: "radio_button_unchecked"

    fun detail(currency: String, locale: Locale = Locale.getDefault()): String = listOf(
        if (to.isNotEmpty()) "to $to" else "",
        if (client.isNotEmpty()) "in $client" else "",
        if (amount > 0) InvoiceMoney.format(amount, currency, locale) else "",
        method,
        if (actor.isNotEmpty()) "by $actor" else "",
        error,
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun activityLine(locale: Locale = Locale.getDefault()): String = listOf(
        clientName,
        client,
        if (kind == "paid" && amount > 0) InvoiceMoney.format(amount, currency, locale) else "",
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    companion object {
        val LABELS = mapOf(
            "created" to "Created", "sent" to "Emailed", "reminder" to "Reminder emailed", "failed" to "Email failed",
            "opened" to "Email opened", "viewed" to "Invoice viewed", "clicked" to "Pay link clicked", "paid" to "Payment recorded",
            "reopened" to "Reopened", "voided" to "Voided", "overdue" to "Became overdue",
        )
        val ICONS = mapOf(
            "created" to "note_add", "sent" to "send", "reminder" to "notifications", "failed" to "error",
            "opened" to "drafts", "viewed" to "visibility", "clicked" to "ads_click", "paid" to "verified",
            "reopened" to "undo", "voided" to "block", "overdue" to "schedule",
        )

        fun fromJson(json: JsonMap) = InvoiceEvent(
            json.str("kind"), json.long("at"), json.str("actor"), json.str("client"), json.str("to"), maxOf(0L, json.long("amount")),
            json.str("method"), json.str("error"), json.str("invoiceId"), json.str("number"), json.str("clientName"), json.str("currency"),
        )
    }
}

data class InvoicePayment(val amount: Long, val at: Long, val method: String = "", val note: String = "", val by: String = "") {
    companion object {
        fun fromJson(json: JsonMap) = InvoicePayment(json.long("amount"), json.long("at"), json.str("method"), json.str("note"), json.str("by"))
    }
}

enum class InvoiceFilter(val wire: String, val title: String) {
    ALL("", "All"), OPEN("open", "Unpaid"), OVERDUE("overdue", "Overdue"), DRAFT("draft", "Drafts"), PAID("paid", "Paid"), VOID("void", "Void");

    companion object {
        fun of(wire: String?): InvoiceFilter = entries.firstOrNull { it.wire == wire } ?: ALL
    }
}

data class Invoice(
    val id: String,
    val organization: String = "",
    val number: String = "",
    val status: String = "draft",
    val displayStatus: String = "",
    val contactId: String = "",
    val client: InvoiceClient = InvoiceClient(),
    val currency: String = "USD",
    val items: List<InvoiceLine> = emptyList(),
    val taxRate: Long = 0,
    val subtotal: Long = 0,
    val tax: Long = 0,
    val total: Long = 0,
    val paidAmount: Long = 0,
    val balance: Long = 0,
    val notes: String = "",
    val terms: String = "",
    val paymentUrl: String = "",
    val payments: List<InvoicePayment> = emptyList(),
    val issuedAt: Long = 0,
    val dueAt: Long = 0,
    val sentAt: Long = 0,
    val lastSentAt: Long = 0,
    val sentTo: String = "",
    val paidAt: Long = 0,
    val opens: Int = 0,
    val views: Int = 0,
    val clicks: Int = 0,
    val firstOpenedAt: Long = 0,
    val firstViewedAt: Long = 0,
    val lastActivityAt: Long = 0,
    val createdBy: String = "",
    val updatedBy: String = "",
    val sentBy: String = "",
    val createdAt: Long = 0,
    val updatedAt: Long = 0,
    val publicUrl: String = "",
    val events: List<InvoiceEvent>? = null,
    val contact: InvoiceContactLink? = null,
) {
    val state: String get() = displayStatus.ifEmpty { status }

    val statusLabel: String get() = STATUS_LABELS[state] ?: CrmStageLabel.title(state)

    val clientLabel: String get() = client.label

    val editable: Boolean get() = status in EDITABLE

    val deletable: Boolean get() = status in DELETABLE

    val canVoid: Boolean get() = !deletable && status != "void"

    val canReopen: Boolean get() = status == "paid" || status == "void"

    val canSend: Boolean get() = editable

    val canRecordPayment: Boolean get() = editable && sentAt > 0

    val canMarkPaid: Boolean get() = status == "draft"

    val wasSent: Boolean get() = sentAt > 0

    val contactRemoved: Boolean get() = contactId.isNotEmpty() && events != null && contact == null

    fun matches(query: String, filter: InvoiceFilter): Boolean {
        val passes = when (filter) {
            InvoiceFilter.ALL -> true
            InvoiceFilter.OPEN -> state in setOf("sent", "partial", "overdue")
            InvoiceFilter.PAID -> state == "paid" || status == "paid"
            else -> state == filter.wire
        }
        return passes && PackagesText.matches(query, number, client.name, client.company, client.email)
    }

    companion object {
        val STATUS_LABELS = mapOf(
            "draft" to "Draft", "sent" to "Sent", "partial" to "Part paid", "overdue" to "Overdue", "paid" to "Paid", "void" to "Void",
        )
        val EDITABLE = setOf("draft", "sent")
        val DELETABLE = setOf("draft", "void")

        fun fromJson(json: JsonMap): Invoice = Invoice(
            id = json.str("id"),
            organization = json.str("organization"),
            number = json.str("number"),
            status = json.str("status", "draft"),
            displayStatus = json.str("displayStatus"),
            contactId = json.str("contactId"),
            client = InvoiceClient.fromJson(json.obj("client")),
            currency = json.str("currency", "USD"),
            items = json.maps("items").map(InvoiceLine::fromJson),
            taxRate = json.long("taxRate"),
            subtotal = json.long("subtotal"),
            tax = json.long("tax"),
            total = json.long("total"),
            paidAmount = json.long("paidAmount"),
            balance = json.long("balance"),
            notes = json.str("notes"),
            terms = json.str("terms"),
            paymentUrl = json.str("paymentUrl"),
            payments = json.maps("payments").map(InvoicePayment::fromJson),
            issuedAt = json.long("issuedAt"),
            dueAt = json.long("dueAt"),
            sentAt = json.long("sentAt"),
            lastSentAt = json.long("lastSentAt"),
            sentTo = json.str("sentTo"),
            paidAt = json.long("paidAt"),
            opens = json.int("opens"),
            views = json.int("views"),
            clicks = json.int("clicks"),
            firstOpenedAt = json.long("firstOpenedAt"),
            firstViewedAt = json.long("firstViewedAt"),
            lastActivityAt = json.long("lastActivityAt"),
            createdBy = json.str("createdBy"),
            updatedBy = json.str("updatedBy"),
            sentBy = json.str("sentBy"),
            createdAt = json.long("createdAt"),
            updatedAt = json.long("updatedAt"),
            publicUrl = json.str("publicUrl").takeIf { it.startsWith("https://") }.orEmpty(),
            events = if (json.containsKey("events")) json.maps("events").map(InvoiceEvent::fromJson) else null,
            contact = json.obj("contact").takeIf { it.isNotEmpty() }?.let(InvoiceContactLink::fromJson),
        )
    }
}

data class InvoiceSummary(
    val counts: Map<String, Int> = emptyMap(),
    val outstanding: Map<String, Long> = emptyMap(),
    val overdueAmount: Map<String, Long> = emptyMap(),
    val paidLast30Days: Map<String, Long> = emptyMap(),
    val sent: Int = 0,
    val opened: Int = 0,
    val viewed: Int = 0,
    val clicked: Int = 0,
    val openRate: Int = 0,
    val viewRate: Int = 0,
    val averageDaysToPay: Double = 0.0,
    val averageHoursToOpen: Double = 0.0,
) {
    fun count(key: String): Int = counts[key] ?: 0

    val openRateText: String get() = if (sent > 0) "$openRate%" else "—"

    val engagementText: String
        get() = if (sent > 0) "$opened of $sent opened · $viewed viewed · $clicked clicked pay" else "No invoices sent yet"

    val daysToPayText: String get() = if (averageDaysToPay > 0) "${number(averageDaysToPay)} days" else "—"

    val hoursToOpenText: String
        get() = if (averageHoursToOpen > 0) "Opened ${number(averageHoursToOpen)} h after sending, on average" else "From sending to paid"

    companion object {
        fun plural(count: Int, one: String, many: String = one + "s"): String = "$count ${if (count == 1) one else many}"

        private fun number(value: Double): String = BigDecimal.valueOf(value).setScale(1, RoundingMode.HALF_UP).stripTrailingZeros().toPlainString()

        private fun amounts(json: JsonMap): Map<String, Long> = json.mapValues { jsonLong(it.value) }.filterValues { it != 0L }

        fun fromJson(json: JsonMap) = InvoiceSummary(
            counts = json.obj("counts").mapValues { jsonLong(it.value).toInt() },
            outstanding = amounts(json.obj("outstanding")),
            overdueAmount = amounts(json.obj("overdueAmount")),
            paidLast30Days = amounts(json.obj("paidLast30Days")),
            sent = json.int("sent"),
            opened = json.int("opened"),
            viewed = json.int("viewed"),
            clicked = json.int("clicked"),
            openRate = json.int("openRate"),
            viewRate = json.int("viewRate"),
            averageDaysToPay = json.double("averageDaysToPay"),
            averageHoursToOpen = json.double("averageHoursToOpen"),
        )
    }
}

data class InvoiceSettings(
    val businessName: String = "",
    val businessAddress: String = "",
    val fromName: String = "",
    val fromEmail: String = "",
    val currency: String = "USD",
    val dueDays: Int = 30,
    val taxRate: Long = 0,
    val prefix: String = "INV-",
    val nextNumber: Long = 1,
    val paymentUrl: String = "",
    val paymentInstructions: String = "",
    val footer: String = "",
) {
    val body: Map<String, Any?>
        get() = linkedMapOf(
            "businessName" to businessName.trim(),
            "businessAddress" to businessAddress.trim(),
            "fromName" to fromName.trim(),
            "fromEmail" to fromEmail.trim(),
            "currency" to currency.trim().uppercase(),
            "dueDays" to dueDays.coerceIn(0, 365),
            "taxRate" to taxRate.coerceIn(0L, 100_000L),
            "prefix" to prefix.trim(),
            "nextNumber" to nextNumber.coerceAtLeast(1L),
            "paymentUrl" to paymentUrl.trim(),
            "paymentInstructions" to paymentInstructions.trim(),
            "footer" to footer.trim(),
        )

    val problem: String?
        get() = when {
            !InvoiceDraft.currencyIsValid(currency) -> "Use a three-letter currency code such as USD."
            paymentUrl.isNotBlank() && !InvoiceDraft.linkIsValid(paymentUrl) -> "The pay link must start with https://."
            fromEmail.isNotBlank() && !fromEmail.trim().contains('@') -> "That From email is not valid."
            else -> null
        }

    companion object {
        fun fromJson(json: JsonMap) = InvoiceSettings(
            businessName = json.str("businessName"),
            businessAddress = json.str("businessAddress"),
            fromName = json.str("fromName"),
            fromEmail = json.str("fromEmail"),
            currency = json.str("currency", "USD").ifEmpty { "USD" },
            dueDays = json.int("dueDays", 30),
            taxRate = json.long("taxRate"),
            prefix = json.str("prefix", "INV-"),
            nextNumber = json.long("nextNumber", 1).coerceAtLeast(1L),
            paymentUrl = json.str("paymentUrl"),
            paymentInstructions = json.str("paymentInstructions"),
            footer = json.str("footer"),
        )
    }
}

data class InvoicesOverview(
    val organization: String,
    val role: String = "member",
    val canManagePlugin: Boolean = false,
    val crmInstalled: Boolean = false,
    val invoices: List<Invoice> = emptyList(),
    val summary: InvoiceSummary = InvoiceSummary(),
    val activity: List<InvoiceEvent> = emptyList(),
    val settings: InvoiceSettings = InvoiceSettings(),
) {
    val canManage: Boolean get() = canManagePlugin || role == "owner" || role == "admin"

    fun invoice(id: String): Invoice? = invoices.firstOrNull { it.id == id }

    fun filtered(query: String, filter: InvoiceFilter): List<Invoice> = invoices.filter { it.matches(query, filter) }

    fun count(filter: InvoiceFilter): Int = invoices.count { it.matches("", filter) }

    companion object {
        fun fromJson(json: JsonMap) = InvoicesOverview(
            organization = json.str("organization"),
            role = json.str("role", "member"),
            canManagePlugin = json.bool("canManagePlugin"),
            crmInstalled = json.bool("crmInstalled"),
            invoices = json.maps("invoices").map(Invoice::fromJson).filter { PackagesText.isHexId(it.id) },
            summary = InvoiceSummary.fromJson(json.obj("summary")),
            activity = json.maps("activity").map(InvoiceEvent::fromJson),
            settings = InvoiceSettings.fromJson(json.obj("settings")),
        )
    }
}

data class ContactInvoices(
    val contactId: String,
    val invoices: List<Invoice>,
    val billed: Map<String, Long>,
    val paid: Map<String, Long>,
    val outstanding: Map<String, Long>,
) {
    companion object {
        private fun amounts(json: JsonMap): Map<String, Long> = json.mapValues { jsonLong(it.value) }.filterValues { it != 0L }

        fun fromJson(json: JsonMap): ContactInvoices {
            val totals = json.obj("totals")
            return ContactInvoices(
                json.str("contactId"),
                json.maps("invoices").map(Invoice::fromJson).filter { PackagesText.isHexId(it.id) },
                amounts(totals.obj("billed")), amounts(totals.obj("paid")), amounts(totals.obj("outstanding")),
            )
        }
    }
}

data class InvoiceLineDraft(val description: String = "", val quantity: String = "1", val unit: String = "") {
    val quantityValue: Double get() = quantity.trim().toDoubleOrNull()?.takeIf { it.isFinite() && it >= 0 } ?: 0.0

    val unitAmount: Long get() = InvoiceMoney.minor(unit)

    val amount: Long get() = InvoiceMoney.lineAmount(quantityValue, unitAmount)

    val isBlank: Boolean get() = description.isBlank() && unitAmount == 0L

    companion object {
        fun of(line: InvoiceLine) = InvoiceLineDraft(line.description, InvoiceMoney.quantity(line.quantity), InvoiceMoney.input(line.unitAmount))
    }
}

data class InvoiceDraft(
    val id: String = "",
    val contactId: String = "",
    val name: String = "",
    val email: String = "",
    val company: String = "",
    val address: String = "",
    val number: String = "",
    val issued: String = "",
    val due: String = "",
    val currency: String = "USD",
    val tax: String = "0",
    val paymentUrl: String = "",
    val notes: String = "",
    val terms: String = "",
    val lines: List<InvoiceLineDraft> = listOf(InvoiceLineDraft()),
) {
    val isNew: Boolean get() = id.isEmpty()

    val currencyCode: String get() = currency.trim().uppercase().ifEmpty { "USD" }

    val taxRate: Long get() = InvoiceMoney.basisPoints(tax)

    val subtotal: Long get() = lines.sumOf { it.amount }

    val taxAmount: Long get() = InvoiceMoney.tax(subtotal, taxRate)

    val total: Long get() = subtotal + taxAmount

    val problem: String?
        get() = when {
            issued.isNotBlank() && InvoiceDates.parse(issued) == null -> "Write the issue date as YYYY-MM-DD."
            due.isNotBlank() && InvoiceDates.parse(due) == null -> "Write the due date as YYYY-MM-DD."
            !currencyIsValid(currency) -> "Use a three-letter currency code such as USD."
            tax.isNotBlank() && tax.trim().toBigDecimalOrNull() == null -> "Write the tax rate as a number, such as 7.5."
            paymentUrl.isNotBlank() && !linkIsValid(paymentUrl) -> "The pay link must start with https://."
            lines.any { it.quantity.isNotBlank() && it.quantity.trim().toDoubleOrNull() == null } -> "Quantities are numbers, such as 1 or 2.5."
            else -> null
        }

    fun withContact(contact: InvoiceContactLink?): InvoiceDraft {
        if (contact == null) return copy(contactId = "")
        return copy(
            contactId = contact.id,
            name = contact.name.ifEmpty { name },
            email = contact.email.ifEmpty { email },
            company = contact.company.ifEmpty { company },
        )
    }

    fun line(index: Int, change: (InvoiceLineDraft) -> InvoiceLineDraft): InvoiceDraft =
        copy(lines = lines.mapIndexed { at, line -> if (at == index) change(line) else line })

    fun addLine(): InvoiceDraft = copy(lines = lines + InvoiceLineDraft())

    fun removeLine(index: Int): InvoiceDraft {
        val rest = lines.filterIndexed { at, _ -> at != index }
        return copy(lines = rest.ifEmpty { listOf(InvoiceLineDraft()) })
    }

    fun body(crmInstalled: Boolean): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>(
            "client" to linkedMapOf("name" to name, "email" to email, "company" to company, "address" to address),
            "currency" to currencyCode,
            "taxRate" to taxRate,
            "issuedAt" to (InvoiceDates.parse(issued) ?: 0L),
            "dueAt" to (InvoiceDates.parse(due) ?: 0L),
            "paymentUrl" to paymentUrl.trim(),
            "notes" to notes,
            "terms" to terms,
            "items" to lines.filter { !it.isBlank }.map {
                linkedMapOf("description" to it.description, "quantity" to it.quantityValue, "unitAmount" to it.unitAmount)
            },
        )
        val trimmed = number.trim()
        if (trimmed.isNotEmpty()) body["number"] = trimmed
        if (crmInstalled) body["contactId"] = contactId
        return body
    }

    companion object {
        fun currencyIsValid(value: String): Boolean {
            val code = value.trim().uppercase()
            return code.length == 3 && code.all { it in 'A'..'Z' }
        }

        fun linkIsValid(value: String): Boolean {
            val trimmed = value.trim()
            return trimmed.lowercase().startsWith("https://") && trimmed.length > 8 && trimmed.none { it.isWhitespace() || it in "<>\"'" }
        }

        fun blank(settings: InvoiceSettings, nowMs: Long, contact: InvoiceContactLink? = null): InvoiceDraft {
            val issued = InvoiceDates.today(nowMs)
            return InvoiceDraft(
                issued = InvoiceDates.input(issued),
                due = InvoiceDates.input(issued + settings.dueDays * InvoiceDates.DAY_MS),
                currency = settings.currency.ifEmpty { "USD" },
                tax = InvoiceMoney.percent(settings.taxRate),
                paymentUrl = settings.paymentUrl,
            ).withContact(contact)
        }

        fun of(invoice: Invoice) = InvoiceDraft(
            id = invoice.id,
            contactId = invoice.contactId,
            name = invoice.client.name,
            email = invoice.client.email,
            company = invoice.client.company,
            address = invoice.client.address,
            number = invoice.number,
            issued = InvoiceDates.input(invoice.issuedAt),
            due = InvoiceDates.input(invoice.dueAt),
            currency = invoice.currency,
            tax = InvoiceMoney.percent(invoice.taxRate),
            paymentUrl = invoice.paymentUrl,
            notes = invoice.notes,
            terms = invoice.terms,
            lines = invoice.items.map(InvoiceLineDraft::of).ifEmpty { listOf(InvoiceLineDraft()) },
        )

        fun duplicate(invoice: Invoice, settings: InvoiceSettings, nowMs: Long): InvoiceDraft {
            val issued = InvoiceDates.today(nowMs)
            return of(invoice).copy(
                id = "",
                number = "",
                issued = InvoiceDates.input(issued),
                due = InvoiceDates.input(issued + settings.dueDays * InvoiceDates.DAY_MS),
            )
        }
    }
}

data class InvoiceSendDraft(val to: String = "", val subject: String = "", val message: String = "") {
    val body: Map<String, Any?> get() = linkedMapOf("to" to to.trim(), "subject" to subject.trim(), "message" to message)

    companion object {
        fun of(invoice: Invoice) = InvoiceSendDraft(to = invoice.client.email)

        fun subjectHint(invoice: Invoice, business: String): String =
            "${if (invoice.wasSent) "Reminder: invoice" else "Invoice"} ${invoice.number} from $business"
    }
}

data class InvoicePaymentDraft(val amount: String = "", val date: String = "", val method: String = "", val note: String = "") {
    val amountMinor: Long get() = InvoiceMoney.minor(amount)

    val problem: String?
        get() = when {
            amountMinor <= 0 -> "Enter the amount received."
            date.isNotBlank() && InvoiceDates.parse(date) == null -> "Write the date as YYYY-MM-DD."
            else -> null
        }

    val body: Map<String, Any?>
        get() = linkedMapOf("amount" to amountMinor, "paidAt" to (InvoiceDates.parse(date) ?: 0L), "method" to method.trim(), "note" to note.trim())

    companion object {
        fun of(invoice: Invoice, nowMs: Long) = InvoicePaymentDraft(
            amount = InvoiceMoney.input(if (invoice.balance > 0) invoice.balance else invoice.total),
            date = InvoiceDates.input(InvoiceDates.today(nowMs)),
        )
    }
}

object InvoiceErrors {
    const val PACKAGE_ID = "invoices"

    fun sentence(code: String): String? = when (code) {
        "invoice_locked" -> "Only draft and sent invoices can be changed."
        "void_before_delete" -> "Void a sent invoice before deleting it; only drafts and void invoices can be deleted."
        "invoice_already_void" -> "That invoice is already void."
        "invoice_not_closed" -> "Only paid or void invoices can be reopened."
        "invoice_has_no_amount" -> "Add a line with an amount before emailing the invoice."
        "invalid_amount" -> "Enter the amount received."
        "invalid_payment_url" -> "The pay link must start with https://."
        "invoice_number_taken" -> "Another invoice already uses that number."
        "invalid_contact_id", "crm_contact_not_found" -> "That CRM contact was not found."
        "crm_not_installed" -> "The CRM is not installed for this organization."
        "settings_require_admin" -> "Only an owner or admin of the organization can change Invoicing settings."
        "invoice_not_found" -> "That invoice was removed or you no longer have access."
        "invoice_catalog_full" -> "This organization has reached 10,000 invoices."
        "invalid_sender" -> "Add a From email in Invoicing settings, or a sender in the CRM's email settings."
        "invalid_recipient" -> "Add the client's email address."
        "plugin_not_installed" -> "Invoicing is not installed for that organization yet."
        else -> null
    }

    fun describe(error: Throwable): String {
        if (error is PackagesException) sentence(error.code)?.let { return it }
        return PackagesException.describe(error)
    }

    fun code(error: Throwable): String = (error as? PackagesException)?.code.orEmpty()

    fun status(error: Throwable): Int = (error as? PackagesException)?.status ?: 0
}

private fun PackagesClient.invoicePath(organization: String, suffix: String = ""): String = "/api/invoices/" + segment(organization) + suffix

fun PackagesClient.invoiceOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/invoices"))

fun PackagesClient.invoices(organization: String): InvoicesOverview = InvoicesOverview.fromJson(call("GET", invoicePath(organization)))

fun PackagesClient.invoice(organization: String, id: String): Invoice =
    Invoice.fromJson(call("GET", invoicePath(organization, "/items/" + item(id))).obj("invoice"))

fun PackagesClient.saveInvoice(organization: String, draft: InvoiceDraft, crmInstalled: Boolean): Invoice {
    val body = draft.body(crmInstalled)
    if (draft.isNew) return Invoice.fromJson(call("POST", invoicePath(organization, "/items"), body = body).obj("invoice"))
    return Invoice.fromJson(call("PATCH", invoicePath(organization, "/items/" + item(draft.id)), body = body).obj("invoice"))
}

fun PackagesClient.deleteInvoice(organization: String, id: String) {
    call("DELETE", invoicePath(organization, "/items/" + item(id)), body = emptyMap())
}

fun PackagesClient.sendInvoice(organization: String, id: String, send: InvoiceSendDraft): Invoice =
    Invoice.fromJson(call("POST", invoicePath(organization, "/items/${item(id)}/send"), body = send.body).obj("invoice"))

fun PackagesClient.recordInvoicePayment(organization: String, id: String, payment: InvoicePaymentDraft): Invoice =
    Invoice.fromJson(call("POST", invoicePath(organization, "/items/${item(id)}/paid"), body = payment.body).obj("invoice"))

fun PackagesClient.invoiceAction(organization: String, id: String, action: String): Invoice {
    if (action != "void" && action != "reopen") throw PackagesException("invalid_action")
    return Invoice.fromJson(call("POST", invoicePath(organization, "/items/${item(id)}/$action"), body = emptyMap()).obj("invoice"))
}

fun PackagesClient.invoiceSettings(organization: String): InvoiceSettings =
    InvoiceSettings.fromJson(call("GET", invoicePath(organization, "/settings")).obj("settings"))

fun PackagesClient.saveInvoiceSettings(organization: String, settings: InvoiceSettings): InvoiceSettings =
    InvoiceSettings.fromJson(call("PATCH", invoicePath(organization, "/settings"), body = settings.body).obj("settings"))

fun PackagesClient.invoiceContacts(organization: String): List<InvoiceContactLink> =
    call("GET", invoicePath(organization, "/contacts")).maps("contacts").map(InvoiceContactLink::fromJson).filter { PackagesText.isHexId(it.id) }

fun PackagesClient.contactInvoices(organization: String, contactId: String): ContactInvoices =
    ContactInvoices.fromJson(call("GET", invoicePath(organization, "/contacts/" + item(contactId))))

class InvoicesModel(val client: PackagesClient, var organization: String = "") {
    var organizations: List<PackageOrganization> = emptyList()
        private set
    var overview: InvoicesOverview? = null
        private set
    var contacts: List<InvoiceContactLink>? = null
        private set
    var errorText: String = ""

    val selected: PackageOrganization? get() = organizations.firstOrNull { it.name == organization }

    val needsInstall: Boolean get() = selected?.installed == false

    val canInstall: Boolean get() = selected?.canManage == true

    val canManage: Boolean get() = overview?.canManage ?: (selected?.canManage == true)

    val crmInstalled: Boolean get() = overview?.crmInstalled == true

    val settings: InvoiceSettings get() = overview?.settings ?: InvoiceSettings()

    fun refresh(preferred: String? = null) {
        try {
            organizations = client.invoiceOrganizations()
            errorText = ""
        } catch (thrown: Exception) {
            errorText = InvoiceErrors.describe(thrown)
            return
        }
        val wanted = preferred?.takeIf { name -> organizations.any { it.name == name } }
        organization = wanted
            ?: organization.takeIf { name -> organizations.any { it.name == name } }
            ?: organizations.firstOrNull { it.installed }?.name
            ?: organizations.firstOrNull()?.name
            ?: ""
        load()
    }

    fun choose(name: String) {
        if (name == organization) return
        organization = name
        overview = null
        contacts = null
        load()
    }

    fun load() {
        if (!PackagesText.isOrgName(organization)) {
            overview = null
            return
        }
        if (needsInstall) {
            overview = null
            return
        }
        try {
            val next = client.invoices(organization)
            if (overview?.crmInstalled != next.crmInstalled) contacts = null
            overview = next
            errorText = ""
        } catch (thrown: Exception) {
            if (InvoiceErrors.code(thrown) == "plugin_not_installed") {
                markInstalled(false)
                overview = null
                errorText = ""
            } else {
                errorText = InvoiceErrors.describe(thrown)
            }
        }
    }

    fun install(): Boolean = try {
        client.install(InvoiceErrors.PACKAGE_ID, organization)
        markInstalled(true)
        errorText = ""
        load()
        true
    } catch (thrown: Exception) {
        errorText = InvoiceErrors.describe(thrown)
        false
    }

    fun loadContacts(): List<InvoiceContactLink> {
        contacts?.let { return it }
        if (!crmInstalled) return emptyList()
        val loaded = runCatching { client.invoiceContacts(organization) }.getOrDefault(emptyList())
        contacts = loaded
        return loaded
    }

    fun ensureOverview() {
        if (overview == null) {
            if (organizations.isEmpty() && organization.isEmpty()) refresh() else load()
        }
    }

    fun adopt(settings: InvoiceSettings) {
        overview = overview?.copy(settings = settings)
    }

    fun locate(invoiceId: String): Invoice? {
        if (!PackagesText.isHexId(invoiceId)) return null
        overview?.invoice(invoiceId)?.let { return it }
        for (org in organizations.filter { it.installed && it.name != organization }) {
            val found = runCatching { client.invoice(org.name, invoiceId) }.getOrNull() ?: continue
            organization = org.name
            contacts = null
            load()
            return found
        }
        return null
    }

    private fun markInstalled(installed: Boolean) {
        organizations = organizations.map { if (it.name == organization) it.copy(installed = installed) else it }
    }
}
