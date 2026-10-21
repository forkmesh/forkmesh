#if canImport(Fmm) && canImport(Opus)
import AVFoundation
import AudioToolbox
import FMChat
import Fmm
import Foundation
import Opus
import os

final class FmmPcmRing: @unchecked Sendable {
    private var storage: [Float]
    private var head = 0
    private var filled = 0
    private var lock = os_unfair_lock()

    init(capacity: Int) {
        storage = [Float](repeating: 0, count: capacity)
    }

    var available: Int {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        return filled
    }

    func write(_ samples: UnsafePointer<Float>, count: Int) {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        let capacity = storage.count
        for index in 0..<count {
            storage[(head + filled) % capacity] = samples[index]
            if filled < capacity {
                filled += 1
            } else {
                head = (head + 1) % capacity
            }
        }
    }

    func read(into out: UnsafeMutablePointer<Float>, count: Int, blocking: Bool) -> Int {
        if blocking {
            os_unfair_lock_lock(&lock)
        } else if !os_unfair_lock_trylock(&lock) {
            out.update(repeating: 0, count: count)
            return 0
        }
        defer { os_unfair_lock_unlock(&lock) }
        let taken = min(count, filled)
        let capacity = storage.count
        for index in 0..<taken { out[index] = storage[(head + index) % capacity] }
        if taken < count { (out + taken).update(repeating: 0, count: count - taken) }
        head = (head + taken) % capacity
        filled -= taken
        return taken
    }
}

final class FmmOpusDecoder {
    private let decoder: OpaquePointer
    private let dredDecoder: OpaquePointer?
    private let dred: OpaquePointer?

    init?() {
        var error: Int32 = 0
        guard let made = opus_decoder_create(Int32(GroupCallAudioPlan.sampleRate), 1, &error), error == OPUS_OK else { return nil }
        decoder = made
        _ = fm_opus_decoder_set(made, OPUS_SET_COMPLEXITY_REQUEST, 10)
        _ = fm_opus_decoder_set(made, OPUS_SET_OSCE_BWE_REQUEST, 1)
        dredDecoder = opus_dred_decoder_create(&error)
        dred = opus_dred_alloc(&error)
    }

    deinit {
        opus_decoder_destroy(decoder)
        if let dredDecoder { opus_dred_decoder_destroy(dredDecoder) }
        if let dred { opus_dred_free(dred) }
    }

    func decode(_ out: FmmmobileAudioOut, into pcm: UnsafeMutablePointer<Float>) {
        let samples = Int32(GroupCallAudioPlan.frameSamples)
        let data = out.data ?? Data()
        data.withUnsafeBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self).baseAddress
            let length = Int32(data.count)
            let lbrr = bytes.map { opus_packet_has_lbrr($0, length) > 0 } ?? false
            var dredSamples = 0
            if out.status == GroupCallAudioDecode.dred, let bytes, let dredDecoder, let dred {
                var end: Int32 = 0
                let parsed = opus_dred_parse(dredDecoder, dred, bytes, length, Int32(GroupCallAudioPlan.sampleRate), Int32(GroupCallAudioPlan.sampleRate), &end, 0)
                dredSamples = max(Int(parsed), 0)
            }
            let step = GroupCallAudioDecode.step(
                status: out.status, offset: out.offset, hasData: bytes != nil && length > 0, hasLBRR: lbrr, dredSamples: dredSamples)
            var made: Int32 = 0
            switch step {
            case .silence:
                pcm.update(repeating: 0, count: Int(samples))
                return
            case .play:
                made = opus_decode_float(decoder, bytes, length, pcm, samples, 0)
            case .fec:
                made = opus_decode_float(decoder, bytes, length, pcm, samples, 1)
            case let .dred(offset):
                made = dred.map { opus_decoder_dred_decode_float(decoder, $0, Int32(offset), pcm, samples) } ?? -1
            case .conceal:
                made = opus_decode_float(decoder, nil, 0, pcm, samples, 0)
            }
            if made < samples {
                let from = Int(max(made, 0))
                (pcm + from).update(repeating: 0, count: Int(samples) - from)
            }
        }
    }
}

