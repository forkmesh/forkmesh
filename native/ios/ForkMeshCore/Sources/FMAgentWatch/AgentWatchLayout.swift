// The Watch field's geometry and words (`WatchFieldLayout`, `watchCardLines`,
// `watchFaceAsset`): the parts that are not painting, kept here so they are
// tested on Linux and shared by the phone's field and the watch's reduced one.

import FMBase
import Foundation
#if canImport(CoreGraphics)
// On Apple platforms CGRect's minX/width/midX live in CoreGraphics; Linux
// Foundation carries them itself, which is why this compiled there alone.
import CoreGraphics
#endif

// The words circling a body start this far outside it (kLabelGap), clear of
// its running ring and its marks; kLabelPixels is their size.
public let kWatchLabelGap: Double = 19.0
public let kWatchLabelPixels: Double = 11.0
// The display under a body (kLedPitch, kLedPad, kLedHold, kLedSpeed,
// kLedGapDots, kLedMaxWidth); ten rows, as the desktop's 8 px fixed font.
public let kWatchLedPitch: Double = 2.5
public let kWatchLedPad: Double = 3.0
public let kWatchLedHold: Double = 0.6
public let kWatchLedSpeed: Double = 150.0
public let kWatchLedGapDots: Int = 14
public let kWatchLedMaxWidth: Double = 220.0
public let kWatchLedRows: Int = 10
public let kWatchLedHeight: Double = Double(kWatchLedRows) * kWatchLedPitch + 2 * kWatchLedPad
let ledRoom: Double = kWatchLedHeight + 2.0
// The phone draws no hosts along the top, so its rows start near it; the
// bottom leaves the desktop's node its room, as on the desktop.
let rowsTop = 0.06
let rowsBottom = 0.80
public let kWatchBandHeader: Double = 0.035
// The least height a row of bodies wants on a phone: the body, its words
// circling it and its display under them.
let rowPixels = 150.0
let packedTopPixels = 6.0
let packedHeaderPixels = 24.0
let packedNodePixels = 96.0
public let kWatchGlideSeconds: Double = 0.35

/// One row of agents in one band (`BandRow`).
public struct WatchRow: Equatable, Sendable {
    public let band: WatchBand
    /// Its centre, in field units (0..1 down the field).
    public let y: Double
    /// Its height, in field units.
    public let height: Double
    /// How many agents the band holds, on its first row; 0 on the rest.
    public let count: Int
}

/// Where everything sits on a field of one size: the rows, each agent's seat
/// and the room its words and display take (`Scene::relayout` with the view's
/// `layoutField`, `pointFor` and `radiusFor`).
@MainActor
public struct WatchFieldLayout {
    public let size: CGSize
    public let field: CGRect
    public let unit: Double
    public let bandHeader: Double
    public private(set) var rows: [WatchRow] = []

    /// Seat (x, y) in field units, and the width of its place in its row.
    private var seats: [ObjectIdentifier: (Double, Double, Double)] = [:]
    private var seated: [WatchedAgent] = []

    public init(_ size: CGSize, _ agents: [WatchedAgent], packed: Bool? = nil) {
        self.size = size
        let margin = Self.marginFor(Double(size.width))
        let fieldWidth: Double = max(60.0, Double(size.width) - 2 * margin)
        let fieldHeight: Double = max(60.0, Double(size.height) - 28)
        field = CGRect(x: margin, y: 14, width: fieldWidth, height: fieldHeight)
        unit = min(fieldWidth, fieldHeight)
        let packed = packed ?? Self.packsFor(Double(size.width))
        let top = packed ? packedTopPixels / fieldHeight : rowsTop
        bandHeader = packed ? packedHeaderPixels / fieldHeight : kWatchBandHeader
        let rowSize = Self.rowSizeFor(Double(size.width))
        var banded: [[WatchedAgent]] = Array(repeating: [], count: WatchBand.allCases.count)
        for agent in agents {
            banded[bandFor(agent.state).rawValue].append(agent)
        }
        var rowCount = 0
        var bands = 0
        for b in banded.indices {
            banded[b].sort { a, b in
                if a.rank != b.rank { return a.rank < b.rank }
                let ax = min(max(a.voice.pan, 0.0), 1.0)
                let bx = min(max(b.voice.pan, 0.0), 1.0)
                return ax != bx ? ax < bx : a.id < b.id
            }
            rowCount += (banded[b].count + rowSize - 1) / rowSize
            if !banded[b].isEmpty { bands += 1 }
        }
        if rowCount == 0 { return }
        let step = packed
            ? max(0.01, min(rowPixels / fieldHeight, (1 - packedNodePixels / fieldHeight - top - Double(bands) * bandHeader) / Double(rowCount)))
            : min(0.26, (rowsBottom - rowsTop - Double(bands) * kWatchBandHeader) / Double(rowCount))
        var y = top + step / 2
        for (b, band) in banded.enumerated() {
            if !band.isEmpty { y += bandHeader }
            var first = 0
            while first < band.count {
                let count = min(rowSize, band.count - first)
                rows.append(WatchRow(
                    band: WatchBand(rawValue: b)!, y: y, height: step, count: first == 0 ? band.count : 0))
                // Spread evenly across the row, so every body has the same room.
                let slot = 0.88 / Double(count)
                for i in 0..<count {
                    let agent = band[first + i]
                    seats[ObjectIdentifier(agent)] = (0.06 + slot * (Double(i) + 0.5), y, slot)
                    seated.append(agent)
                }
                y += step
                first += rowSize
            }
        }
    }

