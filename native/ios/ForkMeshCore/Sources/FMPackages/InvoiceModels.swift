import FMBase
import Foundation

public enum InvoiceMoney {
    public static func isCurrency(_ value: String) -> Bool {
        value.count == 3 && value.unicodeScalars.allSatisfy { $0.value >= 65 && $0.value <= 90 }
    }

    public static func code(_ value: String, fallback: String = "USD") -> String {
        let upper = value.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        return isCurrency(upper) ? upper : fallback
    }

    public static func format(_ minor: Int64, _ currency: String, locale: Locale = .current) -> String {
        let upper = currency.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        guard isCurrency(upper) else {
            let plain = decimal(minor)
            return upper.isEmpty ? plain : upper + " " + plain
        }
        let formatter = NumberFormatter()
        formatter.numberStyle = .currency
        formatter.locale = locale
        formatter.currencyCode = upper
        formatter.minimumFractionDigits = 2
        formatter.maximumFractionDigits = 2
        let value = NSDecimalNumber(mantissa: UInt64(minor.magnitude), exponent: -2, isNegative: minor < 0)
        return formatter.string(from: value) ?? upper + " " + decimal(minor)
    }

    public static func map(_ amounts: [String: Int64], fallback: String, locale: Locale = .current) -> String {
        let entries = amounts.filter { $0.value != 0 }.sorted { $0.key < $1.key }
        if entries.isEmpty { return format(0, fallback, locale: locale) }
        return entries.map { format($0.value, $0.key, locale: locale) }.joined(separator: " · ")
    }

    public static func decimal(_ minor: Int64) -> String {
        let whole = minor.magnitude / 100
        let part = minor.magnitude % 100
        return (minor < 0 ? "-" : "") + "\(whole)." + (part < 10 ? "0" : "") + "\(part)"
    }

    public static func minor(from text: String) -> Int64 {
        var kept = String(text.filter { $0.isASCII && ($0.isNumber || $0 == "." || $0 == ",") })
        if let last = kept.lastIndex(where: { $0 == "." || $0 == "," }) {
            let fraction = kept[kept.index(after: last)...]
            if fraction.count <= 2 {
                let head = kept[..<last].filter(\.isNumber)
                kept = head + "." + fraction
            } else {
                kept = kept.filter(\.isNumber)
            }
        }
        guard let value = Double(kept), value.isFinite else { return 0 }
        return min(1_000_000_000_000, max(0, Int64((value * 100).rounded())))
    }

    public static func quantity(from text: String) -> Double {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        guard let value = Double(cleaned), value.isFinite else { return 0 }
        return min(1_000_000, max(0, value))
    }

    public static func lineAmount(quantity: Double, unit: Int64) -> Int64 {
        Int64((quantity * Double(unit)).rounded())
    }

    public static func tax(subtotal: Int64, basisPoints: Int) -> Int64 {
        Int64((Double(subtotal) * Double(basisPoints) / 10_000).rounded())
    }
}

public enum InvoicePercent {
    public static func text(_ basisPoints: Int) -> String {
        let value = Double(basisPoints) / 100
        return InvoiceText.number(value, places: 2)
    }

    public static func basisPoints(_ text: String) -> Int {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        guard let value = Double(cleaned), value.isFinite else { return 0 }
        return min(100_000, max(0, Int((value * 100).rounded())))
    }
}

public enum InvoiceDates {
    public static let dayMs: Int64 = 86_400_000

