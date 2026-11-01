import FMAssistant
import SwiftUI

@MainActor
final class CloudDragonStage {
    private var body: DragonBody = DragonBody()
    private let origin: Date = Date()
    private var lastDate: Date?
    private var lastFrame: DragonFrame?
    private var lastPlace: CGPoint?
    private var lastPlaceAt: Date?
    private var velocity: CGVector = .zero
    private var rest: Double = 0

    func frame(_ date: Date, _ make: (inout DragonScene, CloudDragonStage) -> Void, art: CloudDragonArt) -> DragonFrame {
        if let last = lastDate, date <= last, let frame = lastFrame { return frame }
        var scene: DragonScene = DragonScene()
        make(&scene, self)
        scene.t = date.timeIntervalSince(origin) + 3
        if scene.reduced { scene.t = 3 }
        scene.vx = Double(velocity.dx)
        scene.vy = Double(velocity.dy)
        if abs(scene.facing) > 0.5 { rest = scene.facing < 0 ? -1 : 1 }
        scene.rest = rest
        let frame: DragonFrame = body.frame(scene, atlas: art.atlas)
        lastDate = date
        lastFrame = frame
        return frame
    }

    func follow(_ place: CGPoint?, turn: Double, at date: Date) {
        guard let place = place else {
            lastPlace = nil
            velocity = .zero
            return
        }
        defer {
            lastPlace = place
            lastPlaceAt = date
        }
        guard let before = lastPlace, let when = lastPlaceAt else { return }
        let seconds: Double = date.timeIntervalSince(when)
        guard seconds > 0.0005, seconds < 0.5 else { return }
        let dx: CGFloat = (place.x - before.x) / CGFloat(seconds)
        let dy: CGFloat = (place.y - before.y) / CGFloat(seconds)
        let angle: Double = turn * Double.pi / 180
        let lx: CGFloat = dx * CGFloat(cos(angle)) + dy * CGFloat(sin(angle))
        let ly: CGFloat = -dx * CGFloat(sin(angle)) + dy * CGFloat(cos(angle))
        let keep: CGFloat = 0.6
        velocity = CGVector(dx: velocity.dx * keep + lx * (1 - keep), dy: velocity.dy * keep + ly * (1 - keep))
    }
}

