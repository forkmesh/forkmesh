import FMApi
import FMBase
import FMCode
import SwiftUI

@MainActor
enum RepoSectionRegistry {
    typealias Builder = @MainActor (RepoRef) -> AnyView

    private static var builders: [RepoSection: (owner: String, build: Builder)] = [:]

    static func register(_ section: RepoSection, by owner: String, _ build: @escaping Builder) {
        builders[section] = (owner, build)
    }

    static func view(_ section: RepoSection, _ repo: RepoRef) -> AnyView? {
        builders[section]?.build(repo)
    }

    static let owners: [RepoSection: String] = [.insights: "WS20", .quality: "WS20"]
}

struct RepoSectionScreen: View {
    let repo: RepoRef
    let section: RepoSection
    var ref: String? = nil
    var full: Repository? = nil
    @State private var composingIssue: Bool = false

    private var repository: Repository { full ?? ReviewEnv.repository(repo) }

    var body: some View {
        content
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .safeAreaInset(edge: .bottom, alignment: .trailing, spacing: 0) {
                if section == .issues {
                    FmFloatingActionButton(icon: "add", label: "New issue", accessibilityLabel: "New issue") {
                        Task {
                            if await AccountFeature.requireAccount(action: "open an issue") {
                                composingIssue = true
                            }
                        }
                    }
                    .padding(FmSpace.x4)
                }
            }
            .navigationDestination(isPresented: $composingIssue) {
                NewIssueScreen(repo: repository)
                    .shellPageClearance()
            }
    }

    private var title: String {
        RepoOverviewModel.title(of: section)
    }

    @ViewBuilder private var content: some View {
        switch section {
        case .code: RepoCodeTab(repo: repository)
        case .issues: RepoIssuesTab(repo: repository)
        case .pulls: RepoPullsTab(repo: repository)
        case .discussions: RepoDiscussionsTab(repo: repository)
        case .commits: RepoCommitsTab(repo: repository, ref: ref)
        case .about: RepoAboutTab(repo: repository)
        case .agents: RepoAgentsTab(repo: repository)
        case .worktrees: RepoWorktreesTab(repo: repository)
        case .branches: BranchesScreen(repo: repo)
        case .settings: RepoSettingsScreen(repo: repo, section: nil)
        case .actions, .releases, .security, .mirrors, .artifacts, .projects, .insights, .quality:
            if let view = CIRepoSections.view(for: section, repo: repo) ?? PlanningRepoSections.view(for: section, repo: repo)
                ?? RepoSectionRegistry.view(section, repo)
            {
                view
            } else {
                ContentUnavailableView(
                    "Coming soon", systemImage: "hammer",
                    description: Text("\(title) is not in this version of the app yet."))
            }
        }
    }
}
