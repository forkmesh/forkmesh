import FMApi
import FMBase
import FMChat
import FMCode
import SwiftUI
import UIKit

struct MessagesBubbleShape: Shape {
    static let joinedRadius: CGFloat = 5

    let fromSelf: Bool
    var joinsAbove: Bool = false
    var joinsBelow: Bool = false

    func path(in rect: CGRect) -> Path {
        let width: CGFloat = rect.width
        let height: CGFloat = rect.height
        let radius: CGFloat = min(18, height / 2, (width - 5) / 2)
        let joined: CGFloat = min(MessagesBubbleShape.joinedRadius, radius)
        let top: CGFloat = joinsAbove ? joined : radius
        let bottom: CGFloat = joinsBelow ? joined : radius
        let control: CGFloat = 0.45
        var path: Path = Path()
        path.move(to: CGPoint(x: 5 + bottom, y: height))
        path.addLine(to: CGPoint(x: width - radius, y: height))
        path.addCurve(
            to: CGPoint(x: width, y: height - radius), control1: CGPoint(x: width - radius * control, y: height),
            control2: CGPoint(x: width, y: height - radius * control))
        path.addLine(to: CGPoint(x: width, y: radius))
        path.addCurve(
            to: CGPoint(x: width - radius, y: 0), control1: CGPoint(x: width, y: radius * control),
            control2: CGPoint(x: width - radius * control, y: 0))
        path.addLine(to: CGPoint(x: 5 + top, y: 0))
        path.addCurve(
            to: CGPoint(x: 5, y: top), control1: CGPoint(x: 5 + top * control, y: 0),
            control2: CGPoint(x: 5, y: top * control))
        path.addLine(to: CGPoint(x: 5, y: height - bottom))
        path.addCurve(
            to: CGPoint(x: 5 + bottom, y: height), control1: CGPoint(x: 5, y: height - bottom * control),
            control2: CGPoint(x: 5 + bottom * control, y: height))
        path.closeSubpath()
        guard fromSelf else { return path.offsetBy(dx: rect.minX, dy: rect.minY) }
        let mirror: CGAffineTransform = CGAffineTransform(scaleX: -1, y: 1).translatedBy(x: -width, y: 0)
        return path.applying(mirror).offsetBy(dx: rect.minX, dy: rect.minY)
    }
}

struct MessagesEqualWidthStack: Layout {
    var spacing: CGFloat

    private func width(_ proposal: ProposedViewSize, _ subviews: Subviews) -> CGFloat {
        let limit: CGFloat = proposal.width ?? .infinity
        return subviews.map { min($0.sizeThatFits(.unspecified).width, limit) }.max() ?? 0
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let shared: CGFloat = width(proposal, subviews)
        let heights: [CGFloat] = subviews.map { $0.sizeThatFits(ProposedViewSize(width: shared, height: nil)).height }
        return CGSize(width: shared, height: heights.reduce(0, +) + spacing * CGFloat(max(0, subviews.count - 1)))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y: CGFloat = bounds.minY
        for subview in subviews {
            let height: CGFloat = subview.sizeThatFits(ProposedViewSize(width: bounds.width, height: nil)).height
            subview.place(
                at: CGPoint(x: bounds.minX, y: y), proposal: ProposedViewSize(width: bounds.width, height: height))
            y += height + spacing
        }
    }
}

struct MessagesReplyQuote: View {
    let quote: ChatReplyQuote
    let author: String
    let inSent: Bool
    let colors: MessagesColors
    var onTap: (() -> Void)? = nil

    private var bar: Color { inSent ? Color.white : MessagesColors.blue }
    private var title: Color { inSent ? Color.white : MessagesColors.blue }
    private var excerpt: Color { inSent ? Color.white.opacity(0.88) : colors.receivedText.opacity(0.78) }
    private var fill: Color {
        if inSent { return Color.white.opacity(0.2) }
        return colors.dark ? Color.white.opacity(0.1) : Color.black.opacity(0.06)
    }

