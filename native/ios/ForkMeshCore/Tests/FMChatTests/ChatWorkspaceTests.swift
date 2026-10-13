import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class ChatWorkspaceTests: XCTestCase {
    func testRichTextSpansFollowSlackMarkers() {
        let spans = ChatRichText.spans("*bold* and _it_ ~gone~ `a *b*` **strong**")
        XCTAssertEqual(
            spans,
            [
                ChatRichSpan(text: "bold", bold: true), ChatRichSpan(text: " and "), ChatRichSpan(text: "it", italic: true),
                ChatRichSpan(text: " "), ChatRichSpan(text: "gone", strike: true), ChatRichSpan(text: " "),
                ChatRichSpan(text: "a *b*", code: true), ChatRichSpan(text: " "), ChatRichSpan(text: "strong", bold: true),
            ])
        XCTAssertEqual(ChatRichText.spans("*_both_*"), [ChatRichSpan(text: "both", bold: true, italic: true)])
        for plain in ["snake_case_name", "2*3*4", "* bullet *", "a * b", "path/to_file_x", "*open\nclose*", "**"] {
            XCTAssertFalse(ChatRichText.hasFormatting(plain), plain)
        }
        XCTAssertEqual(ChatRichText.spans("(see *this*)."), [
            ChatRichSpan(text: "(see "), ChatRichSpan(text: "this", bold: true), ChatRichSpan(text: ")."),
        ])
    }

    func testQuoteBlocksFormattingAndForwarding() {
        XCTAssertEqual(
            ChatRichText.blocks("hi\n> quoted\n>more\nafter"),
            [
                ChatTextBlock(quote: false, text: "hi"), ChatTextBlock(quote: true, text: "quoted\nmore"),
                ChatTextBlock(quote: false, text: "after"),
            ])
        XCTAssertEqual(ChatRichText.apply(.bold, to: " hi "), "*hi*")
        XCTAssertEqual(ChatRichText.apply(.bold, to: "*hi*"), "hi")
        XCTAssertEqual(ChatRichText.apply(.code, to: "x"), "`x`")
        XCTAssertEqual(ChatRichText.apply(.codeBlock, to: "let a = 1"), "```\nlet a = 1\n```")
        XCTAssertEqual(ChatRichText.apply(.codeBlock, to: "```\nlet a = 1\n```"), "let a = 1")
        XCTAssertEqual(ChatRichText.apply(.quote, to: "a\nb"), "> a\n> b")
        XCTAssertEqual(ChatRichText.apply(.quote, to: "> a\n> b"), "a\nb")
        XCTAssertEqual(ChatRichText.apply(.bullets, to: "a\nb"), "\u{2022} a\n\u{2022} b")
        XCTAssertEqual(ChatRichText.apply(.italic, to: "  "), "  ")
        XCTAssertEqual(
            ChatRichText.forwardText(comment: "look", sender: "Pat", place: "#ops", text: "line one\nline two"),
            "look\n> line one\n> line two\n> \u{2014} Pat in #ops")
        XCTAssertEqual(
            ChatRichText.forwardText(comment: "", sender: "Pat", place: "", text: "", fileName: "a.pdf"),
            "> \u{1F4CE} a.pdf\n> \u{2014} Pat")
    }

    func testEmojiShortcodesAndChannelTokens() {
        XCTAssertEqual(ChatEmojiShortcodes.expand("ship it :rocket: :+1: at 10:30 :nope:"), "ship it \u{1F680} \u{1F44D} at 10:30 :nope:")
        XCTAssertEqual(ChatEmojiShortcodes.candidates("thu").map(\.name), ["thumbsup", "thumbsdown"])
        XCTAssertEqual(ChatEmojiShortcodes.candidates("think").count, 1, "aliases of one emoji are shown once")
        XCTAssertEqual(ChatComposerTools.emojiToken("nice :ro")?.query, "ro")
        XCTAssertNil(ChatComposerTools.emojiToken("nice :r"), "one letter is too little to suggest")
        XCTAssertNil(ChatComposerTools.emojiToken("at 10:30"))
        XCTAssertEqual(ChatComposerTools.insertEmoji("nice :ro", emoji: "\u{1F680}"), "nice \u{1F680} ")
        XCTAssertEqual(ChatComposerTools.channelToken("see #ge")?.query, "ge")
        XCTAssertNil(ChatComposerTools.channelToken("issue#12"))
        XCTAssertEqual(ChatComposerTools.insertChannel("see #ge", name: "general"), "see #general ")
        XCTAssertEqual(
            ChatComposerTools.channelCandidates(["#general", "#office/design", "#gear", "#General"], query: "ge"), ["general", "gear"])
        XCTAssertEqual(ChatComposerTools.channelCandidates(["#office/design"], query: "des"), ["office/design"])
    }

    func testSlackSlashCommands() {
        XCTAssertEqual(ChatComposerTools.parseSlash("/msg @pat see you there"), .message(to: "pat", text: "see you there"))
        XCTAssertEqual(ChatComposerTools.parseSlash("/dm sam"), .message(to: "sam", text: ""))
        XCTAssertEqual(ChatComposerTools.parseSlash("/shrug"), .shrug(ChatComposerTools.shrug))
        XCTAssertEqual(ChatComposerTools.parseSlash("/shrug oh well"), .shrug("oh well \(ChatComposerTools.shrug)"))
        XCTAssertEqual(ChatComposerTools.parseSlash("/me waves"), .me("_waves_"))
        XCTAssertNil(ChatComposerTools.parseSlash("/me"))
        XCTAssertEqual(ChatComposerTools.parseSlash("/MUTE"), .mute)
        XCTAssertEqual(ChatComposerTools.parseSlash("/search from:pat deploy"), .search("from:pat deploy"))
        XCTAssertEqual(ChatComposerTools.parseSlash("/saved"), .saved)
        XCTAssertEqual(ChatComposerTools.slashCommands("/s", canSetTopic: false).map(\.command), ["/search", "/saved", "/shrug"])
        XCTAssertEqual(ChatComposerTools.slashCommands("/m", canSetTopic: false).map(\.command), ["/msg", "/mute", "/me"])
    }

    @MainActor func testWorkspaceKeepsSavedMutedStarredDraftsAndRecents() {
        let store = InMemoryKeyValueStore()
        let workspace = ChatWorkspace(store: store, scope: "next")
        let item = ChatSavedItem(
            id: "m1", conversation: "#ops", place: "#ops", senderName: "Pat", text: "deploy at 5",
            sentAt: Date(timeIntervalSince1970: 1_800_000_000), savedAt: Date(timeIntervalSince1970: 1_800_000_100))
        workspace.save(item)
        workspace.save(item)
        workspace.setMuted("#random", true)
        workspace.setStarred("#ops", true)
        workspace.setStarred("direct:abc", true)
        workspace.setDraft("half a thought", for: "#ops")
        workspace.setDraft("   ", for: "#random")
        workspace.noteRecipients([.person("Sam"), ChatRecipient(kind: .channel, name: "#ops", conversation: "#ops"), .person("@pat")])
        workspace.noteRecipients([.person("sam")])
        XCTAssertEqual(store.stringList(forKey: "chat/workspace/next/muted"), ["#random"])

        let reloaded = ChatWorkspace(store: store, scope: "next")
        XCTAssertEqual(reloaded.saved, [item])
        XCTAssertTrue(reloaded.isMuted("#random"))
        XCTAssertEqual(reloaded.starred, ["#ops", "direct:abc"])
        XCTAssertEqual(reloaded.draft(for: "#ops"), "half a thought")
        XCTAssertFalse(reloaded.hasDraft("#random"))
        XCTAssertEqual(reloaded.recentPeople, ["sam", "pat"])
        reloaded.unsave("m1")
        reloaded.forget(conversation: "#ops")
        XCTAssertTrue(reloaded.saved.isEmpty)
        XCTAssertFalse(reloaded.isStarred("#ops"))
        XCTAssertEqual(ChatWorkspace(store: store, scope: "next").draft(for: "#ops"), "")
        XCTAssertTrue(ChatWorkspace(store: store, scope: "other").saved.isEmpty, "each project keeps its own")
    }

    func testDirectoryMergesSourcesAndRanksMatches() {
        var source = ChatDirectorySource()
        source.own = ["Me Phone", "me"]
        source.recents = ["sam"]
        source.roster = [
            MemberGroup(name: "Me Phone", members: [Member(id: "self", name: "Me Phone", isSelf: true, online: true)], isSelf: true),
            MemberGroup(name: "Pat Smith", members: [Member(id: "peer-1", name: "Pat Smith", online: true, owner: "pat")]),
            MemberGroup(name: "Guest 1234", members: [Member(id: "peer-2", name: "Guest 1234", online: true)]),
        ]
        source.directs = [DirectConversation(id: String(repeating: "d", count: 32), otherUser: "pat")]
        source.officeMembers = ["me", "sam", "patricia", "forkbot"]
        source.senders = ["Pat Smith", "Zed"]
        source.searched = ["paula"]
        source.channels = [
            ChatDirectoryChannel(conversation: "#general", name: "#general"),
            ChatDirectoryChannel(conversation: "#office/patterns", name: "#patterns", detail: "4 members"),
        ]
        let people = ChatDirectory.people(source)
        XCTAssertEqual(people.map(\.handle), ["sam", "pat", "patricia", "Zed", "paula"])
        let pat = people[1]
        XCTAssertEqual(pat.name, "Pat Smith")
        XCTAssertEqual(pat.peerId, "peer-1")
        XCTAssertEqual(pat.conversation, "direct:\(String(repeating: "d", count: 32))")
        XCTAssertEqual(pat.detail, "Online")

        let pa = ChatDirectory.suggestions(source, query: "pa")
        XCTAssertEqual(pa.map(\.label), ["Pat Smith", "patricia", "paula", "#patterns"])
        XCTAssertEqual(ChatDirectory.suggestions(source, query: "smi").map(\.label), ["Pat Smith"], "a later word matches")
        XCTAssertEqual(ChatDirectory.suggestions(source, query: "#pa").map(\.label), ["#patterns"])
        XCTAssertEqual(ChatDirectory.suggestions(source, query: "@pa").map(\.label), ["Pat Smith", "patricia", "paula"])
        XCTAssertEqual(ChatDirectory.suggestions(source, query: "pa", selected: [pat]).map(\.label), ["patricia", "paula", "#patterns"])
        XCTAssertEqual(ChatDirectory.suggestions(source, query: "", selected: [pat]).map(\.label), ["sam", "patricia", "Zed", "paula"])
        XCTAssertEqual(ChatDirectory.exact(source, name: "@PAT")?.peerId, "peer-1")
        XCTAssertEqual(ChatDirectory.exact(source, name: "#patterns")?.conversation, "#office/patterns")
        XCTAssertNil(ChatDirectory.exact(source, name: "nobody"))
    }

    func testSearchQueriesMentionsAndSnippets() {
        let query = ChatSearch.parse("from:@Pat in:#ops deploy has:link")
        XCTAssertEqual(query.from, "pat")
        XCTAssertEqual(query.place, "ops")
        XCTAssertEqual(query.terms, ["deploy"])
        XCTAssertTrue(query.hasLink)
        let message = ChatMessage(
            id: "1", conversation: "#ops", senderId: "p", senderName: "Pat Smith", text: "", timestamp: Date())
        XCTAssertTrue(ChatSearch.matches(message, text: "Deploy notes https://x.dev", place: "#ops", query: query))
        XCTAssertFalse(ChatSearch.matches(message, text: "Deploy notes", place: "#ops", query: query))
        XCTAssertFalse(ChatSearch.matches(message, text: "Deploy https://x.dev", place: "#general", query: query))
        XCTAssertFalse(ChatSearch.matches(message, text: "anything", place: "#ops", query: ChatSearch.parse("  ")))
        var file = message
        file.fileName = "Report.pdf"
        XCTAssertTrue(ChatSearch.matches(file, text: "", place: "#ops", query: ChatSearch.parse("report has:file")))

        XCTAssertTrue(ChatSearch.mentions("hey @Me, look", names: ["me"]))
        XCTAssertTrue(ChatSearch.mentions("@here standup", names: []))
        XCTAssertFalse(ChatSearch.mentions("hey @meg", names: ["me"]))
        XCTAssertFalse(ChatSearch.mentions("mail me@me.com", names: ["me"]))

        let long = String(repeating: "word ", count: 60) + "needle " + String(repeating: "tail ", count: 60)
        let snippet = ChatSearch.snippet(long, terms: ["needle"])
        XCTAssertTrue(snippet.contains("needle"))
        XCTAssertTrue(snippet.hasPrefix("\u{2026}") && snippet.hasSuffix("\u{2026}"))
        XCTAssertEqual(ChatSearch.snippet("short\ntext", terms: []), "short text")
    }

    func testReminderTimes() {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let wednesday = Date(timeIntervalSince1970: 1_790_000_000 + 2 * 86_400)
        XCTAssertEqual(calendar.component(.weekday, from: wednesday), 2 + 2)
        XCTAssertEqual(ChatReminderOption.oneHour.fireDate(from: wednesday, calendar: calendar), wednesday.addingTimeInterval(3_600))
        let tomorrow = ChatReminderOption.tomorrow.fireDate(from: wednesday, calendar: calendar)
        XCTAssertEqual(calendar.component(.hour, from: tomorrow), 9)
        XCTAssertEqual(calendar.dateComponents([.day], from: calendar.startOfDay(for: wednesday), to: tomorrow).day, 1)
        let monday = ChatReminderOption.nextWeek.fireDate(from: wednesday, calendar: calendar)
        XCTAssertEqual(calendar.component(.weekday, from: monday), 2)
        XCTAssertEqual(calendar.component(.hour, from: monday), 9)
        XCTAssertGreaterThan(monday, wednesday)
        let alreadyMonday = calendar.date(byAdding: .day, value: 5, to: wednesday)!
        let following = ChatReminderOption.nextWeek.fireDate(from: alreadyMonday, calendar: calendar)
        XCTAssertEqual(calendar.dateComponents([.day], from: calendar.startOfDay(for: alreadyMonday), to: following).day, 7)
    }
}

