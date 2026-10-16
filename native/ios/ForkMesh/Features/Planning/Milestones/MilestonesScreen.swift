import FMApi
import FMBase
import FMPlanning
import SwiftUI

struct MilestonesScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoPlanningModel

    var body: some View {
        List {
            Section {
                Toggle("Show closed", isOn: Binding(get: { model.showClosed }, set: { model.showClosed = $0 }))
            } footer: {
                Text("Milestones are defined in the repository and edited on the desktop; issues pick one when they are filed or edited.")
            }
            if model.shownMilestones.isEmpty && !model.loading {
                Text("No milestones.").foregroundStyle(fm.textSecondary)
            }
            ForEach(model.shownMilestones) { (milestone: RepoMilestone) in
                Section {
                    HStack {
                        Text(milestone.title).font(.headline)
                        Spacer()
                        if milestone.dueMs > 0 {
                            Text("Due \(PlanningFormat.day(milestone.dueMs, zone: .current, style: .medium))")
                                .font(.caption)
                                .foregroundStyle(fm.textSecondary)
                        }
                    }
                    if !milestone.description.isEmpty {
                        Text(milestone.description).font(.callout).foregroundStyle(fm.textSecondary)
                    }
                    PlanningProgressBar(progress: model.progress(milestone))
                    ForEach(model.issues(in: milestone)) { (issue: Issue) in
                        NavigationLink {
                            RouteDestinationView(route: .issue(model.repo, number: issue.number))
                                .shellPageClearance()
                        } label: {
                            PlanningIssueRow(issue: issue)
                        }
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .fmLoading(model.loading)
        .navigationTitle("Milestones")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            if model.milestones.isEmpty && model.projects.isEmpty {
                await model.load()
            }
        }
    }
}