    /// How many bodies a row holds at this width: the desktop's eight on a
    /// wide window, fewer on a phone so each keeps room for its words.
    public static func rowSizeFor(_ width: Double) -> Int {
        min(max(Int(((width - 2 * marginFor(width)) / 110).rounded(.down)), 1), 8)
    }

    public static func marginFor(_ width: Double) -> Double {
        width < 420 ? 10 : 26
    }

    public static func packsFor(_ width: Double) -> Bool {
        width < 700
    }

    /// How tall a field of this width wants to be for these agents, so each
    /// row keeps its room; a phone scrolls the rest.
    public static func heightFor(_ width: Double, _ agents: [WatchedAgent], packed: Bool? = nil) -> Double {
        let rowSize = rowSizeFor(width)
        var counts = Array(repeating: 0, count: WatchBand.allCases.count)
        for agent in agents { counts[bandFor(agent.state).rawValue] += 1 }
        var rows = 0
        var bands = 0
        for count in counts {
            rows += (count + rowSize - 1) / rowSize
            if count > 0 { bands += 1 }
        }
        if packed ?? packsFor(width) {
            return packedTopPixels + Double(bands) * packedHeaderPixels + Double(rows) * rowPixels + packedNodePixels + 28
        }
        let room = rowsBottom - rowsTop - Double(bands) * kWatchBandHeader
        return Double(rows) * rowPixels / room + 28
    }

    /// The agent's seat, or the middle when it has none.
    public func pointFor(_ agent: WatchedAgent) -> CGPoint {
        if let drawn = agent.drawn { return CGPoint(x: drawn.x, y: drawn.y) }
        return seatFor(agent)
    }

    public func seatFor(_ agent: WatchedAgent) -> CGPoint {
        let seat = seats[ObjectIdentifier(agent)] ?? (0.5, 0.5, 0.2)
        let x: Double = Double(field.minX) + seat.0 * Double(field.width)
        let top: Double = Double(field.minY) + seat.1 * Double(field.height)
        return CGPoint(x: x, y: top - ledRoom / 2)
    }

    @discardableResult
    public func glide(_ now: Double) -> Bool {
        var moving = false
        for agent in seated {
            let seat = seatFor(agent)
            let sx = Double(seat.x)
            let sy = Double(seat.y)
            guard let drawn = agent.drawn else {
                agent.drawn = (sx, sy, now)
                continue
            }
            let step = min(max(now - drawn.at, 0.0), 0.1)
            let k = 1.0 - exp(-step / kWatchGlideSeconds)
            var x = drawn.x + (sx - drawn.x) * k
            var y = drawn.y + (sy - drawn.y) * k
            if abs(sx - x) + abs(sy - y) < 0.5 {
                x = sx
                y = sy
            } else {
                moving = true
            }
            agent.drawn = (x, y, now)
        }
        return moving
    }

    /// How big its body is drawn now: tokens and energy, capped by its row.
    public func radiusFor(_ agent: WatchedAgent, _ now: Double) -> Double {
        let scale = unit / 520.0
        let base = 9.0 + 17.0 * agent.size + 9.0 * agent.energyAt(now)
        var most = 64.0
        if let first = rows.first {
            let half: Double = first.height * Double(field.height) / 2
            let words: Double = kWatchLabelGap + kWatchLabelPixels + 3.0 + ledRoom / 2
            most = max(8.0, half - words)
        }
        return min(max(base * scale, 5.0), most)
    }

    /// The desktop they run on, at the foot of the field.
    public var desktopNode: CGPoint {
        let lift: Double = 34.0 * max(0.8, unit / 520.0)
        return CGPoint(x: Double(field.midX), y: Double(field.maxY) - lift)
    }

    /// The agent under a point, within reach of its body, or nil.
    public func agentAt(_ point: CGPoint, _ now: Double, shown: ((WatchedAgent) -> Bool)? = nil) -> WatchedAgent? {
        var best: WatchedAgent?
        var bestDistance = Double.infinity
        for agent in seated {
            if let shown, !shown(agent) { continue }
            let at = pointFor(agent)
            let dx: Double = Double(at.x) - Double(point.x)
            let dy: Double = Double(at.y) - Double(point.y)
            let distance: Double = (dx * dx + dy * dy).squareRoot()
            let reach = radiusFor(agent, now) + kWatchLabelGap + 4
            if distance <= reach && distance < bestDistance {
                best = agent
                bestDistance = distance
            }
        }
        return best
    }

