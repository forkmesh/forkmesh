import FMBase
import Foundation
import ImageIO
import Observation
import SwiftUI
import UIKit

enum FmWallpaperStore {
    static let maxPixels: Int = 2800

    static var directory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("wallpapers", isDirectory: true)
    }

    static func url(_ name: String) -> URL? {
        guard !name.isEmpty, !name.contains("/"), let directory else { return nil }
        return directory.appendingPathComponent(name)
    }

    static func save(_ data: Data) async -> (name: String, image: UIImage)? {
        await Task.detached(priority: .userInitiated) { saveNow(data) }.value
    }

    static func saveNow(_ data: Data) -> (name: String, image: UIImage)? {
        guard let image = shrink(data), let jpeg = image.jpegData(compressionQuality: 0.88), let directory else {
            return nil
        }
        let name: String = "wallpaper-\(Date().epochMillis).jpg"
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try jpeg.write(to: directory.appendingPathComponent(name), options: .atomic)
        } catch {
            Log.warning("appearance", "could not save the background image: \(error)")
            return nil
        }
        return (name: name, image: image.preparingForDisplay() ?? image)
    }

    static func delete(_ name: String) {
        guard let url = url(name) else { return }
        _ = unlink(url.path)
    }

    static func shrink(_ data: Data) -> UIImage? {
        let sourceOptions: CFDictionary = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions),
            CGImageSourceGetCount(source) > 0
        else { return nil }
        let options: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
        ] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options) else { return nil }
        return UIImage(cgImage: image)
    }
}

@MainActor
@Observable
final class FmWallpaper {
    static let shared: FmWallpaper = FmWallpaper()

    private(set) var name: String = ""
    private(set) var image: UIImage?
    var previewDim: Double?

    func image(for name: String) -> UIImage? {
        name.isEmpty || name != self.name ? nil : image
    }

    func adopt(_ name: String, image: UIImage) {
        self.name = name
        self.image = image
    }

    func load(_ name: String) async {
        if name.isEmpty {
            self.name = ""
            image = nil
            return
        }
        if name == self.name && image != nil { return }
        guard let url = FmWallpaperStore.url(name) else { return }
        let loaded: UIImage? = await Task.detached(priority: .userInitiated) {
            (try? Data(contentsOf: url)).flatMap { UIImage(data: $0) }.map { $0.preparingForDisplay() ?? $0 }
        }.value
        guard let loaded else {
            Log.warning("appearance", "the background image \(name) could not be read")
            return
        }
        self.name = name
        image = loaded
    }
}

struct FmWallpaperBackdrop: View {
    @Environment(\.fmPalette) private var fm
    let name: String
    let dim: Double
    @State private var wallpaper: FmWallpaper = FmWallpaper.shared

    var body: some View {
        ZStack {
            fm.bgBase
            if let image = wallpaper.image(for: name) {
                GeometryReader { (proxy: GeometryProxy) in
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                        .frame(width: proxy.size.width, height: proxy.size.height)
                        .clipped()
                }
                fm.bgBase.opacity(wallpaper.previewDim ?? dim)
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .task(id: name) { await wallpaper.load(name) }
    }
}

extension View {
    @ViewBuilder func fmNavigationCanvas(_ color: Color) -> some View {
        if #available(iOS 18.0, *) {
            containerBackground(color, for: .navigation)
        } else {
            self
        }
    }
}
