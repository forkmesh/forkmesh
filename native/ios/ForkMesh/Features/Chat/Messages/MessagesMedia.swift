import AVFoundation
import FMApi
import FMBase
import FMChat
import PhotosUI
import SwiftUI
import UIKit

enum MessagesImages {
    static func jpeg(_ image: UIImage, maxSide: CGFloat, maxBytes: Int) -> Data? {
        var side: CGFloat = maxSide
        while side >= 320 {
            let scaled: UIImage = resized(image, maxSide: side)
            for quality in [0.82, 0.7, 0.58, 0.46, 0.34] as [CGFloat] {
                if let data = scaled.jpegData(compressionQuality: quality), data.count <= maxBytes { return data }
            }
            side *= 0.75
        }
        return nil
    }

    static func jpeg(_ data: Data, maxSide: CGFloat, maxBytes: Int) -> Data? {
        guard let image = UIImage(data: data) else { return nil }
        return jpeg(image, maxSide: maxSide, maxBytes: maxBytes)
    }

    static func resized(_ image: UIImage, maxSide: CGFloat) -> UIImage {
        let longest: CGFloat = max(image.size.width, image.size.height)
        let scale: CGFloat = longest > maxSide ? maxSide / longest : 1
        let size: CGSize = CGSize(width: floor(image.size.width * scale), height: floor(image.size.height * scale))
        let format: UIGraphicsImageRendererFormat = UIGraphicsImageRendererFormat.default()
        format.scale = 1
        format.opaque = true
        return UIGraphicsImageRenderer(size: size, format: format).image { _ in
            image.draw(in: CGRect(origin: .zero, size: size))
        }
    }

    static func photoName() -> String {
        "IMG_\(Int(Date().timeIntervalSince1970)).jpg"
    }
}

struct MessagesBackdrop: View {
    let look: ChatLook
    let canvas: Color

    static func isDark(_ look: ChatLook) -> Bool? {
        if look.backgroundImage != nil { return true }
        return ChatBackgroundCatalog.preset(look.backgroundPreset)?.dark
    }

    var body: some View {
        ZStack {
            canvas
            if let data = look.backgroundImage {
                if let picture = UIImage(data: data) {
                    GeometryReader { (geometry: GeometryProxy) in
                        Image(uiImage: picture)
                            .resizable()
                            .scaledToFill()
                            .frame(width: geometry.size.width, height: geometry.size.height)
                            .clipped()
                    }
                    Color.black.opacity(0.18)
                }
            } else if let preset = ChatBackgroundCatalog.preset(look.backgroundPreset) {
                MessagesPresetSwatch(preset: preset)
            }
        }
        .accessibilityHidden(true)
    }
}

struct MessagesPresetSwatch: View {
    let preset: ChatBackgroundPreset

    var body: some View {
        LinearGradient(
            colors: preset.colors.map { Color(fmARGB: 0xFF00_0000 | $0) }, startPoint: .top, endPoint: .bottom)
    }
}

struct MessagesCameraPicker: UIViewControllerRepresentable {
    let onImage: (UIImage) -> Void
    let onClose: () -> Void

    static var available: Bool { UIImagePickerController.isSourceTypeAvailable(.camera) }

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker: UIImagePickerController = UIImagePickerController()
        picker.sourceType = .camera
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIImagePickerController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let parent: MessagesCameraPicker

        init(_ parent: MessagesCameraPicker) { self.parent = parent }

        func imagePickerController(
            _ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]
        ) {
            if let image = info[.originalImage] as? UIImage { parent.onImage(image) }
            parent.onClose()
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) { parent.onClose() }
    }
}

struct MessagesPhotoBox: Identifiable {
    let attachment: ChatAttachment
    var id: String { attachment.messageId }
}

struct MessagesPhotoViewer: View {
    @Environment(\.dismiss) private var dismiss
    let attachment: ChatAttachment
    @State private var zoom: CGFloat = 1

