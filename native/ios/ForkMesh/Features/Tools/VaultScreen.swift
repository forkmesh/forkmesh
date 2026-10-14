// The Password Vault's main screen (setup, unlock, recovery, the list and an item's actions and
// sheet). The logic is FMVault's VaultScreenModel (docs/FMVault.md); the share and settings screens
// are in VaultSubscreens.swift. The VaultService is AppServices'; the clipboard is ToolsFeature's
// PasteboardVaultClipboard.

import FMBase
import FMVault
import SwiftUI

struct VaultScreen: View {
    @State private var model: VaultScreenModel?

    init() {
        if let vault = Services.get(VaultService.self) {
            let clipboard: VaultClipboard = Services.get(VaultClipboard.self) ?? PasteboardVaultClipboard()
            _model = State(initialValue: VaultScreenModel(vault: vault, clipboard: clipboard))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            VaultMain(model: model)
        } else {
            FmEmptyState(icon: "lock_outline", title: VaultCopy.signedOutTitle, message: VaultCopy.signedOutMessage)
                .navigationTitle(VaultCopy.title)
        }
    }
}

private struct VaultMain: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    @Bindable var model: VaultScreenModel
    @State private var actionsFor: VaultItem?
    @State private var draft: ToolsSheet<VaultItemDraft>?
    @State private var recovering: ToolsSheet<VaultRecoverForm>?
    @State private var deleting: VaultItem?
    @State private var shareItemId: String?
    @State private var settingsOpen: Bool = false
    @State private var transfer: VaultTransferModel

    init(model: VaultScreenModel) {
        _model = Bindable(model)
        _transfer = State(initialValue: VaultTransferModel(vault: model.vault))
    }

    var body: some View {
        VStack(spacing: 0) {
            if model.busy || transfer.busy {
                FmLinearProgressIndicator()
                    .frame(height: 2)
            }
            if !model.error.isEmpty {
                VaultBanner(text: model.error, color: fm.danger)
                    .accessibilityIdentifier("vault-error")
            }
            if !model.notice.isEmpty {
                VaultBanner(text: model.notice, color: fm.accent)
                    .accessibilityIdentifier("vault-notice")
            }
            if !transfer.message.isEmpty {
                VaultBanner(text: transfer.message, color: fm.accent)
                    .accessibilityIdentifier("vault-transfer-notice")
            }
            phaseBody
                .frame(maxHeight: .infinity)
        }
        .background(fm.canvas)
        .navigationTitle(VaultCopy.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbar }
        .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) { addButton }
        // Any touch on the screen pushes the idle lock out again.
        .simultaneousGesture(TapGesture().onEnded { model.touch() })
        .task { await model.start() }
        .navigationDestination(isPresented: $settingsOpen) {
            VaultSettingsScreen(vault: model.vault, clipboard: model.clipboard)
                .shellPageClearance()
        }
        .navigationDestination(isPresented: shareShown) {
            if let id = shareItemId {
                VaultShareScreen(vault: model.vault, itemId: id)
                    .shellPageClearance()
            }
        }
        .sheet(item: $actionsFor) { (item: VaultItem) in
            VaultItemActionsSheet(model: model, item: item) { (action: VaultItemAction) in
                actionsFor = nil
                perform(action, item)
            }
        }
        .sheet(item: $draft) { (box: ToolsSheet<VaultItemDraft>) in
            VaultItemSheet(draft: box.value) {
                draft = nil
                Task { await model.save(box.value) }
            }
        }
        .sheet(item: $recovering) { (box: ToolsSheet<VaultRecoverForm>) in
            VaultRecoverSheet(form: box.value) { (result: (code: String, password: String)) in
                recovering = nil
                Task { await model.recover(code: result.code, newPassword: result.password) }
            }
        }
        .onChange(of: incomingReady, initial: true) { (_: Bool, ready: Bool) in
            if ready {
                transfer.takeIncoming()
            }
        }
        .vaultTransferDialogs(transfer)
        .fmDialog(isPresented: deleteShown) { deleteDialog }
        .fmDialog(isPresented: recoveryShown, barrierDismissible: false) { recoveryDialog }
    }

    private var incomingReady: Bool {
        model.phase == .unlocked && model.vault.incomingImport != nil
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            if model.showsUnlockedActions {
                FmIconButton(icon: "lock_outline", label: VaultCopy.lockTooltip, onPressed: { model.lock() })
                    .accessibilityIdentifier("vault-lock")
                FmIconButton(icon: "settings_outlined", label: VaultCopy.settingsTooltip, onPressed: { settingsOpen = true })
                    .accessibilityIdentifier("vault-settings")
            }
        }
    }

    @ViewBuilder private var addButton: some View {
        if model.showsUnlockedActions {
            FmFloatingActionButton(icon: "add", accessibilityLabel: VaultCopy.addTooltip, onPressed: {
                draft = ToolsSheet(model.draft(for: nil))
            })
            .padding(FmSpace.x4)
            .accessibilityIdentifier("vault-add")
        }
    }

    @ViewBuilder private var phaseBody: some View {
        switch model.phase {
        case .loading:
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        case .signedOut:
            FmEmptyState(icon: "lock_outline", title: VaultCopy.signedOutTitle, message: VaultCopy.signedOutMessage)
        case .notSetUp:
            VaultSetupForm(model: model)
        case .locked:
            VaultUnlockForm(model: model, onRecover: { recovering = ToolsSheet(VaultRecoverForm()) })
        case .unlocked:
            VaultList(model: model, onOpen: { (item: VaultItem) in
                if item.readable {
                    actionsFor = item
                }
            })
        }
    }

    private func perform(_ action: VaultItemAction, _ item: VaultItem) {
        switch action {
        case .username:
            model.copyUsername(item)
        case .password:
            model.copyPassword(item)
        case .open:
            if let url = model.siteURL(item) {
                openURL(url)
            }
        case .edit:
            draft = ToolsSheet(model.draft(for: item))
        case .share:
            shareItemId = item.id
        case .delete:
            deleting = item
        }
    }

    // MARK: Dialogs

    private var shareShown: Binding<Bool> {
        Binding<Bool>(get: { shareItemId != nil }, set: { (shown: Bool) in
            if !shown {
                shareItemId = nil
            }
        })
    }

    private var deleteShown: Binding<Bool> {
        Binding<Bool>(get: { deleting != nil }, set: { (shown: Bool) in
            if !shown {
                deleting = nil
            }
        })
    }

    @ViewBuilder private var deleteDialog: some View {
        if let item = deleting {
            let words: VaultConfirmation = model.deleteConfirmation(item)
            FmConfirmDialog(
                title: words.title, message: words.message, confirmLabel: words.confirm, destructive: words.destructive,
                onCancel: { deleting = nil },
                onConfirm: {
                    deleting = nil
                    Task { await model.delete(item) }
                })
        }
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

struct VaultBanner: View {
    @Environment(\.fmPalette) private var fm
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .fmText(.bodySmall, color: fm.textPrimary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .fmMessageTools(text)
            .padding(FmSpace.x3)
            .overlay { FmRoundedShape(radius: 8).strokeBorder(color, lineWidth: 1) }
            .padding(.horizontal, FmSpace.x4)
            .padding(.top, FmSpace.x2)
    }
}

/// A password field that stays out of keyboards' learned words and offers no suggestions, and is
/// never autofilled by the system: this is the password manager.
struct VaultSecretField: View {
    let label: String
    @Binding var text: String
    var reveal: Bool = false
    var readOnly: Bool = false
    var onSubmit: (() -> Void)? = nil

    var body: some View {
        FmField(label: label) {
            FmTextField(text: $text, secure: !reveal, onSubmit: onSubmit)
                .textContentType(nil)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .disabled(readOnly)
        }
    }
}

private struct VaultSetupForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultScreenModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(VaultCopy.setupTitle)
                    .fmText(.titleMedium)
                Text(VaultCopy.setupMessage)
                    .fmText(.bodySmall, color: fm.textSecondary)
                VaultSecretField(label: VaultCopy.masterPasswordLabel, text: $model.setupPassword)
                    .accessibilityIdentifier("vault-setup-password")
                VaultSecretField(label: VaultCopy.typeItAgainLabel, text: $model.setupConfirm, onSubmit: submit)
                    .accessibilityIdentifier("vault-setup-confirm")
                if !model.setupProblem.isEmpty {
                    Text(model.setupProblem)
                        .fmText(.bodyMedium, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("vault-setup-problem")
                        .fmMessageTools(model.setupProblem)
                }
                FmFilledButton(VaultCopy.createVault, fillWidth: true, onPressed: model.busy ? nil : submit)
                    .padding(.top, FmSpace.x2)
                    .accessibilityIdentifier("vault-setup-submit")
            }
            .padding(FmSpace.x4)
        }
    }

    private func submit() {
        Task { await model.submitSetup() }
    }
}

