// The account's markdown notes (`/api/notes`), one note's editor, its history sheet and its share
// sheet. State: FMTools' NotesListModel, NoteEditorModel, NoteHistoryModel and NoteShareModel.

import FMAccount
import FMApi
import FMBase
import FMNet
import FMTools
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct NotesScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: NotesListModel?
    @State private var openedId: String?
    @State private var editorOpen: Bool = false

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: NotesListModel(api: api, me: Services.get(AuthService.self)?.session?.nodeName ?? ""))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(NotesListModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .shellDetail(isPresented: $editorOpen, id: openedId) {
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
        if let id = id {
            model?.markViewed(id)
        }
        openedId = id
        editorOpen = true
    }
}

private struct NoteListRow: View {
    @Environment(\.fmPalette) private var fm
    let row: NoteRow
    let onTap: () -> Void

    var body: some View {
        FmListRow(
            title: row.title,
            titleView: titleLine.fmAny,
            subtitleView: subtitleLine.fmAny,
            leading: leading.fmAny,
            trailing: trailing.fmAny,
            onTap: onTap)
            .accessibilityLabel(Text(accessibilityText))
    }

    private var titleLine: some View {
        Text(row.title)
            .fmText(.titleSmall, weight: row.unread ? .bold : nil)
            .lineLimit(1)
            .truncationMode(.tail)
    }

    private var subtitleLine: some View {
        HStack(spacing: FmSpace.x2) {
            Text(row.subtitle)
                .fmText(.bodySmall, color: fm.textTertiary, height: FmType.leadingTight)
                .lineLimit(1)
                .truncationMode(.tail)
            if row.openComments > 0 {
                HStack(spacing: 2) {
                    FmIcon("chat_bubble_outline", size: 12, color: fm.textTertiary)
                    Text("\(row.openComments)")
                        .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                }
                .accessibilityLabel(Text(row.openComments == 1 ? "1 open comment" : "\(row.openComments) open comments"))
            }
        }
    }

    private var leading: some View {
        FmIconTile(icon: row.icon)
            .overlay(alignment: .topLeading) {
                if row.unread {
                    Circle()
                        .fill(fm.accent)
                        .frame(width: 10, height: 10)
                        .overlay { Circle().strokeBorder(fm.canvas, lineWidth: 2) }
                        .offset(x: -3, y: -3)
                        .accessibilityHidden(true)
                }
            }
    }

    private var badgeColor: Color {
        switch row.reach {
        case .publicNote: return fm.accent
        case .shared, .privateNote: return fm.textTertiary
        }
    }

    private var trailing: some View {
        VStack(alignment: .trailing, spacing: FmSpace.x1) {
            FmStatusBadge(label: row.reach.label, color: badgeColor)
            if !row.people.isEmpty {
                HStack(spacing: -4) {
                    ForEach(row.people) { (person: NoteAvatar) in
                        NoteShareAvatar(avatar: person)
                    }
                    if row.morePeople > 0 {
                        Text("+\(row.morePeople)")
                            .fmText(.labelSmall, color: fm.textSecondary)
                            .padding(.leading, FmSpace.x2)
                    }
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(peopleText))
            }
        }
    }

    private var peopleText: String {
        let names: String = row.people.map(\.name).joined(separator: ", ")
        return row.morePeople > 0 ? "Shared with \(names) and \(row.morePeople) more" : "Shared with \(names)"
    }

    private var accessibilityText: String {
        var parts: [String] = []
        if row.unread {
            parts.append("Unread")
        }
        parts.append(row.title)
        parts.append(row.reach.label)
        if !row.subtitle.isEmpty {
            parts.append(row.subtitle)
        }
        if row.openComments > 0 {
            parts.append(row.openComments == 1 ? "1 open comment" : "\(row.openComments) open comments")
        }
        if !row.people.isEmpty {
            parts.append(peopleText)
        }
        return parts.joined(separator: ", ")
    }
}

private struct NoteOwnerActions: ViewModifier {
    let isOwner: Bool
    let onShare: () -> Void
    let onDelete: () -> Void

    func body(content: Content) -> some View {
        if isOwner {
            content
                .accessibilityAction(named: Text("Share…"), onShare)
                .accessibilityAction(named: Text("Delete"), onDelete)
        } else {
            content
        }
    }
}

