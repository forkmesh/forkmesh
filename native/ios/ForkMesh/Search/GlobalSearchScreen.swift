import FMApi
import FMBase
import FMChat
import FMCode
import SwiftUI

@MainActor
enum GlobalSearchSources {
    nonisolated static let maxRepos = 3

    static func searchRepos(named: [RepoRef] = []) -> [RepoRef] {
        var seen: [RepoRef] = []
        let router = AppNavigator.shared?.router
        let candidates: [Route] = router.map { router in
            router.currentPath.reversed() + router.recents + AppTab.allCases.flatMap { router.path(for: $0).reversed() }
        } ?? []
        for repo in named + candidates.compactMap(\.repoRef) where !seen.contains(repo) {
            seen.append(repo)
        }
        return Array(seen.prefix(maxRepos))
    }

    static func localRepos() async -> [LocalRepositoryItem] {
        guard let store = LocalRepoServices.store else { return [] }
        await store.ensureLoaded()
        return store.records.map {
            LocalRepositoryItem(name: $0.name, description: $0.description, publishedAs: $0.publishedAs ?? "")
        }
    }

    static func standard() -> [GlobalSearchSource] {
        var sources: [GlobalSearchSource] = [
            DestinationsSearchSource(),
            PeopleSearchSource(accounts: { (name: String) in
                guard let direct = await MainActor.run(body: { Services.get(DirectMessageService.self) }) else { return [] }
                return await direct.searchUsers(name)
            }),
        ]
        sources.append(ChatSearchSource {
            await MainActor.run { () -> [String] in
                guard let chat = Services.get(ShellChatBridge.self) else { return [] }
                return [chat.currentConversation] + chat.unreadConversations
            }
        })
        guard let api = Services.get(ApiService.self) else {
            sources.append(RepositoriesSearchSource(load: { [] }, local: { await GlobalSearchSources.localRepos() }))
            return sources
        }
        let repos: @Sendable (String) async -> [RepoRef] = { (query: String) in
            let catalog = (try? await api.repositories()) ?? []
            let named = RepositoriesSearchSource.named(query, in: catalog, limit: GlobalSearchSources.maxRepos)
            return await MainActor.run { GlobalSearchSources.searchRepos(named: named) }
        }
        sources.append(RepositoriesSearchSource(
            load: { try await api.repositories() }, local: { await GlobalSearchSources.localRepos() }))
        sources.append(IssuesSearchSource(pulls: false, repos: repos) { (repo: RepoRef) in
            try await api.publishedIssues(repo.owner, repo.name).map {
                SearchableItem(number: $0.number, title: $0.title, body: $0.body, status: $0.status, isPull: false)
            }
        })
        sources.append(IssuesSearchSource(pulls: true, repos: repos) { (repo: RepoRef) in
            try await api.publishedPulls(repo.owner, repo.name).map {
                SearchableItem(number: $0.number, title: $0.title, body: $0.body, status: $0.status, isPull: true)
            }
        })
        sources.append(CommitsSearchSource(repos: repos) { (repo: RepoRef) in
            try await api.commits(repo.owner, repo.name).map {
                CommitItem(
                    sha: RepoDisplay.commitHash($0), subject: RepoDisplay.commitSubject($0),
                    author: RepoDisplay.commitAuthor($0))
            }
        })
        sources.append(CodeSearchSource(repos: repos) { (repo: RepoRef, query: String) in
            try await api.searchRepo(repo.owner, repo.name, query).code.map {
                CodeMatchItem(path: $0.path, line: $0.line, text: $0.text)
            }
        })
        return sources
    }
}

