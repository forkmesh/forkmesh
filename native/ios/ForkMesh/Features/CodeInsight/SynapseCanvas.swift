import CoreGraphics
import FMCodeInsight
import Foundation
import SwiftUI

struct SynapseBrainView: View {
    let model: SynapseModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var renderer = SynapseRenderer()
    @State private var dragLast: CGSize?
    @State private var pinchStart: SynapseCamera?

    var body: some View {
        let viewport = model.viewport
        let renderer = renderer
        let revision = model.revision
        let autoRotate = model.autoRotate
        let moving = model.animating
        let reduce = reduceMotion
        let model = model
        TimelineView(.animation(minimumInterval: nil, paused: !moving)) { timeline in
            Canvas(opaque: true) { context, size in
                viewport.reduceMotion = reduce
                let landed = renderer.draw(&context, size: size, viewport: viewport, revision: revision,
                                           time: SynapseViewport.now(), clock: timeline.date.timeIntervalSinceReferenceDate,
                                           autoRotate: autoRotate, moving: moving, reduceMotion: reduce)
                if landed {
                    Task { @MainActor in model.landed() }
                }
            }
        }
        .contentShape(Rectangle())
        .gesture(orbit.simultaneously(with: zoom))
        .onTapGesture(count: 2) { location in model.doubleTap(at: location) }
        .onTapGesture { location in model.tap(at: location) }
        .sensoryFeedback(.selection, trigger: model.selected)
        .accessibilityElement()
        .accessibilityLabel("Import graph")
        .accessibilityValue(model.summary)
        .accessibilityHint("The overview lists every file, import and circular dependency.")
        .accessibilityAction(named: "Show overview") {
            model.clearSelection()
            model.sheetShown = true
        }
        .accessibilityAction(named: "Fit") { model.fit(animate: true) }
        .onAppear { model.viewport.reduceMotion = reduceMotion }
        .onChange(of: reduceMotion) { _, value in model.viewport.reduceMotion = value }
    }

    private var orbit: some Gesture {
        DragGesture(minimumDistance: 4)
            .onChanged { value in
                let viewport = model.viewport
                guard pinchStart == nil else {
                    dragLast = value.translation
                    return
                }
                let last = dragLast ?? .zero
                dragLast = value.translation
                viewport.touching = true
                viewport.flight = nil
                viewport.camera = viewport.camera.orbited(dx: Float(value.translation.width - last.width),
                                                          dy: Float(value.translation.height - last.height))
                viewport.pause()
                if !model.interacting { model.interacting = true }
            }
            .onEnded { _ in
                dragLast = nil
                finishTouch()
            }
    }

    private var zoom: some Gesture {
        MagnifyGesture()
            .onChanged { value in
                let viewport = model.viewport
                let start = pinchStart ?? viewport.camera
                if pinchStart == nil { pinchStart = start }
                viewport.touching = true
                viewport.flight = nil
                viewport.camera = start.zoomed(by: Float(1 / max(0.05, value.magnification)),
                                               towardX: Float(value.startLocation.x), y: Float(value.startLocation.y),
                                               width: Float(viewport.size.width), height: Float(viewport.size.height))
                viewport.pause()
                if !model.interacting { model.interacting = true }
            }
            .onEnded { _ in
                pinchStart = nil
                finishTouch()
            }
    }

    private func finishTouch() {
        let viewport = model.viewport
        guard dragLast == nil && pinchStart == nil else { return }
        viewport.touching = false
        viewport.pause()
        model.interacting = false
        model.landed()
    }
}

final class SynapseRenderer {
    private struct Bucket {
        var colour: Color
        var width: CGFloat
        var glow: Bool
    }

    private static let fogLevels: [Double] = [0.45, 0.7, 0.95]
    private static let opacityLevels: [Double] = [0.3, 0.55, 0.8, 1.0]
    private static let starColour = SynapseColour(bytes: 170, 196, 255)

