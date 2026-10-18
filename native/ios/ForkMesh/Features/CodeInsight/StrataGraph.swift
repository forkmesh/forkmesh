import FMCodeInsight
import SwiftUI

struct StrataGraphScene {
    struct Item {
        var rect: CGRect
        var module: Bool
        var external: Bool
        var dependent: Bool
        var index: Int
        var title: String
        var caption: String
        var fill: Color
        var titleColor: Color
        var cycle: Bool
        var match: Bool
        var cells: [(CGRect, Color)]
    }

    struct Link {
        var from: Int
        var to: Int
        var weight: Int
        var cycle: Bool
        var width: CGFloat
        var path: Path
        var arrow: Path
    }

    var items: [Item] = []
    var links: [Link] = []
    var bounds = StrataRect()
    var drill = -1
    var hiddenFiles = 0

    static func edgePath(_ points: [CGPoint], flat: Bool) -> Path {
        var path = Path()
        guard points.count >= 2 else { return path }
        path.move(to: points[0])
        if flat && points.count == 3 {
            let s = points[0]
            let m = points[1]
            let e = points[2]
            path.addCurve(to: e, control1: CGPoint(x: s.x, y: s.y + (m.y - s.y) * 1.33), control2: CGPoint(x: e.x, y: e.y + (m.y - e.y) * 1.33))
            return path
        }
        for i in 0..<(points.count - 1) {
            let a = points[i]
            let b = points[i + 1]
            if abs(a.x - b.x) < 0.5 {
                path.addLine(to: b)
                continue
            }
            let dy = (b.y - a.y) * 0.5
            path.addCurve(to: b, control1: CGPoint(x: a.x, y: a.y + dy), control2: CGPoint(x: b.x, y: b.y - dy))
        }
        return path
    }

    static func build(_ model: StrataModel, drill: StrataDrillGraph?, colorBy: StrataGraphColor, query: String) -> StrataGraphScene {
        var scene = StrataGraphScene()
        let report = model.report
        let snapshot = model.snapshot
        let layout: StrataGraphLayout
        if let drill, drill.module >= 0 {
            layout = drill.layout
            scene.drill = drill.module
            scene.hiddenFiles = drill.hiddenFiles
            let drillCycle = report.graph.nodes[drill.module].cycle
            for (k, node) in drill.nodes.enumerated() where k < layout.boxes.count {
                if node.module {
                    let mod = report.graph.nodes[node.index]
                    scene.items.append(Item(
                        rect: layout.boxes[k].cg, module: true, external: true, dependent: node.dependent, index: node.index,
                        title: StrataText.moduleName(mod), caption: Strata.drillCaption(model, node),
                        fill: Strata.moduleColor(model, node.index, by: colorBy).color,
                        titleColor: Strata.moduleColor(model, node.index, by: colorBy).readable,
                        cycle: mod.cycle >= 0 && mod.cycle == drillCycle,
                        match: query.isEmpty || Strata.matches(mod.path, query) || Strata.matches(mod.name, query), cells: []))
                } else {
                    let file = snapshot.files[node.index]
                    scene.items.append(Item(
                        rect: layout.boxes[k].cg, module: false, external: false, dependent: false, index: node.index,
                        title: Strata.fileName(file.path), caption: Strata.drillCaption(model, node),
                        fill: Strata.fileColor(model, node.index, by: colorBy).color,
                        titleColor: Strata.fileColor(model, node.index, by: colorBy).readable, cycle: false,
                        match: Strata.matches(file.path, query), cells: []))
                }
            }
        } else {
            layout = model.layout
            for (i, node) in report.graph.nodes.enumerated() where i < layout.boxes.count {
                let rect = layout.boxes[i].cg
                var files = Strata.stableSorted(node.files) { snapshot.files[$0].codeLines > snapshot.files[$1].codeLines }
                if files.count > 240 { files = Array(files.prefix(240)) }
                var cells: [(CGRect, Color)] = []
                let inner = layout.boxes[i].adjusted(7, 42, -7, -7)
                if inner.width > 6 && inner.height > 6 {
                    let rects = Strata.squarify(files.map { Double(max(1, snapshot.files[$0].codeLines)) }, inner)
                    for (k, cell) in rects.enumerated() where !cell.isEmpty {
                        cells.append((cell.cg, StrataPalette.health(snapshot.files[files[k]].health).color))
                    }
                }
                scene.items.append(Item(
                    rect: rect, module: true, external: false, dependent: false, index: i,
                    title: StrataText.moduleName(node), caption: Strata.moduleCaption(node),
                    fill: Strata.moduleColor(model, i, by: colorBy).color,
                    titleColor: Strata.moduleColor(model, i, by: colorBy).readable, cycle: node.cycle >= 0,
                    match: Strata.moduleMatches(model, i, query), cells: cells))
            }
        }
        for edge in layout.edges {
            let width = CGFloat(Strata.edgeWidth(edge.weight))
            var points = edge.points.map(\.cg)
            var arrow = Path()
            if points.count >= 2 {
                let tip = points[points.count - 1]
                let before = points[points.count - 2]
                let direction: CGFloat = tip.y >= before.y ? 1 : -1
                let length = 8 + width * 1.4
                let half = 4 + width * 0.9
                arrow.move(to: tip)
                arrow.addLine(to: CGPoint(x: tip.x - half, y: tip.y - direction * length))
                arrow.addLine(to: CGPoint(x: tip.x + half, y: tip.y - direction * length))
                arrow.closeSubpath()
                points[points.count - 1] = CGPoint(x: tip.x, y: tip.y - direction * length * 0.8)
            }
            scene.links.append(Link(from: edge.from, to: edge.to, weight: edge.weight, cycle: edge.cycle, width: width,
                                    path: edgePath(points, flat: edge.flat), arrow: arrow))
        }
        scene.bounds = layout.bounds
        return scene
    }