    var body: some View {
        HStack(spacing: 0) {
            bar.frame(width: 3)
            VStack(alignment: .leading, spacing: 1) {
                Text(author)
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(title)
                    .lineLimit(1)
                Text(quote.text.isEmpty ? "Message" : quote.text)
                    .font(.system(size: 14))
                    .foregroundStyle(excerpt)
                    .lineLimit(3)
                    .multilineTextAlignment(.leading)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fill)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .onTapGesture { onTap?() }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Reply to \(author): \(quote.text)")
        .accessibilityAddTraits(onTap == nil ? [] : .isButton)
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

struct MessagesBubbleLife: Equatable {
    var expiresAt: Date?
    var lifetime: TimeInterval = 0
    var onServer: Bool = false
}

struct MessagesExpiryRing: View {
    let expiresAt: Date
    let lifetime: TimeInterval
    let tint: Color

    var body: some View {
        TimelineView(.everyMinute) { (context: TimelineViewDefaultContext) in
            let left: TimeInterval = max(0, expiresAt.timeIntervalSince(context.date))
            let share: Double = lifetime > 0 ? min(1, left / lifetime) : 0
            ZStack {
                MessagesClockWedge(elapsed: 1 - share).fill(tint.opacity(0.3)).padding(1.6)
                Circle().stroke(tint, lineWidth: 1.2)
                Capsule()
                    .fill(tint)
                    .frame(width: 1.3, height: 3.6)
                    .offset(y: -1.6)
                    .rotationEffect(.degrees((1 - share) * 360))
                Circle().fill(tint).frame(width: 2, height: 2)
            }
            .frame(width: 11, height: 11)
            .accessibilityElement()
            .accessibilityLabel("Disappears in \(MessagesExpiryRing.remaining(left))")
        }
    }

    static func remaining(_ seconds: TimeInterval) -> String {
        let minutes: Int = max(1, Int((seconds / 60).rounded(.up)))
        if minutes < 60 { return minutes == 1 ? "1 minute" : "\(minutes) minutes" }
        let hours: Int = Int((Double(minutes) / 60).rounded())
        if hours < 48 { return hours == 1 ? "1 hour" : "\(hours) hours" }
        let days: Int = Int((Double(hours) / 24).rounded())
        return "\(days) days"
    }
}

struct MessagesClockWedge: Shape {
    let elapsed: Double

    func path(in rect: CGRect) -> Path {
        let center: CGPoint = CGPoint(x: rect.midX, y: rect.midY)
        var path: Path = Path()
        guard elapsed < 1 else { return path }
        path.move(to: center)
        path.addArc(
            center: center, radius: min(rect.width, rect.height) / 2, startAngle: .degrees(-90 + max(0, elapsed) * 360),
            endAngle: .degrees(270), clockwise: false)
        path.closeSubpath()
        return path
    }
}

struct MessagesBubbleMeta: View {
    let sentAt: Date
    let life: MessagesBubbleLife
    let tint: Color

    static let clock: DateFormatter = {
        let formatter: DateFormatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "h:mm a"
        return formatter
    }()

    var body: some View {
        HStack(spacing: 4) {
            if life.onServer {
                Circle()
                    .fill(tint)
                    .frame(width: 4, height: 4)
                    .accessibilityElement()
                    .accessibilityLabel("Still on the server")
            }
            Text(MessagesBubbleMeta.clock.string(from: sentAt))
                .font(.system(size: 11, weight: .medium))
                .monospacedDigit()
                .foregroundStyle(tint)
                .lineLimit(1)
                .fixedSize()
            if let expiresAt = life.expiresAt {
                MessagesExpiryRing(expiresAt: expiresAt, lifetime: life.lifetime, tint: tint)
            }
        }
    }
}

struct MessagesBubbleSheen<S: Shape>: View {
    let shape: S
    let fill: Color
    let mine: Bool
    let dark: Bool

    var body: some View {
        ZStack {
            if !mine { shape.fill(.ultraThinMaterial) }
            shape.fill(
                LinearGradient(
                    colors: [fill.opacity(mine ? 0.78 : 0.55), fill.opacity(mine ? 0.98 : 0.8)], startPoint: .top,
                    endPoint: .bottom))
            shape.fill(
                LinearGradient(
                    colors: [Color.white.opacity(dark ? 0.14 : 0.32), Color.white.opacity(0)], startPoint: .top,
                    endPoint: .center))
            shape.stroke(
                LinearGradient(
                    colors: [Color.white.opacity(dark ? 0.35 : 0.7), Color.white.opacity(0.04)], startPoint: .top,
                    endPoint: .bottom), lineWidth: 0.8)
        }
        .shadow(color: Color.black.opacity(dark ? 0.32 : 0.1), radius: 6, y: 2)
    }
}

extension View {
    @ViewBuilder func messagesBubbleGlass<S: Shape>(_ shape: S, fill: Color, mine: Bool, dark: Bool) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.glassEffect(.regular.tint(fill.opacity(mine ? 0.88 : 0.5)), in: shape)
        } else {
            self.background(MessagesBubbleSheen(shape: shape, fill: fill, mine: mine, dark: dark))
        }
        #else
        self.background(MessagesBubbleSheen(shape: shape, fill: fill, mine: mine, dark: dark))
        #endif
    }
}

struct MessagesBubbleRow: View {
    let bubble: ChatBubble
    let group: Bool
    let colors: MessagesColors
    let actions: ChatRowActions
    let onTapback: () -> Void
    let onPhoto: (ChatAttachment) -> Void
    var quoteAuthor: String = ""
    var onQuote: (() -> Void)? = nil
    var life: MessagesBubbleLife = MessagesBubbleLife()
    @State private var confirmDelete: Bool = false
    @State private var selecting: Bool = false

