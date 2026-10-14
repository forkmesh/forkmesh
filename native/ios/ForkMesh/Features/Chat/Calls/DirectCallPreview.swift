import AVFoundation
import FMBase
import FMChat
import Foundation
import ImageIO
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct CallPhotoStore {
    let name: String

    private var file: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("ForkMesh", isDirectory: true)
            .appendingPathComponent(name)
    }

    func load() -> String {
        guard let file, let data = try? Data(contentsOf: file), !data.isEmpty else { return "" }
        return "data:image/jpeg;base64," + data.base64EncodedString()
    }

    func save(_ image: String) {
        guard let file, let comma = image.firstIndex(of: ","),
            let data = Data(base64Encoded: String(image[image.index(after: comma)...]))
        else { return }
        try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: file, options: [.atomic, .completeFileProtection])
    }
}

enum CallBackgroundStore {
    private static let store = CallPhotoStore(name: "call-background.jpg")

    static func load() -> String { store.load() }

    static func save(_ image: String) { store.save(image) }
}

enum CallFaceStore {
    private static let store = CallPhotoStore(name: "call-face.jpg")

    static func load() -> String { store.load() }

    static func save(_ image: String) { store.save(image) }
}

enum CallBackgroundPhoto {
    static let side = 1920
    static let faceSide = 1024

    static func dataURL(from data: Data, side: Int = CallBackgroundPhoto.side) async -> String? {
        await Task.detached(priority: .userInitiated) { prepare(data, side: side) }.value
    }

    private static func prepare(_ data: Data, side: Int) -> String? {
        guard let source = CGImageSourceCreateWithData(data as CFData, [kCGImageSourceShouldCache: false] as CFDictionary)
        else { return nil }
        let options: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: side,
        ] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options) else { return nil }
        let out = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(out, UTType.jpeg.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: 0.86] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return "data:image/jpeg;base64," + (out as Data).base64EncodedString()
    }

    static func image(fromDataURL text: String) -> UIImage? {
        guard let comma = text.firstIndex(of: ","),
            let data = Data(base64Encoded: String(text[text.index(after: comma)...]))
        else { return nil }
        return UIImage(data: data)
    }
}

struct DirectCallBackgroundSheet: View {
    @Environment(\.dismiss) private var dismiss
    let engine: DirectCallEngine
    let peer: WebCallPeer?
    @State private var thumbs: [String: UIImage] = [:]
    @State private var photo: PhotosPickerItem?
    @State private var working = false

