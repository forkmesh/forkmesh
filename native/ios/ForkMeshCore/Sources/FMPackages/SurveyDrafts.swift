import FMBase
import Foundation

public struct SurveyOptionDraft: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String

    public init(id: String = UUID().uuidString, label: String = "") {
        self.id = id
        self.label = label
    }
}

public struct SurveyQuestionDraft: Equatable, Sendable, Identifiable {
    public var id: String
    public var questionId: String
    public var type: SurveyQuestionType
    public var title: String
    public var help: String
    public var required: Bool
    public var options: [SurveyOptionDraft]
    public var scale: Int

    public init(type: SurveyQuestionType = .single) {
        id = UUID().uuidString
        questionId = ""
        self.type = type
        title = ""
        help = ""
        required = false
        options = type.isChoice ? [SurveyOptionDraft(), SurveyOptionDraft()] : []
        scale = SurveyLimits.defaultScale
    }

    public init(_ question: SurveyQuestion) {
        id = UUID().uuidString
        questionId = SurveyQuestion.isQuestionId(question.id) ? question.id : ""
        type = question.type
        title = question.title
        help = question.help
        required = question.required
        options = question.options.map { SurveyOptionDraft(label: $0) }
        if type.isChoice && options.count < SurveyLimits.minOptions {
            options += (options.count..<SurveyLimits.minOptions).map { _ in SurveyOptionDraft() }
        }
        scale = type == .rating ? SurveyQuestion.clampScale(question.scale) : SurveyLimits.defaultScale
    }

    public var trimmedTitle: String { title.trimmingCharacters(in: .whitespacesAndNewlines) }

    public var cleanedOptions: [String] {
        var seen = Set<String>()
        var out: [String] = []
        for option in options {
            let label = option.label.trimmingCharacters(in: .whitespacesAndNewlines)
            if label.isEmpty || seen.contains(label) { continue }
            seen.insert(label)
            out.append(label)
        }
        return out
    }

    public var hasRepeatedOptions: Bool {
        let labels = options.map { $0.label.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        return Set(labels).count != labels.count
    }

    public var canAddOption: Bool { type.isChoice && options.count < SurveyLimits.options }

    public var problem: String? {
        if trimmedTitle.isEmpty { return "Give the question a title." }
        if trimmedTitle.count > SurveyLimits.questionTitle {
            return "Keep the question under \(SurveyLimits.questionTitle) characters."
        }
        if help.count > SurveyLimits.questionHelp { return "Keep the help text under \(SurveyLimits.questionHelp) characters." }
        if type.isChoice {
            let count = cleanedOptions.count
            if count < SurveyLimits.minOptions || count > SurveyLimits.options {
                return "Give it \(SurveyLimits.minOptions) to \(SurveyLimits.options) different options."
            }
            if cleanedOptions.contains(where: { $0.count > SurveyLimits.optionLabel }) {
                return "Keep each option under \(SurveyLimits.optionLabel) characters."
            }
        }
        if type == .rating && !(SurveyLimits.minScale...SurveyLimits.maxScale).contains(scale) {
            return "Pick a scale of \(SurveyLimits.minScale) to \(SurveyLimits.maxScale)."
        }
        return nil
    }

    public var isValid: Bool { problem == nil }

    public mutating func setType(_ next: SurveyQuestionType) {
        type = next
        if next.isChoice && options.count < SurveyLimits.minOptions {
            options += (options.count..<SurveyLimits.minOptions).map { _ in SurveyOptionDraft() }
        }
        if next == .rating { scale = SurveyQuestion.clampScale(scale) }
    }

    public mutating func addOption() {
        guard canAddOption else { return }
        options.append(SurveyOptionDraft())
    }

    public mutating func removeOption(_ id: String) {
        options.removeAll { $0.id == id }
    }

    public mutating func moveOptions(from source: IndexSet, to destination: Int) {
        options = SurveyDraft.moved(options, from: source, to: destination)
    }

    public var body: JSONObject {
        var body: JSONObject = [
            "type": .string(type.rawValue),
            "title": .string(trimmedTitle),
            "help": .string(help.trimmingCharacters(in: .whitespacesAndNewlines)),
            "required": .bool(required),
            "options": .array(type.isChoice ? cleanedOptions.map { .string($0) } : []),
            "scale": .int(type == .rating ? Int64(SurveyQuestion.clampScale(scale)) : 0),
        ]
        if SurveyQuestion.isQuestionId(questionId) { body["id"] = .string(questionId) }
        return body
    }
}

public struct SurveyDraft: Equatable, Sendable {
    public var id: String
    public var title: String
    public var description: String
    public var thankYou: String
    public var questions: [SurveyQuestionDraft]
    public var settings: SurveySettings

