import FMBase
import FMTools
import SwiftUI
import UIKit

struct NoteTextPalette: Equatable {
    let text: UIColor
    let mark: UIColor
    let tint: UIColor
}

struct NoteTextView: UIViewRepresentable {
    @Environment(\.fmPalette) private var fm

    @Binding var text: String
    let editable: Bool
    let highlight: Range<Int>?
    let request: NoteSelectionRequest?
    let canComment: Bool
    let onSelect: (Range<Int>) -> Void
    let onComment: () -> Void

    func makeCoordinator() -> NoteTextCoordinator {
        NoteTextCoordinator(parent: self)
    }

    var palette: NoteTextPalette {
        NoteTextPalette(text: UIColor(fm.textPrimary), mark: UIColor(fm.accent).withAlphaComponent(0.22), tint: UIColor(fm.accent))
    }

    func makeUIView(context: Context) -> UITextView {
        let view = UITextView()
        let coordinator: NoteTextCoordinator = context.coordinator
        coordinator.parent = self
        view.delegate = coordinator
        view.backgroundColor = .clear
        view.autocapitalizationType = .sentences
        view.adjustsFontForContentSizeCategory = true
        view.accessibilityIdentifier = "note-markdown"
        view.isEditable = editable
        view.isSelectable = true
        coordinator.applying = true
        coordinator.palette = palette
        view.text = text
        coordinator.restyle(view)
        coordinator.applying = false
        return view
    }

    func updateUIView(_ view: UITextView, context: Context) {
        let coordinator: NoteTextCoordinator = context.coordinator
        coordinator.parent = self
        coordinator.applying = true
        defer { coordinator.applying = false }
        if view.isEditable != editable {
            view.isEditable = editable
        }
        var changed = false
        if coordinator.palette != palette {
            coordinator.palette = palette
            changed = true
        }
        if view.text != text {
            let selection: NSRange = view.selectedRange
            view.text = text
            let length: Int = (text as NSString).length
            let location: Int = min(selection.location, length)
            view.selectedRange = NSRange(location: location, length: min(selection.length, length - location))
            changed = true
        }
        if changed || coordinator.appliedHighlight != highlight {
            coordinator.restyle(view)
        }
        if let request = request, request.serial != coordinator.appliedRequest {
            coordinator.appliedRequest = request.serial
            let range = NSRange(location: request.range.lowerBound, length: request.range.count)
            if range.location + range.length <= (view.text as NSString).length {
                view.selectedRange = range
                view.scrollRangeToVisible(range)
            }
        }
    }
}

@MainActor
final class NoteTextCoordinator: NSObject, UITextViewDelegate {
    var parent: NoteTextView
    var applying = false
    var appliedHighlight: Range<Int>?
    var appliedRequest = 0
    var palette: NoteTextPalette?

    init(parent: NoteTextView) {
        self.parent = parent
    }

    private var font: UIFont {
        let base: UIFont = UIFont(name: FmFonts.mono, size: FmType.small) ?? UIFont.monospacedSystemFont(ofSize: FmType.small, weight: .regular)
        return UIFontMetrics(forTextStyle: .body).scaledFont(for: base)
    }

    private var base: [NSAttributedString.Key: Any] {
        let style = NSMutableParagraphStyle()
        let font: UIFont = self.font
        style.lineSpacing = FmFonts.lineSpacing(size: font.pointSize, height: FmType.leadingSnug)
        return [
            NSAttributedString.Key.font: font,
            NSAttributedString.Key.foregroundColor: palette?.text ?? UIColor.label,
            NSAttributedString.Key.paragraphStyle: style,
        ]
    }

    func restyle(_ view: UITextView) {
        if view.markedTextRange != nil {
            return
        }
        let attributes = base
        let storage: NSTextStorage = view.textStorage
        let full = NSRange(location: 0, length: storage.length)
        let selection: NSRange = view.selectedRange
        storage.beginEditing()
        storage.setAttributes(attributes, range: full)
        let highlight: Range<Int>? = parent.highlight
        if let highlight = highlight, let mark = palette?.mark, highlight.upperBound <= storage.length {
            storage.addAttribute(
                NSAttributedString.Key.backgroundColor, value: mark,
                range: NSRange(location: highlight.lowerBound, length: highlight.count))
        }
        storage.endEditing()
        view.selectedRange = selection
        view.typingAttributes = attributes
        view.tintColor = palette?.tint
        appliedHighlight = highlight
    }

