// The push notification service (`PushTestResult.summary` is in PushTestResultTests) and
// native/android core/src/test/.../api/PushBinderTest.kt. The platform is the `PushPlatform`
// protocol, faked by `RecordingPlatform`, with the callbacks `didReceiveToken`,
// `didFailToRegister` and `didOpen`. ApiService is final, so a FakeHTTPClient answers
// /api/accounts/register-device and /api/accounts/push-test.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

/// The native half, recording every call (`calls` = method names, `nativeCalls` with arguments).
@MainActor
private final class RecordingPlatform: PushPlatform {
    struct Call: Equatable {
        let method: String
        let arguments: [String: String]
    }

    var nativeCalls: [Call] = []
    var calls: [String] { nativeCalls.map(\.method) }

    var permission: String? = "notDetermined"
    var granted: Bool? = true
    var environmentAnswer: String? = "development"
    var cachedToken: String? = ""
    var parkedOpen: PushOpen?
    var failPermissionStatus: Error?

    private func record(_ method: String, _ arguments: [String: String] = [:]) {
        nativeCalls.append(Call(method: method, arguments: arguments))
    }

    func permissionStatus() async throws -> String? {
        record("permissionStatus")
        if let failPermissionStatus { throw failPermissionStatus }
        return permission
    }

    func requestPermission() async throws -> Bool? {
        record("requestPermission")
        return granted
    }

    func environment() async throws -> String? {
        record("environment")
        return environmentAnswer
    }

    func getToken() async throws -> String? {
        record("getToken")
        return cachedToken
    }

    func takePendingOpen() async throws -> PushOpen? {
        record("takePendingOpen")
        return parkedOpen
    }

    func setBadge(_ count: Int) async throws { record("setBadge", ["count": "\(count)"]) }

    func setCredentials(origin: String, account: String, session: String) async throws {
        record("setCredentials", ["origin": origin, "account": account, "session": session])
    }

    func clearCredentials() async throws { record("clearCredentials") }

    func clearCredentials(origin: String) async throws { record("clearCredentials", ["origin": origin]) }

    /// "" is a platform that does not say (the protocol default).
    var bundleIdAnswer = ""
    func appBundleId() -> String { bundleIdAnswer }

    func registerForCalls() async throws { record("registerForCalls") }
}

/// A PlatformException from the bridge (Kotlin's IllegalStateException("bridge missing")).
private struct BridgeError: Error, CustomStringConvertible {
    let description: String
}

/// The Worker's device-registration and push-test endpoints.
private final class PushRelay: @unchecked Sendable {
    let registered = SvcBox<[[String: String]]>([])
    let unregistered = SvcBox<[String]>([])
    let testPushes = SvcBox(0)
    /// `pushConfigured` in the registration answer; nil leaves the field out (an older relay).
    let relayConfigured = SvcBox<Bool?>(true)
    /// Answer registrations with this status instead of 200; 0 means the relay is unreachable.
    let registerStatus = SvcBox(200)
    let registerDelayMs = SvcBox(0)
    let testAnswer = SvcBox<JSONObject>([
        "ok": true, "configured": true, "devices": 1, "delivered": 1,
        "results": [["environment": "development", "delivered": true, "status": 200, "reason": ""]],
    ])

    lazy var http = FakeHTTPClient { request in
        let delay = self.registerDelayMs.value
        if delay > 0, request.method == "POST", request.url.path == "/api/accounts/register-device" {
            try await Task.sleep(for: .milliseconds(delay))
        }
        return try self.answer(request)
    }

    private func answer(_ request: HTTPRequest) throws -> HTTPResponse {
        let body = JSON.parseObject(request.bodyText)
        switch (request.method, request.url.path) {
        case ("POST", "/api/accounts/register-device"):
            registered.mutate { $0.append(["token": body.str("deviceToken"), "environment": body.str("environment")]) }
            let status = registerStatus.value
            if status == 0 { throw HTTPError.transport("offline") }
            if status != 200 { return HTTPResponse(status: status, json: ["error": "forbidden"]) }
            var answer: JSONObject = ["ok": true]
            if let configured = relayConfigured.value { answer["pushConfigured"] = .bool(configured) }
            return HTTPResponse(status: 200, json: .object(answer))
        case ("DELETE", "/api/accounts/register-device"):
            unregistered.mutate { $0.append(body.str("deviceToken")) }
            return HTTPResponse(status: 200, json: ["ok": true])
        case ("POST", "/api/accounts/push-test"):
            testPushes.mutate { $0 += 1 }
            return HTTPResponse(status: 200, json: .object(testAnswer.value))
        default:
            return HTTPResponse(status: 404, text: "not found")
        }
    }
}

