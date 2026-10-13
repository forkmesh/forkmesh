// The marks of the Watch field and the icons of its sound list, a
// line-by-line port of the desktop's AgentActivityView::paintFigure and
// ::paintIcon (desktop/src/AgentActivityView.cpp), drawn with the QPainter
// stand-in in AgentWatchPainter.swift, so a run looks the same on the phone
// as it does on the desktop and a change to one side can be carried to the
// other line for line.

import FMAgentWatch
import Foundation
import SwiftUI

// The figure of a cue - a log line's family, a new host, background work -
// centred on `at`, `size` its half-width. The same drawing is the mark in the
// field and the icon on the list, so the two always match. `turn` spins the
// ones that move (0..1).
private func paintCueGlyph(
    _ painter: inout QtPainter, _ motion: AgentMotion, _ at: CGPoint, _ size: Double, _ colour: WatchRGBA,
    _ turn: Double
) {
    let pen = QtPen(colour, max(1.0, size * 0.26), .solid, .round, .round)
    let fill = colour.withAlpha(colour.a * 0.3)
    painter.save()
    painter.translate(at)
    switch motion {
    case .connect:
        // A signal: a dot and two arcs opening upwards from it.
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(CGPoint(0, size * 0.55), size * 0.22, size * 0.22)
        painter.setPen(pen)
        painter.noBrush()
        for k in 1...2 {
            let radius: Double = size * 0.5 * Double(k)
            painter.drawArc(watchBox(-radius, size * 0.55 - radius, radius * 2, radius * 2), 45 * 16, 90 * 16)
        }
    case .task:
        // A box turning over: work picked up.
        painter.rotate(turn * 180.0)
        painter.setPen(pen)
        painter.setBrush(fill)
        painter.drawRect(watchBox(-size * 0.6, -size * 0.6, size * 1.2, size * 1.2))
    case .logError:
        // A cross, struck.
        painter.rotate(turn * 30.0)
        painter.setPen(QtPen(colour, max(1.2, size * 0.34), .solid, .round))
        painter.drawLine(CGPoint(-size * 0.7, -size * 0.7), CGPoint(size * 0.7, size * 0.7))
        painter.drawLine(CGPoint(-size * 0.7, size * 0.7), CGPoint(size * 0.7, -size * 0.7))
    case .logStall:
        // An hourglass.
        painter.setPen(pen)
        painter.setBrush(fill)
        painter.drawPolygon([
            CGPoint(-size * 0.6, -size * 0.8),
            CGPoint(size * 0.6, -size * 0.8),
            CGPoint(-size * 0.6, size * 0.8),
            CGPoint(size * 0.6, size * 0.8),
        ])
    case .logSync:
        // Two arrows chasing round a circle.
        painter.rotate(turn * 180.0)
        painter.setPen(pen)
        painter.noBrush()
        let box = watchBox(-size * 0.75, -size * 0.75, size * 1.5, size * 1.5)
        for k in 0..<2 {
            painter.drawArc(box, (k * 180 + 20) * 16, 130 * 16)
            let tip: Double = Double(k * 180 + 150) * Double.pi / 180.0
            let end = CGPoint(cos(tip) * size * 0.75, -sin(tip) * size * 0.75)
            let back = CGPoint(cos(tip - 0.5) * size * 1.0, -sin(tip - 0.5) * size * 1.0)
            painter.drawLine(end, back)
        }
    case .logGit:
        // A branch: a stem forking to two tips.
        painter.setPen(pen)
        painter.drawLine(CGPoint(-size * 0.4, size * 0.8), CGPoint(-size * 0.4, -size * 0.8))
        painter.drawLine(CGPoint(-size * 0.4, size * 0.2), CGPoint(size * 0.45, -size * 0.3))
        painter.noPen()
        painter.setBrush(colour)
        for node in [
            CGPoint(-size * 0.4, size * 0.8),
            CGPoint(-size * 0.4, -size * 0.8),
            CGPoint(size * 0.45, -size * 0.3),
        ] {
            painter.drawEllipse(node, size * 0.24, size * 0.24)
        }
    case .logCloud:
        // A cloud: three puffs on a flat base.
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(CGPoint(-size * 0.45, size * 0.15), size * 0.42, size * 0.38)
        painter.drawEllipse(CGPoint(0, -size * 0.15), size * 0.5, size * 0.48)
        painter.drawEllipse(CGPoint(size * 0.45, size * 0.15), size * 0.42, size * 0.38)
        painter.drawRect(watchBox(-size * 0.45, size * 0.1, size * 0.9, size * 0.43))
    case .logTask:
        // A note: a page with two lines on it.
        painter.setPen(pen)
        painter.setBrush(fill)
        painter.drawRect(watchBox(-size * 0.55, -size * 0.75, size * 1.1, size * 1.5))
        painter.drawLine(CGPoint(-size * 0.25, -size * 0.25), CGPoint(size * 0.25, -size * 0.25))
        painter.drawLine(CGPoint(-size * 0.25, size * 0.2), CGPoint(size * 0.25, size * 0.2))
    case .logAccount:
        // A key.
        painter.rotate(-30.0 + turn * 40.0)
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(CGPoint(-size * 0.45, 0), size * 0.35, size * 0.35)
        painter.drawLine(CGPoint(-size * 0.1, 0), CGPoint(size * 0.8, 0))
        painter.drawLine(CGPoint(size * 0.55, 0), CGPoint(size * 0.55, size * 0.3))
        painter.drawLine(CGPoint(size * 0.8, 0), CGPoint(size * 0.8, size * 0.3))
    case .logInfo:
        // An i: a dot over a stem.
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(CGPoint(0, -size * 0.6), size * 0.2, size * 0.2)
        painter.setPen(pen)
        painter.drawLine(CGPoint(0, -size * 0.15), CGPoint(0, size * 0.75))
    default:
        break
    }
    painter.restore()
}

