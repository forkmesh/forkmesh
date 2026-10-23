import AVFoundation
import FMChat
import SwiftUI

enum NativeMicTestPhase: Equatable {
    case idle
    case recording(Date)
    case review(URL)
}

struct NativePreCallScreen: View {
    let request: DirectCallPrecheckRequest
    let engine: DirectCallEngine
    @State private var starting = false
    @State private var failed = ""
    @State private var opened = false
    @State private var test: NativeMicTestPhase = .idle
    @State private var recorder: AVAudioRecorder?
    @State private var speaker = false

    static let testSeconds: Double = 10

    private var video: Bool { request.media == .video }

    private var reviewing: Bool {
        if case .review = test { return true }
        return false
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            #if canImport(WebRTC)
            if video, let local = NativeCallPeerFactory.shared.localVideo {
                NativeVideoView(track: local, mirrored: true)
                    .ignoresSafeArea()
                    .accessibilityLabel("Your camera")
            } else if video && !opened {
                ProgressView().tint(.white)
            }
            #endif
            VStack(spacing: FmSpace.x3) {
                Text("Call @\(request.partner)")
                    .font(.system(size: 22, weight: .semibold))
                    .padding(.top, FmSpace.x6)
                    .accessibilityAddTraits(.isHeader)
                testLine
                Text(DirectCallRoute.hint)
                    .font(.system(size: 13))
                    .opacity(0.75)
                #if canImport(WebRTC)
                if !reviewing {
                    CallQualityWidget(monitor: NativeCallPeerFactory.shared.quality)
                }
                #endif
                if !video && !reviewing {
                    Spacer()
                    FmAvatar(name: request.partner, size: 110, fontSize: 40)
                }
                Spacer()
                if case .review(let url) = test {
                    CallClipAudioPlayer(url: url)
                        .id(url)
                        .onAppear(perform: routeToSpeaker)
                    Spacer()
                }
                if !failed.isEmpty {
                    Text(failed)
                        .font(.system(size: 13, weight: .semibold))
                        .multilineTextAlignment(.center)
                        .accessibilityIdentifier("chat-precall-notice")
                }
                testControls
                HStack(spacing: FmSpace.x6) {
                    Button("Cancel") { close() }
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("chat-precall-cancel")
                    Button(video ? "Video call" : "Voice call") { start() }
                        .buttonStyle(.borderedProminent)
                        .tint(.green)
                        .disabled(starting)
                        .accessibilityIdentifier("chat-precall-call")
                }
                .padding(.bottom, FmSpace.x7)
            }
            .foregroundStyle(.white)
            .shadow(radius: 4)
            .padding(.horizontal, FmSpace.x5)
        }
        .task { await open() }
        .onDisappear(perform: dropTest)
        .accessibilityIdentifier("chat-precall")
    }

    @ViewBuilder private var testLine: some View {
        switch test {
        case .recording(let started):
            TimelineView(.periodic(from: Date(), by: 0.25)) { (context: TimelineViewDefaultContext) in
                let left = Int(max(0, Self.testSeconds - context.date.timeIntervalSince(started)).rounded(.up))
                Text("Recording a test · \(left) s")
                    .font(.system(size: 15, weight: .semibold).monospacedDigit())
                    .foregroundStyle(Color.red)
                    .accessibilityLabel("Recording, \(left) seconds left")
            }
        case .review:
            Text("This is how you sound.")
                .font(.system(size: 13))
                .opacity(0.85)
        case .idle:
            Text(video ? "Video call · check your camera and microphone first" : "Voice call · check your microphone first")
                .font(.system(size: 13))
                .multilineTextAlignment(.center)
                .opacity(0.85)
        }
    }

    @ViewBuilder private var testControls: some View {
        switch test {
        case .idle:
            pill("Test microphone", systemImage: "waveform", action: startTest)
                .disabled(starting)
                .accessibilityHint("Records 10 seconds and plays it back")
                .accessibilityIdentifier("chat-precall-test")
        case .recording:
            pill("Stop", systemImage: "stop.fill", action: finishTest)
                .accessibilityLabel("Stop the test recording")
                .accessibilityIdentifier("chat-precall-test-stop")
        case .review:
            HStack(spacing: FmSpace.x3) {
                pill("Again", systemImage: "arrow.counterclockwise") {
                    dropTest()
                    startTest()
                }
                .accessibilityLabel("Record the test again")
                .accessibilityIdentifier("chat-precall-test-again")
                pill("Discard", systemImage: "trash", action: dropTest)
                    .accessibilityLabel("Discard the test recording")
                    .accessibilityIdentifier("chat-precall-test-discard")
            }
        }
    }

    private func pill(_ title: String, systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .frame(minHeight: 44)
                .background(Capsule().fill(Color.white.opacity(0.18)))
        }
        .buttonStyle(.plain)
    }

    private func open() async {
        #if canImport(WebRTC)
        if !(await NativeCallPeerFactory.shared.startPreview(media: request.media)) {
            failed = "The camera could not start."
        }
        #endif
        opened = true
    }

    private func startTest() {
        guard test == .idle, !starting else { return }
        failed = ""
        #if canImport(WebRTC)
        NativeCallPeerFactory.shared.pauseMeter()
        #endif
        let session = AVAudioSession.sharedInstance()
        do {
            if session.category != .playAndRecord {
                try session.setCategory(.playAndRecord, mode: .voiceChat, options: [.allowBluetooth, .allowBluetoothA2DP])
            }
            try session.setActive(true)
        } catch {
            failed = "The microphone could not start."
            resumeMeter()
            return
        }
        let url = MessagesViewOnceFiles.fresh("m4a")
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatMPEG4AAC), AVSampleRateKey: 48_000, AVNumberOfChannelsKey: 1,
            AVEncoderBitRateKey: 96_000,
        ]
        guard let made = try? AVAudioRecorder(url: url, settings: settings) else {
            failed = "The microphone could not start."
            resumeMeter()
            return
        }
        made.isMeteringEnabled = true
        guard made.record(forDuration: Self.testSeconds) else {
            MessagesViewOnceFiles.drop(url)
            failed = "The microphone could not start."
            resumeMeter()
            return
        }
        recorder = made
        let started = Date()
        test = .recording(started)
        FmHaptics.tap()
        Task { @MainActor in
            while test == .recording(started), let current = recorder, current === made {
                current.updateMeters()
                #if canImport(WebRTC)
                let level = CallQuality.level(decibels: Double(current.averagePower(forChannel: 0)))
                NativeCallPeerFactory.shared.quality.push(level: CallVoiceLevel(mic: level))
                #endif
                if !current.isRecording || Date().timeIntervalSince(started) >= Self.testSeconds {
                    finishTest()
                    return
                }
                try? await Task.sleep(nanoseconds: UInt64(CallQuality.levelEvery * 1_000_000_000))
            }
        }
    }

    private func finishTest() {
        guard case .recording = test, let made = recorder else { return }
        made.stop()
        recorder = nil
        test = .review(made.url)
    }

    private func dropTest() {
        if let made = recorder {
            made.stop()
            made.deleteRecording()
            recorder = nil
        }
        if case .review(let url) = test { MessagesViewOnceFiles.drop(url) }
        let was = test
        test = .idle
        restoreRoute()
        if was != .idle { resumeMeter() }
    }

    private func resumeMeter() {
        #if canImport(WebRTC)
        if !engine.active { NativeCallPeerFactory.shared.resumeMeter() }
        #endif
    }

    private func routeToSpeaker() {
        guard !CallAudioRoute.headphonesConnected else { return }
        let session = AVAudioSession.sharedInstance()
        guard session.category == .playAndRecord else { return }
        if (try? session.overrideOutputAudioPort(.speaker)) != nil { speaker = true }
    }

    private func restoreRoute() {
        guard speaker else { return }
        speaker = false
        try? AVAudioSession.sharedInstance().overrideOutputAudioPort(.none)
    }

    private func start() {
        starting = true
        failed = ""
        if test != .idle {
            if let made = recorder {
                made.stop()
                made.deleteRecording()
                recorder = nil
            }
            if case .review(let url) = test { MessagesViewOnceFiles.drop(url) }
            test = .idle
            restoreRoute()
            resumeMeter()
        }
        Task { @MainActor in
            let started = await DirectCallActions.start(request, engine: engine)
            starting = false
            if started {
                DirectCallPrecheck.shared.request = nil
            } else {
                failed = "The call could not start. Try again in a moment."
                resumeMeter()
            }
        }
    }

    private func close() {
        dropTest()
        #if canImport(WebRTC)
        if !engine.active { NativeCallPeerFactory.shared.endPreview() }
        #endif
        DirectCallPrecheck.shared.request = nil
    }
}