@MainActor
struct GlobalSearchScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: GlobalSearchModel
    @State private var searchPresented: Bool

    init(query: String, scope: GlobalSearchScope) {
        let saved: SavedSearchStore = AppNavigator.shared?.saved ?? SavedSearchStore(store: nil)
        PackageGateSync.publish()
        let model = GlobalSearchModel(sources: PackageGate.filter(GlobalSearchSources.standard()), saved: saved)
        model.query = query
        model.scope = scope
        _model = State(initialValue: model)
        _searchPresented = State(initialValue: query.isEmpty)
    }

    var body: some View {
        List {
            if model.isEmptyQuery {
                emptyState
            } else {
                results
            }
        }
        .listStyle(.insetGrouped)
        .searchable(
            text: $model.query, isPresented: $searchPresented, placement: .navigationBarDrawer(displayMode: .always),
            prompt: Text("Search everywhere"))
        .searchScopes($model.scope) {
            ForEach(GlobalSearchScope.allCases, id: \.self) { (scope: GlobalSearchScope) in
                Text(scope.title).tag(scope)
            }
        }
        .onChange(of: model.query) { (_: String, _: String) in model.queryChanged() }
        .onChange(of: model.scope) { (_: GlobalSearchScope, _: GlobalSearchScope) in model.queryChanged() }
        .onSubmit(of: .search) {
            model.saved.commit(query: model.query, scope: model.scope)
            Task { await model.run() }
        }
        .fmRefreshable { await model.run() }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                if !model.isEmptyQuery {
                    Button {
                        FmHaptics.selection()
                        model.saveCurrent()
                    } label: {
                        Label(model.isSaved ? "Saved" : "Save search", systemImage: model.isSaved ? "pin.fill" : "pin")
                    }
                    .disabled(model.isSaved)
                    .accessibilityIdentifier("search-save")
                }
            }
        }
        .navigationTitle("Search")
        .navigationBarTitleDisplayMode(.large)
        .task {
            if !model.isEmptyQuery {
                await model.run()
            }
        }
        .accessibilityIdentifier("global-search")
    }

    @ViewBuilder private var emptyState: some View {
        if model.saved.saved.isEmpty && model.saved.recent.isEmpty {
            ContentUnavailableView(
                "Search ForkMesh", systemImage: "magnifyingglass",
                description: Text("Repositories, issues, pull requests, code, people, chat and every screen."))
                .listRowBackground(Color.clear)
        }
        if !model.saved.saved.isEmpty {
            Section("Saved searches") {
                ForEach(model.saved.saved) { (search: SavedSearch) in
                    Button {
                        Task { await model.apply(search) }
                    } label: {
                        HStack {
                            Label(search.query, systemImage: "pin")
                            Spacer(minLength: FmSpace.x2)
                            Text(search.scope.title)
                                .fmText(.bodySmall, color: fm.textSecondary)
                            Text(verbatim: "\(search.lastCount)")
                                .fmText(.labelSmall, color: fm.textSecondary, tabular: true)
                                .accessibilityLabel(Text("\(search.lastCount) results last time"))
                        }
                        .frame(minHeight: 44)
                    }
                    .buttonStyle(.plain)
                    .swipeActions {
                        Button(role: .destructive) {
                            model.saved.remove(search)
                        } label: {
                            Label("Remove", systemImage: "trash")
                        }
                    }
                    .accessibilityAction(named: Text("Remove")) { model.saved.remove(search) }
                }
            }
        }
        if !model.saved.recent.isEmpty {
            Section {
                ForEach(model.saved.recent) { (recent: RecentSearch) in
                    Button {
                        Task { await model.apply(SavedSearch(query: recent.query, scope: recent.scope)) }
                    } label: {
                        Label(recent.query, systemImage: "clock.arrow.circlepath")
                            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    }
                    .buttonStyle(.plain)
                }
            } header: {
                HStack {
                    Text("Recent")
                    Spacer()
                    Button("Clear") { model.saved.clearRecent() }
                        .font(.footnote)
                }
            }
        }
    }

    @ViewBuilder private var results: some View {
        if model.buckets.isEmpty && !model.searching {
            ContentUnavailableView.search(text: model.query)
                .listRowBackground(Color.clear)
        }
        if model.searching && model.buckets.isEmpty {
            HStack {
                Spacer()
                ProgressView()
                Spacer()
            }
            .listRowBackground(Color.clear)
        }
        ForEach(model.buckets) { (bucket: SearchBucket) in
            Section {
                ForEach(bucket.hits) { (hit: SearchHit) in
                    NavigationLink(value: hit.route) {
                        hitRow(hit)
                    }
                    .simultaneousGesture(TapGesture().onEnded {
                        model.saved.commit(query: model.query, scope: model.scope)
                        AppNavigator.shared?.router.remember(hit.route)
                    })
                }
                if bucket.hasMore && model.expanded == nil {
                    Button("See all \(bucket.total)") {
                        Task { await model.expand(bucket.kind) }
                    }
                    .frame(minHeight: 44)
                }
            } header: {
                Text(bucket.kind.title)
            }
        }
        if model.expanded != nil {
            Button("Show every kind") {
                Task { await model.expand(nil) }
            }
            .frame(minHeight: 44)
        }
        if !model.failures.isEmpty {
            Section {
                HStack {
                    Label(
                        "Couldn't search \(model.failures.map(\.title).joined(separator: ", ").lowercased())",
                        systemImage: "exclamationmark.triangle")
                        .fmText(.bodySmall, color: fm.textSecondary)
                    Spacer()
                    Button("Retry") {
                        Task { await model.run() }
                    }
                }
            }
        }
    }

    private func hitRow(_ hit: SearchHit) -> some View {
        Label {
            VStack(alignment: .leading, spacing: 2) {
                Text(hit.title)
                    .fmText(.bodyMedium)
                    .lineLimit(1)
                if !hit.subtitle.isEmpty {
                    Text(hit.subtitle)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(2)
                }
            }
        } icon: {
            Image(systemName: hit.symbol)
                .foregroundStyle(fm.accent)
        }
        .frame(minHeight: 44)
        .accessibilityElement(children: .combine)
    }
}
