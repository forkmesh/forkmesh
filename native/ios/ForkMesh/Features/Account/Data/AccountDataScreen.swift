import FMAccount
import FMApi
import FMBase
import SwiftUI
import UniformTypeIdentifiers

@MainActor
struct AccountDataScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: AccountDataModel?
    @State var browser: AccountDataBrowser?
    @State var document: JSONTextDocument?
    @State var exporting: Bool = false
    @State var confirmClear: Bool = false
    @State var confirmRestore: Bool = false
    @State var confirmStore: AccountStoreItem?
    @State var confirmStoreAll: Bool = false
    @State var exportNotice: FmNotice?
    @State var cacheNotice: FmNotice?
    @State var syncNotice: FmNotice?
    @State var storeNotice: FmNotice?

    private let auth: AuthService?
    private let sync: AccountSyncService?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        auth = Services.get(AuthService.self)
        sync = Services.get(AccountSyncService.self)
        _model = State(initialValue: api.map { AccountDataModel(client: $0) })
        _browser = State(initialValue: api.map { AccountDataBrowser(client: $0) })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if let sync = sync {
                    SettingsSection(title: "Save to ForkMesh") {
                        SettingsCardBox { syncCard(sync) }
                    }
                }
                SettingsSection(title: "Everything ForkMesh has about you") {
                    SettingsCardBox(padded: false) { inventoryCard }
                }
                SettingsSection(title: "Saved from your phone") {
                    SettingsCardBox(padded: false) { storeCard }
                }
                SettingsSection(title: "Download your data") {
                    SettingsCardBox { exportCard }
                }
                SettingsSection(title: "This phone", last: true) {
                    SettingsCardBox { cacheCard }
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { [browser, auth] in await Self.refresh(browser, auth) }
        .task { if browser?.inventory == nil, auth?.session != nil { await browser?.load() } }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle("Data")
        .navigationBarTitleDisplayMode(.inline)
        .fileExporter(
            isPresented: $exporting, document: document, contentType: .json,
            defaultFilename: model?.exportFile?.fileName ?? "forkmesh-export.json"
        ) { (result: Result<URL, Error>) in
            document = nil
            model?.clearExport()
            if case .success = result { exportNotice = FmNotice("Export saved") }
        }
        .fmDialog(isPresented: $confirmClear) {
            FmConfirmDialog(
                title: "Clear cached data?",
                message: "Cached server answers on this phone are removed. Your account, keys, settings and on-device repositories stay.",
                confirmLabel: "Clear",
                onCancel: { confirmClear = false },
                onConfirm: {
                    confirmClear = false
                    clearCaches()
                })
        }
        .fmDialog(isPresented: $confirmRestore) {
            FmConfirmDialog(
                title: "Restore saved data?",
                message: "This phone's home screen layout, news reader subscriptions and app settings are replaced with the copy saved on ForkMesh.",
                confirmLabel: "Restore",
                onCancel: { confirmRestore = false },
                onConfirm: {
                    confirmRestore = false
                    Task { @MainActor in
                        guard let sync = sync else { return }
                        let applied = await sync.restore()
                        if sync.error.isEmpty {
                            syncNotice = FmNotice(applied == 0
                                ? "The saved copy matches this phone; nothing changed."
                                : "Restored \(applied) \(applied == 1 ? "setting" : "settings and subscriptions"). Some appear after you reopen ForkMesh.")
                        }
                    }
                })
        }
        .fmDialog(isPresented: storeShown) {
            if let item = confirmStore {
                FmConfirmDialog(
                    title: "Delete \(item.title)?",
                    message: "The copy saved on ForkMesh is deleted. This phone keeps its own settings, and saves them again while Save to ForkMesh is on.",
                    confirmLabel: "Delete",
                    destructive: true,
                    onCancel: { confirmStore = nil },
                    onConfirm: {
                        confirmStore = nil
                        Task { @MainActor in
                            if await browser?.deleteStore(item.key) == true {
                                sync?.forgetHashes()
                                storeNotice = FmNotice("Deleted from ForkMesh")
                            }
                        }
                    })
            }
        }
        .fmDialog(isPresented: $confirmStoreAll) {
            FmConfirmDialog(
                title: "Delete everything saved from your phone?",
                message: "Every copy saved on ForkMesh and its encryption key are deleted, and Save to ForkMesh is switched off. This phone keeps its own settings.",
                confirmLabel: "Delete all",
                destructive: true,
                onCancel: { confirmStoreAll = false },
                onConfirm: {
                    confirmStoreAll = false
                    Task { @MainActor in
                        if await browser?.deleteStore(nil) == true {
                            sync?.enabled = false
                            storeNotice = FmNotice("Deleted everything saved from your phone")
                        }
                    }
                })
        }
    }

    private static func refresh(_ browser: AccountDataBrowser?, _ auth: AuthService?) async {
        guard auth?.session != nil else { return }
        await browser?.load(refresh: true)
    }

    private var storeShown: Binding<Bool> {
        Binding<Bool>(get: { confirmStore != nil }, set: { (shown: Bool) in if !shown { confirmStore = nil } })
    }

    @ViewBuilder private func syncCard(_ sync: AccountSyncService) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Toggle(isOn: Binding<Bool>(get: { sync.enabled }, set: { (on: Bool) in
                sync.enabled = on
                if on {
                    Task { @MainActor in
                        await sync.saveNow()
                        await browser?.load(refresh: true)
                    }
                }
            })) {
                Text("Save my settings to ForkMesh").fmText(.bodyMedium)
            }
            .tint(fm.accent)
            .disabled(auth?.session == nil)
            SettingsNote(text: "Your home screen layout, news reader subscriptions and app settings are saved to your account, so a new phone or a reinstall can get them back. They are encrypted with a key that belongs to your account alone, and that key is itself encrypted on the server. Sign-in keys, passwords and chat keys never leave this phone.")
            FmKeyValueRow(label: "Last saved", value: AccountTime.label(sync.lastSavedAt, nowMillis: SystemClock().nowMillis))
            if !sync.error.isEmpty {
                SettingsNote(text: sync.error, color: fm.danger)
                    .fmMessageTools(sync.error)
            }
            FmNoticeLine($syncNotice)
            HStack(spacing: FmSpace.x2) {
                FmFilledButton(sync.busy ? "Saving…" : "Save now", icon: "cloud_upload_outlined",
                               onPressed: sync.busy || auth?.session == nil ? nil : {
                    Task { @MainActor in
                        let saved = await sync.saveNow()
                        if sync.error.isEmpty {
                            syncNotice = FmNotice(saved == 0 ? "Nothing to save yet" : saved == 1 ? "Saved 1 item" : "Saved \(saved) items")
                            await browser?.load(refresh: true)
                        }
                    }
                })
                FmOutlinedButton("Restore", icon: "history",
                                 onPressed: sync.busy || auth?.session == nil ? nil : { confirmRestore = true })
            }
        }
    }

    @ViewBuilder private var inventoryCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsNote(text: "Every place ForkMesh keeps something about you: your account and linked nodes, the repositories you own, and each organization you belong to. Open a section to see each item, and remove the ones that can be removed on their own.")
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
            FmHairline()
            if let browser = browser {
                if browser.loading && browser.inventory == nil {
                    FmSkeletonList(rows: 3, label: "Looking through your data")
                } else if let inventory = browser.inventory, !inventory.subjects.isEmpty {
                    ForEach(Array(inventory.subjects.enumerated()), id: \.element.id) { (index: Int, subject: AccountDataSubject) in
                        if index > 0 { FmHairline() }
                        NavigationLink {
                            AccountDataSubjectScreen(browser: browser, subjectId: subject.id)
                                .shellPageClearance()
                        } label: {
                            SettingsLinkRow(icon: icon(for: subject), title: subject.title, subtitle: subject.subtitle)
                        }
                        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
                    }
                } else {
                    SettingsEmptyNote(
                        title: browser.error.isEmpty ? "Nothing to show" : "Your data is unavailable",
                        text: browser.error.isEmpty ? "Sign in to see what ForkMesh has about you." : browser.error)
                        .padding(FmSpace.x4)
                }
                if !browser.error.isEmpty && browser.inventory != nil {
                    FmHairline()
                    SettingsNote(text: browser.error, color: fm.danger)
                        .fmMessageTools(browser.error)
                        .padding(FmSpace.x4)
                }
                FmHairline()
                SettingsRefreshControl(loading: browser.loading, label: "Check again", onRefresh: auth?.session == nil ? nil : {
                    Task { @MainActor in await browser.load(refresh: true) }
                })
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x2)
            }
        }
    }

    @ViewBuilder private var storeCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let items = browser?.inventory?.store, !items.isEmpty {
                ForEach(Array(items.enumerated()), id: \.element.id) { (index: Int, item: AccountStoreItem) in
                    if index > 0 { FmHairline() }
                    FmListRow(
                        title: item.title,
                        subtitle: storeSubtitle(item),
                        leading: FmIcon(storeIcon(item), size: 22, color: fm.textSecondary).fmAny,
                        trailing: FmIconButton(icon: "delete_outline", label: "Delete \(item.title)", size: 20, color: fm.textTertiary,
                                               onPressed: { confirmStore = item }).fmAny,
                        showChevron: false)
                }
                FmHairline()
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmNoticeLine($storeNotice)
                    FmTextButton("Delete everything saved from your phone", icon: "delete_sweep_outlined", onPressed: { confirmStoreAll = true })
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
            } else {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    SettingsEmptyNote(title: "Nothing saved yet", text: "Switch on Save to ForkMesh to keep your layout, subscriptions and settings with your account.")
                    FmNoticeLine($storeNotice)
                }
                .padding(FmSpace.x4)
            }
        }
    }

    private func storeSubtitle(_ item: AccountStoreItem) -> String {
        var parts = [ByteCountFormatter.string(fromByteCount: Int64(item.bytes), countStyle: .file)]
        parts.append("saved " + AccountTime.label(item.updatedAt, nowMillis: SystemClock().nowMillis))
        if !item.device.isEmpty { parts.append("from " + item.device) }
        return parts.joined(separator: " · ")
    }

    private func storeIcon(_ item: AccountStoreItem) -> String {
        switch item.kind {
        case "layout": return "grid_view_outlined"
        case "subscriptions": return "article_outlined"
        default: return "tune"
        }
    }

    private func icon(for subject: AccountDataSubject) -> String {
        switch subject.kind {
        case "account": return "person_outline"
        case "repositories": return "folder_outlined"
        case "organization": return "groups_outlined"
        default: return "storage_outlined"
        }
    }

    @ViewBuilder private var exportCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            SettingsNote(text: "Download everything listed above as one JSON file: your account, devices, SSH public keys, settings saved from your phone, and every item in every section. Password hashes, private keys and other credentials are never included.")
            if let model = model, !model.error.isEmpty {
                SettingsNote(text: model.error, color: fm.danger)
                    .fmMessageTools(model.error)
            }
            FmNoticeLine($exportNotice)
            FmFilledButton(model?.busy == true ? "Preparing…" : "Download all my data", icon: "download_outlined",
                           onPressed: model == nil || model?.busy == true || auth?.session == nil ? nil : {
                exportNotice = nil
                Task { @MainActor in
                    guard let model = model else { return }
                    if let file = await model.prepareExport(account: auth?.session?.nodeName ?? "") {
                        document = JSONTextDocument(text: file.json)
                        exporting = true
                    }
                }
            })
        }
    }

    private var cacheCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmKeyValueRow(label: "Cached responses", value: ByteCountFormatter.string(
                fromByteCount: Int64(URLCache.shared.currentDiskUsage + URLCache.shared.currentMemoryUsage), countStyle: .file))
            SettingsNote(text: "To erase everything for a project, including its sign-in and keys, remove the project from the project switcher.")
            FmNoticeLine($cacheNotice)
            FmOutlinedButton("Clear cached data", icon: "delete_sweep_outlined", onPressed: { confirmClear = true })
        }
    }

    private func clearCaches() {
        URLCache.shared.removeAllCachedResponses()
        cacheNotice = FmNotice("Cached data cleared")
    }
}