/// One mark flying off a body: `c` its centre, `r` its radius, `t` how far
/// through its life the mark is (0..1), `angle` which way it goes (0..1 of a
/// turn) and `s` how strong it is (0..1).
func paintWatchFigure(
    _ ctx: GraphicsContext, _ motion: AgentMotion, _ c: CGPoint, _ r: Double, _ t: Double, _ angle: Double,
    _ s: Double
) {
    let hue = motion.hue
    let fade: Double = pow(1.0 - t, 1.2) * (0.35 + 0.65 * s)
    if fade <= 0.01 {
        return
    }
    var painter = QtPainter(ctx)
    let a: Double = angle * 2.0 * Double.pi
    let dir = CGPoint(cos(a), sin(a))
    let normal = CGPoint(-Double(dir.y), Double(dir.x))
    let line = watchInk(hue, 0.92, 0.66, fade)
    let lit = watchInk(hue, 1.0, 0.80, fade)
    let widthBase: Double = motion.weight * (0.9 + 1.4 * s) * (1.0 - 0.5 * t)
    let width: Double = max(0.8, widthBase)
    let pen = QtPen(line, width, .solid, .round, .round)
    let cx = Double(c.x)
    let cy = Double(c.y)

    switch motion {
    case .start:
        // A flower opening: petals turning outwards, and a ring behind them.
        painter.noPen()
        painter.setBrush(watchInk(hue, 0.9, 0.66, fade * 0.8))
        let reach: Double = r * (1.0 + 1.2 * watchEaseOut(t))
        let petals = 6
        for i in 0..<petals {
            let at: Double = a + Double(i) * 2.0 * Double.pi / Double(petals) + t * 0.6
            painter.save()
            painter.translate(watchPolar(c, reach, at))
            painter.rotate(at * 180.0 / Double.pi)
            let len: Double = r * 0.42 * (1.0 - 0.4 * t)
            painter.drawEllipse(.zero, len, len * 0.38)
            painter.restore()
        }
        painter.setPen(pen)
        painter.noBrush()
        let ring: Double = r * (1.0 + 1.9 * watchEaseOut(t))
        painter.drawEllipse(c, ring, ring)
    case .speak:
        // Sound going out: three wave fronts, one after another.
        painter.noBrush()
        for k in 0..<3 {
            let tk: Double = (t - Double(k) * 0.14) / (1.0 - 0.28)
            if tk <= 0 || tk >= 1 {
                continue
            }
            painter.setPen(pen.withColour(watchInk(hue, 0.92, 0.70, fade * (1.0 - tk))))
            let radius: Double = r * (1.15 + 1.6 * tk)
            let box = watchBox(cx - radius, cy - radius, radius * 2, radius * 2)
            let mid: Double = -a * 180.0 / Double.pi
            painter.drawArc(box, Int((mid - 32) * 16), 64 * 16)
        }
    case .think:
        // Motes orbiting slowly, each with a tail: turning something over.
        let motes = 2
        for k in 0..<motes {
            let at: Double = a + Double(k) * 2.0 * Double.pi / Double(motes) + t * 2.4
            let wobble: Double = sin(t * 6.0 + Double(k) * 2.0)
            let radius: Double = r * (1.35 + 0.22 * wobble)
            painter.setPen(QtPen(watchInk(hue, 0.8, 0.72, fade * 0.6), 1.4, .solid, .round))
            painter.noBrush()
            let box = watchBox(cx - radius, cy - radius, radius * 2, radius * 2)
            let start16: Double = -at * 180.0 / Double.pi * 16
            painter.drawArc(box, Int(start16), 40 * 16)
            painter.noPen()
            painter.setBrush(lit)
            let dot: Double = 1.8 + 1.6 * s
            painter.drawEllipse(watchPolar(c, radius, at), dot, dot)
        }
    case .read:
        // A beam reaching out to a page, and the page drawn back in.
        let out: Double = r * (1.0 + 1.7 * watchEaseOut(min(1.0, t * 2.2)))
        var tip = c + dir * out
        if t > 0.5 {
            let back: Double = r + (out - r) * (1.0 - watchEaseInOut((t - 0.5) * 2.0))
            tip = c + dir * back
        }
        painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * 0.7), 1.2, .solid, .round))
        painter.drawLine(c + dir * r, tip)
        painter.save()
        painter.translate(tip)
        painter.rotate(a * 180.0 / Double.pi + t * 90.0)
        let half: Double = 2.5 + 2.5 * s
        painter.setPen(QtPen(lit, 1.0))
        painter.setBrush(watchInk(hue, 0.9, 0.55, fade * 0.5))
        painter.drawRect(watchBox(-half, -half * 1.3, half * 2, half * 2.6))
        painter.restore()
    case .search:
        // A radar sweep: a wedge turning, and a dashed ring reaching out.
        let radius: Double = r * (1.2 + 1.3 * watchEaseOut(t))
        let start: Double = -a * 180.0 / Double.pi - t * 220.0
        // Qt blends gradient stops premultiplied, so a stop at alpha 0 carries
        // no colour: the centre stop is the edge's colour made clear.
        let edge = watchInk(hue, 0.95, 0.66, fade * 0.55)
        painter.noPen()
        painter.setBrushShader(.radial(centre: c, radius: radius, colours: [edge.withAlpha(0), edge], stops: nil))
        let box = watchBox(cx - radius, cy - radius, radius * 2, radius * 2)
        painter.drawPie(box, Int(start * 16), 34 * 16)
        painter.setPen(pen.withDash(.dash).withWidth(max(0.8, width * 0.7)))
        painter.noBrush()
        painter.drawEllipse(c, radius, radius)
    case .web:
        // Out to the web: a globe flung off the body on a thread, turning, and
        // pulled back in.
        let out: Double = r * (1.2 + 2.0 * watchEaseOut(min(1.0, t * 2.0)))
        let along: Double = t < 0.6 ? out : r + (out - r) * (1.0 - watchEaseInOut((t - 0.6) / 0.4))
        let at = c + dir * along
        painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * 0.6), 1.0, .dot, .round))
        painter.drawLine(c + dir * r, at)
        let g: Double = 3.5 + 3.0 * s
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.5, fade * 0.25))
        painter.drawEllipse(at, g, g)
        painter.noBrush()
        let turn: Double = abs(cos(t * 6.0))
        painter.drawEllipse(at, g * turn, g)
        painter.drawLine(at + CGPoint(-g, 0), at + CGPoint(g, 0))
    case .plan:
        // A checklist beside the body: rows appearing, each ticked off.
        let origin = watchPolar(c, r * 1.5, a) + CGPoint(0, -r * 0.5)
        let row: Double = 4.0 + 1.5 * s
        for k in 0..<3 {
            let appear: Double = min(max(t * 3.2 - Double(k) * 0.6, 0.0), 1.0)
            if appear <= 0 {
                continue
            }
            let at = origin + CGPoint(0, Double(k) * row * 1.5)
            let ax = Double(at.x)
            let ay = Double(at.y)
            painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * appear), 1.1, .solid, .round, .round))
            painter.noBrush()
            painter.drawRect(watchBox(ax, ay - row * 0.5, row, row))
            let lineEnd: Double = row * 1.5 + row * 2.2 * appear
            painter.drawLine(at + CGPoint(row * 1.5, 0), at + CGPoint(lineEnd, 0))
            if appear >= 1.0 {
                painter.setPen(QtPen(lit, 1.4, .solid, .round, .round))
                painter.drawPolyline([
                    at + CGPoint(row * 0.15, 0),
                    at + CGPoint(row * 0.45, row * 0.35),
                    at + CGPoint(row * 1.0, -row * 0.45),
                ])
            }
        }
    case .write:
        // A diamond set down and spreading, ink falling from it.
        let half: Double = r * (0.8 + 0.9 * watchEaseOut(t))
        painter.save()
        painter.translate(c)
        painter.rotate(45.0 + t * 35.0 + angle * 90.0)
        painter.setPen(pen)
        painter.noBrush()
        painter.drawRect(watchBox(-half, -half, half * 2, half * 2))
        painter.noPen()
        painter.setBrush(lit)
        for i in 0..<4 {
            let corner = CGPoint(i % 2 != 0 ? half : -half, i / 2 != 0 ? half : -half)
            let spark: Double = 1.4 + 1.8 * s * (1.0 - t)
            painter.drawEllipse(corner, spark, spark)
        }
        painter.restore()
        painter.noPen()
        painter.setBrush(watchInk(hue, 0.9, 0.7, fade * 0.8))
        for i in 0..<3 {
            let dx: Double = cx + Double(i - 1) * r * 0.45
            let dy: Double = cy + r + t * r * (1.2 + 0.4 * Double(i))
            let drop = CGPoint(dx, dy)
            painter.drawEllipse(drop, 1.6, 2.2)
        }
    case .run:
        // A bolt: a zigzag struck outwards, flickering as it goes.
        let reach: Double = r * (1.0 + 1.9 * watchEaseOut(min(1.0, t * 3.0)))
        let flicker = Int(t * 14.0)
        let from = c + dir * r
        var bolt = Path()
        bolt.move(to: from)
        for i in 1...5 {
            let u: Double = Double(i) / 5.0
            var side: Double = 0.0
            if i != 5 {
                let sign: Double = (i + flicker) % 2 != 0 ? 1.0 : -1.0
                side = sign * r * (0.16 + 0.1 * watchScatter(i * 31 + flicker))
            }
            let along: Double = r + (reach - r) * u
            let to = c + dir * along + normal * side
            bolt.addLine(to: to)
        }
        painter.noBrush()
        painter.setPen(QtPen(watchInk(hue, 1.0, 0.85, fade * 0.35), width * 3.0, .solid, .round, .round))
        painter.drawPath(bolt)
        painter.setPen(pen)
        painter.drawPath(bolt)
        painter.noPen()
        painter.setBrush(lit)
        let tip = c + dir * reach
        for i in 0..<3 {
            let at: Double = a + Double(i - 1) * 0.7
            painter.drawEllipse(watchPolar(tip, r * 0.5 * watchEaseOut(t), at), 1.3, 1.3)
        }
    case .delegate:
        // A little one launched off the body on a curve, trailing, and gone
        // with a pop where it lands.
        let from = c + dir * r
        let to = c + dir * (r * 3.4) + normal * (r * 0.8)
        let control = c + dir * (r * 2.0) - normal * (r * 1.2)
        painter.noPen()
        for k in stride(from: 6, through: 0, by: -1) {
            let u: Double = watchEaseOut(t) - Double(k) * 0.035
            if u <= 0 {
                continue
            }
            painter.setBrush(watchInk(hue, 0.9, 0.72, fade * (1.0 - Double(k) / 7.0)))
            let dot: Double = (2.2 + 2.2 * s) * (1.0 - Double(k) / 9.0)
            painter.drawEllipse(watchOnQuad(from, control, to, u), dot, dot)
        }
        if t > 0.75 {
            painter.setPen(pen)
            painter.noBrush()
            let pop: Double = r * 0.9 * (t - 0.75) * 4.0
            painter.drawEllipse(to, pop, pop)
        }
    case .arrive:
        // An answer coming back: motes falling in from all round.
        let radius: Double = r * (1.0 + 2.1 * (1.0 - watchEaseOut(t)))
        let motes = 6
        for i in 0..<motes {
            let at: Double = a + Double(i) * 2.0 * Double.pi / Double(motes)
            let head = watchPolar(c, radius, at)
            painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * 0.55), 1.1, .solid, .round))
            let trailing: Double = radius + r * 0.45 * (1.0 - t)
            painter.drawLine(head, watchPolar(c, trailing, at))
            painter.noPen()
            painter.setBrush(lit)
            painter.drawEllipse(head, 1.4 + 1.2 * s, 1.4 + 1.2 * s)
        }
    case .prompt:
        // You, steering: a comet falling onto the body from above, and the
        // ring it makes when it lands.
        let fall: Double = watchEaseInOut(min(1.0, t * 1.7))
        let fromX: Double = -r * 1.2 + 2.4 * r * angle
        let from = c + CGPoint(fromX, -r * 4.5)
        let head = from + (c - from) * fall
        if fall < 1.0 {
            let tail = WatchShader.linear(
                from: from, to: head,
                colours: [watchInk(hue, 1.0, 0.8, 0.0), watchInk(hue, 1.0, 0.8, fade)], stops: nil)
            painter.setPen(QtPen(WatchRGBA(hex: 0x0000_0000), 2.2 + 1.6 * s, .solid, .round, .bevel, tail))
            painter.drawLine(from + (head - from) * 0.4, head)
            painter.noPen()
            drawWatchGlow(ctx, head, 7 + 5 * s, watchInk(hue, 1.0, 0.85, fade))
        } else {
            let u: Double = (t - 1.0 / 1.7) / (1.0 - 1.0 / 1.7)
            painter.setPen(pen)
            painter.noBrush()
            let ring: Double = r * (1.0 + 2.2 * watchEaseOut(u))
            painter.drawEllipse(c, ring, ring)
        }
    case .notice:
        // A warning: a triangle flaring and shaking.
        let shake: Double = sin(t * 70.0) * 2.5 * (1.0 - t)
        let radius: Double = r * (1.25 + 1.2 * watchEaseOut(t))
        var triangle: [CGPoint] = []
        for i in 0..<3 {
            let at: Double = -Double.pi / 2 + Double(i) * 2.0 * Double.pi / 3.0
            triangle.append(watchPolar(c + CGPoint(shake, 0), radius, at))
        }
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.5, fade * 0.18))
        painter.drawPolygon(triangle)
    case .finish:
        // A firework: sparks bursting out and drooping, and a ring.
        let burst: Double = r * (1.0 + 3.0 * watchEaseOut(t))
        painter.noPen()
        let sparks = 12
        for i in 0..<sparks {
            let at: Double = a + Double(i) * 2.0 * Double.pi / Double(sparks)
            let spread: Double = burst * (0.8 + 0.2 * watchScatter(i * 13))
            let droop: Double = t * t * r * 1.4
            let p = watchPolar(c, spread, at) + CGPoint(0, droop)
            painter.setBrush(watchInk(hue + 0.04 * Double(i % 3), 1.0, 0.78, fade))
            painter.drawEllipse(p, 1.5 + 1.4 * s, 1.5 + 1.4 * s)
        }
        painter.setPen(pen)
        painter.noBrush()
        let ring: Double = r * (1.0 + 1.6 * watchEaseOut(t))
        painter.drawEllipse(c, ring, ring)
    case .frame:
        // Framing: a pair of brackets closing round a point just off the body,
        // a message or block opening and shutting.
        let at = watchPolar(c, r * 1.35, a)
        let half: Double = (3.0 + 2.0 * s) * (1.0 - 0.3 * t)
        let gap: Double = half * (1.4 - 0.8 * watchEaseOut(t))
        painter.save()
        painter.translate(at)
        painter.rotate(a * 180.0 / Double.pi)
        painter.setPen(QtPen(lit, 1.1, .solid, .square, .miter))
        painter.noBrush()
        for side: Double in [-1.0, 1.0] {
            let inner: Double = side * (gap - half * 0.4)
            let outer: Double = side * gap
            painter.drawPolyline([
                CGPoint(inner, -half),
                CGPoint(outer, -half),
                CGPoint(outer, half),
                CGPoint(inner, half),
            ])
        }
        painter.restore()
    case .compose:
        // A tool call being written: three dots typing out along an arc.
        painter.noPen()
        let shown = min(max(Int(t * 4.0) + 1, 1), 3)
        for k in 0..<shown {
            let at = watchPolar(c, r * (1.3 + 0.1 * Double(k)), a + Double(k) * 0.28)
            painter.setBrush(watchInk(hue, 0.9, 0.74, fade * (1.0 - 0.2 * Double(k))))
            let dot: Double = 1.3 + 0.9 * s
            painter.drawEllipse(at, dot, dot)
        }
    case .usage:
        // The meter: three bars rising under the body, filled as far as the
        // report was big, then draining.
        let rise: Double = watchEaseOut(min(1.0, t * 2.5))
        let base = c + CGPoint(-r * 0.5, r * 1.25)
        let bx = Double(base.x)
        let by = Double(base.y)
        painter.noPen()
        for k in 0..<3 {
            let dk = Double(k)
            let height: Double = r * (0.25 + 0.2 * dk) * rise * (0.5 + 0.5 * s)
            painter.setBrush(watchInk(hue, 0.9, 0.62 + 0.06 * dk, fade * 0.9))
            painter.drawRect(watchBox(bx + dk * r * 0.38, by + r * 0.7 - height, max(1.6, r * 0.24), height))
        }
    case .system:
        // The machinery: a small cog turning just off the body.
        let at = watchPolar(c, r * 1.5, a)
        let radius: Double = (3.2 + 2.0 * s) * (1.0 + 0.3 * watchEaseOut(t))
        painter.save()
        painter.translate(at)
        painter.rotate(t * 120.0)
        painter.setPen(QtPen(lit, 1.1, .solid, .round))
        painter.noBrush()
        painter.drawEllipse(.zero, radius * 0.55, radius * 0.55)
        for i in 0..<6 {
            let at2: Double = Double(i) * Double.pi / 3.0
            painter.drawLine(watchPolar(.zero, radius * 0.6, at2), watchPolar(.zero, radius, at2))
        }
        painter.restore()
    case .tick:
        // A line of bookkeeping: a small twinkle just off the body.
        let at = watchPolar(c, r * 1.4, a)
        let size: Double = (2.5 + 2.5 * s) * (1.0 - 0.5 * t)
        painter.save()
        painter.translate(at)
        painter.rotate(t * 90.0)
        painter.setPen(QtPen(lit, 1.0, .solid, .round))
        painter.drawLine(CGPoint(-size, 0), CGPoint(size, 0))
        painter.drawLine(CGPoint(0, -size), CGPoint(0, size))
        painter.restore()
    case .network:
        // Bytes on the wire: two beads spiralling off the body.
        painter.noPen()
        for k in 0..<2 {
            let at: Double = a + Double(k) * Double.pi + t * 5.0
            let radius: Double = r * (1.1 + 1.2 * t)
            painter.setBrush(watchInk(hue, 0.9, 0.78, fade))
            painter.drawEllipse(watchPolar(c, radius, at), 1.6, 1.6)
        }
    case .scan:
        // A listing read off: a row of dots lighting one after another round
        // an arc outside the body.
        let dots = 5
        painter.noPen()
        for k in 0..<dots {
            let dk = Double(k)
            let raw: Double = t * Double(dots + 2) * 0.5 - dk * 0.5
            let appear: Double = min(max(raw, 0.0), 1.0)
            if appear <= 0 {
                continue
            }
            let at: Double = a - 0.6 + 1.2 * dk / Double(max(1, dots - 1))
            painter.setBrush(watchInk(hue, 0.9, 0.72, fade * appear))
            let radius: Double = r * (1.7 + 0.5 * watchEaseOut(t))
            painter.drawEllipse(watchPolar(c, radius, at), 1.5 + s, 1.5 + s)
        }
    case .create:
        // Something new: a four-pointed star born out beyond the body and
        // growing, tied back to it by a line.
        let at = c + dir * (r * (1.6 + 1.3 * watchEaseOut(t)))
        painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * 0.5), 1.0, .dot))
        painter.drawLine(c + dir * r, at)
        let size: Double = (4.0 + 5.0 * s) * watchEaseOut(min(1.0, t * 2.5))
        var star = Path()
        for i in 0..<8 {
            let at2: Double = a + t * 1.5 + Double(i) * Double.pi / 4.0
            let p = watchPolar(at, i % 2 != 0 ? size * 0.3 : size, at2)
            if i == 0 {
                star.move(to: p)
            } else {
                star.addLine(to: p)
            }
        }
        star.closeSubpath()
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 1.0, 0.75, fade * 0.7))
        painter.drawPath(star)
    case .test:
        // Tests: a pendulum swinging fast back and forth outside the body, and
        // a tick at the end of each swing.
        let swing: Double = sin(t * 2.0 * Double.pi * 3.0) * 0.55
        let pivot = c + dir * r
        let bob = watchPolar(pivot, r * (0.9 + 0.5 * s), a + swing)
        painter.setPen(QtPen(watchInk(hue, 0.8, 0.7, fade * 0.6), 1.0))
        painter.drawLine(pivot, bob)
        painter.noPen()
        painter.setBrush(lit)
        painter.drawEllipse(bob, 2.4 + 1.6 * s, 2.4 + 1.6 * s)
        painter.setPen(QtPen(lit, 1.3, .solid, .round, .round))
        painter.noBrush()
        for side: Double in [-0.55, 0.55] {
            let end = watchPolar(pivot, r * 1.7, a + side)
            painter.drawPolyline([
                end + CGPoint(-2.5, 0),
                end + CGPoint(-0.5, 2.2),
                end + CGPoint(3.0, -2.6),
            ])
        }
    case .build:
        // A build: bricks stacking up beside the body, each landing with a
        // thud-ring under it.
        let bricks = 3
        let w: Double = r * 0.55
        let hgt: Double = r * 0.24
        let base = c + dir * (r * 1.9)
        let bx = Double(base.x)
        let by = Double(base.y)
        for k in 0..<bricks {
            let dk = Double(k)
            let land: Double = min(max(t * Double(bricks) * 1.6 - dk, 0.0), 1.0)
            if land <= 0 {
                continue
            }
            let drop: Double = (1.0 - watchEaseOut(land)) * r * 1.2
            let left: Double = bx - w / 2 + Double(k % 2) * w * 0.25
            let top: Double = by - Double(k + 1) * hgt * 1.15 - drop
            let brick = watchBox(left, top, w, hgt)
            painter.setPen(QtPen(lit, 1.0))
            painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.7))
            painter.drawRect(brick)
        }
        painter.setPen(QtPen(watchInk(hue, 0.9, 0.65, fade * (1.0 - t)), 2.0))
        painter.noBrush()
        let phase: Double = (t * Double(bricks)).truncatingRemainder(dividingBy: 1.0)
        let ring: Double = r * (1.0 + 0.6 * watchEaseOut(phase))
        painter.drawEllipse(c, ring, ring)
    case .git:
        // History: a line out of the body forking into branches, a commit dot
        // at each tip.
        let grow: Double = watchEaseOut(min(1.0, t * 1.8))
        let root = c + dir * r
        let fork = root + dir * (r * 0.9 * grow)
        painter.setPen(pen)
        painter.drawLine(root, fork)
        painter.setBrush(lit)
        let branches = 2
        for k in 0..<branches {
            let spread: Double = (Double(k) - Double(branches - 1) / 2.0) * 0.6
            let tip = watchPolar(fork, r * 1.1 * grow, a + spread)
            painter.setPen(pen)
            painter.drawLine(fork, tip)
            painter.noPen()
            painter.drawEllipse(tip, 2.2 + s, 2.2 + s)
        }
        painter.drawEllipse(fork, 1.8 + s, 1.8 + s)
    case .install:
        // A package arriving: a box dropping in from outside onto the body.
        let fall: Double = watchEaseInOut(min(1.0, t * 1.5))
        let from = c + dir * (r * 3.4)
        let at = from + (c + dir * (r * 1.1) - from) * fall
        let size: Double = 3.5 + 3.0 * s
        painter.save()
        painter.translate(at)
        painter.rotate(a * 180.0 / Double.pi + (1.0 - fall) * 120.0)
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.5))
        painter.drawRect(watchBox(-size, -size, size * 2, size * 2))
        painter.drawLine(CGPoint(-size, 0), CGPoint(size, 0))
        painter.restore()
    case .spawn:
        // Sent off to run on its own: a little copy of the body flung out on a
        // spiral, settling into an orbit of its own.
        let u: Double = watchEaseOut(t)
        let at: Double = a + u * 2.4
        let p = watchPolar(c, r * (1.2 + 2.2 * u), at)
        painter.setPen(QtPen(watchInk(hue, 0.9, 0.7, fade * 0.4), 1.0, .dot))
        painter.noBrush()
        let first = watchPolar(c, r * 1.2, a)
        var trail = Path()
        trail.move(to: first)
        for k in 1...12 {
            let v: Double = u * Double(k) / 12.0
            let next = watchPolar(c, r * (1.2 + 2.2 * v), a + v * 2.4)
            trail.addLine(to: next)
        }
        painter.drawPath(trail)
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.6, fade * 0.6))
        painter.drawEllipse(p, r * 0.28, r * 0.28)
    case .wait:
        // Waiting: a dashed ring slowly filling round the body, like a timer.
        let radius: Double = r * 1.55
        let box = watchBox(cx - radius, cy - radius, radius * 2, radius * 2)
        painter.setPen(QtPen(watchInk(hue, 0.7, 0.6, fade * 0.35), 1.0, .dot))
        painter.noBrush()
        painter.drawEllipse(c, radius, radius)
        painter.setPen(pen.withWidth(width * 1.4))
        let span: Double = -360.0 * watchEaseInOut(t) * 16
        painter.drawArc(box, 90 * 16, Int(span))
    case .ask:
        // A question to you: a speech bubble rising off the body with a "?".
        let rise: Double = -r * (1.6 + 1.2 * watchEaseOut(t))
        let at = c + CGPoint(r * 0.6, rise)
        let ax = Double(at.x)
        let ay = Double(at.y)
        let size: Double = 6.0 + 4.0 * s
        painter.save()
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.35))
        painter.drawRoundedRect(watchBox(ax - size, ay - size * 0.8, size * 2, size * 1.6), size * 0.5, size * 0.5)
        painter.setPen(QtPen(lit))
        painter.drawText(watchBox(ax - size, ay - size * 0.8, size * 2, size * 1.6), "?", Int(size * 1.4))
        painter.restore()
    case .skill:
        // A skill: a five-pointed star wheeling out and back.
        let out: Double = r * (1.3 + 1.4 * sin(Double.pi * t))
        let at = c + dir * out
        let size: Double = 4.0 + 4.0 * s
        var star = Path()
        for i in 0..<10 {
            let turn: Double = -Double.pi / 2 + t * 5.0 + Double(i) * Double.pi / 5
            let p = watchPolar(at, i % 2 != 0 ? size * 0.42 : size, turn)
            if i == 0 {
                star.move(to: p)
            } else {
                star.addLine(to: p)
            }
        }
        star.closeSubpath()
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 1.0, 0.7, fade * 0.6))
        painter.drawPath(star)
    case .mcp:
        // A remote tool: a cable run out to a socket, a square node at its end
        // that lights when the call connects.
        let from = c + dir * r
        let to = c + dir * (r * 3.0) + normal * (r * 0.6)
        let control = c + dir * (r * 2.0) - normal * (r * 0.8)
        let u: Double = watchEaseOut(min(1.0, t * 1.8))
        var cable = Path()
        cable.move(to: from)
        for k in 1...12 {
            let next = watchOnQuad(from, control, to, u * Double(k) / 12.0)
            cable.addLine(to: next)
        }
        painter.setPen(pen)
        painter.noBrush()
        painter.drawPath(cable)
        let end = watchOnQuad(from, control, to, u)
        let ex = Double(end.x)
        let ey = Double(end.y)
        let size: Double = 3.0 + 2.5 * s
        painter.setBrush(u >= 1.0 ? lit : watchInk(hue, 0.8, 0.45, fade * 0.5))
        painter.drawRect(watchBox(ex - size, ey - size, size * 2, size * 2))
    case .todo:
        // The list updated: short bars beside the body, each struck through in
        // turn.
        let origin = watchPolar(c, r * 1.6, a) + CGPoint(0, -r * 0.4)
        let rows = 3
        let row: Double = 3.5 + 1.5 * s
        for k in 0..<rows {
            let dk = Double(k)
            let raw: Double = t * Double(rows) * 1.4 - dk * 0.8
            let appear: Double = min(max(raw, 0.0), 1.0)
            if appear <= 0 {
                continue
            }
            let at = origin + CGPoint(0, dk * row * 1.6)
            painter.noPen()
            painter.setBrush(watchInk(hue, 0.9, 0.7, fade * appear))
            painter.drawEllipse(at, 1.4, 1.4)
            painter.setPen(QtPen(watchInk(hue, 0.8, 0.65, fade * appear), 1.0))
            painter.drawLine(at + CGPoint(row, 0), at + CGPoint(row * 4, 0))
            if appear >= 1.0 && k < rows - 1 {
                painter.setPen(QtPen(lit, 1.3))
                painter.drawLine(at + CGPoint(row * 0.6, -row * 0.35), at + CGPoint(row * 4.4, row * 0.35))
            }
        }
    case .fumble:
        // An error: a ring that cracks into shaking shards.
        let shake: Double = sin(t * 60.0) * 2.0 * (1.0 - t)
        let radius: Double = r * (1.2 + 0.9 * watchEaseOut(t))
        let shards = 5
        painter.setPen(pen)
        painter.noBrush()
        for k in 0..<shards {
            let sign: Double = k % 2 != 0 ? 1 : -1
            let start: Double = Double(k) * 360.0 / Double(shards) + t * 25.0 * sign
            let crack: Double = (Double(k) + 0.5) * 2 * Double.pi / Double(shards)
            let centre = c + CGPoint(shake, 0) + watchPolar(.zero, r * 0.3 * t, crack)
            let box = watchBox(Double(centre.x) - radius, Double(centre.y) - radius, radius * 2, radius * 2)
            let span: Double = -(360.0 / Double(shards) - 12) * 16
            painter.drawArc(box, Int(-start * 16), Int(span))
        }
    case .denied:
        // Refused: a no-entry sign flaring over the body.
        let radius: Double = r * (1.15 + 0.8 * watchEaseOut(t))
        painter.setPen(pen.withWidth(width * 1.6))
        painter.setBrush(watchInk(hue, 0.9, 0.45, fade * 0.15))
        painter.drawEllipse(c, radius, radius)
        painter.drawLine(
            watchPolar(c, radius * 0.72, -Double.pi * 0.75), watchPolar(c, radius * 0.72, Double.pi * 0.25))
    case .image:
        // A picture coming back: a framed landscape falling in to the body.
        let fall: Double = watchEaseInOut(t)
        let at = c + dir * (r * (3.0 - 1.8 * fall))
        let w: Double = 7.0 + 5.0 * s
        let frame = watchBox(Double(at.x) - w, Double(at.y) - w * 0.7, w * 2, w * 1.4)
        let left = Double(frame.minX)
        let top = Double(frame.minY)
        let right = Double(frame.maxX)
        let bottom = Double(frame.maxY)
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.6, 0.3, fade * 0.6))
        painter.drawRect(frame)
        painter.noPen()
        painter.setBrush(lit)
        painter.drawPolygon([
            CGPoint(left, bottom),
            CGPoint(left + w * 0.7, top + w * 0.6),
            CGPoint(left + w * 1.1, bottom - w * 0.4),
            CGPoint(left + w * 1.5, top + w * 0.8),
            CGPoint(right, bottom),
        ])
        painter.drawEllipse(CGPoint(right - w * 0.4, top + w * 0.35), 1.5, 1.5)
    case .compact:
        // Folding down: rings closing in on the body from far out.
        painter.noBrush()
        for k in 0..<3 {
            let tk: Double = min(max(t * 1.4 - Double(k) * 0.2, 0.0), 1.0)
            if tk <= 0 || tk >= 1 {
                continue
            }
            painter.setPen(pen.withColour(watchInk(hue, 0.9, 0.7, fade * (0.3 + 0.7 * tk))))
            let radius: Double = r * (4.0 - 3.0 * watchEaseInOut(tk))
            painter.drawEllipse(c, radius, radius * (1.0 - 0.3 * tk))
        }
    case .retry:
        // Again: an arrow chasing round the body.
        let radius: Double = r * 1.5
        let start: Double = a * 180.0 / Double.pi + t * 540.0
        let box = watchBox(cx - radius, cy - radius, radius * 2, radius * 2)
        painter.setPen(pen)
        painter.noBrush()
        painter.drawArc(box, Int(-start * 16), -220 * 16)
        let tipAngle: Double = (start + 220.0) * Double.pi / 180.0
        let tip = watchPolar(c, radius, tipAngle)
        let back = watchPolar(c, radius, tipAngle - 0.3)
        let side = (tip - back) * 0.8
        let barb = CGPoint(-Double(side.y), Double(side.x))
        painter.drawLine(tip, back + barb)
        painter.drawLine(tip, back - barb)
    case .fail:
        // A failed turn: a cross struck through the body, and sparks raining
        // down off it.
        let grow: Double = watchEaseOut(min(1.0, t * 2.5))
        let arm: Double = r * (1.0 + 0.8 * grow)
        painter.setPen(pen.withWidth(width * 1.8))
        painter.drawLine(c + CGPoint(-arm, -arm) * 0.7, c + CGPoint(arm, arm) * 0.7)
        painter.drawLine(c + CGPoint(arm, -arm) * 0.7, c + CGPoint(-arm, arm) * 0.7)
        painter.noPen()
        let drops = 8
        for i in 0..<drops {
            let x: Double = (Double(i) / Double(drops - 1) - 0.5) * r * 3.0
            let y: Double = r + t * t * r * (2.5 + watchScatter(i * 7) * 2.0)
            painter.setBrush(watchInk(hue, 1.0, 0.65, fade))
            painter.drawEllipse(c + CGPoint(x, y), 1.4 + s, 1.4 + s)
        }
    case .close:
        // A block closing: two brackets snapping shut onto a bar.
        let at = watchPolar(c, r * 1.35, a)
        let half: Double = (3.0 + 2.0 * s) * (1.0 - 0.3 * t)
        let gap: Double = half * (0.4 + 1.0 * (1.0 - watchEaseOut(t)))
        painter.save()
        painter.translate(at)
        painter.rotate(a * 180.0 / Double.pi)
        painter.setPen(QtPen(lit, 1.1, .solid, .square, .miter))
        painter.noBrush()
        for side: Double in [-1.0, 1.0] {
            let outer: Double = side * (gap + half * 0.4)
            let inner: Double = side * gap
            painter.drawPolyline([
                CGPoint(outer, -half),
                CGPoint(inner, -half),
                CGPoint(inner, half),
                CGPoint(outer, half),
            ])
        }
        painter.drawLine(CGPoint(0, -half * 0.7), CGPoint(0, half * 0.7))
        painter.restore()
    case .seal:
        // A seal pressed just off the body, a glint crossing it.
        let at = watchPolar(c, r * 1.45, a)
        let size: Double = (2.4 + 1.8 * s) * (1.0 + 0.3 * watchEaseOut(t))
        painter.noPen()
        painter.setBrush(watchInk(hue, 0.9, 0.55, fade))
        painter.drawEllipse(at, size, size)
        painter.setPen(QtPen(watchInk(hue, 0.4, 0.95, fade), 1.0))
        painter.noBrush()
        painter.drawEllipse(at, size * 1.5, size * 1.5)
        let glint: Double = -size + 2.0 * size * t
        painter.drawLine(at + CGPoint(glint - 1.5, -size), at + CGPoint(glint + 1.5, size))
    case .connect, .task, .logError, .logStall, .logSync, .logGit, .logCloud, .logTask, .logAccount, .logInfo:
        // Its glyph thrown off the body - outwards, or falling in for the kinds
        // that report something coming back - and a thin ring behind it, so a
        // log line reads as a line in the log and not an agent's work.
        let travel: Double = motion.inbound ? 1.0 - 0.6 * watchEaseOut(t) : watchEaseOut(t)
        let at = watchPolar(c, r * (1.3 + 0.8 * travel), a)
        let size: Double = (3.2 + 2.4 * s) * (1.0 + 0.25 * watchEaseOut(t))
        paintCueGlyph(&painter, motion, at, size, lit, t)
        painter.setPen(QtPen(line, max(0.8, width * 0.6)))
        painter.noBrush()
        let ring: Double = r * (1.0 + (motion.reach - 1.0) * watchEaseOut(t))
        painter.drawEllipse(c, ring, ring)
    case .hook:
        // A hook swinging off the body on its line.
        let swing: Double = sin(t * 2.0 * Double.pi * 1.5) * 0.35 * (1.0 - t)
        let pivot = watchPolar(c, r * 1.1, a)
        let end = watchPolar(pivot, r * 0.9, a + swing)
        painter.setPen(QtPen(lit, 1.2, .solid, .round))
        painter.noBrush()
        painter.drawLine(pivot, end)
        let size: Double = 2.5 + 1.5 * s
        let box = watchBox(Double(end.x) - size, Double(end.y) - size, size * 2, size * 2)
        let start: Double = (-(a + swing) * 180.0 / Double.pi + 90) * 16
        painter.drawArc(box, Int(start), 200 * 16)
    }
}