private struct NoteShareAvatar: View {
    @Environment(\.fmPalette) private var fm
    let avatar: NoteAvatar

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: avatar.organization ? 5 : 11, style: .continuous)
        Text(avatar.initials)
            .fmText(.labelSmall, color: fm.textPrimary)
            .frame(width: 22, height: 22)
            .background(fm.bgRaised)
            .clipShape(shape)
            .overlay { shape.strokeBorder(fm.border, lineWidth: 1) }
    }
}

private struct NotesList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NotesListModel
    let onOpen: (String?) -> Void
    @State private var notice: FmNotice?
    @State private var share: ToolsSheet<NoteShareModel>?
    @State private var pendingDelete: String?
    @State private var details: NoteRow?

    var body: some View {
        content
            .sheet(item: $share, onDismiss: {
                Task { await model.load() }
            }) { (box: ToolsSheet<NoteShareModel>) in
                NoteShareSheet(model: box.value)
            }
            .confirmationDialog(
                details?.title ?? "", isPresented: detailsShown, titleVisibility: .visible, presenting: details
            ) { (row: NoteRow) in
                menuItems(row)
            } message: { (row: NoteRow) in
                Text(model.accessLines(row.id).joined(separator: "\n"))
            }
            .fmDialog(isPresented: deleteShown) {
                FmConfirmDialog(
                    title: NoteEditorModel.deleteConfirmation.title, message: NoteEditorModel.deleteConfirmation.message,
                    confirmLabel: NoteEditorModel.deleteConfirmation.confirm, destructive: true,
                    onCancel: { pendingDelete = nil },
                    onConfirm: {
                        let id: String? = pendingDelete
                        pendingDelete = nil
                        if let id = id {
                            Task {
                                let outcome: ToolsOutcome = await model.delete(id)
                                notice = outcome.notice.map { (next: ToolsNotice) -> FmNotice in FmNotice(next.message, isError: next.isError) }
                            }
                        }
                    })
            }
    }

    private var detailsShown: Binding<Bool> {
        Binding<Bool>(get: { details != nil }, set: { (shown: Bool) in if !shown { details = nil } })
    }

    private var deleteShown: Binding<Bool> {
        Binding<Bool>(get: { pendingDelete != nil }, set: { (shown: Bool) in if !shown { pendingDelete = nil } })
    }

    private func copyLink(_ id: String) {
        guard let url = model.accessLink(id) else {
            notice = FmNotice.error("The link is not available.")
            return
        }
        UIPasteboard.general.string = url.absoluteString
        notice = FmNotice(NoteEditorModel.linkCopied)
    }

    private func startShare(_ id: String) {
        if let sheet = model.shareModel(id) {
            share = ToolsSheet(sheet)
        }
    }

    @ViewBuilder private func menuItems(_ row: NoteRow) -> some View {
        Button {
            onOpen(row.id)
        } label: {
            Label("Open", systemImage: "doc.text")
        }
        Button {
            copyLink(row.id)
        } label: {
            Label("Copy link", systemImage: "link")
        }
        if row.isOwner {
            Button {
                startShare(row.id)
            } label: {
                Label("Share…", systemImage: "person.crop.circle.badge.plus")
            }
            Button(role: .destructive) {
                pendingDelete = row.id
            } label: {
                Label("Delete", systemImage: "trash")
            }
        }
    }

    private func preview(_ row: NoteRow) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(row.title)
                .fmText(.titleSmall)
                .lineLimit(2)
            ForEach(Array(model.accessLines(row.id).enumerated()), id: \.offset) { (pair: (offset: Int, element: String)) in
                Text(pair.element)
                    .fmText(.bodySmall, color: pair.offset == 0 ? fm.textPrimary : fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(FmSpace.x4)
        .frame(width: 320, alignment: .leading)
        .background(fm.bgRaised)
    }

    @ViewBuilder private var content: some View {
        if model.showsSpinner {
            FmSkeletonList()
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
                FmSegmentTabBar(tabs: filterTabs, index: model.filterIndex) { (index: Int) in
                    model.filter = NotesFilter.allCases[index]
                }
                .accessibilityIdentifier("notes-filters")
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                if model.rows.isEmpty {
                    Text(model.emptyFilterMessage)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .frame(maxWidth: .infinity)
                        .padding(FmSpace.x5)
                }
                ForEach(Array(model.rows.enumerated()), id: \.element.id) { (pair: (offset: Int, element: NoteRow)) in
                    if pair.offset > 0 {
                        FmRowDivider()
                    }
                    NoteListRow(row: pair.element, onTap: { onOpen(pair.element.id) })
                        .contextMenu {
                            menuItems(pair.element)
                        } preview: {
                            preview(pair.element)
                        }
                        .accessibilityAction(named: Text("Copy link")) { copyLink(pair.element.id) }
                        .accessibilityAction(named: Text("Note details and actions")) {
                            details = pair.element
                        }
                        .modifier(NoteOwnerActions(isOwner: pair.element.isOwner, onShare: { startShare(pair.element.id) }, onDelete: { pendingDelete = pair.element.id }))
                        .accessibilityIdentifier("note-\(pair.element.id)")
                }
            }
            .padding(.bottom, 96)
        }
        .fmRefreshable { await model.load() }
        .onChange(of: model.query) { (_: String, _: String) in model.queryChanged() }
    }

    private var filterTabs: [FmSegmentTab] {
        NotesFilter.allCases.map { (filter: NotesFilter) -> FmSegmentTab in FmSegmentTab(label: filter.label, count: model.count(filter)) }
    }
}

