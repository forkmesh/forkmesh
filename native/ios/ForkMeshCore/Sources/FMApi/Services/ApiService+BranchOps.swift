import FMBase
import FMNet
import Foundation

extension ApiService {
    nonisolated public static let branchOpPollAttempts = 90
    nonisolated static let branchOpPollDelay: TimeInterval = 2

    public func branchOp(
        _ owner: String, _ repo: String, _ request: BranchOpRequest, maxPolls: Int = ApiService.branchOpPollAttempts,
        onProgress: (String) -> Void = { _ in }
    ) async throws -> BranchOpOutcome {
        guard request.isPinned else {
            return BranchOpOutcome(kind: .failed, request: request, answer: nil, message: BranchOpText.notPinned)
        }
        let target = url("/api/repo/\(owner)/\(repo)/branches/ops")
        var current = request
        var polls = 0
        while true {
            let response = try await rawRequest("POST", target, headers: [
                "Accept": "application/json", "Content-Type": "application/json", "Origin": gitOrigin,
            ], body: JSON.encodeData(.object(current.body)))
            let answer = BranchOpAnswer(json: ApiParse.jsonMap(response.body), httpStatus: response.status)
            let waiting = answer.isProcessing || (answer.isRetryable && current.isResuming)
            if waiting {
                current = current.resumed(answer)
                onProgress(answer.isProcessing && !answer.message.isEmpty ? answer.message : BranchOpText.progress(current))
                polls += 1
                if polls >= maxPolls || !current.isResuming {
                    return BranchOpOutcome(
                        kind: .pending, request: current, answer: answer, message: BranchOpText.stillWorking(current))
                }
                try? await sleeper(ApiService.branchOpPollDelay)
                continue
            }
            if answer.isExecuted {
                clearRepoCache(owner, repo)
                return BranchOpOutcome(
                    kind: .done, request: current, answer: answer, message: BranchOpText.done(answer, current))
            }
            if answer.isQueued {
                return BranchOpOutcome(kind: .queued, request: current, answer: answer, message: BranchOpText.queued(current))
            }
            return BranchOpOutcome(kind: .failed, request: current, answer: answer, message: BranchOpText.failure(answer))
        }
    }

    public func updateRunnerAgent(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> RunnerAgentMergeResult {
        try await runnerBranchAction(owner, repo, sessionId, action: "update", cleanup: false, orgScope: orgScope)
    }

    public func completeTask(_ taskId: String, note: String) async throws {
        var body = JSONObject()
        if !note.dartTrimmed.isEmpty { body["completionNote"] = .string(note.dartTrimmed) }
        _ = try await tasksApi("POST", "/\(taskId)/complete", body)
    }
}
