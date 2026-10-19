import FMBase
import Foundation

public enum CrmText {
    public static func strippingTags(_ html: String) -> String {
        guard !html.isEmpty else { return "" }
        var out = ""
        var tag: String?
        let blocks: Set<String> = ["p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "blockquote", "ul", "ol", "table"]
        for character in html {
            if character == "<" {
                tag = ""
            } else if character == ">", let name = tag {
                let bare = name.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "/ "))
                let word = String(bare.prefix { $0.isLetter || $0.isNumber })
                out.append(blocks.contains(word) ? "\n" : " ")
                tag = nil
            } else if tag != nil {
                tag?.append(character)
            } else {
                out.append(character)
            }
        }
        let entities = [("&nbsp;", " "), ("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"), ("&quot;", "\""), ("&#39;", "'")]
        for (entity, plain) in entities { out = out.replacingOccurrences(of: entity, with: plain) }
        return out.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }.joined(separator: "\n")
    }

    public static func domain(of email: String) -> String {
        guard let at = email.lastIndex(of: "@") else { return "" }
        return String(email[email.index(after: at)...]).lowercased()
    }

    public static func slug(_ value: String) -> String {
        var out = ""
        var dash = false
        for scalar in value.lowercased().unicodeScalars {
            let isAlnum = (scalar.value >= 97 && scalar.value <= 122) || (scalar.value >= 48 && scalar.value <= 57)
            if isAlnum {
                out.unicodeScalars.append(scalar)
                dash = false
            } else if !dash && !out.isEmpty {
                out.append("-")
                dash = true
            }
        }
        while out.hasSuffix("-") { out.removeLast() }
        out = String(out.prefix(40))
        while out.hasSuffix("-") { out.removeLast() }
        guard let first = out.unicodeScalars.first, first.value >= 97 && first.value <= 122 else { return "" }
        return out
    }

    public static func profileURL(_ field: String, _ value: String) -> URL? {
        let raw = value.trimmingCharacters(in: .whitespaces)
        guard !raw.isEmpty else { return nil }
        if raw.lowercased().hasPrefix("https://") || raw.lowercased().hasPrefix("http://") { return URL(string: raw) }
        let handle = raw.hasPrefix("@") ? String(raw.dropFirst()) : raw
        switch field {
        case "website": return URL(string: "https://" + raw)
        case "linkedin":
            return raw.contains("linkedin.com") ? URL(string: "https://" + raw) : URL(string: "https://www.linkedin.com/in/" + handle)
        case "github": return URL(string: "https://github.com/" + handle)
        case "twitter": return URL(string: "https://x.com/" + handle)
        case "bluesky": return URL(string: "https://bsky.app/profile/" + handle)
        case "mastodon":
            let parts = handle.split(separator: "@")
            guard parts.count == 2 else { return nil }
            return URL(string: "https://\(parts[1])/@\(parts[0])")
        default: return nil
        }
    }

    public static func money(_ value: Int64) -> String {
        if value >= 1_000_000 { return String(format: "%.1fM", Double(value) / 1_000_000).replacingOccurrences(of: ".0M", with: "M") }
        if value >= 10_000 { return "\(value / 1000)k" }
        if value >= 1000 { return String(format: "%.1fk", Double(value) / 1000).replacingOccurrences(of: ".0k", with: "k") }
        return String(value)
    }

    public static func duration(_ seconds: Int) -> String {
        if seconds < 60 { return "\(seconds)s" }
        if seconds < 3600 { return "\(seconds / 60)m \(seconds % 60)s" }
        return "\(seconds / 3600)h \((seconds % 3600) / 60)m"
    }
}

public enum CrmColumnCatalog {
    public static let builtIn: [(field: String, label: String)] = [
        ("name", "Name"), ("email", "Email"), ("company", "Company"), ("title", "Title"), ("phone", "Phone"),
        ("twitter", "Twitter"), ("mastodon", "Mastodon"), ("bluesky", "Bluesky"), ("linkedin", "LinkedIn"),
        ("github", "GitHub"), ("website", "Website"), ("location", "Location"), ("flow", "Flow"), ("stage", "Stage"),
        ("value", "Deal value"), ("nextStep", "Next step"), ("owner", "Owner"), ("source", "Source"),
        ("tags", "Tags"), ("notes", "Notes"), ("lastContactedAt", "Last contacted"),
    ]

    public static let deletable: Set<String> = Set(builtIn.map(\.field)).subtracting([
        "name", "email", "flow", "stage", "lastContactedAt",
    ])

    public static func isBuiltIn(_ field: String) -> Bool { builtIn.contains { $0.field == field } }

    public static func editorColumns(_ columns: [CrmColumn], removed: [String]) -> [CrmColumn] {
        var result = columns.filter { !removed.contains($0.field) || $0.custom }
        let known = Set(result.map(\.field))
        for entry in builtIn where !known.contains(entry.field) && !removed.contains(entry.field) {
            result.append(CrmColumn(field: entry.field, label: entry.label, custom: false, hidden: true))
        }
        return result
    }

