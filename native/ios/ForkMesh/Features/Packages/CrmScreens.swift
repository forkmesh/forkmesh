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
    @State private var board = false
    @State private var selecting = false
    @State private var selection = Set<String>()
    @State private var visible = false
    @Environment(\.openURL) private var openURL

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
    }

    @ViewBuilder private var content: some View {
        if model.needsInstall && !model.loading {
            List { CrmInstallSection(model: model) }
        } else if let data = model.data {
            CrmTabContent(
                model: model, data: data, board: board, selecting: $selecting, selection: $selection,
                present: { sheet = $0 }
            )
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
                addMenu
                moreMenu
            }
        }
    }

    private var addMenu: some View {
        Menu {
            Button("New contact", systemImage: "person.badge.plus") {
                sheet = .newContact(model.activeView.flatMap { $0.isPipeline ? nil : $0.id })
            }
            Button("New list", systemImage: "list.bullet.rectangle") { sheet = .newList }
            if model.canManage {
                Button("New pipeline", systemImage: "point.3.connected.trianglepath.dotted") { sheet = .pipeline(CrmFlowDraft()) }
            }
            Divider()
            Button("Import CSV", systemImage: "square.and.arrow.down") { importing = true }
        } label: {
            Label("Add", systemImage: "plus")
        }
        .disabled(model.data == nil)
    }

    private var moreMenu: some View {
        Menu {
            if model.activeTab.isContacts {
                Button("Select contacts", systemImage: "checkmark.circle") { selecting = true }
                if model.activeFlow != nil {
                    Picker("Layout", selection: $board) {
                        Label("List", systemImage: "list.bullet").tag(false)
                        Label("Board", systemImage: "rectangle.split.3x1").tag(true)
                    }
                }
                CrmFilterMenu(model: model)
                if let item = model.data?.contactTabs.first(where: { $0.tab == model.activeTab }), item.view != nil || item.flow != nil {
                    Button("\(item.title) settings", systemImage: "slider.horizontal.3") { sheet = .tabSettings(item) }
                }
            }
            Divider()
            Button("Pipelines", systemImage: "point.3.connected.trianglepath.dotted") { sheet = .pipelines }
            Button(model.noteDrafts.isEmpty ? "Unsent drafts" : "Unsent drafts (\(model.noteDrafts.count))", systemImage: "pencil.and.list.clipboard") {
                sheet = .noteDrafts
            }
            Button("Export CSV", systemImage: "square.and.arrow.up") { Task { await export() } }
            Button("Sync mail", systemImage: "arrow.triangle.2.circlepath") { Task { await syncMail() } }
            if let url = model.data?.settings.composeURL(to: "") {
                Button(model.data?.settings.mailProvider == "gmail" ? "Open Gmail" : "Open Zoho Mail", systemImage: "envelope") {
                    openURL(url)
                }
            }
            Button("CRM settings", systemImage: "gearshape") { sheet = .settings }
            Button("Refresh", systemImage: "arrow.clockwise") { Task { await model.load() } }
        } label: {
            Label("More", systemImage: "ellipsis.circle")
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

    private func syncMail() async {
        if let url = await model.syncMail() { openURL(url) }
    }
}

struct CrmHeader: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData

    var body: some View {
        VStack(spacing: 8) {
            if !model.errorText.isEmpty {
                PackagesErrorBanner(text: model.errorText) { await model.load() }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    .padding(.horizontal)
            }
            HStack(spacing: 8) {
                CrmStatTile(value: "\(data.contacts.count)", label: "Contacts", icon: "person.2.fill", tint: .blue)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
                CrmStatTile(value: CrmText.money(data.pipelineValue), label: "Pipeline", icon: "chart.line.uptrend.xyaxis", tint: .green)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
                CrmStatTile(value: "\(data.activity.count)", label: "Emails", icon: "paperplane.fill", tint: .indigo)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
                CrmStatTile(value: "\(data.decks.filter(\.active).count)", label: "Decks", icon: "rectangle.on.rectangle.angled", tint: .orange)
                    .shellGlass(RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
            .padding(.horizontal)
            CrmTabStrip(model: model, data: data)
        }
        .padding(.top, 6)
        .padding(.bottom, 8)
    }
}

struct CrmTabStrip: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(data.contactTabs) { item in chip(item) }
                    Divider().frame(height: 22)
                    ForEach(data.workspaceTabs) { item in chip(item) }
                }
                .padding(.horizontal)
                .padding(.vertical, 2)
            }
            .onAppear { proxy.scrollTo(model.activeTab.id, anchor: .center) }
            .onChange(of: model.activeTab) { _, tab in
                withAnimation { proxy.scrollTo(tab.id, anchor: .center) }
            }
        }
    }

    private func chip(_ item: CrmTabItem) -> some View {
        let active = model.activeTab == item.tab
        return Button {
            FmHaptics.selection()
            model.select(item.tab)
        } label: {
            HStack(spacing: 5) {
                Image(systemName: item.icon).imageScale(.small)
                Text(item.title).lineLimit(1)
                if let count = item.count {
                    Text("\(count)")
                        .font(.caption2.monospacedDigit().weight(.bold))
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(active ? Color.white.opacity(0.25) : Color.secondary.opacity(0.15), in: Capsule())
                }
            }
            .font(.subheadline.weight(active ? .semibold : .regular))
            .padding(.horizontal, 11)
            .padding(.vertical, 7)
            .foregroundStyle(active ? Color.white : Color.primary)
            .background(active ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(Color.clear), in: Capsule())
            .shellGlass(Capsule())
        }
        .buttonStyle(.plain)
        .id(item.tab.id)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

struct CrmTabContent: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let board: Bool
    @Binding var selecting: Bool
    @Binding var selection: Set<String>
    let present: (CrmSheet) -> Void

    var body: some View {
        switch model.activeTab {
        case .list, .pipeline, .noPipeline:
            if board, let flow = model.activeFlow, !selecting {
                CrmBoard(model: model, data: data, flow: flow, present: present)
            } else {
                CrmContactList(model: model, data: data, selecting: $selecting, selection: $selection, present: present)
            }
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
