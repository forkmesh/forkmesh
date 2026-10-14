// The relay probe, the relay switch controller and the cross-relay catalog, plus the view-model
// logic of the relay settings screen.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

private func versionResponse(status: Int = 200, headers: HTTPHeaders? = nil, body: JSONValue? = nil) -> HTTPResponse {
    let json: JSONValue =
        body ?? ["ok": true, "rev": "abc1234", "worker": "app", "version": "1.2.0", "deployFingerprint": "fp", "now": 1]
    return HTTPResponse(status: status, headers: headers ?? ["x-forkmesh-worker": "app"], body: JSON.encodeData(json))
}

private func probe(timeout: TimeInterval = 8, _ handler: @escaping FakeHTTPClient.Handler) -> RelayProbe {
    RelayProbe(http: FakeHTTPClient(handler), timeout: timeout)
}

final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0
    private var urls: [URL] = []
    func hit(_ url: URL) {
        lock.lock()
        value += 1
        urls.append(url)
        lock.unlock()
    }
    var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return value
    }
    var last: URL? {
        lock.lock()
        defer { lock.unlock() }
        return urls.last
    }
}

final class RelayProbeTests: XCTestCase {
    func testARealRelayIsAdmittedAndCarriesItsBuildBack() async {
        let seen = Counter()
        let result = await probe { request in
            seen.hit(request.url)
            return versionResponse()
        }.check("staging.forkmesh.com")
        XCTAssertEqual(seen.last?.absoluteString, "https://staging.forkmesh.com/api/version")
        XCTAssertTrue(result.ok)
        XCTAssertEqual(result.rev, "abc1234")
        XCTAssertEqual(result.version, "1.2.0")
        XCTAssertTrue(result.message.contains("1.2.0"))
        XCTAssertEqual(result.profile?.id, "staging.forkmesh.com")
        XCTAssertGreaterThan(result.profile?.verifiedAt ?? 0, 0)
        XCTAssertEqual(result.profile?.verifiedRev, "abc1234")
    }

    func testALoopbackRelayIsProbedOverPlainHttp() async {
        let seen = Counter()
        _ = await probe { request in
            seen.hit(request.url)
            return versionResponse()
        }.check("localhost:8787")
        XCTAssertEqual(seen.last?.absoluteString, "http://localhost:8787/api/version")
    }

    func testAHostThatIsNotARelayIsRefused() async {
        let result = await probe { _ in HTTPResponse(status: 200, text: "<!doctype html><html>Hello</html>") }
            .check("example.com")
        XCTAssertEqual(result.status, .notForkMesh)
        XCTAssertNil(result.profile)
        XCTAssertTrue(result.message.contains("not a ForkMesh relay"))
    }

    func testAGenericJSONAPIThatMerelyAnswersOkIsRefused() async {
        let result = await probe { _ in HTTPResponse(status: 200, json: ["ok": true]) }.check("some-api.example.com")
        XCTAssertEqual(result.status, .notForkMesh)
    }

    func testTheRevWorkerPairAdmitsARelayBehindAHeaderStrippingProxy() async {
        let result = await probe { _ in versionResponse(headers: ["content-type": "application/json"]) }.check("relay.test")
        XCTAssertTrue(result.ok)
    }

    func testARelayErroringRightNowIsToldApartFromAWrongHost() async {
        let down = await probe { _ in versionResponse(status: 503, body: ["error": "maintenance"]) }.check("relay.test")
        XCTAssertEqual(down.status, .serverError)
        XCTAssertEqual(down.message, "That relay answered 503. Try again in a moment.")
        let wrong = await probe { _ in HTTPResponse(status: 404, text: "nope") }.check("relay.test")
        XCTAssertEqual(wrong.status, .notForkMesh)
    }

    func testAnUnreachableHostATimeoutAndABadCertificateEachDiffer() async {
        let unreachable = await probe { _ in throw HTTPError.transport("no route") }.check("relay.test")
        XCTAssertEqual(unreachable.status, .unreachable)
        let slow = await probe(timeout: 0.03) { _ in
            try await Task.sleep(nanoseconds: 5_000_000_000)
            return versionResponse()
        }.check("relay.test")
        XCTAssertEqual(slow.status, .timeout)
        let insecure = await probe { _ in throw HTTPError.transport("An SSL error has occurred: bad certificate") }
            .check("relay.test")
        XCTAssertEqual(insecure.status, .insecure)
    }