final class FmmOpusEncoder {
    private let encoder: OpaquePointer
    private(set) var plan = GroupCallAudioPlan(bitrate: 24000, fec: false, lossPercent: 0, dredFrames: 0, depthMs: 0)
    private var packet = [UInt8](repeating: 0, count: 1500)

    init?() {
        var error: Int32 = 0
        guard let made = opus_encoder_create(Int32(GroupCallAudioPlan.sampleRate), 1, OPUS_APPLICATION_VOIP, &error), error == OPUS_OK else {
            return nil
        }
        encoder = made
        _ = fm_opus_encoder_set(made, OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_VOICE)
        _ = fm_opus_encoder_set(made, OPUS_SET_COMPLEXITY_REQUEST, 9)
        _ = fm_opus_encoder_set(made, OPUS_SET_VBR_REQUEST, 1)
        _ = fm_opus_encoder_set(made, OPUS_SET_DTX_REQUEST, 0)
        apply(plan)
    }

    deinit { opus_encoder_destroy(encoder) }

    func apply(_ next: GroupCallAudioPlan) {
        plan = next
        _ = fm_opus_encoder_set(encoder, OPUS_SET_BITRATE_REQUEST, Int32(next.bitrate))
        _ = fm_opus_encoder_set(encoder, OPUS_SET_INBAND_FEC_REQUEST, next.fec ? 1 : 0)
        _ = fm_opus_encoder_set(encoder, OPUS_SET_PACKET_LOSS_PERC_REQUEST, Int32(next.lossPercent))
        _ = fm_opus_encoder_set(encoder, OPUS_SET_DRED_DURATION_REQUEST, Int32(next.dredFrames))
    }

    func encode(_ pcm: UnsafePointer<Float>) -> Data? {
        let made = packet.withUnsafeMutableBufferPointer { out in
            opus_encode_float(encoder, pcm, Int32(GroupCallAudioPlan.frameSamples), out.baseAddress!, Int32(out.count))
        }
        guard made > 0 else { return nil }
        return Data(packet[0..<Int(made)])
    }
}

final class FmmAudioEngine: @unchecked Sendable {
    static let ringSamples = 48000
    static let playoutTarget = 2 * GroupCallAudioPlan.frameSamples
    static let tick = DispatchTimeInterval.milliseconds(5)
    static let retuneEvery: TimeInterval = 1

    private weak var call: FmmmobileCall?
    private let queue = DispatchQueue(label: "com.forkmesh.fmm.audio", qos: .userInteractive)
    private let capture = FmmPcmRing(capacity: ringSamples)
    private let playout = FmmPcmRing(capacity: ringSamples)
    private let lock = NSLock()
    private var unit: AudioUnit?
    private var timer: DispatchSourceTimer?
    private var stream = 0
    private var decoders: [Int: FmmOpusDecoder] = [:]
    private var encoder: FmmOpusEncoder?
    private var voice: CallVoiceProcessor
    private var voiceId = CallVoiceCatalog.off
    private var retunedAt = Date.distantPast
    private var inputBuffer = [Float](repeating: 0, count: 4096)
    private var frame = [Float](repeating: 0, count: GroupCallAudioPlan.frameSamples)
    private var mix = [Float](repeating: 0, count: GroupCallAudioPlan.frameSamples)
    private var decoded = [Float](repeating: 0, count: GroupCallAudioPlan.frameSamples)
    private var interruption: NSObjectProtocol?
    let voiceSource: () -> String
    var muted = false
    private(set) var running = false

    init(call: FmmmobileCall, voice: @escaping () -> String) {
        self.call = call
        self.voiceSource = voice
        self.voice = CallVoiceProcessor(sampleRate: Double(GroupCallAudioPlan.sampleRate))
        encoder = FmmOpusEncoder()
    }

