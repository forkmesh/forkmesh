// The Watch field, painted as the desktop paints it (AgentActivityView's paintSky,
// paintRows, paintDesktopNode, paintPulses, paintBodies with
// paintHistoryRing and drawRingText, paintLed and paintHoverCard). The
// geometry (WatchFieldLayout) and the card's words (watchCardLines) are in
// FMAgentWatch; this file only paints. What the phone cannot see (the hosts
// along the top, packets, processes, the desktop's log) is left out.

import CoreText
import FMAgentWatch
import SwiftUI
import UIKit

/// What the field is drawn on (the desktop's `Backdrop`). The phone's field
/// is always dark, so Theme is the desktop's dark theme.
enum WatchBackdrop: String, CaseIterable {
    case theme = "Theme"
    case space = "Space"
    case aurora = "Aurora"
    case ocean = "Ocean"

    var label: String { rawValue }
}

let watchBackground = WatchRGBA(hex: 0xFF06_0910)
let watchForeground = WatchRGBA(hex: 0xFFEC_F2FA)
let watchMuted = WatchRGBA(hex: 0xFF8C_98AC)
private let watchGuide = WatchRGBA(hex: 0x10FF_FFFF)

/// How many of an agent's latest marks circle it (kRingMarks).
private let ringMarks = 40

// MARK: - Text

/// A line of text in the field's type: size, weight and colour.
struct WatchTextStyle {
    var size: Double
    var bold: Bool = false
    var colour: WatchRGBA = watchForeground
    var tracking: Double = 0

    func text(_ string: String) -> Text {
        Text(string)
            .font(.system(size: CGFloat(size), weight: bold ? .bold : .regular))
            .tracking(CGFloat(tracking))
            .foregroundColor(colour.color)
    }
}

private let unbounded = CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)

/// A resolved line and its size.
struct WatchLine {
    let resolved: GraphicsContext.ResolvedText
    let size: CGSize
    let baseline: Double

    var width: Double { Double(size.width) }
    var height: Double { Double(size.height) }

    /// Draws with its top-left at `at`.
    func draw(in ctx: GraphicsContext, at: CGPoint) {
        ctx.draw(resolved, in: CGRect(origin: at, size: size))
    }
}

/// One line of `string`, cut with an ellipsis to fit `maxWidth` when given.
func watchLine(_ ctx: GraphicsContext, _ string: String, _ style: WatchTextStyle, maxWidth: Double? = nil) -> WatchLine {
    func make(_ s: String) -> WatchLine {
        let resolved = ctx.resolve(style.text(s))
        let size = resolved.measure(in: unbounded)
        return WatchLine(resolved: resolved, size: size, baseline: Double(resolved.firstBaseline(in: size)))
    }
    let full = make(string)
    guard let maxWidth, full.width > maxWidth, !string.isEmpty else { return full }
    let characters = Array(string)
    var low = 0
    var high = characters.count
    var best = make("\u{2026}")
    while low <= high {
        let mid = (low + high) / 2
        let candidate = make(String(characters[0..<mid]) + "\u{2026}")
        if candidate.width <= maxWidth {
            best = candidate
            low = mid + 1
        } else {
            high = mid - 1
        }
    }
    return best
}

// MARK: - The sky

/// The backdrop, drawn by its own Canvas, which SwiftUI redraws only when the
/// size, the backdrop or the phone's tilt changes.
enum WatchSky {
    /// (x, y, size, brightness, phase) for each star.
    static let stars: [(Double, Double, Double, Double, Double)] = (0..<460).map { i in
        let r = watchScatter(i * 7 + 3)
        // Most stars are faint pinpricks; a few are bright enough to glow.
        return (
            watchScatter(i * 7 + 1),
            watchScatter(i * 7 + 2),
            0.5 + 1.9 * r * r * r,
            0.25 + 0.75 * watchScatter(i * 7 + 4),
            6.2832 * watchScatter(i * 7 + 6)
        )
    }