/// A kind's icon on the sound list and the hover card: centred on `c`,
/// `size` its half-width, at `alpha`.
func paintWatchIcon(_ ctx: GraphicsContext, _ motion: AgentMotion, _ c: CGPoint, _ size: Double, _ alpha: Double) {
    var painter = QtPainter(ctx)
    let hue = motion.hue
    let colour = watchInk(hue, 0.92, 0.68, alpha)
    let pen = QtPen(colour, max(1.0, size * 0.28), .solid, .round, .round)
    let cx = Double(c.x)
    let cy = Double(c.y)
    painter.save()
    switch motion {
    case .start:
        painter.noPen()
        painter.setBrush(colour)
        for i in 0..<5 {
            let at: Double = -Double.pi / 2 + Double(i) * 2 * Double.pi / 5
            painter.drawEllipse(watchPolar(c, size * 0.62, at), size * 0.34, size * 0.34)
        }
    case .speak:
        painter.setPen(pen)
        painter.noBrush()
        for k in 0..<2 {
            let radius: Double = size * (0.55 + 0.55 * Double(k))
            let box = watchBox(cx - size - radius, cy - radius, radius * 2, radius * 2)
            painter.drawArc(box.offsetBy(dx: size * 0.5, dy: 0), -40 * 16, 80 * 16)
        }
    case .think:
        painter.noPen()
        painter.setBrush(colour)
        for i in 0..<3 {
            let at: Double = -Double.pi / 2 + Double(i) * 2 * Double.pi / 3
            painter.drawEllipse(watchPolar(c, size * 0.6, at), size * 0.3, size * 0.3)
        }
    case .read:
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.55, alpha * 0.4))
        painter.drawRect(watchBox(cx - size * 0.6, cy - size * 0.8, size * 1.2, size * 1.6))
    case .search:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c + CGPoint(-size * 0.2, -size * 0.2), size * 0.6, size * 0.6)
        painter.drawLine(c + CGPoint(size * 0.25, size * 0.25), c + CGPoint(size * 0.9, size * 0.9))
    case .web:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c, size * 0.85, size * 0.85)
        painter.drawEllipse(c, size * 0.38, size * 0.85)
        painter.drawLine(c + CGPoint(-size * 0.85, 0), c + CGPoint(size * 0.85, 0))
    case .plan:
        painter.setPen(pen.withWidth(max(1.0, size * 0.2)))
        painter.noBrush()
        for k in -1...1 {
            let y: Double = cy + Double(k) * size * 0.62
            painter.drawLine(CGPoint(cx - size * 0.8, y), CGPoint(cx - size * 0.45, y + size * 0.2))
            painter.drawLine(CGPoint(cx - size * 0.45, y + size * 0.2), CGPoint(cx - size * 0.1, y - size * 0.25))
            painter.drawLine(CGPoint(cx + size * 0.15, y), CGPoint(cx + size * 0.9, y))
        }
    case .write:
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.55, alpha * 0.4))
        painter.drawPolygon([
            c + CGPoint(0, -size),
            c + CGPoint(size * 0.8, 0),
            c + CGPoint(0, size),
            c + CGPoint(-size * 0.8, 0),
        ])
    case .run:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawPolyline([
            c + CGPoint(size * 0.3, -size),
            c + CGPoint(-size * 0.35, size * 0.1),
            c + CGPoint(size * 0.35, -size * 0.1),
            c + CGPoint(-size * 0.3, size),
        ])
    case .delegate:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c + CGPoint(-size * 0.35, size * 0.2), size * 0.5, size * 0.5)
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c + CGPoint(size * 0.6, -size * 0.55), size * 0.3, size * 0.3)
    case .arrive:
        painter.setPen(pen)
        painter.noBrush()
        for i in 0..<4 {
            let at: Double = Double.pi / 4 + Double(i) * Double.pi / 2
            painter.drawLine(watchPolar(c, size, at), watchPolar(c, size * 0.45, at))
        }
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c, size * 0.25, size * 0.25)
    case .prompt:
        painter.setPen(pen)
        painter.drawLine(c + CGPoint(-size * 0.7, -size), c + CGPoint(size * 0.1, size * 0.2))
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c + CGPoint(size * 0.2, size * 0.35), size * 0.45, size * 0.45)
    case .notice:
        painter.setPen(pen)
        painter.setBrush(watchInk(hue, 0.9, 0.5, alpha * 0.3))
        var triangle: [CGPoint] = []
        for i in 0..<3 {
            let at: Double = -Double.pi / 2 + Double(i) * 2 * Double.pi / 3
            triangle.append(watchPolar(c, size, at))
        }
        painter.drawPolygon(triangle)
    case .finish:
        painter.setPen(pen)
        for i in 0..<8 {
            let at: Double = Double(i) * Double.pi / 4
            painter.drawLine(watchPolar(c, size * 0.35, at), watchPolar(c, size, at))
        }
    case .frame:
        painter.setPen(pen)
        painter.noBrush()
        for side: Double in [-1.0, 1.0] {
            let inner: Double = side * size * 0.3
            let outer: Double = side * size * 0.7
            painter.drawPolyline([
                c + CGPoint(inner, -size * 0.8),
                c + CGPoint(outer, -size * 0.8),
                c + CGPoint(outer, size * 0.8),
                c + CGPoint(inner, size * 0.8),
            ])
        }
    case .compose:
        painter.noPen()
        painter.setBrush(colour)
        for k in -1...1 {
            painter.drawEllipse(c + CGPoint(Double(k) * size * 0.6, 0), size * 0.22, size * 0.22)
        }
    case .usage:
        painter.noPen()
        painter.setBrush(colour)
        for k in 0..<3 {
            let dk = Double(k)
            let height: Double = size * (0.6 + 0.5 * dk)
            painter.drawRect(watchBox(cx - size * 0.8 + dk * size * 0.6, cy + size * 0.8 - height, size * 0.4, height))
        }
    case .system:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c, size * 0.45, size * 0.45)
        for i in 0..<6 {
            let at: Double = Double(i) * Double.pi / 3.0
            painter.drawLine(watchPolar(c, size * 0.5, at), watchPolar(c, size * 0.9, at))
        }
    case .tick:
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c, size * 0.3, size * 0.3)
    case .network:
        painter.setPen(pen)
        painter.drawLine(c + CGPoint(-size * 0.4, -size * 0.7), c + CGPoint(-size * 0.4, size * 0.7))
        painter.drawLine(c + CGPoint(-size * 0.75, -size * 0.3), c + CGPoint(-size * 0.4, -size * 0.7))
        painter.drawLine(c + CGPoint(size * 0.4, -size * 0.7), c + CGPoint(size * 0.4, size * 0.7))
        painter.drawLine(c + CGPoint(size * 0.75, size * 0.3), c + CGPoint(size * 0.4, size * 0.7))
    case .scan:
        painter.noPen()
        painter.setBrush(colour)
        for k in -2...2 {
            let at: Double = -Double.pi / 2 + Double(k) * 0.4
            painter.drawEllipse(watchPolar(c + CGPoint(0, size * 0.6), size, at), size * 0.18, size * 0.18)
        }
    case .create, .skill:
        let points = motion == .create ? 4 : 5
        var star = Path()
        for i in 0..<(points * 2) {
            let at: Double = -Double.pi / 2 + Double(i) * Double.pi / Double(points)
            let p = watchPolar(c, i % 2 != 0 ? size * 0.4 : size, at)
            if i == 0 {
                star.move(to: p)
            } else {
                star.addLine(to: p)
            }
        }
        star.closeSubpath()
        painter.noPen()
        painter.setBrush(colour)
        painter.drawPath(star)
    case .test:
        painter.setPen(pen)
        painter.drawPolyline([
            c + CGPoint(-size * 0.7, 0),
            c + CGPoint(-size * 0.2, size * 0.5),
            c + CGPoint(size * 0.8, -size * 0.6),
        ])
    case .build:
        painter.noPen()
        painter.setBrush(colour)
        for k in 0..<3 {
            let left: Double = cx - size * 0.8 + Double(k % 2) * size * 0.3
            let top: Double = cy + size * 0.5 - Double(k) * size * 0.55
            painter.drawRect(watchBox(left, top, size * 1.3, size * 0.42))
        }
    case .git:
        painter.setPen(pen)
        painter.drawLine(c + CGPoint(-size * 0.4, size), c + CGPoint(-size * 0.4, -size))
        painter.drawLine(c + CGPoint(-size * 0.4, size * 0.2), c + CGPoint(size * 0.5, -size * 0.5))
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c + CGPoint(size * 0.5, -size * 0.5), size * 0.28, size * 0.28)
        painter.drawEllipse(c + CGPoint(-size * 0.4, -size), size * 0.28, size * 0.28)
    case .install:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawRect(watchBox(cx - size * 0.7, cy - size * 0.3, size * 1.4, size * 1.1))
        painter.drawLine(c + CGPoint(0, -size), c + CGPoint(0, size * 0.3))
    case .spawn:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c + CGPoint(-size * 0.3, size * 0.3), size * 0.45, size * 0.45)
        painter.drawArc(watchBox(cx - size, cy - size, size * 2, size * 2), 10 * 16, 80 * 16)
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c + CGPoint(size * 0.7, -size * 0.7), size * 0.25, size * 0.25)
    case .wait:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c, size * 0.85, size * 0.85)
        painter.drawLine(c, c + CGPoint(0, -size * 0.6))
        painter.drawLine(c, c + CGPoint(size * 0.45, 0))
    case .ask:
        painter.setPen(QtPen(colour))
        painter.drawText(watchBox(cx - size, cy - size * 1.2, size * 2, size * 2.4), "?", Int(size * 2.2))
    case .mcp:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawRect(watchBox(cx - size * 0.3, cy - size * 0.5, size * 1.1, size))
        painter.drawLine(c + CGPoint(-size * 0.3, -size * 0.25), c + CGPoint(-size, -size * 0.25))
        painter.drawLine(c + CGPoint(-size * 0.3, size * 0.25), c + CGPoint(-size, size * 0.25))
    case .todo:
        painter.setPen(pen)
        for k in -1...1 {
            let y: Double = cy + Double(k) * size * 0.62
            painter.drawLine(CGPoint(cx - size * 0.4, y), CGPoint(cx + size * 0.9, y))
            painter.drawPoint(CGPoint(cx - size * 0.85, y))
        }
    case .fumble:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawArc(watchBox(cx - size * 0.8, cy - size * 0.8, size * 1.6, size * 1.6), 20 * 16, 140 * 16)
        painter.drawArc(watchBox(cx - size * 0.7, cy - size * 0.7, size * 1.6, size * 1.6), 200 * 16, 130 * 16)
    case .denied:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c, size * 0.85, size * 0.85)
        painter.drawLine(watchPolar(c, size * 0.6, -Double.pi * 0.75), watchPolar(c, size * 0.6, Double.pi * 0.25))
    case .image:
        painter.setPen(pen)
        painter.noBrush()
        let frame = watchBox(cx - size * 0.9, cy - size * 0.7, size * 1.8, size * 1.4)
        painter.drawRect(frame)
        painter.drawPolyline([
            CGPoint(x: frame.minX, y: frame.maxY),
            CGPoint(cx - size * 0.2, cy - size * 0.1),
            CGPoint(x: frame.maxX, y: frame.maxY),
        ])
    case .compact:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawLine(c + CGPoint(-size, -size * 0.6), c + CGPoint(-size * 0.2, 0))
        painter.drawLine(c + CGPoint(-size, size * 0.6), c + CGPoint(-size * 0.2, 0))
        painter.drawLine(c + CGPoint(size, -size * 0.6), c + CGPoint(size * 0.2, 0))
        painter.drawLine(c + CGPoint(size, size * 0.6), c + CGPoint(size * 0.2, 0))
    case .retry:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawArc(watchBox(cx - size * 0.8, cy - size * 0.8, size * 1.6, size * 1.6), 60 * 16, 270 * 16)
        let head = watchPolar(c, size * 0.8, -Double.pi / 3)
        painter.drawLine(head, head + CGPoint(size * 0.4, 0))
    case .fail:
        painter.setPen(pen)
        painter.drawLine(c + CGPoint(-size * 0.7, -size * 0.7), c + CGPoint(size * 0.7, size * 0.7))
        painter.drawLine(c + CGPoint(size * 0.7, -size * 0.7), c + CGPoint(-size * 0.7, size * 0.7))
    case .close:
        painter.setPen(pen)
        painter.noBrush()
        for side: Double in [-1.0, 1.0] {
            let outer: Double = side * size * 0.8
            let inner: Double = side * size * 0.4
            painter.drawPolyline([
                c + CGPoint(outer, -size * 0.8),
                c + CGPoint(inner, -size * 0.8),
                c + CGPoint(inner, size * 0.8),
                c + CGPoint(outer, size * 0.8),
            ])
        }
        painter.drawLine(c + CGPoint(0, -size * 0.5), c + CGPoint(0, size * 0.5))
    case .seal:
        painter.noPen()
        painter.setBrush(colour)
        painter.drawEllipse(c, size * 0.55, size * 0.55)
        painter.setPen(pen)
        painter.noBrush()
        painter.drawEllipse(c, size * 0.9, size * 0.9)
    case .connect, .task, .logError, .logStall, .logSync, .logGit, .logCloud, .logTask, .logAccount, .logInfo:
        paintCueGlyph(&painter, motion, c, size, colour, 0.0)
    case .hook:
        painter.setPen(pen)
        painter.noBrush()
        painter.drawLine(c + CGPoint(size * 0.3, -size), c + CGPoint(size * 0.3, size * 0.2))
        painter.drawArc(watchBox(cx - size * 0.5, cy - size * 0.2, size * 0.8, size * 0.9), 180 * 16, 180 * 16)
    }
    painter.restore()
}