    func publish(_ stream: Int) {
        lock.withLock { self.stream = stream }
        retune(force: true)
    }

    func addRemote(_ stream: Int) {
        guard let made = FmmOpusDecoder() else { return }
        lock.withLock { decoders[stream] = made }
    }

    func removeRemote(_ stream: Int) {
        lock.withLock { _ = decoders.removeValue(forKey: stream) }
    }

    func clearRemotes() {
        lock.withLock { decoders.removeAll() }
    }

    func start(sessionOwned: Bool) -> Bool {
        guard !running else { return true }
        if !sessionOwned {
            let session = AVAudioSession.sharedInstance()
            do {
                try session.setCategory(.playAndRecord, mode: .voiceChat, options: [.allowBluetooth, .allowBluetoothA2DP, .duckOthers])
                try session.setPreferredSampleRate(Double(GroupCallAudioPlan.sampleRate))
                try session.setPreferredIOBufferDuration(0.01)
                try session.setActive(true)
            } catch {
                return false
            }
        }
        guard makeUnit() else { return false }
        var silence = [Float](repeating: 0, count: Self.playoutTarget)
        silence.withUnsafeMutableBufferPointer { playout.write($0.baseAddress!, count: $0.count) }
        let made = DispatchSource.makeTimerSource(queue: queue)
        made.schedule(deadline: .now(), repeating: Self.tick, leeway: .milliseconds(1))
        made.setEventHandler { [weak self] in self?.pump() }
        made.resume()
        timer = made
        running = true
        interruption = NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: nil) { [weak self] note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                AVAudioSession.InterruptionType(rawValue: raw) == .ended, let unit = self?.unit
            else { return }
            AudioOutputUnitStart(unit)
        }
        return true
    }

    func stop(sessionOwned: Bool) {
        timer?.cancel()
        timer = nil
        if let interruption { NotificationCenter.default.removeObserver(interruption) }
        interruption = nil
        if let unit {
            AudioOutputUnitStop(unit)
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
        }
        unit = nil
        running = false
        if !sessionOwned { try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation) }
    }

    private func makeUnit() -> Bool {
        var description = AudioComponentDescription(
            componentType: kAudioUnitType_Output, componentSubType: kAudioUnitSubType_VoiceProcessingIO,
            componentManufacturer: kAudioUnitManufacturer_Apple, componentFlags: 0, componentFlagsMask: 0)
        guard let component = AudioComponentFindNext(nil, &description) else { return false }
        var made: AudioUnit?
        guard AudioComponentInstanceNew(component, &made) == noErr, let made else { return false }
        var one: UInt32 = 1
        var zero: UInt32 = 0
        let flag = UInt32(MemoryLayout<UInt32>.size)
        AudioUnitSetProperty(made, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, flag)
        AudioUnitSetProperty(made, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, flag)
        var format = AudioStreamBasicDescription(
            mSampleRate: Double(GroupCallAudioPlan.sampleRate), mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: 1, mBitsPerChannel: 32, mReserved: 0)
        let size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        AudioUnitSetProperty(made, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, size)
        AudioUnitSetProperty(made, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, size)
        AudioUnitSetProperty(made, kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, 1, &zero, flag)
        AudioUnitSetProperty(made, kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, 1, &one, flag)
        let context = Unmanaged.passUnretained(self).toOpaque()
        var input = AURenderCallbackStruct(inputProc: fmmCaptureCallback, inputProcRefCon: context)
        AudioUnitSetProperty(made, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &input, UInt32(MemoryLayout<AURenderCallbackStruct>.size))
        var render = AURenderCallbackStruct(inputProc: fmmRenderCallback, inputProcRefCon: context)
        AudioUnitSetProperty(made, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render, UInt32(MemoryLayout<AURenderCallbackStruct>.size))
        guard AudioUnitInitialize(made) == noErr, AudioOutputUnitStart(made) == noErr else {
            AudioComponentInstanceDispose(made)
            return false
        }
        unit = made
        return true
    }

    fileprivate func captured(_ flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>, _ time: UnsafePointer<AudioTimeStamp>, _ frames: UInt32) -> OSStatus {
        guard let unit, Int(frames) <= inputBuffer.count else { return noErr }
        return inputBuffer.withUnsafeMutableBufferPointer { buffer -> OSStatus in
            var list = AudioBufferList(
                mNumberBuffers: 1, mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: frames * 4, mData: UnsafeMutableRawPointer(buffer.baseAddress)))
            let status = AudioUnitRender(unit, flags, time, 1, frames, &list)
            if status == noErr, let base = buffer.baseAddress { capture.write(base, count: Int(frames)) }
            return status
        }
    }

    fileprivate func render(_ data: UnsafeMutablePointer<AudioBufferList>?, _ frames: UInt32) -> OSStatus {
        guard let data else { return noErr }
        let buffers = UnsafeMutableAudioBufferListPointer(data)
        for buffer in buffers {
            guard let out = buffer.mData?.assumingMemoryBound(to: Float.self) else { continue }
            _ = playout.read(into: out, count: Int(frames), blocking: false)
        }
        return noErr
    }

    private func pump() {
        guard let call else { return }
        let samples = GroupCallAudioPlan.frameSamples
        while capture.available >= samples {
            frame.withUnsafeMutableBufferPointer { buffer in
                guard let base = buffer.baseAddress else { return }
                _ = capture.read(into: base, count: samples, blocking: true)
                changeVoice(buffer)
                if muted { base.update(repeating: 0, count: samples) }
                let target = lock.withLock { stream }
                if target != 0, let packet = encoder?.encode(base) { try? call.sendAudio(target, frame: packet) }
            }
        }
        while playout.available < Self.playoutTarget {
            let streams = lock.withLock { decoders }
            mix.withUnsafeMutableBufferPointer { out in
                guard let base = out.baseAddress else { return }
                base.update(repeating: 0, count: samples)
                for (id, decoder) in streams {
                    guard let pulled = call.pullAudio(id) else { continue }
                    decoded.withUnsafeMutableBufferPointer { pcm in
                        guard let one = pcm.baseAddress else { return }
                        decoder.decode(pulled, into: one)
                        for index in 0..<samples { base[index] += one[index] }
                    }
                }
                for index in 0..<samples { base[index] = max(-1, min(1, base[index])) }
                playout.write(base, count: samples)
            }
        }
        if Date().timeIntervalSince(retunedAt) >= Self.retuneEvery { retune(force: false) }
    }

    private func changeVoice(_ buffer: UnsafeMutableBufferPointer<Float>) {
        let wanted = voiceSource()
        if wanted != voiceId {
            voiceId = wanted
            voice.setVoice(wanted)
        }
        guard voiceId != CallVoiceCatalog.off, !voiceId.isEmpty else { return }
        voice.process(buffer)
    }

    func retune(force: Bool) {
        guard let call else { return }
        retunedAt = Date()
        let target = lock.withLock { stream }
        guard target != 0, let encoder else { return }
        let plan = GroupCallAudioPlan.make(
            engineBitrate: call.audioBitrate(target), targetBitrate: call.targetBitrate(),
            redundancyMs: call.audioRedundancyMs(target), loss: call.audioLoss())
        guard force || plan != encoder.plan else { return }
        encoder.apply(plan)
        try? call.setAudioRedundancy(target, ms: plan.depthMs)
    }
}

private let fmmCaptureCallback: AURenderCallback = { context, flags, time, _, frames, _ in
    Unmanaged<FmmAudioEngine>.fromOpaque(context).takeUnretainedValue().captured(flags, time, frames)
}

private let fmmRenderCallback: AURenderCallback = { context, _, _, _, frames, data in
    Unmanaged<FmmAudioEngine>.fromOpaque(context).takeUnretainedValue().render(data, frames)
}
#endif
