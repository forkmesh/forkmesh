// ApiService, part 5: agent sessions (owner-sealed lists and
// transcripts), the agent queue (launch, steer, retry, cancel), pull-request merges, actions,
// desktop commands and the retired bounty endpoints.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation

extension ApiService {
    /// The relay caps an agent prompt at this length (UTF-16 code units).
    nonisolated public static let maxAgentPromptChars = 8000
    nonisolated static let pullsLedgerRef = "forkmesh/pulls"
    nonisolated static let mergePollAttempts = 8
    nonisolated static let mergePollDelay: TimeInterval = 0.75
    nonisolated static let mergeRouteRetryAttempts = 75
    nonisolated static let mergeRouteRetryDelay: TimeInterval = 2

    /// Opens an owner-sealed envelope off the main actor (ML-KEM takes milliseconds).
    func openSealed(_ envelope: JSONObject) async -> JSONObject? {
        guard let crypto = ownerCrypto else { return nil }
        return await Task.detached { crypto.openJson(envelope) }.value
    }

    nonisolated static func ownerAccountBody(_ ownerAccount: String) -> JSONObject {
        var body = JSONObject()
        if !ownerAccount.dartTrimmed.isEmpty { body["ownerAccount"] = .string(ownerAccount.dartTrimmed.lowercased()) }
        return body
    }

    // MARK: Agent sessions

    /// Agent sessions for a repository (`POST /api/repo/<o>/<r>/agents/list`). Under owner
    /// encryption the relay returns `encryptedAgents`; this device opens the ones sealed to it and
    /// counts the rest (plus `legacyRowsHeldForOwnerMigration`) in `sealedForOthers`.
    public func agentSessionPage(_ owner: String, _ name: String, ownerAccount: String = "") async throws -> AgentSessionPage {
        let data = try await postJson(url("/api/repo/\(owner)/\(name)/agents/list"), ApiService.ownerAccountBody(ownerAccount))
        var sessions: [AgentSession] = []
        var sealedForOthers = 0
        let raw: [JSONValue] = data.objectValue != nil ? ApiService.asList(data) : (data.arrayValue ?? [])
        for item in raw.compactMap(\.objectValue) { sessions.append(AgentSession(json: item, now: clock.now)) }
        if let map = data.objectValue {
            if let encrypted = map["encryptedAgents"]?.arrayValue {
                for entry in encrypted.compactMap(\.objectValue) {
                    guard let envelope = entry["envelope"]?.objectValue else { continue }
                    guard let opened = await openSealed(envelope) else {
                        sealedForOthers += 1
                        continue
                    }
                    sessions.append(AgentSession(json: opened, now: clock.now))
                }
            }
            // Pre-E2EE rows the relay withholds until the owner device migrates them are sessions
            // that exist and cannot be shown here.
            if let held = map["legacyRowsHeldForOwnerMigration"], let count = held.doubleValue, count > 0 {
                sealedForOthers += ApiParse.numInt(held)
            }
        }
        return AgentSessionPage(sessions: sessions, sealedForOthers: sealedForOthers)
    }

    /// Only the readable sessions of `agentSessionPage`.
    public func agentSessions(_ owner: String, _ name: String, ownerAccount: String = "") async throws -> [AgentSession] {
        try await agentSessionPage(owner, name, ownerAccount: ownerAccount).sessions
    }

