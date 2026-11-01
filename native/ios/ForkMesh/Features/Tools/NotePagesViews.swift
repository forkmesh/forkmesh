import FMAccount
import FMApi
import FMBase
import FMTools
import SwiftUI
import UIKit

func notePageColor(_ accent: NotePageAccent, _ fm: FmPalette) -> Color {
    let dark: Bool = fm.dark
    switch accent {
    case .blue: return fm.accent
    case .purple: return Color(fmARGB: dark ? 0xFFBF5AF2 : 0xFFAF52DE)
    case .pink: return Color(fmARGB: dark ? 0xFFFF375F : 0xFFFF2D55)
    case .red: return Color(fmARGB: dark ? 0xFFFF453A : 0xFFFF3B30)
    case .orange: return Color(fmARGB: dark ? 0xFFFF9F0A : 0xFFC93400)
    case .yellow: return Color(fmARGB: dark ? 0xFFFFD60A : 0xFFB25000)
    case .teal: return Color(fmARGB: dark ? 0xFF40C8E0 : 0xFF00788A)
    case .gray: return Color(fmARGB: dark ? 0xFF98989D : 0xFF6C6C70)
    }
}

@MainActor
enum NotePageNavigation {
    static var customizeOnOpen: String?

    static func openPage(_ id: String, customize: Bool = false) {
        if customize {
            customizeOnOpen = id
        }
        Services.get(AppRouter.self)?.open(.notePage(id: id), placement: .currentTab)
    }

    static func openNote(_ id: String) {
        Services.get(AppRouter.self)?.open(.note(id: id), placement: .currentTab)
    }

    static func openLink(_ raw: String) {
        if let url = URL(string: raw) {
            UIApplication.shared.open(url)
        }
    }
}

struct NotePageTile: View {
    @Environment(\.fmPalette) private var fm
    let page: NotePage
    var size: CGFloat = 38

    var body: some View {
        let tint: Color = notePageColor(page.accent, fm)
        ZStack {
            RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous)
                .fill(tint.opacity(0.16))
            if page.emoji.isEmpty {
                FmIcon("book_outlined", size: size * 0.5, color: tint)
            } else {
                Text(page.emoji)
                    .font(.system(size: size * 0.5))
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

struct NotePagesSection: View {
    @Environment(\.fmPalette) private var fm
    let model: NotePagesModel
    let onNew: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text(NotePagesModel.title)
                    .fmText(.titleSmall, color: fm.textSecondary)
                    .accessibilityAddTraits(.isHeader)
                Spacer(minLength: 0)
                FmTextButton("New page", icon: "add", minHeight: 36, onPressed: model.busy ? nil : onNew)
                    .accessibilityIdentifier("notes-new-page")
            }
            .padding(.leading, FmSpace.x4)
            .padding(.trailing, FmSpace.x1)
            .padding(.top, FmSpace.x2)
            if model.pages.isEmpty {
                Text("Gather notes into a page you can share, with an outline.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x1)
            }
            ForEach(model.pages, id: \.id) { (page: NotePage) in
                FmListRow(
                    title: page.title, subtitle: page.entriesLabel,
                    leading: NotePageTile(page: page).fmAny,
                    onTap: { NotePageNavigation.openPage(page.id) },
                    titleColor: notePageColor(page.accent, fm))
                    .accessibilityIdentifier("note-page-\(page.id)")
                FmRowDivider(indent: 66)
            }
            Text("Notes")
                .fmText(.titleSmall, color: fm.textSecondary)
                .accessibilityAddTraits(.isHeader)
                .padding(.leading, FmSpace.x4)
                .padding(.top, FmSpace.x3)
                .padding(.bottom, FmSpace.x1)
        }
    }
}

final class NoteEmojiUITextField: UITextField {
    override var textInputMode: UITextInputMode? {
        UITextInputMode.activeInputModes.first { (mode: UITextInputMode) in mode.primaryLanguage == "emoji" } ?? super.textInputMode
    }
}

struct NoteEmojiInput: UIViewRepresentable {
    @Binding var text: String
    let placeholder: String

    func makeCoordinator() -> NoteEmojiInputCoordinator {
        NoteEmojiInputCoordinator(text: $text)
    }

