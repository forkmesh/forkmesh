import FMAccount
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

final class AccountEndpointAdapterTests: XCTestCase {
    @MainActor
    func testWritesCarryTheServerOriginAndReadsDoNot() async throws {
        let http = apiRouteByPath { _, _, _ in apiJSON(["ok": true, "keys": []]) }
        let api = makeTestApi(http, token: "session-token", url: "wss://app.forkmesh.com/ws")
        XCTAssertEqual(api.serverOrigin, "https://app.forkmesh.com")
        _ = try await api.accountCall("GET", "/api/accounts/ssh-keys", nil)
        _ = try await api.accountCall("DELETE", "/api/orgs/acme/bot-tokens", ["tokenId": "x"])
        let requests = http.requests
        XCTAssertNil(requests[0].headers["Origin"])
        XCTAssertNil(requests[0].body)
        XCTAssertEqual(requests[1].headers["Origin"], "https://app.forkmesh.com")
        XCTAssertEqual(requests[1].headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(JSON.parseObject(requests[1].body ?? Data()).str("tokenId"), "x")
    }

    @MainActor
    func testErrorsCarryTheServerCodeAndStatus() async {
        let http = apiRouteByPath { path, _, _ in
            path.hasSuffix("ssh-keys") ? apiJSON(["error": "ssh_key_limit_reached"], status: 429) : nil
        }
        let api = makeTestApi(http, token: "t")
        do {
            _ = try await api.accountCall("POST", "/api/accounts/ssh-keys", [:])
            XCTFail("no error")
        } catch let error as AccountEndpointError {
            XCTAssertEqual(error, AccountEndpointError("ssh_key_limit_reached", status: 429))
        } catch {
            XCTFail("\(error)")
        }
        do {
            _ = try await api.accountCall("GET", "/api/accounts/export", nil)
            XCTFail("no error")
        } catch let error as AccountEndpointError {
            XCTAssertEqual(error.status, 404)
        } catch {
            XCTFail("\(error)")
        }
    }

    @MainActor
    func testRefusesPathsOutsideTheApi() async {
        let http = apiRouteByPath { _, _, _ in apiJSON(["ok": true]) }
        let api = makeTestApi(http)
        do {
            _ = try await api.accountCall("GET", "https://evil.example/api", nil)
            XCTFail("no error")
        } catch {
            XCTAssertTrue(http.requests.isEmpty)
        }
    }
}
