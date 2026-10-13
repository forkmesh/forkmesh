import FMApi
import FMBase
import Foundation
import XCTest
@testable import FMChat

final class LocalAgentTranscriptTests: XCTestCase {
    private let id = String(repeating: "c", count: 32)
    private let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0] + [UInt8](repeating: 1, count: 40))

    func testTheDesktopListBecomesRefsReadOverLocalControl() throws {
        let json = try XCTUnwrap(JSON.parse(Data("""
        {"id":4,"transcript":"Attached image: /home/f/agent-images/paste.png\\nWorking...",
         "attachments":[{"id":"\(id)","name":"paste.png","mediaType":"image/png","size":2048},
                        {"id":"/home/f/secret.png","name":"x","mediaType":"image/png"}]}
        """.utf8)).objectValue)
        let detail = LocalAgentTranscript.fromJSON(json, sessionId: 4)
        XCTAssertTrue(detail.text.hasPrefix("Attached image:"))
        XCTAssertEqual(detail.attachments.count, 1)
        XCTAssertEqual(detail.attachments[0].origin, .desktop(sessionId: 4))
        XCTAssertEqual(detail.attachments[0].cacheKey, "desktop:4:\(id)")
        XCTAssertTrue(detail.attachments[0].showsThumbnail)
        XCTAssertEqual(detail.attachments[0].sizeLabel, "2.0 KB")
    }

    func testAnOlderDesktopStillShowsWhatTheTranscriptNames() throws {
        let json = try XCTUnwrap(JSON.parse(Data("""
        {"id":4,"transcript":"Attached image: /home/f/agent-images/paste.png\\nWorking..."}
        """.utf8)).objectValue)
        let detail = LocalAgentTranscript.fromJSON(json, sessionId: 4)
        XCTAssertEqual(detail.attachments.map(\.name), ["paste.png"])
        XCTAssertEqual(detail.attachments[0].origin, .elsewhere)
        XCTAssertFalse(detail.attachments[0].canFetch)
    }

    func testAttachmentBytesMustBeASmallEnoughImage() throws {
        let ok: JSONObject = ["data": .string(jpeg.base64EncodedString()), "mediaType": .string("image/jpeg")]
        XCTAssertEqual(try LocalAgentTranscript.attachmentBytes(ok), jpeg)
        XCTAssertThrowsError(try LocalAgentTranscript.attachmentBytes([:]))
        XCTAssertThrowsError(try LocalAgentTranscript.attachmentBytes(["data": .string("not base64!")]))
        XCTAssertThrowsError(
            try LocalAgentTranscript.attachmentBytes(["data": .string(Data("<html>".utf8).base64EncodedString())]))
        let huge = String(repeating: "A", count: (LocalAgentTranscript.maxAttachmentBytes + 2) / 3 * 4 + 8)
        XCTAssertThrowsError(try LocalAgentTranscript.attachmentBytes(["data": .string(huge)]))
    }

    func testAPreviewReplyCarriesItsSizes() throws {
        let preview: JSONObject = [
            "data": .string(jpeg.base64EncodedString()), "mediaType": "image/jpeg", "preview": true,
            "originalSize": 5_000_000.0, "width": 1600, "height": 900,
        ]
        let payload = try LocalAgentTranscript.attachmentPayload(preview)
        XCTAssertTrue(payload.preview)
        XCTAssertEqual(payload.data, jpeg)
        XCTAssertEqual(payload.originalSize, 5_000_000)
        XCTAssertEqual(payload.width, 1600)
        XCTAssertEqual(payload.height, 900)
        let full = try LocalAgentTranscript.attachmentPayload(["data": .string(jpeg.base64EncodedString()), "preview": false])
        XCTAssertEqual(full, .full(jpeg))
        let tooBig = Data([0xFF, 0xD8, 0xFF, 0xE0] + [UInt8](repeating: 1, count: LocalAgentTranscript.maxPreviewBytes))
        XCTAssertThrowsError(
            try LocalAgentTranscript.attachmentPayload(["data": .string(tooBig.base64EncodedString()), "preview": true]))
        XCTAssertEqual(
            LocalAgentTranscript.attachmentParams(sessionId: 3, attachmentId: id, preview: true),
            ["id": 3, "attachment": .string(id), "preview": true])
        XCTAssertEqual(
            LocalAgentTranscript.attachmentParams(sessionId: 3, attachmentId: id, preview: false),
            ["id": 3, "attachment": .string(id)])
        XCTAssertEqual(
            LocalAgentTranscript.attachmentErrorText("too_large_over_relay"),
            "This file is too large to open through the relay. Open it on the desktop's network.")
    }

    func testRefusalsReadAsSentences() {
        XCTAssertEqual(
            LocalAgentTranscript.attachmentErrorText("unsupported"),
            "Update ForkMesh on the desktop to see attachments here.")
        XCTAssertEqual(
            LocalAgentTranscript.attachmentErrorText("unknown_attachment"),
            "The desktop no longer has this attachment.")
        XCTAssertTrue(LocalAgentTranscript.attachmentErrorText("weird").contains("weird"))
    }
}
