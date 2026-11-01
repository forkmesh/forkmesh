import FMApi
import FMBase
import XCTest

@testable import FMPackages

final class SurveysTests: XCTestCase {
    let us = Locale(identifier: "en_US")
    let utc = TimeZone(identifier: "UTC")!
    let jan1: Int64 = 1_767_225_600_000
    let hexC = String(repeating: "c", count: 32)

    func surveyJSON(
        _ id: String, _ title: String, status: String, display: String = "", responses: Int = 0, questions: Int = 0
    ) -> JSONValue {
        [
            "id": .string(id), "organization": "acme", "title": .string(title), "status": .string(status),
            "displayStatus": .string(display.isEmpty ? status : display), "questionCount": .int(Int64(questions)),
            "responseCount": .int(Int64(responses)), "createdBy": "alice", "canDelete": true,
            "publicUrl": .string(status == "draft" ? "" : "https://app.forkmesh.com/survey?t=\(id).0123456789abcdef01234567"),
        ]
    }

    func overview() -> JSONObject {
        [
            "ok": true, "organization": "acme", "role": "member", "canManagePlugin": false,
            "surveys": [
                surveyJSON(hexA, "Team offsite feedback", status: "open", responses: 12, questions: 4),
                surveyJSON(hexB, "Lunch poll", status: "draft", questions: 1),
                surveyJSON(hexC, "Old survey", status: "closed", responses: 3, questions: 2),
                surveyJSON(String(repeating: "d", count: 32), "Spring launch", status: "open", display: "scheduled"),
                ["id": "bad", "title": "dropped"],
            ],
            "summary": ["surveys": 4, "open": 2, "drafts": 1, "closed": 1, "responses": 15, "responsesLast7Days": 6],
        ]
    }

    func detailJSON() -> JSONObject {
        [
            "survey": [
                "id": .string(hexA), "organization": "acme", "title": "Team offsite feedback", "description": "Tell us",
                "thankYou": "Thanks for answering.", "status": "open", "displayStatus": "open",
                "settings": [
                    "oneResponsePerPerson": true, "opensAt": 0, "closesAt": .int(jan1), "maxResponses": 100,
                    "notifyOnResponse": false, "shuffleOptions": true,
                ],
                "questionCount": 4, "responseCount": 12, "canDelete": false,
                "publicUrl": "https://app.forkmesh.com/survey?t=x",
                "questions": [
                    [
                        "id": "a1b2c3d4", "type": "single", "title": "How was it?", "help": "Be honest", "required": true,
                        "options": ["Great", "Fine"], "scale": 0,
                    ],
                    ["id": "b1b2c3d4", "type": "multiple", "title": "What did you like?", "options": ["Food", "Talks", "Walks"]],
                    ["id": "c1b2c3d4", "type": "rating", "title": "Rate the venue", "scale": 4],
                    ["id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?"],
                ],
                "results": [
                    [
                        "id": "a1b2c3d4", "type": "single", "title": "How was it?", "answered": 10, "skipped": 2,
                        "options": [["label": "Great", "count": 7], ["label": "Fine", "count": 3]],
                        "average": 0, "distribution": [], "answers": [],
                    ],
                    [
                        "id": "b1b2c3d4", "type": "multiple", "title": "What did you like?", "answered": 4, "skipped": 0,
                        "options": [["label": "Food", "count": 4], ["label": "Talks", "count": 2], ["label": "Walks", "count": 0]],
                    ],
                    [
                        "id": "c1b2c3d4", "type": "rating", "title": "Rate the venue", "answered": 5, "skipped": 7,
                        "average": 3.2, "distribution": [0, 1, 2, 2],
                    ],
                    [
                        "id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?", "answered": 2, "skipped": 10,
                        "answers": ["More coffee", " ", "Great talks"],
                    ],
                ],
            ],
        ]
    }

