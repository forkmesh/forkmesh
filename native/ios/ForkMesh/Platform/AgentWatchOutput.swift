// The Watch screen's speaker and motor, plus the render step that feeds them
// (PlatformAgentWatchOutput). Sound is FMAgentWatch's synth rendered off the
// main thread into an AVAudioEngine voice pool; touch is Core Haptics, or
// UIKit's impact generator on hardware without it.

import AVFoundation
import CoreHaptics
import FMAgentWatch
import Foundation
import UIKit

@MainActor
final class PlatformAgentWatchOutput: AgentWatchOutput {
    private static let voiceCount = 8

    private let engine = AVAudioEngine()
    private var players: [AVAudioPlayerNode] = []
    private var nextPlayer = 0
    private var format: AVAudioFormat?
    private var haptics: CHHapticEngine?
    private var hapticPlayer: CHHapticPatternPlayer?
    private let supportsHaptics = CHHapticEngine.capabilitiesForHardware().supportsHaptics
    /// Bumped by `stop()`, so a phrase still rendering when the screen closed
    /// is dropped rather than played.
    private var generation = 0

    init() {}

    // MARK: AgentWatchOutput

    func play(_ notes: [Note], now: Double) {
        if notes.isEmpty { return }
        let started = DispatchTime.now().uptimeNanoseconds
        let mine = generation
        // Rendering is arithmetic on every sample, and a cadence rings for four
        // seconds: done off the main thread, so the field never waits on it.
        Task.detached(priority: .userInitiated) {
            let pcm = renderNotes(notes, from: now)
            await MainActor.run { [weak self] in
                guard let self, self.generation == mine else { return }
                // The buffer starts at `now`; whatever the render took is
                // already past, so it is cut off the front rather than played late.
                let took = Double(DispatchTime.now().uptimeNanoseconds &- started) / 1e9
                let skip = Int((took * Double(kWatchSampleRate)).rounded())
                if skip * 2 >= pcm.count { return }
                self.play(pcm, from: skip, rate: Double(kWatchSampleRate), channels: 2)
            }
        }
    }

    func feel(_ pulses: [Pulse]) {
        if pulses.isEmpty { return }
        let clamped = pulses.map(\.clamped)
        guard supportsHaptics else {
            let first = clamped[0]
            let style: UIImpactFeedbackGenerator.FeedbackStyle =
                first.ms >= 100 || first.intensity >= 0.8 ? .heavy : first.intensity >= 0.5 ? .medium : .light
            UIImpactFeedbackGenerator(style: style).impactOccurred(intensity: CGFloat(first.intensity))
            return
        }
        do {
            let engine = try hapticEngine()
            let events = clamped.map { (pulse: Pulse) -> CHHapticEvent in
                let parameters = [
                    CHHapticEventParameter(parameterID: .hapticIntensity, value: Float(pulse.intensity)),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: Float(pulse.sharpness)),
                ]
                let at = Double(pulse.atMs) / 1000
                let seconds = Double(pulse.ms) / 1000
                return seconds >= 0.04
                    ? CHHapticEvent(
                        eventType: .hapticContinuous, parameters: parameters, relativeTime: at, duration: seconds)
                    : CHHapticEvent(eventType: .hapticTransient, parameters: parameters, relativeTime: at)
            }
            let pattern = try CHHapticPattern(events: events, parameters: [])
            try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
            let player = try engine.makePlayer(with: pattern)
            try player.start(atTime: CHHapticTimeImmediate)
            hapticPlayer = player
        } catch {
            // One touch lost; the next beat builds a fresh engine if this one died.
            haptics = nil
        }
    }

    func stop() {
        generation &+= 1
        for player in players {
            player.stop()
        }
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
    }

    // MARK: Audio

    private func startAudio(rate: Double, channels: Int) -> AVAudioFormat? {
        // Checked every phrase: something else taking the shared session back
        // to ambient would hand the Watch to the silent switch again.
        if PromptDictation.isListening || OtherAppCall.inProgress { return nil }
        PromptDictation.holdOverhearing(10)
        let session = AVAudioSession.sharedInstance()
        if session.category != .playback || !session.categoryOptions.contains(.mixWithOthers) {
            try? session.setCategory(.playback, options: [.mixWithOthers])
            try? session.setActive(true)
        }
        if let format, format.sampleRate == rate, Int(format.channelCount) == channels {
            if !engine.isRunning {
                try? engine.start()
            }
            return engine.isRunning ? format : nil
        }
        guard let wanted = AVAudioFormat(standardFormatWithSampleRate: rate, channels: AVAudioChannelCount(channels))
        else { return nil }
        engine.stop()
        for player in players {
            engine.detach(player)
        }
        players = (0..<Self.voiceCount).map { _ in AVAudioPlayerNode() }
        for player in players {
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: wanted)
        }
        engine.prepare()
        do {
            try engine.start()
        } catch {
            return nil
        }
        format = wanted
        return wanted
    }

    /// Interleaved 16-bit `pcm` from frame `from` on, as one phrase.
    private func play(_ pcm: [Int16], from: Int, rate: Double, channels: Int) {
        guard channels > 0, let format = startAudio(rate: rate, channels: channels) else { return }
        let frames = pcm.count / channels - from
        guard frames > 0,
            let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)),
            let out = buffer.floatChannelData
        else { return }
        buffer.frameLength = AVAudioFrameCount(frames)
        pcm.withUnsafeBufferPointer { (samples: UnsafeBufferPointer<Int16>) in
            for channel in 0..<channels {
                let destination = out[channel]
                for frame in 0..<frames {
                    destination[frame] = Float(samples[(from + frame) * channels + channel]) / 32768
                }
            }
        }
        // Round-robin: by the time a node comes round again its last phrase has
        // almost always rung out, and if not, the new one cuts it off.
        let player = players[nextPlayer]
        nextPlayer = (nextPlayer + 1) % players.count
        guard engine.isRunning, player.engine === engine else { return }
        player.scheduleBuffer(buffer, at: nil, options: .interrupts, completionHandler: nil)
        if !player.isPlaying {
            player.play()
        }
    }

    // MARK: Haptics

    private func hapticEngine() throws -> CHHapticEngine {
        if let haptics {
            try haptics.start()
            return haptics
        }
        let engine = try CHHapticEngine()
        engine.isAutoShutdownEnabled = true
        engine.resetHandler = { [weak engine] in
            try? engine?.start()
        }
        try engine.start()
        haptics = engine
        return engine
    }
}
