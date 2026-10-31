import FMApi
import FMBase
import XCTest

@testable import FMPackages

final class InvoicesTests: XCTestCase {
    let us = Locale(identifier: "en_US")
    let jan1: Int64 = 1_767_225_600_000

    func overview(installedCrm: Bool = true) -> JSONObject {
        [
            "ok": true, "organization": "acme", "role": "member", "canManagePlugin": false,
            "crmInstalled": .bool(installedCrm),
            "invoices": [
                [
                    "id": .string(hexA), "organization": "acme", "number": "INV-0001", "status": "sent",
                    "displayStatus": "overdue", "contactId": .string(hexB),
                    "client": ["name": "Ada", "email": "ada@example.com", "company": "Engines", "address": "1 Way"],
                    "currency": "usd", "items": [["description": "Work", "quantity": 1.5, "unitAmount": 10000]],
                    "taxRate": 1000, "subtotal": 15000, "tax": 1500, "total": 16500, "paidAmount": 0, "balance": 16500,
                    "payments": [], "issuedAt": .int(jan1), "dueAt": .int(jan1 + 30 * 86_400_000), "sentAt": 5,
                    "lastSentAt": 6, "sentTo": "ada@example.com", "opens": 3, "views": 2, "clicks": 1,
                    "firstOpenedAt": 7, "firstViewedAt": 8, "publicUrl": "https://app.forkmesh.com/invoice?t=x",
                ],
                ["id": .string(hexB), "number": "INV-0002", "status": "draft", "displayStatus": "draft", "currency": "EUR"],
                ["id": "bad", "number": "dropped"],
                ["id": .string(String(repeating: "c", count: 32)), "number": "INV-0003", "status": "paid", "currency": "USD"],
                ["id": .string(String(repeating: "d", count: 32)), "number": "INV-0004", "status": "void"],
                [
                    "id": .string(String(repeating: "e", count: 32)), "number": "INV-0005", "status": "sent",
                    "displayStatus": "partial", "client": ["company": "Only Co"],
                ],
            ],
            "summary": [
                "counts": ["draft": 1, "sent": 2, "overdue": 1, "paid": 1, "void": 1],
                "outstanding": ["USD": 16500, "EUR": 2000, "bad": 5], "overdueAmount": ["USD": 16500],
                "paidLast30Days": [:], "sent": 4, "opened": 3, "viewed": 2, "clicked": 1, "openRate": 75,
                "viewRate": 50, "averageDaysToPay": 4.5, "averageHoursToOpen": 2,
            ],
            "activity": [
                ["kind": "opened", "at": 9, "invoiceId": .string(hexA), "number": "INV-0001", "clientName": "Ada", "client": "Gmail", "currency": "USD"],
                ["kind": "paid", "at": 8, "invoiceId": .string(hexA), "number": "INV-0001", "amount": 500, "currency": "USD"],
            ],
            "settings": [
                "businessName": "Acme Ltd", "currency": "EUR", "dueDays": 14, "taxRate": 750, "prefix": "A-",
                "nextNumber": 7, "paymentUrl": "https://pay.example/acme",
            ],
        ]
    }

    func testMoneyFormatsMinorUnits() {
        XCTAssertEqual(InvoiceMoney.format(123_456, "USD", locale: us), "$1,234.56")
        XCTAssertEqual(InvoiceMoney.format(5, "eur", locale: us), "€0.05")
        XCTAssertEqual(InvoiceMoney.format(1200, "", locale: us), "12.00")
        XCTAssertEqual(InvoiceMoney.format(1200, "points", locale: us), "POINTS 12.00")
        XCTAssertEqual(InvoiceMoney.map(["USD": 1000, "EUR": 250, "GBP": 0], fallback: "USD", locale: us), "€2.50 · $10.00")
        XCTAssertEqual(InvoiceMoney.map([:], fallback: "EUR", locale: us), "€0.00")
        XCTAssertEqual(InvoiceMoney.decimal(1205), "12.05")
        XCTAssertEqual(InvoiceMoney.decimal(-7), "-0.07")
        XCTAssertEqual(InvoiceMoney.minor(from: "12.5"), 1250)
        XCTAssertEqual(InvoiceMoney.minor(from: "$1,234.56"), 123_456)
        XCTAssertEqual(InvoiceMoney.minor(from: "1.234,56"), 123_456)
        XCTAssertEqual(InvoiceMoney.minor(from: "12,5"), 1250)
        XCTAssertEqual(InvoiceMoney.minor(from: "1,000"), 100_000)
        XCTAssertEqual(InvoiceMoney.minor(from: "-3"), 300)
        XCTAssertEqual(InvoiceMoney.minor(from: "abc"), 0)
        XCTAssertEqual(InvoiceMoney.quantity(from: "1,5"), 1.5)
        XCTAssertEqual(InvoiceMoney.quantity(from: "-2"), 0)
        XCTAssertEqual(InvoicePercent.text(1000), "10")
        XCTAssertEqual(InvoicePercent.text(750), "7.5")
        XCTAssertEqual(InvoicePercent.text(1234), "12.34")
        XCTAssertEqual(InvoicePercent.basisPoints("7,5"), 750)
        XCTAssertEqual(InvoicePercent.basisPoints("x"), 0)
        XCTAssertEqual(InvoiceText.number(4.0), "4")
        XCTAssertEqual(InvoiceText.number(4.25), "4.3")
    }

