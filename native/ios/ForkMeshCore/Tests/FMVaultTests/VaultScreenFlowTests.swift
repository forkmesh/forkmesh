// The vault screen's models over the real VaultService, the
// real crypto and the real Worker module (through the stdio bridge), with recording stand-ins for
// the native autofill half, the device lock and the clipboard. Plus the screen logic that needs no
// server. The bridge flow skips when python3 is missing.
import FMBase
import Foundation
import XCTest
@testable import FMVault

@MainActor
final class RecordingPlatform: VaultPlatform {
    var providerChosen = false
    var synced: [VaultAutofillLogin] = []
    var clears = 0
    var pending: [VaultPendingSave] = []

    func autofillStatus() async -> VaultAutofillStatus {
        VaultAutofillStatus(supported: true, enabled: providerChosen, deviceSecure: true, logins: synced.count)
    }
    func openAutofillSettings() async { providerChosen = true }
    func syncAutofill(account: String, logins: [VaultAutofillLogin]) async -> Bool {
        synced = logins
        return true
    }
    func clearAutofill() async {
        clears += 1
        synced = []
    }
    func takePendingSaves() async -> [VaultPendingSave] {
        let taken = pending
        pending = []
        return taken
    }
}

@MainActor
final class FakeDeviceAuth: VaultDeviceAuth {
    var answer = true
    var prompts = 0
    func available() async -> Bool { true }
    func authenticate(reason: String) async -> Bool {
        prompts += 1
        return answer
    }
}

@MainActor
final class RecordingClipboard: VaultClipboard {
    var copies: [(String, TimeInterval?)] = []
    func copy(_ text: String, expiresAfter: TimeInterval?) { copies.append((text, expiresAfter)) }
}

final class AccountBox: @unchecked Sendable {
    var name: String
    init(_ name: String) { self.name = name }
}

final class VaultScreenFlowTests: XCTestCase {
    @MainActor
    func testCreateAddAutofillLockUnlockQuickUnlockShareSignOut() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let account = AccountBox("alice")
        let store = InMemorySecureStore()
        let platform = RecordingPlatform()
        let deviceAuth = FakeDeviceAuth()
        let clipboard = RecordingClipboard()
        let clock = FixedClock(epochMillis: 1_700_000_000_000)
        let client = VaultClient(request: bridge.request(as: { account.name }))
        client.newMemoryKiB = VaultKdf.floorMemoryKiB
        client.newPasses = VaultKdf.floorPasses
        let vault = VaultService(
            request: bridge.request(as: { account.name }), accountProvider: { account.name }, store: store,
            platform: platform, deviceAuth: deviceAuth, clock: clock, client: client)
        defer { vault.dispose() }
        let screen = VaultScreenModel(vault: vault, clipboard: clipboard)

        XCTAssertEqual(screen.phase, .loading)
        await screen.start()
        XCTAssertEqual(screen.phase, .notSetUp)
        XCTAssertFalse(screen.showsUnlockedActions)

        // A short master password never reaches the server.
        screen.setupPassword = "short"
        screen.setupConfirm = "short"
        await screen.submitSetup()
        XCTAssertEqual(screen.setupProblem, "Use at least 12 characters for the master password.")
        XCTAssertTrue(bridge.calls.filter { $0.0 == "POST" }.isEmpty)
        screen.setupPassword = "correct horse battery staple"
        screen.setupConfirm = "correct horse battery stapl"
        await screen.submitSetup()
        XCTAssertEqual(screen.setupProblem, "The two passwords do not match.")

