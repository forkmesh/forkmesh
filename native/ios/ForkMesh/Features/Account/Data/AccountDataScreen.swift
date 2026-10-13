import FMAccount
import FMApi
import FMBase
import SwiftUI
import UniformTypeIdentifiers

@MainActor
struct AccountDataScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: AccountDataModel?
    @State var document: JSONTextDocument?
    @State var exporting: Bool = false
    @State var confirmClear: Bool = false
    @State var exportNotice: FmNotice?
    @State var cacheNotice: FmNotice?

    private let auth: AuthService?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        auth = Services.get(AuthService.self)
        _model = State(initialValue: api.map { AccountDataModel(client: $0) })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                SettingsSection(title: "Export your account") {
                    SettingsCardBox { exportCard }
                }
                SettingsSection(title: "This phone", last: true) {
                    SettingsCardBox { cacheCard }
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase.ignoresSafeArea())
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
    }

    @ViewBuilder private var exportCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            SettingsNote(text: "Download what the server keeps about your account: profile, devices, SSH public keys and notification settings, as JSON. Password hashes and private keys are never included.")
            if let model = model, !model.error.isEmpty {
                SettingsNote(text: model.error, color: fm.danger)
                    .fmMessageTools(model.error)
            }
            FmNoticeLine($exportNotice)
            FmFilledButton(model?.busy == true ? "Preparing…" : "Export account data", icon: "download_outlined",
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
        .background(fm.bgBase.ignoresSafeArea())
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
