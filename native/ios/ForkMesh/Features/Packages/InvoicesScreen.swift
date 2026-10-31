import FMApi
import FMBase
import FMPackages
import SwiftUI

struct InvoicesDestination: View {
    let organization: String?
    let invoice: String?

    var body: some View {
        PackagesGate(feature: "Invoicing") { client in
            InvoicesScreen(client: client, organization: organization, invoice: invoice)
        }
    }
}

struct InvoiceOpen: Identifiable, Hashable {
    let id: String
}

struct InvoicesScreen: View {
    @State private var model: InvoicesModel
    @State private var query = ""
    @State private var filter: InvoiceFilter = .all
    @State private var opened: InvoiceOpen?
    @State private var editing: InvoiceDraftTarget?
    @State private var showSettings = false
    @State private var notice: FmNotice?
    @State private var pendingInvoice: String?

    init(client: PackagesClient, organization: String?, invoice: String?) {
        _model = State(initialValue: InvoicesModel(client: client, organization: organization ?? ""))
        _pendingInvoice = State(initialValue: invoice)
    }

    var body: some View {
        List {
            Section {
                OrganizationPicker(organizations: model.organizations, selection: $model.organization) {
                    await model.organizationChanged()
                }
                PackagesErrorBanner(text: model.errorText) { await model.refresh() }
                if notice != nil { FmNoticeLine($notice) }
            }
            if let board = model.board {
                InvoiceSummarySection(summary: board.summary, currency: board.settings.currency)
                invoicesSection(board)
                activitySection(board)
            } else if model.loaded && !model.loading {
                if model.organizations.isEmpty {
                    PackageNotInstalledView(package: "Invoicing", organizations: model.organizations)
                } else if model.needsInstall {
                    InvoicesInstallPrompt(model: model)
                }
            }
        }
        .navigationTitle("Invoices")
        .searchable(text: $query, prompt: "Number, client or email")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    editing = InvoiceDraftTarget(draft: model.newDraft())
                } label: {
                    Label("New invoice", systemImage: "plus")
                }
                .disabled(model.board == nil)
                .accessibilityIdentifier("invoices-new")
            }
            ToolbarItem(placement: .secondaryAction) {
                Button("Invoicing settings", systemImage: "gearshape") { showSettings = true }
                    .disabled(model.board == nil)
            }
        }
        .overlay {
            if model.loading && model.board == nil { ProgressView() }
        }
        .fmRefreshable { await model.load() }
        .task {
            await model.refresh()
            await openPending()
        }
        .navigationDestination(item: $opened) { target in
            InvoiceDetailScreen(model: model, invoiceId: target.id)
        }
        .sheet(item: $editing) { target in
            NavigationStack {
                InvoiceEditor(model: model, draft: target.draft) { saved in
                    opened = InvoiceOpen(id: saved.id)
                }
            }
        }
        .sheet(isPresented: $showSettings) {
            NavigationStack {
                InvoiceSettingsSheet(model: model)
            }
        }
        .onChange(of: model.notice) { _, text in
            guard !text.isEmpty, opened == nil else { return }
            notice = FmNotice(text)
            model.notice = ""
        }
    }

    private func openPending() async {
        guard let id = pendingInvoice else { return }
        pendingInvoice = nil
        if model.board?.invoice(id) != nil {
            opened = InvoiceOpen(id: id)
        } else if await model.locate(id) != nil {
            opened = InvoiceOpen(id: id)
        } else if model.errorText.isEmpty {
            model.errorText = "That invoice was removed or you no longer have access."
        }
    }

    @ViewBuilder private func invoicesSection(_ board: InvoiceBoard) -> some View {
        let items = board.filtered(query: query, filter: filter)
        Section {
            if board.invoices.isEmpty {
                ContentUnavailableView {
                    Label("No invoices yet", systemImage: "receipt")
                } description: {
                    Text("Create an invoice, email it with a view-and-pay link, and see when it is opened and paid.")
                } actions: {
                    Button("New invoice") { editing = InvoiceDraftTarget(draft: model.newDraft()) }
                        .buttonStyle(.borderedProminent)
                        .tint(.green)
                }
            } else if items.isEmpty {
                Text("No invoices match.").foregroundStyle(.secondary)
            }
            ForEach(items) { invoice in
                Button {
                    opened = InvoiceOpen(id: invoice.id)
                } label: {
                    InvoiceRow(invoice: invoice)
                }
                .buttonStyle(.plain)
            }
        } header: {
            HStack {
                Text("Invoices")
                Spacer()
                Picker("Status", selection: $filter) {
                    ForEach(InvoiceFilter.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .textCase(nil)
                .accessibilityIdentifier("invoices-filter")
            }
        }
    }

    @ViewBuilder private func activitySection(_ board: InvoiceBoard) -> some View {
        Section("Recent activity") {
            if board.activity.isEmpty {
                Text("Sends, opens, views and payments show here.").font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(board.activity.prefix(15)) { event in
                Button {
                    if PackagesText.isHexId(event.invoiceId) { opened = InvoiceOpen(id: event.invoiceId) }
                } label: {
                    InvoiceActivityRow(event: event)
                }
                .buttonStyle(.plain)
            }
        }
    }
}

struct InvoiceDraftTarget: Identifiable {
    let id = UUID()
    let draft: InvoiceDraft
}

struct InvoicesInstallPrompt: View {
    let model: InvoicesModel

    var body: some View {
        Section {
            ContentUnavailableView {
                Label("Invoicing is not installed for \(model.organization)", systemImage: "receipt")
            } description: {
                Text(
                    model.canManage
                        ? "Install it to create invoices for CRM contacts, email them, and see when each one is opened, viewed and paid."
                        : "Ask an owner or admin of \(model.organization) to install Invoicing.")
            } actions: {
                if model.canManage {
                    Button {
                        Task {
                            if await model.install() { FmHaptics.commit() } else { FmHaptics.failure() }
                        }
                    } label: {
                        if model.working { ProgressView() } else { Text("Install for \(model.organization)") }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.working)
                    .accessibilityIdentifier("invoices-install")
                }
            }
        }
    }
}

struct InvoiceSummarySection: View {
    let summary: InvoiceSummary
    let currency: String

    var body: some View {
        Section {
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], spacing: 8) {
                InvoiceSummaryTile(
                    value: InvoiceMoney.map(summary.outstanding, fallback: currency), label: "Outstanding",
                    detail: summary.outstandingDetail, icon: "tray.full", tint: .blue)
                InvoiceSummaryTile(
                    value: InvoiceMoney.map(summary.overdueAmount, fallback: currency), label: "Overdue",
                    detail: summary.overdueDetail, icon: "clock.badge.exclamationmark",
                    tint: summary.counts.overdue > 0 ? .red : .secondary)
                InvoiceSummaryTile(
                    value: InvoiceMoney.map(summary.paidLast30Days, fallback: currency), label: "Paid, last 30 days",
                    detail: summary.paidDetail, icon: "checkmark.seal", tint: .green)
                InvoiceSummaryTile(
                    value: summary.openRateText, label: "Open rate", detail: summary.engagementText,
                    icon: "envelope.open", tint: .indigo)
                InvoiceSummaryTile(
                    value: summary.daysToPayText, label: "Average days to pay", detail: summary.daysToPayDetail,
                    icon: "calendar.badge.clock", tint: .teal)
            }
            .listRowInsets(EdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0))
            .listRowBackground(Color.clear)
        }
    }
}