    /// The display under a body.
    public func ledRect(_ agent: WatchedAgent, _ centre: CGPoint, _ radius: Double) -> CGRect {
        let slot = seats[ObjectIdentifier(agent)]?.2 ?? 0.2
        let room = min(max(slot * Double(field.width) - 12, 64.0), kWatchLedMaxWidth)
        let dots: Double = ((room - 2 * kWatchLedPad) / kWatchLedPitch).rounded(.down)
        let width: Double = dots * kWatchLedPitch + 2 * kWatchLedPad
        let x: Double = (Double(centre.x) - width / 2).rounded()
        let below: Double = radius + kWatchLabelGap + kWatchLabelPixels + 2
        let y: Double = (Double(centre.y) + below).rounded()
        return CGRect(x: x, y: y, width: width, height: kWatchLedHeight)
    }
}

/// The face the desktop draws in an agent's body: its model's portrait, or
/// its runtime's mark (`agentModelFaceIconIndex`). One of: opus, sonnet,
/// haiku, fable, sol, luna, terra, feather, crescent-moon, sun, globe-leaf.
public func watchFaceName(provider: String, model: String) -> String {
    let m = model.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    let p = provider.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    for face in ["opus", "sonnet", "haiku", "fable", "sol", "luna", "terra"] where m.contains(face) {
        return face
    }
    if p.hasPrefix("claude") || m.hasPrefix("claude") { return "feather" }
    if m.contains("mini") || m.contains("nano") { return "crescent-moon" }
    if p == "codex" || m.contains("codex") || p == "openai" { return "sun" }
    return "globe-leaf"
}

/// A name as a body carries it: without the "#N" its tag already shows.
public func watchTitleWithoutNumber(_ id: Int, _ text: String) -> String {
    var title = collapseWhitespace(text)
    let number = "#\(id)"
    if title.hasPrefix(number) { title = dartTrim(String(title.dropFirst(number.count))) }
    while title.hasPrefix("-") || title.hasPrefix(":") {
        title = dartTrim(String(title.dropFirst()))
    }
    return title
}

/// What the card on a tapped agent says (the body half of `hoverLines`).
@MainActor
public func watchCardLines(_ controller: AgentWatchController, _ agent: WatchedAgent) -> [String] {
    let project = agent.target.projectName
    let task = watchTitleWithoutNumber(agent.id, agent.target.title)
    let name = [project, task].filter { !$0.isEmpty }.joined(separator: " \u{00B7} ")
    var lines: [String] = [
        agent.id > 0 ? (name.isEmpty ? "#\(agent.id)" : "#\(agent.id) \(name)") : name,
    ]
    let states: [WatchState: String] = [
        .starting: "running", .running: "running", .waiting: "waiting", .done: "done", .failed: "failed",
    ]
    let heard = agent.listening && controller.sound
    lines.append(
        "\(states[agent.state] ?? "") \u{00B7} \(heard ? "playing as" : "would sound as") \(agent.voice.description)")
    if !agent.target.subtitle.isEmpty {
        lines.append("on: \(agent.target.subtitle)")
    }
    if agent.turns > 0 || agent.tokens > 0 || agent.events > 0 {
        lines.append("\(agent.turns) turns \u{00B7} \(agent.tokens / 1000)k tokens \u{00B7} \(agent.events) events")
    }
    func wrapped(_ label: String, _ text: String) {
        let words = text.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        let width = 64
        let maxLines = 4
        var line = label
        var used = 0
        for word in words {
            if line.utf16.count + word.utf16.count + 1 > width && line.utf16.count > label.utf16.count {
                used += 1
                if used == maxLines {
                    lines.append("\(line)\u{2026}")
                    return
                }
                lines.append(line)
                line = "    "
            }
            line += (line.hasSuffix(" ") ? "" : " ") + word
        }
        if !dartTrim(line).isEmpty { lines.append(line) }
    }
    if !agent.prompt.isEmpty { wrapped("prompt:", agent.prompt) }
    if !agent.lastPrompt.isEmpty && dartTrim(agent.lastPrompt) != dartTrim(agent.prompt) {
        wrapped("latest:", agent.lastPrompt)
    }
    if !agent.said.isEmpty { wrapped("said:", agent.said) }
    let doing = agent.last?.text ?? ""
    if !doing.isEmpty { lines.append("now: \(doing)") }
    if !agent.error.isEmpty { lines.append("error: \(agent.error)") }
    lines.append("size: tokens burned \u{00B7} glow: how busy it is now")
    var rings = "icons round it: its latest marks, as the list of sounds draws them, newest at the top"
    if agent.state == .running || agent.state == .starting {
        rings += " \u{00B7} outer ring: running"
    } else if agent.state == .waiting {
        rings += " \u{00B7} dotted ring: waiting on you"
    }
    lines.append(rings)
    lines.append(heard ? "double tap to stop hearing it" : "double tap to hear it")
    return lines
}
