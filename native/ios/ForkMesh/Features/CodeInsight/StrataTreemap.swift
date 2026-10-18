import FMCodeInsight
import SwiftUI

struct StrataTreemapKey: Equatable {
    let stamp: Int
    let sizeBy: StrataTreemapSize
    let width: Int
    let height: Int
}

struct StrataTreemapView: View {
    let model: StrataModel
    let stamp: Int
    let query: String
    @Binding var focus: StrataFocus?
    let open: (StrataFocus) -> Void
    let openFile: (String, Int) -> Void
    let showFiles: (Int) -> Void

    @Environment(\.fmPalette) private var fm
    @State private var sizeBy: StrataTreemapSize = .lines
    @State private var colorBy: StrataTreemapColor = .health
    @State private var map: StrataTreemap?
    @State private var size: CGSize = .zero
    @State private var zoom = StrataZoom()
    @State private var live = StrataLiveGesture()
    @State private var showList = false

    var body: some View {
        VStack(spacing: 0) {
            bar
            if showList {
                ScrollView {
                    StrataModuleList(model: model, query: query, open: open)
                        .background(fm.bgRaised)
                        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
                        .padding(FmSpace.x4)
                }
            } else {
                ZStack(alignment: .bottom) {
                    canvas
                    if let focus {
                        StrataSelectionCard(model: model, focus: focus, drilled: -1, open: open, openFile: openFile,
                                            showFiles: showFiles, clear: { self.focus = nil })
                            .padding(FmSpace.x3)
                    }
                }
            }
            legend
        }
    }

    private var bar: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                Menu {
                    Picker("Size", selection: $sizeBy) {
                        ForEach(StrataTreemapSize.allCases, id: \.self) { (option: StrataTreemapSize) in
                            Text(option.label).tag(option)
                        }
                    }
                } label: {
                    StrataMenuLabel(icon: "aspect_ratio", text: "Size: \(sizeBy.label)")
                }
                .accessibilityLabel("Size by \(sizeBy.label)")
                Menu {
                    Picker("Colour", selection: $colorBy) {
                        ForEach(StrataTreemapColor.allCases, id: \.self) { (option: StrataTreemapColor) in
                            Text(option.label).tag(option)
                        }
                    }
                } label: {
                    StrataMenuLabel(icon: "palette", text: "Colour: \(colorBy.label)")
                }
                .accessibilityLabel("Colour by \(colorBy.label)")
                Button {
                    FmHaptics.selection()
                    zoom = StrataZoom()
                } label: {
                    StrataMenuLabel(icon: "fit_screen", text: "Fit")
                }
                .buttonStyle(.plain)
                .disabled(showList)
                Button {
                    FmHaptics.selection()
                    showList.toggle()
                } label: {
                    StrataMenuLabel(icon: showList ? "view_quilt" : "list", text: showList ? "Treemap" : "List", highlighted: showList)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("strata-treemap-list")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private var legend: some View {
        let maxCommits = Int(Strata.maxCommits(model.snapshot))
        let ends: (String, String)
        switch colorBy {
        case .health: ends = ("Healthy", "Unhealthy")
        case .maxComplexity: ends = ("≤ 4", "30+")
        case .debt: ends = ("0 %", "50 %+")
        case .changeFrequency: ends = ("0", "\(maxCommits) commits")
        }
        let colors: [Color] = (0...10).map { (i: Int) -> Color in
            let t = Double(i) / 10
            return colorBy == .changeFrequency ? StrataPalette.change(t, dark: fm.dark).color : StrataPalette.scale(t).color
        }
        return HStack(spacing: FmSpace.x2) {
            Text(ends.0).fmText(.labelSmall, color: fm.textSecondary)
            StrataGradientBar(colors: colors).frame(maxWidth: 140)
            Text(ends.1).fmText(.labelSmall, color: fm.textSecondary)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(colorBy.label) from \(ends.0) to \(ends.1)")
    }

    private var canvas: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let view = zoom.applying(live)
            let tones = StrataTones(fm)
            let maxCommits = Strata.maxCommits(model.snapshot)
            Canvas { (context: inout GraphicsContext, canvasSize: CGSize) in
                guard let map else { return }
                paint(&context, map: map, size: canvasSize, view: view, tones: tones, maxCommits: maxCommits)
            }
            .background(tones.canvas)
            .contentShape(Rectangle())
            .gesture(
                SpatialTapGesture(count: 2)
                    .onEnded { (value: SpatialTapGesture.Value) in doubleTap(value.location) }
                    .exclusively(before: SpatialTapGesture(count: 1).onEnded { (value: SpatialTapGesture.Value) in tap(value.location) })
            )
            .strataZoomPan($zoom, live: $live, maxScale: 30)
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Treemap of \(model.snapshot.files.count) files in \(model.report.graph.nodes.count) modules")
            .accessibilityHint("The List button shows every module and its files")
            .accessibilityChildren {
                ForEach(model.order, id: \.self) { (m: Int) in
                    Text("\(StrataText.moduleName(model.report.graph.nodes[m])), \(Strata.moduleCaption(model.report.graph.nodes[m]))")
                        .accessibilityAddTraits(.isButton)
                        .accessibilityAction { open(.module(m)) }
                }
            }
            .onAppear { size = proxy.size }
            .onChange(of: proxy.size) { (_: CGSize, newSize: CGSize) in size = newSize }
            .task(id: StrataTreemapKey(stamp: stamp, sizeBy: sizeBy, width: Int(proxy.size.width), height: Int(proxy.size.height))) {
                let area = StrataRect(6, 6, max(0, proxy.size.width - 12), max(0, proxy.size.height - 12))
                let snapshot = model.snapshot
                let graph = model.report.graph
                let measure = sizeBy
                let built: StrataTreemap = await Task.detached(priority: .userInitiated) {
                    Strata.treemap(snapshot, graph, sizeBy: measure, area: area)
                }.value
                if !Task.isCancelled { map = built }
            }
        }
    }