    private var preparedRevision = -1
    private var preparedGraphNodes = -1
    private var edgeBucket: [Int] = []
    private var buckets: [Bucket] = []
    private var bucketPaths: [Path] = []
    private var plainEdges = 0
    private var pulseCandidates = Set<Int>()
    private var nodePaletteIndex: [Int] = []
    private var nodePalette: [SynapseColour] = []
    private var nodePaletteColours: [Color] = []
    private var dotPaths: [Path] = []
    private var sprites: [UInt32: Image] = [:]
    private var halos: [UInt32: Image] = [:]
    private var resolvedSprites: [GraphicsContext.ResolvedImage?] = []
    private var resolvedHalos: [GraphicsContext.ResolvedImage?] = []
    private var large: [Int] = []
    private var pulseEdges: [Int] = []
    private var pulsePoints: [CGPoint] = []
    private var pulsePaths: [Path] = Array(repeating: Path(), count: 16)
    private var starPaths: [Path] = Array(repeating: Path(), count: 3)
    private var dustPaths: [Path] = Array(repeating: Path(), count: 3)
    private var taken: [CGRect] = []

    static func color(_ c: SynapseColour, alpha: Double? = nil) -> Color {
        Color(.sRGB, red: Double(c.r), green: Double(c.g), blue: Double(c.b), opacity: alpha ?? Double(c.a))
    }

    static func cg(_ c: SynapseColour) -> CGColor {
        CGColor(srgbRed: CGFloat(c.r), green: CGFloat(c.g), blue: CGFloat(c.b), alpha: CGFloat(c.a))
    }

    private static func bitmap(_ side: Int, _ draw: (CGContext, CGFloat, CGPoint) -> Void) -> Image? {
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        let centre = CGPoint(x: CGFloat(side) / 2, y: CGFloat(side) / 2)
        draw(context, CGFloat(side) / 2 - 1, centre)
        guard let image = context.makeImage() else { return nil }
        return Image(decorative: image, scale: 1)
    }

