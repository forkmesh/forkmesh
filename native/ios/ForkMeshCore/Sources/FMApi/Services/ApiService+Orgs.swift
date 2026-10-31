// ApiService, part 4: organizations (`/api/orgs/...`) and
// the account-scoped package APIs built on `accountApiCall` (vault, calories, notes, tasks).

import FMAccount
import FMBase
import FMNet
import Foundation

extension ApiService {
    /// `Uri.encodeComponent(value.trim().toLowerCase())`.
    nonisolated static func orgSegment(_ value: String) -> String {
        DartURI.encodeComponent(value.dartTrimmed.lowercased())
    }

    // MARK: Account-scoped package APIs

    /// Password Vault package API (`/api/vault/...`); every body is ciphertext made on this
    /// device. The same call as `accountApiCall`.
    public func vaultCall(_ method: String, _ path: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, path, body)
    }

    /// The signed-in account's Calorie Counter overview for one day (`GET /api/calories?day=`).
    public func caloriesOverview(_ day: String) async throws -> JSONObject {
        try await accountApiCall("GET", "/api/calories?day=\(day)")
    }

    /// A request under the account's Calorie Counter API, e.g. `caloriesApi("POST", "/entries", ...)`.
    /// Every route answers 409 `plugin_not_installed` until `installCalories` has run.
    public func caloriesApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/calories\(suffix)", body)
    }

    /// Installs Calorie Counter for the signed-in account. `tzOffsetMinutes` seeds the reminder
    /// time zone (JavaScript getTimezoneOffset convention).
    public func installCalories(tzOffsetMinutes: Int? = nil) async throws {
        var body = JSONObject()
        if let tzOffsetMinutes { body["tzOffsetMinutes"] = .int(Int64(tzOffsetMinutes)) }
        _ = try await accountApiCall("POST", "/api/calories/install", body)
    }

    public func stepsApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/steps\(suffix)", body)
    }

    public func waterApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/water\(suffix)", body)
    }

    public func instanceFeedApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/instance-feed\(suffix)", body)
    }

    public func sleepApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/sleep\(suffix)", body)
    }

    public func sugarBreakApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/sugar-break\(suffix)", body)
    }

    public func videoMixDjApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/video-mix-dj-studio\(suffix)", body)
    }

    public func pokerApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/poker\(suffix)", body)
    }

    public func stretchApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/stretch\(suffix)", body)
    }

    /// A request under the account's Notes API, e.g. `notesApi("GET", "")` or
    /// `notesApi("PATCH", "/<id>", ...)`.
    public func notesApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/notes\(suffix)", body)
    }

    public func noteSocketURL(_ noteId: String) -> URL {
        let path = "/api/notes/\(noteId)/ws"
        return URL(string: ServerURL.parse(settings.serverUrl).webSocket(path)) ?? url(path)
    }

    public func noteSocket(_ noteId: String, connector: WebSocketConnector) async throws -> WebSocketConnection {
        try await connector.connect(
            url: noteSocketURL(noteId), headers: withAuth([:]), protocols: [],
            options: WebSocketOptions(connectTimeout: 15, pingInterval: 30))
    }

    public func workflowsApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/workflows\(suffix)", body)
    }

    /// A request under the organization task board API, e.g. `tasksApi("GET", "?offset=0")`.
    /// Non-members get 403 `org_member_required`.
    public func tasksApi(_ method: String, _ suffix: String, _ body: JSONObject? = nil) async throws -> JSONObject {
        try await accountApiCall(method, "/api/tasks\(suffix)", body)
    }

    // MARK: Organizations

    public func myOrgs() async throws -> [OrgSummary] {
        let data = try await orgRequest("GET", url("/api/orgs"))
        return (data.objectValue?["orgs"]?.arrayValue ?? []).compactMap(\.objectValue)
            .map(OrgSummary.init(json:)).filter { !$0.name.isEmpty }
    }

    public func createOrg(name: String, displayName: String = "", description: String = "") async throws -> OrgSummary {
        var body: JSONObject = ["name": .string(name.dartTrimmed.lowercased())]
        if !displayName.dartTrimmed.isEmpty { body["displayName"] = .string(displayName.dartTrimmed) }
        if !description.dartTrimmed.isEmpty { body["description"] = .string(description.dartTrimmed) }
        let data = try await orgRequest("POST", url("/api/orgs"), body)
        let map = data.objectValue ?? JSONObject()
        let org = map["org"].flatMap { $0.isNull ? nil : $0.text } ?? name
        let role = map["role"].flatMap { $0.isNull ? nil : $0.text } ?? "owner"
        return OrgSummary(name: org, role: role)
    }

    public func orgProfile(_ org: String) async throws -> OrgProfile {
        let data = try await orgRequest("GET", url("/api/orgs/\(ApiService.orgSegment(org))"))
        return OrgProfile(json: data.objectValue ?? JSONObject())
    }

    public func deleteOrg(_ org: String) async throws {
        _ = try await orgRequest("DELETE", url("/api/orgs/\(ApiService.orgSegment(org))"), JSONObject())
    }

    public func orgMembers(_ org: String) async throws -> [OrgMember] {
        let data = try await orgRequest("GET", url("/api/orgs/\(ApiService.orgSegment(org))/members"))
        return (data.objectValue?["members"]?.arrayValue ?? []).compactMap(\.objectValue)
            .map(OrgMember.init(json:)).filter { !$0.name.isEmpty }
    }

    public func setOrgMember(_ org: String, _ member: String, role: String = "member") async throws {
        _ = try await orgRequest("POST", url("/api/orgs/\(ApiService.orgSegment(org))/members"), [
            "member": .string(member.dartTrimmed.lowercased()), "role": .string(role.dartTrimmed.lowercased()),
        ])
    }

    public func removeOrgMember(_ org: String, _ member: String) async throws {
        _ = try await orgRequest("DELETE", url("/api/orgs/\(ApiService.orgSegment(org))/members"), [
            "member": .string(member.dartTrimmed.lowercased()),
        ])
    }

    public func orgTeams(_ org: String) async throws -> [OrgTeam] {
        let data = try await orgRequest("GET", url("/api/orgs/\(ApiService.orgSegment(org))/teams"))
        return (data.objectValue?["teams"]?.arrayValue ?? []).compactMap(\.objectValue)
            .map(OrgTeam.init(json:)).filter { !$0.team.isEmpty }
    }

    public func setOrgTeam(_ org: String, _ team: String, permission: String = "read") async throws {
        _ = try await orgRequest("POST", url("/api/orgs/\(ApiService.orgSegment(org))/teams"), [
            "team": .string(team.dartTrimmed.lowercased()), "permission": .string(permission.dartTrimmed.lowercased()),
        ])
    }

    public func deleteOrgTeam(_ org: String, _ team: String) async throws {
        _ = try await orgRequest("DELETE", url("/api/orgs/\(ApiService.orgSegment(org))/teams"), [
            "team": .string(team.dartTrimmed.lowercased()),
        ])
    }

    public func orgTeamMembers(_ org: String, _ team: String) async throws -> [OrgMember] {
        let path = "/api/orgs/\(ApiService.orgSegment(org))/teams/\(ApiService.orgSegment(team))/members"
        let data = try await orgRequest("GET", url(path))
        return (data.objectValue?["members"]?.arrayValue ?? []).compactMap(\.objectValue)
            .map(OrgMember.init(json:)).filter { !$0.name.isEmpty }
    }

    public func addOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws {
        let path = "/api/orgs/\(ApiService.orgSegment(org))/teams/\(ApiService.orgSegment(team))/members"
        _ = try await orgRequest("POST", url(path), ["member": .string(member.dartTrimmed.lowercased())])
    }

    public func removeOrgTeamMember(_ org: String, _ team: String, _ member: String) async throws {
        let path = "/api/orgs/\(ApiService.orgSegment(org))/teams/\(ApiService.orgSegment(team))/members"
        _ = try await orgRequest("DELETE", url(path), ["member": .string(member.dartTrimmed.lowercased())])
    }

    public func orgRepos(_ org: String) async throws -> [OrgRepo] {
        let data = try await orgRequest("GET", url("/api/orgs/\(ApiService.orgSegment(org))/repos"))
        return (data.objectValue?["repos"]?.arrayValue ?? []).compactMap(\.objectValue)
            .map(OrgRepo.init(json:)).filter { !$0.repo.isEmpty }
    }

    public func linkOrgRepo(_ org: String, _ repo: String, node: String = "") async throws {
        var body: JSONObject = ["repo": .string(repo.dartTrimmed.lowercased())]
        if !node.dartTrimmed.isEmpty { body["node"] = .string(node.dartTrimmed.lowercased()) }
        _ = try await orgRequest("POST", url("/api/orgs/\(ApiService.orgSegment(org))/repos"), body)
    }

    public func unlinkOrgRepo(_ org: String, _ repo: String) async throws {
        _ = try await orgRequest("DELETE", url("/api/orgs/\(ApiService.orgSegment(org))/repos"), [
            "repo": .string(repo.dartTrimmed.lowercased()),
        ])
    }
}
