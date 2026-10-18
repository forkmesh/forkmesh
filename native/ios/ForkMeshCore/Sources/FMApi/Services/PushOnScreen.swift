import Foundation

public struct PushScreen: Equatable, Sendable {
    public var tab: AppTab
    public var route: Route?
    public var covered: Bool
    public var conversation: String

    public init(tab: AppTab, route: Route? = nil, covered: Bool = false, conversation: String = "") {
        self.tab = tab
        self.route = route
        self.covered = covered
        self.conversation = conversation
    }
}

extension PushOpen {
    public func isShown(on screen: PushScreen) -> Bool {
        if screen.covered || isAlarm { return false }
        if isChat || isDirectMessage {
            return screen.tab == .chat && screen.route == nil && !conversation.isEmpty
                && conversation == screen.conversation
        }
        if !agent.isEmpty, screen.tab == .agents {
            switch screen.route {
            case .none, .some(.agents(_)), .some(.allAgents), .some(.agentWatch(_)):
                return true
            case let .some(.agent(id)):
                return PushOpen.sameAgent(id, agent)
            default:
                break
            }
        }
        if !alertId.isEmpty, screen.tab == .inbox, screen.route == nil { return true }
        if case let .some(.task(id)) = screen.route, PushOpen.taskId(href) == id { return true }
        if let link = deepLink, let route = screen.route { return InboxRoutes.route(link) == route }
        return false
    }

    static func taskId(_ href: String) -> String? {
        guard href.hasPrefix("/dashboard/tasks"), let parts = URLComponents(string: href) else { return nil }
        let id: String = parts.queryItems?.first { $0.name == "task" }?.value ?? ""
        return id.isEmpty ? nil : id
    }

    static func sameAgent(_ a: String, _ b: String) -> Bool {
        func bare(_ id: String) -> String { id.split(separator: ":").last.map(String.init) ?? id }
        return !a.isEmpty && bare(a) == bare(b)
    }
}
