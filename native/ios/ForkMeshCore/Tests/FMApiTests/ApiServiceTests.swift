// The account, catalog, network and URL parts of native/android
// core/src/test/.../api/ApiServiceTest.kt, plus the ApiService URL half of the production
// endpoint cases (ProductionEndpointTest.kt). FakeHTTPClient answers and records the requests.

import FMAccount
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiServiceTests: XCTestCase {
    // MARK: Catalog

    func testRepositoriesRetriesAfterAFailedCatalogLoad() async throws {
        let count = ApiCallCounter()
        let http = FakeHTTPClient { request in
            let n = count.next()
            XCTAssertEqual(request.apiPath, "/api/repositories")
            if n <= 2 { return apiText("temporary failure", status: 500) }
            return apiJSON(["ok": true, "repositories": [["owner": "owner", "name": "forkmesh"]]])
        }
        let api = makeTestApi(http)

        let error = await apiAssertThrows { try await api.repositories() }
        XCTAssertTrue(error is ApiException, "\(String(describing: error))")
        // The 500 is retried once inside the call.
        XCTAssertEqual(count.value, 2)

        let repos = try await api.repositories()
        XCTAssertEqual(count.value, 3)
        XCTAssertEqual(repos.map(\.fullName), ["owner/forkmesh"])

        // Fresh for the catalog TTL: no further request.
        _ = try await api.repositories()
        XCTAssertEqual(count.value, 3)
    }

    func testAuthenticatedCatalogRequestsCarryTheAccountBearerToken() async throws {
        let http = FakeHTTPClient { _ in apiJSON(["repositories": []]) }
        _ = try await makeTestApi(http, token: "session-token").repositories()

        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.apiHeader("Authorization"), "Bearer session-token")
        XCTAssertEqual(http.lastRequest?.apiHeader("Accept"), "application/json")
    }

    func testPublishAndImportReturnTheStatusInsteadOfThrowing() async throws {
        let http = FakeHTTPClient { request in
            request.apiPath == "/api/repositories"
                ? apiJSON(["mirror": true], status: 202)
                : apiJSON(["error": "stale_update"], status: 409)
        }
        let api = makeTestApi(http, token: "tok")

        let published = try await api.publishRepository(["owner": "alice", "catalogSig": "sig"])
        XCTAssertTrue(published.ok)
        XCTAssertEqual(published.statusCode, 202)
        XCTAssertEqual(published.body["mirror"], .bool(true))
        XCTAssertEqual(http.lastRequest?.method, "POST")
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"owner":"alice","catalogSig":"sig"}"#)

        let imported = try await api.createRepositoryImport(sourceUrl: "https://github.com/a/b", targetOwner: "alice")
        XCTAssertFalse(imported.ok)
        XCTAssertEqual(imported.statusCode, 409)
        XCTAssertEqual(imported.error, "stale_update")
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/repository-imports")
        XCTAssertEqual(
            http.lastRequest?.bodyText,
            #"{"sourceUrl":"https://github.com/a/b","targetOwner":"alice","mode":"import","sessionToken":"tok"}"#)

        _ = try await api.createRepositoryImport(sourceUrl: "u", targetOwner: "o", mode: "mirror", providerToken: "ghp")
        XCTAssertEqual(
            http.lastRequest?.bodyText,
            #"{"sourceUrl":"u","targetOwner":"o","mode":"mirror","providerToken":"ghp","sessionToken":"tok"}"#)
    }

    func testASessionShaped401OnAPublishBouncesButBadSignatureDoesNot() async throws {
        let code = ApiShared("bad_signature")
        let http = FakeHTTPClient { _ in apiJSON(["error": .string(code.value)], status: 401) }
        let api = makeTestApi(http, token: "tok")
        var codes: [String] = []
        var unauthorized = 0
        api.onSessionInvalid = { codes.append($0) }
        api.onUnauthorized = { unauthorized += 1 }

        let refused = try await api.publishRepository([:])
        XCTAssertEqual(refused.statusCode, 401)
        XCTAssertEqual(unauthorized, 0)
        XCTAssertEqual(codes, [])

        code.value = "invalid_session"
        _ = try await api.publishRepository([:])
        XCTAssertEqual(codes, ["invalid_session"])
        XCTAssertEqual(unauthorized, 1)
    }

    // MARK: Session handling

    func testBearer401InvokesTheCallbacksAndFailsClosed() async throws {
        let http = FakeHTTPClient { _ in apiText("", status: 401) }
        let api = makeTestApi(http, token: "expired-token")
        var unauthorized = 0
        var codes: [String] = []
        api.onUnauthorized = { unauthorized += 1 }
        api.onSessionInvalid = { codes.append($0) }

        let error = await apiAssertThrows { try await api.notifications("alice") }
        XCTAssertTrue(apiErrorText(error).contains("session expired"), apiErrorText(error))
        XCTAssertEqual(unauthorized, 1)
        XCTAssertEqual(codes, [""])

        http.answerAll { _ in apiJSON(["error": "device_revoked"], status: 401) }
        let revoked = await apiAssertThrows { try await api.notifications("alice") }
        XCTAssertEqual(apiErrorMessage(revoked), "This device was signed out from the web. Please sign in again.")
        XCTAssertEqual(codes, ["", "device_revoked"])

        http.answerAll { _ in apiJSON(["error": "unauthorized"], status: 401) }
        let repo = await apiAssertThrows { try await api.issues("owner", "repo") }
        XCTAssertTrue(apiErrorMessage(repo).contains("private repo view token"), apiErrorMessage(repo))
        XCTAssertEqual(codes, ["", "device_revoked"])
        XCTAssertEqual(unauthorized, 2)

        http.answerAll { _ in apiJSON(["error": "invalid_session"], status: 401) }
        _ = await apiAssertThrows { try await api.issues("owner", "repo") }
        XCTAssertEqual(codes, ["", "device_revoked", "invalid_session"])
    }

    func testSignedOut401DoesNotFireSessionCallbacks() async throws {
        let http = FakeHTTPClient { _ in apiText("", status: 401) }
        let api = makeTestApi(http)
        var fired = 0
        api.onUnauthorized = { fired += 1 }
        api.onSessionInvalid = { _ in fired += 1 }

        _ = await apiAssertThrows { try await api.repositories() }
        XCTAssertEqual(fired, 0)
    }

    // MARK: Account

    func testRoomChatKeyRequestIncludesTheConfiguredRepositoryRoom() async throws {
        let http = FakeHTTPClient { _ in apiJSON(["passphrase": "room-passphrase"]) }
        let api = makeTestApi(http)

        let passphrase = try await api.roomChatPassphrase()
        XCTAssertEqual(passphrase, "room-passphrase")
        XCTAssertEqual(http.requests.count, 1)
        let request = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(request.apiPath, "/api/chat/room-key")
        XCTAssertEqual(request.apiQuery, ["owner": "mainnode", "repo": "forkmesh", "room": "world-general"])

        http.answerAll { _ in apiJSON(["passphrase": 5]) }
        let error = await apiAssertThrows { try await api.roomChatPassphrase() }
        XCTAssertEqual(apiErrorText(error), "Exception: room key unavailable")
    }

    func testNotificationsFetchMarkReadAndClearThroughTheWorkerApi() async throws {
        let http = apiRouteByPath { path, _, request in
            guard path == "/api/notifications" else { return apiJSON(["error": "not_found"], status: 404) }
            if request.method == "GET" {
                return apiJSON([
                    "ok": true,
                    "unread": 1,
                    "total": 137,
                    "notifications": [[
                        "id": "n1", "kind": "mention", "title": "You were mentioned in mona/forkmesh",
                        "repo": "mona/forkmesh", "ts": 1_770_000_000_000,
                    ]],
                ])
            }
            if request.method == "POST" || request.method == "DELETE" { return apiJSON(["ok": true]) }
            return nil
        }
        let api = makeTestApi(http)

        let page = try await api.notifications(" Mona ", limit: 25)
        try await api.markNotificationsRead("mona", ids: ["n1"])
        try await api.markNotificationsRead("mona", all: true)
        try await api.deleteAllNotifications(" Mona ")

        XCTAssertEqual(page.unread, 1)
        // The page holds one row; the inbox holds 137. A whole-inbox action quotes the latter.
        XCTAssertEqual(page.total, 137)
        XCTAssertEqual(page.notifications.map(\.kind), ["mention"])
        let requests = http.requests
        XCTAssertEqual(requests.count, 4)
        XCTAssertEqual(requests[0].method, "GET")
        XCTAssertEqual(requests[0].apiRawQuery, "node=mona&limit=25")
        XCTAssertEqual(requests[1].method, "POST")
        XCTAssertEqual(requests[1].bodyText, #"{"node":"mona","ids":["n1"]}"#)
        XCTAssertEqual(requests[2].method, "POST")
        XCTAssertEqual(requests[2].bodyText, #"{"node":"mona","all":true}"#)
        // Clearing the inbox is a DELETE, not another mark-read POST: the Worker binds a separate
        // capability to it.
        XCTAssertEqual(requests[3].method, "DELETE")
        XCTAssertEqual(requests[3].bodyText, #"{"node":"mona","all":true}"#)

        // No account: nothing is requested.
        let empty = try await api.notifications("  ")
        XCTAssertEqual(empty.unread, 0)
        try await api.markNotificationsRead("")
        try await api.deleteAllNotifications(" ")
        XCTAssertEqual(http.requests.count, 4)
    }

    func testAccountEventTicketOutcomesStayDistinguishable() async throws {
        let http = FakeHTTPClient { _ in apiJSON(["ticket": "abc"]) }
        let signedOut = await makeTestApi(http).accountEventTicket()
        XCTAssertEqual(signedOut, EventTicket(.signedOut))
        XCTAssertEqual(http.requests.count, 0)

        let api = makeTestApi(http, token: "tok")
        let ok = await api.accountEventTicket()
        XCTAssertEqual(ok, EventTicket(.ok, "abc"))
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/accounts/event-ticket")
        XCTAssertEqual(http.lastRequest?.apiHeader("Authorization"), "Bearer tok")
        XCTAssertEqual(http.lastRequest?.apiHeader("accept"), "application/json")

        let cases: [(HTTPResponse, EventTicketStatus)] = [
            (apiText("", status: 401), .signedOut),
            (apiText("", status: 403), .signedOut),
            (apiText("", status: 502), .unavailable),
            (apiJSON(["authenticated": false, "ticket": "x"]), .signedOut),
            (apiJSON(["ticket": ""]), .signedOut),
            (apiJSON([1]), .unavailable),
            (apiText("not json"), .unavailable),
        ]
        for (response, status) in cases {
            http.answerAll { _ in response }
            let ticket = await api.accountEventTicket()
            XCTAssertEqual(ticket.status, status, response.text)
        }
        http.answerAll { _ in throw HTTPError.transport("offline") }
        let offline = await api.accountEventTicket()
        XCTAssertEqual(offline.status, .unavailable)
    }

    func testPushRegistrationTestAndUnregister() async throws {
        let http = apiRouteByPath { path, _, request in
            switch (path, request.method) {
            case ("/api/accounts/register-device", "POST"):
                return apiJSON(["ok": true, "pushConfigured": false])
            case ("/api/accounts/register-device", "DELETE"):
                return apiJSON(["ok": true])
            case ("/api/accounts/push-test", _):
                return apiJSON([
                    "ok": true, "configured": true, "devices": 1, "delivered": 1,
                    "results": [["environment": "production", "delivered": true, "status": 200]],
                ])
            default:
                return nil
            }
        }
        let api = makeTestApi(http, token: "tok")
        let token = String(repeating: "a", count: 64)

        let blank = await api.registerPushDevice(deviceToken: "", environment: "production")
        XCTAssertEqual(blank, PushRegistration(ok: false))
        XCTAssertEqual(http.requests.count, 0)

        let registered = await api.registerPushDevice(deviceToken: token, environment: "development")
        XCTAssertEqual(registered, PushRegistration(ok: true, statusCode: 200, relayConfigured: false))
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"deviceToken":"\#(token)","environment":"development"}"#)

        let test = await api.sendTestPush()
        XCTAssertEqual(test.delivered, 1)
        XCTAssertEqual(test.summary, "Sent to 1 of 1 device. A ForkMesh banner should arrive within a few seconds.")

        let unregistered = await api.unregisterPushDevice(token)
        XCTAssertTrue(unregistered)
        XCTAssertEqual(http.lastRequest?.method, "DELETE")
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"deviceToken":"\#(token)"}"#)
        let unregisteredBlank = await api.unregisterPushDevice("")
        XCTAssertFalse(unregisteredBlank)

        http.answerAll { _ in throw HTTPError.transport("offline") }
        let offlineRegistration = await api.registerPushDevice(deviceToken: "t", environment: "production")
        XCTAssertEqual(offlineRegistration, PushRegistration(ok: false))
        let offlineTest = await api.sendTestPush()
        XCTAssertEqual(offlineTest.statusCode, 0)
        let offlineUnregister = await api.unregisterPushDevice("t")
        XCTAssertFalse(offlineUnregister)
    }

    func testAccountAvatarIsReadOncePerSession() async throws {
        let http = FakeHTTPClient { request in
            switch request.apiPath {
            case "/api/accounts/alice": return apiJSON(["exists": true, "kind": "user", "avatarPng": "iVBOR"])
            case "/api/accounts/acme": return apiJSON(["exists": true, "kind": "org", "avatarPng": "x"])
            default: return apiText("", status: 404)
            }
        }
        let api = makeTestApi(http)

        let first = await api.accountAvatarPng(" Alice ")
        XCTAssertEqual(first, "iVBOR")
        let again = await api.accountAvatarPng("alice")
        XCTAssertEqual(again, "iVBOR")
        let org = await api.accountAvatarPng("acme")
        XCTAssertEqual(org, "")
        let missing = await api.accountAvatarPng("mainnode-a")
        XCTAssertEqual(missing, "")
        let missingAgain = await api.accountAvatarPng("mainnode-a")
        XCTAssertEqual(missingAgain, "")
        let blank = await api.accountAvatarPng(" ")
        XCTAssertEqual(blank, "")
        XCTAssertEqual(http.requests.count, 3)
    }

    func testAccountAvatarTellsAnUnreachableServerFromNoPicture() async throws {
        let http = FakeHTTPClient { request in
            switch request.apiPath {
            case "/api/accounts/alice": return apiJSON(["exists": true, "kind": "user", "avatarPng": "iVBOR"])
            case "/api/accounts/bob": throw HTTPError.transport("offline")
            default: return apiText("", status: 404)
            }
        }
        let api = makeTestApi(http)

        let alice = await api.accountAvatarPngIfReached("alice")
        XCTAssertEqual(alice, "iVBOR")
        let missing = await api.accountAvatarPngIfReached("mainnode-a")
        XCTAssertEqual(missing, "")
        let offline = await api.accountAvatarPngIfReached("bob")
        XCTAssertNil(offline)
        let offlinePlain = await api.accountAvatarPng("bob")
        XCTAssertEqual(offlinePlain, "")
    }

    func testListDevicesAcceptsAWrappedOrBareList() async throws {
        let http = FakeHTTPClient { _ in apiJSON([["id": "d1", "kind": "mobile_node", "enabled": 0]]) }
        let devices = try await makeTestApi(http, token: "tok").listDevices()
        XCTAssertEqual(devices.map(\.id), ["d1"])
        XCTAssertEqual(devices.first?.enabled, false)

        http.answerAll { _ in apiJSON(["devices": "nope"]) }
        let none = try await makeTestApi(http).listDevices()
        XCTAssertEqual(none.count, 0)
    }

    // MARK: Network

    func testNetworkStatsParsesCountsWithoutRequestingPayoutReadiness() async throws {
        let http = apiRouteByPath { path, _, request in
            guard request.method == "GET", path == "/api/network/stats" else { return nil }
            return apiJSON([
                "clients": 4,
                "hosts": 1,
                "repos": 3,
                "onlineNodes": ["mainnode-a", "mainnode-b"],
            ])
        }
        let stats = try await makeTestApi(http).networkStats()

        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/network/stats")
        XCTAssertNil(http.lastRequest?.apiQuery["payouts"])
        XCTAssertEqual(stats.nodesOnline, 2)
        XCTAssertEqual(stats.hostsOnline, 1)
        XCTAssertEqual(stats.repos, 3)
    }

    func testNetworkStatsFailuresStayRetryable() async throws {
        let http = FakeHTTPClient { _ in apiText("", status: 503) }
        let api = makeTestApi(http)

        let first = await apiAssertThrows { try await api.networkStats() }
        XCTAssertTrue(apiErrorText(first).contains("HTTP 503"), apiErrorText(first))
        let second = await apiAssertThrows { try await api.networkStats() }
        XCTAssertTrue(apiErrorText(second).contains("HTTP 503"), apiErrorText(second))
        // A 503 is not retried inside one call.
        XCTAssertEqual(http.requests.count, 2)
    }

    func testNetworkActivityRejectsEmptyPartialAndMalformedPayloads() async throws {
        let payloads: [@Sendable (String) -> JSONValue] = [
            { _ in [:] },
            { path in path == "/api/network/stats" ? ["nodesOnline": 1] : ["fundsMainnodes": []] },
            { path in
                path == "/api/network/stats"
                    ? ["nodesOnline": "not-a-number", "hostsOnline": 1, "repos": 1]
                    : ["fundsMainnodes": [:]]
            },
        ]
        for (index, payload) in payloads.enumerated() {
            let api = makeTestApi(FakeHTTPClient { request in apiJSON(payload(request.apiPath)) })
            let stats = await apiAssertThrows({ try await api.networkStats() }, "payload \(index)")
            XCTAssertTrue(apiErrorText(stats).contains("Invalid network stats response"), "\(index): \(apiErrorText(stats))")
            let boards = await apiAssertThrows({ try await api.networkLeaderboards() }, "payload \(index)")
            XCTAssertTrue(
                apiErrorText(boards).contains("Invalid network leaderboards response"), "\(index): \(apiErrorText(boards))")
        }
    }

    func testNetworkLeaderboardsRejectMalformedNestedEntries() async throws {
        let invalid: [JSONValue] = [
            ["name": "", "lamports": 1],
            ["name": "alice", "lamports": "not-a-number"],
            ["name": "alice", "lamports": 1, "sol": "not-a-number"],
        ]
        for entry in invalid {
            let api = makeTestApi(FakeHTTPClient { _ in
                apiJSON(["fundsMainnodes": [entry], "fundsContributors": [], "fundsProjects": []])
            })
            let error = await apiAssertThrows({ try await api.networkLeaderboards() }, JSON.encode(entry))
            XCTAssertTrue(
                apiErrorText(error).contains("Invalid network leaderboards response"),
                "\(JSON.encode(entry)): \(apiErrorText(error))")
        }
    }

    func testNetworkActivityRejectsFractionalAndNegativeCounters() async throws {
        let onlineNodes = ApiShared<JSONValue>(1.5)
        let http = FakeHTTPClient { _ in
            apiJSON(["clients": 1, "hosts": 1, "repos": 1, "onlineNodes": onlineNodes.value])
        }
        let api = makeTestApi(http)
        let fractionalNodes = await apiAssertThrows { try await api.networkStats() }
        XCTAssertTrue(apiErrorText(fractionalNodes).contains("Invalid network stats response"))

        onlineNodes.value = -1
        let negative = await apiAssertThrows { try await api.networkStats() }
        XCTAssertTrue(apiErrorText(negative).contains("Invalid network stats response"))

        // Fractional counters in the counted fields are refused too.
        let fractional = makeTestApi(FakeHTTPClient { _ in
            apiJSON(["clients": 1.5, "hosts": 1, "repos": 1, "onlineNodes": []])
        })
        _ = await apiAssertThrows { try await fractional.networkStats() }

        let whole = makeTestApi(FakeHTTPClient { _ in
            apiJSON(["clients": 2.0, "hosts": "1", "repos": 1, "onlineNodes": []])
        })
        let stats = try await whole.networkStats()
        XCTAssertEqual(stats.nodesOnline, 0)
    }

    func testNetworkLeaderboardsParseFundsReceivedBoards() async throws {
        let http = apiRouteByPath { path, _, request in
            guard request.method == "GET", path == "/api/network/leaderboards" else { return nil }
            return apiJSON([
                "fundsMainnodes": [["name": "mainnode-a", "lamports": "1250000000", "sol": "1.25"]],
                "fundsContributors": [["name": "alice", "lamports": 500_000_000, "sol": 0.5]],
                "fundsProjects": [["name": "forkmesh/mobile", "lamports": "42"]],
            ])
        }
        let boards = try await makeTestApi(http).networkLeaderboards()

        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.apiPath, "/api/network/leaderboards")
        XCTAssertEqual(boards.fundsMainnodes.map(\.name), ["mainnode-a"])
        XCTAssertEqual(boards.fundsContributors.first?.amountLabel, "0.5 SOL (500000000 lamports)")
        XCTAssertEqual(boards.fundsProjects.first?.amountLabel, "42 lamports")
    }

    // MARK: URLs

    func testUrlsDeriveFromTheRelayUrl() async throws {
        let local = makeTestApi(
            FakeHTTPClient(), url: "ws://127.0.0.1:8787/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        XCTAssertEqual(
            local.rawURL("mainnode", "forkmesh", "README.md").absoluteString,
            "http://127.0.0.1:8787/api/repo/mainnode/forkmesh/raw?path=README.md")
        XCTAssertEqual(
            local.rawURL("o", "r", "docs/a b.md", ref: "main").absoluteString,
            "http://127.0.0.1:8787/api/repo/o/r/raw?path=docs%2Fa+b.md&ref=main")
        XCTAssertEqual(local.cloneURL("mainnode", "forkmesh").absoluteString, "http://127.0.0.1:8787/mainnode/forkmesh")
        XCTAssertEqual(local.gitOrigin, "http://127.0.0.1:8787")
        XCTAssertEqual(
            local.accountEventsURL("t k/1").absoluteString, "ws://127.0.0.1:8787/api/nodes/events?ticket=t+k%2F1")

        let prod = makeTestApi(
            FakeHTTPClient(), url: "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        XCTAssertEqual(prod.cloneURL("mainnode", "forkmesh").absoluteString, "https://app.forkmesh.com/mainnode/forkmesh")
        XCTAssertEqual(prod.gitOrigin, "https://app.forkmesh.com")
        XCTAssertEqual(prod.accountEventsURL("abc").absoluteString, "wss://app.forkmesh.com/api/nodes/events?ticket=abc")

        XCTAssertEqual(makeTestApi(FakeHTTPClient(), url: "https://X.test:443/ws").gitOrigin, "https://x.test")
        XCTAssertEqual(makeTestApi(FakeHTTPClient(), url: "ws://[::1]:9000/ws").gitOrigin, "http://[::1]:9000")
        let port80 = makeTestApi(FakeHTTPClient(), url: "ws://h.test:80/ws")
        XCTAssertEqual(port80.accountEventsURL("a").absoluteString, "ws://h.test:80/api/nodes/events?ticket=a")
        XCTAssertEqual(port80.cloneURL("x", "y").absoluteString, "http://h.test/x/y")
    }

    /// The ApiService half; the signup URL belongs to AuthService's tests.
    func testHttpOriginsFollowTheProductionRelayHost() async throws {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore([:]), secureStore: InMemorySecureStore([:]))
        XCTAssertEqual(settings.serverUrl, "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")
        let api = ApiService(settings: settings, http: FakeHTTPClient())

        let raw = api.rawURL("mainnode", "forkmesh", "README.md")
        XCTAssertEqual(raw.scheme, "https")
        XCTAssertEqual(raw.host, "app.forkmesh.com")
        XCTAssertNil(raw.port)
        XCTAssertTrue(raw.absoluteString.hasPrefix("https://app.forkmesh.com/"), raw.absoluteString)
        XCTAssertEqual(api.gitOrigin, "https://app.forkmesh.com")
    }
}
