import FMBase
import FMChat
import Foundation
import ImageIO
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

enum CallBackgroundStore {
    private static var file: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("ForkMesh", isDirectory: true)
            .appendingPathComponent("call-background.jpg")
    }

    static func load() -> String {
        guard let file, let data = try? Data(contentsOf: file), !data.isEmpty else { return "" }
        return "data:image/jpeg;base64," + data.base64EncodedString()
    }

    static func save(_ image: String) {
        guard let file, let comma = image.firstIndex(of: ","),
            let data = Data(base64Encoded: String(image[image.index(after: comma)...]))
        else { return }
        try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: file, options: .atomic)
    }
}

enum CallBackgroundPhoto {
    static let side = 1920

    static func dataURL(from data: Data) async -> String? {
        await Task.detached(priority: .userInitiated) { prepare(data) }.value
    }

    private static func prepare(_ data: Data) -> String? {
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

struct DirectCallPreviewScreen: View {
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory
    let onStart: () -> Void
    let onClose: () -> Void
    @State private var peer: WebCallPeer?
    @State private var failed = false
    @State private var showingBackgrounds = false
    @State private var hearing = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let peer {
                WebCallSurface(webView: peer.webView).ignoresSafeArea()
            } else if failed {
                Text("The camera could not start.")
                    .foregroundStyle(.white)
            } else {
                ProgressView().tint(.white)
            }
            VStack {
                VStack(spacing: 4) {
                    Text("Preview")
                        .font(.system(size: 17, weight: .semibold))
                    Text("Only you can see this. The other person will see exactly this picture.")
                        .font(.system(size: 13))
                        .multilineTextAlignment(.center)
                        .opacity(0.85)
                }
                .foregroundStyle(.white)
                .shadow(radius: 4)
                .padding(.top, FmSpace.x4)
                .padding(.horizontal, FmSpace.x4)
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
        .task { await open() }
        .accessibilityIdentifier("chat-call-preview")
    }

    private var controls: some View {
        VStack(spacing: FmSpace.x4) {
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
            HStack(spacing: FmSpace.x4) {
                Button(action: close) {
                    Text("Close")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 22)
                        .frame(height: 50)
                        .background(Capsule().fill(Color.white.opacity(0.18)))
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("chat-call-preview-close")
                Button(action: onStart) {
                    Label("Start video call", systemImage: "video.fill")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 22)
                        .frame(height: 50)
                        .background(Capsule().fill(Color.green))
                }
                .buttonStyle(.plain)
                .disabled(peer == nil)
                .accessibilityIdentifier("chat-call-start")
            }
        }
    }

    private var faceMenu: some View {
        Menu {
            Picker("Face", selection: Binding(get: { engine.face }, set: { chooseFace($0) })) {
                Text("Your own face").tag("")
                ForEach(DirectCallEngine.faces) { (face: DirectCallFace) in
                    Text(face.label).tag(face.id)
                }
            }
        } label: {
            roundLabel("theatermasks.fill", on: !engine.face.isEmpty)
        }
        .accessibilityLabel("Face")
        .accessibilityValue(DirectCallEngine.faces.first { $0.id == engine.face }?.label ?? "Your own face")
        .accessibilityIdentifier("chat-call-face")
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
        peer?.setFace(engine.face)
        peer?.setVoice(engine.effectiveVoice)
    }

    private func chooseVoice(_ choice: String) {
        engine.setVoiceChoice(choice)
        peer?.setVoice(engine.effectiveVoice)
    }

    private func open() async {
        guard await WebCallPeerFactory.requestAccess(for: .video) else {
            failed = true
            return
        }
        do {
            let image = engine.background == DirectCallEngine.customBackground ? engine.backgroundImage : ""
            peer = try await factory.preview(face: engine.face, background: engine.background, image: image)
            if CallAudioRoute.headphonesConnected {
                hearing = await peer?.setMonitor(true) ?? false
            }
        } catch {
            failed = true
        }
    }

    private func close() {
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
                    engine: engine, factory: factory,
                    onStart: {
                        startAfter = true
                        showing = false
                    },
                    onClose: { showing = false })
            }
        }
    }
}