    var body: some View {
        NavigationStack {
            ZStack {
                Color.black.ignoresSafeArea()
                if let picture = UIImage(data: attachment.data) {
                    Image(uiImage: picture)
                        .resizable()
                        .scaledToFit()
                        .scaleEffect(zoom)
                        .gesture(MagnifyGesture().onChanged { zoom = max(1, min(5, $0.magnification)) }.onEnded { _ in
                            withAnimation(.spring()) { if zoom < 1.2 { zoom = 1 } }
                        })
                        .onTapGesture(count: 2) { withAnimation(.spring()) { zoom = zoom > 1 ? 1 : 2.5 } }
                        .accessibilityLabel("Photo")
                }
            }
            .toolbarBackground(.hidden, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(action: { dismiss() }) { Image(systemName: "xmark").foregroundStyle(Color.white) }
                        .accessibilityLabel("Close")
                }
                ToolbarItem(placement: .primaryAction) {
                    if let picture = UIImage(data: attachment.data) {
                        ShareLink(item: Image(uiImage: picture), preview: SharePreview(attachment.fileName, image: Image(uiImage: picture))) {
                            Image(systemName: "square.and.arrow.up").foregroundStyle(Color.white)
                        }
                    }
                }
            }
        }
    }
}

@MainActor
@Observable
final class MessagesVoiceRecorder: NSObject, AVAudioRecorderDelegate {
    static let maxSeconds: TimeInterval = 120
    private(set) static var isRecording: Bool = false
    private(set) var recording: Bool = false
    private(set) var startedAt: Date?
    @ObservationIgnored private var recorder: AVAudioRecorder?
    @ObservationIgnored private var limit: Task<Void, Never>?
    @ObservationIgnored var onFinished: (@MainActor (Data) -> Void)?
    @ObservationIgnored var onRefused: (@MainActor () -> Void)?

    private var fileURL: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("messages-voice.m4a")
    }

    func start() {
        Task { @MainActor in
            guard await AVAudioApplication.requestRecordPermission() else {
                onRefused?()
                return
            }
            begin()
        }
    }

    private func begin() {
        PromptDictation.yieldOverhearing()
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
            try session.setActive(true)
            let settings: [String: Any] = [
                AVFormatIDKey: Int(kAudioFormatMPEG4AAC), AVSampleRateKey: 22_050, AVNumberOfChannelsKey: 1,
                AVEncoderBitRateKey: 32_000,
            ]
            try? FileManager.default.removeItem(at: fileURL)
            let made: AVAudioRecorder = try AVAudioRecorder(url: fileURL, settings: settings)
            made.delegate = self
            guard made.record(forDuration: MessagesVoiceRecorder.maxSeconds) else { return }
            recorder = made
            recording = true
            Self.isRecording = true
            startedAt = Date()
            FmHaptics.tap()
        } catch {
            recording = false
        }
    }

    func finish(send: Bool) {
        guard let recorder else { return }
        let keep: (@MainActor (Data) -> Void)? = send ? onFinished : nil
        recorder.stop()
        self.recorder = nil
        recording = false
        Self.isRecording = false
        startedAt = nil
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        if let keep, let data = try? Data(contentsOf: fileURL), data.count > 1_000 { keep(data) }
        try? FileManager.default.removeItem(at: fileURL)
    }

    nonisolated func audioRecorderDidFinishRecording(_ recorder: AVAudioRecorder, successfully flag: Bool) {
        Task { @MainActor in
            if self.recording { self.finish(send: true) }
        }
    }
}

struct MessagesAudioBubble: View {
    let attachment: ChatAttachment
    let tint: Color
    @State private var player: AVAudioPlayer?
    @State private var playing: Bool = false
    @State private var duration: TimeInterval = 0