    func testDatesAreUtcMidnights() {
        XCTAssertEqual(InvoiceDates.midnight(year: 2026, month: 1, day: 1), jan1)
        var tokyo = Calendar(identifier: .gregorian)
        tokyo.timeZone = TimeZone(identifier: "Asia/Tokyo")!
        let lateJan1InTokyo = Date(timeIntervalSince1970: TimeInterval(jan1 / 1000) + 23 * 3600 - 9 * 3600)
        XCTAssertEqual(InvoiceDates.midnight(of: lateJan1InTokyo, in: tokyo), jan1)
        var la = Calendar(identifier: .gregorian)
        la.timeZone = TimeZone(identifier: "America/Los_Angeles")!
        let local = InvoiceDates.localDate(jan1, in: la)
        XCTAssertEqual(la.dateComponents([.year, .month, .day], from: local), DateComponents(year: 2026, month: 1, day: 1))
        XCTAssertEqual(InvoiceDates.midnight(of: local, in: la), jan1)
        XCTAssertEqual(InvoiceDates.label(jan1, locale: us), "Jan 1, 2026")
        XCTAssertEqual(InvoiceDates.label(0, locale: us), "—")
    }

    func testOverviewParsesInvoicesSummaryActivityAndSettings() {
        let board = InvoiceBoard(json: overview())
        XCTAssertEqual(board.invoices.map(\.number), ["INV-0001", "INV-0002", "INV-0003", "INV-0004", "INV-0005"])
        XCTAssertTrue(board.crmInstalled)
        XCTAssertFalse(board.canManage)
        let first = board.invoices[0]
        XCTAssertEqual(first.currency, "USD")
        XCTAssertEqual(first.state, "overdue")
        XCTAssertEqual(first.stateTitle, "Overdue")
        XCTAssertEqual(first.items, [InvoiceLine(description: "Work", quantity: 1.5, unitAmount: 10000)])
        XCTAssertEqual(first.items[0].amount, 15000)
        XCTAssertEqual(first.items[0].quantityText, "1.5")
        XCTAssertEqual(first.money(first.total, locale: us), "$165.00")
        XCTAssertEqual(first.client.label, "Ada")
        XCTAssertEqual(first.opens, 3)
        XCTAssertEqual(first.shareURL?.absoluteString, "https://app.forkmesh.com/invoice?t=x")
        XCTAssertNil(first.events)
        XCTAssertEqual(board.invoices[4].client.label, "Only Co")
        XCTAssertEqual(board.invoices[1].client.label, "No client")

        let summary = board.summary
        XCTAssertEqual(summary.outstanding, ["USD": 16500, "EUR": 2000])
        XCTAssertEqual(summary.outstandingDetail, "2 unpaid invoices")
        XCTAssertEqual(summary.overdueDetail, "1 overdue invoice")
        XCTAssertEqual(summary.paidDetail, "1 paid invoice in all")
        XCTAssertEqual(summary.openRateText, "75%")
        XCTAssertEqual(summary.engagementText, "3 of 4 opened · 2 viewed · 1 clicked pay")
        XCTAssertEqual(summary.daysToPayText, "4.5 days")
        XCTAssertEqual(summary.daysToPayDetail, "Opened 2 h after sending, on average")
        let empty = InvoiceSummary(json: [:])
        XCTAssertEqual(empty.openRateText, "—")
        XCTAssertEqual(empty.engagementText, "No invoices sent yet")
        XCTAssertEqual(empty.daysToPayText, "—")
        XCTAssertEqual(empty.daysToPayDetail, "From sending to paid")

        XCTAssertEqual(board.activity.map(\.title), ["Email opened", "Payment recorded"])
        XCTAssertEqual(board.activity[0].activityDetail(locale: us), "Ada · Gmail")
        XCTAssertEqual(board.activity[1].activityDetail(locale: us), "$5.00")
        XCTAssertEqual(board.activity[1].symbol, "checkmark.seal")

        XCTAssertEqual(board.settings.businessName, "Acme Ltd")
        XCTAssertEqual(board.settings.dueDays, 14)
        XCTAssertEqual(InvoiceSettings(json: [:]).dueDays, 30)
        XCTAssertEqual(InvoiceSettings(json: [:]).prefix, "INV-")
    }