    private let columns = [GridItem(.adaptive(minimum: 100), spacing: 10)]

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 12) {
                    ForEach(DirectCallEngine.backgrounds) { (background: DirectCallFace) in
                        if background.id == DirectCallEngine.customBackground {
                            PhotosPicker(selection: $photo, matching: .images) { tile(background) }
                                .buttonStyle(.plain)
                                .accessibilityIdentifier("chat-call-background-custom")
                        } else {
                            Button(action: { choose(background.id, image: "") }) { tile(background) }
                                .buttonStyle(.plain)
                                .accessibilityIdentifier("chat-call-background-\(background.id.isEmpty ? "off" : background.id)")
                        }
                    }
                }
                .padding(FmSpace.x4)
                if working {
                    ProgressView().padding()
                }
            }
            .navigationTitle("Background")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .task { await loadThumbs() }
        .onChange(of: photo) { _, item in
            guard let item else { return }
            working = true
            Task { @MainActor in
                defer { working = false }
                guard let data = try? await item.loadTransferable(type: Data.self),
                    let image = await CallBackgroundPhoto.dataURL(from: data)
                else { return }
                choose(DirectCallEngine.customBackground, image: image)
            }
        }
    }

    private func tile(_ background: DirectCallFace) -> some View {
        let selected = engine.background == background.id
        return VStack(spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Color.gray.opacity(0.25))
                if let image = preview(background.id) {
                    Image(uiImage: image).resizable().scaledToFill()
                } else {
                    Image(systemName: symbol(background.id))
                        .font(.system(size: 22, weight: .semibold))
                        .foregroundStyle(.secondary)
                }
            }
            .aspectRatio(16 / 9, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .stroke(selected ? Color.accentColor : Color.clear, lineWidth: 3))
            Text(background.label)
                .font(.system(size: 12, weight: selected ? .semibold : .regular))
                .lineLimit(1)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(background.label)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func preview(_ id: String) -> UIImage? {
        if id == DirectCallEngine.customBackground {
            return engine.backgroundImage.isEmpty ? nil : CallBackgroundPhoto.image(fromDataURL: engine.backgroundImage)
        }
        return thumbs[id]
    }

    private func symbol(_ id: String) -> String {
        switch id {
        case "": return "rectangle.slash"
        case "blur", "blur-strong": return "aqi.medium"
        default: return "photo.on.rectangle"
        }
    }

    private func loadThumbs() async {
        guard let peer else { return }
        var images: [String: UIImage] = [:]
        for (id, text) in await peer.backgroundThumbnails() {
            if let image = CallBackgroundPhoto.image(fromDataURL: text) { images[id] = image }
        }
        thumbs = images
    }

    private func choose(_ id: String, image: String) {
        engine.setBackground(id, image: image)
        if !engine.active || peer?.previewing == true {
            peer?.setBackground(engine.background, image: engine.background == DirectCallEngine.customBackground ? engine.backgroundImage : "")
        }
    }
}

enum DirectCallRecordPhase: Equatable {
    case idle
    case starting
    case recording(Date)
    case finishing
    case review(URL, Int64, String)
    case sending(Double)
}

struct DirectCallPreviewScreen: View {
    let hub: ChatHub
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory
    let onStart: () -> Void
    let onClose: () -> Void
    @State private var peer: WebCallPeer?
    @State private var failed = false
    @State private var showingBackgrounds = false
    @State private var hearing = false
    @State private var record: DirectCallRecordPhase = .idle
    @State private var notice = ""
    @State private var captions = MessagesVideoCaptionsModel()
    @State private var pickingFace = false
    @State private var facePhoto: PhotosPickerItem?
    @State private var faceWorking = false

    private static let openTimeout: UInt64 = 30_000_000_000
    private static let limitSeconds = Double(ViewOnceKind.video.maxDurationMs) / 1000

    private var canRecord: Bool { hub.canSendViewOnce }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let surface = peer ?? factory.current, !failed {
                WebCallSurface(webView: surface.webView).ignoresSafeArea()
            }
            if case .review(let url, _, _) = record {
                MessagesViewOnceReviewVideo(url: captions.url(for: url))
                    .id(captions.url(for: url))
                    .onAppear {
                        if !CallAudioRoute.headphonesConnected {
                            try? AVAudioSession.sharedInstance().overrideOutputAudioPort(.speaker)
                        }
                    }
            }
            if failed {
                VStack(spacing: FmSpace.x3) {
                    Text("The camera could not start.")
                        .foregroundStyle(.white)
                    Button("Try again") { Task { await open() } }
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("chat-call-preview-retry")
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
        .statusBarHidden(true)
        .sheet(isPresented: $showingBackgrounds) {
            DirectCallBackgroundSheet(engine: engine, peer: peer)
                .presentationDetents([.medium, .large])
        }
        .photosPicker(isPresented: $pickingFace, selection: $facePhoto, matching: .images)
        .onChange(of: facePhoto) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            guard let item else { return }
            facePhoto = nil
            Task { await useFacePhoto(item) }
        }
        .task { await open() }
        .onChange(of: record) { (_: DirectCallRecordPhase, now: DirectCallRecordPhase) in
            switch now {
            case .review(let url, _, let mime):
                captions.attach(url, mime: mime)
            case .sending:
                break
            default:
                captions.reset()
            }
        }
        .onDisappear { discardReview() }
        .accessibilityIdentifier("chat-call-preview")
    }

