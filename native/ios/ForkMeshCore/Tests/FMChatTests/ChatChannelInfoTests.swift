import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class ChatSmartMentionTests: XCTestCase {
    func testBroadcastTagsAreFoundOnlyAsWholeMentions() {
        XCTAssertEqual(ChatSmartMentions.broadcasts(in: "@here standup in 5, @everyone too"), [.here, .everyone])
        XCTAssertEqual(ChatSmartMentions.strongest(in: "@here and @channel"), .channel)
        XCTAssertEqual(ChatSmartMentions.broadcasts(in: "mail me at team@here.com or @hereafter"), [])
        XCTAssertEqual(ChatSmartMentions.broadcasts(in: "`@everyone` is how you ping"), [])
        XCTAssertEqual(ChatSmartMentions.broadcasts(in: "(@Everyone) read this."), [.everyone])
        XCTAssertNil(ChatSmartMentions.strongest(in: "nothing here"))
    }

    func testSplitMarksYouPeopleAndBroadcasts() {
        let pieces = ChatSmartMentions.split("hi @jett and @Sam, @here.", own: ["Jett"])
        XCTAssertEqual(pieces, [
            ChatMentionPiece("hi "), ChatMentionPiece("@jett", .you), ChatMentionPiece(" and "), ChatMentionPiece("@Sam", .person),
            ChatMentionPiece(", "), ChatMentionPiece("@here", .broadcast), ChatMentionPiece("."),
        ])
        XCTAssertTrue(ChatSmartMentions.mentionsYou("@everyone ship it", own: []))
        XCTAssertTrue(ChatSmartMentions.mentionsYou("ping @jett", own: ["jett"]))
        XCTAssertFalse(ChatSmartMentions.mentionsYou("ping @sam", own: ["jett"]))
        XCTAssertEqual(ChatSmartMentions.suggestions("e").map(\.tag), ["@everyone"])
        XCTAssertEqual(ChatSmartMentions.suggestions("").count, 3)
    }

    func testLinksInTextSkipFencesAndTrailingPunctuation() {
        let text = "see https://youtube.com/watch?v=abc123xyz. and (https://en.wikipedia.org/wiki/Fork_(software))\n```\nhttps://code.example.com\n```\n<https://slack.example.com|label>"
        XCTAssertEqual(ChatLinkPreviewParsing.links(in: text), [
            "https://youtube.com/watch?v=abc123xyz", "https://en.wikipedia.org/wiki/Fork_(software)", "https://slack.example.com",
        ])
    }

    func testFirstUnreadIdCountsOnlyOthersMessages() {
        let t = Date(timeIntervalSince1970: 0)
        let messages = [
            ChatMessage(id: "a", conversation: "#g", senderId: "p", senderName: "P", text: "1", timestamp: t),
            ChatMessage(id: "b", conversation: "#g", senderId: "p", senderName: "P", text: "2", timestamp: t),
            ChatMessage(id: "c", conversation: "#g", senderId: "me", senderName: "Me", text: "3", timestamp: t, isSelf: true),
            ChatMessage(id: "d", conversation: "#g", senderId: "p", senderName: "P", text: "4", timestamp: t),
        ]
        XCTAssertNil(ChatHub.firstUnreadId(messages, unread: 0))
        XCTAssertEqual(ChatHub.firstUnreadId(messages, unread: 1), "d")
        XCTAssertEqual(ChatHub.firstUnreadId(messages, unread: 2), "b")
        XCTAssertEqual(ChatHub.firstUnreadId(messages, unread: 9), "a")
        XCTAssertEqual(ChatHub.membersLabel(1), "1 member")
        XCTAssertEqual(ChatHub.membersLabel(346), "346 members")
    }
}

