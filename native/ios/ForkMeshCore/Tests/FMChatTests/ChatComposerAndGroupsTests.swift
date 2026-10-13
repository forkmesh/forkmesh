import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMChat

final class ChatComposerToolsTests: XCTestCase {
    func testMentionTokenFollowsTheDesktopRules() {
        XCTAssertEqual(ChatComposerTools.mentionToken("hi @pa")?.query, "pa")
        XCTAssertEqual(ChatComposerTools.mentionToken("@")?.query, "")
        XCTAssertNil(ChatComposerTools.mentionToken("mail@pa"))
        XCTAssertNil(ChatComposerTools.mentionToken("hi @pa there"))
        XCTAssertEqual(ChatComposerTools.insertMention("hi @pa", name: "pat"), "hi @pat ")
        XCTAssertEqual(ChatComposerTools.insertMention("no token", name: "pat"), "no token")
        XCTAssertEqual(
            ChatComposerTools.mentionCandidates(["Pat", "pat", "Paul", "me", "Zed"], excluding: "me", query: "pa"), ["Pat", "Paul"])
    }

    func testSlashCommands() {
        XCTAssertEqual(
            ChatComposerTools.slashCommands("/", canSetTopic: false).map(\.command),
            ["/msg", "/dm", "/search", "/saved", "/mute", "/shrug", "/me", "/issue"])
        XCTAssertEqual(ChatComposerTools.slashCommands("/t", canSetTopic: true).map(\.command), ["/topic"])
        XCTAssertTrue(ChatComposerTools.slashCommands("/issue x", canSetTopic: true).isEmpty)
        XCTAssertEqual(ChatComposerTools.parseSlash("/issue  fix it "), .issue("fix it"))
        XCTAssertEqual(ChatComposerTools.parseSlash("/TOPIC new"), .topic("new"))
        XCTAssertNil(ChatComposerTools.parseSlash("/unknown"))
        XCTAssertNil(ChatComposerTools.parseSlash("plain"))
    }

    func testLinkInsertionMatchesTheDesktop() {
        let plain = ChatComposerTools.linkInsertion("see ", selectionStart: 4, selectionEnd: 4)
        XCTAssertEqual(plain.text, "see [link text](https://)")
        XCTAssertEqual(plain.selectionStart, 4 + 1 + 9 + 2)
        XCTAssertEqual(plain.selectionLength, 8)
        let clip = ChatComposerTools.linkInsertion("", selectionStart: 0, selectionEnd: 0, clipboard: " https://forkmesh.com ")
        XCTAssertEqual(clip.text, "[link text](https://forkmesh.com)")
        XCTAssertEqual(clip.selectionStart, 1)
        XCTAssertEqual(clip.selectionLength, 9)
        let both = ChatComposerTools.linkInsertion("docs", selectionStart: 0, selectionEnd: 4, clipboard: "https://a.b")
        XCTAssertEqual(both.text, "[docs](https://a.b)")
        XCTAssertEqual(both.selectionLength, 0)
        XCTAssertEqual(ChatComposerTools.sanitizedHref("javascript:alert(1)"), "")
    }

    func testForkbotMentionAndContext() {
        XCTAssertTrue(ChatComposerTools.mentionsForkbot("hey @ForkBot help"))
        XCTAssertTrue(ChatComposerTools.mentionsForkbot("forkbot"))
        XCTAssertFalse(ChatComposerTools.mentionsForkbot("forkbots rule"))
        XCTAssertFalse(ChatComposerTools.mentionsForkbot("myforkbot"))
        let history = (0..<20).map {
            ChatMessage(id: "m\($0)", conversation: "#general", senderId: $0 == 3 ? "forkbot" : "a", senderName: "A",
                        text: "t\($0)", timestamp: Date(epochMillis: Int64($0)))
        }
        let context = ChatComposerTools.forkbotContext(history)
        XCTAssertEqual(context.count, 12)
        XCTAssertEqual(context.last?["text"]?.text, "t18")
    }