    private var header: some View {
        ZStack(alignment: .top) {
            VStack(spacing: 4) {
                Text(title)
                    .font(.system(size: 17, weight: .semibold))
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
                        .accessibilityIdentifier("chat-call-record-notice")
                }
            }
            .foregroundStyle(.white)
            .shadow(radius: 4)
            .padding(.horizontal, 56)
            HStack {
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: 44, height: 44)
                        .background(Circle().fill(Color.white.opacity(0.18)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close")
                .accessibilityIdentifier("chat-call-preview-close")
                Spacer()
            }
        }
        .padding(.top, FmSpace.x4)
        .padding(.horizontal, FmSpace.x4)
    }

    private var title: String {
        switch record {
        case .recording: return "Recording"
        case .review, .sending: return "Your message"
        default: return "Preview"
        }
    }

    private var subtitle: String {
        switch record {
        case .review, .sending:
            return "They can watch it once, then it is deleted."
        case .starting:
            return "Getting the face, background and voice ready…"
        default:
            return canRecord
                ? "Only you can see this. Call now, or record a message they can watch once."
                : "Only you can see this. The other person will see exactly this picture."
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
                HStack(spacing: FmSpace.x3) {
                    if canRecord {
                        wide("Record message", systemImage: "record.circle", fill: Color.red, action: startRecording)
                            .disabled(peer == nil)
                            .accessibilityHint("Records what you see here as a video message they can watch once")
                            .accessibilityIdentifier("chat-call-record")
                    }
                    wide("Start video call", systemImage: "video.fill", fill: Color.green, action: onStart)
                        .disabled(peer == nil)
                        .accessibilityIdentifier("chat-call-start")
                }
                .padding(.horizontal, FmSpace.x4)
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
            .accessibilityIdentifier("chat-call-record-stop")
        case .review(let url, let millis, let mime):
            VStack(spacing: FmSpace.x3) {
                MessagesCaptionsBar(model: captions)
                HStack(spacing: FmSpace.x3) {
                    pill("Discard", systemImage: "trash") {
                        discardReview()
                        record = .idle
                    }
                    .accessibilityLabel("Discard recording")
                    pill("Re-record", systemImage: "arrow.counterclockwise") {
                        discardReview()
                        record = .idle
                        startRecording()
                    }
                    .accessibilityLabel("Record again")
                    pill("Send", systemImage: "arrow.up.circle.fill", fill: MessagesColors.blue) {
                        send(url, millis, mime)
                    }
                    .disabled(captions.working)
                    .opacity(captions.working ? 0.5 : 1)
                    .accessibilityLabel("Send video message")
                    .accessibilityIdentifier("chat-call-record-send")
                }
            }
        case .sending(let progress):
            VStack(spacing: FmSpace.x2) {
                ProgressView(value: progress).tint(.white)
                Text("Encrypting and sending…").font(.system(size: 13)).foregroundStyle(.white)
            }
            .padding(.horizontal, FmSpace.x6)
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Sending, \(Int(progress * 100)) percent")
        }
    }

    private var filters: some View {
        HStack(spacing: FmSpace.x4) {
            faceMenu
            round("photo.on.rectangle", label: "Background", on: !engine.background.isEmpty) {
                showingBackgrounds = true
            }
            .accessibilityValue(DirectCallEngine.backgroundLabel(engine.background))
            .accessibilityIdentifier("chat-call-background")
            voiceMenu
            round(hearing ? "ear.fill" : "ear", label: hearing ? "Stop hearing myself" : "Hear myself", on: hearing) {
                Task { @MainActor in
                    hearing = await peer?.setMonitor(!hearing) ?? false
                }
            }
            .accessibilityHint(
                CallAudioRoute.headphonesConnected
                    ? "Plays your changed voice in your headphones"
                    : "Use headphones: on the speaker your voice will echo")
            .accessibilityIdentifier("chat-call-hear-myself")
        }
        .disabled(peer == nil)
    }

