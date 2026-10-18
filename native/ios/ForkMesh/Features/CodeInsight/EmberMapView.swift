import FMCodeInsight
import SwiftUI

struct EmberMapPainter {
    let snapshot: InsightSnapshot
    let layout: EmberLayout
    let coloring: EmberColoring
    let coupling: EmberCouplingIndex
    let view: EmberView
    let focus: Int
    let selected: Int
    let linkProgress: Double
    let glowPhase: Double
    let dark: Bool
    let fast: Bool

    static let maxFileLabels = 300
    static let maxFolderLabels = 40
    static let maxLinks = 40

    private var background: EmberColor { EmberStyle.background(dark: dark) }

    private func screen(_ node: EmberNode, _ size: CGSize) -> CGPoint {
        let p = Ember.toScreen(view, x: node.x, y: node.y, width: Double(size.width), height: Double(size.height))
        return CGPoint(x: p.x, y: p.y)
    }

    private func circle(_ c: CGPoint, _ r: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
    }

    private func gray(_ value: Double, _ alpha: Double) -> Color {
        Color(.sRGB, red: value / 255, green: value / 255, blue: value / 255, opacity: alpha / 255)
    }

    private func rgb(_ r: Double, _ g: Double, _ b: Double, _ alpha: Double) -> Color {
        Color(.sRGB, red: r / 255, green: g / 255, blue: b / 255, opacity: alpha / 255)
    }

