// The base64 / utf8 acceptance rules (RFC 4648's vectors plus the decoder's edge cases), hex
// and `dartTrimmed`.

import XCTest
@testable import FMBase

final class EncodingTests: XCTestCase {
    private func bytes(_ text: String) -> [UInt8] { Array(text.utf8) }

    func testBase64Rfc4648Vectors() throws {
        let vectors = [("", ""), ("f", "Zg=="), ("fo", "Zm8="), ("foo", "Zm9v"), ("foob", "Zm9vYg=="),
                       ("fooba", "Zm9vYmE="), ("foobar", "Zm9vYmFy")]
        for (plain, encoded) in vectors {
            XCTAssertEqual(B64.encode(bytes(plain)), encoded)
            XCTAssertEqual(try B64.decode(encoded), Data(plain.utf8))
        }
    }

    func testUrlSafeAlphabetAndPadding() throws {
        let raw: [UInt8] = [0xFB, 0xFF, 0xFE, 0x3E]
        XCTAssertEqual(B64.encode(raw), "+//+Pg==")
        XCTAssertEqual(B64.encodeUrl(raw), "-__-Pg==")
        XCTAssertEqual(B64.encodeUrlNoPadding(raw), "-__-Pg")
        XCTAssertEqual(B64.encodeUrlNoPadding(Data(raw)), "-__-Pg")
        // Either alphabet decodes through either codec.
        XCTAssertEqual(try B64.decode("+//+Pg=="), Data(raw))
        XCTAssertEqual(try B64.decode("-__-Pg=="), Data(raw))
        XCTAssertEqual(try B64.decode("-_/+Pg=="), Data([0xFB, 0xFF, 0xFE, 0x3E]))
    }

    func testDecodeFollowsDartAcceptanceRules() throws {
        // Percent-encoded padding is accepted, in either case.
        XCTAssertEqual(try B64.decode("Zg%3D%3D"), Data("f".utf8))
        XCTAssertEqual(try B64.decode("Zg%3d%3d"), Data("f".utf8))
        XCTAssertEqual(try B64.decode("Zg=%3D"), Data("f".utf8))
        XCTAssertEqual(try B64.decode("Zm8%3D"), Data("fo".utf8))
        for bad in ["Zg", "Zg=", "Zm8", "Z", "Zh==", "Zm9=", "Zg==Zg==", "Zg== ", " Zg==", "Zm9v\n", "Zm9v=",
                    "====", "Zg%3D", "Zg%3D%3", "Zé==", "Z===", "Zg=a"] {
            XCTAssertThrowsError(try B64.decode(bad), bad)
            XCTAssertNil(B64.decodeOrNil(bad))
        }
    }

    func testDecodeUnpaddedRestoresPadding() throws {
        XCTAssertEqual(try B64.decodeUnpadded("-__-Pg"), Data([0xFB, 0xFF, 0xFE, 0x3E]))
        XCTAssertEqual(try B64.decodeUnpadded("Zm8"), Data("fo".utf8))
        XCTAssertEqual(try B64.decodeUnpadded("Zm8="), Data("fo".utf8))
        XCTAssertEqual(try B64.decodeUnpadded("Zm9v"), Data("foo".utf8))
        XCTAssertEqual(try B64.decodeUnpadded(""), Data())
        XCTAssertThrowsError(try B64.decodeUnpadded("Z"))
        XCTAssertThrowsError(try B64.decodeUnpadded("Zh"))
    }

    func testBase64RoundTripsEveryLength() throws {
        for length in 0..<70 {
            let raw = (0..<length).map { UInt8(truncatingIfNeeded: $0 &* 37 &+ 11) }
            XCTAssertEqual(try B64.decode(B64.encode(raw)), Data(raw))
            XCTAssertEqual(try B64.decodeUnpadded(B64.encodeUrlNoPadding(raw)), Data(raw))
            // Foundation agrees on the encoding side.
            XCTAssertEqual(B64.encode(raw), Data(raw).base64EncodedString())
        }
    }