    private var row: ChatRow { bubble.row }
    private var message: ChatMessage { row.message }
    private var mine: Bool { message.isSelf }
    private var copyText: String { row.text.isEmpty ? message.fileName : row.text }
    private var jumbo: Bool { bubble.jumbo && message.replyTo == nil }
    private var joinsAbove: Bool { !bubble.position.startsRun }
    private var joinsBelow: Bool { !bubble.position.hasTail }

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
                    .overlay(alignment: tapbacksLead ? .topLeading : .topTrailing) { tapbacks }
                    .overlay(alignment: .topTrailing) {
                        ChatStickerStack(chips: row.stickers, onToggle: actions.onSticker)
                            .offset(x: 6, y: -14)
                    }
                    .padding(.top, row.reactions.isEmpty && row.stickers.isEmpty ? 0 : 14)
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
            if let summary = row.thread, let onThread = actions.onThread {
                Button(action: onThread) {
                    Text(summary.label)
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(MessagesColors.blue)
                }
                .buttonStyle(.plain)
                .padding(mine ? .trailing : .leading, group && !mine ? 46 : 12)
                .accessibilityHint("Opens the replies")
            }
            if receiptPlace == .below {
                receiptBadge(tint: colors.meta, fill: colors.canvas, accent: MessagesColors.blue)
                    .padding(.trailing, 12)
            }
        }
        .frame(maxWidth: .infinity, alignment: mine ? .trailing : .leading)
        .padding(.top, bubble.position.startsRun ? 8 : 2)
        .padding(.horizontal, 10)
        .sheet(isPresented: $selecting) { FmSelectableTextSheet(text: copyText) }
        .confirmationDialog("Delete message?", isPresented: $confirmDelete, titleVisibility: .visible) {
            if let onDelete = actions.onDelete {
                Button("Delete for everyone", role: .destructive, action: onDelete)
            }
            if let onHide = actions.onHide {
                Button("Delete for me", role: .destructive, action: onHide)
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    private enum ReceiptPlace {
        case text, photo, shell, below
    }

    private var receiptPlace: ReceiptPlace {
        if !row.text.isEmpty { return jumbo ? .below : .text }
        guard let attachment = row.attachment else { return message.fileName.isEmpty ? .below : .shell }
        if attachment.kind == .image { return FmImageCache.image(attachment.data) == nil ? .below : .photo }
        return .shell
    }

    private func receiptBadge(tint: Color, fill: Color, accent: Color) -> some View {
        HStack(spacing: 4) {
            MessagesBubbleMeta(sentAt: message.timestamp, life: life, tint: tint)
            if let label = OfficeFrames.recipientsLabel(message.recipients),
                let description = OfficeFrames.recipientsDescription(message.recipients)
            {
                Text(label)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(tint)
                    .accessibilityLabel(description)
            }
            if let receipt = bubble.receipt {
                ChatReceiptMark(receipt: receipt, muted: tint, background: fill, accent: accent)
            }
        }
    }

    private var inBubbleReceipt: some View {
        receiptBadge(
            tint: mine ? colors.sentText.opacity(0.78) : colors.meta.opacity(0.9),
            fill: mine ? colors.sentFill : colors.receivedFill, accent: mine ? colors.sentText : MessagesColors.blue)
            .padding(.bottom, 1)
    }

    @ViewBuilder private func withReceipt<Inner: View>(_ place: ReceiptPlace, @ViewBuilder _ inner: () -> Inner) -> some View {
        if receiptPlace == place {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .lastTextBaseline, spacing: 8) {
                    inner()
                    inBubbleReceipt
                }
                VStack(alignment: .trailing, spacing: 2) {
                    inner()
                    inBubbleReceipt
                }
            }
        } else {
            inner()
        }
    }

