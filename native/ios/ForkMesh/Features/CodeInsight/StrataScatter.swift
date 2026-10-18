import FMCodeInsight
import SwiftUI

struct StrataScatterView: View {
    let model: StrataModel
    let query: String
    @Binding var focus: StrataFocus?
    let open: (StrataFocus) -> Void
    let showFiles: (Int) -> Void

    @Environment(\.fmPalette) private var fm
    @State private var showList = false

    private var nodes: [InsightModuleNode] { model.report.graph.nodes }

    var body: some View {
        VStack(spacing: 0) {
            bar
            if nodes.isEmpty {
                FmEmptyState(icon: "scatter_plot", title: "No modules", message: "No modules with source files were found.")
            } else if showList {
                list
            } else {
                ZStack(alignment: .bottom) {
                    plot
                    if let focus, focus.module != nil {
                        StrataSelectionCard(model: model, focus: focus, drilled: -1, open: open, openFile: { (_: String, _: Int) in },
                                            showFiles: showFiles, clear: { self.focus = nil })
                            .padding(FmSpace.x3)
                    }
                }
            }
        }
    }

    private var bar: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x3) {
                HStack(spacing: FmSpace.x1) {
                    Text("Distance D").fmText(.labelSmall, color: fm.textPrimary)
                    Text("0 on the sequence").fmText(.labelSmall, color: fm.textSecondary)
                    StrataGradientBar(colors: (0...10).map { StrataPalette.scale(Double($0) / 10).color }).frame(width: 60)
                    Text("1 far from it").fmText(.labelSmall, color: fm.textSecondary)
                }
                .accessibilityElement(children: .combine)
                Text("Bubble size: lines of code").fmText(.labelSmall, color: fm.textTertiary)
                Button {
                    FmHaptics.selection()
                    showList.toggle()
                } label: {
                    StrataMenuLabel(icon: showList ? "scatter_plot" : "list", text: showList ? "Chart" : "List", highlighted: showList)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("strata-scatter-list")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private struct Geometry {
        let plot: CGRect
        let maxLines: Double

        init(size: CGSize, maxLines: Double) {
            let side = max(120, min(size.width - 64, size.height - 150))
            plot = CGRect(x: (size.width - side) / 2 + 18, y: 16, width: side, height: side)
            self.maxLines = max(1, maxLines)
        }

        func point(_ node: InsightModuleNode) -> CGPoint {
            CGPoint(x: plot.minX + min(1, max(0, node.instability)) * plot.width,
                    y: plot.maxY - min(1, max(0, node.abstractness)) * plot.height)
        }

        func radius(_ node: InsightModuleNode) -> CGFloat {
            min(plot.width * 0.05, max(4, 4 + plot.width * 0.04 * (Double(node.codeLines) / maxLines).squareRoot()))
        }
    }

    private var drawOrder: [Int] {
        Strata.stableSorted(Array(nodes.indices)) { nodes[$0].codeLines > nodes[$1].codeLines }
    }

    private var plot: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let geometry = Geometry(size: proxy.size, maxLines: Double(nodes.map(\.codeLines).max() ?? 1))
            let tones = StrataTones(fm)
            let order = drawOrder
            let selected = focus?.module ?? -1
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                drawBackground(&context, geometry: geometry, tones: tones)
                drawBubbles(&context, size: size, geometry: geometry, tones: tones, order: order, selected: selected)
            }
            .background(tones.canvas)
            .contentShape(Rectangle())
            .gesture(SpatialTapGesture().onEnded { (value: SpatialTapGesture.Value) in
                tap(value.location, geometry: geometry, order: order)
            })
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Abstractness against instability for \(nodes.count) modules")
            .accessibilityChildren {
                ForEach(order, id: \.self) { (m: Int) in
                    Text("\(StrataText.moduleName(nodes[m])), instability \(Strata.fixed(nodes[m].instability, 2)), abstractness \(Strata.fixed(nodes[m].abstractness, 2)), distance \(Strata.fixed(nodes[m].distance, 2))")
                        .accessibilityAddTraits(.isButton)
                        .accessibilityAction { open(.module(m)) }
                }
            }
        }
    }

    private func tap(_ location: CGPoint, geometry: Geometry, order: [Int]) {
        var best = -1
        var bestDistance = 1e9
        for m in order.reversed() {
            let p = geometry.point(nodes[m])
            let d = hypot(p.x - location.x, p.y - location.y)
            let r = geometry.radius(nodes[m]) + 8
            if d <= r && d / r < bestDistance {
                bestDistance = d / r
                best = m
            }
        }
        FmHaptics.selection()
        focus = best >= 0 ? .module(best) : nil
    }

    private func drawBackground(_ context: inout GraphicsContext, geometry: Geometry, tones: StrataTones) {
        let plot = geometry.plot
        let frame = Path(roundedRect: plot, cornerRadius: 6)
        context.fill(frame, with: .color(tones.surface))
        var inside = context
        inside.clip(to: frame)
        let pain = StrataPalette.unhealthy.color
        let useless = StrataPalette.problematic.color
        inside.fill(Path(plot), with: .radialGradient(
            Gradient(stops: [.init(color: pain.opacity(tones.dark ? 0.34 : 0.24), location: 0), .init(color: pain.opacity(0.06), location: 0.7), .init(color: pain.opacity(0), location: 1)]),
            center: CGPoint(x: plot.minX, y: plot.maxY), startRadius: 0, endRadius: plot.width * 0.62))
        inside.fill(Path(plot), with: .radialGradient(
            Gradient(stops: [.init(color: useless.opacity(tones.dark ? 0.30 : 0.24), location: 0), .init(color: useless.opacity(0.05), location: 0.7), .init(color: useless.opacity(0), location: 1)]),
            center: CGPoint(x: plot.maxX, y: plot.minY), startRadius: 0, endRadius: plot.width * 0.62))
        let b: CGFloat = 0.12
        var band = Path()
        band.move(to: CGPoint(x: plot.minX, y: plot.minY))
        band.addLine(to: CGPoint(x: plot.minX + b * plot.width, y: plot.minY))
        band.addLine(to: CGPoint(x: plot.maxX, y: plot.maxY - b * plot.height))
        band.addLine(to: CGPoint(x: plot.maxX, y: plot.maxY))
        band.addLine(to: CGPoint(x: plot.maxX - b * plot.width, y: plot.maxY))
        band.addLine(to: CGPoint(x: plot.minX, y: plot.minY + b * plot.height))
        band.closeSubpath()
        inside.fill(band, with: .color(StrataPalette.healthy.color.opacity(tones.dark ? 0.08 : 0.07)))
        var grid = Path()
        for i in 1..<4 {
            let t = CGFloat(i) / 4
            grid.move(to: CGPoint(x: plot.minX + t * plot.width, y: plot.minY))
            grid.addLine(to: CGPoint(x: plot.minX + t * plot.width, y: plot.maxY))
            grid.move(to: CGPoint(x: plot.minX, y: plot.minY + t * plot.height))
            grid.addLine(to: CGPoint(x: plot.maxX, y: plot.minY + t * plot.height))
        }
        inside.stroke(grid, with: .color(tones.border.opacity(0.55)), lineWidth: 1)
        var sequence = Path()
        sequence.move(to: CGPoint(x: plot.minX, y: plot.minY))
        sequence.addLine(to: CGPoint(x: plot.maxX, y: plot.maxY))
        inside.stroke(sequence, with: .color(StrataPalette.healthy.color.opacity(0.85)), style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
        context.draw(Text("Zone of pain").font(.system(size: 12, weight: .semibold)).foregroundColor(pain),
                     at: CGPoint(x: plot.minX + 10, y: plot.maxY - plot.height * 0.1), anchor: .leading)
        context.draw(Text("Zone of uselessness").font(.system(size: 12, weight: .semibold)).foregroundColor(useless),
                     at: CGPoint(x: plot.maxX - 10, y: plot.minY + plot.height * 0.08), anchor: .trailing)
        var rotated = context
        rotated.translateBy(x: plot.minX + plot.width * 0.58, y: plot.minY + plot.height * 0.42)
        rotated.rotate(by: .degrees(45))
        rotated.draw(Text("Main sequence").font(.system(size: 12, weight: .semibold)).foregroundColor(StrataPalette.healthy.color),
                     at: CGPoint(x: 0, y: -12))
        for i in 0...4 {
            let t = CGFloat(i) / 4
            let label = Strata.fixed(Double(t), i % 2 == 1 ? 2 : 1)
            context.draw(Text(label).font(.system(size: 10)).foregroundColor(tones.muted), at: CGPoint(x: plot.minX + t * plot.width, y: plot.maxY + 12))
            context.draw(Text(label).font(.system(size: 10)).foregroundColor(tones.muted), at: CGPoint(x: plot.minX - 6, y: plot.maxY - t * plot.height), anchor: .trailing)
        }
        context.draw(Text("Instability  I = Ce ÷ (Ca + Ce)").font(.system(size: 12, weight: .semibold)).foregroundColor(tones.text),
                     at: CGPoint(x: plot.midX, y: plot.maxY + 32))
        var vertical = context
        vertical.translateBy(x: plot.minX - 40, y: plot.midY)
        vertical.rotate(by: .degrees(-90))
        vertical.draw(Text("Abstractness  A = abstract types ÷ types").font(.system(size: 12, weight: .semibold)).foregroundColor(tones.text), at: .zero)
    }

    private func drawBubbles(_ context: inout GraphicsContext, size: CGSize, geometry: Geometry, tones: StrataTones, order: [Int], selected: Int) {
        for m in order {
            let node = nodes[m]
            let p = geometry.point(node)
            let r = geometry.radius(node)
            let match = query.isEmpty || Strata.matches(node.path, query)
            let color = StrataPalette.scale(node.distance).color
            let circle = Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2))
            context.fill(circle, with: .radialGradient(
                Gradient(colors: [color.opacity(match ? 0.92 : 0.18), color.opacity(match ? 0.78 : 0.14)]),
                center: CGPoint(x: p.x - r * 0.35, y: p.y - r * 0.35), startRadius: 0, endRadius: r * 1.3))
            context.stroke(circle, with: .color(color.opacity(match ? 0.9 : 0.2)), lineWidth: 1)
        }
        var taken: [CGRect] = []
        for m in Strata.scatterLabels(model, selected: selected) {
            let node = nodes[m]
            let p = geometry.point(node)
            let r = geometry.radius(node)
            let text = StrataText.moduleName(node)
            let width = CGFloat(text.count) * 6.6 + 12
            var box = CGRect(x: p.x + r + 4, y: p.y - 10, width: width, height: 20)
            if box.maxX > size.width - 4 { box.origin.x = p.x - r - 4 - width }
            if taken.contains(where: { $0.intersects(box) }) && m != selected { continue }
            taken.append(box)
            context.fill(Path(roundedRect: box, cornerRadius: 5), with: .color(tones.surface.opacity(0.88)))
            context.draw(Text(text).font(.system(size: 11.5, weight: .semibold)).foregroundColor(m == selected ? tones.accent : tones.text),
                         at: CGPoint(x: box.midX, y: box.midY))
        }
        if selected >= 0 && selected < nodes.count {
            let p = geometry.point(nodes[selected])
            let r = geometry.radius(nodes[selected]) + 2
            let ring = Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2))
            for i in stride(from: 3, through: 1, by: -1) {
                context.stroke(ring, with: .color(tones.accent.opacity(0.18 / Double(i))), lineWidth: 2 + CGFloat(i) * 3)
            }
            context.stroke(ring, with: .color(tones.accent), lineWidth: 2.4)
        }
    }

    private var list: some View {
        let ranked = Strata.stableSorted(Array(nodes.indices)) { nodes[$0].distance > nodes[$1].distance }
            .filter { query.isEmpty || Strata.matches(nodes[$0].path, query) }
        return ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(ranked, id: \.self) { (m: Int) in
                    let node = nodes[m]
                    FmListRow(
                        title: StrataText.moduleName(node),
                        subtitle: "I \(Strata.fixed(node.instability, 2)) · A \(Strata.fixed(node.abstractness, 2)) · D \(Strata.fixed(node.distance, 2))\(zone(node))",
                        leading: AnyView(StrataDot(color: StrataPalette.scale(node.distance).color, size: 14)),
                        onTap: { open(.module(m)) },
                        minHeight: 52)
                    if m != ranked.last { FmRowDivider(indent: 42) }
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .padding(FmSpace.x4)
        }
    }

    private func zone(_ node: InsightModuleNode) -> String {
        if node.types > 0 && node.abstractness <= 0.2 && node.instability <= 0.3 { return " · zone of pain" }
        if node.types > 0 && node.abstractness >= 0.7 && node.instability >= 0.7 { return " · zone of uselessness" }
        return ""
    }
}