    func testNothingHostShapedIsRefusedBeforeAnyRequestIsMade() async {
        let calls = Counter()
        let result = await probe { request in
            calls.hit(request.url)
            return versionResponse()
        }.check("not a host at all")
        XCTAssertEqual(result.status, .invalidUrl)
        XCTAssertEqual(calls.count, 0, "a malformed host must not be dialled")
    }

    func testARelayAlreadyInTheListIsRefusedWithoutARequest() async throws {
        let calls = Counter()
        let result = await probe { request in
            calls.hit(request.url)
            return versionResponse()
        }.check("https://relay.test", existing: [try XCTUnwrap(RelayProfile.tryParse("relay.test"))])
        XCTAssertEqual(result.status, .duplicate)
        XCTAssertEqual(calls.count, 0)
    }

    func testARelayMissingCoreAPIsIsRefusedAndNamesThem() async {
        let result = await probe { _ in
            versionResponse(body: ["ok": true, "rev": "r", "worker": "app", "features": ["chat"]])
        }.check("relay.test")
        XCTAssertEqual(result.status, .missingCore)
        XCTAssertTrue(result.message.contains("which the app cannot work without"), result.message)
    }

    func testEveryFailureExplainsItselfInOneSentence() {
        for status in [
            RelayProbeStatus.ok, .invalidUrl, .duplicate, .unreachable, .timeout, .insecure, .notForkMesh, .serverError,
            .missingCore,
        ] {
            let message = RelayProbeResult(status: status).message
            XCTAssertFalse(message.isEmpty, status.rawValue)
            XCTAssertTrue(message.dartTrimmed.hasSuffix("."), status.rawValue)
        }
        XCTAssertEqual(RelayProbeResult.list(["a", "b", "c"]), "a, b or c")
    }
}

// MARK: - Relay switch

private func accountPayload(_ name: String) -> JSONObject {
    [
        "nodeName": .string(name), "name": .string(name), "email": .string("\(name)@example.com"), "status": "active",
        "sessionToken": .string("token-\(name)"), "sessionKind": "mobile_node", "deviceKind": "mobile_node",
        "deviceId": .string("dev-\(name)"), "capabilities": ["browse", "publish_repo", "owner_sign"],
    ]
}

final class NextAnswer: @unchecked Sendable {
    var body: JSONObject
    init(_ body: JSONObject) { self.body = body }
}

@MainActor
final class TracingApi: RelaySwitchCache {
    var trace: [String]
    init(_ trace: [String] = []) { self.trace = trace }
    func resetForRelaySwitch() { trace.append("api.reset") }
}

/// A relay that never opens a socket, so the ordering can be observed without a network.
@MainActor
final class FakeRelaySocket: RelaySwitchSocket {
    let api: TracingApi
    let connects: Bool
    var connectDelay: UInt64 = 0
    var failOnDisconnect = ""
    private(set) var state: RelayConnectionState = .offline

    init(_ api: TracingApi, connects: Bool) {
        self.api = api
        self.connects = connects
    }

    func connect() async throws {
        api.trace.append("connect")
        if connectDelay > 0 { try? await Task.sleep(nanoseconds: connectDelay) }
        state = connects ? .connected : .offline
    }

    func disconnect() async throws {
        api.trace.append("disconnect")
        if !failOnDisconnect.isEmpty { throw RelayError(failOnDisconnect) }
        state = .offline
    }

    func resetForRelaySwitch() { api.trace.append("relay.reset") }
}

@MainActor
struct SwitchHarness {
    let device: ChatDevice
    let auth: AuthService
    let api: TracingApi
    let relay: FakeRelaySocket
    let controller: RelaySwitchController
    let next: NextAnswer

    static func make(connects: Bool = true, loginAs: String? = nil) async throws -> SwitchHarness {
        let device = ChatDevice()
        device.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com")))
        let next = NextAnswer(accountPayload(loginAs ?? "alice"))
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, json: .object(next.body)) }
        let auth = AuthService.create(settings: device.settings, identity: device.identity, http: http)
        if let loginAs { _ = try await auth.login(identifier: loginAs, password: "pw") }
        let api = TracingApi()
        let relay = FakeRelaySocket(api, connects: connects)
        let controller = RelaySwitchController(
            settings: device.settings, auth: auth, api: api, relay: relay,
            onSuspend: { api.trace.append("suspend") }, onResume: { api.trace.append("resume") },
            joinTimeout: 0.2, minimumDuration: 0)
        return SwitchHarness(device: device, auth: auth, api: api, relay: relay, controller: controller, next: next)
    }
}