@MainActor
final class ChatChannelInfoTests: XCTestCase {
    private func hub() async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh, isGuest: { false }), h)
    }

    func testWorkspaceKeepsBookmarksCanvasAndNotifyLevel() {
        let store = InMemoryKeyValueStore()
        let workspace = ChatWorkspace(store: store, scope: "next")
        let now = Date(timeIntervalSince1970: 100)
        XCTAssertNotNil(workspace.addBookmark("#ops", title: "Runbook", url: "https://forkmesh.com/docs", now: now))
        XCTAssertNil(workspace.addBookmark("#ops", title: "bad", url: "javascript:alert(1)", now: now))
        XCTAssertNotNil(workspace.addBookmark("#ops", title: "", url: "https://status.forkmesh.com", now: now))
        workspace.setCanvas("#ops", title: "", text: "On call: Sam", now: now)
        workspace.setNotifyLevel(.mentions, for: "#ops")
        let reloaded = ChatWorkspace(store: store, scope: "next")
        XCTAssertEqual(reloaded.bookmarks(for: "#ops").map(\.title), ["Runbook", "status.forkmesh.com"])
        XCTAssertEqual(reloaded.canvas(for: "#ops")?.title, "Canvas")
        XCTAssertEqual(reloaded.canvas(for: "#ops")?.text, "On call: Sam")
        XCTAssertEqual(reloaded.notifyLevel("#ops"), .mentions)
        reloaded.setNotifyLevel(.nothing, for: "#ops")
        XCTAssertTrue(reloaded.isMuted("#ops"))
        XCTAssertEqual(reloaded.notifyLevel("#ops"), .nothing)
        reloaded.removeBookmark("#ops", id: reloaded.bookmarks(for: "#ops")[0].id)
        XCTAssertEqual(reloaded.bookmarks(for: "#ops").count, 1)
        reloaded.forget(conversation: "#ops")
        XCTAssertEqual(reloaded.notifyLevel("#ops"), .all)
        XCTAssertTrue(reloaded.bookmarks(for: "#ops").isEmpty)
        XCTAssertNil(ChatWorkspace(store: store, scope: "next").canvas(for: "#ops"))
    }

    func testMembersCountOnlinePeopleAndEveryoneWhoPosted() async throws {
        let (hub, h) = try await hub()
        try await h.sendProtected([
            "type": "presence", "id": "p1", "senderId": "peer-1", "sender": "Pat", "ownerUser": "pat", "ts": .int(Date().epochMillis),
        ])
        _ = await waitUntil { h.relay.roster().contains { $0.id == "peer-1" } }
        for (index, sender) in ["Sam", "Pat", "forkbot", "Sam"].enumerated() {
            try h.sendPlain([
                "type": "chat", "id": .string("m\(index)"), "senderId": .string(sender == "Pat" ? "peer-1" : sender.lowercased()),
                "sender": .string(sender), "ts": .int(Date().epochMillis + Int64(index)), "channel": "#general", "text": "hello",
            ])
        }
        _ = await waitUntil { h.relay.messages("#general").count == 4 }
        let members = hub.channelMembers("#general")
        XCTAssertEqual(members.filter { !$0.isSelf }.map { $0.name.lowercased() }, ["pat", "sam"])
        XCTAssertTrue(members.first?.isSelf ?? false, "you are listed first")
        XCTAssertEqual(members.first { $0.name.lowercased() == "pat" }?.online, true)
        XCTAssertEqual(hub.memberCount("#general"), members.count)
        XCTAssertEqual(hub.headerDetail, "\(members.count) members \u{2022} 2 tabs")
        hub.saveCanvas("#general", title: "Plan", text: "x")
        XCTAssertEqual(hub.channelTabCount("#general"), 3)
        XCTAssertTrue(hub.addBookmark("Docs", url: "https://forkmesh.com", to: "#general"))
        XCTAssertEqual(hub.channelTabCount("#general"), 4)
        XCTAssertEqual(hub.bookmarkCount("#general"), 1)
        XCTAssertEqual(hub.broadcastAudience(.everyone, in: "#general"), 2)
        XCTAssertEqual(hub.broadcastAudience(.here, in: "#general"), members.filter { !$0.isSelf && $0.online }.count)
        XCTAssertNil(hub.broadcastWarning("@everyone hi", in: "#general"), "small channels send without asking")
        XCTAssertEqual(hub.broadcastSuggestions("he", in: "#general"), [.here])
        XCTAssertEqual(hub.broadcastSuggestions("", in: "@peer-1"), [])
        await h.close()
    }

    func testBigChannelsAskBeforeABroadcast() async throws {
        let (hub, h) = try await hub()
        for index in 0..<8 {
            try h.sendPlain([
                "type": "chat", "id": .string("b\(index)"), "senderId": .string("n\(index)"), "sender": .string("Person \(index)"),
                "ts": .int(Date().epochMillis + Int64(index)), "channel": "#general", "text": "hi",
            ])
        }
        _ = await waitUntil { h.relay.messages("#general").count == 8 }
        let warning = try XCTUnwrap(hub.broadcastWarning("@channel deploy at 5", in: "#general"))
        XCTAssertEqual(warning.broadcast, .channel)
        XCTAssertEqual(warning.people, 8)
        XCTAssertEqual(warning.title, "Notify 8 people?")
        XCTAssertEqual(hub.broadcastWarning("@here deploy at 5", in: "#general")?.people, hub.broadcastAudience(.here, in: "#general"))
        XCTAssertNil(hub.broadcastWarning("deploy at 5", in: "#general"))
        await h.close()
    }

    func testNewDividerAndMentionsOnlyUnread() async throws {
        let (hub, h) = try await hub()
        hub.workspace = ChatWorkspace(store: InMemoryKeyValueStore(), scope: "t")
        hub.select("#random")
        for (index, text) in ["one", "two", "@everyone three"].enumerated() {
            try h.sendPlain([
                "type": "chat", "id": .string("u\(index)"), "senderId": "sam", "sender": "Sam", "ts": .int(Date().epochMillis + Int64(index)),
                "channel": "#general", "text": .string(text),
            ])
        }
        _ = await waitUntil { h.relay.unreadCountFor("#general") == 3 }
        hub.setNotifyLevel(.mentions, for: "#general")
        XCTAssertTrue(hub.isUnread("#general"), "an @everyone counts as a mention")
        hub.select("#general")
        XCTAssertEqual(hub.unreadStart["#general"], "u0")
        XCTAssertEqual(hub.rows.last?.mentionsYou, true)
        XCTAssertEqual(hub.rows.first?.mentionsYou, false)
        hub.select("#random")
        XCTAssertNil(hub.unreadStart["#general"])
        try h.sendPlain([
            "type": "chat", "id": "u9", "senderId": "sam", "sender": "Sam", "ts": .int(Date().epochMillis + 50), "channel": "#general", "text": "plain",
        ])
        _ = await waitUntil { h.relay.unreadCountFor("#general") == 1 }
        XCTAssertFalse(hub.isUnread("#general"), "a plain message does not light a mentions-only channel")
        hub.setNotifyLevel(.all, for: "#general")
        XCTAssertTrue(hub.isUnread("#general"))
        let files = hub.channelFiles("#general")
        XCTAssertTrue(files.isEmpty)
        await h.close()
    }
}
