import AVFoundation
import FMBase
import FMChat
import Observation
import SwiftUI

struct DirectCallPrecheckRequest: Hashable {
    let conversation: String
    let partner: String
    let media: DirectCallKind
}

@MainActor
@Observable
final class DirectCallPrecheck {
    static let shared = DirectCallPrecheck()

    var request: DirectCallPrecheckRequest?
}

struct DirectCallPreviewScreen: View {
    let request: DirectCallPrecheckRequest
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory
    @State private var peer: WebCallPeer?
    @State private var failed = false
    @State private var busy = false
    @State private var hearing = false
    @State private var record: FaceChangerRecordPhase = .idle
    @State private var notice = ""
    @State private var calling = false
    @State private var speaker = false
    @State private var closed = false

    private static let openTimeout: UInt64 = 30_000_000_000
    static let clipSeconds: Double = 10

    private var look: FaceChangerLook { engine.look }

    private var video: Bool { request.media == .video }

    private var opening: WebCallPeer? {
        guard let current = factory.current, current.previewing else { return nil }
        return current
    }

    private var reviewing: Bool {
        if case .review = record { return true }
        return false
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let surface = peer ?? opening, !failed {
                WebCallSurface(webView: surface.webView)
                    .ignoresSafeArea()
                    .opacity(video ? 1 : 0.01)
                    .allowsHitTesting(video)
            }
            if !video && !reviewing && !busy && !failed {
                VStack(spacing: FmSpace.x3) {
                    FmAvatar(name: request.partner, size: 110, fontSize: 40)
                    Text(peer == nil ? "Starting your microphone…" : "Your microphone is on")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(Color.white.opacity(0.8))
                }
            }
            clip
            if busy || failed {
                failure
            } else if video && peer == nil && !reviewing {
                ProgressView().tint(.white)
            }
            VStack(spacing: 0) {
                HStack {
                    Button("Cancel", action: cancel)
                        .font(.system(size: 17))
                        .foregroundStyle(.white)
                        .frame(minHeight: 44)
                        .accessibilityIdentifier("chat-call-precheck-cancel")
                    Spacer()
                }
                .padding(.horizontal, FmSpace.x4)
                header
                if peer != nil && !failed && !busy && !reviewing {
                    CallQualityWidget(monitor: factory.quality)
                        .padding(.top, 6)
                }
                Spacer()
                controls
                    .padding(.bottom, FmSpace.x5)
            }
        }
        .statusBarHidden(video)
        .task { await open() }
        .onChange(of: factory.current.map { ObjectIdentifier($0) }) { (_: ObjectIdentifier?, now: ObjectIdentifier?) in
            guard let peer, now != ObjectIdentifier(peer), !engine.active, !calling else { return }
            self.peer = nil
            discardReview()
            Task { await open() }
        }
        .onDisappear(perform: close)
        .accessibilityIdentifier("chat-call-precheck")
    }

    @ViewBuilder private var clip: some View {
        if case .review(let url, let mime) = record {
            Group {
                if mime.hasPrefix("audio/") {
                    CallClipAudioPlayer(url: url)
                } else {
                    MessagesViewOnceReviewVideo(url: url)
                }
            }
            .id(url)
            .onAppear(perform: routeToSpeaker)
        }
    }

    private var failure: some View {
        VStack(spacing: FmSpace.x3) {
            Text(failureText)
                .multilineTextAlignment(.center)
                .foregroundStyle(.white)
                .padding(.horizontal, FmSpace.x5)
            Button("Try again") {
                factory.endPreview()
                Task { await open() }
            }
            .buttonStyle(.borderedProminent)
            .accessibilityIdentifier("chat-call-precheck-retry")
        }
    }

    private var failureText: String {
        if busy { return "A call is using the camera and microphone. Try again when it ends." }
        return video
            ? "The camera could not start. You can still call, or try again."
            : "The microphone could not start. You can still call, or try again."
    }

    private var header: some View {
        VStack(spacing: 4) {
            Text("Call @\(request.partner)")
                .font(.system(size: 17, weight: .semibold))
                .lineLimit(1)
                .accessibilityAddTraits(.isHeader)
            if case .recording(let started) = record {
                countdown(started)
            } else {
                Text(subtitle)
                    .font(.system(size: 13))
                    .multilineTextAlignment(.center)
                    .opacity(0.85)
            }
            Text(DirectCallRoute.hint)
                .font(.system(size: 12))
                .multilineTextAlignment(.center)
                .opacity(0.7)
                .accessibilityIdentifier("chat-call-route-hint")
            if !notice.isEmpty {
                Text(notice)
                    .font(.system(size: 13, weight: .semibold))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Capsule().fill(Color.black.opacity(0.55)))
                    .padding(.top, 6)
                    .accessibilityIdentifier("chat-call-precheck-notice")
            }
        }
        .foregroundStyle(.white)
        .shadow(radius: 4)
        .padding(.top, FmSpace.x2)
        .padding(.horizontal, FmSpace.x5)
    }

    private var subtitle: String {
        switch record {
        case .starting: return "Getting ready to record…"
        case .finishing: return "Saving the test clip…"
        case .review: return video ? "This is how you look and sound." : "This is how you sound."
        default:
            if calling { return "Starting the call…" }
            return video ? "Video call · check your camera and voice first" : "Voice call · check your microphone first"
        }
    }

    private func countdown(_ started: Date) -> some View {
        TimelineView(.periodic(from: Date(), by: 0.25)) { (context: TimelineViewDefaultContext) in
            let left = Int(max(0, Self.clipSeconds - context.date.timeIntervalSince(started)).rounded(.up))
            Text("Recording test clip · \(left) s")
                .font(.system(size: 15, weight: .semibold).monospacedDigit())
                .foregroundStyle(Color.red)
                .accessibilityLabel("Recording, \(left) seconds left")
        }
    }

    @ViewBuilder private var controls: some View {
        switch record {
        case .idle:
            VStack(spacing: FmSpace.x4) {
                filters
                pill("Record test clip", systemImage: "record.circle", action: startRecording)
                    .disabled(peer == nil || calling)
                    .accessibilityHint("Records up to 10 seconds and plays it back")
                    .accessibilityIdentifier("chat-call-test-clip")
                callButton
            }
        case .starting, .finishing:
            ProgressView().tint(.white).frame(height: 76)
        case .recording:
            Button(action: { Task { await stopRecording() } }) {
                ZStack {
                    Circle().stroke(Color.white, lineWidth: 4).frame(width: 76, height: 76)
                    RoundedRectangle(cornerRadius: 6).fill(Color.red).frame(width: 30, height: 30)
                }
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Stop recording")
            .accessibilityIdentifier("chat-call-test-clip-stop")
        case .review:
            VStack(spacing: FmSpace.x4) {
                HStack(spacing: FmSpace.x3) {
                    pill("Re-record", systemImage: "arrow.counterclockwise", action: rerecord)
                        .accessibilityIdentifier("chat-call-test-clip-again")
                    pill("Discard", systemImage: "trash", action: discardReview)
                        .accessibilityLabel("Discard test clip")
                        .accessibilityIdentifier("chat-call-test-clip-discard")
                }
                callButton
            }
        }
    }

    private var filters: some View {
        HStack(spacing: FmSpace.x4) {
            if video {
                FaceChangerFaceMenu(look: look, peer: peer, notice: $notice)
                FaceChangerBackgroundButton(look: look, peer: peer)
            }
            FaceChangerVoiceMenu(look: look, shown: look.effectiveVoice)
            Button(action: toggleHearing) {
                FaceChangerRoundLabel(symbol: hearing ? "ear.fill" : "ear", on: hearing)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(hearing ? "Stop hearing myself" : "Hear myself")
            .accessibilityHint(
                CallAudioRoute.headphonesConnected
                    ? "Plays your changed voice in your headphones"
                    : "Use headphones: on the speaker your voice will echo")
            .accessibilityIdentifier("chat-call-hear-myself")
        }
        .disabled(peer == nil || calling)
    }

    private var callButton: some View {
        Button(action: call) {
            Label("Call", systemImage: video ? "video.fill" : "phone.fill")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.white)
                .frame(maxWidth: .infinity)
                .frame(height: 56)
                .background(Capsule().fill(Color.green))
        }
        .buttonStyle(.plain)
        .disabled(calling)
        .padding(.horizontal, FmSpace.x6)
        .accessibilityLabel(video ? "Start video call with @\(request.partner)" : "Start voice call with @\(request.partner)")
        .accessibilityIdentifier("chat-call-precheck-start")
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
        failed = false
        busy = false
        guard await WebCallPeerFactory.requestAccess(for: request.media) else {
            failed = true
            notice = video
                ? "Allow ForkMesh to use the camera and microphone in Settings to make video calls."
                : "Allow ForkMesh to use the microphone in Settings to make calls."
            return
        }
        guard !Task.isCancelled, !closed, !calling, !engine.active else { return }
        if factory.busy {
            busy = true
            return
        }
        let factory = factory
        let watchdog = Task { @MainActor in
            try? await Task.sleep(nanoseconds: Self.openTimeout)
            guard !Task.isCancelled, peer == nil else { return }
            factory.endPreview()
        }
        defer { watchdog.cancel() }
        do {
            let made: WebCallPeer
            if video {
                made = try await factory.preview(
                    media: .video, face: look.face, background: look.background, image: look.customBackgroundImage,
                    faceImage: look.customFaceImage)
            } else {
                made = try await factory.preview(media: .audio)
            }
            if closed {
                if !engine.active { factory.endPreview() }
                return
            }
            peer = made
            if CallAudioRoute.headphonesConnected {
                hearing = await made.setMonitor(true)
            }
        } catch {
            if !closed { failed = true }
        }
    }

    private func toggleHearing() {
        guard let peer else { return }
        let wanted = !hearing
        Task { @MainActor in
            hearing = await peer.setMonitor(wanted)
        }
    }

    private func startRecording() {
        guard let peer, record == .idle, !calling else { return }
        notice = ""
        record = .starting
        Task { @MainActor in
            do {
                try await peer.startRecording(
                    face: video ? look.face : "", background: video ? look.background : "", voice: look.effectiveVoice)
            } catch {
                record = .idle
                notice = Self.recordFailure(error)
                FmHaptics.failure()
                return
            }
            let started = Date()
            record = .recording(started)
            FmHaptics.tap()
            try? await Task.sleep(nanoseconds: UInt64(Self.clipSeconds * 1_000_000_000))
            if record == .recording(started) { await stopRecording() }
        }
    }

    private func stopRecording() async {
        guard case .recording = record, let peer else { return }
        record = .finishing
        if hearing { hearing = await peer.setMonitor(false) }
        do {
            let made = try await peer.finishRecording()
            guard let url = MessagesViewOnceFiles.write(made.data, ext: made.fileExtension) else {
                throw WebCallError.failed("recording_write")
            }
            if closed || calling {
                MessagesViewOnceFiles.drop(url)
                record = .idle
                return
            }
            record = .review(url, made.mime)
        } catch {
            record = .idle
            notice = "The test clip could not be saved. Try again."
            FmHaptics.failure()
        }
    }

    private static func recordFailure(_ error: Error) -> String {
        guard case WebCallError.failed(let reason) = error else { return "The test clip could not start." }
        switch reason {
        case "face_not_ready", "face_unavailable", "background_unavailable":
            return "The face or background is not ready yet. Try again in a moment."
        case "face_not_found":
            return "No face was found in your photo, so nothing was recorded."
        case "voice_unavailable":
            return "The changed voice could not start here, so nothing was recorded."
        case "recording_unsupported":
            return "This phone cannot record a test clip."
        default:
            return "The test clip could not start."
        }
    }

    private func rerecord() {
        discardReview()
        startRecording()
    }

    private func discardReview() {
        if case .review(let url, _) = record { MessagesViewOnceFiles.drop(url) }
        if reviewing { record = .idle }
        restoreRoute()
    }

    private func routeToSpeaker() {
        guard !CallAudioRoute.headphonesConnected else { return }
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        guard session.category == .playAndRecord else { return }
        if (try? session.overrideOutputAudioPort(.speaker)) != nil { speaker = true }
    }

    private func restoreRoute() {
        guard speaker else { return }
        speaker = false
        try? AVAudioSession.sharedInstance().overrideOutputAudioPort(.none)
    }

    private func call() {
        guard !calling else { return }
        calling = true
        notice = ""
        Task { @MainActor in
            discardReview()
            if let peer, hearing { hearing = await peer.setMonitor(false) }
            if peer == nil { factory.endPreview() }
            if await DirectCallActions.start(request, engine: engine) { return }
            calling = false
            notice = "The call could not start. Check that this conversation is connected."
            if peer == nil && !closed { await open() }
        }
    }

    private func cancel() {
        DirectCallPrecheck.shared.request = nil
    }

    private func close() {
        closed = true
        discardReview()
        hearing = false
        peer = nil
        if !engine.active { factory.endPreview() }
    }
}

