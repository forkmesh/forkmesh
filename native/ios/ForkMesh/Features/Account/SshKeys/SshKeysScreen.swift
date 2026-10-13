import FMAccount
import FMApi
import FMBase
import SwiftUI
import UIKit

@MainActor
struct SshKeysScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: SshKeysModel?
    @State var confirmRemove: SshKeyRow?
    @State var listNotice: FmNotice?
    @State var addNotice: FmNotice?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        _model = State(initialValue: api.map { SshKeysModel(client: $0) })
    }

    var body: some View {
        Group {
            if let model = model {
                page(model)
            } else {
                FmEmptyState(icon: "key_outlined", title: "SSH keys", message: "Sign in to manage your SSH keys.")
            }
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle("SSH keys")
        .navigationBarTitleDisplayMode(.inline)
        .fmDialog(isPresented: removeShown) {
            if let row = confirmRemove {
                FmConfirmDialog(
                    title: "Remove \(row.title)?",
                    message: "Git over SSH stops accepting this key at once. You cannot add the same key again; make a fresh key pair instead.",
                    confirmLabel: "Remove",
                    destructive: true,
                    onCancel: { confirmRemove = nil },
                    onConfirm: {
                        confirmRemove = nil
                        Task { @MainActor in
                            if await model?.remove(row.id) == true {
                                listNotice = FmNotice("Key removed")
                            }
                        }
                    })
            }
        }
    }

    private var removeShown: Binding<Bool> {
        Binding<Bool>(get: { confirmRemove != nil }, set: { (shown: Bool) in if !shown { confirmRemove = nil } })
    }

    private func page(_ model: SshKeysModel) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                SettingsSection(title: "Your keys") {
                    SettingsCardBox(padded: false) {
                        VStack(alignment: .leading, spacing: 0) {
                            keyList(model)
                            if listNotice != nil {
                                FmHairline()
                                FmNoticeLine($listNotice)
                                    .padding(.horizontal, FmSpace.x4)
                                    .padding(.vertical, FmSpace.x3)
                            }
                        }
                    }
                }
                SettingsSection(title: "Add a public key") {
                    SettingsCardBox { addForm(model) }
                }
                SettingsSection(title: "About", last: true) {
                    SettingsCardBox {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            SettingsNote(text: model.page?.gatewayLine ?? "")
                            SettingsNote(text: "ForkMesh stores public keys only. Keep every private key and its passphrase on the device that made it.")
                        }
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .scrollDismissesKeyboard(.interactively)
        .fmRefreshable { await model.load() }
        .task { await model.load() }
    }

    @ViewBuilder
    private func keyList(_ model: SshKeysModel) -> some View {
        if model.loading && model.page == nil {
            FmSkeletonList(rows: 2, label: "Loading keys")
        } else if let page = model.page, !page.keys.isEmpty {
            VStack(spacing: 0) {
                ForEach(Array(page.keys.enumerated()), id: \.element.id) { (index: Int, row: SshKeyRow) in
                    if index > 0 { FmHairline() }
                    FmListRow(
                        title: row.title,
                        subtitle: "\(row.fingerprint) · used \(AccountTime.label(row.lastUsedAt, nowMillis: SystemClock().nowMillis))",
                        leading: FmIcon("key_outlined", size: 22, color: fm.textSecondary).fmAny,
                        trailing: FmIconButton(icon: "delete_outline", label: "Remove key", size: 20, color: fm.textTertiary,
                                               onPressed: { confirmRemove = row }).fmAny,
                        showChevron: false)
                }
            }
        } else {
            SettingsEmptyNote(
                title: model.error.isEmpty ? "No SSH keys yet" : "Keys unavailable",
                text: model.error.isEmpty ? "Add the public half of a key to push and pull over SSH." : model.error)
                .fmMessageTools(model.error, alignment: .top)
                .padding(FmSpace.x4)
        }
    }

    private func addForm(_ model: SshKeysModel) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            FmField(label: "Public key (.pub)") {
                FmTextField(text: bind(model, \.draftKey), hint: "ssh-ed25519 AAAA… you@laptop", multiline: true, mono: true)
            }
            .padding(.bottom, FmSpace.x3)
            SettingsTextField(label: "Label (optional)", text: bind(model, \.draftLabel))
            if !draftProblem(model).isEmpty {
                SettingsNote(text: draftProblem(model), color: fm.danger)
                    .fmMessageTools(draftProblem(model))
                    .padding(.bottom, FmSpace.x3)
            }
            if !model.error.isEmpty && model.page != nil {
                SettingsNote(text: model.error, color: fm.danger)
                    .fmMessageTools(model.error)
                    .padding(.bottom, FmSpace.x3)
            }
            if model.page?.atLimit == true {
                SettingsNote(text: "This account has \(model.page?.maximumKeys ?? 0) keys, the most allowed. Remove one to add another.")
                    .fmMessageTools("This account has \(model.page?.maximumKeys ?? 0) keys, the most allowed. Remove one to add another.")
                    .padding(.bottom, FmSpace.x3)
            }
            if addNotice != nil {
                FmNoticeLine($addNotice)
                    .padding(.bottom, FmSpace.x3)
            }
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton("Paste", icon: "content_paste", onPressed: {
                    if let text = UIPasteboard.general.string { model.draftKey = String(text.prefix(SshKeys.maxLineBytes)) }
                })
                FmFilledButton(model.busy ? "Adding…" : "Add key", icon: "add", onPressed: model.canAdd ? {
                    Task { @MainActor in
                        if await model.add() { addNotice = FmNotice("Key added") }
                    }
                } : nil)
            }
        }
    }

    private func draftProblem(_ model: SshKeysModel) -> String {
        model.draftKey.isEmpty ? "" : SshKeys.message(model.draftCheck)
    }

    private func bind(_ model: SshKeysModel, _ path: ReferenceWritableKeyPath<SshKeysModel, String>) -> Binding<String> {
        Binding<String>(get: { model[keyPath: path] }, set: { (value: String) in model[keyPath: path] = value })
    }
}
