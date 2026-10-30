import FMBase
import Foundation

public struct InvoiceDraftLine: Equatable, Sendable, Identifiable {
    public var id: String
    public var description: String
    public var quantity: String
    public var unit: String

    public init(id: String = UUID().uuidString, description: String = "", quantity: String = "1", unit: String = "") {
        self.id = id
        self.description = description
        self.quantity = quantity
        self.unit = unit
    }

    public init(_ line: InvoiceLine) {
        self.init(description: line.description, quantity: line.quantityText, unit: InvoiceMoney.decimal(line.unitAmount))
    }

    public var quantityValue: Double { InvoiceMoney.quantity(from: quantity) }

    public var unitAmount: Int64 { InvoiceMoney.minor(from: unit) }

    public var amount: Int64 { InvoiceMoney.lineAmount(quantity: quantityValue, unit: unitAmount) }

    public var body: JSONObject {
        ["description": .string(description), "quantity": .double(quantityValue), "unitAmount": .int(unitAmount)]
    }
}

public struct InvoiceDraft: Equatable, Sendable {
    public var id: String
    public var contactId: String
    public var client: InvoiceClient
    public var number: String
    public var issuedAt: Int64
    public var dueAt: Int64
    public var currency: String
    public var taxPercent: String
    public var lines: [InvoiceDraftLine]
    public var paymentUrl: String
    public var notes: String
    public var terms: String

    public init(settings: InvoiceSettings, today: Int64, contact: InvoiceContact? = nil) {
        id = ""
        contactId = ""
        client = InvoiceClient()
        number = ""
        issuedAt = today
        dueAt = today + Int64(settings.dueDays) * InvoiceDates.dayMs
        currency = settings.currency
        taxPercent = InvoicePercent.text(settings.taxRate)
        lines = [InvoiceDraftLine()]
        paymentUrl = settings.paymentUrl
        notes = ""
        terms = ""
        if let contact { choose(contact) }
    }

    public init(editing invoice: Invoice) {
        id = invoice.id
        contactId = invoice.contactId
        client = invoice.client
        number = invoice.number
        issuedAt = invoice.issuedAt
        dueAt = invoice.dueAt
        currency = invoice.currency
        taxPercent = InvoicePercent.text(invoice.taxRate)
        lines = invoice.items.map(InvoiceDraftLine.init)
        if lines.isEmpty { lines = [InvoiceDraftLine()] }
        paymentUrl = invoice.paymentUrl
        notes = invoice.notes
        terms = invoice.terms
    }

    public init(duplicating invoice: Invoice, settings: InvoiceSettings, today: Int64) {
        self.init(editing: invoice)
        id = ""
        number = ""
        issuedAt = today
        dueAt = today + Int64(settings.dueDays) * InvoiceDates.dayMs
    }

    public var isNew: Bool { id.isEmpty }

    public var currencyCode: String { InvoiceMoney.code(currency) }

    public var taxRate: Int { InvoicePercent.basisPoints(taxPercent) }

    public var subtotal: Int64 { lines.reduce(0) { $0 + $1.amount } }

    public var tax: Int64 { InvoiceMoney.tax(subtotal: subtotal, basisPoints: taxRate) }

    public var total: Int64 { subtotal + tax }

    public static func isPayLink(_ value: String) -> Bool {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty || (trimmed.lowercased().hasPrefix("https://") && trimmed.count > 8 && !trimmed.contains(" "))
    }

    public var paymentUrlIsValid: Bool { Self.isPayLink(paymentUrl) }

    public var currencyIsValid: Bool {
        InvoiceMoney.isCurrency(currency.trimmingCharacters(in: .whitespacesAndNewlines).uppercased())
    }

    public var datesAreValid: Bool { issuedAt > 0 && dueAt >= issuedAt }

    public var canSave: Bool { paymentUrlIsValid && currencyIsValid && datesAreValid }

    public mutating func choose(_ contact: InvoiceContact?) {
        guard let contact else {
            contactId = ""
            return
        }
        contactId = contact.id
        if !contact.name.isEmpty { client.name = contact.name }
        if !contact.email.isEmpty { client.email = contact.email }
        if !contact.company.isEmpty { client.company = contact.company }
    }

    public mutating func addLine() {
        lines.append(InvoiceDraftLine())
    }

    public mutating func removeLine(_ id: String) {
        lines.removeAll { $0.id == id }
        if lines.isEmpty { lines = [InvoiceDraftLine()] }
    }

    public func body(crmInstalled: Bool) -> JSONObject {
        var body: JSONObject = [
            "client": .object(client.body),
            "currency": .string(currency.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()),
            "taxRate": .int(Int64(taxRate)),
            "issuedAt": .int(issuedAt),
            "dueAt": .int(dueAt),
            "paymentUrl": .string(paymentUrl.trimmingCharacters(in: .whitespacesAndNewlines)),
            "notes": .string(notes),
            "terms": .string(terms),
            "items": .array(lines.map { .object($0.body) }),
        ]
        let trimmedNumber = number.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmedNumber.isEmpty { body["number"] = .string(trimmedNumber) }
        if crmInstalled { body["contactId"] = .string(contactId) }
        return body
    }
}

public struct InvoiceSendDraft: Equatable, Sendable {
    public var to: String
    public var subject: String
    public var message: String
    public let reminder: Bool
    public let number: String

    public init(invoice: Invoice) {
        to = invoice.client.email
        subject = ""
        message = ""
        reminder = invoice.isSent
        number = invoice.number
    }

    public var title: String { reminder ? "Remind about \(number)" : "Email \(number)" }

    public func subjectPlaceholder(business: String) -> String {
        "\(reminder ? "Reminder: invoice" : "Invoice") \(number) from \(business)"
    }

    public static let messagePlaceholder = "Leave empty for a short message with the amount and due date."

    public var canSend: Bool {
        let trimmed = to.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.contains("@") && !trimmed.contains(" ")
    }

    public var body: JSONObject {
        [
            "to": .string(to.trimmingCharacters(in: .whitespacesAndNewlines)), "subject": .string(subject),
            "message": .string(message),
        ]
    }
}

public struct InvoicePaymentDraft: Equatable, Sendable {
    public var amount: String
    public var paidAt: Int64
    public var method: String
    public var note: String

    public init(invoice: Invoice, today: Int64) {
        amount = InvoiceMoney.decimal(invoice.balance > 0 ? invoice.balance : invoice.total)
        paidAt = today
        method = ""
        note = ""
    }

    public var amountMinor: Int64 { InvoiceMoney.minor(from: amount) }

    public var canRecord: Bool { amountMinor > 0 && paidAt > 0 }

    public var body: JSONObject {
        ["amount": .int(amountMinor), "paidAt": .int(paidAt), "method": .string(method), "note": .string(note)]
    }
}
