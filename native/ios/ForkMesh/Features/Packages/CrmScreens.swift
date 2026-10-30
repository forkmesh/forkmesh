import FMAccount
import FMApi
import FMPackages
import SwiftUI
import UniformTypeIdentifiers

@MainActor
enum CrmSession {
    private static var current: (account: String, api: ObjectIdentifier, model: CrmModel)?

    static func model(client: PackagesClient) -> CrmModel {
        let account = AppServices.current?.auth.session?.nodeName ?? ""
        let api = ObjectIdentifier(client)
        if let current, current.account == account, current.api == api { return current.model }
        current?.model.forget()
        let model = CrmModel(client: client)
        current = (account, api, model)
        return model
    }
}

struct CrmDestination: View {
    let organization: String?
    let flow: String?

    var body: some View {
        PackagesGate(feature: "CRM") { client in
            CrmScreen(model: CrmSession.model(client: client), organization: organization, flow: flow)
        }
    }
}

struct CrmOrgDestination: View {
    let organization: String

    var body: some View {
        CrmDestination(organization: organization, flow: nil)
    }
}

enum CrmSheet: Identifiable {
    case newContact(String?)
    case edit(CrmContact)
    case compose(CrmEmailDraft, outreach: Bool)
    case newList
    case pipeline(CrmFlowDraft)
    case pipelines
    case tabSettings(CrmTabItem)
    case settings
    case importCSV(text: String, fileName: String)
    case export(URL, rows: Int)
    case merge(CrmContact)
    case bulkOwner([String])
    case deck(CrmDeckLink)
    case email(CrmActivity)
    case shared(CrmView)
    case noteDrafts

    var id: String {
        switch self {
        case .newContact(let view): return "new-\(view ?? "")"
        case .edit(let contact): return "edit-" + contact.id
        case .compose(let draft, let outreach): return "compose-\(draft.contactId)-\(outreach)"
        case .newList: return "new-list"
        case .pipeline(let draft): return "pipeline-" + draft.id
        case .pipelines: return "pipelines"
        case .tabSettings(let item): return "tab-" + item.id
        case .settings: return "settings"
        case .importCSV(_, let fileName): return "import-" + fileName
        case .export(let url, _): return "export-" + url.lastPathComponent
        case .merge(let contact): return "merge-" + contact.id
        case .bulkOwner(let ids): return "owner-\(ids.count)"
        case .deck(let link): return "deck-" + link.token
        case .email(let email): return "email-" + email.id
        case .shared(let view): return "shared-" + view.id
        case .noteDrafts: return "note-drafts"
        }
    }
}

struct CrmScreen: View {
    @Bindable var model: CrmModel
    let organization: String?
    let flow: String?
    @State private var sheet: CrmSheet?
    @State private var importing = false
    @State private var notice: FmNotice?
    @State private var selecting = false
    @State private var selection = Set<String>()
    @State private var visible = false

