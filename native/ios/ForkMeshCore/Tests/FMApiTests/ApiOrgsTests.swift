// The org ApiService cases, as in native/android OrgApiTest.kt. FakeHTTPClient records the
// method, path and body. The model and orgErrorMessage cases are in
// ModelsTests.swift.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiOrgsTests: XCTestCase {
    private func orgApi(_ http: FakeHTTPClient) -> ApiService {
        ApiPortFixture.api(http, token: "tok", serverUrl: "ws://test.local/ws")
    }

    func testMyOrgsUnwrapsTheOrgsArray() async throws {
        let http = FakeHTTPClient { request in
            XCTAssertEqual(request.method, "GET")
            XCTAssertEqual(portRawPath(request), "/api/orgs")
            return portJSON([
                "ok": true,
                "orgs": [
                    ["name": "acme", "role": "owner"],
                    ["name": "globex", "role": "member"],
                    ["role": "x"],
                ],
            ])
        }

        let orgs = try await orgApi(http).myOrgs()

        XCTAssertEqual(orgs.map(\.name), ["acme", "globex"])
        XCTAssertEqual(orgs.first?.canManage, true)
        XCTAssertEqual(orgs.last?.canManage, false)
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(request.headers["Authorization"], "Bearer tok")
        XCTAssertNil(request.headers["Content-Type"])
    }

    func testCreateOrgPostsThePayloadAndReturnsTheNewOrg() async throws {
        let http = FakeHTTPClient { request in
            XCTAssertEqual(request.method, "POST")
            XCTAssertEqual(portRawPath(request), "/api/orgs")
            return portJSON(["ok": true, "org": "acme", "role": "owner"], 201)
        }

        let created = try await orgApi(http).createOrg(name: "ACME", displayName: "Acme Inc", description: "Things")

        XCTAssertEqual(portBody(http.lastRequest), ["name": "acme", "displayName": "Acme Inc", "description": "Things"])
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"name":"acme","displayName":"Acme Inc","description":"Things"}"#)
        XCTAssertEqual(created.name, "acme")
        XCTAssertEqual(created.role, "owner")

        // Kotlin: a reply without the org falls back to the name as typed.
        let fallback = try await orgApi(FakeHTTPClient { _ in portJSON(["ok": true], 201) }).createOrg(name: "Beta ")
        XCTAssertEqual(fallback.name, "Beta ")
        XCTAssertEqual(fallback.role, "owner")
    }

    func testCreateOrgSurfacesTheWorkerErrorCodeAsAMessage() async {
        let http = FakeHTTPClient { _ in portJSON(["error": "org_name_taken"], 409) }

        let error = await portCaughtError { try await self.orgApi(http).createOrg(name: "taken") }
        let refusal = error as? OrgApiException
        XCTAssertNotNil(refusal, "\(String(describing: error))")
        XCTAssertEqual(refusal?.code, "org_name_taken")
        XCTAssertEqual(refusal?.message, "That name is already taken.")
        XCTAssertEqual(refusal?.description, "That name is already taken.")

        // Kotlin: a body with no code reads as the status.
        let bare = await portCaughtError {
            try await self.orgApi(FakeHTTPClient { _ in HTTPResponse(status: 500, text: "oops") }).myOrgs()
        } as? OrgApiException
        XCTAssertEqual(bare?.code, "http_500")
        XCTAssertEqual(bare?.message, "Request failed.")
    }

    func testMemberTeamAndRepoWritesUseTheRightMethodPathAndBody() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path.hasSuffix("/members") && request.method == "GET" {
                return portJSON([
                    "ok": true,
                    "members": [
                        ["name": "alice", "role": "owner", "since": 1],
                        ["name": "bob", "role": "member", "since": 2],
                    ],
                ])
            }
            if path.hasSuffix("/teams") && request.method == "GET" {
                return portJSON(["teams": [["team": "Core", "permission": "write", "members": 2]]])
            }
            if path.hasSuffix("/repos") && request.method == "GET" {
                return portJSON(["repos": [["repo": "Widget", "node": "alice"]]])
            }
            if request.method == "GET" {
                return portJSON(["org": "acme", "displayName": "Acme", "viewerRole": "owner"])
            }
            return portJSON(["ok": true])
        }
        let api = orgApi(http)

        let members = try await api.orgMembers("acme")
        XCTAssertEqual(members.map(\.name), ["alice", "bob"])

        try await api.setOrgMember("acme", "Carol", role: "admin")
        try await api.removeOrgMember("acme", "Bob")
        try await api.setOrgTeam("acme", "Core", permission: "write")
        try await api.deleteOrgTeam("acme", "Core")
        try await api.linkOrgRepo("acme", "Widget", node: "Alice")
        try await api.unlinkOrgRepo("acme", "Widget")
        try await api.addOrgTeamMember("acme", "core", "Alice")
        try await api.removeOrgTeamMember("acme", "core", "Alice")
        try await api.deleteOrg("acme")

        // Skip the initial GET /members; assert the writes.
        let writes = http.requests.filter { $0.method != "GET" }
        XCTAssertEqual(writes.count, 9)
        func expectWrite(_ i: Int, _ method: String, _ path: String, _ body: JSONObject, line: UInt = #line) {
            guard i < writes.count else { return XCTFail("no write \(i)", line: line) }
            XCTAssertEqual(writes[i].method, method, "write \(i)", line: line)
            XCTAssertEqual(portRawPath(writes[i]), path, "write \(i)", line: line)
            XCTAssertEqual(portBody(writes[i]), body, "write \(i)", line: line)
            XCTAssertEqual(writes[i].headers["Content-Type"], "application/json", "write \(i)", line: line)
        }
        expectWrite(0, "POST", "/api/orgs/acme/members", ["member": "carol", "role": "admin"])
        expectWrite(1, "DELETE", "/api/orgs/acme/members", ["member": "bob"])
        expectWrite(2, "POST", "/api/orgs/acme/teams", ["team": "core", "permission": "write"])
        expectWrite(3, "DELETE", "/api/orgs/acme/teams", ["team": "core"])
        expectWrite(4, "POST", "/api/orgs/acme/repos", ["repo": "widget", "node": "alice"])
        expectWrite(5, "DELETE", "/api/orgs/acme/repos", ["repo": "widget"])
        expectWrite(6, "POST", "/api/orgs/acme/teams/core/members", ["member": "alice"])
        expectWrite(7, "DELETE", "/api/orgs/acme/teams/core/members", ["member": "alice"])
        expectWrite(8, "DELETE", "/api/orgs/acme", [:])
        XCTAssertEqual(writes.last?.bodyText, "{}")

        // Kotlin: the reads.
        let teams = try await api.orgTeams("ACME")
        XCTAssertEqual(teams.map(\.team), ["core"])
        XCTAssertEqual(http.lastRequest.map(portRawPath), "/api/orgs/acme/teams")
        let repos = try await api.orgRepos("acme")
        XCTAssertEqual(repos.map(\.repo), ["widget"])
        let profile = try await api.orgProfile("acme")
        XCTAssertEqual(profile.title, "Acme")
        _ = try await api.orgTeamMembers("A B", "c")
        XCTAssertEqual(http.lastRequest.map(portRawPath), "/api/orgs/a%20b/teams/c/members")
    }
}