    func makeUIView(context: Context) -> NoteEmojiUITextField {
        let field = NoteEmojiUITextField()
        field.placeholder = placeholder
        field.font = UIFont.systemFont(ofSize: 22)
        field.returnKeyType = .done
        field.autocorrectionType = .no
        field.delegate = context.coordinator
        field.accessibilityIdentifier = "note-emoji-field"
        field.addTarget(context.coordinator, action: #selector(NoteEmojiInputCoordinator.changed(_:)), for: .editingChanged)
        field.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return field
    }

    func updateUIView(_ field: NoteEmojiUITextField, context: Context) {
        context.coordinator.text = $text
        if field.text != text {
            field.text = text
        }
    }
}

@MainActor
final class NoteEmojiInputCoordinator: NSObject, UITextFieldDelegate {
    var text: Binding<String>

    init(text: Binding<String>) {
        self.text = text
    }

    @objc func changed(_ field: UITextField) {
        text.wrappedValue = String((field.text ?? "").prefix(64))
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        textField.resignFirstResponder()
        return true
    }
}

struct NoteEmojiPickerSheet: View {
    @Environment(\.fmPalette) private var fm
    let current: String
    let onPick: (String) -> Void
    @State private var typed: String = ""

    private var typedError: String? {
        let trimmed = typed.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty || NoteEmoji.clean(trimmed) != nil {
            return nil
        }
        return NoteEmoji.invalidMessage
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                FmPanelHeader(
                    title: "Emoji",
                    trailing: current.isEmpty ? nil : FmTextButton("Remove", color: fm.danger, onPressed: { onPick("") })
                        .accessibilityIdentifier("note-emoji-remove").fmAny)
                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 4), count: 8), spacing: 4) {
                    ForEach(NoteEmoji.common, id: \.self) { (emoji: String) in
                        Button {
                            onPick(emoji)
                        } label: {
                            Text(emoji)
                                .font(.system(size: 24))
                                .frame(maxWidth: .infinity, minHeight: 40)
                                .background(emoji == current ? fm.accentSubtle : Color.clear)
                                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(Text("Use \(emoji)"))
                    }
                }
                .padding(.horizontal, FmSpace.x3)
                HStack(alignment: .center, spacing: FmSpace.x2) {
                    NoteEmojiInput(text: $typed, placeholder: "Type or paste any emoji")
                        .frame(height: 44)
                        .padding(.horizontal, FmSpace.x3)
                        .background(fm.bgRaised)
                        .clipShape(FmRoundedShape(radius: FmRadius.lg))
                        .overlay { FmRoundedShape(radius: FmRadius.lg).strokeBorder(typedError == nil ? fm.border : fm.danger, lineWidth: 1) }
                    FmTextButton("Use", onPressed: typed.dartTrimmed.isEmpty || typedError != nil ? nil : {
                        onPick(NoteEmoji.clean(typed) ?? "")
                    })
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x4)
                if let error = typedError {
                    Text(error)
                        .fmText(.bodySmall, color: fm.danger)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x1)
                }
            }
            .padding(.bottom, FmSpace.x5)
        }
        .fmSheetStyle(fitsContent: false)
    }
}

struct NoteEmojiButton: View {
    @Environment(\.fmPalette) private var fm
    let emoji: String
    let editable: Bool
    let onTap: () -> Void

    var body: some View {
        if editable {
            Button(action: onTap) {
                face
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(emoji.isEmpty ? "Add an emoji" : "Change the emoji, \(emoji)"))
            .accessibilityIdentifier("note-emoji")
        } else if !emoji.isEmpty {
            face
                .accessibilityLabel(Text("Emoji \(emoji)"))
                .accessibilityIdentifier("note-emoji")
        }
    }

    private var face: some View {
        ZStack {
            if emoji.isEmpty {
                FmIcon("add_reaction_outlined", size: 24, color: fm.textTertiary)
            } else {
                Text(emoji)
                    .font(.system(size: 26))
            }
        }
        .frame(width: 44, height: 44)
        .contentShape(Rectangle())
    }
}