        screen.setupPassword = "correct horse battery staple"
        screen.setupConfirm = "correct horse battery staple"
        await screen.submitSetup()
        XCTAssertEqual(screen.setupProblem, "")
        XCTAssertEqual(screen.setupPassword, "", "the form forgets the password")
        let prompt = try XCTUnwrap(screen.recoveryPrompt)
        XCTAssertNotNil(prompt.code.range(of: #"^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$"#, options: .regularExpression))
        // Done waits for the checkbox.
        XCTAssertFalse(prompt.canFinish)
        screen.finishRecoveryPrompt()
        XCTAssertNotNil(screen.recoveryPrompt)
        prompt.saved = true
        screen.finishRecoveryPrompt()
        XCTAssertNil(screen.recoveryPrompt)
        XCTAssertEqual(vault.phase, .unlocked)
        XCTAssertTrue(screen.showsUnlockedActions)

        // Add a login through the sheet's model.
        let draft = screen.draft(for: nil)
        XCTAssertEqual(draft.sheetTitle, "New login")
        XCTAssertFalse(draft.readOnly)
        draft.title = "Registrar"
        draft.url = "https://www.registrar.example/login"
        draft.username = "ops@acme.test"
        draft.generate()
        XCTAssertEqual(draft.password.count, 24)
        XCTAssertTrue(draft.reveal)
        XCTAssertEqual(draft.revealLabel, "Hide")
        let generated = draft.password
        let savedDraft = await screen.save(draft)
        XCTAssertTrue(savedDraft)
        XCTAssertEqual(screen.notice, "Saved, encrypted.")
        XCTAssertEqual(vault.items.count, 1)
        let registrar = try XCTUnwrap(vault.items.first)
        XCTAssertEqual(screen.rows, [VaultRow(id: registrar.id, title: "Registrar", subtitle: "ops@acme.test · registrar.example",
                                              icon: "key_outlined", readable: true)])
        // Autofill is opt-in: nothing was handed to the native side yet.
        XCTAssertTrue(platform.synced.isEmpty)

        // Copy clears itself after 30 seconds.
        screen.copyPassword(registrar)
        XCTAssertEqual(clipboard.copies.last?.0, generated)
        XCTAssertEqual(clipboard.copies.last?.1, 30)
        XCTAssertEqual(screen.notice, "Password copied. The clipboard is cleared in 30 seconds.")
        XCTAssertEqual(screen.actions(for: registrar).map(\.action), [.username, .password, .open, .edit, .share, .delete])
        XCTAssertEqual(screen.siteURL(registrar)?.absoluteString, "https://www.registrar.example/login")

        // Turn autofill on in settings: the native side gets exact hosts.
        let settings = VaultSettingsModel(vault: vault, clipboard: clipboard)
        await settings.start()
        XCTAssertEqual(settings.fingerprint.count, 79)
        XCTAssertNil(settings.autofillState)
        await settings.setAutofill(true)
        XCTAssertEqual(platform.synced, [VaultAutofillLogin(
            id: registrar.id, title: "Registrar", username: "ops@acme.test", password: generated, host: "registrar.example")])
        XCTAssertEqual(store.read("vault/alice/autofill"), "1")
        XCTAssertTrue(settings.showsOpenAutofillSettings)
        XCTAssertEqual(settings.autofillState, "One more step: in Settings \u{2192} General \u{2192} AutoFill & Passwords, turn on ForkMesh.")
        await settings.openAutofillSettings()
        await settings.appResumed()
        XCTAssertFalse(settings.showsOpenAutofillSettings)
        XCTAssertEqual(settings.autofillState?.hasPrefix("ForkMesh is your autofill provider. 1 login ready."), true)

        // Quick unlock asks the device first, and stores nothing when declined.
        XCTAssertTrue(settings.showsQuickUnlock)
        deviceAuth.answer = false
        await settings.setQuickUnlock(true)
        XCTAssertEqual(deviceAuth.prompts, 1)
        XCTAssertFalse(vault.quickUnlockEnabled)
        XCTAssertNil(store.read("vault/alice/quickUnlock"))
        deviceAuth.answer = true
        await settings.setQuickUnlock(true)
        XCTAssertTrue(vault.quickUnlockEnabled)
        XCTAssertNotNil(store.read("vault/alice/quickUnlock"))

        // Locking closes the settings screen too, and the list is gone.
        screen.lock()
        XCTAssertEqual(vault.phase, .locked)
        XCTAssertTrue(settings.shouldClose)
        XCTAssertTrue(vault.items.isEmpty)
        XCTAssertTrue(screen.rows.isEmpty)
        XCTAssertTrue(screen.quickUnlockOffered)

        screen.unlockPassword = "not the master password"
        await screen.submitUnlock()
        XCTAssertEqual(screen.error, "That is not the master password.")
        XCTAssertEqual(screen.unlockPassword, "")

        // A login saved from another app while locked is imported on unlock.
        platform.pending = [VaultPendingSave(title: "Shop", host: "shop.example", username: "alice@shop", password: "typed-pw", app: "com.example.shop")]
        let promptsBefore = deviceAuth.prompts
        await screen.quickUnlock()
        XCTAssertEqual(screen.error, "")
        XCTAssertEqual(vault.phase, .unlocked)
        XCTAssertEqual(deviceAuth.prompts, promptsBefore + 1)
        XCTAssertEqual(Set(vault.items.map(\.title)), ["Registrar", "Shop"])
        XCTAssertEqual(vault.items.first { $0.title == "Shop" }?.notes, "Saved from com.example.shop by autofill.")
        XCTAssertEqual(Set(platform.synced.map(\.host)), ["registrar.example", "shop.example"])

        // Search and filters.
        screen.search = "  SHOP "
        XCTAssertEqual(screen.visibleItems.map(\.title), ["Shop"])
        screen.search = ""
        screen.filter = .shared
        XCTAssertTrue(screen.visibleItems.isEmpty)
        screen.filter = .mine
        XCTAssertEqual(screen.visibleItems.count, 2)
        screen.filter = .all

        // Sharing: check keys first, then seal.
        let bob = bridge.client(for: "bob")
        _ = try await bob.setUp("bob master password")
        let share = VaultShareModel(vault: vault, itemId: registrar.id)
        await share.start()
        XCTAssertEqual(share.screenTitle, "Share Registrar")
        XCTAssertFalse(share.canCheck)
        share.name = " Bob "
        XCTAssertTrue(share.canCheck)
        await share.check()
        XCTAssertEqual(share.preview?.map(\.state), ["new"])
        XCTAssertEqual(share.stateLabel("new"), "first share")
        XCTAssertNotNil(share.preview.flatMap { $0.first.flatMap(share.fingerprintShown) })
        XCTAssertTrue(share.advice.hasPrefix("First share with this person."))
        XCTAssertTrue(share.canShare)
        await share.share()
        XCTAssertEqual(share.message, "Shared with bob (1 sealed).")
        XCTAssertNil(share.preview)
        XCTAssertEqual(vault.find(registrar.id)?.shares, [VaultShare(kind: "user", name: "bob", permission: "read")])
        XCTAssertEqual(screen.row(vault.find(registrar.id)!).subtitle, "ops@acme.test · registrar.example · bob")
        share.permission = "write"
        await share.check()
        XCTAssertEqual(share.preview?.map(\.state), ["pinned"])
        XCTAssertNil(share.preview.flatMap { $0.first.flatMap(share.fingerprintShown) })
        share.permission = "read"
        XCTAssertNil(share.preview, "changing a choice forgets the checked keys")
        let current = try XCTUnwrap(share.item?.shares.first)
        XCTAssertEqual(share.shareTitle(current), "bob")
        XCTAssertEqual(share.shareSubtitle(current), "Can view and use")
        XCTAssertEqual(share.stopConfirmation(current).title, "Stop sharing?")
        await share.stop(current)
        XCTAssertEqual(share.message, "")
        XCTAssertEqual(share.item?.shares, [])
        share.name = "nobody"
        await share.check()
        XCTAssertEqual(share.message, "No ForkMesh user has that name.")

        // Deleting.
        let shop = try XCTUnwrap(vault.items.first { $0.title == "Shop" })
        XCTAssertEqual(screen.deleteConfirmation(shop).title, "Delete this login?")
        await screen.delete(shop)
        XCTAssertEqual(vault.items.map(\.title), ["Registrar"])

        // Settings: a short new password is refused locally; a new recovery code; the export.
        settings.password = "short"
        settings.confirm = "short"
        await settings.changePassword()
        XCTAssertEqual(settings.message, "Use at least 12 characters for the master password.")
        await settings.renewRecoveryCode()
        XCTAssertNotNil(settings.recoveryPrompt)
        await settings.copyExport()
        XCTAssertEqual(settings.message, VaultSettingsModel.exportCopied)
        XCTAssertTrue(clipboard.copies.last?.0.contains("wrappedKeyset") == true)
        XCTAssertNil(clipboard.copies.last?.1)

        // The background grace.
        vault.appPaused()
        clock.advance(by: 30)
        vault.appResumed()
        XCTAssertEqual(vault.phase, .unlocked, "a brief trip to the background keeps it open")
        vault.appPaused()
        clock.advance(by: 61)
        vault.appResumed()
        XCTAssertEqual(vault.phase, .locked)

        // The idle lock.
        _ = try await vault.unlockWithDevice()
        XCTAssertEqual(vault.phase, .unlocked)
        vault.secondsPerMinute = 0.05
        await settings.setAutoLock(1)
        XCTAssertEqual(store.read("vault/alice/autoLockMinutes"), "1")
        XCTAssertEqual(settings.autoLockLabel(1), "1 minute idle")
        XCTAssertEqual(settings.autoLockLabel(15), "15 minutes idle")
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(vault.phase, .locked)

        // Sign-out: nothing stays on the device.
        _ = try await vault.unlockWithDevice()
        account.name = ""
        await vault.signedOut()
        XCTAssertEqual(vault.phase, .signedOut)
        XCTAssertTrue(platform.synced.isEmpty)
        XCTAssertNil(store.read("vault/alice/quickUnlock"))
        XCTAssertNil(store.read("vault/alice/autofill"))
        try await vault.refresh()
        XCTAssertEqual(vault.phase, .signedOut)
    }

    @MainActor
    func testAnotherFeatureSavesAndFindsALoginThroughTheService() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let account = AccountBox("erin")
        let vault = VaultService(request: bridge.request(as: { account.name }), accountProvider: { account.name },
                                 store: InMemorySecureStore(), client: bridge.client(for: "erin"))
        defer { vault.dispose() }
        let none = await vault.saveLogin(title: "OpenWrt router (192.168.1.1)", url: "http://192.168.1.1", username: "root", password: "router-pass")
        XCTAssertEqual(none, .noVault)
        try await vault.refresh()
        _ = try await vault.create("erin master password")
        let saved = await vault.saveLogin(title: "OpenWrt router (192.168.1.1)", url: "http://192.168.1.1", username: "root", password: "router-pass")
        XCTAssertEqual(saved, .saved(changed: true))
        let same = await vault.saveLogin(title: "x", url: "http://192.168.1.1", username: "root", password: "router-pass")
        XCTAssertEqual(same, .saved(changed: false))
        guard case let .found(logins) = await vault.findLogins(url: "http://192.168.1.1:80") else { return XCTFail("nothing found") }
        XCTAssertEqual(logins.map(\.password), ["router-pass"])
        let answer1 = await vault.findLogins(url: "https://192.168.1.1")
        XCTAssertEqual(answer1, .found([]))
        let answer2 = await vault.saveLogin(title: "x", url: "nowhere at all", username: "root", password: "p")
        XCTAssertEqual(answer2, .failed("That login has no web address the vault can match."))

        vault.lock()
        let answer3 = await vault.findLogins(url: "http://192.168.1.1")
        XCTAssertEqual(answer3, .locked)
        let answer4 = await vault.saveLogin(title: "x", url: "http://192.168.1.1", username: "root", password: "new")
        XCTAssertEqual(answer4, .locked)
        try await vault.unlock("erin master password")
        guard case let .found(again) = await vault.findLogins(url: "http://192.168.1.1") else { return XCTFail("nothing found after unlock") }
        XCTAssertEqual(again.count, 1)

        account.name = ""
        let answer5 = await vault.findLogins(url: "http://192.168.1.1")
        XCTAssertEqual(answer5, .signedOut)
    }