@MainActor
final class RelaySwitchControllerTests: XCTestCase {
    func testTheSequenceRunsInTheOneOrderThatCannotLeak() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let result = await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertTrue(result.ok)
        XCTAssertEqual(h.api.trace, ["suspend", "disconnect", "api.reset", "relay.reset", "resume", "connect"])
        XCTAssertEqual(h.device.settings.relays.activeId, "staging.forkmesh.com")
        XCTAssertTrue(h.device.settings.serverUrl.contains("staging.forkmesh.com"))
        XCTAssertEqual(h.controller.phase, .done)
    }

    func testArrivingWithNoSessionThereAsksForASignIn() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let result = await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertTrue(result.needsSignIn)
        XCTAssertFalse(h.auth.isAuthenticated)
        XCTAssertEqual(h.auth.sessionForRelay(AppEnv.prodHost)?.nodeName, "alice")
    }

    func testArrivingWhereASessionIsAlreadyHeldDoesNotAsk() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        await h.controller.switchTo("staging.forkmesh.com")
        h.next.body = accountPayload("bob")
        _ = try await h.auth.login(identifier: "bob", password: "pw")
        let back = await h.controller.switchTo(AppEnv.prodHost)
        XCTAssertFalse(back.needsSignIn)
        XCTAssertEqual(h.auth.session?.nodeName, "alice")
        let forward = await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertFalse(forward.needsSignIn)
    }

    func testARelayThatWillNotComeUpIsReportedNotRolledBack() async throws {
        let h = try await SwitchHarness.make(connects: false)
        let result = await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertTrue(result.ok, "the user still chose this relay")
        XCTAssertTrue(result.offline)
        XCTAssertTrue(result.error.contains("not answering"))
        XCTAssertEqual(h.device.settings.relays.activeId, "staging.forkmesh.com")
    }

    func testSwitchingToTheRelayAlreadyActiveIsAHarmlessNoOp() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let result = await h.controller.switchTo(AppEnv.prodHost)
        XCTAssertTrue(result.ok)
        XCTAssertTrue(h.api.trace.isEmpty)
    }

    func testAnUnknownRelayIsRefusedWithAReadableReason() async throws {
        let h = try await SwitchHarness.make()
        let result = await h.controller.switchTo("gone.example.com")
        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.error.contains("no longer in your list"))
        XCTAssertTrue(h.api.trace.isEmpty)
    }

    func testASecondSwitchWhileTheOldProjectIsComingDownIsRefusedNotInterleaved() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let api = h.api
        h.controller.onSuspend = {
            api.trace.append("suspend")
            try await Task.sleep(nanoseconds: 60_000_000)
        }
        let controller = h.controller
        let first = Task { await controller.switchTo("staging.forkmesh.com") }
        try await Task.sleep(nanoseconds: 10_000_000)
        XCTAssertTrue(h.controller.covering)
        let second = await h.controller.switchTo(AppEnv.prodHost)
        XCTAssertFalse(second.ok)
        XCTAssertTrue(second.error.contains("Already switching"))
        let firstResult = await first.value
        XCTAssertTrue(firstResult.ok)
        XCTAssertEqual(h.device.settings.relays.activeId, "staging.forkmesh.com")
        XCTAssertEqual(h.api.trace.filter { $0 == "api.reset" }.count, 1)
    }

    func testTheCoverLiftsBeforeTheNewSocketIsUp() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        h.relay.connectDelay = 60_000_000
        let controller = h.controller
        let first = Task { await controller.switchTo("staging.forkmesh.com") }
        try await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(h.controller.phase, .joining)
        XCTAssertFalse(h.controller.covering)
        XCTAssertTrue(h.controller.canSwitch)
        XCTAssertEqual(h.device.settings.relays.activeId, "staging.forkmesh.com")
        _ = await first.value
        XCTAssertEqual(h.controller.phase, .done)
    }

    func testASwitchWhileTheSocketIsJoiningTakesOverInOrder() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        h.relay.connectDelay = 60_000_000
        let controller = h.controller
        let first = Task { await controller.switchTo("staging.forkmesh.com") }
        try await Task.sleep(nanoseconds: 20_000_000)
        let second = await h.controller.switchTo(AppEnv.prodHost)
        XCTAssertTrue(second.ok)
        let firstResult = await first.value
        XCTAssertFalse(firstResult.ok)
        XCTAssertEqual(firstResult.error, "")
        XCTAssertEqual(h.device.settings.relays.activeId, AppEnv.prodHost)
        XCTAssertEqual(h.controller.phase, .done)
        XCTAssertEqual(
            h.api.trace,
            [
                "suspend", "disconnect", "api.reset", "relay.reset", "resume", "connect",
                "suspend", "disconnect", "api.reset", "relay.reset", "resume", "connect",
            ])
    }

    func testAFailureMidSwitchSurfacesWhichRelayAndWhy() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        h.relay.failOnDisconnect = "the socket refused to close"
        let result = await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertFalse(result.ok)
        XCTAssertEqual(h.controller.phase, .failed)
        XCTAssertTrue(result.error.contains("staging.forkmesh.com"), result.error)
        XCTAssertTrue(result.error.contains("socket refused to close"), result.error)
        XCTAssertTrue(result.error.hasSuffix("."))
        h.controller.clearError()
        XCTAssertEqual(h.controller.phase, .idle)
        XCTAssertEqual(h.controller.error, "")
    }

    func testEveryPhaseHasSomethingToSayWhileItRuns() async throws {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let lines = LinesBox()
        let controller = h.controller
        controller.addListener {
            MainActor.assumeIsolated {
                if controller.busy { lines.values.append(controller.statusLine) }
            }
        }
        await h.controller.switchTo("staging.forkmesh.com")
        XCTAssertEqual(lines.values.count, 4)
        XCTAssertTrue(lines.values.allSatisfy { !$0.dartTrimmed.isEmpty })
        XCTAssertTrue(lines.values.first?.contains(RelayDirectory.productionLabel) == true, "Leaving ForkMesh…")
        XCTAssertTrue(lines.values.last?.contains("staging.forkmesh.com") == true)
        XCTAssertEqual(h.controller.overlayTitle, "Switching to staging.forkmesh.com")
    }

    func testRemovingARelaySwitchesAwayFirstThenForgetsItsSession() async throws {
        let h = try await SwitchHarness.make()
        await h.controller.switchTo("staging.forkmesh.com")
        h.next.body = accountPayload("bob")
        _ = try await h.auth.login(identifier: "bob", password: "pw")
        let result = await h.controller.removeRelay("staging.forkmesh.com")
        XCTAssertTrue(result.ok)
        XCTAssertEqual(h.device.settings.relays.activeId, AppEnv.prodHost)
        XCTAssertNil(h.device.settings.relays.byId("staging.forkmesh.com"))
        XCTAssertNil(h.device.secure.read(AuthService.sessionKeyFor("staging.forkmesh.com")))
    }

    func testTheBuiltInRelayIsRefusedWithAnExplanation() async throws {
        let h = try await SwitchHarness.make()
        let result = await h.controller.removeRelay(AppEnv.prodHost)
        XCTAssertFalse(result.ok)
        XCTAssertTrue(result.error.contains("cannot be removed"))
        XCTAssertNotNil(h.device.settings.relays.byId(AppEnv.prodHost))
    }
}

