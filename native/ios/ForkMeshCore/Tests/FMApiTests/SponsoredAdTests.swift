// `SponsoredAd.fromJson` (the ApiService and widget cases belong to the layers above).

import XCTest
import FMBase
@testable import FMApi

final class SponsoredAdTests: XCTestCase {
    private let id = "0123456789abcdef0123456789abcdef"

    private func payload(_ overrides: JSONObject = [:], removing: String? = nil) -> JSONValue {
        var json: JSONObject = [
            "id": .string(id), "label": "Sponsored", "headline": "Ship signed releases",
            "body": "Reproducible builds for small teams.", "cta": "See how", "sponsor": "Example Tools",
            "url": "https://example.com/releases", "category": "developer-tools",
            "categoryLabel": "Developer tools and libraries",
        ]
        json.merge(overrides)
        if let removing { json.removeValue(forKey: removing) }
        return .object(json)
    }

    func testAcceptsAWellFormedAd() throws {
        let ad = try XCTUnwrap(SponsoredAd.parse(payload()))
        XCTAssertEqual(ad.id, id)
        XCTAssertEqual(ad.headline, "Ship signed releases")
        XCTAssertEqual(ad.body, "Reproducible builds for small teams.")
        XCTAssertEqual(ad.cta, "See how")
        XCTAssertEqual(ad.sponsor, "Example Tools")
        XCTAssertEqual(ad.url, "https://example.com/releases")
        XCTAssertEqual(ad.uri?.host, "example.com")
        XCTAssertEqual(ad.categoryLabel, "Developer tools and libraries")
    }

    func testRejectsNullNonMapsAndAMissingAd() {
        XCTAssertNil(SponsoredAd.parse(nil))
        XCTAssertNil(SponsoredAd.parse(.null))
        XCTAssertNil(SponsoredAd.parse("ad"))
        XCTAssertNil(SponsoredAd.parse([]))
        XCTAssertNil(SponsoredAd.parse(.object([:])))
    }

    func testRejectsEveryDestinationThatIsNotPlainHttps() {
        let urls: [JSONValue] = [
            "http://example.com/",
            "javascript:alert(1)",
            "JAVASCRIPT:alert(1)",
            "data:text/html,<script>alert(1)</script>",
            "intent://example.com#Intent;scheme=https;end",
            "forkmesh://open",
            "file:///etc/passwd",
            "//example.com/",
            "example.com",
            "https://",
            "https:///path",
            "https://forkmesh.com@evil.example/",
            #"https://example.com\@evil.example/"#,
            "HTTPS://example.com/",
            "https://example.com/ a",
            "https://example.com/\njavascript:alert(1)",
            "",
            42,
            nil,
        ]
        for url in urls {
            XCTAssertNil(SponsoredAd.parse(payload(["url": url])), "url: \(url)")
        }
        XCTAssertNil(SponsoredAd.parse(payload(["url": .string("https://example.com/" + String(repeating: "a", count: 2048))])))
    }

    func testRejectsIdsThatAreNot32LowercaseHexCharacters() {
        let ids: [JSONValue] = [
            "",
            "abc",
            "0123456789abcdef0123456789abcde",
            "0123456789abcdef0123456789abcdef0",
            "0123456789ABCDEF0123456789ABCDEF",
            "0123456789abcdef0123456789abcdeg",
            "../../accounts/0123456789abcdef01",
            123,
            nil,
        ]
        for bad in ids {
            XCTAssertNil(SponsoredAd.parse(payload(["id": bad])), "id: \(bad)")
        }
    }

    func testRejectsAnEmptyOrNonTextHeadline() {
        XCTAssertNil(SponsoredAd.parse(payload(["headline": ""])))
        XCTAssertNil(SponsoredAd.parse(payload(["headline": "   \n "])))
        XCTAssertNil(SponsoredAd.parse(payload(["headline": 7])))
        XCTAssertNil(SponsoredAd.parse(payload(removing: "headline")))
    }

    func testClampsEveryTextFieldToThePublishedLimits() throws {
        let ad = try XCTUnwrap(SponsoredAd.parse(payload([
            "headline": .string(String(repeating: "h", count: 200)),
            "body": .string(String(repeating: "b", count: 500)),
            "cta": .string(String(repeating: "c", count: 80)),
            "sponsor": .string(String(repeating: "s", count: 90)),
        ])))
        XCTAssertEqual(ad.headline.count, 60)
        XCTAssertEqual(ad.body.count, 160)
        XCTAssertEqual(ad.cta.count, 24)
        XCTAssertEqual(ad.sponsor.count, 48)
    }

    func testClampingNeverSplitsASurrogatePair() throws {
        let ad = try XCTUnwrap(SponsoredAd.parse(payload(["headline": .string(String(repeating: "\u{1F680}", count: 80))])))
        XCTAssertEqual(ad.headline.unicodeScalars.count, 60)
        XCTAssertTrue(ad.headline.unicodeScalars.allSatisfy { $0.value == 0x1F680 })
        // Under the UTF-16 limit twice over but within it in code points: kept whole.
        let short = try XCTUnwrap(SponsoredAd.parse(payload(["headline": .string(String(repeating: "\u{1F680}", count: 40))])))
        XCTAssertEqual(short.headline.unicodeScalars.count, 40)
    }

    func testCollapsesWhitespaceAndFillsOptionalFields() throws {
        let ad = try XCTUnwrap(SponsoredAd.parse(payload(
            ["headline": "  Two\n\nlines\tof   text ", "body": nil, "cta": "", "sponsor": 12],
            removing: "categoryLabel")))
        XCTAssertEqual(ad.headline, "Two lines of text")
        XCTAssertEqual(ad.body, "")
        XCTAssertEqual(ad.cta, SponsoredAd.defaultCta)
        XCTAssertEqual(ad.sponsor, "")
        XCTAssertEqual(ad.categoryLabel, "")
    }
}