private struct VaultUnlockForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultScreenModel
    let onRecover: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(VaultCopy.unlockTitle)
                    .fmText(.titleMedium)
                Text(VaultCopy.unlockMessage)
                    .fmText(.bodySmall, color: fm.textSecondary)
                VaultSecretField(label: VaultCopy.masterPasswordLabel, text: $model.unlockPassword, onSubmit: unlock)
                    .accessibilityIdentifier("vault-unlock-password")
                FmFilledButton(VaultCopy.unlock, fillWidth: true, onPressed: model.busy ? nil : unlock)
                    .padding(.top, FmSpace.x2)
                    .accessibilityIdentifier("vault-unlock-submit")
                if model.quickUnlockOffered {
                    FmOutlinedButton(VaultCopy.unlockWithDevice, icon: "fingerprint", fillWidth: true, onPressed: model.busy ? nil : {
                        Task { await model.quickUnlock() }
                    })
                    .accessibilityIdentifier("vault-quick-unlock")
                }
                FmTextButton(VaultCopy.forgotIt, onPressed: model.busy ? nil : onRecover)
                    .accessibilityIdentifier("vault-recover")
            }
            .padding(FmSpace.x4)
        }
    }

    private func unlock() {
        Task { await model.submitUnlock() }
    }
}

