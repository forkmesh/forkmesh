import AVFoundation
import AVKit
import CoreMedia
import FMChat
import Photos
import SwiftUI
import UIKit

protocol MessageCaptureFilters: AnyObject {
    func processVideo(_ pixels: CVPixelBuffer, at time: CMTime) -> CVPixelBuffer
    func processAudio(_ sample: CMSampleBuffer) -> CMSampleBuffer
}

final class MessagePassThroughFilters: MessageCaptureFilters {
    func processVideo(_ pixels: CVPixelBuffer, at time: CMTime) -> CVPixelBuffer { pixels }

    func processAudio(_ sample: CMSampleBuffer) -> CMSampleBuffer { sample }
}

enum MessagesViewOnceFiles {
    static func fresh(_ ext: String) -> URL {
        URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
            .appendingPathComponent("viewonce-\(UUID().uuidString).\(ext)")
    }

    static func protect(_ url: URL) {
        try? FileManager.default.setAttributes([.protectionKey: FileProtectionType.complete], ofItemAtPath: url.path)
    }

    static func write(_ data: Data, ext: String) -> URL? {
        let url: URL = fresh(ext)
        do {
            try data.write(to: url, options: [.atomic, .completeFileProtection])
            return url
        } catch {
            return nil
        }
    }

    static func drop(_ url: URL?) {
        guard let url else { return }
        try? FileManager.default.removeItem(at: url)
    }
}

