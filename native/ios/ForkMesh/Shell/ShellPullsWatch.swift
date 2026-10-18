import FMApi
import FMBase
import FMCode
import Foundation

@MainActor
final class ShellPullsWatch {
    static let shared = ShellPullsWatch()

    private let marker = NewPullsMarker(store: Services.get(KeyValueStore.self) ?? UserDefaultsStore())
    private var loading = false
    private var queued = false
    private var viewing: [String: Int] = [:]

    func ring() {
        ShellCounts.shared.pullsRings &+= 1
        Task { await refresh() }
    }

    func refresh() async {
        if loading {
            queued = true
            return
        }
        let counts: ShellCounts = ShellCounts.shared
        guard let repo = counts.currentRepo, let api = Services.get(ApiService.self) else { return }
        loading = true
        let pulls: [PublishedPull]? = try? await api.publishedPulls(repo.owner, repo.name)
        loading = false
        if let pulls, counts.currentRepo == repo { adopt(repo, pulls) }
        if queued {
            queued = false
            await refresh()
        }
    }

    func poll() async {
        while !Task.isCancelled {
            let every: TimeInterval? = NewPullsMarker.pollEvery(
                heat: ShellNavigatorCounts.heat, lowPower: ProcessInfo.processInfo.isLowPowerModeEnabled)
            try? await Task.sleep(for: .seconds(every ?? NewPullsMarker.warmPollEvery))
            if Task.isCancelled { return }
            if every != nil { await refresh() }
        }
    }

    func isCurrent(_ repo: RepoRef) -> Bool {
        ShellCounts.shared.currentRepo?.slug.lowercased() == repo.slug.lowercased()
    }

    func opened(_ repo: RepoRef) {
        viewing[repo.slug.lowercased(), default: 0] += 1
    }

    func closed(_ repo: RepoRef) {
        let slug: String = repo.slug.lowercased()
        let left: Int = (viewing[slug] ?? 0) - 1
        viewing[slug] = left > 0 ? left : nil
    }

    func viewed(_ repo: RepoRef, _ pulls: [PublishedPull]) {
        marker.markSeen(repo.slug, pulls: pulls)
        if isCurrent(repo) { adopt(repo, pulls) }
    }

    private func adopt(_ repo: RepoRef, _ pulls: [PublishedPull]) {
        let counts: ShellCounts = ShellCounts.shared
        let slug: String = repo.slug.lowercased()
        if viewing[slug] != nil { marker.markSeen(slug, pulls: pulls) }
        let fresh: Int = marker.newCount(slug, pulls: pulls)
        if counts.pullsSlug != slug || counts.pulls != pulls {
            counts.pulls = pulls
            counts.pullsSlug = slug
            counts.pullsRevision &+= 1
        }
        if counts.newPulls != fresh { counts.newPulls = fresh }
        guard counts.knowsCounts(of: repo) else { return }
        let open: Int = pulls.filter(PullStatusFilter.open.matches).count
        if counts.repoCount(.pulls) != open { counts.adopt(repo, counts: [.pulls: open]) }
    }
}