/// One note: read it, edit it (owner or editor), and for the owner publish, share, restore an
/// older version, or delete it. A new note (`noteId` nil) is created on its first save.
struct NoteEditorScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var model: NoteEditorModel?
    @State private var notice: FmNotice?
    @State private var confirmDelete: Bool = false
    @State private var confirmDiscard: Bool = false
    @State private var history: ToolsSheet<NoteHistoryModel>?
    @State private var share: ToolsSheet<NoteShareModel>?

    init(noteId: String? = nil, openComment: String? = nil) {
        if let api = Services.get(ApiService.self) {
            let me: String = Services.get(AuthService.self)?.session?.nodeName ?? ""
            _model = State(initialValue: NoteEditorModel(
                noteId: noteId, api: api, connector: Services.get(WebSocketConnector.self), me: me, openComment: openComment))
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
        NoteEditorBody(model: model, notice: $notice, onCopy: copy)
            .task {
                await model.load()
                await model.recordView()
            }
            .task(id: model.comments?.noteId) { await model.comments?.load() }
            .onChange(of: model.title) { (_: String, _: String) in model.edited() }
            .onChange(of: model.markdown) { (_: String, _: String) in model.edited() }
            .onDisappear {
                model.leave()
                Task { await model.flush() }
            }
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
            .sheet(isPresented: commentsShown(model)) {
                if let comments = model.comments {
                    NoteCommentsSheet(editor: model, comments: comments)
                }
            }
    }

    private func commentsShown(_ model: NoteEditorModel) -> Binding<Bool> {
        Binding<Bool>(
            get: { model.commentsOpen && model.comments != nil },
            set: { (shown: Bool) in model.commentsOpen = shown })
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
            if let comments = model.comments {
                NoteCommentsButton(comments: comments, onPressed: { model.commentsOpen = true })
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
                if item.value == "export" {
                    ShareLink(
                        item: NoteExport(fileName: model.exportFileName, text: model.exportText),
                        preview: SharePreview(model.exportFileName)
                    ) {
                        Text(item.label)
                    }
                } else {
                    Button(item.label, role: item.value == "delete" ? .destructive : nil) {
                        choose(item.value, model)
                    }
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
        case "copy_link":
            copy(model.accessLink)
        case "open_public":
            if let url = model.publicLink {
                openURL(url)
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

    private func copy(_ url: URL?) {
        guard let url = url else {
            notice = FmNotice.error("The link is not available.")
            return
        }
        UIPasteboard.general.string = url.absoluteString
        notice = FmNotice(NoteEditorModel.linkCopied)
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
    @Environment(\.openURL) private var openURL
    @Bindable var model: NoteEditorModel
    @Binding var notice: FmNotice?
    let onCopy: (URL?) -> Void
    @State private var linking: Bool = false

    var body: some View {
        if model.loading {
            FmSkeletonList(rows: 6, hasLeading: false)
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

    private var modeTabs: [FmSegmentTab] {
        NoteMode.allCases.map { (mode: NoteMode) -> FmSegmentTab in FmSegmentTab(label: mode.label(canEdit: model.canEdit)) }
    }

    private var fields: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            TextField("", text: titleBinding, prompt: Text("Title").foregroundColor(fm.textTertiary))
                .fmText(.titleLarge)
                .textInputAutocapitalization(.sentences)
                .disabled(!model.canEdit)
                .accessibilityIdentifier("note-title")
            Text(metaLine)
                .fmText(.bodySmall, color: fm.textTertiary)
                .accessibilityIdentifier("note-save-state")
            if let room = model.room, room.showsLabel {
                NotePresenceLine(room: room)
            }
            FmNoticeLine($notice)
            if model.conflict != nil {
                conflictBar
            }
            if let url = model.publicLink {
                publicRow(url)
            }
            if model.canEdit || model.canComment {
                FmSegmentTabBar(tabs: modeTabs, index: model.mode == .write ? 0 : 1) { (index: Int) in
                    model.mode = NoteMode.allCases[index]
                }
                .accessibilityIdentifier("note-mode")
            }
            if model.shownMode == .write {
                if model.canEdit || model.canComment {
                    formatBar
                }
                editor
            } else {
                preview
            }
            Text(model.counts)
                .fmText(.labelSmall, color: fm.textTertiary)
            if model.showsLinks {
                linksSection
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x4)
    }

    private var metaLine: String {
        let state: String = model.saveState
        return state.isEmpty ? model.meta : "\(model.meta) · \(state)"
    }

    private var conflictBar: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(NoteEditorModel.conflictMessage)
                .fmText(.bodyMedium)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: FmSpace.x2) {
                FmFilledButton("Keep mine", onPressed: {
                    Task { report(await model.keepMine()) }
                })
                .accessibilityIdentifier("note-conflict-mine")
                FmOutlinedButton("Take theirs", onPressed: {
                    report(model.takeTheirs())
                })
                .accessibilityIdentifier("note-conflict-theirs")
            }
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
    }

    private func publicRow(_ url: URL) -> some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("public", size: 18)
            Text(url.absoluteString.replacingOccurrences(of: "https://", with: ""))
                .fmText(.bodySmall, color: fm.accent)
                .lineLimit(1)
                .truncationMode(.middle)
                .onTapGesture { openURL(url) }
            Spacer(minLength: 0)
            FmIconButton(icon: "content_copy", label: "Copy public link", size: 20, onPressed: { onCopy(url) })
                .accessibilityIdentifier("note-copy-public")
        }
    }

    private var formatBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x1) {
                if model.canEdit {
                    ForEach(NoteFormat.allCases) { (kind: NoteFormat) in
                        Button {
                            model.format(kind)
                        } label: {
                            Text(kind.glyph)
                                .fmText(.labelLarge, color: fm.textSecondary)
                                .frame(minWidth: 36, minHeight: 32)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(Text(kind.label))
                    }
                }
                if model.canComment {
                    FmTextButton("Comment", icon: "add_comment_outlined", minHeight: 32, onPressed: model.selectedQuote == nil ? nil : {
                        model.commentOnSelection()
                    })
                    .accessibilityIdentifier("note-comment-selection")
                }
            }
        }
        .accessibilityIdentifier("note-format-bar")
    }

    private var editor: some View {
        ZStack(alignment: .topLeading) {
            NoteTextView(
                text: $model.markdown, editable: model.canEdit, highlight: model.highlightRange,
                request: model.selectionRequest, canComment: model.canComment,
                onSelect: { (range: Range<Int>) in model.selectionChanged(range) },
                onComment: { model.commentOnSelection() })
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

    private var preview: some View {
        ScrollView {
            NotePreview(markdown: model.markdown)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(FmSpace.x3)
        }
        .frame(maxHeight: .infinity)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay { FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.border, lineWidth: 1) }
        .accessibilityIdentifier("note-preview")
    }

    private var linksSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    ForEach(Array(model.links.enumerated()), id: \.offset) { (pair: (offset: Int, element: NoteLinkRef)) in
                        linkChip(pair.element)
                    }
                    if model.canEdit && !linking {
                        FmTextButton("Link issue or PR", icon: "link", minHeight: 32, onPressed: { linking = true })
                            .accessibilityIdentifier("note-link-start")
                    }
                }
            }
            if linking {
                linkForm
            }
        }
    }

    private func linkChip(_ link: NoteLinkRef) -> some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon(link.icon, size: 16)
            Text(link.title)
                .fmText(.labelMedium, color: fm.accent)
                .onTapGesture {
                    if let url = model.linkURL(link) {
                        openURL(url)
                    }
                }
            if model.canEdit {
                FmIconButton(icon: "close", label: "Unlink \(link.title)", size: 16, onPressed: model.linkBusy ? nil : {
                    Task { report(await model.removeLink(link)) }
                })
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .frame(minHeight: 32)
        .background(fm.bgRaised)
        .clipShape(Capsule())
        .accessibilityElement(children: .contain)
        .accessibilityLabel(Text(link.subtitle))
    }

    private var linkForm: some View {
        HStack(spacing: FmSpace.x2) {
            FmTextField(text: $model.linkText, hint: "URL or owner/repo#123", onSubmit: addLink)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .submitLabel(.done)
                .accessibilityIdentifier("note-link-input")
            Picker("", selection: $model.linkKind) {
                ForEach(NoteLinkRef.kinds, id: \.value) { (kind: (value: String, label: String)) in
                    Text(kind.label).tag(kind.value)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            FmTextButton("Link", onPressed: model.linkBusy ? nil : addLink)
            FmIconButton(icon: "close", label: "Cancel", size: 20, onPressed: {
                linking = false
                model.linkText = ""
            })
        }
    }

    private func addLink() {
        Task {
            let outcome: ToolsOutcome = await model.addLink()
            report(outcome)
            if outcome.notice?.isError == false {
                linking = false
            }
        }
    }

    private func report(_ outcome: ToolsOutcome) {
        if let next = outcome.notice {
            notice = FmNotice(next.message, isError: next.isError)
        }
    }
}

private struct NoteExport: Transferable {
    let fileName: String
    let text: String

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: UTType(filenameExtension: "md", conformingTo: .plainText) ?? .plainText) { (export: NoteExport) in
            let url: URL = try NoteEditorModel.writeExport(
                fileName: export.fileName, text: export.text, in: FileManager.default.temporaryDirectory)
            return SentTransferredFile(url)
        }
    }
}