    func itemIndex(for focus: StrataFocus?) -> Int {
        guard let focus else { return -1 }
        switch focus {
        case .module(let m): return items.firstIndex { $0.module && $0.index == m } ?? -1
        case .file(let f): return items.firstIndex { !$0.module && $0.index == f } ?? -1
        case .cell: return -1
        }
    }

    func hit(_ point: CGPoint) -> Int {
        for i in items.indices.reversed() where items[i].rect.insetBy(dx: -4, dy: -4).contains(point) { return i }
        return -1
    }
}

struct StrataDrillKey: Equatable {
    let stamp: Int
    let drill: Int?
}

struct StrataFitKey: Equatable {
    let stamp: Int
    let drill: Int
}

struct StrataSceneKey: Equatable {
    let stamp: Int
    let drill: Int
    let colorBy: StrataGraphColor
    let query: String
}

struct StrataGraphView: View {
    let model: StrataModel
    let stamp: Int
    let query: String
    @Binding var focus: StrataFocus?
    @Binding var drill: Int?
    let open: (StrataFocus) -> Void
    let openFile: (String, Int) -> Void

    @Environment(\.fmPalette) private var fm
    @State private var drillGraph: StrataDrillGraph?
    @State private var scene: StrataGraphScene?
    @State private var colorBy: StrataGraphColor = .rating
    @State private var zoom = StrataZoom()
    @State private var live = StrataLiveGesture()
    @State private var size: CGSize = .zero
    @State private var showList = false
    @State private var fitted: StrataFitKey?

    private var drillModule: Int { drillGraph?.module ?? -1 }

