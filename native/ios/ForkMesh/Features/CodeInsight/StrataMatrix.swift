import FMCodeInsight
import SwiftUI

struct StrataScrollOrigin: PreferenceKey {
    static let defaultValue: CGPoint = .zero
    static func reduce(value: inout CGPoint, nextValue: () -> CGPoint) { value = nextValue() }
}

struct StrataMatrixView: View {
    let model: StrataModel
    @Binding var focus: StrataFocus?
    let open: (StrataFocus) -> Void

    @Environment(\.fmPalette) private var fm
    @State private var origin: CGPoint = .zero
    @State private var showList = false
    @State private var weights: [Int: Int] = [:]
    @State private var maxWeight = 1
    @State private var breaks: [Int] = []

    private let rowHeader: CGFloat = 120
    private let columnHeader: CGFloat = 104

    var body: some View {
        VStack(spacing: 0) {
            bar
            if model.order.isEmpty {
                FmEmptyState(icon: "grid_on", title: "No modules", message: "No modules with source files were found.")
            } else if showList {
                list
            } else {
                matrix
            }
        }
        .onAppear(perform: prepare)
        .onChange(of: model.order) { (_: [Int], _: [Int]) in prepare() }
    }

    private func prepare() {
        let result = Strata.matrixWeights(model.report.graph)
        weights = result.weights
        maxWeight = result.maxWeight
        breaks = Strata.matrixLayerBreaks(model)
    }