    @ViewBuilder private var avatarSlot: some View {
        if bubble.position.hasTail {
            ChatSenderAvatar(message: message)
                .onTapGesture { actions.onProfile?() }
                .accessibilityAddTraits(actions.onProfile == nil ? [] : .isButton)
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
                bubbleShell {
                    withReceipt(.shell) {
                        MessagesAudioBubble(attachment: attachment, tint: mine ? colors.sentText : colors.receivedText)
                    }
                }
            } else if !message.fileName.isEmpty {
                bubbleShell {
                    withReceipt(.shell) {
                        ChatAttachmentView(fileName: message.fileName, attachment: attachment)
                            .tint(mine ? colors.sentText : MessagesColors.blue)
                    }
                }
            }
            if !row.text.isEmpty {
                if jumbo {
                    Text(row.text)
                        .font(.system(size: 52))
                        .padding(.horizontal, 6)
                        .accessibilityLabel(row.text)
                } else {
                    bubbleShell {
                        if let quote = message.replyTo {
                            MessagesEqualWidthStack(spacing: 6) {
                                MessagesReplyQuote(
                                    quote: quote, author: quoteAuthor.isEmpty ? quote.senderName : quoteAuthor, inSent: mine,
                                    colors: colors, onTap: onQuote)
                                withReceipt(.text) {
                                    MessagesBubbleText(text: row.text, color: mine ? colors.sentText : colors.receivedText)
                                }
                            }
                        } else {
                            withReceipt(.text) {
                                MessagesBubbleText(text: row.text, color: mine ? colors.sentText : colors.receivedText)
                            }
                        }
                    }
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

    private var shellShape: MessagesBubbleShape {
        MessagesBubbleShape(fromSelf: mine, joinsAbove: joinsAbove, joinsBelow: joinsBelow)
    }

    private var photoShape: UnevenRoundedRectangle {
        let near: CGFloat = MessagesBubbleShape.joinedRadius
        let top: CGFloat = joinsAbove ? near : 18
        let bottom: CGFloat = joinsBelow ? near : 18
        return UnevenRoundedRectangle(
            topLeadingRadius: mine ? 18 : top, bottomLeadingRadius: mine ? 18 : bottom,
            bottomTrailingRadius: mine ? bottom : 18, topTrailingRadius: mine ? top : 18, style: .continuous)
    }

    private func bubbleShell<Inner: View>(@ViewBuilder _ inner: () -> Inner) -> some View {
        inner()
            .padding(.vertical, 8)
            .padding(.leading, mine ? 13 : 18)
            .padding(.trailing, mine ? 18 : 13)
            .messagesBubbleGlass(
                shellShape, fill: mine ? colors.sentFill : colors.receivedFill, mine: mine, dark: colors.dark)
            .contentShape(shellShape)
    }

    @ViewBuilder private func photo(_ attachment: ChatAttachment) -> some View {
        if let picture = FmImageCache.image(attachment.data) {
            Button(action: { onPhoto(attachment) }) {
                Image(uiImage: picture)
                    .resizable()
                    .scaledToFill()
                    .frame(width: MessagesBubbleRow.photoSize(picture).width, height: MessagesBubbleRow.photoSize(picture).height)
                    .clipShape(photoShape)
                    .overlay(photoShape.stroke(Color.primary.opacity(0.08), lineWidth: 0.5))
                    .overlay(alignment: .bottomTrailing) {
                        if receiptPlace == .photo {
                            receiptBadge(tint: Color.white, fill: Color(white: 0.15), accent: Color.white)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 3)
                                .background(Capsule().fill(Color.black.opacity(0.45)))
                                .padding(8)
                        }
                    }
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

    private var tapbacksLead: Bool { mine || !row.stickers.isEmpty }

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
        if let onSticker = actions.onSticker {
            ChatStickerMenu(onPick: onSticker)
        }
        if let onReply = actions.onReply ?? actions.onThread {
            Button(action: onReply) { Label("Reply", systemImage: "arrowshape.turn.up.left") }
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
                Label("Delete", systemImage: "trash")
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
                .messagesBubbleGlass(
                    MessagesBubbleShape(fromSelf: false), fill: colors.receivedFill, mine: false,
                    dark: colors.dark)
            }
            Text(ChatExtras.typingLabel(names))
                .font(.system(size: 12))
                .foregroundStyle(colors.meta)
                .lineLimit(1)
                .padding(.bottom, 6)
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
