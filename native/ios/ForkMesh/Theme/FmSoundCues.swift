import AVFoundation
import FMAccount
import FMAgentWatch
import FMApi
import FMBase
import Foundation
import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass

typealias FmCue = InterfaceCue

private let cueCopies = 2

enum FmSoundCues {
    static var isEnabled: Bool {
        guard let prefs: KeyValueStore = Services.get(KeyValueStore.self) else { return true }
        return SettingsService.soundsEnabled(prefs)
    }

    static func play(_ cue: FmCue) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated { FmCuePlayer.shared.hold(cue, always: false) }
        }
    }

    static func preview(_ cue: FmCue) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated { FmCuePlayer.shared.hold(cue, always: true) }
        }
    }

    @MainActor static func ratchet(_ lines: Int, changed: Int, flagged: Int, sound: Bool, haptic: Bool) {
        guard lines > 0 else { return }
        FmCuePlayer.shared.ratchet(lines, changed: changed, flagged: flagged, sound: sound, haptic: haptic)
    }

    static func warmUp() {
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                guard isEnabled else { return }
                FmCuePlayer.shared.activateSession()
                FmCuePlayer.shared.load()
            }
        }
    }

    static func resume() {
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                if isEnabled { FmCuePlayer.shared.activateSession() }
            }
        }
    }
}

@MainActor
final class FmCuePlayer {
    static let shared = FmCuePlayer()
    private var batch = InterfaceCueBatch()
    private var forced = false
    private var gate = InterfaceCueGate()
    private var fallback = InterfaceCueFallback()
    private var voices: [FmCue: [AVAudioPlayer]] = [:]
    private var turns: [FmCue: Int] = [:]
    private var loading = false
    private var ratchetVoices: [RatchetBurst: [AVAudioPlayer]] = [:]
    private var catchVoices: [AVAudioPlayer] = []
    private var ratchetGate = RatchetGate()
    private var ratchetTurn = 0
    private var ratchetSound = true
    private var ratchetHaptic = true
    private var quiet = 0

