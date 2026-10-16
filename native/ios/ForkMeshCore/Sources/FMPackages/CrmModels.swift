import FMBase
import Foundation

public struct PackageOrganization: Equatable, Sendable, Identifiable {
    public var name: String
    public var role: String
    public var installed: Bool

    public var id: String { name }

    public init(name: String, role: String = "member", installed: Bool = false) {
        self.name = name
        self.role = role
        self.installed = installed
    }

    public init(json: JSONObject) {
        self.init(name: json.str("name"), role: json.str("role", "member"), installed: json.bool("installed"))
    }

    public var canManage: Bool { role == "owner" || role == "admin" }

    public static func list(_ json: JSONObject) -> [PackageOrganization] {
        json.maps("organizations").map(PackageOrganization.init(json:)).filter { PackagesText.isOrgName($0.name) }
    }
}

public struct CrmFlow: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var icon: String
    public var stages: [String]

    public init(id: String, name: String, icon: String = "pipeline", stages: [String]) {
        self.id = id
        self.name = name
        self.icon = icon
        self.stages = stages
    }

    public init(json: JSONObject) {
        let stages = json.strings("stages").filter { !$0.isEmpty }
        self.init(
            id: json.str("id"), name: json.str("name", json.str("id")), icon: json.str("icon", "pipeline"),
            stages: stages.isEmpty ? ["lead"] : stages)
    }

    public static let fallback = CrmFlow(
        id: "customers", name: "Customers", stages: ["lead", "qualified", "proposal", "customer"])
}

public struct CrmContact: Equatable, Sendable, Identifiable {
    public var id: String
    public var organization: String
    public var name: String
    public var email: String
    public var company: String
    public var title: String
    public var phone: String
    public var website: String
    public var location: String
    public var linkedin: String
    public var github: String
    public var twitter: String
    public var mastodon: String
    public var bluesky: String
    public var flow: String
    public var stage: String
    public var value: Int64
    public var nextStep: String
    public var tags: [String]
    public var owner: String
    public var source: String
    public var notes: String
    public var lastContactedAt: Int64
    public var fields: [String: String]
    public var views: [String]
    public var createdBy: String
    public var updatedBy: String
    public var createdAt: Int64
    public var updatedAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        organization = json.str("organization")
        name = json.str("name")
        email = json.str("email")
        company = json.str("company")
        title = json.str("title")
        phone = json.str("phone")
        website = json.str("website")
        location = json.str("location")
        linkedin = json.str("linkedin")
        github = json.str("github")
        twitter = json.str("twitter")
        mastodon = json.str("mastodon")
        bluesky = json.str("bluesky")
        flow = json.str("flow", "customers")
        stage = json.str("stage", "lead")
        value = max(0, json.int64("value"))
        nextStep = json.str("nextStep")
        tags = json.strings("tags")
        owner = json.str("owner")
        source = json.str("source")
        notes = json.str("notes")
        lastContactedAt = json.int64("lastContactedAt")
        var custom: [String: String] = [:]
        for (key, value) in json.obj("fields") where !value.text.isEmpty { custom[key] = value.text }
        fields = custom
        views = json.strings("views").filter(PackagesText.isHexId)
        createdBy = json.str("createdBy")
        updatedBy = json.str("updatedBy")
        createdAt = json.int64("createdAt")
        updatedAt = json.int64("updatedAt")
    }

    public var displayName: String {
        if !name.isEmpty { return name }
        if !email.isEmpty { return email }
        return "Unnamed contact"
    }

    public var subtitle: String {
        [title, company].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public var initials: String {
        let words = displayName.split(whereSeparator: { $0 == " " || $0 == "." || $0 == "@" }).prefix(2)
        let letters = words.compactMap(\.first).map { String($0).uppercased() }.joined()
        return letters.isEmpty ? "?" : letters
    }

    public func value(of field: String) -> String {
        switch field {
        case "name": return name
        case "email": return email
        case "company": return company
        case "title": return title
        case "phone": return phone
        case "twitter": return twitter
        case "mastodon": return mastodon
        case "bluesky": return bluesky
        case "linkedin": return linkedin
        case "github": return github
        case "website": return website
        case "location": return location
        case "flow": return flow
        case "stage": return CrmStageLabel.title(stage)
        case "value": return value > 0 ? String(value) : ""
        case "nextStep": return nextStep
        case "owner": return owner
        case "source": return source
        case "tags": return tags.joined(separator: ", ")
        case "notes": return notes
        case "lastContactedAt": return lastContactedAt > 0 ? String(lastContactedAt) : ""
        default: return fields[field] ?? ""
        }
    }

    public var routeId: String { CrmContactRef(organization: organization, contactId: id).routeId }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(
            query, name, email, company, title, tags.joined(separator: " "), nextStep, owner, location, phone,
            fields.values.joined(separator: " "))
    }

    public func moved(flow: String? = nil, stage: String? = nil, views: [String]? = nil, owner: String? = nil) -> CrmContact {
        var copy = self
        if let flow { copy.flow = flow }
        if let stage { copy.stage = stage }
        if let views { copy.views = views }
        if let owner { copy.owner = owner }
        return copy
    }
}

