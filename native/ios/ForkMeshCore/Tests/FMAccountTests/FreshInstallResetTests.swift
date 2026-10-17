import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class FreshInstallResetTests: XCTestCase {
    struct Phone {
        let keychain: MemoryKeychain
        let prefs: InMemoryKeyValueStore

        func reset() -> FreshInstallReset {
            FreshInstallReset(prefs: prefs) {
                for key in try keychain.readAll().keys { try keychain.delete(key) }
            }
        }
    }

    static let leftovers: [String: String] = [
        "identity/ed25519Seed": "seed",
        "auth/session/app.forkmesh.com": "{\"sessionToken\":\"t\"}",
    ]

    func testAReinstallErasesTheKeychainLeftFromTheDeletedApp() {
        let phone = Phone(keychain: MemoryKeychain(Self.leftovers), prefs: InMemoryKeyValueStore())
        XCTAssertEqual(phone.reset().run(), .erased)
        XCTAssertTrue(phone.keychain.snapshot.isEmpty)
        XCTAssertEqual(phone.prefs.bool(forKey: FreshInstallReset.installedKey), true)
        XCTAssertFalse(phone.prefs.contains(FreshInstallReset.pendingKey))
    }

    func testLaterLaunchesKeepTheNewSession() {
        let phone = Phone(keychain: MemoryKeychain(Self.leftovers), prefs: InMemoryKeyValueStore())
        phone.reset().run()
        try? phone.keychain.write("auth/session/app.forkmesh.com", "new")
        XCTAssertEqual(phone.reset().run(), .alreadyInstalled)
        XCTAssertEqual(phone.keychain.snapshot["auth/session/app.forkmesh.com"], "new")
    }

    func testAnUpdatedNativeAppKeepsItsLogin() {
        let phone = Phone(
            keychain: MemoryKeychain(Self.leftovers),
            prefs: InMemoryKeyValueStore(["relays/active": "app.forkmesh.com"]))
        XCTAssertEqual(phone.reset().run(), .upgraded)
        XCTAssertEqual(phone.keychain.snapshot, Self.leftovers)
        XCTAssertEqual(phone.reset().run(), .alreadyInstalled)
    }

    func testALockedKeychainIsErasedOnTheNextLaunch() {
        let keychain = MemoryKeychain(Self.leftovers)
        let phone = Phone(keychain: keychain, prefs: InMemoryKeyValueStore())
        keychain.failing = true
        guard case .deferred = phone.reset().run() else { return XCTFail("expected deferred") }
        phone.prefs.set("dark", forKey: "appearance/themeMode")
        keychain.failing = false
        XCTAssertEqual(phone.reset().run(), .erased)
        XCTAssertTrue(keychain.snapshot.isEmpty)
    }
}
