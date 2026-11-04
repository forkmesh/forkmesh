import FMApi
import FMBase
import FMPackages
import SwiftUI

struct SurveyEditor: View {
    let model: SurveysModel
    @State private var draft: SurveyDraft
    @State private var saving = false
    @State private var errorText = ""
    @State private var showProblems = false
    let onSaved: (Survey) -> Void
    @Environment(\.dismiss) private var dismiss

    init(model: SurveysModel, draft: SurveyDraft, onSaved: @escaping (Survey) -> Void) {
        self.model = model
        _draft = State(initialValue: draft)
        self.onSaved = onSaved
    }

    var body: some View {
        Form {
            Section("Survey") {
                TextField("Title", text: $draft.title)
                    .accessibilityIdentifier("survey-title")
                TextField("Description (optional)", text: $draft.description, axis: .vertical).lineLimit(1...5)
                TextField("Thank-you message (optional)", text: $draft.thankYou, axis: .vertical).lineLimit(1...4)
            }
            questionsSection
            settingsSection
            if showProblems && !draft.problems.isEmpty {
                Section {
                    ForEach(draft.problems, id: \.self) { problem in
                        Label(problem, systemImage: "exclamationmark.circle").font(.footnote).foregroundStyle(.red)
                    }
                }
            }
            if !errorText.isEmpty {
                Section { PackagesErrorBanner(text: errorText) }
            }
        }
        .navigationTitle(draft.isNew ? "New survey" : "Edit survey")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    if draft.canSave {
                        Task { await save() }
                    } else {
                        showProblems = true
                        FmHaptics.failure()
                    }
                }
                .disabled(saving)
                .accessibilityIdentifier("survey-save")
            }
        }
        .interactiveDismissDisabled(saving)
    }

    private var questionsSection: some View {
        Section {
            if draft.questions.isEmpty {
                Text("Add at least one question before opening the survey.").font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(Array(draft.questions.enumerated()), id: \.element.id) { index, question in
                NavigationLink {
                    SurveyQuestionEditor(question: questionBinding(question.id), number: index + 1)
                } label: {
                    SurveyQuestionDraftRow(question: question, number: index + 1)
                }
                .swipeActions {
                    Button("Remove", role: .destructive) { draft.removeQuestion(question.id) }
                }
                .contextMenu {
                    Button("Move up", systemImage: "arrow.up") { draft.moveQuestion(question.id, by: -1) }
                        .disabled(!draft.canMove(question.id, by: -1))
                    Button("Move down", systemImage: "arrow.down") { draft.moveQuestion(question.id, by: 1) }
                        .disabled(!draft.canMove(question.id, by: 1))
                    Button("Remove", systemImage: "trash", role: .destructive) { draft.removeQuestion(question.id) }
                }
            }
            .onMove { draft.moveQuestions(from: $0, to: $1) }
            Menu {
                ForEach(SurveyQuestionType.allCases) { type in
                    Button(type.title, systemImage: type.symbol) { _ = draft.addQuestion(type) }
                }
            } label: {
                Label("Add question", systemImage: "plus.circle")
            }
            .disabled(!draft.canAddQuestion)
            .accessibilityIdentifier("survey-add-question")
        } header: {
            HStack {
                Text("Questions")
                Spacer()
                if draft.questions.count > 1 { EditButton().textCase(nil).font(.footnote) }
            }
        } footer: {
            Text("Up to \(SurveyLimits.questions) questions. Tap Edit to reorder, swipe one to remove it, or touch and hold to move it.")
        }
    }

    private var settingsSection: some View {
        Section {
            Toggle("One response per person", isOn: $draft.settings.oneResponsePerPerson)
            Toggle("Ping me on new responses", isOn: $draft.settings.notifyOnResponse)
            Toggle("Ping me when someone opens or starts it", isOn: $draft.settings.notifyOnVisit)
            Toggle("Shuffle choice options", isOn: $draft.settings.shuffleOptions)
            Toggle("Open at a set time", isOn: timeToggle(\.opensAt, offset: 0))
            if draft.settings.opensAt > 0 {
                DatePicker("Opens", selection: dateBinding(\.opensAt))
            }
            Toggle("Close at a set time", isOn: timeToggle(\.closesAt, offset: 7 * 86_400_000))
            if draft.settings.closesAt > 0 {
                DatePicker("Closes", selection: dateBinding(\.closesAt))
                if !draft.settings.datesAreValid {
                    Text("The closing time is before the opening time.").font(.caption).foregroundStyle(.red)
                }
            }
            LabeledContent("Response limit") {
                TextField("None", value: $draft.settings.maxResponses, format: .number)
                    .multilineTextAlignment(.trailing)
                    .keyboardType(.numberPad)
                    .accessibilityIdentifier("survey-max-responses")
            }
            if !draft.settings.maxResponsesIsValid {
                Text("Use 0 for no limit, or up to \(SurveyLimits.maxResponses).").font(.caption).foregroundStyle(.red)
            }
        } header: {
            Text("Settings")
        } footer: {
            Text("One response per person is checked by connection address; the answer page also remembers it in the browser.")
        }
    }

    private func questionBinding(_ id: String) -> Binding<SurveyQuestionDraft> {
        Binding(
            get: { draft.questions.first { $0.id == id } ?? SurveyQuestionDraft() },
            set: { next in
                if let index = draft.questions.firstIndex(where: { $0.id == id }) { draft.questions[index] = next }
            })
    }

    private func timeToggle(_ key: WritableKeyPath<SurveySettings, Int64>, offset: Int64) -> Binding<Bool> {
        Binding(
            get: { draft.settings[keyPath: key] > 0 },
            set: { on in
                let now = Int64((Date().timeIntervalSince1970 * 1000).rounded())
                let base = key == \SurveySettings.closesAt && draft.settings.opensAt > now ? draft.settings.opensAt : now
                draft.settings[keyPath: key] = on ? base + offset : 0
            })
    }

    private func dateBinding(_ key: WritableKeyPath<SurveySettings, Int64>) -> Binding<Date> {
        Binding(
            get: { Date(timeIntervalSince1970: TimeInterval(draft.settings[keyPath: key]) / 1000) },
            set: { draft.settings[keyPath: key] = max(1, Int64(($0.timeIntervalSince1970 * 1000).rounded())) })
    }

    private func save() async {
        saving = true
        defer { saving = false }
        do {
            let saved = try await model.save(draft)
            FmHaptics.commit()
            onSaved(saved)
            dismiss()
        } catch {
            FmHaptics.failure()
            errorText = "Could not save the survey: " + SurveyText.describe(error)
        }
    }
}