public struct CrmContactEdit: Equatable, Sendable {
    public var name: String
    public var email: String
    public var company: String
    public var title: String
    public var phone: String
    public var website: String
    public var location: String
    public var linkedin: String
    public var github: String
    public var twitter: String
    public var mastodon: String
    public var bluesky: String
    public var owner: String
    public var source: String
    public var flow: String
    public var stage: String
    public var value: Int64
    public var nextStep: String
    public var tags: String
    public var notes: String
    public var fields: [String: String]
    public var lastContactedAt: Int64 = 0

    public static let textKeys = [
        "name", "email", "company", "title", "phone", "website", "location", "linkedin", "github", "twitter",
        "mastodon", "bluesky", "owner", "source", "flow", "stage", "nextStep", "notes",
    ]

    public init(contact: CrmContact) {
        name = contact.name
        email = contact.email
        company = contact.company
        title = contact.title
        phone = contact.phone
        website = contact.website
        location = contact.location
        linkedin = contact.linkedin
        github = contact.github
        twitter = contact.twitter
        mastodon = contact.mastodon
        bluesky = contact.bluesky
        owner = contact.owner
        source = contact.source
        flow = contact.flow
        stage = contact.stage
        value = contact.value
        nextStep = contact.nextStep
        tags = contact.tags.joined(separator: ", ")
        notes = contact.notes
        fields = contact.fields
        lastContactedAt = contact.lastContactedAt
    }

    public init() {
        name = ""
        email = ""
        company = ""
        title = ""
        phone = ""
        website = ""
        location = ""
        linkedin = ""
        github = ""
        twitter = ""
        mastodon = ""
        bluesky = ""
        owner = ""
        source = ""
        flow = CrmFlow.fallback.id
        stage = "lead"
        value = 0
        nextStep = ""
        tags = ""
        notes = ""
        fields = [:]
    }

    public var tagList: [String] {
        var seen = Set<String>()
        return tags.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter {
            !$0.isEmpty && seen.insert($0.lowercased()).inserted
        }
    }

    public var isValid: Bool {
        !name.trimmingCharacters(in: .whitespaces).isEmpty || !email.trimmingCharacters(in: .whitespaces).isEmpty
    }

    public func text(_ key: String) -> String {
        switch key {
        case "name": return name
        case "email": return email
        case "company": return company
        case "title": return title
        case "phone": return phone
        case "website": return website
        case "location": return location
        case "linkedin": return linkedin
        case "github": return github
        case "twitter": return twitter
        case "mastodon": return mastodon
        case "bluesky": return bluesky
        case "owner": return owner
        case "source": return source
        case "flow": return flow
        case "stage": return stage
        case "nextStep": return nextStep
        case "notes": return notes
        default: return ""
        }
    }

    public func changes(from original: CrmContact?) -> JSONObject {
        var body: JSONObject = [:]
        let before = original.map(CrmContactEdit.init(contact:))
        for key in Self.textKeys {
            let trimmed = text(key).trimmingCharacters(in: .whitespacesAndNewlines)
            if before == nil || trimmed != before?.text(key) { body[key] = .string(trimmed) }
        }
        if original == nil || value != original?.value { body["value"] = .int(max(0, value)) }
        if lastContactedAt != (original?.lastContactedAt ?? 0) { body["lastContactedAt"] = .int(max(0, lastContactedAt)) }
        if original == nil || tagList != original?.tags { body["tags"] = .array(tagList.map { .string($0) }) }
        var custom: JSONObject = [:]
        let old = original?.fields ?? [:]
        for key in Set(fields.keys).union(old.keys).sorted() {
            let next = (fields[key] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if next != (old[key] ?? "") { custom[key] = .string(next) }
        }
        if !custom.isEmpty { body["fields"] = .object(custom) }
        return body
    }
}

public struct CrmActivity: Equatable, Sendable, Identifiable {
    public var id: String
    public var contactId: String
    public var sender: String
    public var transport: String
    public var status: String
    public var to: String
    public var subject: String
    public var provider: String
    public var direction: String
    public var error: String
    public var createdAt: Int64
    public var sentAt: Int64
    public var statusAt: Int64
    public var externalId: String
    public var fromEmail: String
    public var fromName: String
    public var providerUrl: String
    public var threadId: String
    public var mailbox: String
    public var handle: String
    public var text: String
    public var html: String