    var body: some View {
        VStack(spacing: 0) {
            bar
            ZStack(alignment: .bottom) {
                if showList {
                    list
                } else {
                    canvas
                }
                if let focus, !showList {
                    StrataSelectionCard(model: model, focus: focus, drilled: drillModule, open: open, openFile: openFile,
                                        showFiles: { (module: Int) in drill = module },
                                        clear: { self.focus = nil })
                        .padding(FmSpace.x3)
                }
            }
        }
        .task(id: StrataDrillKey(stamp: stamp, drill: drill)) {
            guard let target = drill, target >= 0, target < model.report.graph.nodes.count else {
                drillGraph = nil
                return
            }
            let snapshot = model.snapshot
            let graph = model.report.graph
            let built: StrataDrillGraph = await Task.detached(priority: .userInitiated) {
                Strata.drillGraph(snapshot, graph, module: target)
            }.value
            if !Task.isCancelled { drillGraph = built }
        }
        .task(id: StrataSceneKey(stamp: stamp, drill: drillModule, colorBy: colorBy, query: query)) {
            let drilled = drill == nil ? nil : drillGraph
            scene = StrataGraphScene.build(model, drill: drilled, colorBy: colorBy, query: query)
            refit()
        }
    }

    private var bar: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                if let drillGraph, drill != nil {
                    Button {
                        FmHaptics.selection()
                        drill = nil
                    } label: {
                        StrataMenuLabel(icon: "arrow_back", text: "All modules")
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("strata-drill-out")
                    Text(StrataText.moduleName(model.report.graph.nodes[drillGraph.module]))
                        .fmText(.labelLarge)
                        .lineLimit(1)
                    if drillGraph.hiddenFiles > 0 {
                        Text("+\(drillGraph.hiddenFiles) files not shown").fmText(.labelSmall, color: fm.textTertiary)
                    }
                }
                Menu {
                    Picker("Colour", selection: $colorBy) {
                        ForEach(StrataGraphColor.allCases, id: \.self) { (option: StrataGraphColor) in
                            Text(option.label).tag(option)
                        }
                    }
                } label: {
                    StrataMenuLabel(icon: "palette", text: colorBy.label)
                }
                .accessibilityLabel("Colour by \(colorBy.label)")
                Button {
                    FmHaptics.selection()
                    fit()
                } label: {
                    StrataMenuLabel(icon: "fit_screen", text: "Fit")
                }
                .buttonStyle(.plain)
                .disabled(showList)
                Button {
                    FmHaptics.selection()
                    showList.toggle()
                } label: {
                    StrataMenuLabel(icon: showList ? "account_tree" : "list", text: showList ? "Graph" : "List", highlighted: showList)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("strata-graph-list")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private var canvas: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let view = zoom.applying(live)
            let tones = StrataTones(fm)
            Canvas { (context: inout GraphicsContext, canvasSize: CGSize) in
                guard let scene else { return }
                StrataGraphPainter(scene: scene, tones: tones, zoom: view, focus: scene.itemIndex(for: focus), dimming: !query.isEmpty)
                    .paint(&context, size: canvasSize)
            }
            .background(tones.canvas)
            .contentShape(Rectangle())
            .gesture(
                SpatialTapGesture(count: 2)
                    .onEnded { (value: SpatialTapGesture.Value) in doubleTap(value.location) }
                    .exclusively(before: SpatialTapGesture(count: 1).onEnded { (value: SpatialTapGesture.Value) in tap(value.location) })
            )
            .strataZoomPan($zoom, live: $live)
            .overlay(alignment: .topLeading) { legend.padding(FmSpace.x3) }
            .accessibilityElement(children: .contain)
            .accessibilityLabel(drill == nil ? "Dependency graph of \(model.report.graph.nodes.count) modules" : "Files of the module")
            .accessibilityHint("The List button shows the same modules and files as a list")
            .accessibilityChildren { accessibilityItems }
            .onAppear {
                size = proxy.size
                refit()
            }
            .onChange(of: proxy.size) { (_: CGSize, newSize: CGSize) in
                size = newSize
                refit()
            }
        }
    }

    @ViewBuilder private var accessibilityItems: some View {
        if let scene {
            ForEach(Array(scene.items.enumerated()), id: \.offset) { (_: Int, item: StrataGraphScene.Item) in
                Text("\(item.title), \(item.caption)\(item.cycle ? ", in a cycle" : "")")
                    .accessibilityAddTraits(.isButton)
                    .accessibilityAction {
                        open(item.module ? .module(item.index) : .file(item.index))
                    }
            }
        }
    }

