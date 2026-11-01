import FMBase
import Foundation

public enum SurveyLimits {
    public static let questions = 50
    public static let questionTitle = 300
    public static let questionHelp = 1000
    public static let options = 30
    public static let optionLabel = 200
    public static let minOptions = 2
    public static let minScale = 3
    public static let maxScale = 10
    public static let defaultScale = 5
    public static let maxResponses = 10_000
    public static let textAnswer = 500
    public static let paragraphAnswer = 5000
}

public enum SurveyQuestionType: String, CaseIterable, Sendable, Identifiable {
    case single
    case multiple
    case text
    case paragraph
    case rating

    public var id: String { rawValue }

    public init(key: String) {
        self = SurveyQuestionType(rawValue: key.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()) ?? .text
    }

    public var title: String {
        switch self {
        case .single: return "Pick one"
        case .multiple: return "Pick any"
        case .text: return "Short text"
        case .paragraph: return "Long text"
        case .rating: return "Rating"
        }
    }

    public var symbol: String {
        switch self {
        case .single: return "circle.inset.filled"
        case .multiple: return "checklist"
        case .text: return "character.cursor.ibeam"
        case .paragraph: return "text.alignleft"
        case .rating: return "star"
        }
    }

    public var isChoice: Bool { self == .single || self == .multiple }

    public var isText: Bool { self == .text || self == .paragraph }
}

public enum SurveyText {
    public static func number(_ value: Double, places: Int = 1) -> String {
        var scale = 1.0
        for _ in 0..<max(0, places) { scale *= 10 }
        let rounded = (value * scale).rounded() / scale
        if rounded == rounded.rounded() { return String(Int64(rounded)) }
        var text = String(format: "%.\(places)f", rounded)
        while text.hasSuffix("0") { text.removeLast() }
        if text.hasSuffix(".") { text.removeLast() }
        return text
    }

    public static func plural(_ count: Int, _ one: String, _ many: String? = nil) -> String {
        "\(count) \(count == 1 ? one : (many ?? one + "s"))"
    }

    public static func percent(_ part: Int, of whole: Int) -> String {
        guard whole > 0 else { return "0%" }
        return "\(Int((Double(part) * 100 / Double(whole)).rounded()))%"
    }

    public static func share(_ part: Int, of whole: Int) -> Double {
        guard whole > 0, part > 0 else { return 0 }
        return min(1, Double(part) / Double(whole))
    }

    public static func statusTitle(_ key: String) -> String {
        switch key {
        case "draft": return "Draft"
        case "open": return "Open"
        case "scheduled": return "Scheduled"
        case "closed": return "Closed"
        default: return CrmStageLabel.title(key)
        }
    }

    public static func dateTime(_ ms: Int64, locale: Locale = .current, timeZone: TimeZone = .current) -> String {
        guard ms > 0 else { return "" }
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(ms) / 1000))
    }

    public static func describe(_ error: Error) -> String {
        guard let error = error as? PackagesError else { return PackagesError.describe(error) }
        switch error.code {
        case "plugin_not_installed": return "Surveys is not installed for this organization."
        case "survey_not_found", "response_not_found": return "That survey was removed or you no longer have access."
        case "survey_has_no_questions": return "Add a question before opening the survey."
        case "invalid_question_options": return "Choice questions need 2 to 30 different options."
        case "survey_delete_forbidden": return "Only the survey's creator or an organization owner or admin can delete it."
        case "survey_closed": return "This survey is not taking answers."
        case "already_responded": return "This survey takes one answer per person."
        case "too_many_responses": return "Too many answers for now. Try again later."
        case "answer_required": return "Answer every required question."
        case "invalid_answer": return "One of the answers does not fit its question."
        case "payload_too_large": return "The survey is too large to save."
        case "survey_close_date_passed": return "Its closing time has passed. Change it before reopening."
        case "survey_response_limit_reached": return "It reached its response limit. Raise the limit before reopening."
        case "survey_not_open": return "Only an open survey can be closed."
        case "survey_catalog_full": return "This organization already has 500 surveys. Delete some first."
        case "invalid_question_type": return "A question has a type this version does not know."
        case "too_many_questions": return "A survey can have at most 50 questions."
        case "invalid_schedule": return "The closing time must be after the opening time."
        default: return error.message
        }
    }
}