    func textViewDidChange(_ textView: UITextView) {
        let value: String = textView.text ?? ""
        if parent.text != value {
            parent.text = value
        }
    }

    func textViewDidChangeSelection(_ textView: UITextView) {
        if textView.markedTextRange == nil {
            textView.typingAttributes = base
        }
        if applying {
            return
        }
        let range: NSRange = textView.selectedRange
        parent.onSelect(range.location..<(range.location + range.length))
    }

    func textView(_ textView: UITextView, editMenuForTextIn range: NSRange, suggestedActions: [UIMenuElement]) -> UIMenu? {
        guard parent.canComment, range.length > 0 else {
            return nil
        }
        let comment = UIAction(title: "Comment", image: UIImage(systemName: "text.bubble")) { [weak self] (_: UIAction) in
            guard let self = self else {
                return
            }
            self.parent.onSelect(range.location..<(range.location + range.length))
            self.parent.onComment()
        }
        return UIMenu(children: [comment] + suggestedActions)
    }
}

struct NotePresenceLine: View {
    @Environment(\.fmPalette) private var fm
    let room: NoteRoomModel
    @State private var showing: Bool = false

    var body: some View {
        Button {
            showing = true
        } label: {
            HStack(spacing: FmSpace.x1) {
                ForEach(room.faces) { (peer: NotePeer) in
                    NotePeerFace(peer: peer)
                }
                if room.overflow > 0 {
                    Text("+\(room.overflow)")
                        .fmText(.labelSmall, color: fm.textSecondary)
                        .frame(minWidth: 24, minHeight: 24)
                        .padding(.horizontal, 2)
                        .background(fm.bgRaised)
                        .clipShape(Capsule())
                        .overlay { Capsule().strokeBorder(fm.border, lineWidth: 1) }
                        .accessibilityLabel(Text("\(room.overflow) more"))
                }
                Text(room.label)
                    .fmText(.labelSmall, color: fm.textTertiary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(room.everyone.isEmpty)
        .accessibilityElement(children: .combine)
        .accessibilityHint(Text("Shows everyone here"))
        .accessibilityIdentifier("note-presence")
        .popover(isPresented: $showing) {
            NotePeopleList(room: room)
                .presentationCompactAdaptation(.popover)
        }
    }
}

private struct NotePeerFace: View {
    @Environment(\.fmPalette) private var fm
    let peer: NotePeer

    var body: some View {
        Text(peer.initials)
            .fmText(.labelSmall, color: fm.textPrimary)
            .frame(width: 24, height: 24)
            .background(fm.bgRaised)
            .clipShape(Circle())
            .overlay { Circle().strokeBorder(peer.editing ? fm.accent : fm.border, lineWidth: 1) }
            .overlay(alignment: .bottomTrailing) {
                if peer.editing {
                    FmIcon("edit", size: 8, color: fm.onPrimary)
                        .frame(width: 12, height: 12)
                        .background(fm.accent)
                        .clipShape(Circle())
                        .offset(x: 3, y: 3)
                }
            }
            .accessibilityLabel(Text(peer.stateLabel))
    }
}

private struct NotePeopleList: View {
    @Environment(\.fmPalette) private var fm
    let room: NoteRoomModel

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text("Here now")
                .fmText(.labelLarge)
            ForEach(room.everyone) { (peer: NotePeer) in
                HStack(spacing: FmSpace.x2) {
                    NotePeerFace(peer: peer)
                    Text(peer.name)
                        .fmText(.bodyMedium)
                    Spacer(minLength: FmSpace.x4)
                    Text(peer.state.rawValue)
                        .fmText(.bodySmall, color: peer.editing ? fm.accent : fm.textTertiary)
                }
                .accessibilityElement(children: .combine)
            }
        }
        .padding(FmSpace.x4)
        .frame(minWidth: 220, alignment: .leading)
        .accessibilityIdentifier("note-people")
    }
}

struct NoteCommentsButton: View {
    @Environment(\.fmPalette) private var fm
    let comments: NoteCommentsModel
    let onPressed: () -> Void

