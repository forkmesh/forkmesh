import FMBase
import XCTest

@testable import FMChat

final class GroupCallsTests: XCTestCase {
    static let ana = String(repeating: "a", count: 32)
    static let ben = String(repeating: "b", count: 32)

    private func frame(_ type: String, peer: String, sender: String, extra: JSONObject = [:]) -> JSONObject {
        var plain: JSONObject = ["type": .string(type), "peer": .string(peer), "sender": .string(sender)]
        for (key, value) in extra { plain[key] = value }
        return plain
    }

    @MainActor func testPresenceTracksJoinsLeavesAndExpiry() async {
        var clock: TimeInterval = 1000
        let roster = GroupCallRoster(now: { clock })
        let joined = roster.handle(
            frame(GroupCallFrames.presenceType, peer: Self.ana, sender: "Ana", extra: ["call": "in"]),
            conversation: "#team", selfPeer: Self.ben)
        XCTAssertEqual(joined, .joined(peer: Self.ana, isNew: true))
        XCTAssertEqual(
            roster.handle(frame(GroupCallFrames.presenceType, peer: Self.ana, sender: "ana", extra: ["call": "in"]),
                conversation: "#team", selfPeer: Self.ben),
            .joined(peer: Self.ana, isNew: false))
        XCTAssertEqual(roster.roster("#team"), [GroupCallMember(peer: Self.ana, name: "ana")])
        XCTAssertEqual(roster.roster("#other"), [])
        clock += GroupCallFrames.presenceTTL + 1
        XCTAssertEqual(roster.roster("#team"), [])
        _ = roster.handle(frame(GroupCallFrames.presenceType, peer: Self.ana, sender: "ana", extra: ["call": "in"]),
            conversation: "#team", selfPeer: Self.ben)
        XCTAssertEqual(
            roster.handle(frame(GroupCallFrames.presenceType, peer: Self.ana, sender: "ana", extra: ["call": "out"]),
                conversation: "#team", selfPeer: Self.ben),
            .left(peer: Self.ana))
        XCTAssertEqual(roster.roster("#team"), [])
    }

    @MainActor func testKeysAreValidatedAndBoundToTheAnnouncedSender() async {
        let roster = GroupCallRoster()
        let raw = [UInt8](0..<16)
        _ = roster.handle(frame(GroupCallFrames.presenceType, peer: Self.ana, sender: "ana", extra: ["call": "in"]),
            conversation: "#team", selfPeer: Self.ben)
        let fields = GroupCallFrames.key(peer: Self.ana, keyId: 4, raw: raw)
        var good = frame(GroupCallFrames.keyType, peer: Self.ana, sender: "ana")
        for (key, value) in fields { good[key] = value }
        XCTAssertEqual(
            roster.handle(good, conversation: "#team", selfPeer: Self.ben),
            .key(peer: Self.ana, key: GroupCallKey(keyId: 4, raw: raw, name: "ana")))
        var impostor = good
        impostor["sender"] = "mallory"
        XCTAssertNil(roster.handle(impostor, conversation: "#team", selfPeer: Self.ben))
        var short = good
        short["key"] = .string(Data([1, 2, 3]).base64EncodedString())
        XCTAssertNil(roster.handle(short, conversation: "#team", selfPeer: Self.ben))
        var range = good
        range["keyId"] = 256
        XCTAssertNil(roster.handle(range, conversation: "#team", selfPeer: Self.ben))
        var badPeer = good
        badPeer["peer"] = "../x"
        XCTAssertNil(roster.handle(badPeer, conversation: "#team", selfPeer: Self.ben))
        XCTAssertNil(roster.handle(good, conversation: "#team", selfPeer: Self.ana))
        XCTAssertEqual(roster.keys("#team")[Self.ana]?.keyId, 4)
    }

