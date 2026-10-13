import FMApi
import FMBase
import FMChat
import FMCode
import SwiftUI
import UIKit

struct MessagesBubbleShape: Shape {
    let fromSelf: Bool
    let tail: Bool

    func path(in rect: CGRect) -> Path {
        let width: CGFloat = rect.width
        let height: CGFloat = rect.height
        let radius: CGFloat = min(18, height / 2, (width - 5) / 2)
        let control: CGFloat = radius * 0.45
        var path: Path = Path()
        path.move(to: CGPoint(x: 5 + radius, y: height))
        path.addLine(to: CGPoint(x: width - radius, y: height))
        path.addCurve(
            to: CGPoint(x: width, y: height - radius), control1: CGPoint(x: width - control, y: height),
            control2: CGPoint(x: width, y: height - control))
        path.addLine(to: CGPoint(x: width, y: radius))
        path.addCurve(
            to: CGPoint(x: width - radius, y: 0), control1: CGPoint(x: width, y: control),
            control2: CGPoint(x: width - control, y: 0))
        path.addLine(to: CGPoint(x: 5 + radius, y: 0))
        path.addCurve(
            to: CGPoint(x: 5, y: radius), control1: CGPoint(x: 5 + control, y: 0), control2: CGPoint(x: 5, y: control))
        if tail {
            path.addLine(to: CGPoint(x: 5, y: height - 10))
            path.addCurve(to: CGPoint(x: 0, y: height), control1: CGPoint(x: 5, y: height - 1), control2: CGPoint(x: 0, y: height))
            path.addLine(to: CGPoint(x: -1, y: height))
            path.addCurve(
                to: CGPoint(x: 12, y: height - 4), control1: CGPoint(x: 4, y: height + 1), control2: CGPoint(x: 8, y: height - 1))
            path.addCurve(
                to: CGPoint(x: 5 + radius, y: height), control1: CGPoint(x: 15, y: height),
                control2: CGPoint(x: 5 + radius, y: height))
        } else {
            path.addLine(to: CGPoint(x: 5, y: height - radius))
            path.addCurve(
                to: CGPoint(x: 5 + radius, y: height), control1: CGPoint(x: 5, y: height - control),
                control2: CGPoint(x: 5 + control, y: height))
        }
        path.closeSubpath()
        guard fromSelf else { return path.offsetBy(dx: rect.minX, dy: rect.minY) }
        let mirror: CGAffineTransform = CGAffineTransform(scaleX: -1, y: 1).translatedBy(x: -width, y: 0)
        return path.applying(mirror).offsetBy(dx: rect.minX, dy: rect.minY)
    }
}

struct MessagesColors {
    let dark: Bool
    let backdrop: Bool

    static let blue: Color = Color(fmARGB: 0xFF0A84FF)

    var sentFill: Color { MessagesColors.blue }
    var sentText: Color { Color.white }

    var receivedFill: Color {
        if backdrop { return dark ? Color(fmARGB: 0xC0262629) : Color(fmARGB: 0xD9F2F2F7) }
        return dark ? Color(fmARGB: 0xFF26262A) : Color(fmARGB: 0xFFE9E9EB)
    }

    var receivedText: Color { dark ? Color.white : Color.black }

    var meta: Color {
        if backdrop { return dark ? Color.white.opacity(0.85) : Color.black.opacity(0.7) }
        return dark ? Color(fmARGB: 0xFF8E8E93) : Color(fmARGB: 0xFF8A8A8E)
    }

    var canvas: Color { dark ? Color.black : Color.white }
}

struct MessagesBubbleRow: View {
    let bubble: ChatBubble
    let group: Bool
    let colors: MessagesColors
    let actions: ChatRowActions
    let onTapback: () -> Void
    let onPhoto: (ChatAttachment) -> Void
    @State private var confirmDelete: Bool = false
    @State private var selecting: Bool = false

    private var row: ChatRow { bubble.row }
    private var message: ChatMessage { row.message }
    private var mine: Bool { message.isSelf }
    private var copyText: String { row.text.isEmpty ? message.fileName : row.text }

