import AVFoundation
import CoreImage
import CoreMedia
import CoreText
import FMAccount
import FMBase
import FMChat
import Foundation
import Observation
import Speech
import SwiftUI
import UIKit

final class MessagesCaptionGate: @unchecked Sendable {
    private let lock: NSLock = NSLock()
    private var done: Bool = false
    private var cancelled: Bool = false
    private var task: SFSpeechRecognitionTask?
    private var recognizer: SFSpeechRecognizer?

    func hold(_ task: SFSpeechRecognitionTask, _ recognizer: SFSpeechRecognizer) {
        lock.lock()
        self.task = task
        self.recognizer = recognizer
        let stop: Bool = cancelled
        lock.unlock()
        if stop { task.cancel() }
    }

    func cancel() {
        lock.lock()
        cancelled = true
        let running: SFSpeechRecognitionTask? = task
        lock.unlock()
        running?.cancel()
    }

    func finish(_ resume: () -> Void) {
        lock.lock()
        let first: Bool = !done
        done = true
        if first {
            task = nil
            recognizer = nil
        }
        lock.unlock()
        if first { resume() }
    }
}

enum MessagesVideoTranscriber {
    static func authorized() async -> Bool {
        switch SFSpeechRecognizer.authorizationStatus() {
        case .authorized:
            return true
        case .notDetermined:
            let status: SFSpeechRecognizerAuthorizationStatus = await withCheckedContinuation {
                (continuation: CheckedContinuation<SFSpeechRecognizerAuthorizationStatus, Never>) in
                SFSpeechRecognizer.requestAuthorization { (status: SFSpeechRecognizerAuthorizationStatus) in
                    continuation.resume(returning: status)
                }
            }
            return status == .authorized
        default:
            return false
        }
    }

    static func recognizer() -> SFSpeechRecognizer? {
        var locales: [Locale] = []
        if let prefs = Services.get(KeyValueStore.self) {
            let chosen: String = SettingsService.dictationLocale(prefs)
            if !chosen.isEmpty { locales.append(Locale(identifier: chosen)) }
        }
        locales.append(Locale.current)
        locales.append(Locale(identifier: "en-US"))
        for locale in locales {
            if let made = SFSpeechRecognizer(locale: locale) { return made }
        }
        return nil
    }

    static func words(in url: URL) async throws -> [CaptionWord] {
        guard await authorized() else { throw ViewOnceCaptionsFailure.denied }
        guard let recognizer = recognizer(), recognizer.supportsOnDeviceRecognition else {
            throw ViewOnceCaptionsFailure.unavailable
        }
        guard recognizer.isAvailable else { throw ViewOnceCaptionsFailure.unavailable }
        let request: SFSpeechURLRecognitionRequest = SFSpeechURLRecognitionRequest(url: url)
        request.requiresOnDeviceRecognition = true
        request.shouldReportPartialResults = false
        request.taskHint = .dictation
        request.addsPunctuation = true
        let gate: MessagesCaptionGate = MessagesCaptionGate()
        let words: [CaptionWord] = try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<[CaptionWord], Error>) in
                let task: SFSpeechRecognitionTask = recognizer.recognitionTask(with: request) {
                    (result: SFSpeechRecognitionResult?, error: Error?) in
                    if let result, result.isFinal {
                        let found: [CaptionWord] = result.bestTranscription.segments.map {
                            (segment: SFTranscriptionSegment) -> CaptionWord in
                            CaptionWord(text: segment.substring, start: segment.timestamp, duration: segment.duration)
                        }
                        gate.finish { continuation.resume(returning: found) }
                    } else if let error {
                        let failure: Error = classify(error)
                        gate.finish { continuation.resume(throwing: failure) }
                    }
                }
                gate.hold(task, recognizer)
            }
        } onCancel: {
            gate.cancel()
        }
        try Task.checkCancellation()
        guard !ViewOnceCaptions.cleaned(words).isEmpty else { throw ViewOnceCaptionsFailure.noWords }
        return words
    }

    private static func classify(_ error: Error) -> Error {
        let ns: NSError = error as NSError
        if ns.domain == "kAFAssistantErrorDomain" && ns.code == 1110 {
            return ViewOnceCaptionsFailure.noWords
        }
        return ViewOnceCaptionsFailure.failed
    }
}