    @MainActor
    func testDestroyAndAStaleQuickUnlockCopy() async throws {
        let bridge = try VaultBridge.start()
        defer { bridge.stop() }
        let store = InMemorySecureStore()
        let clipboard = RecordingClipboard()
        let deviceAuth = FakeDeviceAuth()
        func service() -> VaultService {
            let client = bridge.client(for: "dave")
            return VaultService(request: bridge.request(as: { "dave" }), accountProvider: { "Dave" }, store: store,
                                deviceAuth: deviceAuth, client: client)
        }
        let vault = service()
        try await vault.refresh()
        _ = try await vault.create("dave master password")
        let quickOn = await vault.setQuickUnlock(true)
        XCTAssertTrue(quickOn)
        let settings = VaultSettingsModel(vault: vault, clipboard: clipboard)
        XCTAssertEqual(settings.destroyConfirmation.confirm, "Delete")
        await settings.destroy()
        XCTAssertEqual(vault.phase, .notSetUp)
        XCTAssertNotNil(clipboard.copies.last)
        XCTAssertNil(store.read("vault/dave/quickUnlock"))

        // A vault recreated elsewhere: the keyset kept here opens nothing and is dropped.
        let elsewhere = service()
        try await elsewhere.refresh()
        _ = try await elsewhere.create("dave second password")
        let quickElsewhere = await elsewhere.setQuickUnlock(true)
        XCTAssertTrue(quickElsewhere)
        let stale = try XCTUnwrap(store.read("vault/dave/quickUnlock"))
        elsewhere.lock()
        _ = try await bridge.client(for: "dave").destroy()
        let third = service()
        try await third.refresh()
        _ = try await third.create("dave third password")
        third.lock()
        store.write("vault/dave/quickUnlock", stale)
        try await third.refresh()
        XCTAssertTrue(third.quickUnlockEnabled)
        let screen = VaultScreenModel(vault: third, clipboard: clipboard)
        await screen.quickUnlock()
        XCTAssertEqual(screen.error, "The keys stored for this account do not match its published keys. Nothing was unlocked.")
        XCTAssertFalse(third.quickUnlockEnabled)
        XCTAssertNil(store.read("vault/dave/quickUnlock"))
    }

