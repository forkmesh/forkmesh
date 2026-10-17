import AVFoundation
import FMBase
import FMChat
import SwiftUI

enum FaceChangerRecordPhase: Equatable {
    case idle
    case starting
    case recording(Date)
    case finishing
    case review(URL, String)
}

struct FaceChangerScreen: View {
    private var studio: FaceChangerStudio { FaceChangerStudio.shared }
    @State private var peer: WebCallPeer?
    @State private var failed = false
    @State private var busy = false
    @State private var hearing = false
    @State private var record: FaceChangerRecordPhase = .idle
    @State private var notice = ""

    private static let openTimeout: UInt64 = 30_000_000_000
    private static let limitSeconds: Double = 5 * 60

    private var look: FaceChangerLook { studio.look }

    private var opening: WebCallPeer? {
        guard let current = studio.factory.current, current.previewing else { return nil }
        return current
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea(edges: .bottom)
            if let surface = peer ?? opening, !failed {
                WebCallSurface(webView: surface.webView).ignoresSafeArea(edges: .bottom)
            }
            if case .review(let url, _) = record {
                MessagesViewOnceReviewVideo(url: url)
                    .id(url)
                    .onAppear {
                        if !CallAudioRoute.headphonesConnected {
                            try? AVAudioSession.sharedInstance().overrideOutputAudioPort(.speaker)
                        }
                    }
            }
            if busy || failed {
                VStack(spacing: FmSpace.x3) {
                    Text(
                        busy
                            ? "A call is using the camera. Change your face from the call screen, or come back when it ends."
                            : "The camera could not start."
                    )
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white)
                    .padding(.horizontal, FmSpace.x5)
                    Button("Try again") { Task { await open() } }
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("face-changer-retry")
                }
            } else if peer == nil {
                ProgressView().tint(.white)
            }
            VStack {
                header
                Spacer()
                controls
                    .padding(.bottom, FmSpace.x5)
            }
        }
        .navigationTitle("Face Changer")
        .navigationBarTitleDisplayMode(.inline)
        .task { await open() }
        .onChange(of: studio.factory.current.map { ObjectIdentifier($0) }) { (_: ObjectIdentifier?, now: ObjectIdentifier?) in
            guard let peer, now != ObjectIdentifier(peer) else { return }
            self.peer = nil
            discardReview()
            Task { await open() }
        }
        .onDisappear(perform: close)
        .accessibilityIdentifier("face-changer")
    }

    private var header: some View {
        VStack(spacing: 4) {
            if case .recording(let started) = record {
                countdown(started)
            } else {
                Text(subtitle)
                    .font(.system(size: 13))
                    .multilineTextAlignment(.center)
                    .opacity(0.85)
            }
            if !notice.isEmpty {
                Text(notice)
                    .font(.system(size: 13, weight: .semibold))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Capsule().fill(Color.black.opacity(0.55)))
                    .padding(.top, 6)
                    .accessibilityIdentifier("face-changer-notice")
            }
        }
        .foregroundStyle(.white)
        .shadow(radius: 4)
        .padding(.top, FmSpace.x4)
        .padding(.horizontal, FmSpace.x5)
    }

    private var subtitle: String {
        switch record {
        case .starting: return "Getting the face, background and voice ready…"
        case .finishing: return "Saving the clip…"
        case .review: return "Share or save your clip."
        default: return "Only you can see this. Calls use the face, background and voice you pick here."
        }
    }

    private func countdown(_ started: Date) -> some View {
        TimelineView(.periodic(from: Date(), by: 0.25)) { (context: TimelineViewDefaultContext) in
            let left = max(0, Self.limitSeconds - context.date.timeIntervalSince(started))
            Text(ViewOnceFrames.durationLabel(Int64(left * 1000)))
                .font(.system(size: 15, weight: .semibold).monospacedDigit())
                .foregroundStyle(left <= 10 ? Color.red : Color.white)
                .accessibilityLabel("\(Int(left.rounded())) seconds left")
        }
    }

    @ViewBuilder private var controls: some View {
        switch record {
        case .idle:
            VStack(spacing: FmSpace.x4) {
                filters
                wide("Record a clip", systemImage: "record.circle", fill: Color.red, action: startRecording)
                    .disabled(peer == nil)
                    .padding(.horizontal, FmSpace.x6)
                    .accessibilityIdentifier("face-changer-record")
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
            .accessibilityIdentifier("face-changer-record-stop")
        case .review(let url, _):
            HStack(spacing: FmSpace.x3) {
                pill("Discard", systemImage: "trash") {
                    discardReview()
                }
                .accessibilityLabel("Discard clip")
                MessagesVideoSaveButton(url: url)
                ShareLink(item: url) {
                    Label("Share", systemImage: "square.and.arrow.up")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 14)
                        .frame(minHeight: 44)
                        .background(Capsule().fill(MessagesColors.blue))
                }
                .accessibilityIdentifier("face-changer-share")
            }
        }
    }

    private var filters: some View {
        HStack(spacing: FmSpace.x4) {
            FaceChangerFaceMenu(look: look, peer: peer, notice: $notice)
            FaceChangerBackgroundButton(look: look, peer: peer)
            FaceChangerVoiceMenu(look: look, shown: look.effectiveVoice)
            Button {
                Task { @MainActor in
                    hearing = await peer?.setMonitor(!hearing) ?? false
                }
            } label: {
                FaceChangerRoundLabel(symbol: hearing ? "ear.fill" : "ear", on: hearing)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(hearing ? "Stop hearing myself" : "Hear myself")
            .accessibilityHint(
                CallAudioRoute.headphonesConnected
                    ? "Plays your changed voice in your headphones"
                    : "Use headphones: on the speaker your voice will echo")
            .accessibilityIdentifier("face-changer-hear-myself")
        }
        .disabled(peer == nil)
    }

    private func wide(_ title: String, systemImage: String, fill: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .lineLimit(1)
                .foregroundStyle(.white)
                .padding(.horizontal, 12)
                .frame(maxWidth: .infinity)
                .frame(height: 50)
                .background(Capsule().fill(fill))
        }
        .buttonStyle(.plain)
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
        notice = ""
        guard await WebCallPeerFactory.requestAccess(for: .video) else {
            failed = true
            notice = "Allow ForkMesh to use the camera and microphone in Settings to use the Face Changer."
            return
        }
        if studio.factory.busy {
            busy = true
            return
        }
        let factory = studio.factory
        let watchdog = Task { @MainActor in
            try? await Task.sleep(nanoseconds: Self.openTimeout)
            guard !Task.isCancelled, peer == nil else { return }
            factory.endPreview()
        }
        defer { watchdog.cancel() }
        do {
            peer = try await factory.preview(
                face: look.face, background: look.background, image: look.customBackgroundImage,
                faceImage: look.customFaceImage)
            if CallAudioRoute.headphonesConnected {
                hearing = await peer?.setMonitor(true) ?? false
            }
        } catch {
            failed = true
        }
    }

    private func startRecording() {
        guard let peer, record == .idle else { return }
        notice = ""
        record = .starting
        Task { @MainActor in
            do {
                try await peer.startRecording(face: look.face, background: look.background, voice: look.effectiveVoice)
            } catch {
                record = .idle
                notice = Self.recordFailure(error)
                FmHaptics.failure()
                return
            }
            let started = Date()
            record = .recording(started)
            FmHaptics.tap()
            try? await Task.sleep(nanoseconds: UInt64(Self.limitSeconds * 1_000_000_000))
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
            record = .review(url, made.mime)
        } catch {
            record = .idle
            notice = "The clip could not be saved. Try again."
            FmHaptics.failure()
        }
    }

    private static func recordFailure(_ error: Error) -> String {
        guard case WebCallError.failed(let reason) = error else { return "Recording could not start." }
        switch reason {
        case "face_not_ready", "face_unavailable", "background_unavailable":
            return "The face or background could not start here, so nothing was recorded."
        case "face_not_found":
            return "No face was found in your photo, so nothing was recorded."
        case "voice_unavailable":
            return "The changed voice could not start here, so nothing was recorded."
        case "recording_unsupported":
            return "This phone cannot record from the Face Changer."
        default:
            return "Recording could not start."
        }
    }

    private func discardReview() {
        if case .review(let url, _) = record { MessagesViewOnceFiles.drop(url) }
        record = .idle
    }

    private func close() {
        discardReview()
        hearing = false
        peer = nil
        studio.factory.endPreview()
    }
}
