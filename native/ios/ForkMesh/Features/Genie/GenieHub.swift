import FMAgentWatch
import FMApi
import FMAssistant
import FMBase
import Foundation
import Observation
import SwiftUI
import UIKit

struct GenieFlyer: Identifiable, Equatable {
    let id: Int
    let spot: GenieSpot
    let running: Bool
}

struct GenieTicker: Equatable {
    let id: Int
    let segments: [GenieSegment]
    let final: Bool
}

@MainActor
@Observable
final class GenieHub {
    static let shared: GenieHub = GenieHub()
    nonisolated static let alwaysKey: String = "genie/always_listening"
    nonisolated static let scaleKey: String = "genie/scale"
    nonisolated static let hiddenKey: String = "genie/hidden"
    nonisolated static let vitalsEvery: TimeInterval = 30 * 60
    nonisolated static let lineSeconds: Double = 5
    nonisolated static let lineRestart: TimeInterval = 60
    nonisolated static let minScale: CGFloat = 0.6
    nonisolated static let maxScale: CGFloat = 2.2
    nonisolated static let flyerSeconds: Double = 7
    nonisolated static let runningFlyerSeconds: Double = 2.4
    nonisolated static let tickerSeconds: Double = 5
    nonisolated static let heardSeconds: Double = 6
    nonisolated static let maxFlyers: Int = 3

    private(set) var session: GenieSession?
    var sheetShown: Bool = false
    private(set) var bubbleOpen: Bool = false
    private(set) var menuOpen: Bool = false
    private(set) var alwaysListening: Bool
    private(set) var scale: CGFloat
    private(set) var flyers: [GenieFlyer] = []
    private(set) var ticker: GenieTicker?
    private(set) var line: String?
    private(set) var vitals: GenieVitals?
    private(set) var finishedAgents: Int = 0
    private(set) var tab: AppTab?
    private(set) var spiderHidden: Bool
    private(set) var farewells: Int = 0

