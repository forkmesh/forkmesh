import FMAssistant
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class GenieScreenPlayer {
    static let shared: GenieScreenPlayer = GenieScreenPlayer()
    nonisolated static let poll: Double = 0.25
    nonisolated static let look: Double = 1.8

    private(set) var acting: Bool = false

    @ObservationIgnored private var play: GenieScreenPlay = GenieScreenPlay()
    @ObservationIgnored private var focus: (rect: CGRect, start: Date, end: Date, sweep: Bool)?
    @ObservationIgnored private var thread: (rect: CGRect, end: Date)?
    @ObservationIgnored private var glance: (point: CGPoint, end: Date)?
    @ObservationIgnored private var show: Int?
    @ObservationIgnored private(set) var effect: GenieScreenEffect?
    @ObservationIgnored var hidden: Bool = false
    @ObservationIgnored var active: Bool = true

    var idle: Bool {
        let crawler: GenieCrawler = GenieCrawler.shared
        let hub: GenieHub = GenieHub.shared
        return active && !hidden && crawler.settled && !crawler.tumbling && crawler.parked == nil && !hub.holdsStill
            && !GenieHandCoach.shared.on && !UIAccessibility.isVoiceOverRunning && !AquariumDirector.shared.spiderPlaying
    }

    func run() async {
        try? await Task.sleep(for: .seconds(Double.random(in: GenieScreenPlay.firstGap)))
        while !Task.isCancelled {
            if idle { await attempt() }
            try? await Task.sleep(for: .seconds(Double.random(in: GenieScreenPlay.gap)))
        }
    }

    func look(at point: CGPoint, for seconds: Double = GenieScreenPlayer.look) {
        glance = (point, Date().addingTimeInterval(seconds))
    }

    func gaze(from spider: CGPoint, at now: Date) -> CGPoint? {
        if let focus = focus, now < focus.end {
            let progress: Double = now.timeIntervalSince(focus.start) / max(focus.end.timeIntervalSince(focus.start), 0.1)
            return GenieScreenPlay.gaze(from: spider, to: focus.rect, progress: progress, sweep: focus.sweep)
        }
        if let glance = glance, now < glance.end {
            let rect: CGRect = CGRect(origin: glance.point, size: .zero)
            return GenieScreenPlay.gaze(from: spider, to: rect, progress: 0, sweep: false)
        }
        return nil
    }

    func target(at now: Date) -> CGRect? {
        guard let focus = focus, now < focus.end else { return nil }
        return focus.rect
    }

    func thread(from spider: CGPoint, at now: Date) -> CGPoint? {
        guard let thread = thread, now < thread.end else { return nil }
        return GenieScreenPlay.thread(from: spider, to: thread.rect)
    }

    func stop() {
        if let show = show { GenieHandCoach.shared.cancel(show: show) }
        show = nil
        focus = nil
        thread = nil
        effect = nil
        acting = false
    }

    func marks(at now: Date) -> GenieScreenEffect? {
        guard let effect = effect, now < effect.end else { return nil }
        return effect
    }

    private func attempt() async {
        let crawler: GenieCrawler = GenieCrawler.shared
        let marks: [GenieLandmark] = GenieScreenScan.shared.current().filter { crawler.full.intersects($0.rect) }
        let travels: Bool = crawler.wanders && !UIAccessibility.isReduceMotionEnabled
        guard let choice = play.choose(marks, from: crawler.spot, travels: travels, random: { Double.random(in: 0..<1) }) else { return }
        acting = true
        if choice.act.travels {
            await travel(choice)
            return
        }
        let blockers: [CGRect] = GenieKeepClear.shared.buttons
        let blinks: Bool = choice.act.blinks && crawler.wanders && !UIAccessibility.isReduceMotionEnabled
        let target: CGPoint
        let edge: GenieEdge?
        if crawler.glass {
            target = GenieScreenPlay.beside(for: choice.mark, arena: crawler.arena, side: crawler.side, blockers: blockers)
            edge = nil
        } else {
            let perch: (CGPoint, GenieEdge) = GenieScreenPlay.perch(
                for: choice.mark, arena: crawler.arena, side: crawler.side, blockers: blockers)
            target = perch.0
            edge = perch.1
        }
        let going: Bool
        if blinks || !crawler.wanders {
            going = false
        } else if let edge = edge {
            going = crawler.visit(target, on: edge)
        } else {
            going = crawler.visit(target)
        }
        if going {
            let deadline: Date = Date().addingTimeInterval(GenieScreenPlay.approach)
            while crawler.track != nil, Date() < deadline {
                try? await Task.sleep(for: .seconds(GenieScreenPlayer.poll))
                if Task.isCancelled || interrupted {
                    stop()
                    return
                }
            }
        }
        guard !interrupted, crawler.track == nil else {
            stop()
            return
        }
        perform(choice)
        let seconds: Double = GenieTricks.named(choice.act.trick)?.duration ?? 2
        let start: Date = Date()
        if blinks {
            guard await wait(until: start.addingTimeInterval(seconds * GenieMagic.swap)) else { return }
            crawler.blink(to: target, on: edge)
        }
        guard await wait(until: start.addingTimeInterval(seconds + GenieHandCoach.lineLinger)) else { return }
        show = nil
        focus = nil
        thread = nil
        acting = false
    }

    private func travel(_ choice: GenieScreenChoice) async {
        let crawler: GenieCrawler = GenieCrawler.shared
        let trick: GenieTrick? = GenieTricks.named(choice.act.trick)
        let hold: Double = trick?.duration ?? 2
        guard let route = GenieScreenPlay.route(for: choice, from: crawler.spot, arena: crawler.arena, side: crawler.side),
            let timing = crawler.act(route, hold: hold)
        else {
            acting = false
            return
        }
        let resting: Date = timing.start.addingTimeInterval(timing.duration)
        effect = GenieScreenEffect(route: route, start: timing.start, duration: timing.duration, hold: hold)
        focus = (choice.mark.rect, timing.start, resting.addingTimeInterval(hold), choice.act.sweeps)
        guard await wait(until: resting) else { return }
        if let trick = trick {
            show = GenieHandCoach.shared.perform(GenieImprovisation(trick: trick, line: "", byModel: false), at: Date())
        }
        guard await wait(until: resting.addingTimeInterval(hold - GenieScreenPlayer.poll)) else { return }
        show = nil
        focus = nil
        acting = false
    }

    private func wait(until end: Date) async -> Bool {
        while Date() < end {
            try? await Task.sleep(for: .seconds(min(GenieScreenPlayer.poll, max(end.timeIntervalSinceNow, 0.01))))
            if Task.isCancelled || interrupted {
                stop()
                return false
            }
        }
        return true
    }

    private var interrupted: Bool {
        let crawler: GenieCrawler = GenieCrawler.shared
        let hub: GenieHub = GenieHub.shared
        return hidden || !active || crawler.held || crawler.tumbling || crawler.parked != nil || hub.holdsStill || hub.busy
            || GenieHandCoach.shared.on
    }

    private func perform(_ choice: GenieScreenChoice) {
        let now: Date = Date()
        let trick: GenieTrick? = GenieTricks.named(choice.act.trick)
        let seconds: Double = trick?.duration ?? 2
        focus = (choice.mark.rect, now, now.addingTimeInterval(seconds), choice.act.sweeps)
        if choice.act.threads { thread = (choice.mark.rect, now.addingTimeInterval(seconds + 0.6)) }
        if let trick = trick {
            show = GenieHandCoach.shared.perform(GenieImprovisation(trick: trick, line: "", byModel: false), at: now)
        }
    }
}
