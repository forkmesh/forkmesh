import AVFoundation
import FMPackages
import Foundation
import os

@MainActor
final class DjSampler {
    private var engine: AVAudioEngine?
    private var players: [DjDrumPad: AVAudioPlayerNode] = [:]
    private var buffers: [DjDrumPad: AVAudioPCMBuffer] = [:]
    private let voice = DjScratchNoise()
    private let decks: [DjDeckSide: DjScratchVoice] = [.a: DjScratchVoice(), .b: DjScratchVoice()]

    var level: Double = 0.8 {
        didSet { for player in players.values { player.volume = Float(level) } }
    }

    func play(_ pad: DjDrumPad) {
        guard running() != nil, let player = players[pad], let buffer = buffers[pad] else { return }
        player.scheduleBuffer(buffer, at: nil, options: .interrupts, completionHandler: nil)
        if !player.isPlaying { player.play() }
    }

    func beginScratch() {
        voice.speed = 0
        voice.target = 0
        _ = running()
    }

    func scratch(level: Double, speed: Double) {
        voice.speed = Float(min(8, max(0, speed)))
        voice.target = Float(min(1, max(0, level)))
    }

    func beginScratch(_ side: DjDeckSide, audio: DjScratchAudio, at seconds: Double, gain: Double) {
        decks[side]?.begin(audio, at: seconds, gain: gain)
        _ = running()
    }

    func scratch(_ side: DjDeckSide, to seconds: Double, gain: Double) {
        decks[side]?.move(to: seconds, gain: gain)
    }

    func endScratch(_ side: DjDeckSide) {
        decks[side]?.end()
    }

    func stop() {
        voice.target = 0
        for deck in decks.values { deck.end() }
        engine?.stop()
    }

    private func running() -> AVAudioEngine? {
        guard let engine = prepared() else { return nil }
        if !engine.isRunning {
            do { try engine.start() } catch { return nil }
        }
        return engine
    }

    private func prepared() -> AVAudioEngine? {
        if let engine { return engine }
        let sampleRate = 44_100.0
        guard let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 1) else { return nil }
        let engine = AVAudioEngine()
        for pad in DjDrumPad.allCases {
            let samples = DjDrumSynth.samples(pad, sampleRate: sampleRate)
            guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(samples.count)),
                let channel = buffer.floatChannelData?[0]
            else { continue }
            buffer.frameLength = AVAudioFrameCount(samples.count)
            samples.withUnsafeBufferPointer { source in
                if let base = source.baseAddress { channel.update(from: base, count: samples.count) }
            }
            let player = AVAudioPlayerNode()
            player.volume = Float(level)
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: format)
            players[pad] = player
            buffers[pad] = buffer
        }
        let scratch = voice.node(format: format)
        engine.attach(scratch)
        engine.connect(scratch, to: engine.mainMixerNode, format: format)
        for side in DjDeckSide.allCases {
            guard let deck = decks[side]?.node(format: format) else { continue }
            engine.attach(deck)
            engine.connect(deck, to: engine.mainMixerNode, format: format)
        }
        engine.prepare()
        self.engine = engine
        return engine
    }
}

final class DjScratchVoice: @unchecked Sendable {
    private struct Shared: Sendable {
        var audio: DjScratchAudio?
        var goal: Double = 0
        var gain: Float = 0
        var epoch = 0
    }

    private let shared = OSAllocatedUnfairLock(initialState: Shared())
    private var head = DjScratchHead()
    private var audio: DjScratchAudio?
    private var goal: Double = 0
    private var gain: Float = 0
    private var epoch = 0

    func begin(_ audio: DjScratchAudio, at seconds: Double, gain: Double) {
        shared.withLock {
            $0.audio = audio
            $0.goal = seconds
            $0.gain = Float(min(1, max(0, gain)))
            $0.epoch += 1
        }
    }

    func move(to seconds: Double, gain: Double) {
        shared.withLock {
            $0.goal = seconds
            $0.gain = Float(min(1, max(0, gain)))
        }
    }

    func end() {
        shared.withLock {
            $0.gain = 0
            $0.audio = nil
        }
    }

    func node(format: AVAudioFormat) -> AVAudioSourceNode {
        let sampleRate = format.sampleRate
        return AVAudioSourceNode(format: format) { [self] silence, _, frameCount, list -> OSStatus in
            self.render(Int(frameCount), list, sampleRate, silence)
            return noErr
        }
    }

    private func render(_ frames: Int, _ list: UnsafeMutablePointer<AudioBufferList>, _ sampleRate: Double, _ silence: UnsafeMutablePointer<ObjCBool>) {
        let buffers = UnsafeMutableAudioBufferListPointer(list)
        if let state = shared.withLockIfAvailable({ $0 }) {
            if state.epoch != epoch {
                epoch = state.epoch
                audio = state.audio
                head.place(state.goal)
            }
            goal = state.goal
            gain = state.gain
            if state.audio == nil, gain == 0, !head.audible { audio = nil }
        }
        guard let first = buffers.first?.mData?.assumingMemoryBound(to: Float.self), let audio, gain > 0 || head.audible else {
            for buffer in buffers {
                if let data = buffer.mData { memset(data, 0, Int(buffer.mDataByteSize)) }
            }
            silence.pointee = true
            return
        }
        let out = UnsafeMutableBufferPointer(start: first, count: frames)
        audio.samples.withUnsafeBufferPointer { pcm in
            head.render(pcm, sourceRate: audio.rate, outputRate: sampleRate, goal: goal, gain: gain, into: out)
        }
        for buffer in buffers.dropFirst() {
            if let data = buffer.mData?.assumingMemoryBound(to: Float.self) { data.update(from: first, count: frames) }
        }
    }
}

final class DjScratchNoise: @unchecked Sendable {
    var target: Float = 0
    var speed: Float = 0
    private var amp: Float = 0
    private var low: Float = 0
    private var band: Float = 0
    private var seed: UInt32 = 0x2545_F491

    func node(format: AVAudioFormat) -> AVAudioSourceNode {
        let sampleRate = Float(format.sampleRate)
        return AVAudioSourceNode(format: format) { [self] silence, _, frameCount, list -> OSStatus in
            self.render(Int(frameCount), list, sampleRate, silence)
            return noErr
        }
    }

    private func render(_ frames: Int, _ list: UnsafeMutablePointer<AudioBufferList>, _ sampleRate: Float, _ silence: UnsafeMutablePointer<ObjCBool>) {
        let buffers = UnsafeMutableAudioBufferListPointer(list)
        let goal = target
        if goal == 0, amp < 0.0005 {
            amp = 0
            for buffer in buffers {
                if let data = buffer.mData { memset(data, 0, Int(buffer.mDataByteSize)) }
            }
            silence.pointee = true
            return
        }
        let centre = 260 + 2600 * min(1, speed / 3)
        let tune = 2 * sinf(Float.pi * min(centre, sampleRate * 0.2) / sampleRate)
        guard let first = buffers.first?.mData?.assumingMemoryBound(to: Float.self) else { return }
        for index in 0..<frames {
            amp += (goal - amp) * 0.003
            seed = seed &* 1_664_525 &+ 1_013_904_223
            let noise = Float(Int32(bitPattern: seed)) / Float(Int32.max)
            let high = noise - low - 0.35 * band
            band += tune * high
            low += tune * band
            first[index] = max(-1, min(1, band * amp * 0.8))
        }
        for buffer in buffers.dropFirst() {
            if let data = buffer.mData?.assumingMemoryBound(to: Float.self) { data.update(from: first, count: frames) }
        }
    }
}
