// The settings screen's logic (native/android SettingsLogicTest.kt) and the Settings view models.

import FMAccount
import FMBase
import FMNet
import XCTest
@testable import FMApi

private let now: Int64 = 1_770_000_000_000

private func settingsSession(_ extra: JSONObject = [:]) -> AuthSession {
    var json: JSONObject = ["nodeName": "alice", "email": "alice@example.com", "status": "active"]
    json.merge(extra)
    return AuthSession.fromJSON(json)
}

private func device(
    _ id: String,
    _ kind: String,
    capabilities: [String] = ["owner_sign"],
    enabled: Bool = true,
    revokedAt: Int64 = 0,
    lastSeen: Int64 = 0,
    label: String = "",
    current: Bool = false
) -> AccountDevice {
    AccountDevice(
        id: id, pubkey: "", kind: kind, label: label, capabilities: capabilities, enabled: enabled,
        lastSeen: lastSeen, revokedAt: revokedAt, current: current)
}

private struct Boom: Error {}

final class SettingsViewModelTests: XCTestCase {
    func testProfileCardCoversGuestPreviewSignedOutAndEmailOnly() {
        XCTAssertEqual(
            profileCardText(nil, guest: true), ProfileCardText("Guest", "Exploring read-only without an account"))
        XCTAssertEqual(
            profileCardText(settingsSession(["sessionKind": "preview"]), guest: false),
            ProfileCardText(
                "Preview mode", "This is local preview data. Sign out and log in to show your Worker account."))
        XCTAssertEqual(
            profileCardText(nil, guest: false),
            ProfileCardText("Not signed in", "No Worker account session found on this device"))
        XCTAssertEqual(
            profileCardText(AuthSession.fromJSON(["email": "bob@example.com"]), guest: false),
            ProfileCardText("bob@example.com", "bob@example.com"))
        XCTAssertEqual(profileCardText(settingsSession(), guest: false), ProfileCardText("alice", "alice@example.com"))
    }

    func testDisplayNameAdoptsTheAccountOnlyWhenEmptyOrPreview() {
        XCTAssertEqual(adoptedDisplayName("", session: settingsSession()), "alice")
        XCTAssertEqual(adoptedDisplayName("preview-node", session: settingsSession()), "alice")
        XCTAssertNil(adoptedDisplayName("Alice W", session: settingsSession()))
        XCTAssertNil(adoptedDisplayName("", session: nil))
    }

    func testThisDeviceCardStates() {
        XCTAssertEqual(thisDeviceCard(nil, nowMillis: now), .signInToRegister)
        XCTAssertEqual(thisDeviceCard(settingsSession(["sessionKind": "preview"]), nowMillis: now), .signInToRegister)
        XCTAssertEqual(thisDeviceCard(settingsSession(), nowMillis: now), .notRegistered)

        let bound = settingsSession([
            "sessionKind": "mobile_node",
            "deviceId": "abcdef0123456789",
            "capabilities": ["publish_repo", "owner_sign"],
            "devices": [
                [
                    "id": "abcdef0123456789",
                    "kind": "mobile_node",
                    "label": " Pixel 9 · Android 16 ",
                    "capabilities": ["publish_repo", "owner_sign"],
                    "lastSeen": .int(now - 5 * 60_000),
                ]
            ],
        ])
        XCTAssertEqual(
            thisDeviceCard(bound, nowMillis: now),
            .mobileNode(
                label: "Pixel 9 · Android 16", deviceId: "abcdef…6789", capabilities: "Publishes · signs as owner",
                lastSeen: "5m ago", canServeAsNode: true))

        // A mobile-node session whose device row did not come back.
        let rowless = settingsSession(["sessionKind": "mobile_node", "capabilities": ["owner_sign"]])
        XCTAssertEqual(
            thisDeviceCard(rowless, nowMillis: now),
            .mobileNode(label: "This phone", deviceId: "—", capabilities: "Signs as owner", lastSeen: "now", canServeAsNode: true))
    }

    func testShortFormsAndCounts() {
        XCTAssertEqual(capabilitySummary([]), "Browse only")
        XCTAssertEqual(capabilitySummary(["publish_repo", "host_repo"]), "Publishes")
        XCTAssertEqual(shortDeviceId("short"), "short")
        XCTAssertEqual(shortDeviceId("0123456789"), "0123456789")
        XCTAssertEqual(shortDeviceKey("abcdefghijklmnopqrstu"), "abcdefghi...pqrstu")
        XCTAssertEqual(shortDeviceKey("18-characters-key!"), "18-characters-key!")
        XCTAssertEqual(countLabel(1, "desktop node"), "1 desktop node")
        XCTAssertEqual(countLabel(0, "mobile node"), "0 mobile nodes")
    }