    public static func label(_ field: String) -> String {
        if let known = builtIn.first(where: { $0.field == field }) { return known.label }
        return field.split(separator: "_").map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: " ")
    }

    public static let importTargets: [(field: String, label: String)] =
        builtIn + [("first_name", "First name"), ("last_name", "Last name"), ("custom", "Its own column"), ("", "Skip")]
}

public struct CrmColumn: Equatable, Sendable, Identifiable {
    public var field: String
    public var label: String
    public var custom: Bool
    public var hidden: Bool

    public var id: String { field }

    public init(field: String, label: String, custom: Bool, hidden: Bool) {
        self.field = field
        self.label = label
        self.custom = custom
        self.hidden = hidden
    }

    public init(json: JSONObject) {
        let field = json.str("field")
        self.init(
            field: field, label: json.str("label", CrmColumnCatalog.label(field)),
            custom: json.optBool("custom") ?? !CrmColumnCatalog.isBuiltIn(field), hidden: json.bool("hidden"))
    }

    public var json: JSONValue {
        ["field": .string(field), "label": .string(label), "custom": .bool(custom), "hidden": .bool(hidden)]
    }
}

public struct CrmView: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var columns: [CrmColumn]
    public var visibility: String
    public var sharedWith: [String]
    public var shareAccess: String
    public var position: Int
    public var source: String
    public var fileName: String
    public var flow: String
    public var owner: String
    public var isOwner: Bool
    public var canManage: Bool
    public var canEdit: Bool
    public var organization: String
    public var stages: [String]
    public var shared: Bool
    public var createdAt: Int64
    public var updatedAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name", "Untitled sheet")
        columns = json.maps("columns").map(CrmColumn.init(json:)).filter { !$0.field.isEmpty }
        visibility = json.str("visibility", "organization")
        sharedWith = json.strings("sharedWith")
        shareAccess = json.str("shareAccess", "view")
        position = json.int("position")
        source = json.str("source", "manual")
        fileName = json.str("fileName")
        flow = json.str("flow")
        owner = json.str("owner")
        isOwner = json.bool("isOwner")
        canManage = json.bool("canManage")
        canEdit = json.bool("canEdit")
        organization = json.str("organization")
        stages = json.strings("stages")
        shared = json.bool("shared")
        createdAt = json.int64("createdAt")
        updatedAt = json.int64("updatedAt")
    }

    public var isPipeline: Bool { !flow.isEmpty }

    public var isPrivate: Bool { visibility == "private" }

    public var visibleColumns: [CrmColumn] { columns.filter { !$0.hidden } }

    public var summaryColumns: [CrmColumn] {
        let shown = Set(["name", "email", "company", "title", "flow", "stage", "value", "nextStep", "tags", "notes"])
        return visibleColumns.filter { !shown.contains($0.field) }
    }
}

public enum CrmTab: Hashable, Sendable, Identifiable {
    case list(String)
    case pipeline(String)
    case noPipeline
    case activity
    case mail
    case outreach
    case team
    case decks
    case shared

    public var id: String {
        switch self {
        case .list(let id): return "list:" + id
        case .pipeline(let id): return "pipeline:" + id
        case .noPipeline: return "no-pipeline"
        case .activity: return "activity"
        case .mail: return "mail"
        case .outreach: return "outreach"
        case .team: return "team"
        case .decks: return "decks"
        case .shared: return "shared"
        }
    }

    public var isContacts: Bool {
        switch self {
        case .list, .pipeline, .noPipeline: return true
        default: return false
        }
    }
}

public struct CrmTabItem: Equatable, Sendable, Identifiable {
    public var tab: CrmTab
    public var title: String
    public var icon: String
    public var count: Int?
    public var view: CrmView?
    public var flow: CrmFlow?

    public var id: String { tab.id }
}

extension CrmOrganizationData {
    public var pipelineLists: [String: [String]] {
        var lists: [String: [String]] = [:]
        for view in views where view.isPipeline { lists[view.flow, default: []].append(view.id) }
        return lists
    }

    public func contains(_ contact: CrmContact, in view: CrmView) -> Bool {
        guard view.isPipeline else { return contact.views.contains(view.id) }
        guard contact.flow == view.flow else { return false }
        let lists = pipelineLists[view.flow] ?? [view.id]
        let mine = lists.first { contact.views.contains($0) }
        return (mine ?? lists.first) == view.id
    }

    public var livePipelineIds: Set<String> { Set(flows.map(\.id)) }

    public var strayContacts: [CrmContact] {
        let live = livePipelineIds
        return contacts.filter { !live.contains($0.flow) }
    }

    public func contacts(for tab: CrmTab) -> [CrmContact] {
        switch tab {
        case .list(let id):
            guard let view = views.first(where: { $0.id == id }) else { return [] }
            return contacts.filter { contains($0, in: view) }
        case .pipeline(let id): return contacts.filter { $0.flow == id }
        case .noPipeline: return strayContacts
        default: return []
        }
    }

    public func flow(for tab: CrmTab) -> CrmFlow? {
        switch tab {
        case .list(let id):
            guard let view = views.first(where: { $0.id == id }), view.isPipeline else { return nil }
            return flows.first { $0.id == view.flow }
        case .pipeline(let id): return flows.first { $0.id == id }
        default: return nil
        }
    }