    func testFiltersAndSearchFollowTheWebPage() {
        let board = InvoiceBoard(json: overview())
        XCTAssertEqual(board.filtered(filter: .unpaid).map(\.number), ["INV-0001", "INV-0005"])
        XCTAssertEqual(board.filtered(filter: .overdue).map(\.number), ["INV-0001"])
        XCTAssertEqual(board.filtered(filter: .drafts).map(\.number), ["INV-0002"])
        XCTAssertEqual(board.filtered(filter: .paid).map(\.number), ["INV-0003"])
        XCTAssertEqual(board.filtered(filter: .void).map(\.number), ["INV-0004"])
        XCTAssertEqual(board.filtered(query: "engines").map(\.number), ["INV-0001"])
        XCTAssertEqual(board.filtered(query: "0002").map(\.number), ["INV-0002"])
        XCTAssertEqual(board.filtered(query: "ada@", filter: .drafts), [])
        XCTAssertEqual(InvoiceFilter.allCases.map(\.title), ["All invoices", "Unpaid", "Overdue", "Drafts", "Paid", "Void"])
    }

    func testActionsMatchTheWebPage() {
        let board = InvoiceBoard(json: overview())
        let sent = board.invoices[0]
        XCTAssertTrue(sent.isEditable)
        XCTAssertFalse(sent.isDeletable)
        XCTAssertTrue(sent.canVoid)
        XCTAssertTrue(sent.canRecordPayment)
        XCTAssertFalse(sent.canMarkPaid)
        XCTAssertEqual(sent.sendTitle, "Send reminder")
        let draft = board.invoices[1]
        XCTAssertTrue(draft.isDeletable)
        XCTAssertFalse(draft.canVoid)
        XCTAssertTrue(draft.canMarkPaid)
        XCTAssertFalse(draft.canRecordPayment)
        XCTAssertEqual(draft.sendTitle, "Email invoice")
        let paid = board.invoices[2]
        XCTAssertFalse(paid.isEditable)
        XCTAssertTrue(paid.canReopen)
        XCTAssertTrue(paid.canVoid)
        let void = board.invoices[3]
        XCTAssertTrue(void.isDeletable)
        XCTAssertTrue(void.canReopen)
        XCTAssertFalse(void.canSend)
    }

