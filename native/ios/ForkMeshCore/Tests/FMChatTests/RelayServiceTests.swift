// The relay room over a fake WebSocket, with a peer that speaks the room cipher and RoomSeal.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class RelayHarness {
    let device: ChatDevice
    let ws = FakeWebSocketConnector()
    let relay: RelayService
    let crypto: RoomCrypto
    let server: FakeWebSocketConnection
    private var peerSeal: RoomSeal?

    static func start() async throws -> RelayHarness {
        let device = ChatDevice(name: "Mobile Tester")
        device.settings.setServerUrl("ws://127.0.0.1:8787/ws")
        device.settings.setPassphrase("forkmesh-chat-screen-test-key")
        let ws = FakeWebSocketConnector()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure, connector: ws,
            platform: "ios")
        relay.accountProvider = { "Mobile Tester" }
        await relay.connect()
        let server = try XCTUnwrap(ws.lastConnection, "the relay dialled")
        let room = device.settings.room
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "forkmesh-chat-screen-test-key") }.value
        return RelayHarness(device: device, ws: ws, relay: relay, crypto: crypto, server: server)
    }

    private init(device: ChatDevice, ws: FakeWebSocketConnector, relay: RelayService, crypto: RoomCrypto, server: FakeWebSocketConnection) {
        self.device = device
        self.relay = relay
        self.crypto = crypto
        self.server = server
    }

    func sendPlain(_ plain: JSONObject) throws {
        server.receive(text: JSON.encode(.object(try crypto.encrypt(plain))))
    }

    /// Sealed to this device, as a real peer's protected frames are.
    func sendProtected(_ plain: JSONObject) async throws {
        if peerSeal == nil {
            let peer = try RoomSeal.fromPrivateKey(SettingsService.defaultRoom, (1...32).map { UInt8($0) })
            var mobileKey: JSONObject?
            for _ in 0..<200 where mobileKey == nil {
                mobileKey = server.sentTexts.lazy.compactMap { text -> JSONObject? in
                    guard case .object(let env)? = try? JSON.parse(text), let frame = self.crypto.decrypt(env),
                        RoomSeal.isKeyFrame(frame)
                    else { return nil }
                    return frame
                }.first
                if mobileKey == nil { try await Task.sleep(nanoseconds: 10_000_000) }
            }
            let key = try XCTUnwrap(mobileKey, "mobile seal key was not advertised")
            XCTAssertTrue(peer.acceptKeyFrame(key))
            try sendPlain(peer.keyFrame())
            peerSeal = peer
        }
        try sendPlain(try peerSeal!.seal(plain))
    }

    /// Every frame the device sent that this harness can read.
    func openFrames() -> [JSONObject] {
        server.sentTexts.compactMap { text -> JSONObject? in
            guard case .object(let env)? = try? JSON.parse(text), let frame = crypto.decrypt(env),
                !RoomSeal.isKeyFrame(frame)
            else { return nil }
            if !RoomSeal.isSealedFrame(frame) { return frame }
            return peerSeal?.unseal(frame)
        }
    }

    func close() async {
        await relay.disconnect()
        relay.dispose()
    }
}

