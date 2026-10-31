import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class InvoicesModel {
    public private(set) var organizations: [PackageOrganization] = []
    public private(set) var board: InvoiceBoard?
    public private(set) var contacts: [InvoiceContact]?
    public private(set) var loading = false
    public private(set) var working = false
    public private(set) var loaded = false
    public var organization: String
    public var errorText = ""
    public var notice = ""

    @ObservationIgnored public let client: PackagesClient

    public init(client: PackagesClient, organization: String = "") {
        self.client = client
        self.organization = organization
    }

    public var selected: PackageOrganization? { organizations.first { $0.name == organization } }

    public var needsInstall: Bool { selected.map { !$0.installed } ?? false }

    public var canManage: Bool { board?.canManage ?? selected?.canManage ?? false }

    public var crmInstalled: Bool { board?.crmInstalled ?? false }

    public var settings: InvoiceSettings { board?.settings ?? InvoiceSettings() }

    public var business: String { settings.businessName.isEmpty ? organization : settings.businessName }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            organizations = try await client.invoiceOrganizations()
            errorText = ""
        } catch {
            errorText = InvoiceText.describe(error)
            return
        }
        if organization.isEmpty || selected == nil {
            organization = organizations.first(where: \.installed)?.name ?? organizations.first?.name ?? ""
        }
        await load()
    }

    public func select(_ name: String) async {
        guard name != organization else { return }
        organization = name
        await organizationChanged()
    }

    public func organizationChanged() async {
        board = nil
        contacts = nil
        await load()
    }

    public func locate(_ invoiceId: String) async -> Invoice? {
        guard PackagesText.isHexId(invoiceId) else { return nil }
        if board != nil, let found = try? await detail(invoiceId) { return found }
        for org in organizations where org.installed && org.name != organization {
            if let found = try? await client.invoice(org.name, id: invoiceId) {
                organization = org.name
                await organizationChanged()
                return found
            }
        }
        return nil
    }

    public func load() async {
        guard PackagesText.isOrgName(organization), !needsInstall else {
            board = nil
            return
        }
        do {
            let next = try await client.invoices(organization)
            if board?.crmInstalled != next.crmInstalled { contacts = nil }
            board = next
            errorText = ""
        } catch let error as PackagesError where error.code == "plugin_not_installed" {
            markInstalled(false)
            board = nil
        } catch {
            errorText = InvoiceText.describe(error)
        }
    }

    private func markInstalled(_ installed: Bool) {
        guard let index = organizations.firstIndex(where: { $0.name == organization }) else { return }
        organizations[index].installed = installed
    }

    @discardableResult
    public func install() async -> Bool {
        guard canManage || selected?.canManage == true else { return false }
        working = true
        defer { working = false }
        do {
            _ = try await client.installInvoices(organization)
            markInstalled(true)
            notice = "Invoicing installed."
            errorText = ""
            await load()
            return true
        } catch {
            errorText = "Could not install: " + InvoiceText.describe(error)
            return false
        }
    }

    public func loadContacts() async -> [InvoiceContact] {
        guard crmInstalled else { return [] }
        if let contacts { return contacts }
        let fetched = (try? await client.invoiceContacts(organization)) ?? []
        contacts = fetched
        return fetched
    }

    public func detail(_ id: String) async throws -> Invoice {
        let invoice = try await client.invoice(organization, id: id)
        board?.replace(invoice)
        return invoice
    }

    public func newDraft(contact: InvoiceContact? = nil, today: Int64 = InvoiceDates.today()) -> InvoiceDraft {
        InvoiceDraft(settings: settings, today: today, contact: contact)
    }

    public func save(_ draft: InvoiceDraft) async throws -> Invoice {
        let saved = try await perform { try await self.client.saveInvoice(self.organization, draft, crmInstalled: self.crmInstalled) }
        notice = "Invoice \(saved.number) saved."
        return saved
    }

    public func send(_ invoice: Invoice, _ draft: InvoiceSendDraft) async throws -> Invoice {
        let sent = try await perform { try await self.client.sendInvoice(self.organization, id: invoice.id, draft) }
        notice = "Invoice \(sent.number) emailed to \(sent.sentTo.isEmpty ? draft.to : sent.sentTo)."
        return sent
    }

    public func recordPayment(_ invoice: Invoice, _ draft: InvoicePaymentDraft) async throws -> Invoice {
        let paid = try await perform { try await self.client.recordInvoicePayment(self.organization, id: invoice.id, draft) }
        notice = paid.status == "paid"
            ? "Invoice \(paid.number) is paid." : "Payment recorded; \(paid.money(paid.balance)) still due."
        return paid
    }

    public func void(_ invoice: Invoice) async throws -> Invoice {
        let voided = try await perform { try await self.client.voidInvoice(self.organization, id: invoice.id) }
        notice = "Invoice \(voided.number) is void."
        return voided
    }

    public func reopen(_ invoice: Invoice) async throws -> Invoice {
        let reopened = try await perform { try await self.client.reopenInvoice(self.organization, id: invoice.id) }
        notice = "Invoice \(reopened.number) reopened."
        return reopened
    }

    public func delete(_ invoice: Invoice) async throws {
        working = true
        defer { working = false }
        try await client.deleteInvoice(organization, id: invoice.id)
        board?.remove(invoice.id)
        notice = "Invoice \(invoice.number) deleted."
        await load()
    }

    public func saveSettings(_ settings: InvoiceSettings) async throws -> InvoiceSettings {
        working = true
        defer { working = false }
        let saved = try await client.saveInvoiceSettings(organization, settings)
        board?.settings = saved
        notice = "Invoicing settings saved."
        return saved
    }

    private func perform(_ action: @MainActor () async throws -> Invoice) async throws -> Invoice {
        working = true
        defer { working = false }
        let invoice = try await action()
        board?.replace(invoice)
        await load()
        return invoice
    }
}