    public init(json: JSONObject) {
        id = json.str("id")
        contactId = json.str("contactId")
        sender = json.str("sender")
        transport = json.str("transport", "external")
        status = json.str("status", "external")
        to = json.str("to")
        subject = json.str("subject")
        provider = json.str("provider")
        direction = json.str("direction")
        error = json.str("error")
        createdAt = json.int64("createdAt")
        sentAt = json.int64("sentAt")
        statusAt = json.int64("statusAt")
        externalId = json.str("externalId")
        fromEmail = json.str("fromEmail")
        fromName = json.str("fromName")
        providerUrl = json.str("providerUrl")
        threadId = json.str("threadId")
        mailbox = json.str("mailbox")
        handle = json.str("handle")
        text = json.str("text")
        html = json.str("html")
    }

    public var at: Int64 { sentAt > 0 ? sentAt : createdAt }

    public static let statuses = [
        "draft", "external", "sending", "accepted", "delivery", "delivered", "open", "click", "failed", "reject",
        "bounce", "soft bounce", "suspension", "unsubscribe", "spam",
    ]

    public var engaged: Bool { status == "open" || status == "click" }

    public var delivered: Bool { ["accepted", "delivery", "delivered", "open", "click"].contains(status) }

    public var plainBody: String {
        if !text.isEmpty { return text }
        return CrmText.strippingTags(html)
    }

    public var failed: Bool {
        ["failed", "reject", "bounce", "soft bounce", "suspension"].contains(status)
    }

    public var incoming: Bool { direction == "inbound" || direction == "received" }
}

public struct CrmSummaryCounts: Equatable, Sendable {
    public var contacts: Int
    public var customers: Int
    public var sent: Int
    public var failed: Int
    public var mail: Int
}

public struct CrmOrganizationData: Equatable, Sendable {
    public var organization: String
    public var role: String
    public var canManageSettings: Bool
    public var contacts: [CrmContact]
    public var activity: [CrmActivity]
    public var flows: [CrmFlow]
    public var members: [String]
    public var memberEmails: [String]
    public var counts: CrmSummaryCounts
    public var revision: String
    public var mail: [CrmMailMessage]
    public var settings: CrmSettings
    public var outreach: CrmOutreach
    public var views: [CrmView]
    public var sharedViews: [CrmView]
    public var team: CrmTeamActivity
    public var decks: [CrmDeckLink]

    public init(json: JSONObject) {
        organization = json.str("organization")
        role = json.str("role", "member")
        canManageSettings = json.bool("canManageSettings")
        contacts = json.maps("contacts").map(CrmContact.init(json:)).filter { PackagesText.isHexId($0.id) }
        activity = json.maps("emails").map(CrmActivity.init(json:)).sorted { $0.at > $1.at }
        let flows = json.obj("settings").maps("flows").map(CrmFlow.init(json:)).filter { !$0.id.isEmpty }
        self.flows = flows.isEmpty ? [CrmFlow.fallback] : flows
        members = json.list("members").map { value -> String in
            if let object = value.objectValue { return object.str("name") }
            return value.text
        }.filter { !$0.isEmpty }
        memberEmails = json.strings("memberEmails").filter { $0.contains("@") }
        let summary = json.obj("summary")
        counts = CrmSummaryCounts(
            contacts: summary.int("contacts", contacts.count), customers: summary.int("customers"),
            sent: summary.int("sent"), failed: summary.int("failed"), mail: summary.int("mail"))
        revision = json.str("revision")
        mail = json.maps("mail").map(CrmMailMessage.init(json:)).filter { PackagesText.isHexId($0.id) }
        settings = CrmSettings(json: json.obj("settings"))
        outreach = CrmOutreach(json: json.obj("outreach"))
        views = json.maps("views").map(CrmView.init(json:)).filter { PackagesText.isHexId($0.id) }
        sharedViews = json.maps("sharedViews").map(CrmView.init(json:)).filter { PackagesText.isHexId($0.id) }
        team = CrmTeamActivity(json: json.obj("activity"))
        decks = json.maps("decks").map(CrmDeckLink.init(json:)).filter { !$0.token.isEmpty }
    }

    public var manages: Bool { canManageSettings }

    public func flow(_ id: String?) -> CrmFlow {
        flows.first { $0.id == id } ?? flows.first ?? CrmFlow.fallback
    }

