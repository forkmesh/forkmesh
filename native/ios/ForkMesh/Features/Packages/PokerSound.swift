import AVFoundation
import FMAccount
import FMApi
import FMBase
import FMPackages
import Foundation
import UIKit

@MainActor
final class PokerSound {
    static let shared = PokerSound()
    static let mutedKey = "fm.poker.muted"
    private static let voiceCount = 6

    var muted: Bool {
        get { UserDefaults.standard.bool(forKey: Self.mutedKey) }
        set { UserDefaults.standard.set(newValue, forKey: Self.mutedKey) }
    }

    private let engine = AVAudioEngine()
    private var players: [AVAudioPlayerNode] = []
    private var nextPlayer = 0
    private var format: AVAudioFormat?
    private var clips: [PokerCue: [Float]] = [:]
    private var buffers: [PokerCue: AVAudioPCMBuffer] = [:]
    private var rendering = false
    private var generation = 0

    private init() {}

    func warmUp() {
        guard clips.isEmpty, !rendering else { return }
        rendering = true
        Task.detached(priority: .utility) {
            var rendered: [PokerCue: [Float]] = [:]
            for cue in PokerCue.allCases {
                rendered[cue] = PokerSynth.render(cue, rate: PokerSynth.sampleRate)
            }
            let done = rendered
            await MainActor.run {
                PokerSound.shared.clips = done
                PokerSound.shared.rendering = false
            }
        }
    }

    func play(_ cue: PokerCue) {
        guard !muted else { return }
        guard let buffer = buffer(cue) else {
            warmUp()
            return
        }
        guard sessionReady(), startEngine() else { return }
        let player = players[nextPlayer]
        nextPlayer = (nextPlayer + 1) % players.count
        guard engine.isRunning, player.engine === engine else { return }
        player.scheduleBuffer(buffer, at: nil, options: .interrupts, completionHandler: nil)
        if !player.isPlaying { player.play() }
    }

    func play(_ events: [PokerEvent]) {
        guard !muted else { return }
        let steps = PokerCues.plan(events)
        guard !steps.isEmpty else { return }
        generation += 1
        let current = generation
        for step in steps {
            if step.delayMs == 0 {
                play(step.cue)
                continue
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(step.delayMs)) {
                MainActor.assumeIsolated {
                    guard PokerSound.shared.generation == current else { return }
                    PokerSound.shared.play(step.cue)
                }
            }
        }
    }

    func stop() {
        generation += 1
        for player in players where player.isPlaying { player.stop() }
        if engine.isRunning { engine.pause() }
    }

    private func buffer(_ cue: PokerCue) -> AVAudioPCMBuffer? {
        if let ready = buffers[cue] { return ready }
        guard let samples = clips[cue], !samples.isEmpty, let format = audioFormat(),
            let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count)),
            let channel = buffer.floatChannelData
        else { return nil }
        buffer.frameLength = AVAudioFrameCount(samples.count)
        samples.withUnsafeBufferPointer { (source: UnsafeBufferPointer<Float>) in
            for index in 0..<samples.count { channel[0][index] = source[index] }
        }
        buffers[cue] = buffer
        return buffer
    }

    private func audioFormat() -> AVAudioFormat? {
        if let format { return format }
        let made = AVAudioFormat(standardFormatWithSampleRate: PokerSynth.sampleRate, channels: 1)
        format = made
        return made
    }

    private func startEngine() -> Bool {
        guard let format = audioFormat() else { return false }
        if players.isEmpty {
            players = (0..<Self.voiceCount).map { _ in AVAudioPlayerNode() }
            for player in players {
                engine.attach(player)
                engine.connect(player, to: engine.mainMixerNode, format: format)
            }
            engine.prepare()
        }
        if engine.isRunning { return true }
        do {
            try engine.start()
        } catch {
            return false
        }
        return engine.isRunning
    }

    private func sessionReady() -> Bool {
        if PromptDictation.isListening || MessagesVoiceRecorder.isRecording || OtherAppCall.inProgress { return false }
        let session = AVAudioSession.sharedInstance()
        if session.category != .ambient {
            try? session.setCategory(.ambient, options: [.mixWithOthers])
        }
        try? session.setActive(true)
        return true
    }
}

@MainActor
enum PokerHaptics {
    static func yourTurn() {
        guard FmHaptics.enabled, !UIAccessibility.isReduceMotionEnabled else { return }
        UINotificationFeedbackGenerator().notificationOccurred(.warning)
    }

    static func tap() {
        guard FmHaptics.enabled, !UIAccessibility.isReduceMotionEnabled else { return }
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    static func win() {
        guard FmHaptics.enabled, !UIAccessibility.isReduceMotionEnabled else { return }
        UINotificationFeedbackGenerator().notificationOccurred(.success)
    }
}

@MainActor
final class PokerHub {
    static let shared = PokerHub()

    private(set) weak var current: PokerModel?
    private var wired = false
    var visible = false

    func model(api: ApiService) -> PokerModel {
        if let current { return current }
        let model = PokerModel(api: PokerAPI(api: api))
        let sound = PokerSound.shared
        model.onEvents = { (events: [PokerEvent], table: PokerTable) in
            sound.play(events)
            if events.contains(where: { $0.kind == "win" }), table.winners.contains(where: { $0.index == table.mySeat }) {
                PokerHaptics.win()
            }
        }
        model.onCue = { (cue: PokerCue) in sound.play(cue) }
        model.onMyTurn = {
            sound.play(.yourTurn)
            PokerHaptics.yourTurn()
        }
        current = model
        wire()
        return model
    }

    func hold(_ model: PokerModel) {
        current = model
    }

    private func wire() {
        guard !wired, let events = Services.get(AccountEventChannel.self) else { return }
        wired = true
        let previous: (@MainActor (String) -> Void)? = events.onTopic
        events.onTopic = { (topic: String) in
            previous?(topic)
            guard topic == PokerModel.topic else { return }
            let hub = PokerHub.shared
            guard hub.visible, let model = hub.current else { return }
            model.ring()
        }
        let previousConnected: (@MainActor () -> Void)? = events.onConnected
        events.onConnected = {
            previousConnected?()
            let hub = PokerHub.shared
            guard hub.visible, let model = hub.current else { return }
            model.catchUp()
        }
    }
}
