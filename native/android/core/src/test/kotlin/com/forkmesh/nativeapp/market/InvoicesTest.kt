package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteParser
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.util.Locale

class InvoicesTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val contactId = "c".repeat(32)
    private val day = InvoiceDates.DAY_MS
    private val jan10 = 1_768_003_200_000L

    private fun invoiceJson(id: String = hexA, status: String = "sent", display: String = status, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        linkedMapOf<String, Any?>(
            "id" to id, "organization" to "acme", "number" to "INV-0001", "status" to status, "displayStatus" to display,
            "contactId" to contactId, "client" to mapOf("name" to "Ada", "email" to "ada@example.com", "company" to "Engines"),
            "currency" to "USD", "items" to listOf(mapOf("description" to "Design", "quantity" to 2.5, "unitAmount" to 10_000)),
            "taxRate" to 1000, "subtotal" to 25_000, "tax" to 2_500, "total" to 27_500, "paidAmount" to 0, "balance" to 27_500,
            "issuedAt" to jan10, "dueAt" to jan10 + 30 * day, "sentAt" to jan10 + 1000, "opens" to 3, "views" to 2, "clicks" to 1,
            "publicUrl" to "https://app.forkmesh.com/invoice?t=$hexA.abc",
        ) + extra

    fun testMoneyFormatsMinorUnits() {
        assertEq("$1,234.50", InvoiceMoney.format(123_450, "USD", Locale.US))
        assertEq("$0.05", InvoiceMoney.format(5, "usd", Locale.US))
        assertEq("€12.00", InvoiceMoney.format(1200, "EUR", Locale.US))
        assertEq("ZZ1 12.00", InvoiceMoney.format(1200, "ZZ1", Locale.US))
        assertEq("$0.00", InvoiceMoney.totals(emptyMap(), "USD", Locale.US))
        assertEq("$10.00 · €2.00", InvoiceMoney.totals(linkedMapOf("USD" to 1000L, "EUR" to 200L, "GBP" to 0L), "USD", Locale.US))
        assertEq(1234L, InvoiceMoney.minor("12.34"))
        assertEq(1235L, InvoiceMoney.minor("$12.345"))
        assertEq(120_000L, InvoiceMoney.minor("1,200"))
        assertEq(0L, InvoiceMoney.minor("-5"))
        assertEq(0L, InvoiceMoney.minor("abc"))
        assertEq("12.30", InvoiceMoney.input(1230))
        assertEq(750L, InvoiceMoney.basisPoints("7.5"))
        assertEq(0L, InvoiceMoney.basisPoints("x"))
        assertEq("7.5", InvoiceMoney.percent(750))
        assertEq("10", InvoiceMoney.percent(1000))
        assertEq("0", InvoiceMoney.percent(0))
        assertEq("2", InvoiceMoney.quantity(2.0))
        assertEq("2.5", InvoiceMoney.quantity(2.5))
        assertEq(2_500L, InvoiceMoney.tax(25_000, 1000))
    }

    fun testDatesAreUtcMidnights() {
        assertEq("2026-01-10", InvoiceDates.input(jan10))
        assertEq(jan10, InvoiceDates.parse("2026-01-10"))
        assertEq(null, InvoiceDates.parse("10/01/2026"))
        assertEq(null, InvoiceDates.parse(" "))
        assertEq(jan10, InvoiceDates.today(jan10 + 5 * 3_600_000))
        assertEq("10 Jan 2026", InvoiceDates.label(jan10))
        assertEq("—", InvoiceDates.label(0))
        assertEq("", InvoiceDates.input(0))
    }

    fun testInvoiceParsesAndAllowsTheWebsActions() {
        val sent = Invoice.fromJson(invoiceJson(extra = mapOf("events" to listOf(mapOf("kind" to "opened", "at" to 5, "client" to "Gmail")))))
        assertEq("INV-0001", sent.number)
        assertEq(2.5, sent.items[0].quantity)
        assertEq(25_000L, sent.items[0].amount)
        assertEq("Ada", sent.clientLabel)
        assertEq("Sent", sent.statusLabel)
        expect(sent.editable && sent.canSend && sent.canRecordPayment && sent.canVoid)
        expect(!sent.deletable && !sent.canReopen && !sent.canMarkPaid)
        expect(sent.contactRemoved)
        assertEq("Email opened", sent.events!![0].title)
        assertEq("in Gmail", sent.events!![0].detail("USD", Locale.US))
        val draft = Invoice.fromJson(invoiceJson(status = "draft", extra = mapOf("sentAt" to 0, "publicUrl" to "http://bad")))
        expect(draft.deletable && draft.canMarkPaid && !draft.canRecordPayment && !draft.canVoid)
        assertEq("", draft.publicUrl)
        assertEq(null, draft.events)
        expect(!draft.contactRemoved)
        val paid = Invoice.fromJson(invoiceJson(status = "paid"))
        expect(paid.canReopen && paid.canVoid && !paid.editable && !paid.deletable)
        val void = Invoice.fromJson(invoiceJson(status = "void"))
        expect(void.canReopen && void.deletable && !void.canVoid)
        val linked = Invoice.fromJson(invoiceJson(extra = mapOf("events" to emptyList<Any>(), "contact" to mapOf("id" to contactId, "name" to "Ada"))))
        expect(!linked.contactRemoved)
        assertEq("Ada", linked.contact?.name)
        assertEq("No client", Invoice(id = hexA).clientLabel)
    }

    fun testFiltersMatchTheWebStatusMenu() {
        val list = listOf(
            Invoice.fromJson(invoiceJson(hexA, "sent", "overdue")),
            Invoice.fromJson(invoiceJson(hexB, "sent", "partial", mapOf("number" to "INV-0002", "client" to mapOf("company" to "Initech")))),
            Invoice(id = "c".repeat(32), status = "draft", number = "INV-0003"),
            Invoice(id = "d".repeat(32), status = "paid", number = "INV-0004"),
            Invoice(id = "e".repeat(32), status = "void", number = "INV-0005"),
        )
        val overview = InvoicesOverview("acme", invoices = list)
        assertEq(5, overview.count(InvoiceFilter.ALL))
        assertEq(2, overview.count(InvoiceFilter.OPEN))
        assertEq(1, overview.count(InvoiceFilter.OVERDUE))
        assertEq(1, overview.count(InvoiceFilter.DRAFT))
        assertEq(1, overview.count(InvoiceFilter.PAID))
        assertEq(1, overview.count(InvoiceFilter.VOID))
        assertEq(listOf(hexB), overview.filtered("initech", InvoiceFilter.ALL).map { it.id })
        assertEq(listOf("INV-0004"), overview.filtered("0004", InvoiceFilter.PAID).map { it.number })
        assertEq(InvoiceFilter.OPEN, InvoiceFilter.of("open"))
        assertEq(InvoiceFilter.ALL, InvoiceFilter.of("nope"))
    }

    fun testOverviewSummaryAndSettings() {
        val overview = InvoicesOverview.fromJson(
            mapOf(
                "organization" to "acme", "role" to "member", "crmInstalled" to true,
                "invoices" to listOf(invoiceJson(), invoiceJson(id = "nothex")),
                "summary" to mapOf(
                    "counts" to mapOf("sent" to 1, "overdue" to 0, "paid" to 2), "outstanding" to mapOf("USD" to 27_500, "EUR" to 0),
                    "paidLast30Days" to mapOf("EUR" to 100), "sent" to 4, "opened" to 3, "viewed" to 2, "clicked" to 1, "openRate" to 75,
                    "averageDaysToPay" to 4.25, "averageHoursToOpen" to 0,
                ),
                "activity" to listOf(mapOf("kind" to "paid", "at" to 9, "amount" to 500, "number" to "INV-0001", "clientName" to "Ada", "currency" to "USD", "invoiceId" to hexA)),
                "settings" to mapOf("businessName" to "Acme", "currency" to "EUR", "dueDays" to 14, "taxRate" to 2000, "nextNumber" to 0),
            ),
        )
        assertEq(1, overview.invoices.size)
        expect(overview.crmInstalled && !overview.canManage)
        assertEq(mapOf("USD" to 27_500L), overview.summary.outstanding)
        assertEq(2, overview.summary.count("paid"))
        assertEq(0, overview.summary.count("void"))
        assertEq("75%", overview.summary.openRateText)
        assertEq("3 of 4 opened · 2 viewed · 1 clicked pay", overview.summary.engagementText)
        assertEq("4.3 days", overview.summary.daysToPayText)
        assertEq("From sending to paid", overview.summary.hoursToOpenText)
        assertEq("—", InvoiceSummary().openRateText)
        assertEq("No invoices sent yet", InvoiceSummary().engagementText)
        assertEq("1 unpaid invoice", InvoiceSummary.plural(1, "unpaid invoice"))
        assertEq("2 unpaid invoices", InvoiceSummary.plural(2, "unpaid invoice"))
        assertEq("Payment recorded", overview.activity[0].title)
        assertEq("Ada · $5.00", overview.activity[0].activityLine(Locale.US))
        assertEq(1L, overview.settings.nextNumber)
        assertEq(14, overview.settings.dueDays)
        expect(InvoicesOverview("acme", role = "admin").canManage)
        val settings = InvoiceSettings(currency = "eur", taxRate = 200_000, dueDays = 999, paymentUrl = " https://pay.example ")
        assertEq("EUR", settings.body["currency"])
        assertEq(100_000L, settings.body["taxRate"])
        assertEq(365, settings.body["dueDays"])
        assertEq("https://pay.example", settings.body["paymentUrl"])
        assertEq(null, settings.problem)
        assertEq("The pay link must start with https://.", settings.copy(paymentUrl = "http://x").problem)
        assertEq("Use a three-letter currency code such as USD.", settings.copy(currency = "EURO").problem)
    }

    fun testDraftComputesTotalsAndBody() {
        val settings = InvoiceSettings(currency = "EUR", dueDays = 14, taxRate = 750, paymentUrl = "https://pay.example")
        val contact = InvoiceContactLink(contactId, "Ada", "ada@example.com", "")
        val blank = InvoiceDraft.blank(settings, jan10 + 3_600_000, contact)
        assertEq("2026-01-10", blank.issued)
        assertEq("2026-01-24", blank.due)
        assertEq("EUR", blank.currency)
        assertEq("7.5", blank.tax)
        assertEq(contactId, blank.contactId)
        assertEq("Ada", blank.name)
        val filled = blank.copy(company = "Old").line(0) { it.copy(description = "Work", quantity = "2", unit = "10.50") }.addLine()
            .line(1) { it.copy(quantity = "0.5", unit = "3") }
        assertEq(2100L + 150L, filled.subtotal)
        assertEq(169L, filled.taxAmount)
        assertEq(2250L + 169L, filled.total)
        assertEq(null, filled.problem)
        val body = filled.addLine().body(crmInstalled = true)
        @Suppress("UNCHECKED_CAST")
        val items = body["items"] as List<Map<String, Any?>>
        assertEq(2, items.size)
        assertEq(1050L, items[0]["unitAmount"])
        assertEq(2.0, items[0]["quantity"])
        assertEq(750L, body["taxRate"])
        assertEq(jan10, body["issuedAt"])
        assertEq(contactId, body["contactId"])
        expect("number" !in body)
        assertEq("Old", filled.withContact(contact).company)
        assertEq("", filled.withContact(null).contactId)
        expect("contactId" !in filled.copy(number = " INV-9 ").body(crmInstalled = false))
        assertEq("INV-9", filled.copy(number = " INV-9 ").body(false)["number"])
        assertEq("Write the due date as YYYY-MM-DD.", filled.copy(due = "soon").problem)
        assertEq("The pay link must start with https://.", filled.copy(paymentUrl = "pay.example").problem)
        assertEq("Quantities are numbers, such as 1 or 2.5.", filled.line(0) { it.copy(quantity = "two") }.problem)
        assertEq(listOf(InvoiceLineDraft()), filled.removeLine(0).removeLine(0).lines)
        val invoice = Invoice.fromJson(invoiceJson())
        val edit = InvoiceDraft.of(invoice)
        assertEq(hexA, edit.id)
        assertEq("2.5", edit.lines[0].quantity)
        assertEq("100.00", edit.lines[0].unit)
        assertEq("10", edit.tax)
        assertEq(invoice.total, edit.total)
        val copy = InvoiceDraft.duplicate(invoice, settings, jan10 + 2 * day)
        expect(copy.isNew)
        assertEq("", copy.number)
        assertEq("2026-01-12", copy.issued)
        assertEq("2026-01-26", copy.due)
        assertEq(invoice.items.size, copy.lines.size)
    }

    fun testSendAndPaymentDrafts() {
        val invoice = Invoice.fromJson(invoiceJson(extra = mapOf("balance" to 7_500)))
        assertEq("ada@example.com", InvoiceSendDraft.of(invoice).to)
        assertEq("Reminder: invoice INV-0001 from Acme", InvoiceSendDraft.subjectHint(invoice, "Acme"))
        assertEq("Invoice INV-0001 from Acme", InvoiceSendDraft.subjectHint(invoice.copy(sentAt = 0), "Acme"))
        val payment = InvoicePaymentDraft.of(invoice, jan10 + 1000)
        assertEq("75.00", payment.amount)
        assertEq("2026-01-10", payment.date)
        assertEq(null, payment.problem)
        assertEq(7_500L, payment.body["amount"])
        assertEq(jan10, payment.body["paidAt"])
        assertEq("Enter the amount received.", payment.copy(amount = "0").problem)
        assertEq("275.00", InvoicePaymentDraft.of(invoice.copy(balance = 0), 0).amount)
    }

    fun testClientCallsEveryEndpoint() {
        val transport = FakeTransport()
        val client = PackagesClient(transport)
        transport.answer("GET", "/api/invoices", mapOf("organizations" to listOf(mapOf("name" to "acme", "role" to "owner", "installed" to true), mapOf("name" to "Bad"))))
        assertEq(listOf("acme"), client.invoiceOrganizations().map { it.name })
        transport.answer("GET", "/api/invoices/acme", mapOf("organization" to "acme", "invoices" to listOf(invoiceJson())))
        assertEq(1, client.invoices("acme").invoices.size)
        transport.answer("GET", "/api/invoices/acme/items/$hexA", mapOf("invoice" to invoiceJson(extra = mapOf("events" to emptyList<Any>()))))
        assertEq(emptyList<InvoiceEvent>(), client.invoice("acme", hexA).events)
        transport.answer("POST", "/api/invoices/acme/items", mapOf("invoice" to invoiceJson(id = hexB, status = "draft")), 201)
        assertEq(hexB, client.saveInvoice("acme", InvoiceDraft(name = "Ada"), crmInstalled = false).id)
        assertEq("Ada", (transport.last("POST", "/api/invoices/acme/items")!!.body!!["client"] as Map<*, *>)["name"])
        transport.answer("PATCH", "/api/invoices/acme/items/$hexA", mapOf("invoice" to invoiceJson()))
        client.saveInvoice("acme", InvoiceDraft(id = hexA), crmInstalled = true)
        assertEq("", transport.last("PATCH", "/api/invoices/acme/items/$hexA")!!.body!!["contactId"])
        client.deleteInvoice("acme", hexA)
        expect(transport.last("DELETE", "/api/invoices/acme/items/$hexA") != null)
        transport.answer("POST", "/api/invoices/acme/items/$hexA/send", mapOf("invoice" to invoiceJson(extra = mapOf("sentTo" to "b@x.io"))), 201)
        assertEq("b@x.io", client.sendInvoice("acme", hexA, InvoiceSendDraft(" b@x.io ", "", "Hi")).sentTo)
        assertEq("b@x.io", transport.last("POST", "/api/invoices/acme/items/$hexA/send")!!.body!!["to"])
        transport.answer("POST", "/api/invoices/acme/items/$hexA/paid", mapOf("invoice" to invoiceJson(status = "paid")))
        assertEq("paid", client.recordInvoicePayment("acme", hexA, InvoicePaymentDraft("10", "", "Bank", "")).status)
        assertEq(1000L, transport.last("POST", "/api/invoices/acme/items/$hexA/paid")!!.body!!["amount"])
        transport.answer("POST", "/api/invoices/acme/items/$hexA/void", mapOf("invoice" to invoiceJson(status = "void")))
        assertEq("void", client.invoiceAction("acme", hexA, "void").status)
        client.invoiceAction("acme", hexA, "reopen")
        expect(transport.last("POST", "/api/invoices/acme/items/$hexA/reopen") != null)
        assertFails<PackagesException> { client.invoiceAction("acme", hexA, "send") }
        assertFails<PackagesException> { client.invoice("acme", "nope") }
        assertFails<PackagesException> { client.invoices("Bad Org") }
        transport.answer("GET", "/api/invoices/acme/settings", mapOf("settings" to mapOf("businessName" to "Acme")))
        assertEq("Acme", client.invoiceSettings("acme").businessName)
        transport.answer("PATCH", "/api/invoices/acme/settings", mapOf("settings" to mapOf("businessName" to "Acme Ltd")))
        assertEq("Acme Ltd", client.saveInvoiceSettings("acme", InvoiceSettings(businessName = " Acme Ltd ")).businessName)
        assertEq("Acme Ltd", transport.last("PATCH", "/api/invoices/acme/settings")!!.body!!["businessName"])
        transport.answer("GET", "/api/invoices/acme/contacts", mapOf("contacts" to listOf(mapOf("id" to contactId, "name" to "Ada", "company" to "Engines"), mapOf("id" to "x"))))
        val contacts = client.invoiceContacts("acme")
        assertEq(1, contacts.size)
        assertEq("Ada · Engines", contacts[0].label)
        expect(contacts[0].matches("engi"))
        transport.answer(
            "GET", "/api/invoices/acme/contacts/$contactId",
            mapOf("contactId" to contactId, "invoices" to listOf(invoiceJson()), "totals" to mapOf("billed" to mapOf("USD" to 27_500), "paid" to emptyMap<String, Any>(), "outstanding" to mapOf("USD" to 27_500))),
        )
        val history = client.contactInvoices("acme", contactId)
        assertEq(1, history.invoices.size)
        assertEq(mapOf("USD" to 27_500L), history.billed)
        expect(history.paid.isEmpty())
    }

    fun testErrorsReadPlainly() {
        assertEq("Another invoice already uses that number.", InvoiceErrors.describe(PackagesException("invoice_number_taken", 409)))
        assertEq("Add a From email in Invoicing settings, or a sender in the CRM's email settings.", InvoiceErrors.describe(PackagesException("invalid_sender", 400)))
        assertEq("Sign in again to continue.", InvoiceErrors.describe(PackagesException("invalid_session", 401)))
        assertEq("Something went wrong.", InvoiceErrors.describe(IllegalStateException()))
        assertEq(409, InvoiceErrors.status(PackagesException("crm_not_installed", 409)))
        assertEq("", InvoiceErrors.code(IllegalStateException()))
    }

    fun testModelPicksAnOrganizationAndHandlesInstall() {
        val transport = FakeTransport()
        val model = InvoicesModel(PackagesClient(transport))
        transport.answer(
            "GET", "/api/invoices",
            mapOf("organizations" to listOf(mapOf("name" to "beta", "role" to "member"), mapOf("name" to "acme", "role" to "owner", "installed" to true))),
        )
        transport.answer("GET", "/api/invoices/acme", mapOf("organization" to "acme", "role" to "owner", "crmInstalled" to true, "invoices" to listOf(invoiceJson())))
        model.refresh()
        assertEq("acme", model.organization)
        expect(model.overview != null && model.crmInstalled && model.canManage && !model.needsInstall)
        transport.answer("GET", "/api/invoices/acme/contacts", mapOf("contacts" to listOf(mapOf("id" to contactId, "name" to "Ada"))))
        assertEq(1, model.loadContacts().size)
        transport.answer("GET", "/api/invoices/acme/contacts", mapOf("contacts" to emptyList<Any>()))
        assertEq(1, model.loadContacts().size)
        model.choose("beta")
        expect(model.needsInstall && !model.canInstall && model.overview == null)
        assertEq(null, transport.last("GET", "/api/invoices/beta"))
        transport.answer("POST", "/api/plugins/beta/invoices/install", mapOf("organization" to "beta", "plugins" to emptyList<Any>()))
        transport.answer("GET", "/api/invoices/beta", mapOf("organization" to "beta", "invoices" to emptyList<Any>()))
        expect(model.install())
        expect(!model.needsInstall && model.overview != null)
        transport.answer("GET", "/api/invoices/beta", mapOf("error" to "plugin_not_installed", "canInstall" to false), 409)
        model.load()
        expect(model.needsInstall && model.overview == null)
        assertEq("", model.errorText)
        model.refresh("acme")
        assertEq("acme", model.organization)
        transport.answer("GET", "/api/invoices/acme", mapOf("error" to "rate_limited"), 429)
        model.load()
        assertEq("Too many requests for now. Try again in a minute.", model.errorText)
        assertEq(null, model.locate("nope"))
    }

    fun testModelLocatesAnInvoiceFromANotification() {
        val transport = FakeTransport()
        val model = InvoicesModel(PackagesClient(transport))
        transport.answer(
            "GET", "/api/invoices",
            mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true), mapOf("name" to "beta", "installed" to true))),
        )
        transport.answer("GET", "/api/invoices/acme", mapOf("organization" to "acme", "invoices" to emptyList<Any>()))
        transport.answer("GET", "/api/invoices/beta", mapOf("organization" to "beta", "invoices" to listOf(invoiceJson(id = hexB))))
        transport.answer("GET", "/api/invoices/beta/items/$hexB", mapOf("invoice" to invoiceJson(id = hexB)))
        transport.answer("GET", "/api/invoices/acme/items/$hexB", mapOf("error" to "invoice_not_found"), 404)
        model.refresh()
        assertEq(hexB, model.locate(hexB)?.id)
        assertEq("beta", model.organization)
        assertEq("beta", model.overview?.organization)
    }

    fun testRoutesAndPackageRegistration() {
        assertEq(Route.Kind.Invoices, PackageGate.ROUTE_KINDS.entries.first { it.value == "invoices" }.key)
        assertEq("invoices", PackageGate.HUB_ITEMS[Route.Kind.Invoices.raw])
        expect("invoices" in PackageGate.ORGANIZATION_PACKAGES)
        assertEq("invoices", MarketPackage.NATIVE_DESTINATIONS["invoices"])
        assertEq("invoices", PackageRoutes.destination(MarketPackage(id = "invoices", name = "Invoicing")))
        expect("invoices" in PackageRoutes.BUSINESS_PACKAGE_IDS)
        assertEq(listOf("market", "crm", "invoices", "ads", "ledger"), PackageRoutes.hubRows(setOf("invoices")).map { it.id })
        expect(HubCatalog.business.any { it.id == "invoices" && it.needsRegistration })
        val surfaces = PackageSurfaces.of { it != "invoices" }
        expect("invoices" in surfaces.hubItems)
        expect(!surfaces.showsRoute(Route.Invoices(null, null)))
        assertEq(
            listOf(Route.Invoices("acme", hexA)),
            RouteParser.parse("https://app.forkmesh.com/dashboard/invoices?org=acme&invoice=$hexA")!!.stack,
        )
        assertEq(listOf(Route.Invoices(null, hexA)), RouteParser.parse("https://app.forkmesh.com/dashboard/invoices?invoice=$hexA")!!.stack)
        assertEq(listOf(Route.Invoices(null, null)), RouteParser.parse("https://app.forkmesh.com/dashboard/invoices")!!.stack)
    }
}