private struct VaultList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultScreenModel
    let onOpen: (VaultItem) -> Void

    var body: some View {
        VStack(spacing: FmSpace.x2) {
            FmTextField(text: $model.search, hint: VaultCopy.searchHint)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x3)
                .accessibilityIdentifier("vault-search")
            Picker("", selection: $model.filter) {
                ForEach(VaultFilter.allCases, id: \.self) { (filter: VaultFilter) in
                    Text(filter.label).tag(filter)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, FmSpace.x4)
            list
        }
    }

    @ViewBuilder private var list: some View {
        let items: [VaultItem] = model.visibleItems
        if items.isEmpty {
            ScrollView {
                FmEmptyState(icon: "key_outlined", title: VaultCopy.emptyTitle, message: VaultCopy.emptyMessage)
            }
            .fmRefreshable { await model.reload() }
        } else {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(items.enumerated()), id: \.element.id) { (pair: (offset: Int, element: VaultItem)) in
                        if pair.offset > 0 {
                            FmRowDivider(indent: FmSpace.x4)
                        }
                        row(pair.element)
                    }
                }
                .padding(.bottom, 96)
            }
            .fmRefreshable { await model.reload() }
        }
    }

    private func row(_ item: VaultItem) -> some View {
        let row: VaultRow = model.row(item)
        let tint: Color = row.readable ? fm.textSecondary : fm.warning
        let trailing: AnyView? = row.readable
            ? FmIconButton(icon: "copy_outlined", label: VaultCopy.copyPasswordTooltip, onPressed: { model.copyPassword(item) })
                .accessibilityIdentifier("vault-copy-\(item.id)").fmAny
            : nil
        return FmListRow(
            title: row.title, subtitle: row.subtitle,
            leading: FmIcon(row.icon, size: 24, color: tint).fmAny,
            trailing: trailing,
            onTap: row.readable ? { onOpen(item) } : nil,
            showChevron: false)
            .accessibilityIdentifier("vault-item-\(item.id)")
    }
}

private struct VaultItemActionsSheet: View {
    @Environment(\.fmPalette) private var fm
    let model: VaultScreenModel
    let item: VaultItem
    let onAction: (VaultItemAction) -> Void

    var body: some View {
        let header: (title: String, subtitle: String) = model.actionsHeader(item)
        return VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 2) {
                Text(header.title)
                    .fmText(.titleMedium)
                if !header.subtitle.isEmpty {
                    Text(header.subtitle)
                        .fmText(.bodySmall)
                }
            }
            .padding(FmSpace.x4)
            ForEach(model.actions(for: item)) { (entry: VaultItemActionEntry) in
                FmListRow(
                    title: entry.label,
                    leading: FmIcon(entry.icon, size: 24, color: entry.destructive ? fm.danger : fm.textSecondary).fmAny,
                    onTap: { onAction(entry.action) },
                    showChevron: false,
                    titleColor: entry.destructive ? fm.danger : nil)
                    .accessibilityIdentifier("vault-action-\(entry.action.rawValue)")
            }
        }
        .padding(.bottom, FmSpace.x3)
        .fmSheetStyle()
    }
}