    static func paint(_ ctx: GraphicsContext, _ size: CGSize, _ backdrop: WatchBackdrop, look: CGPoint = .zero) {
        let w = Double(size.width)
        let h = Double(size.height)
        let rect = CGRect(origin: .zero, size: size)
        let reach = 0.08 * max(w, h)
        let far = CGPoint(-Double(look.x) * reach * 0.35, Double(look.y) * reach * 0.35)
        func vertical(_ colours: [WatchRGBA], _ stops: [Double]) -> GraphicsContext.Shading {
            WatchShader.linear(from: .zero, to: CGPoint(0, h), colours: colours, stops: stops).shading
        }
        switch backdrop {
        case .theme:
            ctx.fill(Path(rect), with: vertical([WatchRGBA(hex: 0xFF0B_101C), watchBackground], [0, 1]))
        case .space:
            ctx.fill(
                Path(rect),
                with: vertical(
                    [WatchRGBA(hex: 0xFF02_030C), WatchRGBA(hex: 0xFF07_091C), WatchRGBA(hex: 0xFF0D_0920)],
                    [0, 0.6, 1]))
            // Nebulae: three clouds of colour.
            let clouds: [(Double, Double, Double, Double)] = [
                (0.22, 0.32, 0.46, 0.78), (0.76, 0.64, 0.52, 0.53), (0.56, 0.12, 0.30, 0.93),
            ]
            for (x, y, r, hue) in clouds {
                let centre = CGPoint(x * w + Double(far.x), y * h + Double(far.y))
                let radius = r * max(w, h)
                ctx.fill(
                    Path(ellipseIn: CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2)),
                    with: WatchShader.radial(
                        centre: centre, radius: radius,
                        colours: [watchInk(hue, 0.65, 0.45, 0.17), watchInk(hue, 0.6, 0.35, 0.06), watchInk(hue, 0.6, 0.3, 0.0)],
                        stops: [0, 0.5, 1]
                    ).shading)
            }
            paintStars(ctx, size, 1.0, 1.0, look, reach)
        case .aurora:
            ctx.fill(
                Path(rect),
                with: vertical(
                    [WatchRGBA(hex: 0xFF02_0710), WatchRGBA(hex: 0xFF05_121E), WatchRGBA(hex: 0xFF07_0C18)],
                    [0, 0.55, 1]))
            paintStars(ctx, size, 0.5, 0.7, look, reach)
            let hues: [Double] = [0.38, 0.46, 0.78]
            for k in 0..<3 {
                let base = h * (0.16 + 0.11 * Double(k)) + Double(far.y)
                let band = h * (0.20 + 0.04 * Double(k))
                var top: [CGPoint] = []
                var x = -20.0
                while x <= w + 20 {
                    let u = (x - Double(far.x)) / max(1.0, w)
                    let wave1: Double = h * 0.05 * sin(u * 8.2 + Double(k))
                    let wave2: Double = h * 0.02 * sin(u * 19.5 + Double(k) * 2.0)
                    top.append(CGPoint(x, base + wave1 + wave2))
                    x += 14
                }
                var curtain = Path()
                curtain.move(to: top[0])
                for point in top { curtain.addLine(to: point) }
                for point in top.reversed() { curtain.addLine(to: CGPoint(Double(point.x), Double(point.y) + band)) }
                curtain.closeSubpath()
                ctx.fill(
                    curtain,
                    with: WatchShader.linear(
                        from: CGPoint(0, base - h * 0.07), to: CGPoint(0, base + band + h * 0.07),
                        colours: [
                            watchInk(hues[k], 0.9, 0.55, 0.0), watchInk(hues[k], 0.9, 0.55, 0.20),
                            watchInk(hues[k], 0.9, 0.45, 0.0),
                        ],
                        stops: [0, 0.3, 1]
                    ).shading)
            }
        case .ocean:
            ctx.fill(
                Path(rect),
                with: vertical(
                    [WatchRGBA(hex: 0xFF06_203A), WatchRGBA(hex: 0xFF03_1226), WatchRGBA(hex: 0xFF01_0814)],
                    [0, 0.5, 1]))
            // Shafts of light from the surface.
            for i in 0..<5 {
                let x: Double = w * (0.1 + 0.2 * Double(i)) + 40 * sin(Double(i) * 1.3) + Double(far.x)
                let spread: Double = w * 0.05 + 20 * sin(Double(i))
                var shaft = Path()
                shaft.move(to: CGPoint(x - spread * 0.4, 0))
                shaft.addLine(to: CGPoint(x + spread * 0.4, 0))
                shaft.addLine(to: CGPoint(x + spread * 2.2 + 60, h))
                shaft.addLine(to: CGPoint(x - spread * 0.6 + 60, h))
                shaft.closeSubpath()
                ctx.fill(
                    shaft,
                    with: WatchShader.linear(
                        from: .zero, to: CGPoint(0, h),
                        colours: [WatchRGBA(hex: 0x1AA0_DCFF), WatchRGBA(hex: 0x00A0_DCFF)], stops: nil
                    ).shading)
            }
            // Motes in the water.
            var i = 0
            while i < stars.count {
                let (x, y, starSize, brightness, _) = stars[i]
                let alpha = 0.10 + 0.25 * brightness * (1.0 - y * 0.6)
                let r = starSize * 0.8
                let at = drift(x, y, starSize, size, look, reach)
                ctx.fill(
                    Path(ellipseIn: CGRect(x: Double(at.x) - r, y: Double(at.y) - r, width: r * 2, height: r * 2)),
                    with: .color(WatchRGBA(rgbo: 179, 230, 255, alpha).color))
                i += 3
            }
        }
    }

    static func drift(_ x: Double, _ y: Double, _ starSize: Double, _ size: CGSize, _ look: CGPoint, _ reach: Double) -> CGPoint {
        let w = max(1.0, Double(size.width))
        let h = max(1.0, Double(size.height))
        let depth = 0.45 + 0.55 * min(max((starSize - 0.5) / 1.9, 0), 1)
        func wrap(_ v: Double, _ span: Double) -> Double {
            let m = v.truncatingRemainder(dividingBy: span)
            return m < 0 ? m + span : m
        }
        return CGPoint(
            wrap(x * w - Double(look.x) * reach * depth, w),
            wrap(y * h + Double(look.y) * reach * depth, h))
    }

    private static func paintStars(
        _ ctx: GraphicsContext, _ size: CGSize, _ density: Double, _ brightness: Double, _ look: CGPoint, _ reach: Double
    ) {
        let count = Int(Double(stars.count) * density)
        for i in 0..<count {
            let (x, y, starSize, starBrightness, phase) = stars[i]
            let at = drift(x, y, starSize, size, look, reach)
            let shine = brightness * starBrightness * (0.62 + 0.38 * sin(phase))
            if starSize > 1.6 {
                drawWatchGlow(ctx, at, starSize * 4, WatchRGBA(rgbo: 204, 222, 255, min(max(0.35 * shine, 0), 1)))
            }
            let r = starSize * 0.6
            ctx.fill(
                Path(ellipseIn: CGRect(x: Double(at.x) - r, y: Double(at.y) - r, width: r * 2, height: r * 2)),
                with: .color(WatchRGBA(rgbo: 235, 242, 255, min(max(shine, 0), 1)).color))
        }
    }
}

