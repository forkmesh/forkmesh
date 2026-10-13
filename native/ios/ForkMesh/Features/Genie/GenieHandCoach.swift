import FMAgentWatch
import FMAssistant
import FMBase
import Foundation
import Observation
import SwiftUI
import UIKit

struct GenieShow: Equatable {
    let id: Int
    let trick: GenieTrick
    let line: String
    let start: Date
    let byModel: Bool

    func frame(at now: Date) -> GenieTrickFrame? {
        trick.frame(age: now.timeIntervalSince(start))
    }
}

@MainActor
@Observable
final class GenieHandCoach {
    static let shared: GenieHandCoach = GenieHandCoach()
    nonisolated static let quietOff: TimeInterval = 120
    nonisolated static let lineLinger: Double = 1.2
    nonisolated static let askGap: TimeInterval = 6

    private(set) var on: Bool = false
    private(set) var state: GenieHandState = GenieHandState()
    private(set) var manipulation: GenieManipulation?
    private(set) var showing: GenieShow?
    private(set) var problem: String?
    private(set) var grabbing: Bool = false
    private(set) var thinking: Bool = false

    @ObservationIgnored private let camera: GenieDepthCamera = GenieDepthCamera()
    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private var reader: GenieHandReader = GenieHandReader()
    @ObservationIgnored private var play: GenieHandPlay = GenieHandPlay()
    @ObservationIgnored private(set) var taste: GenieTrickTaste
    @ObservationIgnored private var running: Bool = false
    @ObservationIgnored private var lastHands: Date = Date()
    @ObservationIgnored private var lastAsked: Date = .distantPast
    @ObservationIgnored private var shows: Int = 0
    @ObservationIgnored private var fade: Task<Void, Never>?
    @ObservationIgnored private var cachedModel: GenieModel??

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        self.prefs = prefs
        taste = GenieTrickTaste(encoded: prefs?.string(forKey: GenieTrickTaste.key) ?? "")
    }

    var available: Bool { GenieDepthCamera.available }

    var hasDepthSensor: Bool { GenieDepthCamera.trueDepth != nil }

    var engaged: Bool { on && state.hands > 0 }

    func toggle() {
        if on {
            turnOff()
        } else {
            turnOn()
        }
    }

    func turnOn() {
        guard !on else { return }
        on = true
        problem = nil
        lastHands = Date()
        FmHaptics.impact(.medium)
        GenieSounds.shared.play(.wakeOn)
        GenieCues.shared.play(.eureka)
        resume()
    }

    func turnOff(_ reason: String? = nil) {
        guard on else { return }
        on = false
        problem = reason
        pause()
        GenieSounds.shared.play(.wakeOff)
        GenieCues.shared.play(.nod)
    }

    func resume() {
        guard on, !running else { return }
        running = true
        camera.onHands = { (hands: [GenieHandPose], at: Date) in
            Task { @MainActor in GenieHandCoach.shared.feed(hands, at: at) }
        }
        camera.start { (failure: GenieCameraFailure?) in
            guard let failure = failure else { return }
            Task { @MainActor in GenieHandCoach.shared.failed(failure) }
        }
    }

    func pause() {
        guard running else { return }
        running = false
        camera.stop()
        reader = GenieHandReader()
        if grabbing {
            grabbing = false
            GenieCrawler.shared.drop()
        }
        state = GenieHandState()
        manipulation = nil
    }

    private func failed(_ failure: GenieCameraFailure) {
        running = false
        switch failure {
        case .denied: turnOff("Allow the camera in Settings to play with your hands.")
        case .noCamera: turnOff("This device has no front camera.")
        case .failed: turnOff("The camera did not start. Try again in a moment.")
        }
    }

    func feed(_ hands: [GenieHandPose], at now: Date) {
        guard on, running else { return }
        let events: [GenieHandEvent] = reader.read(hands, at: now)
        if reader.state != state { state = reader.state }
        if hands.isEmpty {
            if now.timeIntervalSince(lastHands) > GenieHandCoach.quietOff {
                turnOff("Hand play turned off after two quiet minutes.")
                return
            }
        } else {
            lastHands = now
        }
        let scene: GenieHandScene = self.scene
        var intents: [GenieHandIntent] = []
        for event in events {
            intents += play.react(event, state: state, scene: scene, at: now)
        }
        intents += play.tick(state: state, scene: scene, at: now)
        for intent in intents {
            apply(intent, at: now)
        }
    }

    private var scene: GenieHandScene {
        let crawler: GenieCrawler = GenieCrawler.shared
        let full: CGRect = crawler.full
        guard full.width > 0, full.height > 0 else { return GenieHandScene(spider: CGPoint(x: 0.5, y: 0.5), reach: 0.1, busy: true) }
        let hub: GenieHub = GenieHub.shared
        return GenieHandScene(
            spider: CGPoint(x: crawler.spot.x / full.width, y: crawler.spot.y / full.height), reach: Double(crawler.side * 0.6 / full.width),
            busy: hub.busy || hub.sheetShown || hub.menuOpen)
    }

    func screenPoint(_ fraction: CGPoint) -> CGPoint {
        let full: CGRect = GenieCrawler.shared.full
        return CGPoint(x: full.minX + fraction.x * full.width, y: full.minY + fraction.y * full.height)
    }

    var cursorPoint: CGPoint? {
        state.cursor.map { screenPoint($0) }
    }

    func gaze(from spider: CGPoint) -> CGPoint? {
        guard engaged, let hand = cursorPoint else { return nil }
        let dx: CGFloat = hand.x - spider.x
        let dy: CGFloat = hand.y - spider.y
        let length: CGFloat = max(hypot(dx, dy), 90)
        return CGPoint(x: dx / length, y: dy / length)
    }

    private func apply(_ intent: GenieHandIntent, at now: Date) {
        let crawler: GenieCrawler = GenieCrawler.shared
        switch intent {
        case .grab:
            grabbing = true
            crawler.grab()
            FmHaptics.impact(.medium)
            GenieSounds.shared.play(.spotted)
            GenieCues.shared.play(.surprise)
        case .carry(let point):
            crawler.carry(to: screenPoint(point))
        case .drop(let fling):
            grabbing = false
            if let fling = fling {
                let full: CGRect = crawler.full
                crawler.fling(CGSize(width: fling.width * full.width, height: fling.height * full.height))
                FmHaptics.impact(.heavy)
                GenieSounds.shared.play(.whoosh)
            } else {
                withAnimation(.spring(response: 0.4, dampingFraction: 0.72)) {
                    GenieHub.shared.dropped(parked: crawler.drop() != nil)
                }
            }
        case .trick(let id, _):
            guard let trick = GenieTricks.named(id) else { return }
            _ = perform(GenieImprovisation(trick: trick, line: trick.line, byModel: false), at: now)
        case .improvise(let situation):
            improvise(situation)
        case .crawl(let point):
            crawler.stroll(to: screenPoint(point))
            GenieCues.shared.play(.glance(point.x < 0.5 ? -1 : 1))
        case .recoil(let point):
            crawler.recoil(from: screenPoint(point))
            FmHaptics.impact(.rigid)
        case .manipulate(let next):
            manipulation = next
        case .settle(let done):
            withAnimation(.spring(response: 0.45, dampingFraction: 0.68)) {
                manipulation = nil
                if abs(done.scale - 1) > 0.05 { GenieHub.shared.resized(to: GenieHub.shared.scale * CGFloat(done.scale)) }
            }
        case .rate(let id, let delta):
            taste.rate(id, delta)
            prefs?.set(taste.encoded, forKey: GenieTrickTaste.key)
        }
    }

    @discardableResult
    func perform(_ act: GenieImprovisation, at now: Date = Date()) -> Int {
        shows += 1
        let show: GenieShow = GenieShow(id: shows, trick: act.trick, line: act.line, start: now, byModel: act.byModel)
        withAnimation(.spring(response: 0.35, dampingFraction: 0.75)) { showing = show }
        play.played(act.trick.id, at: now)
        if let chime = GenieHandCoach.chime(act.trick.sound) { GenieSounds.shared.play(chime) }
        let hub: GenieHub = GenieHub.shared
        let crawler: GenieCrawler = GenieCrawler.shared
        if act.trick.travel != .none, !grabbing, !hub.bubbleOpen, !hub.menuOpen, !hub.sheetShown, !UIAccessibility.isReduceMotionEnabled {
            crawler.tour(
                GenieCrawl.tour(act.trick.travel, from: crawler.spot, roam: crawler.roamArea, toward: cursorPoint), seconds: act.trick.duration)
        }
        let id: Int = show.id
        fade?.cancel()
        fade = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(act.trick.duration + GenieHandCoach.lineLinger))
            guard !Task.isCancelled, let self = self, self.showing?.id == id else { return }
            withAnimation(.easeOut(duration: 0.3)) { self.showing = nil }
        }
        return id
    }

    func cancel(show id: Int) {
        guard showing?.id == id else { return }
        fade?.cancel()
        withAnimation(.easeOut(duration: 0.2)) { showing = nil }
    }

    func improvise(_ situation: String) {
        let now: Date = Date()
        guard !thinking, now.timeIntervalSince(lastAsked) >= GenieHandCoach.askGap else {
            perform(GenieImproviser.fallback(recent: play.recent, taste: taste) { Double.random(in: 0..<1) })
            return
        }
        lastAsked = now
        thinking = true
        Task { @MainActor [weak self] in
            guard let self = self else { return }
            let act: GenieImprovisation = await GenieImproviser.improvise(
                scene: situation, recent: self.play.recent, taste: self.taste, vitals: GenieHub.shared.vitals, model: self.model())
            self.thinking = false
            self.perform(act)
        }
    }

    func surprise() {
        improvise("The user asked you for a surprise trick.")
    }

    func playTrick(_ wanted: GenieTrick?) async -> GenieImprovisation? {
        if let wanted = wanted {
            let act: GenieImprovisation = GenieImprovisation(trick: wanted, line: wanted.line, byModel: false)
            perform(act)
            return act
        }
        thinking = true
        lastAsked = Date()
        let act: GenieImprovisation = await GenieImproviser.improvise(
            scene: "The user asked you for a surprise trick.", recent: play.recent, taste: taste, vitals: GenieHub.shared.vitals, model: model())
        thinking = false
        perform(act)
        return act
    }

    private func model() -> GenieModel? {
        if let cached = cachedModel { return cached }
        let made: GenieModel? = GenieFeature.model()
        cachedModel = .some(made)
        return made
    }

    static func chime(_ sound: GenieTrickSound) -> GenieChime? {
        switch sound {
        case .none: return nil
        case .giggle: return .giggle
        case .boing: return .boing
        case .whoosh: return .whoosh
        case .dizzy: return .dizzy
        case .spotted: return .spotted
        case .appear: return .appear
        case .vanish: return .vanish
        case .reply: return .reply
        case .wakeOn: return .wakeOn
        case .listen: return .listen
        }
    }
}