    func paint(_ ctx: inout GraphicsContext, size: CGSize) {
        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(background.color))
        guard !layout.isEmpty else { return }
        let folderFill = dark ? gray(255, 10) : rgb(20, 30, 50, 10)
        let folderRim = dark ? gray(255, 34) : rgb(20, 30, 50, 42)
        let focusRim = dark ? gray(255, 60) : rgb(20, 30, 50, 70)
        let shadow = dark ? gray(0, 90) : rgb(30, 40, 60, 40)
        let markRing = dark ? gray(255, 200) : rgb(30, 35, 45, 200)
        var fileLabels: [Int] = []
        var folderLabels: [Int] = []
        var tiny = Path()
        var tinyColors: [(CGRect, Color)] = []
        for index in Ember.visibleNodes(layout, view: view, width: Double(size.width), height: Double(size.height)) {
            let node = layout.nodes[index]
            let c = screen(node, size)
            let r = CGFloat(node.r * view.k)
            if node.file < 0 {
                ctx.fill(circle(c, r), with: .color(folderFill))
                if r >= 3 {
                    ctx.stroke(circle(c, r), with: .color(index == focus ? focusRim : folderRim), lineWidth: index == focus ? 1.2 : 1)
                }
                if r >= 70 && index != 0 && folderLabels.count < Self.maxFolderLabels { folderLabels.append(index) }
                continue
            }
            let style = node.file < coloring.files.count ? coloring.files[node.file] : EmberFileStyle(color: Ember.neutral)
            let shades = Ember.fileFill(style, background: background, dark: dark)
            if r < 2.2 {
                let rect = CGRect(x: c.x - r * 0.89, y: c.y - r * 0.89, width: r * 1.78, height: r * 1.78)
                if fast {
                    tiny.addRect(rect)
                } else {
                    tinyColors.append((rect, shades.fill.color))
                }
                continue
            }
            if !fast && r >= 5 && !style.dimmed {
                ctx.fill(circle(CGPoint(x: c.x, y: c.y + max(1, r * 0.07)), r * 1.01), with: .color(shadow))
            }
            if !fast && r >= 4 {
                let center = CGPoint(x: c.x - r * 0.38, y: c.y - r * 0.45)
                let gradient = Gradient(stops: [
                    .init(color: shades.light.color, location: 0),
                    .init(color: shades.fill.color, location: 0.55),
                    .init(color: shades.shade.color, location: 1),
                ])
                ctx.fill(circle(c, r), with: .radialGradient(gradient, center: center, startRadius: 0, endRadius: r * 1.45))
            } else {
                ctx.fill(circle(c, r), with: .color(shades.fill.color))
            }
            if r >= 7 && !style.dimmed {
                ctx.stroke(circle(c, r - 0.5), with: .color(Ember.darken(shades.fill, 0.45).opacity(110 / 255)), lineWidth: 1)
            }
            if style.marked && r >= 3.5 {
                ctx.stroke(circle(c, r + 2.5), with: .color(markRing),
                           style: StrokeStyle(lineWidth: max(1, min(2, r * 0.06)), dash: [2, 2]))
            }
            if r >= 18 && fileLabels.count < Self.maxFileLabels { fileLabels.append(index) }
        }
        if fast {
            ctx.fill(tiny, with: .color(Ember.neutral.opacity(0.8)))
        } else {
            for (rect, color) in tinyColors { ctx.fill(Path(rect), with: .color(color)) }
        }
        paintLabels(&ctx, size: size, files: fileLabels, folders: folderLabels)
        paintGlow(&ctx, size: size)
        paintLinks(&ctx, size: size)
        paintBadges(&ctx, size: size)
    }

    private func elide(_ text: String, fontSize: CGFloat, room: CGFloat) -> String {
        let fits = Int(room / (fontSize * 0.56))
        if text.count <= fits { return text }
        if fits <= 1 { return "" }
        return String(text.prefix(fits - 1)) + "…"
    }

    private func paintLabels(_ ctx: inout GraphicsContext, size: CGSize, files: [Int], folders: [Int]) {
        var labels = ctx
        if !fast { labels.addFilter(.shadow(color: Color.black.opacity(0.55), radius: 1.2)) }
        let folderText = dark ? rgb(230, 237, 243, 215) : rgb(31, 35, 40, 220)
        if !fast {
            for index in folders {
                let node = layout.nodes[index]
                let c = screen(node, size)
                let r = CGFloat(node.r * view.k)
                let fontSize = max(11, min(14, r * 0.05))
                let baseline = r - fontSize * 0.8 * 0.35 - max(3, r * 0.012)
                let top = CGPoint(x: c.x, y: c.y - baseline)
                if top.x < -40 || top.x > size.width + 40 || top.y < -10 || top.y > size.height + 10 { continue }
                paintCurved(&ctx, center: c, radius: baseline, text: node.name, fontSize: fontSize, color: folderText)
            }
        }
        let dimmedText = dark ? rgb(230, 237, 243, 85) : rgb(31, 35, 40, 110)
        for index in files {
            let node = layout.nodes[index]
            let c = screen(node, size)
            let r = CGFloat(node.r * view.k)
            let dimmed = node.file < coloring.files.count && coloring.files[node.file].dimmed
            let room = r * 1.86
            var fontSize = 8 + max(0, min(7, (r * 0.24 - 8).rounded(.down)))
            while fontSize > 8 && CGFloat(node.name.count) * fontSize * 0.56 > room { fontSize -= 1 }
            let name = elide(node.name, fontSize: fontSize, room: room)
            if name.isEmpty || (name.count < 5 && name != node.name) { continue }
            let showMetric = r >= 38 && !fast
            let lineHeight = fontSize * 1.2
            let nameY = c.y - (showMetric ? lineHeight * 0.45 : 0)
            let text = Text(name).font(.system(size: fontSize)).foregroundStyle(dimmed ? dimmedText : Color.white)
            if dimmed {
                ctx.draw(text, at: CGPoint(x: c.x, y: nameY), anchor: .center)
            } else {
                labels.draw(text, at: CGPoint(x: c.x, y: nameY), anchor: .center)
            }
            if showMetric {
                let metric = Ember.metricLabel(snapshot, tab: coloring.tab, coupling: coupling, file: node.file)
                if !metric.isEmpty {
                    let metricText = Text(elide(metric, fontSize: 10, room: r * 1.5)).font(.system(size: 10))
                        .foregroundStyle(dimmed ? dimmedText : Color.white.opacity(200 / 255))
                    labels.draw(metricText, at: CGPoint(x: c.x, y: nameY + lineHeight * 0.5 + 6), anchor: .center)
                }
            }
        }
    }

    private func paintCurved(_ ctx: inout GraphicsContext, center: CGPoint, radius: CGFloat, text: String, fontSize: CGFloat, color: Color) {
        let advance = fontSize * 0.6
        let maxSpan = CGFloat.pi * 0.8
        var label = text
        let fits = Int(maxSpan * radius / advance)
        if CGFloat(label.count) * advance / radius > maxSpan {
            guard fits > 3 else { return }
            let head = (fits - 1) / 2
            label = String(text.prefix(head)) + "…" + String(text.suffix(fits - 1 - head))
        }
        if label.isEmpty { return }
        let width = CGFloat(label.count) * advance
        var angle = -CGFloat.pi / 2 - (width / radius) / 2
        var curved = ctx
        curved.addFilter(.shadow(color: dark ? rgb(8, 10, 14, 200) : rgb(255, 255, 255, 220), radius: 1.2))
        for ch in label {
            let mid = angle + (advance / 2) / radius
            var glyph = curved
            glyph.translateBy(x: center.x + radius * cos(mid), y: center.y + radius * sin(mid))
            glyph.rotate(by: .radians(Double(mid + .pi / 2)))
            glyph.draw(Text(String(ch)).font(.system(size: fontSize, weight: .semibold)).foregroundStyle(color), at: .zero, anchor: .center)
            angle += advance / radius
        }
    }

    private func paintGlow(_ ctx: inout GraphicsContext, size: CGSize) {
        guard coloring.tab == .hotspots, coloring.glowing > 0 else { return }
        let glow = Ember.topHotspotGlow
        for (i, style) in coloring.files.enumerated() where style.glow > 0 && !style.dimmed {
            let index = layout.fileNode(i)
            guard index >= 0 else { continue }
            let r = CGFloat(layout.nodes[index].r * view.k)
            if r < 2.5 { continue }
            let c = screen(layout.nodes[index], size)
            let breathe = 0.5 + 0.5 * sin(glowPhase + Double(i) * 0.7)
            let outer = r + max(6, r * CGFloat(0.22 + 0.12 * breathe))
            if c.x < -outer || c.y < -outer || c.x > size.width + outer || c.y > size.height + outer { continue }
            let inner = r / outer
            let gradient = Gradient(stops: [
                .init(color: glow.opacity(0), location: max(0, inner - 0.02)),
                .init(color: glow.opacity((90 + 90 * breathe) * style.glow / 255), location: inner),
                .init(color: glow.opacity(0), location: 1),
            ])
            ctx.fill(circle(c, outer), with: .radialGradient(gradient, center: c, startRadius: 0, endRadius: outer))
        }
    }

    private func paintLinks(_ ctx: inout GraphicsContext, size: CGSize) {
        guard selected >= 0 else { return }
        let selNode = layout.fileNode(selected)
        guard selNode >= 0 else { return }
        let a = screen(layout.nodes[selNode], size)
        let ra = CGFloat(layout.nodes[selNode].r * view.k)
        let link = Ember.couplingLink
        let p = Ember.easeInOut(linkProgress)
        var labels: [(CGPoint, String)] = []
        if selected < coupling.count {
            for ref in coupling[selected].prefix(Self.maxLinks) {
                let other = layout.fileNode(ref.file)
                guard other >= 0 else { continue }
                let b = screen(layout.nodes[other], size)
                let rb = CGFloat(layout.nodes[other].r * view.k)
                let dx = b.x - a.x
                let dy = b.y - a.y
                let length = hypot(dx, dy)
                if length < 1 { continue }
                let control = (x: Double((a.x + b.x) / 2 - dy / length * length * 0.22), y: Double((a.y + b.y) / 2 + dx / length * length * 0.22))
                var path = Path()
                path.move(to: a)
                let steps = 36
                let upto = max(1, Int((Double(steps) * p).rounded(.up)))
                for s in 1...upto {
                    let point = Ember.bezier((Double(a.x), Double(a.y)), control, (Double(b.x), Double(b.y)), min(p, Double(s) / Double(steps)))
                    path.addLine(to: CGPoint(x: point.x, y: point.y))
                }
                let w = 1.2 + 3.8 * CGFloat(max(0, min(1, ref.degree)))
                ctx.stroke(path, with: .color((dark ? gray(0, 110) : gray(255, 110))), style: StrokeStyle(lineWidth: w + 2.5, lineCap: .round))
                ctx.stroke(path, with: .color(link.opacity((110 + 130 * ref.degree) / 255)), style: StrokeStyle(lineWidth: w, lineCap: .round))
                if p >= 1 {
                    let ring = max(rb + 1.5, 3)
                    if rb < 3 { ctx.fill(circle(b, ring), with: .color(link.color)) }
                    ctx.stroke(circle(b, ring), with: .color(link.color), lineWidth: 2)
                    if length > 70 {
                        let mid = Ember.bezier((Double(a.x), Double(a.y)), control, (Double(b.x), Double(b.y)), 0.5)
                        labels.append((CGPoint(x: mid.x, y: mid.y), Ember.percentText(ref.degree)))
                    }
                }
            }
        }
        for (at, label) in labels {
            let w = CGFloat(label.count) * 6 + 10
            let pill = Path(roundedRect: CGRect(x: at.x - w / 2, y: at.y - 8, width: w, height: 16), cornerRadius: 8)
            ctx.fill(pill, with: .color(dark ? rgb(22, 27, 34, 235) : rgb(255, 255, 255, 240)))
            ctx.draw(Text(label).font(.system(size: 10, weight: .bold)).foregroundStyle(dark ? link.color : Ember.darken(link, 0.45).color),
                     at: at, anchor: .center)
        }
        ctx.stroke(circle(a, ra + 1.5), with: .color(dark ? rgb(13, 17, 23, 255) : rgb(31, 35, 40, 255)), lineWidth: 3)
        ctx.stroke(circle(a, ra + 3.5), with: .color(dark ? rgb(240, 246, 252, 255) : Color.white), lineWidth: 1.5)
        if ra < 8 {
            let pulse = 10 + 6 * CGFloat(1 - p)
            ctx.stroke(circle(a, ra + pulse), with: .color(dark ? rgb(240, 246, 252, 160) : rgb(31, 35, 40, 160)), lineWidth: 1.2)
        }
    }

    private func paintBadges(_ ctx: inout GraphicsContext, size: CGSize) {
        guard coloring.tab == .refactoringTargets, !coloring.targets.isEmpty else { return }
        let radius: CGFloat = 8.5
        for target in coloring.targets {
            let index = layout.fileNode(target.file)
            guard index >= 0 else { continue }
            let c = screen(layout.nodes[index], size)
            let r = CGFloat(layout.nodes[index].r * view.k)
            let at = r >= 10 ? CGPoint(x: c.x + r * 0.72, y: c.y - r * 0.72) : CGPoint(x: c.x + r + radius - 2, y: c.y - r - radius + 2)
            if at.x < -12 || at.y < -12 || at.x > size.width + 12 || at.y > size.height + 12 { continue }
            ctx.fill(circle(at, radius + 1), with: .color(background.color))
            ctx.fill(circle(at, radius), with: .color(dark ? rgb(240, 246, 252, 255) : rgb(31, 35, 40, 255)))
            ctx.draw(Text("\(target.rank)").font(.system(size: 10, weight: .bold))
                .foregroundStyle(dark ? rgb(13, 17, 23, 255) : Color.white), at: at, anchor: .center)
        }
    }
}

