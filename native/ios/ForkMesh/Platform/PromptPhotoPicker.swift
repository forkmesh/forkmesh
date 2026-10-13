// The platform half of the composer's photo attachments: picking
// photos (the library through PhotosPicker, the camera through
// UIImagePickerController, image files through fileImporter) and preparing
// them with ImageIO. A picked photo is decoded, its EXIF orientation baked
// into the pixels, shrunk to fit 2048 px, flattened onto white, re-encoded as
// JPEG q80 without any metadata dictionary, and then run through FMAgents'
// `stripJpegMetadata` (belt and braces), so no location, camera or ICC data
// leaves the phone. The rules are FMAgents' PromptPhotoRules, tested on Linux.

import FMAgents
import FMApi
import Foundation
import ImageIO
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// Where the "+" menu takes a photo from.
enum PromptAttachmentSource {
    case library, camera, file
}

enum PromptPhotoPreparer {
    /// Prepare one picked photo. Runs off the main actor: a camera photo takes
    /// a noticeable moment to decode and re-encode.
    static func prepare(_ data: Data, name: String) async throws -> PromptAttachment {
        return try await Task.detached(priority: .userInitiated) {
            try prepareNow(data, name: name)
        }.value
    }

    static func prepareNow(_ data: Data, name: String) throws -> PromptAttachment {
        let unreadable = PromptAttachmentError("That file is not a photo this app can read. Try a JPEG or PNG.")
        let sourceOptions: CFDictionary = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions),
            CGImageSourceGetCount(source) > 0
        else {
            throw unreadable
        }
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any] ?? [:]
        var width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue ?? 0
        var height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue ?? 0
        // Orientations 5-8 are rotated a quarter turn: the drawn image is the
        // other way round.
        let orientation = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        if orientation >= 5 && orientation <= 8 {
            swap(&width, &height)
        }
        let target = PromptPhotoRules.targetSize(width: width, height: height)
        let thumbnailOptions: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            // Bakes the EXIF orientation in; the metadata is about to go.
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: max(target.width, target.height),
        ] as CFDictionary
        guard let decoded = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions) else {
            throw unreadable
        }
        let flat = flattenOntoWhite(decoded) ?? decoded
        let out = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(out as CFMutableData, UTType.jpeg.identifier as CFString, 1, nil) else {
            throw unreadable
        }
        // Only the quality goes in: no properties dictionary means no EXIF,
        // GPS, TIFF or IPTC block is written.
        let encodeOptions: CFDictionary = [
            kCGImageDestinationLossyCompressionQuality: Double(PromptAttachmentLimits.jpegQuality) / 100
        ] as CFDictionary
        CGImageDestinationAddImage(destination, flat, encodeOptions)
        if !CGImageDestinationFinalize(destination) {
            throw unreadable
        }
        return try PromptPhotoRules.finish(jpeg: out as Data, pickedName: name, width: flat.width, height: flat.height)
    }

    /// JPEG has no alpha: draw onto white rather than let transparent pixels
    /// turn black. Opaque images are returned as they are.
    private static func flattenOntoWhite(_ image: CGImage) -> CGImage? {
        switch image.alphaInfo {
        case .none, .noneSkipFirst, .noneSkipLast:
            return image
        default:
            break
        }
        let space = CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
        guard let context = CGContext(
            data: nil, width: image.width, height: image.height, bitsPerComponent: 8, bytesPerRow: 0, space: space,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else {
            return nil
        }
        let rect = CGRect(x: 0, y: 0, width: image.width, height: image.height)
        context.setFillColor(UIColor.white.cgColor)
        context.fill(rect)
        context.draw(image, in: rect)
        return context.makeImage()
    }
}

/// Loads PhotosPicker selections as raw bytes (HEIC included; ImageIO reads it).
enum PromptLibraryLoader {
    static func load(_ items: [PhotosPickerItem]) async -> [(String, Data)] {
        var picked: [(String, Data)] = []
        for item in items {
            if let data = try? await item.loadTransferable(type: Data.self) {
                picked.append(("photo", data))
            }
        }
        return picked
    }
}

/// Image files from the Files app, read under their security scope.
enum PromptFileLoader {
    static func load(_ urls: [URL]) -> [(String, Data)] {
        var picked: [(String, Data)] = []
        for url in urls {
            let scoped = url.startAccessingSecurityScopedResource()
            defer {
                if scoped { url.stopAccessingSecurityScopedResource() }
            }
            if let data = try? Data(contentsOf: url) {
                picked.append((url.lastPathComponent, data))
            }
        }
        return picked
    }
}

enum PromptTextFileLoader {
    static func load(_ urls: [URL]) async -> [Result<PromptTextFile, PromptAttachmentError>] {
        await Task.detached(priority: .userInitiated) {
            urls.map { (url: URL) -> Result<PromptTextFile, PromptAttachmentError> in
                let scoped = url.startAccessingSecurityScopedResource()
                defer {
                    if scoped { url.stopAccessingSecurityScopedResource() }
                }
                let name: String = url.lastPathComponent
                if let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize, size > PromptTextFiles.maxBytes {
                    return .failure(PromptTextFiles.tooLarge(name))
                }
                guard let data = try? Data(contentsOf: url) else {
                    return .failure(PromptAttachmentError("\(name) could not be read from this device."))
                }
                do {
                    return .success(try PromptTextFiles.read(name: name, bytes: data))
                } catch let error as PromptAttachmentError {
                    return .failure(error)
                } catch {
                    return .failure(PromptAttachmentError(error.localizedDescription))
                }
            }
        }.value
    }
}

/// The camera, as a full-screen cover. Hands back one JPEG (the platform's
/// first encode; `PromptPhotoPreparer` makes the exact one), or nothing.
struct PromptCameraPicker: UIViewControllerRepresentable {
    let onPicked: (Data?) -> Void

    static var isAvailable: Bool { UIImagePickerController.isSourceTypeAvailable(.camera) }

    func makeCoordinator() -> Coordinator { Coordinator(onPicked: onPicked) }

    func makeUIViewController(context: Context) -> UIImagePickerController {
        let picker = UIImagePickerController()
        picker.sourceType = .camera
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ controller: UIImagePickerController, context: Context) {}

    final class Coordinator: NSObject, UIImagePickerControllerDelegate, UINavigationControllerDelegate {
        let onPicked: (Data?) -> Void

        init(onPicked: @escaping (Data?) -> Void) { self.onPicked = onPicked }

        func imagePickerController(
            _ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey: Any]
        ) {
            let image = info[.originalImage] as? UIImage
            onPicked(image?.jpegData(compressionQuality: 0.95))
        }

        func imagePickerControllerDidCancel(_ picker: UIImagePickerController) {
            onPicked(nil)
        }
    }
}