    public func view(for tab: CrmTab) -> CrmView? {
        guard case .list(let id) = tab else { return nil }
        return views.first { $0.id == id }
    }

    public var contactTabs: [CrmTabItem] {
        var items: [CrmTabItem] = []
        let flowIds = livePipelineIds
        for view in views where !view.isPipeline || flowIds.contains(view.flow) {
            let flow = view.isPipeline ? flows.first { $0.id == view.flow } : nil
            let tab = CrmTab.list(view.id)
            items.append(
                CrmTabItem(
                    tab: tab, title: view.name, icon: flow.map { CrmFlowIcon.symbol($0.icon) } ?? (view.isPrivate ? "lock" : "list.bullet"),
                    count: contacts(for: tab).count, view: view, flow: flow))
        }
        let bound = Set(views.filter(\.isPipeline).map(\.flow))
        for flow in flows where !bound.contains(flow.id) {
            let tab = CrmTab.pipeline(flow.id)
            items.append(
                CrmTabItem(tab: tab, title: flow.name, icon: CrmFlowIcon.symbol(flow.icon), count: contacts(for: tab).count, flow: flow))
        }
        let stray = strayContacts.count
        if stray > 0 {
            items.append(CrmTabItem(tab: .noPipeline, title: "No pipeline", icon: "questionmark.folder", count: stray))
        }
        return items
    }

    public var workspaceTabs: [CrmTabItem] {
        var items = [
            CrmTabItem(tab: .activity, title: "Activity", icon: "clock.arrow.circlepath", count: nil),
            CrmTabItem(tab: .mail, title: "Mail", icon: "tray.full", count: mail.isEmpty ? nil : mail.count),
            CrmTabItem(tab: .outreach, title: "Outreach", icon: "paperplane", count: nil),
            CrmTabItem(tab: .team, title: "Team", icon: "chart.bar.xaxis", count: nil),
            CrmTabItem(tab: .decks, title: "Decks", icon: "rectangle.on.rectangle.angled", count: decks.isEmpty ? nil : decks.count),
        ]
        if !sharedViews.isEmpty {
            items.append(CrmTabItem(tab: .shared, title: "Shared with me", icon: "person.2", count: sharedViews.count))
        }
        return items
    }

    public var defaultTab: CrmTab { contactTabs.first?.tab ?? .activity }

    public func tab(forFlow flowId: String) -> CrmTab? {
        contactTabs.first { $0.flow?.id == flowId }?.tab
    }
}

public enum CrmFlowIcon {
    public static let all = ["pipeline", "investors", "hiring", "partners", "customers", "sales", "support", "projects", "events"]

    public static func symbol(_ icon: String) -> String {
        switch icon {
        case "investors": return "banknote"
        case "hiring": return "person.badge.plus"
        case "partners": return "hands.sparkles"
        case "customers": return "person.2"
        case "sales": return "cart"
        case "support": return "lifepreserver"
        case "projects": return "folder"
        case "events": return "calendar"
        default: return "line.3.horizontal.decrease"
        }
    }

    public static func title(_ icon: String) -> String {
        icon == "pipeline" ? "Funnel" : CrmStageLabel.title(icon)
    }
}

public enum CrmSort: String, CaseIterable, Sendable, Identifiable {
    case stage
    case name
    case value
    case updated
    case lastContacted
    case created

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .stage: return "Stage"
        case .name: return "Name"
        case .value: return "Deal value"
        case .updated: return "Recently updated"
        case .lastContacted: return "Least recently contacted"
        case .created: return "Newest"
        }
    }
}

public struct CrmFilter: Equatable, Sendable {
    public var query: String = ""
    public var stage: String = ""
    public var owner: String = ""
    public var tag: String = ""
    public var sort: CrmSort = .stage

    public init() {}

    public var isActive: Bool { !stage.isEmpty || !owner.isEmpty || !tag.isEmpty }

    public func apply(_ contacts: [CrmContact], flow: CrmFlow?) -> [CrmContact] {
        let rows = contacts.filter { contact in
            contact.matches(query) && (stage.isEmpty || contact.stage == stage)
                && (owner.isEmpty || contact.owner.lowercased() == owner.lowercased())
                && (tag.isEmpty || contact.tags.contains(tag))
        }
        switch sort {
        case .stage:
            let order = Dictionary(uniqueKeysWithValues: (flow?.stages ?? []).enumerated().map { ($1, $0) })
            return rows.sorted {
                let left = order[$0.stage] ?? Int.max
                let right = order[$1.stage] ?? Int.max
                return left == right ? $0.updatedAt > $1.updatedAt : left < right
            }
        case .name: return rows.sorted { $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending }
        case .value: return rows.sorted { $0.value == $1.value ? $0.updatedAt > $1.updatedAt : $0.value > $1.value }
        case .updated: return rows.sorted { $0.updatedAt > $1.updatedAt }
        case .lastContacted: return rows.sorted { $0.lastContactedAt == $1.lastContactedAt ? $0.updatedAt > $1.updatedAt : $0.lastContactedAt < $1.lastContactedAt }
        case .created: return rows.sorted { $0.createdAt > $1.createdAt }
        }
    }
}

