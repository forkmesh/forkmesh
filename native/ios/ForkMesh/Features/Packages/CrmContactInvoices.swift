import FMApi
import FMBase
import FMPackages
import SwiftUI

struct CrmContactInvoicesSection: View {
    let client: PackagesClient
    let ref: CrmContactRef
    let contact: CrmContact
    @Binding var state: ContactInvoicesState
    @State private var model: InvoicesModel? = nil
    @State private var editing: InvoiceDraftTarget? = nil
    @State private var installing = false

    var body: some View {
        Group {
            switch state {
            case .loading, .hidden:
                EmptyView()
            case let .notInstalled(canInstall):
                Section("Invoices") {
                    Text(
                        canInstall
                            ? "Install Invoicing for \(ref.organization) to bill this contact and see when invoices are opened and paid."
                            : "Invoicing is not installed for \(ref.organization). Ask an owner or admin to install it from the Market."
                    )
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    if canInstall {
                        Button {
                            Task { await install() }
                        } label: {
                            Label(installing ? "Installing…" : "Install Invoicing", systemImage: "receipt")
                        }
                        .disabled(installing)
                    }
                }
            case let .failed(text):
                Section("Invoices") {
                    PackagesErrorBanner(text: text) { await load() }
                }
            case let .loaded(data):
                loadedSection(data)
            }
        }
        .sheet(item: $editing) { target in
            if let model {
                NavigationStack {
                    InvoiceEditor(model: model, draft: target.draft) { _ in
                        Task { await load() }
                    }
                }
            }
        }
    }

    @ViewBuilder private func loadedSection(_ data: ContactInvoices) -> some View {
        Section {
            if !data.invoices.isEmpty {
                let fallback = data.invoices.first?.currency ?? "USD"
                LabeledContent("Billed", value: InvoiceMoney.map(data.billed, fallback: fallback))
                LabeledContent("Paid", value: InvoiceMoney.map(data.paid, fallback: fallback))
                LabeledContent("Outstanding", value: InvoiceMoney.map(data.outstanding, fallback: fallback))
            } else {
                Text("No invoices for this contact yet.").font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(data.invoices) { invoice in
                NavigationLink(value: Route.invoices(organization: ref.organization, invoice: invoice.id)) {
                    InvoiceRow(invoice: invoice)
                }
            }
            Button {
                Task { await startInvoice() }
            } label: {
                Label("New invoice", systemImage: "plus")
            }
            .accessibilityIdentifier("crm-contact-new-invoice")
        } header: {
            Text("Invoices")
        }
    }

    static func fetch(client: PackagesClient, ref: CrmContactRef) async -> ContactInvoicesState {
        do {
            return .loaded(try await client.contactInvoices(ref))
        } catch {
            return ContactInvoicesState.from(error)
        }
    }

    private func load() async {
        state = await Self.fetch(client: client, ref: ref)
    }

    private func install() async {
        installing = true
        defer { installing = false }
        do {
            _ = try await client.installInvoices(ref.organization)
            FmHaptics.commit()
            await load()
        } catch {
            FmHaptics.failure()
            state = .failed("Could not install: " + InvoiceText.describe(error))
        }
    }

    private func startInvoice() async {
        let invoices = model ?? InvoicesModel(client: client, organization: ref.organization)
        model = invoices
        if invoices.board == nil { await invoices.load() }
        guard invoices.board != nil else {
            state = .failed(invoices.errorText.isEmpty ? "Invoicing is unavailable." : invoices.errorText)
            return
        }
        let seed = InvoiceContact(id: contact.id, name: contact.name, email: contact.email, company: contact.company)
        editing = InvoiceDraftTarget(draft: invoices.newDraft(contact: seed))
    }
}