// MARK: - The dot displays

/// One display line's lit dots.
struct WatchLedStrip {
    /// Column, row pairs of every lit dot.
    let dots: [Int32]
    let width: Double
}

/// The dots of each agent's display line: its words drawn once in a small
/// fixed font, and every pixel lit past the desktop's threshold kept as a dot
/// (`ledFor`). Rasterised with Core Text into a ten-row bitmap; a line is a
/// few hundred columns, so it is built on first use.
@MainActor
final class WatchLeds {
    private var strips: [String: WatchLedStrip] = [:]
    private var order: [String] = []

    func stripFor(_ text: String) -> WatchLedStrip? {
        if text.isEmpty { return nil }
        if let strip = strips[text] { return strip }
        guard let strip = Self.build(text) else { return nil }
        strips[text] = strip
        order.append(text)
        if order.count > 64 { strips[order.removeFirst()] = nil }
        return strip
    }

    private static func build(_ text: String) -> WatchLedStrip? {
        let font = UIFont(name: "Menlo-Regular", size: 8) ?? UIFont.monospacedSystemFont(ofSize: 8, weight: .regular)
        let attributed = NSAttributedString(
            string: text, attributes: [.font: font, .foregroundColor: UIColor.white])
        let line = CTLineCreateWithAttributedString(attributed)
        let width = CTLineGetTypographicBounds(line, nil, nil, nil)
        let columns = max(1, Int(width.rounded(.up)) + 1)
        let rows = kWatchLedRows
        var pixels = [UInt8](repeating: 0, count: columns * rows * 4)
        let drawn: Bool = pixels.withUnsafeMutableBytes { buffer -> Bool in
            guard let context = CGContext(
                data: buffer.baseAddress, width: columns, height: rows, bitsPerComponent: 8,
                bytesPerRow: columns * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            // Stood on the display's baseline, two rows up from the bottom,
            // as the desktop draws at its ascent in a ten-pixel line.
            context.textPosition = CGPoint(x: 0, y: 2)
            CTLineDraw(line, context)
            return true
        }
        if !drawn { return nil }
        var dots: [Int32] = []
        // The bitmap's first row in memory is its top row.
        for row in 0..<rows {
            for column in 0..<columns where pixels[(row * columns + column) * 4 + 3] > 110 {
                dots.append(Int32(column))
                dots.append(Int32(row))
            }
        }
        return WatchLedStrip(dots: dots, width: Double(columns) * kWatchLedPitch)
    }
}

// MARK: - The field

/// Everything the field paints for one frame (`WatchFieldPainter`).
@MainActor
struct WatchFieldPainter {
    let controller: AgentWatchController
    let layout: WatchFieldLayout
    let leds: WatchLeds
    let faces: [ObjectIdentifier: Image]
    let hidden: Set<AgentMotion>
    let selected: WatchedAgent?
    var nodeLabel: String = "this desktop"
    var traffic: String = ""
    var showRings: Bool = true
    let now: Double

    func paint(_ ctx: GraphicsContext, _ size: CGSize) {
        paintRows(ctx)
        paintDesktopNode(ctx)
        paintPulses(ctx)
        paintBodies(ctx)
        paintLeds(ctx)
        paintTags(ctx)
        paintCard(ctx, size)
    }

    /// `saveLayer` at the agent's presence while it fades in or out.
    private func layered(_ ctx: GraphicsContext, _ shown: Double, _ draw: (GraphicsContext) -> Void) {
        if shown < 0.99 {
            var faded = ctx
            faded.opacity = shown
            faded.drawLayer { layer in draw(layer) }
        } else {
            draw(ctx)
        }
    }

    private func paintRows(_ ctx: GraphicsContext) {
        let field = layout.field
        for (i, row) in layout.rows.enumerated() where row.count > 0 {
            let rowTop = Double(field.minY) + (row.y - row.height / 2) * Double(field.height)
            let top = rowTop - layout.bandHeader * Double(field.height)
            if i > 0 {
                dashedLine(ctx, CGPoint(Double(field.minX), top), CGPoint(Double(field.maxX), top), watchGuide, 1, 4, 2)
            }
            let text = watchLine(
                ctx, "\(bandLabel(row.band)) \u{00B7} \(row.count)".uppercased(),
                WatchTextStyle(size: 10, bold: true, colour: watchMuted, tracking: 0.8))
            let descent = text.height - text.baseline
            text.draw(in: ctx, at: CGPoint(Double(field.minX) + 2, rowTop - descent - 1 - text.height))
        }
    }