@MainActor
struct SecurityReportScreen: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    @State var model: AccountDataModel?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        _model = State(initialValue: api.map { AccountDataModel(client: $0) })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                SettingsNote(text: "Reports go privately to the ForkMesh security team, encrypted at rest. Please do not post vulnerabilities in public issues or chat.")
                if let model = model {
                    if model.reportSent {
                        FmCard(radius: FmRadius.lg) {
                            HStack(alignment: .top, spacing: FmSpace.x3) {
                                FmIcon("verified_outlined", size: 22, color: fm.success)
                                SettingsNote(text: "Thank you. The report was sent. If you left a contact, the team will reply there.")
                                    .fmMessageTools("Thank you. The report was sent. If you left a contact, the team will reply there.")
                            }
                        }
                    }
                    form(model)
                } else {
                    FmTextButton("Email security@forkmesh.com", icon: "open_in_new", onPressed: {
                        if let url = URL(string: "mailto:security@forkmesh.com") { openURL(url) }
                    })
                }
            }
            .padding(FmSpace.x4)
        }
        .scrollDismissesKeyboard(.interactively)
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle("Report a vulnerability")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func form(_ model: AccountDataModel) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            SettingsTextField(label: "Title", text: Binding<String>(get: { model.report.title }, set: { (v: String) in model.report.title = v }))
            FmField(label: "Component") {
                Picker("Component", selection: Binding<String>(get: { model.report.component }, set: { (v: String) in model.report.component = v })) {
                    ForEach(SecurityReportDraft.components.map { $0.id }, id: \.self) { (id: String) in
                        Text(SecurityReportDraft.components.first { $0.id == id }?.label ?? id).tag(id)
                    }
                }
                .pickerStyle(.menu)
            }
            .padding(.bottom, FmSpace.x3)
            FmField(label: "What is wrong, and how to reproduce it") {
                FmTextField(
                    text: Binding<String>(get: { model.report.body }, set: { (v: String) in model.report.body = String(v.prefix(SecurityReportDraft.maxBody)) }),
                    hint: "Steps, affected URL or version, impact", multiline: true)
            }
            .padding(.bottom, FmSpace.x3)
            SettingsTextField(label: "Contact (optional)", text: Binding<String>(get: { model.report.contact }, set: { (v: String) in model.report.contact = v }))
            if !model.error.isEmpty {
                SettingsNote(text: model.error, color: fm.danger)
                    .fmMessageTools(model.error)
                    .padding(.bottom, FmSpace.x3)
            }
            FmFilledButton(model.busy ? "Sending…" : "Send report", icon: "verified_user_outlined", fillWidth: true,
                           onPressed: model.busy ? nil : { Task { @MainActor in await model.sendReport() } })
        }
    }
}