    var body: some View {
        FmIconButton(label: comments.buttonLabel, onPressed: onPressed) {
            FmIcon("chat_bubble_outline", size: 22, tone: .primary)
                .overlay(alignment: .topTrailing) {
                    if comments.openCount > 0 {
                        Text("\(comments.openCount)")
                            .fmText(.labelSmall, color: fm.onPrimary, tabular: true)
                            .padding(.horizontal, 4)
                            .frame(minWidth: 16, minHeight: 16)
                            .background(fm.accent)
                            .clipShape(Capsule())
                            .offset(x: 8, y: -6)
                    }
                }
        }
        .accessibilityIdentifier("note-comments")
    }
}

struct NoteCommentsSheet: View {
    @Environment(\.fmPalette) private var fm
    let editor: NoteEditorModel
    @Bindable var comments: NoteCommentsModel
    @State private var notice: FmNotice?
    @State private var pendingDelete: NoteComment?

    var body: some View {
        let list: NoteThreadList = comments.threads(markdown: editor.markdown)
        VStack(alignment: .leading, spacing: 0) {
            FmPanelHeader(
                title: NoteCommentsModel.title,
                subtitle: comments.openCount == 1 ? "1 open thread" : "\(comments.openCount) open threads",
                trailing: comments.composing ? nil : FmTextButton("Comment", icon: "add_comment_outlined", onPressed: {
                    comments.startComment(nil)
                }).accessibilityIdentifier("note-comment-whole").fmAny)
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
            }
            ScrollViewReader { (proxy: ScrollViewProxy) in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                        if comments.composing {
                            composer
                        }
                        content(list)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x5)
                }
                .onAppear { scroll(proxy) }
                .onChange(of: comments.loaded) { (_: Bool, _: Bool) in scroll(proxy) }
            }
        }
        .task { await comments.load() }
        .fmSheetStyle(fitsContent: false)
        .fmDialog(isPresented: deleteShown) {
            deleteDialog
        }
    }

    private var deleteShown: Binding<Bool> {
        Binding<Bool>(get: { pendingDelete != nil }, set: { (shown: Bool) in if !shown { pendingDelete = nil } })
    }

    @ViewBuilder private var deleteDialog: some View {
        if let comment = pendingDelete {
            let words: ToolsConfirmation = comments.deleteConfirmation(comment)
            FmConfirmDialog(
                title: words.title, message: words.message, confirmLabel: words.confirm, destructive: words.destructive,
                onCancel: { pendingDelete = nil },
                onConfirm: {
                    pendingDelete = nil
                    Task { report(await comments.delete(comment)) }
                })
        }
    }

    private func scroll(_ proxy: ScrollViewProxy) {
        guard let id = comments.focusedThreadId else {
            return
        }
        DispatchQueue.main.async {
            withAnimation { proxy.scrollTo(id, anchor: .top) }
        }
    }

    @ViewBuilder private func content(_ list: NoteThreadList) -> some View {
        if !comments.loaded && !comments.error.isEmpty {
            FmEmptyState(icon: "chat_bubble_outline", title: "Comments unavailable", message: comments.error)
        } else if !comments.loaded {
            FmSkeletonList(rows: 3, hasLeading: false)
        } else if list.all.isEmpty && !comments.composing {
            Text(NoteCommentsModel.emptyMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.vertical, FmSpace.x3)
        }
        ForEach(list.open) { (thread: NoteThread) in
            card(thread)
        }
        if !list.resolved.isEmpty {
            Button {
                comments.showResolved.toggle()
            } label: {
                HStack(spacing: FmSpace.x1) {
                    FmIcon(comments.showResolved ? "expand_more" : "chevron_right", size: 18)
                    Text(comments.resolvedTitle(list))
                        .fmText(.labelMedium, color: fm.textSecondary)
                    Spacer(minLength: 0)
                }
                .frame(minHeight: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("note-comments-resolved")
            if comments.showResolved {
                ForEach(list.resolved) { (thread: NoteThread) in
                    card(thread)
                }
            }
        }
    }

    private func card(_ thread: NoteThread) -> some View {
        NoteThreadCard(
            thread: thread, comments: comments, focused: comments.focusedThreadId == thread.id,
            onPick: {
                editor.pick(thread)
                editor.commentsOpen = false
            },
            onDelete: { (comment: NoteComment) in pendingDelete = comment },
            onReport: report)
            .id(thread.id)
    }

    private var composer: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if let quote = comments.draftQuote {
                NoteQuoteBlock(text: quote.quote, removed: false)
            } else {
                Text(NoteCommentsModel.wholeNote)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            FmTextField(text: $comments.draft, hint: "Add a comment. Type @ to mention someone.", multiline: true, autofocus: true)
                .accessibilityIdentifier("note-comment-draft")
            NoteMentionChips(text: $comments.draft, comments: comments)
            HStack(spacing: FmSpace.x2) {
                Spacer(minLength: 0)
                FmTextButton("Cancel", onPressed: { comments.cancelComment() })
                FmFilledButton("Comment", onPressed: comments.busy || comments.draft.dartTrimmed.isEmpty ? nil : {
                    Task { report(await comments.post()) }
                })
                .accessibilityIdentifier("note-comment-post")
            }
        }
        .padding(FmSpace.x3)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay { FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.accent, lineWidth: 1) }
    }

    private func report(_ outcome: ToolsOutcome) {
        if let next = outcome.notice {
            notice = FmNotice(next.message, isError: next.isError)
        }
    }
}