    func testOverviewParsesSurveysAndSummary() {
        let board = SurveyBoard(json: overview())
        XCTAssertEqual(board.surveys.map(\.title), ["Team offsite feedback", "Lunch poll", "Old survey", "Spring launch"])
        XCTAssertFalse(board.canManage)
        XCTAssertEqual(board.openCount, 2)
        let first = board.surveys[0]
        XCTAssertEqual(first.state, "open")
        XCTAssertEqual(first.stateTitle, "Open")
        XCTAssertEqual(first.countsLine, "4 questions · 12 responses")
        XCTAssertNil(first.questions)
        XCTAssertNotNil(first.shareURL)
        XCTAssertTrue(first.isTakingAnswers)
        XCTAssertEqual(board.surveys[1].countsLine, "1 question · 0 responses")
        XCTAssertNil(board.surveys[1].shareURL)
        XCTAssertEqual(board.surveys[3].stateTitle, "Scheduled")
        XCTAssertFalse(board.surveys[3].isTakingAnswers)
        XCTAssertEqual(board.summary.responses, 15)
        XCTAssertEqual(board.summary.surveysDetail, "1 draft · 1 closed")
        XCTAssertEqual(board.summary.responsesDetail, "6 in the last 7 days")
        let empty = Survey(json: ["id": .string(hexA), "status": "weird"])
        XCTAssertEqual(empty.status, "draft")
        XCTAssertEqual(empty.displayTitle, "Untitled survey")
        XCTAssertEqual(empty.settings, SurveySettings())
        XCTAssertTrue(empty.settings.notifyOnResponse)
        XCTAssertEqual(SurveySummary(json: [:]).surveys, 0)
    }

    func testFiltersAndSearch() {
        let board = SurveyBoard(json: overview())
        XCTAssertEqual(board.filtered(filter: .open).map(\.title), ["Team offsite feedback", "Spring launch"])
        XCTAssertEqual(board.filtered(filter: .drafts).map(\.title), ["Lunch poll"])
        XCTAssertEqual(board.filtered(filter: .closed).map(\.title), ["Old survey"])
        XCTAssertEqual(board.filtered(query: "LUNCH").map(\.id), [hexB])
        XCTAssertEqual(board.filtered(query: "alice").count, 4)
        XCTAssertEqual(board.filtered(query: "lunch", filter: .closed), [])
        XCTAssertEqual(SurveyFilter.allCases.map(\.title), ["All surveys", "Open", "Drafts", "Closed"])
    }

    func testActionsFollowTheStatus() {
        let board = SurveyBoard(json: overview())
        let open = board.surveys[0]
        XCTAssertTrue(open.canClose)
        XCTAssertFalse(open.canOpen)
        XCTAssertTrue(open.canDuplicate)
        XCTAssertTrue(open.canEdit)
        let draft = board.surveys[1]
        XCTAssertTrue(draft.canOpen)
        XCTAssertFalse(draft.canClose)
        XCTAssertEqual(draft.openTitle, "Open for answers")
        XCTAssertFalse(draft.openNeedsQuestions)
        XCTAssertTrue(Survey(json: ["id": .string(hexB), "status": "draft"]).openNeedsQuestions)
        let closed = board.surveys[2]
        XCTAssertTrue(closed.canOpen)
        XCTAssertEqual(closed.openTitle, "Reopen")
        XCTAssertFalse(closed.canClose)
        let scheduled = board.surveys[3]
        XCTAssertTrue(scheduled.canClose)
        XCTAssertEqual(scheduled.stateNote, "Published; answers open at the set time.")
        var full = Survey(json: surveyJSON(hexA, "Full", status: "open", display: "closed", responses: 5).objectValue!)
        full.settings.maxResponses = 5
        XCTAssertEqual(full.stateNote, "Open, but it has reached its response limit.")
        full.settings.maxResponses = 0
        XCTAssertEqual(full.stateNote, "Open, but its closing time has passed.")
        XCTAssertEqual(SurveyText.statusTitle("draft"), "Draft")
    }