    public func contacts(in flow: CrmFlow, query: String = "") -> [CrmContact] {
        contacts.filter { $0.flow == flow.id && $0.matches(query) }
    }

    public func stageColumns(_ flow: CrmFlow, query: String = "") -> [(stage: String, contacts: [CrmContact])] {
        let rows = contacts(in: flow, query: query)
        var columns = flow.stages.map { stage in (stage: stage, contacts: rows.filter { $0.stage == stage }) }
        let known = Set(flow.stages)
        let stray = rows.filter { !known.contains($0.stage) }
        if !stray.isEmpty { columns.append((stage: "other", contacts: stray)) }
        return columns
    }

    public func activity(for contactId: String) -> [CrmActivity] {
        activity.filter { $0.contactId == contactId }
    }

    public func stageValue(_ flow: CrmFlow, stage: String) -> Int64 {
        contacts.filter { $0.flow == flow.id && $0.stage == stage }.reduce(0) { $0 + $1.value }
    }

    public mutating func replace(_ contact: CrmContact) {
        if let index = contacts.firstIndex(where: { $0.id == contact.id }) {
            contacts[index] = contact
        } else {
            contacts.insert(contact, at: 0)
        }
    }

    public mutating func remove(_ contactId: String) {
        contacts.removeAll { $0.id == contactId }
        activity.removeAll { $0.contactId == contactId }
    }

    public mutating func replace(_ view: CrmView) {
        if let index = views.firstIndex(where: { $0.id == view.id }) {
            views[index] = view
        } else {
            views.append(view)
        }
    }

    public mutating func replace(_ message: CrmMailMessage) {
        if let index = mail.firstIndex(where: { $0.id == message.id }) { mail[index] = message }
    }

    public mutating func replace(_ link: CrmDeckLink) {
        if let index = decks.firstIndex(where: { $0.token == link.token }) {
            decks[index] = link
        } else {
            decks.insert(link, at: 0)
        }
    }

    public func decks(for contactId: String) -> [CrmDeckLink] {
        decks.filter { $0.contactId == contactId }
    }

    public func contact(id: String) -> CrmContact? { contacts.first { $0.id == id } }

    public func contact(email: String) -> CrmContact? {
        let needle = email.trimmingCharacters(in: .whitespaces).lowercased()
        guard !needle.isEmpty else { return nil }
        return contacts.first { $0.email.lowercased() == needle }
    }

    public var pipelineValue: Int64 { contacts.reduce(0) { $0 + $1.value } }

    public var tags: [String] {
        var seen = Set<String>()
        return contacts.flatMap(\.tags).filter { seen.insert($0).inserted }.sorted()
    }

    public var owners: [String] {
        var seen = Set<String>()
        return (members + contacts.map(\.owner)).filter { !$0.isEmpty && seen.insert($0.lowercased()).inserted }
            .sorted()
    }

    public var listViews: [CrmView] { views.filter { $0.flow.isEmpty } }

    public func customFieldKeys() -> [String] {
        var keys: [String] = []
        var seen = Set<String>()
        for view in views {
            for column in view.columns where column.custom && seen.insert(column.field).inserted {
                keys.append(column.field)
            }
        }
        for contact in contacts {
            for key in contact.fields.keys.sorted() where seen.insert(key).inserted { keys.append(key) }
        }
        return keys
    }

    public func duplicates(of contact: CrmContact) -> [CrmContact] {
        let email = contact.email.lowercased()
        let name = contact.name.lowercased().trimmingCharacters(in: .whitespaces)
        let domain = CrmText.domain(of: contact.email)
        return contacts.filter { other in
            guard other.id != contact.id else { return false }
            if !email.isEmpty && other.email.lowercased() == email { return true }
            if !name.isEmpty && other.name.lowercased().trimmingCharacters(in: .whitespaces) == name { return true }
            return !domain.isEmpty && CrmText.domain(of: other.email) == domain && !name.isEmpty
                && other.name.lowercased().split(separator: " ").last == name.split(separator: " ").last
        }
    }
}

public struct CrmContactRef: Equatable, Sendable {
    public var organization: String
    public var contactId: String

    public init(organization: String, contactId: String) {
        self.organization = organization
        self.contactId = contactId
    }

    public init?(routeId: String) {
        let parts = routeId.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 2, PackagesText.isOrgName(parts[0]), PackagesText.isHexId(parts[1]) else { return nil }
        self.init(organization: parts[0], contactId: parts[1])
    }

    public var routeId: String { organization + "/" + contactId }
}

public enum CrmStageLabel {
    public static func title(_ stage: String) -> String {
        stage.split(separator: "-").map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: " ")
    }
}