final class LinesBox: @unchecked Sendable {
    var values: [String] = []
}

// MARK: - Cross-relay catalog

private func repos(_ names: [String]) -> JSONValue {
    .array(names.map { .object(["owner": "someone", "name": .string($0), "defaultBranch": "main"]) })
}

@MainActor
final class CrossRelayCatalogTests: XCTestCase {
    /// Signed in on production as alice and on staging as bob.
    private func bothRelays() async throws -> (ChatDevice, AuthService) {
        let device = ChatDevice()
        device.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com")))
        let next = NextAnswer(accountPayload("alice"))
        let auth = AuthService.create(
            settings: device.settings, identity: device.identity,
            http: FakeHTTPClient { _ in HTTPResponse(status: 200, json: .object(next.body)) })
        _ = try await auth.login(identifier: "alice", password: "pw")
        next.body = accountPayload("bob")
        device.settings.relays.setActive("staging.forkmesh.com")
        auth.adoptActiveRelay()
        _ = try await auth.login(identifier: "bob", password: "pw")
        device.settings.relays.setActive(AppEnv.prodHost)
        auth.adoptActiveRelay()
        return (device, auth)
    }

    func testEachRelayIsReadAtItsOwnOriginWithItsOwnToken() async throws {
        let (device, auth) = try await bothRelays()
        let http = FakeHTTPClient { request in
            HTTPResponse(status: 200, json: repos(request.url.host == AppEnv.prodHost ? ["one"] : ["two", "three"]))
        }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http)
        await catalog.refresh()
        var seen: [String: String] = [:]
        for request in http.requests {
            seen["\(request.url.scheme ?? "")://\(request.url.host ?? "")"] = request.headers["Authorization"] ?? ""
        }
        XCTAssertEqual(seen, [
            "https://\(AppEnv.prodHost)": "Bearer token-alice", "https://staging.forkmesh.com": "Bearer token-bob",
        ])
        XCTAssertEqual(catalog.totalRepositories, 3)
        XCTAssertEqual(catalog.entries.first { $0.active }?.repositories.first?.name, "one")
        XCTAssertFalse(catalog.loading)
    }

    func testOneRelayFailingLeavesTheOthersIntact() async throws {
        let (device, auth) = try await bothRelays()
        let http = FakeHTTPClient { request in
            if request.url.host == "staging.forkmesh.com" { throw HTTPError.transport("down") }
            return HTTPResponse(status: 200, json: repos(["one"]))
        }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http)
        await catalog.refresh()
        let good = try XCTUnwrap(catalog.entries.first { $0.relay.id == AppEnv.prodHost })
        let bad = try XCTUnwrap(catalog.entries.first { $0.relay.id == "staging.forkmesh.com" })
        XCTAssertEqual(good.repositories.count, 1)
        XCTAssertEqual(good.error, "")
        XCTAssertTrue(bad.repositories.isEmpty)
        XCTAssertFalse(bad.error.isEmpty)
        XCTAssertFalse(bad.loading)
    }

    func testASlowRelayDoesNotHoldUpAFastOne() async throws {
        let (device, auth) = try await bothRelays()
        let http = FakeHTTPClient { request in
            if request.url.host == "staging.forkmesh.com" { try await Task.sleep(nanoseconds: 5_000_000_000) }
            return HTTPResponse(status: 200, json: repos(["one"]))
        }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http, timeout: 0.06)
        await catalog.refresh()
        XCTAssertEqual(catalog.entries.first { $0.relay.id == AppEnv.prodHost }?.repositories.count, 1)
        XCTAssertTrue(
            catalog.entries.first { $0.relay.id == "staging.forkmesh.com" }?.error.contains("did not answer in time") == true)
    }

    func testASignedOutRelayIsListedButNeverDialled() async throws {
        let device = ChatDevice()
        device.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com")))
        let auth = AuthService.create(
            settings: device.settings, identity: device.identity,
            http: FakeHTTPClient { _ in HTTPResponse(status: 200, json: .object(accountPayload("alice"))) })
        _ = try await auth.login(identifier: "alice", password: "pw")
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, json: repos(["one"])) }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http)
        await catalog.refresh()
        XCTAssertEqual(http.requests.map { $0.url.host ?? "" }, [AppEnv.prodHost])
        let staging = try XCTUnwrap(catalog.entries.first { $0.relay.id == "staging.forkmesh.com" })
        XCTAssertFalse(staging.signedIn)
        XCTAssertFalse(staging.loading)
        XCTAssertEqual(catalog.entries.count, 2, "listed, not hidden")
    }

    func testA401FromABackgroundRelayDoesNotSignAnyoneOut() async throws {
        let (device, auth) = try await bothRelays()
        let http = FakeHTTPClient { request in
            if request.url.host == "staging.forkmesh.com" {
                return HTTPResponse(status: 401, json: ["error": "invalid_session"])
            }
            return HTTPResponse(status: 200, json: repos(["one"]))
        }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http)
        await catalog.refresh()
        XCTAssertEqual(auth.session?.nodeName, "alice")
        XCTAssertEqual(auth.sessionForRelay("staging.forkmesh.com")?.nodeName, "bob")
        XCTAssertTrue(catalog.entries.first { $0.relay.id == "staging.forkmesh.com" }?.error.contains("expired") == true)
    }

    func testASecondRefreshAbandonsTheFirstRatherThanInterleaving() async throws {
        let (device, auth) = try await bothRelays()
        let calls = Counter()
        let http = FakeHTTPClient { request in
            calls.hit(request.url)
            let mine = calls.count
            try await Task.sleep(nanoseconds: 20_000_000)
            return HTTPResponse(status: 200, json: repos(["repo-\(mine)"]))
        }
        let catalog = CrossRelayCatalog(settings: device.settings, auth: auth, http: http)
        async let first: Void = catalog.refresh()
        async let second: Void = catalog.refresh()
        _ = await (first, second)
        XCTAssertFalse(catalog.loading)
        XCTAssertEqual(catalog.entries.count, 2)
        XCTAssertEqual(catalog.totalRepositories, 2)
    }
}