public struct SurveySettings: Equatable, Sendable {
    public var oneResponsePerPerson: Bool
    public var opensAt: Int64
    public var closesAt: Int64
    public var maxResponses: Int
    public var notifyOnResponse: Bool
    public var shuffleOptions: Bool

    public init(
        oneResponsePerPerson: Bool = false, opensAt: Int64 = 0, closesAt: Int64 = 0, maxResponses: Int = 0,
        notifyOnResponse: Bool = true, shuffleOptions: Bool = false
    ) {
        self.oneResponsePerPerson = oneResponsePerPerson
        self.opensAt = opensAt
        self.closesAt = closesAt
        self.maxResponses = maxResponses
        self.notifyOnResponse = notifyOnResponse
        self.shuffleOptions = shuffleOptions
    }

    public init(json: JSONObject) {
        self.init(
            oneResponsePerPerson: json.bool("oneResponsePerPerson"), opensAt: max(0, json.int64("opensAt")),
            closesAt: max(0, json.int64("closesAt")),
            maxResponses: min(SurveyLimits.maxResponses, max(0, json.int("maxResponses"))),
            notifyOnResponse: json.optBool("notifyOnResponse") ?? true, shuffleOptions: json.bool("shuffleOptions"))
    }

    public var body: JSONObject {
        [
            "oneResponsePerPerson": .bool(oneResponsePerPerson), "opensAt": .int(max(0, opensAt)),
            "closesAt": .int(max(0, closesAt)),
            "maxResponses": .int(Int64(min(SurveyLimits.maxResponses, max(0, maxResponses)))),
            "notifyOnResponse": .bool(notifyOnResponse), "shuffleOptions": .bool(shuffleOptions),
        ]
    }

    public var datesAreValid: Bool { opensAt == 0 || closesAt == 0 || closesAt > opensAt }

    public var maxResponsesIsValid: Bool { (0...SurveyLimits.maxResponses).contains(maxResponses) }

    public func lines(locale: Locale = .current, timeZone: TimeZone = .current) -> [String] {
        var out: [String] = []
        if opensAt > 0 { out.append("Opens " + SurveyText.dateTime(opensAt, locale: locale, timeZone: timeZone)) }
        if closesAt > 0 { out.append("Closes " + SurveyText.dateTime(closesAt, locale: locale, timeZone: timeZone)) }
        if maxResponses > 0 { out.append("Up to " + SurveyText.plural(maxResponses, "response")) }
        if oneResponsePerPerson { out.append("One response per person") }
        if shuffleOptions { out.append("Options shuffled") }
        if !notifyOnResponse { out.append("No Pings") }
        return out
    }
}

public struct SurveyQuestion: Equatable, Sendable, Identifiable {
    public var id: String
    public var type: SurveyQuestionType
    public var title: String
    public var help: String
    public var required: Bool
    public var options: [String]
    public var scale: Int

    public init(
        id: String = "", type: SurveyQuestionType = .text, title: String = "", help: String = "", required: Bool = false,
        options: [String] = [], scale: Int = 0
    ) {
        self.id = id
        self.type = type
        self.title = title
        self.help = help
        self.required = required
        self.options = type.isChoice ? options : []
        self.scale = type == .rating ? SurveyQuestion.clampScale(scale) : 0
    }

    public init(json: JSONObject) {
        let type = SurveyQuestionType(key: json.str("type"))
        self.init(
            id: json.str("id"), type: type, title: json.str("title"), help: json.str("help"), required: json.bool("required"),
            options: json.strings("options"), scale: json.int("scale", SurveyLimits.defaultScale))
    }

    public static func isQuestionId(_ value: String) -> Bool {
        value.count == 8 && value.unicodeScalars.allSatisfy {
            ($0.value >= 48 && $0.value <= 57) || ($0.value >= 97 && $0.value <= 102)
        }
    }

    public static func clampScale(_ value: Int) -> Int {
        guard value > 0 else { return SurveyLimits.defaultScale }
        return min(SurveyLimits.maxScale, max(SurveyLimits.minScale, value))
    }

    public var displayTitle: String { title.isEmpty ? "Untitled question" : title }

    public var detail: String {
        var parts = [type.title]
        if type.isChoice { parts.append(SurveyText.plural(options.count, "option")) }
        if type == .rating { parts.append("1 to \(scale)") }
        if required { parts.append("Required") }
        return parts.joined(separator: " · ")
    }
}

