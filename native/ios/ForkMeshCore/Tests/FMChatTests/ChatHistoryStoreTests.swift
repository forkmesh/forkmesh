import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMChat

private let hour: Int64 = 3_600_000

private func message(_ id: String, _ conversation: String, _ ts: Int64) -> ChatMessage {
    ChatMessage(id: id, conversation: conversation, senderId: "peer", senderName: "Pat", text: id, timestamp: Date(epochMillis: ts))
}

private func parts(_ messages: [ChatMessage]) -> ChatHistoryParts {
    var parts = ChatHistoryParts()
    parts.conversations = Dictionary(grouping: messages, by: \.conversation)
    return parts
}

@MainActor
final class ChatHistoryStoreTests: XCTestCase {
    func testScheduledSaveWritesOnceAndSkipsAnUnchangedHistory() async throws {
        let dir = temporaryDirectory("chat-history-store")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        let now = Date().epochMillis
        let first = [message("a", "#general", now - hour), message("b", "#general", now)]

        store.scheduleSave(scope: "s") { parts(first) }
        var writes = await store.flush()
        XCTAssertEqual(writes, 1)
        XCTAssertEqual(store.load(scope: "s")?.messages.map(\.id), ["a", "b"])

        store.scheduleSave(scope: "s") { parts(first) }
        writes = await store.flush()
        XCTAssertEqual(writes, 1)

        store.scheduleSave(scope: "s") { parts(first + [message("c", "#random", now)]) }
        writes = await store.flush()
        XCTAssertEqual(writes, 2)
        XCTAssertEqual(store.load(scope: "s")?.messages.map(\.id), ["a", "b", "c"])
    }

    func testAHistoryJustLoadedIsNotWrittenBack() async throws {
        let dir = temporaryDirectory("chat-history-store")
        defer { try? FileManager.default.removeItem(at: dir) }
        let now = Date().epochMillis
        let kept = [message("a", "#general", now - hour)]
        ChatHistoryStore(directory: dir, delay: 0).save(parts(kept).snapshot, scope: "s")

        let store = ChatHistoryStore(directory: dir, delay: 0)
        XCTAssertEqual(store.load(scope: "s")?.messages.map(\.id), ["a"])
        store.scheduleSave(scope: "s") { parts(kept) }
        let writes = await store.flush()
        XCTAssertEqual(writes, 0)
    }

    func testEraseDropsASavePendingBehindTheDelay() async throws {
        let dir = temporaryDirectory("chat-history-store")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0.2)
        store.scheduleSave(scope: "s") { parts([message("a", "#general", Date().epochMillis)]) }
        store.erase(scope: "s")
        try await Task.sleep(nanoseconds: 400_000_000)
        let writes = await store.flush()
        XCTAssertEqual(writes, 0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.file(scope: "s").path))
    }

    func testPartsLeaveExpiredRepliesAndTheirReactionsOut() {
        let now = Date(epochMillis: 10 * 24 * hour)
        var parts = parts([message("root", "#general", now.epochMillis - hour)])
        parts.replies = [message("old", "#general", now.epochMillis - 3 * 24 * hour), message("new", "#general", now.epochMillis)]
        parts.replyRoots = ["old": "root", "new": "root"]
        parts.reactions = ["old": ["👍": ["p": "Pat"]], "new": ["👍": ["p": "Pat"]], "gone": ["👍": ["p": "Pat"]]]
        parts.privateChannels = ["#b", "#a"]
        parts.retention = ChatRetention(defaultMs: 24 * hour)
        parts.now = now

        let snapshot = parts.snapshot
        XCTAssertEqual(snapshot.messages.map(\.id), ["root"])
        XCTAssertEqual(snapshot.replies.map(\.id), ["new"])
        XCTAssertEqual(snapshot.replyRoots, ["new": "root"])
        XCTAssertEqual(Set(snapshot.reactions.keys), ["new"])
        XCTAssertEqual(snapshot.privateChannels, ["#a", "#b"])
    }
}
