// Wiring for the Agents area: the on-device session service (branch records
// under `agent/branch-records-v1`) and the keychain-backed provider store
// (`agent/credentials-v1`), created once and registered in `Services`.
// AppServices calls `register()` after LocalAI, so the Gemma manager is
// already there.

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMCode
import FMLocalAI
import FMNet
import Foundation

enum AgentsFeature {
    /// Creates and registers `AgentSessionService` (loaded) and
    /// `AgentCredentialStore`. Idempotent. Needs `KeyValueStore` and
    /// `SecureStore` registered; `HTTPClient` and `LocalModelManager` are used
    /// when present.
    @MainActor
    static func register() {
        if Services.get(AgentSessionService.self) == nil {
            let prefs: KeyValueStore = Services.get(KeyValueStore.self) ?? InMemoryKeyValueStore()
            let sessions: AgentSessionService = AgentSessionService(prefs: prefs)
            sessions.load()
            Services.put(sessions)
        }
        if Services.get(AgentCredentialStore.self) == nil {
            let secure: SecureStore = Services.get(SecureStore.self) ?? InMemorySecureStore()
            let store: AgentCredentialStore = AgentCredentialStore(
                secureStore: secure,
                http: Services.get(HTTPClient.self),
                localModels: Services.get(LocalModelManager.self)
            )
            Services.put(store)
        }
        if Services.get(RunnerAttentionModel.self) == nil, let api = Services.get(ApiService.self) {
            let attention: RunnerAttentionModel = RunnerAttentionModel(api: api)
            Services.put(attention)
            if let notifications = Services.get(NotificationService.self) {
                runnerAlertsToken = notifications.addListener {
                    Task { @MainActor in
                        await attention.noteNotifications(notifications.notifications)
                    }
                }
            }
        }
        if Services.get(RunnerAccountsModel.self) == nil, let api = Services.get(ApiService.self) {
            Services.put(RunnerAccountsModel(api: api))
        }
    }

    @MainActor static var runnerAlertsToken: ListenerToken?

    /// The registered session service, registering one if the shell has not
    /// (previews, a partial build).
    @MainActor
    static func sessions() -> AgentSessionService {
        if let existing = Services.get(AgentSessionService.self) {
            return existing
        }
        register()
        return Services.require(AgentSessionService.self)
    }

    @MainActor
    static func credentialStore() -> AgentCredentialStore {
        if let existing = Services.get(AgentCredentialStore.self) {
            return existing
        }
        register()
        return Services.require(AgentCredentialStore.self)
    }

    @MainActor
    static var session: AuthSession? {
        return Services.get(AuthService.self)?.session
    }

    /// Commits carry the signed-in person, not a placeholder.
    @MainActor
    static func author() -> AgentAuthor {
        guard let session = session else {
            return AgentAuthor.placeholder
        }
        return AgentAuthor.fromAccount(nodeName: session.nodeName, email: session.email)
    }

    /// The pull-request flow, when publishing and the signed inbox are wired.
    @MainActor
    static func pullRequests() -> PullRequestService? {
        guard let publisher = Services.get(PublishService.self), let inbox = Services.get(InboxService.self) else {
            return nil
        }
        return PullRequestService.live(publisher: publisher, inbox: inbox)
    }

    /// The on-device repositories, when this build has the store.
    @MainActor
    static func localRecords() -> (@MainActor () async -> [LocalRepoRecord])? {
        guard let store = Services.get(LocalRepoStore.self) else {
            return nil
        }
        return {
            try? await store.ensureLoaded()
            return store.records
        }
    }

    @MainActor
    static func localModels() -> LocalModelManager? {
        return Services.get(LocalModelManager.self)
    }
}