    public func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String = "") async throws -> AgentTranscript {
        let data = try await postJson(
            url("/api/repo/\(owner)/\(name)/agents/\(id)/transcript"), ApiService.ownerAccountBody(ownerAccount))
        guard let map = data.objectValue else { return AgentTranscript(json: JSONObject()) }
        if map["encrypted"] == .bool(true) {
            if let envelope = map["envelope"]?.objectValue, let opened = await openSealed(envelope) {
                return AgentTranscript(json: opened)
            }
            return AgentTranscript(status: "", transcript: "", sealedForOtherDevice: true)
        }
        return AgentTranscript(json: map)
    }

    // MARK: Agent queue

    /// The owner's `agent-sessions` route, or the organization's `agent-bots` route (keyed by the
    /// public owner) when `orgScope`.
    func agentSessionsURL(_ owner: String, _ repo: String, sessionId: String = "", orgScope: Bool = false) -> URL {
        let pathOwner = agentSessionsOwner(owner, repo, orgScope: orgScope)
        let base = orgScope
            ? "/api/orgs/\(pathOwner)/repos/\(repo)/agent-bots"
            : "/api/repo/\(pathOwner)/\(repo)/agent-sessions"
        return url(base + (sessionId.isEmpty ? "" : "/\(DartURI.encodeComponent(sessionId))"))
    }

    func agentSessionsOwner(_ owner: String, _ repo: String, orgScope: Bool) -> String {
        orgScope ? readOwner(owner, repo) : owner
    }

    func deployApproval(
        kind: String, owner: String, repo: String, target: String, base: String = "", head: String = "",
        requestId: String
    ) -> JSONValue? {
        guard let identity = deployApprovalIdentity,
              let approval = DeployApproval.sign(
                identity, kind: kind, owner: owner, repo: repo, target: target, base: base, head: head,
                requestId: requestId, ts: clock.nowMillis)
        else { return nil }
        return .object(approval)
    }

    /// Where an agent can run for this repository, and what is queued already. The owner route
    /// first; a 403/404 there asks the organization route, and when both refuse the owner
    /// route's refusal is thrown. Signed out: empty options.
    public func agentLaunchOptions(_ owner: String, _ repo: String) async throws -> AgentLaunchOptions {
        if !hasSession { return AgentLaunchOptions() }
        do {
            let json = try await agentJson("GET", agentSessionsURL(owner, repo))
            return await ApiParse.modelAsync(json) { AgentLaunchOptions(json: $0) }
        } catch let refusal as AgentLaunchException {
            if refusal.status != 403 && refusal.status != 404 { throw refusal }
            do {
                let json = try await agentJson("GET", agentSessionsURL(owner, repo, orgScope: true))
                return await ApiParse.modelAsync(json) { AgentLaunchOptions(json: $0, orgScope: true) }
            } catch is AgentLaunchException {
                throw refusal
            }
        }
    }

    /// Queue a new agent run from a typed prompt. Returns once the relay has written the job.
    public func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String = "auto", model: String = "",
        targetNode: String = "", title: String = "", createPr: Bool = false, issueNumber: Int = 0, orgScope: Bool = false
    ) async throws -> AgentQueueResult {
        try await launchAgentSession(
            owner, repo, prompt: prompt, provider: provider, model: model, targetNode: targetNode, title: title,
            createPr: createPr, issueNumber: issueNumber, orgScope: orgScope, attachments: [])
    }

    public func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String = "auto", model: String = "",
        targetNode: String = "", title: String = "", createPr: Bool = false, issueNumber: Int = 0, orgScope: Bool = false,
        attachments: [PromptAttachment]
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
        let data = try await agentJson("POST", agentSessionsURL(owner, repo, orgScope: orgScope), body)
        return AgentQueueResult(json: data)
    }

    /// Send a follow-up prompt into a session that is already queued or running.
    public func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool = false
    ) async throws -> AgentQueueResult {
        try await steerAgentSession(owner, repo, sessionId, prompt: prompt, orgScope: orgScope, attachments: [])
    }

    public func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool = false,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        let text = promptOrPhotoNote(prompt, attachments)
        if text.isEmpty { throw AgentLaunchException("Write what the agent should do next.") }
        if let problem = promptAttachmentsProblem(attachments) { throw AgentLaunchException(problem) }
        var body: JSONObject = ["prompt": .string(text)]
        if !attachments.isEmpty { body["attachments"] = .array(attachments.map(\.wire)) }
        let data = try await agentJson(
            "POST", agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope), body)
        return AgentQueueResult(json: data)
    }

    /// Run a failed or stopped session again: a session a node claimed is resumed with its task
    /// as a follow-up; one no node started is queued again with the same settings.
    public func retryAgentSession(_ owner: String, _ repo: String, _ session: AgentLaunchSession) async throws -> AgentRetryResult {
        try await retryAgentSession(owner, repo, session, followUp: "", attachments: [])
    }

    public func retryAgentSession(
        _ owner: String, _ repo: String, _ session: AgentLaunchSession, followUp: String,
        attachments: [PromptAttachment]
    ) async throws -> AgentRetryResult {
        let extra = followUp.dartTrimmed
        let prompt = session.retryPrompt(followUp: extra)
        if prompt.isEmpty && attachments.isEmpty {
            throw AgentLaunchException(
                "This session did not keep the task it was given, so there is nothing to run again. Start a new one.")
        }
        if session.canResume {
            let result = try await steerAgentSession(
                owner, repo, session.sessionId, prompt: extra.isEmpty && attachments.isEmpty ? prompt : extra,
                orgScope: session.orgScope, attachments: attachments)
            return AgentRetryResult(result: result, resumed: true)
        }
        let result = try await launchAgentSession(
            owner, repo, prompt: prompt, provider: session.provider.isEmpty ? "auto" : session.provider,
            model: session.requestedModel, targetNode: session.targetNode, title: session.title,
            createPr: session.createPr, issueNumber: session.issueNumber, orgScope: session.orgScope,
            attachments: attachments)
        return AgentRetryResult(result: result, resumed: false)
    }

    /// Drop a queued session. Work already running on the node keeps going there.
    public func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false) async throws {
        _ = try await agentJson("DELETE", agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope))
    }

    /// One request against the agent queue. Mutations carry `Origin: <relay origin>`, which the
    /// Worker's CSRF guard requires. A non-2xx throws `AgentLaunchException` with the relay's
    /// sentence.
    func agentJson(_ method: String, _ url: URL, _ body: JSONObject? = nil) async throws -> JSONObject {
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if body != nil { headers["Content-Type"] = "application/json" }
        if method != "GET" { headers["Origin"] = gitOrigin }
        let response = try await rawRequest(method, url, headers: headers, body: body.map { JSON.encodeData(.object($0)) })
        let decoded = await ApiParse.jsonMapAsync(response.body)
        if response.ok { return decoded }
        throw AgentLaunchException(
            ApiService.agentMessage(decoded, response.status), code: decoded.str("error"), status: response.status)
    }

    /// The relay's `message` for a person, or a sentence for its error code.
    nonisolated static func agentMessage(_ body: JSONObject, _ status: Int) -> String {
        let message = body.str("message").dartTrimmed
        if !message.isEmpty { return message }
        let code = body.str("error").dartTrimmed
        switch code {
        case "origin_not_allowed": return "This build cannot queue agent work against that relay."
        case "invalid_session": return "Sign in again before starting an agent."
        case "forbidden": return "Only the owner of this repository can start agents on its nodes."
        case "": return "The relay refused the agent request (HTTP \(status))."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }

    // MARK: Merging a pull request

    /// Whether this account may merge `owner`/`repo`: it owns the organization the catalog row is
    /// linked to. The relay is the authority; this only avoids offering a button that would be refused.
    public func canMergePulls(_ owner: String, _ repo: String) async -> Bool {
        if !hasSession { return false }
        do {
            return try await orgProfile(readOwner(owner, repo)).isOwner
        } catch {
            return false
        }
    }

    public func canManageIssues(_ owner: String, _ repo: String, account: String, isAdmin: Bool) async -> Bool {
        if !hasSession { return false }
        if isAdmin || (!account.isEmpty && account.lowercased() == owner.lowercased()) { return true }
        do {
            return try await orgProfile(readOwner(owner, repo)).canManage
        } catch {
            return false
        }
    }

    /// Resolve the object ids a merge of `pull` must pin, or nil when it cannot be merged from
    /// here (not open, refs unknown, a merge trial unavailable or against another base).
    public func pullMergePlan(_ owner: String, _ repo: String, _ pull: PublishedPull) async -> PullMergePlan? {
        if pull.status.dartTrimmed.lowercased() != "open" { return nil }
        let pullsOid = await pullsLedgerCommit(owner, repo)
        if pullsOid.isEmpty { return nil }
        let baseTip = await branchTip(owner, repo, pull.base)
        if baseTip.isEmpty { return nil }
        let head = pull.head.dartTrimmed
        if !pull.branchBacked || head.isEmpty {
            // A patch-backed pull carries its reviewed payload in the ledger commit itself.
            if !(await pullHasPayload(owner, repo, pull.number)) { return nil }
            return PullMergePlan(
                number: pull.number, expectedBaseOid: baseTip, expectedHeadOid: pullsOid, expectedPullsOid: pullsOid,
                branchBacked: false)
        }
        guard let preview = await mergePreview(owner, repo, base: baseTip, head: head) else { return nil }
        let baseOid = preview.str("baseOid").dartTrimmed.lowercased()
        let headOid = preview.str("headOid").dartTrimmed.lowercased()
        if baseOid.isEmpty || headOid.isEmpty || baseOid != baseTip { return nil }
        let conflicts = (preview["conflicts"]?.arrayValue ?? []).map { DartCore.toString($0) }.filter { !$0.isEmpty }
        return PullMergePlan(
            number: pull.number, expectedBaseOid: baseOid, expectedHeadOid: headOid, expectedPullsOid: pullsOid,
            clean: preview["clean"] == .bool(true), conflicts: Array(conflicts.prefix(200)))
    }

    /// Merge the pull request described by `plan`. The relay answers 202 while a node works, so
    /// the same request id is re-sent (idempotent) up to 8 times, 750 ms apart; after that the
    /// outcome is `pending`. A refusal throws `AgentLaunchException`.
    public func mergePull(_ owner: String, _ repo: String, _ plan: PullMergePlan, overrideReview: Bool = false, deleteHead: Bool = false) async throws -> PullMergeOutcome {
        if !plan.isConsistent {
            throw AgentLaunchException(
                "This pull request could not be pinned to exact commits, so it was not merged. Refresh and try again.")
        }
        let pathOwner = readOwner(owner, repo)
        let target = url("/api/repo/\(pathOwner)/\(repo)/pulls/\(plan.number)/merge")
        let requestId = ApiService.mergeRequestId()
        var body: JSONObject = [
            "schemaVersion": 1,
            "type": "forkmesh.pull-merge-v1",
            "pullNumber": .int(Int64(plan.number)),
            "requestId": .string(requestId),
            "expectedBaseOid": .string(plan.expectedBaseOid),
            "expectedHeadOid": .string(plan.expectedHeadOid),
            "expectedPullsOid": .string(plan.expectedPullsOid),
        ]
        if overrideReview { body["overrideReview"] = true }
        if deleteHead { body["deleteHead"] = true }
        body["deployApproval"] = deployApproval(
            kind: "pull", owner: pathOwner, repo: repo, target: String(plan.number), base: plan.expectedBaseOid,
            head: plan.expectedHeadOid, requestId: requestId)
        let encoded = JSON.encodeData(.object(body))
        var pollAttempts = 0
        var routeAttempts = 0
        while true {
            let response = try await rawRequest("POST", target, headers: [
                "Accept": "application/json", "Content-Type": "application/json", "Origin": gitOrigin,
            ], body: encoded)
            let decoded = ApiParse.jsonMap(response.body)
            let status = decoded.str("status").dartTrimmed.lowercased()
            let code = decoded.str("error").dartTrimmed
            let retryRoute = response.status == 503 && (
                code == "mirror_unavailable" ||
                    (code == "merge_node_unavailable" && decoded.str("reason").dartTrimmed != "no_merge_executor")
            )
            if retryRoute && routeAttempts < ApiService.mergeRouteRetryAttempts - 1 {
                routeAttempts += 1
                try? await sleeper(ApiService.mergeRouteRetryDelay)
                continue
            }
            if response.ok && (response.status == 202 || status == "processing") {
                pollAttempts += 1
                if pollAttempts == ApiService.mergePollAttempts { break }
                try? await sleeper(ApiService.mergePollDelay)
                continue
            }
            if response.ok && status == "merged" {
                clearRepoCache(owner, repo)
                return PullMergeOutcome(merged: true, message: "Merged.")
            }
            throw AgentLaunchException(
                ApiService.mergeMessage(decoded, response.status), code: decoded.str("error"), status: response.status)
        }
        return PullMergeOutcome(
            merged: false,
            message: "The node is still merging. It finishes on its own; reopen this pull request in a moment to see the result.",
            pending: true)
    }

    nonisolated static func mergeMessage(_ body: JSONObject, _ status: Int) -> String {
        let message = body.str("message").dartTrimmed
        if !message.isEmpty { return message }
        let code = body.str("error").dartTrimmed
        switch code {
        case "forbidden": return "Only an owner of this repository can merge it from here."
        case "invalid_session": return "Sign in again before merging."
        case "merge_conflict": return "The branches conflict. Resolve the conflict, then merge."
        case "review_required": return "This pull request needs an independent review before it can merge."
        case "checks_required": return "Required checks have not passed yet."
        case "stale_base", "stale_head", "stale_pull_metadata":
            return "The repository moved while this was being prepared. Refresh and merge again."
        case "pull_not_open": return "This pull request is no longer open."
        case "unsupported_pull":
            return "This pull request cannot be merged from the app; merge it from the ForkMesh desktop app."
        case "merge_node_unavailable":
            switch body.str("reason").dartTrimmed {
            case "refs_catching_up":
                return "The available nodes have not agreed on this repository's latest refs yet. ForkMesh will retry while they catch up."
            case "mirror_unverified":
                return "The available nodes could not verify this repository's signed merge state. ForkMesh will retry shortly."
            case "no_merge_executor":
                return "The available nodes do not run the merge executor for this repository. Merge from an updated desktop or bring an executor online."
            case "job_node_offline":
                return "The node already handling this merge is temporarily offline. ForkMesh will keep checking it."
            default:
                return "No healthy mirror could verify this repository for the merge yet. ForkMesh will retry shortly."
            }
        case "mirror_unavailable":
            return "The relay could not load this repository's current mirror route. ForkMesh will retry shortly."
        case "merge_queue_full": return "Too many merges are queued for this repository. Try again shortly."
        case "": return "The merge was refused (HTTP \(status))."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }

    /// The immutable commit of the `forkmesh/pulls` ledger, through a tree read of its path.
    func pullsLedgerCommit(_ owner: String, _ repo: String) async -> String {
        do {
            return try await tree(owner, repo, path: ".forkmesh/pulls", ref: ApiService.pullsLedgerRef)
                .commit.dartTrimmed.lowercased()
        } catch {
            return ""
        }
    }

    func pullHasPayload(_ owner: String, _ repo: String, _ number: Int) async -> Bool {
        guard let listing = try? await tree(owner, repo, path: ".forkmesh/pulls/\(number)", ref: ApiService.pullsLedgerRef)
        else { return false }
        return listing.entries.contains { !$0.isDirectory && ($0.name == "commits.mbox" || $0.name == "changes.patch") }
    }

    func branchTip(_ owner: String, _ repo: String, _ branch: String) async -> String {
        let name = branch.dartTrimmed
        if name.isEmpty { return "" }
        guard let list = try? await branches(owner, repo) else { return "" }
        return list.first { $0.name == name }?.sha.dartTrimmed.lowercased() ?? ""
    }

    func mergePreview(_ owner: String, _ repo: String, base: String, head: String) async -> JSONObject? {
        let target = url("/api/repo/\(readOwner(owner, repo))/\(repo)/merge-preview", [("base", base), ("head", head), ("ref", "")])
        guard let response = try? await rawRequest("GET", target), response.ok else { return nil }
        let decoded = await ApiParse.jsonMapAsync(response.body)
        return decoded.isEmpty ? nil : decoded
    }

    /// `mobile_` + 36 lower-case hex characters (18 random bytes).
    nonisolated static func mergeRequestId() -> String { "mobile_" + Hex.encode(Bytes.random(18)) }

    // MARK: Actions and desktop commands

    public func repoActions(
        _ owner: String, _ name: String, ownerAccount: String = "", ts: String = "", sig: String = ""
    ) async throws -> RepoActions {
        let data = try await postJson(
            url("/api/repo/\(owner)/\(name)/actions/list"), ApiService.signedBody(ownerAccount, ts, sig))
        return RepoActions(json: data.objectValue ?? JSONObject())
    }

    public func actionLog(
        _ owner: String, _ name: String, _ id: Int, ownerAccount: String = "", ts: String = "", sig: String = ""
    ) async throws -> ActionLog {
        let data = try await postJson(
            url("/api/repo/\(owner)/\(name)/actions/\(id)/log"), ApiService.signedBody(ownerAccount, ts, sig))
        return ActionLog(json: data.objectValue ?? JSONObject())
    }

    nonisolated static func signedBody(_ ownerAccount: String, _ ts: String, _ sig: String) -> JSONObject {
        var body = ownerAccountBody(ownerAccount)
        if !ts.dartTrimmed.isEmpty { body["ts"] = .string(ts.dartTrimmed) }
        if !sig.dartTrimmed.isEmpty { body["sig"] = .string(sig.dartTrimmed) }
        return body
    }

    public func desktopCommand(
        _ owner: String, _ name: String, ownerAccount: String, command: String, target: String, ts: String, sig: String,
        payload: JSONObject = JSONObject()
    ) async throws -> DesktopCommandResult {
        let body: JSONObject = [
            "ownerAccount": .string(ownerAccount.dartTrimmed.lowercased()),
            "command": .string(command.dartTrimmed),
            "target": .string(target.dartTrimmed),
            "ts": .string(ts.dartTrimmed),
            "sig": .string(sig.dartTrimmed),
            "payload": .object(payload),
        ]
        let response = try await http.send(HTTPRequest(
            method: "POST", url: url(DesktopCommandResult.path(owner, name)),
            headers: ["Accept": "application/json", "Content-Type": "application/json"],
            body: JSON.encodeData(.object(body)), timeout: 12))
        let map = ApiParse.jsonMap(response.body)
        if response.status >= 200 && response.status < 300 {
            return DesktopCommandResult(json: map)
        }
        throw ApiException(DesktopCommandResult.errorMessage(status: response.status, code: map.str("error")))
    }

    public func desktopCommandRouteAnswered(_ owner: String, _ name: String) async -> Bool? {
        guard let response = try? await http.send(HTTPRequest(
            method: "POST", url: url(DesktopCommandResult.path(owner, name)),
            headers: ["Accept": "application/json", "Content-Type": "application/json"],
            body: JSON.encodeData(.object([:])), timeout: 12))
        else { return nil }
        return DesktopCommandResult.routeAnswered(
            status: response.status, code: ApiParse.jsonMap(response.body).str("error"))
    }

    // MARK: Bounties

    /// `POST /api/repo/<o>/<r>/bounty {action: status}`; a 404 reads as no bounty.
    public func issueBountyStatus(_ owner: String, _ repo: String, _ number: Int) async throws -> IssueBounty {
        do {
            let data = try await postJson(url("/api/repo/\(owner)/\(repo)/bounty"), [
                "action": "status", "number": .int(Int64(number)),
            ])
            return IssueBounty(json: data)
        } catch let error as ApiException where error.message.contains("HTTP 404") {
            return IssueBounty()
        }
    }

    /// Retired: always throws `ApiUnsupportedError`.
    public func createIssueBounty(
        _ owner: String, _ repo: String, number: Int, amountUsd: Double, payee: String = "", payeeNode: String = "",
        ts: String, sig: String
    ) async throws -> IssueBounty {
        throw ApiUnsupportedError(
            "Worker-held issue-bounty wallets are retired. Use an external self-custodial wallet, reviewed program, or multisig.")
    }

    /// Retired: always throws `ApiUnsupportedError`.
    public func bountyWallet(_ owner: String, _ repo: String, ts: String, sig: String) async throws -> BountyWallet {
        throw ApiUnsupportedError(
            "Worker-held bounty wallets are retired and cannot be created or prepared by this client.")
    }
}
