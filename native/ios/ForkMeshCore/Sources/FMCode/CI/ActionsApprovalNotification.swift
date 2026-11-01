import FMApi
import FMBase
import Foundation

public struct ActionsApprovalNotification: Equatable, Sendable {
    public static let category = NotificationCategoryId.actionsApproval.rawValue

    public var repo: RepoRef
    public var number: Int
    public var head: String
    public var runId: Int

    public init(repo: RepoRef, number: Int, head: String, runId: Int = 0) {
        self.repo = repo
        self.number = number
        self.head = head.lowercased()
        self.runId = runId
    }

    public init?(payload: JSONObject) {
        let layers = [payload, payload.obj("alert"), payload.obj("data"), payload.obj("aps"), payload.obj("meta")]
        func field(_ key: String) -> String {
            layers.map { layer -> String in
                guard let value = layer[key] else { return "" }
                if let text = value.stringValue { return text.trimmingCharacters(in: .whitespaces) }
                let number = value.int(or: 0)
                return number > 0 ? String(number) : ""
            }.first { !$0.isEmpty } ?? ""
        }
        guard field("category") == Self.category, let repo = RepoRef(slug: field("repo")) else { return nil }
        let number = Int(field("number")) ?? 0
        guard number > 0 else { return nil }
        self.init(repo: repo, number: number, head: field("head"), runId: Int(field("runId")) ?? 0)
    }

    public var route: Route { .pull(repo, number: number, tab: .checks) }

    public var canApprove: Bool { NotificationActionRouter.isCommitId(head) }

    @MainActor
    public func approve(client: CIClient) async -> Route? {
        guard canApprove else { return route }
        do {
            _ = try await client.pullChecksAction(repo, number: number, ["action": "approve", "head": .string(head)])
            return nil
        } catch {
            return route
        }
    }
}
