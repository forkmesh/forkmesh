import FMBase
import FMLocalAI
import FMPackages
import Foundation

extension ForkMeshToolbox {
    public nonisolated static let crmTextFields = [
        "name", "email", "company", "title", "phone", "website", "location", "linkedin", "github", "twitter",
        "mastodon", "bluesky", "flow", "stage", "nextStep", "owner", "source",
    ]

    func crmClient() throws -> PackagesClient {
        guard let client = sources.crm() else { throw AssistantError("the CRM is not available in this build") }
        return client
    }

    func crmOrganization(_ args: JSONObject) async throws -> String {
        let client = try crmClient()
        if crmOrgCache == nil {
            crmOrgCache = try await client.crmOrganizations().filter(\.installed).map(\.name)
        }
        let installed = crmOrgCache ?? []
        if let named = AssistantText.argument(args, "organization") {
            return installed.first { $0.lowercased() == named.lowercased() } ?? named
        }
        guard let first = installed.first else {
            throw AssistantError("none of the user's organizations has the CRM package installed")
        }
        return first
    }

    func crmData(_ organization: String) async throws -> CrmOrganizationData {
        guard let data = try await crmClient().crm(organization) else {
            throw AssistantError("the CRM answered without data; try again")
        }
        return data
    }

    func findContact(_ data: CrmOrganizationData, _ key: String) throws -> CrmContact {
        let lower = key.lowercased()
        if let hit = data.contacts.first(where: { $0.id == lower || $0.email.lowercased() == lower || $0.name.lowercased() == lower }) {
            return hit
        }
        let partial = data.contacts.filter { $0.matches(key) }
        if partial.count == 1 { return partial[0] }
        if partial.isEmpty { throw AssistantError("no contact in \(data.organization) matches \"\(key)\"") }
        let names = partial.prefix(8).map { "[\($0.id)] \($0.displayName)" }.joined(separator: ", ")
        throw AssistantError("\"\(key)\" matches \(partial.count) contacts: \(names); use an id")
    }

    func contactKey(_ args: JSONObject) -> String? {
        AssistantText.argument(args, "id") ?? AssistantText.argument(args, "contact")
    }

    func crmContacts(_ args: JSONObject) async throws -> String {
        let organization = try await crmOrganization(args)
        let data = try await crmData(organization)
        if let key = contactKey(args) {
            let contact = try findContact(data, key)
            return AssistantText.crmContactDetail(contact, activity: data.activity(for: contact.id), timeZone)
        }
        var rows = data.contacts
        if let flow = AssistantText.argument(args, "flow")?.lowercased() {
            rows = rows.filter { $0.flow.lowercased() == flow || data.flow($0.flow).name.lowercased() == flow }
        }
        if let stage = AssistantText.argument(args, "stage")?.lowercased() { rows = rows.filter { $0.stage.lowercased() == stage } }
        if let tag = AssistantText.argument(args, "tag")?.lowercased() { rows = rows.filter { $0.tags.contains { $0.lowercased() == tag } } }
        if let query = AssistantText.argument(args, "query") { rows = rows.filter { $0.matches(query) || AssistantText.matches(query, [$0.notes, $0.location, $0.source]) } }
        rows.sort { $0.updatedAt > $1.updatedAt }
        var lines = ["CRM of \(organization): \(data.counts.contacts) contacts, \(data.counts.customers) customers, \(data.counts.sent) emails sent."]
        lines += data.flows.map { flow in
            let stages = flow.stages.map { stage in "\(stage) \(data.contacts.filter { $0.flow == flow.id && $0.stage == stage }.count)" }
            return "Pipeline \(flow.id) (\(flow.name)): " + stages.joined(separator: " → ")
        }
        if !data.members.isEmpty { lines.append("Members: " + data.members.joined(separator: ", ")) }
        lines.append(AssistantText.list("Contacts: \(rows.count)", rows.map(AssistantText.crmContactLine)))
        return lines.joined(separator: "\n")
    }