final class MessagesCaptionPainter: @unchecked Sendable {
    private let lines: [CaptionLine]
    private let lock: NSLock = NSLock()
    private var size: CGSize = .zero
    private var cache: [CaptionCue: CIImage] = [:]

    private static let pill: CGColor = CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.58)
    private static let spoken: CGColor = CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1)
    private static let upcoming: CGColor = CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.55)
    private static let active: CGColor = CGColor(srgbRed: 1, green: 0.84, blue: 0.04, alpha: 1)
    private static let glow: CGColor = CGColor(srgbRed: 1, green: 0.84, blue: 0.04, alpha: 0.55)

    init(lines: [CaptionLine]) {
        self.lines = lines
    }

    func apply(_ request: AVAsynchronousCIImageFilteringRequest) {
        let source: CIImage = request.sourceImage
        let extent: CGRect = source.extent
        let seconds: Double = CMTimeGetSeconds(request.compositionTime)
        guard !extent.isInfinite, !extent.isEmpty, let cue = ViewOnceCaptions.cue(at: seconds, in: lines),
            let overlay = overlay(cue, size: extent.size)
        else {
            request.finish(with: source, context: nil)
            return
        }
        let placed: CIImage = overlay.transformed(by: CGAffineTransform(translationX: extent.minX, y: extent.minY))
        request.finish(with: placed.composited(over: source).cropped(to: extent), context: nil)
    }

    private func overlay(_ cue: CaptionCue, size: CGSize) -> CIImage? {
        lock.lock()
        if size != self.size {
            self.size = size
            cache.removeAll()
        }
        if let cached = cache[cue] {
            lock.unlock()
            return cached
        }
        lock.unlock()
        guard let made = draw(cue, size: size) else { return nil }
        lock.lock()
        cache[cue] = made
        lock.unlock()
        return made
    }

    private static func font(_ size: CGFloat) -> CTFont {
        let base: UIFont = UIFont.systemFont(ofSize: size, weight: .heavy)
        guard let rounded = base.fontDescriptor.withDesign(.rounded) else { return base as CTFont }
        return UIFont(descriptor: rounded, size: size) as CTFont
    }

    private static func line(_ text: String, font: CTFont) -> CTLine {
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorFromContextAttributeName as String): true,
        ]
        return CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
    }

    private static func width(_ line: CTLine) -> CGFloat {
        CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    }

    private func draw(_ cue: CaptionCue, size: CGSize) -> CIImage? {
        guard cue.line >= 0, cue.line < lines.count else { return nil }
        let words: [CaptionWord] = lines[cue.line].words
        let layout: CaptionLayout = CaptionLayout(width: Double(size.width), height: Double(size.height))
        var fontSize: CGFloat = CGFloat(layout.fontSize)
        var font: CTFont = Self.font(fontSize)
        var runs: [CTLine] = words.map { Self.line($0.text, font: font) }
        var space: CGFloat = Self.width(Self.line(" ", font: font))
        var total: CGFloat = runs.map(Self.width).reduce(0, +) + space * CGFloat(max(0, runs.count - 1))
        let fitted: CGFloat = CGFloat(layout.fittedFontSize(textWidth: Double(total)))
        if fitted < fontSize {
            fontSize = fitted
            font = Self.font(fontSize)
            runs = words.map { Self.line($0.text, font: font) }
            space = Self.width(Self.line(" ", font: font))
            total = runs.map(Self.width).reduce(0, +) + space * CGFloat(max(0, runs.count - 1))
        }
        let ascent: CGFloat = CTFontGetAscent(font)
        let descent: CGFloat = CTFontGetDescent(font)
        let horizontal: CGFloat = CGFloat(layout.horizontalPadding(Double(fontSize)))
        let vertical: CGFloat = CGFloat(layout.verticalPadding(Double(fontSize)))
        let margin: CGFloat = CGFloat(layout.margin(Double(fontSize)))
        let pillWidth: CGFloat = (total + 2 * horizontal).rounded(.up)
        let pillHeight: CGFloat = (ascent + descent + 2 * vertical).rounded(.up)
        let canvasWidth: Int = Int(pillWidth + 2 * margin)
        let canvasHeight: Int = Int(pillHeight + 2 * margin)
        guard canvasWidth > 0, canvasHeight > 0, let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
            let context = CGContext(
                data: nil, width: canvasWidth, height: canvasHeight, bitsPerComponent: 8, bytesPerRow: 0,
                space: colorSpace, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        context.setShouldAntialias(true)
        context.textMatrix = .identity
        let pill: CGRect = CGRect(x: margin, y: margin, width: pillWidth, height: pillHeight)
        let corner: CGFloat = min(pillHeight / 2, CGFloat(layout.cornerRadius(Double(fontSize))))
        context.addPath(CGPath(roundedRect: pill, cornerWidth: corner, cornerHeight: corner, transform: nil))
        context.setFillColor(Self.pill)
        context.fillPath()
        let baseline: CGFloat = margin + vertical + descent
        var x: CGFloat = margin + horizontal
        for (index, run) in runs.enumerated() {
            let runWidth: CGFloat = Self.width(run)
            context.saveGState()
            switch cue.state(of: index) {
            case .active:
                let scale: CGFloat = CGFloat(CaptionLayout.activeScale)
                let centerX: CGFloat = x + runWidth / 2
                let centerY: CGFloat = baseline + (ascent - descent) / 2
                context.translateBy(x: centerX, y: centerY)
                context.scaleBy(x: scale, y: scale)
                context.translateBy(x: -centerX, y: -centerY)
                context.setShadow(offset: .zero, blur: fontSize * 0.35, color: Self.glow)
                context.setFillColor(Self.active)
            case .spoken:
                context.setFillColor(Self.spoken)
            case .upcoming:
                context.setFillColor(Self.upcoming)
            }
            context.textPosition = CGPoint(x: x, y: baseline)
            CTLineDraw(run, context)
            context.restoreGState()
            x += runWidth + space
        }
        guard let image = context.makeImage() else { return nil }
        let left: CGFloat = ((size.width - CGFloat(canvasWidth)) / 2).rounded()
        let bottom: CGFloat = CGFloat(layout.bottomInset) - margin
        return CIImage(cgImage: image).transformed(by: CGAffineTransform(translationX: left, y: bottom))
    }
}