    var body: some View {
        VStack(alignment: mine ? .trailing : .leading, spacing: 2) {
            if bubble.showsSender {
                Text(message.senderName)
                    .font(.system(size: 12))
                    .foregroundStyle(colors.meta)
                    .padding(.leading, 46)
                    .padding(.top, 6)
            }
            HStack(alignment: .bottom, spacing: 6) {
                if mine {
                    Spacer(minLength: 56)
                } else if group {
                    avatarSlot
                }
                content
                    .overlay(alignment: mine ? .topLeading : .topTrailing) { tapbacks }
                    .padding(.top, row.reactions.isEmpty ? 0 : 14)
                    .contextMenu { menu }
                    .onTapGesture(count: 2) { if actions.onReact != nil { onTapback() } }
                if !mine { Spacer(minLength: 56) }
            }
            if row.edited {
                Text("Edited")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(colors.meta)
                    .padding(mine ? .trailing : .leading, group && !mine ? 46 : 12)
            }
            if !bubble.receipt.isEmpty {
                Text(bubble.receipt)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(colors.meta)
                    .padding(.trailing, 12)
                    .accessibilityLabel(bubble.receipt)
            }
        }
        .frame(maxWidth: .infinity, alignment: mine ? .trailing : .leading)
        .padding(.top, bubble.position.startsRun ? 8 : 1)
        .padding(.horizontal, 10)
        .sheet(isPresented: $selecting) { FmSelectableTextSheet(text: copyText) }
        .confirmationDialog("Delete message?", isPresented: $confirmDelete, titleVisibility: .visible) {
            if let onDelete = actions.onDelete {
                Button("Unsend for everyone", role: .destructive, action: onDelete)
            }
            if let onHide = actions.onHide {
                Button("Delete for me", role: .destructive, action: onHide)
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    @ViewBuilder private var avatarSlot: some View {
        if bubble.position.hasTail {
            ChatSenderAvatar(message: message)
        } else {
            Color.clear.frame(width: 28, height: 1)
        }
    }

    @ViewBuilder private var content: some View {
        let attachment: ChatAttachment? = row.attachment
        VStack(alignment: mine ? .trailing : .leading, spacing: 2) {
            if let attachment, attachment.kind == .image {
                photo(attachment)
            } else if let attachment, attachment.kind == .audio {
                bubbleShell { MessagesAudioBubble(attachment: attachment, tint: mine ? colors.sentText : colors.receivedText) }
            } else if !message.fileName.isEmpty {
                bubbleShell {
                    ChatAttachmentView(fileName: message.fileName, attachment: attachment)
                        .tint(mine ? colors.sentText : MessagesColors.blue)
                }
            }
            if !row.text.isEmpty {
                if bubble.jumbo {
                    Text(row.text)
                        .font(.system(size: 52))
                        .padding(.horizontal, 6)
                        .accessibilityLabel(row.text)
                } else {
                    bubbleShell { MessagesBubbleText(text: row.text, color: mine ? colors.sentText : colors.receivedText) }
                }
            }
            if let app = message.app, !app.blocks.isEmpty {
                bubbleShell {
                    ChatAppBlocksView(
                        app: app, busy: actions.appBusy,
                        onAction: { (actionId: String, value: String) in actions.onAppAction?(actionId, value) })
                }
            }
        }
    }

    private func bubbleShell<Inner: View>(@ViewBuilder _ inner: () -> Inner) -> some View {
        let tail: Bool = bubble.position.hasTail
        return inner()
            .padding(.vertical, 7)
            .padding(.leading, mine ? 12 : 17)
            .padding(.trailing, mine ? 17 : 12)
            .background(MessagesBubbleShape(fromSelf: mine, tail: tail).fill(mine ? colors.sentFill : colors.receivedFill))
            .contentShape(MessagesBubbleShape(fromSelf: mine, tail: tail))
    }

    @ViewBuilder private func photo(_ attachment: ChatAttachment) -> some View {
        if let picture = UIImage(data: attachment.data) {
            Button(action: { onPhoto(attachment) }) {
                Image(uiImage: picture)
                    .resizable()
                    .scaledToFill()
                    .frame(width: MessagesBubbleRow.photoSize(picture).width, height: MessagesBubbleRow.photoSize(picture).height)
                    .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                            .stroke(Color.primary.opacity(0.08), lineWidth: 0.5))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Photo")
        }
    }

    static func photoSize(_ image: UIImage) -> CGSize {
        let maxSide: CGFloat = 240
        let width: CGFloat = max(1, image.size.width)
        let height: CGFloat = max(1, image.size.height)
        let scale: CGFloat = min(maxSide / width, maxSide / height)
        return CGSize(width: max(120, width * scale), height: max(90, height * scale))
    }

    @ViewBuilder private var tapbacks: some View {
        if !row.reactions.isEmpty {
            MessagesTapbackBadge(chips: row.reactions, colors: colors, onTap: { onTapback() })
                .offset(x: mine ? -14 : 14, y: -16)
        }
    }

    @ViewBuilder private var menu: some View {
        if let onReact = actions.onReact {
            ControlGroup {
                ForEach(ReactionCatalog.quick, id: \.self) { (value: String) in
                    Button(ReactionCatalog.emoji(for: value)) { onReact(value) }
                }
            }
            ChatReactionMenu(onPick: onReact)
        }
        ChatMessageLaterMenu(actions: actions)
        if !copyText.isEmpty {
            Button(action: copy) { Label("Copy", systemImage: "doc.on.doc") }
            Button(action: { selecting = true }) { Label("Select text", systemImage: "selection.pin.in.out") }
        }
        if let onEdit = actions.onEdit {
            Button(action: onEdit) { Label("Edit", systemImage: "pencil") }
        }
        if !row.text.isEmpty, let onPrompt = actions.onPrompt {
            Button(action: onPrompt) { Label("Send to prompt", systemImage: "text.bubble") }
        }
        if !row.text.isEmpty, let onIssue = actions.onIssue {
            Button(action: onIssue) { Label("Create issue", systemImage: "exclamationmark.circle") }
        }
        if let onProfile = actions.onProfile {
            Button(action: onProfile) { Label("View \(message.senderName)", systemImage: "person.crop.circle") }
        }
        if actions.onDelete != nil {
            Button(role: .destructive, action: { confirmDelete = true }) {
                Label("Undo Send", systemImage: "arrow.uturn.backward")
            }
        } else if actions.onHide != nil {
            Button(role: .destructive, action: { confirmDelete = true }) { Label("Delete", systemImage: "trash") }
        }
    }

    private func copy() {
        UIPasteboard.general.string = copyText
        actions.onCopied()
    }
}

struct MessagesBubbleText: View {
    let text: String
    let color: Color

    var body: some View {
        Text(attributed)
            .font(.system(size: 17))
            .foregroundStyle(color)
            .tint(color)
            .fixedSize(horizontal: false, vertical: true)
            .multilineTextAlignment(.leading)
            .environment(\.openURL, referenceOpener)
    }

    private var attributed: AttributedString {
        chatRichAttributed(referenceChatSegments(parseChatText(text), repo: ReferenceLinks.chatRepo))
    }
}

struct MessagesTapbackBadge: View {
    let chips: [ReactionChip]
    let colors: MessagesColors
    let onTap: () -> Void

    private var mine: Bool { chips.contains { $0.mine } }
    private var total: Int { chips.reduce(0) { $0 + $1.count } }

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 1) {
                ForEach(chips.prefix(3)) { (chip: ReactionChip) in
                    Text(chip.emoji).font(.system(size: 14))
                }
                if total > 1 {
                    Text("\(total)")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(mine ? Color.white : colors.receivedText)
                        .padding(.leading, 2)
                }
            }
            .padding(.horizontal, 7)
            .frame(minWidth: 30, minHeight: 28)
            .background(Capsule().fill(mine ? MessagesColors.blue : colors.receivedFill))
            .overlay(Capsule().stroke(colors.canvas, lineWidth: 2))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(chips.map { "\($0.label) \($0.count): \($0.names.joined(separator: ", "))" }.joined(separator: "; "))
    }
}

struct MessagesTapbackPicker: View {
    let chosen: Set<String>
    let onPick: (String) -> Void