struct InvoiceSummaryTile: View {
    let value: String
    let label: String
    let detail: String
    let icon: String
    let tint: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Label(label, systemImage: icon).font(.caption).foregroundStyle(tint).lineLimit(1)
            Text(value).font(.headline.monospacedDigit()).lineLimit(2).minimumScaleFactor(0.6)
            Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
        }
        .frame(maxWidth: .infinity, minHeight: 78, alignment: .topLeading)
        .padding(10)
        .shellGlass(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

struct InvoiceStatusBadge: View {
    let state: String

    var tint: Color {
        switch state {
        case "sent": return .accentColor
        case "partial": return .orange
        case "overdue": return .red
        case "paid": return .green
        default: return .secondary
        }
    }

    var body: some View {
        Text(InvoiceText.statusTitle(state))
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .foregroundStyle(tint)
            .background(tint.opacity(0.12), in: Capsule())
    }
}

struct InvoiceEngagement: View {
    let invoice: Invoice

    var body: some View {
        if invoice.isSent {
            HStack(spacing: 10) {
                Label("\(invoice.opens)", systemImage: "envelope.open").accessibilityLabel("\(invoice.opens) email opens")
                Label("\(invoice.views)", systemImage: "eye").accessibilityLabel("\(invoice.views) page views")
                Label("\(invoice.clicks)", systemImage: "cursorarrow.click").accessibilityLabel("\(invoice.clicks) pay link clicks")
            }
            .labelStyle(.titleAndIcon)
            .font(.caption2.monospacedDigit())
            .foregroundStyle(.secondary)
        } else {
            Text("Not sent").font(.caption2).foregroundStyle(.secondary)
        }
    }
}

struct InvoiceRow: View {
    let invoice: Invoice

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(invoice.number).font(.body.weight(.semibold))
                InvoiceStatusBadge(state: invoice.state)
                Spacer()
                Text(invoice.money(invoice.total)).font(.body.weight(.semibold).monospacedDigit())
            }
            HStack(alignment: .firstTextBaseline) {
                Text(invoice.client.label + (invoice.contactId.isEmpty ? "" : " · CRM"))
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                if invoice.balance > 0 && invoice.balance != invoice.total {
                    Text("\(invoice.money(invoice.balance)) due").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }
            }
            HStack {
                Text("Due \(InvoiceDates.label(invoice.dueAt))").font(.caption2).foregroundStyle(.secondary)
                Spacer()
                InvoiceEngagement(invoice: invoice)
            }
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}

struct InvoiceActivityRow: View {
    let event: InvoiceEvent

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: event.symbol).foregroundStyle(.secondary).frame(width: 20)
            VStack(alignment: .leading, spacing: 1) {
                Text(event.number.isEmpty ? event.title : "\(event.title) · \(event.number)").font(.subheadline)
                let detail = [event.activityDetail(), PackagesFormat.relative(event.at)].filter { !$0.isEmpty }
                    .joined(separator: " · ")
                if !detail.isEmpty {
                    Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}