    static var utc: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC") ?? TimeZone(secondsFromGMT: 0)!
        return calendar
    }

    public static func midnight(year: Int, month: Int, day: Int) -> Int64 {
        let date = utc.date(from: DateComponents(year: year, month: month, day: day)) ?? Date(timeIntervalSince1970: 0)
        return Int64((date.timeIntervalSince1970 * 1000).rounded())
    }

    public static func midnight(of date: Date, in calendar: Calendar = .current) -> Int64 {
        let parts = calendar.dateComponents([.year, .month, .day], from: date)
        return midnight(year: parts.year ?? 1970, month: parts.month ?? 1, day: parts.day ?? 1)
    }

    public static func today(now: Date = Date(), calendar: Calendar = .current) -> Int64 {
        midnight(of: now, in: calendar)
    }

    public static func localDate(_ ms: Int64, in calendar: Calendar = .current) -> Date {
        let parts = utc.dateComponents([.year, .month, .day], from: Date(timeIntervalSince1970: TimeInterval(ms) / 1000))
        return calendar.date(from: DateComponents(year: parts.year, month: parts.month, day: parts.day))
            ?? Date(timeIntervalSince1970: TimeInterval(ms) / 1000)
    }

    public static func label(_ ms: Int64, locale: Locale = .current) -> String {
        guard ms > 0 else { return "—" }
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = utc.timeZone
        formatter.dateStyle = .medium
        formatter.timeStyle = .none
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(ms) / 1000))
    }
}

public enum InvoiceText {
    public static func number(_ value: Double, places: Int = 1) -> String {
        var scale = 1.0
        for _ in 0..<max(0, places) { scale *= 10 }
        let rounded = (value * scale).rounded() / scale
        if rounded == rounded.rounded() { return String(Int64(rounded)) }
        var text = String(format: "%.\(places)f", rounded)
        while text.hasSuffix("0") { text.removeLast() }
        if text.hasSuffix(".") { text.removeLast() }
        return text
    }

    public static func plural(_ count: Int, _ one: String, _ many: String? = nil) -> String {
        "\(count) \(count == 1 ? one : (many ?? one + "s"))"
    }

    public static func statusTitle(_ key: String) -> String {
        switch key {
        case "draft": return "Draft"
        case "sent": return "Sent"
        case "partial": return "Part paid"
        case "overdue": return "Overdue"
        case "paid": return "Paid"
        case "void": return "Void"
        default: return CrmStageLabel.title(key)
        }
    }

    public static func describe(_ error: Error) -> String {
        guard let error = error as? PackagesError else { return PackagesError.describe(error) }
        switch error.code {
        case "invoice_locked": return "Only draft and sent invoices can be changed."
        case "void_before_delete": return "Void a sent invoice before deleting it."
        case "invoice_number_taken": return "Another invoice already uses that number."
        case "invoice_has_no_amount": return "Add a line with an amount before sending."
        case "invalid_payment_url": return "The pay link must start with https://."
        case "crm_not_installed": return "Install the CRM for this organization to link contacts."
        case "invoice_not_found": return "That invoice was removed or you no longer have access."
        case "settings_require_admin": return "Only an organization owner or admin can change these."
        case "invalid_amount": return "Enter an amount above zero."
        case "invoice_already_void": return "This invoice is already void."
        case "invoice_not_closed": return "Only paid or void invoices can be reopened."
        case "crm_contact_not_found": return "That CRM contact was removed."
        case "invalid_contact_id": return "That CRM contact is not valid."
        case "invalid_sender": return "Add a From email in Invoicing settings, or in the CRM's email settings."
        case "invalid_recipient": return "That recipient address is not valid."
        case "plugin_not_installed": return "Invoicing is not installed for this organization."
        case "payload_too_large": return "The invoice is too large to save."
        default: return error.message
        }
    }
}

public struct InvoiceClient: Equatable, Sendable {
    public var name: String
    public var email: String
    public var company: String
    public var address: String

    public init(name: String = "", email: String = "", company: String = "", address: String = "") {
        self.name = name
        self.email = email
        self.company = company
        self.address = address
    }

    public init(json: JSONObject) {
        self.init(name: json.str("name"), email: json.str("email"), company: json.str("company"), address: json.str("address"))
    }

    public var body: JSONObject {
        ["name": .string(name), "email": .string(email), "company": .string(company), "address": .string(address)]
    }

    public var label: String {
        if !name.isEmpty { return name }
        if !company.isEmpty { return company }
        if !email.isEmpty { return email }
        return "No client"
    }
}

public struct InvoiceLine: Equatable, Sendable {
    public var description: String
    public var quantity: Double
    public var unitAmount: Int64