    public init() {
        id = ""
        title = ""
        description = ""
        thankYou = ""
        questions = [SurveyQuestionDraft(type: .single)]
        settings = SurveySettings()
    }

    public init(editing survey: Survey) {
        id = survey.id
        title = survey.title
        description = survey.description
        thankYou = survey.thankYou
        questions = (survey.questions ?? []).map(SurveyQuestionDraft.init)
        settings = survey.settings
    }

    public var isNew: Bool { id.isEmpty }

    public var trimmedTitle: String { title.trimmingCharacters(in: .whitespacesAndNewlines) }

    public var canAddQuestion: Bool { questions.count < SurveyLimits.questions }

    public var problems: [String] {
        var out: [String] = []
        if trimmedTitle.isEmpty { out.append("Give the survey a title.") }
        if questions.count > SurveyLimits.questions { out.append("A survey holds up to \(SurveyLimits.questions) questions.") }
        for (index, question) in questions.enumerated() {
            if let problem = question.problem { out.append("Question \(index + 1): \(problem)") }
        }
        if !settings.datesAreValid { out.append("The closing time is before the opening time.") }
        if !settings.maxResponsesIsValid {
            out.append("The response limit is 0 for none, or up to \(SurveyLimits.maxResponses).")
        }
        return out
    }

    public var canSave: Bool { problems.isEmpty }

    @discardableResult
    public mutating func addQuestion(_ type: SurveyQuestionType = .single) -> String? {
        guard canAddQuestion else { return nil }
        let question = SurveyQuestionDraft(type: type)
        questions.append(question)
        return question.id
    }

    public mutating func removeQuestion(_ id: String) {
        questions.removeAll { $0.id == id }
    }

    public mutating func moveQuestions(from source: IndexSet, to destination: Int) {
        questions = SurveyDraft.moved(questions, from: source, to: destination)
    }

    public mutating func moveQuestion(_ id: String, by offset: Int) {
        guard let index = questions.firstIndex(where: { $0.id == id }) else { return }
        let target = index + offset
        guard questions.indices.contains(target) else { return }
        questions.swapAt(index, target)
    }

    public func canMove(_ id: String, by offset: Int) -> Bool {
        guard let index = questions.firstIndex(where: { $0.id == id }) else { return false }
        return questions.indices.contains(index + offset)
    }

    static func moved<Item>(_ items: [Item], from source: IndexSet, to destination: Int) -> [Item] {
        let picked = source.filter { items.indices.contains($0) }
        guard !picked.isEmpty else { return items }
        let before = picked.filter { $0 < destination }.count
        var rest: [Item] = []
        for (index, item) in items.enumerated() where !picked.contains(index) { rest.append(item) }
        let insertAt = min(rest.count, max(0, destination - before))
        rest.insert(contentsOf: picked.map { items[$0] }, at: insertAt)
        return rest
    }

    public var body: JSONObject {
        [
            "title": .string(trimmedTitle),
            "description": .string(description.trimmingCharacters(in: .whitespacesAndNewlines)),
            "thankYou": .string(thankYou.trimmingCharacters(in: .whitespacesAndNewlines)),
            "questions": .array(questions.map { .object($0.body) }),
            "settings": .object(settings.body),
        ]
    }
}