    func testDetailParsesQuestionsSettingsAndResults() {
        let survey = Survey(json: detailJSON().obj("survey"))
        XCTAssertEqual(survey.questions?.map(\.type), [.single, .multiple, .rating, .paragraph])
        XCTAssertEqual(survey.questions?[0].options, ["Great", "Fine"])
        XCTAssertEqual(survey.questions?[0].scale, 0)
        XCTAssertEqual(survey.questions?[0].detail, "Pick one · 2 options · Required")
        XCTAssertEqual(survey.questions?[2].scale, 4)
        XCTAssertEqual(survey.questions?[2].options, [])
        XCTAssertEqual(survey.questions?[2].detail, "Rating · 1 to 4")
        XCTAssertEqual(survey.questions?[3].scale, 0)
        XCTAssertFalse(survey.canDelete)
        XCTAssertEqual(survey.thankYou, "Thanks for answering.")
        XCTAssertTrue(survey.settings.oneResponsePerPerson)
        XCTAssertFalse(survey.settings.notifyOnResponse)
        let lines = survey.settings.lines(locale: us, timeZone: utc)
        XCTAssertEqual(lines.count, 5)
        XCTAssertTrue(lines[0].hasPrefix("Closes Jan 1, 2026"), lines[0])
        XCTAssertEqual(Array(lines.dropFirst()), ["Up to 100 responses", "One response per person", "Options shuffled", "No Pings"])
        XCTAssertEqual(SurveySettings().lines(), [])

        let results = survey.results ?? []
        XCTAssertEqual(results.count, 4)
        XCTAssertEqual(results[0].countsLine, "10 answers · 2 skipped")
        XCTAssertEqual(results[0].optionBars.map(\.percent), ["70%", "30%"])
        XCTAssertEqual(results[0].optionBars.map(\.share), [0.7, 0.3])
        XCTAssertEqual(results[1].countsLine, "4 answers")
        XCTAssertEqual(results[1].optionBars.map(\.percent), ["100%", "50%", "0%"])
        XCTAssertEqual(results[2].averageText, "3.2 / 4")
        XCTAssertEqual(results[2].distributionBars.map(\.label), ["1", "2", "3", "4"])
        XCTAssertEqual(results[2].distributionBars.map(\.count), [0, 1, 2, 2])
        XCTAssertEqual(results[2].distributionBars.map(\.percent), ["0%", "20%", "40%", "40%"])
        XCTAssertEqual(survey.scale(for: results[2]), 4)
        XCTAssertEqual(results[3].answers, ["More coffee", "Great talks"])
        let none = SurveyResult(json: ["id": "e1b2c3d4", "type": "rating", "answered": 0, "distribution": [0, 0, 0]])
        XCTAssertEqual(none.averageText, "—")
        XCTAssertFalse(none.hasData)
        XCTAssertEqual(none.optionBars, [])
        XCTAssertEqual(SurveyResult(json: ["type": "single"]).countsLine, "0 answers")
        XCTAssertEqual(SurveyText.number(4), "4")
        XCTAssertEqual(SurveyText.number(4.25), "4.3")
        XCTAssertEqual(SurveyText.percent(1, of: 3), "33%")
        XCTAssertEqual(SurveyText.percent(1, of: 0), "0%")
        XCTAssertEqual(SurveyText.share(5, of: 2), 1)
    }

    func testQuestionDecodingIsLenient() {
        let odd = SurveyQuestion(json: ["id": "nothex", "type": "dropdown", "title": "Q", "options": ["x", "y"], "scale": 99])
        XCTAssertEqual(odd.type, .text)
        XCTAssertEqual(odd.options, [])
        XCTAssertEqual(odd.scale, 0)
        let rating = SurveyQuestion(json: ["type": "RATING", "scale": 99])
        XCTAssertEqual(rating.type, .rating)
        XCTAssertEqual(rating.scale, 10)
        XCTAssertEqual(SurveyQuestion(json: ["type": "rating"]).scale, 5)
        XCTAssertEqual(SurveyQuestion(json: ["type": "rating", "scale": 2]).scale, 3)
        XCTAssertEqual(rating.displayTitle, "Untitled question")
        XCTAssertTrue(SurveyQuestion.isQuestionId("a1b2c3d4"))
        XCTAssertFalse(SurveyQuestion.isQuestionId("A1B2C3D4"))
        XCTAssertFalse(SurveyQuestion.isQuestionId("a1b2c3d"))
        XCTAssertEqual(SurveyQuestionType.allCases.map(\.title), ["Pick one", "Pick any", "Short text", "Long text", "Rating"])
    }

    func testQuestionDraftValidation() {
        var question = SurveyQuestionDraft(type: .single)
        XCTAssertEqual(question.options.count, 2)
        XCTAssertEqual(question.problem, "Give the question a title.")
        question.title = "  Pick a day  "
        XCTAssertEqual(question.problem, "Give it 2 to 30 different options.")
        question.options[0].label = "Monday"
        question.options[1].label = " Monday "
        XCTAssertTrue(question.hasRepeatedOptions)
        XCTAssertEqual(question.cleanedOptions, ["Monday"])
        XCTAssertFalse(question.isValid)
        question.options[1].label = "Tuesday"
        question.addOption()
        XCTAssertTrue(question.isValid)
        XCTAssertEqual(question.cleanedOptions, ["Monday", "Tuesday"])
        question.options = (1...31).map { SurveyOptionDraft(label: "Day \($0)") }
        XCTAssertFalse(question.canAddOption)
        XCTAssertEqual(question.problem, "Give it 2 to 30 different options.")
        question.options.removeLast()
        XCTAssertTrue(question.isValid)
        question.options[0].label = String(repeating: "x", count: 201)
        XCTAssertEqual(question.problem, "Keep each option under 200 characters.")
        question.options[0].label = "Day 1"
        question.title = String(repeating: "q", count: 301)
        XCTAssertEqual(question.problem, "Keep the question under 300 characters.")
        question.title = "Pick a day"
        question.help = String(repeating: "h", count: 1001)
        XCTAssertEqual(question.problem, "Keep the help text under 1000 characters.")
        question.help = ""

        var rating = SurveyQuestionDraft(type: .rating)
        rating.title = "Rate it"
        XCTAssertEqual(rating.options, [])
        XCTAssertEqual(rating.scale, 5)
        XCTAssertTrue(rating.isValid)
        rating.scale = 11
        XCTAssertEqual(rating.problem, "Pick a scale of 3 to 10.")
        rating.scale = 2
        XCTAssertFalse(rating.isValid)
        rating.scale = 10
        XCTAssertTrue(rating.isValid)

        var text = SurveyQuestionDraft(type: .text)
        text.title = "Name"
        XCTAssertTrue(text.isValid)
        text.setType(.multiple)
        XCTAssertEqual(text.options.count, 2)
        XCTAssertFalse(text.isValid)
        text.setType(.paragraph)
        XCTAssertTrue(text.isValid)
        XCTAssertEqual(text.body.list("options"), [])
    }