public struct CrmStageStat: Equatable, Sendable, Identifiable {
    public var stage: String
    public var count: Int
    public var value: Int64

    public var id: String { stage }
}

extension CrmFlow {
    public func stats(_ contacts: [CrmContact]) -> [CrmStageStat] {
        var stats = stages.map { stage in
            let rows = contacts.filter { $0.stage == stage }
            return CrmStageStat(stage: stage, count: rows.count, value: rows.reduce(0) { $0 + $1.value })
        }
        let known = Set(stages)
        let stray = contacts.filter { !known.contains($0.stage) }
        if !stray.isEmpty {
            stats.append(CrmStageStat(stage: "other", count: stray.count, value: stray.reduce(0) { $0 + $1.value }))
        }
        return stats
    }

    public func next(after stage: String) -> String? {
        guard let index = stages.firstIndex(of: stage), index + 1 < stages.count else { return nil }
        return stages[index + 1]
    }

    public func previous(before stage: String) -> String? {
        guard let index = stages.firstIndex(of: stage), index > 0 else { return nil }
        return stages[index - 1]
    }

    public func progress(of stage: String) -> Double {
        guard let index = stages.firstIndex(of: stage), stages.count > 1 else { return 0 }
        return Double(index) / Double(stages.count - 1)
    }

    public var json: JSONValue {
        ["id": .string(id), "name": .string(name), "icon": .string(icon), "stages": .array(stages.map { .string($0) })]
    }
}

public struct CrmFlowDraft: Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var icon: String
    public var stages: [String]
    public var isNew: Bool

    public init(flow: CrmFlow) {
        id = flow.id
        name = flow.name
        icon = flow.icon
        stages = flow.stages.map(CrmStageLabel.title)
        isNew = false
    }

    public init(name: String = "") {
        id = ""
        self.name = name
        icon = "pipeline"
        stages = ["Lead", "Qualified", "Won"]
        isNew = true
    }

    public var flow: CrmFlow? {
        let slug = isNew ? CrmText.slug(name) : id
        guard !slug.isEmpty else { return nil }
        var seen = Set<String>()
        let slugs = stages.map(CrmText.slug).filter { !$0.isEmpty && seen.insert($0).inserted }
        let title = name.trimmingCharacters(in: .whitespaces)
        return CrmFlow(id: slug, name: title.isEmpty ? CrmStageLabel.title(slug) : title, icon: icon, stages: slugs.isEmpty ? ["lead"] : slugs)
    }

    public static func settingsBody(_ flows: [CrmFlow]) -> JSONObject {
        ["flows": .array(flows.prefix(12).map(\.json))]
    }
}

public struct CrmMailMessage: Equatable, Sendable, Identifiable {
    public var id: String
    public var provider: String
    public var box: String
    public var groupKey: String
    public var subject: String
    public var summary: String
    public var sender: String
    public var from: String
    public var to: String
    public var flag: String
    public var hasAttachment: Bool
    public var providerUrl: String
    public var receivedAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        provider = json.str("provider", "zoho")
        box = json.str("box", "Inbox")
        groupKey = json.str("groupKey")
        subject = json.str("subject")
        summary = json.str("summary")
        sender = json.str("sender")
        from = json.str("from")
        to = json.str("to")
        flag = json.str("flag")
        hasAttachment = json.bool("hasAttachment")
        providerUrl = json.str("providerUrl")
        receivedAt = json.int64("receivedAt")
    }

    public var who: String { sender.isEmpty ? from : sender }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(query, subject, summary, sender, from, to)
    }
}

public struct CrmZohoStep: Equatable, Sendable, Identifiable {
    public var key: String
    public var action: String
    public var text: String

    public var id: String { key }

    public init(json: JSONObject) {
        key = json.str("key")
        action = json.str("action")
        text = json.str("text")
    }
}

public struct CrmZohoStatus: Equatable, Sendable {
    public var connected: Bool
    public var region: String
    public var accountId: String
    public var clientId: String
    public var hasClientSecret: Bool
    public var lastSyncAt: Int64
    public var oneClick: Bool
    public var managed: Bool
    public var canSend: Bool
    public var lastSentAt: Int64
    public var lastSyncError: String
    public var steps: [CrmZohoStep]

    public init(json: JSONObject) {
        connected = json.bool("connected")
        region = json.str("region", "com")
        accountId = json.str("accountId")
        clientId = json.str("clientId")
        hasClientSecret = json.bool("hasClientSecret")
        lastSyncAt = json.int64("lastSyncAt")
        oneClick = json.bool("oneClick")
        managed = json.bool("managed")
        canSend = json.optBool("canSend") ?? connected
        lastSentAt = json.int64("lastSentAt")
        lastSyncError = json.str("lastSyncError")
        steps = json.maps("steps").map(CrmZohoStep.init(json:)).filter { !$0.text.isEmpty }
    }
}