// MARK: - Relay settings (logic half)

@MainActor
final class RelaySettingsModelTests: XCTestCase {
    private func model(_ probeHandler: @escaping FakeHTTPClient.Handler) async throws -> (RelaySettingsModel, SwitchHarness) {
        let h = try await SwitchHarness.make(loginAs: "alice")
        let relay = RelayService(
            settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure,
            connector: FakeWebSocketConnector())
        let model = RelaySettingsModel(
            settings: h.device.settings, auth: h.auth, relay: relay, switcher: h.controller,
            probe: { RelayProbe(http: FakeHTTPClient(probeHandler)) })
        return (model, h)
    }

    func testEveryRelayIsListedWithWhoYouAreOnIt() async throws {
        let (model, _) = try await model { _ in versionResponse() }
        let rows = model.rows
        XCTAssertEqual(rows.map(\.relay.id), [AppEnv.prodHost, "staging.forkmesh.com"])
        XCTAssertEqual(rows[0].who, "Signed in as alice")
        XCTAssertTrue(rows[0].active)
        XCTAssertFalse(rows[0].removable, "the built-in relay stays")
        XCTAssertEqual(rows[1].who, "Not signed in")
        XCTAssertTrue(model.hasMultiple, "the cross-relay browser appears once there is more than one")
    }