    func testIssueDraftParsing() throws {
        let repos = [RepoRef(owner: "alice", name: "tool"), RepoRef(owner: "forkmesh", name: "forkmesh")]
        let now = Date(epochMillis: 0)
        let draft = try ChatIssueDraft.parse(
            "alice/tool Crash on start\nmore detail", repos: repos, current: nil, channel: "#general", author: "pat", now: now).get()
        XCTAssertEqual(draft.repo.slug, "alice/tool")
        XCTAssertEqual(draft.title, "Crash on start")
        XCTAssertTrue(draft.body.hasSuffix("Filed from a #general chat message by pat at 1970-01-01T00:00:00Z."))
        let fallback = try ChatIssueDraft.parse("Just a title", repos: repos, current: nil, channel: "", author: "", now: now).get()
        XCTAssertEqual(fallback.repo.slug, "forkmesh/forkmesh")
        XCTAssertEqual(ChatIssueDraft.parse("", repos: repos, current: nil, channel: "", author: "", now: now), .failure(.usage))
        XCTAssertEqual(
            ChatIssueDraft.parse("bob/none title", repos: repos, current: nil, channel: "", author: "", now: now),
            .failure(.unknownRepository("bob/none")))
        XCTAssertEqual(ChatIssueDraft.parse("x", repos: [], current: nil, channel: "", author: "", now: now), .failure(.noRepositories))
    }

    func testVisitorsAndWelcome() {
        XCTAssertTrue(ChatVisitorPresence.isTransientVisitor(name: "Guest 42"))
        XCTAssertTrue(ChatVisitorPresence.isTransientVisitor(name: "World visitor"))
        XCTAssertTrue(ChatVisitorPresence.isTransientVisitor(accountKind: "guest", name: "anyone"))
        XCTAssertFalse(ChatVisitorPresence.isTransientVisitor(name: "guesthouse"))
        XCTAssertTrue(ChatVisitorPresence.isIdle(lastSeen: Date(timeIntervalSince1970: 0), now: Date(timeIntervalSince1970: 601)))
    }

    @MainActor func testWelcomeIsOnlyForFreshVerifiedAccounts() async {
        let prefs = InMemoryKeyValueStore()
        let announcer = ChatWelcomeAnnouncer(prefs: prefs)
        let now: Int64 = 10_000_000_000
        XCTAssertFalse(announcer.shouldAnnounce(account: "old", isGuest: false, emailVerified: true, createdAtMs: 1, nowMs: now, alreadyInRoom: false))
        XCTAssertFalse(announcer.shouldAnnounce(account: "g", isGuest: true, emailVerified: true, createdAtMs: now, nowMs: now, alreadyInRoom: false))
        XCTAssertTrue(announcer.shouldAnnounce(account: "new", isGuest: false, emailVerified: true, createdAtMs: now - 1000, nowMs: now, alreadyInRoom: false))
        XCTAssertTrue(announcer.shouldAnnounce(account: "secs", isGuest: false, emailVerified: true, createdAtMs: now / 1000 - 5, nowMs: now, alreadyInRoom: false))
        announcer.markAnnounced(account: "new")
        XCTAssertFalse(announcer.shouldAnnounce(account: "new", isGuest: false, emailVerified: true, createdAtMs: now - 1000, nowMs: now, alreadyInRoom: false))
    }
}

final class ChannelGroupsTests: XCTestCase {
    func testLayoutPutsUngroupedFirstThenGroups() {
        var state = ChannelGroups.State()
        XCTAssertTrue(ChannelGroups.createGroup(&state, "  Work  stuff "))
        XCTAssertFalse(ChannelGroups.createGroup(&state, "Work stuff"))
        let rows0 = ChannelGroups.layout(state, channels: ["#general", "#dev", "#discord/1"]) {
            $0.hasPrefix("#discord/") ? "Discord · Server" : ""
        }
        XCTAssertEqual(rows0.map(\.id), ["channel:#general", "channel:#dev", "group:Work stuff", "group:Discord · Server", "channel:#discord/1"])
        XCTAssertTrue(ChannelGroups.assign(&state, channel: "#dev", to: "Work stuff", index: -1, members: [], automaticGroup: false))
        let rows = ChannelGroups.layout(state, channels: ["#general", "#dev"])
        XCTAssertEqual(ChannelGroups.renderedMembers(rows, "Work stuff"), ["#dev"])
        ChannelGroups.setCollapsed(&state, "Work stuff", collapsed: true, automatic: false)
        let visible = ChannelGroups.visibleRows(ChannelGroups.layout(state, channels: ["#general", "#dev"]))
        XCTAssertEqual(visible.map(\.id), ["channel:#general", "group:Work stuff"])
    }