    // MARK: Without a server

    @MainActor
    func testScreenLogic() {
        XCTAssertEqual(VaultCopy.errorText(VaultError("wrong_password")), "That is not the master password.")
        XCTAssertEqual(VaultCopy.errorText(VaultCryptoError("invalid_share")), "invalid share")
        XCTAssertEqual(VaultCopy.errorText(URLError(.timedOut)), "Something went wrong. Check your connection and try again.")
        XCTAssertNil(VaultCopy.newPasswordProblem("twelve chars", "twelve chars"))

        let vault = VaultService(request: { _, _, _ in throw VaultError("offline") }, accountProvider: { "" }, store: InMemorySecureStore())
        let screen = VaultScreenModel(vault: vault, clipboard: RecordingClipboard())
        let shared = VaultItem(
            id: "a", owner: "bob", owned: false, permission: "read", revision: 1, updatedAt: 0, updatedBy: "carol",
            viaKind: "organization", viaName: "acme", fields: ["title": "Bank", "password": "pw", "url": "javascript:alert(1)"])
        XCTAssertEqual(screen.row(shared).subtitle, "from bob via acme · view only")
        XCTAssertEqual(screen.row(shared).icon, "people_outline")
        XCTAssertEqual(screen.actions(for: shared).map(\.action), [.password, .edit], "no site, no share, no removal of an org share")
        XCTAssertEqual(screen.actions(for: shared).last?.label, "View")
        XCTAssertNil(screen.siteURL(shared))
        XCTAssertEqual(screen.deleteConfirmation(shared).message, "bob keeps \"Bank\".")
        let draft = VaultItemDraft(item: shared)
        XCTAssertTrue(draft.readOnly)
        XCTAssertEqual(draft.sheetTitle, "Login")
        XCTAssertEqual(draft.sharedByLine, "Shared by bob, last changed by carol.")
        XCTAssertEqual(VaultItemDraft.maxLength("notes"), 8000)

        let direct = VaultItem(id: "b", owner: "bob", owned: false, permission: "write", revision: 1, updatedAt: 0, updatedBy: "bob",
                               viaKind: "user", viaName: "alice", fields: ["title": "Mail"])
        XCTAssertEqual(screen.actions(for: direct).map(\.label), ["Copy password", "Edit", "Remove from my vault"])
        XCTAssertEqual(VaultItemDraft(item: direct).sheetTitle, "Edit login")
        XCTAssertEqual(VaultItemDraft(item: direct).sharedByLine, "Shared by bob.")

        let closed = VaultItem(id: "c", owner: "bob", owned: false, permission: "read", revision: 1, updatedAt: 0, updatedBy: "", error: "sender_keys_changed")
        XCTAssertEqual(screen.row(closed), VaultRow(
            id: "c", title: "Unreadable item",
            subtitle: "bob's keys changed since you first received a share from them. It stays closed until you compare fingerprints.",
            icon: "warning_amber_outlined", readable: false))
        let broken = VaultItem(id: "d", owner: "me", owned: true, permission: "owner", revision: 1, updatedAt: 0, updatedBy: "", error: "decryption_failed")
        XCTAssertEqual(screen.row(broken).subtitle, "This item could not be opened with your keys (decryption failed).")

        let form = VaultRecoverForm()
        form.code = "code"
        form.password = "twelve chars"
        form.confirm = "twelve chars!"
        XCTAssertNil(form.submit())
        XCTAssertEqual(form.problem, "The two passwords do not match.")
        form.confirm = "twelve chars"
        XCTAssertEqual(form.submit()?.password, "twelve chars")
        XCTAssertEqual(VaultFilter.shared.label, "Shared with me")
    }

    @MainActor
    func testRefreshWhileSignedOutAndAnOfflineServer() async throws {
        let account = AccountBox("")
        let vault = VaultService(request: { _, _, _ in throw VaultError("invalid_session") }, accountProvider: { account.name }, store: InMemorySecureStore())
        let screen = VaultScreenModel(vault: vault, clipboard: RecordingClipboard())
        await screen.start()
        XCTAssertEqual(vault.phase, .signedOut)
        account.name = "alice"
        await screen.start()
        XCTAssertEqual(screen.error, "Sign in to ForkMesh to open your vault.")
        XCTAssertFalse(screen.busy)
        let quickLocked = await vault.setQuickUnlock(true)
        XCTAssertFalse(quickLocked, "nothing to keep while locked")
        let unlockedByDevice = try await vault.unlockWithDevice()
        XCTAssertFalse(unlockedByDevice)
    }
}
