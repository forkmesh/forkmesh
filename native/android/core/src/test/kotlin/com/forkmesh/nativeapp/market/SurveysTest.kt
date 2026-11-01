package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteCatalog
import com.forkmesh.nativeapp.navigation.RouteParser
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset
import kotlin.random.Random

class SurveysTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val token = "$hexA.${"c".repeat(24)}"
    private val utc = ZoneOffset.UTC
    private val jan10 = 1_768_003_200_000L

    private fun questionJson(id: String, type: String, title: String, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        linkedMapOf<String, Any?>("id" to id, "type" to type, "title" to title, "help" to "", "required" to false, "options" to emptyList<String>(), "scale" to 0) + extra

    private fun surveyJson(id: String = hexA, status: String = "open", display: String = status, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        linkedMapOf<String, Any?>(
            "id" to id, "organization" to "acme", "title" to "Team offsite feedback", "description" to "", "thankYou" to "Thanks for answering.",
            "status" to status, "displayStatus" to display,
            "settings" to mapOf("oneResponsePerPerson" to false, "opensAt" to 0, "closesAt" to 0, "maxResponses" to 0, "notifyOnResponse" to true, "shuffleOptions" to false),
            "questionCount" to 4, "responseCount" to 12, "lastResponseAt" to 2_000_000_000_000L, "publishedAt" to 2_000_000_000_000L, "closedAt" to 0,
            "createdBy" to "alice", "updatedBy" to "alice", "createdAt" to 0, "updatedAt" to 0, "canDelete" to true,
            "publicUrl" to if (status == "draft") "" else "https://app.forkmesh.com/survey?t=$token",
        ) + extra

    private fun detailJson(status: String = "open"): Map<String, Any?> = surveyJson(
        status = status,
        extra = mapOf(
            "questions" to listOf(
                questionJson("a1b2c3d4", "single", "How was it?", mapOf("required" to true, "options" to listOf("Great", "Fine", " "))),
                questionJson("b1b2c3d4", "multiple", "What did you like?", mapOf("options" to listOf("Food", "Talks"))),
                questionJson("c1b2c3d4", "rating", "Rate the venue", mapOf("scale" to 7)),
                questionJson("d1b2c3d4", "paragraph", "Anything else?", mapOf("help" to "Optional")),
                questionJson("e1b2c3d4", "mystery", "Odd one", mapOf("scale" to 99)),
            ),
            "results" to listOf(
                mapOf("id" to "a1b2c3d4", "type" to "single", "title" to "How was it?", "answered" to 10, "skipped" to 2,
                    "options" to listOf(mapOf("label" to "Great", "count" to 7), mapOf("label" to "Fine", "count" to 3)), "average" to 0, "distribution" to emptyList<Int>(), "answers" to emptyList<String>()),
                mapOf("id" to "c1b2c3d4", "type" to "rating", "title" to "Rate the venue", "answered" to 4, "skipped" to 0, "options" to emptyList<Any>(),
                    "average" to 4.25, "distribution" to listOf(0, 0, 1, 1, 0, 2, 0), "answers" to emptyList<String>()),
                mapOf("id" to "d1b2c3d4", "type" to "paragraph", "title" to "Anything else?", "answered" to 2, "skipped" to 10,
                    "answers" to listOf("More coffee", "", "Loved it")),
            ),
        ),
    )

    fun testSurveyParsesTheListShapeLeniently() {
        val survey = Survey.fromJson(surveyJson())
        assertEq("Team offsite feedback", survey.title)
        assertEq("Open", survey.statusLabel)
        assertEq(4, survey.questionCount)
        assertEq(12, survey.responseCount)
        assertEq("4 questions · 12 responses", survey.countsLine)
        assertEq(null, survey.questions)
        assertEq(null, survey.results)
        expect(survey.settings.notifyOnResponse && !survey.settings.shuffleOptions)
        assertEq("https://app.forkmesh.com/survey?t=$token", survey.publicUrl)
        val loose = Survey.fromJson(mapOf("id" to hexB, "questionCount" to "3", "responseCount" to -4, "publicUrl" to "http://x", "settings" to mapOf("maxResponses" to 50_000)))
        assertEq("draft", loose.status)
        assertEq("Draft", loose.statusLabel)
        assertEq(3, loose.questionCount)
        assertEq(0, loose.responseCount)
        assertEq("", loose.publicUrl)
        assertEq(10_000, loose.settings.maxResponses)
        expect(loose.settings.notifyOnResponse)
        assertEq("Untitled survey", loose.displayTitle)
        assertEq("Scheduled", Survey.fromJson(surveyJson(display = "scheduled")).statusLabel)
        assertEq("Closed", Survey.fromJson(surveyJson(display = "closed")).statusLabel)
    }

    fun testDetailParsesQuestionsAndResults() {
        val survey = Survey.fromJson(detailJson())
        val questions = survey.questions!!
        assertEq(5, questions.size)
        assertEq(listOf("Great", "Fine"), questions[0].options)
        expect(questions[0].required)
        assertEq(SurveyQuestionType.MULTIPLE, questions[1].type)
        assertEq(7, questions[2].scale)
        assertEq(0, questions[3].scale)
        assertEq("Optional", questions[3].help)
        assertEq(SurveyQuestionType.TEXT, questions[4].type)
        assertEq(0, questions[4].scale)
        assertEq(SurveyQuestionType.RATING, SurveyQuestion.fromJson(questionJson("f1b2c3d4", "rating", "x", mapOf("scale" to 2))).type)
        assertEq(5, SurveyQuestion.fromJson(questionJson("f1b2c3d4", "rating", "x", mapOf("scale" to 2))).scale)
        val results = survey.results!!
        val single = results[0]
        assertEq("10 answers · 2 skipped", single.answeredText)
        assertEq("70%", single.percent(7))
        assertEq(0.7f, single.share(7))
        assertEq("7 · 70%", single.optionLine(single.options[0]))
        val rating = results[1]
        assertEq(7, rating.scale)
        assertEq("4.3 of 7", rating.averageText)
        assertEq(2, rating.distributionPeak)
        assertEq(0.5f, rating.distributionShare(1))
        assertEq(listOf(1 to 0, 2 to 0, 3 to 1, 4 to 1, 5 to 0, 6 to 2, 7 to 0), rating.ratingRows)
        val text = results[2]
        assertEq(listOf("More coffee", "Loved it"), text.answers)
        assertEq("—", SurveyResult("x", SurveyQuestionType.RATING).averageText)
        assertEq(0f, SurveyResult("x").share(3))
        assertEq("1 answer", SurveyResult("x", answered = 1).answeredText)
        assertEq("a1b2c3d4", survey.question("a1b2c3d4")?.id)
    }

    fun testActionsFollowTheStatus() {
        val draft = Survey.fromJson(surveyJson(status = "draft", extra = mapOf("responseCount" to 0)))
        assertEq(listOf(SurveyAction.EDIT, SurveyAction.PUBLISH, SurveyAction.DUPLICATE, SurveyAction.DELETE), draft.actions)
        assertEq(null, draft.publishProblem)
        assertEq("Add at least one question before opening the survey.", draft.copy(questionCount = 0).publishProblem)
        val open = Survey.fromJson(surveyJson())
        assertEq(listOf(SurveyAction.EDIT, SurveyAction.CLOSE, SurveyAction.DUPLICATE, SurveyAction.SHARE, SurveyAction.EXPORT, SurveyAction.DELETE), open.actions)
        expect(open.acceptsAnswers)
        val scheduled = Survey.fromJson(surveyJson(display = "scheduled"))
        expect(scheduled.canClose && !scheduled.acceptsAnswers)
        val closed = Survey.fromJson(surveyJson(status = "closed", extra = mapOf("canDelete" to false)))
        assertEq(listOf(SurveyAction.EDIT, SurveyAction.REOPEN, SurveyAction.DUPLICATE, SurveyAction.SHARE, SurveyAction.EXPORT), closed.actions)
        assertEq("open", SurveyAction.PUBLISH.wire)
        assertEq("open", SurveyAction.REOPEN.wire)
        assertEq("close", SurveyAction.CLOSE.wire)
        assertEq("duplicate", SurveyAction.DUPLICATE.wire)
        assertEq(null, SurveyAction.DELETE.wire)
    }

    fun testOverviewFiltersAndSummary() {
        val overview = SurveysOverview.fromJson(
            mapOf(
                "organization" to "acme", "role" to "member", "canManagePlugin" to false,
                "surveys" to listOf(
                    surveyJson(),
                    surveyJson(id = hexB, status = "draft", extra = mapOf("title" to "Onboarding check-in")),
                    surveyJson(id = "c".repeat(32), status = "open", display = "scheduled"),
                    surveyJson(id = "d".repeat(32), status = "open", display = "closed"),
                    surveyJson(id = "nothex"),
                ),
                "summary" to mapOf("surveys" to 4, "open" to 2, "drafts" to 1, "closed" to 1, "responses" to 48, "responsesLast7Days" to 5),
            ),
        )
        assertEq(4, overview.surveys.size)
        expect(!overview.canManage)
        assertEq(4, overview.count(SurveyFilter.ALL))
        assertEq(1, overview.count(SurveyFilter.OPEN))
        assertEq(1, overview.count(SurveyFilter.SCHEDULED))
        assertEq(1, overview.count(SurveyFilter.DRAFT))
        assertEq(1, overview.count(SurveyFilter.CLOSED))
        assertEq(listOf(hexB), overview.filtered("onboard", SurveyFilter.ALL).map { it.id })
        assertEq(emptyList<String>(), overview.filtered("onboard", SurveyFilter.OPEN).map { it.id })
        assertEq(48, overview.summary.responses)
        assertEq("2 open · 1 draft · 1 closed", overview.summary.surveysDetail)
        assertEq("5 in the last 7 days", overview.summary.weekDetail)
        assertEq("None in the last 7 days", SurveySummary().weekDetail)
        assertEq(SurveyFilter.CLOSED, SurveyFilter.of("closed"))
        assertEq(SurveyFilter.ALL, SurveyFilter.of("nope"))
        expect(SurveysOverview("acme", role = "admin").canManage)
    }

    fun testQuestionDraftValidatesAndBuildsTheBody() {
        val random = Random(7)
        val single = SurveyQuestionDraft.blank(SurveyQuestionType.SINGLE, random)
        expect(SurveyLimits.isQuestionId(single.id))
        assertEq(listOf("", ""), single.options)
        expect(single.isBlank)
        assertEq(null, single.problem(1))
        val titled = single.copy(title = "Pick one").option(0, " Red ").option(1, "Red")
        assertEq("Question 1 needs at least 2 different options.", titled.problem(1))
        val fixed = titled.option(1, "Blue").addOption().option(2, "  ")
        assertEq(null, fixed.problem(1))
        assertEq(listOf("Red", "Blue"), fixed.cleanOptions)
        val body = fixed.copy(required = true).body
        assertEq(single.id, body["id"])
        assertEq("single", body["type"])
        assertEq(listOf("Red", "Blue"), body["options"])
        assertEq(0, body["scale"])
        assertEq(true, body["required"])
        val many = (1..31).fold(fixed.copy(options = emptyList())) { draft, n -> draft.copy(options = draft.options + "Option $n") }
        assertEq("Question 2 can have at most 30 options.", many.problem(2))
        assertEq(31, many.addOption().options.size)
        assertEq(listOf("Blue"), fixed.removeOption(0).removeOption(1).options)
        val rating = fixed.withType(SurveyQuestionType.RATING)
        assertEq(5, rating.scale)
        assertEq(10, rating.stepScale(9).scale)
        assertEq(3, rating.stepScale(-9).scale)
        assertEq(emptyList<String>(), rating.body["options"])
        assertEq(5, rating.body["scale"])
        assertEq("Question 3's rating scale runs from 3 to 10.", rating.copy(scale = 11).problem(3))
        val text = SurveyQuestionDraft.blank(SurveyQuestionType.TEXT, random)
        assertEq(emptyList<String>(), text.options)
        assertEq("Give question 4 a title.", text.copy(help = "Why?").problem(4))
        assertEq(listOf("", ""), text.withType(SurveyQuestionType.MULTIPLE).options)
        expect("id" !in SurveyQuestionDraft(id = "bad", title = "x").body)
        val fromServer = SurveyQuestionDraft.of(SurveyQuestion("zz", SurveyQuestionType.RATING, "Rate", scale = 4), random)
        expect(SurveyLimits.isQuestionId(fromServer.id))
        assertEq(4, fromServer.scale)
        assertEq("Question 1's title is longer than 300 characters.", text.copy(title = "x".repeat(301)).problem(1))
    }

    fun testSurveyDraftValidatesSettingsAndQuestions() {
        val random = Random(3)
        val blank = SurveyDraft.blank(random)
        assertEq(1, blank.questions.size)
        assertEq("Give the survey a title.", blank.problem(utc))
        val named = blank.copy(title = "  Pulse  ")
        assertEq(null, named.problem(utc))
        val body = named.body(utc)
        assertEq("Pulse", body["title"])
        assertEq(emptyList<Any>(), body["questions"])
        @Suppress("UNCHECKED_CAST")
        val settings = body["settings"] as Map<String, Any?>
        assertEq(true, settings["notifyOnResponse"])
        assertEq(0L, settings["opensAt"])
        assertEq(0, settings["maxResponses"])
        val scheduled = named.copy(opens = "2026-01-10 09:30", closes = "2026-01-11", maxResponses = "250", oneResponsePerPerson = true, shuffleOptions = true)
        assertEq(null, scheduled.problem(utc))
        val parsed = scheduled.settings(utc)
        assertEq(jan10 + 9 * 3_600_000L + 30 * 60_000L, parsed.opensAt)
        assertEq(jan10 + 86_400_000L, parsed.closesAt)
        assertEq(250, parsed.maxResponses)
        expect(parsed.oneResponsePerPerson && parsed.shuffleOptions)
        assertEq("Write the opening time as YYYY-MM-DD HH:MM.", named.copy(opens = "soon").problem(utc))
        assertEq("Write the closing time as YYYY-MM-DD HH:MM.", named.copy(closes = "10/01/2026").problem(utc))
        assertEq("The survey must close after it opens.", named.copy(opens = "2026-01-11", closes = "2026-01-10").problem(utc))
        assertEq("The response limit is a number from 0 to 10,000; 0 means no limit.", named.copy(maxResponses = "20000").problem(utc))
        assertEq("The response limit is a number from 0 to 10,000; 0 means no limit.", named.copy(maxResponses = "lots").problem(utc))
        val question = named.question(0) { it.copy(title = "Colour?").option(0, "Red") }
        assertEq("Question 1 needs at least 2 different options.", question.problem(utc))
        val two = question.question(0) { it.option(1, "Blue") }.addQuestion(SurveyQuestionType.RATING, random).question(1) { it.copy(title = "Rate") }
        assertEq(null, two.problem(utc))
        assertEq(listOf("Rate", "Colour?"), two.moveQuestion(1, -1).questions.map { it.title })
        assertEq(two, two.moveQuestion(0, -1))
        assertEq(two, two.moveQuestion(1, 1))
        assertEq(listOf("Rate"), two.removeQuestion(0).questions.map { it.title })
        @Suppress("UNCHECKED_CAST")
        val questions = two.addQuestion(SurveyQuestionType.TEXT, random).body(utc)["questions"] as List<Map<String, Any?>>
        assertEq(listOf("single", "rating"), questions.map { it["type"] })
        val full = (1..60).fold(named.copy(questions = emptyList())) { draft, _ -> draft.addQuestion(SurveyQuestionType.TEXT, random) }
        assertEq(50, full.questions.size)
        expect(!full.canAddQuestion)
        assertEq(full.questions.size, full.questions.map { it.id }.toSet().size)
    }

    fun testDraftFromASurveyKeepsQuestionIdsAndSettings() {
        val survey = Survey.fromJson(
            detailJson().let { json ->
                json + mapOf("settings" to mapOf("opensAt" to jan10, "closesAt" to 0, "maxResponses" to 100, "notifyOnResponse" to false, "oneResponsePerPerson" to true))
            },
        )
        val draft = SurveyDraft.of(survey, utc, Random(1))
        assertEq(hexA, draft.id)
        expect(!draft.isNew)
        assertEq("2026-01-10 00:00", draft.opens)
        assertEq("", draft.closes)
        assertEq("100", draft.maxResponses)
        expect(!draft.notifyOnResponse && draft.oneResponsePerPerson)
        assertEq(listOf("a1b2c3d4", "b1b2c3d4", "c1b2c3d4", "d1b2c3d4", "e1b2c3d4"), draft.questions.map { it.id })
        assertEq(7, draft.questions[2].scale)
        assertEq(null, draft.problem(utc))
        @Suppress("UNCHECKED_CAST")
        val questions = draft.body(utc)["questions"] as List<Map<String, Any?>>
        assertEq("a1b2c3d4", questions[0]["id"])
        assertEq(listOf("Great", "Fine"), questions[0]["options"])
        assertEq(7, questions[2]["scale"])
        assertEq("", SurveyDraft.of(Survey(hexB), utc).maxResponses)
    }

    fun testResponsesAndExportFormatting() {
        val survey = Survey.fromJson(detailJson())
        val questions = survey.questions!!
        val response = SurveyResponse.fromJson(
            mapOf("id" to "f".repeat(32), "submittedAt" to jan10, "answers" to mapOf("a1b2c3d4" to "Great", "b1b2c3d4" to listOf("Food", "Talks"), "c1b2c3d4" to 6, "d1b2c3d4" to "  ")),
        )
        assertEq("Great", response.answer(questions[0]))
        assertEq("Food; Talks", response.answer(questions[1]))
        assertEq("6 of 7", response.answer(questions[2]))
        assertEq("", response.answer(questions[3]))
        assertEq(3, response.answeredCount)
        assertEq("How was it?" to "Great", response.lines(questions)[0])
        assertEq("4.5", SurveyFormat.answer(4.5))
        assertEq("4", SurveyFormat.answer(4.0))
        expect(SurveyFormat.isEmptyAnswer(listOf("", null)))
        val list = SurveyResponse.list(mapOf("responses" to listOf(mapOf("id" to "1"), mapOf("id" to ""), mapOf("id" to "../x"))))
        assertEq(listOf("1"), list.map { it.id })
        val export = SurveyExport.fromJson(mapOf("filename" to "Team offsite/feedback 2026.csv", "csv" to "Submitted,How was it?\n2026-01-10,Great\n2026-01-11,Fine\n"))
        assertEq("Team-offsite-feedback-2026.csv", export.safeFilename)
        assertEq(2, export.rows)
        assertEq("survey.csv", SurveyExport("../..", "").safeFilename)
        assertEq("results.csv", SurveyExport("results", "").safeFilename)
        assertEq("4.0", SurveyFormat.oneDecimal(4.0))
        assertEq("0%", SurveyFormat.percent(3, 0))
        assertEq("33%", SurveyFormat.percent(1, 3))
    }

    fun testDatesUseTheGivenZone() {
        assertEq("2026-01-10 00:00", SurveyDates.input(jan10, utc))
        assertEq("", SurveyDates.input(0, utc))
        assertEq(jan10, SurveyDates.parse("2026-01-10", utc))
        assertEq(jan10 + 60_000, SurveyDates.parse("2026-01-10T00:01", utc))
        assertEq(null, SurveyDates.parse("  ", utc))
        assertEq(null, SurveyDates.parse("2026-13-01", utc))
        assertEq("10 Jan 2026, 00:00", SurveyDates.label(jan10, utc))
        assertEq("—", SurveyDates.label(0, utc))
    }

    fun testClientCallsEveryRoute() {
        val transport = FakeTransport()
        val client = PackagesClient(transport)
        transport.answer("GET", "/api/surveys", mapOf("actor" to "alice", "organizations" to listOf(mapOf("name" to "acme", "role" to "owner", "installed" to true), mapOf("name" to "Bad Org"))))
        assertEq(listOf("acme"), client.surveyOrganizations().map { it.name })
        transport.answer("GET", "/api/surveys/acme", mapOf("organization" to "acme", "surveys" to listOf(surveyJson())))
        assertEq(1, client.surveys("acme").surveys.size)
        transport.answer("GET", "/api/surveys/acme/items/$hexA", mapOf("survey" to detailJson()))
        assertEq(5, client.survey("acme", hexA).questions?.size)
        transport.answer("POST", "/api/surveys/acme/items", mapOf("survey" to surveyJson(id = hexB, status = "draft")), 201)
        val created = client.saveSurvey("acme", SurveyDraft(title = " New "), utc)
        assertEq(hexB, created.id)
        val sent = transport.last("POST", "/api/surveys/acme/items")!!.body!!
        assertEq("New", sent["title"])
        expect(sent.keys.containsAll(listOf("title", "description", "thankYou", "questions", "settings")))
        transport.answer("PATCH", "/api/surveys/acme/items/$hexA", mapOf("survey" to surveyJson()))
        client.saveSurvey("acme", SurveyDraft(id = hexA, title = "Edit"), utc)
        assertEq("Edit", transport.last("PATCH", "/api/surveys/acme/items/$hexA")!!.body!!["title"])
        transport.answer("DELETE", "/api/surveys/acme/items/$hexA", mapOf("deleted" to hexA))
        client.deleteSurvey("acme", hexA)
        expect(transport.last("DELETE", "/api/surveys/acme/items/$hexA") != null)
        transport.answer("POST", "/api/surveys/acme/items/$hexA/open", mapOf("survey" to surveyJson()))
        assertEq("open", client.surveyAction("acme", hexA, SurveyAction.PUBLISH).status)
        client.surveyAction("acme", hexA, SurveyAction.REOPEN)
        expect(transport.sent.count { it.path == "/api/surveys/acme/items/$hexA/open" } == 2)
        transport.answer("POST", "/api/surveys/acme/items/$hexA/close", mapOf("survey" to surveyJson(status = "closed")))
        assertEq("closed", client.surveyAction("acme", hexA, SurveyAction.CLOSE).status)
        transport.answer("POST", "/api/surveys/acme/items/$hexA/duplicate", mapOf("survey" to surveyJson(id = hexB, status = "draft", extra = mapOf("title" to "Copy of Team offsite feedback"))), 201)
        assertEq("Copy of Team offsite feedback", client.surveyAction("acme", hexA, SurveyAction.DUPLICATE).title)
        assertFails<PackagesException> { client.surveyAction("acme", hexA, SurveyAction.EXPORT) }
        transport.answer(
            "GET", "/api/surveys/acme/items/$hexA/responses",
            mapOf("responses" to listOf(mapOf("id" to "e".repeat(32), "submittedAt" to 2, "answers" to mapOf("a1b2c3d4" to "Fine")), mapOf("id" to "d".repeat(32), "submittedAt" to 1))),
        )
        val responses = client.surveyResponses("acme", hexA)
        assertEq(2, responses.size)
        assertEq("Fine", responses[0].answers["a1b2c3d4"])
        val rid = "e".repeat(32)
        transport.answer("DELETE", "/api/surveys/acme/items/$hexA/responses/$rid", mapOf("deleted" to rid, "survey" to surveyJson(extra = mapOf("responseCount" to 11))))
        assertEq(11, client.deleteSurveyResponse("acme", hexA, rid)?.responseCount)
        assertFails<PackagesException> { client.deleteSurveyResponse("acme", hexA, "../../x") }
        transport.answer("GET", "/api/surveys/acme/items/$hexA/export", mapOf("filename" to "team.csv", "csv" to "a,b\n1,2\n"))
        assertEq("team.csv", client.exportSurvey("acme", hexA).filename)
        transport.answer(
            "GET", "/api/surveys/-/public/$token",
            mapOf("survey" to mapOf("title" to "Pulse", "status" to "open", "questions" to listOf(questionJson("a1b2c3d4", "text", "Why?")), "organization" to "acme", "shuffleOptions" to true)),
        )
        val public = client.publicSurvey(token)
        expect(public.acceptsAnswers && public.shuffleOptions)
        assertEq(1, public.questions.size)
        transport.answer("POST", "/api/surveys/-/public/$token", mapOf("thankYou" to "Thanks!"), 201)
        assertEq("Thanks!", client.submitSurvey(token, mapOf("a1b2c3d4" to "Because")))
        assertEq(mapOf("a1b2c3d4" to "Because"), transport.last("POST", "/api/surveys/-/public/$token")!!.body!!["answers"])
        assertFails<PackagesException> { client.publicSurvey("nope") }
        assertFails<PackagesException> { client.survey("acme", "nope") }
        assertFails<PackagesException> { client.surveys("Bad Org") }
    }

    fun testErrorsReadPlainly() {
        assertEq("Add at least one question before opening the survey.", SurveyErrors.describe(PackagesException("survey_has_no_questions", 409)))
        assertEq("Only the survey's creator or an owner or admin of the organization can delete it.", SurveyErrors.describe(PackagesException("survey_delete_forbidden", 403)))
        assertEq("Choice questions need at least 2 different options.", SurveyErrors.describe(PackagesException("invalid_question_options", 400)))
        assertEq("Surveys is not installed for that organization yet.", SurveyErrors.describe(PackagesException("plugin_not_installed", 409)))
        assertEq("Sign in again to continue.", SurveyErrors.describe(PackagesException("invalid_session", 401)))
        assertEq("Something went wrong.", SurveyErrors.describe(IllegalStateException()))
        assertEq(403, SurveyErrors.status(PackagesException("survey_delete_forbidden", 403)))
        assertEq("", SurveyErrors.code(IllegalStateException()))
        for (code in listOf("survey_not_found", "survey_closed", "already_responded", "too_many_responses", "invalid_answer", "answer_required")) {
            expect(SurveyErrors.sentence(code) != null, code)
        }
    }

    fun testModelPicksAnOrganizationAndHandlesInstall() {
        val transport = FakeTransport()
        val model = SurveysModel(PackagesClient(transport))
        transport.answer(
            "GET", "/api/surveys",
            mapOf("organizations" to listOf(mapOf("name" to "beta", "role" to "member"), mapOf("name" to "acme", "role" to "owner", "installed" to true))),
        )
        transport.answer("GET", "/api/surveys/acme", mapOf("organization" to "acme", "role" to "owner", "surveys" to listOf(surveyJson())))
        model.refresh()
        assertEq("acme", model.organization)
        expect(model.overview != null && model.canManage && !model.needsInstall)
        model.adopt(Survey.fromJson(detailJson()).copy(title = "Renamed"))
        assertEq("Renamed", model.overview!!.surveys[0].title)
        assertEq(null, model.overview!!.surveys[0].questions)
        model.adopt(Survey.fromJson(surveyJson(id = hexB, status = "draft")))
        assertEq(listOf(hexB, hexA), model.overview!!.surveys.map { it.id })
        model.forget(hexB)
        assertEq(listOf(hexA), model.overview!!.surveys.map { it.id })
        model.choose("beta")
        expect(model.needsInstall && !model.canInstall && model.overview == null)
        assertEq(null, transport.last("GET", "/api/surveys/beta"))
        transport.answer("POST", "/api/plugins/beta/surveys/install", mapOf("organization" to "beta", "plugins" to emptyList<Any>()))
        transport.answer("GET", "/api/surveys/beta", mapOf("organization" to "beta", "surveys" to emptyList<Any>()))
        expect(model.install())
        expect(!model.needsInstall && model.overview != null)
        transport.answer("GET", "/api/surveys/beta", mapOf("error" to "plugin_not_installed", "canInstall" to false), 409)
        model.load()
        expect(model.needsInstall && model.overview == null)
        assertEq("", model.errorText)
        model.refresh("acme")
        assertEq("acme", model.organization)
        transport.answer("GET", "/api/surveys/acme", mapOf("error" to "rate_limited"), 429)
        model.load()
        expect(model.errorText.isNotEmpty())
        assertEq(null, model.locate("nope"))
    }

    fun testModelLocatesASurveyFromAPing() {
        val transport = FakeTransport()
        val model = SurveysModel(PackagesClient(transport))
        transport.answer(
            "GET", "/api/surveys",
            mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true), mapOf("name" to "beta", "installed" to true))),
        )
        transport.answer("GET", "/api/surveys/acme", mapOf("organization" to "acme", "surveys" to emptyList<Any>()))
        transport.answer("GET", "/api/surveys/beta", mapOf("organization" to "beta", "surveys" to listOf(surveyJson(id = hexB))))
        transport.answer("GET", "/api/surveys/beta/items/$hexB", mapOf("survey" to surveyJson(id = hexB)))
        transport.answer("GET", "/api/surveys/acme/items/$hexB", mapOf("error" to "survey_not_found"), 404)
        model.refresh()
        assertEq(hexB, model.locate(hexB)?.id)
        assertEq("beta", model.organization)
        assertEq("beta", model.overview?.organization)
    }

    fun testRoutesAndPackageRegistration() {
        assertEq(Route.Kind.Surveys, PackageGate.ROUTE_KINDS.entries.first { it.value == "surveys" }.key)
        assertEq("surveys", PackageGate.HUB_ITEMS[Route.Kind.Surveys.raw])
        expect("surveys" in PackageGate.ORGANIZATION_PACKAGES)
        assertEq("surveys", MarketPackage.NATIVE_DESTINATIONS["surveys"])
        assertEq("surveys", PackageRoutes.destination(MarketPackage(id = "surveys", name = "Surveys")))
        expect("surveys" in PackageRoutes.BUSINESS_PACKAGE_IDS)
        assertEq(listOf("market", "crm", "surveys", "ads", "ledger"), PackageRoutes.hubRows(setOf("surveys")).map { it.id })
        expect(HubCatalog.business.any { it.id == "surveys" && it.needsRegistration })
        assertEq("Surveys", RouteCatalog.title(Route.Kind.Surveys))
        val surfaces = PackageSurfaces.of { it != "surveys" }
        expect("surveys" in surfaces.hubItems)
        expect(!surfaces.showsRoute(Route.Surveys(null, null)))
        assertEq(listOf(Route.Surveys("acme", hexA)), RouteParser.parse("https://app.forkmesh.com/dashboard/surveys?org=acme&survey=$hexA")!!.stack)
        assertEq(listOf(Route.Surveys(null, hexA)), RouteParser.parse("https://app.forkmesh.com/dashboard/surveys?survey=$hexA")!!.stack)
        assertEq(listOf(Route.Surveys(null, null)), RouteParser.parse("https://app.forkmesh.com/dashboard/surveys")!!.stack)
    }
}
