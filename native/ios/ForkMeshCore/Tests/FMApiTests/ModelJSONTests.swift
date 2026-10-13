// The DartCore scalar semantics the models rely on (ModelJsonKt's helpers in
// native/android ModelsTest.testDartScalarHelpers, plus the corners Swift
// has to spell out).

import XCTest
import FMBase
@testable import FMApi

final class ModelJSONTests: XCTestCase {
    func testIntFollowsDartIntTryParse() {
        XCTAssertEqual(ModelJSON.int("0x1F"), 31)
        XCTAssertEqual(ModelJSON.int("1.5"), 0)
        XCTAssertEqual(ModelJSON.int(1.9), 1)
        XCTAssertEqual(ModelJSON.int(-1.9), -1)
        XCTAssertEqual(ModelJSON.int(" +12 "), 12)
        XCTAssertEqual(ModelJSON.int("-7"), -7)
        XCTAssertEqual(ModelJSON.int(true), 0)
        XCTAssertEqual(ModelJSON.int(nil), 0)
        XCTAssertEqual(ModelJSON.int(.null), 0)
        XCTAssertEqual(ModelJSON.int("99999999999999999999"), 0)
        XCTAssertEqual(ModelJSON.optionalInt(nil), -1)
        XCTAssertEqual(ModelJSON.optionalInt("x"), -1)
        XCTAssertEqual(ModelJSON.optionalInt("0"), 0)
        XCTAssertNil(DartCore.intTryParse("0x"))
        XCTAssertNil(DartCore.intTryParse("-"))
    }

    func testDoubleAndBoolFollowDart() {
        XCTAssertEqual(ModelJSON.double("1.25"), 1.25)
        XCTAssertEqual(ModelJSON.double(" 2e3 "), 2000)
        XCTAssertEqual(ModelJSON.double(".5"), 0.5)
        XCTAssertEqual(ModelJSON.double("0x10"), 0)
        XCTAssertEqual(ModelJSON.double("inf"), 0)
        XCTAssertEqual(ModelJSON.double(3), 3)
        XCTAssertEqual(ModelJSON.double(nil), 0)
        XCTAssertTrue(ModelJSON.double("NaN").isNaN)
        XCTAssertTrue(ModelJSON.bool("YES"))
        XCTAssertTrue(ModelJSON.bool(" 1 "))
        XCTAssertTrue(ModelJSON.bool(1))
        XCTAssertFalse(ModelJSON.bool("no"))
        XCTAssertFalse(ModelJSON.bool(nil))
    }

    func testStringsFollowDartInterpolation() {
        XCTAssertEqual(ModelJSON.string(nil), "")
        XCTAssertEqual(ModelJSON.string(.null), "")
        XCTAssertEqual(ModelJSON.string(2.0), "2.0")
        XCTAssertEqual(DartCore.toString(.null), "null")
        XCTAssertEqual(ModelJSON.firstString(["a": " ", "b": nil, "c": " x "], ["a", "b", "c"]), "x")
        XCTAssertEqual(ModelJSON.coalesce(nil, .null, 3), 3)
        XCTAssertNil(ModelJSON.coalesce(nil, .null))
    }

    func testTrimDoubleAndToStringAsFixed() {
        XCTAssertEqual(ModelJSON.trimDouble(1.25), "1.25")
        XCTAssertEqual(ModelJSON.trimDouble(2.0), "2")
        XCTAssertEqual(ModelJSON.trimDouble(0.000009), "0.000009")
        XCTAssertEqual(ModelJSON.trimDouble(1e-7), "1e-7")
        XCTAssertEqual(ModelJSON.trimDouble(10), "10")
        XCTAssertEqual(DartCore.toStringAsFixed(0.125, 2), "0.13")
        XCTAssertEqual(DartCore.toStringAsFixed(1.005, 2), "1.00") // 1.005 is 1.00499999... in binary
        XCTAssertEqual(DartCore.toStringAsFixed(2.5, 0), "3")
        XCTAssertEqual(DartCore.toStringAsFixed(9.96, 1), "10.0")
        XCTAssertEqual(DartCore.toStringAsFixed(0, 2), "0.00")
        XCTAssertEqual(DartCore.toStringAsFixed(-0.001, 2), "-0.00")
        XCTAssertEqual(DartCore.toStringAsFixed(14.2571544647, 1), "14.3")
        XCTAssertEqual(DartCore.toStringAsFixed(1e21, 2), "1e+21")
    }

    func testTitleCaseTruncateAndCompactDuration() {
        XCTAssertEqual(ModelJSON.titleCase("weird_state"), "Weird State")
        XCTAssertEqual(ModelJSON.titleCase("  a--b  "), "A B")
        XCTAssertEqual(ModelJSON.titleCase("__"), "")
        XCTAssertEqual(ModelJSON.truncate("abcdef", 5), "ab...")
        XCTAssertEqual(ModelJSON.truncate("abc", 0), "abc")
        XCTAssertEqual(ModelJSON.truncate("abcdef", 2), "...")
        XCTAssertEqual(ModelJSON.compactDuration(milliseconds: 9_999), "9s")
        XCTAssertEqual(ModelJSON.compactDuration(milliseconds: 65_000), "1m 5s")
        XCTAssertEqual(ModelJSON.compactDuration(milliseconds: -5), "0s")
        XCTAssertEqual(ModelJSON.epochMs(1_700_000_000), 1_700_000_000_000)
        XCTAssertEqual(ModelJSON.epochMs(-1), 0)
    }

    func testDateTimeTryParse() {
        let utc = TimeZone(identifier: "UTC")!
        XCTAssertEqual(DartCore.dateTimeTryParseMs("2026-07-04T12:00:00Z"), 1_783_166_400_000)
        XCTAssertEqual(DartCore.dateTimeTryParseMs("20260704T120000.5Z"), 1_783_166_400_500)
        XCTAssertEqual(DartCore.dateTimeTryParseMs("2026-07-04", timeZone: utc), 1_783_123_200_000)
        XCTAssertEqual(DartCore.dateTimeTryParseMs("2026-13-01", timeZone: utc), DartCore.dateTimeTryParseMs("2027-01-01", timeZone: utc))
        XCTAssertEqual(DartCore.dateTimeTryParseMs("2026-07-04T12:00:00-0130"), 1_783_171_800_000)
        let tokyo = TimeZone(identifier: "Asia/Tokyo")!
        XCTAssertEqual(DartCore.dateTimeTryParseMs("2026-07-04 21:00", timeZone: tokyo), 1_783_166_400_000)
        XCTAssertNil(DartCore.dateTimeTryParseMs("July 4"))
        XCTAssertNil(DartCore.dateTimeTryParseMs("2026-07-04T"))
    }

    func testLocalDateAndEncodeComponent() {
        XCTAssertEqual(ModelJSON.localDate(0, timeZone: TimeZone(identifier: "UTC")!), "1970-01-01")
        XCTAssertEqual(ModelJSON.localDate(0, timeZone: TimeZone(identifier: "America/New_York")!), "1969-12-31")
        XCTAssertEqual(DartCore.encodeComponent("a b/c?d=é!*'()~"), "a%20b%2Fc%3Fd%3D%C3%A9!*'()~")
    }
}