struct SurveyQuestionDraftRow: View {
    let question: SurveyQuestionDraft
    let number: Int

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: question.type.symbol).foregroundStyle(.secondary).frame(width: 20)
            VStack(alignment: .leading, spacing: 2) {
                Text("\(number). \(question.trimmedTitle.isEmpty ? "Untitled question" : question.trimmedTitle)")
                    .font(.subheadline)
                    .foregroundStyle(question.trimmedTitle.isEmpty ? .secondary : .primary)
                    .lineLimit(2)
                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                if let problem = question.problem {
                    Text(problem).font(.caption).foregroundStyle(.red)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }

    private var detail: String {
        var parts = [question.type.title]
        if question.type.isChoice { parts.append(SurveyText.plural(question.cleanedOptions.count, "option")) }
        if question.type == .rating { parts.append("1 to \(question.scale)") }
        if question.required { parts.append("Required") }
        return parts.joined(separator: " · ")
    }
}

struct SurveyQuestionEditor: View {
    @Binding var question: SurveyQuestionDraft
    let number: Int

    var body: some View {
        Form {
            Section {
                Picker("Type", selection: Binding(get: { question.type }, set: { question.setType($0) })) {
                    ForEach(SurveyQuestionType.allCases) { type in
                        Label(type.title, systemImage: type.symbol).tag(type)
                    }
                }
                .accessibilityIdentifier("survey-question-type")
                TextField("Question", text: $question.title, axis: .vertical).lineLimit(1...4)
                    .accessibilityIdentifier("survey-question-title")
                TextField("Help text (optional)", text: $question.help, axis: .vertical).lineLimit(1...4)
                Toggle("Required", isOn: $question.required)
            } footer: {
                if let problem = question.problem {
                    Text(problem).foregroundStyle(.red)
                }
            }
            if question.type.isChoice { optionsSection }
            if question.type == .rating {
                Section {
                    Stepper(
                        "Scale: 1 to \(question.scale)", value: $question.scale,
                        in: SurveyLimits.minScale...SurveyLimits.maxScale)
                } header: {
                    Text("Rating")
                } footer: {
                    Text("Respondents pick a whole number from 1 to \(question.scale).")
                }
            }
            if question.type.isText {
                Section {
                    Text(
                        question.type == .text
                            ? "One line, up to \(SurveyLimits.textAnswer) characters."
                            : "Long text, up to \(SurveyLimits.paragraphAnswer) characters.")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Question \(number)")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var optionsSection: some View {
        Section {
            ForEach($question.options) { $option in
                TextField("Option", text: $option.label)
            }
            .onDelete { offsets in
                question.options.remove(atOffsets: offsets)
            }
            .onMove { question.moveOptions(from: $0, to: $1) }
            Button {
                question.addOption()
            } label: {
                Label("Add option", systemImage: "plus.circle")
            }
            .disabled(!question.canAddOption)
            .accessibilityIdentifier("survey-add-option")
        } header: {
            HStack {
                Text("Options")
                Spacer()
                if question.options.count > 1 { EditButton().textCase(nil).font(.footnote) }
            }
        } footer: {
            if question.hasRepeatedOptions {
                Text("Repeated options are saved once.").foregroundStyle(.orange)
            } else {
                Text("\(SurveyLimits.minOptions) to \(SurveyLimits.options) options. Blank ones are left out.")
            }
        }
    }
}