    private func paintDesktopNode(_ ctx: GraphicsContext) {
        let at = layout.desktopNode
        let x = Double(at.x)
        let y = Double(at.y)
        let s = 9.0
        let colour = watchInk(0.58, 0.55, 0.78, 0.95)
        let style = StrokeStyle(lineWidth: max(1.0, s * 0.2), lineCap: .round, lineJoin: .round)
        let screen = Path(
            roundedRect: CGRect(x: x - s, y: y - s * 0.8, width: s * 2, height: s * 1.25), cornerRadius: s * 0.18)
        ctx.fill(screen, with: .color(colour.withAlpha(colour.a * 0.3).color))
        ctx.stroke(screen, with: .color(colour.color), style: style)
        var stand = Path()
        stand.move(to: CGPoint(x, y + s * 0.45))
        stand.addLine(to: CGPoint(x, y + s * 0.8))
        stand.move(to: CGPoint(x - s * 0.5, y + s * 0.8))
        stand.addLine(to: CGPoint(x + s * 0.5, y + s * 0.8))
        ctx.stroke(stand, with: .color(colour.color), style: style)
        let label = watchLine(ctx, nodeLabel, WatchTextStyle(size: 10, colour: watchMuted))
        label.draw(in: ctx, at: CGPoint(x - label.width / 2, y + 12))
        if !traffic.isEmpty {
            let fresh = exp(-max(0.0, now - controller.lastReadSeconds) / 1.2)
            let line = watchLine(
                ctx, traffic,
                WatchTextStyle(size: 9, colour: watchInk(0.58, 0.55, 0.62 + 0.25 * fresh, 0.6 + 0.4 * fresh)),
                maxWidth: Double(layout.field.width))
            line.draw(in: ctx, at: CGPoint(x - line.width / 2, y + 13 + label.height))
            if fresh > 0.02 {
                drawWatchGlow(ctx, CGPoint(x, y), s * (1.6 + fresh), watchInk(0.58, 0.8, 0.7, 0.45 * fresh))
            }
        }
    }

    private func paintTags(_ ctx: GraphicsContext) {
        for agent in controller.agents where agent.presence > 0.01 {
            guard let motion = agent.tag, !hidden.contains(motion) else { continue }
            let age = now - agent.tagSeconds
            if age < 0 || age >= kTagSeconds { continue }
            let t = age / kTagSeconds
            let alpha = min(1.0, age / 0.12) * (t < 0.55 ? 1.0 : 1.0 - (t - 0.55) / 0.45) * agent.presence
            if alpha <= 0.01 { continue }
            let centre = layout.pointFor(agent)
            let radius = layout.radiusFor(agent, now)
            let hue = motion.hue
            let word = watchLine(ctx, motion.label, WatchTextStyle(size: 11, bold: true, colour: watchInk(hue, 0.7, 0.86)))
            let icon = 5.5
            let height = max(word.height, icon * 2) + 4
            let width = icon * 2 + 5 + word.width + 12
            let rise = 10.0 * watchEaseOut(t)
            let top = Double(centre.y) - radius - kWatchLabelGap - kWatchLabelPixels - height + 2 - rise
            let box = CGRect(x: Double(centre.x) - width / 2, y: top, width: width, height: height)
            var faded = ctx
            faded.opacity = alpha
            let pill = Path(roundedRect: box, cornerRadius: height / 2)
            faded.fill(pill, with: .color(WatchRGBA(argb: 210, 8, 11, 20).color))
            faded.stroke(pill, with: .color(watchInk(hue, 0.9, 0.62, 0.85).color), lineWidth: 1)
            paintWatchIcon(faded, motion, CGPoint(Double(box.minX) + 6 + icon, Double(box.midY)), icon, 1.0)
            word.draw(in: faded, at: CGPoint(Double(box.minX) + 6 + icon * 2 + 5, Double(box.midY) - word.height / 2))
        }
    }

    private func paintPulses(_ ctx: GraphicsContext) {
        for pulse in controller.pulses {
            if pulse.pending(now) || hidden.contains(pulse.motion) { continue }
            let agent = pulse.agent
            if agent.presence <= 0.01 { continue }
            layered(ctx, agent.presence) { layer in
                paintWatchFigure(
                    layer, pulse.motion, layout.pointFor(agent), layout.radiusFor(agent, now), pulse.life(now),
                    pulse.angle, pulse.strength)
            }
        }
    }

    private func paintBodies(_ ctx: GraphicsContext) {
        for agent in controller.agents where agent.presence > 0.01 {
            layered(ctx, agent.presence) { layer in paintBody(layer, agent) }
        }
    }

    private func circle(_ centre: CGPoint, _ radius: Double) -> Path {
        Path(ellipseIn: CGRect(
            x: Double(centre.x) - radius, y: Double(centre.y) - radius, width: radius * 2, height: radius * 2))
    }