    var body: some View {
        HStack(spacing: 4) {
            ForEach(ReactionCatalog.quick, id: \.self) { (value: String) in
                Button(action: { onPick(value) }) {
                    Text(ReactionCatalog.emoji(for: value))
                        .font(.system(size: 26))
                        .frame(width: 42, height: 42)
                        .background(Circle().fill(chosen.contains(value) ? MessagesColors.blue.opacity(0.85) : Color.clear))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(ReactionCatalog.label(for: value))
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .chatGlass(Capsule(), border: Color.secondary)
        .shadow(color: Color.black.opacity(0.18), radius: 12, y: 4)
    }
}

struct MessagesTypingBubble: View {
    let colors: MessagesColors
    let names: [String]

    var body: some View {
        HStack(alignment: .bottom, spacing: 6) {
            TimelineView(.periodic(from: Date(), by: 0.35)) { (context: TimelineViewDefaultContext) in
                let step: Int = Int(context.date.timeIntervalSinceReferenceDate / 0.35) % 3
                HStack(spacing: 5) {
                    ForEach(0..<3, id: \.self) { (index: Int) in
                        Circle()
                            .fill(colors.meta.opacity(index == step ? 1 : 0.45))
                            .frame(width: 8, height: 8)
                    }
                }
                .padding(.vertical, 12)
                .padding(.leading, 18)
                .padding(.trailing, 14)
                .background(MessagesBubbleShape(fromSelf: false, tail: true).fill(colors.receivedFill))
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(ChatExtras.typingLabel(names))
    }
}

struct MessagesSystemLine: View {
    let text: String
    let colors: MessagesColors
    var bold: Bool = false

    var body: some View {
        Text(text)
            .font(.system(size: 11, weight: bold ? .semibold : .regular))
            .foregroundStyle(colors.meta)
            .multilineTextAlignment(.center)
            .frame(maxWidth: .infinity)
            .padding(.horizontal, FmSpace.x6)
            .padding(.top, bold ? 14 : 2)
            .padding(.bottom, 2)
    }
}
