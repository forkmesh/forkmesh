import Foundation
import XCTest

@testable import FMChat

final class ChatMediaFileTests: XCTestCase {
    private let folder = URL(fileURLWithPath: "/tmp/fm-chat-media-test/chat-media", isDirectory: true)

    private func attachment(id: String, name: String) -> ChatAttachment {
        ChatAttachment(messageId: id, fileName: name, mime: "application/pdf", data: Data([1]))
    }

    func testASenderChosenMessageIdCannotLeaveTheMediaFolder() throws {
        for id in ["../../Documents/Invoice", "/etc/passwd", "a/../../b", "..", "x\\..\\y"] {
            let url = try XCTUnwrap(ChatMediaFile.url(for: attachment(id: id, name: "2026.pdf"), in: folder))
            XCTAssertEqual(url.deletingLastPathComponent().path, folder.standardizedFileURL.path, id)
            XCTAssertFalse(url.lastPathComponent.contains("/"), id)
            XCTAssertTrue(url.lastPathComponent.hasSuffix("-2026.pdf"), id)
        }
    }

    func testTheFileNameCannotLeaveTheMediaFolder() throws {
        for name in ["../../Documents/x", "..", "a/b", "..\\..\\c"] {
            let url = try XCTUnwrap(ChatMediaFile.url(for: attachment(id: "m1", name: name), in: folder))
            XCTAssertEqual(url.deletingLastPathComponent().path, folder.standardizedFileURL.path, name)
        }
    }

    func testTheNameIsStablePerMessageAndDiffersBetweenMessages() {
        let first = ChatMediaFile.name(for: attachment(id: "message-1", name: "a.pdf"))
        XCTAssertEqual(first, ChatMediaFile.name(for: attachment(id: "message-1", name: "a.pdf")))
        XCTAssertNotEqual(first, ChatMediaFile.name(for: attachment(id: "message-2", name: "a.pdf")))
        XCTAssertEqual(first.count, 24 + 1 + "a.pdf".count)
    }
}
