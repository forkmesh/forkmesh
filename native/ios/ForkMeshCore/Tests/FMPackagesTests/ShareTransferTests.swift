import Foundation
import XCTest

@testable import FMPackages

final class ShareTransferTests: XCTestCase {
    func folder() -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("share-transfer-" + UUID().uuidString)
    }

    func message(_ fields: [String: Any]) -> String {
        String(decoding: (try? JSONSerialization.data(withJSONObject: fields)) ?? Data(), as: UTF8.self)
    }

    func begin(_ id: String, name: String = "face-changer-2026.mp4", mime: String = "video/mp4", size: Int) -> String {
        message(["event": "begin", "id": id, "name": name, "mime": mime, "size": size, "title": "Face Changer clip"])
    }

    func chunk(_ id: String, _ data: Data) -> String {
        message(["event": "chunk", "id": id, "data": data.base64EncodedString()])
    }

    func end(_ id: String) -> String {
        message(["event": "end", "id": id])
    }

    func testAClipArrivesInChunksAndBecomesOneFile() throws {
        let writer = ShareTransferWriter(folder: folder())
        let first = Data((0..<300).map { UInt8($0 % 256) })
        let second = Data([1, 2, 3])
        XCTAssertNil(writer.receive(begin("a", size: first.count + second.count)))
        XCTAssertNil(writer.receive(chunk("a", first)))
        XCTAssertNil(writer.receive(chunk("other", Data([9]))))
        XCTAssertNil(writer.receive(chunk("a", second)))
        let finished = try XCTUnwrap(writer.receive(end("a")))
        XCTAssertEqual(finished.url.lastPathComponent, "face-changer-2026.mp4")
        XCTAssertEqual(finished.mime, "video/mp4")
        XCTAssertEqual(finished.title, "Face Changer clip")
        XCTAssertEqual(try Data(contentsOf: finished.url), first + second)
        XCTAssertNil(writer.receive(end("a")))
    }

    func testAShortOrOversizedClipIsDropped() {
        let short = ShareTransferWriter(folder: folder())
        _ = short.receive(begin("a", size: 10))
        _ = short.receive(chunk("a", Data([1, 2])))
        XCTAssertNil(short.receive(end("a")))
        let long = ShareTransferWriter(folder: folder())
        _ = long.receive(begin("b", size: 2))
        _ = long.receive(chunk("b", Data([1, 2, 3])))
        XCTAssertNil(long.receive(end("b")))
    }

    func testOnlyMediaWithinTheLimitIsAccepted() {
        for (mime, size) in [("text/html", 10), ("application/json", 10), ("video/mp4", 0),
                             ("video/mp4", ShareTransfer.maxBytes + 1)] {
            let writer = ShareTransferWriter(folder: folder())
            _ = writer.receive(begin("a", mime: mime, size: max(size, 0)))
            _ = writer.receive(chunk("a", Data([1])))
            XCTAssertNil(writer.receive(end("a")), mime)
        }
        XCTAssertTrue(ShareTransfer.allows(mime: "image/png"))
        XCTAssertTrue(ShareTransfer.allows(mime: "Video/WebM;codecs=vp8"))
    }

    func testNamesStayInsideTheShareFolder() {
        XCTAssertEqual(ShareTransfer.safeName("../../etc/passwd"), "passwd")
        XCTAssertEqual(ShareTransfer.safeName("a b<c>.mp4"), "abc.mp4")
        XCTAssertEqual(ShareTransfer.safeName(".."), "face-changer.mp4")
        XCTAssertEqual(ShareTransfer.safeName(""), "face-changer.mp4")
        XCTAssertEqual(ShareTransfer.safeName("ü.mp4"), "mp4")
        XCTAssertEqual(ShareTransfer.safeName(String(repeating: "x", count: 200)).count, 80)
    }
}