private struct VaultItemSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var draft: VaultItemDraft
    let onSave: () -> Void

    private func limited(_ field: String, _ get: @escaping () -> String, _ set: @escaping (String) -> Void) -> Binding<String> {
        let limit: Int = VaultItemDraft.maxLength(field)
        return Binding<String>(get: get, set: { (value: String) in set(String(value.prefix(limit))) })
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(draft.sheetTitle)
                    .fmText(.titleMedium)
                if let line = draft.sharedByLine {
                    Text(line)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                fields
                if !draft.readOnly {
                    FmFilledButton(VaultCopy.save, fillWidth: true, onPressed: onSave)
                        .padding(.top, FmSpace.x2)
                        .accessibilityIdentifier("vault-item-save")
                }
            }
            .padding(FmSpace.x4)
        }
        .fmSheetStyle(fitsContent: false)
    }

    @ViewBuilder private var fields: some View {
        ToolsLabeledField(label: VaultCopy.titleLabel, text: limited("title", { draft.title }, { draft.title = $0 }))
            .disabled(draft.readOnly)
            .accessibilityIdentifier("vault-field-title")
        ToolsLabeledField(label: VaultCopy.websiteLabel, text: limited("url", { draft.url }, { draft.url = $0 }), hint: VaultCopy.websiteHint)
            .keyboardType(.URL)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled(true)
            .disabled(draft.readOnly)
            .accessibilityIdentifier("vault-field-url")
        ToolsLabeledField(label: VaultCopy.usernameLabel, text: limited("username", { draft.username }, { draft.username = $0 }))
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled(true)
            .disabled(draft.readOnly)
            .accessibilityIdentifier("vault-field-username")
        HStack(alignment: .bottom, spacing: FmSpace.x1) {
            VaultSecretField(
                label: VaultCopy.passwordLabel,
                text: limited("password", { draft.password }, { draft.password = $0 }),
                reveal: draft.reveal, readOnly: draft.readOnly)
                .accessibilityIdentifier("vault-field-password")
            FmIconButton(
                icon: draft.reveal ? "visibility_off_outlined" : "visibility_outlined", label: draft.revealLabel,
                onPressed: { draft.reveal.toggle() })
        }
        if !draft.readOnly {
            FmTextButton(VaultCopy.generatePassword, icon: "casino_outlined", onPressed: { draft.generate() })
                .accessibilityIdentifier("vault-generate")
        }
        ToolsLabeledField(label: VaultCopy.notesLabel, text: limited("notes", { draft.notes }, { draft.notes = $0 }), multiline: true)
            .disabled(draft.readOnly)
            .accessibilityIdentifier("vault-field-notes")
    }
}

private struct VaultRecoverSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var form: VaultRecoverForm
    let onRecover: ((code: String, password: String)) -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(VaultCopy.recoverTitle)
                    .fmText(.titleMedium)
                Text(VaultCopy.recoverMessage)
                    .fmText(.bodySmall, color: fm.textSecondary)
                ToolsLabeledField(label: VaultCopy.recoveryCodeLabel, text: $form.code, multiline: true, mono: true)
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled(true)
                    .accessibilityIdentifier("vault-recover-code")
                VaultSecretField(label: VaultCopy.newMasterPasswordLabel, text: $form.password)
                    .accessibilityIdentifier("vault-recover-password")
                VaultSecretField(label: VaultCopy.typeItAgainLabel, text: $form.confirm)
                    .accessibilityIdentifier("vault-recover-confirm")
                if !form.problem.isEmpty {
                    Text(form.problem)
                        .fmText(.bodyMedium, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(form.problem)
                }
                FmFilledButton(VaultCopy.recover, fillWidth: true, onPressed: {
                    if let result = form.submit() {
                        onRecover(result)
                    }
                })
                .padding(.top, FmSpace.x2)
                .accessibilityIdentifier("vault-recover-submit")
            }
            .padding(FmSpace.x4)
        }
        .fmSheetStyle(fitsContent: false)
    }
}

/// Shown once; not dismissable until the person ticks "saved".
struct VaultRecoveryCodeDialog: View {
    @Bindable var prompt: VaultRecoveryCodePrompt
    let onDone: () -> Void

    var body: some View {
        FmDialog(
            title: VaultCopy.recoveryCodeTitle,
            message: VaultCopy.recoveryCodeMessage,
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text(prompt.code)
                        .fmMono(size: FmType.body)
                        .textSelection(.enabled)
                        .accessibilityIdentifier("vault-recovery-code")
                    Toggle(VaultCopy.recoveryCodeSaved, isOn: $prompt.saved)
                        .accessibilityIdentifier("vault-recovery-saved")
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    FmTextButton(VaultCopy.copy, onPressed: { prompt.copy() })
                    Spacer(minLength: 0)
                    FmFilledButton(VaultCopy.done, onPressed: prompt.canFinish ? onDone : nil)
                        .accessibilityIdentifier("vault-recovery-done")
                }
            })
    }
}