    private func paintBody(_ ctx: GraphicsContext, _ agent: WatchedAgent) {
        let centre = layout.pointFor(agent)
        let radius = layout.radiusFor(agent, now)
        let hovered = agent === selected
        let energy = agent.energyAt(now)
        let hue = agent.hue

        // The glow: how busy it is right now, in its own colour.
        let glowAlpha: Double = 0.10 + 0.50 * energy + (hovered ? 0.12 : 0.0)
        drawWatchGlow(ctx, centre, radius * (2.0 + 1.2 * energy), watchInk(hue, 0.95, 0.60, glowAlpha))

        // The body itself, lit from one side like a planet; dim once it ended.
        let ended = agent.state == .done || agent.state == .failed
        let light = CGPoint(Double(centre.x) - radius * 0.35, Double(centre.y) - radius * 0.4)
        let inner = watchInk(hue, ended ? 0.35 : 0.75, ended ? 0.45 : 0.70 + 0.15 * energy, ended ? 0.8 : 1.0)
        let outer = watchInk(hue, ended ? 0.3 : 0.7, ended ? 0.18 : 0.30, ended ? 0.8 : 1.0)
        ctx.fill(
            circle(centre, radius),
            with: WatchShader.radial(centre: light, radius: radius * 1.4, colours: [inner, outer], stops: nil).shading)
        let outlineColour = watchInk(hue, 0.95, 0.76, hovered ? 1.0 : 0.7).color
        let outlineWidth: CGFloat = hovered ? 2.2 : 1.2
        ctx.stroke(circle(centre, radius), with: .color(outlineColour), lineWidth: outlineWidth)

        // Its face, the one the agent list shows, inside its own outline.
        if let face = faces[ObjectIdentifier(agent)] {
            let innerRadius = radius - (hovered ? 1.1 : 0.6)
            var clipped = ctx
            clipped.clip(to: circle(centre, innerRadius))
            clipped.opacity = ended ? 0.55 : 1.0
            clipped.draw(
                face,
                in: CGRect(
                    x: Double(centre.x) - innerRadius, y: Double(centre.y) - innerRadius,
                    width: innerRadius * 2, height: innerRadius * 2))
            ctx.stroke(circle(centre, radius), with: .color(outlineColour), lineWidth: outlineWidth)
        }

        if showRings { paintHistoryRing(ctx, agent, centre, radius) }

        // Being heard: a dotted ring that beats with its notes.
        if agent.listening && controller.sound {
            let beat = exp(-(now - agent.lastNoteSeconds) / 0.35)
            let width = 1.2 + 1.4 * beat
            dottedCircle(
                ctx, centre, radius + 4 + 3 * beat, watchInk(hue, 1.0, 0.82, 0.45 + 0.5 * beat), width, .butt)
        }

        // Still running (solid) or waiting on you (dotted): a still ring.
        let running = agent.state == .running || agent.state == .starting
        if running || agent.state == .waiting {
            let colour = watchInk(hue, 1.0, 0.85, 0.55)
            if running {
                ctx.stroke(
                    circle(centre, radius + 15), with: .color(colour.color),
                    style: StrokeStyle(lineWidth: 2, lineCap: .round))
            } else {
                dottedCircle(ctx, centre, radius + 15, colour, 2.0, .round)
            }
        }
        if agent.state == .failed {
            ctx.stroke(
                circle(centre, radius + 3), with: .color(WatchRGBA(argb: 220, 248, 81, 73).color), lineWidth: 2)
        }

        // Its number on a tag sitting on top of the body, in its own colour.
        if agent.id > 0 {
            let tag = watchLine(
                ctx, "\(agent.id)",
                WatchTextStyle(size: 10, bold: true, colour: hovered ? watchForeground : watchInk(hue, 0.9, 0.82)))
            let tagHeight = tag.height + 2
            let tagWidth = max(tagHeight, tag.width + 8)
            let box = CGRect(
                x: Double(centre.x) - tagWidth / 2, y: Double(centre.y) - radius - tagHeight * 0.6,
                width: tagWidth, height: tagHeight)
            let rounded = Path(roundedRect: box, cornerRadius: tagHeight / 2)
            ctx.fill(rounded, with: .color(WatchRGBA(argb: 225, 8, 11, 20).color))
            ctx.stroke(rounded, with: .color(watchInk(hue, 0.95, 0.76, hovered ? 1.0 : 0.8).color), lineWidth: 1)
            tag.draw(in: ctx, at: CGPoint(Double(box.midX) - tag.width / 2, Double(box.midY) - tag.height / 2))
        }

        // Its names circling it: the project over the top in its own colour,
        // its task under the bottom.
        let project = agent.target.projectName
        let task = watchTitleWithoutNumber(agent.id, agent.target.title)
        let ring = radius + kWatchLabelGap
        let gap = 0.45
        var projectSweep = 0.0
        let words: WatchRGBA = hovered ? watchForeground : ended ? watchMuted : watchInk(hue, 0.5, 0.80)
        if !project.isEmpty {
            projectSweep = ringText(
                ctx, centre, ring, true, project, hovered || ended ? words : watchInk(hue, 0.9, 0.78),
                bold: true, sweep: Double.pi)
        }
        if !task.isEmpty {
            _ = ringText(ctx, centre, ring, false, task, words, bold: false, sweep: 2 * Double.pi - projectSweep - 2 * gap)
        }
    }

