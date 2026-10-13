// Sync status and trust lists (Kotlin: api/SyncStatusTest.kt, security/TrustListsCryptoTest.kt):
// parity with the shared fixtures in conformance/ that the Worker, desktop, web and Kotlin
// implementations also pass.

import FMBase
import XCTest
@testable import FMAccount

final class SyncStatusTests: XCTestCase {
    var fixture: JSONObject!
    var privateStrings: [String] = []

    override func setUpWithError() throws {
        fixture = try loadConformance("conformance/sync-status/v1/fixture.json")
        privateStrings = fixture.strings("privateStrings")
    }

    func expectCase(_ id: String, _ got: JSONObject, _ want: JSONValue?, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(JSONValue.object(got), want, id, file: file, line: line)
        let text = JSON.encode(got)
        for secret in privateStrings {
            XCTAssertFalse(text.contains(secret), "\(id) leaks \(secret)", file: file, line: line)
        }
    }

    func testEvidenceCasesMatchTheSharedFixture() {
        let cases = fixture.maps("cases")
        XCTAssertFalse(cases.isEmpty)
        for entry in cases {
            expectCase(entry.str("id"), SyncStatus.evaluate(entry.obj("evidence")), entry["expect"])
        }
    }

    func testTransitionSequencesNeverReportFalseSuccess() {
        let sequences = fixture.maps("sequences")
        XCTAssertFalse(sequences.isEmpty)
        for sequence in sequences {
            for (i, step) in sequence.maps("steps").enumerated() {
                expectCase("\(sequence.str("id"))#\(i)", SyncStatus.evaluate(step.obj("evidence")), step["expect"])
            }
        }
    }

    func testMirrorsPayloadsMatchTheSharedFixture() {
        let cases = fixture.maps("mirrorsPayloadCases")
        XCTAssertFalse(cases.isEmpty)
        for entry in cases {
            expectCase(entry.str("id"), SyncStatus.mirrorsPayloadStatus(entry.obj("payload")), entry["expect"])
        }
    }
}

extension SyncStatusTests {
    func testOutputKeepsTheDartKeyOrder() {
        let status = SyncStatus.evaluate(["now": 0])
        XCTAssertEqual(status.keys, [
            "schema", "state", "label", "severity", "current", "totalCopies", "nodesReached", "verifiedCopies",
            "trustedMirrors", "lastConfirmedAt", "lagMs", "error", "action", "summary",
        ])
        XCTAssertEqual(status["state"], "stale")
        XCTAssertEqual(status["error"], "unconfirmed")
        XCTAssertEqual(status["lastConfirmedAt"], .null)
    }
}

final class TrustListsTests: XCTestCase {
    var vectors: JSONObject!

    override func setUpWithError() throws {
        vectors = try loadConformance("conformance/trust-lists/v1/vectors.json")
    }

    var cases: [JSONObject] { vectors.maps("cases") }
    var testKeys: JSONObject { vectors.obj("testKeys") }

    func resolve(_ vector: JSONObject, reversed: Bool = false) -> JSONObject {
        let input = vector.obj("input")
        func ordered<T>(_ values: [T]) -> [T] { reversed ? values.reversed() : values }
        return TrustLists.resolve(
            ordered(input.strings("documents")),
            now: input.int64("now"),
            follows: ordered(input.maps("follows")),
            overrides: ordered(input.maps("overrides")))
    }

    func caseNamed(_ id: String) -> JSONObject { cases.first { $0.str("id") == id } ?? [:] }

    /// The first difference between two JSON values, for a readable failure.
    func firstDifference(_ a: JSONValue?, _ b: JSONValue?, path: String = "$") -> String? {
        switch (a, b) {
        case (.object(let x)?, .object(let y)?):
            for key in Set(x.keys).union(y.keys).sorted() {
                if let diff = firstDifference(x[key], y[key], path: "\(path).\(key)") { return diff }
            }
            return nil
        case (.array(let x)?, .array(let y)?):
            if x.count != y.count { return "\(path): \(x.count) items vs \(y.count)" }
            for i in 0..<x.count {
                if let diff = firstDifference(x[i], y[i], path: "\(path)[\(i)]") { return diff }
            }
            return nil
        default:
            return a == b ? nil : "\(path): expected \(a.map { JSON.encode($0) } ?? "missing"), got \(b.map { JSON.encode($0) } ?? "missing")"
        }
    }