    func testEachSignedInProjectShowsItsAccountAvatarAndStatus() async throws {
        let device = ChatDevice()
        device.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("staging.forkmesh.com")))
        device.settings.relays.add(try XCTUnwrap(RelayProfile.tryParse("signed-out.example.com")))
        let next = NextAnswer(accountPayload("alice"))
        let auth = AuthService.create(
            settings: device.settings, identity: device.identity,
            http: FakeHTTPClient { _ in HTTPResponse(status: 200, json: .object(next.body)) })
        _ = try await auth.login(identifier: "alice", password: "pw")
        var bob = accountPayload("bob")
        bob["avatarPng"] = "Ym9i"
        next.body = bob
        device.settings.relays.setActive("staging.forkmesh.com")
        auth.adoptActiveRelay()
        _ = try await auth.login(identifier: "bob", password: "pw")
        device.settings.relays.setActive(AppEnv.prodHost)
        auth.adoptActiveRelay()
        let relay = RelayService(
            settings: device.settings, identity: device.identity, secureStore: device.secure,
            connector: FakeWebSocketConnector())
        relay.setOwnStatus(UserStatus(emoji: "📅", text: "In a meeting"))
        let http = FakeHTTPClient { _ in
            HTTPResponse(status: 200, json: ["userStatus": ["away": true, "emoji": "🌴", "text": "Vacationing"]])
        }
        let model = RelaySettingsModel(
            settings: device.settings, auth: auth, relay: relay, switcher: nil,
            probe: { RelayProbe(http: http) }, http: http)

        await model.refreshStatuses()

        XCTAssertEqual(http.requests.map { $0.url.absoluteString }, ["https://staging.forkmesh.com/api/accounts/user-status"])
        XCTAssertEqual(http.requests.first?.method, "POST")
        XCTAssertEqual(http.requests.first?.headers["Authorization"], "Bearer token-bob")
        let rows = model.rows
        XCTAssertEqual(rows[0].account, "alice")
        XCTAssertEqual(rows[0].userStatus, UserStatus(emoji: "📅", text: "In a meeting"))
        XCTAssertEqual(rows[1].account, "bob")
        XCTAssertEqual(rows[1].avatarPng, "Ym9i")
        XCTAssertEqual(rows[1].userStatus, UserStatus(away: true, emoji: "🌴", text: "Vacationing"))
        XCTAssertEqual(rows[2].account, "")
        XCTAssertEqual(rows[2].userStatus, .empty)
    }

    func testTappingAnotherRelaySwitchesTheAppToIt() async throws {
        let (model, h) = try await model { _ in versionResponse() }
        await model.switchTo(try XCTUnwrap(h.device.settings.relays.byId("staging.forkmesh.com")))
        XCTAssertEqual(h.device.settings.relays.activeId, "staging.forkmesh.com")
        XCTAssertEqual(model.notice, RelayNotice(message: "Now on staging.forkmesh.com.", error: false))
    }

    func testTestingARelayRecordsItsVerification() async throws {
        let (model, h) = try await model { _ in versionResponse() }
        await model.test(try XCTUnwrap(h.device.settings.relays.byId("staging.forkmesh.com")))
        XCTAssertEqual(h.device.settings.relays.byId("staging.forkmesh.com")?.verifiedRev, "abc1234")
        XCTAssertEqual(model.notice?.error, false)
        XCTAssertEqual(model.testing, "")
    }

    func testAHostThatIsNotARelayIsRefusedAndNotSaved() async throws {
        let h = try await SwitchHarness.make()
        let add = AddRelayModel(
            relays: h.device.settings.relays,
            probe: RelayProbe(http: FakeHTTPClient { _ in HTTPResponse(status: 200, text: "<html></html>") }))
        add.host = "example.com"
        await add.check()
        XCTAssertEqual(add.result?.status, .notForkMesh)
        XCTAssertFalse(add.canAdd)
        XCTAssertNil(add.add())
        XCTAssertNil(h.device.settings.relays.byId("example.com"))
    }

    func testARealRelayIsVerifiedAndSavedWithoutSwitchingToIt() async throws {
        let h = try await SwitchHarness.make()
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: FakeHTTPClient { _ in versionResponse() }))
        add.host = "relay.example.com"
        add.label = "Staging two"
        await add.check()
        XCTAssertTrue(add.canAdd)
        XCTAssertEqual(add.checkLabel, "Check again")
        let saved = try XCTUnwrap(add.add())
        XCTAssertEqual(saved.label, "Staging two")
        XCTAssertNotNil(h.device.settings.relays.byId("relay.example.com"))
        XCTAssertEqual(h.device.settings.relays.activeId, AppEnv.prodHost)
        // Editing the host drops the verdict about the old one.
        add.host = "other.example.com"
        XCTAssertNil(add.result)
    }

    func testADuplicateHostIsRefusedBeforeAnyRequest() async throws {
        let h = try await SwitchHarness.make()
        let http = FakeHTTPClient { _ in versionResponse() }
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: http))
        add.host = "staging.forkmesh.com"
        await add.check()
        XCTAssertEqual(add.result?.status, .duplicate)
        XCTAssertTrue(http.requests.isEmpty)
    }

    func testANextHostIsRecognisedAsForkMeshWithoutAnyRequest() async throws {
        let h = try await SwitchHarness.make()
        let http = FakeHTTPClient { _ in versionResponse() }
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: http))
        let before = h.device.settings.relays.relays.map(\.id)
        for host in ["app.next.forkmesh.com", "https://next.forkmesh.com/"] {
            add.host = host
            await add.check()
            XCTAssertEqual(add.result?.status, .duplicate, host)
            XCTAssertEqual(add.known?.id, AppEnv.prodHost, host)
            XCTAssertTrue(add.canAdd, host)
            XCTAssertEqual(add.addLabel, "Open ForkMesh", host)
            XCTAssertTrue(add.result?.message.contains("ForkMesh (app.forkmesh.com)") == true, host)
            XCTAssertEqual(add.add()?.id, AppEnv.prodHost, host)
        }
        XCTAssertTrue(http.requests.isEmpty)
        XCTAssertEqual(h.device.settings.relays.relays.map(\.id), before)
        add.host = "staging.forkmesh.com"
        await add.check()
        XCTAssertNil(add.known, "typing a known host itself is a plain duplicate")
        XCTAssertFalse(add.canAdd)
    }

    func testAHostThatPermanentlyRedirectsToAKnownProjectIsThatProject() async throws {
        let h = try await SwitchHarness.make()
        let http = FakeHTTPClient { request in
            if request.url.host == "beta.acme.dev" {
                return HTTPResponse(status: 308, headers: ["location": "https://app.forkmesh.com/api/version"])
            }
            return versionResponse()
        }
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: http))
        let before = h.device.settings.relays.relays.map(\.id)
        add.host = "beta.acme.dev"
        await add.check()
        XCTAssertEqual(add.result?.status, .duplicate)
        XCTAssertEqual(add.known?.id, AppEnv.prodHost)
        XCTAssertEqual(http.requests.map(\.url.host), ["beta.acme.dev"])
        XCTAssertEqual(http.requests.first?.followRedirects, false)
        XCTAssertEqual(add.add()?.id, AppEnv.prodHost)
        XCTAssertEqual(h.device.settings.relays.relays.map(\.id), before, "no duplicate row")
    }

    func testAPermanentRedirectToANewHostSavesTheTarget() async throws {
        let h = try await SwitchHarness.make()
        let http = FakeHTTPClient { request in
            if request.url.host == "old.acme.dev" {
                return HTTPResponse(status: 301, headers: ["location": "https://relay.acme.dev/api/version"])
            }
            return versionResponse()
        }
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: http))
        add.host = "old.acme.dev"
        add.label = "Acme"
        await add.check()
        XCTAssertTrue(add.canAdd)
        XCTAssertNil(add.known)
        XCTAssertEqual(add.addLabel, "Add relay")
        let saved = try XCTUnwrap(add.add())
        XCTAssertEqual(saved.id, "relay.acme.dev")
        XCTAssertEqual(saved.label, "Acme")
        XCTAssertNil(h.device.settings.relays.byId("old.acme.dev"))
    }

    func testATemporaryRedirectIsFollowedWithoutRenamingTheProject() async throws {
        let h = try await SwitchHarness.make()
        let http = FakeHTTPClient { request in
            if request.url.host == "lab.acme.dev" && !request.followRedirects {
                return HTTPResponse(status: 302, headers: ["location": "https://elsewhere.acme.dev/api/version"])
            }
            return versionResponse()
        }
        let add = AddRelayModel(relays: h.device.settings.relays, probe: RelayProbe(http: http))
        add.host = "lab.acme.dev"
        await add.check()
        XCTAssertTrue(add.canAdd)
        XCTAssertEqual(add.add()?.id, "lab.acme.dev")
        XCTAssertEqual(http.requests.map(\.followRedirects), [false, true])
    }
}

