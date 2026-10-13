import FMApi
import FMBase
import FMCode
import SwiftUI

@MainActor
enum CIRepoSections {
    static let sections: Set<RepoSection> = [.actions, .releases, .security, .mirrors, .artifacts]

    static func handles(_ section: RepoSection) -> Bool { sections.contains(section) }

    static func view(for section: RepoSection, repo: RepoRef) -> AnyView? {
        switch section {
        case .actions: return AnyView(RepoActionsScreen(repo: repo))
        case .releases: return AnyView(RepoReleasesScreen(repo: repo))
        case .security: return AnyView(RepoSecurityScreen(repo: repo))
        case .mirrors: return AnyView(RepoMirrorsScreen(repo: repo))
        case .artifacts: return AnyView(RepoArtifactsScreen(repo: repo))
        default: return nil
        }
    }

    static func webhooks(repo: RepoRef) -> AnyView { AnyView(RepoWebhooksScreen(repo: repo)) }

    static func pullChecks(repo: RepoRef, pull: Int) -> AnyView { AnyView(PullChecksView(repo: repo, pull: pull)) }
}

struct PullChecksView: View {
    @Environment(\.fmPalette) private var fm
    let pull: Int
    @State private var model: RepoActionsModel?
    @State private var dispatching: ActionsWorkflowSummary?

    init(repo: RepoRef, pull: Int) {
        self.pull = pull
        _model = State(initialValue: CIServices.actionsModel(repo))
    }

    var body: some View {
        Group {
            if let model = model {
                PullChecksList(model: model, pull: pull, dispatching: $dispatching)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
                    .onDisappear { model.stop() }
                    .sheet(item: $dispatching) { (workflow: ActionsWorkflowSummary) in
                        DispatchWorkflowSheet(model: model, workflow: workflow)
                    }
            } else {
                FmEmptyState(icon: "fact_check_outlined", title: "Checks are unavailable")
            }
        }
    }
}

struct PullChecksList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    let pull: Int
    @Binding var dispatching: ActionsWorkflowSummary?

    var body: some View {
        let checks: PullChecksSummary = model.checks(forPull: pull)
        List {
            Section {
                Text(checks.label).font(.headline)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(checks.label)
                if let notice = model.notice {
                    Text(notice.text).font(.caption).foregroundStyle(notice.good ? fm.success : fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(notice.text)
                }
            }
            if !checks.awaitingApproval.isEmpty && model.canControl {
                Section("Waiting for approval") {
                    ForEach(checks.awaitingApproval) { (run: ActionsRunSummary) in
                        HStack {
                            Text(run.displayName)
                            Spacer()
                            Button("Approve") {
                                Task {
                                    guard await CIAuthentication.confirm("Approve checks for pull request #\(pull)") else {
                                        return
                                    }
                                    await model.send(.approve, runId: run.id)
                                }
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(model.isPending(runId: run.id))
                        }
                    }
                }
            }
            Section("Checks") {
                if checks.runs.isEmpty && !model.loading {
                    Text("No workflow has run for this pull request yet.").foregroundStyle(fm.textSecondary)
                }
                ForEach(checks.runs) { (run: ActionsRunSummary) in
                    NavigationLink {
                        ActionRunScreen(model: model, runId: run.id)
                            .shellPageClearance()
                    } label: {
                        ActionRunRow(run: run, pending: model.isPending(runId: run.id))
                    }
                }
            }
            if !model.dispatchableWorkflows.isEmpty {
                Section("Run a workflow") {
                    ForEach(model.dispatchableWorkflows) { (workflow: ActionsWorkflowSummary) in
                        Button(workflow.displayName) { dispatching = workflow }
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
    }
}
