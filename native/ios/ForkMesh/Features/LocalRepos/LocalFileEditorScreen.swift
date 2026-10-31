// LocalFileEditorScreen with its status bar, and PublishRepositorySheet. The
// editor is the Code feature's CodeEditorView; state is FMCode's
// LocalFileEditorModel.

import FMCode
import FMDiff
import SwiftUI

/// Code editor for one worktree file with an explicit Save. `onClose` gets
/// whether anything was saved or deleted, so the repository refreshes.
struct LocalFileEditorScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: LocalFileEditorModel
    @State private var caret: CodeCaret = CodeCaret.start
    @State private var editing: Bool = false
    @State private var indent: String = "  "
    @State private var confirmingDelete: Bool = false
    let onClose: (Bool) -> Void

    init(workspace: GitWorkspace, path: String, onClose: @escaping (Bool) -> Void) {
        _model = State(initialValue: LocalFileEditorModel(workspace: workspace, path: path))
        self.onClose = onClose
    }

    var body: some View {
        VStack(spacing: 0) {
            if let notice = model.notice {
                FmNoticeLine(notice.message, isError: notice.isError, onDismiss: { model.notice = nil })
                    .padding(.horizontal, FmSpace.x3)
            }
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(fm.readingSurface)
        .navigationTitle(model.fileName)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                FmIconButton(icon: "delete_outline", label: "Delete file", onPressed: model.busy ? nil : { startDelete() })
                FmToneButton(
                    label: "Save", icon: "save_outlined", tone: .accent, filled: true, height: 32, busy: model.busy,
                    onPressed: model.canSave ? { startSave() } : nil)
            }
        }
        .task {
            await model.load()
            indent = CodeEditing.detectIndentUnit(model.text)
        }
        .onDisappear { onClose(model.changed) }
        .alert("Delete file?", isPresented: $confirmingDelete) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) { runDelete() }
        } message: {
            Text("\(model.path)\(LocalFileEditorModel.deleteMessageSuffix)")
        }
    }

    @ViewBuilder private var content: some View {
        if let error = model.error {
            FmEmptyState(icon: "error_outline", title: "Could not read file", message: error)
        } else if model.content == nil {
            FmSkeletonList(rows: 8, hasLeading: false, label: "Loading file")
        } else if let title = model.blockedTitle {
            FmEmptyState(
                icon: title == "Binary file" ? "insert_drive_file_outlined" : "text_snippet_outlined",
                title: title, message: model.blockedMessage)
        } else {
            VStack(spacing: 0) {
                CodeEditorView(
                    text: $model.text, language: CodeLang.forPath(model.path), indentUnit: indent,
                    caret: $caret, focused: $editing)
                LocalEditorStatusBar(
                    caret: caret, focused: editing, trailing: "\(indentLabel) · \(CodeLang.forPath(model.path).label)",
                    dirty: model.dirty)
            }
        }
    }

    private var indentLabel: String {
        indent == "\t" ? "Tabs" : "Spaces: \(indent.count)"
    }

    private func startSave() {
        Task { @MainActor in
            guard await AccountFeature.requireLocalAccess(action: "edit files") else { return }
            await model.save()
        }
    }

    private func startDelete() {
        Task { @MainActor in
            guard await AccountFeature.requireLocalAccess(action: "delete files") else { return }
            confirmingDelete = true
        }
    }

    private func runDelete() {
        Task { @MainActor in
            if await model.delete() { dismiss() }
        }
    }
}

/// Editor footer: caret position, line count and a trailing caption.
struct LocalEditorStatusBar: View {
    @Environment(\.fmPalette) private var fm
    let caret: CodeCaret
    let focused: Bool
    let trailing: String
    let dirty: Bool

    private var lines: String {
        "\(caret.lineCount) \(caret.lineCount == 1 ? "line" : "lines")"
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            if dirty {
                Circle()
                    .fill(fm.warning)
                    .frame(width: 7, height: 7)
            }
            Text(focused ? "Ln \(caret.line), Col \(caret.column) · \(lines)" : lines)
                .fmText(.bodySmall, color: fm.textTertiary, weight: .semibold, tabular: true)
                .lineLimit(1)
            Spacer(minLength: 0)
            Text(trailing)
                .fmText(.bodySmall, color: fm.textTertiary, weight: .semibold, tabular: true)
                .lineLimit(1)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(minHeight: 32)
        .shellGlass(Capsule())
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x1)
    }
}

/// What the user chose in PublishRepositorySheet.
struct PublishRequest: Equatable {
    let name: String
    let description: String
    let isPublic: Bool
}

/// Name / description / visibility for a first publication.
///
/// Private is shown but disabled: a private catalog record needs the
/// owner-sealed encrypted mirror this app cannot produce yet (the Worker
/// answers 426). Showing it greyed out with the reason is honest about the gap.
struct PublishRepositorySheet: View {
    @Environment(\.fmPalette) private var fm
    let onDone: (PublishRequest?) -> Void
    @State private var name: String
    @State private var description: String
    @State private var problem: String?

    init(initialName: String, initialDescription: String = "", onDone: @escaping (PublishRequest?) -> Void) {
        self.onDone = onDone
        _name = State(initialValue: initialName)
        _description = State(initialValue: initialDescription)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name on ForkMesh", text: $name)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .onChange(of: name) { (_: String, _: String) in problem = nil }
                    if let problem = problem {
                        Text(problem).fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(problem)
                    }
                } header: {
                    Text("Name on ForkMesh")
                } footer: {
                    Text("Letters, digits, dots, dashes, underscores.")
                }
                Section {
                    TextField("Description (optional)", text: $description, axis: .vertical)
                        .lineLimit(1...3)
                }
                Section {
                    visibilityRow("Public", "Anyone on ForkMesh can clone it.", selected: true, enabled: true)
                    visibilityRow(
                        "Private",
                        "Needs owner-sealed encryption, which this app cannot do yet — publish privately from the desktop node.",
                        selected: false, enabled: false)
                } footer: {
                    Text("Pushes this branch to ForkMesh and publishes a signed catalog record from this device.")
                }
            }
            .navigationTitle("Publish to ForkMesh")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { onDone(nil) }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Publish") { confirm() }
                }
            }
        }
    }

    private func visibilityRow(_ title: String, _ subtitle: String, selected: Bool, enabled: Bool) -> some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon(selected ? "radio_button_checked" : "radio_button_unchecked", size: 20, color: selected ? fm.accent : fm.textDisabled)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).fmText(.titleSmall, color: enabled ? fm.textPrimary : fm.textDisabled)
                Text(subtitle).fmText(.bodySmall, color: enabled ? fm.textSecondary : fm.textDisabled)
            }
        }
    }

    private func confirm() {
        let trimmed: String = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if let found = PublishService.validatePublishName(trimmed) {
            problem = found
            return
        }
        onDone(PublishRequest(
            name: trimmed, description: description.trimmingCharacters(in: .whitespacesAndNewlines), isPublic: true))
    }
}