final class MessagesCaptionLane: @unchecked Sendable {
    let input: AVAssetWriterInput
    let output: AVAssetReaderOutput
    let reports: Bool
    let queue: DispatchQueue
    var finished: Bool = false

    init(input: AVAssetWriterInput, output: AVAssetReaderOutput, reports: Bool, label: String) {
        self.input = input
        self.output = output
        self.reports = reports
        queue = DispatchQueue(label: label)
    }
}

final class MessagesCaptionRender: @unchecked Sendable {
    private let reader: AVAssetReader
    private let writer: AVAssetWriter
    private let lanes: [MessagesCaptionLane]
    private let seconds: Double
    private let lock: NSLock = NSLock()
    private var cancelled: Bool = false

    init(reader: AVAssetReader, writer: AVAssetWriter, lanes: [MessagesCaptionLane], seconds: Double) {
        self.reader = reader
        self.writer = writer
        self.lanes = lanes
        self.seconds = max(0.1, seconds)
    }

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    func run(progress: @escaping @Sendable (Double) -> Void) async throws {
        guard reader.startReading() else { throw ViewOnceCaptionsFailure.failed }
        guard writer.startWriting() else {
            reader.cancelReading()
            throw ViewOnceCaptionsFailure.failed
        }
        writer.startSession(atSourceTime: .zero)
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                let group: DispatchGroup = DispatchGroup()
                for lane in lanes {
                    group.enter()
                    lane.input.requestMediaDataWhenReady(on: lane.queue) { [self] in
                        pump(lane, group: group, progress: progress)
                    }
                }
                group.notify(queue: DispatchQueue.global(qos: .userInitiated)) { [self] in
                    finish(continuation)
                }
            }
        } onCancel: {
            cancel()
        }
    }

    private func pump(_ lane: MessagesCaptionLane, group: DispatchGroup, progress: @escaping @Sendable (Double) -> Void) {
        while !lane.finished && lane.input.isReadyForMoreMediaData {
            if isCancelled { reader.cancelReading() }
            guard reader.status == .reading, let sample = lane.output.copyNextSampleBuffer() else {
                close(lane, group: group)
                return
            }
            if lane.reports {
                let time: Double = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))
                if time.isFinite { progress(max(0, min(1, time / seconds))) }
            }
            if !lane.input.append(sample) {
                reader.cancelReading()
                close(lane, group: group)
                return
            }
        }
    }

    private func close(_ lane: MessagesCaptionLane, group: DispatchGroup) {
        guard !lane.finished else { return }
        lane.finished = true
        lane.input.markAsFinished()
        group.leave()
    }

    private func finish(_ continuation: CheckedContinuation<Void, Error>) {
        guard !isCancelled, reader.status == .completed, writer.status == .writing else {
            writer.cancelWriting()
            continuation.resume(throwing: isCancelled ? CancellationError() : ViewOnceCaptionsFailure.failed)
            return
        }
        let made: AVAssetWriter = writer
        made.finishWriting {
            if made.status == .completed {
                continuation.resume()
            } else {
                continuation.resume(throwing: ViewOnceCaptionsFailure.failed)
            }
        }
    }
}

