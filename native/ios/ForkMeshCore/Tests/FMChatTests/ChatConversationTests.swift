import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class ChatConversationTests: XCTestCase {
    @MainActor
    private func hub(clock: Clock = SystemClock()) async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, clock: clock), h)
    }

    private func peerFrame(_ type: String, _ fields: JSONObject, id: String = UUID().uuidString, sender: String = "peer-1", ts: Int64? = nil)
        -> JSONObject
    {
        var frame: JSONObject = [
            "type": .string(type), "id": .string(id), "senderId": .string(sender), "sender": "Pat",
            "ts": .int(ts ?? Date().epochMillis),
        ]
        for (key, value) in fields { frame[key] = value }
        return frame
    }

    private func row(_ id: String, sender: String, at seconds: TimeInterval, mine: Bool = false, text: String = "hi") -> ChatRow {
        ChatRow(
            message: ChatMessage(
                id: id, conversation: "@peer-1", senderId: mine ? "me" : sender, senderName: mine ? "Me" : sender,
                text: text, timestamp: Date(timeIntervalSince1970: 1_800_000_000 + seconds), isSelf: mine),
            continuesGroup: false, queued: false, deletable: mine)
    }

    private func bubbles(_ items: [ChatConversationItem]) -> [ChatBubble] {
        items.compactMap {
            if case .message(let bubble) = $0 { return bubble }
            return nil
        }
    }

    func testRunsTailsHeadersAndTheReceipt() {
        let rows = [
            row("a", sender: "pat", at: 0), row("b", sender: "pat", at: 10), row("c", sender: "pat", at: 20),
            row("d", sender: "me", at: 30, mine: true), row("e", sender: "pat", at: 40),
            row("f", sender: "me", at: 3_600, mine: true), row("g", sender: "me", at: 3_610, mine: true),
        ]
        let items = ChatConversationLayout.items(rows: rows, events: [], group: false)
        XCTAssertEqual(items.map(\.id), ["time-a", "a", "b", "c", "d", "e", "time-f", "f", "g"])
        let laid = bubbles(items)
        XCTAssertEqual(laid.map(\.position), [.first, .middle, .last, .single, .single, .first, .last])
        XCTAssertEqual(laid.map(\.receipt), ["", "", "", "", "", "", "Sent"])
        XCTAssertTrue(laid.allSatisfy { !$0.showsSender }, "a direct message never names the sender")
        let group = bubbles(ChatConversationLayout.items(rows: rows, events: [], group: true))
        XCTAssertEqual(group.map(\.showsSender), [true, false, false, false, true, false, false])
        let answered = bubbles(ChatConversationLayout.items(rows: Array(rows.prefix(5)), events: [], group: false))
        XCTAssertTrue(answered.allSatisfy { $0.receipt.isEmpty }, "no receipt once the other side has answered")
    }

    func testANewDayAlwaysGetsATimeHeader() {
        let utc = TimeZone(identifier: "UTC")!
        let rows = [row("a", sender: "pat", at: 57_540), row("b", sender: "pat", at: 57_660)]
        let items = ChatConversationLayout.items(rows: rows, events: [], group: false, timeZone: utc)
        XCTAssertEqual(items.map(\.id), ["time-a", "a", "time-b", "b"])
        XCTAssertEqual(bubbles(items).map(\.position), [.single, .single])
        let eastern = TimeZone(secondsFromGMT: -5 * 3600)!
        let sameDay = ChatConversationLayout.items(rows: rows, events: [], group: false, timeZone: eastern)
        XCTAssertEqual(sameDay.map(\.id), ["time-a", "a", "b"])
    }

    func testLookEventsSitInTimeOrderAndBreakRuns() {
        let rows = [row("a", sender: "pat", at: 0), row("b", sender: "pat", at: 20)]
        let event = ChatLookEvent(
            id: "look-1", conversation: "@peer-1", field: .background, actorName: "Pat", isSelf: false,
            timestamp: Date(timeIntervalSince1970: 1_800_000_010), cleared: false)
        let items = ChatConversationLayout.items(rows: rows, events: [event], group: false)
        XCTAssertEqual(items.map(\.id), ["time-a", "a", "look-look-1", "b"])
        XCTAssertEqual(bubbles(items).map(\.position), [.single, .single])
        XCTAssertEqual(event.text, "Pat changed the background.")
        let mine = ChatLookEvent(
            id: "x", conversation: "#crew", field: .name, actorName: "Me", isSelf: true, timestamp: Date(), cleared: false,
            value: "Weekend")
        XCTAssertEqual(mine.text, "You named the conversation \u{201C}Weekend\u{201D}.")
    }

    func testReceiptsComeFromOfficeRecipientCounts() {
        var delivered = row("a", sender: "me", at: 0, mine: true)
        delivered = ChatRow(
            message: ChatMessage(
                id: "a", conversation: "#office/crew", senderId: "me", senderName: "Me", text: "hi", timestamp: Date(),
                isSelf: true, recipients: 2), continuesGroup: false, queued: false, deletable: false)
        XCTAssertEqual(ChatConversationLayout.receipt(delivered), "Delivered")
        XCTAssertEqual(ChatConversationLayout.receipt(row("b", sender: "me", at: 0, mine: true)), "Sent")
        let queued = ChatRow(message: delivered.message, continuesGroup: false, queued: true, deletable: false)
        XCTAssertEqual(ChatConversationLayout.receipt(queued), "Not delivered yet")
    }

    func testJumboEmoji() {
        XCTAssertTrue(ChatConversationLayout.isJumboEmoji("\u{1F602}"))
        XCTAssertTrue(ChatConversationLayout.isJumboEmoji(" \u{2764}\u{FE0F}\u{1F44D} "))
        XCTAssertTrue(ChatConversationLayout.isJumboEmoji("\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F467}"))
        XCTAssertFalse(ChatConversationLayout.isJumboEmoji("\u{1F602}\u{1F602}\u{1F602}\u{1F602}"))
        XCTAssertFalse(ChatConversationLayout.isJumboEmoji("ok \u{1F602}"))
        XCTAssertFalse(ChatConversationLayout.isJumboEmoji("1"))
        XCTAssertFalse(ChatConversationLayout.isJumboEmoji("#"))
        XCTAssertFalse(ChatConversationLayout.isJumboEmoji(""))
    }

    func testTimeLabels() {
        let zone = TimeZone(identifier: "UTC")!
        let now = Date(timeIntervalSince1970: 1_790_000_000)
        XCTAssertEqual(ChatConversationLayout.timeLabel(now.addingTimeInterval(-60), now: now, timeZone: zone), "Today 2:12 PM")
        XCTAssertEqual(ChatConversationLayout.timeLabel(now.addingTimeInterval(-86_400), now: now, timeZone: zone), "Yesterday 2:13 PM")
        XCTAssertEqual(ChatConversationLayout.timeLabel(now.addingTimeInterval(-3 * 86_400), now: now, timeZone: zone), "Friday 2:13 PM")
        XCTAssertEqual(ChatConversationLayout.timeLabel(now.addingTimeInterval(-30 * 86_400), now: now, timeZone: zone), "Aug 22 at 2:13 PM")
        XCTAssertEqual(
            ChatConversationLayout.timeLabel(now.addingTimeInterval(-400 * 86_400), now: now, timeZone: zone), "Aug 17, 2025 at 2:13 PM")
    }

    func testLookFramesRoundTripAndTheLatestChangeWins() throws {
        let picture = Data(repeating: 7, count: 300)
        let fields = ChatLookFrames.fields(ChatLookChange(field: .background, preset: "sunset", image: picture))
        XCTAssertNil(fields["preset"], "a picture replaces the preset")
        var frame: JSONObject = ["type": .string(ChatLookFrames.type)]
        for (key, value) in fields { frame[key] = value }
        let change = try XCTUnwrap(ChatLookFrames.change(from: frame))
        XCTAssertEqual(change.image, picture)
        XCTAssertFalse(change.cleared)
        let preset = try XCTUnwrap(
            ChatLookFrames.change(from: ["type": "look", "field": "background", "preset": "ocean"]))
        XCTAssertEqual(preset.preset, "ocean")
        let unknown = try XCTUnwrap(ChatLookFrames.change(from: ["type": "look", "field": "background", "preset": "nope"]))
        XCTAssertTrue(unknown.cleared)
        XCTAssertNil(ChatLookFrames.change(from: ["type": "look", "field": "colour"]))
        let huge = Data(repeating: 1, count: ChatLookFrames.maxImageBytes + 1).base64EncodedString()
        XCTAssertNil(ChatLookFrames.change(from: ["type": "look", "field": "photo", "image": .string(huge)]))
        XCTAssertEqual(
            ChatLookFrames.change(from: ["type": "look", "field": "name", "name": "  Road\n trip  "])?.name, "Road trip")

        let first = try XCTUnwrap(ChatLookFrames.applying(preset, to: ChatLook(), stamp: ChatLookStamp(ts: 10, id: "b")))
        XCTAssertTrue(first.hasBackground)
        XCTAssertNil(ChatLookFrames.applying(change, to: first, stamp: ChatLookStamp(ts: 9, id: "z")))
        XCTAssertNil(ChatLookFrames.applying(change, to: first, stamp: ChatLookStamp(ts: 10, id: "a")))
        let second = try XCTUnwrap(ChatLookFrames.applying(change, to: first, stamp: ChatLookStamp(ts: 10, id: "c")))
        XCTAssertEqual(second.backgroundImage, picture)
        XCTAssertEqual(second.backgroundPreset, "")
    }

    @MainActor func testAPeerSharesADirectMessageBackgroundAndItPersists() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("hello", ["to": .string(h.relay.selfId)], sender: "peer-9"))
        let known = await waitUntil { h.relay.roster().contains { $0.id == "peer-9" } }
        XCTAssertTrue(known)
        try await h.sendProtected(
            peerFrame(
                "look", ["field": "background", "preset": "sunset", "conversation": "@someone-else", "to": "someone-else"],
                sender: "peer-9"))
        try await h.sendProtected(
            peerFrame(
                "look",
                ["field": "background", "preset": "beach", "conversation": .string("@\(h.relay.selfId)"), "to": .string(h.relay.selfId)],
                id: "look-beach", sender: "peer-9"))
        let applied = await waitUntil { hub.extras.look(for: "@peer-9").backgroundPreset == "beach" }
        XCTAssertTrue(applied)
        XCTAssertEqual(hub.extras.look(for: "@someone-else"), ChatLook(), "a DM frame for someone else is ignored")
        hub.select("@peer-9")
        XCTAssertTrue(hub.inConversation)
        XCTAssertFalse(hub.inGroupConversation)
        XCTAssertEqual(hub.conversationLook.backgroundPreset, "beach")
        XCTAssertEqual(hub.extras.lookEvents(in: "@peer-9").map(\.text), ["Pat changed the background."])

        let snapshot = hub.historySnapshot()
        let restored = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(snapshot)))
        XCTAssertEqual(restored.looks["@peer-9"]?.backgroundPreset, "beach")
        XCTAssertEqual(restored.lookEvents.map(\.id), ["look-beach"])
        let fresh = ChatExtras()
        fresh.restoreLooks(restored.looks, events: restored.lookEvents)
        XCTAssertEqual(fresh.look(for: "@peer-9").backgroundPreset, "beach")
        XCTAssertEqual(fresh.lookEvents(in: "@peer-9").count, 1)
        await h.close()
    }

    @MainActor func testChangingTheBackgroundAndGroupNameSendsSealedLookFrames() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("presence", [:]))
        let peerKnown = await waitUntil { h.relay.roster().count > 1 }
        XCTAssertTrue(peerKnown)
        guard let channel = hub.extras.createPrivateChannel("crew") else { return XCTFail("channel") }
        hub.select(channel)
        XCTAssertTrue(hub.inGroupConversation)
        XCTAssertTrue(hub.canChangeLook)
        let sent = await hub.setLook(ChatLookChange(field: .background, preset: "forest"))
        XCTAssertTrue(sent)
        XCTAssertEqual(hub.conversationLook.backgroundPreset, "forest")
        let named = await hub.setLook(ChatLookChange(field: .name, name: "Road trip"))
        XCTAssertTrue(named)
        XCTAssertEqual(hub.conversationName, "Road trip")
        let frames = await waitUntil { h.openFrames().filter { $0["type"]?.text == "look" }.count == 2 }
        XCTAssertTrue(frames)
        let look = try XCTUnwrap(h.openFrames().first { $0["type"]?.text == "look" })
        XCTAssertEqual(look["channel"]?.text, "#crew")
        XCTAssertEqual(look["private"]?.isTrue, true)
        XCTAssertEqual(
            hub.conversationItems.compactMap {
                if case .event(let event) = $0 { return event.text }
                return nil
            }, ["You changed the background.", "You named the conversation \u{201C}Road trip\u{201D}."])
        hub.leaveGroupConversation()
        XCTAssertEqual(hub.currentConversation, "#general")
        XCTAssertEqual(hub.extras.look(for: "#crew"), ChatLook())
        await h.close()
    }

    @MainActor func testPhotosAreCappedAndPublicChannelsAreNotConversations() async throws {
        let (hub, h) = try await hub()
        hub.select("#general")
        XCTAssertFalse(hub.inConversation)
        guard let channel = hub.extras.createPrivateChannel("pics") else { return XCTFail("channel") }
        hub.select(channel)
        let tooBig = await hub.sendMedia(
            fileName: "big.jpg", mime: "image/jpeg", data: Data(repeating: 1, count: ChatConversationLayout.maxMediaBytes + 1))
        XCTAssertFalse(tooBig)
        let ok = await hub.sendMedia(fileName: "small.jpg", mime: "image/jpeg", data: Data(repeating: 1, count: 1024))
        XCTAssertTrue(ok)
        XCTAssertEqual(hub.conversationPhotos.map(\.fileName), ["small.jpg"])
        hub.clearConversationHistory()
        XCTAssertTrue(hub.rows.isEmpty)
        await h.close()
    }

    @MainActor func testAPhotoSentInADirectMessageShowsInTheSendersOwnTranscript() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("hello", ["to": .string(h.relay.selfId)], sender: "peer-9"))
        let known = await waitUntil { h.relay.roster().contains { $0.id == "peer-9" } }
        XCTAssertTrue(known)
        hub.select("@peer-9")
        let sent = await hub.sendMedia(fileName: "Audio Message.m4a", mime: "audio/mp4", data: Data(repeating: 2, count: 2048))
        XCTAssertTrue(sent)
        XCTAssertEqual(hub.rows.map(\.message.fileName), ["Audio Message.m4a"])
        XCTAssertEqual(hub.rows.first?.attachment?.kind, .audio)
        XCTAssertEqual(hub.rows.first?.message.isSelf, true)
        let frame = await waitUntil { h.openFrames().contains { $0["type"]?.text == "dm" && $0["fileName"]?.text == "Audio Message.m4a" } }
        XCTAssertTrue(frame)
        await h.close()
    }

    @MainActor func testOfficeRoomsCarryLookAndPhotoFramesBothWays() async throws {
        let roomId = String(repeating: "b", count: 32)
        let api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = ["channels": [["id": .string(roomId), "name": "crew", "visibility": "public"]]]
        let room = "chat-channel:\(roomId):v1"
        api.replies["GET /api/chat/channels/\(roomId)/room-access"] = [
            "room": .string(room), "passphrase": "office-pass", "webSocketUrl": .string("/api/chat/channels/\(roomId)/ws?ticket=t"),
        ]
        let crypto = await Task.detached { RoomCrypto.withPassphrase(room, "office-pass") }.value
        let picture = Data(repeating: 9, count: 64)
        let now = Date().epochMillis
        let earlier = try crypto.encrypt([
            "type": "look", "id": "look-old", "senderId": "peer", "sender": "Pat", "ts": .int(now - 2_000), "field": "background", "preset": "mint",
        ])
        let photo = try crypto.encrypt([
            "type": "chat", "id": "pic-1", "senderId": "peer", "sender": "Pat", "ts": .int(now - 1_000), "fileName": "beach.jpg",
            "fileMime": "image/jpeg", "file": .string(picture.base64EncodedString()),
        ])
        api.replies["GET /api/chat/channels/\(roomId)/history"] = [
            "room": .string(room), "passphrase": "office-pass",
            "messages": [["ts": .int(now - 2_000), "body": .string(JSON.encode(earlier))], ["ts": .int(now - 1_000), "body": .string(JSON.encode(photo))]],
        ]
        let ws = FakeWebSocketConnector()
        let service = OfficeChannelService(
            api: api, connector: ws, secureStore: InMemorySecureStore(), origin: { URL(string: "https://app.next.forkmesh.com") },
            selfId: { "me-node" }, displayName: { "Me" })
        let (hub, h) = try await hub()
        hub.office = service
        await service.refresh()
        await service.loadHistory("#office/crew")
        XCTAssertEqual(hub.extras.look(for: "#office/crew").backgroundPreset, "mint")
        XCTAssertEqual(hub.extras.attachment(for: "pic-1")?.data, picture)
        hub.select("#office/crew")
        XCTAssertEqual(hub.rows.first?.attachment?.fileName, "beach.jpg")
        let opened = await waitUntil { service.isConnected("#office/crew") }
        XCTAssertTrue(opened)

        let posted = await service.postFrame(
            "#office/crew", type: ChatLookFrames.type, fields: ChatLookFrames.fields(ChatLookChange(field: .name, name: "Crew")))
        XCTAssertNotNil(posted)
        XCTAssertEqual(hub.extras.look(for: "#office/crew").name, "Crew")
        XCTAssertEqual(hub.extras.lookEvents(in: "#office/crew").last?.isSelf, true)
        let connection = try XCTUnwrap(ws.lastConnection)
        let sent = await waitUntil {
            connection.sentTexts.contains { crypto.decrypt(JSON.parseObject($0))?["type"]?.text == ChatLookFrames.type }
        }
        XCTAssertTrue(sent)
        XCTAssertEqual(service.messages("#office/crew").count, 1, "a look frame is not a chat message")
        await service.closeAll()
        await h.close()
    }
}