struct EmberMapView: View {
    let model: EmberPageModel
    let onHelp: () -> Void
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityVoiceOverEnabled) private var voiceOver
    @State private var lastDrag: CGSize = .zero
    @State private var lastScale: CGFloat = 1
    @State private var dragging = false
    @State private var pinching = false

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size
            ZStack {
                if let layout = model.layout {
                    canvas(layout)
                        .gesture(tapGesture)
                        .simultaneousGesture(panZoomGesture)
                        .accessibilityLabel("Hotspot map, \(model.coloring.tab.title)")
                        .accessibilityHint("Swipe to reach files and folders. The List view ranks every file.")
                        .accessibilityChildren {
                            if voiceOver { accessibleChildren(size) }
                        }
                }
            }
            .onAppear { model.resize(size) }
            .onChange(of: size) { (_: CGSize, newSize: CGSize) in model.resize(newSize) }
        }
        .clipped()
        .overlay(alignment: .topLeading) { EmberLegend(coloring: model.coloring, onHelp: onHelp).padding(FmSpace.x2) }
        .overlay(alignment: .bottomTrailing) { zoomControls.padding(FmSpace.x2) }
        .overlay(alignment: .top) {
            if !model.coloring.note.isEmpty {
                Text(model.coloring.note)
                    .fmText(.bodySmall)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    .background(fm.sheetSurface.opacity(0.94))
                    .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
                    .padding(.top, 52)
                    .padding(.horizontal, FmSpace.x4)
                    .allowsHitTesting(false)
            }
        }
    }

    private func canvas(_ layout: EmberLayout) -> some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !model.animating)) { timeline in
            let date = timeline.date
            let painter = EmberMapPainter(
                snapshot: model.snapshot, layout: layout, coloring: model.coloring, coupling: model.coupling,
                view: model.currentView(at: date), focus: model.focus, selected: model.selected,
                linkProgress: model.linkProgress(at: date),
                glowPhase: model.reduceMotion ? 0 : date.timeIntervalSinceReferenceDate * 2 * Double.pi / 2.4,
                dark: fm.dark, fast: model.interacting || model.transition != nil)
            Canvas(opaque: true) { (ctx: inout GraphicsContext, canvasSize: CGSize) in
                painter.paint(&ctx, size: canvasSize)
            }
        }
    }

    private func accessibleChildren(_ size: CGSize) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(model.accessibleNodes()) { (item: EmberAccessibleNode) in
                Circle()
                    .frame(width: max(24, item.diameter), height: max(24, item.diameter))
                    .position(x: item.x, y: item.y)
                    .accessibilityLabel(item.label)
                    .accessibilityAddTraits(item.file >= 0 && item.file == model.selected ? [.isButton, .isSelected] : .isButton)
                    .accessibilityAction { model.activate(item) }
            }
        }
        .frame(width: size.width, height: size.height)
    }

    private var tapGesture: some Gesture {
        SpatialTapGesture(count: 2)
            .onEnded { (_: SpatialTapGesture.Value) in model.fitAll() }
            .exclusively(before: SpatialTapGesture(count: 1).onEnded { (value: SpatialTapGesture.Value) in
                model.tap(at: value.location)
            })
    }

    private var panZoomGesture: some Gesture {
        let drag = DragGesture(minimumDistance: 4)
            .onChanged { (value: DragGesture.Value) in
                if !dragging {
                    dragging = true
                    lastDrag = .zero
                    model.beginGesture()
                }
                let dx = value.translation.width - lastDrag.width
                let dy = value.translation.height - lastDrag.height
                lastDrag = value.translation
                model.panBy(dx: Double(dx), dy: Double(dy))
            }
            .onEnded { (_: DragGesture.Value) in
                dragging = false
                lastDrag = .zero
                if !pinching { model.settle() }
            }
        let pinch = MagnifyGesture()
            .onChanged { (value: MagnifyGesture.Value) in
                if !pinching {
                    pinching = true
                    lastScale = 1
                    model.beginGesture()
                }
                let factor = value.magnification / lastScale
                lastScale = value.magnification
                model.pinch(by: Double(factor), at: value.startLocation)
            }
            .onEnded { (_: MagnifyGesture.Value) in
                pinching = false
                lastScale = 1
                if !dragging { model.settle() }
            }
        return drag.simultaneously(with: pinch)
    }

    private var zoomControls: some View {
        VStack(spacing: FmSpace.x1) {
            control("plus", label: "Zoom in") { model.zoomBy(1.6) }
            control("minus", label: "Zoom out") { model.zoomBy(1 / 1.6) }
            control("arrow.up.left.and.arrow.down.right", label: "Show the whole repository") { model.fitAll() }
        }
    }

    private func control(_ symbol: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(fm.textSecondary)
                .frame(width: 40, height: 40)
                .background(fm.sheetSurface.opacity(0.92))
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}

