import FMApi
import FMBase
import FMPlanning
import SwiftUI

@MainActor
enum PlanningRepoSections {
    static func view(for section: RepoSection, repo: RepoRef) -> AnyView? {
        section == .projects ? AnyView(RepoProjectsScreen(repo: repo)) : nil
    }
}

struct PlanningProgressBar: View {
    @Environment(\.fmPalette) private var fm
    let progress: PlanningProgress

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ProgressView(value: progress.fraction)
                .tint(progress.fraction >= 1 ? fm.success : fm.accent)
            Text(progress.label).font(.caption).foregroundStyle(fm.textSecondary)
        }
    }
}

struct RepoProjectsScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.horizontalSizeClass) private var sizeClass
    @State private var model: RepoPlanningModel?

    init(repo: RepoRef) {
        if let api = PlanningServices.api {
            _model = State(initialValue: RepoPlanningModel(client: api, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                List {
                    Section {
                        Toggle("Show closed", isOn: Binding(get: { model.showClosed }, set: { model.showClosed = $0 }))
                        NavigationLink {
                            MilestonesScreen(model: model)
                                .shellPageClearance()
                        } label: {
                            Label("Milestones (\(model.shownMilestones.count))", systemImage: "flag")
                        }
                    }
                    if let span = model.timeline, sizeClass == .regular {
                        Section("Timeline") {
                            ProjectGantt(projects: model.shownProjects, span: span)
                        }
                    }
                    Section("Projects") {
                        if model.shownProjects.isEmpty && !model.loading {
                            Text("No projects. Projects are created on the desktop.").foregroundStyle(fm.textSecondary)
                        }
                        ForEach(model.shownProjects) { (project: RepoProject) in
                            NavigationLink {
                                ProjectDetailScreen(model: model, number: project.number)
                                    .shellPageClearance()
                            } label: {
                                ProjectRow(project: project, progress: model.progress(project), span: model.timeline)
                            }
                        }
                    }
                }
                .listStyle(.insetGrouped)
                .overlay {
                    if model.loading && model.projects.isEmpty {
                        ProgressView()
                    }
                }
                .task { await model.load() }
                .fmRefreshable { await model.load(refresh: true) }
            } else {
                FmEmptyState(icon: "view_timeline_outlined", title: "Projects are unavailable")
            }
        }
        .navigationTitle("Projects")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct ProjectRow: View {
    @Environment(\.fmPalette) private var fm
    let project: RepoProject
    let progress: PlanningProgress
    let span: (start: Int64, end: Int64)?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("#\(project.number) \(project.title)").font(.body.weight(.medium))
                Spacer()
                FmStatusBadge(label: project.status.capitalized, color: project.status == "closed" ? fm.textSecondary : fm.accent)
            }
            if project.hasDates {
                Text("\(PlanningFormat.day(project.startMs, zone: .current, style: .medium)) – \(PlanningFormat.day(project.endMs, zone: .current, style: .medium))")
                    .font(.caption)
                    .foregroundStyle(fm.textSecondary)
                if let span = span, let bar = PlanningRecords.bar(project, in: span) {
                    GeometryReader { (proxy: GeometryProxy) in
                        Capsule()
                            .fill(fm.accent.opacity(0.6))
                            .frame(width: proxy.size.width * bar.width, height: 6)
                            .offset(x: proxy.size.width * bar.offset)
                    }
                    .frame(height: 6)
                }
            }
            PlanningProgressBar(progress: progress)
        }
        .padding(.vertical, 2)
    }
}

struct ProjectGantt: View {
    @Environment(\.fmPalette) private var fm
    let projects: [RepoProject]
    let span: (start: Int64, end: Int64)

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(PlanningFormat.day(span.start, zone: .current, style: .medium))
                Spacer()
                Text(PlanningFormat.day(span.end, zone: .current, style: .medium))
            }
            .font(.caption)
            .foregroundStyle(fm.textSecondary)
            ForEach(projects.filter(\.hasDates)) { (project: RepoProject) in
                HStack(spacing: 8) {
                    Text(project.title).font(.caption).lineLimit(1).frame(width: 140, alignment: .leading)
                    GeometryReader { (proxy: GeometryProxy) in
                        if let bar = PlanningRecords.bar(project, in: span) {
                            RoundedRectangle(cornerRadius: 3)
                                .fill(project.status == "closed" ? fm.textTertiary : fm.accent)
                                .frame(width: proxy.size.width * bar.width, height: 14)
                                .offset(x: proxy.size.width * bar.offset)
                        }
                    }
                    .frame(height: 14)
                }
            }
        }
        .padding(.vertical, 4)
    }
}

struct ProjectDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoPlanningModel
    let number: Int

    var body: some View {
        List {
            if let project = model.project(number) {
                Section {
                    Text(project.title).font(.title3.weight(.semibold))
                    FmKeyValueRow(label: "Status", value: project.status.capitalized)
                    if project.hasDates {
                        FmKeyValueRow(label: "Starts", value: PlanningFormat.day(project.startMs, zone: .current, style: .medium))
                        FmKeyValueRow(label: "Ends", value: PlanningFormat.day(project.endMs, zone: .current, style: .medium))
                    }
                    if !project.milestone.isEmpty {
                        FmKeyValueRow(label: "Milestone", value: project.milestone)
                    }
                    if !project.authorName.isEmpty {
                        FmKeyValueRow(label: "Author", value: "@\(project.authorName)")
                    }
                    PlanningProgressBar(progress: model.progress(project))
                }
                if !project.body.isEmpty {
                    Section("Description") {
                        Text(project.body).textSelection(.enabled)
                    }
                }
                Section("Issues") {
                    let issues: [Issue] = model.linkedIssues(project)
                    if issues.isEmpty {
                        Text("No linked issues.").foregroundStyle(fm.textSecondary)
                    }
                    ForEach(issues) { (issue: Issue) in
                        NavigationLink {
                            RouteDestinationView(route: .issue(model.repo, number: issue.number))
                                .shellPageClearance()
                        } label: {
                            PlanningIssueRow(issue: issue)
                        }
                    }
                }
            } else {
                Text("This project was not found.").foregroundStyle(fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("This project was not found.")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Project #\(number)")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            if model.projects.isEmpty {
                await model.load()
            }
        }
    }
}

struct PlanningIssueRow: View {
    @Environment(\.fmPalette) private var fm
    let issue: Issue

    var body: some View {
        HStack {
            Image(systemName: issue.isOpen ? "circle" : "checkmark.circle.fill")
                .foregroundStyle(issue.isOpen ? fm.success : fm.textSecondary)
            Text("#\(issue.number) \(issue.title)").lineLimit(2)
        }
    }
}

struct RepoPlanningDestination: View {
    let repo: RepoRef
    let projectNumber: Int?
    @State private var model: RepoPlanningModel?

    init(repo: RepoRef, projectNumber: Int?) {
        self.repo = repo
        self.projectNumber = projectNumber
        if let api = PlanningServices.api {
            _model = State(initialValue: RepoPlanningModel(client: api, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            if let number = projectNumber {
                ProjectDetailScreen(model: model, number: number)
            } else {
                MilestonesScreen(model: model)
            }
        } else {
            FmEmptyState(icon: "view_timeline_outlined", title: "Projects are unavailable")
        }
    }
}