final class MessagesVideoCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate,
    AVCaptureAudioDataOutputSampleBufferDelegate, @unchecked Sendable
{
    static let width: Int = 480
    static let height: Int = 640
    static let videoBitRate: Int = 1_000_000
    static let audioBitRate: Int = 64_000

    let session: AVCaptureSession = AVCaptureSession()
    private let queue: DispatchQueue = DispatchQueue(label: "forkmesh.messages.viewonce.capture")
    private let videoOutput: AVCaptureVideoDataOutput = AVCaptureVideoDataOutput()
    private let audioOutput: AVCaptureAudioDataOutput = AVCaptureAudioDataOutput()
    private var writer: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?
    private var adaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var startTime: CMTime = .invalid
    private var lastTime: CMTime = .invalid
    private var recording: Bool = false
    private var configured: Bool = false
    var filters: MessageCaptureFilters = MessagePassThroughFilters()
    var maxSeconds: Double = Double(ViewOnceKind.video.maxDurationMs) / 1000
    var onLimit: (@Sendable () -> Void)?

    func configure() -> Bool {
        queue.sync {
            if configured { return true }
            session.beginConfiguration()
            defer { session.commitConfiguration() }
            if session.canSetSessionPreset(.vga640x480) { session.sessionPreset = .vga640x480 }
            guard let camera = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front),
                let cameraInput = try? AVCaptureDeviceInput(device: camera), session.canAddInput(cameraInput)
            else { return false }
            session.addInput(cameraInput)
            if let microphone = AVCaptureDevice.default(for: .audio),
                let microphoneInput = try? AVCaptureDeviceInput(device: microphone), session.canAddInput(microphoneInput)
            {
                session.addInput(microphoneInput)
            }
            videoOutput.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: Int(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
            ]
            videoOutput.alwaysDiscardsLateVideoFrames = true
            videoOutput.setSampleBufferDelegate(self, queue: queue)
            guard session.canAddOutput(videoOutput) else { return false }
            session.addOutput(videoOutput)
            audioOutput.setSampleBufferDelegate(self, queue: queue)
            if session.canAddOutput(audioOutput) { session.addOutput(audioOutput) }
            if let connection = videoOutput.connection(with: .video) {
                if connection.isVideoRotationAngleSupported(90) { connection.videoRotationAngle = 90 }
                if connection.isVideoMirroringSupported {
                    connection.automaticallyAdjustsVideoMirroring = false
                    connection.isVideoMirrored = true
                }
            }
            configured = true
            return true
        }
    }

    func run() {
        let running: AVCaptureSession = session
        DispatchQueue.global(qos: .userInitiated).async {
            if !running.isRunning { running.startRunning() }
        }
    }

    func halt() {
        let running: AVCaptureSession = session
        DispatchQueue.global(qos: .userInitiated).async {
            if running.isRunning { running.stopRunning() }
        }
    }

    func start(_ url: URL) -> Bool {
        queue.sync {
            guard !recording, let made = try? AVAssetWriter(outputURL: url, fileType: .mp4) else { return false }
            let video: AVAssetWriterInput = AVAssetWriterInput(
                mediaType: .video,
                outputSettings: [
                    AVVideoCodecKey: AVVideoCodecType.h264,
                    AVVideoWidthKey: MessagesVideoCapture.width,
                    AVVideoHeightKey: MessagesVideoCapture.height,
                    AVVideoCompressionPropertiesKey: [
                        AVVideoAverageBitRateKey: MessagesVideoCapture.videoBitRate,
                        AVVideoMaxKeyFrameIntervalKey: 60,
                        AVVideoProfileLevelKey: AVVideoProfileLevelH264MainAutoLevel,
                    ] as [String: Any],
                ])
            video.expectsMediaDataInRealTime = true
            let audio: AVAssetWriterInput = AVAssetWriterInput(
                mediaType: .audio,
                outputSettings: [
                    AVFormatIDKey: kAudioFormatMPEG4AAC,
                    AVNumberOfChannelsKey: 1,
                    AVSampleRateKey: 44_100,
                    AVEncoderBitRateKey: MessagesVideoCapture.audioBitRate,
                ])
            audio.expectsMediaDataInRealTime = true
            guard made.canAdd(video) else { return false }
            made.add(video)
            if made.canAdd(audio) { made.add(audio) }
            writer = made
            videoInput = video
            audioInput = made.inputs.contains(audio) ? audio : nil
            adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: video, sourcePixelBufferAttributes: nil)
            startTime = .invalid
            lastTime = .invalid
            recording = true
            return true
        }
    }

    func stop(_ done: @escaping @Sendable (URL?, Double) -> Void) {
        queue.async {
            guard self.recording, let writer = self.writer else {
                done(nil, 0)
                return
            }
            self.recording = false
            let seconds: Double = self.startTime.isValid && self.lastTime.isValid
                ? CMTimeGetSeconds(CMTimeSubtract(self.lastTime, self.startTime)) : 0
            self.writer = nil
            self.adaptor = nil
            guard writer.status == .writing else {
                writer.cancelWriting()
                done(nil, 0)
                return
            }
            self.videoInput?.markAsFinished()
            self.audioInput?.markAsFinished()
            writer.endSession(atSourceTime: self.lastTime.isValid ? self.lastTime : self.startTime)
            writer.finishWriting {
                done(writer.status == .completed ? writer.outputURL : nil, seconds)
            }
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard recording, let writer else { return }
        let time: CMTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        if output === videoOutput {
            guard let pixels = CMSampleBufferGetImageBuffer(sampleBuffer), let adaptor, let videoInput else { return }
            if writer.status == .unknown {
                guard writer.startWriting() else { return }
                writer.startSession(atSourceTime: time)
                startTime = time
            }
            guard writer.status == .writing, videoInput.isReadyForMoreMediaData else { return }
            let filtered: CVPixelBuffer = filters.processVideo(pixels, at: time)
            if adaptor.append(filtered, withPresentationTime: time) { lastTime = time }
            if CMTimeGetSeconds(CMTimeSubtract(time, startTime)) >= maxSeconds {
                let limit: (@Sendable () -> Void)? = onLimit
                DispatchQueue.main.async { limit?() }
            }
        } else if output === audioOutput {
            guard startTime.isValid, writer.status == .writing, let audioInput, audioInput.isReadyForMoreMediaData
            else { return }
            audioInput.append(filters.processAudio(sampleBuffer))
        }
    }
}

struct MessagesCameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    final class PreviewView: UIView {
        override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer? { layer as? AVCaptureVideoPreviewLayer }
    }

    func makeUIView(context: Context) -> PreviewView {
        let view: PreviewView = PreviewView()
        view.backgroundColor = .black
        view.previewLayer?.session = session
        view.previewLayer?.videoGravity = .resizeAspectFill
        if let connection = view.previewLayer?.connection, connection.isVideoRotationAngleSupported(90) {
            connection.videoRotationAngle = 90
        }
        return view
    }

    func updateUIView(_ view: PreviewView, context: Context) {}
}