    @MainActor func testAPeerIdStaysWithTheFirstSenderThatClaimedIt() async {
        var clock: TimeInterval = 1000
        let roster = GroupCallRoster(now: { clock })
        let cat = String(repeating: "c", count: 32)
        func presence(_ peer: String, _ sender: String, _ state: String = "in") -> GroupCallFrameEvent? {
            roster.handle(frame(GroupCallFrames.presenceType, peer: peer, sender: sender, extra: ["call": .string(state)]),
                conversation: "#team", selfPeer: Self.ben)
        }
        func key(_ peer: String, _ sender: String, _ keyId: Int) -> GroupCallFrameEvent? {
            var plain = frame(GroupCallFrames.keyType, peer: peer, sender: sender)
            for (field, value) in GroupCallFrames.key(peer: peer, keyId: keyId, raw: [UInt8](repeating: UInt8(keyId), count: 16)) {
                plain[field] = value
            }
            return roster.handle(plain, conversation: "#team", selfPeer: Self.ben)
        }
        XCTAssertEqual(presence(Self.ana, "Ana"), .joined(peer: Self.ana, isNew: true))
        XCTAssertNil(presence(Self.ana, "mallory", "out"))
        XCTAssertNil(presence(Self.ana, "mallory"))
        XCTAssertNil(key(Self.ana, "mallory", 1))
        XCTAssertEqual(roster.roster("#team"), [GroupCallMember(peer: Self.ana, name: "ana")])
        XCTAssertNil(roster.keys("#team")[Self.ana])
        XCTAssertNotNil(key(Self.ana, "ana", 2))
        XCTAssertNil(presence(cat, ""))
        XCTAssertNil(presence(cat, "  "))
        XCTAssertNil(key(cat, "", 3))
        XCTAssertNotNil(key(cat, "cat", 3))
        XCTAssertNil(presence(cat, "mallory"))
        XCTAssertEqual(presence(cat, "cat"), .joined(peer: cat, isNew: true))
        XCTAssertEqual(presence(Self.ana, "ana", "out"), .left(peer: Self.ana))
        XCTAssertNil(roster.keys("#team")[Self.ana])
        XCTAssertEqual(presence(Self.ana, "mallory"), .joined(peer: Self.ana, isNew: true))
        clock += GroupCallFrames.presenceTTL + 1
        XCTAssertEqual(presence(cat, "dan"), .joined(peer: cat, isNew: false))
        XCTAssertEqual(roster.roster("#team"), [GroupCallMember(peer: cat, name: "dan")])
    }

    func testTicketsNeedASecureURLAndAPeerId() {
        XCTAssertNotNil(GroupCallTicket(json: ["ticket": "t", "url": "wss://n2.next.forkmesh.com/calls/ws", "peer": .string(Self.ana)]))
        XCTAssertNil(GroupCallTicket(json: ["ticket": "t", "url": "ws://x/calls/ws", "peer": .string(Self.ana)]))
        XCTAssertNil(GroupCallTicket(json: ["ticket": "", "url": "wss://x", "peer": .string(Self.ana)]))
        XCTAssertNil(GroupCallTicket(json: ["ticket": "t", "url": "wss://x", "peer": "nope"]))
    }

    func testTheSendKeyRotatesItsIdAndBytes() {
        var counter: UInt8 = 0
        let next = { () -> [UInt8] in
            counter &+= 1
            return [UInt8](repeating: counter, count: 16)
        }
        var key = GroupCallSendKey(random: next)
        XCTAssertEqual(key.keyId, 0)
        XCTAssertEqual(key.raw, [UInt8](repeating: 1, count: 16))
        key.rotate(random: next)
        XCTAssertEqual(key.keyId, 1)
        XCTAssertEqual(key.raw, [UInt8](repeating: 2, count: 16))
        for _ in 0..<255 { key.rotate(random: next) }
        XCTAssertEqual(key.keyId, 0)
        XCTAssertEqual(GroupCallSendKey.randomKey().count, 16)
    }
}
