import FMBase
import Foundation

extension PackagesClient {
    public static let surveysPackageId = "surveys"

    private func surveysPath(_ organization: String, _ rest: String = "") throws -> String {
        "/api/surveys/" + (try Self.segment(organization)) + rest
    }

    private func surveyItemPath(_ organization: String, _ id: String, _ action: String = "") throws -> String {
        try surveysPath(organization, "/items/" + Self.item(id) + (action.isEmpty ? "" : "/" + action))
    }

    private static func publicPath(_ token: String) throws -> String {
        guard SurveyPublicForm.isToken(token) else { throw PackagesError("survey_not_found", status: 404) }
        return "/api/surveys/-/public/" + token
    }

    @discardableResult
    func surveysCall(_ method: String, _ path: String, body: JSONObject? = nil) async throws -> JSONObject {
        let reply = try await transport.send(method, path, query: [], body: body)
        if reply.ok && reply.json.optBool("ok") != false { return reply.json }
        var error = PackagesError.from(reply)
        if error.code == "plugin_not_installed" {
            error.fields["canInstall"] = reply.json.bool("canInstall") ? "true" : "false"
        }
        let question = reply.json.str("question")
        if SurveyQuestion.isQuestionId(question) { error.fields["question"] = question }
        if error.code.isEmpty && reply.ok { error.code = "send_failed" }
        throw error
    }

    public func surveyOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await surveysCall("GET", "/api/surveys"))
    }

    public func surveys(_ organization: String) async throws -> SurveyBoard {
        let reply = try await surveysCall("GET", try surveysPath(organization))
        return await Task.detached(priority: .userInitiated) { SurveyBoard(json: reply) }.value
    }

    public func survey(_ organization: String, id: String) async throws -> Survey {
        Survey(json: try await surveysCall("GET", try surveyItemPath(organization, id)).obj("survey"))
    }

    public func saveSurvey(_ organization: String, _ draft: SurveyDraft) async throws -> Survey {
        if draft.isNew {
            return Survey(json: try await surveysCall("POST", try surveysPath(organization, "/items"), body: draft.body).obj("survey"))
        }
        return Survey(json: try await surveysCall("PATCH", try surveyItemPath(organization, draft.id), body: draft.body).obj("survey"))
    }

    public func deleteSurvey(_ organization: String, id: String) async throws {
        try await surveysCall("DELETE", try surveyItemPath(organization, id), body: [:])
    }

    public func openSurvey(_ organization: String, id: String) async throws -> Survey {
        Survey(json: try await surveysCall("POST", try surveyItemPath(organization, id, "open"), body: [:]).obj("survey"))
    }

    public func closeSurvey(_ organization: String, id: String) async throws -> Survey {
        Survey(json: try await surveysCall("POST", try surveyItemPath(organization, id, "close"), body: [:]).obj("survey"))
    }

    public func duplicateSurvey(_ organization: String, id: String) async throws -> Survey {
        Survey(json: try await surveysCall("POST", try surveyItemPath(organization, id, "duplicate"), body: [:]).obj("survey"))
    }

    public func surveyResponses(_ organization: String, id: String) async throws -> [SurveyResponse] {
        try await surveysCall("GET", try surveyItemPath(organization, id, "responses")).maps("responses")
            .map(SurveyResponse.init(json:)).filter { SurveyResponse.isResponseId($0.id) }
    }

    public func deleteSurveyResponse(_ organization: String, id: String, response: String) async throws -> Survey {
        guard SurveyResponse.isResponseId(response) else { throw PackagesError("not_found", status: 404) }
        let path = try surveyItemPath(organization, id, "responses/" + response)
        return Survey(json: try await surveysCall("DELETE", path, body: [:]).obj("survey"))
    }

    public func exportSurvey(_ organization: String, id: String) async throws -> SurveyExport {
        SurveyExport(json: try await surveysCall("GET", try surveyItemPath(organization, id, "export")))
    }

    public func publicSurvey(token: String) async throws -> SurveyPublicForm {
        SurveyPublicForm(json: try await surveysCall("GET", try Self.publicPath(token)).obj("survey"))
    }

    public func submitSurvey(token: String, answers: JSONObject) async throws -> String {
        try await surveysCall("POST", try Self.publicPath(token), body: ["answers": .object(answers)]).str("thankYou")
    }

    public func installSurveys(_ organization: String) async throws -> OrgPackageState {
        try await install(Self.surveysPackageId, organization: organization)
    }
}