struct NotePageScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: NotePageModel?
    @State private var notice: FmNotice?
    @State private var customizing: Bool = false
    @State private var share: ToolsSheet<NoteShareModel>?
    @State private var confirmDelete: Bool = false
    @Environment(\.dismiss) private var dismiss

    init(pageId: String, noteId: String? = nil) {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: NotePageModel(pageId: pageId, selectedNoteId: noteId, api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            page(model)
        } else {
            FmEmptyState(icon: "book_outlined", title: "Page unavailable")
        }
    }

    private func page(_ model: NotePageModel) -> some View {
        NotePageBody(model: model, notice: $notice)
            .navigationTitle(model.page?.title ?? "Page")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .toolbar { toolbar(model) }
            .task {
                await model.load()
                if NotePageNavigation.customizeOnOpen == model.pageId {
                    NotePageNavigation.customizeOnOpen = nil
                    model.startCustomize()
                    customizing = model.draft != nil
                }
            }
            .fmRefreshable { await model.load() }
            .sheet(isPresented: $customizing, onDismiss: { model.draft = nil }) {
                NotePageCustomizeSheet(model: model, onClose: { (message: String?) in
                    customizing = false
                    if let message = message {
                        notice = FmNotice(message)
                    }
                })
            }
            .sheet(item: $share, onDismiss: {
                Task { await model.load() }
            }) { (box: ToolsSheet<NoteShareModel>) in
                NoteShareSheet(model: box.value)
            }
            .fmDialog(isPresented: $confirmDelete) {
                let words: ToolsConfirmation = model.deleteConfirmation
                FmConfirmDialog(
                    title: words.title, message: words.message, confirmLabel: words.confirm, destructive: true,
                    onCancel: { confirmDelete = false },
                    onConfirm: {
                        confirmDelete = false
                        Task {
                            let outcome: ToolsOutcome = await model.delete()
                            if outcome.close {
                                dismiss()
                            } else if let next = outcome.notice {
                                notice = FmNotice(next.message, isError: next.isError)
                            }
                        }
                    })
            }
    }

    @ToolbarContentBuilder
    private func toolbar(_ model: NotePageModel) -> some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            if model.canEdit {
                FmIconButton(icon: "tune", label: "Customize", onPressed: {
                    model.startCustomize()
                    customizing = model.draft != nil
                })
                .accessibilityIdentifier("note-page-customize")
            }
            if model.isOwner {
                FmIconButton(icon: "person_add", label: "Share", onPressed: {
                    if let sheet = model.shareModel() {
                        share = ToolsSheet(sheet)
                    }
                })
                .accessibilityIdentifier("note-page-share")
            }
            if model.page != nil {
                Menu {
                    Button {
                        if let url = model.accessLink {
                            UIPasteboard.general.string = url.absoluteString
                            notice = FmNotice("Link copied")
                        } else {
                            notice = FmNotice.error("The link is not available.")
                        }
                    } label: {
                        Label("Copy link", systemImage: "link")
                    }
                    if model.isOwner {
                        Button(role: .destructive) {
                            confirmDelete = true
                        } label: {
                            Label("Delete page", systemImage: "trash")
                        }
                    }
                } label: {
                    FmIcon("more_vert", size: 24, tone: .primary)
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel(Text("More"))
                .accessibilityIdentifier("note-page-menu")
            }
        }
    }
}

private struct NotePageBody: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NotePageModel
    @Binding var notice: FmNotice?

    var body: some View {
        if model.loading && model.page == nil {
            FmSkeletonList(rows: 6, hasLeading: false)
        } else if let page = model.page {
            if page.layout == .outline, let id = model.selectedNoteId, let note = model.note(id) {
                NotePageNoteView(model: model, page: page, note: note)
            } else {
                outline(page)
            }
        } else {
            FmEmptyState(icon: "book_outlined", title: "Page unavailable", message: model.error)
        }
    }

    private func outline(_ page: NotePage) -> some View {
        let continuous: Bool = page.layout == .continuous
        let tint: Color = notePageColor(page.accent, fm)
        return ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if notice != nil {
                        FmNoticeLine($notice)
                            .padding(.horizontal, FmSpace.x4)
                    }
                    NotePageHeader(page: page)
                    Text(continuous ? "Contents" : "Outline")
                        .fmText(.titleSmall, color: fm.textSecondary)
                        .accessibilityAddTraits(.isHeader)
                        .padding(.leading, FmSpace.x4)
                        .padding(.top, FmSpace.x2)
                        .padding(.bottom, FmSpace.x1)
                    if model.rows.isEmpty {
                        Text(model.emptyMessage)
                            .fmText(.bodyMedium, color: fm.textTertiary)
                            .fixedSize(horizontal: false, vertical: true)
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.vertical, FmSpace.x2)
                    }
                    ForEach(model.rows) { (row: NotePageRow) in
                        NotePageOutlineRow(row: row, tint: tint) {
                            tap(row, continuous: continuous, proxy: proxy)
                        }
                    }
                    if continuous {
                        ForEach(model.readableNoteIds, id: \.self) { (id: String) in
                            if let note = model.note(id) {
                                FmRowDivider()
                                NotePageNoteSection(model: model, note: note)
                                    .padding(FmSpace.x4)
                                    .id("note-\(id)")
                            }
                        }
                    }
                }
                .padding(.bottom, 48)
            }
            .task(id: continuous) {
                if continuous {
                    await model.ensureAllBodies()
                    if let id = model.selectedNoteId {
                        withAnimation { proxy.scrollTo("note-\(id)", anchor: .top) }
                    }
                }
            }
        }
    }

    private func tap(_ row: NotePageRow, continuous: Bool, proxy: ScrollViewProxy) {
        switch row.kind {
        case .section:
            model.toggle(row.id)
        case let .link(url):
            NotePageNavigation.openLink(url)
        case let .note(id):
            model.opened(id)
            if continuous {
                Task { await model.recordView(id) }
                withAnimation { proxy.scrollTo("note-\(id)", anchor: .top) }
            }
        case .locked:
            break
        }
    }
}

