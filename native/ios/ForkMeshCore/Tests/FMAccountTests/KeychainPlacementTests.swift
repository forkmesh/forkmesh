import XCTest

@testable import FMAccount

final class KeychainPlacementTests: XCTestCase {
    func testOnlyChatKeysReachTheNotificationExtension() {
        XCTAssertEqual(KeychainPlacement.group(forSecureKey: "chat/e2ee/ed25519IdentityV3"), KeychainPlacement.pushGroup)
        XCTAssertEqual(KeychainPlacement.group(forSecureKey: "p/next/chat/e2ee/trustV3/abc"), KeychainPlacement.pushGroup)
        for key in ["identity/ed25519Seed", "auth/session", "p/next/identity/ed25519Seed", "owner/key", "vault/copy",
            "p/x/chat-not-e2ee", "chat/e2eeX", "agent/credentials/claude"]
        {
            XCTAssertEqual(KeychainPlacement.group(forSecureKey: key), KeychainPlacement.appOnlyGroup, key)
        }
        XCTAssertNotEqual(KeychainPlacement.appOnlyGroup, KeychainPlacement.sharedGroup)
        XCTAssertNotEqual(KeychainPlacement.pushGroup, KeychainPlacement.sharedGroup)
    }

    func testItemsInTheOldSharedGroupMoveOut() {
        let shared = KeychainPlacement.sharedGroup
        XCTAssertEqual(
            KeychainPlacement.destination(service: KeychainPlacement.secureService, account: "identity/ed25519Seed", group: shared),
            KeychainPlacement.appOnlyGroup)
        XCTAssertEqual(
            KeychainPlacement.destination(service: KeychainPlacement.secureService, account: "chat/e2ee/prekeysV3", group: shared),
            KeychainPlacement.pushGroup)
        XCTAssertEqual(
            KeychainPlacement.destination(service: KeychainPlacement.pushService, account: "session", group: shared),
            KeychainPlacement.pushGroup)
        XCTAssertNil(KeychainPlacement.destination(service: "com.forkmesh.forkmesh.share", account: "manifest", group: shared))
        XCTAssertNil(KeychainPlacement.destination(service: "com.forkmesh.vault.autofill", account: "autofill-cache-key", group: shared))
        XCTAssertNil(
            KeychainPlacement.destination(
                service: KeychainPlacement.secureService, account: "identity/ed25519Seed", group: KeychainPlacement.appOnlyGroup))
    }
}
