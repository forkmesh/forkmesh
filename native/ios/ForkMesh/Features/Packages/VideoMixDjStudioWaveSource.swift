import AVFoundation
import CoreMedia
import FMPackages
import Foundation

enum DjWaveSource {
    static let sampleRate: Double = 22_050
    static let largestDownload: Int64 = 200 * 1024 * 1024
    static let parallel = 4

    static func cached(_ id: String) -> [DjWaveBar]? {
        guard let url = cacheURL(id), let data = try? Data(contentsOf: url) else { return nil }
        let bars = DjWaveCache.decode(data)
        return bars.isEmpty ? nil : bars
    }

    static func read(for id: String, source: URL, bars wantsBars: Bool) async -> (bars: [DjWaveBar]?, audio: DjScratchAudio?) {
        await Task.detached(priority: .utility) { () -> ([DjWaveBar]?, DjScratchAudio?) in
            guard let file = await download(source) else { return (nil, nil) }
            defer { try? FileManager.default.removeItem(at: file) }
            guard let decoded = await decode(file, bars: wantsBars) else { return (nil, nil) }
            let bars = decoded.bars.flatMap { $0.isEmpty ? nil : $0 }
            if let bars, let url = cacheURL(id) { try? DjWaveCache.encode(bars).write(to: url, options: .atomic) }
            return (bars, decoded.audio)
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
        guard source.scheme == "https", let (head, response) = await fetch(source, span: 0...(DjWaveRanges.chunk - 1)) else { return nil }
        let mime = (response.mimeType ?? "").lowercased()
        guard !mime.contains("webm"), !mime.contains("mpegurl") else { return nil }
        let total = response.statusCode == 206
            ? DjWaveRanges.total(contentRange: response.value(forHTTPHeaderField: "Content-Range")) ?? DjWaveRanges.declaredLength(source)
            : Int64(head.count)
        guard let total, total > 0, total <= largestDownload, Int64(head.count) <= total else { return nil }
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("dj-wave-" + UUID().uuidString)
            .appendingPathExtension(mime.hasPrefix("audio/") ? "m4a" : "mp4")
        guard FileManager.default.createFile(atPath: file.path, contents: head), let handle = try? FileHandle(forWritingTo: file) else { return nil }
        let complete = await fill(handle, from: source, spans: DjWaveRanges.spans(total: total, from: Int64(head.count)))
        try? handle.close()
        guard complete else {
            try? FileManager.default.removeItem(at: file)
            return nil
        }
        return file
    }

    private static func fill(_ handle: FileHandle, from source: URL, spans: [ClosedRange<Int64>]) async -> Bool {
        await withTaskGroup(of: (ClosedRange<Int64>, Data?).self) { group in
            var waiting = spans.makeIterator()
            for _ in 0..<parallel {
                guard let span = waiting.next() else { break }
                group.addTask { (span, await fetch(source, span: span)?.0) }
            }
            while let (span, data) = await group.next() {
                guard let data, data.count == span.count, !Task.isCancelled,
                    (try? handle.seek(toOffset: UInt64(span.lowerBound))) != nil, (try? handle.write(contentsOf: data)) != nil
                else {
                    group.cancelAll()
                    return false
                }
                if let next = waiting.next() { group.addTask { (next, await fetch(source, span: next)?.0) } }
            }
            return true
        }
    }

    private static func fetch(_ source: URL, span: ClosedRange<Int64>) async -> (Data, HTTPURLResponse)? {
        var request = URLRequest(url: source)
        request.timeoutInterval = 30
        request.allowsConstrainedNetworkAccess = false
        request.setValue(DjWaveRanges.header(span), forHTTPHeaderField: "Range")
        request.setValue("https://www.youtube-nocookie.com/", forHTTPHeaderField: "Referer")
        request.setValue("https://www.youtube-nocookie.com", forHTTPHeaderField: "Origin")
        for attempt in 0..<3 {
            guard !Task.isCancelled else { return nil }
            if let (data, response) = try? await URLSession.shared.data(for: request), let http = response as? HTTPURLResponse {
                if (200...299).contains(http.statusCode) { return (data, http) }
                if (400...499).contains(http.statusCode) { return nil }
            }
            try? await Task.sleep(for: .milliseconds(400 * (attempt + 1)))
        }
        return nil
    }

    private static func decode(_ file: URL, bars wantsBars: Bool) async -> (bars: [DjWaveBar]?, audio: DjScratchAudio?)? {
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
        let keepsAudio = DjScratchAudio.fits(seconds: duration)
        guard wantsBars || keepsAudio else { return nil }
        let count = DjWaveAccumulator.bars(seconds: duration)
        var accumulator = DjWaveAccumulator(sampleRate: sampleRate, totalSamples: Int(duration * sampleRate), bars: count)
        var samples: [Float] = []
        var pcm: [Int16] = []
        if keepsAudio { pcm.reserveCapacity(Int(duration * sampleRate) + Int(sampleRate)) }
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
            if wantsBars { accumulator.add(samples) }
            if keepsAudio { pcm.append(contentsOf: samples.lazy.map(DjScratchAudio.pcm)) }
        }
        guard reader.status == .completed else { return nil }
        let audio = keepsAudio && pcm.count > Int(sampleRate) ? DjScratchAudio(samples: pcm, rate: sampleRate) : nil
        return (wantsBars ? accumulator.finish(bars: count) : nil, audio)
    }
}
