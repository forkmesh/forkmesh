// QuickEditScreen and its status bar: edit one file of a repository this device
// has NOT cloned and propose the change as a pull request. Logic: FMCode's
// QuickEditModel / QuickEdit.

import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI

struct QuickEditScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: QuickEditModel
    @State private var caret: CodeCaret = CodeCaret.start
    @State private var editing: Bool = false
    @State private var indent: String = "  "
    @State private var composing: Bool = false
    @State private var title: String = ""
    @State private var summary: String = ""
    @State private var notice: FmNotice?

    /// `ref` is the branch being browsed (the pull request's base, never a
    /// bare commit id); `servedBlobSha` the blob id the tree listing reported,
    /// used only to cross-check the bytes ("" = no claim).
    init(repo: Repository, path: String, ref: String = "", servedBlobSha: String = "") {
        _model = State(initialValue: QuickEditModel(
            repo: repo, path: path, ref: ref, servedBlobSha: servedBlobSha,
            api: Services.require(ApiService.self), inbox: Services.get(InboxService.self)))
    }

    var body: some View {
        VStack(spacing: 0) {
            if notice != nil {
                FmNoticeLine($notice)
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
            ToolbarItem(placement: .topBarTrailing) {
                FmToneButton(
                    label: "Propose", icon: "call_merge", tone: .accent, filled: true, height: 32,
                    onPressed: model.canPropose ? { startPropose() } : nil)
            }
        }
        .task {
            await model.load()
            indent = CodeEditing.detectIndentUnit(model.text)
        }
        .sheet(isPresented: $composing) { composeSheet }
    }

    @ViewBuilder private var content: some View {
        if model.loading {
            FmSkeletonList(rows: 8, hasLeading: false, label: "Loading file")
        } else if let blocker = model.blocker {
            FmEmptyState(
                icon: blocker == .bytesDisagree ? "error_outline" : "text_snippet_outlined",
                title: blocker.title, message: blocker.message)
        } else if let error = model.error {
            FmEmptyState(icon: "error_outline", title: "Could not read file", message: error)
        } else {
            VStack(spacing: 0) {
                CodeEditorView(
                    text: $model.text, language: CodeLang.forPath(model.path), indentUnit: indent,
                    caret: $caret, focused: $editing)
                LocalEditorStatusBar(caret: caret, focused: editing, trailing: "into \(model.base)", dirty: model.dirty)
            }
        }
    }

    private func startPropose() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "propose a change") else { return }
            if let patch = model.buildPatch(), patch.isEmpty {
                // Reachable when an edit is made and then undone by hand.
                notice = FmNotice("Nothing changed in \(model.path).")
                return
            }
            title = model.suggestedTitle
            summary = ""
            composing = true
        }
    }

    private var composeSheet: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Title", text: $title)
                } header: {
                    Text("Title")
                }
                Section {
                    TextField("Description", text: $summary, axis: .vertical)
                        .lineLimit(3...6)
                } header: {
                    Text("Description")
                } footer: {
                    Text("\(model.path) · into \(model.base)")
                }
            }
            .navigationTitle("Propose change")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { composing = false }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Submit") { submit() }
                        .disabled(title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func submit() {
        let subject: String = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if subject.isEmpty { return }
        composing = false
        let account: String = LocalRepoServices.session?.nodeName ?? ""
        let body: String = summary.trimmingCharacters(in: .whitespacesAndNewlines)
        Task { @MainActor in
            let result: QuickEditModel.ProposeResult = await model.propose(title: subject, description: body, account: account)
            switch result {
            case .unchanged(let message):
                notice = FmNotice(message)
            case .failed(let message):
                notice = FmNotice.error(message)
            case .submitted:
                dismiss()
            }
        }
    }
}
