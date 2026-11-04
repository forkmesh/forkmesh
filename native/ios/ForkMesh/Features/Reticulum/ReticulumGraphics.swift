import FMPackages
import SwiftUI

struct ReticulumColors {
    let dark: Bool

    private static let darkSlots: [UInt32] = [0xFF3987E5, 0xFFD95926, 0xFF199E70, 0xFFC98500, 0xFFD55181, 0xFF008300]
    private static let lightSlots: [UInt32] = [0xFF2A78D6, 0xFFEB6834, 0xFF1BAF7A, 0xFFEDA100, 0xFFE87BA4, 0xFF008300]

    func series(_ slot: Int) -> Color {
        let slots = dark ? Self.darkSlots : Self.lightSlots
        return Color(fmARGB: slots[min(max(slot, 0), slots.count - 1)])
    }

    func group(_ group: ReticulumMapGroup) -> Color {
        guard let slot = group.seriesSlot else { return Color(fmARGB: dark ? 0xFF6E7681 : 0xFF8C959F) }
        return series(slot)
    }

    var grid: Color { Color(fmARGB: dark ? 0xFF262C36 : 0xFFD8DEE4) }
    var border: Color { Color(fmARGB: dark ? 0xFF30363D : 0xFFD0D7DE) }
}

enum ReticulumMarker {
    static func path(_ group: ReticulumMapGroup, center: CGPoint, radius: CGFloat) -> Path {
        let x = center.x
        let y = center.y
        switch group {
        case .peer:
            return Path(ellipseIn: CGRect(x: x - radius, y: y - radius, width: radius * 2, height: radius * 2))
        case .node:
            let side = radius * 1.8
            return Path(roundedRect: CGRect(x: x - side / 2, y: y - side / 2, width: side, height: side), cornerRadius: 1.5)
        case .propagation:
            var path = Path()
            path.move(to: CGPoint(x: x, y: y - radius * 1.2))
            path.addLine(to: CGPoint(x: x + radius * 1.2, y: y))
            path.addLine(to: CGPoint(x: x, y: y + radius * 1.2))
            path.addLine(to: CGPoint(x: x - radius * 1.2, y: y))
            path.closeSubpath()
            return path
        case .transport:
            var path = Path()
            path.move(to: CGPoint(x: x, y: y - radius * 1.25))
            path.addLine(to: CGPoint(x: x + radius * 1.15, y: y + radius * 0.85))
            path.addLine(to: CGPoint(x: x - radius * 1.15, y: y + radius * 0.85))
            path.closeSubpath()
            return path
        case .other:
            let small = radius * 0.75
            return Path(ellipseIn: CGRect(x: x - small, y: y - small, width: small * 2, height: small * 2))
        }
    }
}

struct ReticulumMarkerIcon: View {
    @Environment(\.fmPalette) private var fm
    let group: ReticulumMapGroup
    var size: CGFloat = 12