    func testDraftTotalsAndBody() {
        let settings = InvoiceBoard(json: overview()).settings
        var draft = InvoiceDraft(settings: settings, today: jan1, contact: InvoiceContact(id: hexB, name: "Ada", email: "ada@example.com"))
        XCTAssertTrue(draft.isNew)
        XCTAssertEqual(draft.contactId, hexB)
        XCTAssertEqual(draft.client.name, "Ada")
        XCTAssertEqual(draft.client.email, "ada@example.com")
        XCTAssertEqual(draft.dueAt, jan1 + 14 * 86_400_000)
        XCTAssertEqual(draft.currency, "EUR")
        XCTAssertEqual(draft.taxPercent, "7.5")
        XCTAssertEqual(draft.paymentUrl, "https://pay.example/acme")
        XCTAssertEqual(draft.lines.count, 1)
        draft.lines[0].description = "Design"
        draft.lines[0].quantity = "2"
        draft.lines[0].unit = "100.00"
        draft.addLine()
        draft.lines[1].quantity = "0.5"
        draft.lines[1].unit = "19.99"
        XCTAssertEqual(draft.subtotal, 20000 + 1000)
        XCTAssertEqual(draft.tax, 1575)
        XCTAssertEqual(draft.total, 22575)
        draft.removeLine(draft.lines[1].id)
        XCTAssertEqual(draft.total, 21500)
        draft.removeLine(draft.lines[0].id)
        XCTAssertEqual(draft.lines.count, 1)
        draft.lines[0].unit = "10"
        draft.number = "  A-77 "
        draft.choose(InvoiceContact(id: hexA, name: "", email: "", company: "Engines"))
        XCTAssertEqual(draft.client.name, "Ada")
        XCTAssertEqual(draft.client.company, "Engines")
        let body = draft.body(crmInstalled: true)
        XCTAssertEqual(body.str("number"), "A-77")
        XCTAssertEqual(body.str("contactId"), hexA)
        XCTAssertEqual(body.int("taxRate"), 750)
        XCTAssertEqual(body.int64("issuedAt"), jan1)
        XCTAssertEqual(body.str("currency"), "EUR")
        XCTAssertEqual(body.obj("client").str("company"), "Engines")
        XCTAssertEqual(body.maps("items").first?.int64("unitAmount"), 1000)
        XCTAssertEqual(body.maps("items").first?.double("quantity"), 1)
        XCTAssertNil(draft.body(crmInstalled: false)["contactId"])
        draft.number = ""
        XCTAssertNil(draft.body(crmInstalled: false)["number"])
        draft.paymentUrl = "http://insecure"
        XCTAssertFalse(draft.canSave)
        draft.paymentUrl = ""
        draft.currency = "EURO"
        XCTAssertFalse(draft.canSave)
        draft.currency = "gbp"
        XCTAssertTrue(draft.canSave)
        draft.dueAt = jan1 - 1
        XCTAssertFalse(draft.canSave)
        draft.choose(nil)
        XCTAssertEqual(draft.contactId, "")
    }

    func testEditAndDuplicateDrafts() {
        let board = InvoiceBoard(json: overview())
        let invoice = board.invoices[0]
        let edit = InvoiceDraft(editing: invoice)
        XCTAssertEqual(edit.id, hexA)
        XCTAssertEqual(edit.number, "INV-0001")
        XCTAssertEqual(edit.taxPercent, "10")
        XCTAssertEqual(edit.lines.map(\.unit), ["100.00"])
        XCTAssertEqual(edit.lines.map(\.quantity), ["1.5"])
        XCTAssertEqual(edit.total, invoice.total)
        let copy = InvoiceDraft(duplicating: invoice, settings: board.settings, today: jan1 + 86_400_000)
        XCTAssertTrue(copy.isNew)
        XCTAssertEqual(copy.number, "")
        XCTAssertEqual(copy.contactId, hexB)
        XCTAssertEqual(copy.issuedAt, jan1 + 86_400_000)
        XCTAssertEqual(copy.dueAt, jan1 + 15 * 86_400_000)
        XCTAssertEqual(copy.client, invoice.client)
    }

    func testSendAndPaymentDrafts() {
        let board = InvoiceBoard(json: overview())
        let send = InvoiceSendDraft(invoice: board.invoices[0])
        XCTAssertEqual(send.to, "ada@example.com")
        XCTAssertEqual(send.title, "Remind about INV-0001")
        XCTAssertEqual(send.subjectPlaceholder(business: "Acme Ltd"), "Reminder: invoice INV-0001 from Acme Ltd")
        XCTAssertTrue(send.canSend)
        let first = InvoiceSendDraft(invoice: board.invoices[1])
        XCTAssertEqual(first.title, "Email INV-0002")
        XCTAssertFalse(first.canSend)
        var payment = InvoicePaymentDraft(invoice: board.invoices[0], today: jan1)
        XCTAssertEqual(payment.amount, "165.00")
        payment.amount = "50"
        payment.method = "Bank transfer"
        XCTAssertEqual(payment.body.int64("amount"), 5000)
        XCTAssertEqual(payment.body.int64("paidAt"), jan1)
        XCTAssertEqual(payment.body.str("method"), "Bank transfer")
        payment.amount = "0"
        XCTAssertFalse(payment.canRecord)
    }

