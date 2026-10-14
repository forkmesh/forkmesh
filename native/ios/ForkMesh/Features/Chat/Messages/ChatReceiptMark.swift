import FMApi
import SwiftUI

struct ChatReceiptMark: View {
    let receipt: ChatReceipt
    let muted: Color
    let background: Color
    var accent: Color = MessagesColors.blue

    static let box: CGSize = CGSize(width: 22, height: 14)
    static let size: CGSize = CGSize(width: 17, height: 11)
    static let front: CGPoint = CGPoint(x: 14, y: 7)
    static let back: CGPoint = CGPoint(x: 8.5, y: 7)
    static let radius: CGFloat = 5.2
    static let line: CGFloat = 1.4
    static let gap: CGFloat = 0.9
    static let frontTick: [CGPoint] = [CGPoint(x: 11.7, y: 7.2), CGPoint(x: 13.4, y: 8.9), CGPoint(x: 16.4, y: 5.6)]
    static let backTick: [CGPoint] = [CGPoint(x: 6.2, y: 7.2), CGPoint(x: 7.9, y: 8.9), CGPoint(x: 9.3, y: 7.4)]

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let scale: CGFloat = min(size.width / ChatReceiptMark.box.width, size.height / ChatReceiptMark.box.height)
            context.translateBy(
                x: (size.width - ChatReceiptMark.box.width * scale) / 2,
                y: (size.height - ChatReceiptMark.box.height * scale) / 2)
            context.scaleBy(x: scale, y: scale)
            ChatReceiptMark.draw(receipt, in: &context, muted: muted, accent: accent, background: background)
        }
        .frame(width: ChatReceiptMark.size.width, height: ChatReceiptMark.size.height)
        .opacity(receipt == .sending ? 0.7 : 1)
        .accessibilityElement()
        .accessibilityLabel(receipt.label)
        .help(receipt.label)
    }

    static func circle(_ center: CGPoint, _ radius: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2))
    }

    static func polyline(_ points: [CGPoint]) -> Path {
        var path: Path = Path()
        path.addLines(points)
        return path
    }

    static func outside(_ center: CGPoint, _ radius: CGFloat) -> Path {
        var path: Path = Path(CGRect(x: -box.width, y: -box.height, width: box.width * 3, height: box.height * 3))
        path.addPath(circle(center, radius))
        return path
    }

    static func draw(
        _ receipt: ChatReceipt, in context: inout GraphicsContext, muted: Color, accent: Color, background: Color
    ) {
        let stroke: StrokeStyle = StrokeStyle(lineWidth: line, lineCap: .round, lineJoin: .round)
        let outer: CGFloat = radius + line / 2
        switch receipt {
        case .sending:
            let dotted: StrokeStyle = StrokeStyle(lineWidth: line, lineCap: .round, lineJoin: .round, dash: [0.01, 2.6])
            context.stroke(circle(front, radius), with: .color(muted), style: dotted)
        case .sent:
            context.stroke(circle(front, radius), with: .color(muted), style: stroke)
            context.stroke(polyline(frontTick), with: .color(muted), style: stroke)
        case .delivered:
            var behind: GraphicsContext = context
            behind.clip(to: outside(front, outer + line / 2), style: FillStyle(eoFill: true))
            behind.stroke(circle(back, radius), with: .color(muted), style: stroke)
            behind.stroke(polyline(backTick), with: .color(muted), style: stroke)
            context.stroke(circle(front, radius), with: .color(muted), style: stroke)
            context.stroke(polyline(frontTick), with: .color(muted), style: stroke)
        case .read:
            var behind: GraphicsContext = context
            behind.clip(to: outside(front, outer + gap), style: FillStyle(eoFill: true))
            behind.fill(circle(back, outer), with: .color(accent))
            behind.stroke(polyline(backTick), with: .color(background), style: stroke)
            context.fill(circle(front, outer), with: .color(accent))
            context.stroke(polyline(frontTick), with: .color(background), style: stroke)
        }
    }
}