    private var bar: some View {
        let tones = StrataTones(fm)
        return FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x3) {
                key(tones.accent, "row uses column")
                key(tones.dependent, "column uses row")
                key(tones.cycle, "both, a cycle")
                Button {
                    FmHaptics.selection()
                    showList.toggle()
                } label: {
                    StrataMenuLabel(icon: showList ? "grid_on" : "list", text: showList ? "Matrix" : "List", highlighted: showList)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("strata-matrix-list")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private func key(_ color: Color, _ text: String) -> some View {
        HStack(spacing: 4) {
            RoundedRectangle(cornerRadius: 3).fill(color.opacity(0.75)).frame(width: 14, height: 14)
            Text(text).fmText(.labelSmall, color: fm.textSecondary)
        }
        .accessibilityElement(children: .combine)
    }

    private func cellSize(_ size: CGSize) -> CGFloat {
        let n = CGFloat(max(1, model.order.count))
        let available = min(size.width - rowHeader, size.height - columnHeader) - 8
        return min(46, max(26, floor(available / n)))
    }

    private var matrix: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let cell = cellSize(proxy.size)
            let n = model.order.count
            let tones = StrataTones(fm)
            let selected = focus?.module.flatMap { model.order.firstIndex(of: $0) } ?? -1
            VStack(spacing: 0) {
                HStack(spacing: 0) {
                    Text("Uses →")
                        .fmText(.labelSmall, color: fm.textTertiary)
                        .frame(width: rowHeader, height: columnHeader, alignment: .bottomLeading)
                        .padding(.leading, FmSpace.x2)
                        .accessibilityHidden(true)
                    Canvas { (context: inout GraphicsContext, size: CGSize) in
                        drawColumns(&context, size: size, cell: cell, tones: tones, selected: selected)
                    }
                    .frame(height: columnHeader)
                    .clipped()
                    .contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { (value: SpatialTapGesture.Value) in
                        let column = Int(floor((value.location.x - origin.x) / cell))
                        if column >= 0 && column < n { open(.module(model.order[column])) }
                    })
                    .accessibilityHidden(true)
                }
                HStack(spacing: 0) {
                    Canvas { (context: inout GraphicsContext, size: CGSize) in
                        drawRows(&context, size: size, cell: cell, tones: tones, selected: selected)
                    }
                    .frame(width: rowHeader)
                    .clipped()
                    .contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { (value: SpatialTapGesture.Value) in
                        let row = Int(floor((value.location.y - origin.y) / cell))
                        if row >= 0 && row < n { open(.module(model.order[row])) }
                    })
                    .accessibilityHidden(true)
                    ZStack(alignment: .topLeading) {
                        Canvas { (context: inout GraphicsContext, size: CGSize) in
                            drawGrid(&context, size: size, cell: cell, tones: tones, selected: selected)
                        }
                        .allowsHitTesting(false)
                        ScrollView([.horizontal, .vertical]) {
                            Color.clear
                                .frame(width: CGFloat(n) * cell + 8, height: CGFloat(n) * cell + 8)
                                .contentShape(Rectangle())
                                .background(GeometryReader { (inner: GeometryProxy) in
                                    Color.clear.preference(key: StrataScrollOrigin.self, value: inner.frame(in: .named("strata-matrix")).origin)
                                })
                                .gesture(SpatialTapGesture().onEnded { (value: SpatialTapGesture.Value) in
                                    tapCell(value.location, cell: cell)
                                })
                        }
                        .coordinateSpace(name: "strata-matrix")
                        .onPreferenceChange(StrataScrollOrigin.self) { (value: CGPoint) in origin = value }
                    }
                    .clipped()
                }
            }
            .background(tones.canvas)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Dependency structure matrix of \(n) modules")
            .accessibilityHint("The List button lists every dependency")
        }
    }

    private func weight(_ from: Int, _ to: Int) -> Int {
        weights[from * model.report.graph.nodes.count + to] ?? 0
    }

    private func tapCell(_ location: CGPoint, cell: CGFloat) {
        let n = model.order.count
        let row = Int(floor(location.y / cell))
        let column = Int(floor(location.x / cell))
        guard row >= 0, row < n, column >= 0, column < n else { return }
        let a = model.order[row]
        let b = model.order[column]
        if a == b {
            open(.module(a))
        } else if weight(a, b) > 0 || weight(b, a) > 0 {
            open(.cell(a, b))
        }
    }

    private func visibleRange(_ offset: CGFloat, _ length: CGFloat, cell: CGFloat) -> ClosedRange<Int>? {
        let n = model.order.count
        guard n > 0 else { return nil }
        let first = max(0, Int(floor(-offset / cell)))
        let last = min(n - 1, Int(floor((length - offset) / cell)))
        return first <= last ? first...last : nil
    }

    private func drawGrid(_ context: inout GraphicsContext, size: CGSize, cell: CGFloat, tones: StrataTones, selected: Int) {
        let n = model.order.count
        let order = model.order
        let ox = origin.x
        let oy = origin.y
        let full = CGFloat(n) * cell
        context.fill(Path(CGRect(x: ox, y: oy, width: full, height: full)), with: .color(tones.surface))
        if selected >= 0 {
            context.fill(Path(CGRect(x: ox, y: oy + CGFloat(selected) * cell, width: full, height: cell)), with: .color(tones.accent.opacity(0.10)))
            context.fill(Path(CGRect(x: ox + CGFloat(selected) * cell, y: oy, width: cell, height: full)), with: .color(tones.accent.opacity(0.10)))
        }
        guard let rows = visibleRange(oy, size.height, cell: cell), let columns = visibleRange(ox, size.width, cell: cell) else { return }
        let logMax = log(1.0 + Double(maxWeight))
        let fontSize = min(11.5, max(8, cell * 0.42))
        for r in rows {
            for c in columns {
                let rect = CGRect(x: ox + CGFloat(c) * cell, y: oy + CGFloat(r) * cell, width: cell, height: cell)
                if r == c {
                    context.fill(Path(rect.insetBy(dx: 1, dy: 1)), with: .color(tones.faint))
                    continue
                }
                let uses = weight(order[r], order[c])
                let usedBy = weight(order[c], order[r])
                if uses == 0 && usedBy == 0 { continue }
                let color = uses > 0 && usedBy > 0 ? tones.cycle : (uses > 0 ? tones.accent : tones.dependent)
                let shown = uses > 0 ? uses : usedBy
                let t = logMax > 0 ? log(1.0 + Double(shown)) / logMax : 1
                context.fill(Path(roundedRect: rect.insetBy(dx: 1.5, dy: 1.5), cornerRadius: 3), with: .color(color.opacity(0.28 + 0.62 * t)))
                if cell >= 18 {
                    let text = shown >= 1000 ? "\(shown / 1000)k" : "\(shown)"
                    let ink = t > 0.45 ? Color.white : tones.text
                    context.draw(Text(text).font(.system(size: fontSize, weight: .medium)).foregroundColor(ink), at: CGPoint(x: rect.midX, y: rect.midY))
                }
            }
        }
        var grid = Path()
        for i in 0...n {
            let p = CGFloat(i) * cell
            grid.move(to: CGPoint(x: ox, y: oy + p))
            grid.addLine(to: CGPoint(x: ox + full, y: oy + p))
            grid.move(to: CGPoint(x: ox + p, y: oy))
            grid.addLine(to: CGPoint(x: ox + p, y: oy + full))
        }
        context.stroke(grid, with: .color(tones.border.opacity(0.45)), lineWidth: 0.5)
        var layers = Path()
        for i in breaks {
            let p = CGFloat(i) * cell
            layers.move(to: CGPoint(x: ox, y: oy + p))
            layers.addLine(to: CGPoint(x: ox + full, y: oy + p))
            layers.move(to: CGPoint(x: ox + p, y: oy))
            layers.addLine(to: CGPoint(x: ox + p, y: oy + full))
        }
        context.stroke(layers, with: .color(tones.muted.opacity(0.6)), lineWidth: 1.4)
        for cycle in model.report.graph.cycles {
            let positions = cycle.compactMap { order.firstIndex(of: $0) }
            guard let low = positions.min(), let high = positions.max() else { continue }
            let rect = CGRect(x: ox + CGFloat(low) * cell, y: oy + CGFloat(low) * cell, width: CGFloat(high - low + 1) * cell, height: CGFloat(high - low + 1) * cell)
            context.stroke(Path(roundedRect: rect.insetBy(dx: 1, dy: 1), cornerRadius: 4), with: .color(tones.cycle), lineWidth: 2)
        }
    }

    private func drawRows(_ context: inout GraphicsContext, size: CGSize, cell: CGFloat, tones: StrataTones, selected: Int) {
        guard let rows = visibleRange(origin.y, size.height, cell: cell) else { return }
        let nodes = model.report.graph.nodes
        for r in rows {
            let node = nodes[model.order[r]]
            let y = origin.y + CGFloat(r) * cell + cell / 2
            let color = r == selected ? tones.accent : (node.cycle >= 0 ? tones.cycle : tones.text)
            let name = elided(StrataText.moduleName(node), width: size.width - 12, font: 12)
            context.draw(Text(name).font(.system(size: 12, weight: r == selected ? .semibold : .regular)).foregroundColor(color),
                         at: CGPoint(x: size.width - 6, y: y), anchor: .trailing)
        }
    }

    private func drawColumns(_ context: inout GraphicsContext, size: CGSize, cell: CGFloat, tones: StrataTones, selected: Int) {
        guard let columns = visibleRange(origin.x, size.width, cell: cell) else { return }
        let nodes = model.report.graph.nodes
        for c in columns {
            let node = nodes[model.order[c]]
            let x = origin.x + CGFloat(c) * cell + cell / 2
            let color = c == selected ? tones.accent : (node.cycle >= 0 ? tones.cycle : tones.text)
            let name = elided(StrataText.moduleName(node), width: size.height * 1.3 - 12, font: 12)
            var layer = context
            layer.translateBy(x: x, y: size.height - 6)
            layer.rotate(by: .degrees(-50))
            layer.draw(Text(name).font(.system(size: 12, weight: c == selected ? .semibold : .regular)).foregroundColor(color),
                       at: .zero, anchor: .leading)
        }
    }

    private func elided(_ text: String, width: CGFloat, font: CGFloat) -> String {
        let maxChars = max(4, Int(width / (font * 0.56)))
        guard text.count > maxChars else { return text }
        return String(text.prefix(maxChars / 2 - 1)) + "…" + String(text.suffix(maxChars - maxChars / 2))
    }

    private var list: some View {
        let graph = model.report.graph
        let edges = Strata.stableSorted(Array(graph.edges.indices)) { graph.edges[$0].weight > graph.edges[$1].weight }
        return ScrollView {
            LazyVStack(spacing: 0) {
                if edges.isEmpty {
                    Text("No module depends on another.").fmText(.bodyMedium, color: fm.textSecondary).padding(FmSpace.x4)
                }
                ForEach(edges, id: \.self) { (e: Int) in
                    let edge = graph.edges[e]
                    FmListRow(
                        title: "\(StrataText.moduleName(graph.nodes[edge.from])) → \(StrataText.moduleName(graph.nodes[edge.to]))",
                        subtitle: "\(edge.weight) imports\(edge.inCycle ? " · cycle" : "")",
                        leading: AnyView(StrataDot(color: edge.inCycle ? StrataPalette.unhealthy.color : fm.accent, size: 10)),
                        onTap: { open(.cell(edge.from, edge.to)) },
                        minHeight: 52)
                    if e != edges.last { FmRowDivider(indent: 38) }
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .padding(FmSpace.x4)
        }
    }
}