    func testDetailParsesEventsAndContact() {
        let invoice = Invoice(json: [
            "id": .string(hexA), "number": "INV-1", "status": "sent", "currency": "USD", "contactId": .string(hexB),
            "contact": ["id": .string(hexB), "name": "Ada", "email": "ada@example.com", "company": "Engines"],
            "events": [
                ["kind": "reminder", "at": 3, "actor": "jett", "to": "ada@example.com"],
                ["kind": "paid", "at": 2, "amount": 1250, "method": "Card"],
                ["kind": "failed", "at": 1, "error": "mailbox full"],
            ],
            "payments": [["amount": 1250, "at": 2, "method": "Card", "note": "", "by": "jett"]],
            "publicUrl": "javascript:alert(1)",
        ])
        XCTAssertEqual(invoice.contact?.label, "Ada · Engines")
        XCTAssertEqual(invoice.events?.map(\.title), ["Reminder emailed", "Payment recorded", "Email failed"])
        XCTAssertEqual(invoice.events?[0].detail(currency: "USD", locale: us), "to ada@example.com · by jett")
        XCTAssertEqual(invoice.events?[1].detail(currency: "USD", locale: us), "$12.50 · Card")
        XCTAssertEqual(invoice.events?[2].detail(currency: "USD", locale: us), "mailbox full")
        XCTAssertEqual(invoice.payments.first?.by, "jett")
        XCTAssertEqual(invoice.publicUrl, "")
        XCTAssertNil(invoice.shareURL)
        XCTAssertNil(Invoice(json: ["id": .string(hexA), "contact": nil]).contact)
    }

    @MainActor func testClientPathsAndBodies() async throws {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/invoices", ["organizations": [["name": "acme", "role": "owner", "installed": true], ["name": "Bad!"]]])
        let orgs = try await client.invoiceOrganizations()
        XCTAssertEqual(orgs.map(\.name), ["acme"])
        XCTAssertTrue(orgs[0].canManage)

        transport.on("GET", "/api/invoices/acme", overview())
        let board = try await client.invoices("acme")
        XCTAssertEqual(board.invoices.count, 5)

        let invoiceJSON: JSONObject = ["invoice": ["id": .string(hexA), "number": "INV-0001", "status": "sent"]]
        transport.on("GET", "/api/invoices/acme/items/" + hexA, invoiceJSON)
        let fetched = try await client.invoice("acme", id: hexA)
        XCTAssertEqual(fetched.number, "INV-0001")

        let draft = InvoiceDraft(settings: board.settings, today: jan1)
        transport.on("POST", "/api/invoices/acme/items", status: 201, invoiceJSON)
        _ = try await client.saveInvoice("acme", draft, crmInstalled: true)
        XCTAssertEqual(transport.last("POST", "/api/invoices/acme/items")?.body?.str("contactId"), "")
        transport.on("PATCH", "/api/invoices/acme/items/" + hexA, invoiceJSON)
        _ = try await client.saveInvoice("acme", InvoiceDraft(editing: board.invoices[0]), crmInstalled: false)
        XCTAssertEqual(transport.last("PATCH", "/api/invoices/acme/items/" + hexA)?.body?.str("number"), "INV-0001")

        var send = InvoiceSendDraft(invoice: board.invoices[0])
        send.subject = "Hello"
        transport.on("POST", "/api/invoices/acme/items/\(hexA)/send", status: 201, invoiceJSON)
        _ = try await client.sendInvoice("acme", id: hexA, send)
        XCTAssertEqual(transport.last("POST", "/api/invoices/acme/items/\(hexA)/send")?.body?.str("subject"), "Hello")

        transport.on("POST", "/api/invoices/acme/items/\(hexA)/paid", invoiceJSON)
        _ = try await client.recordInvoicePayment("acme", id: hexA, InvoicePaymentDraft(invoice: board.invoices[0], today: jan1))
        XCTAssertEqual(transport.last("POST", "/api/invoices/acme/items/\(hexA)/paid")?.body?.int64("amount"), 16500)

        transport.on("POST", "/api/invoices/acme/items/\(hexA)/void", invoiceJSON)
        transport.on("POST", "/api/invoices/acme/items/\(hexA)/reopen", invoiceJSON)
        transport.on("DELETE", "/api/invoices/acme/items/" + hexA, ["ok": true, "deleted": true])
        _ = try await client.voidInvoice("acme", id: hexA)
        _ = try await client.reopenInvoice("acme", id: hexA)
        try await client.deleteInvoice("acme", id: hexA)

        transport.on("GET", "/api/invoices/acme/settings", ["settings": ["businessName": "Acme"]])
        let read = try await client.invoiceSettings("acme")
        XCTAssertEqual(read.businessName, "Acme")
        var settings = board.settings
        settings.taxRate = 2000
        transport.on("PATCH", "/api/invoices/acme/settings", ["settings": ["taxRate": 2000]])
        let written = try await client.saveInvoiceSettings("acme", settings)
        XCTAssertEqual(written.taxRate, 2000)
        XCTAssertEqual(transport.last("PATCH", "/api/invoices/acme/settings")?.body?.int("taxRate"), 2000)
        XCTAssertEqual(transport.last("PATCH", "/api/invoices/acme/settings")?.body?.int("dueDays"), 14)

        transport.on("GET", "/api/invoices/acme/contacts", ["contacts": [["id": .string(hexB), "name": "Ada"], ["id": "x"]]])
        let contacts = try await client.invoiceContacts("acme")
        XCTAssertEqual(contacts.map(\.id), [hexB])

        transport.on("GET", "/api/invoices/acme/contacts/" + hexB, [
            "contactId": .string(hexB), "invoices": [["id": .string(hexA), "number": "INV-0001", "status": "sent"]],
            "totals": ["billed": ["USD": 16500], "paid": [:], "outstanding": ["USD": 16500]],
        ])
        let mine = try await client.contactInvoices(CrmContactRef(organization: "acme", contactId: hexB))
        XCTAssertEqual(mine.invoices.map(\.number), ["INV-0001"])
        XCTAssertEqual(mine.billed, ["USD": 16500])
        XCTAssertEqual(mine.paid, [:])

        transport.on("POST", "/api/plugins/acme/invoices/install", ["organization": "acme", "installed": ["invoices"]])
        _ = try await client.installInvoices("acme")
        XCTAssertTrue(transport.paths.contains("POST /api/plugins/acme/invoices/install"))

        do {
            _ = try await client.invoice("acme", id: "../x")
            XCTFail("expected a refusal")
        } catch {}
        do {
            _ = try await client.invoices("../admin")
            XCTFail("expected a refusal")
        } catch {}
        XCTAssertFalse(transport.paths.contains { $0.contains("..") })
    }