    private func paint(_ context: inout GraphicsContext, map: StrataTreemap, size: CGSize, view: StrataZoom, tones: StrataTones, maxCommits: Double) {
        var ctx = context
        ctx.translateBy(x: view.offset.width, y: view.offset.height)
        ctx.scaleBy(x: view.scale, y: view.scale)
        let scale = view.scale
        let visible = CGRect(x: -view.offset.width / scale, y: -view.offset.height / scale, width: size.width / scale, height: size.height / scale)
        let nodes = model.report.graph.nodes
        let files = model.snapshot.files
        for m in nodes.indices where m < map.modules.count {
            let box = map.modules[m].cg
            if box.width < 2 || box.height < 2 || !box.intersects(visible) { continue }
            ctx.fill(Path(box.insetBy(dx: 0.5, dy: 0.5)), with: .color(tones.surface))
            for f in nodes[m].files {
                let cell = map.files[f].cg
                if cell.width < 0.6 || cell.height < 0.6 || !cell.intersects(visible) { continue }
                let rgb = Strata.treemapColor(model, f, by: colorBy, maxCommits: maxCommits, dark: tones.dark)
                let match = query.isEmpty || Strata.matches(files[f].path, query)
                let inset = cell.width > 3 && cell.height > 3 ? cell.insetBy(dx: 0.5 / scale, dy: 0.5 / scale) : cell
                ctx.fill(Path(inset), with: .color(rgb.color.opacity(match ? 1 : 0.18)))
                if inset.width * scale > 64 && inset.height * scale > 18 {
                    let fontSize = 10.5 / scale
                    let maxChars = max(3, Int(inset.width * scale / 6))
                    let name = Strata.fileName(files[f].path)
                    let shown = name.count > maxChars ? String(name.prefix(maxChars / 2 - 1)) + "…" + String(name.suffix(maxChars - maxChars / 2)) : name
                    ctx.draw(Text(shown).font(.system(size: fontSize)).foregroundColor(rgb.readable.opacity(0.92)),
                             at: CGPoint(x: inset.minX + 5 / scale, y: inset.minY + 2 / scale), anchor: .topLeading)
                }
            }
            ctx.stroke(Path(box), with: .color(tones.canvas), lineWidth: 2 / scale)
            if m < map.headers.count && map.headers[m] {
                ctx.draw(Text(StrataText.moduleName(nodes[m])).font(.system(size: 11, weight: .semibold)).foregroundColor(tones.text),
                         at: CGPoint(x: box.minX + 6, y: box.minY + 9), anchor: .leading)
            }
        }
        switch focus {
        case .file(let f)? where f < map.files.count:
            let cell = map.files[f].cg
            ctx.stroke(Path(cell.insetBy(dx: -1 / scale, dy: -1 / scale)), with: .color(tones.accent), lineWidth: 2.6 / scale)
        case .module(let m)? where m < map.modules.count:
            ctx.stroke(Path(map.modules[m].cg), with: .color(tones.accent), lineWidth: 2.6 / scale)
        default:
            break
        }
    }

    private func hit(_ location: CGPoint) -> (module: Int, file: Int) {
        guard let map else { return (-1, -1) }
        let point = zoom.world(location)
        let nodes = model.report.graph.nodes
        for m in nodes.indices where m < map.modules.count && map.modules[m].cg.contains(point) {
            for f in nodes[m].files where map.files[f].cg.contains(point) { return (m, f) }
            return (m, -1)
        }
        return (-1, -1)
    }

    private func tap(_ location: CGPoint) {
        let target = hit(location)
        FmHaptics.selection()
        if target.file >= 0 {
            focus = .file(target.file)
        } else if target.module >= 0 {
            focus = .module(target.module)
        } else {
            focus = nil
        }
    }

    private func doubleTap(_ location: CGPoint) {
        let target = hit(location)
        if target.file >= 0 {
            open(.file(target.file))
        } else if target.module >= 0 {
            open(.module(target.module))
        } else {
            zoom = StrataZoom()
        }
    }
}
