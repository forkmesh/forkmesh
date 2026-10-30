import FMApi
import FMBase
import FMPackages
import SwiftUI

struct InvoiceEditor: View {
    let model: InvoicesModel
    @State private var draft: InvoiceDraft
    @State private var contacts: [InvoiceContact] = []
    @State private var saving = false
    @State private var errorText = ""
    let onSaved: (Invoice) -> Void
    @Environment(\.dismiss) private var dismiss

    init(model: InvoicesModel, draft: InvoiceDraft, onSaved: @escaping (Invoice) -> Void) {
        self.model = model
        _draft = State(initialValue: draft)
        self.onSaved = onSaved
    }

    private var chosenContact: InvoiceContact? { contacts.first { $0.id == draft.contactId } }

    private func money(_ minor: Int64) -> String { InvoiceMoney.format(minor, draft.currencyCode) }

    var body: some View {
        Form {
            if model.crmInstalled {
                Section {
                    NavigationLink {
                        InvoiceContactPicker(contacts: contacts, selected: draft.contactId) { contact in
                            draft.choose(contact)
                        }
                    } label: {
                        LabeledContent("CRM contact") {
                            Text(chosenContact?.label ?? (draft.contactId.isEmpty ? "Not linked" : "Linked contact"))
                                .lineLimit(1)
                        }
                    }
                    .accessibilityIdentifier("invoice-contact")
                } footer: {
                    Text("Choosing a contact fills in the name, email and company.")
                }
            }
            Section("Client") {
                TextField("Name", text: $draft.client.name).textContentType(.name)
                TextField("Email", text: $draft.client.email)
                    .keyboardType(.emailAddress).textContentType(.emailAddress).textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                TextField("Company", text: $draft.client.company).textContentType(.organizationName)
                TextField("Address", text: $draft.client.address, axis: .vertical).lineLimit(1...4)
            }
            Section("Invoice") {
                TextField(draft.isNew ? "Number (next in sequence)" : "Number", text: $draft.number)
                    .textInputAutocapitalization(.characters).autocorrectionDisabled()
                DatePicker("Issue date", selection: dateBinding(\.issuedAt), displayedComponents: .date)
                DatePicker("Due date", selection: dateBinding(\.dueAt), displayedComponents: .date)
                if !draft.datesAreValid {
                    Text("The due date is before the issue date.").font(.caption).foregroundStyle(.red)
                }
                LabeledContent("Currency") {
                    TextField("USD", text: $draft.currency)
                        .multilineTextAlignment(.trailing)
                        .textInputAutocapitalization(.characters).autocorrectionDisabled()
                }
                if !draft.currencyIsValid {
                    Text("Use a three-letter currency code such as USD.").font(.caption).foregroundStyle(.red)
                }
                LabeledContent("Tax %") {
                    TextField("0", text: $draft.taxPercent)
                        .multilineTextAlignment(.trailing)
                        .keyboardType(.decimalPad)
                }
            }
            Section {
                ForEach($draft.lines) { $line in
                    VStack(alignment: .leading, spacing: 6) {
                        TextField("Description", text: $line.description, axis: .vertical).lineLimit(1...3)
                        HStack(spacing: 8) {
                            TextField("Qty", text: $line.quantity)
                                .keyboardType(.decimalPad)
                                .frame(maxWidth: 70)
                                .accessibilityLabel("Quantity")
                            Text("×").foregroundStyle(.secondary)
                            TextField("Unit price", text: $line.unit)
                                .keyboardType(.decimalPad)
                                .accessibilityLabel("Unit price")
                            Spacer()
                            Text(money(line.amount)).font(.subheadline.monospacedDigit()).foregroundStyle(.secondary)
                        }
                    }
                    .swipeActions {
                        Button("Remove", role: .destructive) { draft.removeLine(line.id) }
                    }
                }
                Button {
                    draft.addLine()
                } label: {
                    Label("Add line", systemImage: "plus.circle")
                }
            } header: {
                Text("Items")
            } footer: {
                Text("Swipe a line to remove it.")
            }
            Section {
                LabeledContent("Subtotal", value: money(draft.subtotal))
                LabeledContent("Tax (\(InvoicePercent.text(draft.taxRate))%)", value: money(draft.tax))
                LabeledContent("Total") {
                    Text(money(draft.total)).font(.body.weight(.semibold).monospacedDigit())
                }
            }
            Section {
                TextField("https://", text: $draft.paymentUrl)
                    .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                if !draft.paymentUrlIsValid {
                    Text("Use an https:// address.").font(.caption).foregroundStyle(.red)
                }
            } header: {
                Text("Pay link")
            } footer: {
                Text("Shown as a Pay button on the client's invoice page.")
            }
            Section("Notes") {
                TextField("Notes for the client", text: $draft.notes, axis: .vertical).lineLimit(2...8)
            }
            Section("Terms") {
                TextField("Payment terms", text: $draft.terms, axis: .vertical).lineLimit(2...6)
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .navigationTitle(draft.isNew ? "New invoice" : "Edit \(draft.number)")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") { Task { await save() } }
                    .disabled(saving || !draft.canSave)
                    .accessibilityIdentifier("invoice-save")
            }
        }
        .interactiveDismissDisabled(saving)
        .task {
            if model.crmInstalled { contacts = await model.loadContacts() }
        }
    }

