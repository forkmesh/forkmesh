import FMApi
import FMBase
import FMCode
import SwiftUI

struct PullRouteScreen: View {
    let repo: RepoRef
    let number: Int
    @State private var tab: PullTab
    @State private var queue = MergeQueueFlow()

    init(repo: RepoRef, number: Int, tab: PullTab) {
        self.repo = repo
        self.number = number
        _tab = State(initialValue: tab)
    }

    var body: some View {
        VStack(spacing: 0) {
            Picker("Section", selection: $tab) {
                Text("Conversation").tag(PullTab.conversation)
                Text("Commits").tag(PullTab.commits)
                Text("Files").tag(PullTab.files)
                Text("Checks").tag(PullTab.checks)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            if tab != .conversation {
                CollabNoticeLine(horizontalPadding: 16)
            }
            switch tab {
            case .conversation:
                PullDetailScreen(repo: ReviewEnv.repository(repo), number: number)
            case .files:
                PullReviewScreen(repo: repo, number: number)
            case .commits:
                PullCommitsView(repo: repo, number: number)
            case .checks:
                CIRepoSections.pullChecks(repo: repo, pull: number)
            }
        }
        .mergeQueueConfirmation(queue)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    NavigationLink(value: Route.review(repo, number: number)) {
                        Label("Review changes", systemImage: "text.bubble")
                    }
                    Button {
                        Task {
                            await queue.prepare(MergeQueueRequest(owner: repo.owner, name: repo.name, action: .enqueue, number: number))
                        }
                    } label: {
                        Label("Add to desktop merge queue", systemImage: "arrow.triangle.merge")
                    }
                    .disabled(ReviewEnv.mergeQueue == nil)
                    Button {
                        collabCopy("\(repo.slug)#\(number)", label: "Reference")
                    } label: {
                        Label("Copy reference", systemImage: "number")
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("More")
            }
        }
    }
}

struct PullCommitsView: View {
    let repo: RepoRef
    let number: Int
    @State private var record: PullReviewRecord?
    @State private var error: String?

    var body: some View {
        Group {
            if let record {
                let base = record.creationBaseOid.isEmpty ? record.base : record.creationBaseOid
                let head = record.creationHeadOid.isEmpty ? record.head : record.creationHeadOid
                if base.isEmpty || head.isEmpty {
                    ContentUnavailableView {
                        Label("No commit range", systemImage: "point.3.connected.trianglepath.dotted")
                    } description: {
                        Text("This pull request carries a patch, not a branch.")
                    } actions: {
                        FmMessageTools("This pull request carries a patch, not a branch.")
                    }
                } else {
                    BranchDiffScreen(repo: ReviewEnv.repository(repo), base: base, head: head, title: "PR #\(number)")
                }
            } else if let error {
                ContentUnavailableView {
                    Label("Could not load commits", systemImage: "exclamationmark.triangle")
                } description: {
                    Text(error)
                } actions: {
                    FmMessageTools(error)
                }
            } else {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .task {
            do {
                record = try await ReviewLedger(reader: ReviewEnv.api, owner: repo.owner, name: repo.name).load(number)
            } catch {
                self.error = "\(error)"
            }
        }
    }
}