enum MessagesVideoCaptions {
    static func caption(_ source: URL, progress: @escaping @Sendable (Double) -> Void) async throws -> URL {
        let asset: AVURLAsset = AVURLAsset(url: source)
        guard let video = try? await asset.loadTracks(withMediaType: .video).first else {
            throw ViewOnceCaptionsFailure.unsupported
        }
        guard let audio = try? await asset.loadTracks(withMediaType: .audio).first else {
            throw ViewOnceCaptionsFailure.noAudio
        }
        let words: [CaptionWord] = try await MessagesVideoTranscriber.words(in: source)
        let lines: [CaptionLine] = ViewOnceCaptions.lines(words)
        guard !lines.isEmpty else { throw ViewOnceCaptionsFailure.noWords }
        progress(0.3)
        let painter: MessagesCaptionPainter = MessagesCaptionPainter(lines: lines)
        let composition: AVMutableVideoComposition
        do {
            composition = try await AVMutableVideoComposition.videoComposition(
                with: asset,
                applyingCIFiltersWithHandler: { @Sendable (request: AVAsynchronousCIImageFilteringRequest) in
                    painter.apply(request)
                })
        } catch {
            throw ViewOnceCaptionsFailure.failed
        }
        let natural: CGSize = (try? await video.load(.naturalSize)) ?? .zero
        let transform: CGAffineTransform = (try? await video.load(.preferredTransform)) ?? .identity
        let turned: CGRect = CGRect(origin: .zero, size: natural).applying(transform)
        var size: CGSize = composition.renderSize
        if size.width <= 0 || size.height <= 0 { size = CGSize(width: abs(turned.width), height: abs(turned.height)) }
        let width: Int = max(2, Int(size.width.rounded()) / 2 * 2)
        let height: Int = max(2, Int(size.height.rounded()) / 2 * 2)
        composition.renderSize = CGSize(width: width, height: height)
        let duration: Double = CMTimeGetSeconds((try? await asset.load(.duration)) ?? .zero)
        let rate: Float = (try? await video.load(.estimatedDataRate)) ?? 0
        let format: CMFormatDescription? = (try? await audio.load(.formatDescriptions))?.first
        let stream: AudioStreamBasicDescription? = format.flatMap {
            CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee
        }
        let channels: Int = min(2, max(1, Int(stream?.mChannelsPerFrame ?? 1)))
        let sampleRate: Double = (stream?.mSampleRate ?? 44_100) >= 48_000 ? 48_000 : 44_100
        let audioBitRate: Int = ViewOnceCaptions.audioBitRate * channels
        let bitRate: Int = ViewOnceCaptions.videoBitRate(
            durationSeconds: duration, sourceBitRate: Double(rate), maxBytes: ViewOnceKind.video.maxBytes,
            audioBitRate: audioBitRate)
        let target: URL = MessagesViewOnceFiles.fresh("mp4")
        do {
            let reader: AVAssetReader = try AVAssetReader(asset: asset)
            let writer: AVAssetWriter = try AVAssetWriter(outputURL: target, fileType: .mp4)
            writer.shouldOptimizeForNetworkUse = true
            let videoOutput: AVAssetReaderVideoCompositionOutput = AVAssetReaderVideoCompositionOutput(
                videoTracks: [video],
                videoSettings: [kCVPixelBufferPixelFormatTypeKey as String: Int(kCVPixelFormatType_32BGRA)])
            videoOutput.videoComposition = composition
            videoOutput.alwaysCopiesSampleData = false
            let videoInput: AVAssetWriterInput = AVAssetWriterInput(
                mediaType: .video,
                outputSettings: [
                    AVVideoCodecKey: AVVideoCodecType.h264,
                    AVVideoWidthKey: width,
                    AVVideoHeightKey: height,
                    AVVideoCompressionPropertiesKey: [
                        AVVideoAverageBitRateKey: bitRate,
                        AVVideoMaxKeyFrameIntervalKey: 60,
                        AVVideoProfileLevelKey: AVVideoProfileLevelH264MainAutoLevel,
                    ] as [String: Any],
                ])
            videoInput.expectsMediaDataInRealTime = false
            guard reader.canAdd(videoOutput), writer.canAdd(videoInput) else { throw ViewOnceCaptionsFailure.failed }
            reader.add(videoOutput)
            writer.add(videoInput)
            var lanes: [MessagesCaptionLane] = [
                MessagesCaptionLane(
                    input: videoInput, output: videoOutput, reports: true, label: "forkmesh.messages.captions.video")
            ]
            let audioOutput: AVAssetReaderTrackOutput = AVAssetReaderTrackOutput(
                track: audio, outputSettings: [AVFormatIDKey: kAudioFormatLinearPCM])
            audioOutput.alwaysCopiesSampleData = false
            let audioInput: AVAssetWriterInput = AVAssetWriterInput(
                mediaType: .audio,
                outputSettings: [
                    AVFormatIDKey: kAudioFormatMPEG4AAC,
                    AVNumberOfChannelsKey: channels,
                    AVSampleRateKey: sampleRate,
                    AVEncoderBitRateKey: audioBitRate,
                ])
            audioInput.expectsMediaDataInRealTime = false
            if reader.canAdd(audioOutput), writer.canAdd(audioInput) {
                reader.add(audioOutput)
                writer.add(audioInput)
                lanes.append(
                    MessagesCaptionLane(
                        input: audioInput, output: audioOutput, reports: false, label: "forkmesh.messages.captions.audio"))
            }
            let render: MessagesCaptionRender = MessagesCaptionRender(
                reader: reader, writer: writer, lanes: lanes, seconds: duration)
            try await render.run { (value: Double) in progress(0.3 + 0.7 * value) }
        } catch {
            MessagesViewOnceFiles.drop(target)
            if error is CancellationError { throw error }
            throw error as? ViewOnceCaptionsFailure ?? ViewOnceCaptionsFailure.failed
        }
        MessagesViewOnceFiles.protect(target)
        let bytes: Int = (try? target.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard bytes > 0 else {
            MessagesViewOnceFiles.drop(target)
            throw ViewOnceCaptionsFailure.failed
        }
        guard bytes + ViewOnceFrames.tagBytes <= ViewOnceKind.video.maxBytes else {
            MessagesViewOnceFiles.drop(target)
            throw ViewOnceCaptionsFailure.tooLarge
        }
        progress(1)
        return target
    }
}

enum MessagesCaptionsState: Equatable {
    case off
    case working(Double)
    case ready(URL)
    case failed(String)
}

@MainActor
@Observable
final class MessagesVideoCaptionsModel {
    private(set) var enabled: Bool = Services.get(KeyValueStore.self)?.bool(forKey: ViewOnceCaptions.prefKey) ?? false
    private(set) var state: MessagesCaptionsState = .off
    @ObservationIgnored private var source: URL?
    @ObservationIgnored private var mime: String = "video/mp4"
    @ObservationIgnored private var captioned: URL?
    @ObservationIgnored private var job: Task<Void, Never>?
    @ObservationIgnored private var generation: Int = 0

