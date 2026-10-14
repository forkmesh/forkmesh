// The account's markdown notes (`/api/notes`), one note's editor, its history sheet and its share
// sheet. State: FMTools' NotesListModel, NoteEditorModel, NoteHistoryModel and NoteShareModel.

import FMApi
import FMBase
import FMTools
import SwiftUI

struct NotesScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: NotesListModel?
    @State private var openedId: String?
    @State private var editorOpen: Bool = false

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: NotesListModel(api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(NotesListModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .navigationDestination(isPresented: $editorOpen) {
                NoteEditorScreen(noteId: openedId)
                    .shellPageClearance()
            }
            .onChange(of: editorOpen) { (_: Bool, open: Bool) in
                if !open, let model = model {
                    Task { await model.load() }
                }
            }
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            NotesList(model: model, onOpen: open)
                .task { await model.load() }
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                    }
                }
                .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
                    FmFloatingActionButton(icon: "add", label: "New note", accessibilityLabel: "New note", onPressed: { open(nil) })
                        .padding(FmSpace.x4)
                        .accessibilityIdentifier("notes-new")
                }
        } else {
            FmEmptyState(icon: "sticky_note_2_outlined", title: NotesListModel.unavailableTitle)
        }
    }

    private func open(_ id: String?) {
        openedId = id
        editorOpen = true
    }
}

private struct NotesList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NotesListModel
    let onOpen: (String?) -> Void

    var body: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "sticky_note_2_outlined", title: NotesListModel.unavailableTitle, message: model.error)
        } else if model.notes.isEmpty {
            ScrollView {
                FmEmptyState(icon: "sticky_note_2_outlined", title: NotesListModel.emptyTitle, message: NotesListModel.emptyMessage)
            }
            .fmRefreshable { await model.load() }
        } else {
            list
        }
    }

    private var list: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                FmTextField(text: $model.query, hint: "Search notes")
                    .autocorrectionDisabled(true)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x3)
                    .padding(.bottom, FmSpace.x2)
                    .accessibilityIdentifier("notes-search")
                ForEach(Array(model.rows.enumerated()), id: \.element.id) { (pair: (offset: Int, element: NoteRow)) in
                    if pair.offset > 0 {
                        FmRowDivider()
                    }
                    FmListRow(
                        title: pair.element.title, subtitle: pair.element.subtitle,
                        leading: FmIconTile(icon: pair.element.icon).fmAny,
                        onTap: { onOpen(pair.element.id) })
                        .accessibilityIdentifier("note-\(pair.element.id)")
                }
            }
            .padding(.bottom, 96)
        }
        .fmRefreshable { await model.load() }
    }
}

