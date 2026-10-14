import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

@MainActor
final class ChatEveryoneTests: XCTestCase {
    private func hub() async throws -> (ChatHub, RelayHarness, FakeAccountApi) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh, isGuest: { false })
        let api = FakeAccountApi()
        hub.accounts = api
        return (hub, h, api)
    }

    private func user(_ name: String, createdAt: Int64) -> JSONValue {
        .object(["name": .string(name), "kind": "user", "createdAt": .int(createdAt)])
    }

    func testEveryMemberIsInGeneralRandomAndWelcome() async throws {
        let (hub, h, api) = try await hub()
        let now = Date().epochMillis
        api.replies["GET /api/accounts/users"] = [
            "users": .array([
                user("alice", createdAt: now - 40 * 86_400_000), user("bob", createdAt: now - 3_600_000),
                user("pat", createdAt: now - 90 * 86_400_000), user("Alice", createdAt: 0),
            ]),
        ]
        try await h.sendProtected([
            "type": "presence", "id": "p1", "senderId": "peer-1", "sender": "Pat", "ownerUser": "pat", "ts": .int(now),
        ])
        _ = await waitUntil { h.relay.roster().contains { $0.id == "peer-1" } }
        XCTAssertEqual(h.relay.channels, ["#general", "#random", "#welcome"])
        await hub.refreshMemberDirectory()
        XCTAssertEqual(api.calls.map(\.1), [PeopleModel.directoryPath])
        for channel in ChatEveryone.channels {
            let others = hub.channelMembers(channel).filter { !$0.isSelf }
            XCTAssertEqual(Set(others.map { $0.account.lowercased() }), ["alice", "bob", "pat"], channel)
            XCTAssertEqual(others.first { $0.account == "pat" }?.online, true)
            XCTAssertEqual(others.first { $0.account == "bob" }?.online, false)
        }
        XCTAssertFalse(hub.channelMembers("#ops").contains { $0.account == "alice" })
        let welcome = h.relay.messages("#welcome")
        XCTAssertEqual(welcome.map(\.text), [ChatEveryone.welcomeText("bob")])
        XCTAssertEqual(welcome.first?.senderId, "forkbot")
        XCTAssertFalse(h.relay.hasUnread("#welcome"), "backfilled welcomes are not news")
        await hub.refreshMemberDirectory()
        XCTAssertEqual(api.calls.count, 1, "the directory is not re-read within five minutes")
        hub.sharedRoom = { false }
        XCTAssertFalse(hub.channelMembers("#general").contains { $0.account == "alice" })
        await h.close()
    }

    func testSignedNewMemberFrameWelcomesThemInWelcome() async throws {
        let (hub, h, api) = try await hub()
        api.replies["GET /api/accounts/users"] = ["users": .array([])]
        h.server.receive(text: #"{"kind":"forkmesh-new-member","v":1,"name":"newbie"}"#)
        h.server.receive(text: #"{"kind":"forkmesh-new-member","v":1,"name":"Not A Handle!"}"#)
        h.server.receive(text: #"{"kind":"forkmesh-new-member","v":1,"name":"newbie"}"#)
        _ = await waitUntil { !h.relay.messages("#welcome").isEmpty }
        let welcome = h.relay.messages("#welcome")
        XCTAssertEqual(welcome.map(\.text), ["\u{1F389} Welcome to ForkMesh, @newbie! Everyone, come say hi \u{1F44B}"])
        XCTAssertTrue(ChatEveryone.isWelcome(try XCTUnwrap(welcome.first)))
        XCTAssertEqual(h.relay.unreadCountFor("#welcome"), 1)
        _ = await waitUntil { !api.calls.isEmpty }
        XCTAssertEqual(api.calls.map(\.1), [PeopleModel.directoryPath], "a join refreshes the member list")
        _ = hub
        await h.close()
    }

    func testEveryoneChannelsCannotBeLeft() {
        let extras = ChatExtras(clock: SystemClock())
        extras.restoreChannels(privateChannels: [], hiddenChannels: ["#random", "#welcome", "#ops"])
        XCTAssertEqual(extras.hiddenChannels, ["#ops"])
        for channel in ChatEveryone.channels {
            extras.removeChannel(channel)
            XCTAssertFalse(extras.hiddenChannels.contains(channel))
        }
        XCTAssertNil(ChatEveryone.handle("-bad"))
        XCTAssertNil(ChatEveryone.handle(""))
        XCTAssertEqual(ChatEveryone.handle(" Jett-2 "), "jett-2")
    }
}