    private func paintHistoryRing(_ ctx: GraphicsContext, _ agent: WatchedAgent, _ centre: CGPoint, _ radius: Double) {
        // Its own log round it like a clock: newest at the top, older following
        // round and fading.
        let history = controller.history
        let ring = radius + 10.0
        let icon = min(max(2.2 + radius * 0.06, 2.6), 4.0)
        let marks = min(max(Int((2.0 * Double.pi * ring / (icon * 3.0)).rounded(.down)), 6), ringMarks)
        var drawn = 0
        var i = history.count - 1
        while i >= 0 && drawn < marks {
            let mark = history[i]
            i -= 1
            if mark.agent !== agent || hidden.contains(mark.motion) { continue }
            let at = -Double.pi / 2 + Double(drawn) * (2.0 * Double.pi / Double(marks))
            let age = min(max((now - mark.seconds) / kHistorySeconds, 0.0), 1.0)
            let alpha = (1.0 - age) * (1.0 - Double(drawn) / Double(marks)) * 0.9
            let size = mark.motion.isFaint ? icon * 0.8 : icon * (0.85 + 0.3 * mark.strength)
            let point = centre + CGPoint(cos(at), sin(at)) * ring
            if alpha > 0.01 { paintWatchIcon(ctx, mark.motion, point, size, alpha) }
            drawn += 1
        }
    }

    private func paintLeds(_ ctx: GraphicsContext) {
        for agent in controller.agents where agent.presence > 0.01 && !agent.said.isEmpty {
            guard let strip = leds.stripFor(agent.said) else { continue }
            layered(ctx, agent.presence) { layer in paintLed(layer, agent, strip) }
        }
    }

    private func paintLed(_ ctx: GraphicsContext, _ agent: WatchedAgent, _ strip: WatchLedStrip) {
        let centre = layout.pointFor(agent)
        let radius = layout.radiusFor(agent, now)
        let box = layout.ledRect(agent, centre, radius)
        let rounded = Path(roundedRect: box, cornerRadius: 3)
        ctx.fill(rounded, with: .color(WatchRGBA(argb: 235, 14, 10, 6).color))
        ctx.stroke(rounded, with: .color(watchInk(agent.hue, 0.6, 0.5, 0.7).color), lineWidth: 1)
        let window = box.insetBy(dx: kWatchLedPad, dy: kWatchLedPad)
        var clipped = ctx
        clipped.clip(to: Path(window))
        let dot = kWatchLedPitch * 0.36
        let left = Double(window.minX)
        let top = Double(window.minY)
        // Every unlit dot, then the line's lit ones over them.
        let columns = Int((Double(window.width) / kWatchLedPitch).rounded(.up))
        var dark = Path()
        for row in 0..<kWatchLedRows {
            for column in 0..<columns {
                let x = left + (Double(column) + 0.5) * kWatchLedPitch
                let y = top + (Double(row) + 0.5) * kWatchLedPitch
                dark.addEllipse(in: CGRect(x: x - dot, y: y - dot, width: dot * 2, height: dot * 2))
            }
        }
        clipped.fill(dark, with: .color(WatchRGBA(argb: 36, 255, 150, 40).color))
        var lefts: [Double] = []
        if strip.width <= Double(window.width) {
            lefts.append(left + ((Double(window.width) - strip.width) / 2 / kWatchLedPitch).rounded(.down) * kWatchLedPitch)
        } else {
            let first = left - scroll(agent, strip.width)
            lefts.append(first)
            let again = first + strip.width + Double(kWatchLedGapDots) * kWatchLedPitch
            if again < Double(window.maxX) { lefts.append(again) }
        }
        var halo = Path()
        var lit = Path()
        let haloRadius = dot * 1.5
        for start in lefts {
            var i = 0
            while i + 1 < strip.dots.count {
                let x = start + (Double(strip.dots[i]) + 0.5) * kWatchLedPitch
                if x >= left - kWatchLedPitch && x <= Double(window.maxX) + kWatchLedPitch {
                    let y = top + (Double(strip.dots[i + 1]) + 0.5) * kWatchLedPitch
                    halo.addEllipse(in: CGRect(x: x - haloRadius, y: y - haloRadius, width: haloRadius * 2, height: haloRadius * 2))
                    lit.addEllipse(in: CGRect(x: x - dot, y: y - dot, width: dot * 2, height: dot * 2))
                }
                i += 2
            }
        }
        clipped.fill(halo, with: .color(WatchRGBA(argb: 50, 255, 140, 0).color))
        clipped.fill(lit, with: .color(WatchRGBA(argb: 255, 255, 190, 60).color))
    }

    /// How far a display has scrolled, a whole dot at a time (`ledScroll`).
    private func scroll(_ agent: WatchedAgent, _ width: Double) -> Double {
        let t = now - agent.saidSeconds - kWatchLedHold
        if t <= 0 { return 0 }
        let moved = (t * kWatchLedSpeed).truncatingRemainder(dividingBy: width + Double(kWatchLedGapDots) * kWatchLedPitch)
        return (moved / kWatchLedPitch).rounded(.down) * kWatchLedPitch
    }