    public init(description: String = "", quantity: Double = 1, unitAmount: Int64 = 0) {
        self.description = description
        self.quantity = quantity
        self.unitAmount = unitAmount
    }

    public init(json: JSONObject) {
        self.init(description: json.str("description"), quantity: json.double("quantity", 1), unitAmount: json.int64("unitAmount"))
    }

    public var amount: Int64 { InvoiceMoney.lineAmount(quantity: quantity, unit: unitAmount) }

    public var quantityText: String { InvoiceText.number(quantity, places: 3) }
}

public struct InvoicePayment: Equatable, Sendable {
    public var amount: Int64
    public var at: Int64
    public var method: String
    public var note: String
    public var by: String

    public init(json: JSONObject) {
        amount = json.int64("amount")
        at = json.int64("at")
        method = json.str("method")
        note = json.str("note")
        by = json.str("by")
    }
}

public struct InvoiceContact: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var email: String
    public var company: String
    public var stage: String

    public init(id: String, name: String = "", email: String = "", company: String = "", stage: String = "") {
        self.id = id
        self.name = name
        self.email = email
        self.company = company
        self.stage = stage
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), name: json.str("name"), email: json.str("email"), company: json.str("company"),
            stage: json.str("stage"))
    }

    public var label: String {
        [name.isEmpty ? email : name, company].filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

public struct InvoiceEvent: Equatable, Sendable, Identifiable {
    public var kind: String
    public var at: Int64
    public var actor: String
    public var client: String
    public var to: String
    public var amount: Int64
    public var method: String
    public var error: String
    public var invoiceId: String
    public var number: String
    public var clientName: String
    public var currency: String

    public init(json: JSONObject) {
        kind = json.str("kind")
        at = json.int64("at")
        actor = json.str("actor")
        client = json.str("client")
        to = json.str("to")
        amount = json.int64("amount")
        method = json.str("method")
        error = json.str("error")
        invoiceId = json.str("invoiceId")
        number = json.str("number")
        clientName = json.str("clientName")
        currency = json.str("currency")
    }

    public var id: String { "\(invoiceId):\(kind):\(at):\(actor):\(client)" }

    public var title: String {
        switch kind {
        case "created": return "Created"
        case "sent": return "Emailed"
        case "reminder": return "Reminder emailed"
        case "failed": return "Email failed"
        case "opened": return "Email opened"
        case "viewed": return "Invoice viewed"
        case "clicked": return "Pay link clicked"
        case "paid": return "Payment recorded"
        case "reopened": return "Reopened"
        case "voided": return "Voided"
        case "overdue": return "Became overdue"
        default: return CrmStageLabel.title(kind)
        }
    }

    public var symbol: String {
        switch kind {
        case "created": return "doc.badge.plus"
        case "sent": return "paperplane"
        case "reminder": return "bell"
        case "failed": return "exclamationmark.circle"
        case "opened": return "envelope.open"
        case "viewed": return "eye"
        case "clicked": return "cursorarrow.click"
        case "paid": return "checkmark.seal"
        case "reopened": return "arrow.uturn.backward"
        case "voided": return "nosign"
        case "overdue": return "clock"
        default: return "circle"
        }
    }

    public func detail(currency fallback: String, locale: Locale = .current) -> String {
        let money = amount > 0 ? InvoiceMoney.format(amount, currency.isEmpty ? fallback : currency, locale: locale) : ""
        return [
            to.isEmpty ? "" : "to \(to)", client.isEmpty ? "" : "in \(client)", money, method,
            actor.isEmpty ? "" : "by \(actor)", error,
        ].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public func activityDetail(locale: Locale = .current) -> String {
        let money = kind == "paid" && amount > 0 ? InvoiceMoney.format(amount, currency, locale: locale) : ""
        return [clientName, client, money].filter { !$0.isEmpty }.joined(separator: " · ")
    }
}

public enum InvoiceFilter: String, CaseIterable, Sendable, Identifiable {
    case all
    case unpaid
    case overdue
    case drafts
    case paid
    case void

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .all: return "All invoices"
        case .unpaid: return "Unpaid"
        case .overdue: return "Overdue"
        case .drafts: return "Drafts"
        case .paid: return "Paid"
        case .void: return "Void"
        }
    }
}

