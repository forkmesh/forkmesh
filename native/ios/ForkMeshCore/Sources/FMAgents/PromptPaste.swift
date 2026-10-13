import FMApi
import Foundation

public struct PasteboardItem: Equatable, Sendable {
    public var representations: [String: Data]
    public var embeddedImages: [Data]

    public init(representations: [String: Data], embeddedImages: [Data] = []) {
        self.representations = representations
        self.embeddedImages = embeddedImages
    }
}

public struct PastedImage: Equatable, Sendable {
    public let name: String
    public let data: Data

    public init(name: String, data: Data) {
        self.name = name
        self.data = data
    }
}

public struct PromptPaste: Equatable, Sendable {
    public let images: [PastedImage]
    public let text: String

    public init(images: [PastedImage], text: String) {
        self.images = images
        self.text = text
    }
}

public enum PromptPasteRules {
    public static let imageTypes = [
        "public.png", "public.jpeg", "public.heic", "public.heif", "public.tiff", "com.compuserve.gif",
        "org.webmproject.webp", "com.microsoft.bmp",
    ]
    public static let rtfdTypes = ["com.apple.flat-rtfd", "public.rtf"]
    public static let htmlType = "public.html"
    public static let textTypes = ["public.utf8-plain-text", "public.plain-text", "public.text"]

    public static func wants(_ type: String) -> Bool {
        imageTypes.contains(type) || rtfdTypes.contains(type) || type == htmlType || textTypes.contains(type)
    }

    public static func paste(_ items: [PasteboardItem]) -> PromptPaste {
        var images = [PastedImage]()
        var texts = [String]()
        for item in items {
            if let type = imageTypes.first(where: { item.representations[$0].map(looksLikeImage) ?? false }),
                let data = item.representations[type]
            {
                images.append(PastedImage(name: "pasted-image-\(images.count + 1)", data: data))
                continue
            }
            var embedded = item.embeddedImages.filter(looksLikeImage)
            if embedded.isEmpty, let html = item.representations[htmlType] {
                embedded = dataUriImages(String(decoding: html, as: UTF8.self))
            }
            for data in embedded {
                images.append(PastedImage(name: "pasted-image-\(images.count + 1)", data: data))
            }
            if let text = plainText(item), !text.isEmpty { texts.append(text) }
        }
        return PromptPaste(images: images, text: texts.joined(separator: "\n"))
    }

    static func plainText(_ item: PasteboardItem) -> String? {
        guard let type = textTypes.first(where: { item.representations[$0] != nil }),
            let data = item.representations[type]
        else { return nil }
        let text = String(decoding: data, as: UTF8.self)
        return String(text.unicodeScalars.filter { $0 != "\u{FFFC}" }.map(Character.init))
    }

    public static func dataUriImages(_ html: String) -> [Data] {
        var out = [Data]()
        var rest = Substring(html)
        while let start = rest.range(of: "data:image/", options: .caseInsensitive) {
            rest = rest[start.upperBound...]
            guard let marker = rest.range(of: ";base64,", options: .caseInsensitive),
                rest.distance(from: rest.startIndex, to: marker.lowerBound) <= 32
            else { continue }
            let body = rest[marker.upperBound...]
            let end = body.firstIndex { !($0.isLetter || $0.isNumber || $0 == "+" || $0 == "/" || $0 == "=" || $0.isWhitespace) }
                ?? body.endIndex
            let encoded = body[..<end].filter { !$0.isWhitespace }
            if let data = Data(base64Encoded: String(encoded)), looksLikeImage(data) { out.append(data) }
            rest = body[end...]
        }
        return out
    }

    public static func looksLikeImage(_ data: Data) -> Bool {
        let b = [UInt8](data.prefix(16))
        func starts(_ prefix: [UInt8], at offset: Int = 0) -> Bool {
            b.count >= offset + prefix.count && Array(b[offset..<(offset + prefix.count)]) == prefix
        }
        if starts([0x89, 0x50, 0x4E, 0x47]) || starts([0xFF, 0xD8, 0xFF]) || starts(Array("GIF8".utf8)) { return true }
        if starts([0x49, 0x49, 0x2A, 0x00]) || starts([0x4D, 0x4D, 0x00, 0x2A]) || starts(Array("BM".utf8)) { return true }
        if starts(Array("RIFF".utf8)) && starts(Array("WEBP".utf8), at: 8) { return true }
        if starts(Array("ftyp".utf8), at: 4) {
            let brand = b.count >= 12 ? String(decoding: b[8..<12], as: UTF8.self) : ""
            return ["heic", "heix", "hevc", "heim", "heis", "mif1", "msf1", "avif"].contains(brand)
        }
        return false
    }

    public static func admit(_ images: [PastedImage], existing: Int) -> (accepted: [PastedImage], problem: String?) {
        let room = max(0, PromptAttachmentLimits.maxCount - existing)
        if images.count <= room { return (images, nil) }
        let left = images.count - room
        let problem = "A prompt can carry \(PromptAttachmentLimits.maxCount) photos, so "
            + (left == 1 ? "1 pasted image was" : "\(left) pasted images were") + " left out."
        return (Array(images.prefix(room)), problem)
    }
}