    func crmFields(_ args: JSONObject) -> JSONObject {
        var fields = JSONObject()
        for key in ForkMeshToolbox.crmTextFields {
            if let value = AssistantText.argument(args, key) { fields[key] = .string(value) }
        }
        if let value = args["value"], let number = value.int64Value ?? Int64(value.text.filter(\.isNumber)) {
            fields["value"] = .int(max(0, number))
        }
        if let tags = args["tags"], !tags.isNull {
            let list = tags.arrayValue.map { $0.map(\.text) } ?? tags.text.split(separator: ",").map(String.init)
            fields["tags"] = JSONValue(list.map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty })
        }
        return fields
    }

    func noteLine(_ note: String) -> String {
        "[\(AssistantText.date(Int64(now().timeIntervalSince1970 * 1000), timeZone)) \(sources.account()?.username ?? "assistant")] \(note)"
    }

    func createCrmContact(_ args: JSONObject) async throws -> String {
        let organization = try await crmOrganization(args)
        var fields = crmFields(args)
        if fields["name"] == nil && fields["email"] == nil { throw AssistantError("a new contact needs a name or an email") }
        if let note = AssistantText.argument(args, "notes") ?? AssistantText.argument(args, "append_note") {
            fields["notes"] = .string(noteLine(note))
        }
        let contact = try await crmClient().createCrmContact(organization, fields: fields)
        return "Added to the \(organization) CRM: " + AssistantText.crmContactLine(contact)
    }

    func updateCrmContact(_ args: JSONObject) async throws -> String {
        guard let key = contactKey(args) else { throw AssistantError("update_crm_contact needs the contact's id, name or email") }
        let organization = try await crmOrganization(args)
        let contact = try findContact(try await crmData(organization), key)
        var changes = crmFields(args)
        if let note = AssistantText.argument(args, "append_note") {
            let existing = contact.notes.trimmingCharacters(in: .whitespacesAndNewlines)
            changes["notes"] = .string((existing.isEmpty ? "" : existing + "\n\n") + noteLine(note))
        }
        if args["mark_contacted"]?.isTrue == true {
            changes["lastContactedAt"] = .int(Int64(now().timeIntervalSince1970 * 1000))
        }
        if changes.isEmpty { return "Nothing to change: pass the fields to set, or append_note." }
        let updated = try await crmClient().updateCrmContact(CrmContactRef(organization: organization, contactId: contact.id), changes: changes)
        return "Updated \(updated.displayName) (\(changes.keys.joined(separator: ", "))): " + AssistantText.crmContactLine(updated)
    }

    func deleteApproval(_ args: JSONObject) async -> String {
        let key = contactKey(args) ?? "(no contact given)"
        guard let organization = try? await crmOrganization(args),
              let data = try? await crmData(organization),
              let contact = try? findContact(data, key)
        else { return "Delete CRM contact \(key)" }
        return "Delete \(contact.displayName)\(contact.email.isEmpty ? "" : " <\(contact.email)>") from the \(organization) CRM"
    }

    func deleteCrmContact(_ args: JSONObject) async throws -> String {
        guard let key = contactKey(args) else { throw AssistantError("delete_crm_contact needs the contact's id, name or email") }
        let organization = try await crmOrganization(args)
        let contact = try findContact(try await crmData(organization), key)
        try await crmClient().deleteCrmContact(CrmContactRef(organization: organization, contactId: contact.id))
        return "Deleted \(contact.displayName) from the \(organization) CRM."
    }

    func draftEmail(_ args: JSONObject) async throws -> OnDeviceToolResult {
        guard let subject = AssistantText.argument(args, "subject"), let body = AssistantText.argument(args, "body") else {
            throw AssistantError("draft_email needs a subject and a body")
        }
        var to = AssistantText.argument(args, "to") ?? ""
        var name = ""
        if let key = contactKey(args), sources.crm() != nil {
            let organization = try await crmOrganization(args)
            let contact = try findContact(try await crmData(organization), key)
            name = contact.displayName
            if to.isEmpty { to = contact.email }
        }
        if to.isEmpty { throw AssistantError("no email address: pass to, or a CRM contact that has one") }
        let draft = OnDeviceEmailDraft(to: to, recipientName: name, subject: subject, body: body)
        let text = "Draft shown to the user with Open in Mail and Copy buttons. It has not been sent.\n" + draft.plainText
        return OnDeviceToolResult(text, email: draft)
    }
}
