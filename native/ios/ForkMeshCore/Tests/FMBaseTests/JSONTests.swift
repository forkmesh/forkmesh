// Cases of native/android core/src/test json/JsonTest.kt, plus the encoding rules signed
// payloads depend on.

import XCTest
@testable import FMBase

final class JSONTests: XCTestCase {
    func testParsesNestedValues() throws {
        let map = JSON.parseObject(#"{"a":1,"b":[true,null,"x"],"c":{"d":-2.5},"e":"é\n"}"#)
        XCTAssertEqual(map["a"], .int(1))
        XCTAssertEqual(map["b"], [true, nil, "x"])
        XCTAssertEqual(map.obj("c")["d"], .double(-2.5))
        XCTAssertEqual(map["e"], "é\n")
        XCTAssertEqual(map.keys, ["a", "b", "c", "e"])
    }

    func testWritesLikeDartJsonEncode() {
        let map: JSONObject = [
            "name": "fork\"mesh", "n": 3, "f": 1.0, "ok": true,
            "list": [1, "two"], "none": nil, "uni": "é",
        ]
        XCTAssertEqual(
            JSON.encode(map),
            #"{"name":"fork\"mesh","n":3,"f":1.0,"ok":true,"list":[1,"two"],"none":null,"uni":"é"}"#
        )
    }

    func testRoundTripsLargeIntegers() throws {
        XCTAssertEqual(try JSON.parse("1786636663064"), .int(1_786_636_663_064))
        XCTAssertEqual(JSON.encode(try JSON.parse("1786636663064")), "1786636663064")
        XCTAssertEqual(try JSON.parse("-9223372036854775808"), .int(Int64.min))
        // Past 64 bits it falls back to a double.
        XCTAssertEqual(try JSON.parse("18446744073709551616"), .double(18446744073709551616))
    }

    func testRejectsInvalidInput() {
        for bad in [#"{"a":}"#, "[1,2", "{} x", "", "01", "1.", "-", "1e", "[1,]", #"{"a":1,}"#, "\"a\nb\"",
                    #""\x""#, #""\u12g4""#, "tru", "nul", "'a'", "{1:2}"] {
            XCTAssertThrowsError(try JSON.parse(bad), bad)
        }
        XCTAssertEqual(JSON.parseObject("not json").count, 0)
        XCTAssertEqual(JSON.parseObject("[1]").count, 0)
    }

    func testDeepNestingFailsInsteadOfCrashing() {
        let text = String(repeating: "[", count: 100_000)
        XCTAssertThrowsError(try JSON.parse(text))
    }

    func testLenientAccessors() {
        let map = JSON.parseObject(#"{"s":5,"n":"42","b":"true","l":[{"x":1},2],"d":2.0,"z":null}"#)
        XCTAssertEqual(map.str("s"), "5")
        XCTAssertEqual(map.str("missing"), "")
        XCTAssertEqual(map.int64("n"), 42)
        XCTAssertFalse(map.bool("b"), "only a real true is true")
        XCTAssertEqual(map.maps("l").count, 1)
        XCTAssertEqual(map.str("d"), "2.0")
        XCTAssertEqual(map.firstStr("missing", "z", "s"), "5")
        XCTAssertEqual(map.str("z", "fallback"), "fallback")
        XCTAssertEqual(map.str("s", "fallback"), "5")
        XCTAssertEqual(map.int("d"), 2)
        XCTAssertEqual(map.int("missing", 7), 7)
        XCTAssertEqual(map.double("n"), 42.0)
        XCTAssertEqual(map.strings("l"), [#"{"x":1}"#, "2"])
        XCTAssertNil(map.optBool("b"))
        XCTAssertNil(map.optStr("s"))
    }

    func testStrictAccessorsAndSubscripts() throws {
        let value = try JSON.parse(#"{"a":{"b":[10,"x",2.9]},"t":true}"#)
        XCTAssertEqual(value["a"]?["b"]?[0]?.intValue, 10)
        XCTAssertEqual(value["a"]?["b"]?[1]?.stringValue, "x")
        XCTAssertEqual(value["a"]?["b"]?[2]?.intValue, 2)
        XCTAssertEqual(value["a"]?["b"]?[2]?.doubleValue, 2.9)
        XCTAssertNil(value["a"]?["b"]?[3])
        XCTAssertNil(value["a"]?["b"]?[-1])
        XCTAssertNil(value["a"]?["b"]?[1]?.intValue)
        XCTAssertEqual(value["t"]?.boolValue, true)
        XCTAssertNil(value["missing"])
        XCTAssertNil(value["t"]?["x"])
        XCTAssertEqual(value.obj("a").list("b").count, 3)
        XCTAssertEqual(value["a"]?["b"]?[1]?.int(or: 4), 4)
        XCTAssertEqual(JSONValue.string(" 12 ").int(or: 0), 12)
        XCTAssertEqual(JSONValue.string("3.7").int(or: 0), 3)
        XCTAssertEqual(JSONValue.null.string(or: "x"), "x")
    }

    func testStringEscapesMatchDart() {
        // Control characters: short forms where JSON has them, otherwise
        // \u00xx in lower-case hex. "/" and DEL and non-ASCII are left alone.
        let text = "\u{08}\u{0C}\n\r\t\u{00}\u{1f}\u{1B}/\u{7f}\\\"é漢😀"
        XCTAssertEqual(
            JSON.encode(.string(text)),
            "\"\\b\\f\\n\\r\\t\\u0000\\u001f\\u001b/\u{7f}\\\\\\\"é漢😀\""
        )
        XCTAssertEqual(try JSON.parse(JSON.encode(.string(text))), .string(text))
    }

    func testParsesEscapes() throws {
        XCTAssertEqual(try JSON.parse(#""\u00e9\/\b\f\n\r\t\"\\""#), "é/\u{08}\u{0C}\n\r\t\"\\")
        XCTAssertEqual(try JSON.parse(#""\uD83D\uDE00""#), "😀")
        XCTAssertEqual(try JSON.parse(#""\ud83d\ude00!""#), "😀!")
        // A Swift string cannot hold half a surrogate pair.
        XCTAssertEqual(try JSON.parse(#""\uD83Dx""#), "\u{FFFD}x")
        XCTAssertEqual(try JSON.parse(#""\uD83D\u0041""#), "\u{FFFD}A")
        XCTAssertEqual(try JSON.parse(#""\uDE00""#), "\u{FFFD}")
    }

    func testDoubleFormattingMatchesDart() {
        // Expected strings are what `dart` prints for `print(x)` / jsonEncode(x).
        let cases: [(Double, String)] = [
            (0.0, "0.0"), (-0.0, "-0.0"), (1.0, "1.0"), (-1.0, "-1.0"), (1.5, "1.5"), (-2.5, "-2.5"),
            (0.1, "0.1"), (100.0, "100.0"), (123456789.0, "123456789.0"), (0.1 + 0.2, "0.30000000000000004"),
            (1e20, "100000000000000000000.0"), (1e21, "1e+21"), (1.5e21, "1.5e+21"), (1.2345e25, "1.2345e+25"),
            (123456789012345680000.0, "123456789012345680000.0"),
            (0.000001, "0.000001"), (0.0000015, "0.0000015"), (1e-7, "1e-7"), (1.5e-7, "1.5e-7"),
            (-1.5e-7, "-1.5e-7"), (Double.leastNonzeroMagnitude, "5e-324"), (1.7976931348623157e308, "1.7976931348623157e+308"),
            (3.14159, "3.14159"), (1e16, "10000000000000000.0"), (9007199254740993.0, "9007199254740992.0"),
            (0.5, "0.5"), (12.5e-3, "0.0125"), (4.35, "4.35"), (1786636663.064, "1786636663.064"),
        ]
        for (number, expected) in cases {
            XCTAssertEqual(JSON.formatDouble(number), expected)
            XCTAssertEqual(JSON.encode(.double(number)), expected)
        }
        XCTAssertEqual(JSON.encode(.double(.nan)), "null")
        XCTAssertEqual(JSON.encode(.double(.infinity)), "null")
    }

    func testNumberFormsSurviveARoundTrip() throws {
        let text = #"{"i":1,"d":1.0,"e":1000.0,"neg":-0.5,"big":1e+21,"zero":0,"nz":-0.0}"#
        XCTAssertEqual(JSON.encode(try JSON.parse(text)), text)
        XCTAssertEqual(try JSON.parse("1e3"), .double(1000))
        XCTAssertEqual(try JSON.parse("1E+3"), .double(1000))
        XCTAssertEqual(try JSON.parse("0"), .int(0))
        XCTAssertEqual(try JSON.parse(" 12 "), .int(12))
    }

    func testKeyOrderIsInsertionOrder() throws {
        let text = #"{"z":1,"a":2,"m":{"y":1,"b":2},"0":3}"#
        XCTAssertEqual(JSON.encode(try JSON.parse(text)), text)

        var object = JSONObject()
        object["z"] = 1
        object["a"] = 2
        object["z"] = 9 // keeps its place
        XCTAssertEqual(JSON.encode(object), #"{"z":9,"a":2}"#)
        object["z"] = nil
        object["z"] = 1 // removed and re-added goes last
        XCTAssertEqual(JSON.encode(object), #"{"a":2,"z":1}"#)
        XCTAssertEqual(object.removeValue(forKey: "a"), 2)
        XCTAssertNil(object.removeValue(forKey: "a"))
        XCTAssertEqual(object.keys, ["z"])
        XCTAssertEqual(object.count, 1)
        XCTAssertFalse(object.isEmpty)
        object["n"] = .null
        XCTAssertTrue(object.contains("n"))
        XCTAssertEqual(JSON.encode(object), #"{"z":1,"n":null}"#)
    }

    func testDuplicateKeysKeepFirstPositionLastValue() throws {
        XCTAssertEqual(JSON.encode(try JSON.parse(#"{"a":1,"b":2,"a":3}"#)), #"{"a":3,"b":2}"#)
    }

    func testObjectIsASequenceAndEqualityIgnoresOrder() {
        let object: JSONObject = ["b": 1, "a": "x"]
        XCTAssertEqual(object.map(\.key), ["b", "a"])
        XCTAssertEqual(object.map(\.value), [1, "x"])
        XCTAssertEqual(object.values, [1, "x"])
        XCTAssertEqual(object, ["a": "x", "b": 1])
        XCTAssertNotEqual(object, ["a": "x", "b": 2])
        XCTAssertNotEqual(JSONValue.int(1), JSONValue.double(1))
        var merged: JSONObject = ["a": 0]
        merged.merge(object)
        XCTAssertEqual(merged.keys, ["a", "b"])
        XCTAssertEqual(merged["a"], "x")
    }

    func testSubscriptSetterOnValue() {
        var value: JSONValue = ["a": 1]
        value["b"] = "x"
        value["a"] = nil
        XCTAssertEqual(JSON.encode(value), #"{"b":"x"}"#)
    }

    func testWrappingInitialisers() {
        let none: String? = nil
        let object: JSONObject = [
            "s": JSONValue("x"), "n": JSONValue(none), "i": JSONValue(3), "d": JSONValue(2.0),
            "b": JSONValue(false), "l": JSONValue(["a", "b"]),
        ]
        XCTAssertEqual(JSON.encode(object), #"{"s":"x","n":null,"i":3,"d":2.0,"b":false,"l":["a","b"]}"#)
    }

    func testPrettyMatchesDartWithIndent() {
        let value: JSONValue = ["a": 1, "b": [true, ["c": nil]], "e": [], "o": [:]]
        XCTAssertEqual(JSON.encodePretty(value), """
        {
          "a": 1,
          "b": [
            true,
            {
              "c": null
            }
          ],
          "e": [],
          "o": {}
        }
        """)
    }

    func testParsesData() throws {
        XCTAssertEqual(try JSON.parse(Data(#"{"a":"é"}"#.utf8)), ["a": "é"])
        XCTAssertEqual(try JSON.parse(Data([0xEF, 0xBB, 0xBF] + Array("[1]".utf8))), [1])
        XCTAssertEqual(JSON.encodeData(["a": "é"]), Data(#"{"a":"é"}"#.utf8))
        // Invalid UTF-8 inside a string is replaced, not fatal.
        XCTAssertEqual(try JSON.parse(Data([0x22, 0xFF, 0x22])), "\u{FFFD}")
    }

    func testWhitespaceIsAccepted() throws {
        XCTAssertEqual(try JSON.parse(" \n\t{ \"a\" : [ 1 , 2 ] , \"b\" : { } }\r\n"), ["a": [1, 2], "b": [:]])
    }
}
