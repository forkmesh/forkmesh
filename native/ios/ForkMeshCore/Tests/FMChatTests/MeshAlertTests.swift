import FMApi
import Foundation
import XCTest

@testable import FMChat

final class MeshAlertTests: XCTestCase {
    private let now: Date = Date(timeIntervalSince1970: 1_800_000_000)

    private func message(
        _ text: String = "lunch is here", sender: String = "grace", isSelf: Bool = false, age: TimeInterval = 1
    ) -> ChatMessage {
        ChatMessage(
            id: "m-1", conversation: meshConversation, senderId: "g", senderName: sender, text: text,
            timestamp: now.addingTimeInterval(-age), isSelf: isSelf)
    }

    func testAMessageFromSomeoneNearbyAlerts() throws {
        let alert: MeshAlert = try XCTUnwrap(meshAlert(message(), unread: 1, watching: false, now: now))
        XCTAssertEqual(alert.title, "grace in #mesh")
        XCTAssertEqual(alert.body, "lunch is here")
        XCTAssertEqual(alert.sender, "grace")
    }

    func testWatchingTheRoomOrOwnMessagesStayQuiet() {
        XCTAssertNil(meshAlert(message(), unread: 1, watching: true, now: now))
        XCTAssertNil(meshAlert(message(isSelf: true), unread: 0, watching: false, now: now))
        var general: ChatMessage = message()
        general.conversation = "#general"
        XCTAssertNil(meshAlert(general, unread: 1, watching: false, now: now))
    }

    func testOldBackfillStaysQuiet() {
        XCTAssertNil(meshAlert(message(age: meshAlertFreshSeconds + 1), unread: 1, watching: false, now: now))
        XCTAssertNotNil(meshAlert(message(age: meshAlertFreshSeconds), unread: 1, watching: false, now: now))
    }

    func testUnreadCountAndPreview() throws {
        let long: String = "  a\n\nb  " + String(repeating: "x", count: 300)
        let alert: MeshAlert = try XCTUnwrap(meshAlert(message(long, sender: " "), unread: 3, watching: false, now: now))
        XCTAssertEqual(alert.title, "Someone nearby in #mesh · 3 new")
        XCTAssertEqual(alert.body.count, meshAlertPreviewChars)
        XCTAssertTrue(alert.body.hasPrefix("a b xxx"), alert.body)
        XCTAssertTrue(alert.body.hasSuffix("..."), alert.body)
        XCTAssertEqual(meshAlert(message(" "), unread: 1, watching: false, now: now)?.body, "New message")
    }
}
