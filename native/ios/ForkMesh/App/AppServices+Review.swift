import FMApi
import FMBase
import SwiftUI

@MainActor
struct ReviewInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        let routes = services.routes
        let owner = "WS11"
        routes.register(.repos, by: owner) { _ in AnyView(ReposScreen()) }
        routes.register(.repo, by: owner) { route in
            guard case .repo(let repo, let section) = route else { return AnyView(EmptyView()) }
            if let section { return AnyView(RepoSectionScreen(repo: repo, section: section)) }
            return AnyView(RepoDetailScreen(ref: repo))
        }
        routes.register(.tree, by: owner) { route in
            guard case .tree(let repo, _, _) = route else { return AnyView(EmptyView()) }
            return AnyView(RepoSectionScreen(repo: repo, section: .code))
        }
        routes.register(.file, by: owner) { route in
            guard case .file(let repo, let ref, let path) = route else { return AnyView(EmptyView()) }
            return AnyView(RepoFileScreen(repo: ReviewEnv.repository(repo), path: path, ref: ref))
        }
        routes.register(.fileHistory, by: owner) { route in
            guard case .fileHistory(let repo, let ref, let path) = route else { return AnyView(EmptyView()) }
            return AnyView(FileHistoryScreen(repo: repo, ref: ref, path: path))
        }
        routes.register(.codeMap, by: owner) { route in
            guard case .codeMap(let repo, let ref) = route else { return AnyView(EmptyView()) }
            return AnyView(CodeMapScreen(repo: repo, ref: ref))
        }
        routes.register(.commits, by: owner) { route in
            guard case .commits(let repo, let ref) = route else { return AnyView(EmptyView()) }
            return AnyView(RepoSectionScreen(repo: repo, section: .commits, ref: ref))
        }
        routes.register(.commit, by: owner) { route in
            guard case .commit(let repo, let sha) = route else { return AnyView(EmptyView()) }
            return AnyView(RepoCommitDetailScreen(repo: ReviewEnv.repository(repo), commit: ["hash": .string(sha)]))
        }
        routes.register(.range, by: owner) { route in
            guard case .range(let repo, let base, let head) = route else { return AnyView(EmptyView()) }
            return AnyView(BranchDiffScreen(repo: ReviewEnv.repository(repo), base: base, head: head))
        }
        routes.register(.compare, by: owner) { route in
            guard case .compare(let repo, let base, let head) = route else { return AnyView(EmptyView()) }
            return AnyView(BranchDiffScreen(repo: ReviewEnv.repository(repo), base: base, head: head))
        }
        routes.register(.conflicts, by: owner) { route in
            guard case .conflicts(let repo, let branch) = route else { return AnyView(EmptyView()) }
            return AnyView(ConflictsScreen(repo: repo, branch: branch))
        }
        routes.register(.issue, by: owner) { route in
            guard case .issue(let repo, let number) = route else { return AnyView(EmptyView()) }
            return AnyView(IssueDetailScreen(repo: ReviewEnv.repository(repo), number: number))
        }
        routes.register(.newIssue, by: owner) { route in
            guard case .newIssue(let repo) = route else { return AnyView(EmptyView()) }
            return AnyView(NewIssueScreen(repo: ReviewEnv.repository(repo)))
        }
        routes.register(.board, by: owner) { route in
            guard case .board(let repo) = route else { return AnyView(EmptyView()) }
            return AnyView(IssueBoardScreen(repo: repo))
        }
        routes.register(.pull, by: owner) { route in
            guard case .pull(let repo, let number, let tab) = route else { return AnyView(EmptyView()) }
            return AnyView(PullRouteScreen(repo: repo, number: number, tab: tab))
        }
        routes.register(.review, by: owner) { route in
            guard case .review(let repo, let number) = route else { return AnyView(EmptyView()) }
            return AnyView(PullReviewScreen(repo: repo, number: number))
        }
        routes.register(.discussion, by: owner) { route in
            guard case .discussion(let repo, let number) = route else { return AnyView(EmptyView()) }
            return AnyView(DiscussionDetailScreen(repo: ReviewEnv.repository(repo), number: number))
        }
        routes.register(.repoSettings, by: owner) { route in
            guard case .repoSettings(let repo, let section) = route else { return AnyView(EmptyView()) }
            return AnyView(RepoSettingsScreen(repo: repo, section: section))
        }
    }
}
