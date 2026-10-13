import FMBase
import Foundation

extension PackagesClient {
    private func crmPath(_ organization: String, _ rest: String = "") throws -> String {
        "/api/crm/" + (try Self.segment(organization)) + rest
    }

    public func bulkUpdateCrmContacts(_ organization: String, edits: [JSONObject]) async throws -> (
        contacts: [CrmContact], missing: [String]
    ) {
        guard !edits.isEmpty else { return ([], []) }
        let reply = try await call(
            "POST", try crmPath(organization, "/contacts/bulk"), body: ["edits": .array(edits.map { .object($0) })])
        return (reply.maps("contacts").map(CrmContact.init(json:)), reply.strings("missing"))
    }

    public func mergeCrmContacts(_ organization: String, from duplicate: String, into kept: String) async throws -> CrmContact {
        let reply = try await call(
            "POST", try crmPath(organization, "/contacts/merge"),
            body: ["from": .string(try Self.item(duplicate)), "into": .string(try Self.item(kept))])
        return CrmContact(json: reply.obj("contact"))
    }

    public func deleteCrmColumn(_ organization: String, field: String) async throws {
        try await call("POST", try crmPath(organization, "/contacts/delete-column"), body: ["field": .string(field)])
    }

    public func exportCrmContacts(_ organization: String) async throws -> (filename: String, csv: String) {
        let reply = try await call("GET", try crmPath(organization, "/contacts/export"))
        return (reply.str("filename", "forkmesh-crm-\(organization).csv"), reply.str("csv"))
    }

    public func previewCrmImport(_ organization: String, csv: String) async throws -> CrmImportPreview {
        CrmImportPreview(
            json: try await call(
                "POST", try crmPath(organization, "/contacts/import"), body: ["preview": true, "csv": .string(csv)]))
    }

    public func importCrmContacts(_ organization: String, body: JSONObject) async throws -> JSONObject {
        try await call("POST", try crmPath(organization, "/contacts/import"), body: body)
    }

    public func crmEmail(_ organization: String, id: String) async throws -> CrmActivity {
        CrmActivity(json: try await call("GET", try crmPath(organization, "/emails/" + Self.item(id))).obj("email"))
    }

    public func sendCrmEmail(_ organization: String, draft: CrmEmailDraft) async throws -> CrmActivity {
        CrmActivity(json: try await call("POST", try crmPath(organization, "/emails/send"), body: draft.sendBody).obj("email"))
    }

    public func logCrmEmail(_ organization: String, body: JSONObject) async throws -> CrmActivity {
        CrmActivity(json: try await call("POST", try crmPath(organization, "/emails"), body: body).obj("email"))
    }

    public func updateCrmEmail(_ organization: String, id: String, status: String) async throws -> CrmActivity {
        CrmActivity(
            json: try await call(
                "PATCH", try crmPath(organization, "/emails/" + Self.item(id)), body: ["status": .string(status)]
            ).obj("email"))
    }

    public func deleteCrmEmail(_ organization: String, id: String) async throws {
        try await call("DELETE", try crmPath(organization, "/emails/" + Self.item(id)), body: [:])
    }

    public func moveCrmMail(_ organization: String, id: String, box: String) async throws -> CrmMailMessage {
        CrmMailMessage(
            json: try await call(
                "PATCH", try crmPath(organization, "/mail/" + Self.item(id)), body: ["box": .string(box)]
            ).obj("message"))
    }

    public func deleteCrmMail(_ organization: String, id: String) async throws {
        try await call("DELETE", try crmPath(organization, "/mail/" + Self.item(id)), body: [:])
    }

    public func importCrmMail(_ organization: String, limit: Int = 200) async throws -> JSONObject {
        try await call("POST", try crmPath(organization, "/mail/import"), body: ["limit": .int(Int64(limit))])
    }

    public func connectCrmZoho(_ organization: String) async throws -> URL? {
        URL(string: try await call("POST", try crmPath(organization, "/zoho/connect"), body: [:]).str("authorizationUrl"))
    }

    public func exchangeCrmZoho(_ organization: String, code: CrmZohoCode) async throws -> CrmSettings {
        CrmSettings(json: try await call("POST", try crmPath(organization, "/zoho/exchange"), body: code.body).obj("settings"))
    }

    public func saveCrmSettings(_ organization: String, body: JSONObject) async throws -> CrmSettings {
        CrmSettings(json: try await call("PUT", try crmPath(organization, "/settings"), body: body).obj("settings"))
    }

    public func crmSettingsFlows(_ organization: String) async throws -> [CrmFlow] {
        try await call("GET", try crmPath(organization, "/settings")).obj("settings").maps("flows").map(CrmFlow.init(json:))
    }