/// One note: read it, edit it (owner or editor), and for the owner publish, share, restore an
/// older version, or delete it. A new note (`noteId` nil) is created on its first save.
struct NoteEditorScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: NoteEditorModel?
    @State private var notice: FmNotice?
    @State private var confirmDelete: Bool = false
    @State private var confirmDiscard: Bool = false
    @State private var history: ToolsSheet<NoteHistoryModel>?
    @State private var share: ToolsSheet<NoteShareModel>?

    init(noteId: String? = nil) {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: NoteEditorModel(noteId: noteId, api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            editor(model)
        } else {
            FmEmptyState(icon: "sticky_note_2_outlined", title: "Note unavailable")
        }
    }

    private func editor(_ model: NoteEditorModel) -> some View {
        NoteEditorBody(model: model, notice: $notice)
            .task { await model.load() }
            .navigationTitle(model.screenTitle)
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarBackButtonHidden(model.confirmsLeave)
            .toolbar { toolbar(model) }
            .background(fm.canvas)
            .fmDialog(isPresented: $confirmDelete) {
                confirmation(NoteEditorModel.deleteConfirmation, isPresented: $confirmDelete) {
                    Task {
                        let outcome: ToolsOutcome = await model.delete()
                        apply(outcome)
                    }
                }
            }
            .fmDialog(isPresented: $confirmDiscard) {
                confirmation(NoteEditorModel.discardConfirmation, isPresented: $confirmDiscard) {
                    model.discard()
                    dismiss()
                }
            }
            .sheet(item: $history) { (box: ToolsSheet<NoteHistoryModel>) in
                NoteHistorySheet(model: box.value) { (restored: JSONObject) in
                    history = nil
                    apply(model.applyRestored(restored))
                }
            }
            .sheet(item: $share, onDismiss: {
                Task { await model.refreshMeta() }
            }) { (box: ToolsSheet<NoteShareModel>) in
                NoteShareSheet(model: box.value)
            }
    }

    @ToolbarContentBuilder
    private func toolbar(_ model: NoteEditorModel) -> some ToolbarContent {
        if model.confirmsLeave {
            ToolbarItem(placement: .navigationBarLeading) {
                FmIconButton(icon: "arrow_back", label: "Back", onPressed: { confirmDiscard = true })
            }
        }
        ToolbarItemGroup(placement: .primaryAction) {
            if model.showsSave {
                FmTextButton(model.saveLabel, onPressed: model.saveEnabled ? {
                    Task { apply(await model.save()) }
                } : nil)
                .accessibilityIdentifier("note-save")
            }
            if model.showsMenu {
                menu(model)
            }
            if model.id != nil {
                NoteConvertMenu(model: model, onNotice: { (next: FmNotice) in notice = next })
            }
        }
    }

    private func menu(_ model: NoteEditorModel) -> some View {
        Menu {
            ForEach(model.menuItems, id: \.value) { (item: (value: String, label: String)) in
                Button(item.label, role: item.value == "delete" ? .destructive : nil) {
                    choose(item.value, model)
                }
            }
        } label: {
            FmIcon("more_vert", size: 24, tone: .primary)
                .frame(width: 44, height: 44)
        }
        .accessibilityLabel(Text("More"))
        .accessibilityIdentifier("note-menu")
    }

    private func choose(_ value: String, _ model: NoteEditorModel) {
        switch value {
        case "history":
            if let sheet = model.historyModel() {
                history = ToolsSheet(sheet)
            }
        case "share":
            if let sheet = model.shareModel() {
                share = ToolsSheet(sheet)
            }
        case "publish":
            Task { apply(await model.setVisibility("public")) }
        case "unpublish":
            Task { apply(await model.setVisibility("private")) }
        case "delete":
            confirmDelete = true
        default:
            break
        }
    }

    private func apply(_ outcome: ToolsOutcome) {
        if let next = outcome.notice {
            notice = FmNotice(next.message, isError: next.isError)
        }
        if outcome.close {
            dismiss()
        }
    }

    private func confirmation(_ words: ToolsConfirmation, isPresented: Binding<Bool>, onConfirm: @escaping () -> Void) -> some View {
        FmConfirmDialog(
            title: words.title, message: words.message, confirmLabel: words.confirm, destructive: words.destructive,
            onCancel: { isPresented.wrappedValue = false },
            onConfirm: {
                isPresented.wrappedValue = false
                onConfirm()
            })
    }
}

private struct NoteEditorBody: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NoteEditorModel
    @Binding var notice: FmNotice?

    var body: some View {
        if model.loading {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if !model.error.isEmpty {
            FmEmptyState(icon: "sticky_note_2_outlined", title: "Note unavailable", message: model.error)
        } else {
            fields
        }
    }

    private var titleBinding: Binding<String> {
        Binding<String>(
            get: { model.title },
            set: { (value: String) in model.title = String(value.prefix(NoteEditorModel.titleMaxLength)) })
    }

    private var fields: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            TextField("", text: titleBinding, prompt: Text("Title").foregroundColor(fm.textTertiary))
                .fmText(.titleLarge)
                .textInputAutocapitalization(.sentences)
                .disabled(!model.canEdit)
                .accessibilityIdentifier("note-title")
            Text(model.meta)
                .fmText(.bodySmall, color: fm.textTertiary)
            FmNoticeLine($notice)
            ZStack(alignment: .topLeading) {
                TextEditor(text: $model.markdown)
                    .fmMono(size: FmType.small)
                    .scrollContentBackground(.hidden)
                    .textInputAutocapitalization(.sentences)
                    .disabled(!model.canEdit)
                    .accessibilityIdentifier("note-markdown")
                if model.markdown.isEmpty {
                    Text("Write in markdown…")
                        .fmMono(size: FmType.small, color: fm.textTertiary)
                        .padding(.top, 8)
                        .padding(.leading, 5)
                        .allowsHitTesting(false)
                }
            }
            .padding(FmSpace.x2)
            .background(fm.bgRaised)
            .clipShape(FmRoundedShape(radius: FmRadius.lg))
            .overlay { FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.border, lineWidth: 1) }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x4)
    }
}