    var body: some View {
        let colors = ReticulumColors(dark: fm.dark)
        Canvas { context, canvas in
            let path = ReticulumMarker.path(group, center: CGPoint(x: canvas.width / 2, y: canvas.height / 2), radius: size * 0.36)
            context.fill(path, with: .color(colors.group(group)))
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

struct ReticulumSparkline: View {
    @Environment(\.fmPalette) private var fm
    let values: [Double]

    var body: some View {
        let color = ReticulumColors(dark: fm.dark).series(0)
        Canvas { context, size in
            guard values.count > 1 else { return }
            let maximum = max(1e-9, values.max() ?? 0)
            var line = Path()
            for (index, value) in values.enumerated() {
                let point = CGPoint(x: size.width * CGFloat(index) / CGFloat(values.count - 1),
                                    y: size.height - size.height * CGFloat(value / maximum))
                if index == 0 { line.move(to: point) } else { line.addLine(to: point) }
            }
            var area = line
            area.addLine(to: CGPoint(x: size.width, y: size.height))
            area.addLine(to: CGPoint(x: 0, y: size.height))
            area.closeSubpath()
            context.fill(area, with: .color(color.opacity(0.16)))
            context.stroke(line, with: .color(color), style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
        }
        .accessibilityHidden(true)
    }
}

struct ReticulumStatTile: View {
    @Environment(\.fmPalette) private var fm
    let tile: ReticulumTile

    private var toneColor: Color {
        switch tile.tone {
        case 1: return fm.success
        case 2: return fm.warning
        case 3: return fm.danger
        default: return fm.textTertiary
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(spacing: 6) {
                if tile.tone != 0 {
                    Circle().fill(toneColor).frame(width: 8, height: 8)
                }
                Text(tile.label)
                    .fmText(.labelMedium, color: fm.textSecondary)
                    .lineLimit(1)
            }
            HStack(alignment: .bottom, spacing: FmSpace.x2) {
                Text(tile.value)
                    .fmText(.titleMedium, tabular: true)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 0)
                if tile.spark.count > 1 {
                    ReticulumSparkline(values: tile.spark)
                        .frame(width: 64, height: 26)
                }
            }
            Text(tile.detail)
                .fmText(.labelSmall, color: fm.textTertiary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .padding(FmSpace.x3)
        .frame(width: 176, alignment: .leading)
        .background(fm.bgRaised)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(tile.label))
        .accessibilityValue(Text(tile.detail.isEmpty ? tile.value : "\(tile.value), \(tile.detail)"))
        .accessibilityIdentifier("reticulum-tile-\(tile.id)")
    }
}

struct ReticulumTrafficChart: View {
    @Environment(\.fmPalette) private var fm
    let samples: [ReticulumTrafficSample]
    var title = "Traffic, last 3 minutes"
    @State private var hover: Int?

    private static let slots = ReticulumNode.historySeconds
    private static let insets = EdgeInsets(top: 8, leading: 58, bottom: 22, trailing: 8)

    private func plot(_ size: CGSize) -> CGRect {
        CGRect(x: Self.insets.leading, y: Self.insets.top,
               width: max(10, size.width - Self.insets.leading - Self.insets.trailing),
               height: max(10, size.height - Self.insets.top - Self.insets.bottom))
    }

    private func point(_ index: Int, _ value: Double, plot: CGRect, top: Double) -> CGPoint {
        let offset = Self.slots - samples.count
        let x = plot.minX + plot.width * CGFloat(offset + index) / CGFloat(Self.slots - 1)
        return CGPoint(x: x, y: plot.maxY - plot.height * CGFloat(value / top))
    }

    private func index(at x: CGFloat, plot: CGRect) -> Int? {
        guard !samples.isEmpty else { return nil }
        let offset = Self.slots - samples.count
        let slot = (x - plot.minX) / plot.width * CGFloat(Self.slots - 1)
        return min(max(Int(slot.rounded()) - offset, 0), samples.count - 1)
    }

    private var summary: String {
        let rx = samples.reduce(Int64(0)) { $0 + $1.rx }
        let tx = samples.reduce(Int64(0)) { $0 + $1.tx }
        return "Received \(ReticulumFormat.bytes(Double(rx))) and sent \(ReticulumFormat.bytes(Double(tx))) in the last \(samples.count) seconds"
    }

    var body: some View {
        let colors = ReticulumColors(dark: fm.dark)
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x3) {
                Text(title).fmText(.labelLarge)
                Spacer(minLength: 0)
                legend("Received", colors.series(0))
                legend("Sent", colors.series(1))
            }
            GeometryReader { geometry in
                let area = plot(geometry.size)
                Canvas { context, size in draw(context, size: size, plot: area, colors: colors) }
                    .contentShape(Rectangle())
                    .simultaneousGesture(DragGesture(minimumDistance: 0)
                        .onChanged { value in hover = index(at: value.location.x, plot: area) }
                        .onEnded { _ in hover = nil })
            }
            .frame(height: 170)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(title))
        .accessibilityValue(Text(summary))
    }

    private func legend(_ label: String, _ color: Color) -> some View {
        HStack(spacing: 6) {
            Capsule().fill(color).frame(width: 12, height: 2)
            Text(label).fmText(.labelSmall, color: fm.textSecondary)
        }
    }

    private func draw(_ context: GraphicsContext, size: CGSize, plot: CGRect, colors: ReticulumColors) {
        let top = ReticulumFormat.chartTop(samples)
        let label = Font.system(size: 10)
        for step in 0...4 {
            let y = plot.maxY - plot.height * CGFloat(step) / 4
            var line = Path()
            line.move(to: CGPoint(x: plot.minX, y: y))
            line.addLine(to: CGPoint(x: plot.maxX, y: y))
            context.stroke(line, with: .color(step == 0 ? colors.border : colors.grid), lineWidth: 1)
            let text = step == 0 ? "0" : ReticulumFormat.rate(top * Double(step) / 4)
            context.draw(Text(text).font(label).foregroundColor(fm.textTertiary), at: CGPoint(x: plot.minX - 6, y: y), anchor: .trailing)
        }
        let marks = ["3 min ago", "2 min", "1 min", "now"]
        for (position, mark) in marks.enumerated() {
            let x = plot.minX + plot.width * CGFloat(position) / 3
            let anchor: UnitPoint = position == 0 ? .topLeading : position == 3 ? .topTrailing : .top
            context.draw(Text(mark).font(label).foregroundColor(fm.textTertiary), at: CGPoint(x: x, y: plot.maxY + 4), anchor: anchor)
        }
        guard samples.count >= 2 else {
            context.draw(Text("Traffic shows here a few seconds after an interface comes online").font(.system(size: 11)).foregroundColor(fm.textTertiary),
                         in: plot.insetBy(dx: 12, dy: plot.height / 2 - 20))
            return
        }
        for series in [1, 0] {
            var line = Path()
            for (index, sample) in samples.enumerated() {
                let value = Double(series == 0 ? sample.rx : sample.tx)
                let next = point(index, value, plot: plot, top: top)
                if index == 0 { line.move(to: next) } else { line.addLine(to: next) }
            }
            var area = line
            area.addLine(to: point(samples.count - 1, 0, plot: plot, top: top))
            area.addLine(to: point(0, 0, plot: plot, top: top))
            area.closeSubpath()
            context.fill(area, with: .color(colors.series(series).opacity(0.12)))
            context.stroke(line, with: .color(colors.series(series)), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
        }
        guard let hover, hover < samples.count else { return }
        let sample = samples[hover]
        let rx = point(hover, Double(sample.rx), plot: plot, top: top)
        let tx = point(hover, Double(sample.tx), plot: plot, top: top)
        var cross = Path()
        cross.move(to: CGPoint(x: rx.x, y: plot.minY))
        cross.addLine(to: CGPoint(x: rx.x, y: plot.maxY))
        context.stroke(cross, with: .color(fm.textTertiary), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
        for (dot, color) in [(rx, colors.series(0)), (tx, colors.series(1))] {
            let circle = Path(ellipseIn: CGRect(x: dot.x - 4.5, y: dot.y - 4.5, width: 9, height: 9))
            context.fill(circle, with: .color(color))
            context.stroke(circle, with: .color(fm.bgRaised), lineWidth: 2)
        }
        let ago = samples.count - 1 - hover
        let lines = [ago == 0 ? "Now" : "\(ago) s ago", "Received \(ReticulumFormat.rate(Double(sample.rx)))",
                     "Sent \(ReticulumFormat.rate(Double(sample.tx)))", "\(sample.announces) announce\(sample.announces == 1 ? "" : "s")"]
        let width: CGFloat = 132
        let height = CGFloat(lines.count) * 15 + 10
        var x = rx.x + 10
        if x + width > size.width - 2 { x = rx.x - 10 - width }
        let box = CGRect(x: x, y: plot.minY + 2, width: width, height: height)
        let shape = Path(roundedRect: box, cornerRadius: 6)
        context.fill(shape, with: .color(fm.bgRaised))
        context.stroke(shape, with: .color(colors.border), lineWidth: 1)
        for (row, text) in lines.enumerated() {
            context.draw(Text(text).font(.system(size: 11)).foregroundColor(row == 0 ? fm.textTertiary : fm.textPrimary),
                         at: CGPoint(x: box.minX + 8, y: box.minY + 5 + CGFloat(row) * 15), anchor: .topLeading)
        }
    }
}

struct ReticulumNetworkMap: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let spokes: [ReticulumMapSpoke]
    let destinations: [ReticulumDestination]
    let selected: [UInt8]
    let filter: String
    let selfOnline: Bool
    let selfLabel: String
    let pulses: [[UInt8]: Double]
    let now: Double
    let onSelect: ([UInt8]) -> Void

    var body: some View {
        GeometryReader { geometry in
            let layout = ReticulumMapLayout.compute(width: geometry.size.width, height: geometry.size.height,
                                                    spokes: spokes, destinations: destinations)
            TimelineView(.animation(minimumInterval: 1.0 / 30, paused: pulses.isEmpty || reduceMotion)) { timeline in
                Canvas { context, _ in
                    draw(context, layout: layout, clock: timeline.date.timeIntervalSince1970)
                }
            }
            .contentShape(Rectangle())
            .onTapGesture(coordinateSpace: .local) { point in
                if let hash = layout.hash(at: ReticulumPoint(Double(point.x), Double(point.y))) {
                    FmHaptics.tap()
                    onSelect(hash)
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Reticulum network map"))
        .accessibilityValue(Text("\(destinations.count) destinations on \(spokes.count) interfaces"))
        .accessibilityIdentifier("reticulum-map")
    }

    private func vector(_ angle: Double, _ length: Double, from middle: ReticulumPoint) -> CGPoint {
        CGPoint(x: middle.x + cos(angle) * length, y: middle.y + sin(angle) * length)
    }

    private func draw(_ context: GraphicsContext, layout: ReticulumMapLayout, clock: Double) {
        let colors = ReticulumColors(dark: fm.dark)
        let middle = layout.center
        let center = CGPoint(x: middle.x, y: middle.y)
        let small = Font.system(size: 9)
        for ring in 1...max(1, layout.maxRing) {
            let radius = layout.ringRadius(ring)
            let circle = Path(ellipseIn: CGRect(x: middle.x - radius, y: middle.y - radius, width: radius * 2, height: radius * 2))
            context.stroke(circle, with: .color(colors.grid), lineWidth: 1)
            context.draw(Text(layout.ringLabel(ring)).font(small).foregroundColor(fm.textTertiary),
                         at: CGPoint(x: middle.x + 4, y: middle.y - radius - 2), anchor: .bottomLeading)
        }
        let outer = layout.ringRadius(layout.maxRing)
        let byId = Dictionary(spokes.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        for sector in layout.sectors {
            if layout.sectors.count > 1 {
                var divider = Path()
                divider.move(to: vector(sector.start, 14, from: middle))
                divider.addLine(to: vector(sector.start, outer + 8, from: middle))
                context.stroke(divider, with: .color(colors.grid), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
            }
            let spoke = byId[sector.id]
            let online = !sector.id.isEmpty && (spoke?.online ?? false)
            var trunk = Path()
            trunk.move(to: vector(sector.middle, 14, from: middle))
            trunk.addLine(to: vector(sector.middle, layout.ringRadius(1) * 0.92, from: middle))
            context.stroke(trunk, with: .color(online ? fm.accent : colors.border),
                           style: StrokeStyle(lineWidth: online ? 2.5 : 1.5, lineCap: .round))
            let name = sector.id.isEmpty ? "Earlier interfaces" : (spoke?.name ?? "")
            let anchorPoint = vector(sector.middle, outer + 16, from: middle)
            let cosine = cos(sector.middle)
            let anchor: UnitPoint = cosine > 0.35 ? .leading : cosine < -0.35 ? .trailing : .center
            let clamped = CGPoint(x: min(max(anchorPoint.x, 8), layout.width - 8), y: min(max(anchorPoint.y, 8), layout.height - 8))
            let label = Text(name.count > 22 ? String(name.prefix(21)) + "…" : name)
                .font(.system(size: 10, weight: .semibold))
                .foregroundColor(online ? fm.textPrimary : fm.textTertiary)
            context.draw(label, at: clamped, anchor: anchor)
        }
        if let point = layout.positions[selected] {
            var line = Path()
            line.move(to: center)
            line.addLine(to: CGPoint(x: point.x, y: point.y))
            context.stroke(line, with: .color(fm.accent), style: StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
        }
        let surface = fm.bgBase
        for record in destinations {
            guard let position = layout.positions[record.hash] else { continue }
            let point = CGPoint(x: position.x, y: position.y)
            let group = record.group
            let age = now - record.lastHeard
            var alpha = age < 600 ? 1.0 : age < 3600 ? 0.75 : 0.45
            if !ReticulumHub.matches(record, filter: filter) { alpha = 0.12 }
            let emphasised = record.hash == selected
            let marker = ReticulumMarker.path(group, center: point, radius: emphasised ? 6.5 : 4.5)
            context.fill(marker, with: .color(colors.group(group).opacity(alpha)))
            context.stroke(marker, with: .color(surface), lineWidth: 1.5)
            if emphasised {
                context.stroke(Path(ellipseIn: CGRect(x: point.x - 10, y: point.y - 10, width: 20, height: 20)), with: .color(fm.accent), lineWidth: 2)
            }
            if let started = pulses[record.hash] {
                let progress = (clock - started) / ReticulumHub.pulseSeconds
                if progress >= 0 && progress <= 1 {
                    let radius = 5 + 22 * progress
                    let ripple = Path(ellipseIn: CGRect(x: point.x - radius, y: point.y - radius, width: radius * 2, height: radius * 2))
                    context.stroke(ripple, with: .color(colors.group(group).opacity(0.8 * (1 - progress))), lineWidth: 2)
                }
            }
        }
        if let position = layout.positions[selected], let record = destinations.first(where: { $0.hash == selected }) {
            let name = record.displayName.isEmpty ? String(ReticulumWire.pretty(record.hash).prefix(12)) + "…>" : record.displayName
            let resolved = context.resolve(Text(name).font(.system(size: 11, weight: .semibold)).foregroundColor(fm.textPrimary))
            let measured = resolved.measure(in: CGSize(width: 220, height: 40))
            var box = CGRect(x: position.x + 12, y: position.y - 11, width: measured.width + 14, height: 22)
            if box.maxX > layout.width - 2 { box.origin.x = position.x - 12 - box.width }
            let shape = Path(roundedRect: box, cornerRadius: 5)
            context.fill(shape, with: .color(fm.bgRaised))
            context.stroke(shape, with: .color(colors.border), lineWidth: 1)
            context.draw(resolved, at: CGPoint(x: box.midX, y: box.midY), anchor: .center)
        }
        let selfRing = Path(ellipseIn: CGRect(x: center.x - 11, y: center.y - 11, width: 22, height: 22))
        context.fill(selfRing, with: .color(selfOnline ? fm.accent : fm.textTertiary))
        context.stroke(selfRing, with: .color(surface), lineWidth: 3)
        context.fill(Path(ellipseIn: CGRect(x: center.x - 4, y: center.y - 4, width: 8, height: 8)), with: .color(surface))
        context.draw(Text(selfLabel).font(.system(size: 10, weight: .semibold)).foregroundColor(fm.textPrimary),
                     at: CGPoint(x: center.x, y: center.y + 14), anchor: .top)
        if destinations.isEmpty {
            let hint = spokes.isEmpty ? "Add an interface to join a Reticulum network" : "Destinations appear here as their announces arrive"
            context.draw(Text(hint).font(.system(size: 12)).foregroundColor(fm.textTertiary),
                         in: CGRect(x: center.x - 140, y: center.y + 36, width: 280, height: 40))
        }
    }
}

struct ReticulumLegend: View {
    @Environment(\.fmPalette) private var fm
    let destinations: [ReticulumDestination]

    var body: some View {
        let counts = Dictionary(grouping: destinations, by: \.group).mapValues(\.count)
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), alignment: .leading)], alignment: .leading, spacing: FmSpace.x1) {
            ForEach(ReticulumMapGroup.allCases, id: \.self) { group in
                HStack(spacing: 6) {
                    ReticulumMarkerIcon(group: group, size: 12)
                    Text("\(group.label) \(counts[group] ?? 0)")
                        .fmText(.labelSmall, color: fm.textSecondary)
                }
            }
        }
    }
}
