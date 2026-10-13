import FMBase
import FMControl
import Foundation
import Observation

@MainActor
@Observable
public final class ApprovalsQueue {
    public private(set) var approvals: [PendingApproval] = []
    public private(set) var loading = false
    public private(set) var errorText = ""
    public private(set) var unsupported = false
    public private(set) var answering: Set<String> = []
    public private(set) var lastAnswered: String?

    @ObservationIgnored private let client: AgentOpsClient
    @ObservationIgnored private var tap: UUID?
    @ObservationIgnored private var refreshTask: Task<Void, Never>?

    public init(client: AgentOpsClient) {
        self.client = client
    }

    public var count: Int { approvals.count }

    public var isEmpty: Bool { approvals.isEmpty }

    public func approval(id: String) -> PendingApproval? { approvals.first { $0.id == id } }

    public func approvals(forSession sessionId: Int) -> [PendingApproval] {
        approvals.filter { $0.sessionId == sessionId }
    }

    public var sessionIdsNeedingYou: Set<Int> { Set(approvals.map(\.sessionId)) }

    public func start() {
        if tap != nil { return }
        tap = client.onChanged { [weak self] _ in
            self?.scheduleRefresh()
        }
    }

    public func stop() {
        if let tap { client.removeTap(tap) }
        tap = nil
        refreshTask?.cancel()
        refreshTask = nil
    }

    public func scheduleRefresh() {
        if refreshTask != nil { return }
        refreshTask = Task { [weak self] in
            await self?.refresh()
            self?.refreshTask = nil
        }
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            approvals = try await client.approvals().sorted { $0.askedAtMs > $1.askedAtMs }
            errorText = ""
            unsupported = false
        } catch {
            if AgentOpsClient.isUnsupported(error) {
                unsupported = true
                approvals = []
                errorText = ""
            } else {
                errorText = AgentOpsClient.describe(error)
            }
        }
    }

    @discardableResult
    public func approve(_ approvalId: String, lifetime: ApprovalLifetime = .once) async -> Bool {
        await answer(approvalId, allow: true, lifetime: lifetime)
    }

    @discardableResult
    public func deny(_ approvalId: String) async -> Bool {
        await answer(approvalId, allow: false, lifetime: .once)
    }

    private func answer(_ approvalId: String, allow: Bool, lifetime: ApprovalLifetime) async -> Bool {
        guard AgentOpsIds.isApprovalId(approvalId), !answering.contains(approvalId) else { return false }
        answering.insert(approvalId)
        defer { answering.remove(approvalId) }
        do {
            let confirmation = try await client.prepareAnswer(approvalId, allow: allow, lifetime: lifetime)
            try await client.answer(approvalId, allow: allow, lifetime: lifetime, token: confirmation.token)
            approvals.removeAll { $0.id == approvalId }
            lastAnswered = approvalId
            errorText = ""
            return true
        } catch {
            errorText = AgentOpsClient.describe(error)
            if AgentOpsClient.errorCode(error) == "unknown_approval" {
                approvals.removeAll { $0.id == approvalId }
            }
            return false
        }
    }
}