public struct CrmSettings: Equatable, Sendable {
    public var providerName: String
    public var providerUrlTemplate: String
    public var fromName: String
    public var fromEmail: String
    public var mailProvider: String
    public var mailBoxes: [String]
    public var zoho: CrmZohoStatus
    public var mailtrapConfigured: Bool
    public var updatedBy: String
    public var updatedAt: Int64
    public var removedColumns: [String] = []

    public static let defaultBoxes = ["Inbox", "Follow-up", "Important", "Archive"]

    public init(json: JSONObject) {
        providerName = json.str("providerName")
        providerUrlTemplate = json.str("providerUrlTemplate")
        fromName = json.str("fromName")
        fromEmail = json.str("fromEmail")
        mailProvider = json.str("mailProvider", "zoho")
        let boxes = json.list("mailBoxes").map { value -> String in
            if let object = value.objectValue { return object.str("name") }
            return value.text
        }.filter { !$0.isEmpty }
        mailBoxes = boxes.isEmpty ? Self.defaultBoxes : boxes
        zoho = CrmZohoStatus(json: json.obj("zoho"))
        mailtrapConfigured = json.bool("mailtrapConfigured")
        updatedBy = json.str("updatedBy")
        updatedAt = json.int64("updatedAt")
        removedColumns = json.strings("removedColumns")
    }

    public func composeURL(to email: String, subject: String = "", body: String = "") -> URL? {
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: "&=+?")
        let to = email.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let su = subject.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let bo = body.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        if mailProvider == "gmail" {
            return URL(string: "https://mail.google.com/mail/?view=cm&fs=1&to=\(to)&su=\(su)&body=\(bo)")
        }
        return Self.zohoComposeURL(region: zoho.region, to: email, subject: subject, body: body)
    }

    public static let zohoMailHosts: [String: String] = [
        "com": "https://mail.zoho.com", "eu": "https://mail.zoho.eu", "in": "https://mail.zoho.in",
        "com.au": "https://mail.zoho.com.au", "jp": "https://mail.zoho.jp", "ca": "https://mail.zohocloud.ca",
    ]

    public static func zohoComposeURL(
        region: String, to email: String, subject: String = "", body: String = "", cc: [String] = []
    ) -> URL? {
        guard let mailto = mailtoURL(to: email, subject: subject, body: body, cc: cc) else { return nil }
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        let ct = mailto.absoluteString.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let host = zohoMailHosts[region] ?? zohoMailHosts["com"] ?? ""
        return URL(string: "\(host)/zm/comp.do?ct=\(ct)")
    }

    public static func mailtoURL(to email: String, subject: String = "", body: String = "", cc: [String] = []) -> URL? {
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: "&=+?")
        let su = subject.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let bo = body.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let to = email.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
        let copies = cc.filter { $0.caseInsensitiveCompare(email) != .orderedSame }
            .compactMap { $0.addingPercentEncoding(withAllowedCharacters: allowed) }
        let carbon = copies.isEmpty ? "" : "cc=\(copies.joined(separator: ","))&"
        return URL(string: "mailto:\(to)?\(carbon)subject=\(su)&body=\(bo)")
    }
}

public struct CrmSettingsEdit: Equatable, Sendable {
    public var fromName: String
    public var fromEmail: String
    public var mailProvider: String
    public var providerName: String
    public var providerUrlTemplate: String
    public var mailBoxes: String

    public init(settings: CrmSettings) {
        fromName = settings.fromName
        fromEmail = settings.fromEmail
        mailProvider = settings.mailProvider
        providerName = settings.providerName
        providerUrlTemplate = settings.providerUrlTemplate
        mailBoxes = settings.mailBoxes.joined(separator: ", ")
    }

    public var body: JSONObject {
        var seen = Set<String>()
        let boxes = mailBoxes.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty && seen.insert($0.lowercased()).inserted }
        return [
            "fromName": .string(fromName.trimmingCharacters(in: .whitespaces)),
            "fromEmail": .string(fromEmail.trimmingCharacters(in: .whitespaces)),
            "mailProvider": .string(mailProvider),
            "providerName": .string(providerName.trimmingCharacters(in: .whitespaces)),
            "providerUrlTemplate": .string(providerUrlTemplate.trimmingCharacters(in: .whitespaces)),
            "mailBoxes": .array((boxes.isEmpty ? CrmSettings.defaultBoxes : boxes).map { .string($0) }),
        ]
    }
}

public struct CrmOutreachTemplate: Equatable, Sendable, Identifiable {
    public var key: String
    public var label: String
    public var subject: String
    public var body: String

    public var id: String { key }

    public init(json: JSONObject) {
        key = json.str("key")
        label = json.str("label", json.str("key"))
        subject = json.str("subject")
        body = json.str("body")
    }

    public func filled(for contact: CrmContact?) -> (subject: String, body: String) {
        guard let contact else { return (subject, body) }
        let first = contact.name.split(separator: " ").first.map(String.init) ?? ""
        func fill(_ text: String) -> String {
            text.replacingOccurrences(of: "{name}", with: contact.name)
                .replacingOccurrences(of: "{firstName}", with: first)
                .replacingOccurrences(of: "{company}", with: contact.company)
        }
        return (fill(subject), fill(body))
    }
}