@MainActor
final class PushNotificationServiceTests: XCTestCase {
    private let tokenA = String(repeating: "a", count: 64)
    private let tokenB = String(repeating: "b", count: 64)

    private func api(_ relay: PushRelay, session: @escaping @MainActor () -> String = { "" }) -> ApiService {
        svcApi(relay.http, token: session)
    }

    private func build(_ api: ApiService, _ platform: RecordingPlatform) -> PushNotificationService {
        PushNotificationService(api: api, platform: platform, supported: true)
    }

    private func registrations(_ relay: PushRelay) -> [[String: String]] { relay.registered.value }

    // MARK: push notification service

    func testAGrantedPhoneRegistersItsTokenAgainstTheAccount() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        XCTAssertTrue(platform.calls.contains("requestPermission"))
        // The token was minted by a sandbox build, so the Worker has to send it to the sandbox
        // gateway or Apple answers BadDeviceToken.
        XCTAssertEqual(registrations(relay), [["token": tokenA, "environment": "development"]])
    }

    func testADeniedPhoneNeverRePromptsButStillRegistersForCalls() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.permission = "denied"
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        XCTAssertEqual(platform.calls.prefix(2), ["permissionStatus", "registerForCalls"])
        XCTAssertFalse(platform.calls.contains("requestPermission"))
        XCTAssertEqual(registrations(relay), [["token": tokenA, "environment": "development"]])
        XCTAssertEqual(push.permission, "denied")
        XCTAssertTrue(push.registered)
    }

    func testDecliningTheSystemPromptStillRegistersForCalls() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.granted = false
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        await push.start(accountKey: "alice")

        XCTAssertTrue(platform.calls.contains("registerForCalls"))
        XCTAssertTrue(registrations(relay).isEmpty)
        XCTAssertEqual(push.permission, "denied")

        await push.didReceiveToken(tokenA)
        XCTAssertEqual(registrations(relay), [["token": tokenA, "environment": "development"]])
    }

    func testARotatedTokenFromAPNsReRegistersOnItsOwn() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        await push.start(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)

        // Reinstall / restore-from-backup hands the app a new token; the stored one is dead from
        // that moment and must be replaced without a sign-in.
        await push.didReceiveToken(tokenB, environment: "production")

        XCTAssertEqual(registrations(relay).count, 2)
        XCTAssertEqual(registrations(relay).last, ["token": tokenB, "environment": "production"])
        XCTAssertEqual(push.token, tokenB)
        XCTAssertEqual(push.environment, "production")
    }

    func testAnUnchangedTokenIsNotRePostedOnEveryRebind() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        await push.start(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)

        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1, "same account and token")

        // Switching accounts must re-bind: the token is stored per account.
        await push.rebind(accountKey: "bob")
        XCTAssertEqual(registrations(relay).count, 2)
    }

    func testOverlappingRebindsAndTokenCallbacksPostTheTokenOnce() async {
        let relay = PushRelay()
        relay.registerDelayMs.value = 100
        let platform = RecordingPlatform()
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        await push.didReceiveToken(tokenA, environment: "production")
        XCTAssertTrue(registrations(relay).isEmpty)

        async let launch: Void = push.rebind(accountKey: "alice")
        async let resume: Void = push.rebind(accountKey: "alice")
        async let apns: Void = push.didReceiveToken(tokenA, environment: "production")
        _ = await (launch, resume, apns)

        XCTAssertEqual(registrations(relay).count, 1)
        XCTAssertTrue(push.registered)

        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)
    }

    func testAFailedRegistrationIsPostedAgainOnTheNextRebind() async {
        let relay = PushRelay()
        relay.registerStatus.value = 0
        let platform = RecordingPlatform()
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        await push.didReceiveToken(tokenA, environment: "production")
        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)
        XCTAssertFalse(push.registered)

        relay.registerStatus.value = 200
        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 2)
        XCTAssertTrue(push.registered)
    }

    func testSigningOutDropsTheRegistrationImmediately() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        await push.stop()
        XCTAssertEqual(relay.unregistered.value, [tokenA])
    }

    func testARegisteredPhoneHandsTheExtensionWhatItNeedsToExpandABanner() async {
        // The notification service extension reads GET /api/push/alert itself to add the actor
        // portrait and badge. It is a separate process and cannot see this app's storage:
        // without the session travelling across here, every banner goes without them.
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay, session: { "session-token" }), platform)
        await push.start(accountKey: "alice")

        let shared = platform.nativeCalls.first { $0.method == "setCredentials" }
        XCTAssertEqual(
            shared?.arguments, ["origin": "https://app.forkmesh.com", "account": "alice", "session": "session-token"])
    }

    func testAReSignedSessionReachesTheExtensionWithoutANewDeviceToken() async {
        // The device registration is keyed on the APNs token, which does not move when a session
        // is re-minted. Sharing only on a registration that actually posts would leave the
        // extension presenting a dead session and reading 401 on every push.
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let session = SvcBox("first-session")
        let push = build(api(relay, session: { session.value }), platform)
        await push.start(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)

        session.value = "second-session"
        await push.rebind(accountKey: "alice")

        XCTAssertEqual(registrations(relay).count, 1, "same account and token")
        XCTAssertEqual(
            platform.nativeCalls.filter { $0.method == "setCredentials" }.map { $0.arguments["session"] ?? "" },
            ["first-session", "second-session"])
    }

    func testAPhoneWithNoSessionSharesNothingAndClearsWhatItHad() async {
        // Registration can run before the session is restored. Publishing an empty token would
        // leave the extension retrying an unauthorized read on every push.
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        XCTAssertFalse(platform.calls.contains("setCredentials"))
        XCTAssertTrue(platform.calls.contains("clearCredentials"))
    }

    func testSigningOutTakesTheSharedSessionWithIt() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay, session: { "session-token" }), platform)
        await push.start(accountKey: "alice")
        XCTAssertTrue(platform.calls.contains("setCredentials"))

        await push.stop()
        // A stale session would keep turning a signed-out account's notifications into banners on
        // a handset that has left it.
        XCTAssertEqual(platform.calls.filter { $0 == "clearCredentials" }.count, 1)
    }

    func testTappingAPushRefreshesAndParksWhereItShouldLand() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        var opened = 0
        let push = build(api(relay), platform)
        push.onOpened = { opened += 1 }
        await push.start(accountKey: "alice")

        push.didOpen(PushOpen(arguments: ["alertId": tokenA, "href": "/acme/widgets/issues/42"]))
        XCTAssertEqual(opened, 1)
        let open = push.takePendingOpen()
        XCTAssertEqual(open?.alertId, tokenA)
        XCTAssertEqual(open?.href, "/acme/widgets/issues/42")
        XCTAssertEqual(open?.isChat, false)
        // Consumed once: a second read must not route the same tap again.
        XCTAssertNil(push.takePendingOpen())
    }

    func testATapOnABannerThatNamedNothingStillAsksToOpen() async {
        let push = build(api(PushRelay()), RecordingPlatform())
        push.didOpen(PushOpen(arguments: [:]))
        // An older build's bridge, or a banner the extension could not expand: the shell falls
        // back to the inbox rather than doing nothing.
        let open = push.pendingOpen
        XCTAssertNotNil(open)
        XCTAssertEqual(open?.href, "")
        XCTAssertEqual(open?.topic, "")
    }

    func testConversationTapsSayWhichKindTheyAre() async {
        let push = build(api(PushRelay()), RecordingPlatform())
        push.didOpen(PushOpen(arguments: ["topic": "direct-messages"]))
        XCTAssertEqual(push.takePendingOpen()?.isDirectMessage, true)
        push.didOpen(PushOpen(arguments: ["topic": "chat"]))
        XCTAssertEqual(push.takePendingOpen()?.isChat, true)
    }

    func testAnAgentPushNamesTheSessionToOpen() async {
        let done = PushOpen(arguments: ["category": "agent-finished", "sessionId": "12", "href": "/acme/widgets/agents/12"])
        XCTAssertEqual(done.agent, "12")
        XCTAssertEqual(PushOpen(arguments: ["category": "agent-failed", "sessionId": "9"]).agent, "9")
        XCTAssertEqual(PushOpen(arguments: ["category": "ping", "sessionId": "9"]).agent, "")
        XCTAssertEqual(PushOpen(arguments: ["sessionId": "9"]).agent, "")
    }

    func testOnlyASameSitePathIsEverRouted() async {
        let push = build(api(PushRelay()), RecordingPlatform())
        for href in ["https://evil.example/x", "//evil.example/x"] {
            push.didOpen(PushOpen(arguments: ["href": href]))
            XCTAssertEqual(push.takePendingOpen()?.href, "", href)
        }
    }

    func testATapThatColdLaunchedTheAppIsCollectedFromNative() async {
        let platform = RecordingPlatform()
        platform.parkedOpen = PushOpen(arguments: ["topic": "chat"])
        let push = build(api(PushRelay()), platform)
        await push.collectLaunchOpen()
        XCTAssertTrue(platform.calls.contains("takePendingOpen"))
        XCTAssertEqual(push.takePendingOpen()?.isChat, true)
    }

    func testNothingParkedNativelyLeavesNothingToRoute() async {
        let push = build(api(PushRelay()), RecordingPlatform())
        await push.collectLaunchOpen()
        XCTAssertNil(push.pendingOpen)
    }

    func testTheAppBadgeFollowsTheUnreadCountItIsGiven() async {
        let platform = RecordingPlatform()
        let push = build(api(PushRelay()), platform)
        await push.setBadge(3)
        // Deleting every ping has to lower the number again, and a repeat of the same count is
        // still sent: the extension may have redrawn the icon while the app was suspended.
        await push.setBadge(0)
        await push.setBadge(0)
        await push.setBadge(-2)

        let sent = platform.nativeCalls.filter { $0.method == "setBadge" }.map { $0.arguments["count"] ?? "" }
        XCTAssertEqual(sent, ["3", "0", "0", "0"])
    }

    func testSigningOutClearsTheAppBadge() async {
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(PushRelay()), platform)
        await push.start(accountKey: "alice")
        await push.setBadge(4)

        await push.stop()
        let last = platform.nativeCalls.last { $0.method == "setBadge" }
        XCTAssertEqual(last?.arguments["count"], "0")
    }

    func testAnAPNsRegistrationErrorIsSurfacedNotThrown() async {
        let push = build(api(PushRelay()), RecordingPlatform())
        await push.start(accountKey: "alice")

        push.didFailToRegister("no valid aps-environment entitlement")
        XCTAssertTrue(push.lastError.contains("aps-environment"), push.lastError)

        // Kotlin: a bridge that throws is recorded, not rethrown.
        let broken = RecordingPlatform()
        broken.failPermissionStatus = BridgeError(description: "bridge missing")
        let throwing = build(api(PushRelay()), broken)
        await throwing.start(accountKey: "alice")
        XCTAssertEqual(throwing.lastError, "bridge missing")
    }

    func testUnsupportedPlatformsStayInert() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        let push = PushNotificationService(api: api(relay), platform: platform, supported: false)
        await push.start(accountKey: "alice")
        await push.rebind(accountKey: "alice")
        await push.setBadge(2)
        await push.collectLaunchOpen()

        XCTAssertFalse(push.supported)
        XCTAssertTrue(platform.calls.isEmpty)
        XCTAssertTrue(registrations(relay).isEmpty)

        // Kotlin: no push platform at all.
        let none = PushNotificationService(api: api(relay), platform: nil)
        XCTAssertFalse(none.supported)
        await none.start(accountKey: "alice")
        await none.enable()
        XCTAssertTrue(registrations(relay).isEmpty)
        XCTAssertEqual(none.permission, "unknown")
    }

    func testRegistrationRecordsWhereTheChainStandsForTheSettingsScreen() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        let notifications = SvcBox(0)
        push.addListener { notifications.mutate { $0 += 1 } }
        await push.start(accountKey: "alice")

        XCTAssertEqual(push.permission, "authorized")
        XCTAssertEqual(push.environment, "development")
        XCTAssertTrue(push.registered)
        // The relay said it holds APNs credentials, so a banner can actually follow;
        // "registered" alone would not have told the user that.
        XCTAssertEqual(push.relayConfigured, true)
        XCTAssertEqual(push.lastError, "")
        XCTAssertGreaterThan(notifications.value, 0)
    }

    func testARelayWithoutAPNsCredentialsIsReportedNotHidden() async {
        let relay = PushRelay()
        relay.relayConfigured.value = false
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        XCTAssertTrue(push.registered)
        XCTAssertEqual(push.relayConfigured, false)
    }

    func testARelayThatPredatesTheFlagLeavesTheRelayStateUnknown() async {
        let relay = PushRelay()
        relay.relayConfigured.value = nil
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")

        XCTAssertTrue(push.registered)
        XCTAssertNil(push.relayConfigured)
    }

    func testADeclinedPromptAndADeniedSettingAreBothVisible() async {
        let relay = PushRelay()
        let declinedPlatform = RecordingPlatform()
        declinedPlatform.granted = false
        let declined = build(api(relay), declinedPlatform)
        await declined.start(accountKey: "alice")
        XCTAssertEqual(declined.permission, "denied")

        let deniedPlatform = RecordingPlatform()
        deniedPlatform.permission = "denied"
        let denied = build(api(relay), deniedPlatform)
        await denied.start(accountKey: "alice")
        XCTAssertEqual(denied.permission, "denied")
        XCTAssertEqual(denied.token, "")
    }

    func testATestPushGoesThroughTheRelayAndUpdatesTheRelayState() async {
        let relay = PushRelay()
        relay.relayConfigured.value = nil
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")
        XCTAssertNil(push.relayConfigured)

        let result = await push.sendTest()

        XCTAssertEqual(relay.testPushes.value, 1)
        XCTAssertTrue(result.summary.contains("Sent to 1 of 1 device"), result.summary)
        XCTAssertTrue(result.summary.contains("APNs sandbox"), result.summary)
        // The test answer carries the relay's own configured flag too.
        XCTAssertEqual(push.relayConfigured, true)

        // Kotlin: works without a push platform too; it is a relay round trip.
        _ = await PushNotificationService(api: api(relay), platform: nil).sendTest()
        XCTAssertEqual(relay.testPushes.value, 2)
    }

    func testSigningOutForgetsTheRelayStateAlongWithTheRow() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")
        XCTAssertTrue(push.registered)

        await push.stop()
        XCTAssertFalse(push.registered)
        XCTAssertNil(push.relayConfigured)
    }

    // MARK: PushBinderTest.kt

    func testARefusedRegistrationExplainsItself() async {
        let relay = PushRelay()
        relay.registerStatus.value = 403
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay), platform)
        await push.start(accountKey: "alice")
        XCTAssertFalse(push.registered)
        XCTAssertEqual(push.lastError, "The relay refused the registration (HTTP 403).")

        relay.registerStatus.value = 0
        await push.rebind(accountKey: "bob")
        XCTAssertEqual(push.lastError, "The relay could not be reached to register this phone.")
    }

    func testRegistrationPostsTheTokenWithTheSession() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let push = build(api(relay, session: { "session-token" }), platform)
        await push.start(accountKey: "alice")
        let request = relay.http.requests.first { $0.url.path == "/api/accounts/register-device" }
        XCTAssertEqual(request?.method, "POST")
        XCTAssertEqual(request?.url.absoluteString, "https://app.forkmesh.com/api/accounts/register-device")
        XCTAssertEqual(request?.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(request?.bodyText, "{\"deviceToken\":\"\(tokenA)\",\"environment\":\"development\"}")

        await push.stop()
        let removal = relay.http.lastRequest
        XCTAssertEqual(removal?.method, "DELETE")
        XCTAssertEqual(removal?.bodyText, "{\"deviceToken\":\"\(tokenA)\"}")
    }

    func testATokenIssuedBeforeStartWaitsForIt() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        let push = build(api(relay), platform)
        push.accountKeyProvider = { "alice" }
        // APNs answered before the shell started the service: nothing is bound yet.
        await push.didReceiveToken(tokenA, environment: "development")
        XCTAssertTrue(registrations(relay).isEmpty)

        // start() finds the token pending (getToken "") and registers it once it rebinds.
        await push.start(accountKey: "alice")
        XCTAssertTrue(registrations(relay).isEmpty)
        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay), [["token": tokenA, "environment": "development"]])
    }

    // MARK: Per-project push (PARITY row #52) and the per-token apns-topic (5.2)

    func testRegistrationNamesTheAppThatMintedTheToken() async throws {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        platform.bundleIdAnswer = "com.forkmesh.forkmesh"
        let push = build(api(relay, session: { "session-token" }), platform)
        await push.start(accountKey: "alice")
        let request = try XCTUnwrap(relay.http.requests.first { $0.url.path == "/api/accounts/register-device" })
        let body = JSON.parseObject(request.bodyText)
        XCTAssertEqual(body.str("bundleId"), "com.forkmesh.forkmesh")
        XCTAssertEqual(body.str("deviceToken"), tokenA)
    }

    func testEachProjectHoldsItsOwnRegistrationAndTheOneLeftKeepsIt() async {
        let relay = PushRelay()
        let platform = RecordingPlatform()
        platform.cachedToken = tokenA
        let first = api(relay, session: { "session-token" })
        let push = PushNotificationService(
            api: first, platform: platform, supported: true, projectId: "app.forkmesh.com")
        push.accountKeyProvider = { "alice" }
        await push.start(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 1)

        // The switch steps the machinery down: not a sign-out, so nothing is unregistered and
        // the extension keeps the project's session.
        push.beginProjectSwitch()
        await push.stop()
        XCTAssertTrue(relay.unregistered.value.isEmpty)
        XCTAssertFalse(platform.calls.contains("clearCredentials"))

        // Same account name on another instance: a second registration, at that origin.
        let next = svcApi(relay.http, token: { "next-session" }, serverUrl: "wss://app.next.forkmesh.com/ws")
        push.finishProjectSwitch(api: next, projectId: "app.next.forkmesh.com")
        XCTAssertEqual(push.projectId, "app.next.forkmesh.com")
        await push.rebind(accountKey: "alice")
        XCTAssertEqual(registrations(relay).count, 2)
        XCTAssertEqual(
            relay.http.requests.last { $0.url.path == "/api/accounts/register-device" }?.url.host,
            "app.next.forkmesh.com")
        let shared = platform.nativeCalls.last { $0.method == "setCredentials" }
        XCTAssertEqual(shared?.arguments["origin"], "https://app.next.forkmesh.com")

        // The flag is one-shot: a real sign-out afterwards unregisters and clears only this
        // project's shared session.
        await push.stop()
        XCTAssertEqual(relay.unregistered.value, [tokenA])
        XCTAssertEqual(
            platform.nativeCalls.last { $0.method == "clearCredentials" }?.arguments,
            ["origin": "https://app.next.forkmesh.com"])
    }

    func testATapNamesTheProjectThatSentIt() {
        let open = PushOpen(arguments: [
            "topic": "code-push", "alertId": "row-1", "href": "/alice/atlas/commits/main",
            "origin": "https://app.acme.dev",
        ])
        XCTAssertTrue(open.isCodePush)
        XCTAssertEqual(open.projectId, "app.acme.dev")
        XCTAssertTrue(open.isFor("app.acme.dev"))
        XCTAssertFalse(open.isFor("app.forkmesh.com"))
        XCTAssertEqual(open.deepLink?.initialTab, .commits)
        XCTAssertEqual(open.deepLink?.branch, "main")
        // A relay that sends a bare host, and one that names nothing (opens where the app is).
        XCTAssertEqual(PushOpen(arguments: ["host": "app.forkmesh.com"]).projectId, "app.forkmesh.com")
        XCTAssertEqual(PushOpen(arguments: ["origin": "https://app.next.forkmesh.com"]).projectId, "app.forkmesh.com")
        XCTAssertTrue(PushOpen(arguments: [:]).isFor("anything"))
    }

    func testATapFromAnotherKnownProjectSwitchesThereBeforeItOpens() async {
        let relay = PushRelay()
        let push = build(api(relay), RecordingPlatform())
        var active = "app.forkmesh.com"
        var followed: [String] = []
        push.activeProjectProvider = { active }
        push.knowsProject = { $0 == "app.forkmesh.com" || $0 == "app.acme.dev" }
        push.onForeignProjectOpen = { open in
            followed.append(open.projectId)
            // The shell of the project being left must not route it.
            XCTAssertNil(push.pendingOpen)
            active = open.projectId
        }

        let foreign = PushOpen(href: "/alice/atlas/commits/main", origin: "https://app.acme.dev")
        push.didOpen(foreign)
        XCTAssertNil(push.pendingOpen)
        XCTAssertEqual(push.awaitingProjectOpen, foreign)
        let routed = await svcEventually { push.pendingOpen == foreign }
        XCTAssertTrue(routed)
        XCTAssertEqual(followed, ["app.acme.dev"])
        XCTAssertNil(push.awaitingProjectOpen)

        // Its own project, an unknown one, or none named: opened where the app is.
        _ = push.takePendingOpen()
        push.didOpen(PushOpen(href: "/a/b", origin: "https://app.acme.dev"))
        XCTAssertNotNil(push.takePendingOpen())
        push.didOpen(PushOpen(href: "/a/b", origin: "https://elsewhere.example"))
        XCTAssertNotNil(push.takePendingOpen())
        push.didOpen(PushOpen(href: "/a/b"))
        XCTAssertNotNil(push.takePendingOpen())
        XCTAssertEqual(followed.count, 1)
    }
}
