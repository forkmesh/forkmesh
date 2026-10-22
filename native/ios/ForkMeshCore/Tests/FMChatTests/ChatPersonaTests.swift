import FMBase
import FMNet
import XCTest

@testable import FMChat

@MainActor
final class ChatPersonaTests: XCTestCase {
    func testGuestDigitsMatchTheDesktopDerivation() async {
        XCTAssertEqual(guestChatName("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), "Guest 3813")
        XCTAssertEqual(guestChatName("demo-key"), "Guest 4804")
        XCTAssertEqual(guestChatName(""), "Guest")
    }

    func testASignedOutDeviceSpeaksAsAGuestNeverAsItsNode() async {
        let device = ChatDevice()
        let persona = ChatPersona.resolve(nodeKey: device.identity.publicKeyB64url, account: "", displayName: "jett")
        XCTAssertEqual(persona.name, guestChatName(device.identity.publicKeyB64url))
        XCTAssertFalse(persona.name.contains(device.identity.shortKey))
        XCTAssertEqual(persona.accountKind, "guest")
        XCTAssertEqual(persona.ownerUser, "")
        XCTAssertTrue(persona.isGuest)
    }

    func testASignedInDeviceSpeaksAsTheUserAndNamesItsOwner() async {
        let key = ChatDevice().identity.publicKeyB64url
        let plain = ChatPersona.resolve(nodeKey: key, account: "jett")
        XCTAssertEqual(plain.name, "jett")
        XCTAssertEqual(plain.accountKind, "user")
        XCTAssertEqual(plain.ownerUser, "jett")

        let named = ChatPersona.resolve(nodeKey: key, account: "jett", displayName: "Jett R.")
        XCTAssertEqual(named.name, "Jett R.")
        XCTAssertEqual(named.ownerUser, "jett")

        var frame: JSONObject = [:]
        named.stamp(&frame)
        XCTAssertEqual(frame["accountKind"]?.text, "user")
        XCTAssertEqual(frame["ownerUser"]?.text, "jett")
    }

    func testTheRelayRosterRowFollowsTheSessionNotTheNodeKey() async {
        let device = ChatDevice()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            connector: FakeWebSocketConnector(), platform: "ios")
        var account = ""
        relay.accountProvider = { account }
        var me = relay.roster().first { $0.isSelf }
        XCTAssertEqual(me?.name, guestChatName(device.identity.publicKeyB64url))
        XCTAssertEqual(me?.owner, "")

        account = "jett"
        relay.identityChanged()
        me = relay.roster().first { $0.isSelf }
        XCTAssertEqual(me?.name, "jett")
        XCTAssertEqual(me?.owner, "jett")
    }

    func testTheMeshRosterRowFollowsTheSessionNotTheNodeKey() async {
        let device = ChatDevice()
        let mesh = LocalMeshService(settings: device.settings, identity: device.identity, secureStore: device.secure)
        XCTAssertEqual(mesh.displayName, guestChatName(device.identity.publicKeyB64url))
        mesh.accountProvider = { "jett" }
        device.settings.setDisplayName("Jett")
        XCTAssertEqual(
            mesh.displayName, guestChatName(device.identity.publicKeyB64url), "#mesh never names the account to strangers")
        XCTAssertEqual(mesh.members().first { $0.isSelf }?.owner, "jett")
        device.settings.setDisplayName("Jett P")
        XCTAssertEqual(mesh.displayName, "Jett P", "a name the user chose is shown")
    }
}