private struct NoteQuoteBlock: View {
    @Environment(\.fmPalette) private var fm
    let text: String
    let removed: Bool

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            RoundedRectangle(cornerRadius: 2)
                .fill(removed ? fm.border : fm.accent)
                .frame(width: 3)
            Text(text)
                .fmMono(size: FmType.meta, color: removed ? fm.textTertiary : fm.textSecondary)
                .strikethrough(removed, color: fm.textTertiary)
                .lineLimit(4)
                .truncationMode(.tail)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .fixedSize(horizontal: false, vertical: true)
    }
}

private struct NoteThreadCard: View {
    @Environment(\.fmPalette) private var fm
    let thread: NoteThread
    @Bindable var comments: NoteCommentsModel
    let focused: Bool
    let onPick: () -> Void
    let onDelete: (NoteComment) -> Void
    let onReport: (ToolsOutcome) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            quote
            NoteCommentRow(comment: thread.comment, comments: comments, onDelete: onDelete, onReport: onReport)
            ForEach(thread.replies) { (reply: NoteComment) in
                NoteCommentRow(comment: reply, comments: comments, onDelete: onDelete, onReport: onReport)
                    .padding(.leading, FmSpace.x4)
            }
            if thread.resolved {
                Text(comments.resolvedLine(thread))
                    .fmText(.bodySmall, color: fm.textTertiary)
            } else {
                replyField
            }
            if comments.canResolve(thread) {
                HStack {
                    Spacer(minLength: 0)
                    FmTextButton(
                        thread.resolved ? "Reopen" : "Resolve", icon: thread.resolved ? "undo" : "check_circle_outline",
                        minHeight: 36,
                        onPressed: comments.busy ? nil : {
                            Task { onReport(await comments.setResolved(thread, !thread.resolved)) }
                        })
                        .accessibilityIdentifier("note-thread-resolve-\(thread.id)")
                }
            }
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay { FmRoundedShape(radius: FmRadius.lg).strokeBorder(focused ? fm.accent : fm.border, lineWidth: 1) }
        .accessibilityIdentifier("note-thread-\(thread.id)")
    }

    @ViewBuilder private var quote: some View {
        switch thread.place {
        case .whole:
            Text(NoteCommentsModel.wholeNote)
                .fmText(.bodySmall, color: fm.textTertiary)
        case .detached:
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                NoteQuoteBlock(text: thread.comment.quote, removed: true)
                Text(NoteCommentsModel.detachedNote)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
        case .inline:
            Button(action: onPick) {
                NoteQuoteBlock(text: thread.comment.quote, removed: false)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityHint(Text("Shows this passage in the note"))
        }
    }

    private var replyBinding: Binding<String> {
        Binding<String>(
            get: { comments.replies[thread.id] ?? "" },
            set: { (value: String) in comments.replies[thread.id] = value })
    }

    private var replyField: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(alignment: .bottom, spacing: FmSpace.x1) {
                FmTextField(text: replyBinding, hint: "Reply", multiline: true)
                    .accessibilityIdentifier("note-thread-reply-\(thread.id)")
                FmIconButton(icon: "send", label: "Send reply", size: 20, color: fm.accent, onPressed: comments.busy || replyBinding.wrappedValue.dartTrimmed.isEmpty ? nil : {
                    Task { onReport(await comments.reply(to: thread.id)) }
                })
            }
            NoteMentionChips(text: replyBinding, comments: comments)
        }
    }
}