public struct Invoice: Equatable, Sendable, Identifiable {
    public var id: String
    public var organization: String
    public var number: String
    public var status: String
    public var displayStatus: String
    public var contactId: String
    public var client: InvoiceClient
    public var currency: String
    public var items: [InvoiceLine]
    public var taxRate: Int
    public var subtotal: Int64
    public var tax: Int64
    public var total: Int64
    public var paidAmount: Int64
    public var balance: Int64
    public var notes: String
    public var terms: String
    public var paymentUrl: String
    public var payments: [InvoicePayment]
    public var issuedAt: Int64
    public var dueAt: Int64
    public var sentAt: Int64
    public var lastSentAt: Int64
    public var sentTo: String
    public var paidAt: Int64
    public var opens: Int
    public var views: Int
    public var clicks: Int
    public var firstOpenedAt: Int64
    public var firstViewedAt: Int64
    public var lastActivityAt: Int64
    public var createdBy: String
    public var updatedBy: String
    public var sentBy: String
    public var createdAt: Int64
    public var updatedAt: Int64
    public var publicUrl: String
    public var events: [InvoiceEvent]?
    public var contact: InvoiceContact?

    public init(json: JSONObject) {
        id = json.str("id")
        organization = json.str("organization")
        number = json.str("number")
        status = json.str("status", "draft")
        displayStatus = json.str("displayStatus")
        contactId = json.str("contactId")
        client = InvoiceClient(json: json.obj("client"))
        currency = InvoiceMoney.code(json.str("currency"))
        items = json.maps("items").map(InvoiceLine.init(json:))
        taxRate = json.int("taxRate")
        subtotal = json.int64("subtotal")
        tax = json.int64("tax")
        total = json.int64("total")
        paidAmount = json.int64("paidAmount")
        balance = json.int64("balance")
        notes = json.str("notes")
        terms = json.str("terms")
        paymentUrl = json.str("paymentUrl")
        payments = json.maps("payments").map(InvoicePayment.init(json:))
        issuedAt = json.int64("issuedAt")
        dueAt = json.int64("dueAt")
        sentAt = json.int64("sentAt")
        lastSentAt = json.int64("lastSentAt")
        sentTo = json.str("sentTo")
        paidAt = json.int64("paidAt")
        opens = json.int("opens")
        views = json.int("views")
        clicks = json.int("clicks")
        firstOpenedAt = json.int64("firstOpenedAt")
        firstViewedAt = json.int64("firstViewedAt")
        lastActivityAt = json.int64("lastActivityAt")
        createdBy = json.str("createdBy")
        updatedBy = json.str("updatedBy")
        sentBy = json.str("sentBy")
        createdAt = json.int64("createdAt")
        updatedAt = json.int64("updatedAt")
        let link = json.str("publicUrl")
        publicUrl = link.lowercased().hasPrefix("https://") ? link : ""
        events = json["events"]?.arrayValue.map { $0.compactMap(\.objectValue).map(InvoiceEvent.init(json:)) }
        let linked = json.obj("contact")
        contact = PackagesText.isHexId(linked.str("id")) ? InvoiceContact(json: linked) : nil
    }

    public var state: String { displayStatus.isEmpty ? status : displayStatus }

    public var stateTitle: String { InvoiceText.statusTitle(state) }

    public var isEditable: Bool { status == "draft" || status == "sent" }

    public var isDeletable: Bool { status == "draft" || status == "void" }

    public var canVoid: Bool { !isDeletable }

    public var canReopen: Bool { status == "paid" || status == "void" }

    public var canRecordPayment: Bool { isEditable && sentAt > 0 }

    public var canMarkPaid: Bool { status == "draft" }

    public var canSend: Bool { isEditable }

    public var isSent: Bool { sentAt > 0 }

    public var sendTitle: String { isSent ? "Send reminder" : "Email invoice" }

