// The watch's sonifier: WS4's FMAgentWatch, the same voices, melody and
// marimba synth as the phone's Listen and the desktop's AgentSonifier. Each
// agent keeps its own voice (from its key and title) and its own melody, so
// two agents heard together sound like two players, and the same agent
// sounds the same on the watch as on the phone.

import AVFoundation
import FMAgentWatch
import FMWatchWire
import Foundation

@MainActor
final class AgentWatchSound {
    static let format = AVAudioFormat(standardFormatWithSampleRate: Double(kWatchSampleRate), channels: 2)!

    private struct Player {
        var voice: Voice
        var melody: Melody
    }

    private var players: [String: Player] = [:]
    private let epoch = Date()

    /// The sonifier's clock, in seconds.
    var now: Double { Date().timeIntervalSince(epoch) }

    private func player(_ key: String, _ title: String) -> Player {
        if let existing = players[key] { return existing }
        let fresh = Player(voice: Voice.forAgent(fnv1a(key), title), melody: Melody())
        players[key] = fresh
        return fresh
    }

    /// The notes for an agent's new beats, on the shared slot grid.
    func notes(for beats: [WireBeat], key: String, title: String) -> [Note] {
        var p = player(key, title)
        let clock = now
        let notes = beats.map { (beat: WireBeat) -> Note in
            noteForBeat(beat.motion, p.voice, &p.melody, clock, bytes: beat.bytes, seed: beat.seed)
        }
        players[key] = p
        return notes
    }

    /// The figure that says a run is over.
    func cadence(key: String, title: String, success: Bool) -> [Note] {
        doneCadence(player(key, title).voice, success, now + 0.1)
    }

    /// Renders `notes` from `from` into a stereo buffer. The synth is
    /// arithmetic on every sample, so the synth runs off the main actor.
    static func buffer(_ notes: [Note], from: Double) async -> AVAudioPCMBuffer? {
        let pcm: [Int16] = await Task.detached(priority: .userInitiated) {
            renderNotes(notes, from: from)
        }.value
        let frames = pcm.count / 2
        guard frames > 0,
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)),
              let channels = buffer.floatChannelData
        else { return nil }
        buffer.frameLength = AVAudioFrameCount(frames)
        let left = channels[0]
        let right = channels[1]
        for i in 0..<frames {
            left[i] = Float(pcm[i * 2]) / 32768
            right[i] = Float(pcm[i * 2 + 1]) / 32768
        }
        return buffer
    }
}
