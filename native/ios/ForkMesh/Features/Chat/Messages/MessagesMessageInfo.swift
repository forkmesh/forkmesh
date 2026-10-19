import FMApi
import FMChat
import SwiftUI
import UIKit

struct MessagesMessageInfoScreen: View {
    let bubble: ChatBubble
    let times: ChatReceiptTimes
    let life: MessagesBubbleLife
    let direct: Bool
    let colors: MessagesColors
    let onClose: () -> Void

    private var message: ChatMessage { bubble.row.message }
    private var mine: Bool { message.isSelf }
    private var ink: Color { colors.dark ? Color.white : Color.black }
    private var page: Color { colors.dark ? Color.black : Color(fmARGB: 0xFFF2F2F7) }
    private var card: Color { colors.dark ? Color(fmARGB: 0xFF1C1C1E) : Color.white }
    private var rule: Color { colors.dark ? Color.white.opacity(0.12) : Color.black.opacity(0.1) }

    static let stamp: DateFormatter = {
        let formatter: DateFormatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        formatter.doesRelativeDateFormatting = true
        return formatter
    }()

    static let seconds: DateFormatter = {
        let formatter: DateFormatter = DateFormatter()
        formatter.dateFormat = DateFormatter.dateFormat(fromTemplate: "jms", options: 0, locale: Locale.current) ?? "h:mm:ss a"
        return formatter
    }()

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    preview
                    section(mine ? "Your message" : "Message", rows: timeline)
                    if !extras.isEmpty { section("More", rows: extras) }
                    if let note { Text(note).font(.system(size: 13)).foregroundStyle(colors.meta).padding(.horizontal, 16) }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 32)
            }
        }
        .background(page.ignoresSafeArea())
        .environment(\.colorScheme, colors.dark ? .dark : .light)
        .accessibilityAction(.escape, onClose)
        .accessibilityIdentifier("chat-message-info")
    }

    private var header: some View {
        ZStack {
            Text("Message Info")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(ink)
            HStack {
                Button(action: onClose) {
                    Image(systemName: "chevron.left")
                        .font(.system(size: 19, weight: .semibold))
                        .foregroundStyle(ink)
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Back")
                Spacer()
            }
        }
        .padding(.horizontal, 6)
        .frame(height: 52)
    }

    private var preview: some View {
        HStack {
            if mine { Spacer(minLength: 48) }
            Text(previewText)
                .font(.system(size: 17))
                .foregroundStyle(mine ? colors.sentText : colors.receivedText)
                .lineLimit(8)
                .padding(.vertical, 8)
                .padding(.leading, mine ? 13 : 18)
                .padding(.trailing, mine ? 18 : 13)
                .background(
                    MessagesBubbleShape(fromSelf: mine, joinsAbove: false, joinsBelow: false)
                        .fill(mine ? colors.sentFill : colors.receivedFill))
            if !mine { Spacer(minLength: 48) }
        }
        .padding(.top, 8)
        .accessibilityElement(children: .combine)
    }

    private var previewText: String {
        if !bubble.row.text.isEmpty { return bubble.row.text }
        if let attachment = bubble.row.attachment {
            switch attachment.kind {
            case .image: return "Photo"
            case .audio: return "Audio message"
            case .video: return "Video"
            case .file: break
            }
        }
        return message.fileName.isEmpty ? "Message" : message.fileName
    }

    private struct Line: Identifiable {
        let title: String
        let value: String
        var detail: String = ""
        var icon: String
        var tint: Color? = nil
        var id: String { title }
    }

    private var timeline: [Line] {
        var lines: [Line] = []
        if !mine && !message.senderName.isEmpty {
            lines.append(Line(title: "From", value: message.senderName, icon: "person.crop.circle"))
        }
        if bubble.receipt == .sending {
            lines.append(Line(title: "Sent", value: "Sending…", icon: "paperplane"))
        } else {
            lines.append(timed("Sent", message.timestamp, icon: "paperplane"))
        }
        guard mine, direct else { return lines }
        let rank: Int = bubble.receipt?.rank ?? 0
        if let delivered = times.delivered {
            lines.append(timed("Delivered", delivered, icon: "checkmark.circle"))
        } else {
            let value: String = rank >= ChatReceipt.delivered.rank ? "Delivered" : "Not yet"
            lines.append(Line(title: "Delivered", value: value, icon: "checkmark.circle"))
        }
        if let read = times.read {
            lines.append(timed("Read", read, icon: "eye", tint: MessagesColors.blue))
        } else {
            let value: String = rank >= ChatReceipt.read.rank ? "Read" : "Not yet"
            lines.append(Line(title: "Read", value: value, icon: "eye"))
        }
        return lines
    }

    private var extras: [Line] {
        var lines: [Line] = []
        if let description = OfficeFrames.recipientsDescription(message.recipients) {
            lines.append(Line(title: "Recipients", value: description, icon: "person.2"))
        }
        if bubble.row.edited { lines.append(Line(title: "Edited", value: "Yes", icon: "pencil")) }
        if let expiresAt = life.expiresAt { lines.append(timed("Disappears", expiresAt, icon: "timer")) }
        if life.onServer { lines.append(Line(title: "Server copy", value: "Waiting for pickup", icon: "externaldrive")) }
        return lines
    }

    private var note: String? {
        guard mine, direct, bubble.receipt != .sending else { return nil }
        if times.read == nil && (bubble.receipt?.rank ?? 0) >= ChatReceipt.read.rank {
            return "Read before this device kept read times."
        }
        return nil
    }

    private func timed(_ title: String, _ date: Date, icon: String, tint: Color? = nil) -> Line {
        Line(
            title: title, value: MessagesMessageInfoScreen.stamp.string(from: date),
            detail: MessagesMessageInfoScreen.seconds.string(from: date), icon: icon, tint: tint)
    }

    private func section(_ title: String, rows: [Line]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title.uppercased())
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(colors.meta)
                .padding(.leading, 16)
            VStack(spacing: 0) {
                ForEach(Array(rows.enumerated()), id: \.element.id) { (index: Int, line: Line) in
                    if index > 0 { rule.frame(height: 0.5).padding(.leading, 48) }
                    row(line)
                }
            }
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(card))
        }
    }

    private func row(_ line: Line) -> some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: line.icon)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(line.tint ?? colors.meta)
                .frame(width: 22)
            Text(line.title)
                .font(.system(size: 16))
                .foregroundStyle(ink)
            Spacer(minLength: 12)
            VStack(alignment: .trailing, spacing: 1) {
                Text(line.value)
                    .font(.system(size: 16))
                    .foregroundStyle(line.value == "Not yet" ? colors.meta : ink)
                    .multilineTextAlignment(.trailing)
                if !line.detail.isEmpty {
                    Text(line.detail)
                        .font(.system(size: 12))
                        .monospacedDigit()
                        .foregroundStyle(colors.meta)
                }
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 11)
        .frame(minHeight: 50)
        .accessibilityElement(children: .combine)
    }
}

