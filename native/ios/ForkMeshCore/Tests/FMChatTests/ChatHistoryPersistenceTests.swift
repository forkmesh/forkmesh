import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

private func note(_ id: String, _ conversation: String, _ ts: Int64 = Date().epochMillis) -> ChatMessage {
    ChatMessage(id: id, conversation: conversation, senderId: "peer", senderName: "Pat", text: id, timestamp: Date(epochMillis: ts))
}

private func snapshot(_ messages: [ChatMessage]) -> ChatHistorySnapshot {
    var parts = ChatHistoryParts()
    parts.conversations = Dictionary(grouping: messages, by: \.conversation)
    return parts.snapshot
}

private func ids(_ load: ChatHistoryLoad) -> [String]? {
    if case .loaded(let snapshot) = load { return snapshot.messages.map(\.id) }
    return nil
}

private let legacyUrl = "wss://forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws"
private let nextUrl = "wss://app.next.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"

final class ChatHistoryPersistenceTests: XCTestCase {
    @MainActor func testRelayDirectAndPrivateHistoryIsDroppedOnLoad() async throws {
        let dir = temporaryDirectory("chat-history-purge")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        let dm = "direct:" + String(repeating: "b", count: 32)
        var old = ChatHistorySnapshot()
        old.messages = [
            note("pub", "#general"), note("peer-dm", "@peer-1"), note("secret-1", "#secret"), note("office-1", "#office/crew"),
            note("server-dm", dm),
        ]
        old.replies = [note("r-pub", "#general"), note("r-secret", "#secret"), note("r-under-dm", "#general")]
        old.replyRoots = ["r-pub": "pub", "r-secret": "secret-1", "r-under-dm": "peer-dm"]
        old.reactions = ["pub": ["like": ["x": "X"]], "peer-dm": ["like": ["x": "X"]], "r-secret": ["like": ["x": "X"]]]
        old.reactionOrder = ["pub": ["like"], "peer-dm": ["like"], "r-secret": ["like"]]
        old.stickers = ["peer-dm": ["lgtm": ["x": "X"]]]
        old.edits = ["pub": "fixed", "secret-1": "edited"]
        old.privateChannels = ["#secret", "#office/crew"]
        var look = ChatLook()
        look.backgroundPreset = "sunset"
        old.looks = ["@peer-1": look, "#secret": look, "#general": look]
        old.lookEvents = [
            ChatLookEvent(
                id: "e-1", conversation: "@peer-1", field: .background, actorName: "Pat", isSelf: false, timestamp: Date(),
                cleared: false)
        ]
        store.save(old, scope: "s")

        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh)
        hub.attachHistory(store, scope: { "s" })
        XCTAssertEqual(h.relay.messages("#general").map(\.id), ["pub"])
        XCTAssertTrue(h.relay.messages("@peer-1").isEmpty)
        XCTAssertTrue(h.relay.messages("#secret").isEmpty)
        XCTAssertFalse(hub.channels.contains("#secret"))
        XCTAssertEqual(hub.extras.replies(to: "pub").map(\.id), ["r-pub"])
        XCTAssertTrue(hub.extras.replies(to: "secret-1").isEmpty)
        XCTAssertTrue(hub.extras.replies(to: "peer-dm").isEmpty)
        XCTAssertEqual(hub.extras.reactions(for: "pub").count, 1)
        XCTAssertTrue(hub.extras.reactions(for: "peer-dm").isEmpty)
        XCTAssertTrue(hub.extras.stickers(for: "peer-dm").isEmpty)
        XCTAssertEqual(hub.extras.look(for: "#general").backgroundPreset, "sunset")
        XCTAssertEqual(hub.extras.look(for: "@peer-1"), ChatLook())
        XCTAssertTrue(hub.extras.lookEvents(in: "@peer-1").isEmpty)
        let kept = hub.historySnapshot()
        XCTAssertEqual(Set(kept.messages.map(\.id)), ["pub", "office-1", "server-dm"])
        XCTAssertTrue(kept.privateChannels.isEmpty)
        XCTAssertNil(kept.edits["secret-1"])
        XCTAssertEqual(kept.edits["pub"], "fixed")
        await store.flush()
        let rewritten = try XCTUnwrap(store.load(scope: "s"))
        XCTAssertEqual(Set(rewritten.messages.map(\.id)), ["pub", "office-1", "server-dm"], "the purge is saved")
        XCTAssertTrue(rewritten.privateChannels.isEmpty)
        await h.close()
    }

    @MainActor func testAFileFromANewerOrOlderBuildStillLoads() async throws {
        var object = JSON.parseObject(ChatHistoryCodec.encode(snapshot([note("a", "#general")])))
        object["v"] = 7
        object["somethingNew"] = ["x": 1]
        XCTAssertEqual(ChatHistoryCodec.decode(JSON.encodeData(.object(object)))?.messages.map(\.id), ["a"])
        object["v"] = nil
        XCTAssertNil(ChatHistoryCodec.decode(JSON.encodeData(.object(object))))
        XCTAssertNil(ChatHistoryCodec.decode(Data("not json".utf8)))
    }

    @MainActor func testACorruptFileIsSetAsideInsteadOfOverwritten() async throws {
        let dir = temporaryDirectory("chat-history-corrupt")
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = ChatHistoryStore(directory: dir, delay: 0)
        let target = store.file(scope: "s")
        try Data("{\"truncated".utf8).write(to: target)

        XCTAssertEqual(store.read(scope: "s"), .missing)
        store.save(snapshot([note("new", "#general")]), scope: "s")
        let aside = try FileManager.default.contentsOfDirectory(atPath: dir.path).filter { $0.contains(".unreadable-") }
        XCTAssertEqual(aside.count, 1)
        XCTAssertEqual(try Data(contentsOf: dir.appendingPathComponent(aside[0])), Data("{\"truncated".utf8))
        XCTAssertEqual(store.load(scope: "s")?.messages.map(\.id), ["new"])
    }

    @MainActor func testAFileThatCannotBeReadYetIsNeverOverwritten() async throws {
        try XCTSkipIf(getuid() == 0, "root reads files whatever their mode")
        let dir = temporaryDirectory("chat-history-locked")
        defer { try? FileManager.default.removeItem(at: dir) }
        ChatHistoryStore(directory: dir, delay: 0).save(snapshot([note("kept", "#general")]), scope: "s")
        let store = ChatHistoryStore(directory: dir, delay: 0)
        let target = store.file(scope: "s")
        let original = try Data(contentsOf: target)
        try FileManager.default.setAttributes([.posixPermissions: 0], ofItemAtPath: target.path)

        XCTAssertEqual(store.read(scope: "s"), .unreadable)
        store.save(snapshot([]), scope: "s")
        store.scheduleSave(scope: "s") {
            var parts = ChatHistoryParts()
            parts.conversations = ["#general": [note("other", "#general")]]
            return parts
        }
        let writes = await store.flush()
        XCTAssertEqual(writes, 0)

        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: target.path)
        XCTAssertEqual(try Data(contentsOf: target), original)
        XCTAssertEqual(ids(store.read(scope: "s")), ["kept"])
        store.save(snapshot([note("kept", "#general"), note("later", "#general")]), scope: "s")
        XCTAssertEqual(store.load(scope: "s")?.messages.map(\.id), ["kept", "later"])
    }

    func testFormerScopesCoverTheRetiredRoomPathAndTheProductionAliases() {
        let scopes = Set(ChatHistoryCodec.formerScopes(serverUrl: AppEnv.prodServerUrl, room: "world"))
        for url in [AppEnv.prodServerUrl, legacyUrl, nextUrl, "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/general/ws"] {
            XCTAssertTrue(scopes.contains(ChatHistoryCodec.scopeName(serverUrl: url, room: "world")), url)
        }
        XCTAssertFalse(scopes.contains(ChatHistoryCodec.scopeName(serverUrl: AppEnv.prodServerUrl, room: "other")))
        let custom = ChatHistoryCodec.formerScopes(serverUrl: "wss://relay.example/custom/ws", room: "world")
        XCTAssertEqual(custom, [ChatHistoryCodec.scopeName(serverUrl: "wss://relay.example/custom/ws", room: "world")])
    }

    @MainActor func testHistoryKeptUnderAnOlderRelayUrlMovesToTheProjectScope() async throws {
        let dir = temporaryDirectory("chat-history-migrate")
        defer { try? FileManager.default.removeItem(at: dir) }
        let now = Date().epochMillis
        let writer = ChatHistoryStore(directory: dir, delay: 0)
        let old = ChatHistoryCodec.scopeName(serverUrl: legacyUrl, room: "world")
        let recent = ChatHistoryCodec.scopeName(serverUrl: AppEnv.prodServerUrl, room: "world")
        let direct = "\(DirectMessageFrames.prefix)\(String(repeating: "d", count: 32))"
        writer.save(snapshot([note("a", "#general", now - 3000), note("dm", direct, now - 2500)]), scope: old)
        writer.save(snapshot([note("a", "#general", now - 3000), note("b", "#general", now - 1000)]), scope: recent)

        let store = ChatHistoryStore(directory: dir, delay: 0)
        let scope = ChatHistoryCodec.projectScope(projectId: AppEnv.prodHost, room: "world")
        let former = ChatHistoryCodec.formerScopes(serverUrl: AppEnv.prodServerUrl, room: "world")
        XCTAssertEqual(ids(store.read(scope: scope, formerScopes: former)), ["a", "dm", "b"])
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.file(scope: old).path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.file(scope: recent).path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: store.file(scope: old).appendingPathExtension("migrated").path))
        XCTAssertEqual(ChatHistoryStore(directory: dir).load(scope: scope)?.messages.map(\.id), ["a", "dm", "b"])
        XCTAssertEqual(ChatHistoryStore(directory: dir).read(scope: "elsewhere", formerScopes: former), .missing)
    }

    @MainActor func testGroupChannelMessagesSurviveARestart() async throws {
        let dir = temporaryDirectory("chat-history-office")
        defer { try? FileManager.default.removeItem(at: dir) }
        let first = try await RelayHarness.start()
        let hub = ChatHub(relay: first.relay, mesh: makeMesh(first))
        let office = makeOffice()
        hub.office = office
        let now = Date().epochMillis
        office.restore([note("o1", "#office/general", now - 2000), note("o2", "#office/general", now - 1000)])
        ChatHistoryStore(directory: dir).save(hub.historySnapshot(), scope: "s")
        await first.close()

        let second = try await RelayHarness.start()
        let fresh = ChatHub(relay: second.relay, mesh: makeMesh(second))
        fresh.attachHistory(ChatHistoryStore(directory: dir)) { "s" }
        let restarted = makeOffice()
        fresh.office = restarted
        XCTAssertEqual(restarted.messages("#office/general").map(\.id), ["o1", "o2"])
        XCTAssertTrue(fresh.relay.messages("#office/general").isEmpty)
        XCTAssertEqual(fresh.historySnapshot().messages.map(\.id), ["o1", "o2"])
        await second.close()
    }

    @MainActor func testDirectMessagesHeldAcrossASignOutComeBackForTheSameAccount() async throws {
        let h = try await RelayHarness.start()
        let hub = ChatHub(relay: h.relay, mesh: makeMesh(h))
        let direct = makeDirect()
        hub.direct = direct
        let conversation = "\(DirectMessageFrames.prefix)\(String(repeating: "d", count: 32))"
        direct.restore([note("d1", conversation)], receipts: [:])
        hub.directSessionChanged(account: "pat")

        hub.directSessionChanged(account: "")
        await direct.clear()
        XCTAssertTrue(direct.messages(conversation).isEmpty)
        XCTAssertEqual(hub.historySnapshot().messages.map(\.id), ["d1"], "a save while signed out keeps the direct messages")

        hub.directSessionChanged(account: "pat")
        XCTAssertEqual(direct.messages(conversation).map(\.id), ["d1"])

        hub.directSessionChanged(account: "")
        await direct.clear()
        hub.directSessionChanged(account: "sam")
        XCTAssertTrue(direct.messages(conversation).isEmpty)
        XCTAssertTrue(hub.historySnapshot().messages.isEmpty, "another account never sees them")
        await h.close()
    }

    @MainActor private func makeMesh(_ h: RelayHarness) -> LocalMeshService {
        LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
    }

    @MainActor private func makeOffice() -> OfficeChannelService {
        OfficeChannelService(
            api: FakeAccountApi(), connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
    }

    @MainActor private func makeDirect() -> DirectMessageService {
        DirectMessageService(
            api: FakeAccountApi(), connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
    }
}
