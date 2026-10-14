import FMAccount
import PhotosUI
import SwiftUI
import UIKit

struct AppearanceBackgroundRow: View {
    @Environment(\.fmPalette) private var fm
    let settings: SettingsService
    @State private var wallpaper: FmWallpaper = FmWallpaper.shared
    @State private var pickerItem: PhotosPickerItem?
    @State private var saving: Bool = false
    @State private var error: String?
    @State private var dim: Double = SettingsService.defaultBackgroundDim

    private var name: String { settings.backgroundImage }

    private var thumbnail: some View {
        RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous)
            .fill(fm.bgOverlay)
            .overlay {
                if let image = wallpaper.image(for: name) {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                } else if saving {
                    FmCircularProgressIndicator(size: 16, strokeWidth: 2)
                } else {
                    FmIcon("image_outlined", size: 22, color: fm.textTertiary)
                }
            }
            .frame(width: 48, height: 84)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous).strokeBorder(fm.border, lineWidth: 1)
            }
            .accessibilityHidden(true)
    }

    private var dimSlider: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack {
                Text("Dim").fmText(.bodyMedium)
                Spacer()
                Text("\(Int((dim * 100).rounded()))%").fmText(.bodySmall, color: fm.textSecondary, tabular: true)
            }
            Slider(
                value: $dim, in: 0...SettingsService.maxBackgroundDim, step: 0.05,
                onEditingChanged: { (sliding: Bool) in
                    if sliding {
                        wallpaper.previewDim = dim
                    } else {
                        settings.setBackgroundDim(dim)
                        wallpaper.previewDim = nil
                    }
                }
            )
            .tint(fm.accent)
            .onChange(of: dim) { (_: Double, value: Double) in
                if wallpaper.previewDim != nil {
                    wallpaper.previewDim = value
                } else if value != settings.backgroundDim {
                    settings.setBackgroundDim(value)
                }
            }
            .accessibilityLabel(Text("Background dim"))
            .accessibilityValue(Text("\(Int((dim * 100).rounded())) percent"))
            .accessibilityIdentifier("appearance-background-dim")
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x3) {
                thumbnail
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    PhotosPicker(selection: $pickerItem, matching: .images) {
                        Text(name.isEmpty ? "Choose image…" : "Change image")
                            .fmText(.labelLarge, color: fm.accent)
                            .frame(minHeight: 44)
                    }
                    .disabled(saving)
                    .accessibilityIdentifier("appearance-background-choose")
                    if !name.isEmpty {
                        Button {
                            remove()
                        } label: {
                            Text("Remove").fmText(.labelLarge, color: fm.textSecondary).frame(minHeight: 44)
                        }
                        .buttonStyle(.plain)
                        .disabled(saving)
                        .accessibilityIdentifier("appearance-background-remove")
                    }
                }
                Spacer(minLength: 0)
            }
            if let error {
                Text(error).fmText(.bodySmall, color: fm.danger)
            }
            if !name.isEmpty {
                dimSlider
            }
            SettingsNote(text: "Shown behind every screen on this phone. Dim keeps text readable over a busy picture.")
        }
        .onAppear { dim = settings.backgroundDim }
        .onChange(of: pickerItem) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            guard let item else { return }
            Task { await pick(item) }
        }
    }

    private func pick(_ item: PhotosPickerItem) async {
        error = nil
        saving = true
        defer {
            saving = false
            pickerItem = nil
        }
        guard let data = try? await item.loadTransferable(type: Data.self),
            let saved = await FmWallpaperStore.save(data)
        else {
            error = "That image could not be read. Try a PNG, JPEG or HEIC."
            return
        }
        let previous: String = name
        wallpaper.adopt(saved.name, image: saved.image)
        settings.setBackgroundImage(saved.name)
        if !previous.isEmpty && previous != saved.name {
            FmWallpaperStore.delete(previous)
        }
        FmHaptics.selection()
    }

    private func remove() {
        let previous: String = name
        error = nil
        settings.setBackgroundImage("")
        Task { await wallpaper.load("") }
        FmWallpaperStore.delete(previous)
        FmHaptics.selection()
    }
}
