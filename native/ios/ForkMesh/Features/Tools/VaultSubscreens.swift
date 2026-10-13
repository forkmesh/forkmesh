// The Password Vault's share screen and settings screen. Logic: FMVault's VaultShareModel and
// VaultSettingsModel.

import FMBase
import FMVault
import SwiftUI

struct VaultShareScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: VaultShareModel
    @State private var stopping: VaultShare?

    init(vault: VaultService, itemId: String) {
        _model = State(initialValue: VaultShareModel(vault: vault, itemId: itemId))
    }

    var body: some View {
        content
            .background(fm.bgBase)
            .navigationBarTitleDisplayMode(.inline)
            .task { await model.start() }
            .fmDialog(isPresented: stopShown) { stopDialog }
    }

    @ViewBuilder private var content: some View {
        if let item = model.item {
            VaultShareForm(model: model, item: item, onStop: { (share: VaultShare) in stopping = share })
                .navigationTitle(model.screenTitle)
        } else {
            FmEmptyState(icon: "lock_outline", title: VaultShareModel.lockedTitle)
        }
    }

    private var stopShown: Binding<Bool> {
        Binding<Bool>(get: { stopping != nil }, set: { (shown: Bool) in
            if !shown {
                stopping = nil
            }
        })
    }

    @ViewBuilder private var stopDialog: some View {
        if let share = stopping {
            let words: VaultConfirmation = model.stopConfirmation(share)
            FmConfirmDialog(
                title: words.title, message: words.message, confirmLabel: words.confirm, destructive: words.destructive,
                onCancel: { stopping = nil },
                onConfirm: {
                    stopping = nil
                    Task { await model.stop(share) }
                })
        }
    }
}

