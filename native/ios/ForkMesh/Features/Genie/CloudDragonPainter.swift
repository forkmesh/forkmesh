import FMAssistant
import SwiftUI

enum DragonInk {
    static let cream: Color = Color(red: 0.953, green: 0.902, blue: 0.8)
    static let mint: Color = Color(red: 0.055, green: 0.745, blue: 0.376)
    static let leaf: Color = Color(red: 0.184, green: 0.812, blue: 0.486)
    static let gold: Color = Color(red: 1, green: 0.788, blue: 0.302)
    static let ember: Color = Color(red: 1, green: 0.604, blue: 0.18)
    static let pink: Color = Color(red: 0.957, green: 0.498, blue: 0.525)
    static let white: Color = Color.white
    static let lid: Color = Color(red: 0.89, green: 0.8, blue: 0.694)
    static let lash: Color = Color(red: 0.169, green: 0.141, blue: 0.118)
    static let steam: Color = Color(red: 0.745, green: 0.769, blue: 0.824)
    static let flight: Color = Color(red: 0.361, green: 0.494, blue: 0.706)
    static let ground: CGFloat = GeniePainter.ground
}

extension GraphicsContext {
    mutating func apply(_ m: DragonAffine) {
        concatenate(CGAffineTransform(a: m.a, b: m.b, c: m.c, d: m.d, tx: m.tx, ty: m.ty))
    }
}

struct CloudDragonPainter {
    let art: CloudDragonArt
    let frame: DragonFrame
    var trick: GenieTrickFrame? = nil

    static let reach: CGFloat = 3

    var atlas: DragonAtlas { art.atlas }

    var unit: CGFloat { CGFloat(atlas.units) }

    var beat: DragonBeat { frame.beat }

    func paint(_ context: inout GraphicsContext, size: CGSize) {
        let side: CGFloat = min(size.width, size.height) / CloudDragonPainter.reach
        guard side > 0 else { return }
        var world: GraphicsContext = context
        world.translateBy(x: (size.width - side) / 2, y: (size.height - side) / 2)
        world.scaleBy(x: side / 100, y: side / 100)
        world.translateBy(x: 0, y: 50 + GenieGravity.stand * 100 - DragonInk.ground)
        var stage: GraphicsContext = world
        if let trick = trick {
            stage.opacity = trick.opacity
            stage.translateBy(x: 50 + CGFloat(trick.dx), y: 60 + CGFloat(trick.dy))
            stage.rotate(by: .degrees(trick.rotation))
            stage.scaleBy(x: CGFloat(trick.scaleX), y: CGFloat(trick.scaleY))
            stage.translateBy(x: -50, y: -60)
            if abs(trick.hue) > 0.5 { stage.addFilter(.hueRotation(.degrees(trick.hue))) }
        }
        stage.translateBy(x: 50, y: DragonInk.ground)
        shadow(&stage)
        let flip: CGFloat = CGFloat(frame.flip)
        if let act = frame.activity {
            var under: GraphicsContext = stage
            under.scaleBy(x: flip, y: 1)
            underHook(&under, act)
        }
        var rig: GraphicsContext = stage
        rig.scaleBy(x: unit, y: unit)
        let stretch: CGFloat = CGFloat(frame.stretch)
        rig.scaleBy(x: 1 - stretch * 0.55, y: 1 + stretch)
        rig.translateBy(x: -CGFloat(atlas.joints.axis.x), y: -CGFloat(atlas.joints.axis.y))
        draw(&rig)
        var over: GraphicsContext = stage
        over.scaleBy(x: flip, y: 1)
        if let sneeze = frame.sneeze {
            for i in 0..<4 {
                let u: Double = DragonMath.clamp01(sneeze * 1.3 - Double(i) * 0.1)
                puff(
                    &over, 16 + u * 15 + Double(i), -49 - u * 3 + (Double(i) - 1.5) * u * 2.4, 1 + u * 2.6, 0.75 * (1 - u),
                    Color(red: 0.84, green: 0.86, blue: 0.89))
            }
        }
        if let act = frame.activity { overHook(&over, act) }
        effects(&world)
    }

    private func effects(_ context: inout GraphicsContext) {
        guard let trick = trick else { return }
        for particle in trick.particles where particle.opacity > 0.02 {
            var layer: GraphicsContext = context
            layer.opacity = particle.opacity
            layer.draw(Text(verbatim: particle.glyph).font(.system(size: CGFloat(particle.size) * 1.3)), at: CGPoint(x: particle.x, y: particle.y))
        }
    }

    func shadow(_ context: inout GraphicsContext) {
        var flat: GraphicsContext = context
        flat.scaleBy(x: 1, y: 0.16)
        let width: CGFloat = CGFloat(frame.shadow.width)
        flat.fill(
            Path(ellipseIn: CGRect(x: -width, y: -width, width: width * 2, height: width * 2)),
            with: .radialGradient(
                Gradient(colors: [
                    Color(red: 0.031, green: 0.039, blue: 0.078).opacity(0.3 * frame.shadow.alpha),
                    Color(red: 0.031, green: 0.039, blue: 0.078).opacity(0),
                ]),
                center: .zero, startRadius: 0, endRadius: width))
    }

