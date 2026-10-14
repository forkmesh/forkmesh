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
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "root"], id: "root-1"))
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
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "question"], id: "root-2"))
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
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "keep"], id: "keep-1"))
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "root"], id: "root-3"))
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
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "ship it"], id: "m-1"))
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

    @MainActor func testOnlyTheAuthorCanEdit() async throws {
        let (hub, h) = try await hub()
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "original"], id: "m-2"))
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

    @MainActor func testAnInviteJoinsAPrivateChannelAndItsMessagesAreMarkedPrivate() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("invite", ["to": .string(h.relay.selfId), "channel": "secret"]))
        let ok114 = await waitUntil { hub.channels.contains("#secret") }
        XCTAssertTrue(ok114)
        XCTAssertTrue(hub.extras.isPrivate("#secret"))
        XCTAssertNotNil(hub.extras.inviteNotice)
        hub.select("#secret")
        XCTAssertTrue(hub.isPrivateRoom)
        XCTAssertEqual(hub.badge, "end-to-end")
        XCTAssertFalse(hub.canAskForkbot("#secret"))
        let ok121 = await hub.sendChatText("psst")
        XCTAssertTrue(ok121)
        XCTAssertEqual(hub.rows.map(\.text), ["psst"])
        let ok123 = await waitUntil { h.openFrames().contains { $0["private"]?.isTrue == true && $0["text"]?.text == "psst" } }
        XCTAssertTrue(ok123)
        hub.extras.removeChannel("#secret")
        XCTAssertFalse(hub.channels.contains("#secret"))
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
        XCTAssertEqual(hub.extras.createPrivateChannel("Plans"), "#plans")
        XCTAssertTrue(hub.channels.contains("#plans"))
        XCTAssertEqual(hub.extras.addPublicChannel("lobby"), "#lobby")
        XCTAssertTrue(hub.channels.contains("#lobby"))
        let ok138 = await waitUntil { h.openFrames().contains { $0["type"]?.text == "channel" } }
        XCTAssertTrue(ok138)
        await h.close()
    }

    @MainActor func testFilesArriveAsAttachmentsWithinBudget() async throws {
        let (hub, h) = try await hub()
        let png = Data([0x89, 0x50, 0x4E, 0x47])
        try h.sendPlain(peerFrame("chat", [
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
        let entries: [JSONValue] = [
            .object(peerFrame("chat", ["channel": "#random", "text": "old news"], id: "h-1")),
            .object(peerFrame("chat", ["channel": "#hidden-room", "text": "nope", "private": true], id: "h-2")),
        ]
        try await h.sendProtected(peerFrame("history", ["to": .string(h.relay.selfId), "entries": .array(entries)]))
        let ok166 = await waitUntil { h.relay.messages("#random").count == 1 }
        XCTAssertTrue(ok166)
        XCTAssertTrue(h.relay.messages("#hidden-room").isEmpty)
        await h.close()
    }

    @MainActor func testHistoryPersistsAndRestores() async throws {
        let (hub, h) = try await hub()
        let dir = temporaryDirectory("chat-history")
        let store = ChatHistoryStore(directory: dir, delay: 0)
        hub.attachHistory(store, scope: { "scope-a" })
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "keep me"], id: "p-1"))
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

    func testReactionCatalogTranslatesBothWays() {
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{2764}"), "love")
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{2764}\u{FE0F}"), "love")
        XCTAssertEqual(ReactionCatalog.emoji(for: "check"), "\u{2705}")
        XCTAssertEqual(ReactionCatalog.value(forEmoji: "\u{1F984}"), "\u{1F984}")
        XCTAssertEqual(ReactionCatalog.label(for: "check"), "Done")
    }

    @MainActor func testReplySenderResolvesAndSendsWithoutSwitching() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected(peerFrame("hello", ["ownerUser": "pat", "nodeName": "pat-laptop", "to": .string(h.relay.selfId)], sender: "peer-9"))
        let ok220 = await waitUntil { h.relay.roster().contains { $0.id == "peer-9" } }
        XCTAssertTrue(ok220)
        let sender = ChatReplySender(relay: h.relay, extras: hub.extras, connectWait: 1)
        try await sender.reply(channel: "pat", text: "on my way")
        XCTAssertEqual(h.relay.currentConversation, "#general")
        XCTAssertEqual(h.relay.messages("@peer-9").map(\.text), ["on my way"])
        try await sender.reply(channel: "general", text: "hi all")
        XCTAssertEqual(h.relay.messages("#general").map(\.text), ["hi all"])
        do {
            try await sender.reply(channel: "nobody", text: "x")
            XCTFail("an unknown conversation must throw")
        } catch let error as ChatReplyError {
            XCTAssertEqual(error, .unknownConversation("nobody"))
        }
        XCTAssertEqual(try ChatReplyTarget.resolve("#x", channels: [], roster: []), "#x")
        XCTAssertEqual(try ChatReplyTarget.resolve("@pat", channels: [], roster: h.relay.roster()), "@peer-9")
        XCTAssertEqual(try ChatReplyTarget.resolve("@stranger", channels: [], roster: h.relay.roster()), "@stranger")
        hub.switchConversation("@pat")
        XCTAssertEqual(hub.currentConversation, "@peer-9")
        await h.close()
    }

    @MainActor func testForkbotAnswersOnlyInPublicChannels() async throws {
        let (hub, h) = try await hub()
        let api = FakeAccountApi()
        api.replies["POST /api/forkbot/chat"] = ["botMessage": "Hello from ForkBot"]
        hub.forkbot = api
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "context line"], id: "c-1"))
        let ok243 = await waitUntil { hub.rows.count == 1 }
        XCTAssertTrue(ok243)
        let ok244 = await hub.sendChatText("@forkbot what is up")
        XCTAssertTrue(ok244)
        let ok245 = await waitUntil { hub.rows.contains { $0.message.senderId == "forkbot" } }
        XCTAssertTrue(ok245)
        let body = try XCTUnwrap(api.calls.first?.2)
        XCTAssertEqual(body["context"]?.arrayValue?.count, 1)
        XCTAssertEqual(body["message"]?.text, "@forkbot what is up")
        _ = hub.extras.createPrivateChannel("quiet")
        hub.select("#quiet")
        _ = await hub.sendChatText("forkbot?")
        XCTAssertEqual(api.calls.count, 1)
        XCTAssertEqual(hub.notice?.text, "ForkBot is available only in public, controller-readable channels.")
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
