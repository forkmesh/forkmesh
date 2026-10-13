import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import FMNet
import FMPackages
import FMPlanning
import Foundation

public struct AssistantAccount: Equatable, Sendable {
    public var username: String
    public var email: String
    public var isAdmin: Bool

    public init(username: String, email: String = "", isAdmin: Bool = false) {
        self.username = username
        self.email = email
        self.isAdmin = isAdmin
    }
}

@MainActor
public struct AssistantSources {
    public var account: () -> AssistantAccount?
    public var orgs: () async throws -> [OrgSummary]
    public var tasks: (_ offset: Int) async throws -> JSONObject
    public var task: (_ id: String) async throws -> JSONObject
    public var notes: () async throws -> [JSONObject]
    public var note: (_ id: String) async throws -> JSONObject
    public var repositories: () async throws -> [Repository]
    public var issues: (_ owner: String, _ name: String) async throws -> [Issue]
    public var pulls: (_ owner: String, _ name: String) async throws -> [PublishedPull]
    public var commits: (_ owner: String, _ name: String) async throws -> [JSONObject]
    public var chatConversations: () -> [String]
    public var chatMessages: () -> [ChatMessage]
    public var agents: () async throws -> [AgentLaunchSession]
    public var pings: (_ limit: Int) async throws -> NotificationPage
    public var calendar: (_ startMs: Int64, _ endMs: Int64) async throws -> [CalendarEvent]
    public var crm: () -> PackagesClient?
    public var agentControl: () -> AgentControlling?
    public var web: () -> HTTPClient?

    public init(
        account: @escaping () -> AssistantAccount? = { nil },
        orgs: @escaping () async throws -> [OrgSummary] = { [] },
        tasks: @escaping (_ offset: Int) async throws -> JSONObject = { _ in JSONObject() },
        task: @escaping (_ id: String) async throws -> JSONObject = { _ in JSONObject() },
        notes: @escaping () async throws -> [JSONObject] = { [] },
        note: @escaping (_ id: String) async throws -> JSONObject = { _ in JSONObject() },
        repositories: @escaping () async throws -> [Repository] = { [] },
        issues: @escaping (_ owner: String, _ name: String) async throws -> [Issue] = { _, _ in [] },
        pulls: @escaping (_ owner: String, _ name: String) async throws -> [PublishedPull] = { _, _ in [] },
        commits: @escaping (_ owner: String, _ name: String) async throws -> [JSONObject] = { _, _ in [] },
        chatConversations: @escaping () -> [String] = { [] },
        chatMessages: @escaping () -> [ChatMessage] = { [] },
        agents: @escaping () async throws -> [AgentLaunchSession] = { [] },
        pings: @escaping (_ limit: Int) async throws -> NotificationPage = { _ in NotificationPage(notifications: [], unread: 0) },
        calendar: @escaping (_ startMs: Int64, _ endMs: Int64) async throws -> [CalendarEvent] = { _, _ in [] },
        crm: @escaping () -> PackagesClient? = { nil },
        agentControl: @escaping () -> AgentControlling? = { nil },
        web: @escaping () -> HTTPClient? = { nil }
    ) {
        self.account = account
        self.orgs = orgs
        self.tasks = tasks
        self.task = task
        self.notes = notes
        self.note = note
        self.repositories = repositories
        self.issues = issues
        self.pulls = pulls
        self.commits = commits
        self.chatConversations = chatConversations
        self.chatMessages = chatMessages
        self.agents = agents
        self.pings = pings
        self.calendar = calendar
        self.crm = crm
        self.agentControl = agentControl
        self.web = web
    }

    public static func live(
        api: ApiService,
        auth: @escaping () -> AuthService? = { Services.get(AuthService.self) },
        relay: @escaping () -> RelayService? = { Services.get(RelayService.self) },
        agentControl: @escaping () -> AgentControlling? = { Services.get(AgentControlCenter.self) },
        web: HTTPClient = URLSessionHTTPClient()
    ) -> AssistantSources {
        let account: () -> AssistantAccount? = {
            guard let session = auth()?.session, !session.nodeName.isEmpty else { return nil }
            return AssistantAccount(username: session.nodeName, email: session.email, isAdmin: session.isAdmin)
        }
        return AssistantSources(
            account: account,
            orgs: { try await api.myOrgs() },
            tasks: { offset in try await api.tasksApi("GET", offset > 0 ? "?offset=\(offset)" : "") },
            task: { id in try await api.tasksApi("GET", "/\(AssistantText.pathSegment(id))").obj("task") },
            notes: { try await api.notesApi("GET", "").maps("notes") },
            note: { id in try await api.notesApi("GET", "/\(AssistantText.pathSegment(id))").obj("note") },
            repositories: { try await api.repositories() },
            issues: { owner, name in try await api.publishedIssues(owner, name) },
            pulls: { owner, name in try await api.publishedPulls(owner, name) },
            commits: { owner, name in try await api.commits(owner, name) },
            chatConversations: { relay()?.channels ?? [] },
            chatMessages: { relay()?.recentMessages ?? [] },
            agents: { try await api.accountAgentSessions().sessions },
            pings: { limit in
                guard let node = account()?.username else { return NotificationPage(notifications: [], unread: 0) }
                return try await api.notifications(node, limit: limit)
            },
            calendar: { start, end in
                try await api.accountApiCall("GET", "/api/calendar?start=\(start)&end=\(end)", nil)
                    .maps("events").compactMap(CalendarEvent.init(json:))
            },
            crm: {
                if let client = Services.get(PackagesClient.self) { return client }
                return Services.put(PackagesClient(transport: ApiPackagesTransport(api: api)))
            },
            agentControl: agentControl,
            web: { web }
        )
    }
}
