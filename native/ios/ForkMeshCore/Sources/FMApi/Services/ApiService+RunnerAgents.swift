import FMBase
import FMNet
import Foundation

extension ApiService {
    nonisolated static let runnerMergePollAttempts = 20
    nonisolated public static let runnerDiffPageSize = 25

    public func runnerAgentDiff(
        _ owner: String, _ repo: String, _ sessionId: String, page: Int = 0,
        pageSize: Int = ApiService.runnerDiffPageSize, orgScope: Bool = false
    ) async throws -> RunnerAgentDiffPage {
        let base = agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope)
        var components = URLComponents(url: base.appendingPathComponent("diff"), resolvingAgainstBaseURL: false)
        components?.queryItems = [
            URLQueryItem(name: "page", value: String(max(0, page))),
            URLQueryItem(name: "pageSize", value: String(min(100, max(1, pageSize)))),
        ]
        let target = components?.url ?? base
        let response = try await rawRequest("GET", target, headers: ["Accept": "application/json"])
        let decoded = await ApiParse.jsonMapAsync(response.body)
        if response.ok { return RunnerAgentDiffPage(json: decoded) }
        throw AgentLaunchException(
            RunnerAgentText.mergeMessage(decoded, response.status), code: decoded.str("error"), status: response.status)
    }

    public func mergeRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, cleanup: Bool = true, orgScope: Bool = false
    ) async throws -> RunnerAgentMergeResult {
        try await runnerBranchAction(owner, repo, sessionId, action: "merge", cleanup: cleanup, orgScope: orgScope)
    }

    public func cleanupRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> RunnerAgentMergeResult {
        try await runnerBranchAction(owner, repo, sessionId, action: "cleanup", cleanup: true, orgScope: orgScope)
    }

    public func discardRunnerAgentBranch(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> RunnerAgentMergeResult {
        try await runnerBranchAction(
            owner, repo, sessionId, action: "cleanup", cleanup: true, discard: true, orgScope: orgScope)
    }

    public func linkRunnerAgentPull(
        _ owner: String, _ repo: String, _ sessionId: String, number: Int, orgScope: Bool = false
    ) async throws -> RunnerAgentPullLink {
        let target = agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope)
        let body: JSONObject = ["action": "link-pull", "number": .int(Int64(number))]
        let response = try await rawRequest("POST", target, headers: [
            "Accept": "application/json", "Content-Type": "application/json", "Origin": gitOrigin,
        ], body: JSON.encodeData(.object(body)))
        let decoded = ApiParse.jsonMap(response.body)
        if response.ok { return RunnerAgentPullLink(json: decoded) }
        throw AgentLaunchException(
            RunnerAgentText.linkMessage(decoded, response.status), code: decoded.str("error"), status: response.status)
    }

    public func runnerDefaultBranch(_ owner: String, _ repo: String) async -> String {
        let list = (try? await branches(owner, repo)) ?? []
        return list.first(where: \.isDefault)?.name ?? "main"
    }

    public func continueRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> AgentQueueResult {
        try await steerAgentSession(owner, repo, sessionId, prompt: RunnerAgentText.continuePrompt, orgScope: orgScope)
    }

    public func runnerBranchNames(_ owner: String, _ repo: String) async -> [String] {
        guard let list = try? await branches(owner, repo) else { return [] }
        return list.map(\.name).filter { !$0.hasPrefix("agent/") && !$0.hasPrefix("forkmesh/") }
    }

    public func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String = "auto", model: String = "",
        targetNode: String = "", title: String = "", createPr: Bool = false, issueNumber: Int = 0,
        orgScope: Bool = false, attachments: [PromptAttachment] = [], baseBranch: String, branchName: String
    ) async throws -> AgentQueueResult {
        let text = promptOrPhotoNote(prompt, attachments)
        if text.isEmpty { throw AgentLaunchException("Write what the agent should do.") }
        if let problem = promptAttachmentsProblem(attachments) { throw AgentLaunchException(problem) }
        if !hasSession {
            throw AgentLaunchException("Sign in before starting an agent.", code: "invalid_session", status: 401)
        }
        if text.utf16.count > ApiService.maxAgentPromptChars {
            throw AgentLaunchException("That prompt is longer than the relay accepts. Trim it and try again.")
        }
        var body: JSONObject = ["provider": .string(provider), "prompt": .string(text)]
        if !model.dartTrimmed.isEmpty { body["model"] = .string(model.dartTrimmed) }
        if !targetNode.dartTrimmed.isEmpty { body["targetNode"] = .string(targetNode.dartTrimmed) }
        if !title.dartTrimmed.isEmpty { body["title"] = .string(title.dartTrimmed) }
        body["createPr"] = .bool(createPr)
        if issueNumber > 0 { body["issueNumber"] = .int(Int64(issueNumber)) }
        if !attachments.isEmpty { body["attachments"] = .array(attachments.map(\.wire)) }
        if !baseBranch.dartTrimmed.isEmpty { body["baseBranch"] = .string(baseBranch.dartTrimmed) }
        let slug = RunnerAgentText.branchSlug(branchName)
        if !slug.isEmpty { body["branchName"] = .string(slug) }
        let data = try await agentJson("POST", agentSessionsURL(owner, repo, orgScope: orgScope), body)
        return AgentQueueResult(json: data)
    }

    func runnerBranchAction(
        _ owner: String, _ repo: String, _ sessionId: String, action: String, cleanup: Bool, discard: Bool = false,
        orgScope: Bool
    ) async throws -> RunnerAgentMergeResult {
        let target = agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope)
        var retriedStale = false
        var requestId = ApiService.mergeRequestId()
        var approval: (requestId: String, value: JSONValue?)?
        var attempt = 0
        while attempt < ApiService.runnerMergePollAttempts {
            var body: JSONObject = ["action": .string(action), "requestId": .string(requestId)]
            if action == "merge" {
                body["cleanup"] = .bool(cleanup)
                if approval?.requestId != requestId {
                    approval = (requestId, deployApproval(
                        kind: "branch", owner: agentSessionsOwner(owner, repo, orgScope: orgScope), repo: repo,
                        target: sessionId, requestId: requestId))
                }
                body["deployApproval"] = approval?.value
            }
            if discard { body["discard"] = .bool(true) }
            let response = try await rawRequest("POST", target, headers: [
                "Accept": "application/json", "Content-Type": "application/json", "Origin": gitOrigin,
            ], body: JSON.encodeData(.object(body)))
            let decoded = ApiParse.jsonMap(response.body)
            let status = decoded.str("status").dartTrimmed.lowercased()
            if response.ok && (response.status == 202 || status == "processing") {
                let echoed = decoded.str("requestId").dartTrimmed
                if !echoed.isEmpty { requestId = echoed }
                attempt += 1
                if attempt < ApiService.runnerMergePollAttempts { try? await sleeper(ApiService.mergePollDelay) }
                continue
            }
            if response.ok {
                clearRepoCache(owner, repo)
                return RunnerAgentMergeResult(json: decoded)
            }
            let code = decoded.str("error")
            if response.status == 503 && code == "merge_status_pending" {
                attempt += 1
                if attempt < ApiService.runnerMergePollAttempts { try? await sleeper(ApiService.mergePollDelay) }
                continue
            }
            if (code == "stale_base" || code == "stale_head") && !retriedStale {
                retriedStale = true
                requestId = ApiService.mergeRequestId()
                continue
            }
            throw AgentLaunchException(
                RunnerAgentText.mergeMessage(decoded, response.status), code: code, status: response.status)
        }
        if action == "update" {
            return RunnerAgentMergeResult(
                pending: requestId,
                message: "The core is still updating the branch. It finishes on its own; pull to refresh in a moment to see the result.")
        }
        return RunnerAgentMergeResult(
            pending: requestId,
            message: "The core is still merging. It finishes on its own; pull to refresh in a moment to see the result.")
    }
}