    private func wide(_ title: String, systemImage: String, fill: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .foregroundStyle(.white)
                .padding(.horizontal, 12)
                .frame(maxWidth: .infinity)
                .frame(height: 50)
                .background(Capsule().fill(fill))
        }
        .buttonStyle(.plain)
    }

    private func pill(_ title: String, systemImage: String, fill: Color = Color.white.opacity(0.18), action: @escaping () -> Void)
        -> some View
    {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .frame(minHeight: 44)
                .background(Capsule().fill(fill))
        }
        .buttonStyle(.plain)
    }

    private var faceMenu: some View {
        Menu {
            Picker("Face", selection: Binding(get: { engine.face }, set: { chooseFace($0) })) {
                Text("Your own face").tag("")
                ForEach(DirectCallEngine.faces) { (face: DirectCallFace) in
                    Text(face.label).tag(face.id)
                }
                if !engine.faceImage.isEmpty {
                    Text(DirectCallEngine.faceLabel(DirectCallEngine.customFace)).tag(DirectCallEngine.customFace)
                }
            }
            Button {
                pickingFace = true
            } label: {
                Label(engine.faceImage.isEmpty ? "Use a photo of a face…" : "Choose another photo…", systemImage: "person.crop.square.badge.camera")
            }
            .accessibilityIdentifier("chat-call-face-photo")
        } label: {
            if faceWorking {
                ProgressView().tint(.white).frame(width: 56, height: 56)
            } else {
                roundLabel("theatermasks.fill", on: !engine.face.isEmpty)
            }
        }
        .accessibilityLabel("Face")
        .accessibilityValue(DirectCallEngine.faceLabel(engine.face))
        .accessibilityIdentifier("chat-call-face")
    }

    private func useFacePhoto(_ item: PhotosPickerItem) async {
        faceWorking = true
        defer { faceWorking = false }
        guard let data = try? await item.loadTransferable(type: Data.self),
            let image = await CallBackgroundPhoto.dataURL(from: data, side: CallBackgroundPhoto.faceSide)
        else {
            notice = "That photo could not be opened."
            return
        }
        let previous = engine.face
        engine.setFace(DirectCallEngine.customFace, image: image)
        peer?.setFace(DirectCallEngine.customFace, image: image)
        peer?.setVoice(engine.effectiveVoice)
        guard let peer else { return }
        for _ in 0..<120 {
            let state = await peer.faceState()
            if state == "ready" {
                notice = ""
                FmHaptics.tap()
                return
            }
            if state == "failed" {
                notice = "No face was found in that photo. Try a clear photo of one face looking at the camera."
                FmHaptics.failure()
                chooseFace(previous == DirectCallEngine.customFace ? "" : previous)
                return
            }
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
    }

    private var voiceMenu: some View {
        Menu {
            Picker("Voice", selection: Binding(get: { engine.voiceChoice }, set: { chooseVoice($0) })) {
                Text("Match face").tag(DirectCallEngine.voiceMatchFace)
                Text("Your own voice").tag(DirectCallEngine.voiceOff)
                ForEach(DirectCallEngine.voices) { (voice: DirectCallFace) in
                    Text(voice.label).tag(voice.id)
                }
            }
        } label: {
            roundLabel("waveform", on: !engine.effectiveVoice.isEmpty)
        }
        .accessibilityLabel("Voice")
        .accessibilityValue("Voice: \(DirectCallEngine.voiceLabel(engine.effectiveVoice))")
        .accessibilityIdentifier("chat-call-voice")
    }

    private func roundLabel(_ symbol: String, on: Bool) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 20, weight: .semibold))
            .foregroundStyle(on ? Color.black : Color.white)
            .frame(width: 56, height: 56)
            .background(Circle().fill(on ? Color.white : Color.white.opacity(0.18)))
    }

    private func round(_ symbol: String, label: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) { roundLabel(symbol, on: on) }
            .buttonStyle(.plain)
            .accessibilityLabel(label)
    }

    private func chooseFace(_ id: String) {
        engine.setFace(id)
        if engine.face == DirectCallEngine.customFace {
            peer?.setFace(engine.face, image: engine.faceImage)
        } else {
            peer?.setFace(engine.face)
        }
        peer?.setVoice(engine.effectiveVoice)
    }

    private func chooseVoice(_ choice: String) {
        engine.setVoiceChoice(choice)
        peer?.setVoice(engine.effectiveVoice)
    }

    private func open() async {
        failed = false
        guard await WebCallPeerFactory.requestAccess(for: .video) else {
            failed = true
            return
        }
        let watchdog = Task { @MainActor in
            try? await Task.sleep(nanoseconds: Self.openTimeout)
            guard !Task.isCancelled, peer == nil else { return }
            factory.endPreview()
        }
        defer { watchdog.cancel() }
        do {
            let image = engine.background == DirectCallEngine.customBackground ? engine.backgroundImage : ""
            let faceImage = engine.face == DirectCallEngine.customFace ? engine.faceImage : ""
            peer = try await factory.preview(
                face: engine.face, background: engine.background, image: image, faceImage: faceImage)
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
                try await peer.startRecording(
                    face: engine.face, background: engine.background, voice: engine.effectiveVoice)
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
            guard let url = MessagesViewOnceFiles.write(made.data, ext: made.mime == "video/mp4" ? "mp4" : "webm") else {
                throw WebCallError.failed("recording_write")
            }
            record = .review(url, min(ViewOnceKind.video.maxDurationMs, max(0, made.durationMs)), made.mime)
        } catch {
            record = .idle
            notice = "The recording could not be saved. Try again."
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
            return "This phone cannot record from the preview."
        default:
            return "Recording could not start."
        }
    }

    private func send(_ url: URL, _ millis: Int64, _ mime: String) {
        let chosen = captions.url(for: url)
        let sentMime = chosen == url ? mime : "video/mp4"
        guard let data = try? Data(contentsOf: chosen), !data.isEmpty else {
            notice = "The recording could not be read."
            discardReview()
            record = .idle
            return
        }
        record = .sending(0)
        Task { @MainActor in
            let sent = await hub.sendViewOnce(
                kind: .video, mime: sentMime, data: data, durationMs: millis,
                progress: { (value: Double) in record = .sending(value) })
            if sent {
                FmHaptics.send()
                MessagesViewOnceFiles.drop(url)
                captions.reset()
                record = .idle
                close()
            } else {
                FmHaptics.failure()
                record = .review(url, millis, mime)
                notice = hub.notice?.text ?? "The message could not be sent."
            }
        }
    }

    private func discardReview() {
        if case .review(let url, _, _) = record { MessagesViewOnceFiles.drop(url) }
        captions.reset()
    }

    private func close() {
        discardReview()
        factory.endPreview()
        onClose()
    }
}

struct DirectCallPreviewButton: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let engine: DirectCallEngine
    let dark: Bool
    @State private var showing = false
    @State private var startAfter = false

    var body: some View {
        Button(action: { showing = true }) {
            Image(systemName: "eye")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(dark ? Color.white : Color.black)
                .frame(width: 44, height: 44)
                .chatGlass(Circle(), border: fm.border)
        }
        .buttonStyle(.plain)
        .disabled(engine.active)
        .accessibilityLabel("Preview camera, face and background")
        .accessibilityIdentifier("chat-call-preview-open")
        .fullScreenCover(
            isPresented: $showing,
            onDismiss: {
                guard startAfter else { return }
                startAfter = false
                DirectCallActions.start(.video, hub: hub)
            }
        ) {
            if let factory = engine.peerFactory as? WebCallPeerFactory {
                DirectCallPreviewScreen(
                    hub: hub, engine: engine, factory: factory,
                    onStart: {
                        startAfter = true
                        showing = false
                    },
                    onClose: { showing = false })
            }
        }
    }
}