    private var legend: some View {
        let tones = StrataTones(fm)
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: FmSpace.x1) {
                Text(colorBy.label).fmText(.labelSmall, color: fm.textPrimary)
                switch colorBy {
                case .rating:
                    ForEach(0..<5, id: \.self) { (rating: Int) in
                        StrataRatingChip(rating: rating, size: 15)
                    }
                case .health:
                    gradientKey("Unhealthy", (0...10).map { StrataPalette.health(1 + 9 * Double($0) / 10).color }, "Healthy")
                case .instability:
                    gradientKey("Stable", (0...10).map { StrataPalette.instability(Double($0) / 10).color }, "Unstable")
                case .complexity:
                    gradientKey("Simple", (0...10).map { StrataPalette.scale(Double($0) / 10).color }, "Complex")
                }
            }
            HStack(spacing: FmSpace.x2) {
                lineKey(tones.accent, "uses")
                lineKey(tones.dependent, "used by")
                lineKey(tones.cycle, "cycle")
            }
            Text(drill == nil ? "Pinch to zoom · drag to pan · double-tap a module to see its files" : "Double-tap a file to open it, a module to go there")
                .fmText(.labelSmall, color: fm.textTertiary)
        }
        .padding(FmSpace.x2)
        .background(fm.bgRaised.opacity(0.94))
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .accessibilityElement(children: .combine)
    }

    private func gradientKey(_ low: String, _ colors: [Color], _ high: String) -> some View {
        HStack(spacing: FmSpace.x1) {
            Text(low).fmText(.labelSmall, color: fm.textSecondary)
            StrataGradientBar(colors: colors).frame(width: 64)
            Text(high).fmText(.labelSmall, color: fm.textSecondary)
        }
    }

    private func lineKey(_ color: Color, _ text: String) -> some View {
        HStack(spacing: 4) {
            Capsule().fill(color).frame(width: 14, height: 3)
            Text(text).fmText(.labelSmall, color: fm.textSecondary)
        }
    }

    @ViewBuilder private var list: some View {
        ScrollView {
            if let drillGraph, drill != nil {
                StrataDrillList(model: model, drill: drillGraph, query: query, open: open)
                    .background(fm.bgRaised)
                    .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
                    .padding(FmSpace.x4)
            } else {
                StrataModuleList(model: model, query: query, open: open)
                    .background(fm.bgRaised)
                    .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
                    .padding(FmSpace.x4)
            }
        }
    }

    private func refit() {
        guard scene != nil, size.width > 0, size.height > 0 else { return }
        let key = StrataFitKey(stamp: stamp, drill: drillModule)
        if fitted != key {
            fit()
            fitted = key
        }
    }

    private func fit() {
        guard let scene, size.width > 0 else { return }
        let target = StrataZoom.fit(scene.bounds, in: CGSize(width: size.width, height: max(1, size.height - 120)), inset: 20)
        zoom = StrataZoom(scale: target.scale, offset: CGSize(width: target.offset.width, height: target.offset.height + 70))
    }

    private func tap(_ location: CGPoint) {
        guard let scene else { return }
        let index = scene.hit(zoom.world(location))
        if index < 0 {
            if focus != nil { FmHaptics.selection() }
            focus = nil
            return
        }
        let item = scene.items[index]
        FmHaptics.selection()
        focus = item.module ? .module(item.index) : .file(item.index)
    }

    private func doubleTap(_ location: CGPoint) {
        guard let scene else { return }
        let index = scene.hit(zoom.world(location))
        if index < 0 {
            fit()
            return
        }
        let item = scene.items[index]
        if item.module {
            if drill == nil || item.external {
                FmHaptics.tap()
                focus = .module(item.index)
                drill = item.index
            }
        } else {
            FmHaptics.tap()
            openFile(model.snapshot.files[item.index].path, 0)
        }
    }
}