private struct NotePageHeader: View {
    @Environment(\.fmPalette) private var fm
    let page: NotePage

    var body: some View {
        let tint: Color = notePageColor(page.accent, fm)
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                if !page.emoji.isEmpty {
                    Text(page.emoji)
                        .font(.system(size: 34))
                        .accessibilityHidden(true)
                }
                Text(page.title)
                    .fmText(.titleLarge, color: tint, weight: .bold)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
            }
            if !page.description.dartTrimmed.isEmpty {
                NotePreview(markdown: page.description)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(FmSpace.x3)
                    .background(tint.opacity(0.08))
                    .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                    .overlay {
                        RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous).strokeBorder(tint.opacity(0.24), lineWidth: 1)
                    }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
    }
}

private struct NotePageOutlineRow: View {
    @Environment(\.fmPalette) private var fm
    let row: NotePageRow
    let tint: Color
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            content
                .padding(.leading, FmSpace.x4 + CGFloat(row.depth) * 18)
                .padding(.trailing, FmSpace.x4)
                .padding(.vertical, 6)
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(row.kind == .locked)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("note-page-row-\(row.id)")
    }

    @ViewBuilder private var content: some View {
        switch row.kind {
        case let .section(collapsed):
            HStack(spacing: FmSpace.x2) {
                FmIcon(collapsed ? "chevron_right" : "expand_more", size: 18, color: tint)
                Text(row.title)
                    .fmText(.titleSmall, color: tint, weight: .semibold)
                    .lineLimit(2)
            }
            .accessibilityAddTraits(.isHeader)
            .accessibilityValue(Text(collapsed ? "Collapsed" : "Expanded"))
        case .link:
            HStack(spacing: FmSpace.x2) {
                FmIcon("link", size: 18, color: fm.textSecondary)
                Text(row.title)
                    .fmText(.bodyMedium, color: fm.accent)
                    .lineLimit(1)
                Spacer(minLength: 0)
                FmIcon("open_in_new", size: 16, color: fm.textTertiary)
            }
            .accessibilityHint(Text("Opens in Safari"))
        case .locked:
            HStack(spacing: FmSpace.x2) {
                FmIcon("lock_outline", size: 18, color: fm.textTertiary)
                VStack(alignment: .leading, spacing: 0) {
                    Text(row.title)
                        .fmText(.bodyMedium, color: fm.textTertiary)
                        .lineLimit(1)
                    if row.title != NotePageOutline.lockedText {
                        Text(NotePageOutline.lockedText)
                            .fmText(.bodySmall, color: fm.textTertiary)
                    }
                }
            }
        case .note:
            HStack(spacing: FmSpace.x2) {
                Circle()
                    .fill(row.unread ? fm.accent : Color.clear)
                    .frame(width: 8, height: 8)
                    .accessibilityHidden(true)
                if !row.emoji.isEmpty {
                    Text(row.emoji)
                        .font(.system(size: 16))
                        .accessibilityHidden(true)
                }
                Text(row.title)
                    .fmText(.bodyMedium, weight: row.unread ? .bold : nil)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .accessibilityLabel(Text(row.unread ? "Unread, \(row.title)" : row.title))
        }
    }
}

