import FMApi
import FMBase
import FMPackages
import SwiftUI

enum SurveyDetailTab: String, CaseIterable, Identifiable {
    case results
    case responses

    var id: String { rawValue }

    var title: String {
        switch self {
        case .results: return "Results"
        case .responses: return "Responses"
        }
    }
}

enum SurveyDetailSheet: Identifiable {
    case edit(SurveyDraft)
    case export(URL, rows: Int)

    var id: String {
        switch self {
        case let .edit(draft): return "edit:" + draft.id
        case let .export(url, _): return "export:" + url.lastPathComponent
        }
    }
}

struct SurveyDetailScreen: View {
    let model: SurveysModel
    @State private var shownId: String
    @State private var survey: Survey?
    @State private var tab: SurveyDetailTab = .results
    @State private var responses: [SurveyResponse]?
    @State private var errorText = ""
    @State private var notice: FmNotice?
    @State private var sheet: SurveyDetailSheet?
    @State private var confirmClose = false
    @State private var confirmDelete = false
    @State private var deletingResponse: SurveyResponse?
    @Environment(\.dismiss) private var dismiss

    init(model: SurveysModel, surveyId: String) {
        self.model = model
        _shownId = State(initialValue: surveyId)
        _survey = State(initialValue: model.board?.survey(surveyId))
    }

