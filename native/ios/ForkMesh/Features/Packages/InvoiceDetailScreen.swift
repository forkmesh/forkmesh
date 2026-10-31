import FMApi
import FMBase
import FMPackages
import SwiftUI

struct InvoiceDetailScreen: View {
    let model: InvoicesModel
    @State private var shownId: String
    @State private var invoice: Invoice?
    @State private var errorText = ""
    @State private var notice: FmNotice?
    @State private var sheet: InvoiceDetailSheet?
    @State private var confirmVoid = false
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss

    init(model: InvoicesModel, invoiceId: String) {
        self.model = model
        _shownId = State(initialValue: invoiceId)
        _invoice = State(initialValue: model.board?.invoice(invoiceId))
    }

    var body: some View {
        List {
            if !errorText.isEmpty || notice != nil {
                Section {
                    PackagesErrorBanner(text: errorText) { await load() }
                    if notice != nil { FmNoticeLine($notice) }
                }
            }
            if let invoice {
                headerSection(invoice)
                metricsSection(invoice)
                linesSection(invoice)
                if let url = invoice.shareURL { linkSection(url) }
                actionsSection(invoice)
                if !invoice.notes.isEmpty { Section("Notes") { Text(invoice.notes).textSelection(.enabled) } }
                if !invoice.terms.isEmpty { Section("Terms") { Text(invoice.terms).textSelection(.enabled) } }
                historySection(invoice)
            } else if errorText.isEmpty {
                ProgressView().frame(maxWidth: .infinity)
            }
        }
        .navigationTitle(invoice?.number ?? "Invoice")
        .navigationBarTitleDisplayMode(.inline)
        .task(id: shownId) { await load() }
        .fmRefreshable { await load() }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button("Edit") {
                    if let invoice { sheet = .edit(InvoiceDraft(editing: invoice)) }
                }
                .disabled(invoice?.isEditable != true || model.working)
            }
        }
        .sheet(item: $sheet) { item in
            NavigationStack {
                sheetContent(item)
            }
        }
        .confirmationDialog(
            "Void invoice \(invoice?.number ?? "")?", isPresented: $confirmVoid, titleVisibility: .visible
        ) {
            Button("Void", role: .destructive) { Task { await run { try await model.void($0) } } }
        } message: {
            Text("Its link will show it as void.")
        }
        .confirmationDialog(
            "Delete invoice \(invoice?.number ?? "")?", isPresented: $confirmDelete, titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        } message: {
            Text("This cannot be undone.")
        }
    }

    @ViewBuilder private func sheetContent(_ item: InvoiceDetailSheet) -> some View {
        switch item {
        case let .edit(draft):
            InvoiceEditor(model: model, draft: draft) { saved in
                if saved.id == shownId {
                    invoice = saved
                } else {
                    shownId = saved.id
                }
                showNotice()
            }
        case let .send(invoice, draft):
            InvoiceSendSheet(model: model, invoice: invoice, draft: draft) { sent in
                self.invoice = sent
                showNotice()
            }
        case let .payment(invoice, draft):
            InvoicePaymentSheet(model: model, invoice: invoice, draft: draft) { paid in
                self.invoice = paid
                showNotice()
            }
        }
    }

    private func showNotice() {
        guard !model.notice.isEmpty else { return }
        notice = FmNotice(model.notice)
        model.notice = ""
    }

    private func load() async {
        do {
            invoice = try await model.detail(shownId)
            errorText = ""
        } catch {
            errorText = "Could not open the invoice: " + InvoiceText.describe(error)
        }
    }

    private func run(_ action: (Invoice) async throws -> Invoice) async {
        guard let invoice else { return }
        do {
            self.invoice = try await action(invoice)
            FmHaptics.commit()
            errorText = ""
            showNotice()
        } catch {
            FmHaptics.failure()
            errorText = InvoiceText.describe(error)
        }
    }

    private func delete() async {
        guard let invoice else { return }
        do {
            try await model.delete(invoice)
            FmHaptics.commit(.delete)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "Could not delete the invoice: " + InvoiceText.describe(error)
        }
    }

    private func headerSection(_ invoice: Invoice) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(invoice.number).font(.title3.weight(.semibold))
                    InvoiceStatusBadge(state: invoice.state)
                    Spacer()
                }
                if invoice.contact != nil {
                    NavigationLink(value: Route.contact(id: model.organization + "/" + invoice.contactId)) {
                        Label(invoice.client.label, systemImage: "person.crop.rectangle.stack")
                            .font(.subheadline)
                    }
                } else {
                    Text(invoice.client.label).font(.subheadline)
                }
                let extra = [
                    invoice.client.name.isEmpty ? "" : invoice.client.company,
                    invoice.contactId.isEmpty || invoice.contact != nil ? "" : "CRM contact removed",
                ].filter { !$0.isEmpty }.joined(separator: " · ")
                if !extra.isEmpty { Text(extra).font(.caption).foregroundStyle(.secondary) }
                if !invoice.client.email.isEmpty {
                    Text(invoice.client.email).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }
        }
    }

    private func metricsSection(_ invoice: Invoice) -> some View {
        let openHint = invoice.firstOpenedAt > 0
            ? "first \(PackagesFormat.relative(invoice.firstOpenedAt))" : invoice.isSent ? "not yet" : ""
        let viewHint = invoice.firstViewedAt > 0
            ? "first \(PackagesFormat.relative(invoice.firstViewedAt))" : invoice.isSent ? "not yet" : ""
        let clickHint = invoice.clicks > 0 ? InvoiceText.plural(invoice.clicks, "pay click") : ""
        let dueHint = invoice.balance > 0
            ? "\(invoice.money(invoice.balance)) to pay" : invoice.status == "paid" ? "paid \(InvoiceDates.label(invoice.paidAt))" : ""
        return Section {
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], spacing: 8) {
                InvoiceSummaryTile(
                    value: invoice.money(invoice.total), label: "Total",
                    detail: invoice.paidAmount > 0 ? "\(invoice.money(invoice.paidAmount)) paid" : "", icon: "sum", tint: .blue)
                InvoiceSummaryTile(
                    value: InvoiceDates.label(invoice.dueAt), label: "Due", detail: dueHint, icon: "calendar",
                    tint: invoice.state == "overdue" ? .red : .secondary)
                InvoiceSummaryTile(
                    value: invoice.isSent ? "\(invoice.opens)" : "—", label: "Email opens", detail: openHint,
                    icon: "envelope.open", tint: .indigo)
                InvoiceSummaryTile(
                    value: invoice.isSent ? "\(invoice.views)" : "—", label: "Page views",
                    detail: [viewHint, clickHint].filter { !$0.isEmpty }.joined(separator: " · "), icon: "eye", tint: .teal)
            }
            .listRowInsets(EdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0))
            .listRowBackground(Color.clear)
        } footer: {
            if invoice.isSent {
                Text(sentLine(invoice))
            }
        }
    }

    private func sentLine(_ invoice: Invoice) -> String {
        let to = invoice.sentTo.isEmpty ? (invoice.client.email.isEmpty ? "the client" : invoice.client.email) : invoice.sentTo
        let when = PackagesFormat.relative(invoice.lastSentAt > 0 ? invoice.lastSentAt : invoice.sentAt)
        let by = invoice.sentBy.isEmpty ? "" : " by \(invoice.sentBy)"
        return "Emailed to \(to) \(when)\(by). Opens count when the reader's mail app loads images; some apps load them for privacy without the reader opening it."
    }

    private func linesSection(_ invoice: Invoice) -> some View {
        Section("Items") {
            ForEach(Array(invoice.items.enumerated()), id: \.offset) { _, line in
                HStack(alignment: .firstTextBaseline) {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(line.description.isEmpty ? "Item" : line.description).font(.subheadline)
                        Text("\(line.quantityText) × \(invoice.money(line.unitAmount))").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text(invoice.money(line.amount)).font(.subheadline.monospacedDigit())
                }
            }
            LabeledContent("Subtotal", value: invoice.money(invoice.subtotal))
            if invoice.tax > 0 {
                LabeledContent("Tax (\(InvoicePercent.text(invoice.taxRate))%)", value: invoice.money(invoice.tax))
            }
            LabeledContent("Total") {
                Text(invoice.money(invoice.total)).font(.body.weight(.semibold).monospacedDigit())
            }
            if invoice.paidAmount > 0 {
                LabeledContent("Paid", value: invoice.money(invoice.paidAmount))
            }
        }
    }

    private func linkSection(_ url: URL) -> some View {
        Section {
            ShareLink(item: url) {
                Label("Share invoice link", systemImage: "square.and.arrow.up")
            }
            Button {
                CrmClipboard.copy(url.absoluteString)
                notice = FmNotice("Invoice link copied.")
            } label: {
                Label("Copy link", systemImage: "link")
            }
            Link(destination: url) {
                Label("Preview", systemImage: "safari")
            }
        } header: {
            Text("Client link")
        } footer: {
            Text("Opening the link yourself counts as a view.")
        }
    }

    @ViewBuilder private func actionsSection(_ invoice: Invoice) -> some View {
        Section {
            if invoice.canSend {
                Button {
                    sheet = .send(invoice, InvoiceSendDraft(invoice: invoice))
                } label: {
                    Label(invoice.sendTitle, systemImage: invoice.isSent ? "bell" : "paperplane")
                }
                .accessibilityIdentifier("invoice-send")
            }
            if invoice.canRecordPayment {
                Button {
                    sheet = .payment(invoice, InvoicePaymentDraft(invoice: invoice, today: InvoiceDates.today()))
                } label: {
                    Label("Record payment", systemImage: "banknote")
                }
            }
            if invoice.canMarkPaid {
                Button {
                    sheet = .payment(invoice, InvoicePaymentDraft(invoice: invoice, today: InvoiceDates.today()))
                } label: {
                    Label("Mark paid", systemImage: "checkmark.seal")
                }
            }
            if invoice.isEditable {
                Button {
                    sheet = .edit(InvoiceDraft(editing: invoice))
                } label: {
                    Label("Edit", systemImage: "pencil")
                }
            }
            Button {
                sheet = .edit(InvoiceDraft(duplicating: invoice, settings: model.settings, today: InvoiceDates.today()))
            } label: {
                Label("Duplicate", systemImage: "plus.square.on.square")
            }
            if invoice.canReopen {
                Button {
                    Task { await run { try await model.reopen($0) } }
                } label: {
                    Label("Reopen", systemImage: "arrow.uturn.backward")
                }
            }
            if invoice.isDeletable {
                Button(role: .destructive) {
                    confirmDelete = true
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            } else if invoice.canVoid {
                Button(role: .destructive) {
                    confirmVoid = true
                } label: {
                    Label("Void", systemImage: "nosign")
                }
            }
        }
        .disabled(model.working)
    }

    private func historySection(_ invoice: Invoice) -> some View {
        Section("History") {
            if let events = invoice.events {
                if events.isEmpty {
                    Text("Nothing has happened to this invoice yet.").font(.subheadline).foregroundStyle(.secondary)
                }
                ForEach(events) { event in
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: event.symbol)
                            .foregroundStyle(event.kind == "failed" ? Color.red : Color.secondary)
                            .frame(width: 20)
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(alignment: .firstTextBaseline) {
                                Text(event.title).font(.subheadline)
                                Text(PackagesFormat.relative(event.at)).font(.caption).foregroundStyle(.secondary)
                            }
                            let detail = event.detail(currency: invoice.currency)
                            if !detail.isEmpty {
                                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                            }
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
            } else {
                ProgressView().frame(maxWidth: .infinity)
            }
        }
    }
}

enum InvoiceDetailSheet: Identifiable {
    case edit(InvoiceDraft)
    case send(Invoice, InvoiceSendDraft)
    case payment(Invoice, InvoicePaymentDraft)

    var id: String {
        switch self {
        case let .edit(draft): return "edit:" + draft.id
        case let .send(invoice, _): return "send:" + invoice.id
        case let .payment(invoice, _): return "payment:" + invoice.id
        }
    }
}