private struct NotePageNoteHeading: View {
    @Environment(\.fmPalette) private var fm
    let model: NotePageModel
    let note: NotePageNote

    var body: some View {
        let me: String = Services.get(AuthService.self)?.session?.nodeName ?? ""
        let edited: String = model.editedLine(note, me: me, nowMillis: SystemClock().nowMillis)
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                if !note.emoji.isEmpty {
                    Text(note.emoji)
                        .font(.system(size: 24))
                        .accessibilityHidden(true)
                }
                Text(note.title)
                    .fmText(.titleLarge)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
            }
            if !edited.isEmpty {
                Text(edited)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            FmFilledButton("Open in editor", icon: "edit", minHeight: 32, onPressed: {
                NotePageNavigation.openNote(note.id)
            })
            .accessibilityIdentifier("note-page-open-editor")
        }
    }
}

private struct NotePageMarkdown: View {
    @Environment(\.fmPalette) private var fm
    let model: NotePageModel
    let note: NotePageNote
    @State private var tried: Bool = false

    var body: some View {
        Group {
            if let markdown = model.body(note.id) {
                if markdown.dartTrimmed.isEmpty {
                    Text(NotePageModel.emptyNote)
                        .fmText(.bodyMedium, color: fm.textTertiary)
                } else {
                    NotePreview(markdown: markdown)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else if tried {
                Text(NotePageModel.noteFailed)
                    .fmText(.bodyMedium, color: fm.textTertiary)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 80)
            }
        }
        .task(id: note.id) {
            tried = false
            await model.ensureBody(note.id)
            tried = true
        }
    }
}

private struct NotePageNoteSection: View {
    let model: NotePageModel
    let note: NotePageNote

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            NotePageNoteHeading(model: model, note: note)
            NotePageMarkdown(model: model, note: note)
        }
    }
}

private struct NotePageNoteView: View {
    @Environment(\.fmPalette) private var fm
    let model: NotePageModel
    let page: NotePage
    let note: NotePageNote