    func testNodesSummaryPrefersTheRefreshedRegistryAndSkipsRevokedNodes() {
        let cached = settingsSession([
            "devices": [["id": "d1", "kind": "desktop_node", "capabilities": ["host_repo"]]]
        ])
        let fromLogin = nodesSummary(nil, session: cached)
        XCTAssertEqual(fromLogin.headline, "1 desktop node · 0 mobile nodes")
        XCTAssertTrue(fromLogin.showsDesktopNote)

        let refreshed = nodesSummary(
            [
                device("d1", "desktop_node", revokedAt: 5),
                device("m1", "mobile_node"),
                device("m2", "mobile_node", enabled: false),
            ],
            session: cached)
        XCTAssertEqual(refreshed.headline, "0 desktop nodes · 1 mobile node")
        XCTAssertFalse(refreshed.showsDesktopNote)

        XCTAssertEqual(nodesSummary([], session: cached).headline, "No nodes registered yet")
        XCTAssertEqual(nodesEmptyText(loading: true), "Loading nodes…")
        XCTAssertTrue(nodesEmptyText(loading: false).hasPrefix("Log in from the ForkMesh desktop app"))
    }

    func testDeviceRows() {
        let bound = settingsSession(["deviceId": "m1"])
        let mine = device("m1", "mobile_node", lastSeen: now - 2 * 3_600_000, label: "Pixel")
        let other = device("d1", "desktop_node", capabilities: ["host_repo"])
        XCTAssertTrue(isCurrentDevice(mine, session: bound))
        XCTAssertFalse(isCurrentDevice(other, session: bound))
        var currentOther = other
        currentOther.current = true
        XCTAssertTrue(isCurrentDevice(currentOther, session: settingsSession()))
        XCTAssertEqual(deviceRowTitle(mine, isCurrent: true), "Pixel (this device)")
        XCTAssertEqual(deviceRowTitle(other, isCurrent: false), "Desktop node")
        XCTAssertEqual(deviceRowDetail(mine), "Mobile node · Owner signing")
        XCTAssertEqual(deviceRowTimeline(mine, nowMillis: now), "Last seen 2h ago")
        var revoked = mine
        revoked.revokedAt = now
        XCTAssertEqual(deviceRowTimeline(revoked, nowMillis: now), "Revoked just now")
        XCTAssertNil(deviceRowTimeline(other, nowMillis: now))
        XCTAssertEqual(deviceTone(mine), .success)
        XCTAssertEqual(deviceTone(other), .warning)
        var disabled = mine
        disabled.enabled = false
        XCTAssertEqual(deviceTone(disabled), .offline)
    }

    func testWebSettingsUrlFollowsTheRelayOrigin() {
        XCTAssertEqual(
            webSettingsURL("wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"),
            "https://app.forkmesh.com/dashboard/settings/nodes")
        XCTAssertEqual(webSettingsURL("ws://localhost:8787/api/x/ws"), "http://localhost:8787/dashboard/settings/nodes")
        XCTAssertEqual(webSettingsURL("wss://staging.forkmesh.com:443/ws"), "https://staging.forkmesh.com/dashboard/settings/nodes")
        XCTAssertEqual(webSettingsURL("ws://test.local:80/ws"), "http://test.local/dashboard/settings/nodes")
        XCTAssertEqual(webSettingsURL(""), "https://app.forkmesh.com/dashboard/settings/nodes")
        XCTAssertEqual(webSettingsURL("not a url"), "https://app.forkmesh.com/dashboard/settings/nodes")
    }

    func testPushLabels() {
        XCTAssertTrue(showsPushCard(supported: true, guest: false))
        XCTAssertFalse(showsPushCard(supported: false, guest: false))
        XCTAssertFalse(showsPushCard(supported: true, guest: true))
        XCTAssertTrue(showsPingSettings(session: settingsSession(), guest: false))
        XCTAssertFalse(showsPingSettings(session: settingsSession(), guest: true))
        XCTAssertFalse(showsPingSettings(session: settingsSession(["sessionKind": "preview"]), guest: false))
        XCTAssertFalse(showsPingSettings(session: nil, guest: false))
        XCTAssertEqual(pushPermissionLabel("authorized"), "Allowed")
        XCTAssertEqual(pushPermissionLabel("denied"), "Turned off in iOS Settings")
        XCTAssertEqual(pushPermissionLabel("unknown"), "Not checked yet")
        XCTAssertEqual(pushTokenLabel(token: "", permission: "denied", environment: "", registered: false), "Not requested")
        XCTAssertEqual(
            pushTokenLabel(token: "", permission: "authorized", environment: "", registered: false),
            "Waiting for Apple to issue one")
        XCTAssertEqual(
            pushTokenLabel(token: "t", permission: "authorized", environment: "development", registered: true),
            "Registered with the relay (APNs sandbox)")
        XCTAssertEqual(
            pushTokenLabel(token: "t", permission: "authorized", environment: "production", registered: false),
            "Issued, not yet registered (APNs production)")
        XCTAssertEqual(pushRelayLabel(nil), "Unknown until the next registration")
        XCTAssertEqual(pushRelayLabel(false), "No APNs credentials configured")
        XCTAssertEqual(pushRelayLabel(true), "Can send pushes")
        XCTAssertTrue(pushCanEnable(token: "", permission: "notDetermined"))
        XCTAssertFalse(pushCanEnable(token: "", permission: "denied"))
        XCTAssertFalse(pushCanEnable(token: "t", permission: "authorized"))
        XCTAssertEqual(pushSendLabel(sending: true), "Sending…")
        XCTAssertEqual(pushSendLabel(sending: false), "Send test notification")
    }