    func testRenameRemoveAndRoundTrip() {
        var state = ChannelGroups.State()
        ChannelGroups.createGroup(&state, "A")
        ChannelGroups.assign(&state, channel: "#x", to: "A", index: 0, members: [], automaticGroup: false)
        ChannelGroups.assign(&state, channel: "#y", to: "A", index: 0, members: ["#x"], automaticGroup: false)
        XCTAssertEqual(state.groups[0].channels, ["#y", "#x"])
        XCTAssertTrue(ChannelGroups.renameGroup(&state, from: "A", to: "B", members: ["#y", "#x"]))
        XCTAssertEqual(ChannelGroups.fromJSON(ChannelGroups.toJSON(state)), state)
        ChannelGroups.removeGroup(&state, "B", members: ["#y", "#x"])
        XCTAssertTrue(state.groups.isEmpty)
        XCTAssertEqual(state.ungrouped, ["#y", "#x"])
        XCTAssertTrue(ChannelGroups.purge(&state, channelMatches: { $0 == "#x" }, groupMatches: { _ in true }))
        XCTAssertEqual(state.ungrouped, ["#y"])
        XCTAssertEqual(ChannelGroups.fromJSON("garbage"), ChannelGroups.State())
    }

    @MainActor func testStorePersists() async {
        let prefs = InMemoryKeyValueStore()
        let store = ChannelGroupsStore(prefs: prefs, scope: "relay")
        store.update { ChannelGroups.createGroup(&$0, "Mine") }
        XCTAssertEqual(ChannelGroupsStore(prefs: prefs, scope: "relay").state.groups.map(\.name), ["Mine"])
        store.update { ChannelGroups.removeGroup(&$0, "Mine", members: []) }
        XCTAssertFalse(prefs.contains("chat/channelGroups/relay"))
    }

    @MainActor func testGroupsSurviveAProjectUrlChange() async {
        let prefs = InMemoryKeyValueStore()
        ChannelGroupsStore(prefs: prefs, projectId: "relay.example", serverUrl: "https://relay.example", room: "lobby").update {
            ChannelGroups.createGroup(&$0, "Ops")
            ChannelGroups.assign(&$0, channel: "#deploy", to: "Ops", index: 0, members: [], automaticGroup: false)
        }
        let moved = ChannelGroupsStore(prefs: prefs, projectId: "relay.example", serverUrl: "https://relay.example/relay/", room: "lobby")
        XCTAssertEqual(moved.state.groups.map(\.name), ["Ops"])
        XCTAssertEqual(moved.state.groups.first?.channels, ["#deploy"])
        XCTAssertTrue(
            ChannelGroupsStore(prefs: prefs, projectId: "other.example", serverUrl: "https://relay.example", room: "lobby").state.isEmpty)
        XCTAssertTrue(
            ChannelGroupsStore(prefs: prefs, projectId: "relay.example", serverUrl: "https://relay.example", room: "annex").state.isEmpty)
    }

    @MainActor func testGroupsSavedUnderTheUrlAreAdopted() async {
        let prefs = InMemoryKeyValueStore()
        let urlScope = ChatHistoryCodec.scopeName(serverUrl: "https://relay.example", room: "lobby")
        ChannelGroupsStore(prefs: prefs, scope: urlScope).update { ChannelGroups.createGroup(&$0, "Old") }
        let store = ChannelGroupsStore(prefs: prefs, projectId: "relay.example", serverUrl: "https://relay.example", room: "lobby")
        XCTAssertEqual(store.state.groups.map(\.name), ["Old"])
        XCTAssertFalse(prefs.contains(ChannelGroupsStore.keyFor(urlScope)))
        XCTAssertTrue(prefs.contains(ChannelGroupsStore.keyFor(ChannelGroupsStore.projectScope(projectId: "relay.example", room: "lobby"))))
        XCTAssertEqual(
            ChannelGroupsStore(prefs: prefs, projectId: "relay.example", serverUrl: "https://relay.example/new", room: "lobby").state.groups
                .map(\.name), ["Old"])
    }
}