enum MessagesViewOncePhase: Equatable {
    case preparing
    case idle
    case recording
    case review(URL, Int64)
    case sending(Double)
    case denied(String)
}

@MainActor
@Observable
final class MessagesViewOnceVideoModel {
    let capture: MessagesVideoCapture = MessagesVideoCapture()
    var phase: MessagesViewOncePhase = .preparing
    var startedAt: Date?

    func prepare() async {
        let camera: Bool = await AVCaptureDevice.requestAccess(for: .video)
        let microphone: Bool = await AVAudioApplication.requestRecordPermission()
        guard camera, microphone else {
            phase = .denied("Allow the camera and microphone in Settings to record video messages.")
            return
        }
        PromptDictation.yieldOverhearing()
        guard capture.configure() else {
            phase = .denied("The front camera is not available.")
            return
        }
        capture.onLimit = { [weak self] in
            Task { @MainActor in self?.stop() }
        }
        capture.run()
        phase = .idle
    }

    func record() {
        guard phase == .idle else { return }
        let url: URL = MessagesViewOnceFiles.fresh("mp4")
        guard capture.start(url) else { return }
        startedAt = Date()
        phase = .recording
        FmHaptics.tap()
    }

    func stop() {
        guard phase == .recording else { return }
        phase = .preparing
        capture.stop { [weak self] (url: URL?, seconds: Double) in
            Task { @MainActor in
                guard let self else {
                    MessagesViewOnceFiles.drop(url)
                    return
                }
                self.startedAt = nil
                guard let url else {
                    self.phase = .idle
                    return
                }
                MessagesViewOnceFiles.protect(url)
                let millis: Int64 = min(ViewOnceKind.video.maxDurationMs, Int64((seconds * 1000).rounded()))
                self.phase = .review(url, max(0, millis))
            }
        }
    }

    func discard() {
        if case .review(let url, _) = phase { MessagesViewOnceFiles.drop(url) }
        phase = .idle
    }

    func close() {
        if phase == .recording {
            capture.stop { (url: URL?, _: Double) in MessagesViewOnceFiles.drop(url) }
        }
        if case .review(let url, _) = phase { MessagesViewOnceFiles.drop(url) }
        capture.halt()
    }
}

@MainActor
@Observable
final class MessagesViewOnceVoiceModel: NSObject, AVAudioRecorderDelegate {
    var phase: MessagesViewOncePhase = .idle
    var startedAt: Date?
    var levels: [CGFloat] = []
    @ObservationIgnored private var recorder: AVAudioRecorder?
    @ObservationIgnored private var meter: Task<Void, Never>?
    @ObservationIgnored private var url: URL?

    func record() {
        guard phase == .idle else { return }
        Task { @MainActor in
            guard await AVAudioApplication.requestRecordPermission() else {
                phase = .denied("Allow the microphone in Settings to record voice messages.")
                return
            }
            begin()
        }
    }

    private func begin() {
        PromptDictation.yieldOverhearing()
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        let target: URL = MessagesViewOnceFiles.fresh("m4a")
        do {
            if !OtherAppCall.ownCall {
                try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
                try session.setActive(true)
            }
            let settings: [String: Any] = [
                AVFormatIDKey: Int(kAudioFormatMPEG4AAC), AVSampleRateKey: 22_050, AVNumberOfChannelsKey: 1,
                AVEncoderBitRateKey: 32_000,
            ]
            let made: AVAudioRecorder = try AVAudioRecorder(url: target, settings: settings)
            made.delegate = self
            made.isMeteringEnabled = true
            guard made.record(forDuration: Double(ViewOnceKind.voice.maxDurationMs) / 1000) else { return }
            MessagesViewOnceFiles.protect(target)
            recorder = made
            url = target
            levels = []
            startedAt = Date()
            phase = .recording
            FmHaptics.tap()
            meter = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 60_000_000)
                    guard let self, let recorder = self.recorder, recorder.isRecording else { return }
                    recorder.updateMeters()
                    let power: Float = recorder.averagePower(forChannel: 0)
                    let level: CGFloat = CGFloat(max(0, min(1, (power + 50) / 50)))
                    self.levels.append(level)
                    if self.levels.count > 48 { self.levels.removeFirst(self.levels.count - 48) }
                }
            }
        } catch {
            MessagesViewOnceFiles.drop(target)
            phase = .idle
        }
    }

    func stop() {
        guard phase == .recording, let recorder else { return }
        let seconds: TimeInterval = recorder.currentTime
        recorder.stop()
        finished(seconds: seconds)
    }

    private func finished(seconds: TimeInterval) {
        meter?.cancel()
        meter = nil
        recorder = nil
        startedAt = nil
        OtherAppCall.deactivateSession()
        guard let url else {
            phase = .idle
            return
        }
        let millis: Int64 = min(ViewOnceKind.voice.maxDurationMs, Int64((seconds * 1000).rounded()))
        phase = .review(url, max(0, millis))
    }

    nonisolated func audioRecorderDidFinishRecording(_ recorder: AVAudioRecorder, successfully flag: Bool) {
        Task { @MainActor in
            if self.phase == .recording, self.recorder === recorder {
                self.finished(seconds: Double(ViewOnceKind.voice.maxDurationMs) / 1000)
            }
        }
    }

    func discard() {
        if case .review(let url, _) = phase { MessagesViewOnceFiles.drop(url) }
        url = nil
        phase = .idle
    }

    func close() {
        if phase == .recording {
            recorder?.stop()
            recorder = nil
            meter?.cancel()
            OtherAppCall.deactivateSession()
        }
        MessagesViewOnceFiles.drop(url)
        url = nil
    }
}