enum MessagesSwipeHeading {
    case left, right

    func matches(_ dx: CGFloat) -> Bool { self == .left ? dx < 0 : dx > 0 }
}

struct MessagesSideSwipe: ViewModifier {
    let heading: MessagesSwipeHeading
    let enabled: Bool
    let moved: (CGFloat) -> Void
    let ended: (CGFloat, CGFloat) -> Void

    private static func sideways(_ moved: CGSize) -> Bool { abs(moved.width) > abs(moved.height) * 1.5 }

    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.gesture(MessagesSidewaysPan(heading: heading, enabled: enabled, moved: moved, ended: ended))
        } else {
            content.simultaneousGesture(
                DragGesture(minimumDistance: 16)
                    .onChanged { (value: DragGesture.Value) in
                        guard Self.sideways(value.translation), heading.matches(value.translation.width) else { return }
                        moved(value.translation.width)
                    }
                    .onEnded { (value: DragGesture.Value) in
                        guard Self.sideways(value.translation) else {
                            ended(0, 0)
                            return
                        }
                        ended(value.translation.width, value.predictedEndTranslation.width)
                    },
                including: enabled ? .all : .subviews)
        }
    }
}

@available(iOS 18.0, *)
private struct MessagesSidewaysPan: UIGestureRecognizerRepresentable {
    let heading: MessagesSwipeHeading
    let enabled: Bool
    let moved: (CGFloat) -> Void
    let ended: (CGFloat, CGFloat) -> Void

    private static let flingSeconds: CGFloat = 0.25

    func makeCoordinator(converter: CoordinateSpaceConverter) -> Coordinator { Coordinator() }

    func makeUIGestureRecognizer(context: Context) -> UIPanGestureRecognizer {
        let pan = UIPanGestureRecognizer()
        pan.maximumNumberOfTouches = 1
        pan.delegate = context.coordinator
        context.coordinator.enabled = enabled
        context.coordinator.heading = heading
        return pan
    }

    func updateUIGestureRecognizer(_ recognizer: UIPanGestureRecognizer, context: Context) {
        context.coordinator.enabled = enabled
        context.coordinator.heading = heading
    }

    func handleUIGestureRecognizerAction(_ recognizer: UIPanGestureRecognizer, context: Context) {
        let dx: CGFloat = recognizer.translation(in: recognizer.view).x
        switch recognizer.state {
        case .began, .changed:
            moved(dx)
        case .ended:
            ended(dx, dx + recognizer.velocity(in: recognizer.view).x * Self.flingSeconds)
        case .cancelled, .failed:
            ended(0, 0)
        default:
            break
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var enabled = true
        var heading: MessagesSwipeHeading = .left

        func gestureRecognizerShouldBegin(_ gestureRecognizer: UIGestureRecognizer) -> Bool {
            guard enabled, let pan = gestureRecognizer as? UIPanGestureRecognizer else { return false }
            let velocity: CGPoint = pan.velocity(in: pan.view)
            let motion: CGPoint = velocity == .zero ? pan.translation(in: pan.view) : velocity
            return abs(motion.x) > abs(motion.y) * 1.5 && heading.matches(motion.x)
        }
    }
}

extension View {
    func messagesSideSwipe(
        _ heading: MessagesSwipeHeading, enabled: Bool = true, moved: @escaping (CGFloat) -> Void,
        ended: @escaping (CGFloat, CGFloat) -> Void
    ) -> some View {
        modifier(MessagesSideSwipe(heading: heading, enabled: enabled, moved: moved, ended: ended))
    }
}