struct CallClipAudioPlayer: View {
    let url: URL
    @State private var player: AVAudioPlayer?
    @State private var playing = false
    @State private var failed = false
    @State private var turn = 0

    var body: some View {
        VStack(spacing: FmSpace.x4) {
            Image(systemName: "waveform")
                .font(.system(size: 64))
                .foregroundStyle(Color.white)
                .accessibilityHidden(true)
            if failed {
                Text("This clip cannot be played on this phone.")
                    .font(.system(size: 13))
                    .multilineTextAlignment(.center)
                    .foregroundStyle(Color.white.opacity(0.8))
            } else {
                HStack(spacing: FmSpace.x5) {
                    Button(action: toggle) {
                        Image(systemName: playing ? "pause.circle.fill" : "play.circle.fill")
                            .font(.system(size: 56))
                            .foregroundStyle(Color.white)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(playing ? "Pause test clip" : "Play test clip")
                    Button(action: replay) {
                        Image(systemName: "arrow.counterclockwise.circle.fill")
                            .font(.system(size: 56))
                            .foregroundStyle(Color.white.opacity(0.85))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Replay test clip")
                }
            }
        }
        .onAppear(perform: replay)
        .onDisappear {
            turn += 1
            player?.stop()
            player = nil
            playing = false
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("chat-call-test-clip-player")
    }

    private func replay() {
        guard let made = loaded() else { return }
        made.currentTime = 0
        play(made)
    }

    private func toggle() {
        guard let made = loaded() else { return }
        if made.isPlaying {
            made.pause()
            playing = false
            turn += 1
            return
        }
        if made.currentTime >= made.duration { made.currentTime = 0 }
        play(made)
    }

    private func loaded() -> AVAudioPlayer? {
        if let player { return player }
        guard let made = try? AVAudioPlayer(contentsOf: url) else {
            failed = true
            return nil
        }
        made.prepareToPlay()
        player = made
        return made
    }

    private func play(_ made: AVAudioPlayer) {
        guard made.play() else {
            playing = false
            return
        }
        playing = true
        turn += 1
        let mine = turn
        let left = max(0.2, made.duration - made.currentTime)
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64((left + 0.1) * 1_000_000_000))
            guard mine == turn else { return }
            if player?.isPlaying != true { playing = false }
        }
    }
}