    var body: some View {
        VStack(spacing: 0) {
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal)
            }
            content
        }
        .navigationTitle(model.organization.isEmpty ? "CRM" : model.organization)
        .navigationBarTitleDisplayMode(.inline)
        .toolbarTitleMenu { organizationMenu }
        .searchable(text: $model.query, prompt: "Search contacts")
        .toolbar { toolbar }
        .overlay {
            if model.loading && model.data == nil { ProgressView() }
        }
        .task {
            if let organization, PackagesText.isOrgName(organization) { model.organization = organization }
            if let flow { model.flowId = flow }
            await model.refresh()
            if flow != nil { model.show(.pipeline) }
        }
        .sheet(item: $sheet) { item in
            CrmSheetHost(model: model, sheet: item, present: { sheet = $0 })
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.commaSeparatedText, .plainText, .text]) { result in
            guard case .success(let url) = result else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            if let data = try? Data(contentsOf: url) {
                let text = String(decoding: data, as: UTF8.self)
                let name = url.lastPathComponent
                Task { @MainActor in sheet = .importCSV(text: text, fileName: name) }
            }
        }
        .onAppear { visible = true }
        .onDisappear { visible = false }
        .onChange(of: model.notice) { _, text in
            guard visible, !text.isEmpty else { return }
            notice = FmNotice(text)
            model.notice = ""
        }
        .onChange(of: model.activeTab) { _, _ in
            selection.removeAll()
            selecting = false
        }
        .onChange(of: model.query) { _, query in
            if !query.isEmpty, model.section != .contacts, model.section != .pipeline { model.show(.contacts) }
        }
    }

    @ViewBuilder private var content: some View {
        if model.needsInstall && !model.loading {
            List { CrmInstallSection(model: model) }
        } else if let data = model.data {
            section(data)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .safeAreaInset(edge: .top, spacing: 0) {
                    CrmHeader(model: model, data: data)
                }
        } else if !model.loading && model.installedOrganizations.isEmpty {
            List {
                PackagesErrorBanner(text: model.errorText) { await model.refresh() }
                PackageNotInstalledView(package: "CRM", organizations: model.organizations)
            }
        } else {
            List { PackagesErrorBanner(text: model.errorText) { await model.refresh() } }
        }
    }

    @ViewBuilder private func section(_ data: CrmOrganizationData) -> some View {
        switch model.section {
        case .home:
            CrmHomePane(model: model, data: data, present: { sheet = $0 })
        case .contacts:
            CrmTabContent(
                model: model, data: data, selecting: $selecting, selection: $selection,
                present: { sheet = $0 })
        case .pipeline:
            CrmPipelinePane(model: model, data: data, present: { sheet = $0 })
        case .activity:
            CrmActivitySection(model: model, data: data, present: { sheet = $0 })
        case .more:
            CrmMorePane(
                model: model, data: data, present: { sheet = $0 }, importCSV: { importing = true },
                export: { await export() })
        }
    }

    @ViewBuilder private var organizationMenu: some View {
        ForEach(model.organizations) { org in
            Button {
                model.organization = org.name
                model.tab = nil
                Task { await model.load() }
            } label: {
                if org.name == model.organization {
                    Label(org.name, systemImage: "checkmark")
                } else {
                    Text(org.installed ? org.name : "\(org.name) (not installed)")
                }
            }
        }
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            if selecting {
                Button("Done") {
                    selecting = false
                    selection.removeAll()
                }
            } else {
                if model.section == .contacts { moreMenu }
                CrmModePill()
                addMenu
            }
        }
    }

    private var addMenu: some View {
        Menu {
            Button("New contact", systemImage: "person.badge.plus") { newContact() }
            Button("New list", systemImage: "list.bullet.rectangle") { sheet = .newList }
            if model.canManage {
                Button("New pipeline", systemImage: "point.3.connected.trianglepath.dotted") { sheet = .pipeline(CrmFlowDraft()) }
            }
            Divider()
            Button("Import CSV", systemImage: "square.and.arrow.down") { importing = true }
        } label: {
            Label("New contact", systemImage: "plus")
        } primaryAction: {
            newContact()
        }
        .disabled(model.data == nil)
        .accessibilityIdentifier("crm-new-contact")
    }

    private func newContact() {
        if !model.activeTab.isContacts, let first = model.data?.defaultTab { model.select(first) }
        sheet = .newContact(model.activeView.flatMap { $0.isPipeline ? nil : $0.id })
    }

    private var moreMenu: some View {
        Menu {
            Button("Select contacts", systemImage: "checkmark.circle") { selecting = true }
            CrmFilterMenu(model: model)
            if let item = model.data?.contactTabs.first(where: { $0.tab == model.activeTab }), item.view != nil || item.flow != nil {
                Button("\(item.title) settings", systemImage: "slider.horizontal.3") { sheet = .tabSettings(item) }
            }
        } label: {
            Label("Sort and filter", systemImage: model.filter.isActive ? "line.3.horizontal.decrease.circle.fill" : "line.3.horizontal.decrease.circle")
        }
        .disabled(model.data == nil)
    }

    private func export() async {
        guard let file = await model.exportCSV() else { return }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(file.filename)
        do {
            try Data(file.csv.utf8).write(to: url, options: .atomic)
            sheet = .export(url, rows: max(0, file.csv.split(whereSeparator: \.isNewline).count - 1))
        } catch {
            notice = FmNotice.error("Couldn't save the export.")
        }
    }
}

struct CrmHeader: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData

    var body: some View {
        VStack(spacing: 6) {
            CrmSectionBar(model: model, data: data)
            if !model.errorText.isEmpty {
                PackagesErrorBanner(text: model.errorText) { await model.load() }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    .padding(.horizontal)
            }
            switch model.section {
            case .contacts:
                CrmTabChips(model: model, items: data.contactTabs)
            case .activity:
                CrmTabChips(model: model, items: data.workspaceTabs)
            default:
                EmptyView()
            }
        }
        .padding(.top, 4)
        .padding(.bottom, 6)
    }
}

struct CrmTabContent: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    @Binding var selecting: Bool
    @Binding var selection: Set<String>
    let present: (CrmSheet) -> Void

    var body: some View {
        switch model.activeTab {
        case .list, .pipeline, .noPipeline:
            CrmContactList(model: model, data: data, selecting: $selecting, selection: $selection, present: present)
        case .activity:
            CrmActivityPane(model: model, data: data, present: present)
        case .mail:
            CrmMailPane(model: model, data: data)
        case .outreach:
            CrmOutreachPane(model: model, data: data, present: present)
        case .team:
            CrmTeamPane(data: data)
        case .decks:
            CrmDecksPane(model: model, data: data, present: present)
        case .shared:
            CrmSharedPane(model: model, data: data, present: present)
        }
    }
}