    var working: Bool {
        if case .working = state { return true }
        return false
    }

    func url(for original: URL) -> URL {
        if enabled, source == original, case .ready(let made) = state { return made }
        return original
    }

    func attach(_ url: URL, mime: String = "video/mp4") {
        guard source != url else { return }
        reset()
        source = url
        self.mime = mime
        if enabled { start() }
    }

    func toggle() {
        enabled.toggle()
        Services.get(KeyValueStore.self)?.set(enabled, forKey: ViewOnceCaptions.prefKey)
        FmHaptics.tap()
        if enabled {
            start()
        } else {
            stop()
            state = .off
        }
    }

    func reset() {
        stop()
        MessagesViewOnceFiles.drop(captioned)
        captioned = nil
        source = nil
        state = .off
    }

    private func stop() {
        generation += 1
        job?.cancel()
        job = nil
    }

    private func start() {
        guard let source else { return }
        if let captioned {
            state = .ready(captioned)
            return
        }
        guard ViewOnceCaptions.canCaption(mime: mime) else {
            state = .failed(ViewOnceCaptionsFailure.unsupported.message)
            return
        }
        stop()
        let attempt: Int = generation
        state = .working(0)
        job = Task { @MainActor [weak self] in
            do {
                let made: URL = try await MessagesVideoCaptions.caption(source) { [weak self] (value: Double) in
                    Task { @MainActor in self?.advance(value, attempt) }
                }
                guard let self, self.generation == attempt, self.source == source else {
                    MessagesViewOnceFiles.drop(made)
                    return
                }
                self.captioned = made
                self.state = self.enabled ? .ready(made) : .off
                FmHaptics.tap()
            } catch {
                guard let self, self.generation == attempt, !(error is CancellationError) else { return }
                self.state = .failed((error as? ViewOnceCaptionsFailure ?? ViewOnceCaptionsFailure.failed).message)
            }
        }
    }

