import Foundation

public struct DragonGearDraw: Equatable, Sendable {
    public var prop: String
    public var x: Double
    public var y: Double
    public var face: Double
    public var scale: Double
}

public struct DragonHeadDraw: Equatable, Sendable {
    public var xf: DragonAffine
    public var earXf: DragonAffine
    public var blink: Double
    public var lidNear: Double
    public var lidFar: Double
    public var happy: Double
    public var look: DragonPoint
    public var open: Double
    public var oval: Double
    public var pull: Double
}

public struct DragonTailPart: Equatable, Sendable {
    public var from: Int
    public var to: Int
    public var xf: DragonAffine
}

public struct DragonTriangle: Equatable, Sendable {
    public var clip: [DragonPoint]
    public var xf: DragonAffine
    public var column: Int
}

public enum DragonStep: Equatable, Sendable {
    case layer(DragonLayer, DragonAffine, Double)
    case tail(DragonTailPart)
    case gear(DragonGearDraw)
    case head(DragonHeadDraw)
}

public struct DragonTail: Equatable, Sendable {
    public var points: [DragonPoint]
    public var aim: [Double]
    public var outer: [DragonPoint]
    public var inner: [DragonPoint]
    public var stripWidth: Double
    public var stripHeight: Double

    public var segments: Int { points.count - 1 }

    public func column(_ i: Int) -> (x: Double, w: Double) {
        DragonTail.column(i, segments: segments, stripWidth: stripWidth)
    }

    public static func column(_ i: Int, segments: Int, stripWidth: Double) -> (x: Double, w: Double) {
        let n: Double = Double(segments)
        let u0: Double = stripWidth * Double(i) / n
        let u1: Double = stripWidth * Double(i + 1) / n
        let x0: Double = max((u0.rounded(.down)) - 1, 0)
        let w: Double = min((u1 - u0).rounded(.up) + 3, stripWidth - x0)
        return (x0, w)
    }

    public func triangles(from: Int, to: Int) -> [DragonTriangle] {
        let n: Double = Double(segments)
        var out: [DragonTriangle] = []
        var i: Int = to - 1
        while i >= from {
            let u0: Double = stripWidth * Double(i) / n
            let u1: Double = stripWidth * Double(i + 1) / n
            let a: DragonPoint = DragonPoint(u0, 0)
            let c: DragonPoint = DragonPoint(u1, 0)
            let e: DragonPoint = DragonPoint(u1, stripHeight)
            let g: DragonPoint = DragonPoint(u0, stripHeight)
            if let t = DragonTail.texture(a, outer[i], c, outer[i + 1], e, inner[i + 1], column: i) { out.append(t) }
            if let t = DragonTail.texture(a, outer[i], e, inner[i + 1], g, inner[i], column: i) { out.append(t) }
            i -= 1
        }
        return out
    }

    static func texture(
        _ a: DragonPoint, _ b: DragonPoint, _ c: DragonPoint, _ d: DragonPoint, _ e: DragonPoint, _ f: DragonPoint, column: Int
    ) -> DragonTriangle? {
        let du1: Double = c.x - a.x
        let dv1: Double = c.y - a.y
        let du2: Double = e.x - a.x
        let dv2: Double = e.y - a.y
        let den: Double = du1 * dv2 - du2 * dv1
        guard den != 0 else { return nil }
        let dx1: Double = d.x - b.x
        let dy1: Double = d.y - b.y
        let dx2: Double = f.x - b.x
        let dy2: Double = f.y - b.y
        let m: Double = (dx1 * dv2 - dx2 * dv1) / den
        let n: Double = (dx2 * du1 - dx1 * du2) / den
        let o: Double = (dy1 * dv2 - dy2 * dv1) / den
        let p: Double = (dy2 * du1 - dy1 * du2) / den
        let centre: DragonPoint = DragonPoint((b.x + d.x + f.x) / 3, (b.y + d.y + f.y) / 3)
        func out(_ q: DragonPoint) -> DragonPoint {
            let dx: Double = q.x - centre.x
            let dy: Double = q.y - centre.y
            let size: Double = hypot(dx, dy)
            let s: Double = size == 0 ? 1 : size
            return DragonPoint(q.x + dx / s * 4, q.y + dy / s * 4)
        }
        let xf: DragonAffine = DragonAffine(a: m, b: o, c: n, d: p, tx: b.x - m * a.x - n * a.y, ty: b.y - o * a.x - p * a.y)
        return DragonTriangle(clip: [out(b), out(d), out(f)], xf: xf, column: column)
    }
}