public struct SurveyOptionCount: Equatable, Sendable {
    public var label: String
    public var count: Int

    public init(label: String, count: Int) {
        self.label = label
        self.count = count
    }

    public init(json: JSONObject) {
        self.init(label: json.str("label"), count: max(0, json.int("count")))
    }
}

public struct SurveyBar: Equatable, Sendable, Identifiable {
    public var label: String
    public var count: Int
    public var share: Double
    public var percent: String

    public var id: String { label }
}

public struct SurveyResult: Equatable, Sendable, Identifiable {
    public var id: String
    public var type: SurveyQuestionType
    public var title: String
    public var answered: Int
    public var skipped: Int
    public var options: [SurveyOptionCount]
    public var average: Double
    public var distribution: [Int]
    public var answers: [String]

    public init(json: JSONObject) {
        id = json.str("id")
        type = SurveyQuestionType(key: json.str("type"))
        title = json.str("title")
        answered = max(0, json.int("answered"))
        skipped = max(0, json.int("skipped"))
        options = json.maps("options").map(SurveyOptionCount.init(json:))
        average = max(0, json.double("average"))
        distribution = json.list("distribution").map { max(0, $0.int()) }
        answers = json.strings("answers").filter { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
    }

    public var displayTitle: String { title.isEmpty ? "Untitled question" : title }

    public var countsLine: String {
        let answeredText = SurveyText.plural(answered, "answer")
        return skipped > 0 ? "\(answeredText) · \(skipped) skipped" : answeredText
    }

    public var optionBars: [SurveyBar] {
        options.map {
            SurveyBar(
                label: $0.label, count: $0.count, share: SurveyText.share($0.count, of: answered),
                percent: SurveyText.percent($0.count, of: answered))
        }
    }

    public var scale: Int { distribution.count }

    public var averageText: String {
        guard answered > 0, scale > 0 else { return "—" }
        return "\(SurveyText.number(average)) / \(scale)"
    }

    public var distributionBars: [SurveyBar] {
        distribution.enumerated().map { index, count in
            SurveyBar(
                label: "\(index + 1)", count: count, share: SurveyText.share(count, of: answered),
                percent: SurveyText.percent(count, of: answered))
        }
    }

    public var hasData: Bool { answered > 0 }
}

public struct SurveyResponse: Equatable, Sendable, Identifiable {
    public var id: String
    public var submittedAt: Int64
    public var answers: JSONObject

    public init(json: JSONObject) {
        id = json.str("id")
        submittedAt = json.int64("submittedAt")
        answers = json.obj("answers")
    }

    public static func isResponseId(_ value: String) -> Bool {
        (1...64).contains(value.count) && value.unicodeScalars.allSatisfy {
            ($0.value >= 48 && $0.value <= 57) || ($0.value >= 97 && $0.value <= 122) || $0 == "-" || $0 == "_"
        }
    }

    public func answer(_ question: SurveyQuestion) -> String {
        guard let value = answers[question.id] else { return "" }
        switch value {
        case .null: return ""
        case .array(let items): return items.map(\.text).filter { !$0.isEmpty }.joined(separator: "; ")
        case .object: return ""
        default: return value.text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
    }

    public func answer(_ question: SurveyQuestion, scale: Int) -> String {
        let text = answer(question)
        guard question.type == .rating, !text.isEmpty else { return text }
        return "\(text) / \(scale)"
    }

    public func answeredCount(_ questions: [SurveyQuestion]) -> Int {
        questions.filter { !answer($0).isEmpty }.count
    }

    public func preview(_ questions: [SurveyQuestion]) -> String {
        for question in questions where question.type.isText {
            let text = answer(question)
            if !text.isEmpty { return text }
        }
        for question in questions {
            let text = answer(question, scale: question.scale)
            if !text.isEmpty { return text }
        }
        return "No answers"
    }
}

public struct SurveyExport: Equatable, Sendable {
    public var filename: String
    public var csv: String

    public init(filename: String, csv: String) {
        self.filename = SurveyExport.safeFilename(filename)
        self.csv = csv
    }

    public init(json: JSONObject) {
        self.init(filename: json.str("filename"), csv: json.str("csv"))
    }