/// Stored versions, newest first. Restoring writes the old text as a new version.
private struct NoteHistorySheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NoteHistoryModel
    let onRestored: (JSONObject) -> Void
    @State private var notice: FmNotice?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmPanelHeader(title: NoteHistoryModel.title)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
            }
            content
        }
        .task { await model.load() }
        .fmSheetStyle(fitsContent: false)
    }

    @ViewBuilder private var content: some View {
        if let versions = model.versions {
            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(Array(versions.enumerated()), id: \.offset) { (pair: (offset: Int, element: JSONObject)) in
                        if pair.offset > 0 {
                            FmRowDivider(indent: 16)
                        }
                        FmListRow(
                            title: model.title(pair.element), subtitle: model.subtitle(pair.element),
                            trailing: trailing(pair.element), showChevron: false)
                    }
                }
            }
        } else if model.error.isEmpty {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            FmEmptyState(icon: "history", title: "History unavailable", message: model.error)
        }
    }

    private func trailing(_ entry: JSONObject) -> AnyView? {
        if model.isCurrent(entry) {
            return FmStatusBadge(label: "Current", color: fm.accent).fmAny
        }
        if !model.canRestore {
            return nil
        }
        let version: Int = model.version(entry)
        return FmTextButton("Restore", onPressed: model.busy ? nil : {
            Task {
                let result: (note: JSONObject?, failure: String?) = await model.restore(version)
                if let note = result.note {
                    onRestored(note)
                } else if let failure = result.failure {
                    notice = FmNotice.error(failure)
                }
            }
        }).fmAny
    }
}

/// Owner-only sharing: people or organizations, as viewers or editors.
private struct NoteShareSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NoteShareModel
    @State private var notice: FmNotice?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                FmPanelHeader(title: NoteShareModel.title, subtitle: NoteShareModel.subtitle)
                if model.shares.isEmpty {
                    Text(NoteShareModel.notShared)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .padding(FmSpace.x4)
                }
                ForEach(Array(model.shares.enumerated()), id: \.offset) { (pair: (offset: Int, element: JSONObject)) in
                    shareRow(pair.element)
                }
                addRow
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                FmFilledButton("Share", fillWidth: true, onPressed: model.busy ? nil : add)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x1)
                    .padding(.bottom, FmSpace.x5)
                    .accessibilityIdentifier("note-share-add")
            }
        }
        .fmSheetStyle(fitsContent: false)
    }

    private func shareRow(_ share: JSONObject) -> some View {
        FmListRow(
            title: model.shareTitle(share), subtitle: model.shareSubtitle(share),
            leading: FmIconTile(icon: model.shareIcon(share)).fmAny,
            trailing: FmIconButton(icon: "close", label: "Stop sharing", onPressed: model.busy ? nil : {
                Task { report(await model.stop(share)) }
            }).fmAny,
            showChevron: false)
    }

    private var addRow: some View {
        HStack(spacing: FmSpace.x2) {
            Picker("", selection: $model.type) {
                Text("User").tag("user")
                Text("Org").tag("organization")
            }
            .pickerStyle(.menu)
            .labelsHidden()
            FmTextField(text: $model.name, hint: "Name", onSubmit: add)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .submitLabel(.done)
                .accessibilityIdentifier("note-share-name")
            Picker("", selection: $model.role) {
                Text("Viewer").tag("viewer")
                Text("Editor").tag("editor")
            }
            .pickerStyle(.menu)
            .labelsHidden()
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x3)
        .padding(.bottom, FmSpace.x2)
    }

    private func add() {
        Task { report(await model.add()) }
    }

    private func report(_ failure: String?) {
        notice = failure.map { (text: String) -> FmNotice in FmNotice.error(text) }
    }
}
