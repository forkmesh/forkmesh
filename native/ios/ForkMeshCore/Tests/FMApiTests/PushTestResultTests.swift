// PushTestResult.summary names the broken link.

import FMBase
import Foundation
import XCTest
@testable import FMApi

final class PushTestResultTests: XCTestCase {
    private func summary(_ status: Int, _ json: JSONObject) -> String {
        PushTestResult(statusCode: status, json: json).summary
    }

    private func assertContains(_ text: String, _ part: String, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertTrue(text.contains(part), "\"\(text)\" does not contain \"\(part)\"", file: file, line: line)
    }

    func testRelayUnreachableOrTooOld() {
        assertContains(PushTestResult.unreachable.summary, "Could not reach the relay")
        assertContains(summary(404, [:]), "does not support push tests yet")
        assertContains(summary(429, ["error": "too_many_requests"]), "Try again in a few seconds")
    }

    func testRelayWithoutCredentialsOrWithAKeyThatWillNotSign() {
        assertContains(
            summary(200, ["ok": true, "configured": false, "devices": 1, "delivered": 0, "results": []]),
            "no APNs credentials")
        assertContains(
            summary(502, ["ok": false, "configured": true, "error": "apns_provider_key"]),
            "APNs key is invalid")
    }

    func testRelayWhoseAppleIdentifierHasTheWrongShape() {
        // Apple refuses every push from such a relay, so "registered" and "credentials present"
        // would both look healthy without this.
        let text = summary(200, [
            "ok": true, "configured": false, "configProblem": "key_id_malformed", "devices": 1, "delivered": 0,
            "results": [],
        ])
        assertContains(text, "APNS_KEY_ID")
        assertContains(text, "ten characters")
        assertContains(summary(200, ["configured": false, "configProblem": "team_id_malformed"]), "APNS_TEAM_ID")
    }

    func testNoDeviceRowThenAppleRefusalsWithAHint() {
        assertContains(
            summary(200, ["ok": true, "configured": true, "devices": 0, "delivered": 0, "results": []]),
            "not registered for pushes yet")
        let rejected = summary(200, [
            "ok": true, "configured": true, "devices": 1, "delivered": 0,
            "results": [["environment": "production", "delivered": false, "status": 400, "reason": "BadDeviceToken"]],
        ])
        assertContains(rejected, "Apple rejected the push (BadDeviceToken)")
        // The one mistake that made every Simulator install silent: the token belongs to the
        // other gateway.
        assertContains(rejected, "other APNs environment")
        assertContains(
            summary(200, [
                "ok": true, "configured": true, "devices": 1, "delivered": 0,
                "results": [["environment": "development", "delivered": false, "status": 0, "reason": ""]],
            ]),
            "could not reach Apple's gateway")
    }
}