    func layer(_ c: inout GraphicsContext, _ name: DragonLayer) {
        guard let image = art.layers[name] else { return }
        let f: DragonLayerFrame = atlas.layer(name)
        let k: Double = f.s / atlas.rigScale
        c.draw(image, in: CGRect(x: f.ox, y: f.oy, width: f.w * k, height: f.h * k))
    }

    func prop(_ c: inout GraphicsContext, _ name: String, _ x: Double, _ y: Double, _ scale: Double, alpha: Double? = nil, rot: Double = 0, sy: Double? = nil, pivot: CGPoint = .zero) {
        guard let f = atlas.props[name], let image = art.props[name], scale > 0 else { return }
        let w: Double = f.w * f.k * scale
        let h: Double = f.h * f.k * scale
        var p: GraphicsContext = c
        if let alpha = alpha { p.opacity *= alpha }
        let px: Double = Double(pivot.x) * w
        let py: Double = Double(pivot.y) * h
        p.translateBy(x: x + px, y: y + py)
        if rot != 0 { p.rotate(by: .radians(rot)) }
        if let sy = sy { p.scaleBy(x: 1, y: sy) }
        p.translateBy(x: -px, y: -py)
        p.draw(image, in: CGRect(x: -w / 2, y: -h / 2, width: w, height: h))
    }

    func propAt(_ c: inout GraphicsContext, _ name: String, _ x: Double, _ y: Double, _ scale: Double, alpha: Double? = nil, rot: Double = 0) {
        var p: GraphicsContext = c
        p.translateBy(x: x, y: y)
        p.scaleBy(x: unit, y: unit)
        prop(&p, name, 0, 0, scale, alpha: alpha, rot: rot)
    }

    func flame(_ c: inout GraphicsContext, _ size: Double, _ tall: Double) {
        guard let f = atlas.props["flame"], let image = art.props["flame"] else { return }
        var p: GraphicsContext = c
        p.translateBy(x: atlas.joints.mouth.x + 6, y: atlas.joints.mouth.y + 30)
        p.scaleBy(x: 1, y: tall)
        let w: Double = f.w * f.k * size
        let h: Double = f.h * f.k * size
        p.draw(image, in: CGRect(x: -10, y: -h * 0.58, width: w, height: h))
    }

    func local(_ p: DragonPoint) -> CGPoint {
        CGPoint(x: (p.x - atlas.joints.axis.x) * atlas.units, y: (p.y - atlas.joints.axis.y) * atlas.units)
    }

    func draw(_ r: inout GraphicsContext) {
        for step in frame.steps {
            switch step {
            case .layer(let name, let xf, let alpha):
                var c: GraphicsContext = r
                c.opacity *= alpha
                c.apply(xf)
                layer(&c, name)
            case .tail(let part):
                var c: GraphicsContext = r
                c.apply(part.xf)
                tail(&c, part.from, part.to)
            case .gear(let gear):
                var c: GraphicsContext = r
                c.translateBy(x: gear.x, y: gear.y)
                c.scaleBy(x: gear.face, y: 1)
                prop(&c, gear.prop, 0, 0, gear.scale)
            case .head(let head):
                drawHead(r, head)
            }
        }
    }

    func tail(_ c: inout GraphicsContext, _ from: Int, _ to: Int) {
        for triangle in frame.tail.triangles(from: from, to: to) {
            var t: GraphicsContext = c
            var clip: Path = Path()
            clip.move(to: CGPoint(x: triangle.clip[0].x, y: triangle.clip[0].y))
            clip.addLine(to: CGPoint(x: triangle.clip[1].x, y: triangle.clip[1].y))
            clip.addLine(to: CGPoint(x: triangle.clip[2].x, y: triangle.clip[2].y))
            clip.closeSubpath()
            t.clip(to: clip)
            t.apply(triangle.xf)
            let column = art.columns[triangle.column]
            t.draw(column.image, in: CGRect(x: column.x, y: 0, width: column.w, height: art.stripHeight))
        }
    }

    func drawHead(_ r: GraphicsContext, _ head: DragonHeadDraw) {
        var ear: GraphicsContext = r
        ear.apply(head.earXf)
        layer(&ear, .ear)
        var c: GraphicsContext = r
        c.apply(head.xf)
        layer(&c, .head)
        let near: DragonEye = atlas.eyeNear
        let far: DragonEye = atlas.eyeFar
        eye(c, .eyeNear, near, head.look, 1)
        eye(c, .eyeFar, far, head.look, 0.45)
        lid(c, near, head.lidNear, head.happy)
        lid(c, far, head.lidFar, head.happy)
        mouth(c, head.open, head.oval)
        let pull: Double = head.pull
        let centre: CGPoint = CGPoint(x: 735, y: 575)
        c.fill(
            Path(ellipseIn: CGRect(x: centre.x - 58, y: centre.y - 58, width: 116, height: 116)),
            with: .radialGradient(
                Gradient(colors: [Color(red: 0.969, green: 0.588, blue: 0.588).opacity(pull), Color(red: 0.969, green: 0.588, blue: 0.588).opacity(0)]),
                center: centre, startRadius: 0, endRadius: 58))
        headHook(&c)
    }

