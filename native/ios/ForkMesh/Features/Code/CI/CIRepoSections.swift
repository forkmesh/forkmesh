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
    @State private var runner: PullChecksModel?
    @State private var dispatching: ActionsWorkflowSummary?

    init(repo: RepoRef, pull: Int) {
        self.pull = pull
        _model = State(initialValue: CIServices.actionsModel(repo))
        _runner = State(initialValue: CIServices.pullChecksModel(repo, number: pull))
    }

    var body: some View {
        Group {
            if let model = model {
                PullChecksList(model: model, runner: runner, pull: pull, dispatching: $dispatching)
                    .task { await reload(model) }
                    .fmRefreshable { await reload(model) }
                    .onDisappear { model.stop() }
                    .sheet(item: $dispatching) { (workflow: ActionsWorkflowSummary) in
                        DispatchWorkflowSheet(model: model, workflow: workflow)
                    }
            } else {
                FmEmptyState(icon: "fact_check_outlined", title: "Checks are unavailable")
            }
        }
    }

    private func reload(_ model: RepoActionsModel) async {
        await model.load()
        await runner?.load()
    }
}

struct PullChecksList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    let runner: PullChecksModel?
    let pull: Int
    @Binding var dispatching: ActionsWorkflowSummary?

    private var watching: Bool {
        model.hasActiveRuns || model.needsFollowUp || (runner?.hasActiveRuns ?? false)
    }

    var body: some View {
        List {
            if let runner = runner, runner.isVisible, let checks = runner.checks {
                PullRunnerChecksSections(model: model, runner: runner, checks: checks, pull: pull)
            } else {
                summarySections
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
        .task(id: watching) {
            guard watching else {
                return
            }
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(RepoActionsModel.followUpDelaySeconds * 1_000_000_000))
                if Task.isCancelled {
                    return
                }
                await model.load()
                await runner?.load()
            }
        }
    }

    @ViewBuilder
    private var summarySections: some View {
        let checks: PullChecksSummary = model.checks(forPull: pull)
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
    }
}

struct PullRunnerChecksSections: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoActionsModel
    let runner: PullChecksModel
    let checks: PullChecks
    let pull: Int

    var body: some View {
        let approval: PullChecksApproval = checks.approval
        Section {
            Text(checks.summary.label).font(.headline)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(checks.summary.label)
            if !checks.head.isEmpty {
                FmKeyValueRow(label: "Commit", value: checks.shortHead)
            }
            if approval.required {
                Label("Waiting for a maintainer to approve", systemImage: "hand.raised")
                    .foregroundStyle(fm.warning)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !approval.reasonLabel.isEmpty {
                    Text(approval.reasonLabel).font(.caption).foregroundStyle(fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(approval.reasonLabel)
                }
            }
            if !approval.approvedBy.isEmpty {
                Label("Approved by @\(approval.approvedBy)", systemImage: "checkmark.shield")
                    .foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !approval.trustLabel.isEmpty {
                Text(approval.trustLabel).font(.caption).foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if let notice = runner.notice {
                Text(notice.text).font(.caption).foregroundStyle(notice.good ? fm.success : fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(notice.text)
            }
            if let error = runner.error {
                Text(error).font(.caption).foregroundStyle(fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error)
            }
            if runner.busy != nil {
                ProgressView().frame(maxWidth: .infinity)
            }
            ForEach(runner.actions) { (action: PullChecksAction) in
                actionButton(action, author: approval.author)
            }
        } footer: {
            if checks.previousHeads > 0 && approval.required {
                Text("An approval covers only commit \(checks.shortHead). A newer push waits for approval again.")
            }
        }
        Section("Checks") {
            if checks.runs.isEmpty {
                Text(checks.configured ? "No checks have run for this commit yet." : "No workflow in .forkmesh/ runs on pull requests.")
                    .foregroundStyle(fm.textSecondary)
            }
            ForEach(checks.runs) { (run: ActionsRunSummary) in
                NavigationLink {
                    ActionRunScreen(model: model, runId: run.id)
                        .shellPageClearance()
                } label: {
                    ActionRunRow(run: run, pending: false)
                }
            }
        }
    }

    @ViewBuilder
    private func actionButton(_ action: PullChecksAction, author: String) -> some View {
        let label: String = action.label(author: author)
        if action == .approve {
            Button(label) { perform(action, label: label) }
                .buttonStyle(.borderedProminent)
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityIdentifier("pull-checks-approve")
        } else {
            Button(label, role: action.isDestructive ? .destructive : nil) { perform(action, label: label) }
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityIdentifier("pull-checks-\(action.rawValue)")
        }
    }

    private func perform(_ action: PullChecksAction, label: String) {
        let head: String = checks.shortHead
        Task {
            if action.needsAuthentication {
                let reason: String = action == .approve
                    ? "Approve checks for pull request #\(pull) at \(head)"
                    : label
                guard await CIAuthentication.confirm(reason) else {
                    return
                }
            }
            await runner.perform(action)
            await model.load()
        }
    }
}
