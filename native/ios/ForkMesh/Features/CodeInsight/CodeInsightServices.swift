import FMAccount
import FMApi
import FMBase
import FMCode
import Foundation

@MainActor
enum CodeInsightServices {
    private static var shared: CodeInsightSession?
    private static var sharedStore: ObjectIdentifier?

    static var session: CodeInsightSession {
        let store = LocalRepoServices.store
        let identity = store.map(ObjectIdentifier.init)
        if let shared, sharedStore == identity { return shared }
        let fetcher = CodeInsightFetcher(
            store: store, publisher: LocalRepoServices.publisher, session: { LocalRepoServices.session })
        let made = CodeInsightSession(prefs: Services.get(KeyValueStore.self), fetcher: fetcher)
        shared = made
        sharedStore = identity
        return made
    }

    static func prepareLocal(_ record: LocalRepoRecord) {
        session.preferredTarget = .local(record)
    }

    static func accountRepositories() async throws -> [Repository] {
        guard let api = Services.get(ApiService.self) else { return [] }
        return try await api.repositories()
    }
}