    var body: some View {
        let tint: Color = notePageColor(page.accent, fm)
        let around = model.neighbours(note.id)
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Button {
                    model.closeNote()
                } label: {
                    HStack(spacing: FmSpace.x1) {
                        FmIcon("arrow_back", size: 16, color: tint)
                        Text(page.emoji.isEmpty ? page.title : "\(page.emoji) \(page.title)")
                            .fmText(.bodySmall, color: tint)
                            .lineLimit(1)
                    }
                    .frame(minHeight: 32)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Back to \(page.title)"))
                NotePageNoteSection(model: model, note: note)
                HStack(spacing: FmSpace.x2) {
                    if let previous = around.previous {
                        FmOutlinedButton(previous.emoji.isEmpty ? previous.title : "\(previous.emoji) \(previous.title)", icon: "arrow_back", minHeight: 32, onPressed: {
                            model.opened(previous.id)
                        })
                    }
                    if let next = around.next {
                        FmOutlinedButton(next.emoji.isEmpty ? next.title : "\(next.emoji) \(next.title)", icon: "arrow_forward", minHeight: 32, onPressed: {
                            model.opened(next.id)
                        })
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .task(id: note.id) {
            await model.recordView(note.id)
        }
    }
}

private enum NotePageAdding {
    case none, note, section, link
}

struct NotePageCustomizeSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: NotePageModel
    let onClose: (String?) -> Void
    @State private var notice: FmNotice?
    @State private var picking: Bool = false
    @State private var adding: NotePageAdding = .none
    @State private var sectionName: String = ""
    @State private var linkLabel: String = ""
    @State private var linkUrl: String = ""
    @State private var renaming: NotePageEntry?
    @State private var renameText: String = ""

    var body: some View {
        NavigationStack {
            List {
                if notice != nil {
                    FmNoticeLine($notice)
                        .listRowBackground(Color.clear)
                }
                fieldsSection
                accentSection
                layoutSection
                outlineSection
                addSection
            }
            .scrollContentBackground(.hidden)
            .background(fm.sheetSurface)
            .navigationTitle("Customize")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        model.draft = nil
                        onClose(nil)
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        Task {
                            let outcome: ToolsOutcome = await model.saveCustomize()
                            if model.draft == nil {
                                onClose(outcome.notice?.message ?? "Page saved")
                            } else if let next = outcome.notice {
                                notice = FmNotice(next.message, isError: next.isError)
                            }
                        }
                    }
                    .disabled(!model.draftChanged || model.busy)
                    .accessibilityIdentifier("note-page-save")
                }
            }
            .sheet(isPresented: $picking) {
                NoteEmojiPickerSheet(current: model.draft?.emoji ?? "") { (emoji: String) in
                    model.draft?.emoji = emoji
                    picking = false
                }
            }
            .alert("Rename", isPresented: renameShown) {
                TextField("Name", text: $renameText)
                Button("Rename") {
                    if let entry = renaming {
                        model.rename(entry.id, renameText)
                    }
                    renaming = nil
                }
                Button("Cancel", role: .cancel) { renaming = nil }
            }
        }
        .task { await model.loadMyNotes() }
        .presentationDetents([.large])
    }

    private var renameShown: Binding<Bool> {
        Binding<Bool>(get: { renaming != nil }, set: { (shown: Bool) in if !shown { renaming = nil } })
    }

    private func binding<Value>(_ key: WritableKeyPath<NotePageDraft, Value>, _ fallback: Value) -> Binding<Value> {
        Binding<Value>(
            get: { model.draft?[keyPath: key] ?? fallback },
            set: { (value: Value) in model.draft?[keyPath: key] = value })
    }

    private var fieldsSection: some View {
        Section {
            HStack(spacing: FmSpace.x2) {
                NoteEmojiButton(emoji: model.draft?.emoji ?? "", editable: true, onTap: { picking = true })
                TextField("Untitled page", text: Binding<String>(
                    get: { model.draft?.title ?? "" },
                    set: { (value: String) in model.draft?.title = String(value.prefix(NoteEditorModel.titleMaxLength)) }))
                    .fmText(.bodyLarge)
                    .accessibilityIdentifier("note-page-title")
            }
            TextField("Description (Markdown)", text: Binding<String>(
                get: { model.draft?.description ?? "" },
                set: { (value: String) in model.draft?.description = String(value.prefix(NotePageModel.maxDescription)) }),
                axis: .vertical)
                .lineLimit(2...6)
                .fmText(.bodyMedium)
                .accessibilityIdentifier("note-page-description")
        }
    }

    private var accentSection: some View {
        Section {
            HStack(spacing: FmSpace.x3) {
                ForEach(NotePageAccent.allCases) { (option: NotePageAccent) in
                    let chosen: Bool = model.draft?.accent == option
                    Button {
                        model.draft?.accent = option
                    } label: {
                        Circle()
                            .fill(notePageColor(option, fm))
                            .frame(width: 24, height: 24)
                            .padding(3)
                            .overlay { Circle().strokeBorder(chosen ? fm.textPrimary : Color.clear, lineWidth: 2) }
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(Text(chosen ? "\(option.label), selected" : option.label))
                }
            }
        } header: {
            Text("Colour")
        }
    }

    private var layoutSection: some View {
        Section {
            Picker("Layout", selection: binding(\.layout, .outline)) {
                ForEach(NotePageLayout.allCases) { (option: NotePageLayout) in
                    Text(option.label).tag(option)
                }
            }
            .pickerStyle(.segmented)
            Toggle("Show note emoji in the outline", isOn: binding(\.showEmoji, true))
                .tint(fm.accent)
        } header: {
            Text("Layout")
        }
    }

    private var outlineSection: some View {
        Section {
            ForEach(model.draft?.outline ?? []) { (entry: NotePageEntry) in
                outlineEntry(entry)
            }
            .onMove { (offsets: IndexSet, destination: Int) in model.move(from: offsets, to: destination) }
            .onDelete { (offsets: IndexSet) in
                let ids: [String] = offsets.compactMap { (index: Int) -> String? in
                    guard let outline = model.draft?.outline, index < outline.count else { return nil }
                    return outline[index].id
                }
                for id in ids {
                    model.remove(id)
                }
            }
        } header: {
            HStack {
                Text("Outline")
                Spacer()
                Text("\(model.draft?.outline.count ?? 0) of \(NotePageOutline.maxEntries)")
            }
        } footer: {
            Text("Add notes, sections and links below. Drag the handle to reorder.")
        }
    }

    private func outlineEntry(_ entry: NotePageEntry) -> some View {
        let title: String = model.entryTitle(entry)
        let icon: String
        switch entry.kind {
        case .section: icon = "format_list_bulleted"
        case .link: icon = "link"
        case .note: icon = "sticky_note_2_outlined"
        }
        let outline: [NotePageEntry] = model.draft?.outline ?? []
        let index: Int = outline.firstIndex(where: { $0.id == entry.id }) ?? 0
        return HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 18, color: entry.kind == .section ? fm.textPrimary : fm.textSecondary)
            Text(title)
                .fmText(entry.kind == .section ? .labelLarge : .bodyMedium)
                .lineLimit(1)
            Spacer(minLength: 0)
            Menu {
                Button("Rename") {
                    renameText = entry.label.isEmpty ? title : entry.label
                    renaming = entry
                }
                Button("Indent") { model.indent(entry.id) }
                    .disabled(entry.depth >= NotePageEntry.maxDepth)
                Button("Outdent") { model.outdent(entry.id) }
                    .disabled(entry.depth == 0)
                if index > 0 {
                    Button("Move up") { model.moveUp(entry.id) }
                }
                if index < outline.count - 1 {
                    Button("Move down") { model.moveDown(entry.id) }
                }
                Button("Remove", role: .destructive) { model.remove(entry.id) }
            } label: {
                FmIcon("more_vert", size: 20, color: fm.textSecondary)
                    .frame(width: 36, height: 36)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel(Text("Edit \(title)"))
        }
        .padding(.leading, CGFloat(entry.depth) * 18)
        .accessibilityElement(children: .combine)
        .accessibilityAction(named: Text("Rename")) {
            renameText = entry.label.isEmpty ? title : entry.label
            renaming = entry
        }
        .accessibilityAction(named: Text("Indent")) { model.indent(entry.id) }
        .accessibilityAction(named: Text("Outdent")) { model.outdent(entry.id) }
        .accessibilityAction(named: Text("Move up")) { model.moveUp(entry.id) }
        .accessibilityAction(named: Text("Move down")) { model.moveDown(entry.id) }
    }

    private var room: Bool { (model.draft?.outline.count ?? 0) < NotePageOutline.maxEntries }

    @ViewBuilder private var addSection: some View {
        Section {
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton("Add note", icon: "add", minHeight: 30, onPressed: room ? { adding = .note } : nil)
                FmOutlinedButton("Add section", icon: "format_list_bulleted", minHeight: 30, onPressed: room ? { adding = .section } : nil)
                FmOutlinedButton("Add link", icon: "link", minHeight: 30, onPressed: room ? { adding = .link } : nil)
            }
            switch adding {
            case .note:
                HStack {
                    TextField("Search your notes", text: $model.noteQuery)
                        .textInputAutocapitalization(.never)
                    Button("Done") { adding = .none }
                        .buttonStyle(.borderless)
                }
                let choices = model.noteChoices
                if choices.isEmpty {
                    Text("No notes match.")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                ForEach(Array(choices.prefix(30)), id: \.id) { (choice: (id: String, title: String, emoji: String)) in
                    Button {
                        model.addNote(choice.id)
                    } label: {
                        Text(choice.emoji.isEmpty ? choice.title : "\(choice.emoji) \(choice.title)")
                            .fmText(.bodyMedium)
                    }
                    .buttonStyle(.borderless)
                }
            case .section:
                HStack {
                    TextField("Section name", text: $sectionName)
                    Button("Add") {
                        if model.addSection(sectionName) {
                            sectionName = ""
                            adding = .none
                        }
                    }
                    .buttonStyle(.borderless)
                    .disabled(sectionName.dartTrimmed.isEmpty)
                }
            case .link:
                TextField("Label", text: $linkLabel)
                TextField("https://", text: $linkUrl)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                if linkUrl.count > 8 && NotePageOutline.validLink(linkUrl) == nil {
                    Text("Links must start with https://")
                        .fmText(.bodySmall, color: fm.danger)
                }
                HStack {
                    Button("Cancel") {
                        adding = .none
                        linkLabel = ""
                        linkUrl = ""
                    }
                    .buttonStyle(.borderless)
                    Spacer()
                    Button("Add") {
                        let outcome: ToolsOutcome = model.addLink(label: linkLabel, url: linkUrl)
                        if let next = outcome.notice {
                            notice = FmNotice(next.message, isError: next.isError)
                        } else {
                            linkLabel = ""
                            linkUrl = ""
                            adding = .none
                        }
                    }
                    .buttonStyle(.borderless)
                    .disabled(NotePageOutline.validLink(linkUrl) == nil)
                }
            case .none:
                EmptyView()
            }
        }
    }
}