    var body: some View {
        List {
            if !errorText.isEmpty || notice != nil {
                Section {
                    PackagesErrorBanner(text: errorText) { await load() }
                    if notice != nil { FmNoticeLine($notice) }
                }
            }
            if let survey {
                headerSection(survey)
                if let url = survey.shareURL { linkSection(url, survey: survey) }
                actionsSection(survey)
                if survey.questions != nil {
                    PageActivitySection(activity: survey.activity, now: Int64(Date().timeIntervalSince1970 * 1000))
                }
                Section {
                    Picker("Show", selection: $tab) {
                        ForEach(SurveyDetailTab.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 4, leading: 0, bottom: 4, trailing: 0))
                    .accessibilityIdentifier("survey-tab")
                }
                switch tab {
                case .results: resultsSections(survey)
                case .responses: responsesSection(survey)
                }
            } else if errorText.isEmpty {
                ProgressView().frame(maxWidth: .infinity)
            }
        }
        .navigationTitle(survey?.displayTitle ?? "Survey")
        .navigationBarTitleDisplayMode(.inline)
        .task(id: shownId) {
            responses = nil
            await load()
        }
        .task(id: tab) {
            if tab == .responses && responses == nil { await loadResponses() }
        }
        .fmRefreshable {
            await load()
            if tab == .responses { await loadResponses() }
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button("Edit") { edit() }
                    .disabled(survey?.questions == nil || model.working)
                    .accessibilityIdentifier("survey-edit")
            }
        }
        .sheet(item: $sheet) { item in
            NavigationStack {
                sheetContent(item)
            }
        }
        .confirmationDialog(
            "Close \(survey?.displayTitle ?? "the survey")?", isPresented: $confirmClose, titleVisibility: .visible
        ) {
            Button("Close survey") { Task { await run { try await model.close($0) } } }
        } message: {
            Text("The link stops taking answers. You can reopen it later.")
        }
        .confirmationDialog(
            "Delete \(survey?.displayTitle ?? "the survey")?", isPresented: $confirmDelete, titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) { Task { await delete() } }
        } message: {
            Text("The survey and all \(survey?.responseCount ?? 0) responses are removed. This cannot be undone.")
        }
        .confirmationDialog(
            "Delete this response?",
            isPresented: Binding(get: { deletingResponse != nil }, set: { if !$0 { deletingResponse = nil } }),
            titleVisibility: .visible, presenting: deletingResponse
        ) { response in
            Button("Delete response", role: .destructive) { Task { await deleteResponse(response) } }
        } message: { _ in
            Text("Its answers leave the results. This cannot be undone.")
        }
    }

    @ViewBuilder private func sheetContent(_ item: SurveyDetailSheet) -> some View {
        switch item {
        case let .edit(draft):
            SurveyEditor(model: model, draft: draft) { saved in
                if saved.id == shownId {
                    survey = saved
                } else {
                    shownId = saved.id
                }
                showNotice()
            }
        case let .export(url, rows):
            SurveyExportSheet(url: url, rows: rows)
        }
    }

    private func edit() {
        guard let survey, survey.questions != nil else { return }
        sheet = .edit(SurveyDraft(editing: survey))
    }

    private func showNotice() {
        guard !model.notice.isEmpty else { return }
        notice = FmNotice(model.notice)
        model.notice = ""
    }

    private func load() async {
        do {
            survey = try await model.detail(shownId)
            errorText = ""
        } catch {
            errorText = "Could not open the survey: " + SurveyText.describe(error)
        }
    }

    private func loadResponses() async {
        guard let survey else { return }
        do {
            responses = try await model.responses(survey)
        } catch {
            errorText = "Could not load the responses: " + SurveyText.describe(error)
            if responses == nil { responses = [] }
        }
    }

    private func run(_ action: (Survey) async throws -> Survey) async {
        guard let survey else { return }
        do {
            let next = try await action(survey)
            FmHaptics.commit()
            errorText = ""
            showNotice()
            if next.id == shownId {
                self.survey = next.questions == nil ? self.survey.map { merged($0, next) } : next
                await load()
            } else {
                shownId = next.id
            }
        } catch {
            FmHaptics.failure()
            errorText = SurveyText.describe(error)
        }
    }

    private func merged(_ old: Survey, _ next: Survey) -> Survey {
        var out = next
        out.questions = old.questions
        out.results = old.results
        out.activity = old.activity
        return out
    }

    private func delete() async {
        guard let survey else { return }
        do {
            try await model.delete(survey)
            FmHaptics.commit(.delete)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "Could not delete the survey: " + SurveyText.describe(error)
        }
    }

    private func deleteResponse(_ response: SurveyResponse) async {
        guard let survey else { return }
        deletingResponse = nil
        do {
            _ = try await model.deleteResponse(response, of: survey)
            responses?.removeAll { $0.id == response.id }
            FmHaptics.commit(.delete)
            showNotice()
            await load()
        } catch {
            FmHaptics.failure()
            errorText = "Could not delete the response: " + SurveyText.describe(error)
        }
    }

    private func export() async {
        guard let survey else { return }
        do {
            let file = try await model.export(survey)
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(file.filename)
            try Data(file.csv.utf8).write(to: url, options: .atomic)
            sheet = .export(url, rows: file.rows)
            errorText = ""
        } catch {
            FmHaptics.failure()
            errorText = "Could not export the responses: " + SurveyText.describe(error)
        }
    }

    private func headerSection(_ survey: Survey) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(survey.displayTitle).font(.title3.weight(.semibold))
                    Spacer()
                    SurveyStatusBadge(state: survey.state)
                }
                if !survey.description.isEmpty {
                    Text(survey.description).font(.subheadline).foregroundStyle(.secondary).textSelection(.enabled)
                }
                Text(survey.countsLine).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                Text(survey.stateNote).font(.caption).foregroundStyle(.secondary)
                let settings = survey.settings.lines()
                if !settings.isEmpty {
                    Text(settings.joined(separator: " · ")).font(.caption).foregroundStyle(.secondary)
                }
                let by = [
                    survey.createdBy.isEmpty ? "" : "Created by \(survey.createdBy)",
                    survey.lastResponseAt > 0 ? "last answer \(PackagesFormat.relative(survey.lastResponseAt))" : "",
                ].filter { !$0.isEmpty }.joined(separator: " · ")
                if !by.isEmpty { Text(by).font(.caption2).foregroundStyle(.secondary) }
            }
        }
    }

    private func linkSection(_ url: URL, survey: Survey) -> some View {
        Section {
            ShareLink(item: url, subject: Text(survey.displayTitle)) {
                Label("Share survey link", systemImage: "square.and.arrow.up")
            }
            .accessibilityIdentifier("survey-share")
            Button {
                CrmClipboard.copy(url.absoluteString)
                notice = FmNotice("Survey link copied.")
            } label: {
                Label("Copy link", systemImage: "link")
            }
            Link(destination: url) {
                Label("Preview", systemImage: "safari")
            }
        } header: {
            Text("Answer link")
        } footer: {
            Text(survey.isTakingAnswers ? "Anyone with the link can answer, no account needed." : survey.stateNote)
        }
    }

    @ViewBuilder private func actionsSection(_ survey: Survey) -> some View {
        Section {
            if survey.canOpen {
                Button {
                    Task { await run { try await model.open($0) } }
                } label: {
                    Label(survey.openTitle, systemImage: survey.isDraft ? "paperplane" : "arrow.uturn.backward")
                }
                .disabled(survey.openNeedsQuestions)
                .accessibilityIdentifier("survey-open")
            }
            if survey.canClose {
                Button {
                    confirmClose = true
                } label: {
                    Label("Close survey", systemImage: "stop.circle")
                }
                .accessibilityIdentifier("survey-close")
            }
            Button {
                edit()
            } label: {
                Label("Edit questions and settings", systemImage: "pencil")
            }
            .disabled(survey.questions == nil)
            Button {
                Task { await run { try await model.duplicate($0) } }
            } label: {
                Label("Duplicate", systemImage: "plus.square.on.square")
            }
            Button {
                Task { await export() }
            } label: {
                Label("Export CSV", systemImage: "tablecells")
            }
            .disabled(survey.responseCount == 0)
            .accessibilityIdentifier("survey-export")
            if survey.canDelete {
                Button(role: .destructive) {
                    confirmDelete = true
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
        } footer: {
            if survey.openNeedsQuestions { Text("Add a question before opening the survey.") }
        }
        .disabled(model.working)
    }

    @ViewBuilder private func resultsSections(_ survey: Survey) -> some View {
        if let results = survey.results {
            if results.isEmpty {
                Section {
                    Text("Add questions to see results here.").font(.subheadline).foregroundStyle(.secondary)
                }
            }
            ForEach(Array(results.enumerated()), id: \.offset) { index, result in
                Section {
                    SurveyResultView(result: result, scale: survey.scale(for: result))
                } header: {
                    Text("\(index + 1). \(result.displayTitle)").textCase(nil)
                } footer: {
                    Text(result.countsLine)
                }
            }
        } else {
            Section { ProgressView().frame(maxWidth: .infinity) }
        }
    }

    @ViewBuilder private func responsesSection(_ survey: Survey) -> some View {
        let questions = survey.questions ?? []
        Section {
            if let responses {
                if responses.isEmpty {
                    Text(survey.isTakingAnswers ? "No responses yet. Share the link to get answers." : "No responses.")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                ForEach(responses) { response in
                    NavigationLink {
                        SurveyResponseScreen(survey: survey, response: response) {
                            deletingResponse = response
                        }
                    } label: {
                        SurveyResponseRow(response: response, questions: questions)
                    }
                    .swipeActions {
                        Button("Delete", role: .destructive) { deletingResponse = response }
                    }
                }
            } else {
                ProgressView().frame(maxWidth: .infinity)
            }
        } header: {
            Text(responses.map { SurveyText.plural($0.count, "response") } ?? "Responses")
        } footer: {
            if let responses, !responses.isEmpty { Text("Newest first. Swipe a response to delete it.") }
        }
    }
}

struct SurveyResultView: View {
    let result: SurveyResult
    let scale: Int

    var body: some View {
        switch result.type {
        case .single, .multiple:
            if result.optionBars.isEmpty {
                Text("No options.").font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(result.optionBars) { bar in
                SurveyBarRow(bar: bar)
            }
        case .rating:
            HStack(alignment: .firstTextBaseline) {
                Text(result.averageText).font(.title2.weight(.semibold).monospacedDigit())
                Text("average on 1 to \(scale)").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Image(systemName: "star.fill").foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
            ForEach(result.distributionBars) { bar in
                SurveyBarRow(bar: bar)
            }
        case .text, .paragraph:
            if result.answers.isEmpty {
                Text("No answers yet.").font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(Array(result.answers.enumerated()), id: \.offset) { _, answer in
                Text(answer).font(.subheadline).textSelection(.enabled)
            }
            if result.answered > result.answers.count && !result.answers.isEmpty {
                Text("Showing the \(result.answers.count) newest. Export the CSV for every answer.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

struct SurveyBarRow: View {
    let bar: SurveyBar

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(bar.label).font(.subheadline).lineLimit(2)
                Spacer()
                Text("\(bar.count) · \(bar.percent)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            ProgressView(value: bar.share)
                .tint(.accentColor)
                .accessibilityHidden(true)
        }
        .accessibilityElement(children: .combine)
    }
}

struct SurveyResponseRow: View {
    let response: SurveyResponse
    let questions: [SurveyQuestion]

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(response.preview(questions)).font(.subheadline).lineLimit(2)
            let when = PackagesFormat.relative(response.submittedAt)
            let count = "\(response.answeredCount(questions)) of \(questions.count) answered"
            Text([when, count].filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.caption).foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
    }
}

struct SurveyResponseScreen: View {
    let survey: Survey
    let response: SurveyResponse
    let onDelete: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                LabeledContent("Submitted", value: SurveyText.dateTime(response.submittedAt))
            }
            ForEach(Array((survey.questions ?? []).enumerated()), id: \.offset) { index, question in
                Section {
                    let answer = response.answer(question, scale: question.scale)
                    Text(answer.isEmpty ? "No answer" : answer)
                        .foregroundStyle(answer.isEmpty ? .secondary : .primary)
                        .textSelection(.enabled)
                } header: {
                    Text("\(index + 1). \(question.displayTitle)").textCase(nil)
                }
            }
            Section {
                Button(role: .destructive) {
                    dismiss()
                    onDelete()
                } label: {
                    Label("Delete response", systemImage: "trash")
                }
            }
        }
        .navigationTitle("Response")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct SurveyExportSheet: View {
    let url: URL
    let rows: Int
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 18) {
            Image(systemName: "tablecells.badge.ellipsis")
                .font(.system(size: 44))
                .foregroundStyle(Color.accentColor)
            Text("\(SurveyText.plural(rows, "response")) exported").font(.title3.bold())
            Text(url.lastPathComponent).font(.footnote).foregroundStyle(.secondary)
            ShareLink(item: url) {
                Label("Share CSV", systemImage: "square.and.arrow.up")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .accessibilityIdentifier("survey-export-share")
        }
        .padding(24)
        .navigationTitle("Export")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .presentationDetents([.medium])
    }
}