    private static func gradient(_ stops: [(CGFloat, SynapseColour)]) -> CGGradient? {
        CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: stops.map { cg($0.1) } as CFArray,
                   locations: stops.map(\.0))
    }

    private static func sphere(_ colour: SynapseColour) -> Image? {
        bitmap(64) { context, r, centre in
            let rect = CGRect(x: centre.x - r, y: centre.y - r, width: r * 2, height: r * 2)
            context.saveGState()
            context.addEllipse(in: rect)
            context.clip()
            if let body = gradient([
                (0, colour.mixed(SynapsePalette.white, 0.55)),
                (0.32, colour.lighter(112)),
                (0.78, colour.darker(165)),
                (1, colour.darker(260)),
            ]) {
                context.drawRadialGradient(body, startCenter: CGPoint(x: centre.x - 0.38 * r, y: centre.y + 0.42 * r), startRadius: 0,
                                           endCenter: centre, endRadius: r, options: [.drawsAfterEndLocation])
            }
            if let rim = gradient([
                (0, colour.withAlpha(0)),
                (0.72, colour.withAlpha(0)),
                (0.93, colour.lighter(150).withAlpha(0.55)),
                (1, colour.withAlpha(0)),
            ]) {
                context.drawRadialGradient(rim, startCenter: centre, startRadius: 0, endCenter: centre, endRadius: r, options: [])
            }
            let spot = CGPoint(x: centre.x - 0.36 * r, y: centre.y + 0.4 * r)
            if let spec = gradient([(0, SynapseColour(1, 1, 1, 210.0 / 255)), (1, SynapseColour(1, 1, 1, 0))]) {
                context.drawRadialGradient(spec, startCenter: spot, startRadius: 0, endCenter: spot, endRadius: r * 0.42, options: [])
            }
            context.restoreGState()
        }
    }

    private static func halo(_ colour: SynapseColour) -> Image? {
        bitmap(64) { context, r, centre in
            if let glow = gradient([
                (0, colour.withAlpha(1)),
                (0.25, colour.withAlpha(0.45)),
                (0.6, colour.withAlpha(0.1)),
                (1, colour.withAlpha(0)),
            ]) {
                context.drawRadialGradient(glow, startCenter: centre, startRadius: 0, endCenter: centre, endRadius: r, options: [])
            }
        }
    }

    private func prepare(_ viewport: SynapseViewport, _ graph: SynapseGraph) {
        if preparedRevision == viewport.styleRevision && preparedGraphNodes == graph.nodes.count { return }
        preparedRevision = viewport.styleRevision
        preparedGraphNodes = graph.nodes.count
        let style = viewport.style
        edgeBucket = [Int](repeating: -1, count: graph.edges.count)
        buckets.removeAll(keepingCapacity: true)
        pulseCandidates.removeAll(keepingCapacity: true)
        plainEdges = 0
        var index: [UInt64: Int] = [:]
        for e in style.drawEdges where e < graph.edges.count && e < style.edgeState.count {
            let state = style.edgeState[e]
            if !state.highlighted { plainEdges += 1 }
            if style.edgePulses(graph, e) && pulseCandidates.count < 900 { pulseCandidates.insert(e) }
            let colour = style.edgeMid[e]
            let key = UInt64(colour.key) << 3 | UInt64(state.rawValue)
            if let existing = index[key] {
                edgeBucket[e] = existing
                continue
            }
            index[key] = buckets.count
            edgeBucket[e] = buckets.count
            buckets.append(Bucket(colour: Self.color(colour), width: CGFloat(style.edgeWidth(e)) * 0.8, glow: state.highlighted))
        }
        bucketPaths = Array(repeating: Path(), count: buckets.count * Self.fogLevels.count)
        nodePaletteIndex = [Int](repeating: 0, count: graph.nodes.count)
        nodePalette.removeAll(keepingCapacity: true)
        var nodeIndex: [UInt32: Int] = [:]
        for i in graph.nodes.indices where i < style.nodeColour.count {
            let colour = style.nodeColour[i].withAlpha(1)
            let key = colour.key
            if let existing = nodeIndex[key] {
                nodePaletteIndex[i] = existing
            } else {
                nodeIndex[key] = nodePalette.count
                nodePaletteIndex[i] = nodePalette.count
                nodePalette.append(colour)
            }
        }
        nodePaletteColours = nodePalette.map { Self.color($0) }
        dotPaths = Array(repeating: Path(), count: nodePalette.count * Self.opacityLevels.count)
    }

    private func sprite(_ palette: Int, _ context: inout GraphicsContext) -> GraphicsContext.ResolvedImage? {
        if let resolved = resolvedSprites[palette] { return resolved }
        let colour = nodePalette[palette]
        let key = colour.key
        if sprites[key] == nil {
            if sprites.count > 400 { sprites.removeAll() }
            sprites[key] = Self.sphere(colour)
        }
        guard let image = sprites[key] else { return nil }
        let resolved = context.resolve(image)
        resolvedSprites[palette] = resolved
        return resolved
    }

    private func glow(_ palette: Int, _ context: inout GraphicsContext) -> GraphicsContext.ResolvedImage? {
        if let resolved = resolvedHalos[palette] { return resolved }
        let colour = nodePalette[palette]
        let key = colour.key
        if halos[key] == nil {
            if halos.count > 400 { halos.removeAll() }
            halos[key] = Self.halo(colour)
        }
        guard let image = halos[key] else { return nil }
        let resolved = context.resolve(image)
        resolvedHalos[palette] = resolved
        return resolved
    }

    private func background(_ context: inout GraphicsContext, _ size: CGSize) {
        let rect = CGRect(origin: .zero, size: size)
        context.fill(Path(rect), with: .linearGradient(
            Gradient(colors: [Color(.sRGB, red: 11 / 255, green: 17 / 255, blue: 34 / 255), Color(.sRGB, red: 3 / 255, green: 5 / 255, blue: 10 / 255)]),
            startPoint: .zero, endPoint: CGPoint(x: 0, y: size.height)))
        let reach = (size.width * size.width + size.height * size.height).squareRoot() * 0.62
        context.fill(Path(rect), with: .radialGradient(
            Gradient(stops: [
                .init(color: .black.opacity(0), location: 0),
                .init(color: .black.opacity(0), location: 0.55),
                .init(color: .black.opacity(150 / 255), location: 1),
            ]),
            center: CGPoint(x: size.width / 2, y: size.height / 2), startRadius: 0, endRadius: reach))
    }

    func draw(_ context: inout GraphicsContext, size: CGSize, viewport: SynapseViewport, revision: Int, time: Double, clock: Double,
              autoRotate: Bool, moving: Bool, reduceMotion: Bool) -> Bool {
        viewport.size = size
        let landed = viewport.advance(to: time, autoRotate: autoRotate)
        background(&context, size)
        guard let graph = viewport.graph, viewport.positions.count == graph.nodes.count, !graph.nodes.isEmpty else { return landed }
        prepare(viewport, graph)
        let style = viewport.style
        let positions = viewport.positions
        let projection = SynapseProjection(camera: viewport.camera, width: Float(size.width), height: Float(size.height),
                                           boundingRadius: graph.shape.boundingRadius)
        let frame = viewport.frame
        frame.update(graph: graph, positions: positions, style: style, projection: projection)
        var glowContext = context
        glowContext.blendMode = .plusLighter

        let centre = projection.project(SynapseVector())
        if centre.depth > 1 {
            let radius = CGFloat(graph.shape.boundingRadius * 1.25 * projection.focal / centre.depth)
            let c = CGPoint(x: CGFloat(centre.x), y: CGFloat(centre.y))
            glowContext.fill(Path(ellipseIn: CGRect(x: c.x - radius, y: c.y - radius, width: radius * 2, height: radius * 2)),
                             with: .radialGradient(Gradient(stops: [
                                 .init(color: Color(.sRGB, red: 40 / 255, green: 72 / 255, blue: 160 / 255, opacity: 70 / 255), location: 0),
                                 .init(color: Color(.sRGB, red: 30 / 255, green: 50 / 255, blue: 120 / 255, opacity: 30 / 255), location: 0.5),
                                 .init(color: Color(.sRGB, red: 20 / 255, green: 30 / 255, blue: 80 / 255, opacity: 0), location: 1),
                             ]), center: c, startRadius: 0, endRadius: radius))
        }
        drawStars(&glowContext, viewport, projection, size)
        drawDust(&glowContext, viewport, projection)
        drawEdges(&glowContext, graph, style, frame, positions, projection, size, moving: moving)
        if style.input.pulses && !reduceMotion { drawPulses(&glowContext, graph, style, frame, clock) }
        drawNodes(&context, &glowContext, graph, style, frame, moving: moving, pulse: reduceMotion ? 0.5 : 0.5 + 0.5 * sin(clock * 4))
        drawLabels(&context, viewport, graph, style, frame, projection, size)
        return landed
    }

    private func drawStars(_ context: inout GraphicsContext, _ viewport: SynapseViewport, _ projection: SynapseProjection, _ size: CGSize) {
        for i in starPaths.indices { starPaths[i] = Path() }
        let w = Float(size.width)
        let h = Float(size.height)
        for star in viewport.stars {
            let p = projection.project(star.direction * SynapseCamera.starRadius)
            if p.depth <= 1 || p.x < 0 || p.y < 0 || p.x > w || p.y > h { continue }
            let level = star.brightness < 0.1 ? 0 : (star.brightness < 0.4 ? 1 : 2)
            let r = CGFloat(0.4 + 0.9 * star.brightness)
            starPaths[level].addEllipse(in: CGRect(x: CGFloat(p.x) - r, y: CGFloat(p.y) - r, width: r * 2, height: r * 2))
        }
        for (level, path) in starPaths.enumerated() {
            context.fill(path, with: .color(Self.color(Self.starColour, alpha: [0.2, 0.45, 0.85][level])))
        }
    }

    private func drawDust(_ context: inout GraphicsContext, _ viewport: SynapseViewport, _ projection: SynapseProjection) {
        for i in dustPaths.indices { dustPaths[i] = Path() }
        for point in viewport.dust {
            let p = projection.project(point)
            if p.depth <= 1 { continue }
            let fog = projection.fog(p.depth, strength: 0.8)
            let level = fog < 0.45 ? 0 : (fog < 0.75 ? 1 : 2)
            dustPaths[level].addEllipse(in: CGRect(x: CGFloat(p.x) - 0.8, y: CGFloat(p.y) - 0.8, width: 1.6, height: 1.6))
        }
        let base = SynapseColour(bytes: 120, 160, 235)
        for (level, path) in dustPaths.enumerated() {
            context.fill(path, with: .color(Self.color(base, alpha: 90.0 / 255 * [0.3, 0.6, 0.9][level])))
        }
    }

    private func drawEdges(_ context: inout GraphicsContext, _ graph: SynapseGraph, _ style: SynapseStyle, _ frame: SynapseFrame,
                           _ positions: [SynapseVector], _ projection: SynapseProjection, _ size: CGSize, moving: Bool) {
        for i in bucketPaths.indices { bucketPaths[i] = Path() }
        pulseEdges.removeAll(keepingCapacity: true)
        pulsePoints.removeAll(keepingCapacity: true)
        let budget = 2000
        let thin = moving && plainEdges > budget
        let stride = thin ? max(1, plainEdges / budget) : 1
        var plainIndex = 0
        let levels = Self.fogLevels.count
        let left: Float = -40
        let top: Float = -40
        let right = Float(size.width) + 40
        let bottom = Float(size.height) + 40
        let pulsing = style.input.pulses
        for e in style.drawEdges {
            let bucket = e < edgeBucket.count ? edgeBucket[e] : -1
            if bucket < 0 { continue }
            let state = style.edgeState[e]
            if thin && !state.highlighted {
                if state == .dim { continue }
                plainIndex += 1
                if plainIndex % stride != 0 { continue }
            }
            let edge = graph.edges[e]
            let a = edge.from
            let b = edge.to
            guard frame.visible[a] && frame.visible[b] else { continue }
            let pa = positions[a]
            let pb = positions[b]
            let mid = (pa + pb) * 0.5
            let control = projection.project(mid - mid * SynapseCamera.bow)
            if control.depth <= 0.5 { continue }
            let ax = frame.x[a], ay = frame.y[a], bx = frame.x[b], by = frame.y[b]
            let minX = min(ax, bx, control.x), maxX = max(ax, bx, control.x)
            let minY = min(ay, by, control.y), maxY = max(ay, by, control.y)
            if maxX < left || minX > right || maxY < top || minY > bottom { continue }
            let fog = 0.5 * (frame.fog[a] + frame.fog[b])
            let level = fog < 0.55 ? 0 : (fog < 0.8 ? 1 : 2)
            let start = CGPoint(x: CGFloat(ax), y: CGFloat(ay))
            let end = CGPoint(x: CGFloat(bx), y: CGFloat(by))
            let c = CGPoint(x: CGFloat(control.x), y: CGFloat(control.y))
            bucketPaths[bucket * levels + level].move(to: start)
            bucketPaths[bucket * levels + level].addQuadCurve(to: end, control: c)
            if pulsing && pulseEdges.count < 900 && pulseCandidates.contains(e) {
                pulseEdges.append(e)
                pulsePoints.append(start)
                pulsePoints.append(c)
                pulsePoints.append(end)
            }
        }
        for (b, bucket) in buckets.enumerated() {
            for level in 0..<levels {
                let path = bucketPaths[b * levels + level]
                if path.isEmpty { continue }
                let fog = Self.fogLevels[level]
                if bucket.glow {
                    context.stroke(path, with: .color(bucket.colour.opacity(fog * 0.18)),
                                   style: StrokeStyle(lineWidth: bucket.width * 4, lineCap: .round))
                    context.stroke(path, with: .color(bucket.colour.opacity(fog)),
                                   style: StrokeStyle(lineWidth: bucket.width * 1.4, lineCap: .round))
                } else {
                    context.stroke(path, with: .color(bucket.colour.opacity(fog)),
                                   style: StrokeStyle(lineWidth: bucket.width * 1.1, lineCap: .round))
                }
            }
        }
    }

    private func drawPulses(_ context: inout GraphicsContext, _ graph: SynapseGraph, _ style: SynapseStyle, _ frame: SynapseFrame,
                            _ time: Double) {
        if pulseEdges.isEmpty { return }
        for i in pulsePaths.indices { pulsePaths[i] = Path() }
        for (n, e) in pulseEdges.enumerated() {
            let edge = graph.edges[e]
            let phase = Double(edge.seed & 0xffff) / 65535
            let head = (time * 0.32 + phase).truncatingRemainder(dividingBy: 1)
            let kind: Int
            switch style.edgeState[e] {
            case .imports: kind = 1
            case .importers: kind = 2
            case .cycle: kind = 3
            default: kind = 0
            }
            let a = pulsePoints[n * 3]
            let c = pulsePoints[n * 3 + 1]
            let b = pulsePoints[n * 3 + 2]
            for k in 0..<4 {
                let t = head - Double(k) * 0.022
                if t < 0 { break }
                let u = 1 - t
                let wa = u * u
                let wc = 2 * u * t
                let wb = t * t
                let x = Double(a.x) * wa + Double(c.x) * wc + Double(b.x) * wb
                let y = Double(a.y) * wa + Double(c.y) * wc + Double(b.y) * wb
                let r = (3.6 - Double(k) * 0.7) * 0.45
                pulsePaths[kind * 4 + k].addEllipse(in: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2))
            }
        }
        let tones = [
            SynapseColour(bytes: 220, 235, 255),
            SynapsePalette.imports.mixed(SynapsePalette.white, 0.55),
            SynapsePalette.importers.mixed(SynapsePalette.white, 0.55),
            SynapsePalette.cycle.mixed(SynapsePalette.white, 0.55),
        ]
        for kind in 0..<4 {
            for k in 0..<4 {
                let path = pulsePaths[kind * 4 + k]
                if path.isEmpty { continue }
                context.fill(path, with: .color(Self.color(tones[kind], alpha: (0.9 - 0.2 * Double(k)) * 0.8)))
            }
        }
    }

    private func drawNodes(_ context: inout GraphicsContext, _ glowContext: inout GraphicsContext, _ graph: SynapseGraph,
                           _ style: SynapseStyle, _ frame: SynapseFrame, moving: Bool, pulse: Double) {
        for i in dotPaths.indices { dotPaths[i] = Path() }
        resolvedSprites = Array(repeating: nil, count: nodePalette.count)
        resolvedHalos = Array(repeating: nil, count: nodePalette.count)
        large.removeAll(keepingCapacity: true)
        var visible = 0
        for i in frame.order where frame.onScreen[i] && frame.visible[i] { visible += 1 }
        let many = visible > (moving ? 1500 : 4000)
        let levels = Self.opacityLevels.count
        for i in frame.order where frame.onScreen[i] && frame.visible[i] {
            let r = max(0.9, frame.radius[i])
            if r >= 1.6 {
                large.append(i)
                continue
            }
            let state = style.nodeState[i]
            let opacity = state == .dim ? Double(frame.fog[i]) * 0.85 : 0.25 + 0.75 * Double(frame.fog[i])
            let level = min(levels - 1, Int(opacity * Double(levels)))
            let x = CGFloat(frame.x[i])
            let y = CGFloat(frame.y[i])
            let rr = CGFloat(r)
            dotPaths[nodePaletteIndex[i] * levels + level].addEllipse(in: CGRect(x: x - rr, y: y - rr, width: rr * 2, height: rr * 2))
        }
        for p in nodePalette.indices {
            for level in 0..<levels {
                let path = dotPaths[p * levels + level]
                if path.isEmpty { continue }
                context.fill(path, with: .color(nodePaletteColours[p].opacity(Self.opacityLevels[level])))
            }
        }
        var body = context
        for i in large {
            let state = style.nodeState[i]
            let r = CGFloat(max(0.9, frame.radius[i]))
            let x = CGFloat(frame.x[i])
            let y = CGFloat(frame.y[i])
            let fog = Double(frame.fog[i])
            let palette = nodePaletteIndex[i]
            let cycle = graph.nodes[i].cycle && state == .normal
            if state != .dim && r >= 1.2 && (!many || state != .normal || cycle) {
                var intensity = state == .normal ? 0.32 : 0.62
                if state == .selected { intensity = 0.9 }
                if cycle { intensity = 0.4 + (style.input.pulses ? 0.45 * pulse : 0.2) }
                if let halo = glow(palette, &context) {
                    glowContext.opacity = min(max(intensity * fog, 0), 1)
                    let hr = r * 3 * 64 / 62
                    glowContext.draw(halo, in: CGRect(x: x - hr, y: y - hr, width: hr * 2, height: hr * 2))
                }
            }
            body.opacity = min(max(state == .dim ? fog * 0.85 : 0.25 + 0.75 * fog, 0), 1)
            let sr = r * 64 / 62
            if let image = sprite(palette, &context) {
                body.draw(image, in: CGRect(x: x - sr, y: y - sr, width: sr * 2, height: sr * 2))
            }
            if cycle && style.input.pulses {
                body.fill(Path(ellipseIn: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2)),
                          with: .color(Color(.sRGB, red: 1, green: 190 / 255, blue: 180 / 255, opacity: 0.3 * pulse)))
            }
            if state == .selected {
                body.opacity = 1
                body.stroke(Path(ellipseIn: CGRect(x: x - r - 4, y: y - r - 4, width: (r + 4) * 2, height: (r + 4) * 2)),
                            with: .color(.white.opacity(200 / 255)), lineWidth: 1.4)
                body.stroke(Path(ellipseIn: CGRect(x: x - r - 8, y: y - r - 8, width: (r + 8) * 2, height: (r + 8) * 2)),
                            with: .color(Color(.sRGB, red: 120 / 255, green: 200 / 255, blue: 1, opacity: 90 / 255)), lineWidth: 1)
            }
        }
    }

    private func place(_ context: inout GraphicsContext, at anchor: CGPoint, offset: CGFloat, text: String, size: CGFloat,
                       weight: Font.Weight, colour: Color, border: Color?, tracking: CGFloat, bounds: CGRect) -> Bool {
        let resolved = context.resolve(Text(text).font(.system(size: size, weight: weight)).tracking(tracking).foregroundStyle(colour))
        let measured = resolved.measure(in: CGSize(width: bounds.width * 0.7, height: 40))
        let pill = border != nil
        let w = measured.width + (pill ? 14 : 0)
        let h = measured.height + (pill ? 6 : 0)
        var rect = CGRect(x: anchor.x + offset, y: anchor.y - h / 2, width: w, height: h)
        if rect.maxX > bounds.maxX - 6 { rect.origin.x = anchor.x - offset - w }
        if !bounds.insetBy(dx: 4, dy: 4).contains(rect) { return false }
        for other in taken where other.insetBy(dx: -3, dy: -2).intersects(rect) { return false }
        taken.append(rect)
        if let border {
            let shape = Path(roundedRect: rect, cornerRadius: h / 2)
            context.fill(shape, with: .color(Color(.sRGB, red: 10 / 255, green: 14 / 255, blue: 26 / 255, opacity: 205 / 255)))
            context.stroke(shape, with: .color(border), lineWidth: 1)
        } else {
            let shadow = context.resolve(Text(text).font(.system(size: size, weight: weight)).tracking(tracking)
                .foregroundStyle(Color.black.opacity(150 / 255)))
            context.draw(shadow, at: CGPoint(x: rect.midX, y: rect.midY + 1))
        }
        context.draw(resolved, at: CGPoint(x: rect.midX, y: rect.midY))
        return true
    }

    private func drawLabels(_ context: inout GraphicsContext, _ viewport: SynapseViewport, _ graph: SynapseGraph, _ style: SynapseStyle,
                            _ frame: SynapseFrame, _ projection: SynapseProjection, _ size: CGSize) {
        taken.removeAll(keepingCapacity: true)
        let bounds = CGRect(origin: .zero, size: size)
        let text = Self.color(SynapsePalette.text)
        func nodeLabel(_ node: Int, _ border: Color, _ strong: Bool) {
            guard node >= 0 && node < frame.count && frame.onScreen[node] && frame.visible[node] else { return }
            let name = viewport.fileName(graph.nodes[node].file)
            _ = place(&context, at: CGPoint(x: CGFloat(frame.x[node]), y: CGFloat(frame.y[node])),
                      offset: CGFloat(max(4, frame.radius[node])) + 7, text: name, size: strong ? 13 : 11,
                      weight: strong ? .semibold : .regular, colour: text, border: border, tracking: 0, bounds: bounds)
        }
        let selected = style.input.selected
        if selected >= 0 {
            nodeLabel(graph.nodeFor(selected), Color(.sRGB, red: 120 / 255, green: 200 / 255, blue: 1, opacity: 220 / 255), true)
            var neighbours: [Int] = []
            for i in graph.nodes.indices where style.nodeState[i] == .imports || style.nodeState[i] == .importers {
                neighbours.append(i)
            }
            let hop = { (n: Int) -> Int in
                let file = graph.nodes[n].file
                return max(style.neighbours.imports[file] ?? 9, 0) + (style.neighbours.importers[file] ?? 0)
            }
            neighbours.sort { a, b in
                let da = hop(a)
                let db = hop(b)
                if da != db { return da < db }
                return graph.nodes[a].fanIn > graph.nodes[b].fanIn
            }
            for n in neighbours.prefix(14) {
                let tone = style.nodeState[n] == .imports ? SynapsePalette.imports : SynapsePalette.importers
                nodeLabel(n, Self.color(tone, alpha: 0.6), false)
            }
            return
        }
        let isolated = style.input.isolatedCycle
        if isolated >= 0 && isolated < graph.cycles.count {
            for file in graph.cycles[isolated].files { nodeLabel(graph.nodeFor(file), Self.color(SynapsePalette.cycle, alpha: 0.7), false) }
        }
        for hub in style.hubs.prefix(8) { nodeLabel(hub, .white.opacity(50 / 255), false) }
        guard style.input.showMode == .everything && style.input.query.isEmpty && isolated < 0 else { return }
        let lobes = graph.lobes.indices.sorted { graph.lobes[$0].nodes.count > graph.lobes[$1].nodes.count }
        var shown = 0
        for l in lobes {
            if shown >= 10 || l >= viewport.lobeCentres.count || graph.lobes[l].nodes.count < 2 { break }
            let p = projection.project(viewport.lobeCentres[l])
            if p.depth <= 1 { continue }
            let colour = style.input.colourBy == .module
                ? Self.color(SynapsePalette.lobe(l).lighter(115), alpha: 0.9)
                : Color(.sRGB, red: 200 / 255, green: 210 / 255, blue: 230 / 255, opacity: 190 / 255)
            let name = SynapsePalette.shortLobeName(graph.lobes[l].name).uppercased()
            let resolved = context.resolve(Text(name).font(.system(size: 10, weight: .semibold)).tracking(1.2))
            let width = resolved.measure(in: size).width
            if place(&context, at: CGPoint(x: CGFloat(p.x) - width / 2, y: CGFloat(p.y)), offset: 0, text: name, size: 10,
                     weight: .semibold, colour: colour, border: nil, tracking: 1.2, bounds: bounds) {
                shown += 1
            }
        }
    }
}