struct CrmInstallSection: View {
    let model: CrmModel

    var body: some View {
        if model.canInstall {
            Section {
                VStack(alignment: .leading, spacing: 8) {
                    Text("CRM is not installed for \(model.organization)").font(.headline)
                    Text("Install it to keep contacts, pipelines and email history with everyone in the organization.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                Button {
                    Task {
                        if await model.install() { FmHaptics.commit() }
                    }
                } label: {
                    Label(model.saving ? "Installing…" : "Install CRM", systemImage: "square.and.arrow.down")
                }
                .disabled(model.saving)
                PackagesErrorBanner(text: model.errorText)
            }
        } else {
            PackageNotInstalledView(package: "CRM", organizations: model.organizations)
        }
    }
}

struct CrmSheetHost: View {
    @Bindable var model: CrmModel
    let sheet: CrmSheet
    let present: (CrmSheet?) -> Void

    var body: some View {
        NavigationStack {
            switch sheet {
            case .newContact(let view):
                CrmContactEditor(model: model, original: nil, addTo: view)
            case .edit(let contact):
                CrmContactEditor(model: model, original: contact)
            case .compose(let draft, let outreach):
                CrmComposeSheet(model: model, draft: draft, outreach: outreach)
            case .newList:
                CrmNewListSheet(model: model)
            case .pipeline(let draft):
                CrmPipelineEditor(model: model, draft: draft)
            case .pipelines:
                CrmPipelinesSheet(model: model)
            case .tabSettings(let item):
                CrmTabSettingsSheet(model: model, item: item)
            case .settings:
                CrmSettingsSheet(model: model)
            case .importCSV(let text, let fileName):
                CrmImportSheet(model: model, csv: text, fileName: fileName)
            case .export(let url, let rows):
                CrmExportSheet(url: url, rows: rows)
            case .merge(let contact):
                CrmMergeSheet(model: model, contact: contact)
            case .bulkOwner(let ids):
                CrmOwnerSheet(model: model, ids: ids)
            case .deck(let link):
                CrmDeckSheet(model: model, link: link)
            case .email(let email):
                CrmEmailSheet(model: model, email: email)
            case .shared(let view):
                CrmSharedViewSheet(model: model, view: view)
            case .noteDrafts:
                CrmNoteDraftsSheet(model: model, present: present)
            }
        }
    }
}

struct CrmZohoBar: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    @Environment(\.openURL) private var openURL
    @State private var working = false

    private var zoho: CrmZohoStatus { data.settings.zoho }

    private var badge: (String, Color) {
        if !zoho.connected { return ("Not connected", .orange) }
        if !zoho.canSend { return ("Read only", .orange) }
        if !zoho.lastSyncError.isEmpty { return ("Sync failed", .orange) }
        return ("Reads and sends", .green)
    }

    private var detail: String {
        guard zoho.connected else { return "The CRM cannot read or send from your mailbox yet." }
        var parts: [String] = [zoho.lastSyncAt > 0 ? "Synced \(PackagesFormat.relative(zoho.lastSyncAt))" : "Never synced"]
        if zoho.lastSentAt > 0 { parts.append("sent \(PackagesFormat.relative(zoho.lastSentAt))") }
        return parts.joined(separator: " · ")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: "envelope").font(.caption).foregroundStyle(.secondary)
                Text("Zoho Mail").font(.subheadline.weight(.semibold))
                Text(badge.0).font(.caption.weight(.semibold)).foregroundStyle(badge.1)
                Spacer(minLength: 4)
                if working { ProgressView().controlSize(.small) }
            }
            Text(detail).font(.caption).foregroundStyle(.secondary)
            ForEach(zoho.steps) { step in
                VStack(alignment: .leading, spacing: 4) {
                    Text(step.text).font(.caption)
                    action(for: step)
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    @ViewBuilder private func action(for step: CrmZohoStep) -> some View {
        switch step.action {
        case "sync" where zoho.connected:
            Button("Sync now", systemImage: "arrow.clockwise") { run { await model.syncMail() } }
                .buttonStyle(.bordered).controlSize(.small).disabled(working)
        case "connect", "reconnect":
            if zoho.oneClick && model.canManage {
                Button(step.action == "connect" ? "Connect Zoho Mail" : "Reconnect Zoho Mail", systemImage: "link") {
                    run { await model.connectZoho() }
                }
                .buttonStyle(.borderedProminent).controlSize(.small).disabled(working)
            } else {
                Text(model.canManage ? "Open CRM settings to connect with a Self Client code." : "Ask an organization owner or admin.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        default:
            Text(model.canManage ? "Open CRM settings to connect with a Self Client code." : "Ask an organization owner or admin.")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }

    private func run(_ work: @escaping @MainActor () async -> URL?) {
        working = true
        Task { @MainActor in
            defer { working = false }
            if let url = await work() { openURL(url) }
        }
    }
}