    func testQuestionDraftBody() {
        let survey = Survey(json: detailJSON().obj("survey"))
        let drafts = (survey.questions ?? []).map(SurveyQuestionDraft.init)
        XCTAssertEqual(drafts.map(\.questionId), ["a1b2c3d4", "b1b2c3d4", "c1b2c3d4", "d1b2c3d4"])
        let single = drafts[0].body
        XCTAssertEqual(single.str("id"), "a1b2c3d4")
        XCTAssertEqual(single.str("type"), "single")
        XCTAssertEqual(single.str("title"), "How was it?")
        XCTAssertEqual(single.str("help"), "Be honest")
        XCTAssertTrue(single.bool("required"))
        XCTAssertEqual(single.strings("options"), ["Great", "Fine"])
        XCTAssertEqual(single.int("scale"), 0)
        let rating = drafts[2].body
        XCTAssertEqual(rating.int("scale"), 4)
        XCTAssertEqual(rating.strings("options"), [])
        var fresh = SurveyQuestionDraft(type: .multiple)
        fresh.title = "New"
        fresh.options = [SurveyOptionDraft(label: "A"), SurveyOptionDraft(label: ""), SurveyOptionDraft(label: "A"), SurveyOptionDraft(label: "B")]
        XCTAssertNil(fresh.body["id"])
        XCTAssertEqual(fresh.body.strings("options"), ["A", "B"])
        let short = SurveyQuestionDraft(SurveyQuestion(id: "zzzzzzzz", type: .single, title: "Few", options: ["Only"]))
        XCTAssertEqual(short.questionId, "")
        XCTAssertEqual(short.options.map(\.label), ["Only", ""])
    }

    func testSurveyDraftValidationAndBody() {
        var draft = SurveyDraft()
        XCTAssertTrue(draft.isNew)
        XCTAssertEqual(draft.questions.count, 1)
        XCTAssertEqual(draft.problems, ["Give the survey a title.", "Question 1: Give the question a title."])
        XCTAssertFalse(draft.canSave)
        draft.title = "  Offsite  "
        draft.questions[0].title = "Which day?"
        draft.questions[0].options[0].label = "Mon"
        draft.questions[0].options[1].label = "Tue"
        XCTAssertTrue(draft.canSave)
        let second = draft.addQuestion(.rating)
        XCTAssertNotNil(second)
        draft.questions[1].title = "How much?"
        let third = draft.addQuestion(.text)!
        draft.questions[2].title = "Why?"
        XCTAssertTrue(draft.canMove(third, by: -1))
        XCTAssertFalse(draft.canMove(third, by: 1))
        draft.moveQuestion(third, by: -1)
        XCTAssertEqual(draft.questions.map(\.type), [.single, .text, .rating])
        draft.moveQuestions(from: IndexSet(integer: 2), to: 0)
        XCTAssertEqual(draft.questions.map(\.type), [.rating, .single, .text])
        draft.moveQuestions(from: IndexSet(integer: 0), to: 3)
        XCTAssertEqual(draft.questions.map(\.type), [.single, .text, .rating])
        draft.removeQuestion(third)
        XCTAssertEqual(draft.questions.map(\.type), [.single, .rating])

        draft.settings.opensAt = jan1
        draft.settings.closesAt = jan1 - 1
        XCTAssertEqual(draft.problems, ["The closing time is before the opening time."])
        draft.settings.closesAt = jan1 + 3_600_000
        draft.settings.maxResponses = 10_001
        XCTAssertEqual(draft.problems, ["The response limit is 0 for none, or up to 10000."])
        draft.settings.maxResponses = 50
        draft.settings.notifyOnResponse = false
        XCTAssertTrue(draft.canSave)

        let body = draft.body
        XCTAssertEqual(body.str("title"), "Offsite")
        XCTAssertEqual(body.str("description"), "")
        XCTAssertEqual(body.maps("questions").map { $0.str("type") }, ["single", "rating"])
        XCTAssertEqual(body.maps("questions")[0].strings("options"), ["Mon", "Tue"])
        XCTAssertEqual(body.maps("questions")[1].int("scale"), 5)
        let settings = body.obj("settings")
        XCTAssertEqual(settings.int64("opensAt"), jan1)
        XCTAssertEqual(settings.int64("closesAt"), jan1 + 3_600_000)
        XCTAssertEqual(settings.int("maxResponses"), 50)
        XCTAssertEqual(settings.optBool("notifyOnResponse"), false)
        XCTAssertEqual(settings.optBool("oneResponsePerPerson"), false)
        XCTAssertEqual(settings.optBool("shuffleOptions"), false)

        while draft.canAddQuestion { draft.addQuestion(.text) }
        XCTAssertEqual(draft.questions.count, 50)
        XCTAssertNil(draft.addQuestion())

        let edit = SurveyDraft(editing: Survey(json: detailJSON().obj("survey")))
        XCTAssertEqual(edit.id, hexA)
        XCTAssertFalse(edit.isNew)
        XCTAssertEqual(edit.description, "Tell us")
        XCTAssertEqual(edit.questions.count, 4)
        XCTAssertTrue(edit.settings.shuffleOptions)
        XCTAssertTrue(edit.canSave)
    }