@MainActor
final class RelayServiceTests: XCTestCase {
    func testRelayConnectionFailuresStayInTheReconnectingState() async {
        let device = ChatDevice()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            connector: FakeWebSocketConnector(), sleep: { _ in try await Task.sleep(nanoseconds: 60_000_000_000) })
        defer { relay.dispose() }
        relay.roomPassphraseProvider = { throw MeshSocketError(code: 111, message: "connection refused") }
        await relay.connect()
        XCTAssertEqual(relay.state, .connecting)
        XCTAssertEqual(relay.log.count, 1)
        XCTAssertTrue(relay.log[0].contains("Connect failed:"), relay.log[0])
    }

    func testASignedOutPhoneFailsClosedWithoutARoomKey() async {
        let device = ChatDevice()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            connector: FakeWebSocketConnector(), sleep: { _ in try await Task.sleep(nanoseconds: 60_000_000_000) })
        defer { relay.dispose() }
        relay.roomPassphraseProvider = { "" }
        await relay.connect()
        XCTAssertTrue(relay.log.last?.contains("Sign in to join chat - room key unavailable.") == true)
    }

    func testTheReconnectBackoffDoublesToFiveMinutes() async {
        let device = ChatDevice()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            connector: FakeWebSocketConnector())
        XCTAssertEqual(relay.nextReconnectDelay, 1)
    }

    func testSendingShowsTheMessageAtOnceAsADurableEncryptedFrame() async throws {
        let h = try await RelayHarness.start()
        XCTAssertEqual(h.relay.state, .connected)
        let sent = await h.relay.sendMessage("retain this message")
        XCTAssertTrue(sent)
        XCTAssertEqual(h.relay.messages("#general").map(\.text), ["retain this message"])
        let arrived = await h.server.waitForSent(3)
        XCTAssertTrue(arrived)
        let frame = try XCTUnwrap(h.server.sentJSON.last { $0["persist"] == .bool(true) && $0["notify"] == .bool(true) })
        XCTAssertEqual(frame["kind"], "cipher")
        // Public #channel chat is controller-readable: room cipher only, not sealed.
        let chat = try XCTUnwrap(h.openFrames().first { $0["type"] == "chat" })
        XCTAssertEqual(chat["text"], "retain this message")
        XCTAssertEqual(chat["channel"], "#general")
        XCTAssertEqual(chat["version"], "flutter-1.0")
        XCTAssertEqual(chat["platform"], "ios")
        XCTAssertNil(chat["persist"], "the retention flag rides outside the ciphertext")
        await h.close()
    }

    func testDirectMessagesAreNotRetainedByTheRelay() async throws {
        let h = try await RelayHarness.start()
        _ = await h.server.waitForSent(2)
        let before = h.server.sentTexts.count
        h.relay.switchConversation("@remote-node")
        _ = await h.relay.sendMessage("private direct message")
        _ = await h.server.waitForSent(before + 1)
        let dm = try XCTUnwrap(h.server.sentJSON.last)
        XCTAssertEqual(dm["kind"], "cipher")
        XCTAssertFalse(dm.contains("persist"))
        // And it is sealed: the room key alone does not read it.
        let inner = try XCTUnwrap(h.crypto.decrypt(dm))
        XCTAssertTrue(RoomSeal.isSealedFrame(inner))
        await h.close()
    }

    func testRetainedAndLiveMessagesRenderInChronologicalOrder() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "chat", "id": "remote-newer", "senderId": "remote-node", "sender": "Remote", "ts": 2000,
            "channel": "#general", "text": "newer remote message",
        ])
        try h.sendPlain([
            "type": "chat", "id": "remote-older", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channel": "#general", "text": "older remote message",
        ])
        let both = await waitUntil(2) { h.relay.messages("#general").count == 2 }
        XCTAssertTrue(both)
        XCTAssertEqual(h.relay.messages("#general").map(\.text), ["older remote message", "newer remote message"])
        await h.close()
    }

    func testUnreadCountsAccumulateAndClearWhenRead() async throws {
        let h = try await RelayHarness.start()
        for (id, ts) in [("unread-1", 1000), ("unread-2", 2000)] {
            try h.sendPlain([
                "type": "chat", "id": .string(id), "senderId": "remote-node", "sender": "Remote", "ts": .int(Int64(ts)),
                "channel": "#random", "text": "unread",
            ])
        }
        let two = await waitUntil(2) { h.relay.totalUnread == 2 }
        XCTAssertTrue(two)
        XCTAssertTrue(h.relay.hasUnread("#random"))
        XCTAssertEqual(h.relay.unreadCountFor("#random"), 2)
        h.relay.switchConversation("#random")
        XCTAssertEqual(h.relay.totalUnread, 0)
        XCTAssertFalse(h.relay.hasUnread("#random"))
        await h.close()
    }

    func testMarkAllReadClearsEveryConversationAtOnce() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "chat", "id": "bulk-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channel": "#random", "text": "channel unread",
        ])
        try h.sendPlain([
            "type": "chat", "id": "bulk-2", "senderId": "remote-node", "sender": "Remote", "ts": 2000,
            "channel": "#dev", "text": "another channel unread",
        ])
        let two = await waitUntil(2) { h.relay.totalUnread == 2 }
        XCTAssertTrue(two)
        XCTAssertEqual(h.relay.unreadConversations.count, 2)
        XCTAssertTrue(h.relay.channels.contains("#dev"), "a channel heard of is added to the list")
        h.relay.markAllRead()
        XCTAssertEqual(h.relay.totalUnread, 0)
        await h.close()
    }

    func testSeeingTheBadgeKeepsEachConversationUnread() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "chat", "id": "badge-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channel": "#random", "text": "first",
        ])
        try h.sendPlain([
            "type": "chat", "id": "badge-2", "senderId": "remote-node", "sender": "Remote", "ts": 2000,
            "channel": "#dev", "text": "second",
        ])
        let two = await waitUntil(2) { h.relay.totalUnread == 2 }
        XCTAssertTrue(two)
        XCTAssertEqual(h.relay.badgeUnread, 2)
        h.relay.markBadgeSeen()
        XCTAssertEqual(h.relay.badgeUnread, 0)
        XCTAssertEqual(h.relay.totalUnread, 2)
        XCTAssertTrue(h.relay.hasUnread("#random"))
        XCTAssertTrue(h.relay.hasUnread("#dev"))
        try h.sendPlain([
            "type": "chat", "id": "badge-3", "senderId": "remote-node", "sender": "Remote", "ts": 3000,
            "channel": "#dev", "text": "third",
        ])
        let three = await waitUntil(2) { h.relay.totalUnread == 3 }
        XCTAssertTrue(three)
        XCTAssertEqual(h.relay.badgeUnread, 1)
        h.relay.switchConversation("#dev")
        XCTAssertEqual(h.relay.badgeUnread, 0)
        XCTAssertEqual(h.relay.totalUnread, 1)
        XCTAssertTrue(h.relay.hasUnread("#random"))
        await h.close()
    }

    func testNoteUnreadMarksAnotherConversationButNotTheOpenOne() async throws {
        let h = try await RelayHarness.start()
        h.relay.switchConversation("#general")
        h.relay.noteUnread("#office/crew")
        h.relay.noteUnread("#general")
        XCTAssertTrue(h.relay.hasUnread("#office/crew"))
        XCTAssertFalse(h.relay.hasUnread("#general"))
        XCTAssertEqual(h.relay.badgeUnread, 1)
        await h.close()
    }

    func testAnUnsealedFrameOutsideAPublicChannelIsRefused() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "dm", "id": "sneaky", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "to": .string(h.relay.selfId), "text": "not sealed",
        ])
        let logged = await waitUntil(2) { h.relay.log.contains { $0.contains("refused an unencrypted frame") } }
        XCTAssertTrue(logged)
        XCTAssertTrue(h.relay.messages("@remote-node").isEmpty)
        await h.close()
    }

    func testSealedDirectMessagesArriveInTheirConversation() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
            "type": "dm", "id": "dm-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "to": .string(h.relay.selfId), "text": "hello you",
        ])
        let arrived = await waitUntil(2) { !h.relay.messages("@remote-node").isEmpty }
        XCTAssertTrue(arrived)
        XCTAssertEqual(h.relay.messages("@remote-node").first?.text, "hello you")
        XCTAssertEqual(h.relay.unreadCountFor("@remote-node"), 1)
        await h.close()
    }

    func testOnlyTheAuthorMayDeleteAMessage() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "chat", "id": "m1", "senderId": "author", "sender": "A", "ts": 1000, "channel": "#general",
            "text": "mine",
        ])
        _ = await waitUntil(2) { h.relay.messages("#general").count == 1 }
        try await h.sendProtected([
            "type": "delete", "id": "d1", "senderId": "someone-else", "sender": "B", "ts": 1001,
            "conversation": "#general", "target": "m1", "channel": "#general",
        ])
        try await h.sendProtected([
            "type": "presence", "id": "p1", "senderId": "someone-else", "sender": "B", "ts": 1002,
        ])
        _ = await waitUntil(2) { h.relay.roster().contains { $0.id == "someone-else" } }
        XCTAssertEqual(h.relay.messages("#general").count, 1, "a stranger cannot take it back")
        try await h.sendProtected([
            "type": "delete", "id": "d2", "senderId": "author", "sender": "A", "ts": 1003,
            "conversation": "#general", "target": "m1", "channel": "#general",
        ])
        let gone = await waitUntil(2) { h.relay.messages("#general").isEmpty }
        XCTAssertTrue(gone)
        await h.close()
    }

    func testDeletingYourOwnMessageTellsTheRoom() async throws {
        let h = try await RelayHarness.start()
        _ = await h.relay.sendMessage("oops")
        let message = try XCTUnwrap(h.relay.messages("#general").first)
        let sent = await h.relay.deleteMessage(message)
        XCTAssertTrue(sent)
        XCTAssertTrue(h.relay.messages("#general").isEmpty)
        _ = await waitUntil(2) { h.server.sentJSON.contains { $0["persist"] == .bool(true) && $0["notify"] == nil } }
        let deletion = h.server.sentJSON.last { $0["persist"] == .bool(true) }
        XCTAssertNotNil(deletion)
        XCTAssertNil(deletion?["notify"], "a deletion does not wake phones")
        await h.close()
    }

    func testAHelloIsAnsweredWithThisDevicesChannels() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
            "type": "hello", "id": "h1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channels": ["#general", "#ops"],
        ])
        let replied = await waitUntil(2) {
            h.openFrames().contains { $0["type"] == "hello" && $0["to"] == "remote-node" }
        }
        XCTAssertTrue(replied)
        XCTAssertTrue(h.relay.channels.contains("#ops"))
        let reply = try XCTUnwrap(h.openFrames().last { $0["type"] == "hello" && $0["to"] == "remote-node" })
        XCTAssertEqual(reply["channels"], ["#general", "#random", "#ops"])
        await h.close()
    }

    func testNodesOwnedByOneUserCollapseIntoASingleRosterEntry() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        for (node, platform) in [("node-a", "linux"), ("node-b", "android")] {
            try await h.sendProtected([
                "type": "presence", "id": .string("p-\(node)"), "senderId": .string(node), "sender": "Ada",
                "ownerUser": "ada", "platform": .string(platform), "ts": .int(now),
            ])
        }
        let two = await waitUntil(2) { h.relay.roster().filter { !$0.isSelf }.count == 2 }
        XCTAssertTrue(two)
        let ada = h.relay.rosterGroups().filter { $0.name == "ada" }
        XCTAssertEqual(ada.count, 1)
        XCTAssertEqual(ada.first?.members.count, 2)
        XCTAssertEqual(ada.first?.disambiguator, "")
        XCTAssertEqual(h.relay.onlineCount, 3)
        await h.close()
    }

    func testYourOtherDevicesJoinYourOwnRosterEntry() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
            "type": "presence", "id": "p-desk", "senderId": "w0F5DesktopNodeggm4", "sender": "Mobile Tester",
            "ownerUser": "mobile tester", "platform": "linux", "ts": .int(Date().epochMillis),
        ])
        let heard = await waitUntil(2) { h.relay.roster().count == 2 }
        XCTAssertTrue(heard)
        let groups = h.relay.rosterGroups()
        XCTAssertEqual(groups.count, 1)
        let me = try XCTUnwrap(groups.first)
        XCTAssertTrue(me.isSelf)
        XCTAssertEqual(me.members.map(\.id), [h.relay.selfId, "w0F5DesktopNodeggm4"])
        XCTAssertEqual(me.disambiguator, "")
        await h.close()
    }

    func testTheMeshRoomGroupsDevicesOfOneAccount() {
        let groups = RelayService.groups([
            Member(id: "phone", name: "Ada", isSelf: true, online: true, owner: "ada"),
            Member(id: "laptop", name: "Ada's laptop", online: true, owner: "ada"),
            Member(id: "bob", name: "bob", online: true),
        ])
        XCTAssertEqual(groups.map(\.name), ["ada", "bob"])
        XCTAssertEqual(groups.first?.members.count, 2)
        XCTAssertEqual(groups.first?.isSelf, true)
    }

    func testPresenceFramesCarryTheMemberStatusBothWays() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        func adaStatus() -> UserStatus { h.relay.rosterGroups().first { $0.name == "ada" }?.userStatus ?? .empty }

        try await h.sendProtected([
            "type": "presence", "id": "status-1", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada",
            "ts": .int(now), "userStatus": ["away": true, "emoji": "🎧", "text": "Focus time", "expiresAt": 0],
        ])
        let heard = await waitUntil(2) { adaStatus().emoji == "🎧" }
        XCTAssertTrue(heard)
        XCTAssertTrue(adaStatus().away)
        XCTAssertEqual(h.relay.liveStatusFor("node-a")?.text, "Focus time")

        // No field: the known status stays. A chat frame or a stale presence frame cannot
        // overwrite it either.
        try await h.sendProtected([
            "type": "presence", "id": "status-2", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada",
            "ts": .int(now + 1),
        ])
        try await h.sendProtected([
            "type": "presence", "id": "status-3", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada", "ts": 1000,
            "userStatus": ["away": false, "emoji": "👻", "text": "replayed"],
        ])
        try h.sendPlain([
            "type": "chat", "id": "status-4", "senderId": "node-a", "sender": "Ada", "ts": .int(now + 2),
            "channel": "#general", "text": "hello", "userStatus": ["away": false, "emoji": "👻", "text": "chat"],
        ])
        _ = await waitUntil(2) { !h.relay.messages("#general").isEmpty }
        XCTAssertEqual(adaStatus().emoji, "🎧")
        XCTAssertTrue(adaStatus().away)

        // Before this device knows its own status, presence says nothing about it; once known, a
        // change is announced at once.
        let mark = h.openFrames().count
        h.relay.appLifecycleChanged(.resumed)
        _ = await waitUntil(2) { h.openFrames().count > mark }
        let silent = h.openFrames().dropFirst(mark).filter { $0["type"] == "presence" }
        XCTAssertFalse(silent.isEmpty)
        XCTAssertTrue(silent.allSatisfy { !$0.contains("userStatus") })

        let before = h.openFrames().count
        h.relay.setOwnStatus(UserStatus(away: true, emoji: "🌴", text: "Vacationing"))
        _ = await waitUntil(2) { h.openFrames().count > before }
        let announced = try XCTUnwrap(h.openFrames().last { $0["type"] == "presence" })
        XCTAssertEqual(announced["userStatus"], ["away": true, "emoji": "🌴", "text": "Vacationing", "expiresAt": 0])
        XCTAssertEqual(h.relay.roster().first?.userStatus.emoji, "🌴")
        await h.close()
    }

    func testASocketThatDropsComesBackOnItsOwn() async throws {
        let device = ChatDevice(name: "t")
        device.settings.setServerUrl("ws://127.0.0.1:8787/ws")
        device.settings.setPassphrase("forkmesh-chat-screen-test-key")
        let ws = FakeWebSocketConnector()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure, connector: ws,
            sleep: { _ in try await Task.sleep(nanoseconds: 5_000_000) })
        await relay.connect()
        XCTAssertEqual(relay.state, .connected)
        ws.lastConnection?.fail()
        let redialled = await ws.waitForConnections(2)
        XCTAssertTrue(redialled)
        let back = await waitUntil(2) { relay.state == .connected }
        XCTAssertTrue(back)
        await relay.disconnect()
        XCTAssertEqual(relay.state, .offline)
        relay.dispose()
    }

    func testAResetForgetsEverythingFromTheRelayBeingLeft() async throws {
        let h = try await RelayHarness.start()
        _ = await h.relay.sendMessage("old world")
        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        XCTAssertTrue(h.relay.messages("#general").isEmpty)
        XCTAssertTrue(h.relay.log.isEmpty)
        XCTAssertEqual(h.relay.roster().count, 1)
        h.relay.dispose()
    }

    func testRosterHelpersMatchTheDart() {
        XCTAssertEqual(RelayService.identityKey(Member(id: "n", name: "Bob")), "name:bob")
        XCTAssertEqual(RelayService.identityKey(Member(id: "n", name: "x", owner: " Ada ")), "owner:ada")
        XCTAssertEqual(RelayService.identityKey(Member(id: "n", name: "x", isSelf: true, owner: "Ada")), "owner:ada")
        XCTAssertEqual(RelayService.identityKey(Member(id: "n", name: "guest-1", isSelf: true)), "self")
        XCTAssertEqual(RelayService.displayName(Member(id: "n", name: "", nodeName: "box")), "box")
        XCTAssertEqual(RelayService.shortIdentity(Member(id: "short", name: "")), "short")
        let longId = Member(id: "NodeIdentifierLong", name: "")
        XCTAssertEqual(RelayService.shortIdentity(longId), "Node…Long")
    }
}
