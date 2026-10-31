import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class CrmModel {
    public private(set) var organizations: [PackageOrganization] = []
    public private(set) var data: CrmOrganizationData?
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var saving = false
    public private(set) var contactsLoading = false
    public var notice = ""
    public var organization = ""
    public var flowId: String?
    public var query = ""
    public var tab: CrmTab?
    public var filter = CrmFilter()
    public var nextSkipped: Set<String> = []
    public var existing: CrmContact?
    public var section: CrmSection = .home

    @ObservationIgnored private let client: PackagesClient
    @ObservationIgnored private var memory: [String: CrmOrganizationData] = [:]
    @ObservationIgnored private var listViewsRecorded: [String: Date] = [:]
    @ObservationIgnored private var contactsGeneration = 0
    @ObservationIgnored public private(set) var contactsTask: Task<Void, Never>?
    @ObservationIgnored public var now: () -> Date = { Date() }
    @ObservationIgnored public var retryDelays: [UInt64] = [400_000_000, 1_500_000_000]

    public init(client: PackagesClient, organization: String = "", flowId: String? = nil) {
        self.client = client
        self.organization = organization
        self.flowId = flowId
    }

    public var installedOrganizations: [PackageOrganization] { organizations.filter(\.installed) }

    public var current: PackageOrganization? { organizations.first { $0.name == organization } }

    public var needsInstall: Bool { current.map { !$0.installed } ?? false }

    public var canInstall: Bool { current.map { !$0.installed && $0.canManage } ?? false }

    public var flow: CrmFlow { data?.flow(flowId) ?? CrmFlow.fallback }

    public var canManage: Bool { data?.canManageSettings ?? false }

    public var activeTab: CrmTab {
        guard let data else { return tab ?? .activity }
        if let tab, tabExists(tab, in: data) { return tab }
        if let flowId, let match = data.tab(forFlow: flowId) { return match }
        return data.defaultTab
    }

    private func tabExists(_ tab: CrmTab, in data: CrmOrganizationData) -> Bool {
        switch tab {
        case .list, .pipeline, .noPipeline: return data.contactTabs.contains { $0.tab == tab }
        case .shared: return !data.sharedViews.isEmpty
        default: return true
        }
    }

    public var activeFlow: CrmFlow? { data?.flow(for: activeTab) }

    public var activeView: CrmView? { data?.view(for: activeTab) }

    public var activeTitle: String {
        data?.contactTabs.first { $0.tab == activeTab }?.title
            ?? data?.workspaceTabs.first { $0.tab == activeTab }?.title ?? "CRM"
    }

    public var tabContacts: [CrmContact] { data?.contacts(for: activeTab) ?? [] }

    public var visibleContacts: [CrmContact] {
        var current = filter
        current.query = query
        return current.apply(tabContacts, flow: activeFlow)
    }

    public func select(_ tab: CrmTab) {
        self.tab = tab
        filter.stage = ""
        if let flow = data?.flow(for: tab) { flowId = flow.id }
        if case .list(let id) = tab { Task { await self.recordListView(id) } }
    }

    public func show(_ section: CrmSection) {
        self.section = section
        guard let data else { return }
        switch section {
        case .contacts:
            if !activeTab.isContacts { select(data.defaultTab) }
        case .pipeline:
            if activeFlow == nil, let item = data.contactTabs.first(where: { $0.flow != nil }) { select(item.tab) }
        case .activity:
            if activeTab.isContacts { select(.activity) }
        case .home, .more:
            break
        }
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            organizations = try await client.crmOrganizations()
            if organization.isEmpty || !organizations.contains(where: { $0.name == organization }) {
                organization = installedOrganizations.first?.name ?? organizations.first?.name ?? ""
            }
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return
        }
        await load()
    }

    public var contactsStatus: String {
        guard contactsLoading, let data else { return "" }
        let loaded = data.contacts.count
        return "Loading contacts, \(loaded.formatted()) of \(max(loaded, data.contactsTotal).formatted())"
    }

    private func stopContacts() -> Int {
        contactsGeneration += 1
        contactsTask?.cancel()
        contactsTask = nil
        contactsLoading = false
        return contactsGeneration
    }

    public func load() async {
        let generation = stopContacts()
        guard PackagesText.isOrgName(organization) else {
            data = nil
            return
        }
        let org = organization
        let cached = memory[org]
        if cached != nil || data?.organization != org { data = cached }
        do {
            let fresh = try await client.crm(org, since: cached?.revision ?? "", contacts: PackagesClient.crmFirstContacts)
            guard generation == contactsGeneration else { return }
            if let fresh {
                data = fresh
                if fresh.contactsComplete {
                    memory[org] = fresh
                } else {
                    loadRemainingContacts(org, cursor: fresh.contactsNext, generation: generation)
                }
            }
            errorText = ""
        } catch {
            guard generation == contactsGeneration else { return }
            errorText = PackagesError.describe(error)
        }
    }

    private func loadRemainingContacts(_ org: String, cursor first: String, generation: Int) {
        contactsLoading = true
        contactsTask = Task {
            var cursor = first
            while !cursor.isEmpty {
                do {
                    let page = try await client.crmContactsPage(org, cursor: cursor)
                    guard generation == contactsGeneration else { return }
                    guard org == organization else {
                        contactsLoading = false
                        return
                    }
                    data?.appendContacts(page.contacts, next: page.next)
                    cursor = page.next
                } catch {
                    guard generation == contactsGeneration else { return }
                    contactsLoading = false
                    data?.revision = ""
                    memory[org]?.revision = ""
                    errorText = "Some contacts did not load: \(PackagesError.describe(error))"
                    return
                }
            }
            guard generation == contactsGeneration else { return }
            contactsLoading = false
            if let data, org == organization { memory[org] = data }
        }
    }

    public func contact(_ id: String) -> CrmContact? { data?.contacts.first { $0.id == id } }

    public var dashboard: CrmDashboard {
        CrmDashboard(contacts: data?.contacts ?? [], now: Int64(now().timeIntervalSince1970 * 1000))
    }

    private func remember() {
        if let data, data.contactsComplete { memory[organization] = data }
    }

    private func perform<T>(_ work: () async throws -> T) async -> T? {
        guard PackagesText.isOrgName(organization) else { return nil }
        saving = true
        defer { saving = false }
        do {
            let value = try await work()
            errorText = ""
            return value
        } catch {
            errorText = PackagesError.describe(error)
            return nil
        }
    }

    public func clearError() {
        errorText = ""
        existing = nil
    }

    @discardableResult
    public func save(
        _ edit: CrmContactEdit, original: CrmContact?, addTo viewId: String? = nil, requestId: String = ""
    ) async -> CrmContact? {
        existing = nil
        guard edit.isValid, PackagesText.isOrgName(organization) else {
            errorText = PackagesError.sentence("name_or_email_required")
            return nil
        }
        if let original {
            let changes = edit.changes(from: original)
            if changes.isEmpty { return original }
            return await update(original, changes: changes)
        }
        let org = organization
        var fields = edit.changes(from: nil)
        if let viewId, PackagesText.isHexId(viewId) { fields["views"] = [.string(viewId)] }
        if PackagesText.isHexId(requestId) { fields["requestId"] = .string(requestId) }
        saving = true
        defer { saving = false }
        var attempt = 0
        while true {
            do {
                let saved = try await client.createCrmContact(org, fields: fields)
                errorText = ""
                guard org == organization else { return saved }
                data?.replace(saved)
                remember()
                return saved
            } catch {
                if let error = error as? PackagesError, let found = duplicate(from: error) {
                    existing = found
                    let label = found.name.isEmpty ? found.email : found.name
                    errorText = "\(label.isEmpty ? "A contact" : label) already uses that email address, so they are already in the CRM."
                    return nil
                }
                guard PackagesText.isHexId(requestId), PackagesError.isTransient(error), attempt < retryDelays.count else {
                    errorText = PackagesError.describe(error)
                    return nil
                }
                try? await Task.sleep(nanoseconds: retryDelays[attempt])
                attempt += 1
            }
        }
    }

    private func duplicate(from error: PackagesError) -> CrmContact? {
        guard error.code == "contact_email_exists", let id = error.fields["contactId"], PackagesText.isHexId(id) else {
            return nil
        }
        if let known = contact(id) { return known }
        return CrmContact(json: [
            "id": .string(id), "organization": .string(organization),
            "name": .string(error.fields["contactName"] ?? ""), "email": .string(error.fields["contactEmail"] ?? ""),
        ])
    }

    @discardableResult
    public func addDetails(_ edit: CrmContactEdit, to found: CrmContact, addTo viewId: String? = nil) async -> CrmContact? {
        if contact(found.id) == nil { await load() }
        let current = contact(found.id) ?? found
        var changes = edit.additions(to: current)
        if let viewId, PackagesText.isHexId(viewId), !current.views.contains(viewId) { changes["views"] = [.string(viewId)] }
        let saved = await update(current, changes: changes)
        if saved != nil { existing = nil }
        return saved
    }

    nonisolated public static func newRequestId() -> String {
        UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
    }

    @discardableResult
    public func update(_ contact: CrmContact, changes: JSONObject) async -> CrmContact? {
        guard !changes.isEmpty else { return contact }
        let org = organization
        let saved = await perform {
            try await client.updateCrmContact(CrmContactRef(organization: org, contactId: contact.id), changes: changes)
        }
        guard let saved else { return nil }
        data?.replace(saved)
        remember()
        return saved
    }

    @discardableResult
    public func move(_ contact: CrmContact, toStage stage: String) async -> Bool {
        guard stage != contact.stage else { return true }
        return await update(contact, changes: ["stage": .string(stage)]) != nil
    }

    @discardableResult
    public func move(_ contact: CrmContact, toFlow flow: String) async -> Bool {
        guard let data, flow != contact.flow else { return true }
        return await update(contact, changes: CrmBulkAction.flow(flow).edit(for: contact, flows: data.flows).body.removing("id")) != nil
    }

    @discardableResult
    public func delete(_ contact: CrmContact) async -> Bool {
        let org = organization
        let done = await perform { try await client.deleteCrmContact(CrmContactRef(organization: org, contactId: contact.id)) }
        guard done != nil else { return false }
        data?.remove(contact.id)
        remember()
        return true
    }

    @discardableResult
    public func delete(ids: [String]) async -> Int {
        var removed = 0
        for id in ids {
            guard let contact = contact(id) else { continue }
            if await delete(contact) { removed += 1 } else { break }
        }
        if removed > 0 { notice = removed == 1 ? "Deleted 1 contact." : "Deleted \(removed) contacts." }
        return removed
    }

    @discardableResult
    public func bulk(_ action: CrmBulkAction, ids: [String]) async -> Bool {
        guard let data else { return false }
        var bodies: [JSONObject] = []
        for id in ids {
            guard let contact = data.contact(id: id) else { continue }
            bodies.append(action.edit(for: contact, flows: data.flows).body)
        }
        return await sendBulk(bodies)
    }

    @discardableResult
    public func moveToPipelineList(_ ids: [String], view: CrmView) async -> Bool {
        guard let data, view.isPipeline, let flow = data.flows.first(where: { $0.id == view.flow }) else { return false }
        let siblings = (data.pipelineLists[view.flow] ?? []).filter { $0 != view.id }
        let bodies: [JSONObject] = ids.compactMap { (id: String) -> JSONObject? in
            guard let contact = data.contact(id: id) else { return nil }
            let stage = flow.stages.contains(contact.stage) && contact.flow == flow.id ? contact.stage : (flow.stages.first ?? "lead")
            var body: JSONObject = [
                "id": .string(id), "flow": .string(flow.id), "stage": .string(stage), "views": [.string(view.id)],
            ]
            if !siblings.isEmpty { body["removeViews"] = .array(siblings.map { .string($0) }) }
            return body
        }
        return await sendBulk(bodies)
    }

    @discardableResult
    public func setOnlyList(_ ids: [String], viewId: String) async -> Bool {
        await sendBulk(ids.map { ["id": .string($0), "setViews": [.string(viewId)]] })
    }

    @discardableResult
    public func markContacted(_ ids: [String]) async -> Bool {
        await bulk(.contacted(Int64(now().timeIntervalSince1970 * 1000)), ids: ids)
    }

    private func sendBulk(_ bodies: [JSONObject]) async -> Bool {
        guard !bodies.isEmpty else { return false }
        let org = organization
        let reply = await perform { try await client.bulkUpdateCrmContacts(org, edits: bodies) }
        guard let reply else { return false }
        for contact in reply.contacts where PackagesText.isHexId(contact.id) { data?.replace(contact) }
        for id in reply.missing { data?.remove(id) }
        remember()
        return true
    }

    @discardableResult
    public func merge(_ duplicate: CrmContact, into kept: CrmContact) async -> CrmContact? {
        let org = organization
        let merged = await perform { try await client.mergeCrmContacts(org, from: duplicate.id, into: kept.id) }
        guard let merged else { return nil }
        for index in (data?.activity.indices ?? 0..<0) where data?.activity[index].contactId == duplicate.id {
            data?.activity[index].contactId = kept.id
        }
        for index in (data?.decks.indices ?? 0..<0) where data?.decks[index].contactId == duplicate.id {
            data?.decks[index].contactId = kept.id
        }
        data?.contacts.removeAll { $0.id == duplicate.id }
        data?.replace(merged)
        remember()
        notice = "Merged \(duplicate.displayName) into \(merged.displayName)."
        return merged
    }

    @discardableResult
    public func install() async -> Bool {
        guard canInstall else { return false }
        saving = true
        defer { saving = false }
        do {
            _ = try await client.install("crm", organization: organization)
            organizations = organizations.map {
                $0.name == organization ? PackageOrganization(name: $0.name, role: $0.role, installed: true) : $0
            }
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
            return false
        }
        await load()
        return data != nil
    }

    public func forget() {
        _ = stopContacts()
        memory.removeAll()
        listViewsRecorded.removeAll()
        data = nil
        tab = nil
    }

    public func emailDetail(_ id: String) async -> CrmActivity? {
        let org = organization
        do {
            return try await client.crmEmail(org, id: id)
        } catch {
            errorText = PackagesError.describe(error)
            return nil
        }
    }

    @discardableResult
    public func sendEmail(_ draft: CrmEmailDraft) async -> CrmActivity? {
        let org = organization
        let sent = await perform { try await client.sendCrmEmail(org, draft: draft) }
        guard let sent else { return nil }
        absorb(sent)
        notice = "Sent to \(draft.to)."
        if draft.noteDraft, PackagesText.isHexId(draft.contactId) {
            let sentNotice = notice
            if await markNoteDraftSent(draft.contactId, stampContacted: false) { notice = sentNotice + " " + notice }
        }
        return sent
    }

    public func nextContact(_ mode: CrmNextContact.Mode, everyone: Bool = false) -> CrmNextContact {
        CrmNextContact.pick(
            mode, contacts: everyone ? (data?.contacts ?? []) : tabContacts, activity: data?.activity ?? [], skipped: nextSkipped,
            now: Int64(now().timeIntervalSince1970 * 1000))
    }

    public var noteDrafts: [CrmNoteDraft] {
        CrmNoteDraft.drafts(data?.contacts ?? [], activity: data?.activity ?? [])
    }

    @discardableResult
    public func markNoteDraftSent(_ contactId: String, stampContacted: Bool) async -> Bool {
        guard let contact = contact(contactId) else { return false }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = .current
        let parts = calendar.dateComponents([.year, .month, .day], from: now())
        let day = String(format: "%04d-%02d-%02d", parts.year ?? 1970, parts.month ?? 1, parts.day ?? 1)
        guard let notes = CrmNoteDraft.markedSent(contact.notes, day: day) else { return false }
        var changes: JSONObject = ["notes": .string(notes)]
        if stampContacted { changes["lastContactedAt"] = .int(Int64(now().timeIntervalSince1970 * 1000)) }
        guard await update(contact, changes: changes) != nil else { return false }
        notice = "Draft marked sent in \(contact.name.isEmpty ? "the contact" : contact.name)'s notes."
        return true
    }

    @discardableResult
    public func restoreColumn(_ field: String) async -> Bool {
        guard let settings = data?.settings else { return false }
        let org = organization
        let removed = settings.removedColumns.filter { $0 != field }
        let saved = await perform { try await client.saveCrmSettings(org, body: ["removedColumns": .array(removed.map { .string($0) })]) }
        guard let saved else { return false }
        data?.settings = saved
        remember()
        notice = "Column \(CrmColumnCatalog.label(field)) restored."
        return true
    }

    @discardableResult
    public func exchangeZoho(_ code: CrmZohoCode) async -> Bool {
        guard code.isComplete else {
            errorText = PackagesError.sentence("zoho_code_incomplete")
            return false
        }
        let org = organization
        let saved = await perform { try await client.exchangeCrmZoho(org, code: code) }
        guard let saved else { return false }
        data?.settings = saved
        remember()
        notice = "Zoho Mail connected (\(saved.zoho.region)). Sync mail to read it."
        return true
    }

    @discardableResult
    public func sendOutreach(_ draft: CrmEmailDraft) async -> Bool {
        let org = organization
        let sentToday = await perform { try await client.sendCrmOutreach(org, draft: draft) }
        guard let sentToday else { return false }
        data?.outreach.sentToday = sentToday
        notice = "Outreach sent to \(draft.to)."
        await load()
        return true
    }

    @discardableResult
    public func logEmail(to: String, subject: String, contactId: String, direction: String = "outbound") async -> CrmActivity? {
        let org = organization
        let at = Int64(now().timeIntervalSince1970 * 1000)
        var body: JSONObject = [
            "to": .string(to), "subject": .string(subject), "externalId": .string("mobile-\(at)"),
            "status": "external", "direction": .string(direction), "provider": "other", "sentAt": .int(at),
        ]
        if PackagesText.isHexId(contactId) { body["contactId"] = .string(contactId) }
        let logged = await perform { try await client.logCrmEmail(org, body: body) }
        guard let logged else { return nil }
        absorb(logged)
        return logged
    }

    @discardableResult
    public func setEmailStatus(_ email: CrmActivity, status: String) async -> CrmActivity? {
        let org = organization
        let updated = await perform { try await client.updateCrmEmail(org, id: email.id, status: status) }
        guard let updated else { return nil }
        absorb(updated)
        return updated
    }

    @discardableResult
    public func deleteEmail(_ email: CrmActivity) async -> Bool {
        let org = organization
        guard await perform({ try await client.deleteCrmEmail(org, id: email.id) }) != nil else { return false }
        data?.activity.removeAll { $0.id == email.id }
        remember()
        return true
    }

    private func absorb(_ email: CrmActivity) {
        guard !email.id.isEmpty else { return }
        var stored = email
        stored.text = ""
        stored.html = ""
        if let index = data?.activity.firstIndex(where: { $0.id == email.id }) {
            data?.activity[index] = stored
        } else {
            data?.activity.insert(stored, at: 0)
        }
        if email.delivered || email.status == "external", let contact = contact(email.contactId) {
            var touched = contact
            touched.lastContactedAt = max(contact.lastContactedAt, email.at)
            data?.replace(touched)
        }
        remember()
    }

    @discardableResult
    public func moveMail(_ message: CrmMailMessage, to box: String) async -> Bool {
        let org = organization
        let moved = await perform { try await client.moveCrmMail(org, id: message.id, box: box) }
        guard let moved else { return false }
        data?.replace(moved)
        remember()
        return true
    }

    @discardableResult
    public func deleteMail(_ message: CrmMailMessage) async -> Bool {
        let org = organization
        guard await perform({ try await client.deleteCrmMail(org, id: message.id) }) != nil else { return false }
        data?.mail.removeAll { $0.id == message.id }
        remember()
        return true
    }

    public func syncMail() async -> URL? {
        guard let data else { return nil }
        let org = organization
        if !data.settings.zoho.connected {
            guard data.canManageSettings, data.settings.zoho.oneClick else {
                errorText = "Zoho Mail is not connected. An owner or admin connects it in CRM settings."
                return nil
            }
            return await perform { try await client.connectCrmZoho(org) } ?? nil
        }
        let reply = await perform { try await client.importCrmMail(org) }
        guard let reply else { return nil }
        let created = reply.int("created")
        let updated = reply.int("updated")
        notice = created + updated == 0 ? "Mail is up to date." : "Mail: \(created) new, \(updated) updated."
        await load()
        return nil
    }

    public func connectZoho() async -> URL? {
        let org = organization
        return await perform { try await client.connectCrmZoho(org) } ?? nil
    }

    @discardableResult
    public func saveSettings(_ edit: CrmSettingsEdit) async -> Bool {
        let org = organization
        let settings = await perform { try await client.saveCrmSettings(org, body: edit.body) }
        guard let settings else { return false }
        data?.settings = settings
        remember()
        notice = "Settings saved."
        return true
    }

    @discardableResult
    public func saveFlow(_ draft: CrmFlowDraft) async -> CrmFlow? {
        guard let data, let flow = draft.flow else {
            errorText = "Give the pipeline a name."
            return nil
        }
        var flows = data.flows
        let previous = flows.first { $0.id == flow.id }
        if draft.isNew {
            guard !flows.contains(where: { $0.id == flow.id }) else {
                errorText = "A pipeline with that name already exists."
                return nil
            }
            guard flows.count < 12 else {
                errorText = "An organization can have up to 12 pipelines."
                return nil
            }
            flows.append(flow)
        } else if let index = flows.firstIndex(where: { $0.id == flow.id }) {
            flows[index] = flow
        }
        let org = organization
        guard let saved = await perform({ try await client.saveCrmFlows(org, flows: flows) }) else { return nil }
        self.data?.flows = saved.isEmpty ? [CrmFlow.fallback] : saved
        if let previous, !draft.isNew {
            let renames = zip(previous.stages, flow.stages).filter { $0.0 != $0.1 && !previous.stages.contains($0.1) }
            let removed = Set(previous.stages).subtracting(flow.stages)
            var edits: [JSONObject] = []
            for contact in data.contacts where contact.flow == flow.id && removed.contains(contact.stage) {
                let target = renames.first { $0.0 == contact.stage }?.1 ?? (flow.stages.first ?? "lead")
                edits.append(["id": .string(contact.id), "stage": .string(target)])
            }
            if !edits.isEmpty { await sendBulk(edits) }
        }
        if draft.isNew {
            if let view = await perform({ try await client.createCrmView(org, body: ["name": .string(flow.name), "flow": .string(flow.id), "visibility": "organization"]) }) {
                self.data?.replace(view)
                tab = .list(view.id)
            } else {
                tab = .pipeline(flow.id)
            }
        }
        remember()
        notice = draft.isNew ? "Pipeline \(flow.name) created." : "Pipeline \(flow.name) saved."
        return flow
    }

    @discardableResult
    public func deleteFlow(_ flowId: String) async -> Bool {
        guard let data else { return false }
        guard data.flows.count > 1 else {
            errorText = "The last pipeline can't be deleted."
            return false
        }
        let flows = data.flows.filter { $0.id != flowId }
        let org = organization
        guard let saved = await perform({ try await client.saveCrmFlows(org, flows: flows) }) else { return false }
        self.data?.flows = saved
        for view in data.views where view.flow == flowId {
            if await perform({ try await client.deleteCrmView(org, id: view.id) }) != nil {
                self.data?.views.removeAll { $0.id == view.id }
            }
        }
        tab = nil
        remember()
        notice = "Pipeline deleted."
        return true
    }

    @discardableResult
    public func reorderFlows(_ order: [String]) async -> Bool {
        guard let data else { return false }
        let flows = order.compactMap { id in data.flows.first { $0.id == id } }
        guard flows.count == data.flows.count else { return false }
        let org = organization
        guard let saved = await perform({ try await client.saveCrmFlows(org, flows: flows) }) else { return false }
        self.data?.flows = saved
        remember()
        return true
    }

    @discardableResult
    public func createList(name: String, flow: String = "", visibility: String = "organization") async -> CrmView? {
        let title = name.trimmingCharacters(in: .whitespaces)
        var body: JSONObject = ["name": .string(title.isEmpty ? "Untitled sheet" : title), "visibility": .string(visibility)]
        if !flow.isEmpty { body["flow"] = .string(flow) }
        let org = organization
        guard let view = await perform({ try await client.createCrmView(org, body: body) }) else { return nil }
        data?.replace(view)
        remember()
        tab = .list(view.id)
        return view
    }

    @discardableResult
    public func updateList(_ view: CrmView, body: JSONObject) async -> CrmView? {
        let org = organization
        guard let updated = await perform({ try await client.updateCrmView(org, id: view.id, body: body) }) else { return nil }
        data?.replace(updated)
        remember()
        return updated
    }

    @discardableResult
    public func renameTab(_ item: CrmTabItem, to name: String) async -> Bool {
        let title = name.trimmingCharacters(in: .whitespaces)
        guard !title.isEmpty else { return false }
        if let view = item.view { return await updateList(view, body: ["name": .string(title)]) != nil }
        guard let flow = item.flow else { return false }
        var draft = CrmFlowDraft(flow: flow)
        draft.name = title
        return await saveFlow(draft) != nil
    }

    @discardableResult
    public func share(_ item: CrmTabItem, with names: [String], access: String, visibility: String) async -> Bool {
        var seen = Set<String>()
        let clean = names.map { $0.trimmingCharacters(in: .whitespaces).lowercased() }.filter { !$0.isEmpty && seen.insert($0).inserted }
        let body: JSONObject = [
            "sharedWith": .array(clean.map { .string($0) }), "shareAccess": .string(access), "visibility": .string(visibility),
        ]
        if let view = item.view { return await updateList(view, body: body) != nil }
        guard let flow = item.flow else { return false }
        var create = body
        create["name"] = .string(flow.name)
        create["flow"] = .string(flow.id)
        let org = organization
        guard let view = await perform({ try await client.createCrmView(org, body: create) }) else { return false }
        data?.replace(view)
        tab = .list(view.id)
        remember()
        return true
    }

    @discardableResult
    public func setColumns(_ view: CrmView, columns: [CrmColumn]) async -> Bool {
        await updateList(view, body: ["columns": .array(columns.map(\.json))]) != nil
    }

    @discardableResult
    public func deleteList(_ view: CrmView) async -> Bool {
        let org = organization
        guard await perform({ try await client.deleteCrmView(org, id: view.id) }) != nil else { return false }
        data?.views.removeAll { $0.id == view.id }
        for index in (data?.contacts.indices ?? 0..<0) {
            data?.contacts[index].views.removeAll { $0 == view.id }
        }
        if tab == .list(view.id) { tab = nil }
        remember()
        notice = "Deleted \(view.name)."
        return true
    }

    @discardableResult
    public func moveList(_ view: CrmView, by offset: Int) async -> Bool {
        guard var order = data?.views.map(\.id), let index = order.firstIndex(of: view.id) else { return false }
        let target = index + offset
        guard order.indices.contains(target) else { return false }
        order.swapAt(index, target)
        return await reorderLists(order)
    }

    @discardableResult
    public func reorderLists(_ order: [String]) async -> Bool {
        let org = organization
        guard let views = await perform({ try await client.orderCrmViews(org, order: order) }) else { return false }
        data?.views = views.filter { PackagesText.isHexId($0.id) }
        remember()
        return true
    }

    @discardableResult
    public func mergeList(_ source: CrmView, into target: CrmView) async -> Bool {
        guard let data, source.id != target.id else { return false }
        let members = data.contacts.filter { data.contains($0, in: source) }.map(\.id)
        if !members.isEmpty {
            let ok: Bool
            if target.isPipeline {
                ok = await moveToPipelineList(members, view: target)
            } else {
                ok = await sendBulk(members.map { ["id": .string($0), "views": [.string(target.id)], "removeViews": [.string(source.id)]] })
            }
            guard ok else { return false }
        }
        let known = Set(target.columns.map(\.field))
        let extra = source.columns.filter { !known.contains($0.field) }
        if !extra.isEmpty { _ = await setColumns(target, columns: target.columns + extra) }
        guard await deleteList(source) else { return false }
        tab = .list(target.id)
        notice = "Merged \(source.name) into \(target.name)."
        return true
    }

    public func listActivity(_ view: CrmView, before: Int64 = 0) async -> (entries: [CrmListActivityEntry], more: Bool) {
        let org = organization
        do {
            return try await client.crmListActivity(org, viewId: view.id, before: before)
        } catch {
            errorText = PackagesError.describe(error)
            return ([], false)
        }
    }

    public func recordListView(_ viewId: String) async {
        let stamp = now()
        if let last = listViewsRecorded[viewId], stamp.timeIntervalSince(last) < 30 * 60 { return }
        listViewsRecorded[viewId] = stamp
        try? await client.recordCrmListView(organization, viewId: viewId)
    }

    @discardableResult
    public func deleteColumn(_ field: String) async -> Bool {
        let org = organization
        guard await perform({ try await client.deleteCrmColumn(org, field: field) }) != nil else { return false }
        await load()
        notice = "Column \(CrmColumnCatalog.label(field)) deleted."
        return true
    }

    @discardableResult
    public func deckLink(for contact: CrmContact) async -> CrmDeckLink? {
        let org = organization
        guard let link = await perform({ try await client.createCrmDeckLink(org, contactId: contact.id) }) else { return nil }
        data?.replace(link)
        remember()
        return link
    }

    public func deckDetail(_ link: CrmDeckLink) async -> CrmDeckDetail? {
        let org = organization
        do {
            return try await client.crmDeckDetail(org, token: link.token)
        } catch {
            errorText = PackagesError.describe(error)
            return nil
        }
    }

    @discardableResult
    public func revoke(_ link: CrmDeckLink) async -> Bool {
        let org = organization
        guard await perform({ try await client.revokeCrmDeckLink(org, token: link.token) }) != nil else { return false }
        var revoked = link
        revoked.active = false
        revoked.revokedAt = Int64(now().timeIntervalSince1970 * 1000)
        data?.replace(revoked)
        remember()
        return true
    }

    @discardableResult
    public func setOutreachMember(_ name: String, allowed: Bool) async -> Bool {
        let org = organization
        guard let members = await perform({ try await client.updateCrmOutreachTeam(org, add: allowed, name: name) }) else { return false }
        data?.outreach.members = members
        remember()
        return true
    }

    public func exportCSV() async -> (filename: String, csv: String)? {
        let org = organization
        return await perform { try await client.exportCrmContacts(org) }
    }

    public func previewImport(_ csv: String) async -> CrmImportPreview? {
        let org = organization
        return await perform { try await client.previewCrmImport(org, csv: CrmCSV.preview(csv)) }
    }

    public func importCSV(
        _ csv: String, fileName: String, sheetName: String, viewId: String = "", mapping: [String],
        progress: ((Int, Int) -> Void)? = nil
    ) async -> CrmImportResult? {
        let split = CrmCSV.split(csv)
        guard split.rows > 0 else {
            errorText = PackagesError.sentence("csv_required")
            return nil
        }
        guard split.rows <= 2000 else {
            errorText = "The file has more than 2000 rows."
            return nil
        }
        let org = organization
        let importId = "mobile-" + String(Int64(now().timeIntervalSince1970 * 1000))
        var totals = (created: 0, updated: 0, skipped: 0)
        var errors: [String] = []
        var viewTarget = viewId
        var offset = 0
        saving = true
        defer { saving = false }
        for (part, chunk) in split.parts.enumerated() {
            progress?(part, split.parts.count)
            var body: JSONObject = [
                "csv": .string(chunk), "fileName": .string(fileName), "sheetName": .string(sheetName),
                "mapping": .array(mapping.map { .string($0) }),
                "import": [
                    "id": .string(importId), "part": .int(Int64(part)), "parts": .int(Int64(split.parts.count)),
                    "offset": .int(Int64(offset)), "rows": .int(Int64(split.rows)), "created": .int(Int64(totals.created)),
                    "updated": .int(Int64(totals.updated)), "skipped": .int(Int64(totals.skipped)),
                ],
            ]
            if PackagesText.isHexId(viewTarget) { body["viewId"] = .string(viewTarget) }
            do {
                let reply = try await client.importCrmContacts(org, body: body)
                let result = CrmImportResult(json: reply)
                totals.created += result.created
                totals.updated += result.updated
                totals.skipped += result.skipped
                errors += result.errors
                if PackagesText.isHexId(result.viewId) { viewTarget = result.viewId }
            } catch {
                errorText = PackagesError.describe(error)
                var failed = body
                failed["csv"] = ""
                var batch = body.obj("import")
                batch["failed"] = .string(errorText)
                failed["import"] = .object(batch)
                _ = try? await client.importCrmContacts(org, body: failed)
                await load()
                return nil
            }
            offset += CrmCSV.split(chunk).rows
        }
        progress?(split.parts.count, split.parts.count)
        errorText = ""
        var result = CrmImportResult(json: [:])
        result.created = totals.created
        result.updated = totals.updated
        result.skipped = totals.skipped
        result.errors = errors
        result.viewId = viewTarget
        await load()
        if PackagesText.isHexId(viewTarget) { tab = .list(viewTarget) }
        notice = result.sentence
        return result
    }

    public func sharedView(_ view: CrmView) async -> (view: CrmView, contacts: [CrmContact])? {
        do {
            return try await client.crmSharedView(view.id)
        } catch {
            errorText = PackagesError.describe(error)
            return nil
        }
    }

    @discardableResult
    public func copySharedView(_ view: CrmView) async -> CrmImportResult? {
        let org = organization
        guard let result = await perform({ try await client.copyCrmSharedView(org, viewId: view.id) }) else { return nil }
        await load()
        if PackagesText.isHexId(result.viewId) { tab = .list(result.viewId) }
        notice = "Copied \(view.name): " + result.sentence
        return result
    }

    @discardableResult
    public func updateSharedContact(_ view: CrmView, contact: CrmContact, changes: JSONObject) async -> CrmContact? {
        await perform { try await client.updateCrmSharedContact(view.id, contactId: contact.id, changes: changes) }
    }
}
