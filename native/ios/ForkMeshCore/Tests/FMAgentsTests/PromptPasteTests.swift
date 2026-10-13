import FMApi
import Foundation
import XCTest
@testable import FMAgents

final class PromptPasteTests: XCTestCase {
    private let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3])
    private let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 0x4A, 0x46])
    private let heic = Data([0, 0, 0, 24] + Array("ftypheic".utf8) + [0, 0, 0, 0])
    private let pdf = Data(Array("%PDF-1.7\n".utf8))

    private func text(_ value: String) -> Data { Data(value.utf8) }

    func testEachImageItemBecomesOneAttachmentAndItsCaptionIsNotTyped() {
        let paste = PromptPasteRules.paste([
            PasteboardItem(representations: ["public.png": png, "public.jpeg": jpeg, "public.utf8-plain-text": text("https://example.com/a.png")]),
            PasteboardItem(representations: ["public.heic": heic]),
        ])

        XCTAssertEqual(paste.images, [
            PastedImage(name: "pasted-image-1", data: png),
            PastedImage(name: "pasted-image-2", data: heic),
        ])
        XCTAssertEqual(paste.text, "")
    }

    func testImagesInsideRichTextComeOutAndTheTextStays() {
        let paste = PromptPasteRules.paste([
            PasteboardItem(
                representations: ["com.apple.flat-rtfd": Data([1]), "public.utf8-plain-text": text("Look at this \u{FFFC} crash")],
                embeddedImages: [jpeg, pdf]),
        ])

        XCTAssertEqual(paste.images, [PastedImage(name: "pasted-image-1", data: jpeg)])
        XCTAssertEqual(paste.text, "Look at this  crash")
    }

    func testHtmlDataUriImagesAreRead() {
        let encoded = png.base64EncodedString()
        let html = "<p>see</p><img src=\"data:image/png;base64,\(encoded)\"><img src='https://tracker.example/x.png'>"
            + "<img src=\"DATA:image/jpeg;base64,\(jpeg.base64EncodedString())\">"
            + "<img src=\"data:image/svg+xml;base64,\(text("<svg/>").base64EncodedString())\">"
        XCTAssertEqual(PromptPasteRules.dataUriImages(html), [png, jpeg])

        let paste = PromptPasteRules.paste([
            PasteboardItem(representations: ["public.html": text(html), "public.utf8-plain-text": text("see")]),
        ])
        XCTAssertEqual(paste.images.map(\.data), [png, jpeg])
        XCTAssertEqual(paste.text, "see")
    }

    func testPlainTextHasNoImages() {
        let paste = PromptPasteRules.paste([PasteboardItem(representations: ["public.utf8-plain-text": text("hello")])])
        XCTAssertTrue(paste.images.isEmpty)
        XCTAssertEqual(paste.text, "hello")
    }

    func testOnlyRealImageBytesCount() {
        XCTAssertTrue(PromptPasteRules.looksLikeImage(png))
        XCTAssertTrue(PromptPasteRules.looksLikeImage(jpeg))
        XCTAssertTrue(PromptPasteRules.looksLikeImage(heic))
        XCTAssertTrue(PromptPasteRules.looksLikeImage(Data(Array("GIF89a".utf8))))
        XCTAssertTrue(PromptPasteRules.looksLikeImage(Data(Array("RIFF".utf8) + [0, 0, 0, 0] + Array("WEBP".utf8))))
        XCTAssertFalse(PromptPasteRules.looksLikeImage(pdf))
        XCTAssertFalse(PromptPasteRules.looksLikeImage(Data()))
        XCTAssertFalse(PromptPasteRules.looksLikeImage(Data([0, 0, 0, 24] + Array("ftypmp42".utf8))))
        let paste = PromptPasteRules.paste([PasteboardItem(representations: ["public.png": pdf])])
        XCTAssertTrue(paste.images.isEmpty)
    }

    func testPastedImagesRespectTheAttachmentLimit() {
        let three = (0..<3).map { PastedImage(name: "pasted-image-\($0 + 1)", data: png) }
        let fits = PromptPasteRules.admit(three, existing: 2)
        XCTAssertEqual(fits.accepted.count, 3)
        XCTAssertNil(fits.problem)

        let over = PromptPasteRules.admit(three, existing: 4)
        XCTAssertEqual(over.accepted.map(\.name), ["pasted-image-1"])
        XCTAssertEqual(over.problem, "A prompt can carry \(PromptAttachmentLimits.maxCount) photos, so 2 pasted images were left out.")
        XCTAssertEqual(PromptPasteRules.admit(three, existing: 5).accepted, [])
        XCTAssertEqual(
            PromptPasteRules.admit(Array(three.prefix(1)), existing: 5).problem,
            "A prompt can carry \(PromptAttachmentLimits.maxCount) photos, so 1 pasted image was left out.")
    }

    func testAPastedImageIsNamedLikeAPickedPhoto() {
        XCTAssertEqual(PromptPhotoRules.attachmentName("pasted-image-1"), "pasted-image-1.jpg")
        XCTAssertTrue(PromptPasteRules.wants("public.png"))
        XCTAssertTrue(PromptPasteRules.wants("com.apple.flat-rtfd"))
        XCTAssertFalse(PromptPasteRules.wants("public.url"))
    }
}
