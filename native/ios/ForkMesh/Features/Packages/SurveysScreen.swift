import FMApi
import FMBase
import FMPackages
import SwiftUI

struct SurveysDestination: View {
    let organization: String?
    let survey: String?

    var body: some View {
        PackagesGate(feature: "Surveys") { client in
            SurveysScreen(client: client, organization: organization, survey: survey)
        }
    }
}

struct SurveyOpen: Identifiable, Hashable {
    let id: String
}

struct SurveyDraftTarget: Identifiable {
    let id = UUID()
    let draft: SurveyDraft
}

struct SurveysScreen: View {
    @State private var model: SurveysModel
    @State private var query = ""
    @State private var filter: SurveyFilter = .all
    @State private var opened: SurveyOpen?
    @State private var editing: SurveyDraftTarget?
    @State private var notice: FmNotice?
    @State private var pendingSurvey: String?

    init(client: PackagesClient, organization: String?, survey: String?) {
        _model = State(initialValue: SurveysModel(client: client, organization: organization ?? ""))
        _pendingSurvey = State(initialValue: survey)
    }

    var body: some View {
        List {
            Section {
                OrganizationPicker(organizations: model.organizations, selection: $model.organization) {
                    await model.organizationChanged()
                }
                PackagesErrorBanner(text: model.errorText) { await model.refresh() }
                if notice != nil { FmNoticeLine($notice) }
            }
            if let board = model.board {
                SurveySummarySection(summary: board.summary)
                surveysSection(board)
            } else if model.loaded && !model.loading {
                if model.organizations.isEmpty {
                    PackageNotInstalledView(package: "Surveys", organizations: model.organizations)
                } else if model.needsInstall {
                    SurveysInstallPrompt(model: model)
                }
            }
        }
        .navigationTitle("Surveys")
        .searchable(text: $query, prompt: "Title, description or creator")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    editing = SurveyDraftTarget(draft: model.newDraft())
                } label: {
                    Label("New survey", systemImage: "plus")
                }
                .disabled(model.board == nil)
                .accessibilityIdentifier("surveys-new")
            }
        }
        .overlay {
            if model.loading && model.board == nil { ProgressView() }
        }
        .fmRefreshable { await model.load() }
        .task {
            await model.refresh()
            await openPending()
        }
        .navigationDestination(item: $opened) { target in
            SurveyDetailScreen(model: model, surveyId: target.id)
        }
        .sheet(item: $editing) { target in
            NavigationStack {
                SurveyEditor(model: model, draft: target.draft) { saved in
                    opened = SurveyOpen(id: saved.id)
                }
            }
        }
        .onChange(of: model.notice) { _, text in
            guard !text.isEmpty, opened == nil else { return }
            notice = FmNotice(text)
            model.notice = ""
        }
    }

    private func openPending() async {
        guard let id = pendingSurvey else { return }
        pendingSurvey = nil
        if model.board?.survey(id) != nil {
            opened = SurveyOpen(id: id)
        } else if await model.locate(id) != nil {
            opened = SurveyOpen(id: id)
        } else if model.errorText.isEmpty {
            model.errorText = "That survey was removed or you no longer have access."
        }
    }

    @ViewBuilder private func surveysSection(_ board: SurveyBoard) -> some View {
        let items = board.filtered(query: query, filter: filter)
        Section {
            if board.surveys.isEmpty {
                ContentUnavailableView {
                    Label("No surveys yet", systemImage: "list.clipboard")
                } description: {
                    Text("Build a survey from choice, text and rating questions, share its link, and read the answers as they arrive.")
                } actions: {
                    Button("New survey") { editing = SurveyDraftTarget(draft: model.newDraft()) }
                        .buttonStyle(.borderedProminent)
                        .tint(.green)
                }
            } else if items.isEmpty {
                Text("No surveys match.").foregroundStyle(.secondary)
            }
            ForEach(items) { survey in
                Button {
                    opened = SurveyOpen(id: survey.id)
                } label: {
                    SurveyRow(survey: survey)
                }
                .buttonStyle(.plain)
            }
        } header: {
            HStack {
                Text("Surveys")
                Spacer()
                Picker("Status", selection: $filter) {
                    ForEach(SurveyFilter.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .textCase(nil)
                .accessibilityIdentifier("surveys-filter")
            }
        }
    }
}

struct SurveysInstallPrompt: View {
    let model: SurveysModel

    var body: some View {
        Section {
            ContentUnavailableView {
                Label("Surveys is not installed for \(model.organization)", systemImage: "list.clipboard")
            } description: {
                Text(
                    model.canManage
                        ? "Install it to build surveys, share a private link anyone can answer, and read the results here."
                        : "Ask an owner or admin of \(model.organization) to install Surveys.")
            } actions: {
                if model.canManage {
                    Button {
                        Task {
                            if await model.install() { FmHaptics.commit() } else { FmHaptics.failure() }
                        }
                    } label: {
                        if model.working { ProgressView() } else { Text("Install for \(model.organization)") }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.working)
                    .accessibilityIdentifier("surveys-install")
                }
            }
        }
    }
}

struct SurveySummarySection: View {
    let summary: SurveySummary

    var body: some View {
        Section {
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], spacing: 8) {
                SurveyTile(
                    value: "\(summary.open)", label: "Open", detail: "Taking answers", icon: "dot.radiowaves.left.and.right",
                    tint: summary.open > 0 ? .green : .secondary)
                SurveyTile(
                    value: PackagesFormat.count(Int64(summary.responses)), label: "Responses",
                    detail: summary.responsesDetail, icon: "tray.full", tint: .blue)
                SurveyTile(
                    value: "\(summary.surveys)", label: "Surveys", detail: summary.surveysDetail, icon: "list.clipboard",
                    tint: .indigo)
                SurveyTile(
                    value: "\(summary.responsesLast7Days)", label: "Last 7 days", detail: "New responses",
                    icon: "calendar", tint: .teal)
            }
            .listRowInsets(EdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0))
            .listRowBackground(Color.clear)
        }
    }
}

struct SurveyTile: View {
    let value: String
    let label: String
    let detail: String
    let icon: String
    let tint: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Label(label, systemImage: icon).font(.caption).foregroundStyle(tint).lineLimit(1)
            Text(value).font(.headline.monospacedDigit()).lineLimit(2).minimumScaleFactor(0.6)
            if !detail.isEmpty {
                Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
            }
        }
        .frame(maxWidth: .infinity, minHeight: 70, alignment: .topLeading)
        .padding(10)
        .shellGlass(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

struct SurveyStatusBadge: View {
    let state: String

    var tint: Color {
        switch state {
        case "open": return .green
        case "scheduled": return .orange
        default: return .secondary
        }
    }

    var body: some View {
        Text(SurveyText.statusTitle(state))
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .foregroundStyle(tint)
            .background(tint.opacity(0.12), in: Capsule())
    }
}

struct SurveyRow: View {
    let survey: Survey

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(survey.displayTitle).font(.body.weight(.semibold)).lineLimit(2)
                Spacer()
                SurveyStatusBadge(state: survey.state)
            }
            if !survey.description.isEmpty {
                Text(survey.description).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            HStack {
                Text(survey.countsLine).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                Spacer()
                let when = survey.lastResponseAt > 0
                    ? "Last answer " + PackagesFormat.relative(survey.lastResponseAt)
                    : (survey.createdBy.isEmpty ? "" : "By \(survey.createdBy)")
                if !when.isEmpty {
                    Text(when).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
            }
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}