    /// Whether any display is scrolling: the field keeps painting for it.
    static func ledsMoving(_ controller: AgentWatchController, _ leds: WatchLeds, _ layout: WatchFieldLayout) -> Bool {
        for agent in controller.agents where agent.presence > 0.01 && !agent.said.isEmpty {
            guard let strip = leds.stripFor(agent.said) else { continue }
            let centre = layout.pointFor(agent)
            let box = layout.ledRect(agent, centre, layout.radiusFor(agent, controller.now))
            if strip.width > Double(box.width) - 2 * kWatchLedPad { return true }
        }
        return false
    }

    // MARK: The card

    private func paintCard(_ ctx: GraphicsContext, _ size: CGSize) {
        guard let agent = selected, agent.presence > 0.01 else { return }
        let lines = watchCardLines(controller, agent)
        let lineStyle = WatchTextStyle(size: 11)
        let lineHeight = watchLine(ctx, "Hg", lineStyle).height + 3
        let maxWidth = min(420.0, Double(size.width) - 28)
        var widest = 0.0
        var painted: [WatchLine] = []
        for (i, line) in lines.enumerated() {
            var style = lineStyle
            style.colour = i == 0 ? watchForeground : watchMuted
            let text = watchLine(ctx, line, style, maxWidth: maxWidth)
            painted.append(text)
            widest = max(widest, text.width)
        }
        let keyRowsMax = 12
        let kinds = controller.kindsOf(agent)
        let keyRows = min(kinds.count, keyRowsMax)
        let keyMore = kinds.count > keyRowsMax
        let keyTotal = kinds.reduce(0) { $0 + $1.1 }
        let heading = watchLine(
            ctx, "its marks: \(keyTotal) in all, \(kinds.count) kind\(kinds.count == 1 ? "" : "s")", lineStyle)
        let iconRoom = 20.0
        var countWidth = 0.0
        var labelWidth = 0.0
        for i in 0..<keyRows {
            countWidth = max(countWidth, watchLine(ctx, "\(kinds[i].1)", lineStyle).width)
            labelWidth = max(labelWidth, watchLine(ctx, kinds[i].0.label, lineStyle).width)
        }
        if keyRows > 0 {
            widest = max(widest, heading.width)
            for i in 0..<keyRows {
                let meaning = watchLine(ctx, kinds[i].0.description, lineStyle).width
                let row = iconRoom + countWidth + 8 + labelWidth + 10 + meaning
                widest = max(widest, min(maxWidth, row))
            }
        }
        let keyLines = keyRows > 0 ? keyRows + 1 + (keyMore ? 1 : 0) : 0
        let w = min(widest + 20, Double(size.width) - 8)
        let keyHeight: Double = keyLines > 0 ? lineHeight * Double(keyLines) + 6 : 0
        let h: Double = lineHeight * Double(lines.count) + keyHeight + 14
        let anchor = layout.pointFor(agent)
        let gap = layout.radiusFor(agent, now) + 20
        var left = Double(anchor.x) + gap
        if left + w > Double(size.width) - 8 { left = Double(anchor.x) - gap - w }
        let rightmost: Double = max(4.0, Double(size.width) - w - 4)
        left = min(max(left, 4.0), rightmost)
        let lowest: Double = max(4.0, Double(size.height) - h - 4)
        let top: Double = min(max(Double(anchor.y) - h / 2, 4.0), lowest)
        let card = Path(roundedRect: CGRect(x: left, y: top, width: w, height: h), cornerRadius: 7)
        ctx.fill(card, with: .color(WatchRGBA(argb: 235, 10, 14, 24).color))
        ctx.stroke(card, with: .color(watchInk(agent.hue, 0.8, 0.6, 0.6).color), lineWidth: 1)
        var y = top + 7
        for text in painted {
            text.draw(in: ctx, at: CGPoint(left + 10, y + (lineHeight - text.height) / 2))
            y += lineHeight
        }
        if keyRows == 0 { return }
        y += 3
        var rule = Path()
        rule.move(to: CGPoint(left + 10, y))
        rule.addLine(to: CGPoint(left + w - 10, y))
        ctx.stroke(rule, with: .color(watchGuide.color), lineWidth: 1)
        y += 3
        heading.draw(in: ctx, at: CGPoint(left + 10, y + (lineHeight - heading.height) / 2))
        y += lineHeight
        for i in 0..<keyRows {
            let (motion, count) = kinds[i]
            var x = left + 10
            paintWatchIcon(ctx, motion, CGPoint(x + 7, y + lineHeight / 2), 6.0, 1.0)
            x += iconRoom
            var countStyle = lineStyle
            countStyle.colour = watchForeground
            let number = watchLine(ctx, "\(count)", countStyle)
            number.draw(in: ctx, at: CGPoint(x + countWidth - number.width, y + (lineHeight - number.height) / 2))
            x += countWidth + 8
            var labelStyle = lineStyle
            labelStyle.colour = watchInk(motion.hue, 0.8, 0.72)
            let label = watchLine(ctx, motion.label, labelStyle)
            label.draw(in: ctx, at: CGPoint(x, y + (lineHeight - label.height) / 2))
            x += labelWidth + 10
            let room = left + w - 10 - x
            if room > 12 {
                var meaningStyle = lineStyle
                meaningStyle.colour = watchMuted
                let meaning = watchLine(ctx, motion.description, meaningStyle, maxWidth: room)
                meaning.draw(in: ctx, at: CGPoint(x, y + (lineHeight - meaning.height) / 2))
            }
            y += lineHeight
        }
        if keyMore {
            var rest = 0
            for i in keyRowsMax..<kinds.count { rest += kinds[i].1 }
            var moreStyle = lineStyle
            moreStyle.colour = watchMuted
            let more = watchLine(
                ctx, "and \(rest) more of \(kinds.count - keyRowsMax) other kinds", moreStyle,
                maxWidth: w - 20 - iconRoom)
            more.draw(in: ctx, at: CGPoint(left + 10 + iconRoom, y + (lineHeight - more.height) / 2))
        }
    }

