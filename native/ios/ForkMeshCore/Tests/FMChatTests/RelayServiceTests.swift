// The relay room over a fake WebSocket, with a peer that speaks the room cipher and RoomSeal.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation
import XCTest

@testable import FMChat

@MainActor
final class RelayHarness {
    let device: ChatDevice
    let ws = FakeWebSocketConnector()
    let relay: RelayService
    let crypto: RoomCrypto
    let server: FakeWebSocketConnection
    var accounts: [String: String] = [:]
    var lookups = 0
    private var peerSeals: [String: RoomSeal] = [:]
    private var nodeKeys: [String: Identity] = [:]
    private var keysByNode: [String: Identity] = [:]

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
        let h = RelayHarness(device: device, ws: ws, relay: relay, crypto: crypto, server: server)
        relay.nodeOwners = ChatNodeOwners(lookup: { [weak h] node in
            h?.lookups += 1
            return h?.accounts[node] ?? ""
        })
        return h
    }

    private init(device: ChatDevice, ws: FakeWebSocketConnector, relay: RelayService, crypto: RoomCrypto, server: FakeWebSocketConnection) {
        self.device = device
        self.relay = relay
        self.crypto = crypto
        self.server = server
    }

    func node(_ name: String) -> String {
        if name.isEmpty || name == relay.selfId || keysByNode[name] != nil { return name }
        if let key = nodeKeys[name] { return key.nodeId }
        let key = try! Identity.fromSeed(Ed25519.generateSeed())
        nodeKeys[name] = key
        keysByNode[key.nodeId] = key
        return key.nodeId
    }

    func sendPlain(_ plain: JSONObject) throws {
        server.receive(text: JSON.encode(.object(try crypto.encrypt(plain))))
    }

    func sendFromServer(_ plain: JSONObject) throws {
        var envelope = try crypto.encrypt(plain)
        envelope["srv"] = true
        server.receive(text: JSON.encode(.object(envelope)))
    }

    func peer(_ name: String) async throws -> RoomSeal {
        let nodeId = node(name)
        if let known = peerSeals[nodeId] { return known }
        let peer = try RoomSeal.loadOrCreate(
            device.settings.room, policy: .open, acct: nodeId, node: nodeId, secureStore: InMemorySecureStore(),
            nodeIdentity: keysByNode[nodeId])
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
        peerSeals[nodeId] = peer
        return peer
    }

    func sendProtected(_ plain: JSONObject, bind: Bool = true, signedIn: Bool = true) async throws {
        var plain = plain
        let name = plain["senderId"]?.stringValue ?? ""
        let nodeId = node(name.isEmpty ? "remote-node" : name)
        plain["senderId"] = .string(nodeId)
        for key in ["to", "reactorId"] {
            if let value = plain[key]?.stringValue, !value.isEmpty { plain[key] = .string(node(value)) }
        }
        let sender = ChatNodeOwners.normalized(plain["sender"]?.text ?? "")
        if signedIn && RelayService.ownerClaim(plain).isEmpty && !sender.isEmpty && plain[ChatFrameText.botField] == nil {
            plain["ownerUser"] = .string(sender)
        }
        let claim = RelayService.ownerClaim(plain)
        if bind && !claim.isEmpty { accounts[nodeId] = claim }
        let peer = try await peer(nodeId)
        try sendPlain(try peer.seal(plain))
    }

    func openFrames() -> [JSONObject] {
        server.sentTexts.compactMap { text -> JSONObject? in
            guard case .object(let env)? = try? JSON.parse(text), let frame = crypto.decrypt(env),
                !RoomSeal.isKeyFrame(frame)
            else { return nil }
            if !RoomSeal.isSealedFrame(frame) { return frame }
            for peer in peerSeals.values {
                if let plain = peer.unseal(frame) { return plain }
            }
            return nil
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

    func testSendingShowsTheMessageAtOnceAsADurableSealedFrame() async throws {
        let h = try await RelayHarness.start()
        XCTAssertEqual(h.relay.state, .connected)
        _ = try await h.peer("remote-node")
        try await Task.sleep(nanoseconds: 150_000_000)
        let sent = await h.relay.sendMessage("retain this message")
        XCTAssertTrue(sent)
        XCTAssertEqual(h.relay.messages("#general").map(\.text), ["retain this message"])
        let arrived = await waitUntil(2) { h.openFrames().contains { $0["type"] == "chat" } }
        XCTAssertTrue(arrived)
        let frame = try XCTUnwrap(h.server.sentJSON.last { $0["persist"] == .bool(true) && $0["notify"] == .bool(true) })
        XCTAssertEqual(frame["kind"], "cipher")
        XCTAssertTrue(RoomSeal.isSealedFrame(try XCTUnwrap(h.crypto.decrypt(frame))), "public channel chat is sealed too")
        let chat = try XCTUnwrap(h.openFrames().first { $0["type"] == "chat" })
        XCTAssertEqual(chat["text"], "retain this message")
        XCTAssertEqual(chat["channel"], "#general")
        XCTAssertEqual(chat["version"], "native-1.0")
        XCTAssertEqual(chat["platform"], "ios")
        XCTAssertNil(chat["persist"], "the retention flag rides outside the ciphertext")
        await h.close()
    }

    func testTheRelayNeverCarriesDirectOrPrivateFrames() async throws {
        let h = try await RelayHarness.start()
        _ = await h.server.waitForSent(2)
        let before = h.server.sentTexts.count
        let dm = await h.relay.sendMessage("private direct message", to: "@\(h.node("remote-node"))")
        XCTAssertFalse(dm)
        XCTAssertTrue(h.relay.messages("@\(h.node("remote-node"))").isEmpty)
        for (type, extra) in [
            ("dm", ["to": JSONValue.string(h.node("remote-node"))]), ("invite", ["channel": "#secret"]),
            ("cove-invite", ["invitee": "ada"]), ("cove-open", ["creator": "x"]), ("chat", ["channel": "#x", "private": true]),
            ("typing", ["conversation": "@someone"]),
        ] as [(String, JSONObject)] {
            var frame = h.relay.makeFrame(type)
            for (key, value) in extra { frame[key] = value }
            XCTAssertFalse(h.relay.sendFrame(frame), type)
        }
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(h.server.sentTexts.count, before, "nothing private reached the socket")
        await h.close()
    }

    func testRetainedAndLiveMessagesRenderInChronologicalOrder() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
            "type": "chat", "id": "remote-newer", "senderId": "remote-node", "sender": "Remote", "ts": 2000,
            "channel": "#general", "text": "newer remote message",
        ])
        try await h.sendProtected([
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
            try await h.sendProtected([
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
        try await h.sendProtected([
            "type": "chat", "id": "bulk-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channel": "#random", "text": "channel unread",
        ])
        try await h.sendProtected([
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
        try await h.sendProtected([
            "type": "chat", "id": "badge-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channel": "#random", "text": "first",
        ])
        try await h.sendProtected([
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
        try await h.sendProtected([
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

    func testAnUnsealedFrameIsRefusedEvenInAPublicChannel() async throws {
        let h = try await RelayHarness.start()
        try h.sendPlain([
            "type": "chat", "id": "sneaky", "senderId": .string(h.node("remote-node")), "sender": "Remote", "ts": 1000,
            "channel": "#general", "text": "not sealed",
        ])
        let logged = await waitUntil(2) { h.relay.log.contains { $0.contains("refused an unencrypted frame") } }
        XCTAssertTrue(logged)
        XCTAssertTrue(h.relay.messages("#general").isEmpty)
        await h.close()
    }

    func testAFrameTheServerInjectedShowsAsTheIntegrationNotAPerson() async throws {
        let h = try await RelayHarness.start()
        let victim = h.node("ada-phone")
        try await h.sendProtected([
            "type": "chat", "id": "ada-1", "senderId": "ada-phone", "sender": "Ada", "ownerUser": "ada", "ts": 1000,
            "channel": "#general", "text": "mine",
        ])
        _ = await waitUntil(2) { h.relay.messages("#general").count == 1 }
        try h.sendFromServer([
            "type": "chat", "id": "hook-1", "senderId": .string(victim), "sender": "Deploy bot", "ownerUser": "ada",
            "accountKind": "user", "ts": 1001, "channel": "#general", "text": "build passed",
        ])
        try h.sendFromServer([
            "type": "delete", "id": "hook-2", "senderId": .string(victim), "conversation": "#general", "target": "ada-1",
            "channel": "#general",
        ])
        try h.sendFromServer([
            "type": "chat", "id": "hook-3", "senderId": "x", "sender": "Leak", "channel": "#secret", "private": true,
            "text": "no",
        ])
        let shown = await waitUntil(2) { h.relay.messages("#general").count == 2 }
        XCTAssertTrue(shown)
        let hook = try XCTUnwrap(h.relay.messages("#general").first { $0.id == "hook-1" })
        XCTAssertEqual(hook.senderId, "app:Deploy bot")
        XCTAssertEqual(hook.senderName, "Deploy bot")
        XCTAssertFalse(hook.isSelf)
        XCTAssertTrue(h.relay.messages("#general").contains { $0.id == "ada-1" }, "an unsealed delete does nothing")
        XCTAssertTrue(h.relay.messages("#secret").isEmpty)
        XCTAssertFalse(h.relay.roster().contains { $0.id.hasPrefix("app:") })
        await h.close()
    }

    func testSealedDirectAndPrivateFramesAreDroppedOnTheRelay() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
            "type": "dm", "id": "dm-1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "to": .string(h.relay.selfId), "text": "hello you",
        ])
        try await h.sendProtected([
            "type": "chat", "id": "p-1", "senderId": "remote-node", "sender": "Remote", "ts": 1001, "channel": "#secret",
            "private": true, "text": "private",
        ])
        try await h.sendProtected([
            "type": "invite", "id": "i-1", "senderId": "remote-node", "sender": "Remote", "ts": 1002,
            "to": .string(h.relay.selfId), "channel": "#secret",
        ])
        try await h.sendProtected([
            "type": "chat", "id": "ok-1", "senderId": "remote-node", "sender": "Remote", "ts": 1003, "channel": "#general",
            "text": "public",
        ])
        let arrived = await waitUntil(2) { !h.relay.messages("#general").isEmpty }
        XCTAssertTrue(arrived)
        XCTAssertTrue(h.relay.messages("@\(h.node("remote-node"))").isEmpty)
        XCTAssertTrue(h.relay.messages("#secret").isEmpty)
        XCTAssertFalse(h.relay.channels.contains("#secret"))
        await h.close()
    }

    func testOnlyTheAuthorMayDeleteAMessage() async throws {
        let h = try await RelayHarness.start()
        try await h.sendProtected([
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
        _ = await waitUntil(2) { h.relay.roster().contains { $0.id == h.node("someone-else") } }
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
        let remote = h.node("remote-node")
        try await h.sendProtected([
            "type": "hello", "id": "h1", "senderId": "remote-node", "sender": "Remote", "ts": 1000,
            "channels": ["#general", "#ops"],
        ])
        let replied = await waitUntil(2) {
            h.openFrames().contains { $0["type"] == "hello" && $0["to"] == .string(remote) }
        }
        XCTAssertTrue(replied)
        XCTAssertTrue(h.relay.channels.contains("#ops"))
        let reply = try XCTUnwrap(h.openFrames().last { $0["type"] == "hello" && $0["to"] == .string(remote) })
        XCTAssertEqual(reply["channels"], ["#general", "#ops"])
        XCTAssertNil(reply["timeZone"], "strangers are not told this phone's time zone")
        await h.close()
    }

    func testABotLineNamesWhoeverReallyPostedIt() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        try await h.sendProtected([
            "type": "chat", "id": "bot-1", "senderId": "pat-phone", "sender": "forkbot", "bot": "forkbot", "ownerUser": "pat",
            "ts": .int(now), "channel": "#general", "text": "an answer",
        ])
        try await h.sendProtected(
            [
                "type": "chat", "id": "bot-2", "senderId": "eve-phone", "sender": "forkbot", "bot": "forkbot",
                "ownerUser": "pat", "ts": .int(now + 1), "channel": "#general", "text": "trust me",
            ], bind: false)
        let both = await waitUntil(2) { h.relay.messages("#general").count == 2 }
        XCTAssertTrue(both)
        let byId = Dictionary(uniqueKeysWithValues: h.relay.messages("#general").map { ($0.id, $0) })
        XCTAssertEqual(byId["bot-1"]?.senderName, "forkbot \u{00B7} via pat")
        XCTAssertEqual(byId["bot-2"]?.senderName, "forkbot \u{00B7} via \(guestChatName(h.node("eve-phone")))")
        XCTAssertEqual(byId["bot-2"]?.senderId, "forkbot")
        await h.close()
    }

    func testAnAccountNameIsShownOnlyOnceTheServerVouchesForTheNode() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        try await h.sendProtected(
            [
                "type": "presence", "id": "liar-1", "senderId": "liar", "sender": "Ada", "ownerUser": "ada",
                "accountKind": "user", "ts": .int(now),
            ], bind: false)
        try await h.sendProtected(
            [
                "type": "chat", "id": "liar-2", "senderId": "liar", "sender": "Ada", "ownerUser": "ada", "ts": .int(now),
                "channel": "#general", "text": "I am Ada",
            ], bind: false)
        h.accounts[h.node("bound-elsewhere")] = "bob"
        try await h.sendProtected(
            [
                "type": "chat", "id": "bob-1", "senderId": "bound-elsewhere", "sender": "Ada", "ownerUser": "ada",
                "ts": .int(now), "channel": "#general", "text": "Also Ada",
            ], bind: false)
        try await h.sendProtected([
            "type": "chat", "id": "ada-1", "senderId": "ada-phone", "sender": "Ada", "ownerUser": "Ada", "ts": .int(now + 1),
            "channel": "#general", "text": "the real Ada",
        ])
        let three = await waitUntil(2) { h.relay.messages("#general").count == 3 }
        XCTAssertTrue(three)
        let byId = Dictionary(uniqueKeysWithValues: h.relay.messages("#general").map { ($0.id, $0) })
        XCTAssertEqual(byId["liar-2"]?.senderName, guestChatName(h.node("liar")))
        XCTAssertEqual(byId["bob-1"]?.senderName, guestChatName(h.node("bound-elsewhere")))
        XCTAssertEqual(byId["ada-1"]?.senderName, "Ada")
        let groups = h.relay.rosterGroups()
        XCTAssertEqual(groups.filter { $0.account == "ada" }.map { $0.members.map(\.id) }, [[h.node("ada-phone")]])
        XCTAssertEqual(groups.first { $0.id == h.node("liar") }?.account, "")
        let lookups = h.lookups
        try await h.sendProtected([
            "type": "chat", "id": "ada-2", "senderId": "ada-phone", "sender": "Ada", "ownerUser": "ada", "ts": .int(now + 2),
            "channel": "#general", "text": "cached",
        ])
        _ = await waitUntil(2) { h.relay.messages("#general").count == 4 }
        XCTAssertEqual(h.lookups, lookups, "a node's owner is cached")
        await h.close()
    }

    func testAGuestLineTakesItsNameOnceTheServerVouchesForTheNode() async throws {
        let h = try await RelayHarness.start()
        let clock = FixedClock(epochMillis: 1_790_000_000_000)
        h.relay.nodeOwners = ChatNodeOwners(lookup: { [weak h] node in h?.accounts[node] ?? "" }, clock: clock)
        let now = Date().epochMillis
        try await h.sendProtected(
            [
                "type": "chat", "id": "launch-1", "senderId": "new-phone", "sender": "Sam", "ownerUser": "sam",
                "ts": .int(now), "channel": "#general", "text": "we launched the new feature",
            ], bind: false)
        try await h.sendProtected(
            [
                "type": "chat", "id": "liar-1", "senderId": "liar", "sender": "Ada", "ownerUser": "ada",
                "ts": .int(now + 1), "channel": "#general", "text": "I am Ada",
            ], bind: false)
        let two = await waitUntil(2) { h.relay.messages("#general").count == 2 }
        XCTAssertTrue(two)
        let node = h.node("new-phone")
        let liar = h.node("liar")
        XCTAssertEqual(h.relay.messages("#general").map(\.senderName), [guestChatName(node), guestChatName(liar)])

        h.accounts[node] = "sam"
        h.accounts[liar] = "bob"
        _ = await h.relay.nodeOwners?.account(for: node)
        _ = await h.relay.nodeOwners?.account(for: liar)
        XCTAssertEqual(h.relay.messages("#general").map(\.senderName), [guestChatName(node), guestChatName(liar)],
            "a no-owner answer is kept for the retry window")

        clock.advance(by: ChatNodeOwners.retryAfter)
        _ = await h.relay.nodeOwners?.account(for: node)
        _ = await h.relay.nodeOwners?.account(for: liar)
        XCTAssertEqual(h.relay.messages("#general").map(\.senderName), ["Sam", guestChatName(liar)])
        XCTAssertEqual(h.relay.rosterGroups().first { $0.members.contains { $0.id == node } }?.account, "sam")
        await h.close()
    }

    func testGuestLinesAreRecheckedWithoutAnotherFrame() async {
        let clock = FixedClock(epochMillis: 1_790_000_000_000)
        var bound = ""
        var calls = 0
        let owners = ChatNodeOwners(lookup: { node in
            calls += 1
            return node == "n1" ? bound : ""
        }, clock: clock)
        let renames = GuestLineRenames(sleep: { seconds in
            clock.advance(by: seconds)
            await Task.yield()
        })
        var confirmed: [GuestLineRenames.Line] = []
        owners.addAnswerObserver { node, account in confirmed += renames.confirmed(node: node, account: account) }
        _ = await owners.account(for: "n1")
        renames.note(["type": "chat", "id": "m1", "sender": "Sam"], node: "n1", claim: "Sam", owners: owners)
        renames.note(["type": "presence", "id": "p1"], node: "n1", claim: "sam", owners: owners)
        bound = "sam"
        let healed = await waitUntil(2) { !confirmed.isEmpty }
        XCTAssertTrue(healed)
        XCTAssertEqual(confirmed, [GuestLineRenames.Line(id: "m1", claim: "sam", name: "Sam")])
        XCTAssertFalse(renames.isPending("n1"))
        let settled = calls
        renames.note(["type": "chat", "id": "m2"], node: "n2", claim: "ada", owners: owners)
        let stopped = await waitUntil(2) { calls >= settled + GuestLineRenames.maxRechecks }
        XCTAssertTrue(stopped)
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(calls, settled + GuestLineRenames.maxRechecks, "rechecks stop after a bounded number of tries")
        XCTAssertTrue(renames.isPending("n2"))
    }

    func testNodeOwnersAreCachedForAnHour() async {
        let clock = FixedClock(epochMillis: 1_790_000_000_000)
        var calls = 0
        let owners = ChatNodeOwners(
            lookup: { node in
                calls += 1
                if node == "down" { throw MeshSocketError(code: 1, message: "offline") }
                return node == "n1" ? " Ada " : ""
            }, clock: clock)
        let first = await owners.account(for: "n1")
        XCTAssertEqual(first, "ada")
        XCTAssertTrue(owners.isVerified(node: "n1", claim: "ADA"))
        XCTAssertFalse(owners.isVerified(node: "n1", claim: "bob"))
        let guest = await owners.account(for: "n2")
        XCTAssertEqual(guest, "")
        XCTAssertFalse(owners.isVerified(node: "n2", claim: ""))
        XCTAssertEqual(owners.cached("n2"), "")
        _ = await owners.account(for: "n1")
        XCTAssertEqual(calls, 2)
        clock.advance(by: ChatNodeOwners.retryAfter)
        XCTAssertNil(owners.cached("n2"), "a no-owner answer is asked again after the retry window")
        XCTAssertEqual(owners.cached("n1"), "ada")
        clock.advance(by: ChatNodeOwners.ttl - ChatNodeOwners.retryAfter)
        XCTAssertNil(owners.cached("n1"))
        _ = await owners.account(for: "n1")
        XCTAssertEqual(calls, 3)
        let down = await owners.account(for: "down")
        XCTAssertNil(down)
        XCTAssertFalse(owners.isVerified(node: "down", claim: "ada"), "a failed check is a guest")
        XCTAssertEqual(owners.cached("down"), "")
        _ = await owners.account(for: "down")
        XCTAssertEqual(calls, 4, "and is not asked again at once")
        clock.advance(by: ChatNodeOwners.retryAfter)
        XCTAssertNil(owners.cached("down"))
        _ = await owners.account(for: "down")
        XCTAssertEqual(calls, 5)
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

    func testRepeatFramesFromAKnownPeerDoNotRedrawTheOnlineCount() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        try await h.sendProtected([
            "type": "presence", "id": "seen-1", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada",
            "platform": "linux", "ts": .int(now),
        ])
        let joined = await waitUntil(2) { h.relay.onlineCount == 2 }
        XCTAssertTrue(joined)
        let flag = RosterChangeFlag()
        withObservationTracking { _ = h.relay.onlineCount } onChange: { flag.set() }
        for index in 0..<5 {
            try await h.sendProtected([
                "type": "chat", "id": .string("chat-\(index)"), "senderId": "node-a", "sender": "Ada",
                "ownerUser": "ada", "platform": "linux", "text": "hi", "ts": .int(now + Int64(index)),
            ])
        }
        let delivered = await waitUntil(2) { h.relay.messages("#general").count >= 5 }
        XCTAssertTrue(delivered)
        XCTAssertFalse(flag.value, "a known peer's frames only refresh its last-seen time")
        try await h.sendProtected([
            "type": "presence", "id": "seen-2", "senderId": "node-b", "sender": "Bo", "ownerUser": "bo",
            "platform": "ios", "ts": .int(now),
        ])
        let grew = await waitUntil(2) { h.relay.onlineCount == 3 }
        XCTAssertTrue(grew)
        XCTAssertTrue(flag.value, "a new peer changes the count")
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
        XCTAssertEqual(me.members.map(\.id), [h.relay.selfId, h.node("w0F5DesktopNodeggm4")])
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

    func testPresenceReadsAPeersTimeZoneButSendsNoneOfItsOwn() async throws {
        let h = try await RelayHarness.start()
        let now = Date().epochMillis
        func ada() -> MemberGroup? { h.relay.rosterGroups().first { $0.name.lowercased() == "ada" } }
        try await h.sendProtected([
            "type": "presence", "id": "zone-1", "senderId": "node-a", "sender": "Ada", "ownerUser": "Ada",
            "ts": .int(now), "timeZone": "Asia/Kolkata",
        ])
        let heard = await waitUntil(2) { ada()?.timeZone == "Asia/Kolkata" }
        XCTAssertTrue(heard)
        XCTAssertEqual(ada()?.account, "ada")
        try await h.sendProtected([
            "type": "presence", "id": "zone-2", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada",
            "ts": .int(now + 1), "timeZone": "Not/AZone",
        ])
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertEqual(ada()?.timeZone, "Asia/Kolkata")
        let mark = h.openFrames().count
        h.relay.appLifecycleChanged(.resumed)
        _ = await waitUntil(2) { h.openFrames().count > mark }
        let presence = try XCTUnwrap(h.openFrames().dropFirst(mark).last { $0["type"] == "presence" })
        XCTAssertNil(presence["timeZone"], "a shared room is not told this phone's time zone")
        XCTAssertEqual(h.relay.roster().first?.timeZone, TimeZone.current.identifier)
        await h.close()
    }

    func testLocalTimeComesFromTheProfileFirstAndSaysHowFarAheadItIs() throws {
        XCTAssertEqual(ChatLocalTime.zone(profile: "Europe/Paris", presence: "Asia/Tokyo")?.identifier, "Europe/Paris")
        XCTAssertEqual(ChatLocalTime.zone(profile: "", presence: "Asia/Tokyo")?.identifier, "Asia/Tokyo")
        XCTAssertNil(ChatLocalTime.zone(profile: "Mars/Base", presence: ""))
        let date = Date(timeIntervalSince1970: 1_767_225_600)
        let utc = try XCTUnwrap(TimeZone(identifier: "UTC"))
        let kolkata = try XCTUnwrap(TimeZone(identifier: "Asia/Kolkata"))
        let newYork = try XCTUnwrap(TimeZone(identifier: "America/New_York"))
        XCTAssertEqual(ChatLocalTime.difference(kolkata, from: utc, at: date), "5\u{00BD} hours ahead of you")
        XCTAssertEqual(ChatLocalTime.difference(newYork, from: utc, at: date), "5 hours behind you")
        XCTAssertEqual(ChatLocalTime.difference(utc, from: utc, at: date), "Same time as you")
        let shown = ChatLocalTime.time(date, in: kolkata, locale: Locale(identifier: "en_US_POSIX"))
        XCTAssertEqual(shown.replacingOccurrences(of: "\u{202F}", with: " "), "5:30 AM")
        XCTAssertEqual(ChatLocalTime.place(newYork), "New York")
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
        XCTAssertEqual(h.relay.liveStatusFor(h.node("node-a"))?.text, "Focus time")

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
        try await h.sendProtected([
            "type": "chat", "id": "status-4", "senderId": "node-a", "sender": "Ada", "ownerUser": "ada", "ts": .int(now + 2),
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
        h.accounts["n1"] = "ada"
        _ = await h.relay.nodeOwners?.account(for: "n1")
        XCTAssertEqual(h.relay.nodeOwners?.cached("n1"), "ada")
        await h.relay.disconnect()
        h.relay.resetForRelaySwitch()
        XCTAssertNil(h.relay.nodeOwners?.cached("n1"), "another relay binds its own nodes")
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

final class RosterChangeFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var raised = false
    var value: Bool { lock.lock(); defer { lock.unlock() }; return raised }
    func set() { lock.lock(); raised = true; lock.unlock() }
}