    func hold(_ cue: FmCue, always: Bool) {
        fallback.heard(atMs: fmUptimeMs())
        guard always || (quiet == 0 && FmSoundCues.isEnabled) else { return }
        forced = forced || always
        guard batch.hold(cue) else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { self.flush() }
        }
    }

    var isQuiet: Bool { quiet > 0 }

    func quieten(_ on: Bool) {
        quiet = max(0, quiet + (on ? 1 : -1))
    }

    func touched(sinceMs: Int64, from shape: InterfaceScreenShape, tapped: Bool, in window: UIWindow) {
        let delay = Int(tapped ? kCueFallbackDelayMs : kCueSlideSettleMs)
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(delay)) { [weak window] in
            MainActor.assumeIsolated {
                guard let window, FmCuePlayer.shared.fallback.silent(sinceMs: sinceMs) else { return }
                guard let cue = InterfaceCue.afterTouch(from: shape, to: window.fmScreenShape, tapped: tapped) else { return }
                FmCuePlayer.shared.hold(cue, always: false)
            }
        }
    }

    func load() {
        guard voices.isEmpty, !loading else { return }
        loading = true
        Task.detached(priority: .utility) {
            var rendered: [FmCue: [Data]] = [:]
            for cue in FmCue.allCases {
                if cue == .keystroke {
                    rendered[cue] = (1...kKeystrokeVariants).map { wavData(renderCue(cue, seed: keystrokeSeed($0))) }
                } else {
                    rendered[cue] = [Data](repeating: wavData(renderCue(cue)), count: cueCopies)
                }
            }
            var bursts: [RatchetBurst: [Data]] = [:]
            for burst in RatchetBurst.all {
                bursts[burst] = (1...kRatchetVariants).map { wavData(burst.render(seed: keystrokeSeed($0))) }
            }
            let catches = (1...kRatchetVariants).map { wavData(renderRatchetCatch(seed: keystrokeSeed($0))) }
            let clips = rendered
            let ratchet = bursts
            await MainActor.run { FmCuePlayer.shared.install(clips, ratchet: ratchet, catches: catches) }
        }
    }

    private func install(_ rendered: [FmCue: [Data]], ratchet: [RatchetBurst: [Data]], catches: [Data]) {
        loading = false
        for (cue, clips) in rendered {
            voices[cue] = prepared(clips)
        }
        for (burst, clips) in ratchet {
            ratchetVoices[burst] = prepared(clips)
        }
        catchVoices = prepared(catches)
    }

    private func prepared(_ clips: [Data]) -> [AVAudioPlayer] {
        clips.compactMap { (clip: Data) -> AVAudioPlayer? in
            guard let player = try? AVAudioPlayer(data: clip) else { return nil }
            player.prepareToPlay()
            return player
        }
    }

    private func flush() {
        let always = forced
        forced = false
        guard let cue = batch.take() else { return }
        guard always || (quiet == 0 && FmSoundCues.isEnabled) else { return }
        guard gate.admit(cue, atMs: fmUptimeMs()), sessionReady() else { return }
        guard let players = voices[cue], !players.isEmpty else {
            load()
            return
        }
        let turn = turns[cue, default: 0]
        turns[cue] = turn &+ 1
        let player = players[turn % players.count]
        player.currentTime = 0
        player.play()
    }

    func ratchet(_ lines: Int, changed: Int, flagged: Int, sound: Bool, haptic: Bool) {
        ratchetSound = sound
        ratchetHaptic = haptic
        guard (sound && FmSoundCues.isEnabled) || (haptic && FmHaptics.enabled) else { return }
        guard let stroke = ratchetGate.pass(lines: lines, changed: changed, flagged: flagged) else { return }
        strike(stroke)
        closeRatchetLater()
    }

    private func closeRatchetLater() {
        DispatchQueue.main.asyncAfter(deadline: .now() + kRatchetBatchSeconds) {
            MainActor.assumeIsolated {
                let player = FmCuePlayer.shared
                guard let stroke = player.ratchetGate.close() else { return }
                player.strike(stroke)
                player.closeRatchetLater()
            }
        }
    }

    private func strike(_ stroke: RatchetStroke) {
        if ratchetHaptic { FmHaptics.ratchet(stroke) }
        guard ratchetSound, FmSoundCues.isEnabled, sessionReady() else { return }
        guard let players = ratchetVoices[stroke.burst], !players.isEmpty else {
            load()
            return
        }
        ratchetTurn &+= 1
        restart(players[ratchetTurn % players.count])
        if stroke.caught, !catchVoices.isEmpty {
            restart(catchVoices[ratchetTurn % catchVoices.count])
        }
    }

    private func restart(_ player: AVAudioPlayer) {
        player.currentTime = 0
        player.play()
    }

    func activateSession() {
        if Self.recording || PromptDictation.isOverhearing || OtherAppCall.inProgress { return }
        let session = AVAudioSession.sharedInstance()
        if session.category != .playback || !session.categoryOptions.contains(.mixWithOthers) {
            try? session.setCategory(.playback, options: [.mixWithOthers])
        }
        try? session.setActive(true)
    }

    private static var recording: Bool {
        PromptDictation.isListening || MessagesVoiceRecorder.isRecording
    }

    private func sessionReady() -> Bool {
        if Self.recording || OtherAppCall.inProgress { return false }
        if PromptDictation.isOverhearing { return true }
        let session = AVAudioSession.sharedInstance()
        if session.category == .playback && session.categoryOptions.contains(.mixWithOthers) { return true }
        try? session.setCategory(.playback, options: [.mixWithOthers])
        try? session.setActive(true)
        return true
    }
}

private func fmUptimeMs() -> Int64 {
    Int64(DispatchTime.now().uptimeNanoseconds / 1_000_000)
}

extension UIWindow {
    var fmScreenShape: InterfaceScreenShape {
        var presented = 0
        var pushed = 0
        var controller = rootViewController
        while let current = controller {
            pushed += fmStackDepth(current)
            guard let next = current.presentedViewController, !next.isBeingDismissed else { break }
            presented += 1
            controller = next
        }
        return InterfaceScreenShape(presented: presented, pushed: pushed)
    }
}

