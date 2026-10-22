import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatReadMarksTests: XCTestCase {
    static let directKey = "direct:" + String(repeating: "a", count: 32)

    private func marks(_ api: FakeAccountApi) -> ChatReadMarks {
        ChatReadMarks(api: api, delay: 3600, sleep: { _ in })
    }

    func testOnlySharedConversationKeysAreSynced() {
        XCTAssertTrue(ChatReadMarks.isSyncable("#general"))
        XCTAssertTrue(ChatReadMarks.isSyncable(ChatReadMarksTests.directKey))
        XCTAssertTrue(ChatReadMarks.isSyncable("private:" + String(repeating: "b", count: 32)))
        XCTAssertFalse(ChatReadMarks.isSyncable("#office/lobby"))
        XCTAssertFalse(ChatReadMarks.isSyncable("#General"))
        XCTAssertFalse(ChatReadMarks.isSyncable("#-x"))
        XCTAssertFalse(ChatReadMarks.isSyncable("@peer"))
        XCTAssertFalse(ChatReadMarks.isSyncable("direct:abc"))
        XCTAssertFalse(ChatReadMarks.isSyncable("mesh"))
    }

    func testQueuedMarksPostOnceAndNeverMoveBack() async {
        let api = FakeAccountApi()
        let readMarks = marks(api)
        readMarks.queue("#general", Date(epochMillis: 2_000))
        readMarks.queue("#general", Date(epochMillis: 1_000))
        readMarks.queue(ChatReadMarksTests.directKey, Date(epochMillis: 5_000))
        readMarks.queue("#office/lobby", Date(epochMillis: 5_000))
        await readMarks.flush()
        XCTAssertEqual(api.calls.count, 1)
        XCTAssertEqual(api.calls[0].0, "POST")
        XCTAssertEqual(api.calls[0].1, ChatReadMarks.endpoint)
        let posted = api.calls[0].2?["marks"]?.objectValue
        XCTAssertEqual(posted?["#general"]?.int64Value, 2_000)
        XCTAssertEqual(posted?[ChatReadMarksTests.directKey]?.int64Value, 5_000)
        XCTAssertEqual(posted?.keys.count, 2)
        readMarks.queue("#general", Date(epochMillis: 2_000))
        await readMarks.flush()
        XCTAssertEqual(api.calls.count, 1)
    }

    func testFetchedMarksMerge() async {
        let api = FakeAccountApi()
        api.replies["GET \(ChatReadMarks.endpoint)"] = ["marks": ["#general": 9_000, "#random": 4_000]]
        let readMarks = marks(api)
        readMarks.queue("#random", Date(epochMillis: 6_000))
        await readMarks.flush()
        let fetched = await readMarks.fetch()
        XCTAssertTrue(fetched)
        XCTAssertEqual(readMarks.mark(for: "#general"), 9_000)
        XCTAssertEqual(readMarks.mark(for: "#random"), 6_000)
        XCTAssertTrue(readMarks.isRead("#general", Date(epochMillis: 9_000)))
        XCTAssertFalse(readMarks.isRead("#general", Date(epochMillis: 9_001)))
    }

    func testReadingElsewhereClearsUnreadAndSkipsReplayedMessages() async throws {
        let base = Date().epochMillis - 60_000
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let api = FakeAccountApi()
        hub.readMarks = marks(api)
        for (id, ts) in [("unread-1", base + 1_000), ("unread-2", base + 2_000)] {
            try h.sendPlain([
                "type": "chat", "id": .string(id), "senderId": "remote-node", "sender": "Remote", "ts": .int(ts),
                "channel": "#random", "text": "unread",
            ])
        }
        let two = await waitUntil(2) { h.relay.totalUnread == 2 }
        XCTAssertTrue(two)
        api.replies["GET \(ChatReadMarks.endpoint)"] = ["marks": ["#random": .int(base + 1_500)]]
        await hub.applyReadMarks()
        XCTAssertEqual(h.relay.unreadCountFor("#random"), 2)
        api.replies["GET \(ChatReadMarks.endpoint)"] = ["marks": ["#random": .int(base + 2_000)]]
        await hub.applyReadMarks()
        XCTAssertEqual(h.relay.totalUnread, 2, "a second read within a few seconds is skipped")
        await hub.applyReadMarks(force: true)
        XCTAssertEqual(h.relay.totalUnread, 0)
        try h.sendPlain([
            "type": "chat", "id": "replayed", "senderId": "remote-node", "sender": "Remote", "ts": .int(base + 1_800),
            "channel": "#random", "text": "already read on the phone",
        ])
        try h.sendPlain([
            "type": "chat", "id": "fresh", "senderId": "remote-node", "sender": "Remote", "ts": .int(base + 3_000),
            "channel": "#random", "text": "new",
        ])
        let fresh = await waitUntil(2) { h.relay.messages("#random").count == 4 }
        XCTAssertTrue(fresh)
        XCTAssertEqual(h.relay.unreadCountFor("#random"), 1)
        await h.close()
    }

    func testOpeningAConversationQueuesItsNewestMessage() async throws {
        let base = Date().epochMillis - 60_000
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        let api = FakeAccountApi()
        let readMarks = marks(api)
        hub.readMarks = readMarks
        try h.sendPlain([
            "type": "chat", "id": "one", "senderId": "remote-node", "sender": "Remote", "ts": .int(base + 7_000),
            "channel": "#random", "text": "hello",
        ])
        let arrived = await waitUntil(2) { h.relay.totalUnread == 1 }
        XCTAssertTrue(arrived)
        hub.select("#random")
        await readMarks.flush()
        XCTAssertEqual(api.calls.last?.2?["marks"]?.objectValue?["#random"]?.int64Value, base + 7_000)
        await h.close()
    }
}