struct CloudDragonAvatar: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var mood: GenieMood = .idle
    var level: Double = 0
    var animated: Bool = true
    var frameRate: Double = 30
    var cue: GenieCue? = nil
    var ambient: Bool = false
    var vitals: GenieVitals? = nil
    var at: Date? = nil
    var stride: GenieStride = .still
    var show: GenieShow? = nil
    var gaze: CGPoint? = nil
    var alarm: Double = 0
    var place: CGPoint? = nil
    var turn: Double = 0
    var held: Bool = false
    var side: CGFloat = GenieCrawler.base

    @State private var stage: CloudDragonStage = CloudDragonStage()

    var body: some View {
        if let art = CloudDragonArt.shared {
            if let at = at {
                painted(at, art)
            } else if animated {
                TimelineView(.animation(minimumInterval: 1.0 / frameRate, paused: reduceMotion && mood != .speaking)) {
                    (timeline: TimelineViewDefaultContext) in
                    painted(timeline.date, art)
                }
            } else {
                painted(Date(), art)
            }
        }
    }

    private func painted(_ date: Date, _ art: CloudDragonArt) -> some View {
        stage.follow(reduceMotion ? nil : place, turn: turn, at: date)
        let frame: DragonFrame = stage.frame(date, { (scene: inout DragonScene, stage: CloudDragonStage) in compose(&scene, date) }, art: art)
        let trick: GenieTrickFrame? = reduceMotion ? nil : show?.frame(at: date)
        let painter: CloudDragonPainter = CloudDragonPainter(art: art, frame: frame, trick: trick)
        return GeometryReader { (proxy: GeometryProxy) in
            let box: CGFloat = min(proxy.size.width, proxy.size.height)
            Canvas(opaque: false, colorMode: .nonLinear, rendersAsynchronously: true) { (context: inout GraphicsContext, size: CGSize) in
                painter.paint(&context, size: size)
            }
            .frame(width: box * CloudDragonPainter.reach, height: box * CloudDragonPainter.reach)
            .position(x: proxy.size.width / 2, y: proxy.size.height / 2)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func local(_ point: CGPoint) -> DragonPoint {
        let angle: Double = turn * Double.pi / 180
        return DragonPoint(
            Double(point.x) * cos(angle) + Double(point.y) * sin(angle), -Double(point.x) * sin(angle) + Double(point.y) * cos(angle))
    }

    private func compose(_ scene: inout DragonScene, _ date: Date) {
        let calm: Bool = reduceMotion
        scene.reduced = calm
        scene.held = held
        scene.walking = calm ? 0 : stride.walking
        scene.air = calm ? 0 : stride.air
        scene.land = calm ? 0 : stride.land
        scene.facing = calm ? 0 : stride.facing
        scene.stridePhase = stride.phase * 11 / (Double(side) * 0.171)
        scene.hover = mood == .listening && !calm
        switch vitals?.level {
        case .sick: scene.tired = 0.45
        case .hungry: scene.tired = 0.28
        case .peckish: scene.tired = 0.12
        default: break
        }
        if !calm, let gaze = gaze { scene.gaze = local(gaze) }
        var overlay: DragonTarget = [:]
        let showing: GenieTrickFrame? = calm ? nil : show?.frame(at: date)
        if !calm, mood == .idle, showing == nil, let (reaction, age) = GenieCues.current(cue, at: date, ambient: false) {
            switch reaction {
            case .wave: scene.reaction = .wave
            case .cheer: scene.reaction = .cheer
            case .nod: scene.reaction = .nod
            case .eureka: scene.reaction = .eureka
            case .giggle: scene.reaction = .giggle
            case .surprise: scene.reaction = .surprise
            case .dizzy: scene.reaction = .dizzy
            case .glance(let direction):
                let k: Double = sin(Double.pi * min(max(age / reaction.duration, 0), 1))
                scene.gaze = DragonPoint(direction * k, 0)
            }
            scene.reactionAge = age
        }
        let seconds: Double = date.timeIntervalSinceReferenceDate
        switch mood {
        case .speaking:
            overlay[.open] = 0.6 * (0.5 + 0.5 * sin(seconds * 17)) * (0.65 + 0.35 * sin(seconds * 6.1))
        case .greeting:
            scene.reaction = calm ? nil : .wave
            scene.reactionAge = seconds.truncatingRemainder(dividingBy: 1.8)
            overlay[.open] = 0.5 * (0.5 + 0.5 * sin(seconds * 17))
        case .listening:
            overlay[.ear] = 0.14
            overlay[.head] = 0.05
        case .thinking:
            overlay[.gy] = -0.8
            overlay[.gx] = sin(seconds * 1.3) * 0.5
            overlay[.head] = -0.05
        case .idle:
            break
        }
        if !calm, alarm > 0.3 {
            overlay[.open] = max(overlay[.open] ?? 0, 0.4)
            overlay[.oval] = 1
            overlay[.wing] = 0.3
        }
        if let showing = showing { overlay.overlay(DragonTrick.overlay(showing)) }
        scene.overlay = overlay
        if ambient, !calm {
            let play: CloudDragonDirector.Play = CloudDragonDirector.shared.play(at: date)
            scene.activity = play.activity
            scene.activityAge = play.age
            if play.activity != nil, play.facing != 0 { scene.facing = play.facing }
            scene.ambient = play.nap
            scene.ambientAge = play.napAge
            scene.habit = play.habit
            if scene.reaction == nil, let forced = play.reaction {
                scene.reaction = forced.reaction
                scene.reactionAge = forced.age
            }
        }
    }
}