final class AddProjectModelTests: XCTestCase {
    @MainActor
    func directory() -> RelayDirectory {
        RelayDirectory.load(prefs: InMemoryKeyValueStore(), canonicalizeUrl: SettingsService.canonicalServerUrl)
    }

    @MainActor
    func testANameAloneCreatesAProjectWithoutAnyRequest() async throws {
        let seen = Counter()
        let relays = directory()
        let model = AddProjectModel(relays: relays, probe: probe { request in
            seen.hit(request.url)
            return versionResponse()
        })
        XCTAssertFalse(model.canCreate)
        model.name = "Garden"
        XCTAssertTrue(model.canCreate)
        XCTAssertFalse(model.needsCheck)
        let created = try XCTUnwrap(model.create(logo: "garden.png"))
        XCTAssertFalse(created.hasInstance)
        XCTAssertEqual(created.label, "Garden")
        XCTAssertEqual(created.logo, "garden.png")
        XCTAssertEqual(seen.count, 0)
        XCTAssertNotNil(relays.byId(created.id))
    }

    @MainActor
    func testAnInstanceIsCheckedBeforeTheProjectIsCreated() async throws {
        let relays = directory()
        let model = AddProjectModel(relays: relays, probe: probe { _ in versionResponse() })
        model.name = "Staging"
        model.host = "staging.forkmesh.com"
        XCTAssertFalse(model.canCreate)
        XCTAssertTrue(model.needsCheck)
        XCTAssertNil(model.create())
        await model.check()
        XCTAssertTrue(model.canCreate)
        let created = try XCTUnwrap(model.create(logo: "s.png"))
        XCTAssertTrue(created.hasInstance)
        XCTAssertEqual(created.id, "staging.forkmesh.com")
        XCTAssertEqual(created.label, "Staging")
        XCTAssertEqual(relays.byId("staging.forkmesh.com")?.logo, "s.png")
    }
}