    public static func safeFilename(_ value: String) -> String {
        let last = value.split(whereSeparator: { $0 == "/" || $0 == "\\" }).last.map(String.init) ?? ""
        var kept = String(
            last.unicodeScalars.filter {
                ($0.value >= 48 && $0.value <= 57) || ($0.value >= 65 && $0.value <= 90) || ($0.value >= 97 && $0.value <= 122)
                    || $0 == "-" || $0 == "_" || $0 == "."
            }.map(Character.init))
        while kept.hasPrefix(".") { kept.removeFirst() }
        if kept.count > 120 { kept = String(kept.suffix(120)) }
        if kept.isEmpty || kept.lowercased() == ".csv" { kept = "survey.csv" }
        if !kept.lowercased().hasSuffix(".csv") { kept += ".csv" }
        return kept
    }

    public var rows: Int {
        var lines = 0
        var quoted = false
        var pending = false
        for character in csv {
            if character == "\"" { quoted.toggle() }
            if !quoted && (character == "\n" || character == "\r\n") {
                if pending { lines += 1 }
                pending = false
            } else if character != "\r" {
                pending = true
            }
        }
        if pending { lines += 1 }
        return max(0, lines - 1)
    }
}

public enum SurveyFilter: String, CaseIterable, Sendable, Identifiable {
    case all
    case open
    case drafts
    case closed

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .all: return "All surveys"
        case .open: return "Open"
        case .drafts: return "Drafts"
        case .closed: return "Closed"
        }
    }
}

public struct Survey: Equatable, Sendable, Identifiable {
    public var id: String
    public var organization: String
    public var title: String
    public var description: String
    public var thankYou: String
    public var status: String
    public var displayStatus: String
    public var settings: SurveySettings
    public var questionCount: Int
    public var responseCount: Int
    public var lastResponseAt: Int64
    public var publishedAt: Int64
    public var closedAt: Int64
    public var createdBy: String
    public var updatedBy: String
    public var createdAt: Int64
    public var updatedAt: Int64
    public var canDelete: Bool
    public var publicUrl: String
    public var questions: [SurveyQuestion]?
    public var results: [SurveyResult]?

    public init(json: JSONObject) {
        id = json.str("id")
        organization = json.str("organization")
        title = json.str("title")
        description = json.str("description")
        thankYou = json.str("thankYou")
        let rawStatus = json.str("status", "draft")
        status = ["draft", "open", "closed"].contains(rawStatus) ? rawStatus : "draft"
        displayStatus = json.str("displayStatus")
        settings = SurveySettings(json: json.obj("settings"))
        let parsed = json["questions"]?.arrayValue.map {
            $0.compactMap(\.objectValue).map(SurveyQuestion.init(json:))
        }
        questions = parsed
        questionCount = max(0, json.int("questionCount", parsed?.count ?? 0))
        responseCount = max(0, json.int("responseCount"))
        lastResponseAt = json.int64("lastResponseAt")
        publishedAt = json.int64("publishedAt")
        closedAt = json.int64("closedAt")
        createdBy = json.str("createdBy")
        updatedBy = json.str("updatedBy")
        createdAt = json.int64("createdAt")
        updatedAt = json.int64("updatedAt")
        canDelete = json.bool("canDelete")
        let link = json.str("publicUrl")
        publicUrl = link.lowercased().hasPrefix("https://") && !link.contains(" ") ? link : ""
        results = json["results"]?.arrayValue.map { $0.compactMap(\.objectValue).map(SurveyResult.init(json:)) }
    }

    public var displayTitle: String { title.isEmpty ? "Untitled survey" : title }

    public var state: String { displayStatus.isEmpty ? status : displayStatus }

    public var stateTitle: String { SurveyText.statusTitle(state) }

    public var isDraft: Bool { status == "draft" }

    public var canOpen: Bool { status != "open" }

    public var openTitle: String { isDraft ? "Open for answers" : "Reopen" }

    public var openNeedsQuestions: Bool { canOpen && questionCount == 0 }

    public var canClose: Bool { status == "open" }

    public var canDuplicate: Bool { true }

    public var canEdit: Bool { true }

    public var isTakingAnswers: Bool { state == "open" }

    public var shareURL: URL? { publicUrl.isEmpty ? nil : URL(string: publicUrl) }

    public var countsLine: String {
        SurveyText.plural(questionCount, "question") + " · " + SurveyText.plural(responseCount, "response")
    }