private struct NoteCommentRow: View {
    @Environment(\.fmPalette) private var fm
    let comment: NoteComment
    @Bindable var comments: NoteCommentsModel
    let onDelete: (NoteComment) -> Void
    let onReport: (ToolsOutcome) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                Text(comments.authorName(comment))
                    .fmText(.labelMedium)
                Text(comments.when(comment))
                    .fmText(.bodySmall, color: fm.textTertiary)
                Spacer(minLength: 0)
                if comments.canEdit(comment) || comments.canDelete(comment) {
                    menu
                }
            }
            if comments.editingId == comment.id {
                editor
            } else {
                bodyText(comment)
                    .fmText(.bodyMedium)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
        }
    }

    private func bodyText(_ comment: NoteComment) -> Text {
        var out = AttributedString()
        for span in NoteMentions.segments(comment.body, mentions: comment.mentions) {
            var part = AttributedString(span.text)
            if span.mention {
                part.foregroundColor = fm.accent
                part.inlinePresentationIntent = .stronglyEmphasized
            }
            out += part
        }
        return Text(out)
    }

    private var menu: some View {
        Menu {
            if comments.canEdit(comment) {
                Button("Edit") { comments.startEdit(comment) }
            }
            if comments.canDelete(comment) {
                Button("Delete", role: .destructive) { onDelete(comment) }
            }
        } label: {
            FmIcon("more_horiz", size: 18)
                .frame(width: 32, height: 32)
                .contentShape(Rectangle())
        }
        .accessibilityLabel(Text("Comment actions"))
        .accessibilityIdentifier("note-comment-menu-\(comment.id)")
    }

    private var editor: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmTextField(text: $comments.editText, hint: "Edit comment", multiline: true, autofocus: true)
            NoteMentionChips(text: $comments.editText, comments: comments)
            HStack(spacing: FmSpace.x2) {
                Spacer(minLength: 0)
                FmTextButton("Cancel", minHeight: 36, onPressed: { comments.cancelEdit() })
                FmFilledButton("Save", minHeight: 36, onPressed: comments.busy ? nil : {
                    Task { onReport(await comments.saveEdit()) }
                })
            }
        }
    }
}

private struct NoteMentionChips: View {
    @Environment(\.fmPalette) private var fm
    @Binding var text: String
    let comments: NoteCommentsModel

    var body: some View {
        let names: [String] = comments.suggestions(for: text)
        if !names.isEmpty {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x1) {
                    ForEach(names, id: \.self) { (name: String) in
                        Button {
                            text = NoteMentions.complete(text, with: name)
                        } label: {
                            Text("@\(name)")
                                .fmText(.labelMedium, color: fm.accent)
                                .padding(.horizontal, FmSpace.x3)
                                .frame(minHeight: 32)
                                .background(fm.accentSubtle)
                                .clipShape(Capsule())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(Text("Mention \(name)"))
                    }
                }
            }
            .accessibilityIdentifier("note-mention-suggestions")
        }
    }
}