private struct VaultShareForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultShareModel
    let item: VaultItem
    let onStop: (VaultShare) -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(VaultShareModel.intro)
                    .fmText(.bodySmall, color: fm.textSecondary)
                currentShares
                kindPicker
                    .padding(.top, FmSpace.x1)
                target
                Picker("", selection: $model.permission) {
                    Text(VaultShareModel.readSegment).tag("read")
                    Text(VaultShareModel.writeSegment).tag("write")
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                FmOutlinedButton(VaultShareModel.checkKeys, fillWidth: true, onPressed: model.canCheck ? {
                    Task { await model.check() }
                } : nil)
                .accessibilityIdentifier("vault-share-check")
                if let preview = model.preview {
                    previewSection(preview)
                }
                if !model.message.isEmpty {
                    Text(model.message)
                        .fmText(.bodyMedium)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("vault-share-message")
                        .fmMessageTools(model.message)
                }
            }
            .padding(FmSpace.x4)
        }
    }

    private var currentShares: some View {
        FmCard(padding: EdgeInsets()) {
            VStack(spacing: 0) {
                if item.shares.isEmpty {
                    FmListRow(title: VaultShareModel.notShared, showChevron: false)
                }
                ForEach(item.shares, id: \.self) { (share: VaultShare) in
                    FmListRow(
                        title: model.shareTitle(share), subtitle: model.shareSubtitle(share),
                        trailing: FmTextButton(VaultShareModel.stopSharing, color: fm.danger, onPressed: model.busy ? nil : {
                            onStop(share)
                        }).fmAny,
                        showChevron: false)
                        .accessibilityIdentifier("vault-unshare-\(share.kind)-\(share.name)")
                }
            }
        }
    }

    private var kindPicker: some View {
        Picker("", selection: kindBinding) {
            Text(VaultShareModel.userSegment).tag("user")
            if model.organizationsAvailable {
                Text(VaultShareModel.organizationSegment).tag("organization")
            }
        }
        .pickerStyle(.segmented)
        .labelsHidden()
    }

    private var kindBinding: Binding<String> {
        Binding<String>(get: { model.kind }, set: { (value: String) in
            if value == "user" || model.organizationsAvailable {
                model.kind = value
            }
        })
    }

    @ViewBuilder private var target: some View {
        if model.kind == "user" {
            ToolsLabeledField(label: VaultShareModel.usernameLabel, text: $model.name)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .accessibilityIdentifier("vault-share-name")
        } else {
            FmField(label: VaultShareModel.organizationLabel) {
                Picker("", selection: $model.organization) {
                    ForEach(model.organizations, id: \.self) { (name: String) in
                        Text(name).tag(name)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
        }
    }

    @ViewBuilder
    private func previewSection(_ preview: [VaultRecipient]) -> some View {
        FmCard(padding: EdgeInsets()) {
            VStack(spacing: 0) {
                if preview.isEmpty {
                    FmListRow(title: VaultShareModel.nobodyElse, showChevron: false)
                }
                ForEach(Array(preview.enumerated()), id: \.offset) { (pair: (offset: Int, element: VaultRecipient)) in
                    recipientRow(pair.element)
                }
            }
        }
        Text(model.advice)
            .fmText(.bodySmall, color: fm.textSecondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .fmMessageTools(model.advice)
        if !model.changed.isEmpty {
            Toggle(VaultShareModel.confirmChangedLabel, isOn: $model.confirmedChanged)
                .fmText(.bodyMedium)
                .accessibilityIdentifier("vault-share-confirm-changed")
        }
        FmFilledButton(VaultShareModel.shareButton, fillWidth: true, onPressed: model.canShare ? {
            Task { await model.share() }
        } : nil)
        .accessibilityIdentifier("vault-share-submit")
    }

    private func recipientRow(_ member: VaultRecipient) -> some View {
        let alarming: Bool = model.isAlarming(member.state)
        let fingerprint: String? = model.fingerprintShown(member)
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(spacing: FmSpace.x2) {
                Text(member.name)
                    .fmText(.titleSmall)
                FmStatusBadge(label: model.stateLabel(member.state), color: alarming ? fm.danger : fm.textSecondary)
            }
            if let fingerprint = fingerprint {
                Text(fingerprint)
                    .fmMono(size: FmType.meta)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .accessibilityIdentifier("vault-recipient-\(member.name)")
    }
}

struct VaultSettingsScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var model: VaultSettingsModel
    @State private var transfer: VaultTransferModel
    @State private var confirmDestroy: Bool = false

    init(vault: VaultService, clipboard: VaultClipboard) {
        _model = State(initialValue: VaultSettingsModel(vault: vault, clipboard: clipboard))
        _transfer = State(initialValue: VaultTransferModel(vault: vault))
    }

    var body: some View {
        VaultSettingsForm(model: model, transfer: transfer, onDestroy: { confirmDestroy = true })
            .background(fm.bgBase)
            .navigationTitle(VaultSettingsModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .task { await model.start() }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                // Back from the system's autofill settings: read the choice again.
                if phase == .active {
                    Task { await model.appResumed() }
                }
            }
            .onChange(of: model.shouldClose) { (_: Bool, close: Bool) in
                // Locked underneath (idle timer): nothing here may stay on screen.
                if close {
                    dismiss()
                }
            }
            .vaultTransferDialogs(transfer)
            .fmDialog(isPresented: $confirmDestroy) { destroyDialog }
            .fmDialog(isPresented: recoveryShown, barrierDismissible: false) { recoveryDialog }
    }

    private var destroyDialog: some View {
        let words: VaultConfirmation = model.destroyConfirmation
        return FmConfirmDialog(
            title: words.title, message: words.message, confirmLabel: words.confirm, destructive: words.destructive,
            onCancel: { confirmDestroy = false },
            onConfirm: {
                confirmDestroy = false
                Task { await model.destroy() }
            })
    }

    private var recoveryShown: Binding<Bool> {
        Binding<Bool>(get: { model.recoveryPrompt != nil }, set: { (shown: Bool) in
            if !shown {
                model.finishRecoveryPrompt()
            }
        })
    }

    @ViewBuilder private var recoveryDialog: some View {
        if let prompt = model.recoveryPrompt {
            VaultRecoveryCodeDialog(prompt: prompt, onDone: { model.finishRecoveryPrompt() })
        }
    }
}

private struct VaultSettingsForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultSettingsModel
    let transfer: VaultTransferModel
    let onDestroy: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                autofill
                VaultTransferSection(model: transfer)
                    .padding(.top, FmSpace.x3)
                locking
                    .padding(.top, FmSpace.x3)
                keys
                    .padding(.top, FmSpace.x3)
                if !model.message.isEmpty {
                    Text(model.message)
                        .fmText(.bodyMedium)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("vault-settings-message")
                        .fmMessageTools(model.message)
                        .padding(.vertical, FmSpace.x3)
                }
                FmTextButton(VaultSettingsModel.deleteVaultButton, color: fm.danger, onPressed: onDestroy)
                    .padding(.top, FmSpace.x3)
                    .accessibilityIdentifier("vault-destroy")
            }
            .padding(FmSpace.x4)
        }
    }

    @ViewBuilder private var autofill: some View {
        FmSectionHeader(title: VaultSettingsModel.autofillHeader)
        if !model.autofill.supported {
            Text(VaultSettingsModel.autofillUnsupported)
                .fmText(.bodySmall, color: fm.textSecondary)
        } else {
            Toggle(isOn: autofillBinding) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(VaultSettingsModel.autofillToggle)
                        .fmText(.bodyLarge)
                    Text(model.autofillSubtitle)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .disabled(!model.autofillToggleEnabled)
            .accessibilityIdentifier("vault-autofill-toggle")
            if let state = model.autofillState {
                Text(state)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(state)
            }
            if model.showsOpenAutofillSettings {
                FmTextButton(VaultSettingsModel.openAutofillSettingsButton, onPressed: {
                    Task { await model.openAutofillSettings() }
                })
                .accessibilityIdentifier("vault-autofill-settings")
            }
        }
    }

    private var autofillBinding: Binding<Bool> {
        Binding<Bool>(get: { model.vault.autofillEnabled }, set: { (on: Bool) in
            Task { await model.setAutofill(on) }
        })
    }

    @ViewBuilder private var locking: some View {
        FmSectionHeader(title: VaultSettingsModel.lockingHeader)
        FmField(label: VaultSettingsModel.lockAfterLabel) {
            Picker("", selection: lockBinding) {
                ForEach(model.autoLockChoices, id: \.self) { (minutes: Int) in
                    Text(model.autoLockLabel(minutes)).tag(minutes)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .accessibilityIdentifier("vault-autolock")
        }
        Text(VaultSettingsModel.lockingNote)
            .fmText(.bodySmall, color: fm.textSecondary)
        if model.showsQuickUnlock {
            Toggle(isOn: quickUnlockBinding) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(VaultSettingsModel.quickUnlockToggle)
                        .fmText(.bodyLarge)
                    Text(VaultSettingsModel.quickUnlockSubtitle)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .accessibilityIdentifier("vault-quick-unlock-toggle")
        }
    }

    private var lockBinding: Binding<Int> {
        Binding<Int>(get: { model.vault.autoLockMinutes }, set: { (minutes: Int) in
            Task { await model.setAutoLock(minutes) }
        })
    }

    private var quickUnlockBinding: Binding<Bool> {
        Binding<Bool>(get: { model.vault.quickUnlockEnabled }, set: { (on: Bool) in
            Task { await model.setQuickUnlock(on) }
        })
    }

    @ViewBuilder private var keys: some View {
        FmSectionHeader(title: VaultSettingsModel.fingerprintHeader)
        Text(model.fingerprint)
            .fmMono(size: FmType.meta)
            .textSelection(.enabled)
            .accessibilityIdentifier("vault-fingerprint")
        Text(VaultSettingsModel.fingerprintNote)
            .fmText(.bodySmall, color: fm.textSecondary)
        FmSectionHeader(title: VaultSettingsModel.masterPasswordHeader)
            .padding(.top, FmSpace.x3)
        VaultSecretField(label: VaultCopy.newMasterPasswordLabel, text: $model.password)
        VaultSecretField(label: VaultCopy.typeItAgainLabel, text: $model.confirm)
        FmOutlinedButton(VaultSettingsModel.changePasswordButton, fillWidth: true, onPressed: {
            Task { await model.changePassword() }
        })
        FmOutlinedButton(VaultSettingsModel.newRecoveryCodeButton, fillWidth: true, onPressed: {
            Task { await model.renewRecoveryCode() }
        })
        FmOutlinedButton(VaultSettingsModel.copyExportButton, fillWidth: true, onPressed: {
            Task { await model.copyExport() }
        })
    }
}
