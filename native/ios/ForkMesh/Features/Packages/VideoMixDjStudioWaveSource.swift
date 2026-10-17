import AVFoundation
import CoreMedia
import FMPackages
import Foundation

enum DjWaveSource {
    static let sampleRate: Double = 22_050
    static let largestDownload: Int64 = 200 * 1024 * 1024

    static func cached(_ id: String) -> [DjWaveBar]? {
        guard let url = cacheURL(id), let data = try? Data(contentsOf: url) else { return nil }
        let bars = DjWaveCache.decode(data)
        return bars.isEmpty ? nil : bars
    }

    static func bars(for id: String, source: URL) async -> [DjWaveBar]? {
        await Task.detached(priority: .utility) { () -> [DjWaveBar]? in
            guard let file = await download(source) else { return nil }
            defer { try? FileManager.default.removeItem(at: file) }
            guard let bars = await decode(file), !bars.isEmpty else { return nil }
            if let url = cacheURL(id) { try? DjWaveCache.encode(bars).write(to: url, options: .atomic) }
            return bars
        }.value
    }

    private static func cacheURL(_ id: String) -> URL? {
        guard DjStudio.isVideoId(id),
            let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
        else { return nil }
        let folder = caches.appendingPathComponent("dj-waveforms", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.appendingPathComponent(id + ".wave")
    }

    private static func download(_ source: URL) async -> URL? {
        guard source.scheme == "https" else { return nil }
        var request = URLRequest(url: source)
        request.timeoutInterval = 60
        request.allowsConstrainedNetworkAccess = false
        guard let (file, response) = try? await URLSession.shared.download(for: request),
            let http = response as? HTTPURLResponse, (200...299).contains(http.statusCode),
            http.expectedContentLength <= largestDownload
        else { return nil }
        let mime = (http.mimeType ?? "").lowercased()
        guard !mime.contains("webm"), !mime.contains("mpegurl") else {
            try? FileManager.default.removeItem(at: file)
            return nil
        }
        let named = FileManager.default.temporaryDirectory
            .appendingPathComponent("dj-wave-" + UUID().uuidString)
            .appendingPathExtension(mime.hasPrefix("audio/") ? "m4a" : "mp4")
        do {
            try FileManager.default.moveItem(at: file, to: named)
        } catch {
            try? FileManager.default.removeItem(at: file)
            return nil
        }
        return named
    }

    private static func decode(_ file: URL) async -> [DjWaveBar]? {
        let asset = AVURLAsset(url: file)
        guard let track = try? await asset.loadTracks(withMediaType: .audio).first,
            let duration = try? await asset.load(.duration).seconds, duration.isFinite, duration > 1,
            let reader = try? AVAssetReader(asset: asset)
        else { return nil }
        let output = AVAssetReaderAudioMixOutput(audioTracks: [track], audioSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsBigEndianKey: false,
            AVLinearPCMIsNonInterleaved: false,
            AVNumberOfChannelsKey: 1,
            AVSampleRateKey: sampleRate,
        ])
        guard reader.canAdd(output) else { return nil }
        reader.add(output)
        guard reader.startReading() else { return nil }
        var accumulator = DjWaveAccumulator(sampleRate: sampleRate, totalSamples: Int(duration * sampleRate))
        var samples: [Float] = []
        while let buffer = output.copyNextSampleBuffer() {
            if Task.isCancelled {
                reader.cancelReading()
                return nil
            }
            guard let block = CMSampleBufferGetDataBuffer(buffer) else { continue }
            let count = CMBlockBufferGetDataLength(block) / MemoryLayout<Float>.size
            guard count > 0 else { continue }
            if samples.count != count { samples = [Float](repeating: 0, count: count) }
            let status = samples.withUnsafeMutableBytes { raw -> OSStatus in
                guard let base = raw.baseAddress else { return -1 }
                return CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: count * MemoryLayout<Float>.size, destination: base)
            }
            guard status == kCMBlockBufferNoErr else { continue }
            accumulator.add(samples)
        }
        guard reader.status == .completed else { return nil }
        return accumulator.finish()
    }
}
