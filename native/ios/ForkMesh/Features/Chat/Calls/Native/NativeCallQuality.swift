import AVFoundation
import FMChat
import Foundation
#if canImport(WebRTC)
import WebRTC
#endif

@MainActor
final class NativeMicMeter {
    private var recorder: AVAudioRecorder?
    private var task: Task<Void, Never>?

    var running: Bool { recorder != nil }

    func start(into monitor: CallQualityMonitor) -> Bool {
        stop(deactivate: false)
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playAndRecord, mode: .voiceChat, options: [.allowBluetooth, .allowBluetoothA2DP])
            try session.setActive(true)
        } catch {
            return false
        }
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatLinearPCM), AVSampleRateKey: 16000, AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false,
        ]
        guard let made = try? AVAudioRecorder(url: URL(fileURLWithPath: "/dev/null"), settings: settings) else { return false }
        made.isMeteringEnabled = true
        guard made.record() else { return false }
        recorder = made
        task = Task { @MainActor [weak self, weak monitor] in
            while !Task.isCancelled {
                guard let self, let monitor, let recorder = self.recorder else { return }
                recorder.updateMeters()
                let power = Double(recorder.averagePower(forChannel: 0))
                monitor.push(level: CallVoiceLevel(mic: CallQuality.level(decibels: power)))
                try? await Task.sleep(nanoseconds: UInt64(CallQuality.levelEvery * 1_000_000_000))
            }
        }
        return true
    }

    func stop(deactivate: Bool) {
        task?.cancel()
        task = nil
        guard let recorder else { return }
        recorder.stop()
        self.recorder = nil
        if deactivate {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }
}

#if canImport(WebRTC)
@MainActor
final class NativeQualityProbe {
    static let statsEveryTicks = Int((CallQuality.statsEvery / CallQuality.levelEvery).rounded())

    private var task: Task<Void, Never>?
    private var counters: CallStatsCounters?

    func start(
        monitor: CallQualityMonitor, connections: @escaping @MainActor () -> [RTCPeerConnection],
        muted: @escaping @MainActor () -> Bool
    ) {
        stop()
        task = Task { @MainActor [weak self, weak monitor] in
            var tick = 0
            while !Task.isCancelled {
                let full = tick % NativeQualityProbe.statsEveryTicks == 0
                let list = connections()
                let reports = await NativeQualityProbe.read(list, full: full)
                guard !Task.isCancelled, let self, let monitor else { return }
                monitor.push(level: CallStatsSummary.levels(reports, muted: muted()))
                if full {
                    let result = CallStatsSummary.summarize(
                        reports, previous: self.counters, at: Date().timeIntervalSince1970)
                    self.counters = list.isEmpty ? nil : result.counters
                    if !list.isEmpty { monitor.push(sample: result.sample) }
                }
                tick += 1
                try? await Task.sleep(nanoseconds: UInt64(CallQuality.levelEvery * 1_000_000_000))
            }
        }
    }

    func stop() {
        task?.cancel()
        task = nil
        counters = nil
    }

    static func read(_ connections: [RTCPeerConnection], full: Bool) async -> [[CallStatsEntry]] {
        var reports: [[CallStatsEntry]] = []
        for connection in connections {
            let entries = await withCheckedContinuation { (continuation: CheckedContinuation<[CallStatsEntry], Never>) in
                connection.statistics { report in
                    continuation.resume(returning: NativeQualityProbe.entries(report, full: full))
                }
            }
            reports.append(entries)
        }
        return reports
    }

    nonisolated static func entries(_ report: RTCStatisticsReport, full: Bool) -> [CallStatsEntry] {
        var out: [CallStatsEntry] = []
        for stat in report.statistics.values {
            guard full || stat.type == "media-source" || stat.type == "inbound-rtp" else { continue }
            var values: [String: CallStatsValue] = [:]
            for (key, raw) in stat.values {
                if let text = raw as? String {
                    values[key] = .text(text)
                } else if let number = raw as? NSNumber {
                    values[key] = CFGetTypeID(number) == CFBooleanGetTypeID() ? .flag(number.boolValue) : .number(number.doubleValue)
                }
            }
            out.append(CallStatsEntry(id: stat.id, type: stat.type, values: values))
        }
        return out
    }
}
#endif
