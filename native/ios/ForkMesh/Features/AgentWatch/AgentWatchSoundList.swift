// The list of sounds, painted as the desktop paints it (AgentActivityView's
// paintSoundList and paintKindSwitch): every kind, the most frequent on top,
// its figure, its name, how many have landed and the latest one word for
// word, lit the moment a note of its kind sounds. Each row carries the two
// switches the desktop's does: its figure (the eye) and its sound (the
// speaker).

import FMAgentWatch
import SwiftUI

/// A row stays lit after its note with this time constant (kRowGlowSeconds).
private let rowGlowSeconds = 0.8

/// Two lines a row: its name and tally, then the latest of its kind.
let watchSoundRowHeight: CGFloat = 42

let watchSoundColumnStacks: CGFloat = 200

/// The kinds the list shows, the most frequent on top (`listedMotions`).
/// Stable: kinds with the same tally keep the table's order.
@MainActor
func listedMotions(_ controller: AgentWatchController) -> [AgentMotion] {
    AgentMotion.allCases.sorted { a, b in
        let countA = controller.landCount[a] ?? 0
        let countB = controller.landCount[b] ?? 0
        return countA != countB ? countA > countB : a.rawValue < b.rawValue
    }
}

/// 0..1: how lit a row is. A note you hear lights it; a mark nobody hears
/// only warms it (`soundGlow`).
@MainActor
func soundGlow(_ controller: AgentWatchController, _ motion: AgentMotion, _ now: Double) -> Double {
    func glow(_ at: Double?) -> Double {
        guard let at else { return 0 }
        let since = now - at
        return since < 0 ? 0.0 : exp(-since / rowGlowSeconds)
    }
    return max(glow(controller.lastSounded[motion]), 0.18 * glow(controller.lastLanded[motion]))
}

/// The list itself. `now` is the frame's reading of the watcher's clock; the
/// screen redraws it with the field.
struct WatchSoundList: View {
    let controller: AgentWatchController
    let now: Double
    let hidden: Set<AgentMotion>
    /// A switch was tapped: the figure's, or (sound) the note's.
    let onToggle: (AgentMotion, Bool) -> Void
    var title = ""
    var summary: [String] = []

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                if !title.isEmpty || !summary.isEmpty {
                    VStack(alignment: .leading, spacing: 3) {
                        if !title.isEmpty {
                            Text(title)
                                .font(.system(size: 13, weight: .bold))
                                .foregroundColor(watchForeground.color)
                        }
                        ForEach(summary.indices, id: \.self) { (index: Int) in
                            Text(summary[index])
                                .font(.system(size: 11, weight: .regular).monospacedDigit())
                                .foregroundColor(watchMuted.color)
                                .lineLimit(1)
                                .truncationMode(.tail)
                        }
                    }
                    .padding(.horizontal, 8)
                    .padding(.top, 6)
                    .padding(.bottom, 8)
                    .accessibilityElement(children: .combine)
                    .accessibilityIdentifier("agent-watch-summary")
                }
                ForEach(listedMotions(controller), id: \.self) { (motion: AgentMotion) in
                    WatchSoundRow(
                        controller: controller,
                        motion: motion,
                        now: now,
                        shown: !hidden.contains(motion),
                        heard: !controller.muted.contains(motion),
                        onToggle: onToggle)
                }
            }
            .padding(6)
        }
        .scrollIndicators(.hidden)
        .watchEdgesClear()
        .accessibilityIdentifier("agent-watch-sounds")
    }
}