struct StrataGraphPainter {
    let scene: StrataGraphScene
    let tones: StrataTones
    let zoom: StrataZoom
    let focus: Int
    let dimming: Bool

    func paint(_ context: inout GraphicsContext, size: CGSize) {
        var ctx = context
        ctx.translateBy(x: zoom.offset.width, y: zoom.offset.height)
        ctx.scaleBy(x: zoom.scale, y: zoom.scale)
        let scale = zoom.scale
        let visible = CGRect(x: -zoom.offset.width / scale, y: -zoom.offset.height / scale, width: size.width / scale, height: size.height / scale)
            .insetBy(dx: -40, dy: -40)
        for link in scene.links {
            let color = link.cycle ? tones.cycle.opacity(0.85) : tones.muted.opacity(0.42)
            stroke(&ctx, link, color: color, boost: 1)
        }
        for item in scene.items where item.rect.intersects(visible) {
            paintItem(&ctx, item, opacity: dimming && !item.match ? 0.22 : 1, raised: false)
        }
        guard focus >= 0 && focus < scene.items.count else { return }
        ctx.fill(Path(visible.insetBy(dx: -2000, dy: -2000)), with: .color(tones.canvas.opacity(0.62)))
        var uses: [Int] = []
        var usedBy: [Int] = []
        for link in scene.links {
            if link.from == focus {
                uses.append(link.to)
                stroke(&ctx, link, color: tones.accent.opacity(0.95), boost: 1.2)
            } else if link.to == focus {
                usedBy.append(link.from)
                stroke(&ctx, link, color: tones.dependent.opacity(0.95), boost: 1.2)
            }
        }
        for i in usedBy {
            paintItem(&ctx, scene.items[i], opacity: 1, raised: true)
            ring(&ctx, scene.items[i], color: tones.dependent, width: 1.6)
        }
        for i in uses {
            paintItem(&ctx, scene.items[i], opacity: 1, raised: true)
            ring(&ctx, scene.items[i], color: tones.accent, width: 1.6)
        }
        paintItem(&ctx, scene.items[focus], opacity: 1, raised: true)
        ring(&ctx, scene.items[focus], color: tones.accent, width: 2.6)
    }