    func eye(_ c: GraphicsContext, _ name: DragonLayer, _ e: DragonEye, _ look: DragonPoint, _ k: Double) {
        var p: GraphicsContext = c
        p.clip(to: Path(ellipseIn: CGRect(x: e.x - e.rx, y: e.y - e.ry, width: e.rx * 2, height: e.ry * 2)))
        p.translateBy(x: look.x * 12 * k, y: look.y * 9)
        layer(&p, name)
    }

    func arc(_ x: Double, _ y: Double, _ rx: Double, _ ry: Double, _ from: Double, _ to: Double) -> Path {
        var path: Path = Path()
        let steps: Int = 18
        for i in 0...steps {
            let a: Double = from + (to - from) * Double(i) / Double(steps)
            let p: CGPoint = CGPoint(x: x + cos(a) * rx, y: y + sin(a) * ry)
            if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
        }
        return path
    }

    func lid(_ c: GraphicsContext, _ e: DragonEye, _ closed: Double, _ happy: Double) {
        guard closed > 0.02 else { return }
        var p: GraphicsContext = c
        p.clip(to: Path(ellipseIn: CGRect(x: e.x - e.rx * 1.42, y: e.y - e.ry * 1.42, width: e.rx * 2.84, height: e.ry * 2.84)))
        p.translateBy(x: e.x, y: e.y)
        p.scaleBy(x: e.rx, y: e.ry)
        p.fill(
            Path(CGRect(x: -1.45, y: -1.45, width: 2.9, height: 2.9 * closed)),
            with: .radialGradient(
                Gradient(colors: [DragonInk.lid, DragonInk.lid.opacity(0)]), center: .zero, startRadius: 1.1, endRadius: 1.42))
        guard closed > 0.55 else { return }
        var s: GraphicsContext = c
        s.opacity *= DragonMath.span(closed, 0.55, 0.8)
        let shape: Path = happy > 0.5
            ? arc(e.x, e.y + e.ry * 0.45, e.rx * 0.95, e.ry * 0.75, Double.pi * 1.1, Double.pi * 1.9)
            : arc(e.x, e.y - e.ry * 0.2, e.rx * 0.95, e.ry * 0.6, Double.pi * 0.1, Double.pi * 0.9)
        s.stroke(shape, with: .color(DragonInk.lash), style: StrokeStyle(lineWidth: 10, lineCap: .round))
    }

    func mouth(_ c: GraphicsContext, _ open: Double, _ oval: Double) {
        guard open > 0.02 else { return }
        let m: DragonPoint = atlas.joints.mouth
        let rx: Double = DragonMath.mixed(64, 30, oval) * (0.7 + open * 0.3)
        let ry: Double = 6 + open * DragonMath.mixed(34, 30, oval)
        let cx: Double = m.x
        let cy: Double = m.y
        let y: Double = cy + ry * 0.55
        var shape: Path = Path()
        shape.move(to: CGPoint(x: cx - rx, y: cy))
        shape.addQuadCurve(to: CGPoint(x: cx + rx, y: cy), control: CGPoint(x: cx, y: cy - 8 * (1 - oval)))
        shape.addQuadCurve(to: CGPoint(x: cx, y: y + ry * 0.75), control: CGPoint(x: cx + rx * 0.92, y: y + ry * 0.85))
        shape.addQuadCurve(to: CGPoint(x: cx - rx, y: cy), control: CGPoint(x: cx - rx * 0.92, y: y + ry * 0.85))
        var p: GraphicsContext = c
        p.fill(shape, with: .color(Color(red: 0.486, green: 0.184, blue: 0.208)))
        p.clip(to: shape)
        p.fill(
            Path(ellipseIn: CGRect(x: cx + rx * 0.05 - rx * 0.56, y: y + ry * 0.72 - ry * 0.5, width: rx * 1.12, height: ry)),
            with: .color(Color(red: 0.933, green: 0.561, blue: 0.58)))
        for side in [-1.0, 1.0] {
            var tooth: Path = Path()
            tooth.move(to: CGPoint(x: cx + side * rx * 0.62, y: cy - 4))
            tooth.addLine(to: CGPoint(x: cx + side * rx * 0.48, y: cy + 15))
            tooth.addLine(to: CGPoint(x: cx + side * rx * 0.34, y: cy - 4))
            tooth.closeSubpath()
            p.fill(tooth, with: .color(Color(red: 1, green: 0.973, blue: 0.925)))
        }
    }
}
