// ApiService.sponsoredAd and the API cases of native/android SponsoredAdTest.kt: the
// unauthenticated ForkMesh Ads serve and click calls. The SponsoredAd.fromJson cases are in
// SponsoredAdTests.swift; the sponsored-message preferences belong with SettingsService
// (FMAccount).

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let adId = "0123456789abcdef0123456789abcdef"

private func adPayload(_ overrides: JSONObject = [:]) -> JSONObject {
    var payload: JSONObject = [
        "id": .string(adId),
        "label": "Sponsored",
        "headline": "Ship signed releases",
        "body": "Reproducible builds for small teams.",
        "cta": "See how",
        "sponsor": "Example Tools",
        "url": "https://example.com/releases",
        "category": "developer-tools",
        "categoryLabel": "Developer tools and libraries",
    ]
    payload.merge(overrides)
    return payload
}

@MainActor
final class ApiSponsoredAdTests: XCTestCase {
    func testServesUnauthenticatedForTheMobilePlacementAndCaches() async throws {
        let http = FakeHTTPClient { _ in
            HTTPResponse(status: 200, text: portEncode(["ok": true, "placement": "mobile", "ad": .object(adPayload())]))
        }
        let api = ApiPortFixture.api(http, token: "secret-session-token")

        let ad = await api.sponsoredAd()
        XCTAssertEqual(ad?.id, adId)
        let again = await api.sponsoredAd()
        XCTAssertEqual(again?.id, adId)

        XCTAssertEqual(http.requests.count, 1, "second call is served from cache")
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(request.method, "GET")
        XCTAssertEqual(portRawPath(request), "/api/ads/serve")
        XCTAssertEqual(portQuery(request), ["placement": "mobile"])
        XCTAssertNil(request.headers["Authorization"])
        XCTAssertNil(request.headers["Cookie"])
    }

    func testANullAdAnInvalidAdAndAFailureAreAllNullOnce() async {
        let bodies: [(String, HTTPResponse)] = [
            ("null ad", HTTPResponse(status: 200, text: portEncode(["ok": true, "ad": nil]))),
            ("invalid ad", HTTPResponse(
                status: 200, text: portEncode(["ok": true, "ad": .object(adPayload(["url": "http://example.com/"]))]))),
            ("not ok", HTTPResponse(status: 200, text: portEncode(["ok": false, "ad": .object(adPayload())]))),
            ("server error", HTTPResponse(status: 500, text: "boom")),
            ("not json", HTTPResponse(status: 200, text: "<html>")),
        ]
        for (name, response) in bodies {
            let http = FakeHTTPClient { _ in response }
            let api = ApiPortFixture.api(http, token: "")
            let first = await api.sponsoredAd()
            let second = await api.sponsoredAd()
            XCTAssertNil(first, name)
            XCTAssertNil(second, name)
            XCTAssertEqual(http.requests.count, 1, "\(name): never retried")
        }

        let throwing = FakeHTTPClient { _ in throw HTTPError.transport("offline") }
        let api = ApiPortFixture.api(throwing, token: "")
        let first = await api.sponsoredAd()
        let second = await api.sponsoredAd()
        XCTAssertNil(first)
        XCTAssertNil(second)
        XCTAssertEqual(throwing.requests.count, 1)
    }

    /// Kotlin: an ad is reused for 30 minutes, a failure for 5.
    func testCacheExpiresAfterThirtyMinutesAndFailuresAfterFive() async {
        let fail = PortBox(false)
        let http = FakeHTTPClient { _ in
            fail.value
                ? HTTPResponse(status: 500, text: "boom")
                : HTTPResponse(status: 200, text: portEncode(["ok": true, "ad": .object(adPayload())]))
        }
        let clock = FixedClock(epochMillis: 1_000_000)
        let api = ApiPortFixture.api(http, token: "", clock: clock)

        var ad = await api.sponsoredAd()
        XCTAssertNotNil(ad)
        clock.advance(by: ApiService.sponsoredAdTtl - 1)
        _ = await api.sponsoredAd()
        XCTAssertEqual(http.requests.count, 1)
        clock.advance(by: 1)
        fail.value = true
        ad = await api.sponsoredAd()
        XCTAssertNil(ad)
        XCTAssertEqual(http.requests.count, 2)
        clock.advance(by: ApiService.sponsoredAdFailureTtl - 1)
        ad = await api.sponsoredAd()
        XCTAssertNil(ad)
        XCTAssertEqual(http.requests.count, 2)
        clock.advance(by: 1)
        fail.value = false
        ad = await api.sponsoredAd()
        XCTAssertNotNil(ad)
        XCTAssertEqual(http.requests.count, 3)
    }

    func testClickIsAnUnauthenticatedPostAndSwallowsFailures() async throws {
        let http = FakeHTTPClient { _ in throw HTTPError.transport("offline") }
        let api = ApiPortFixture.api(http, token: "secret-session-token")

        await api.sponsoredAdClicked(adId)
        await api.sponsoredAdClicked("../../accounts/me")

        XCTAssertEqual(http.requests.count, 1, "a malformed id is never sent")
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(portRawPath(request), "/api/ads/\(adId)/click")
        XCTAssertEqual(portBody(request), ["placement": "mobile"])
        XCTAssertNil(request.headers["Authorization"])
    }
}
