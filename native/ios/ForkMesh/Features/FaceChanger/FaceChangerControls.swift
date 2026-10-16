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

@MainActor
final class FaceChangerStudio {
    static let shared = FaceChangerStudio()

    let look = FaceChangerLook()
    let factory = WebCallPeerFactory()

    private init() {
        factory.voice = { [look] in look.effectiveVoice }
        reload()
        look.addListener { [weak self] (part: FaceChangerPart) in self?.changed(part) }
    }

    func reload() {
        let prefs: KeyValueStore? = Services.get(KeyValueStore.self)
        look.onFaceChanged = nil
        look.onFaceImageChanged = nil
        look.onBackgroundChanged = nil
        look.onVoiceChanged = nil
        look.setFace(prefs?.string(forKey: FaceChangerLook.faceKey) ?? "", image: CallFaceStore.load())
        look.setBackground(prefs?.string(forKey: FaceChangerLook.backgroundKey) ?? "", image: CallBackgroundStore.load())
        look.setVoiceChoice(prefs?.string(forKey: FaceChangerLook.voiceKey) ?? FaceChangerLook.voiceMatchFace)
        look.onFaceChanged = { (face: String) in prefs?.set(face, forKey: FaceChangerLook.faceKey) }
        look.onFaceImageChanged = { (image: String) in CallFaceStore.save(image) }
        look.onBackgroundChanged = { (background: String, image: String) in
            prefs?.set(background, forKey: FaceChangerLook.backgroundKey)
            CallBackgroundStore.save(image)
        }
        look.onVoiceChanged = { (choice: String) in prefs?.set(choice, forKey: FaceChangerLook.voiceKey) }
    }

    private func changed(_ part: FaceChangerPart) {
        guard let peer = factory.current, peer.previewing else { return }
        switch part {
        case .face:
            if look.face == FaceChangerLook.customFace {
                peer.setFace(look.face, image: look.faceImage)
            } else {
                peer.setFace(look.face)
            }
            peer.setVoice(look.effectiveVoice)
        case .background:
            peer.setBackground(look.background, image: look.customBackgroundImage)
        case .voice:
            peer.setVoice(look.effectiveVoice)
        }
    }
}

struct FaceChangerRoundLabel: View {
    let symbol: String
    let on: Bool

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 20, weight: .semibold))
            .foregroundStyle(on ? Color.black : Color.white)
            .frame(width: 56, height: 56)
            .background(Circle().fill(on ? Color.white : Color.white.opacity(0.18)))
    }
}

struct FaceChangerFaceMenu: View {
    let look: FaceChangerLook
    let peer: WebCallPeer?
    @Binding var notice: String
    @State private var picking = false
    @State private var photo: PhotosPickerItem?
    @State private var working = false

    var body: some View {
        Menu {
            Picker("Face", selection: Binding(get: { look.face }, set: { look.setFace($0) })) {
                Text("Your own face").tag("")
                ForEach(FaceChangerLook.faces) { (face: FaceChangerOption) in
                    Text(face.label).tag(face.id)
                }
                if !look.faceImage.isEmpty {
                    Text(FaceChangerLook.faceLabel(FaceChangerLook.customFace)).tag(FaceChangerLook.customFace)
                }
            }
            Button {
                picking = true
            } label: {
                Label(look.faceImage.isEmpty ? "Use a photo of a face…" : "Choose another photo…", systemImage: "person.crop.square.badge.camera")
            }
            .accessibilityIdentifier("chat-call-face-photo")
        } label: {
            if working {
                ProgressView().tint(.white).frame(width: 56, height: 56)
            } else {
                FaceChangerRoundLabel(symbol: "theatermasks.fill", on: !look.face.isEmpty)
            }
        }
        .photosPicker(isPresented: $picking, selection: $photo, matching: .images)
        .onChange(of: photo) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            guard let item else { return }
            photo = nil
            Task { await use(item) }
        }
        .accessibilityLabel("Face")
        .accessibilityValue(FaceChangerLook.faceLabel(look.face))
        .accessibilityHint("Shows a different face that follows your head and lips")
        .accessibilityIdentifier("chat-call-face")
    }

    private func use(_ item: PhotosPickerItem) async {
        working = true
        defer { working = false }
        guard let data = try? await item.loadTransferable(type: Data.self),
            let image = await CallBackgroundPhoto.dataURL(from: data, side: CallBackgroundPhoto.faceSide)
        else {
            notice = "That photo could not be opened."
            return
        }
        let previous = look.face
        look.setFace(FaceChangerLook.customFace, image: image)
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
                look.setFace(previous == FaceChangerLook.customFace ? "" : previous)
                return
            }
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
    }
}

struct FaceChangerVoiceMenu: View {
    let look: FaceChangerLook
    let shown: String

    var body: some View {
        Menu {
            Picker("Voice", selection: Binding(get: { look.voiceChoice }, set: { look.setVoiceChoice($0) })) {
                Text("Match face").tag(FaceChangerLook.voiceMatchFace)
                Text("Your own voice").tag(FaceChangerLook.voiceOff)
                ForEach(FaceChangerLook.voices) { (voice: FaceChangerOption) in
                    Text(voice.label).tag(voice.id)
                }
            }
        } label: {
            FaceChangerRoundLabel(symbol: "waveform", on: !shown.isEmpty)
        }
        .accessibilityLabel("Voice")
        .accessibilityValue("Voice: \(FaceChangerLook.voiceLabel(shown))")
        .accessibilityHint("Changes how your voice sounds")
        .accessibilityIdentifier("chat-call-voice")
    }
}

struct FaceChangerBackgroundButton: View {
    let look: FaceChangerLook
    let peer: WebCallPeer?
    @State private var showing = false

    var body: some View {
        Button(action: { showing = true }) {
            FaceChangerRoundLabel(symbol: "photo.on.rectangle", on: !look.background.isEmpty)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Background")
        .accessibilityValue(FaceChangerLook.backgroundLabel(look.background))
        .accessibilityIdentifier("chat-call-background")
        .sheet(isPresented: $showing) {
            FaceChangerBackgroundSheet(look: look, peer: peer)
                .presentationDetents([.medium, .large])
        }
    }
}

struct FaceChangerBackgroundSheet: View {
    @Environment(\.dismiss) private var dismiss
    let look: FaceChangerLook
    let peer: WebCallPeer?
    @State private var thumbs: [String: UIImage] = [:]
    @State private var photo: PhotosPickerItem?
    @State private var working = false

    private let columns = [GridItem(.adaptive(minimum: 100), spacing: 10)]

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 12) {
                    ForEach(FaceChangerLook.backgrounds) { (background: FaceChangerOption) in
                        if background.id == FaceChangerLook.customBackground {
                            PhotosPicker(selection: $photo, matching: .images) { tile(background) }
                                .buttonStyle(.plain)
                                .accessibilityIdentifier("chat-call-background-custom")
                        } else {
                            Button(action: { look.setBackground(background.id) }) { tile(background) }
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
                look.setBackground(FaceChangerLook.customBackground, image: image)
            }
        }
    }

    private func tile(_ background: FaceChangerOption) -> some View {
        let selected = look.background == background.id
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
        if id == FaceChangerLook.customBackground {
            return look.backgroundImage.isEmpty ? nil : CallBackgroundPhoto.image(fromDataURL: look.backgroundImage)
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
}
