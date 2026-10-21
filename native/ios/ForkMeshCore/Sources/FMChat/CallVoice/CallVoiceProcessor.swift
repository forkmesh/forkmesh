import Foundation

public final class CallVoiceProcessor: @unchecked Sendable {
    private enum Pending {
        case none
        case voice(CallVoice?)
    }

    public let sampleRate: Double
    public let latencySamples: Int
    private let engine: VoiceEngine
    private let lock = NSLock()
    private var pendingVoice = Pending.none
    private var pendingPitch: Double?
    private var pendingNoise: Double?
    private var currentVoice = ""
    private var publishedUserPitchHz: Double
    private var publishedPitchHz = 0.0

    public init(sampleRate: Double = 48000, userPitchHz: Double = 0) {
        self.sampleRate = sampleRate
        engine = VoiceEngine(sampleRate: sampleRate, userPitchHz: userPitchHz)
        latencySamples = engine.latencySamples
        publishedUserPitchHz = engine.userPitchHz
    }

    public var voice: String {
        lock.lock()
        defer { lock.unlock() }
        return currentVoice
    }

    public var userPitchHz: Double {
        lock.lock()
        defer { lock.unlock() }
        return publishedUserPitchHz
    }

    public var pitchHz: Double {
        lock.lock()
        defer { lock.unlock() }
        return publishedPitchHz
    }

    public func setVoice(_ id: String) {
        let preset = CallVoiceCatalog.voice(id)
        lock.lock()
        currentVoice = preset?.id ?? ""
        pendingVoice = .voice(preset)
        lock.unlock()
    }

    public func setUserPitch(_ hz: Double) {
        lock.lock()
        pendingPitch = hz
        lock.unlock()
    }

    public func setNoiseReduction(_ amount: Double) {
        lock.lock()
        pendingNoise = amount
        lock.unlock()
    }

    public func process(_ samples: UnsafeMutableBufferPointer<Float>) {
        if lock.try() {
            if case .voice(let preset) = pendingVoice {
                engine.setVoice(preset)
                pendingVoice = .none
            }
            if let hz = pendingPitch {
                engine.setUserPitch(hz)
                pendingPitch = nil
            }
            if let amount = pendingNoise {
                engine.setNoiseReduction(amount)
                pendingNoise = nil
            }
            lock.unlock()
        }
        engine.process(samples)
        if lock.try() {
            publishedUserPitchHz = engine.userPitchHz
            publishedPitchHz = engine.lastPitchHz
            lock.unlock()
        }
    }

    public func process(_ samples: inout [Float]) {
        samples.withUnsafeMutableBufferPointer { process($0) }
    }

    public func process(_ samples: UnsafeMutablePointer<Float>, count: Int) {
        process(UnsafeMutableBufferPointer(start: samples, count: count))
    }
}
