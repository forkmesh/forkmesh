// The chat and guest chat logic (who may post where, what the transcript and header show), the
// shell bridge, and the user presence decision.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatHubTests: XCTestCase {
    private func hub(guest: Bool = false) async throws -> (ChatHub, RelayHarness, LocalMeshService) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, isGuest: { guest }), h, mesh)
    }

    func testTheSendButtonShowsANewChatMessageWithoutARefresh() async throws {
        let (hub, h, _) = try await hub()
        let sent = await hub.sendChatText("first live message")
        XCTAssertTrue(sent)
        XCTAssertEqual(hub.rows.map(\.message.text), ["first live message"])
        XCTAssertTrue(hub.rows[0].deletable)
        XCTAssertEqual(hub.relayState, .connected)
        await h.close()
    }

    func testAGuestCanPostInGeneralAndMesh() async throws {
        let (hub, h, _) = try await hub(guest: true)
        var blocked: [String] = []
        hub.onGuestBlocked = { blocked.append($0) }
        XCTAssertNil(hub.composeBlocked)
        let general = await hub.sendChatText("hi from a guest")
        XCTAssertTrue(general)

        hub.select("#random")
        XCTAssertEqual(hub.composeBlocked, "post in #random")
        let random = await hub.sendChatText("nope")
        XCTAssertFalse(random)
        XCTAssertEqual(blocked, ["post in #random"])

        hub.select("@someone")
        XCTAssertEqual(hub.composeBlocked, "send direct messages")

        hub.select(meshConversation)
        XCTAssertNil(hub.composeBlocked)
        // Nobody on the network: the mesh says so rather than pretending it sent.
        let mesh = await hub.sendChatText("anyone?")
        XCTAssertFalse(mesh)
        XCTAssertEqual(hub.notice?.text, "No one is on this network or Bluetooth yet.")
        await h.close()
    }

    func testASignedInUserCanPostAnywhere() async throws {
        let (hub, h, _) = try await hub()
        for conversation in ["#random", "@someone", meshConversation] {
            hub.select(conversation)
            XCTAssertNil(hub.composeBlocked, conversation)
        }
        await h.close()
    }

    func testAnOfflineRelaySaysSoAndReconnects() async throws {
        let (hub, h, _) = try await hub()
        await h.relay.disconnect()
        let sent = await hub.sendChatText("hello?")
        XCTAssertFalse(sent)
        XCTAssertEqual(hub.notice?.text, "Chat is offline. Reconnecting...")
        let redialled = await waitUntil(3) { h.relay.state == .connected }
        XCTAssertTrue(redialled, "\(h.relay.state) \(h.relay.log) \(h.ws.connections.count)")
        h.relay.dispose()
    }

    func testTheMeshRoomShowsItsOwnPeopleAndBadge() async throws {
        let (hub, h, _) = try await hub()
        hub.select(meshConversation)
        XCTAssertTrue(hub.inMesh)
        XCTAssertEqual(hub.badge, "local only")
        XCTAssertEqual(hub.titleIcon, "lan_outlined")
        XCTAssertTrue(hub.composerConnected, "the mesh room is writable whenever it is open")
        XCTAssertEqual(hub.memberGroups.count, 1)
        XCTAssertTrue(hub.memberGroups[0].isSelf)
        XCTAssertTrue(hub.emptyMessage.hasPrefix("Messages stay on this network"))
        hub.select("#general")
        XCTAssertEqual(hub.badge, "public")
        XCTAssertEqual(hub.emptyMessage, "Encrypted relay messages will appear here.")
        await h.close()
    }

    func testADirectMessageTitleNamesThePeer() async throws {
        let (hub, h, _) = try await hub()
        try await h.sendProtected(["type": "presence", "id": "p", "senderId": "node-z", "sender": "Zoe", "ts": .int(Date().epochMillis)])
        _ = await waitUntil(2) { h.relay.roster().count == 2 }
        hub.select("@node-z")
        XCTAssertEqual(hub.title, "@Zoe")
        XCTAssertEqual(hub.titleIcon, "alternate_email")
        hub.select("@gone")
        XCTAssertEqual(hub.title, "@direct message")
        await h.close()
    }

    func testTheRoomHeaderNamesTheRoomAndWhoIsIn() async throws {
        let (hub, h, _) = try await hub()
        hub.select("#general")
        XCTAssertEqual(hub.roomName, "general")
        XCTAssertEqual(hub.headerDetail, "1 member \u{2022} 2 tabs")
        XCTAssertEqual(hub.introLine, "This is the very beginning of the general channel.")
        try await h.sendProtected(["type": "presence", "id": "p", "senderId": "node-z", "sender": "Zoe", "ts": .int(Date().epochMillis)])
        _ = await waitUntil(2) { h.relay.roster().count == 2 }
        XCTAssertEqual(hub.headerDetail, "2 members \u{2022} 2 tabs")
        hub.select("@node-z")
        XCTAssertEqual(hub.roomName, "Zoe")
        XCTAssertEqual(hub.headerDetail, "Direct message • end-to-end")
        XCTAssertEqual(hub.introLine, "This is the very beginning of your direct messages with Zoe.")
        hub.select(meshConversation)
        XCTAssertEqual(hub.headerDetail, "local only")
        await h.close()
    }

    func testConsecutiveMessagesFromOneSenderAreGrouped() {
        let t = Date(epochMillis: 1_000_000)
        let a = ChatMessage(id: "1", conversation: "#g", senderId: "x", senderName: "Ada", text: "a", timestamp: t)
        let b = ChatMessage(id: "2", conversation: "#g", senderId: "x", senderName: "Ada", text: "b", timestamp: t.addingTimeInterval(60))
        let c = ChatMessage(id: "3", conversation: "#g", senderId: "x", senderName: "Ada", text: "c", timestamp: t.addingTimeInterval(400))
        let d = ChatMessage(id: "4", conversation: "#g", senderId: "y", senderName: "Bo", text: "d", timestamp: t.addingTimeInterval(410))
        XCTAssertFalse(ChatHub.continuesGroup(a, nil))
        XCTAssertTrue(ChatHub.continuesGroup(b, a))
        XCTAssertFalse(ChatHub.continuesGroup(c, b), "five minutes is a new thought")
        XCTAssertFalse(ChatHub.continuesGroup(d, c))
    }

    func testDayDividersSplitTheTranscriptByLocalDay() {
        let utc = TimeZone(identifier: "UTC")!
        let eastern = TimeZone(secondsFromGMT: -5 * 3600)!
        let day: TimeInterval = 86_400
        let late = ChatMessage(
            id: "1", conversation: "#g", senderId: "x", senderName: "Ada", text: "a",
            timestamp: Date(timeIntervalSince1970: day - 60))
        let early = ChatMessage(
            id: "2", conversation: "#g", senderId: "x", senderName: "Ada", text: "b",
            timestamp: Date(timeIntervalSince1970: day + 60))
        XCTAssertTrue(ChatHub.startsNewDay(late, after: nil, timeZone: utc))
        XCTAssertTrue(ChatHub.startsNewDay(early, after: late, timeZone: utc))
        XCTAssertFalse(ChatHub.startsNewDay(early, after: late, timeZone: eastern))
        XCTAssertFalse(ChatHub.continuesGroup(early, late, timeZone: utc))
        XCTAssertTrue(ChatHub.continuesGroup(early, late, timeZone: eastern))
        let now = Date(timeIntervalSince1970: 1_791_200_000)
        XCTAssertEqual(ChatHub.dayDividerLabel(now.addingTimeInterval(-3600), now: now, timeZone: utc), "Today")
        XCTAssertEqual(ChatHub.dayDividerLabel(now.addingTimeInterval(-day), now: now, timeZone: utc), "Yesterday")
        XCTAssertEqual(
            ChatHub.dayDividerLabel(now.addingTimeInterval(-3 * day), now: now, timeZone: utc), "Friday, October 2")
        XCTAssertEqual(
            ChatHub.dayDividerLabel(now.addingTimeInterval(-365 * day), now: now, timeZone: utc),
            "Sunday, October 5, 2025")
    }

    func testFencedRunsAreSplitOut() {
        XCTAssertEqual(ChatHub.segments("plain"), [ChatBodySegment(code: false, text: "plain")])
        XCTAssertEqual(
            ChatHub.segments("before\n```\nlet x = 1\n```\nafter"),
            [
                ChatBodySegment(code: false, text: "before"), ChatBodySegment(code: true, text: "let x = 1"),
                ChatBodySegment(code: false, text: "after"),
            ])
        XCTAssertEqual(ChatHub.segments("```open fence"), [ChatBodySegment(code: true, text: "open fence")])
        XCTAssertEqual(ChatHub.segments("``````"), [])
    }

    func testLabels() {
        XCTAssertEqual(ChatHub.unreadLabel(1), "1 unread message")
        XCTAssertEqual(ChatHub.unreadLabel(3), "3 unread messages")
        XCTAssertEqual(ChatHub.platformIcon("iOS"), "phone_iphone")
        XCTAssertEqual(ChatHub.platformIcon(""), "devices_other")
        let utc = TimeZone(identifier: "UTC")!
        XCTAssertEqual(ChatHub.timeLabel(Date(epochMillis: 0), timeZone: utc), "12:00 AM")
        XCTAssertEqual(ChatHub.timeLabel(Date(epochMillis: 9 * 3_600_000 + 5 * 60_000), timeZone: utc), "9:05 AM")
        XCTAssertEqual(ChatHub.timeLabel(Date(epochMillis: 12 * 3_600_000 + 30 * 60_000), timeZone: utc), "12:30 PM")
        XCTAssertEqual(ChatHub.timeLabel(Date(epochMillis: 23 * 3_600_000 + 59 * 60_000), timeZone: utc), "11:59 PM")
        let mine = ChatMessage(id: "1", conversation: "#g", senderId: "x", senderName: "Ada", text: "", timestamp: Date(), isSelf: true)
        XCTAssertEqual(ChatHub.senderLabel(mine), "Ada (you)")
    }
}

