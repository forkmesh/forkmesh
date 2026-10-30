import FMBase
import Foundation

extension PackagesClient {
    public static let invoicesPackageId = "invoices"

    private func invoicesPath(_ organization: String, _ rest: String = "") throws -> String {
        "/api/invoices/" + (try Self.segment(organization)) + rest
    }

    private func invoiceItemPath(_ organization: String, _ id: String, _ action: String = "") throws -> String {
        try invoicesPath(organization, "/items/" + Self.item(id) + (action.isEmpty ? "" : "/" + action))
    }

    @discardableResult
    func invoicesCall(_ method: String, _ path: String, body: JSONObject? = nil) async throws -> JSONObject {
        let reply = try await transport.send(method, path, query: [], body: body)
        if reply.ok && reply.json.optBool("ok") != false { return reply.json }
        var error = PackagesError.from(reply)
        if error.code == "plugin_not_installed" {
            error.fields["canInstall"] = reply.json.bool("canInstall") ? "true" : "false"
        }
        if error.code.isEmpty && reply.ok { error.code = "send_failed" }
        throw error
    }

    public func invoiceOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await invoicesCall("GET", "/api/invoices"))
    }

    public func invoices(_ organization: String) async throws -> InvoiceBoard {
        let reply = try await invoicesCall("GET", try invoicesPath(organization))
        return await Task.detached(priority: .userInitiated) { InvoiceBoard(json: reply) }.value
    }

    public func invoice(_ organization: String, id: String) async throws -> Invoice {
        Invoice(json: try await invoicesCall("GET", try invoiceItemPath(organization, id)).obj("invoice"))
    }

    public func saveInvoice(_ organization: String, _ draft: InvoiceDraft, crmInstalled: Bool) async throws -> Invoice {
        let body = draft.body(crmInstalled: crmInstalled)
        if draft.isNew {
            return Invoice(json: try await invoicesCall("POST", try invoicesPath(organization, "/items"), body: body).obj("invoice"))
        }
        return Invoice(json: try await invoicesCall("PATCH", try invoiceItemPath(organization, draft.id), body: body).obj("invoice"))
    }

    public func deleteInvoice(_ organization: String, id: String) async throws {
        try await invoicesCall("DELETE", try invoiceItemPath(organization, id), body: [:])
    }

    public func sendInvoice(_ organization: String, id: String, _ draft: InvoiceSendDraft) async throws -> Invoice {
        Invoice(json: try await invoicesCall("POST", try invoiceItemPath(organization, id, "send"), body: draft.body).obj("invoice"))
    }

    public func recordInvoicePayment(_ organization: String, id: String, _ draft: InvoicePaymentDraft) async throws -> Invoice {
        Invoice(json: try await invoicesCall("POST", try invoiceItemPath(organization, id, "paid"), body: draft.body).obj("invoice"))
    }

    public func voidInvoice(_ organization: String, id: String) async throws -> Invoice {
        Invoice(json: try await invoicesCall("POST", try invoiceItemPath(organization, id, "void"), body: [:]).obj("invoice"))
    }

    public func reopenInvoice(_ organization: String, id: String) async throws -> Invoice {
        Invoice(json: try await invoicesCall("POST", try invoiceItemPath(organization, id, "reopen"), body: [:]).obj("invoice"))
    }

    public func invoiceSettings(_ organization: String) async throws -> InvoiceSettings {
        InvoiceSettings(json: try await invoicesCall("GET", try invoicesPath(organization, "/settings")).obj("settings"))
    }

    public func saveInvoiceSettings(_ organization: String, _ settings: InvoiceSettings) async throws -> InvoiceSettings {
        InvoiceSettings(
            json: try await invoicesCall("PATCH", try invoicesPath(organization, "/settings"), body: settings.body).obj("settings"))
    }

    public func invoiceContacts(_ organization: String) async throws -> [InvoiceContact] {
        try await invoicesCall("GET", try invoicesPath(organization, "/contacts")).maps("contacts")
            .map(InvoiceContact.init(json:)).filter { PackagesText.isHexId($0.id) }
    }

    public func contactInvoices(_ ref: CrmContactRef) async throws -> ContactInvoices {
        ContactInvoices(
            json: try await invoicesCall(
                "GET", try invoicesPath(ref.organization, "/contacts/" + Self.item(ref.contactId))))
    }

    public func installInvoices(_ organization: String) async throws -> OrgPackageState {
        try await install(Self.invoicesPackageId, organization: organization)
    }
}