private struct WatchSoundRow: View {
    let controller: AgentWatchController
    let motion: AgentMotion
    let now: Double
    let shown: Bool
    let heard: Bool
    let onToggle: (AgentMotion, Bool) -> Void

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let size = CGSize(width: proxy.size.width, height: watchSoundRowHeight)
            Canvas { (ctx: inout GraphicsContext, _: CGSize) in
                paintRow(ctx, size)
            }
            .contentShape(Rectangle())
            .onTapGesture(coordinateSpace: .local) { (location: CGPoint) in
                let row = Self.rowRect(size)
                let x = Double(location.x)
                // The switches sit at the right; a thumb gets the whole corner.
                let eye = Self.switchRect(row, false)
                let speaker = Self.switchRect(row, true)
                if Self.stacks(row) {
                    if x >= Double(eye.minX) - 10 {
                        onToggle(motion, Double(location.y) >= Double(row.midY))
                    }
                } else if x >= Double(speaker.minX) - 6 {
                    onToggle(motion, true)
                } else if x >= Double(eye.minX) - 12 {
                    onToggle(motion, false)
                }
            }
        }
        .frame(height: watchSoundRowHeight)
        .accessibilityElement()
        .accessibilityLabel(Text(
            "\(motion.label): \(motion.description). \u{266A} \(motion.sound). \(controller.landCount[motion] ?? 0) so far"))
        .accessibilityIdentifier("agent-watch-sound-\(motion.name)")
        .accessibilityAction(named: Text(heard ? "Mute its sound" : "Hear its sound")) { onToggle(motion, true) }
        .accessibilityAction(named: Text(shown ? "Hide its figure" : "Show its figure")) { onToggle(motion, false) }
    }

    static func rowRect(_ size: CGSize) -> CGRect {
        CGRect(x: 0, y: 0, width: size.width, height: size.height - 3)
    }

    /// The two switches at the right of the row's first line.
    static func switchRect(_ row: CGRect, _ sound: Bool) -> CGRect {
        let side = min(16.0, Double(row.height) - 4)
        if stacks(row) {
            let gap = max(2.0, (Double(row.height) - 2 * side) / 3)
            let top = Double(row.minY) + gap + (sound ? side + gap : 0)
            return CGRect(x: Double(row.maxX) - 4 - side, y: top, width: side, height: side)
        }
        let top = Double(row.minY) + min((Double(row.height) - side) / 2, 4.0)
        let right = Double(row.maxX) - 4 - (sound ? 0 : side + 6)
        return CGRect(x: right - side, y: top, width: side, height: side)
    }

    static func stacks(_ row: CGRect) -> Bool {
        row.width < watchSoundColumnStacks
    }

    private func paintRow(_ ctx: GraphicsContext, _ size: CGSize) {
        let row = Self.rowRect(size)
        let glow = soundGlow(controller, motion, now)
        let hue = motion.hue
        if glow > 0.01 {
            ctx.fill(
                Path(roundedRect: row, cornerRadius: 6),
                with: WatchShader.linear(
                    from: CGPoint(Double(row.minX), Double(row.midY)), to: CGPoint(Double(row.maxX), Double(row.midY)),
                    colours: [watchInk(hue, 0.9, 0.55, 0.55 * glow), watchInk(hue, 0.9, 0.55, 0.08 * glow)],
                    stops: nil
                ).shading)
        }
        ctx.fill(
            Path(
                roundedRect: CGRect(x: row.minX, y: row.minY + 3, width: 3, height: row.height - 6),
                cornerRadius: 1.5),
            with: .color(watchInk(hue, 0.95, 0.66, 0.35 + 0.65 * glow).color))

        let iconSize = min(9.0, Double(row.height) * 0.32) * (1.0 + 0.35 * glow)
        let stacked = Self.stacks(row)
        let iconAt = CGPoint(Double(row.minX) + (stacked ? 15 : 20), Double(row.minY) + min(Double(row.height) / 2, 13.0))
        if glow > 0.05 {
            // The field's own figure for it, small, playing while it is lit.
            let lastAt = max(controller.lastSounded[motion] ?? -1e9, controller.lastLanded[motion] ?? -1e9)
            let since = now - lastAt
            let t = (since / max(0.4, motion.seconds)).truncatingRemainder(dividingBy: 1.0)
            var clipped = ctx
            clipped.clip(to: Path(row))
            paintWatchFigure(clipped, motion, iconAt, 5.0, min(max(t, 0.0), 1.0), 0.1, glow)
        }
        paintWatchIcon(ctx, motion, iconAt, iconSize, (0.55 + 0.45 * glow) * (shown ? 1.0 : 0.35))
        paintSwitch(ctx, Self.switchRect(row, false), false, shown, hue)
        paintSwitch(ctx, Self.switchRect(row, true), true, heard, hue)

        let left = Double(row.minX) + (stacked ? 28 : 38)
        let textWidth = max(20.0, Double(Self.switchRect(row, false).minX) - left - 6)
        let opacity = shown || heard ? 1.0 : 0.45
        let nameSize = min(max(Double(row.height) - 3, 8.0), 12.0)
        let smallSize = min(max(Double(row.height) - 5, 7.0), 10.0)
        let nameInk = glow > 0.05
            ? watchInk(hue, 0.6, 0.92, (0.7 + 0.3 * glow) * opacity)
            : watchMuted.withAlpha(opacity)
        let name = watchLine(ctx, motion.label, WatchTextStyle(size: nameSize, bold: true, colour: nameInk), maxWidth: textWidth)
        let top = 3.0
        name.draw(in: ctx, at: CGPoint(left, Double(row.minY) + top))
        let lineHeight = name.height

        let count = controller.landCount[motion] ?? 0
        if count > 0 {
            let tally = watchLine(
                ctx, "\u{00D7}\(count)",
                WatchTextStyle(size: smallSize, colour: watchMuted.withAlpha((0.6 + 0.4 * glow) * opacity)),
                maxWidth: textWidth)
            tally.draw(
                in: ctx,
                at: CGPoint(left + textWidth - tally.width, Double(row.minY) + top + (lineHeight - tally.height) / 2))
        }
        let live = controller.lastText[motion] ?? ""
        let liveAge = now - (controller.lastTextSeconds[motion] ?? -1e9)
        let fresh = exp(-max(0.0, liveAge) / 2.5)
        let second: WatchLine
        if !live.isEmpty {
            second = watchLine(
                ctx, live, WatchTextStyle(size: smallSize, colour: watchInk(hue, 0.55, 0.9, (0.5 + 0.5 * fresh) * opacity)),
                maxWidth: textWidth)
        } else {
            second = watchLine(
                ctx, motion.description,
                WatchTextStyle(size: smallSize, colour: watchForeground.withAlpha((0.55 + 0.4 * glow) * opacity)),
                maxWidth: textWidth)
        }
        second.draw(in: ctx, at: CGPoint(left, Double(row.minY) + top + lineHeight))
    }

    /// A switch on the row: an eye for the figure, a speaker for the sound,
    /// struck through when off (`paintKindSwitch`).
    private func paintSwitch(_ ctx: GraphicsContext, _ box: CGRect, _ sound: Bool, _ on: Bool, _ hue: Double) {
        let colour = on ? watchInk(hue, 0.7, 0.82, 0.95) : watchMuted
        if on {
            ctx.fill(Path(roundedRect: box, cornerRadius: 3), with: .color(watchInk(hue, 0.8, 0.5, 0.22).color))
        }
        let cx = Double(box.midX)
        let cy = Double(box.midY)
        let r = Double(box.width) / 2 - 2.5
        let style = StrokeStyle(lineWidth: 1.2, lineCap: .round, lineJoin: .round)
        let ink = GraphicsContext.Shading.color(colour.color)
        if sound {
            var cone = Path()
            cone.move(to: CGPoint(cx - r, cy - r * 0.35))
            cone.addLine(to: CGPoint(cx - r * 0.4, cy - r * 0.35))
            cone.addLine(to: CGPoint(cx + r * 0.15, cy - r * 0.85))
            cone.addLine(to: CGPoint(cx + r * 0.15, cy + r * 0.85))
            cone.addLine(to: CGPoint(cx - r * 0.4, cy + r * 0.35))
            cone.addLine(to: CGPoint(cx - r, cy + r * 0.35))
            cone.closeSubpath()
            ctx.stroke(cone, with: ink, style: style)
            if on {
                // Qt's arc from -50 degrees spanning 100: the wave on the right.
                var wave = Path()
                QtPainter.arc(CGRect(x: cx - r * 0.2, y: cy - r * 0.7, width: r * 1.2, height: r * 1.4), -50 * 16, 100 * 16, into: &wave)
                ctx.stroke(wave, with: ink, style: style)
            }
        } else {
            var lids = Path()
            lids.move(to: CGPoint(cx - r, cy))
            lids.addQuadCurve(to: CGPoint(cx + r, cy), control: CGPoint(cx, cy - r * 1.1))
            lids.addQuadCurve(to: CGPoint(cx - r, cy), control: CGPoint(cx, cy + r * 1.1))
            ctx.stroke(lids, with: ink, style: style)
            let pupil = r * 0.3
            ctx.fill(Path(ellipseIn: CGRect(x: cx - pupil, y: cy - pupil, width: pupil * 2, height: pupil * 2)), with: ink)
        }
        if !on {
            var strike = Path()
            strike.move(to: CGPoint(cx - r, cy + r))
            strike.addLine(to: CGPoint(cx + r, cy - r))
            ctx.stroke(strike, with: ink, style: style)
        }
    }
}