    func testHex() throws {
        XCTAssertEqual(Hex.encode([0x00, 0x0F, 0xA5, 0xFF]), "000fa5ff")
        XCTAssertEqual(try Hex.decode("000fa5ff"), Data([0x00, 0x0F, 0xA5, 0xFF]))
        XCTAssertEqual(try Hex.decode("000FA5Ff"), Data([0x00, 0x0F, 0xA5, 0xFF]))
        XCTAssertEqual(try Hex.decode(""), Data())
        XCTAssertThrowsError(try Hex.decode("abc"))
        XCTAssertThrowsError(try Hex.decode("zz"))
        XCTAssertNil(Hex.decodeOrNil("+1"))
    }

    func testUtf8DecodeIsStrict() throws {
        XCTAssertEqual(try Utf8.decode(Data("héllo 漢 😀".utf8)), "héllo 漢 😀")
        XCTAssertEqual(try Utf8.decode([0x61, 0x00, 0x62]), "a\u{0}b")
        XCTAssertEqual(try Utf8.decode([0xEF, 0xBB, 0xBF, 0x61]), "a")
        XCTAssertThrowsError(try Utf8.decode([0xFF]))
        XCTAssertThrowsError(try Utf8.decode([0xC3]))
        XCTAssertThrowsError(try Utf8.decode([0xED, 0xA0, 0x80])) // an encoded surrogate
        XCTAssertThrowsError(try Utf8.decode([0xC0, 0xAF])) // overlong
        XCTAssertEqual(Utf8.decodeLossy([0x61, 0xFF, 0x62]), "a\u{FFFD}b")
        XCTAssertEqual(Utf8.encode("é"), Data([0xC3, 0xA9]))
    }

    func testBytesHelpers() {
        XCTAssertEqual(Bytes.random(32).count, 32)
        XCTAssertNotEqual(Bytes.random(32), Bytes.random(32))
        XCTAssertEqual(Bytes.random(0), Data())
        XCTAssertEqual(Bytes.concat(Data([1]), Data(), Data([2, 3])), Data([1, 2, 3]))
        XCTAssertTrue(Bytes.constantTimeEquals([1, 2, 3], Data([1, 2, 3])))
        XCTAssertFalse(Bytes.constantTimeEquals([1, 2, 3], [1, 2, 4]))
        XCTAssertFalse(Bytes.constantTimeEquals([1, 2, 3], [1, 2]))
        XCTAssertTrue(Bytes.isAllZero([0, 0, 0]))
        XCTAssertFalse(Bytes.isAllZero([0, 1, 0]))
        XCTAssertTrue(Bytes.isAllZero([UInt8]()))
    }

    func testDartTrim() {
        XCTAssertEqual("  room \t\n".dartTrimmed, "room")
        XCTAssertEqual("\u{FEFF}room\u{FEFF}".dartTrimmed, "room")
        XCTAssertEqual("\u{85}\u{A0}\u{3000}room\u{2028}\u{2029}\u{205F}".dartTrimmed, "room")
        // Not trimmed: zero-width space and the C0 separators.
        XCTAssertEqual("\u{200B}room\u{200B}".dartTrimmed, "\u{200B}room\u{200B}")
        XCTAssertEqual("\u{1C}room\u{1F}".dartTrimmed, "\u{1C}room\u{1F}")
        XCTAssertEqual("a b".dartTrimmed, "a b")
        XCTAssertEqual("   ".dartTrimmed, "")
        XCTAssertEqual("".dartTrimmed, "")
        XCTAssertEqual("room\r\n".dartTrimmed, "room")
        XCTAssertEqual(" e\u{301} ".dartTrimmed, "e\u{301}")
        XCTAssertEqual("room".dartTrimmed, "room")
    }

    func testNumEquals() {
        XCTAssertTrue(JSONValue.int(1).numEquals(1))
        XCTAssertTrue(JSONValue.double(1.0).numEquals(1))
        XCTAssertFalse(JSONValue.double(1.5).numEquals(1))
        XCTAssertFalse(JSONValue.string("1").numEquals(1))
        XCTAssertFalse(JSONValue.null.numEquals(0))
    }
}