@MainActor
final class ChatNewMessageTests: XCTestCase {
    static let conversationId = String(repeating: "d", count: 32)

    private func hub(guest: Bool = false) async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, isGuest: { guest }), h)
    }

    private func directService(_ api: FakeAccountApi) -> DirectMessageService {
        DirectMessageService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.next.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
    }

    func testANewMessageToSomeoneStartsAServerDirectMessage() async throws {
        let (hub, h) = try await hub()
        let api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages/users"] = ["users": [["name": "Sam"], ["name": "samira"]]]
        api.replies["POST /api/chat/direct-messages"] = [
            "conversation": ["id": .string(ChatNewMessageTests.conversationId), "otherUser": "sam"]
        ]
        hub.direct = directService(api)
        XCTAssertTrue(hub.canSearchAccounts)
        let found = await hub.searchAccounts("@sa")
        XCTAssertEqual(found, ["sam", "samira"])
        XCTAssertEqual(hub.recipientSuggestions("sa", searched: found).map(\.handle), ["sam", "samira"])
        let opened = await hub.sendNewMessage(to: [.person("Sam")], text: "")
        XCTAssertTrue(opened)
        XCTAssertEqual(hub.currentConversation, "direct:\(ChatNewMessageTests.conversationId)")
        XCTAssertEqual(api.calls.first { $0.0 == "POST" }?.2?["username"]?.text, "sam")
        XCTAssertEqual(hub.workspace.recentPeople, ["sam"])
        XCTAssertEqual(hub.directRows.map(\.name), ["sam"])
        XCTAssertEqual(hub.conversationLabel(hub.currentConversation), "@sam")

        api.calls = []
        let again = await hub.sendNewMessage(to: [.person("sam")], text: "")
        XCTAssertTrue(again)
        XCTAssertTrue(
            api.calls.filter { $0.0 == "POST" && $0.1 == DirectMessageFrames.endpoint }.isEmpty, "an existing conversation is reused")
        await h.close()
    }

    func testWithoutAnAccountAnOnlinePeerGetsARelayDirectMessage() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected([
            "type": "presence", "id": "p1", "senderId": "peer-1", "sender": "Pat Smith", "ownerUser": "pat",
            "ts": .int(Date().epochMillis),
        ])
        let known = await waitUntil { h.relay.roster().count > 1 }
        XCTAssertTrue(known)
        let suggestion = try XCTUnwrap(hub.recipientSuggestions("pat").first)
        XCTAssertEqual(suggestion.peerId, "peer-1")
        XCTAssertEqual(suggestion.detail, "Online")
        let sent = await hub.sendNewMessage(to: [suggestion], text: "hi :wave:")
        XCTAssertTrue(sent)
        XCTAssertEqual(hub.currentConversation, "@peer-1")
        XCTAssertEqual(hub.rows.last?.text, "hi \u{1F44B}")
        XCTAssertEqual(hub.directRows.map(\.conversation), ["@peer-1"])
        XCTAssertEqual(hub.searchMessages("from:zed hi").count, 0)
        XCTAssertEqual(hub.searchMessages("hi in:pat").map(\.place), ["@Pat Smith"])
        await h.close()
    }

    func testSlashCommandsMuteSearchShrugAndMessage() async throws {
        let (hub, h) = try await hub()
        hub.select("#ops")
        let r1 = await hub.sendChatText("/mute")
        XCTAssertTrue(r1)
        XCTAssertTrue(hub.workspace.isMuted("#ops"))
        XCTAssertEqual(hub.notice?.text, "#ops is muted.")
        XCTAssertFalse(hub.isUnread("#ops"))
        let r2 = await hub.sendChatText("/search deploy")
        XCTAssertTrue(r2)
        XCTAssertEqual(hub.pendingSearch, "deploy")
        let r3 = await hub.sendChatText("/saved")
        XCTAssertTrue(r3)
        XCTAssertTrue(hub.pendingSaved)
        let r4 = await hub.sendChatText("/shrug fine")
        XCTAssertTrue(r4)
        XCTAssertEqual(hub.rows.last?.text, "fine \(ChatComposerTools.shrug)")
        XCTAssertEqual(hub.searchMessages("fine").map(\.place), ["#ops"])

        hub.workspace.setDraft("left over", for: "#ops")
        let r5 = await hub.sendChatText("/me ships it")
        XCTAssertTrue(r5)
        XCTAssertEqual(hub.rows.last?.text, "_ships it_")
        XCTAssertFalse(hub.workspace.hasDraft("#ops"), "a sent message clears the room's draft")

        let r6 = await hub.sendChatText("/msg nobody hello")
        XCTAssertFalse(r6)
        XCTAssertEqual(hub.currentConversation, "#ops")
        XCTAssertEqual(hub.notice?.text, "nobody is not online. Sign in to send messages that wait for them.")
        await h.close()
    }

    func testSavingSharingAndMentions() async throws {
        let (hub, h) = try await hub()
        hub.select("#ops")
        try h.sendPlain([
            "type": "chat", "id": "m-peer", "senderId": "peer-1", "sender": "Pat", "channel": "#ops",
            "text": "@Mobile Tester can you check the deploy?", "ts": .int(Date().epochMillis),
        ])
        let arrived = await waitUntil { hub.rows.contains { $0.id == "m-peer" } }
        XCTAssertTrue(arrived)
        let message = try XCTUnwrap(hub.rows.first { $0.id == "m-peer" }).message
        XCTAssertEqual(hub.mentionHits().map(\.message.id), ["m-peer"])

        hub.toggleSaved(message, text: message.text)
        XCTAssertTrue(hub.workspace.isSaved("m-peer"))
        XCTAssertEqual(hub.workspace.saved.first?.place, "#ops")
        hub.applyRetention(ChatRetention.standard)
        XCTAssertTrue(hub.workspace.isSaved("m-peer"), "a fresh message stays saved")
        hub.workspace.save(
            ChatSavedItem(
                id: "old", conversation: "#ops", place: "#ops", senderName: "Pat", text: "ancient",
                sentAt: Date(timeIntervalSince1970: 1_000), savedAt: Date()))
        XCTAssertTrue(hub.pruneExpired())
        XCTAssertFalse(hub.workspace.isSaved("old"), "saved items follow the retention of their room")
        hub.toggleSaved(message, text: message.text)
        XCTAssertFalse(hub.workspace.isSaved("m-peer"))

        let shared = await hub.share(
            message, text: message.text, comment: "fyi", to: [ChatRecipient(kind: .channel, name: "#general", conversation: "#general")])
        XCTAssertTrue(shared)
        XCTAssertEqual(hub.currentConversation, "#ops", "sharing stays in the room")
        XCTAssertEqual(
            h.relay.messages("#general").last?.text, "fyi\n> @Mobile Tester can you check the deploy?\n> \u{2014} Pat in #ops")
        XCTAssertEqual(hub.notice?.text, "Shared to #general.")

        let mixed = await hub.share(
            message, text: message.text, comment: "",
            to: [.person("pat"), ChatRecipient(kind: .channel, name: "#general", conversation: "#general")])
        XCTAssertFalse(mixed)
        XCTAssertEqual(hub.notice?.text, "Send to one channel, or to people, not both.")
        await h.close()
    }

    func testGuestsAreAskedToSignInForDirectMessages() async throws {
        let (hub, h) = try await hub(guest: true)
        var blocked: [String] = []
        hub.onGuestBlocked = { blocked.append($0) }
        let sent = await hub.sendNewMessage(to: [.person("pat")], text: "hi")
        XCTAssertFalse(sent)
        let group = await hub.sendNewMessage(to: [.person("pat"), .person("sam")], text: "hi")
        XCTAssertFalse(group)
        XCTAssertEqual(blocked, ["send direct messages", "message several people at once"])
        XCTAssertFalse(hub.canSearchAccounts)
        await h.close()
    }
}