public struct DragonFrame: Sendable {
    public var steps: [DragonStep]
    public var tail: DragonTail
    public var q: [Double]
    public var activity: DragonActivity?
    public var beat: DragonBeat
    public var pawN: DragonPoint
    public var pawF: DragonPoint
    public var hipW: DragonPoint
    public var lean: Double
    public var squash: Double
    public var wide: Double
    public var stretch: Double
    public var lift: Double
    public var flip: Double
    public var sneeze: Double?
    public var stepBob: Double
    public var hip: DragonPoint
    public var shadow: (width: Double, alpha: Double)

    public func value(_ key: DragonKey) -> Double { q[key.rawValue] }
}

extension DragonBody {
    public mutating func frame(_ s: DragonScene, atlas: DragonAtlas) -> DragonFrame {
        advance(s)
        var tail: DragonTailState = tailState
        let out: DragonFrame = compose(s, atlas: atlas, tail: &tail)
        tailState = tail
        return out
    }

    static let groundY: Double = 1236

    func compose(_ s: DragonScene, atlas: DragonAtlas, tail tailState: inout DragonTailState) -> DragonFrame {
        typealias M = DragonMath
        let J: DragonJoints = atlas.joints
        let axis: DragonPoint = J.axis
        let t: Double = s.t
        let jitter: Double = q(.jitter)
        let shake: DragonPoint = jitter != 0 ? DragonPoint(sin(t * 61) * jitter, cos(t * 47) * jitter) : DragonPoint(0, 0)
        let fb: Double = DragonFlips.seen(flips.body)
        let flipBody: Double = flips.body
        func flipX(_ x: Double) -> Double { axis.x + (x - axis.x) * flipBody }
        let crouch: Double = q(.crouch)
        let hip: DragonPoint = DragonPoint(J.hip.x + q(.x) + shake.x, J.hip.y + q(.y) + crouch * 92 + shake.y)
        let trot: Double = q(.ground)
        let current: (activity: DragonActivity, age: Double)? = s.current
        let beat: DragonBeat = DragonBeat(age: current?.age ?? 0, duration: current?.activity.duration ?? 0, t: t)
        let stride: Double = current.flatMap { $0.activity.stride(beat) } ?? s.stridePhase
        let stepBob: Double = abs(sin(stride)) * trot
        let lean: Double = q(.lean) + trot * (0.12 + sin(stride) * 0.05)
        let hipW: DragonPoint = DragonPoint(hip.x, hip.y - trot * 16 - stepBob * 13)
        let squash: Double = 1 - crouch * 0.26 + q(.chest)
        let wide: Double = 1 + crouch * 0.1 - q(.chest) * 0.5
        let cs: Double = cos(lean)
        let sn: Double = sin(lean)
        func torso(_ p: DragonPoint) -> DragonPoint {
            let px: Double = (p.x - J.hip.x) * wide
            let py: Double = (p.y - J.hip.y) * squash
            return DragonPoint(hipW.x + px * cs - py * sn, hipW.y + px * sn + py * cs)
        }
        func step(_ phase: Double, _ amount: Double) -> (x: Double, y: Double, lift: Double) {
            let p: Double = (phase.truncatingRemainder(dividingBy: M.tau) + M.tau).truncatingRemainder(dividingBy: M.tau) / M.tau
            if p < 0.5 {
                let lift: Double = sin(p * M.tau)
                return (M.mixed(-118, 118, M.smooth(p * 2)) * amount, -lift * 64 * amount, lift)
            }
            return ((118 - (p - 0.5) * 472) * amount, 0, 0)
        }
        func footWorld(_ rest: DragonPoint, _ ox: Double, _ oy: Double, _ phase: Double) -> (point: DragonPoint, lift: Double) {
            let st: (x: Double, y: Double, lift: Double) = step(phase, trot)
            return (DragonPoint(rest.x + ox + st.x + q(.x), min(rest.y + oy + st.y + q(.y), DragonBody.groundY)), st.lift)
        }
        let footN: (point: DragonPoint, lift: Double) = footWorld(J.footNear, q(.footNx), q(.footNy), stride)
        let footF: (point: DragonPoint, lift: Double) = footWorld(J.footFar, q(.footFx), q(.footFy), stride + Double.pi)
        let reach: Double = q(.reach)
        func pawReach(_ rest: DragonPoint, _ ox: Double, _ oy: Double, _ phase: Double) -> DragonPoint {
            let swing: Double = cos(phase) * 62 * trot
            let local: DragonPoint = torso(
                DragonPoint(rest.x + ox * (1 - reach) + swing, rest.y + oy * (1 - reach) - 24 * trot - abs(sin(phase)) * 16 * trot))
            if reach < 0.01 { return local }
            let base: DragonPoint = torso(J.neck)
            return DragonPoint(M.mixed(local.x, base.x + ox, reach), M.mixed(local.y, base.y + oy, reach))
        }
        var pawN: DragonPoint = pawReach(J.pawNear, q(.pawNx), q(.pawNy), stride)
        var pawF: DragonPoint = pawReach(J.pawFar, q(.pawFx), q(.pawFy), stride + Double.pi)
        let shoulderN: DragonPoint = torso(J.shoulderNear)
        let shoulderF: DragonPoint = torso(J.shoulderFar)
        func within(_ from: DragonPoint, _ to: inout DragonPoint, _ most: Double) {
            let dx: Double = to.x - from.x
            let dy: Double = to.y - from.y
            let gap: Double = hypot(dx, dy)
            if gap <= most { return }
            to = DragonPoint(from.x + dx / gap * most, from.y + dy / gap * most)
        }
        within(shoulderN, &pawN, 345)
        within(shoulderF, &pawF, 325)
        let hipN: DragonPoint = torso(J.hipNear)
        let hipF: DragonPoint = torso(J.hipFar)
        let neck: DragonPoint = torso(J.neck)
        let gears: [DragonGear] = current?.activity.gear ?? []
        var steps: [DragonStep] = []

        func gear(_ order: DragonOrder) {
            for g in gears where g.order == order {
                let show: Double = M.back(M.span(beat.u, 0, 0.1)) * (1 - M.smooth(M.span(beat.u, 0.9, 1)))
                let pulse: Double = g.beat ? 1 + pow(max(sin(beat.age * 6.2), 0), 3) * 0.1 : 1
                let face: Double = (flipBody < 0 ? -1 : 1) * (0.56 + 0.44 * abs(flipBody))
                var px: Double
                var py: Double
                switch g.at {
                case .ground:
                    px = axis.x + g.x
                    py = g.y
                case .pawF:
                    px = pawF.x + g.x
                    py = pawF.y + g.y
                case .pawN:
                    px = pawN.x + g.x
                    py = pawN.y + g.y
                case .chest:
                    let p: DragonPoint = torso(DragonPoint(J.hip.x + g.x, g.y))
                    px = p.x
                    py = p.y
                case .torso:
                    let p: DragonPoint = torso(DragonPoint(g.x, g.y))
                    px = p.x
                    py = p.y
                }
                px = axis.x + (px - axis.x) * flipBody
                steps.append(.gear(DragonGearDraw(prop: g.prop, x: px, y: py, face: face, scale: g.scale * show * pulse)))
            }
        }

        func place(
            _ flip: Double, _ at: DragonPoint, _ angle: Double, _ layer: DragonLayer, _ pivot: DragonPoint, sx: Double = 1, sy: Double = 1
        ) {
            let xf: DragonAffine = DragonAffine.identity
                .translated(flipX(at.x), at.y).scaled(flip, 1).rotated(angle).scaled(sx, sy).translated(-pivot.x, -pivot.y)
            steps.append(.layer(layer, xf, 1))
        }

        func limbAngle(_ from: DragonPoint, _ restFrom: DragonPoint, _ restTo: DragonPoint, _ to: DragonPoint, _ lo: Double, _ hi: Double)
            -> (angle: Double, rest: Double, stretch: Double)
        {
            let rest: Double = atan2(restTo.y - restFrom.y, restTo.x - restFrom.x)
            let length: Double = hypot(restTo.x - restFrom.x, restTo.y - restFrom.y)
            let dx: Double = to.x - from.x
            let dy: Double = to.y - from.y
            let angle: Double = atan2(dy, dx)
            let stretch: Double = M.clamp(hypot(dx, dy) / length, lo, hi)
            return (angle - rest, rest, stretch)
        }

        func limb(
            _ layer: DragonLayer, _ from: DragonPoint, _ restFrom: DragonPoint, _ restTo: DragonPoint, _ to: DragonPoint,
            _ lo: Double, _ hi: Double, alpha: Double = 1
        ) -> Double {
            let l: (angle: Double, rest: Double, stretch: Double) = limbAngle(from, restFrom, restTo, to, lo, hi)
            let xf: DragonAffine = DragonAffine.identity
                .translated(flipX(from.x), from.y).scaled(fb, 1).rotated(l.angle).rotated(l.rest).scaled(l.stretch, 1)
                .rotated(-l.rest).translated(-restFrom.x, -restFrom.y)
            steps.append(.layer(layer, xf, alpha))
            return l.angle
        }

        func shin(_ from: DragonPoint, _ foot: DragonPoint) {
            let gap: Double = hypot(foot.x - from.x, foot.y - from.y)
            let show: Double = max(M.clamp01(trot * 4), M.span(gap, 190, 240))
            if show <= 0 { return }
            _ = limb(.armNear, from, J.shoulderNear, J.pawNear, DragonPoint(foot.x, foot.y - 12), 0.5, 1.8, alpha: show)
        }

        func wing(_ p: DragonPoint, _ angle: Double, _ layer: DragonLayer, _ sign: Double, rise: Double = 0) {
            let beatWing: Double = q(.flap) > 0.02 ? sin(t * 24) : 0
            let a: Double = sign * (angle + beatWing * 0.42 * q(.flap) + M.clamp(speed(.y) * 0.00045, -0.28, 0.28))
            let rear: DragonPoint = torso(J.wingFar)
            let own: DragonPoint = torso(p)
            let at: DragonPoint = DragonPoint(M.mixed(own.x, rear.x + 170, rise), M.mixed(own.y, rear.y - 40, rise))
            let spin: Double = sign > 0 ? 0.85 : -0.1 * rise + 0.85 * (1 - rise)
            let xf: DragonAffine = DragonAffine.identity
                .translated(flipX(at.x), at.y).scaled(DragonFlips.seen(flips.wing), 1).rotated(a + lean * spin)
                .scaled(1, 1 - abs(beatWing) * 0.1 * q(.flap)).translated(-p.x, -p.y)
            steps.append(.layer(layer, xf, 1))
        }

        let wingAngle: Double = q(.wing) + q(.flap) * 0.1
        wing(J.wingFar, wingAngle, .wingFar, 1)
        wing(J.wingNear, wingAngle, .wingNear, -1, rise: min(abs(lean) / 0.9, 1))
        gear(.back)
        shin(torso(DragonPoint(930, 1040)), footF.point)
        _ = limb(.thighFar, hipF, J.hipFar, J.footFar, footF.point, 0.55, 1.4)
        place(fb, footF.point, -footF.lift * 0.34 * trot, .footFar, J.footFar)

        let tailFlip: Double = DragonFlips.seen(flips.tail)
        var root: DragonPoint = torso(atlas.tail.curve[0])
        root.y = min(root.y, atlas.tail.curve[0].y + 12)
        let points: [DragonPoint] = tailState.bend(
            atlas, root: root, base: lean * 0.7 + q(.tailBase) + trot * sin(stride) * 0.06, tail: q(.tail), sway: q(.sway), dt: dt, s: s)
        let aim: [Double] = tailState.aim
        let tuck: Int = Int((Double(aim.count) * 0.42).rounded())
        let tailXf: DragonAffine = DragonAffine.identity.translated(flipX(root.x), root.y).scaled(tailFlip, 1).translated(-root.x, -root.y)
        var outer: [DragonPoint] = []
        var inner: [DragonPoint] = []
        let n: Int = points.count - 1
        for i in 0...n {
            let before: Double = aim[max(i - 1, 0)]
            let after: Double = aim[min(i, n - 1)]
            let angle: Double = before + M.wrapped(after - before) / 2
            let nx: Double = -sin(angle)
            let ny: Double = cos(angle)
            outer.append(DragonPoint(points[i].x + nx * atlas.tail.outer, points[i].y + ny * atlas.tail.outer))
            inner.append(DragonPoint(points[i].x - nx * atlas.tail.inner, points[i].y - ny * atlas.tail.inner))
        }
        let strip: DragonLayerFrame = atlas.layer(.tailStrip)
        let tailMesh: DragonTail = DragonTail(
            points: points, aim: aim, outer: outer, inner: inner, stripWidth: strip.w, stripHeight: strip.h)
        steps.append(.tail(DragonTailPart(from: 0, to: tuck, xf: tailXf)))
        place(fb, torso(J.hip), lean, .body, J.hip, sx: wide, sy: squash)
        shin(torso(DragonPoint(650, 1050)), footN.point)
        _ = limb(.thighNear, hipN, J.hipNear, J.footNear, footN.point, 0.6, 1.3)
        place(fb, footN.point, -footN.lift * 0.34 * trot, .footNear, J.footNear)
        steps.append(.tail(DragonTailPart(from: tuck, to: aim.count, xf: tailXf)))
        gear(.under)

        let farSolve: (angle: Double, rest: Double, stretch: Double) = limbAngle(shoulderF, J.shoulderFar, J.pawFar, pawF, 0.55, 1.5)
        let farAngle: Double = farSolve.angle
        let farReach: Double = hypot(pawF.x - shoulderF.x, pawF.y - shoulderF.y) / 215
        let sleeve: Double = M.span(abs(farAngle), 0.14, 0.42)
        _ = limb(.armFar, shoulderF, J.shoulderFar, J.pawFar, pawF, 0.55, 1.5, alpha: 1 - sleeve)
        let nearAngle: Double = limbAngle(shoulderN, J.shoulderNear, J.pawNear, pawN, 0.55, 1.5).angle

        func farSleeve() {
            if sleeve <= 0 { return }
            let xf: DragonAffine = DragonAffine.identity
                .translated(flipX(shoulderF.x), shoulderF.y).scaled(fb, 1).rotated(farAngle - 0.28)
                .scaled(-0.82, 0.82 * 1.22 * M.clamp(farReach, 0.6, 1.5)).translated(-J.shoulderNear.x, -J.shoulderNear.y)
            steps.append(.layer(.armNear, xf, sleeve))
        }

        func head() {
            let fh: Double = DragonFlips.seen(flips.head)
            let nx: Double = flipX(neck.x + q(.gh) * 12 + q(.hx))
            let ny: Double = neck.y + q(.gv) * 7 + q(.hy) + stepBob * 8 - M.clamp(speed(.y) * 0.02, -24, 24)
            let tilt: Double = q(.head) + q(.gh) * 0.075 - trot * 0.1 + trot * sin(stride * 2) * 0.025
            let headXf: DragonAffine = DragonAffine.identity
                .translated(nx, ny).scaled(fh, 1).rotated(tilt).translated(-J.neck.x, -J.neck.y)
            let n: Double = (t / 4.3).rounded(.down)
            let earAt: Double = n * 4.3 + M.seed(n + 11) * 3
            let twitch: Double = t > earAt && t < earAt + 0.28 ? sin((t - earAt) * 34) * 0.1 : 0
            let earAngle: Double = twitch + q(.ear) + sin(t * 1.3) * 0.012 + q(.flap) * 0.1 + trot * sin(stride * 2) * 0.09
                + M.clamp(speed(.head) * 0.09 + speed(.y) * 0.00035, -0.24, 0.24)
            let earXf: DragonAffine = headXf.translated(J.ear.x, J.ear.y).rotated(earAngle).translated(-J.ear.x, -J.ear.y)
            let blink: Double = max(DragonHabit.blink(at: t), M.arc((t - turnAt) / 0.18))
            let pull: Double = s.gaze != nil ? 0.5 + sin(t * 3) * 0.12 : 0.26
            steps.append(.head(DragonHeadDraw(
                xf: headXf, earXf: earXf, blink: blink, lidNear: max(blink, q(.lid), q(.lidN)),
                lidFar: max(blink, q(.lid), q(.lidF)), happy: q(.happy), look: DragonPoint(q(.gx), q(.gy)),
                open: q(.open), oval: q(.oval), pull: pull)))
        }
        func pawLayer(_ layer: DragonLayer, _ to: DragonPoint, _ rest: DragonPoint, _ angle: Double, _ extra: Double) {
            place(fb, to, angle * 0.55 + extra, layer, rest)
        }
        func nearArm() {
            _ = limb(.armNear, shoulderN, J.shoulderNear, J.pawNear, pawN, 0.55, 1.5)
        }
        if q(.front) > 0.5 {
            head()
            farSleeve()
            gear(.hug)
            pawLayer(.pawFar, pawF, J.pawFar, farAngle, q(.pawFa))
            gear(.mid)
            nearArm()
            pawLayer(.pawNear, pawN, J.pawNear, nearAngle, q(.pawNa))
        } else {
            farSleeve()
            pawLayer(.pawFar, pawF, J.pawFar, farAngle, q(.pawFa))
            head()
            gear(.hug)
            gear(.mid)
            nearArm()
            pawLayer(.pawNear, pawN, J.pawNear, nearAngle, q(.pawNa))
        }
        gear(.front)

        let unit: Double = atlas.units
        let lift: Double = max(-q(.y) * unit, 0)
        let stretch: Double = q(.y) < -8 ? M.clamp(abs(speed(.y)) / 2400, 0, 0.15) : 0
        return DragonFrame(
            steps: steps, tail: tailMesh, q: x, activity: current?.activity, beat: beat, pawN: pawN, pawF: pawF, hipW: hipW, lean: lean,
            squash: squash, wide: wide, stretch: stretch, lift: lift, flip: flipBody < 0 ? -1 : 1, sneeze: sneeze, stepBob: stepBob,
            hip: hip, shadow: (44 - M.clamp01(lift / 60) * 16, 1 - M.clamp01(lift / 90) * 0.6))
    }
}
