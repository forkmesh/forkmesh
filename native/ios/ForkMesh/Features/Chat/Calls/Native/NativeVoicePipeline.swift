#if canImport(WebRTC)
import FMChat
import Foundation
import WebRTC

final class NativeVoicePipeline: @unchecked Sendable {
    static let scale: Float = 32768

    let capture = RTCFmCaptureProcessor()
    private let lock = NSLock()
    private var processor: CallVoiceProcessor
    private var rate = 48000
    private var voice = CallVoiceCatalog.off

    init() {
        processor = CallVoiceProcessor(sampleRate: 48000)
        capture.block = { [weak self] samples, frames, sampleRate in
            self?.process(samples, frames: frames, sampleRate: sampleRate)
        }
    }

    func setVoice(_ id: String) {
        lock.lock()
        voice = id
        processor.setVoice(id)
        lock.unlock()
    }

    private func process(_ samples: UnsafeMutablePointer<Float>, frames: Int, sampleRate: Int) {
        guard frames > 0, lock.try() else { return }
        defer { lock.unlock() }
        guard voice != CallVoiceCatalog.off, !voice.isEmpty else { return }
        if sampleRate != rate, sampleRate > 0 {
            rate = sampleRate
            processor = CallVoiceProcessor(sampleRate: Double(sampleRate))
            processor.setVoice(voice)
        }
        let buffer = UnsafeMutableBufferPointer(start: samples, count: frames)
        for index in buffer.indices { buffer[index] /= Self.scale }
        processor.process(buffer)
        for index in buffer.indices { buffer[index] *= Self.scale }
    }
}
#endif