struct MessagesViewOnceRequest: Identifiable {
    let kind: ViewOnceKind
    var id: String { kind.rawValue }
}

struct MessagesViewOnceRecorderSheet: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let kind: ViewOnceKind
    @State private var video: MessagesViewOnceVideoModel = MessagesViewOnceVideoModel()
    @State private var voice: MessagesViewOnceVoiceModel = MessagesViewOnceVoiceModel()
    @State private var sendPhase: MessagesViewOncePhase?
    @State private var captions: MessagesVideoCaptionsModel = MessagesVideoCaptionsModel()

    private var phase: MessagesViewOncePhase { sendPhase ?? (kind == .video ? video.phase : voice.phase) }

    private var startedAt: Date? { kind == .video ? video.startedAt : voice.startedAt }

    private var limitSeconds: Double { Double(kind.maxDurationMs) / 1000 }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            stage
            VStack(spacing: FmSpace.x3) {
                topBar
                Spacer(minLength: 0)
                controls
            }
            .padding(FmSpace.x4)
        }
        .task {
            if kind == .video { await video.prepare() }
        }
        .onChange(of: video.phase) { (_: MessagesViewOncePhase, now: MessagesViewOncePhase) in
            if case .review(let url, _) = now {
                captions.attach(url)
            } else {
                captions.reset()
            }
        }
        .onDisappear {
            captions.reset()
            video.close()
            voice.close()
        }
    }

    @ViewBuilder private var stage: some View {
        switch phase {
        case .review(let url, _):
            if kind == .video {
                MessagesViewOnceReviewVideo(url: captions.url(for: url))
                    .id(captions.url(for: url))
            } else {
                MessagesViewOnceReviewVoice(url: url)
            }
        case .denied(let text):
            Text(text)
                .font(.system(size: 17))
                .foregroundStyle(Color.white)
                .multilineTextAlignment(.center)
                .padding(FmSpace.x6)
        default:
            if kind == .video {
                MessagesCameraPreview(session: video.capture.session)
                    .ignoresSafeArea()
                    .accessibilityLabel("Front camera preview")
            } else {
                MessagesViewOnceLevels(levels: voice.levels, tint: Color.white)
                    .frame(height: 120)
                    .padding(.horizontal, FmSpace.x5)
                    .accessibilityHidden(true)
            }
        }
    }

    private var topBar: some View {
        HStack {
            Button(action: { dismiss() }) {
                Image(systemName: "xmark")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(Color.black.opacity(0.45)))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close without sending")
            Spacer()
            VStack(spacing: 2) {
                Text(kind == .video ? "View-once video" : "View-once voice message")
                    .font(.system(size: 15, weight: .semibold))
                Text("Deleted after it is played once")
                    .font(.system(size: 12))
                    .opacity(0.8)
            }
            .foregroundStyle(Color.white)
            .accessibilityElement(children: .combine)
            Spacer()
            countdown
                .frame(width: 64, alignment: .trailing)
        }
    }

    @ViewBuilder private var countdown: some View {
        if kind == .video, case .review(let url, _) = phase {
            MessagesVideoSaveButton(url: captions.url(for: url), disabled: captions.working)
        } else if phase == .recording, let startedAt {
            TimelineView(.periodic(from: Date(), by: 0.25)) { (context: TimelineViewDefaultContext) in
                let left: Double = max(0, limitSeconds - context.date.timeIntervalSince(startedAt))
                Text(ViewOnceFrames.durationLabel(Int64(left * 1000)))
                    .font(.system(size: 15, weight: .semibold).monospacedDigit())
                    .foregroundStyle(left <= 10 ? Color.red : Color.white)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(Color.black.opacity(0.45)))
                    .accessibilityLabel("\(Int(left.rounded())) seconds left")
            }
        } else {
            Color.clear.frame(width: 1, height: 1)
        }
    }

    @ViewBuilder private var controls: some View {
        switch phase {
        case .preparing:
            ProgressView().tint(Color.white)
        case .idle:
            recordButton(recording: false) { kind == .video ? video.record() : voice.record() }
                .accessibilityLabel(kind == .video ? "Record video message" : "Record voice message")
        case .recording:
            recordButton(recording: true) { kind == .video ? video.stop() : voice.stop() }
                .accessibilityLabel("Stop recording")
        case .review(let url, let millis):
            VStack(spacing: FmSpace.x3) {
                if kind == .video {
                    MessagesCaptionsBar(model: captions)
                }
                HStack(spacing: FmSpace.x4) {
                    pill("Discard", systemImage: "trash") {
                        discard()
                        dismiss()
                    }
                    .accessibilityLabel("Discard recording")
                    pill("Re-record", systemImage: "arrow.counterclockwise") { discard() }
                        .accessibilityLabel("Record again")
                    pill("Send", systemImage: "arrow.up.circle.fill", prominent: true) {
                        send(kind == .video ? captions.url(for: url) : url, millis)
                    }
                    .disabled(kind == .video && captions.working)
                    .opacity(kind == .video && captions.working ? 0.5 : 1)
                    .accessibilityLabel(kind == .video ? "Send view-once video" : "Send view-once voice message")
                }
            }
        case .sending(let progress):
            VStack(spacing: FmSpace.x2) {
                ProgressView(value: progress).tint(Color.white)
                Text("Encrypting and sending…").font(.system(size: 13)).foregroundStyle(Color.white)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Sending, \(Int(progress * 100)) percent")
        case .denied:
            EmptyView()
        }
    }

    private func recordButton(recording: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            ZStack {
                Circle().stroke(Color.white, lineWidth: 4).frame(width: 76, height: 76)
                if recording {
                    RoundedRectangle(cornerRadius: 6).fill(Color.red).frame(width: 30, height: 30)
                } else {
                    Circle().fill(Color.red).frame(width: 60, height: 60)
                }
            }
        }
        .buttonStyle(.plain)
    }

    private func pill(_ title: String, systemImage: String, prominent: Bool = false, action: @escaping () -> Void)
        -> some View
    {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Color.white)
                .padding(.horizontal, 14)
                .frame(minHeight: 44)
                .background(Capsule().fill(prominent ? MessagesColors.blue : Color.white.opacity(0.18)))
        }
        .buttonStyle(.plain)
    }

    private func discard() {
        if kind == .video { video.discard() } else { voice.discard() }
    }

    private func send(_ url: URL, _ millis: Int64) {
        guard let data = try? Data(contentsOf: url), !data.isEmpty else {
            hub.post("The recording could not be read.")
            discard()
            return
        }
        sendPhase = .sending(0)
        let mime: String = kind == .video ? "video/mp4" : "audio/mp4"
        Task { @MainActor in
            let sent: Bool = await hub.sendViewOnce(
                kind: kind, mime: mime, data: data, durationMs: millis,
                progress: { (value: Double) in sendPhase = .sending(value) })
            sendPhase = nil
            if sent {
                FmHaptics.send()
                discard()
                dismiss()
            } else {
                FmHaptics.failure()
            }
        }
    }
}