    @ObservationIgnored private var factory: (@MainActor () -> GenieSession)?
    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private var flyerCount: Int = 0
    @ObservationIgnored private var tickerCount: Int = 0
    @ObservationIgnored private var tickerFade: Task<Void, Never>?
    @ObservationIgnored private var collapse: Task<Void, Never>?
    @ObservationIgnored private var heardOpen: Bool = false
    @ObservationIgnored private var active: Bool = true
    @ObservationIgnored private var lineFade: Task<Void, Never>?
    @ObservationIgnored private var lastLine: Date = .distantPast
    @ObservationIgnored private var lineIndex: Int = 0
    @ObservationIgnored private var measuring: Bool = false
    @ObservationIgnored private var lastDizzy: Date = .distantPast
    @ObservationIgnored let activity: GenieActivityLog
    @ObservationIgnored var blocked: @MainActor () -> Bool = { false }

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        self.prefs = prefs
        alwaysListening = prefs?.bool(forKey: GenieHub.alwaysKey) ?? false
        scale = GenieHub.fitted(CGFloat(prefs?.double(forKey: GenieHub.scaleKey) ?? 1))
        spiderHidden = prefs?.bool(forKey: GenieHub.hiddenKey) ?? false
        activity = GenieActivityLog(store: prefs)
        vitals = GenieVitalSigns.load(prefs).map { GenieVitals($0) }
        let motion: GenieMotion = GenieMotion.shared
        motion.onShake = { [weak self] in self?.shaken() }
        motion.onBump = { [weak self] in self?.bumped() }
        motion.onJolt = { GenieCrawler.shared.jostle($0) }
        let crawler: GenieCrawler = GenieCrawler.shared
        crawler.still = { [weak self] in self?.holdsStill ?? true }
        crawler.vitality = { [weak self] in self?.vitals?.health ?? 0.7 }
        crawler.lean = { GenieMotion.shared.lean }
        crawler.gravity = { GenieMotion.shared.gravity }
        crawler.tilt = { GenieMotion.shared.tilt }
        crawler.flat = { GenieMotion.shared.flat }
        crawler.floats = { GenieMotion.shared.live }
        crawler.avoid = { GenieKeepClear.shared.buttons }
        crawler.knocked = { [weak self] in self?.knocked($0) }
        crawler.loose = { GenieMotion.shared.quick($0) }
        if spiderHidden { crawler.hidden = true }
    }

    var holdsStill: Bool {
        bubbleOpen || menuOpen || sheetShown || busy || ticker != nil || line != nil || !flyers.isEmpty || !active
            || spiderHidden || GenieHandCoach.shared.engaged || GenieOutreachCoach.shared.offer != nil
    }

    var calm: Bool {
        active && !spiderHidden && !blocked() && !menuOpen && !bubbleOpen && !sheetShown && !busy && line == nil
            && ticker == nil && flyers.isEmpty && !GenieHandCoach.shared.engaged
    }

    var busy: Bool { session?.isBusy ?? false }

    func attach(_ make: @escaping @MainActor () -> GenieSession) {
        factory = make
        GenieSounds.shared.load()
        if alwaysListening && !spiderHidden && PromptDictation.allowed {
            ensureSession()?.setAlwaysListening(true)
        }
    }

    @discardableResult
    func ensureSession() -> GenieSession? {
        if let session = session { return session }
        guard let factory = factory else { return nil }
        let made: GenieSession = factory()
        made.onSpot = { [weak self] (spot: GenieSpot, runs: Bool) in self?.spotted(spot, runs: runs) }
        made.onReply = { (_: String) in GenieSounds.shared.play(.reply) }
        made.onFinish = { [weak self] in self?.finished() }
        made.canOverhear = { [weak self] in self?.mayOverhear ?? false }
        made.greeter = { [weak self] in self?.hints().first }
        made.onTakeUp = { [weak self] in self?.tookUp() }
        made.onSettle = { [weak self] in self?.settled() }
        made.onStopListening = { [weak self] in self?.setAlwaysListening(false) }
        session = made
        return made
    }

    var mayOverhear: Bool { active && !spiderHidden && !blocked() && !OtherAppCall.inProgress }

    func tap() {
        FmHaptics.impact(.light)
        if menuOpen {
            closeMenu()
            return
        }
        if bubbleOpen {
            closeBubble()
            return
        }
        say()
    }

    func say(now: Date = Date()) {
        if now.timeIntervalSince(lastLine) > GenieHub.lineRestart { lineIndex = 0 }
        let lines: [String] = GenieHints.lines(log: activity.entries, vitals: vitals, context: hintContext, now: now)
        guard !lines.isEmpty else { return }
        let next: String = lines[lineIndex % lines.count]
        lineIndex += 1
        lastLine = now
        show(line: next)
    }

    func show(line next: String) {
        GenieCrawler.shared.halt()
        GenieCues.shared.play(.wave)
        withAnimation(.spring(response: 0.4, dampingFraction: 0.72)) { line = next }
        UIAccessibility.post(notification: .announcement, argument: next)
        lineFade?.cancel()
        lineFade = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(GenieHub.lineSeconds))
            if !Task.isCancelled { self?.dismissLine() }
        }
    }

    func openSheet() {
        collapse?.cancel()
        heardOpen = false
        GenieCrawler.shared.halt()
        menuOpen = false
        guard ensureSession() != nil else { return }
        if bubbleOpen {
            session?.close()
            bubbleOpen = false
        }
        GenieSounds.shared.play(.appear)
        sheetShown = true
    }

    func openBubble() {
        collapse?.cancel()
        heardOpen = false
        guard let session = ensureSession() else { return }
        GenieCrawler.shared.halt()
        withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) {
            bubbleOpen = true
            menuOpen = false
            line = nil
        }
        GenieSounds.shared.play(.appear)
        GenieCues.shared.play(.wave)
        session.open()
    }

    func closeBubble() {
        collapse?.cancel()
        heardOpen = false
        session?.close()
        withAnimation(.easeOut(duration: 0.22)) {
            bubbleOpen = false
        }
        GenieSounds.shared.play(.vanish)
    }

    func openMenu() {
        collapse?.cancel()
        heardOpen = false
        GenieCrawler.shared.halt()
        FmHaptics.impact(.medium)
        GenieSounds.shared.play(.spotted)
        GenieCues.shared.play(.surprise)
        if bubbleOpen {
            session?.close()
        }
        withAnimation(.spring(response: 0.36, dampingFraction: 0.76)) {
            bubbleOpen = false
            line = nil
            menuOpen = true
        }
        refreshVitals()
    }

    func closeMenu() {
        withAnimation(.easeOut(duration: 0.2)) {
            menuOpen = false
        }
    }

    func expand() {
        openSheet()
    }

    func shrink() {
        session?.close()
        sheetShown = false
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(450))
            self?.openBubble()
        }
    }

    var attached: Bool { factory != nil }

    var listensNow: Bool { session?.alwaysListening ?? false }

    func toggleAlwaysListening() {
        setAlwaysListening(!alwaysListening)
    }

    func setAlwaysListening(_ on: Bool) {
        if on && spiderHidden { setSpiderHidden(false) }
        alwaysListening = on
        prefs?.set(on, forKey: GenieHub.alwaysKey)
        FmHaptics.impact(on ? .medium : .light)
        GenieSounds.shared.play(on ? .wakeOn : .wakeOff)
        GenieCues.shared.play(on ? .eureka : .nod)
        ensureSession()?.setAlwaysListening(on)
        if !on {
            ticker = nil
        }
    }

    func setSpiderHidden(_ hide: Bool, fromMenu: Bool = false) {
        guard hide != spiderHidden else { return }
        prefs?.set(hide, forKey: GenieHub.hiddenKey)
        guard hide else {
            spiderHidden = false
            return
        }
        collapse?.cancel()
        lineFade?.cancel()
        tickerFade?.cancel()
        GenieCrawler.shared.hidden = true
        GenieScreenPlayer.shared.hidden = true
        GenieScreenPlayer.shared.stop()
        GenieHandCoach.shared.turnOff()
        GenieHandCoach.shared.pause()
        if alwaysListening {
            alwaysListening = false
            prefs?.set(false, forKey: GenieHub.alwaysKey)
        }
        session?.setAlwaysListening(false)
        session?.close()
        sheetShown = false
        bubbleOpen = false
        menuOpen = false
        line = nil
        ticker = nil
        flyers = []
        GenieOutreachCoach.shared.dismiss()
        spiderHidden = true
        if fromMenu {
            GenieSounds.shared.play(.vanish)
            farewells += 1
        }
    }

    func alwaysListeningFailed() {
        guard alwaysListening, session?.alwaysListening == false else { return }
        alwaysListening = false
        prefs?.set(false, forKey: GenieHub.alwaysKey)
    }

    func scene(active now: Bool) {
        active = now
        GenieOutreachCoach.shared.scene(active: now)
        if !now {
            PromptDictation.yieldOverhearing()
        }
    }

    func dropped(parked: Bool) {
        GenieSounds.shared.play(.boing)
        GenieCues.shared.play(parked ? .nod : .giggle)
        FmHaptics.impact(parked ? .medium : .light)
    }

    nonisolated static func fitted(_ wanted: CGFloat) -> CGFloat {
        guard wanted.isFinite, wanted > 0 else { return 1 }
        return min(max(wanted, GenieHub.minScale), GenieHub.maxScale)
    }

    func resized(to next: CGFloat) {
        scale = GenieHub.fitted(next)
        prefs?.set(Double(scale), forKey: GenieHub.scaleKey)
        FmHaptics.impact(.light)
        GenieSounds.shared.play(.boing)
        GenieCues.shared.play(.giggle)
    }

    func resetPlace() {
        withAnimation(.spring(response: 0.45, dampingFraction: 0.7)) {
            GenieCrawler.shared.reset()
            resized(to: 1)
        }
    }

    private var hintContext: GenieHintContext {
        GenieHintContext(
            unread: Services.get(NotificationService.self)?.unreadCount ?? 0, running: ShellCounts.shared.agents,
            finished: finishedAgents, tab: tab)
    }

    func hints(now: Date = Date()) -> [GenieHint] {
        GenieHints.suggest(log: activity.entries, vitals: vitals, context: hintContext, now: now)
    }

    func runHint(_ hint: GenieHint) {
        guard let spot = hint.spot, let session = ensureSession() else { return }
        FmHaptics.impact(.medium)
        GenieSounds.shared.play(.whoosh)
        GenieCues.shared.play(.cheer)
        GenieCrawler.shared.halt()
        withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) {
            line = nil
            menuOpen = false
            if !sheetShown { bubbleOpen = true }
        }
        session.perform(spot, said: spot.title)
    }

    func dismissLine() {
        lineFade?.cancel()
        withAnimation(.easeOut(duration: 0.3)) { line = nil }
    }

    func record(tab next: AppTab) {
        tab = next
        if next == .agents { finishedAgents = 0 }
        activity.record(GenieActivity.tab(next))
    }

    func record(route: Route) {
        if route.kind == .agents || route.kind == .agent { finishedAgents = 0 }
        activity.record(GenieActivity.page(route.kind))
    }

    func agentStarted() {
        activity.record(GenieActivity.agentStarted)
    }

    func agentsChanged(from old: Int, to new: Int) {
        GenieCues.shared.agentsChanged(from: old, to: new)
        if new < old {
            finishedAgents += old - new
        }
    }

    func freshVitals() async -> GenieVitals? {
        await measure(force: false)
        return vitals
    }

    func refreshVitals(force: Bool = false) {
        Task { @MainActor [weak self] in
            await self?.measure(force: force)
        }
    }

    private func measure(force: Bool) async {
        if let known = vitals, !force, Date().timeIntervalSince(known.signs.measuredAt) < GenieHub.vitalsEvery { return }
        guard !measuring, let toolbox = LocalAIFeature.toolbox() else { return }
        measuring = true
        defer { measuring = false }
        let since: Date = Date().addingTimeInterval(-GenieVitals.window)
        let signs: GenieVitalSigns = await toolbox.vitalSigns(marketingVisits: activity.marketingVisits(since: since))
        signs.save(prefs)
        let before: GenieVitalLevel? = vitals?.level
        let next: GenieVitals = GenieVitals(signs)
        vitals = next
        if let before = before, GenieVitalLevel.allCases.firstIndex(of: next.level) ?? 0 < GenieVitalLevel.allCases.firstIndex(of: before) ?? 0 {
            GenieCues.shared.play(.cheer)
        }
    }

    func run(_ flyer: GenieFlyer) {
        FmHaptics.impact(.medium)
        GenieSounds.shared.play(.whoosh)
        GenieCues.shared.play(.cheer)
        drop(flyer.id)
        guard let session = ensureSession() else { return }
        GenieCrawler.shared.halt()
        if !bubbleOpen && !sheetShown {
            withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) {
                bubbleOpen = true
            }
        }
        session.perform(flyer.spot, said: tickerText(for: flyer))
    }

    func drop(_ id: Int) {
        withAnimation(.easeOut(duration: 0.25)) {
            flyers.removeAll { $0.id == id }
        }
    }

    func heard(_ text: String, spot: GenieSpot?, final: Bool, fresh: Bool) {
        let words: String = text.dartTrimmed
        guard !words.isEmpty else { return }
        if fresh || ticker == nil { tickerCount += 1 }
        let cut: Int = words.count > 90 ? words.count - 89 : 0
        let lead: Int = cut > 0 ? 1 : 0
        let shown: String = cut > 0 ? "…" + String(words.suffix(89)) : words
        let range: Range<Int>? = spot.map { (found: GenieSpot) -> Range<Int> in
            let lower: Int = max(found.phrase.lowerBound - cut + lead, lead)
            return lower..<max(found.phrase.upperBound - cut + lead, lower)
        }
        ticker = GenieTicker(id: tickerCount, segments: GenieSpotter.segments(shown, highlight: range), final: final)
        tickerFade?.cancel()
        tickerFade = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(final ? GenieHub.tickerSeconds : GenieHub.tickerSeconds * 2))
            if !Task.isCancelled {
                withAnimation(.easeOut(duration: 0.4)) { self?.ticker = nil }
            }
        }
    }

    private func tickerText(for flyer: GenieFlyer) -> String? {
        guard let ticker = ticker else { return nil }
        let text: String = ticker.segments.map(\.text).joined()
        return text.isEmpty ? nil : text
    }

    private func spotted(_ spot: GenieSpot, runs: Bool) {
        flyerCount += 1
        let flyer: GenieFlyer = GenieFlyer(id: flyerCount, spot: spot, running: runs)
        withAnimation(.spring(response: 0.42, dampingFraction: 0.62)) {
            flyers = Array((flyers.filter { $0.spot.tool != spot.tool || $0.spot.arguments != spot.arguments } + [flyer]).suffix(GenieHub.maxFlyers))
        }
        GenieSounds.shared.play(.spotted)
        GenieCues.shared.play(.eureka)
        let id: Int = flyer.id
        let seconds: Double = flyer.running ? GenieHub.runningFlyerSeconds : GenieHub.flyerSeconds
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            self?.drop(id)
        }
    }

    private func tookUp() {
        collapse?.cancel()
        GenieCrawler.shared.halt()
        guard !bubbleOpen, !sheetShown, !menuOpen else { return }
        heardOpen = true
        withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) {
            bubbleOpen = true
            line = nil
        }
        GenieSounds.shared.play(.appear)
    }

    private func settled() {
        guard heardOpen else { return }
        heardOpen = false
        collapse?.cancel()
        collapse = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(GenieHub.heardSeconds))
            guard !Task.isCancelled, let self = self, !self.busy, self.bubbleOpen else { return }
            withAnimation(.easeOut(duration: 0.25)) {
                self.bubbleOpen = false
            }
        }
    }

    private func finished() {
        GenieSounds.shared.play(.whoosh)
        if sheetShown {
            sheetShown = false
            return
        }
        collapse?.cancel()
        collapse = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(2.2))
            guard !Task.isCancelled, let self = self, !self.busy else { return }
            withAnimation(.easeOut(duration: 0.25)) {
                self.bubbleOpen = false
            }
        }
    }

    private func shaken() {
        guard !spiderHidden else { return }
        guard !GenieCrawler.shared.tumbling else { return }
        GenieCrawler.shared.shakeLoose()
        GenieCues.shared.play(.dizzy)
        GenieSounds.shared.play(.dizzy)
    }

    private func bumped() {
        guard !spiderHidden else { return }
        guard !GenieCrawler.shared.tumbling else { return }
        GenieCues.shared.play(.giggle)
        GenieSounds.shared.play(.giggle)
    }

    private func knocked(_ knock: GenieKnock) {
        let now: Date = Date()
        guard knock.dizzying, now.timeIntervalSince(lastDizzy) > GenieReaction.dizzy.duration else { return }
        lastDizzy = now
        GenieCues.shared.play(.dizzy)
        GenieSounds.shared.play(.dizzy)
    }
}
