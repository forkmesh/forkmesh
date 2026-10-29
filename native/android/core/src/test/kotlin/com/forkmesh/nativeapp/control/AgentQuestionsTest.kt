package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.testing.assertEq

class AgentQuestionsTest {
    private val payload = mapOf(
        "questions" to listOf(
            mapOf(
                "id" to "toolu_1",
                "input" to mapOf(
                    "questions" to listOf(
                        mapOf(
                            "header" to "Scope",
                            "question" to "Which scope?",
                            "options" to listOf(mapOf("label" to "Vendor", "description" to "Copy it in"), mapOf("label" to "Skip")),
                        ),
                        mapOf("header" to "Ship", "multiSelect" to true, "options" to listOf(mapOf("label" to "iOS"), mapOf("label" to "Android"))),
                    ),
                ),
            ),
            mapOf("id" to "", "input" to mapOf("questions" to listOf(mapOf("question" to "dropped")))),
            mapOf("id" to "toolu_2", "input" to emptyMap<String, Any?>()),
        ),
    )

    fun testParsesOnlyAnswerableQuestions() {
        val questions = AgentQuestion.listFromJson(payload)
        assertEq(listOf("toolu_1"), questions.map { it.id })
        val fields = questions.single().fields
        assertEq(listOf("Which scope?", "Ship"), fields.map { it.title })
        assertEq(listOf(false, true), fields.map { it.multiSelect })
        assertEq(AgentQuestionOption("Vendor", "Copy it in"), fields[0].options[0])
    }

    fun testAnswerNeedsEveryFieldAndNamesThemWhenSeveral() {
        val question = AgentQuestion.listFromJson(payload).single()
        assertEq("", question.answer(mapOf("0" to setOf("Vendor")), emptyMap()))
        assertEq(
            "Which scope? → Vendor\nShip → Android, iOS",
            question.answer(mapOf("0" to setOf("Vendor"), "1" to setOf("iOS", "Android")), emptyMap()),
        )
        assertEq(
            "Which scope? → later\nShip → iOS",
            question.answer(mapOf("1" to setOf("iOS")), mapOf("0" to "  later ")),
        )
    }

    fun testSingleFieldAnswerIsTheValueAlone() {
        val question = AgentQuestion("q", listOf(AgentQuestionField("0", "Pick", false, false, emptyList())))
        assertEq("Blue", question.answer(emptyMap(), mapOf("0" to "Blue")))
    }
}
