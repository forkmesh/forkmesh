import AVFoundation
import FMPackages

@MainActor
final class DjSampler {
    private var engine: AVAudioEngine?
    private var players: [DjDrumPad: AVAudioPlayerNode] = [:]
    private var buffers: [DjDrumPad: AVAudioPCMBuffer] = [:]

    var level: Double = 0.8 {
        didSet { engine?.mainMixerNode.outputVolume = Float(level) }
    }

    func play(_ pad: DjDrumPad) {
        guard let engine = prepared(), let player = players[pad], let buffer = buffers[pad] else { return }
        if !engine.isRunning {
            do { try engine.start() } catch { return }
        }
        player.scheduleBuffer(buffer, at: nil, options: .interrupts, completionHandler: nil)
        if !player.isPlaying { player.play() }
    }

    func stop() {
        engine?.stop()
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
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: format)
            players[pad] = player
            buffers[pad] = buffer
        }
        engine.mainMixerNode.outputVolume = Float(level)
        engine.prepare()
        self.engine = engine
        return engine
    }
}