    func testAccountCardAndDeleteAccountRules() {
        XCTAssertEqual(accountCard(guest: true), .guest)
        XCTAssertEqual(accountCard(guest: false), .signOut)
        XCTAssertEqual(signedInAsName(nil), "Current account")
        XCTAssertEqual(signedInAsName(AuthSession.fromJSON(["email": "bob@example.com"])), "bob@example.com")
        XCTAssertEqual(signedInAsName(settingsSession()), "alice")
        XCTAssertFalse(showsDeleteAccount(nil))
        XCTAssertFalse(showsDeleteAccount(settingsSession(["sessionKind": "preview"])))
        XCTAssertTrue(showsDeleteAccount(settingsSession()))
        XCTAssertEqual(deleteAccountConfirmError("delete"), "Type DELETE to confirm account deletion.")
        XCTAssertNil(deleteAccountConfirmError(" DELETE "))
        XCTAssertEqual(
            deleteAccountFailure(AuthException("bad_password", "Incorrect password. Check it and try again.")),
            "Incorrect password. Check it and try again.")
        XCTAssertEqual(deleteAccountFailure(Boom()), "Could not delete account. Check your password and try again.")
        XCTAssertTrue(showsUserStatus(settingsSession(), guest: false))
        XCTAssertFalse(showsUserStatus(settingsSession(), guest: true))
        XCTAssertFalse(showsUserStatus(settingsSession(["sessionKind": "preview"]), guest: false))
    }

    func testSessionRows() {
        let mine = AccountSession(id: "s1", deviceLabel: "Mobile app", ipAddress: "203.0.113.9", lastSeenAt: now - 60_000, current: true)
        let browser = AccountSession(id: "s2", deviceLabel: "Browser", createdAt: now - 3 * 86_400_000)
        XCTAssertEqual(sessionRowTitle(mine), "Mobile app (this app)")
        XCTAssertEqual(sessionRowTitle(browser), "Browser")
        XCTAssertEqual(sessionRowMeta(mine, nowMillis: now), "Active 1m ago · 203.0.113.9")
        XCTAssertEqual(sessionRowMeta(browser, nowMillis: now), "Signed in 3d ago")
        XCTAssertEqual(sessionRowIcon(mine), "phone_iphone")
        XCTAssertEqual(sessionRowIcon(browser), "language")
        XCTAssertEqual(sessionRowIcon(AccountSession(id: "x", deviceLabel: "Desktop app")), "desktop_mac_outlined")
        XCTAssertEqual(sessionRowIcon(AccountSession(id: "x", deviceLabel: "Unknown device")), "devices_other")
        XCTAssertEqual(sessionsHeadline(nil), "Signed-in sessions")
        XCTAssertEqual(sessionsHeadline([mine]), "1 active session")
        XCTAssertEqual(sessionsHeadline([mine, browser]), "2 active sessions")
        XCTAssertEqual(revokeOneConfirmation(mine).title, "Log out of this app?")
        XCTAssertEqual(
            revokeOneConfirmation(browser).body, "Browser will be signed out and must sign in again to use your account.")
        XCTAssertEqual(revokeOthersConfirmation(count: 3).target, "others")
        XCTAssertTrue(revokeOthersConfirmation(count: 3).body.contains("(3)"))
        XCTAssertEqual(sessionsEmptyText(apiAvailable: false, loading: false, loaded: false), "Session management is unavailable in this build.")
        XCTAssertEqual(sessionsEmptyText(apiAvailable: true, loading: false, loaded: false), "Loading sessions…")
        XCTAssertEqual(sessionsEmptyText(apiAvailable: true, loading: false, loaded: true), "No active sessions found.")
    }

    // MARK: Models

    func testTheFormHasNoPayoutAddressField() {
        let fields = Set(SettingsFormModel.Field.allCases.map { String(describing: $0) })
        XCTAssertEqual(fields, ["displayName", "room", "passphrase", "github", "gitlab"])
    }

