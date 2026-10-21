import XCTest
@testable import FMBase

final class HiddenUnicodeTests: XCTestCase {
    private func vectors() throws -> JSONObject {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { root.deleteLastPathComponent() }
        let data = try Data(contentsOf: root.appendingPathComponent("conformance/hidden-unicode/v1/vectors.json"))
        return try XCTUnwrap(try JSON.parse(data).objectValue)
    }

    func testEveryVector() throws {
        let suite = try vectors()
        XCTAssertEqual(suite["policy"]?.stringValue, HiddenUnicode.policy)
        let cases = suite["cases"]?.arrayValue ?? []
        XCTAssertGreaterThanOrEqual(cases.count, 40)
        for item in cases {
            let vector = try XCTUnwrap(item.objectValue)
            let id = vector["id"]?.stringValue ?? "?"
            let input = vector["input"]?.stringValue ?? ""
            let output = vector["output"]?.stringValue ?? ""
            let cleaned = HiddenUnicode.clean(input)
            XCTAssertEqual(Array(cleaned.unicodeScalars), Array(output.unicodeScalars), id)
            XCTAssertEqual(HiddenUnicode.hasHidden(input), vector["hidden"]?.boolValue ?? false, id)
            XCTAssertEqual(HiddenUnicode.clean(cleaned), cleaned, id)
        }
    }
}
