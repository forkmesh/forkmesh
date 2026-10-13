// Photos on queued agent runs (the attachment cases added with the composer).

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiAgentAttachmentsTests: XCTestCase {
    private func photo(_ name: String = "photo.jpg", size: Int = 4) -> PromptAttachment {
        PromptAttachment(id: "a1", name: name, bytes: Data([0xFF, 0xD8, 0xFF] + [UInt8](repeating: 1, count: max(0, size - 3))))
    }

    private func queued() -> FakeHTTPClient {
        FakeHTTPClient { _ in portJSON(["ok": true, "session": ["id": "sess-1", "status": "queued"]], 202) }
    }

    func testLaunchSendsAttachmentsOnTheWire() async throws {
        let http = queued()
        let api = ApiPortFixture.api(http)
        _ = try await api.launchAgentSession(
            "alice", "mobile", prompt: "What is wrong here?", provider: "auto", model: "opus", attachments: [photo()])
        let sent = portBody(http.requests.first)
        XCTAssertEqual(sent["model"], "opus")
        let items = try XCTUnwrap(sent["attachments"]?.arrayValue)
        XCTAssertEqual(items.count, 1)
        XCTAssertEqual(items[0]["name"], "photo.jpg")
        XCTAssertEqual(items[0]["mediaType"], "image/jpeg")
        XCTAssertEqual(items[0]["data"]?.stringValue, Data([0xFF, 0xD8, 0xFF, 1]).base64EncodedString())
    }

    func testPhotosOnlyBecomeAPrompt() async throws {
        let http = queued()
        let api = ApiPortFixture.api(http)
        _ = try await api.steerAgentSession("alice", "mobile", "sess-1", prompt: "  ", attachments: [photo()])
        XCTAssertEqual(portBody(http.requests.first)["prompt"], "See the attached photos.")
    }

    func testWithoutPhotosNoAttachmentsKey() async throws {
        let http = queued()
        let api = ApiPortFixture.api(http)
        _ = try await api.launchAgentSession("alice", "mobile", prompt: "Fix it")
        XCTAssertNil(portBody(http.requests.first)["attachments"])
    }

    func testTooManyOrTooLargeIsRefusedBeforeUpload() async throws {
        let http = queued()
        let api = ApiPortFixture.api(http)
        let six = (0..<6).map { _ in photo() }
        do {
            _ = try await api.launchAgentSession("alice", "mobile", prompt: "x", attachments: six)
            XCTFail("expected a refusal")
        } catch let error as AgentLaunchException {
            XCTAssertTrue(error.message.contains("5 photos"))
        }
        let big = (0..<3).map { _ in photo(size: 4 * 1024 * 1024) }
        XCTAssertEqual(promptAttachmentsProblem(big), "Those photos add up to more than 10 MB. Send fewer at once.")
        XCTAssertEqual(promptAttachmentsProblem([photo("huge.jpg", size: 6 * 1024 * 1024)]), "huge.jpg is larger than 5 MB.")
        XCTAssertTrue(http.requests.isEmpty)
    }
}