@MainActor
private func fmStackDepth(_ controller: UIViewController) -> Int {
    let own = (controller as? UINavigationController)?.viewControllers.count ?? 0
    return controller.children.reduce(own) { (sum: Int, child: UIViewController) in sum + fmStackDepth(child) }
}

final class FmTouchSoundRecognizer: UIGestureRecognizer, UIGestureRecognizerDelegate {
    private var start: CGPoint?
    private var startMs: Int64 = 0
    private var shape = InterfaceScreenShape(presented: 0, pushed: 0)
    private var moved = false

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        guard start == nil, let window = view as? UIWindow, let touch = touches.first else { return }
        start = touch.location(in: window)
        startMs = fmUptimeMs()
        shape = window.fmScreenShape
        moved = false
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        guard let start, let window = view, let touch = touches.first else { return }
        let at = touch.location(in: window)
        if hypot(at.x - start.x, at.y - start.y) > 10 { moved = true }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        finish(event)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        moved = true
        finish(event)
    }

    override func reset() {
        super.reset()
        start = nil
    }

    override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer
    ) -> Bool { true }

    private func finish(_ event: UIEvent) {
        guard start != nil else { return }
        let held = event.allTouches?.contains { (touch: UITouch) in touch.phase != .ended && touch.phase != .cancelled } ?? false
        if held { return }
        start = nil
        if let window = view as? UIWindow {
            FmCuePlayer.shared.touched(sinceMs: startMs, from: shape, tapped: !moved, in: window)
        }
        state = .failed
    }
}

private final class FmTouchSoundAnchor: UIView {
    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        guard let window else { return }
        if (window.gestureRecognizers ?? []).contains(where: { (recognizer: UIGestureRecognizer) in recognizer is FmTouchSoundRecognizer }) {
            return
        }
        let recognizer = FmTouchSoundRecognizer(target: nil, action: nil)
        recognizer.cancelsTouchesInView = false
        recognizer.delaysTouchesBegan = false
        recognizer.delaysTouchesEnded = false
        recognizer.delegate = recognizer
        window.addGestureRecognizer(recognizer)
    }
}

private struct FmTouchSoundInstaller: UIViewRepresentable {
    func makeUIView(context: Context) -> FmTouchSoundAnchor { FmTouchSoundAnchor() }

    func updateUIView(_ uiView: FmTouchSoundAnchor, context: Context) {}
}

struct FmToggleSoundStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        FmToggleSoundBody(configuration: configuration)
    }
}

private struct FmToggleSoundBody: View {
    let configuration: ToggleStyleConfiguration
    @State private var shownAt: Date = .distantFuture

    var body: some View {
        Toggle(configuration)
            .onAppear { shownAt = Date() }
            .onChange(of: configuration.isOn) { (_: Bool, on: Bool) in
                if Date().timeIntervalSince(shownAt) > 0.4 {
                    FmSoundCues.play(.toggle(on))
                }
            }
    }
}

extension View {
    func fmInterfaceSounds(router: AppRouter) -> some View {
        self
            .toggleStyle(FmToggleSoundStyle())
            .onChange(of: router.selectedTab) { (_: AppTab, _: AppTab) in
                FmSoundCues.play(.navigate)
            }
            .onChange(of: router.currentPath.count) { (before: Int, after: Int) in
                FmSoundCues.play(after > before ? .navigate : .close)
            }
            .onChange(of: router.presented) { (before: Route?, after: Route?) in
                if after != nil {
                    FmSoundCues.play(.open)
                } else if before != nil {
                    FmSoundCues.play(.close)
                }
            }
            .background(FmTouchSoundInstaller().allowsHitTesting(false))
            .task { FmSoundCues.warmUp() }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
                FmSoundCues.resume()
            }
    }

    func fmQuietSounds() -> some View {
        self
            .onAppear { FmCuePlayer.shared.quieten(true) }
            .onDisappear { FmCuePlayer.shared.quieten(false) }
    }

    func fmRefreshable(action: @escaping @Sendable () async -> Void) -> some View {
        refreshable {
            FmSoundCues.play(.click)
            await action()
        }
    }
}
