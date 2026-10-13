// The prompt attachment preparation rules that need no image codec, so they build and test on Linux: the JPEG metadata
// walker, the downscale arithmetic, the attachment's name and id, and how an
// attachment is referenced from a prompt. The decode/resize/encode itself is
// ImageIO in the app target (Platform/PromptPhotoPicker.swift), which calls
// `PromptPhotoRules.finish` on its output.

import FMApi
import FMBase
import Foundation

public enum PromptPhotoRules {
    /// The size a photo of `width` x `height` is drawn at: its longest side
    /// at most `PromptAttachmentLimits.maxDimension`, never upscaled.
    public static func targetSize(width: Int, height: Int) -> (width: Int, height: Int) {
        let longest = max(width, height)
        let limit = PromptAttachmentLimits.maxDimension
        if longest <= limit || longest <= 0 { return (max(1, width), max(1, height)) }
        let scale = Double(limit) / Double(longest)
        return (max(1, Int((Double(width) * scale).rounded())), max(1, Int((Double(height) * scale).rounded())))
    }

    /// The display name an attachment is sent under: the picked file's base
    /// name with `.jpg`, or `photo.jpg`.
    public static func attachmentName(_ picked: String) -> String {
        var base = picked.dartTrimmed
        if let dot = base.range(of: #"\.[A-Za-z0-9]+$"#, options: .regularExpression) {
            base.removeSubrange(dot)
        }
        return "\(base.isEmpty ? "photo" : base).jpg"
    }

    nonisolated(unsafe) private static var counter = 0
    private static let counterLock = NSLock()

    /// A fresh attachment id: microseconds and a counter, both base 36.
    public static func newId(now: Date = Date()) -> String {
        counterLock.lock()
        let n = counter
        counter += 1
        counterLock.unlock()
        let micros = Int64((now.timeIntervalSince1970 * 1_000_000).rounded())
        return String(micros, radix: 36) + String(n, radix: 36)
    }

    /// The last step of preparing a photo: strip whatever metadata the
    /// encoder wrote, check the size, and wrap it.
    public static func finish(jpeg: Data, pickedName: String, width: Int, height: Int) throws -> PromptAttachment {
        let bytes = stripJpegMetadata(jpeg)
        if bytes.count > PromptAttachmentLimits.maxBytes {
            throw PromptAttachmentError("That photo is still larger than 5 MB after compressing it.")
        }
        return PromptAttachment(
            id: newId(), name: attachmentName(pickedName), bytes: bytes, mediaType: "image/jpeg", width: width,
            height: height)
    }
}

/// Remove every APPn (except the JFIF APP0 header) and COM segment from a
/// JPEG. Belt and braces over the encoder: whatever wrote the file, no EXIF,
/// GPS, XMP, ICC profile or comment leaves the device. Anything this walker
/// does not understand is returned unchanged rather than corrupted.
public func stripJpegMetadata(_ data: Data) -> Data {
    let jpeg = [UInt8](data)
    if jpeg.count < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 { return data }
    var out: [UInt8] = [0xFF, 0xD8]
    out.reserveCapacity(jpeg.count)
    var i = 2
    while i + 4 <= jpeg.count {
        if jpeg[i] != 0xFF { break }
        let marker = jpeg[i + 1]
        if marker == 0xFF {
            i += 1
            continue
        }
        // Start of scan: the entropy-coded data and everything after it is image.
        if marker == 0xDA {
            out.append(contentsOf: jpeg[i...])
            return Data(out)
        }
        let length = (Int(jpeg[i + 2]) << 8) | Int(jpeg[i + 3])
        let end = i + 2 + length
        if length < 2 || end > jpeg.count { break }
        let isJfif = marker == 0xE0 && length >= 7 && i + 7 < jpeg.count
            && jpeg[i + 4] == 0x4A && jpeg[i + 5] == 0x46 && jpeg[i + 6] == 0x49 && jpeg[i + 7] == 0x46
        let isMetadata = (marker >= 0xE0 && marker <= 0xEF && !isJfif) || marker == 0xFE
        if !isMetadata { out.append(contentsOf: jpeg[i..<end]) }
        i = end
    }
    return data
}

/// The text appended to a prompt that names attachments written into the
/// agent's working directory. The Go daemon writes the same block for queued
/// jobs, so an agent sees one format wherever it runs.
public func attachmentPromptBlock(_ paths: [String]) -> String {
    if paths.isEmpty { return "" }
    var text = "\n\nAttached images (open them with your file-reading tool):"
    for path in paths { text += "\n- \(path)" }
    return text
}

/// `prompt` with `paths` referenced after it.
public func promptWithAttachmentPaths(_ prompt: String, _ paths: [String]) -> String {
    prompt + attachmentPromptBlock(paths)
}

/// Where an attachment lands inside a working tree: `.forkmesh/attachments/`
/// under a name the app chose, never one taken from the picked file.
public func attachmentRelativePath(_ attachment: PromptAttachment) -> String {
    let safe = String(attachment.id.unicodeScalars.filter {
        ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "_" || $0 == "-"
    }.map(Character.init))
    let ext: String
    switch attachment.mediaType {
    case "image/png": ext = "png"
    case "image/webp": ext = "webp"
    case "image/gif": ext = "gif"
    default: ext = "jpg"
    }
    return ".forkmesh/attachments/\(safe.isEmpty ? "image" : safe).\(ext)"
}