    public func saveCrmFlows(_ organization: String, flows: [CrmFlow]) async throws -> [CrmFlow] {
        try await call("PUT", try crmPath(organization, "/settings"), body: CrmFlowDraft.settingsBody(flows))
            .obj("settings").maps("flows").map(CrmFlow.init(json:))
    }

    public func createCrmView(_ organization: String, body: JSONObject) async throws -> CrmView {
        CrmView(json: try await call("POST", try crmPath(organization, "/views"), body: body).obj("view"))
    }

    public func updateCrmView(_ organization: String, id: String, body: JSONObject) async throws -> CrmView {
        CrmView(json: try await call("PATCH", try crmPath(organization, "/views/" + Self.item(id)), body: body).obj("view"))
    }

    public func deleteCrmView(_ organization: String, id: String) async throws {
        try await call("DELETE", try crmPath(organization, "/views/" + Self.item(id)), body: [:])
    }

    public func orderCrmViews(_ organization: String, order: [String]) async throws -> [CrmView] {
        try await call(
            "POST", try crmPath(organization, "/views/order"), body: ["order": .array(order.map { .string($0) })]
        ).maps("views").map(CrmView.init(json:))
    }

    public func crmListActivity(_ organization: String, viewId: String, before: Int64 = 0) async throws -> (
        entries: [CrmListActivityEntry], more: Bool
    ) {
        var query = [("view", try Self.item(viewId))]
        if before > 0 { query.append(("before", String(before))) }
        let reply = try await call("GET", try crmPath(organization, "/views/activity"), query: query)
        return (reply.maps("entries").map(CrmListActivityEntry.init(json:)), reply.bool("more"))
    }

    public func recordCrmListView(_ organization: String, viewId: String) async throws {
        try await call("POST", try crmPath(organization, "/views/activity"), body: ["view": .string(try Self.item(viewId))])
    }

    public func copyCrmSharedView(_ organization: String, viewId: String, name: String = "") async throws -> CrmImportResult {
        var body: JSONObject = ["viewId": .string(try Self.item(viewId))]
        if !name.isEmpty { body["name"] = .string(name) }
        return CrmImportResult(json: try await call("POST", try crmPath(organization, "/views/copy"), body: body))
    }

    public func crmSharedView(_ viewId: String) async throws -> (view: CrmView, contacts: [CrmContact]) {
        let reply = try await call("GET", "/api/crm/-/shared/" + Self.item(viewId))
        return (CrmView(json: reply.obj("view")), reply.maps("contacts").map(CrmContact.init(json:)))
    }

    public func updateCrmSharedContact(_ viewId: String, contactId: String, changes: JSONObject) async throws -> CrmContact {
        CrmContact(
            json: try await call(
                "PATCH", "/api/crm/-/shared/\(try Self.item(viewId))/contacts/\(try Self.item(contactId))", body: changes
            ).obj("contact"))
    }

    public func sendCrmOutreach(_ organization: String, draft: CrmEmailDraft) async throws -> Int {
        try await call("POST", try crmPath(organization, "/outreach/send"), body: draft.outreachBody).int("sentToday")
    }

    public func updateCrmOutreachTeam(_ organization: String, add: Bool, name: String) async throws -> [CrmOutreachMember] {
        try await call(
            "POST", try crmPath(organization, "/outreach/team"),
            body: ["action": .string(add ? "add" : "remove"), "name": .string(name.lowercased())]
        ).maps("members").map(CrmOutreachMember.init(json:))
    }

    public func createCrmDeckLink(_ organization: String, contactId: String) async throws -> CrmDeckLink {
        CrmDeckLink(
            json: try await call(
                "POST", try crmPath(organization, "/decks"), body: ["contactId": .string(try Self.item(contactId))]
            ).obj("link"))
    }

    public func crmDeckDetail(_ organization: String, token: String) async throws -> CrmDeckDetail {
        guard CrmText.isDeckToken(token) else { throw PackagesError("deck_link_not_found", status: 404) }
        return CrmDeckDetail(json: try await call("GET", try crmPath(organization, "/decks/" + token)).obj("deck"))
    }

    public func revokeCrmDeckLink(_ organization: String, token: String) async throws {
        guard CrmText.isDeckToken(token) else { throw PackagesError("deck_link_not_found", status: 404) }
        try await call("DELETE", try crmPath(organization, "/decks/" + token), body: [:])
    }
}

extension CrmText {
    public static func isDeckToken(_ value: String) -> Bool {
        (10...40).contains(value.count) && value.unicodeScalars.allSatisfy {
            ($0.value >= 97 && $0.value <= 122) || ($0.value >= 48 && $0.value <= 57)
        }
    }
}