public struct CrmOutreachSend: Equatable, Sendable, Identifiable {
    public var ts: Int64
    public var sender: String
    public var ok: Bool
    public var to: String
    public var subject: String
    public var template: String
    public var error: String

    public var id: String { "\(ts)-\(to)-\(sender)" }

    public init(json: JSONObject) {
        ts = json.int64("ts")
        sender = json.str("sender")
        ok = json.bool("ok")
        to = json.str("to")
        subject = json.str("subject")
        template = json.str("template")
        error = json.str("error")
    }
}

public struct CrmOutreachMember: Equatable, Sendable, Identifiable {
    public var name: String
    public var addedBy: String
    public var addedAt: Int64

    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        addedBy = json.str("addedBy")
        addedAt = json.int64("addedAt")
    }
}

public struct CrmOutreach: Equatable, Sendable {
    public var allowed: Bool
    public var canManage: Bool
    public var configured: Bool
    public var fromEmail: String
    public var fromName: String
    public var dailyLimit: Int
    public var sentToday: Int
    public var templates: [CrmOutreachTemplate]
    public var recent: [CrmOutreachSend]
    public var members: [CrmOutreachMember]

    public init(json: JSONObject) {
        allowed = json.bool("allowed")
        canManage = json.bool("canManage")
        configured = json.bool("configured")
        fromEmail = json.str("fromEmail")
        fromName = json.str("fromName")
        dailyLimit = json.int("dailyLimit", 50)
        sentToday = json.int("sentToday")
        templates = json.maps("templates").map(CrmOutreachTemplate.init(json:)).filter { !$0.key.isEmpty }
        recent = json.maps("recent").map(CrmOutreachSend.init(json:))
        members = json.maps("members").map(CrmOutreachMember.init(json:)).filter { !$0.name.isEmpty }
    }

    public var remaining: Int { max(0, dailyLimit - sentToday) }
}

public struct CrmDeckLink: Equatable, Sendable, Identifiable {
    public var token: String
    public var url: String
    public var contactId: String
    public var deck: String
    public var createdAt: Int64
    public var createdBy: String
    public var revokedAt: Int64
    public var active: Bool
    public var views: Int
    public var seconds: Int
    public var lastViewedAt: Int64
    public var pagesViewed: Int
    public var pageCount: Int

    public var id: String { token }

    public init(json: JSONObject) {
        token = json.str("token")
        url = json.str("url")
        contactId = json.str("contactId")
        deck = json.str("deck")
        createdAt = json.int64("createdAt")
        createdBy = json.str("createdBy")
        revokedAt = json.int64("revokedAt")
        active = json.optBool("active") ?? (json.int64("revokedAt") == 0)
        views = json.int("views")
        seconds = json.int("seconds")
        lastViewedAt = json.int64("lastViewedAt")
        pagesViewed = json.int("pagesViewed")
        pageCount = json.int("pageCount")
    }

    public var pageShare: Double { pageCount > 0 ? min(1, Double(pagesViewed) / Double(pageCount)) : 0 }
}

public struct CrmDeckPage: Equatable, Sendable, Identifiable {
    public var page: Int
    public var title: String
    public var seconds: Int
    public var viewers: Int

    public var id: Int { page }
}

public struct CrmDeckSession: Equatable, Sendable, Identifiable {
    public var id: String
    public var startedAt: Int64
    public var lastAt: Int64
    public var seconds: Int
    public var pages: Int
    public var country: String
    public var agent: String

    public init(json: JSONObject) {
        id = json.str("id")
        startedAt = json.int64("startedAt")
        lastAt = json.int64("lastAt")
        seconds = json.int("seconds")
        pages = json.int("pages")
        country = json.str("country")
        agent = json.str("agent")
    }
}

public struct CrmDeckDetail: Equatable, Sendable {
    public var link: CrmDeckLink
    public var pages: [CrmDeckPage]
    public var sessions: [CrmDeckSession]

    public init(json: JSONObject) {
        link = CrmDeckLink(json: json)
        pages = json.maps("pages").enumerated().map { index, page in
            CrmDeckPage(
                page: page.int("page", index + 1), title: page.str("title"), seconds: page.int("seconds"),
                viewers: page.int("viewers"))
        }
        sessions = json.maps("sessions").map(CrmDeckSession.init(json:))
    }

    public var longestPage: Int { pages.map(\.seconds).max() ?? 0 }
}

public struct CrmTeamMember: Equatable, Sendable, Identifiable {
    public var name: String
    public var emails: [Int]
    public var outreach: [Int]
    public var contacts: [Int]

    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        emails = json.list("emails").map { $0.int(or: 0) }
        outreach = json.list("outreach").map { $0.int(or: 0) }
        contacts = json.list("contacts").map { $0.int(or: 0) }
    }

    public init(name: String, emails: [Int], outreach: [Int], contacts: [Int]) {
        self.name = name
        self.emails = emails
        self.outreach = outreach
        self.contacts = contacts
    }

    public var total: Int { emails.reduce(0, +) + outreach.reduce(0, +) + contacts.reduce(0, +) }

    public func day(_ index: Int) -> (emails: Int, outreach: Int, contacts: Int) {
        func at(_ list: [Int]) -> Int { list.indices.contains(index) ? list[index] : 0 }
        return (at(emails), at(outreach), at(contacts))
    }
}