    var body: some View {
        HStack(spacing: 10) {
            Button(action: toggle) {
                Image(systemName: playing ? "pause.fill" : "play.fill")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(tint)
                    .frame(width: 32, height: 32)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(playing ? "Pause voice message" : "Play voice message")
            HStack(spacing: 2) {
                ForEach(0..<24, id: \.self) { (index: Int) in
                    Capsule()
                        .fill(tint.opacity(0.75))
                        .frame(width: 2.5, height: MessagesAudioBubble.bar(index, seed: attachment.messageId))
                }
            }
            .accessibilityHidden(true)
            Text(MessagesAudioBubble.clock(duration))
                .font(.system(size: 13).monospacedDigit())
                .foregroundStyle(tint)
        }
        .task(id: attachment.messageId) { duration = (try? AVAudioPlayer(data: attachment.data))?.duration ?? 0 }
        .onDisappear { player?.stop() }
    }

    static func bar(_ index: Int, seed: String) -> CGFloat {
        var hash: UInt64 = 1_469_598_103_934_665_603
        for byte in "\(seed)-\(index)".utf8 { hash = (hash ^ UInt64(byte)) &* 1_099_511_628_211 }
        return 6 + CGFloat(hash % 18)
    }

    static func clock(_ seconds: TimeInterval) -> String {
        let total: Int = Int(seconds.rounded())
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    private func toggle() {
        if playing {
            player?.pause()
            playing = false
            return
        }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
            try AVAudioSession.sharedInstance().setActive(true)
            let made: AVAudioPlayer = try player ?? AVAudioPlayer(data: attachment.data)
            player = made
            made.play()
            playing = true
            let length: TimeInterval = max(0.5, made.duration - made.currentTime)
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(length * 1_000_000_000))
                if player?.isPlaying != true { playing = false }
            }
        } catch {
            playing = false
        }
    }
}

struct MessagesBackgroundPicker: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    @State private var item: PhotosPickerItem?
    @State private var working: Bool = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x4) {
                    Text(
                        hub.inDirect
                            ? "You both see the background you choose. It travels end-to-end encrypted."
                            : "Everyone in this conversation sees the background you choose.")
                        .fmText(.bodyMedium, color: fm.textSecondary)
                    PhotosPicker(selection: $item, matching: .images) {
                        Label("Choose a Photo", systemImage: "photo.on.rectangle")
                            .frame(maxWidth: .infinity, minHeight: 48)
                            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(fm.bgOverlay))
                    }
                    .buttonStyle(.plain)
                    .disabled(working)
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 12)], spacing: 12) {
                        tile(label: "None", chosen: !hub.conversationLook.hasBackground) {
                            RoundedRectangle(cornerRadius: 16, style: .continuous).fill(fm.bgBase)
                                .overlay(Image(systemName: "nosign").foregroundStyle(fm.textTertiary))
                        } action: {
                            apply(ChatLookChange(field: .background))
                        }
                        ForEach(ChatBackgroundCatalog.presets) { (preset: ChatBackgroundPreset) in
                            tile(label: preset.label, chosen: hub.conversationLook.backgroundPreset == preset.id) {
                                MessagesPresetSwatch(preset: preset)
                                    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                            } action: {
                                apply(ChatLookChange(field: .background, preset: preset.id))
                            }
                        }
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgRaised)
            .navigationTitle("Background")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .overlay { if working { ProgressView() } }
            .onChange(of: item) { _, picked in
                guard let picked else { return }
                Task { await usePhoto(picked) }
            }
        }
    }

    private func tile<Swatch: View>(
        label: String, chosen: Bool, @ViewBuilder swatch: () -> Swatch, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                swatch()
                    .frame(height: 150)
                    .overlay(
                        RoundedRectangle(cornerRadius: 16, style: .continuous)
                            .stroke(chosen ? MessagesColors.blue : fm.border, lineWidth: chosen ? 3 : 1))
                Text(label).fmText(.labelMedium, color: fm.textPrimary)
            }
        }
        .buttonStyle(.plain)
        .disabled(working)
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }

    private func apply(_ change: ChatLookChange) {
        working = true
        Task { @MainActor in
            let sent: Bool = await hub.setLook(change)
            working = false
            if sent { dismiss() }
        }
    }

    private func usePhoto(_ picked: PhotosPickerItem) async {
        defer { item = nil }
        guard let data = try? await picked.loadTransferable(type: Data.self) else { return }
        guard let jpeg = MessagesImages.jpeg(data, maxSide: 1_400, maxBytes: ChatLookFrames.maxImageBytes) else {
            hub.post("That photo could not be used as a background.")
            return
        }
        apply(ChatLookChange(field: .background, image: jpeg))
    }
}