    func testResponsesReadAnswersByQuestion() {
        let survey = Survey(json: detailJSON().obj("survey"))
        let questions = survey.questions ?? []
        let response = SurveyResponse(json: [
            "id": "r1", "submittedAt": .int(jan1),
            "answers": ["a1b2c3d4": "Great", "b1b2c3d4": ["Food", "Walks"], "c1b2c3d4": 3, "d1b2c3d4": "  More coffee  "],
        ])
        XCTAssertEqual(response.answer(questions[0]), "Great")
        XCTAssertEqual(response.answer(questions[1]), "Food; Walks")
        XCTAssertEqual(response.answer(questions[2]), "3")
        XCTAssertEqual(response.answer(questions[2], scale: 4), "3 / 4")
        XCTAssertEqual(response.answer(questions[3]), "More coffee")
        XCTAssertEqual(response.answeredCount(questions), 4)
        XCTAssertEqual(response.preview(questions), "More coffee")
        let sparse = SurveyResponse(json: ["id": "r2", "answers": ["c1b2c3d4": 2, "a1b2c3d4": nil]])
        XCTAssertEqual(sparse.answer(questions[0]), "")
        XCTAssertEqual(sparse.answeredCount(questions), 1)
        XCTAssertEqual(sparse.preview(questions), "2 / 4")
        XCTAssertEqual(SurveyResponse(json: [:]).preview(questions), "No answers")
        XCTAssertTrue(SurveyResponse.isResponseId(hexA))
        XCTAssertTrue(SurveyResponse.isResponseId("r_1-x"))
        XCTAssertFalse(SurveyResponse.isResponseId("../x"))
        XCTAssertFalse(SurveyResponse.isResponseId(""))
    }

    func testExportFilenameAndRows() {
        let export = SurveyExport(json: [
            "filename": "../../team offsite.csv", "csv": "\"Submitted\",\"How was it?\"\r\n\"2026-01-01\",\"Great\"\r\n\"2026-01-02\",\"Line one\nline two\"\r\n",
        ])
        XCTAssertEqual(export.filename, "teamoffsite.csv")
        XCTAssertEqual(export.rows, 2)
        XCTAssertEqual(SurveyExport.safeFilename(""), "survey.csv")
        XCTAssertEqual(SurveyExport.safeFilename("..."), "survey.csv")
        XCTAssertEqual(SurveyExport.safeFilename("survey-acme"), "survey-acme.csv")
        XCTAssertEqual(SurveyExport.safeFilename(".hidden.CSV"), "hidden.CSV")
        XCTAssertEqual(SurveyExport(filename: "a.csv", csv: "Submitted\n").rows, 0)
        XCTAssertEqual(SurveyExport(filename: "a.csv", csv: "").rows, 0)
        XCTAssertEqual(SurveyExport(filename: "a.csv", csv: "h\nx\ny").rows, 2)
    }