struct EmberLegend: View {
    let coloring: EmberColoring
    let onHelp: () -> Void
    @Environment(\.fmPalette) private var fm

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Button(action: onHelp) {
                FmIcon("info_outline", size: 18, color: fm.textSecondary)
                    .frame(width: 32, height: 32)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("How Ember scores files")
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x3) {
                    if coloring.scale, let low = coloring.scaleLow, let high = coloring.scaleHigh {
                        Text(coloring.scaleLowLabel).fmText(.labelSmall, color: fm.textSecondary)
                        Capsule()
                            .fill(LinearGradient(colors: coloring.tab == .hotspots ? [low.color, Ember.hotspotMid.color, high.color]
                                                    : [low.color, high.color], startPoint: .leading, endPoint: .trailing))
                            .frame(width: 48, height: 8)
                        Text(coloring.scaleHighLabel).fmText(.labelSmall, color: fm.textSecondary)
                    }
                    ForEach(coloring.legend, id: \.self) { (entry: EmberLegendEntry) in
                        HStack(spacing: FmSpace.x1) {
                            if entry.ring {
                                if let color = entry.color {
                                    Circle().stroke(color.color, lineWidth: 3).frame(width: 10, height: 10)
                                } else {
                                    Circle().stroke(fm.textSecondary, style: StrokeStyle(lineWidth: 1.2, dash: [2, 2])).frame(width: 10, height: 10)
                                }
                            } else {
                                Circle().fill(entry.color?.color ?? fm.textTertiary).frame(width: 9, height: 9)
                            }
                            Text(entry.label).fmText(.labelSmall, color: fm.textSecondary).lineLimit(1)
                        }
                    }
                }
                .padding(.trailing, FmSpace.x3)
            }
        }
        .frame(height: 32)
        .padding(.leading, FmSpace.x1)
        .background(fm.sheetSurface.opacity(0.92))
        .clipShape(Capsule())
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Legend")
    }
}
