import AVFoundation
import Accelerate
import Foundation
import Observation

@MainActor
@Observable
final class DictationMeter {
    static let shared: DictationMeter = DictationMeter()
    nonisolated static let bars: Int = 32
    nonisolated static let every: Double = 0.05
    nonisolated static let quietest: Float = -55
    nonisolated static let voice: Float = 0.18
    nonisolated static let voicedBars: Int = 24

    private(set) var levels: [Float] = Array(repeating: 0, count: DictationMeter.bars)
    private(set) var live: Bool = false
    private(set) var voicing: Bool = false
    @ObservationIgnored private var session: Int = 0

    func begin() -> Int {
        session += 1
        levels = Array(repeating: 0, count: DictationMeter.bars)
        live = true
        voicing = false
        return session
    }

    func end(_ ended: Int) {
        guard ended == session else { return }
        session += 1
        live = false
        voicing = false
        levels = Array(repeating: 0, count: DictationMeter.bars)
    }

    func push(_ level: Float, session from: Int) {
        guard from == session, live else { return }
        var next: [Float] = levels
        next.removeFirst()
        next.append(level)
        levels = next
        let loud: Bool = next.suffix(DictationMeter.voicedBars).contains { $0 >= DictationMeter.voice }
        if loud != voicing { voicing = loud }
    }

    nonisolated static func level(rms: Float) -> Float {
        let decibels: Float = 20 * log10(max(rms, 0.000_001))
        return min(max((decibels - quietest) / -quietest, 0), 1)
    }

    nonisolated static func tap(_ inner: @escaping AVAudioNodeTapBlock, session: Int) -> AVAudioNodeTapBlock {
        let gauge: DictationGauge = DictationGauge(session: session)
        return { (buffer: AVAudioPCMBuffer, time: AVAudioTime) in
            inner(buffer, time)
            gauge.take(buffer)
        }
    }
}

final class DictationGauge: @unchecked Sendable {
    private let lock: NSLock = NSLock()
    private let session: Int
    private var peak: Float = 0
    private var frames: Double = 0

    init(session: Int) {
        self.session = session
    }

    func take(_ buffer: AVAudioPCMBuffer) {
        let count: Int = Int(buffer.frameLength)
        guard count > 0, let channel = buffer.floatChannelData?[0] else { return }
        var rms: Float = 0
        vDSP_rmsqv(channel, 1, &rms, vDSP_Length(count))
        let level: Float = DictationMeter.level(rms: rms)
        lock.lock()
        peak = max(peak, level)
        frames += Double(count)
        let due: Bool = frames >= buffer.format.sampleRate * DictationMeter.every
        let sent: Float = peak
        if due {
            peak = 0
            frames = 0
        }
        lock.unlock()
        guard due else { return }
        let session: Int = self.session
        Task { @MainActor in
            DictationMeter.shared.push(sent, session: session)
        }
    }
}