final class AttachInstanceModelTests: XCTestCase {
    @MainActor
    func testAnInstanceIsAttachedOnlyAfterItIsChecked() async throws {
        let seen = Counter()
        var attachedTo: [String] = []
        let model = AttachInstanceModel(probe: probe { request in
            seen.hit(request.url)
            return versionResponse()
        }) { instance in
            attachedTo.append(instance.serverUrl)
            return RelaySwitchResult(ok: true, relay: instance)
        }
        XCTAssertFalse(model.hasHost)
        await model.check()
        XCTAssertEqual(seen.count, 0)
        let early = await model.attach()
        XCTAssertFalse(early)
        model.host = "relay.example.com"
        await model.check()
        XCTAssertEqual(seen.count, 1)
        let verified = try XCTUnwrap(model.verified)
        XCTAssertTrue(verified.hasInstance)
        let done = await model.attach()
        XCTAssertTrue(done)
        XCTAssertEqual(attachedTo, [verified.serverUrl])
        model.host = "other.example.com"
        XCTAssertNil(model.verified)
    }

    @MainActor
    func testAFailedAttachSaysWhy() async throws {
        let model = AttachInstanceModel(probe: probe { _ in versionResponse() }) { instance in
            RelaySwitchResult(ok: false, relay: instance, error: "No.")
        }
        model.host = "relay.example.com"
        await model.check()
        let done = await model.attach()
        XCTAssertFalse(done)
        XCTAssertEqual(model.error, "No.")
    }
}
