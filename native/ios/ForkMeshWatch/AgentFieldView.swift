// The agent on the Watch field, reduced for a watch face: a body that
// breathes in the colour of its latest beat, and a ring of marks for the
// recent beats, each in its kind's colour and sized by the data it moved. The
// newest mark flashes and throws a spoke to the body when it arrives.
//
// A reduced stand-in for WS4's AgentWatchFigures on the phone; it reads only
// what `AgentFrame` carries.

import FMAgentWatch
import FMWatchWire
import SwiftUI

struct AgentFieldView: View {
    let frame: AgentFrame
    let arrivedAt: Date?
    var showLabels = false

    /// Marks on the ring.
    private static let ringSize = 24

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !frame.status.isActive && !recentlyArrived)) { context in
            Canvas { (graphics: inout GraphicsContext, size: CGSize) in
                draw(&graphics, size: size, now: context.date)
            }
        }
        .accessibilityElement()
        .accessibilityLabel("\(frame.title), \(frame.status.label)")
        .accessibilityValue(frame.beats.last.map { "\($0.motion.label): \($0.text)" } ?? "")
    }

    private var recentlyArrived: Bool {
        guard let arrivedAt else { return false }
        return Date().timeIntervalSince(arrivedAt) < 2
    }

    private func draw(_ g: inout GraphicsContext, size: CGSize, now: Date) {
        let center = CGPoint(x: size.width / 2, y: size.height / 2)
        let radius = min(size.width, size.height) / 2
        let t = now.timeIntervalSinceReferenceDate
        let active = frame.status.isActive
        let since = arrivedAt.map { now.timeIntervalSince($0) } ?? 99
        let flash = max(0, 1 - since / 0.8)

        // The ring's guide.
        let ringRadius = radius * 0.78
        g.stroke(
            Path(ellipseIn: CGRect(x: center.x - ringRadius, y: center.y - ringRadius, width: ringRadius * 2, height: ringRadius * 2)),
            with: .color(.white.opacity(0.08)), lineWidth: 1)

        // The marks: newest at the top, older ones clockwise behind it.
        let beats = Array(frame.beats.suffix(AgentFieldView.ringSize))
        let count = beats.count
        var newestPoint: CGPoint?
        for (offset, beat) in beats.enumerated() {
            let age = count - 1 - offset
            let angle = -Double.pi / 2 - Double(age) * (2 * Double.pi / Double(AgentFieldView.ringSize))
            let point = CGPoint(x: center.x + ringRadius * cos(angle), y: center.y + ringRadius * sin(angle))
            let weight = min(1, log10(Double(max(beat.bytes, 10))) / 4)
            var markRadius = radius * (0.035 + 0.055 * weight)
            var opacity = 1 - Double(age) / Double(AgentFieldView.ringSize) * 0.8
            if age == 0 {
                markRadius *= 1 + flash * 0.8
                opacity = 1
                newestPoint = point
            }
            let color = Color(hue: beat.motion.hue, saturation: 0.7, brightness: 0.95).opacity(opacity)
            let rect = CGRect(x: point.x - markRadius, y: point.y - markRadius, width: markRadius * 2, height: markRadius * 2)
            if beat.motion == .fail || beat.motion == .fumble || beat.motion == .denied {
                g.stroke(Path(ellipseIn: rect), with: .color(color), lineWidth: 1.5)
            } else {
                g.fill(Path(ellipseIn: rect), with: .color(color))
            }
        }

        // The body: breathes while running, faster while the beats come fast.
        let hue = frame.hue
        let breath = active ? 0.06 * sin(t * (2.2 + 3 * flash)) : 0
        let bodyRadius = radius * (0.34 + breath + 0.05 * flash)
        let brightness = active ? 0.9 : 0.45
        let bodyColor = Color(hue: hue, saturation: 0.6, brightness: brightness)

        if let newestPoint, flash > 0 {
            var spoke = Path()
            spoke.move(to: center)
            spoke.addLine(to: newestPoint)
            g.stroke(spoke, with: .color(bodyColor.opacity(flash * 0.7)), lineWidth: 1.5)
        }

        // A soft glow, then the body itself.
        let glowRadius = bodyRadius * (1.35 + 0.2 * flash)
        g.fill(
            Path(ellipseIn: CGRect(x: center.x - glowRadius, y: center.y - glowRadius, width: glowRadius * 2, height: glowRadius * 2)),
            with: .radialGradient(
                Gradient(colors: [bodyColor.opacity(0.45), bodyColor.opacity(0)]),
                center: center, startRadius: bodyRadius * 0.6, endRadius: glowRadius))
        g.fill(
            Path(ellipseIn: CGRect(x: center.x - bodyRadius, y: center.y - bodyRadius, width: bodyRadius * 2, height: bodyRadius * 2)),
            with: .radialGradient(
                Gradient(colors: [Color(hue: hue, saturation: 0.35, brightness: 1).opacity(active ? 1 : 0.6), bodyColor]),
                center: CGPoint(x: center.x - bodyRadius * 0.3, y: center.y - bodyRadius * 0.3),
                startRadius: 0, endRadius: bodyRadius * 1.4))

        // Context used, as an arc just outside the body.
        if let fraction = frame.contextFraction, fraction > 0 {
            var arc = Path()
            arc.addArc(
                center: center, radius: bodyRadius + 4, startAngle: .degrees(-90),
                endAngle: .degrees(-90 + 360 * fraction), clockwise: false)
            g.stroke(arc, with: .color(.white.opacity(0.5)), style: StrokeStyle(lineWidth: 2, lineCap: .round))
        }

        // The face's name on the body.
        g.draw(
            Text(String(frame.face.prefix(showLabels ? 8 : 2)))
                .font(.system(size: showLabels ? 14 : 12, weight: .bold, design: .rounded))
                .foregroundColor(.black.opacity(0.75)),
            at: center)

        if showLabels, let beat = frame.beats.last {
            g.draw(
                Text(beat.motion.label)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundColor(Color(hue: beat.motion.hue, saturation: 0.5, brightness: 1)),
                at: CGPoint(x: center.x, y: center.y + bodyRadius + 14))
        }
    }
}