private struct NotePreview: View {
    @Environment(\.fmPalette) private var fm
    let markdown: String

    var body: some View {
        let blocks: [DocsBlock] = DocsMarkdown.blocks(markdown)
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if blocks.isEmpty {
                Text("Nothing written yet.")
                    .fmText(.bodyMedium, color: fm.textTertiary)
            }
            ForEach(Array(blocks.enumerated()), id: \.offset) { (pair: (offset: Int, element: DocsBlock)) in
                block(pair.element)
            }
        }
        .textSelection(.enabled)
    }

    private func inline(_ source: String) -> Text {
        let options = AttributedString.MarkdownParsingOptions(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        if let parsed = try? AttributedString(markdown: source, options: options) {
            return Text(parsed)
        }
        return Text(source)
    }

    private func headingRole(_ level: Int) -> FmTextRole {
        switch level {
        case 1: return .titleLarge
        case 2: return .titleMedium
        default: return .titleSmall
        }
    }

    @ViewBuilder private func block(_ block: DocsBlock) -> some View {
        switch block {
        case let .heading(level, source, _):
            inline(source)
                .fmText(headingRole(level))
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityAddTraits(.isHeader)
        case let .paragraph(source):
            inline(source)
                .fmText(.bodyMedium)
                .fixedSize(horizontal: false, vertical: true)
        case let .listItem(ordered, number, depth, source):
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                Text(ordered ? "\(number)." : "\u{2022}")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                inline(source)
                    .fmText(.bodyMedium)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.leading, CGFloat(min(depth, 6)) * FmSpace.x4)
        case let .quote(source):
            HStack(alignment: .top, spacing: FmSpace.x3) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(fm.border)
                    .frame(width: 3)
                inline(source)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .fixedSize(horizontal: false, vertical: true)
        case let .code(language, source):
            DocsCodeView(language: language, source: source)
        case let .table(header, rows):
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                inline(header.joined(separator: " · "))
                    .fmText(.labelLarge)
                ForEach(Array(rows.enumerated()), id: \.offset) { (row: (offset: Int, element: [String])) in
                    inline(row.element.joined(separator: " · "))
                        .fmText(.bodySmall)
                }
            }
        case .rule:
            FmHairline()
                .padding(.vertical, FmSpace.x2)
        }
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
            FmSkeletonList(rows: 6, hasLeading: false)
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
                suggestions
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                FmFilledButton("Share", fillWidth: true, onPressed: model.busy ? nil : add)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x1)
                    .padding(.bottom, FmSpace.x3)
                    .accessibilityIdentifier("note-share-add")
                if model.showsAccessLink, let url = model.accessLink {
                    accessLink(url)
                }
            }
        }
        .fmSheetStyle(fitsContent: false)
        .onChange(of: model.name) { (_: String, _: String) in model.nameChanged() }
    }

    private func shareRow(_ share: JSONObject) -> some View {
        FmListRow(
            title: model.shareTitle(share), subtitle: model.shareSubtitle(share),
            leading: FmIconTile(icon: model.shareIcon(share)).fmAny,
            trailing: HStack(spacing: FmSpace.x1) {
                Picker("", selection: roleBinding(share)) {
                    ForEach(NoteShareModel.roles, id: \.value) { (role: (value: String, label: String)) in
                        Text(role.label).tag(role.value)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .disabled(model.busy)
                .accessibilityLabel(Text("Access for \(model.shareTitle(share))"))
                FmIconButton(icon: "close", label: "Remove \(model.shareTitle(share))", onPressed: model.busy ? nil : {
                    Task { report(await model.stop(share)) }
                })
            }.fmAny,
            showChevron: false)
    }

    private func roleBinding(_ share: JSONObject) -> Binding<String> {
        Binding<String>(
            get: { model.shareRole(share) },
            set: { (role: String) in
                if role != model.shareRole(share) {
                    Task { report(await model.setRole(share, role)) }
                }
            })
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
                ForEach(NoteShareModel.roles, id: \.value) { (role: (value: String, label: String)) in
                    Text(role.label).tag(role.value)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x3)
        .padding(.bottom, FmSpace.x2)
    }

    @ViewBuilder private var suggestions: some View {
        if !model.suggestions.isEmpty {
            VStack(spacing: 0) {
                ForEach(model.suggestions) { (item: NoteShareSuggestion) in
                    FmListRow(
                        title: item.name, subtitle: item.typeLabel,
                        leading: FmIconTile(icon: item.icon).fmAny,
                        onTap: { model.pick(item) },
                        showChevron: false, minHeight: 48)
                        .accessibilityIdentifier("note-share-suggestion-\(item.id)")
                }
            }
            .background(fm.bgRaised)
            .clipShape(FmRoundedShape(radius: FmRadius.lg))
            .padding(.horizontal, FmSpace.x4)
            .padding(.bottom, FmSpace.x2)
        }
    }

    private func accessLink(_ url: URL) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(NoteShareModel.accessTitle)
                .fmText(.labelLarge)
            Text(url.absoluteString.replacingOccurrences(of: "https://", with: ""))
                .fmText(.bodySmall, color: fm.accent)
                .lineLimit(1)
                .truncationMode(.middle)
                .textSelection(.enabled)
            Text(NoteShareModel.accessHint)
                .fmText(.bodySmall, color: fm.textTertiary)
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton("Copy link", icon: "content_copy", onPressed: {
                    UIPasteboard.general.string = url.absoluteString
                    notice = FmNotice(NoteEditorModel.linkCopied)
                })
                .accessibilityIdentifier("note-copy-access-link")
                ShareLink(item: url) {
                    Label("Send", systemImage: "square.and.arrow.up")
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.bottom, FmSpace.x5)
    }

    private func add() {
        Task { report(await model.add()) }
    }

    private func report(_ outcome: ToolsOutcome) {
        notice = outcome.notice.map { (next: ToolsNotice) -> FmNotice in FmNotice(next.message, isError: next.isError) }
    }
}
