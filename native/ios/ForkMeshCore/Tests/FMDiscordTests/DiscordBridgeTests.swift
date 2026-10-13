import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMDiscord

@MainActor
final class FakeDiscordDesktop: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure = failures[method] { throw failure }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    func emit(_ event: String) { for tap in taps.values { tap(event, [:]) } }
}

final class DiscordBridgeTests: XCTestCase {
    func testChannelsAreParsedAndGrouped() {
        let reply: JSONObject = [
            "channels": [
                ["id": "1", "name": "#general", "group": "Discord · A", "guild": true, "canSend": true],
                ["id": "2", "name": "#dev", "group": "Discord · A", "guild": true],
                ["id": "x", "name": "bad"],
                ["id": "1", "name": "dup"],
                ["id": "3", "name": "Pat", "group": "Discord · Direct messages", "direct": true],
            ]
        ]
        let channels = DiscordBridge.parseChannels(reply)
        XCTAssertEqual(channels.map(\.id), ["1", "2", "3"])
        let guilds = DiscordBridge.guilds(channels)
        XCTAssertEqual(guilds.map(\.name), ["Discord · A", "Discord · Direct messages"])
        XCTAssertEqual(guilds[0].channels.count, 2)
    }

    func testMessagesAreBounded() {
        var rows: [JSONValue] = []
        for index in 0..<150 {
            rows.append(["id": .string("discord-message:\(index)"), "sender": "Pat", "text": .string("m\(index)"), "ts": .int(Int64(index))])
        }
        rows.append(["id": "", "text": "no id"])
        rows[149] = ["id": "discord-message:149", "sender": "Pat", "text": "last", "ts": 149, "reactions": ["\u{1F44D}": 2, "zero": 0]]
        let messages = DiscordBridge.parseMessages(["messages": .array(rows)])
        XCTAssertEqual(messages.count, DiscordBridge.maxMessages - 1)
        XCTAssertEqual(messages.last?.text, "last")
        XCTAssertEqual(messages.last?.reactions, [DiscordReaction(value: "like", count: 2)])
    }

    @MainActor func testSendNeedsAPreparedToken() async throws {
        let desktop = FakeDiscordDesktop()
        desktop.replies["discord.prepare"] = ["token": "tok", "summary": "Send to #general: “hi”", "expiresInMs": 60000]
        let bridge = DiscordBridge(channel: desktop)
        let confirmation = try await bridge.prepareSend(channel: "1", text: "  hi  ")
        XCTAssertEqual(confirmation.text, "hi")
        XCTAssertEqual(confirmation.summary, "Send to #general: “hi”")
        try await bridge.send(confirmation)
        XCTAssertEqual(desktop.requests.map(\.0), ["discord.prepare", "discord.send", "discord.messages"])
        XCTAssertEqual(desktop.requests[1].1["token"], "tok")
        XCTAssertEqual(desktop.requests[1].1["text"], "hi")
        do {
            _ = try await bridge.prepareSend(channel: "1", text: String(repeating: "x", count: 2_001))
            XCTFail("too long")
        } catch let error as DiscordBridgeError {
            XCTAssertEqual(error, .invalid(DiscordBridge.errorMessage("bad_text")))
        }
        let clock = FixedClock(Date())
        let stale = DiscordBridge(channel: desktop, clock: clock)
        let old = try await stale.prepareSend(channel: "1", text: "hi")
        clock.advance(by: 61)
        do {
            try await stale.send(old)
            XCTFail("expired")
        } catch let error as DiscordBridgeError {
            XCTAssertEqual(error, .refused("confirmation_expired"))
        }
    }

    @MainActor func testAnOlderDesktopIsReported() async {
        let desktop = FakeDiscordDesktop()
        desktop.failures["discord.status"] = LocalControlException("unsupported")
        let bridge = DiscordBridge(channel: desktop)
        await bridge.refresh()
        XCTAssertEqual(bridge.lastError, DiscordBridgeError.desktopTooOld.description)
    }

    @MainActor func testRefreshListsAndTheBellReloads() async {
        let desktop = FakeDiscordDesktop()
        desktop.replies["discord.status"] = ["available": true, "configured": true, "connected": true, "bot": "helper"]
        desktop.replies["discord.list"] = ["channels": [["id": "9", "name": "#x", "canSend": true]]]
        let bridge = DiscordBridge(channel: desktop)
        await bridge.refresh()
        XCTAssertEqual(bridge.status.bot, "helper")
        XCTAssertEqual(bridge.channels.map(\.id), ["9"])
        bridge.watching = ["9"]
        bridge.startWatching()
        desktop.emit("discord.changed")
        desktop.emit("teammates.changed")
        let deadline = Date().addingTimeInterval(2)
        while !desktop.requests.contains(where: { $0.0 == "discord.messages" }) && Date() < deadline {
            try? await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTAssertEqual(desktop.requests.filter { $0.0 == "discord.status" }.count, 2)
        XCTAssertTrue(desktop.requests.contains { $0.0 == "discord.messages" })
        bridge.stopWatching()
    }
}
