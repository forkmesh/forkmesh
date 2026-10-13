import AVFoundation
import FMAgentWatch
import Foundation

private let motionRate = 44100.0
private let motionDegrees = 2 * kScale.count

final class WatchMotionVoice: @unchecked Sendable {
    var frequency: Double = kTonicHz / 2
    var gain: Double = 0
    var cutoff: Double = 600
    var pan: Double = 0.5
    var shimmer: Double = 0

    private let rate: Double
    private var f: Double = kTonicHz / 2
    private var g: Double = 0
    private var c: Double = 600
    private var p: Double = 0.5
    private var s: Double = 0
    private var phase = 0.0
    private var beside = 0.0
    private var fifth = 0.0
    private var under = 0.0
    private var low = 0.0

    init(rate: Double) {
        self.rate = rate
    }

    func render(_ frames: Int, _ left: UnsafeMutablePointer<Float>, _ right: UnsafeMutablePointer<Float>?) {
        let glide = 1.0 - exp(-1.0 / (0.09 * rate))
        let swell = 1.0 - exp(-1.0 / (0.06 * rate))
        let tone = 1.0 - exp(-1.0 / (0.12 * rate))
        let toFrequency = frequency
        let toGain = gain
        let toCutoff = cutoff
        let toPan = pan
        let toShimmer = shimmer
        for frame in 0..<frames {
            f += (toFrequency - f) * glide
            g += (toGain - g) * swell
            c += (toCutoff - c) * tone
            p += (toPan - p) * tone
            s += (toShimmer - s) * swell
            phase = Self.step(phase, f / rate)
            beside = Self.step(beside, f * 1.004 / rate)
            fifth = Self.step(fifth, f * 1.5 / rate)
            under = Self.step(under, f * 0.5 / rate)
            var wave = 0.42 * Self.triangle(phase) + 0.32 * Self.triangle(beside)
            wave += 0.18 * sin(2 * Double.pi * fifth) + 0.28 * sin(2 * Double.pi * under)
            wave += s * 0.22 * sin(4 * Double.pi * beside)
            let a = 1.0 - exp(-2 * Double.pi * c / rate)
            low += (wave - low) * a
            let sample = low * g
            left[frame] = Float(sample * (1.0 - p).squareRoot())
            right?[frame] = Float(sample * p.squareRoot())
        }
    }

    private static func step(_ value: Double, _ by: Double) -> Double {
        let next = value + by
        return next >= 1 ? next - 1 : next
    }

    private static func triangle(_ at: Double) -> Double {
        4 * abs(at - 0.5) - 1
    }
}

@MainActor
final class WatchMotionSound {
    private let engine = AVAudioEngine()
    private let voice = WatchMotionVoice(rate: motionRate)
    private var source: AVAudioSourceNode?
    private var wanted = false
    private var place = Double(motionDegrees) / 2
    private var degree = motionDegrees / 2
    private var level = 0.0
    private var retryAt = Date.distantPast

    func play(_ on: Bool) {
        if on == wanted { return }
        wanted = on
        if on {
            start()
        } else {
            voice.gain = 0
            engine.pause()
        }
    }

    func stop() {
        wanted = false
        voice.gain = 0
        engine.stop()
    }

    func feel(_ reading: WatchMotionReading) {
        if !wanted { return }
        if !engine.isRunning && Date() >= retryAt {
            retryAt = Date().addingTimeInterval(2)
            start()
        }
        place = (reading.roll + 1) / 2 * Double(motionDegrees - 1)
        if abs(place - Double(degree)) > 0.65 {
            degree = min(max(Int(place.rounded()), 0), motionDegrees - 1)
        }
        let octave = degree / kScale.count
        let semitones = 12 * octave + kScale[degree % kScale.count]
        voice.frequency = kTonicHz / 2 * pow(2, Double(semitones) / 12)
        level = max(level * 0.86, min(reading.shake / 0.45, 1))
        let bright = 1 - abs(reading.facing)
        voice.cutoff = 260 + 2400 * pow(bright, 1.5) + 1800 * level
        voice.gain = 0.014 + 0.17 * level
        voice.shimmer = level
        voice.pan = 0.5 + 0.4 * reading.roll
    }

    private func start() {
        if PromptDictation.isListening { return }
        PromptDictation.holdOverhearing(10)
        let session = AVAudioSession.sharedInstance()
        if session.category != .playback || !session.categoryOptions.contains(.mixWithOthers) {
            try? session.setCategory(.playback, options: [.mixWithOthers])
        }
        try? session.setActive(true)
        if source == nil {
            guard let format = AVAudioFormat(standardFormatWithSampleRate: motionRate, channels: 2) else { return }
            let made = Self.makeSource(voice, format)
            engine.attach(made)
            engine.connect(made, to: engine.mainMixerNode, format: format)
            engine.prepare()
            source = made
        }
        if !engine.isRunning { try? engine.start() }
    }

    nonisolated private static func makeSource(_ voice: WatchMotionVoice, _ format: AVAudioFormat) -> AVAudioSourceNode {
        AVAudioSourceNode(format: format) { (_, _, frameCount, audioBufferList) -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let first = buffers.first?.mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            let second = buffers.count > 1 ? buffers[1].mData?.assumingMemoryBound(to: Float.self) : nil
            voice.render(Int(frameCount), first, second)
            return noErr
        }
    }
}