    private func dateBinding(_ key: WritableKeyPath<InvoiceDraft, Int64>) -> Binding<Date> {
        Binding(
            get: { InvoiceDates.localDate(draft[keyPath: key] > 0 ? draft[keyPath: key] : InvoiceDates.today()) },
            set: { draft[keyPath: key] = InvoiceDates.midnight(of: $0) })
    }

    private func save() async {
        saving = true
        defer { saving = false }
        do {
            let saved = try await model.save(draft)
            FmHaptics.commit()
            onSaved(saved)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "Could not save the invoice: " + InvoiceText.describe(error)
        }
    }
}

struct InvoiceContactPicker: View {
    let contacts: [InvoiceContact]
    let selected: String
    let onPick: (InvoiceContact?) -> Void
    @State private var query = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Button {
                onPick(nil)
                dismiss()
            } label: {
                HStack {
                    Text("Not linked to a CRM contact")
                    Spacer()
                    if selected.isEmpty { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
                }
            }
            if contacts.isEmpty {
                Text("No CRM contacts yet.").foregroundStyle(.secondary)
            }
            ForEach(contacts.filter { PackagesText.matches(query, $0.name, $0.email, $0.company) }) { contact in
                Button {
                    onPick(contact)
                    dismiss()
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(contact.name.isEmpty ? contact.email : contact.name).foregroundStyle(.primary)
                            let detail = [contact.name.isEmpty ? "" : contact.email, contact.company].filter { !$0.isEmpty }
                                .joined(separator: " · ")
                            if !detail.isEmpty { Text(detail).font(.caption).foregroundStyle(.secondary) }
                        }
                        Spacer()
                        if contact.id == selected { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        .searchable(text: $query, prompt: "Name, email or company")
        .navigationTitle("CRM contact")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct InvoiceSendSheet: View {
    let model: InvoicesModel
    let invoice: Invoice
    @State private var draft: InvoiceSendDraft
    @State private var sending = false
    @State private var errorText = ""
    let onSent: (Invoice) -> Void
    @Environment(\.dismiss) private var dismiss

    init(model: InvoicesModel, invoice: Invoice, draft: InvoiceSendDraft, onSent: @escaping (Invoice) -> Void) {
        self.model = model
        self.invoice = invoice
        _draft = State(initialValue: draft)
        self.onSent = onSent
    }

    var body: some View {
        Form {
            Section {
                TextField("To", text: $draft.to)
                    .keyboardType(.emailAddress).textContentType(.emailAddress).textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                TextField(draft.subjectPlaceholder(business: model.business), text: $draft.subject)
            }
            Section {
                TextField(InvoiceSendDraft.messagePlaceholder, text: $draft.message, axis: .vertical).lineLimit(4...12)
            } header: {
                Text("Message")
            } footer: {
                Text(
                    "The email links to a private page where \(invoice.client.label) can view and pay \(invoice.money(invoice.balance > 0 ? invoice.balance : invoice.total)). You see when it is opened, viewed and the pay link clicked.")
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .navigationTitle(draft.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Send") { Task { await send() } }
                    .disabled(sending || !draft.canSend)
                    .accessibilityIdentifier("invoice-send-submit")
            }
        }
        .interactiveDismissDisabled(sending)
    }

    private func send() async {
        sending = true
        defer { sending = false }
        do {
            let sent = try await model.send(invoice, draft)
            FmHaptics.send()
            onSent(sent)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "The invoice was not sent: " + InvoiceText.describe(error)
        }
    }
}

struct InvoicePaymentSheet: View {
    let model: InvoicesModel
    let invoice: Invoice
    @State private var draft: InvoicePaymentDraft
    @State private var saving = false
    @State private var errorText = ""
    let onPaid: (Invoice) -> Void
    @Environment(\.dismiss) private var dismiss

    init(model: InvoicesModel, invoice: Invoice, draft: InvoicePaymentDraft, onPaid: @escaping (Invoice) -> Void) {
        self.model = model
        self.invoice = invoice
        _draft = State(initialValue: draft)
        self.onPaid = onPaid
    }

    var body: some View {
        Form {
            Section {
                LabeledContent("Amount (\(invoice.currency))") {
                    TextField("0.00", text: $draft.amount)
                        .multilineTextAlignment(.trailing)
                        .keyboardType(.decimalPad)
                }
                DatePicker(
                    "Paid on",
                    selection: Binding(
                        get: { InvoiceDates.localDate(draft.paidAt) }, set: { draft.paidAt = InvoiceDates.midnight(of: $0) }),
                    displayedComponents: .date)
                TextField("Method (bank transfer, card…)", text: $draft.method)
                TextField("Note", text: $draft.note, axis: .vertical).lineLimit(1...4)
            } footer: {
                Text(
                    invoice.balance > 0
                        ? "\(invoice.money(invoice.balance)) is due. A smaller amount records a part payment."
                        : "Recording the full amount marks the invoice paid.")
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .navigationTitle(invoice.canMarkPaid ? "Mark paid" : "Record payment")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Record") { Task { await record() } }
                    .disabled(saving || !draft.canRecord)
            }
        }
        .interactiveDismissDisabled(saving)
    }

    private func record() async {
        saving = true
        defer { saving = false }
        do {
            let paid = try await model.recordPayment(invoice, draft)
            FmHaptics.commit()
            onPaid(paid)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "The payment was not recorded: " + InvoiceText.describe(error)
        }
    }
}

struct InvoiceSettingsSheet: View {
    let model: InvoicesModel
    @State private var settings: InvoiceSettings
    @State private var taxPercent: String
    @State private var saving = false
    @State private var errorText = ""
    @Environment(\.dismiss) private var dismiss

    init(model: InvoicesModel) {
        self.model = model
        _settings = State(initialValue: model.settings)
        _taxPercent = State(initialValue: InvoicePercent.text(model.settings.taxRate))
    }

    var body: some View {
        Form {
            Section {
                TextField("Business name", text: $settings.businessName)
                TextField("Business address", text: $settings.businessAddress, axis: .vertical).lineLimit(2...5)
            } header: {
                Text("Business")
            } footer: {
                Text(model.canManage ? "Shown on every invoice and invoice email." : "Only an organization owner or admin can change these.")
            }
            Section {
                TextField("From name", text: $settings.fromName)
                TextField("From email", text: $settings.fromEmail, prompt: Text("Uses the CRM sender when empty"))
                    .keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
            } header: {
                Text("Sender")
            }
            Section("Numbers and defaults") {
                LabeledContent("Number prefix") {
                    TextField("INV-", text: $settings.prefix).multilineTextAlignment(.trailing)
                        .textInputAutocapitalization(.characters).autocorrectionDisabled()
                }
                LabeledContent("Next number") {
                    TextField("1", value: $settings.nextNumber, format: .number).multilineTextAlignment(.trailing)
                        .keyboardType(.numberPad)
                }
                LabeledContent("Default currency") {
                    TextField("USD", text: $settings.currency).multilineTextAlignment(.trailing)
                        .textInputAutocapitalization(.characters).autocorrectionDisabled()
                }
                LabeledContent("Days until due") {
                    TextField("30", value: $settings.dueDays, format: .number).multilineTextAlignment(.trailing)
                        .keyboardType(.numberPad)
                }
                LabeledContent("Default tax %") {
                    TextField("0", text: $taxPercent).multilineTextAlignment(.trailing).keyboardType(.decimalPad)
                }
            }
            Section("Payment") {
                TextField("Default pay link (https://)", text: $settings.paymentUrl)
                    .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                if !settings.paymentUrlIsValid {
                    Text("Use an https:// address.").font(.caption).foregroundStyle(.red)
                }
                TextField("Payment instructions", text: $settings.paymentInstructions, prompt: Text("Bank details or how to pay"), axis: .vertical)
                    .lineLimit(2...6)
                TextField("Footer", text: $settings.footer, axis: .vertical).lineLimit(1...4)
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .disabled(!model.canManage || saving)
        .navigationTitle("Invoicing settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button(model.canManage ? "Cancel" : "Done") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                if model.canManage {
                    Button("Save") { Task { await save() } }
                        .disabled(saving || !settings.paymentUrlIsValid)
                }
            }
        }
        .task {
            if let fresh = try? await model.client.invoiceSettings(model.organization), !saving {
                settings = fresh
                taxPercent = InvoicePercent.text(fresh.taxRate)
            }
        }
    }

    private func save() async {
        saving = true
        defer { saving = false }
        var next = settings
        next.taxRate = InvoicePercent.basisPoints(taxPercent)
        do {
            _ = try await model.saveSettings(next)
            FmHaptics.commit()
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "Settings were not saved: " + InvoiceText.describe(error)
        }
    }
}