public struct CrmTeamActivity: Equatable, Sendable {
    public var days: [String]
    public var members: [CrmTeamMember]

    public init(json: JSONObject) {
        days = json.strings("days")
        members = json.maps("members").map(CrmTeamMember.init(json:)).filter { !$0.name.isEmpty }
    }

    public var busiestDay: Int {
        var best = 0
        for index in days.indices {
            let total = members.reduce(0) { sum, member in
                let day = member.day(index)
                return sum + day.emails + day.outreach + day.contacts
            }
            best = max(best, total)
        }
        return best
    }

    public func totals(for index: Int) -> (emails: Int, outreach: Int, contacts: Int) {
        members.reduce((0, 0, 0)) { sum, member in
            let day = member.day(index)
            return (sum.0 + day.emails, sum.1 + day.outreach, sum.2 + day.contacts)
        }
    }
}

public struct CrmListActivityEntry: Equatable, Sendable, Identifiable {
    public var at: Int64
    public var actor: String
    public var action: String
    public var text: String
    public var count: Int

    public var id: String { "\(at)-\(actor)-\(action)" }

    public init(json: JSONObject) {
        at = json.int64("at")
        actor = json.str("actor")
        action = json.str("action")
        text = json.str("text")
        count = json.int("count", 1)
    }
}

public struct CrmImportColumn: Equatable, Sendable, Identifiable {
    public var index: Int
    public var header: String
    public var field: String
    public var samples: [String]

    public var id: Int { index }

    public init(json: JSONObject) {
        index = json.int("index")
        header = json.str("header")
        field = json.str("field")
        samples = json.strings("samples")
    }
}

public struct CrmImportPreview: Equatable, Sendable {
    public var delimiter: String
    public var columns: [CrmImportColumn]

    public init(json: JSONObject) {
        delimiter = json.str("delimiter")
        columns = json.maps("columns").map(CrmImportColumn.init(json:))
    }

    public var mapping: [JSONValue] { columns.map { .string($0.field) } }
}

public struct CrmImportResult: Equatable, Sendable {
    public var created: Int
    public var updated: Int
    public var skipped: Int
    public var errors: [String]
    public var viewId: String

    public init(json: JSONObject) {
        created = json.int("created")
        updated = json.int("updated")
        skipped = json.int("skipped")
        errors = json.maps("errors").map { $0.str("message", "Row \($0.int("row")): \($0.str("error"))") }
        viewId = json.str("viewId")
    }

    public var sentence: String {
        var parts: [String] = []
        if created > 0 { parts.append("\(created) added") }
        if updated > 0 { parts.append("\(updated) updated") }
        if skipped > 0 { parts.append("\(skipped) skipped") }
        return parts.isEmpty ? "Nothing to import." : parts.joined(separator: ", ") + "."
    }
}

public struct CrmEmailDraft: Equatable, Sendable {
    public var to: String
    public var subject: String
    public var body: String
    public var template: String
    public var contactId: String
    public var noteDraft = false

    public init(contact: CrmContact? = nil, to: String = "") {
        self.to = to.isEmpty ? (contact?.email ?? "") : to
        subject = ""
        body = ""
        template = "blank"
        contactId = contact?.id ?? ""
    }

    public var isValid: Bool {
        to.contains("@") && !subject.trimmingCharacters(in: .whitespaces).isEmpty
            && !body.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    public var outreachBody: JSONObject {
        [
            "to": .string(to.trimmingCharacters(in: .whitespaces)),
            "subject": .string(subject.trimmingCharacters(in: .whitespaces)),
            "body": .string(body), "template": .string(template),
        ]
    }

    public var sendBody: JSONObject {
        var json: JSONObject = [
            "to": .string(to.trimmingCharacters(in: .whitespaces)),
            "subject": .string(subject.trimmingCharacters(in: .whitespaces)), "text": .string(body),
        ]
        if PackagesText.isHexId(contactId) { json["contactId"] = .string(contactId) }
        return json
    }
}

extension JSONObject {
    public func removing(_ key: String) -> JSONObject {
        var copy = self
        copy.removeValue(forKey: key)
        return copy
    }
}

public enum CrmBulkAction: Equatable, Sendable {
    case stage(String)
    case flow(String)
    case addList(String)
    case removeList(String)
    case owner(String)
    case contacted(Int64)