    func testPublicTokens() {
        let token = hexA + ".0123456789abcdef01234567"
        XCTAssertTrue(SurveyPublicForm.isToken(token))
        XCTAssertFalse(SurveyPublicForm.isToken(hexA))
        XCTAssertFalse(SurveyPublicForm.isToken(hexA + ".0123456789ABCDEF01234567"))
        XCTAssertFalse(SurveyPublicForm.isToken("../x.0123456789abcdef01234567"))
        XCTAssertEqual(SurveyPublicForm.token(from: URL(string: "https://app.forkmesh.com/survey?t=\(token)")!), token)
        XCTAssertNil(SurveyPublicForm.token(from: URL(string: "https://app.forkmesh.com/survey?t=nope")!))
        let form = SurveyPublicForm(json: [
            "title": "Lunch", "status": "open", "oneResponsePerPerson": true, "organization": "acme",
            "questions": [["id": "a1b2c3d4", "type": "single", "title": "Where?", "options": ["Here", "There"]]],
        ])
        XCTAssertTrue(form.isOpen)
        XCTAssertEqual(form.questions.first?.options, ["Here", "There"])
    }

    @MainActor func testClientPathsAndBodies() async throws {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/surveys", [
            "actor": "alice", "organizations": [["name": "acme", "role": "owner", "installed": true], ["name": "Bad!"]],
        ])
        let orgs = try await client.surveyOrganizations()
        XCTAssertEqual(orgs.map(\.name), ["acme"])
        XCTAssertTrue(orgs[0].canManage)

        transport.on("GET", "/api/surveys/acme", overview())
        let board = try await client.surveys("acme")
        XCTAssertEqual(board.surveys.count, 4)

        transport.on("GET", "/api/surveys/acme/items/" + hexA, detailJSON())
        let fetched = try await client.survey("acme", id: hexA)
        XCTAssertEqual(fetched.results?.count, 4)

        let surveyJSON: JSONObject = ["survey": ["id": .string(hexB), "title": "Lunch poll", "status": "draft"]]
        var draft = SurveyDraft()
        draft.title = "Lunch poll"
        transport.on("POST", "/api/surveys/acme/items", status: 201, surveyJSON)
        let created = try await client.saveSurvey("acme", draft)
        XCTAssertEqual(created.id, hexB)
        XCTAssertEqual(transport.last("POST", "/api/surveys/acme/items")?.body?.str("title"), "Lunch poll")
        XCTAssertEqual(transport.last("POST", "/api/surveys/acme/items")?.body?.obj("settings").optBool("notifyOnResponse"), true)

        transport.on("PATCH", "/api/surveys/acme/items/" + hexA, detailJSON())
        _ = try await client.saveSurvey("acme", SurveyDraft(editing: fetched))
        let patched = transport.last("PATCH", "/api/surveys/acme/items/" + hexA)?.body
        XCTAssertEqual(patched?.maps("questions").map { $0.str("id") }, ["a1b2c3d4", "b1b2c3d4", "c1b2c3d4", "d1b2c3d4"])

        for action in ["open", "close", "duplicate"] {
            transport.on("POST", "/api/surveys/acme/items/\(hexA)/\(action)", surveyJSON)
        }
        let reopened = try await client.openSurvey("acme", id: hexA)
        XCTAssertEqual(reopened.id, hexB)
        _ = try await client.closeSurvey("acme", id: hexA)
        _ = try await client.duplicateSurvey("acme", id: hexA)
        XCTAssertTrue(transport.paths.contains("POST /api/surveys/acme/items/\(hexA)/open"))
        XCTAssertTrue(transport.paths.contains("POST /api/surveys/acme/items/\(hexA)/close"))
        XCTAssertTrue(transport.paths.contains("POST /api/surveys/acme/items/\(hexA)/duplicate"))

        transport.on("GET", "/api/surveys/acme/items/\(hexA)/responses", [
            "responses": [
                ["id": .string(hexC), "submittedAt": 5, "answers": ["a1b2c3d4": "Great"]], ["id": "../bad", "answers": [:]],
            ],
        ])
        let responses = try await client.surveyResponses("acme", id: hexA)
        XCTAssertEqual(responses.map(\.id), [hexC])

        transport.on("DELETE", "/api/surveys/acme/items/\(hexA)/responses/\(hexC)", [
            "deleted": .string(hexC), "survey": ["id": .string(hexA), "responseCount": 11, "status": "open"],
        ])
        let afterDelete = try await client.deleteSurveyResponse("acme", id: hexA, response: hexC)
        XCTAssertEqual(afterDelete.responseCount, 11)

        transport.on("GET", "/api/surveys/acme/items/\(hexA)/export", ["filename": "offsite.csv", "csv": "a\nb\n"])
        let export = try await client.exportSurvey("acme", id: hexA)
        XCTAssertEqual(export.filename, "offsite.csv")
        XCTAssertEqual(export.rows, 1)

        transport.on("DELETE", "/api/surveys/acme/items/" + hexA, ["deleted": .string(hexA)])
        try await client.deleteSurvey("acme", id: hexA)

        let token = hexA + ".0123456789abcdef01234567"
        transport.on("GET", "/api/surveys/-/public/" + token, ["survey": ["title": "Lunch", "status": "open"]])
        let form = try await client.publicSurvey(token: token)
        XCTAssertEqual(form.title, "Lunch")
        transport.on("POST", "/api/surveys/-/public/" + token, status: 201, ["thankYou": "Thanks!"])
        let thanks = try await client.submitSurvey(token: token, answers: ["a1b2c3d4": "Here"])
        XCTAssertEqual(thanks, "Thanks!")
        XCTAssertEqual(transport.last("POST", "/api/surveys/-/public/" + token)?.body?.obj("answers").str("a1b2c3d4"), "Here")

        transport.on("POST", "/api/plugins/acme/surveys/install", ["organization": "acme", "installed": ["surveys"]])
        _ = try await client.installSurveys("acme")
        XCTAssertTrue(transport.paths.contains("POST /api/plugins/acme/surveys/install"))

        for refused in [
            { _ = try await client.survey("acme", id: "../x") },
            { _ = try await client.surveys("../admin") },
            { _ = try await client.deleteSurveyResponse("acme", id: hexA, response: "../x") },
            { _ = try await client.publicSurvey(token: "../x") },
        ] as [@MainActor () async throws -> Void] {
            do {
                try await refused()
                XCTFail("expected a refusal")
            } catch {}
        }
        XCTAssertFalse(transport.paths.contains { $0.contains("..") })
    }

    @MainActor func testErrorsReadPlainly() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("POST", "/api/surveys/acme/items/\(hexA)/open", status: 409, ["ok": false, "error": "survey_has_no_questions"])
        do {
            _ = try await client.openSurvey("acme", id: hexA)
            XCTFail("expected a refusal")
        } catch {
            XCTAssertEqual(SurveyText.describe(error), "Add a question before opening the survey.")
        }
        transport.on("GET", "/api/surveys/acme", status: 409, ["error": "plugin_not_installed", "canInstall": true])
        do {
            _ = try await client.surveys("acme")
            XCTFail("expected a refusal")
        } catch let error as PackagesError {
            XCTAssertEqual(error.fields["canInstall"], "true")
            XCTAssertEqual(SurveyText.describe(error), "Surveys is not installed for this organization.")
        } catch {
            XCTFail("unexpected \(error)")
        }
        let token = hexA + ".0123456789abcdef01234567"
        transport.on("POST", "/api/surveys/-/public/" + token, status: 400, ["error": "answer_required", "question": "a1b2c3d4"])
        do {
            _ = try await client.submitSurvey(token: token, answers: [:])
            XCTFail("expected a refusal")
        } catch let error as PackagesError {
            XCTAssertEqual(error.fields["question"], "a1b2c3d4")
            XCTAssertEqual(SurveyText.describe(error), "Answer every required question.")
        } catch {
            XCTFail("unexpected \(error)")
        }
        XCTAssertEqual(
            SurveyText.describe(PackagesError("survey_delete_forbidden", status: 403)),
            "Only the survey's creator or an organization owner or admin can delete it.")
        XCTAssertEqual(
            SurveyText.describe(PackagesError("invalid_question_options", status: 400)),
            "Choice questions need 2 to 30 different options.")
        XCTAssertEqual(SurveyText.describe(PackagesError("survey_limit_reached", status: 409)), "Survey limit reached")
    }

    @MainActor func testModelPicksAnOrganizationAndOffersInstall() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/surveys", ["organizations": [["name": "acme", "role": "admin", "installed": false]]])
        let model = SurveysModel(client: client)
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        XCTAssertTrue(model.needsInstall)
        XCTAssertTrue(model.canManage)
        XCTAssertNil(model.board)
        XCTAssertFalse(transport.paths.contains("GET /api/surveys/acme"))

        transport.on("POST", "/api/plugins/acme/surveys/install", ["organization": "acme"])
        transport.on("GET", "/api/surveys/acme", overview())
        let installed = await model.install()
        XCTAssertTrue(installed)
        XCTAssertFalse(model.needsInstall)
        XCTAssertEqual(model.board?.surveys.count, 4)
        XCTAssertEqual(model.notice, "Surveys installed.")

        var draft = model.newDraft()
        draft.title = "Fresh"
        transport.on("POST", "/api/surveys/acme/items", status: 201, [
            "survey": ["id": .string(String(repeating: "f", count: 32)), "title": "Fresh", "status": "draft"],
        ])
        let saved = try? await model.save(draft)
        XCTAssertEqual(saved?.title, "Fresh")
        XCTAssertEqual(model.notice, "Survey Fresh created as a draft.")

        let open = model.board!.surveys[0]
        transport.on("POST", "/api/surveys/acme/items/\(hexA)/close", [
            "survey": ["id": .string(hexA), "title": "Team offsite feedback", "status": "closed"],
        ])
        let closed = try? await model.close(open)
        XCTAssertEqual(closed?.status, "closed")
        XCTAssertEqual(model.notice, "Team offsite feedback is closed.")

        let detail = Survey(json: detailJSON().obj("survey"))
        let response = SurveyResponse(json: ["id": .string(hexC)])
        transport.on("DELETE", "/api/surveys/acme/items/\(hexA)/responses/\(hexC)", [
            "deleted": .string(hexC), "survey": ["id": .string(hexA), "title": "Team offsite feedback", "status": "open", "responseCount": 11],
        ])
        let updated = try? await model.deleteResponse(response, of: detail)
        XCTAssertEqual(updated?.responseCount, 11)
        XCTAssertEqual(model.board?.survey(hexA)?.responseCount, 11)
        XCTAssertNil(model.board?.survey(hexA)?.questions)
        XCTAssertEqual(model.notice, "Response deleted.")

        transport.on("DELETE", "/api/surveys/acme/items/" + hexB, ["deleted": .string(hexB)])
        try? await model.delete(model.board!.survey(hexB)!)
        XCTAssertEqual(model.notice, "Survey Lunch poll deleted.")
    }

    @MainActor func testModelSeesAnUninstallAsNotInstalled() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/surveys", ["organizations": [
            ["name": "acme", "role": "member", "installed": true], ["name": "beta", "role": "owner", "installed": true],
        ]])
        transport.on("GET", "/api/surveys/acme", status: 409, ["error": "plugin_not_installed", "canInstall": false])
        let model = SurveysModel(client: client, organization: "acme")
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        XCTAssertTrue(model.needsInstall)
        XCTAssertFalse(model.canManage)
        XCTAssertEqual(model.errorText, "")
        let refused = await model.install()
        XCTAssertFalse(refused)
        var beta = overview()
        beta["organization"] = "beta"
        beta["role"] = "owner"
        transport.on("GET", "/api/surveys/beta", beta)
        await model.select("beta")
        XCTAssertEqual(model.board?.organization, "beta")
        XCTAssertFalse(model.needsInstall)
        XCTAssertTrue(model.canManage)
    }

    @MainActor func testModelFindsALinkedSurveyInAnotherOrganization() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/surveys", ["organizations": [
            ["name": "acme", "role": "member", "installed": true], ["name": "beta", "role": "member", "installed": true],
        ]])
        transport.on("GET", "/api/surveys/acme", overview())
        var beta = overview()
        beta["organization"] = "beta"
        transport.on("GET", "/api/surveys/beta", beta)
        transport.on("GET", "/api/surveys/beta/items/" + hexC, ["survey": ["id": .string(hexC), "title": "Beta survey"]])
        let model = SurveysModel(client: client)
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        let found = await model.locate(hexC)
        XCTAssertEqual(found?.title, "Beta survey")
        XCTAssertEqual(model.organization, "beta")
        XCTAssertEqual(model.board?.organization, "beta")
        let missing = await model.locate("nothex")
        XCTAssertNil(missing)
    }

    func testPackageRoutesListSurveys() {
        XCTAssertEqual(
            PackageRoutes.route(for: MarketPackage(id: "surveys", name: "Surveys")), .surveys(organization: nil, survey: nil))
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "surveys"), "surveys")
        XCTAssertTrue(PackageRoutes.businessPackageIds.contains("surveys"))
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["surveys"]).map(\.id), ["market", "surveys", "ledger"])
        XCTAssertEqual(
            PackageRoutes.hubRoutes(installed: ["crm", "invoices", "surveys"]).map(\.title),
            ["Market", "CRM", "Invoices", "Surveys", "Ledger"])
        XCTAssertEqual(PluginMenu.packageId(for: .surveys(organization: "acme", survey: nil)), "surveys")
        XCTAssertTrue(PluginMenu.addsButton(for: .surveys(organization: nil, survey: nil)))
    }
}
