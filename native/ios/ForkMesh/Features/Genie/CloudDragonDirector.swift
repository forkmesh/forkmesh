import FMAssistant
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class CloudDragonDirector {
    static let shared: CloudDragonDirector = CloudDragonDirector()
    nonisolated static let tickEvery: Duration = .milliseconds(250)
    nonisolated static let pokeSeconds: Double = 0.5

    struct Play {
        var activity: DragonActivity? = nil
        var age: Double = 0
        var facing: Double = 0
        var nap: DragonActivity? = nil
        var napAge: Double = 0
        var habit: DragonHabitCue? = nil
        var reaction: (reaction: DragonReaction, age: Double)? = nil
    }

    private(set) var lively: Bool = false

    @ObservationIgnored private var nap: DragonNap = DragonNap(now: Date().timeIntervalSinceReferenceDate)
    @ObservationIgnored private var napStart: Date = Date()
    @ObservationIgnored private var current: (activity: DragonActivity, start: Date, facing: Double)?
    @ObservationIgnored private var habit: (name: String, start: Date)?
    @ObservationIgnored private var reaction: (reaction: DragonReaction, start: Date)?
    @ObservationIgnored private var bag: [DragonActivity] = []
    @ObservationIgnored private var pokedAt: Date = .distantPast
    @ObservationIgnored private var lastTick: Date = Date()

    var napping: Bool { nap.ambient != nil }

    var playing: Bool { current != nil }

    func poke() {
        pokedAt = Date()
    }

    func draw(kinds: [DragonKind]) -> DragonActivity? {
        DragonActivity.drawn(from: &bag, kinds: kinds, random: { Double.random(in: 0..<1) })
    }

    func begin(_ activity: DragonActivity, facing: Double = 0, at now: Date = Date()) {
        nap.rouse(now: now.timeIntervalSinceReferenceDate)
        current = (activity, now, facing)
        habit = nil
        lively = true
    }

    func end() {
        current = nil
        nap.rouse(now: Date().timeIntervalSinceReferenceDate)
    }

    func perform(_ habitName: String, at now: Date = Date()) {
        habit = (habitName, now)
        lively = true
    }

    func react(_ next: DragonReaction, at now: Date = Date()) {
        reaction = (next, now)
        lively = true
    }

    func sleepNow(at now: Date = Date()) {
        current = nil
        nap.lullaby(now: now.timeIntervalSinceReferenceDate)
        napStart = now
        lively = false
    }

    func wakeNow(at now: Date = Date()) {
        current = nil
        nap.rouse(now: now.timeIntervalSinceReferenceDate)
    }

    func play(at now: Date) -> Play {
        var out: Play = Play()
        if let current = current {
            let age: Double = now.timeIntervalSince(current.start)
            if age < current.activity.duration {
                out.activity = current.activity
                out.age = max(age, 0)
                out.facing = current.facing
            }
        }
        if let ambient = nap.ambient {
            out.nap = ambient
            out.napAge = max(now.timeIntervalSince(napStart), 0)
        }
        if let habit = habit, let known = DragonHabit.named(habit.name) {
            let u: Double = now.timeIntervalSince(habit.start) / known.time
            if u >= 0 && u < 1 { out.habit = DragonHabitCue(name: habit.name, u: u) }
        }
        if let reaction = reaction {
            let age: Double = now.timeIntervalSince(reaction.start)
            if age >= 0 && age < reaction.reaction.duration { out.reaction = (reaction.reaction, age) }
        }
        return out
    }

    func run() async {
        while !Task.isCancelled {
            try? await Task.sleep(for: CloudDragonDirector.tickEvery)
            tick()
        }
    }

    func tick(at now: Date = Date()) {
        let hub: GenieHub = GenieHub.shared
        let dt: Double = min(max(now.timeIntervalSince(lastTick), 0), 1)
        lastTick = now
        guard hub.dragon else {
            if lively { lively = false }
            if nap.ambient != nil || current != nil { wakeNow(at: now) }
            return
        }
        if UIAccessibility.isReduceMotionEnabled {
            if nap.ambient != nil || current != nil { wakeNow(at: now) }
            if lively { lively = false }
            return
        }
        let crawler: GenieCrawler = GenieCrawler.shared
        let coach: GenieHandCoach = GenieHandCoach.shared
        let cued: Bool = GenieCues.current(GenieCues.shared.cue, at: now, ambient: false) != nil
        let still: Bool = crawler.settled && crawler.parked == nil
        let awake: Bool = hub.holdsStill || crawler.held || coach.engaged || coach.showing != nil
        let calm: Bool = still && !awake && current == nil && !cued && reaction == nil
        let poked: Bool = now.timeIntervalSince(pokedAt) < CloudDragonDirector.pokeSeconds
        let before: DragonActivity? = nap.ambient
        nap.step(now: now.timeIntervalSinceReferenceDate, dt: dt, poked: poked, awake: awake, calm: calm)
        if nap.ambient != before { napStart = now }
        if let reaction = reaction, now.timeIntervalSince(reaction.start) >= reaction.reaction.duration { self.reaction = nil }
        if let current = current, now.timeIntervalSince(current.start) >= current.activity.duration + 0.2 { self.current = nil }
        if let habit = habit, let known = DragonHabit.named(habit.name), now.timeIntervalSince(habit.start) >= known.time { self.habit = nil }
        let moving: Bool = crawler.moving || crawler.held || crawler.tumbling || !still
        let busy: Bool = hub.busy || hub.bubbleOpen || hub.sheetShown
        let active: Bool = moving || busy || cued || current != nil || nap.ambient == .stretch || coach.showing != nil || habit != nil
            || reaction != nil || GenieScreenPlayer.shared.acting
        if active != lively { lively = active }
    }
}