    @MainActor
    func testTheFormSavesTrimmedTextAndAdoptsTheAccountName() async {
        let prefs = InMemoryKeyValueStore([:])
        let settings = SettingsService(prefs: prefs, secureStore: InMemorySecureStore([:]))
        let form = SettingsFormModel(settings: settings)
        XCTAssertEqual(form.room, "world-general")
        form.setText(.displayName, "  Alice W ")
        XCTAssertEqual(settings.displayName, "Alice W")
        XCTAssertEqual(form.displayName, "  Alice W ")
        form.setText(.github, " ghp_x ")
        XCTAssertEqual(settings.githubToken, "ghp_x")

        let empty = SettingsFormModel(settings: SettingsService(prefs: InMemoryKeyValueStore([:]), secureStore: InMemorySecureStore([:])))
        empty.adoptAccountName(settingsSession())
        XCTAssertEqual(empty.displayName, "alice")
        // Shown, not saved.
        XCTAssertEqual(empty.settings.displayName, "")
    }

    @MainActor
    func testNodesCardFallsBackToTheLoginInventory() async {
        let cached = settingsSession(["devices": [["id": "d1", "kind": "desktop_node"]]])
        let failing = AccountNodesModel(session: cached, listDevices: { throw Boom() })
        await failing.load()
        XCTAssertEqual(failing.error, SettingsCopy.nodesRefreshFailed)
        XCTAssertEqual(failing.summary?.devices.map(\.id), ["d1"])

        let fresh = AccountNodesModel(session: cached, listDevices: { [device("m1", "mobile_node")] })
        await fresh.load()
        XCTAssertEqual(fresh.error, "")
        XCTAssertEqual(fresh.summary?.headline, "0 desktop nodes · 1 mobile node")

        let signedOut = AccountNodesModel(session: nil, listDevices: { XCTFail("no read signed out"); return [] })
        await signedOut.load()
        XCTAssertNil(signedOut.summary)
    }

    @MainActor
    func testSessionsCardRevokesAndDropsRows() async {
        let rows = [
            AccountSession(id: "s1", deviceLabel: "Mobile app", current: true),
            AccountSession(id: "s2", deviceLabel: "Browser"),
            AccountSession(id: "s3", deviceLabel: "Desktop app"),
        ]
        var revoked: [String] = []
        let model = AccountSessionsModel(
            listSessions: { rows },
            revokeSession: { target in
                revoked.append(target)
                return target == "s1"
            })
        await model.load()
        XCTAssertEqual(model.othersCount, 2)
        XCTAssertEqual(model.headline, "3 active sessions")
        let one = await model.revoke("s2")
        XCTAssertEqual(one, .revoked("Session logged out."))
        XCTAssertEqual(model.visible.map(\.id), ["s1", "s3"])
        let others = await model.revoke("others")
        XCTAssertEqual(others, .revoked("Logged out of every other session."))
        XCTAssertEqual(model.visible.map(\.id), ["s1"])
        let current = await model.revoke("s1")
        XCTAssertEqual(current, .currentSignedOut)
        XCTAssertEqual(revoked, ["s2", "others", "s1"])
    }

    @MainActor
    func testSessionsCardReportsFailures() async {
        let model = AccountSessionsModel(listSessions: { throw Boom() }, revokeSession: { _ in throw Boom() })
        await model.load()
        XCTAssertEqual(model.error, SettingsCopy.sessionsLoadFailed)
        XCTAssertEqual(model.emptyText, "Loading sessions…")
        let outcome = await model.revoke("s9")
        XCTAssertEqual(outcome, .failed)
        XCTAssertEqual(model.error, SettingsCopy.sessionRevokeFailed)
        XCTAssertTrue(model.canRevoke)
        let unavailable = AccountSessionsModel(listSessions: nil, revokeSession: nil)
        XCTAssertEqual(unavailable.emptyText, "Session management is unavailable in this build.")
        let ignored = await unavailable.revoke("x")
        XCTAssertEqual(ignored, .ignored)
    }

    @MainActor
    func testDeleteAccountNeedsTheWordAndReportsRefusals() async {
        var passwords: [String] = []
        let model = DeleteAccountModel(delete: { password in
            passwords.append(password)
            throw AuthException("bad_password", "Incorrect password. Check it and try again.")
        })
        model.password = "pw"
        model.confirm = "delete"
        let first = await model.submit()
        XCTAssertFalse(first)
        XCTAssertEqual(model.error, "Type DELETE to confirm account deletion.")
        XCTAssertTrue(passwords.isEmpty)
        model.confirm = "DELETE"
        let second = await model.submit()
        XCTAssertFalse(second)
        XCTAssertEqual(model.error, "Incorrect password. Check it and try again.")
        XCTAssertEqual(passwords, ["pw"])

        let ok = DeleteAccountModel(delete: { _ in })
        ok.confirm = "DELETE"
        let done = await ok.submit()
        XCTAssertTrue(done)
    }
}