    func checkAll(reversed: Bool) {
        XCTAssertEqual(vectors.str("schema"), "forkmesh-trust-list-vectors-v1")
        XCTAssertEqual(cases.count, 24)
        for vector in cases {
            let got = resolve(vector, reversed: reversed)
            if let diff = firstDifference(vector["expect"], .object(got)) {
                XCTFail("\(vector.str("id")) (reversed: \(reversed)): \(diff)")
            }
        }
    }

    func testEveryCaseMatchesTheSharedResolution() { checkAll(reversed: false) }

    func testEveryCaseDoesNotDependOnInputOrder() { checkAll(reversed: true) }

    func testCompromiseDrillFlipsTheListFromActiveToRecovered() throws {
        let alice = testKeys.str("alice")
        let malicious: Set<String> = [testKeys.str("mallory"), testKeys.str("maintainer")]
        func aliceRow(_ resolution: JSONObject) -> JSONObject {
            resolution.maps("lists").first { $0.str("issuer") == alice && $0.str("name") == "security" } ?? [:]
        }
        func subjects(_ resolution: JSONObject) -> Set<String> {
            Set(resolution.maps("assertions").map { $0.str("subject") })
        }

        let before = resolve(caseNamed("compromise-drill-before-revocation"))
        XCTAssertEqual(aliceRow(before)["state"], "active")
        XCTAssertEqual(aliceRow(before)["headVersion"], 3)
        XCTAssertTrue(malicious.isSubset(of: subjects(before)))

        let after = resolve(caseNamed("compromise-drill-revoked-and-recovered"))
        let row = aliceRow(after)
        XCTAssertEqual(row["state"], "recovered")
        XCTAssertEqual(row["headVersion"], 2)
        XCTAssertEqual(row["cutoffVersion"], 3)
        XCTAssertTrue(row.strings("cutoffReasons").contains("revoked"))
        XCTAssertEqual(row["successor"], .object(["issuer": .string(testKeys.str("alice-successor")), "name": "security"]))
        XCTAssertTrue(subjects(after).isDisjoint(with: malicious))
        XCTAssertEqual(after.obj("telemetry")["revocations"], 1)
    }

    func testVerifyDocumentReportsStableCodes() {
        func codeOf(_ raw: String) -> String? {
            do {
                _ = try TrustLists.verifyDocument(raw)
                return nil
            } catch let error as TrustListError {
                return error.code
            } catch {
                return "unexpected \(error)"
            }
        }
        XCTAssertEqual(codeOf("{"), "invalid_json")
        XCTAssertEqual(codeOf("[]"), "unsupported_schema")
        XCTAssertEqual(codeOf("{\"a\":1.5}"), "noncanonical")
        XCTAssertEqual(codeOf("{\"a\":9007199254740992}"), "noncanonical")
        XCTAssertEqual(codeOf("{ \"a\":1}"), "noncanonical")
        XCTAssertEqual(codeOf("{\"schema\":\"forkmesh-trust-list-v1\"}"), "invalid_keys")
        XCTAssertEqual(codeOf("[[[[[[[[[1]]]]]]]]]"), "invalid_json")
        XCTAssertEqual(codeOf("{\"b\":1,\"a\":2}"), "noncanonical")
        XCTAssertEqual(try TrustLists.canonicalJson(JSONValue.object(["b": 1, "a": nil])), "{\"a\":null,\"b\":1}")
        // Ids hash Python `surrogatepass` bytes (values from app/src/trust_lists.py).
        XCTAssertEqual(
            TrustLists.documentId(utf16: [0x61, 0xD800, 0x62]),
            "sha256:45e334b6c74ca5db8d8f8fcd1157fb31a2ebc9953e0d8d182e37b6b67e6a1705")
        XCTAssertEqual(
            TrustLists.documentId("x\u{1F600}y"),
            "sha256:d506f8eb09b320da26e02670fb6ce1685d67541cb373a98c00adbffadef794f4")
    }
}
