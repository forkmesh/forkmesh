import FMAgents
import Foundation
import UIKit
import UniformTypeIdentifiers

enum PromptPasteboard {
    static var hasImages: Bool {
        let pasteboard = UIPasteboard.general
        return pasteboard.hasImages
            || pasteboard.contains(pasteboardTypes: PromptPasteRules.rtfdTypes + [PromptPasteRules.htmlType])
    }

    static func read(_ pasteboard: UIPasteboard = .general) -> PromptPaste {
        var items: [PasteboardItem] = []
        for index in 0..<pasteboard.numberOfItems {
            let set = IndexSet(integer: index)
            let types: [String] = pasteboard.types(forItemSet: set)?.first ?? []
            var representations: [String: Data] = [:]
            for type in types where PromptPasteRules.wants(type) && representations[type] == nil {
                if let data = pasteboard.data(forPasteboardType: type, inItemSet: set)?.first {
                    representations[type] = data
                }
            }
            var embedded: [Data] = []
            if !PromptPasteRules.imageTypes.contains(where: { representations[$0] != nil }) {
                for type in PromptPasteRules.rtfdTypes {
                    if let data = representations[type] {
                        embedded = attachmentImages(data, rtfd: type == "com.apple.flat-rtfd")
                        break
                    }
                }
                if embedded.isEmpty,
                    let other = types.first(where: { UTType($0)?.conforms(to: .image) ?? false }),
                    let data = pasteboard.data(forPasteboardType: other, inItemSet: set)?.first
                {
                    embedded = [data]
                }
            }
            items.append(PasteboardItem(representations: representations, embeddedImages: embedded))
        }
        return PromptPasteRules.paste(items)
    }

    private static func attachmentImages(_ data: Data, rtfd: Bool) -> [Data] {
        let options: [NSAttributedString.DocumentReadingOptionKey: Any] = [
            .documentType: rtfd ? NSAttributedString.DocumentType.rtfd : NSAttributedString.DocumentType.rtf
        ]
        guard let text = try? NSAttributedString(data: data, options: options, documentAttributes: nil) else {
            return []
        }
        var images: [Data] = []
        text.enumerateAttribute(.attachment, in: NSRange(location: 0, length: text.length)) { (value: Any?, _: NSRange, _: UnsafeMutablePointer<ObjCBool>) in
            guard let attachment = value as? NSTextAttachment else {
                return
            }
            if let bytes = attachment.contents ?? attachment.fileWrapper?.regularFileContents,
                PromptPasteRules.looksLikeImage(bytes)
            {
                images.append(bytes)
            } else if let image = attachment.image, let png = image.pngData() {
                images.append(png)
            }
        }
        return images
    }
}
