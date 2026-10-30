import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import Observation
import XCTest

@testable import FMChat

final class ChatExtrasTests: XCTestCase {
    @MainActor
    private func hub(clock: Clock = SystemClock()) async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, clock: clock), h)
    }

    private func peerFrame(_ type: String, _ fields: JSONObject, id: String = UUID().uuidString, sender: String = "peer-1")
        -> JSONObject
    {
        var frame: JSONObject = [
            "type": .string(type), "id": .string(id), "senderId": .string(sender), "sender": "Pat",
            "ts": .int(Date().epochMillis),
        ]
        for (key, value) in fields { frame[key] = value }
        return frame
    }

    @MainActor func testAPeerThreadReplyBuildsTheThreadAndStaysOutOfTheChannel() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "root"], id: "root-1"))
        let ok31 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok31)
        let sent = Date().epochMillis
        try await h.sendProtected(peerFrame("thread-reply", ["channel": "#general", "rootId": "root-1", "text": "first", "ts": .int(sent)]))
        try await h.sendProtected(peerFrame("thread-reply", ["channel": "#general", "rootId": "root-1", "text": "second", "ts": .int(sent + 1)], sender: "peer-2"))
        let ok34 = await waitUntil { hub.extras.replies(to: "root-1").count == 2 }
        XCTAssertTrue(ok34)
        XCTAssertEqual(hub.rows.count, 1)
        let summary = try XCTUnwrap(hub.rows[0].thread)
        XCTAssertEqual(summary.replyCount, 2)
        XCTAssertEqual(summary.label, "2 replies")
        XCTAssertEqual(summary.faces, ["Pat"])
        XCTAssertEqual(hub.threadRows("root-1").map(\.text), ["first", "second"])
        await h.close()
    }

    @MainActor func testReplyingInAThreadSendsAThreadReplyFrame() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("presence", [:]))
        let peerKnown = await waitUntil { h.relay.roster().count > 1 }
        XCTAssertTrue(peerKnown)
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "question"], id: "root-2"))
        let ok47 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok47)
        let sent = await hub.replyInThread(channel: "#general", rootId: "root-2", text: "answer")
        XCTAssertTrue(sent)
        XCTAssertEqual(hub.threadRows("root-2").map(\.text), ["answer"])
        XCTAssertTrue(hub.threadRows("root-2")[0].deletable)
        let ok52 = await waitUntil { !h.openFrames().filter { $0["type"]?.text == "thread-reply" }.isEmpty }
        XCTAssertTrue(ok52)
        await h.close()
    }

    @MainActor func testDeleteForMeHidesMessagesOnThisDeviceOnly() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "keep"], id: "keep-1"))
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "root"], id: "root-3"))
        try await h.sendProtected(peerFrame("thread-reply", ["channel": "#general", "rootId": "root-3", "text": "reply"], id: "reply-3"))
        let arrived = await waitUntil { hub.rows.count == 2 && hub.extras.replies(to: "root-3").count == 1 }
        XCTAssertTrue(arrived)
        XCTAssertEqual(hub.rows[1].thread?.replyCount, 1)
        hub.hide(hub.threadRows("root-3")[0].message)
        XCTAssertTrue(hub.threadRows("root-3").isEmpty)
        XCTAssertNil(hub.rows[1].thread)
        hub.hide(hub.rows[1].message)
        XCTAssertEqual(hub.rows.map(\.text), ["keep"])
        XCTAssertEqual(hub.historySnapshot().hiddenMessages, ["reply-3", "root-3"])
        XCTAssertTrue(h.openFrames().filter { $0["type"]?.text == "delete" }.isEmpty)
        await h.close()
    }

    @MainActor func testHiddenMessagesRestoreAndStayBounded() {
        let extras = ChatExtras()
        extras.restoreHiddenMessages(["a", "b", "a", ""])
        XCTAssertEqual(extras.snapshotHiddenMessages(), ["a", "b"])
        let revision = extras.revision
        extras.hideMessage("b")
        XCTAssertEqual(extras.revision, revision)
        for index in 0..<ChatLimits.hiddenMessages { extras.hideMessage("m\(index)") }
        XCTAssertEqual(extras.snapshotHiddenMessages().count, ChatLimits.hiddenMessages)
        XCTAssertFalse(extras.isHidden("a"))
        XCTAssertFalse(extras.isHidden("b"))
        XCTAssertTrue(extras.isHidden("m0"))
    }

    @MainActor func testReactionsToggleAndRefuseASpoofedReactor() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "ship it"], id: "m-1"))
        let ok59 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok59)
        let message = hub.rows[0].message
        hub.toggleReaction(message, value: "like")
        XCTAssertEqual(hub.rows[0].reactions.map(\.value), ["like"])
        XCTAssertTrue(hub.rows[0].reactions[0].mine)
        XCTAssertEqual(hub.rows[0].reactions[0].emoji, "\u{1F44D}")
        hub.toggleReaction(message, value: "like")
        XCTAssertTrue(hub.rows[0].reactions.isEmpty)
        try await h.sendProtected(
            peerFrame("reaction", ["conversation": "#general", "target": "m-1", "emoji": "\u{2764}", "reactorId": "someone-else", "added": true]))
        try await h.sendProtected(
            peerFrame("reaction", ["conversation": "#general", "target": "m-1", "emoji": "\u{2764}\u{FE0F}", "reactorId": "peer-1", "reactorName": "Pat", "added": true]))
        let ok71 = await waitUntil { !hub.rows[0].reactions.isEmpty }
        XCTAssertTrue(ok71)
        XCTAssertEqual(hub.rows[0].reactions.map(\.value), ["love"])
        XCTAssertEqual(hub.rows[0].reactions[0].names, ["Pat"])
        await h.close()
    }

    @MainActor func testStickersArriveInCatalogOrderAndNeverCollideWithTheSameEmojiReaction() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "ready to merge"], id: "m-s"))
        let shown = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(shown)
        func sticker(_ slug: String, _ emoji: String, by node: String, name: String, added: Bool = true) -> JSONObject {
            peerFrame(
                "reaction",
                [
                    "conversation": "#general", "target": "m-s", "emoji": .string(emoji), "sticker": .string(slug),
                    "reactorId": .string(node), "reactorName": .string(name), "added": .bool(added),
                ], sender: node)
        }
        try await h.sendProtected(sticker("party", "\u{1F389}", by: "peer-1", name: "Pat"))
        try await h.sendProtected(sticker("lgtm", "\u{2705}", by: "peer-1", name: "Pat"))
        try await h.sendProtected(sticker("lgtm", "\u{2705}", by: "peer-2", name: "Sam"))
        try await h.sendProtected(
            peerFrame(
                "reaction",
                ["conversation": "#general", "target": "m-s", "emoji": "\u{1F389}", "reactorId": "peer-1", "added": true]))
        let placed = await waitUntil {
            hub.rows[0].stickers.count == 2 && hub.rows[0].reactions.count == 1 && hub.rows[0].stickers.first?.count == 2
        }
        XCTAssertTrue(placed)
        XCTAssertEqual(hub.rows[0].stickers.map(\.id), ["lgtm", "party"])
        XCTAssertEqual(hub.rows[0].stickers[0].count, 2)
        XCTAssertEqual(hub.rows[0].stickers[0].countLabel, "\u{00D7}2")
        XCTAssertEqual(Set(hub.rows[0].stickers[0].names), ["Pat", "Sam"])
        XCTAssertEqual(hub.rows[0].stickers[1].countLabel, "")
        XCTAssertEqual(hub.rows[0].reactions.map(\.value), ["celebrate"])

        try await h.sendProtected(sticker("party", "\u{1F389}", by: "peer-1", name: "Pat", added: false))
        let unstuck = await waitUntil { hub.rows[0].stickers.map(\.id) == ["lgtm"] }
        XCTAssertTrue(unstuck)
        XCTAssertEqual(hub.rows[0].reactions.map(\.value), ["celebrate"], "taking the sticker off leaves the 🎉 reaction")

        var spoofed = sticker("party", "\u{1F389}", by: "intruder", name: "Mallory")
        spoofed["reactorId"] = "peer-2"
        try await h.sendProtected(spoofed)
        try await h.sendProtected(sticker("hypothetical-new", "\u{1F680}", by: "peer-2", name: "Sam"))
        let fellBack = await waitUntil { hub.rows[0].reactions.count == 2 }
        XCTAssertTrue(fellBack)
        XCTAssertEqual(hub.rows[0].reactions.map(\.value), ["celebrate", "rocket"], "an unknown sticker shows as its fallback reaction")
        XCTAssertEqual(hub.rows[0].stickers.map(\.id), ["lgtm"])
        XCTAssertEqual(hub.rows[0].stickers[0].count, 2, "a spoofed placer is refused")

        let restored = try XCTUnwrap(ChatHistoryCodec.decode(ChatHistoryCodec.encode(hub.historySnapshot())))
        XCTAssertEqual(restored.stickers["m-s"]?["lgtm"], [h.node("peer-1"): "Pat", h.node("peer-2"): "Sam"])
        await h.close()
    }

    @MainActor func testPlacingAStickerSendsAReactionFrameWithTheFallbackAndTogglesOff() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "deployed"], id: "m-t"))
        let shown = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(shown)
        try await h.sendProtected(
            peerFrame(
                "reaction",
                ["conversation": "#general", "target": "m-t", "emoji": "\u{1F680}", "sticker": "ship-it", "reactorId": "peer-1", "added": true]))
        let theirs = await waitUntil { hub.rows[0].stickers.first?.count == 1 }
        XCTAssertTrue(theirs)
        let message = hub.rows[0].message
        hub.toggleSticker(message, slug: "ship-it")
        XCTAssertEqual(hub.rows[0].stickers.first?.count, 2)
        XCTAssertEqual(hub.rows[0].stickers.first?.mine, true)
        XCTAssertTrue(hub.rows[0].reactions.isEmpty)
        let sentFrame = await waitUntil { h.openFrames().contains { $0["sticker"]?.text == "ship-it" } }
        XCTAssertTrue(sentFrame)
        let frame = try XCTUnwrap(h.openFrames().first { $0["sticker"]?.text == "ship-it" })
        XCTAssertEqual(frame["type"]?.text, "reaction")
        XCTAssertEqual(frame["emoji"]?.text, "\u{1F680}")
        XCTAssertEqual(frame["target"]?.text, "m-t")
        XCTAssertEqual(frame["reactorId"]?.text, h.relay.selfId)
        XCTAssertEqual(frame["added"], .bool(true))
        XCTAssertEqual(frame["conversation"]?.text, "#general")

        hub.toggleSticker(message, slug: "ship-it")
        XCTAssertEqual(hub.rows[0].stickers.first?.count, 1)
        XCTAssertEqual(hub.rows[0].stickers.first?.mine, false)
        hub.toggleSticker(message, slug: "not-a-sticker")
        XCTAssertTrue(hub.rows[0].reactions.isEmpty)
        await h.close()
    }

    func testStickerFieldsAndCatalog() {
        XCTAssertEqual(StickerCatalog.entry("love-it")?.fallback, "\u{2764}\u{FE0F}")
        XCTAssertEqual(StickerCatalog.entries.map(\.slug), ["lgtm", "ship-it", "nice", "thanks", "on-it", "wow", "love-it", "party"])
        XCTAssertTrue(StickerCatalog.entries.allSatisfy { $0.fallback.count <= 16 })
        let fields = ChatExtras.stickerFields(added: false, messageId: "m", sticker: StickerCatalog.entries[0], selfId: "me")
        XCTAssertEqual(
            fields, ["target": "m", "emoji": "\u{2705}", "sticker": "lgtm", "reactorId": "me", "added": false])
    }

    @MainActor func testOnlyTheAuthorCanEdit() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "original"], id: "m-2"))
        let ok80 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok80)
        try await h.sendProtected(peerFrame("edit", ["conversation": "#general", "target": "m-2", "text": "forged"], sender: "intruder"))
        try await h.sendProtected(peerFrame("edit", ["conversation": "#general", "target": "m-2", "text": "fixed"]))
        let ok83 = await waitUntil { hub.rows.first?.edited == true }
        XCTAssertTrue(ok83)
        XCTAssertEqual(hub.rows[0].text, "fixed")
        XCTAssertFalse(hub.rows[0].editable)
        _ = await hub.sendChatText("mine")
        let mine = try XCTUnwrap(hub.rows.last)
        XCTAssertTrue(mine.editable)
        XCTAssertTrue(hub.edit(mine.message, text: "mine, edited"))
        XCTAssertEqual(hub.rows.last?.text, "mine, edited")
        XCTAssertFalse(hub.edit(hub.rows[0].message, text: "not mine"))
        await h.close()
    }

    @MainActor func testTypingDoesNotRebuildTheTranscript() async throws {
        let (hub, h) = try await hub(clock: FixedClock(Date()))
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "hello"]))
        let arrived = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(arrived)
        let flag = RosterChangeFlag()
        withObservationTracking { _ = hub.rows } onChange: { flag.set() }
        try await h.sendProtected(peerFrame("typing", ["conversation": "#general", "active": true]))
        let typing = await waitUntil { hub.typingLine == "Pat is typing..." }
        XCTAssertTrue(typing)
        XCTAssertFalse(flag.value, "a typing frame only changes the typing line")
        await h.close()
    }

    @MainActor func testTypingShowsAndExpires() async throws {
        let clock = FixedClock(Date())
        let (hub, h) = try await hub(clock: clock)
        try await h.sendProtected(peerFrame("typing", ["conversation": "#general", "active": true]))
        let ok99 = await waitUntil { hub.typingLine == "Pat is typing..." }
        XCTAssertTrue(ok99)
        clock.advance(by: ChatLimits.typingTimeout + 1)
        XCTAssertEqual(hub.typingLine, "")
        XCTAssertEqual(ChatExtras.typingLabel(["A", "B"]), "A and B are typing...")
        XCTAssertEqual(ChatExtras.typingLabel(["A", "B", "C"]), "3 people are typing...")
        hub.composerChanged("hello")
        hub.composerChanged("hello again")
        let sentTyping = await waitUntil { !h.openFrames().filter { $0["type"]?.text == "typing" }.isEmpty }
        XCTAssertTrue(sentTyping)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(h.openFrames().filter { $0["type"]?.text == "typing" }.count, 1, "typing is throttled")
        await h.close()
    }

    @MainActor func testARelayInviteToAPrivateChannelIsIgnored() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("invite", ["to": .string(h.relay.selfId), "channel": "secret"]))
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "after"]))
        let arrived = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(arrived)
        XCTAssertFalse(hub.channels.contains("#secret"))
        XCTAssertFalse(hub.isPrivateConversation("#secret"))
        hub.select("#secret")
        let sent = await hub.sendChatText("psst")
        XCTAssertTrue(sent)
        let out = await waitUntil { h.openFrames().contains { $0["text"]?.text == "psst" } }
        XCTAssertTrue(out)
        XCTAssertFalse(h.openFrames().contains { $0["private"] != nil }, "nothing on the relay is marked private")
        await h.close()
    }

    @MainActor func testChannelNamesAndCreation() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("presence", [:]))
        let peerKnown = await waitUntil { h.relay.roster().count > 1 }
        XCTAssertTrue(peerKnown)
        XCTAssertTrue(ChatExtras.validChannelName("team-notes"))
        XCTAssertFalse(ChatExtras.validChannelName("#team"))
        XCTAssertFalse(ChatExtras.validChannelName("has space"))
        XCTAssertEqual(hub.extras.addPublicChannel("lobby"), "#lobby")
        XCTAssertTrue(hub.channels.contains("#lobby"))
        let ok138 = await waitUntil { h.openFrames().contains { $0["type"]?.text == "channel" } }
        XCTAssertTrue(ok138)
        await h.close()
    }

    @MainActor func testFilesArriveAsAttachmentsWithinBudget() async throws {
        let (hub, h) = try await hub()
        let png = Data([0x89, 0x50, 0x4E, 0x47])
        try await h.sendProtected(peerFrame("chat", [
            "channel": "#general", "fileName": "shot.png", "fileMime": "image/png", "file": .string(png.base64EncodedString()),
        ], id: "f-1"))
        let ok148 = await waitUntil { hub.rows.first?.attachment != nil }
        XCTAssertTrue(ok148)
        XCTAssertEqual(hub.rows[0].attachment?.kind, .image)
        XCTAssertEqual(hub.rows[0].attachment?.data, png)
        XCTAssertEqual(ChatAttachmentKind.of(fileName: "note.m4a", mime: ""), .audio)
        XCTAssertEqual(ChatAttachmentKind.of(fileName: "clip.mov", mime: ""), .video)
        XCTAssertFalse(hub.sendAttachment(fileName: "big.bin", mime: "", data: Data(count: ChatFileLimits.maxSendBytes + 1)))
        XCTAssertTrue(hub.sendAttachment(fileName: "small.txt", mime: "text/plain", data: Data("hi".utf8)))
        XCTAssertEqual(hub.rows.last?.attachment?.fileName, "small.txt")
        await h.close()
    }

    @MainActor func testHistoryFramesFromAPeerFillTheChannel() async throws {
        let (hub, h) = try await hub()
        let peer = h.node("peer-1")
        let entries: [JSONValue] = [
            .object(peerFrame("chat", ["channel": "#random", "text": "old news"], id: "h-1", sender: peer)),
            .object(peerFrame("chat", ["channel": "#hidden-room", "text": "nope", "private": true], id: "h-2", sender: peer)),
            .object(peerFrame("chat", ["channel": "#random", "text": "forged"], id: "h-3", sender: h.node("someone-else"))),
        ]
        try await h.sendProtected(peerFrame("history", ["to": .string(h.relay.selfId), "entries": .array(entries)]))
        let ok166 = await waitUntil { h.relay.messages("#random").count == 1 }
        XCTAssertTrue(ok166)
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(h.relay.messages("#random").map(\.id), ["h-1"], "a peer can only replay its own messages")
        XCTAssertTrue(h.relay.messages("#hidden-room").isEmpty)
        await h.close()
    }

    @MainActor func testHistoryPersistsAndRestores() async throws {
        let (hub, h) = try await hub()
        let dir = temporaryDirectory("chat-history")
        let store = ChatHistoryStore(directory: dir, delay: 0)
        hub.attachHistory(store, scope: { "scope-a" })
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "keep me"], id: "p-1"))
        let ok177 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok177)
        hub.toggleReaction(hub.rows[0].message, value: "rocket")
        store.save(hub.historySnapshot(), scope: "scope-a")
        await h.close()

        let (other, h2) = try await self.hub()
        other.attachHistory(ChatHistoryStore(directory: dir, delay: 0), scope: { "scope-a" })
        XCTAssertEqual(other.rows.map(\.text), ["keep me"])
        XCTAssertEqual(other.rows.first?.reactions.map(\.value), ["rocket"])
        await h2.close()
    }

    @MainActor func testSlashCommandsAreHandledBeforeSending() async throws {
        let (hub, h) = try await hub()
        let issue = await hub.sendChatText("/issue alice/tool Broken build")
        XCTAssertTrue(issue)
        XCTAssertEqual(hub.pendingIssue, "alice/tool Broken build")
        XCTAssertTrue(hub.rows.isEmpty)
        let topic = await hub.sendChatText("/topic hello")
        XCTAssertFalse(topic)
        XCTAssertEqual(hub.notice?.text, "/topic is available in managed Office channels.")
        await h.close()
    }

    func testHistoryCodecBoundsEachConversation() {
        var many: [ChatMessage] = []
        for index in 0..<(ChatLimits.historyEntriesPerChannel + 20) {
            many.append(ChatMessage(id: "m\(index)", conversation: "#general", senderId: "a", senderName: "A", text: "x",
                                    timestamp: Date(epochMillis: Int64(index))))
        }
        let mesh = ChatMessage(id: "mesh", conversation: meshConversation, senderId: "a", senderName: "A", text: "x", timestamp: Date())
        let kept = ChatHistoryCodec.bounded(["#general": many, meshConversation: [mesh]])
        XCTAssertEqual(kept.count, ChatLimits.historyEntriesPerChannel)
        XCTAssertEqual(kept.last?.id, "m\(ChatLimits.historyEntriesPerChannel + 19)")
        var snapshot = ChatHistorySnapshot()
        snapshot.messages = Array(kept.prefix(2))
        snapshot.edits = ["m0": "edited"]
        snapshot.reactions = ["m0": ["like": ["a": "A"]]]
        snapshot.privateChannels = ["#p"]
        snapshot.hiddenMessages = ["m1"]
        let round = ChatHistoryCodec.decode(ChatHistoryCodec.encode(snapshot))
        XCTAssertEqual(round, snapshot)
        XCTAssertNotEqual(ChatHistoryCodec.scopeName(serverUrl: "a", room: "b"), ChatHistoryCodec.scopeName(serverUrl: "a", room: "c"))
    }

    @MainActor func testBlocksFollowTheAccountAcrossDevices() async throws {
        let (hub, h) = try await hub()
        hub.workspace = ChatWorkspace(store: InMemoryKeyValueStore(), scope: "blocks")
        let api = FakeAccountApi()
        api.replies["GET \(ChatHub.blocksPath)"] = ["accounts": ["mallory", "Eve"]]
        hub.accountApi = api
        func lastPut() -> Set<String> {
            Set(api.calls.last { $0.0 == "PUT" && $0.1 == ChatHub.blocksPath }?.2?["accounts"]?.strings ?? [])
        }
        hub.setBlocked("zed", true)
        await hub.blocksTask?.value
        XCTAssertEqual(lastPut(), ["zed"], "a block is pushed at once")
        hub.directSessionChanged(account: "me")
        let read = await waitUntil { api.calls.contains { $0.0 == "GET" && $0.1 == ChatHub.blocksPath } }
        XCTAssertTrue(read, "the list is read at sign-in")
        let merged = await waitUntil { Set(hub.workspace.blocked) == ["mallory", "eve", "zed"] }
        XCTAssertTrue(merged)
        await hub.blocksTask?.value
        XCTAssertEqual(lastPut(), ["mallory", "eve", "zed"])
        hub.setBlocked("mallory", false)
        await hub.blocksTask?.value
        XCTAssertEqual(lastPut(), ["eve", "zed"], "an unblock is pushed too")
        await h.close()

        let (guest, h2) = try await self.hubAsGuest()
        let quiet = FakeAccountApi()
        guest.accountApi = quiet
        guest.setBlocked("zed", true)
        await guest.syncBlocks()
        XCTAssertTrue(quiet.calls.isEmpty, "a guest keeps blocks on the phone")
        await h2.close()
    }

    @MainActor private func hubAsGuest() async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, isGuest: { true }), h)
    }

    @MainActor func testACoveInviteGoesAsAServerDirectMessage() async throws {
        let (hub, h) = try await hub()
        XCTAssertEqual(
            ChatHub.coveInviteText(inviterName: "ada", coveName: "Plans", shareUrl: "https://forkmesh.com/c/1"),
            "@ada invited you to the cove \u{201C}Plans\u{201D}: https://forkmesh.com/c/1")
        let signedOut = await hub.sendDirectNote(to: "bob", text: "hello")
        XCTAssertFalse(signedOut)
        let api = FakeAccountApi()
        api.replies["POST \(DirectMessageFrames.endpoint)"] = [
            "conversation": ["id": .string(String(repeating: "c", count: 32)), "otherUser": "bob"]
        ]
        hub.direct = DirectMessageService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { h.relay.selfId }, displayName: { "Me" })
        _ = await hub.sendDirectNote(to: " Bob ", text: ChatHub.coveInviteText(inviterName: "ada", coveName: "Plans", shareUrl: "u"))
        XCTAssertEqual(api.calls.first?.2?["username"]?.text, "bob")
        XCTAssertEqual(hub.direct?.records.map(\.otherUser), ["bob"])
        XCTAssertFalse(h.openFrames().contains { ($0["type"]?.text ?? "").hasPrefix("cove-") })
        await h.close()
    }

    @MainActor func testADeleteNeedsAKnownTargetFromItsAuthor() {
        let extras = ChatExtras()
        var removed: [String] = []
        extras.onRemoved = { removed.append($1) }
        extras.authorOf = { _, id in id == "m1" ? "author" : nil }
        extras.apply([
            "type": "reaction", "senderId": "x", "reactorId": "x", "target": "m2", "emoji": "\u{1F44D}", "added": true,
            "conversation": "#general",
        ])
        extras.apply(["type": "delete", "senderId": "mallory", "conversation": "#general", "target": "m2"])
        XCTAssertEqual(extras.reactions(for: "m2", selfId: "me").count, 1, "a delete for an unknown message changes nothing")
        extras.apply(["type": "delete", "senderId": "mallory", "conversation": "#general", "target": "m1"])
        XCTAssertTrue(removed.isEmpty)
        extras.apply(["type": "delete", "senderId": "author", "conversation": "#general", "target": "m1"])
        XCTAssertEqual(removed, ["m1"])
    }

    func testABotNameCannotBorrowANodeKeyOrAnAppId() {
        let node = "SFwW-WvQ09TamVzphDQEF1oi-53Q5kae5TYaXmQquDE"
        XCTAssertEqual(ChatFrameText.shownSender(["senderId": "n1", "bot": "forkbot"]), "forkbot")
        XCTAssertEqual(ChatFrameText.shownSender(["senderId": "n1", "bot": .string(node)]), "n1")
        XCTAssertEqual(ChatFrameText.shownSender(["senderId": "n1", "bot": "app:Deploy"]), "n1")
        XCTAssertTrue(ChatFrameText.isNodeKey(node))
        XCTAssertFalse(ChatFrameText.isNodeKey("forkbot"))
    }

    func testReactionCatalogTranslatesBothWays() {
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{2764}"), "love")
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{2764}\u{FE0F}"), "love")
        XCTAssertEqual(ReactionCatalog.emoji(for: "check"), "\u{2705}")
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{1F984}"), "\u{1F984}")
        XCTAssertEqual(ReactionCatalog.label(for: "check"), "Done")
    }

    @MainActor func testReplySenderSendsPeopleThroughTheirServerDirectMessage() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("hello", ["ownerUser": "pat", "to": .string(h.relay.selfId)], sender: "peer-9"))
        try await h.sendProtected(peerFrame("hello", ["to": .string(h.relay.selfId)], sender: "guest-1"), signedIn: false)
        let peer = h.node("peer-9")
        let ok220 = await waitUntil { h.relay.roster().count == 3 }
        XCTAssertTrue(ok220)
        let api = FakeAccountApi()
        let id = String(repeating: "c", count: 32)
        api.replies["POST \(DirectMessageFrames.endpoint)"] = ["conversation": ["id": .string(id), "otherUser": "pat"]]
        let direct = DirectMessageService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { h.relay.selfId }, displayName: { "Me" })
        let sender = ChatReplySender(relay: h.relay, extras: hub.extras, direct: { direct }, connectWait: 1)
        XCTAssertEqual(try ChatReplyTarget.resolve("pat", channels: [], roster: h.relay.roster()), "@\(peer)")
        _ = try? await sender.reply(channel: "pat", text: "on my way")
        XCTAssertEqual(api.calls.first?.0, "POST")
        XCTAssertEqual(api.calls.first?.2?["username"]?.text, "pat")
        XCTAssertEqual(direct.records.map(\.otherUser), ["pat"])
        XCTAssertTrue(h.relay.messages("@\(peer)").isEmpty)
        XCTAssertFalse(h.openFrames().contains { $0["type"]?.text == "dm" || $0["text"]?.text == "on my way" })
        do {
            try await sender.reply(channel: "@\(h.node("guest-1"))", text: "x")
            XCTFail("a guest has no direct message")
        } catch let error as ChatReplyError {
            XCTAssertEqual(error, .refused)
        }
        try await sender.reply(channel: "general", text: "hi all")
        XCTAssertEqual(h.relay.messages("#general").map(\.text), ["hi all"])
        do {
            try await sender.reply(channel: "nobody", text: "x")
            XCTFail("an unknown conversation must throw")
        } catch let error as ChatReplyError {
            XCTAssertEqual(error, .unknownConversation("nobody"))
        }
        XCTAssertEqual(try ChatReplyTarget.resolve("#x", channels: [], roster: []), "#x")
        XCTAssertEqual(try ChatReplyTarget.resolve("direct:\(id)", channels: [], roster: []), "direct:\(id)")
        await h.close()
    }

    @MainActor func testMessageOnAPersonOpensTheirServerDirectMessageAndGuestsHaveNone() async throws {
        let (hub, h) = try await hub()
        let api = FakeAccountApi()
        let id = String(repeating: "d", count: 32)
        api.replies["POST \(DirectMessageFrames.endpoint)"] = ["conversation": ["id": .string(id), "otherUser": "pat"]]
        hub.direct = DirectMessageService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { h.relay.selfId }, displayName: { "Me" })
        try await h.sendProtected(peerFrame("presence", ["ownerUser": "pat"], sender: "pat-phone"))
        try await h.sendProtected(peerFrame("presence", [:], sender: "guest-2"), signedIn: false)
        let heard = await waitUntil { h.relay.roster().count == 3 }
        XCTAssertTrue(heard)
        let members = hub.channelMembers("#general")
        let pat = try XCTUnwrap(members.first { $0.account == "pat" })
        XCTAssertFalse(members.contains { $0.peerId == h.node("guest-2") && !$0.account.isEmpty })
        let guest = ChatChannelMember(name: guestChatName(h.node("guest-2")), peerId: h.node("guest-2"), online: true)
        XCTAssertTrue(hub.canMessage(pat))
        XCTAssertFalse(hub.canMessage(guest), "a guest has no Message action")
        XCTAssertFalse(hub.canMessage(ChatChannelMember(name: "Deploy", peerId: "app:Deploy")))
        let opened = await hub.message(pat)
        XCTAssertTrue(opened)
        XCTAssertEqual(hub.currentConversation, "direct:\(id)")
        XCTAssertEqual(api.calls.first?.2?["username"]?.text, "pat")
        let none = await hub.message(guest)
        XCTAssertFalse(none)
        hub.select("@\(h.node("guest-2"))")
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(hub.currentConversation, "direct:\(id)", "a relay @peer conversation is never opened")
        XCTAssertFalse(hub.directRows.contains { $0.conversation.hasPrefix("@") })
        await h.close()
    }

    @MainActor func testForkbotAnswersOnlyInPublicChannels() async throws {
        let (hub, h) = try await hub()
        let api = FakeAccountApi()
        api.replies["POST /api/forkbot/chat"] = ["botMessage": "Hello from ForkBot"]
        hub.forkbot = api
        try await h.sendProtected(peerFrame("chat", ["channel": "#general", "text": "context line"], id: "c-1"))
        let ok243 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok243)
        let ok244 = await hub.sendChatText("@forkbot what is up")
        XCTAssertTrue(ok244)
        let ok245 = await waitUntil { hub.rows.contains { $0.message.senderId == "forkbot" } }
        XCTAssertTrue(ok245)
        let body = try XCTUnwrap(api.calls.first?.2)
        XCTAssertEqual(body["context"]?.arrayValue?.count, 1)
        XCTAssertEqual(body["message"]?.text, "@forkbot what is up")
        XCTAssertFalse(hub.canAskForkbot("@someone"))
        XCTAssertFalse(hub.canAskForkbot(meshConversation))
        await h.close()
    }
}

@MainActor
final class FakeAccountApi: ChatAccountCalling {
    var calls: [(String, String, JSONObject?)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        let key = "\(method) \(path.split(separator: "?").first.map(String.init) ?? path)"
        if let failure = failures[key] { throw failure }
        return replies[key] ?? [:]
    }
}