    @MainActor func testErrorsReadPlainly() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/invoices/acme/contacts/" + hexB, status: 409, ["error": "plugin_not_installed", "canInstall": true])
        do {
            _ = try await client.contactInvoices(CrmContactRef(organization: "acme", contactId: hexB))
            XCTFail("expected a refusal")
        } catch {
            XCTAssertEqual(ContactInvoicesState.from(error), .notInstalled(canInstall: true))
            XCTAssertEqual(InvoiceText.describe(error), "Invoicing is not installed for this organization.")
        }
        transport.routes = [:]
        do {
            _ = try await client.contactInvoices(CrmContactRef(organization: "acme", contactId: hexB))
            XCTFail("expected a refusal")
        } catch {
            XCTAssertEqual(ContactInvoicesState.from(error), .hidden)
        }
        transport.on("POST", "/api/invoices/acme/items/\(hexA)/send", status: 502, ["ok": false, "error": "mailbox_unavailable"])
        do {
            _ = try await client.sendInvoice("acme", id: hexA, InvoiceSendDraft(invoice: Invoice(json: ["id": .string(hexA)])))
            XCTFail("expected a refusal")
        } catch {
            XCTAssertEqual(InvoiceText.describe(error), "Mailbox unavailable")
            XCTAssertEqual(ContactInvoicesState.from(error), .failed("Mailbox unavailable"))
        }
        XCTAssertEqual(
            InvoiceText.describe(PackagesError("invalid_sender", status: 400)),
            "Add a From email in Invoicing settings, or in the CRM's email settings.")
        XCTAssertEqual(InvoiceText.describe(PackagesError("void_before_delete", status: 409)), "Void a sent invoice before deleting it.")
    }

    @MainActor func testModelPicksAnOrganizationAndOffersInstall() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/invoices", ["organizations": [["name": "acme", "role": "admin", "installed": false]]])
        let model = InvoicesModel(client: client)
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        XCTAssertTrue(model.needsInstall)
        XCTAssertTrue(model.canManage)
        XCTAssertNil(model.board)
        XCTAssertFalse(transport.paths.contains("GET /api/invoices/acme"))

        transport.on("POST", "/api/plugins/acme/invoices/install", ["organization": "acme"])
        transport.on("GET", "/api/invoices/acme", overview(installedCrm: true))
        transport.on("GET", "/api/invoices/acme/contacts", ["contacts": [["id": .string(hexB), "name": "Ada"]]])
        let installed = await model.install()
        XCTAssertTrue(installed)
        XCTAssertFalse(model.needsInstall)
        XCTAssertEqual(model.board?.invoices.count, 5)
        XCTAssertEqual(model.notice, "Invoicing installed.")
        XCTAssertEqual(model.business, "Acme Ltd")
        let contacts = await model.loadContacts()
        XCTAssertEqual(contacts.map(\.name), ["Ada"])
        _ = await model.loadContacts()
        XCTAssertEqual(transport.paths.filter { $0 == "GET /api/invoices/acme/contacts" }.count, 1)

        let fresh = model.newDraft(contact: contacts.first, today: jan1)
        transport.on("POST", "/api/invoices/acme/items", status: 201, [
            "invoice": ["id": .string(String(repeating: "f", count: 32)), "number": "A-0007", "status": "draft"],
        ])
        let saved = try? await model.save(fresh)
        XCTAssertEqual(saved?.number, "A-0007")
        XCTAssertEqual(model.notice, "Invoice A-0007 saved.")
        XCTAssertEqual(transport.last("POST", "/api/invoices/acme/items")?.body?.str("contactId"), hexB)
    }

    @MainActor func testModelSeesAnUninstallAsNotInstalled() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/invoices", ["organizations": [
            ["name": "acme", "role": "member", "installed": true], ["name": "beta", "role": "owner", "installed": true],
        ]])
        transport.on("GET", "/api/invoices/acme", status: 409, ["error": "plugin_not_installed", "canInstall": false])
        let model = InvoicesModel(client: client, organization: "acme")
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        XCTAssertTrue(model.needsInstall)
        XCTAssertFalse(model.canManage)
        XCTAssertEqual(model.errorText, "")
        let refused = await model.install()
        XCTAssertFalse(refused)
        var beta = overview()
        beta["organization"] = "beta"
        beta["role"] = "owner"
        transport.on("GET", "/api/invoices/beta", beta)
        await model.select("beta")
        XCTAssertEqual(model.board?.organization, "beta")
        XCTAssertFalse(model.needsInstall)
        XCTAssertTrue(model.canManage)
    }

    @MainActor func testModelFindsALinkedInvoiceInAnotherOrganization() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/invoices", ["organizations": [
            ["name": "acme", "role": "member", "installed": true], ["name": "beta", "role": "member", "installed": true],
        ]])
        transport.on("GET", "/api/invoices/acme", overview())
        var beta = overview()
        beta["organization"] = "beta"
        transport.on("GET", "/api/invoices/beta", beta)
        transport.on("GET", "/api/invoices/beta/items/" + hexB, ["invoice": ["id": .string(hexB), "number": "B-1"]])
        let model = InvoicesModel(client: client)
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        let found = await model.locate(hexB)
        XCTAssertEqual(found?.number, "B-1")
        XCTAssertEqual(model.organization, "beta")
        XCTAssertEqual(model.board?.organization, "beta")
        let missing = await model.locate("nothex")
        XCTAssertNil(missing)
    }

    func testPackageRoutesListInvoices() {
        XCTAssertEqual(
            PackageRoutes.route(for: MarketPackage(id: "invoices", name: "Invoicing")), .invoices(organization: nil, invoice: nil))
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "invoices"), "invoices")
        XCTAssertTrue(PackageRoutes.businessPackageIds.contains("invoices"))
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["invoices"]).map(\.id), ["market", "invoices", "ledger"])
        XCTAssertEqual(
            PackageRoutes.hubRoutes(installed: ["crm", "competitor", "invoices"]).map(\.title),
            ["Market", "CRM", "Competitors", "Invoices", "Ledger"])
        XCTAssertEqual(PluginMenu.packageId(for: .invoices(organization: "acme", invoice: nil)), "invoices")
        XCTAssertTrue(PluginMenu.addsButton(for: .invoices(organization: nil, invoice: nil)))
    }
}