struct MessagesVideoSaveButton: View {
    let url: URL
    var disabled: Bool = false
    @State private var saving: Bool = false
    @State private var saved: URL?
    @State private var problem: String?

    private var photosCanKeep: Bool { ["mp4", "mov", "m4v"].contains(url.pathExtension.lowercased()) }

    var body: some View {
        Group {
            if photosCanKeep {
                Button(action: { Task { await save() } }) { face }
                    .buttonStyle(.plain)
                    .disabled(disabled || saving || saved == url)
                    .accessibilityLabel(saved == url ? "Saved to Photos" : "Save video to Photos")
            } else {
                ShareLink(item: url) { face }
                    .disabled(disabled)
                    .accessibilityLabel("Save or share video")
            }
        }
        .opacity(disabled ? 0.5 : 1)
        .alert("Video not saved", isPresented: Binding(get: { problem != nil }, set: { if !$0 { problem = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(problem ?? "")
        }
        .accessibilityIdentifier("chat-video-save")
    }

    private var face: some View {
        ZStack {
            if saving {
                ProgressView().tint(Color.white)
            } else {
                Image(systemName: saved == url ? "checkmark" : (photosCanKeep ? "arrow.down.to.line" : "square.and.arrow.up"))
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Color.white)
            }
        }
        .frame(width: 44, height: 44)
        .background(Circle().fill(Color.black.opacity(0.45)))
    }

    private func save() async {
        saving = true
        defer { saving = false }
        let status: PHAuthorizationStatus = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard status == .authorized || status == .limited else {
            problem = "Allow ForkMesh to add photos in Settings to save this video."
            FmHaptics.failure()
            return
        }
        let file: URL = url
        do {
            try await PHPhotoLibrary.shared().performChanges {
                PHAssetCreationRequest.forAsset().addResource(with: .video, fileURL: file, options: nil)
            }
            saved = file
            FmHaptics.commit()
        } catch {
            problem = "The video could not be saved to Photos."
            FmHaptics.failure()
        }
    }
}

struct MessagesViewOnceReviewVideo: View {
    let url: URL
    @State private var player: AVPlayer?

    var body: some View {
        VideoPlayer(player: player)
            .ignoresSafeArea()
            .onAppear {
                let made: AVPlayer = AVPlayer(url: url)
                player = made
                made.play()
            }
            .onDisappear {
                player?.pause()
                player = nil
            }
            .accessibilityLabel("Your video message")
    }
}

struct MessagesViewOnceReviewVoice: View {
    let url: URL
    @State private var player: AVAudioPlayer?
    @State private var playing: Bool = false

    var body: some View {
        VStack(spacing: FmSpace.x4) {
            Image(systemName: "waveform")
                .font(.system(size: 64))
                .foregroundStyle(Color.white)
                .accessibilityHidden(true)
            Button(action: toggle) {
                Image(systemName: playing ? "pause.circle.fill" : "play.circle.fill")
                    .font(.system(size: 56))
                    .foregroundStyle(Color.white)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(playing ? "Pause your voice message" : "Play your voice message")
        }
        .onDisappear { player?.stop() }
    }

    private func toggle() {
        if playing {
            player?.pause()
            playing = false
            return
        }
        do {
            if !OtherAppCall.ownCall {
                try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
                try AVAudioSession.sharedInstance().setActive(true)
            }
            let made: AVAudioPlayer = try player ?? AVAudioPlayer(contentsOf: url)
            player = made
            made.play()
            playing = true
            let length: TimeInterval = max(0.3, made.duration - made.currentTime)
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(length * 1_000_000_000))
                if player?.isPlaying != true { playing = false }
            }
        } catch {
            playing = false
        }
    }
}

struct MessagesViewOnceLevels: View {
    let levels: [CGFloat]
    let tint: Color

    var body: some View {
        GeometryReader { (geometry: GeometryProxy) in
            HStack(alignment: .center, spacing: 3) {
                ForEach(Array(padded.enumerated()), id: \.offset) { (entry: (offset: Int, element: CGFloat)) in
                    Capsule()
                        .fill(tint.opacity(0.85))
                        .frame(height: max(4, geometry.size.height * entry.element))
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var padded: [CGFloat] {
        let count: Int = 48
        if levels.count >= count { return Array(levels.suffix(count)) }
        return [CGFloat](repeating: 0.04, count: count - levels.count) + levels
    }
}
