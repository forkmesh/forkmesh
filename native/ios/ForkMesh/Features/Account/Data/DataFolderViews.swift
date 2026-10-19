import FMAccount
import FMBase
import SwiftUI
import UniformTypeIdentifiers

struct DataFolderAlertModifier: ViewModifier {
    @State private var model: DataFolderModel = DataFolderModel.shared
    @Environment(\.scenePhase) private var scenePhase

    func body(content: Content) -> some View {
        content
            .alert(DataFolder.alertTitle, isPresented: Binding(
                get: { model.showsAlert },
                set: { (shown: Bool) in if !shown && !model.showSetup { model.dismissedThisLaunch = true } }
            )) {
                Button("Set up") { model.showSetup = true }
                Button("Not now", role: .cancel) { model.dismissedThisLaunch = true }
            } message: {
                Text(DataFolder.message(for: model.need ?? .chooseFolder))
            }
            .sheet(isPresented: Binding(get: { model.showSetup }, set: { model.showSetup = $0 })) {
                DataFolderSetupSheet(model: model)
            }
            .task {
                await model.pickAutomaticFolder()
                await model.sync()
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase == .background { model.syncInBackground() }
            }
    }
}

extension View {
    func dataFolderAlert() -> some View { modifier(DataFolderAlertModifier()) }
}

struct DataFolderSettingsRow: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: DataFolderModel = DataFolderModel.shared
    @State private var showing: Bool = false

    var body: some View {
        Button { showing = true } label: {
            SettingsLinkRow(icon: "folder_open_outlined", title: "Data folder", subtitle: model.summary)
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .sheet(isPresented: $showing) { DataFolderSetupSheet(model: model) }
    }
}

struct DataFolderSetupSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let model: DataFolderModel

    @State private var picking: Bool = false
    @State private var passphrase: String = ""
    @State private var confirm: String = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    SettingsNote(text: DataFolder.alertMessage)
                    SettingsNote(text: model.summary, color: model.reachable ? fm.textPrimary : fm.danger)
                    FmFilledButton(model.chosen ? "Choose another folder" : "Choose a folder in Files",
                                   icon: "folder_open_outlined", fillWidth: true,
                                   onPressed: model.working ? nil : { picking = true })
                    if model.chosen {
                        FmToneButton(label: "Use the automatic folder", icon: "folder_outlined",
                                     onPressed: model.working ? nil : { Task { @MainActor in await model.useAutomaticFolder() } })
                    }
                    if !model.restorable.isEmpty {
                        restoreSection
                    } else if model.need == .saveIdentity {
                        identitySection
                    }
                    if !model.status.isEmpty {
                        SettingsNote(text: model.status, color: model.statusIsError ? fm.danger : fm.textSecondary)
                            .fmMessageTools(model.status)
                    }
                    if model.needsRestart {
                        FmFilledButton("Close ForkMesh", icon: "check", fillWidth: true, onPressed: { model.closeToFinish() })
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Data folder")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(model.need == nil ? "Done" : "Not now") {
                        model.dismissedThisLaunch = true
                        dismiss()
                    }
                }
            }
            .fileImporter(isPresented: $picking, allowedContentTypes: [UTType.folder]) { (result: Result<URL, Error>) in
                if case .success(let url) = result {
                    Task { @MainActor in await model.choose(url) }
                }
            }
        }
    }

    @ViewBuilder
    private var restoreSection: some View {
        SettingsNote(text: "Found \(model.restorable.map(\.name).filter { !$0.isEmpty }.joined(separator: ", ")). Enter the passphrase you saved the identity key with.")
        SettingsTextField(label: "Passphrase", text: $passphrase, secure: true)
        FmFilledButton(model.working ? "Restoring…" : "Restore", icon: "history", fillWidth: true,
                       onPressed: model.working ? nil : {
                           let secret: String = passphrase
                           Task { @MainActor in
                               await model.restore(passphrase: secret)
                               if model.needsRestart { passphrase = "" }
                           }
                       })
        FmToneButton(label: "Keep this install's data instead", icon: "folder_outlined",
                     onPressed: model.working ? nil : { Task { @MainActor in await model.startFresh() } })
    }

    @ViewBuilder
    private var identitySection: some View {
        SettingsNote(text: "Save your identity key in the folder, encrypted with a passphrase of at least \(RecoveryKit.minPassphrase) characters. Nobody can recover the passphrase for you.")
        SettingsTextField(label: "Passphrase", text: $passphrase, secure: true)
        SettingsTextField(label: "Passphrase again", text: $confirm, secure: true)
        FmFilledButton(model.working ? "Encrypting…" : "Save identity key", icon: "save_outlined", fillWidth: true,
                       onPressed: model.working ? nil : {
                           let secret: String = passphrase
                           let again: String = confirm
                           Task { @MainActor in
                               await model.saveIdentity(passphrase: secret, confirm: again)
                               if model.need != .saveIdentity {
                                   passphrase = ""
                                   confirm = ""
                               }
                           }
                       })
    }
}