@MainActor
final class StubPresencePrompt: PresencePrompt {
    let answer: Bool?
    var reasons: [String] = []
    init(_ answer: Bool?) { self.answer = answer }
    func authenticate(reason: String) async -> Bool? {
        reasons.append(reason)
        return answer
    }
}

@MainActor
final class UserPresenceTests: XCTestCase {
    func testThePlatformPromptDecidesWhenItCan() async {
        let prompt = StubPresencePrompt(true)
        let presence = UserPresence(prompt: prompt)
        var dialogs = 0
        let ok = await presence.confirm(reason: "  Delete repo  ") { _ in
            dialogs += 1
            return false
        }
        XCTAssertTrue(ok)
        XCTAssertEqual(dialogs, 0)
        XCTAssertEqual(prompt.reasons, ["Delete repo"])
    }

    func testACancelledPromptIsARealNo() async {
        let presence = UserPresence(prompt: StubPresencePrompt(false))
        let ok = await presence.confirm(reason: "x") { _ in true }
        XCTAssertFalse(ok, "the dialog does not retry a cancel")
    }

    func testFallsBackToTheDialogWhenTheDeviceIsUnsupported() async {
        let presence = UserPresence(prompt: StubPresencePrompt(nil))
        var shown: [String] = []
        let ok = await presence.confirm(reason: "") { reason in
            shown.append(reason)
            return true
        }
        XCTAssertTrue(ok)
        XCTAssertEqual(shown, ["Confirm it's you to continue."])
    }

    func testForceDialogAlwaysUsesTheDialog() async {
        let prompt = StubPresencePrompt(true)
        let presence = UserPresence(prompt: prompt, forceDialog: true)
        let ok = await presence.confirm(reason: "x") { _ in false }
        XCTAssertFalse(ok)
        XCTAssertTrue(prompt.reasons.isEmpty)
    }

    func testOverrideReplacesTheSharedInstance() {
        let custom = UserPresence(forceDialog: true)
        UserPresence.override = custom
        XCTAssertTrue(UserPresence.instance === custom)
        UserPresence.override = nil
        XCTAssertFalse(UserPresence.instance === custom)
    }
}
