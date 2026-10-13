import FMBase
import XCTest

@testable import FMCoves

@MainActor
private final class FakeSecretsChannel: SecretsControlCalling {
    var calls: [(String, JSONObject)] = []
    var answers: [String: JSONObject] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        calls.append((method, params))
        return answers[method] ?? [:]
    }
}

final class ActionsSecretsTests: XCTestCase {
    func testNameValidation() {
        XCTAssertTrue(ActionsSecrets.validName("CLOUDFLARE_API_TOKEN"))
        XCTAssertTrue(ActionsSecrets.validName("_x9"))
        XCTAssertFalse(ActionsSecrets.validName("9x"))
        XCTAssertFalse(ActionsSecrets.validName("A-B"))
        XCTAssertFalse(ActionsSecrets.validName(""))
        XCTAssertFalse(ActionsSecrets.validName(String(repeating: "A", count: 129)))
        XCTAssertFalse(ActionsSecrets.validName("ÄB"))
    }

    func testRowsNeverCarryValuesAndAreBounded() {
        let answer: JSONObject = [
            "variables": .array([
                .object(["name": .string("b_token"), "value": .string("leak")]),
                .object(["name": .string("A"), "updatedAtMs": .int(5)]),
                .object(["name": .string("bad name")]),
                .object(["name": .string("A")]),
            ])
        ]
        let rows = ActionsSecrets.rows(answer)
        XCTAssertEqual(rows, [ActionsSecretRow(name: "A", updatedAtMs: 5), ActionsSecretRow(name: "b_token")])
    }

    @MainActor func testSaveAsksForAConfirmationThenWrites() async {
        let channel = FakeSecretsChannel()
        channel.answers["secrets.prepare"] = ["token": .string("t1"), "summary": .string("Set TOKEN")]
        channel.answers["secrets.list"] = ["variables": .array([.object(["name": .string("TOKEN")])])]
        let model = ActionsSecretsModel(client: ActionsSecretsClient(channel: channel))
        let saved = await model.save(name: " TOKEN ", value: "s3cret")
        XCTAssertTrue(saved)
        XCTAssertEqual(channel.calls.map(\.0), ["secrets.prepare", "secrets.set", "secrets.list"])
        XCTAssertEqual(channel.calls[0].1["action"]?.stringValue, "set")
        XCTAssertNil(channel.calls[0].1["value"])
        XCTAssertEqual(channel.calls[1].1["token"]?.stringValue, "t1")
        XCTAssertEqual(channel.calls[1].1["value"]?.stringValue, "s3cret")
        XCTAssertEqual(model.rows.map(\.name), ["TOKEN"])

        let deleted = await model.delete(name: "TOKEN")
        XCTAssertTrue(deleted)
        XCTAssertEqual(channel.calls[3].1["action"]?.stringValue, "delete")
        XCTAssertEqual(channel.calls[4].0, "secrets.delete")
    }

    @MainActor func testRefusalsAreReported() async {
        let channel = FakeSecretsChannel()
        channel.answers["secrets.prepare"] = ["token": .string("t")]
        channel.answers["secrets.set"] = ["error": .string("confirmation_expired")]
        let model = ActionsSecretsModel(client: ActionsSecretsClient(channel: channel))
        let saved = await model.save(name: "OK", value: "v")
        XCTAssertFalse(saved)
        XCTAssertEqual(model.error, "The confirmation expired. Try again.")
        let invalid = await model.save(name: "no good", value: "v")
        XCTAssertFalse(invalid)
        XCTAssertEqual(channel.calls.count, 2)
        let big = await model.save(name: "BIG", value: String(repeating: "x", count: 16 * 1024 + 1))
        XCTAssertFalse(big)
        XCTAssertEqual(model.error, "Values are limited to 16 KB.")
    }
}