    // MARK: Ring text

    /// Writes `text` round `centre`, standing away from it, centred over the
    /// top or under the bottom, reading left to right the right way up
    /// (`drawRingText`). Cut short past `sweep` radians; returns the radians
    /// it took.
    private func ringText(
        _ ctx: GraphicsContext, _ centre: CGPoint, _ inner: Double, _ top: Bool, _ text: String,
        _ colour: WatchRGBA, bold: Bool, sweep: Double
    ) -> Double {
        let style = WatchTextStyle(size: kWatchLabelPixels, bold: bold, colour: colour)
        let (ascent, descent) = WatchRingMetrics.metrics
        let baseline = inner + (top ? descent : ascent)
        // The project's letters are spaced a tenth wider, as on the desktop.
        let spacing = bold ? 1.1 : 1.0
        func advance(_ letter: String) -> Double { WatchRingMetrics.width(ctx, letter, bold) * spacing }
        var letters = text.map { String($0) }
        let room = sweep * baseline
        var total = letters.reduce(0.0) { $0 + advance($1) }
        if total > room {
            let dots = advance("\u{2026}")
            while !letters.isEmpty && total + dots > room {
                total -= advance(letters.removeLast())
            }
            if letters.isEmpty { return 0 }
            letters.append("\u{2026}")
            total += dots
        }
        if letters.isEmpty { return 0 }
        let direction = top ? 1.0 : -1.0
        let taken = total / baseline
        var at = (top ? -Double.pi / 2 : Double.pi / 2) - direction * taken / 2
        for letter in letters {
            let width = advance(letter)
            let angle = at + direction * width / baseline / 2
            at += direction * width / baseline
            if letter.trimmingCharacters(in: .whitespaces).isEmpty { continue }
            let glyph = watchLine(ctx, letter, style)
            var turned = ctx
            turned.translateBy(
                x: Double(centre.x) + cos(angle) * baseline, y: Double(centre.y) + sin(angle) * baseline)
            turned.rotate(by: .radians(angle + direction * Double.pi / 2))
            glyph.draw(in: turned, at: CGPoint(-glyph.width / 2, -glyph.baseline))
        }
        return taken
    }

    // MARK: Dashes

    private func dashedLine(
        _ ctx: GraphicsContext, _ from: CGPoint, _ to: CGPoint, _ colour: WatchRGBA, _ width: Double, _ dash: Double,
        _ gap: Double
    ) {
        var path = Path()
        path.move(to: from)
        path.addLine(to: to)
        ctx.stroke(path, with: .color(colour.color), style: StrokeStyle(lineWidth: width, dash: [dash, gap]))
    }

    /// Qt's DotLine round a circle: a dot the pen's width long, a gap twice it.
    private func dottedCircle(
        _ ctx: GraphicsContext, _ centre: CGPoint, _ radius: Double, _ colour: WatchRGBA, _ penWidth: Double,
        _ cap: CGLineCap
    ) {
        let circumference = 2 * Double.pi * radius
        let period = penWidth * 3
        let count = max(1, Int((circumference / period).rounded(.down)))
        let sweep = penWidth / radius
        var path = Path()
        for i in 0..<count {
            let start = Double(i) * 2 * Double.pi / Double(count)
            path.move(to: watchPolar(centre, radius, start))
            path.addArc(
                center: centre, radius: radius, startAngle: .radians(start), endAngle: .radians(start + sweep),
                clockwise: false)
        }
        ctx.stroke(path, with: .color(colour.color), style: StrokeStyle(lineWidth: penWidth, lineCap: cap))
    }
}

/// The ring text's font metrics and letter widths, measured once.
@MainActor
enum WatchRingMetrics {
    private static var widths: [String: Double] = [:]

    /// (ascent, descent) of the ring text's font.
    static let metrics: (Double, Double) = {
        let font = UIFont.systemFont(ofSize: CGFloat(kWatchLabelPixels))
        return (Double(font.ascender), Double(-font.descender))
    }()

    static func width(_ ctx: GraphicsContext, _ letter: String, _ bold: Bool) -> Double {
        let key = "\(letter)|\(bold)"
        if let known = widths[key] { return known }
        if widths.count > 6000 { widths.removeAll() }
        let width = watchLine(ctx, letter, WatchTextStyle(size: kWatchLabelPixels, bold: bold)).width
        widths[key] = width
        return width
    }
}