    private func stroke(_ ctx: inout GraphicsContext, _ link: StrataGraphScene.Link, color: Color, boost: CGFloat) {
        let width = max(link.width * boost, 1.1 / zoom.scale)
        ctx.stroke(link.path, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round))
        ctx.fill(link.arrow, with: .color(color))
    }

    private func radius(_ item: StrataGraphScene.Item) -> CGFloat {
        item.external ? item.rect.height / 2 : (item.module ? 9 : 7)
    }

    private func ring(_ ctx: inout GraphicsContext, _ item: StrataGraphScene.Item, color: Color, width: CGFloat) {
        let shape = Path(roundedRect: item.rect, cornerRadius: radius(item))
        for i in stride(from: 3, through: 1, by: -1) {
            ctx.stroke(shape, with: .color(color.opacity(0.16 / Double(i))), lineWidth: (width + CGFloat(i) * 3) / zoom.scale)
        }
        ctx.stroke(shape, with: .color(color), lineWidth: width / zoom.scale)
    }

    private func label(_ ctx: inout GraphicsContext, _ text: String, in rect: CGRect, size: CGFloat, bold: Bool, color: Color) {
        guard rect.width > 4, rect.height > 2 else { return }
        let maxChars = max(3, Int(rect.width / (size * 0.56)))
        let shown = text.count > maxChars ? String(text.prefix(maxChars / 2 - 1)) + "…" + String(text.suffix(maxChars - maxChars / 2)) : text
        let resolved = ctx.resolve(Text(shown).font(.system(size: size, weight: bold ? .semibold : .regular)).foregroundColor(color))
        ctx.drawLayer { (layer: inout GraphicsContext) in
            layer.clip(to: Path(rect))
            layer.draw(resolved, at: CGPoint(x: rect.minX, y: rect.midY), anchor: .leading)
        }
    }

    private func paintItem(_ ctx: inout GraphicsContext, _ item: StrataGraphScene.Item, opacity: Double, raised: Bool) {
        var layer = ctx
        layer.opacity = opacity
        let r = item.rect
        let scale = zoom.scale
        let screenHeight = r.height * scale
        let base = raised ? tones.raised : tones.surface
        if item.external {
            let shape = Path(roundedRect: r, cornerRadius: r.height / 2)
            layer.fill(shape, with: .color(base))
            layer.stroke(shape, with: .color((item.dependent ? tones.dependent : tones.accent).opacity(0.75)),
                         style: StrokeStyle(lineWidth: 1.3 / scale, dash: [5 / scale, 4 / scale]))
            layer.fill(Path(ellipseIn: CGRect(x: r.minX + 11.5, y: r.midY - 4.5, width: 9, height: 9)), with: .color(item.fill))
            if screenHeight > 12 {
                label(&layer, item.title, in: CGRect(x: r.minX + 28, y: r.minY + 2, width: r.width - 40, height: r.height / 2 - 1),
                      size: min(17, max(12.5, 10.5 / scale)), bold: true, color: tones.text)
                label(&layer, item.caption, in: CGRect(x: r.minX + 28, y: r.midY, width: r.width - 40, height: r.height / 2 - 2),
                      size: min(15, max(10.5, 9 / scale)), bold: false, color: tones.muted)
            }
            return
        }
        let corner: CGFloat = item.module ? 9 : 7
        let shape = Path(roundedRect: r, cornerRadius: corner)
        layer.fill(shape, with: .color(base))
        layer.fill(shape, with: .color(item.fill.opacity(tones.dark ? 0.12 : 0.08)))
        if item.module {
            layer.drawLayer { (strip: inout GraphicsContext) in
                strip.clip(to: shape)
                strip.fill(Path(CGRect(x: r.minX, y: r.minY, width: r.width, height: 24)), with: .color(item.fill))
            }
            if screenHeight > 22 {
                label(&layer, item.title, in: CGRect(x: r.minX + 9, y: r.minY, width: r.width - 18, height: 24),
                      size: min(19, max(12.5, 10.5 / scale)), bold: true, color: item.titleColor)
            }
            if screenHeight > 46 {
                label(&layer, item.caption, in: CGRect(x: r.minX + 8, y: r.minY + 25, width: r.width - 16, height: 16),
                      size: min(14, max(10.5, 9 / scale)), bold: false, color: tones.muted)
            }
            if screenHeight > 24 {
                for cell in item.cells {
                    let cr = cell.0.insetBy(dx: 0.6, dy: 0.6)
                    if cr.width * scale >= 2.5 && cr.height * scale >= 2.5 {
                        layer.fill(Path(roundedRect: cr, cornerRadius: 1.5), with: .color(cell.1.opacity(tones.dark ? 0.78 : 0.85)))
                    } else {
                        layer.fill(Path(cell.0), with: .color(cell.1.opacity(tones.dark ? 0.78 : 0.85)))
                    }
                }
            }
        } else {
            layer.fill(Path(roundedRect: CGRect(x: r.minX, y: r.minY, width: 6, height: r.height), cornerRadius: 3), with: .color(item.fill))
            if screenHeight > 14 {
                label(&layer, item.title, in: CGRect(x: r.minX + 14, y: r.minY + 5, width: r.width - 22, height: r.height / 2 - 5),
                      size: min(20, max(12.5, 10.5 / scale)), bold: true, color: tones.text)
                label(&layer, item.caption, in: CGRect(x: r.minX + 14, y: r.midY, width: r.width - 22, height: r.height / 2 - 4),
                      size: min(16, max(10.5, 9 / scale)), bold: false, color: tones.muted)
            }
        }
        layer.stroke(shape, with: .color(item.cycle ? tones.cycle : item.fill.opacity(0.55)), lineWidth: (item.cycle ? 2.0 : 1.2) / scale)
    }
}