    public func edit(for contact: CrmContact, flows: [CrmFlow]) -> (body: JSONObject, moved: CrmContact) {
        var body: JSONObject = ["id": .string(contact.id)]
        switch self {
        case .stage(let stage):
            body["stage"] = .string(stage)
            return (body, contact.moved(stage: stage))
        case .flow(let flow):
            let target = flows.first { $0.id == flow }
            let stage = target.map { $0.stages.contains(contact.stage) ? contact.stage : ($0.stages.first ?? "lead") } ?? contact.stage
            body["flow"] = .string(flow)
            body["stage"] = .string(stage)
            return (body, contact.moved(flow: flow, stage: stage))
        case .addList(let view):
            body["views"] = [.string(view)]
            return (body, contact.moved(views: contact.views.contains(view) ? contact.views : contact.views + [view]))
        case .removeList(let view):
            body["removeViews"] = [.string(view)]
            return (body, contact.moved(views: contact.views.filter { $0 != view }))
        case .owner(let owner):
            body["owner"] = .string(owner)
            return (body, contact.moved(owner: owner))
        case .contacted(let at):
            body["lastContactedAt"] = .int(at)
            var moved = contact
            moved.lastContactedAt = at
            return (body, moved)
        }
    }
}

public enum CrmPrompt {
    public static func outreach(_ contact: CrmContact, sheet: String, flowName: String) -> String {
        let angle = angle(sheet: sheet, flowName: flowName)
        return [
            "Tell me about ForkMesh (https://forkmesh.com).",
            "Then tell me about \(contact.displayName) (\(about(contact).joined(separator: "; "))).",
            "Then write a short toot (under 500 characters) I can send them on Mastodon about ForkMesh."
                + (angle.isEmpty ? "" : " " + angle)
                + " Mention them by their handle, make it specific to their work, and use at most two hashtags.",
        ].joined(separator: "\n\n")
    }

    static func about(_ contact: CrmContact) -> [String] {
        var about: [String] = []
        if !contact.mastodon.isEmpty {
            let profile = CrmText.profileURL("mastodon", contact.mastodon)?.absoluteString ?? ""
            about.append("Mastodon: \(contact.mastodon)" + (profile.isEmpty || profile == contact.mastodon ? "" : " (\(profile))"))
        } else {
            about.append("no Mastodon handle saved, so look it up")
        }
        if !contact.title.isEmpty && !contact.company.isEmpty {
            about.append("\(contact.title) at \(contact.company)")
        } else if !contact.title.isEmpty || !contact.company.isEmpty {
            about.append(contact.title + contact.company)
        }
        for value in [contact.website, contact.location] where !value.isEmpty { about.append(value) }
        return about
    }

    public static func angle(sheet: String, flowName: String) -> String {
        var group = sheet
        if group.isEmpty || group == "No pipeline" { group = flowName }
        let audience = (group + " " + flowName).lowercased()
        func mentions(_ words: [String]) -> Bool { words.contains { audience.contains($0) } }
        let quoted = "\u{201C}\(group)\u{201D}"
        var angle = ""
        if mentions(["journalist", "press", "media", "reporter", "writer", "editor", "podcast", "newsletter", "blog"]) {
            angle = "They are on my \(quoted) sheet as a journalist, so pitch a story: why decentralized code hosting matters now and what is new about ForkMesh, and offer an interview or a demo. No sales talk."
        } else if mentions(["investor", "angel", "vc", "fund", "capital"]) {
            angle = "They are on my \(quoted) sheet as an investor, so lead with the opportunity ForkMesh opens and ask for a short call; state only facts you were given or looked up, and never invent traction, numbers or names."
        } else if mentions(["partner", "integration", "alliance"]) {
            angle = "They are on my \(quoted) sheet as a potential partner, so show how ForkMesh fits what they build and suggest exploring working together."
        } else if mentions(["customer", "lead", "user", "developer", "maintainer", "community", "open source"]) {
            angle = "They are on my \(quoted) sheet as a potential user, so give one concrete way ForkMesh helps their projects and invite them to try it."
        } else if !group.isEmpty {
            angle = "They are on my \(quoted) sheet, so pitch it the way that suits that group."
        }
        return angle
    }

    public static func contactLink(organization: String, contactId: String) -> URL? {
        URL(string: "https://app.forkmesh.com/dashboard/crm?org=\(organization)&contact=\(contactId)")
    }

    public static func listLink(organization: String, viewId: String) -> URL? {
        URL(string: "https://app.forkmesh.com/dashboard/crm?org=\(organization)&list=\(viewId)")
    }
}

public enum CrmCSV {
    public static func split(_ csv: String, rowsPerPart: Int = 100) -> (header: String, parts: [String], rows: Int) {
        var records: [String] = []
        var current = ""
        var quoted = false
        for character in csv {
            if character == "\"" { quoted.toggle() }
            if (character == "\n" || character == "\r\n") && !quoted {
                if !current.trimmingCharacters(in: .whitespaces).isEmpty { records.append(current) }
                current = ""
            } else if character != "\r" || quoted {
                current.append(character)
            }
        }
        if !current.trimmingCharacters(in: .whitespaces).isEmpty { records.append(current) }
        guard let header = records.first else { return ("", [], 0) }
        let rows = Array(records.dropFirst())
        var parts: [String] = []
        var index = 0
        while index < rows.count {
            let chunk = rows[index..<min(rows.count, index + max(1, rowsPerPart))]
            parts.append(([header] + chunk).joined(separator: "\n"))
            index += rowsPerPart
        }
        return (header, parts.isEmpty ? [header] : parts, rows.count)
    }

    public static func preview(_ csv: String, rows: Int = 20) -> String {
        let split = split(csv, rowsPerPart: rows)
        return split.parts.first ?? ""
    }
}