    public var shareURL: URL? { publicUrl.isEmpty ? nil : URL(string: publicUrl) }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(query, number, client.name, client.company, client.email)
    }

    public func matches(_ filter: InvoiceFilter) -> Bool {
        switch filter {
        case .all: return true
        case .unpaid: return ["sent", "partial", "overdue"].contains(state)
        case .overdue: return state == "overdue"
        case .drafts: return state == "draft"
        case .paid: return state == "paid" || status == "paid"
        case .void: return state == "void"
        }
    }

    public func money(_ minor: Int64, locale: Locale = .current) -> String {
        InvoiceMoney.format(minor, currency, locale: locale)
    }
}

public struct InvoiceCounts: Equatable, Sendable {
    public var draft: Int
    public var sent: Int
    public var overdue: Int
    public var paid: Int
    public var void: Int

    public init(json: JSONObject) {
        draft = json.int("draft")
        sent = json.int("sent")
        overdue = json.int("overdue")
        paid = json.int("paid")
        void = json.int("void")
    }
}

public struct InvoiceSummary: Equatable, Sendable {
    public var counts: InvoiceCounts
    public var outstanding: [String: Int64]
    public var overdueAmount: [String: Int64]
    public var paidLast30Days: [String: Int64]
    public var sent: Int
    public var opened: Int
    public var viewed: Int
    public var clicked: Int
    public var openRate: Int
    public var viewRate: Int
    public var averageDaysToPay: Double
    public var averageHoursToOpen: Double

    public init(json: JSONObject) {
        counts = InvoiceCounts(json: json.obj("counts"))
        outstanding = Self.amounts(json.obj("outstanding"))
        overdueAmount = Self.amounts(json.obj("overdueAmount"))
        paidLast30Days = Self.amounts(json.obj("paidLast30Days"))
        sent = json.int("sent")
        opened = json.int("opened")
        viewed = json.int("viewed")
        clicked = json.int("clicked")
        openRate = json.int("openRate")
        viewRate = json.int("viewRate")
        averageDaysToPay = json.double("averageDaysToPay")
        averageHoursToOpen = json.double("averageHoursToOpen")
    }

    static func amounts(_ json: JSONObject) -> [String: Int64] {
        var out: [String: Int64] = [:]
        for key in json.keys where InvoiceMoney.isCurrency(key) {
            let value = json.int64(key)
            if value != 0 { out[key] = value }
        }
        return out
    }

    public var outstandingDetail: String { InvoiceText.plural(counts.sent, "unpaid invoice") }

    public var overdueDetail: String { InvoiceText.plural(counts.overdue, "overdue invoice") }

    public var paidDetail: String { InvoiceText.plural(counts.paid, "paid invoice") + " in all" }

    public var openRateText: String { sent > 0 ? "\(openRate)%" : "—" }

    public var engagementText: String {
        sent > 0 ? "\(opened) of \(sent) opened · \(viewed) viewed · \(clicked) clicked pay" : "No invoices sent yet"
    }

    public var daysToPayText: String {
        averageDaysToPay > 0 ? "\(InvoiceText.number(averageDaysToPay)) days" : "—"
    }

    public var daysToPayDetail: String {
        averageHoursToOpen > 0
            ? "Opened \(InvoiceText.number(averageHoursToOpen)) h after sending, on average" : "From sending to paid"
    }
}

public struct InvoiceSettings: Equatable, Sendable {
    public var businessName: String
    public var businessAddress: String
    public var fromName: String
    public var fromEmail: String
    public var currency: String
    public var dueDays: Int
    public var taxRate: Int
    public var prefix: String
    public var nextNumber: Int
    public var paymentUrl: String
    public var paymentInstructions: String
    public var footer: String

    public init(json: JSONObject = [:]) {
        businessName = json.str("businessName")
        businessAddress = json.str("businessAddress")
        fromName = json.str("fromName")
        fromEmail = json.str("fromEmail")
        currency = InvoiceMoney.code(json.str("currency"))
        dueDays = json.int("dueDays", 30)
        taxRate = json.int("taxRate")
        prefix = json.str("prefix", "INV-")
        nextNumber = max(1, json.int("nextNumber", 1))
        paymentUrl = json.str("paymentUrl")
        paymentInstructions = json.str("paymentInstructions")
        footer = json.str("footer")
    }