struct StrataDrillList: View {
    @Environment(\.fmPalette) private var fm
    let model: StrataModel
    let drill: StrataDrillGraph
    let query: String
    let open: (StrataFocus) -> Void

    var body: some View {
        let files = drill.nodes.filter { !$0.module && Strata.matches(model.snapshot.files[$0.index].path, query) }
        let uses = drill.nodes.filter { $0.module && !$0.dependent }
        let usedBy = drill.nodes.filter { $0.module && $0.dependent }
        LazyVStack(alignment: .leading, spacing: 0) {
            section("Files", count: files.count + drill.hiddenFiles)
            ForEach(files, id: \.index) { (node: StrataDrillNode) in
                let file = model.snapshot.files[node.index]
                FmListRow(title: Strata.fileName(file.path), subtitle: Strata.drillCaption(model, node),
                          leading: AnyView(StrataRatingChip(rating: model.report.files[node.index].rating)),
                          onTap: { open(.file(node.index)) }, minHeight: 48)
            }
            if drill.hiddenFiles > 0 {
                Text("\(drill.hiddenFiles) more files are not drawn; the module's details list them all.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(FmSpace.x4)
            }
            section("Uses", count: uses.count)
            ForEach(uses, id: \.index) { (node: StrataDrillNode) in moduleRow(node) }
            section("Used by", count: usedBy.count)
            ForEach(usedBy, id: \.index) { (node: StrataDrillNode) in moduleRow(node) }
        }
    }

    private func section(_ title: String, count: Int) -> some View {
        FmSectionHeader(title: title, count: count)
            .padding(.horizontal, FmSpace.x4)
            .padding(.top, FmSpace.x3)
            .padding(.bottom, FmSpace.x1)
    }

    private func moduleRow(_ node: StrataDrillNode) -> some View {
        let mod = model.report.graph.nodes[node.index]
        return FmListRow(title: StrataText.moduleName(mod), subtitle: Strata.drillCaption(model, node),
                         leading: AnyView(StrataRatingChip(rating: model.report.modules[node.index].rating)),
                         onTap: { open(.module(node.index)) }, minHeight: 48)
    }
}

struct StrataSelectionCard: View {
    @Environment(\.fmPalette) private var fm
    let model: StrataModel
    let focus: StrataFocus
    let drilled: Int
    let open: (StrataFocus) -> Void
    let openFile: (String, Int) -> Void
    let showFiles: (Int) -> Void
    let clear: () -> Void

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            switch focus {
            case .module(let m):
                let score = model.report.modules[m]
                StrataRatingChip(rating: score.rating, size: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(StrataText.moduleName(model.report.graph.nodes[m])).fmText(.titleSmall).lineLimit(1).truncationMode(.middle)
                    Text("\(Strata.formatPercent(score.ratio)) debt ratio · \(Strata.formatDebt(score.debt)) debt")
                        .fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                }
                Spacer(minLength: 0)
                if drilled != m {
                    FmTextButton("Files", icon: "account_tree", minHeight: 36) { showFiles(m) }
                }
                FmTextButton("Details", minHeight: 36) { open(.module(m)) }
            case .file(let f):
                let score = model.report.files[f]
                StrataRatingChip(rating: score.rating, size: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(Strata.fileName(model.snapshot.files[f].path)).fmText(.titleSmall).lineLimit(1).truncationMode(.middle)
                    Text("\(Strata.formatPercent(score.ratio)) debt ratio · \(Strata.formatDebt(score.debt)) debt")
                        .fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                }
                Spacer(minLength: 0)
                FmTextButton("Open", icon: "open_in_new", minHeight: 36) { openFile(model.snapshot.files[f].path, 0) }
                FmTextButton("Details", minHeight: 36) { open(.file(f)) }
            case .cell:
                EmptyView()
            }
            Button(action: clear) {
                FmIcon("close", size: 18, color: fm.textSecondary).frame(width: 36, height: 36)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Clear the selection")
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgRaised)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .shadow(color: Color.black.opacity(0.18), radius: 10, y: 4)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("strata-selection")
    }
}