    public var stateNote: String {
        if status == "open" && state == "scheduled" { return "Published; answers open at the set time." }
        if status == "open" && state == "closed" {
            if settings.maxResponses > 0 && responseCount >= settings.maxResponses {
                return "Open, but it has reached its response limit."
            }
            return "Open, but its closing time has passed."
        }
        if status == "draft" { return "Only members see a draft. Open it to get a link people can answer." }
        if status == "closed" { return "Closed: the link shows that it no longer takes answers." }
        return "Anyone with the link can answer."
    }

    public func question(_ id: String) -> SurveyQuestion? { questions?.first { $0.id == id } }

    public func scale(for result: SurveyResult) -> Int {
        question(result.id)?.scale ?? result.scale
    }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(query, title, description, createdBy)
    }

    public func matches(_ filter: SurveyFilter) -> Bool {
        switch filter {
        case .all: return true
        case .open: return status == "open"
        case .drafts: return status == "draft"
        case .closed: return status == "closed"
        }
    }
}

public struct SurveySummary: Equatable, Sendable {
    public var surveys: Int
    public var open: Int
    public var drafts: Int
    public var closed: Int
    public var responses: Int
    public var responsesLast7Days: Int

    public init(json: JSONObject) {
        surveys = max(0, json.int("surveys"))
        open = max(0, json.int("open"))
        drafts = max(0, json.int("drafts"))
        closed = max(0, json.int("closed"))
        responses = max(0, json.int("responses"))
        responsesLast7Days = max(0, json.int("responsesLast7Days"))
    }

    public var surveysDetail: String { "\(drafts) draft · \(closed) closed" }

    public var responsesDetail: String { "\(responsesLast7Days) in the last 7 days" }
}

public struct SurveyBoard: Equatable, Sendable {
    public var organization: String
    public var role: String
    public var canManagePlugin: Bool
    public var surveys: [Survey]
    public var summary: SurveySummary

    public init(json: JSONObject) {
        organization = json.str("organization")
        role = json.str("role", "member")
        canManagePlugin = json.bool("canManagePlugin")
        surveys = json.maps("surveys").map(Survey.init(json:)).filter { PackagesText.isHexId($0.id) }
        summary = SurveySummary(json: json.obj("summary"))
    }

    public var canManage: Bool { canManagePlugin || role == "owner" || role == "admin" }

    public var openCount: Int { surveys.filter { $0.status == "open" }.count }

    public func filtered(query: String = "", filter: SurveyFilter = .all) -> [Survey] {
        surveys.filter { $0.matches(filter) && $0.matches(query) }
    }

    public func survey(_ id: String) -> Survey? { surveys.first { $0.id == id } }

    public mutating func replace(_ survey: Survey) {
        var stored = survey
        stored.questions = nil
        stored.results = nil
        if let index = surveys.firstIndex(where: { $0.id == survey.id }) {
            surveys[index] = stored
        } else {
            surveys.insert(stored, at: 0)
        }
    }

    public mutating func remove(_ id: String) {
        surveys.removeAll { $0.id == id }
    }
}

public struct SurveyPublicForm: Equatable, Sendable {
    public var title: String
    public var description: String
    public var questions: [SurveyQuestion]
    public var status: String
    public var thankYou: String
    public var oneResponsePerPerson: Bool
    public var shuffleOptions: Bool
    public var organization: String

    public init(json: JSONObject) {
        title = json.str("title")
        description = json.str("description")
        questions = json.maps("questions").map(SurveyQuestion.init(json:))
        status = json.str("status")
        thankYou = json.str("thankYou")
        oneResponsePerPerson = json.bool("oneResponsePerPerson")
        shuffleOptions = json.bool("shuffleOptions")
        organization = json.str("organization")
    }

    public var isOpen: Bool { status == "open" }

    public static func isToken(_ value: String) -> Bool {
        let parts = value.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 2, PackagesText.isHexId(parts[0]), parts[1].count == 24 else { return false }
        return parts[1].unicodeScalars.allSatisfy { ($0.value >= 48 && $0.value <= 57) || ($0.value >= 97 && $0.value <= 102) }
    }

    public static func token(from url: URL) -> String? {
        guard let parts = URLComponents(url: url, resolvingAgainstBaseURL: false),
              let value = parts.queryItems?.first(where: { $0.name == "t" })?.value, isToken(value)
        else { return nil }
        return value
    }
}