    public var body: JSONObject {
        [
            "businessName": .string(businessName), "businessAddress": .string(businessAddress),
            "fromName": .string(fromName), "fromEmail": .string(fromEmail.trimmingCharacters(in: .whitespaces)),
            "currency": .string(InvoiceMoney.code(currency, fallback: currency.uppercased())),
            "dueDays": .int(Int64(min(365, max(0, dueDays)))), "taxRate": .int(Int64(min(100_000, max(0, taxRate)))),
            "prefix": .string(prefix), "nextNumber": .int(Int64(max(1, nextNumber))),
            "paymentUrl": .string(paymentUrl.trimmingCharacters(in: .whitespaces)),
            "paymentInstructions": .string(paymentInstructions), "footer": .string(footer),
        ]
    }

    public var paymentUrlIsValid: Bool { InvoiceDraft.isPayLink(paymentUrl) }
}

public struct InvoiceBoard: Equatable, Sendable {
    public var organization: String
    public var role: String
    public var canManagePlugin: Bool
    public var crmInstalled: Bool
    public var invoices: [Invoice]
    public var summary: InvoiceSummary
    public var activity: [InvoiceEvent]
    public var settings: InvoiceSettings

    public init(json: JSONObject) {
        organization = json.str("organization")
        role = json.str("role", "member")
        canManagePlugin = json.bool("canManagePlugin")
        crmInstalled = json.bool("crmInstalled")
        invoices = json.maps("invoices").map(Invoice.init(json:)).filter { PackagesText.isHexId($0.id) }
        summary = InvoiceSummary(json: json.obj("summary"))
        activity = json.maps("activity").map(InvoiceEvent.init(json:))
        settings = InvoiceSettings(json: json.obj("settings"))
    }

    public var canManage: Bool { canManagePlugin || role == "owner" || role == "admin" }

    public func filtered(query: String = "", filter: InvoiceFilter = .all) -> [Invoice] {
        invoices.filter { $0.matches(filter) && $0.matches(query) }
    }

    public func invoice(_ id: String) -> Invoice? { invoices.first { $0.id == id } }

    public mutating func replace(_ invoice: Invoice) {
        var stored = invoice
        stored.events = nil
        if let index = invoices.firstIndex(where: { $0.id == invoice.id }) {
            invoices[index] = stored
        } else {
            invoices.insert(stored, at: 0)
        }
    }

    public mutating func remove(_ id: String) {
        invoices.removeAll { $0.id == id }
    }
}

public struct ContactInvoices: Equatable, Sendable {
    public var contactId: String
    public var invoices: [Invoice]
    public var billed: [String: Int64]
    public var paid: [String: Int64]
    public var outstanding: [String: Int64]

    public init(json: JSONObject) {
        contactId = json.str("contactId")
        invoices = json.maps("invoices").map(Invoice.init(json:)).filter { PackagesText.isHexId($0.id) }
        let totals = json.obj("totals")
        billed = InvoiceSummary.amounts(totals.obj("billed"))
        paid = InvoiceSummary.amounts(totals.obj("paid"))
        outstanding = InvoiceSummary.amounts(totals.obj("outstanding"))
    }
}

public enum ContactInvoicesState: Equatable, Sendable {
    case loading
    case hidden
    case notInstalled(canInstall: Bool)
    case loaded(ContactInvoices)
    case failed(String)

    public static func from(_ error: Error) -> ContactInvoicesState {
        guard let error = error as? PackagesError else { return .failed(InvoiceText.describe(error)) }
        if error.status == 404 || error.code == "not_found" || error.code == "organization_not_found" { return .hidden }
        if error.code == "plugin_not_installed" { return .notInstalled(canInstall: error.fields["canInstall"] == "true") }
        if error.code == "crm_not_installed" { return .hidden }
        return .failed(InvoiceText.describe(error))
    }
}