    private func advance(_ value: Double, _ attempt: Int) {
        guard generation == attempt, working else { return }
        state = .working(value)
    }
}

struct MessagesCaptionsBar: View {
    let model: MessagesVideoCaptionsModel

    var body: some View {
        VStack(spacing: FmSpace.x2) {
            Button(action: { model.toggle() }) {
                Label("Captions", systemImage: model.enabled ? "captions.bubble.fill" : "captions.bubble")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(model.enabled ? Color.black : Color.white)
                    .padding(.horizontal, 14)
                    .frame(minHeight: 44)
                    .background(Capsule().fill(model.enabled ? Color.white : Color.white.opacity(0.18)))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Captions")
            .accessibilityValue(model.enabled ? "On" : "Off")
            .accessibilityHint("Shows the words you say on the video, highlighted as you say them")
            .accessibilityAddTraits(model.enabled ? .isSelected : [])
            .accessibilityIdentifier("chat-video-captions")
            status
        }
    }

    @ViewBuilder private var status: some View {
        switch model.state {
        case .working(let value):
            VStack(spacing: 4) {
                ProgressView(value: value).tint(Color.white).frame(maxWidth: 220)
                Text("Adding captions…").font(.system(size: 13)).foregroundStyle(Color.white)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Adding captions, \(Int(value * 100)) percent")
        case .failed(let text):
            Text(text)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Color.white)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(Capsule().fill(Color.black.opacity(0.55)))
                .accessibilityIdentifier("chat-video-captions-notice")
        case .ready, .off:
            EmptyView()
        }
    }
}
