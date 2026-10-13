package com.forkmesh.nativeapp.teammates

import com.forkmesh.nativeapp.agents.ops.FakeControlChannel
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class TeammatesTest {
    private val listJson: Map<String, Any?> = mapOf(
        "repo" to "f/forkmesh",
        "repos" to listOf("f/forkmesh", "f/other"),
        "available" to true,
        "summary" to mapOf("total" to 3L, "working" to 1L, "scheduled" to 1L, "idle" to 1L),
        "teammates" to listOf(
            mapOf("id" to "zed", "name" to "Zed Idle", "status" to "Idle", "schedule" to mapOf("mode" to "on-demand")),
            mapOf(
                "id" to "qa", "name" to "Quinn", "status" to "Working", "provider" to "codex", "model" to "gpt",
                "schedule" to mapOf("mode" to "daily", "time" to "07:30", "active" to true, "human" to "Daily at 07:30"),
                "mood" to mapOf(
                    "key" to "needs-you", "emoji" to "🙋", "label" to "Needs you", "motion" to "wave", "attention" to true,
                    "intensity" to 0.8, "reasons" to listOf("waiting for your reply", "ran today"),
                ),
            ),
            mapOf(
                "id" to "cfo", "name" to "Cass", "status" to "Scheduled", "workflow" to "contributor-payouts",
                "schedule" to mapOf("mode" to "weekly", "days" to listOf(1L, 9L), "active" to true),
            ),
        ),
    )

    private fun near(expected: Double, actual: Double?) = expect(actual != null && abs(expected - actual) < 0.001, "expected $expected, got $actual")

    fun testListSortsAndFilters() {
        val channel = FakeControlChannel()
        channel.reply("teammates.list", listJson)
        val store = TeammatesStore(channel)
        store.refresh()
        val snapshot = store.snapshot!!
        assertEq(listOf("qa", "cfo", "zed"), snapshot.sorted.map { it.id })
        assertEq(listOf("qa"), snapshot.filtered(TeammateStatus.Working).map { it.id })
        assertEq("3 special agents · 1 working · 1 scheduled · 1 idle", snapshot.summary.line)
        assertEq(2, snapshot.automaticSchedules)
        assertEq(listOf(1), snapshot.teammates[2].schedule.days)
        expect(!store.teammate("cfo")!!.takesPrompts)
        assertEq("Q", store.teammate("qa")!!.initials)
        assertEq("ZI", store.teammate("zed")!!.initials)
        val mood = store.teammate("qa")!!.mood
        assertEq("🙋", mood.emoji)
        assertEq("wave", mood.motion)
        expect(mood.attention)
        assertEq("Needs you: waiting for your reply, ran today", mood.line)
        expect(store.teammate("zed")!!.mood.isEmpty)
    }

    fun testChangedBellRefreshes() {
        val channel = FakeControlChannel()
        channel.reply("teammates.list", listJson)
        val store = TeammatesStore(channel)
        channel.emit("insights.changed")
        assertEq(0, channel.methods().count { it == "teammates.list" })
        channel.emit("teammates.changed")
        assertEq(1, channel.methods().count { it == "teammates.list" })
        assertEq(1, store.changesSeen)
        store.stop()
        assertEq(0, channel.tapCount)
    }

    fun testPerformCarriesTheToken() {
        val channel = FakeControlChannel()
        channel.reply("teammates.prepare", mapOf("token" to "t", "action" to "grade", "subject" to "4:5", "summary" to "Grade"))
        channel.reply("teammates.run", mapOf("started" to true, "message" to "Graded 5 out of 5."))
        val store = TeammatesStore(channel)
        assertEq("Graded 5 out of 5.", store.perform(TeammateAction.Grade(4, 5)))
        assertEq(4L, channel.params("teammates.prepare")!!["session"])
        val sent = channel.params("teammates.run")!!
        assertEq("t", sent.str("token"))
        assertEq("grade", sent.str("action"))
        assertEq(5L, sent["grade"])
        assertEq(1, channel.methods().count { it == "teammates.list" })
    }

    fun testUnsupportedDesktop() {
        val channel = FakeControlChannel()
        channel.failures["teammates.list"] = LocalControlException("unsupported")
        val store = TeammatesStore(channel)
        store.refresh()
        expect(store.unsupported)
    }

    fun testDetailBriefingStatsAndRoom() {
        val channel = FakeControlChannel()
        channel.reply(
            "teammates.get",
            mapOf(
                "id" to "qa", "name" to "Quinn", "mode" to "Plan", "instructions" to "Test everything",
                "backupModels" to listOf(mapOf("provider" to "claude-code", "model" to "opus")),
                "taskAgents" to listOf(mapOf("taskType" to "new-issues", "provider" to "codex", "model" to "gpt")),
                "permissions" to listOf(mapOf("id" to "read-repositories", "label" to "Read repositories")),
                "logMonitors" to listOf(mapOf("badge" to "error", "threshold" to 3L, "taskType" to "error-logs", "active" to true)),
            ),
        )
        channel.reply(
            "teammates.briefing",
            mapOf(
                "updates" to 2L, "completed" to 1L, "inProgress" to 1L,
                "entries" to listOf(mapOf("id" to "qa", "name" to "Quinn", "sessionId" to 3L, "output" to "All green"), mapOf("id" to "zed", "name" to "Zed")),
            ),
        )
        channel.reply(
            "teammates.stats",
            mapOf(
                "teammates" to listOf(mapOf("id" to "qa", "name" to "Quinn", "runs" to 4L, "succeeded" to 3L, "costUsd" to 1.5)),
                "tasks" to listOf(mapOf("session" to 9L, "title" to "t", "status" to "success", "gradable" to true, "grade" to 0L)),
                "totalTasks" to 1L,
            ),
        )
        channel.reply("teammates.room", mapOf("messages" to listOf(mapOf("id" to "m1", "sender" to "Quinn", "text" to "done", "t" to 5L))))
        val store = TeammatesStore(channel)
        val detail = store.detail("qa")
        assertEq("Plan", detail.mode)
        assertEq("opus", detail.backupModels.first().model)
        assertEq("new-issues", detail.taskAgents.first().taskType)
        assertEq("Read repositories", detail.permissions.first().label)
        val briefing = store.briefing()
        assertEq("2 updates · 1 done · 1 in progress", briefing.headline)
        assertEq(listOf("qa"), briefing.entries.filter { it.hasUpdate }.map { it.id })
        val stats = store.stats()
        near(0.75, stats.teammates[0].successRate)
        expect(stats.tasks[0].gradable)
        assertEq("done", store.room().first().text)
        assertEq(100L, channel.params("teammates.room")!!["limit"])
    }

    fun testErrorText() {
        assertEq("permissions is edited on the desktop.", TeamErrorText.describe("field_not_editable:permissions"))
        assertEq("The desktop refused the model field.", TeamErrorText.describe("bad_field:model"))
    }

    private fun sampleDetail() = TeammateDetail.from(
        mapOf(
            "id" to "qa", "name" to "Quinn", "avatar" to "sol", "provider" to "codex", "model" to "gpt-5", "skills" to listOf("pytest"),
            "schedule" to mapOf("mode" to "daily", "time" to "07:30", "active" to true),
            "backupModels" to listOf(mapOf("provider" to "claude-code", "model" to "opus")),
        ),
    )

    fun testUnchangedDraftSendsNothing() {
        val original = TeammateDraft.from(sampleDetail())
        expect(TeammateEditorLogic.changes(original, original).isEmpty())
        expect(TeammateEditorLogic.validate(original).isEmpty())
    }

    fun testChangedFieldsOnly() {
        val original = TeammateDraft.from(sampleDetail())
        val draft = original.copy(
            focus = "  flaky tests  ",
            schedule = original.schedule.copy(days = listOf(5, 1), mode = "custom"),
            enabled = false,
        )
        val changes = TeammateEditorLogic.changes(original, draft)
        assertEq(setOf("focus", "schedule", "enabled"), changes.keys)
        assertEq("flaky tests", changes.str("focus"))
        assertEq("custom", changes.obj("schedule").str("mode"))
        assertEq(false, changes["enabled"])
    }

    fun testNewTeammateSendsWhatIsSet() {
        val changes = TeammateEditorLogic.changes(null, TeammateDraft(name = "Dee", model = "gpt-5"))
        assertEq("Dee", changes.str("name"))
        assertEq("codex", changes.str("provider"))
        expect(!changes.containsKey("focus"))
        expect(changes.containsKey("schedule"))
    }

    fun testValidation() {
        var draft = TeammateDraft()
        expect(TeammateDraftIssue.Name in TeammateEditorLogic.validate(draft))
        expect(TeammateDraftIssue.Model in TeammateEditorLogic.validate(draft))
        draft = draft.copy(name = "Dee", model = "gpt-5")
        expect(TeammateEditorLogic.validate(draft).isEmpty())
        draft = draft.copy(
            relayChannel = "team",
            schedule = draft.schedule.copy(time = "25:00"),
            mode = "Yolo",
            backupModels = listOf(AgentChoiceRow(provider = "codex", model = "")),
        )
        val issues = TeammateEditorLogic.validate(draft)
        expect(TeammateDraftIssue.RelayChannel in issues)
        expect(TeammateDraftIssue.Schedule in issues)
        expect(TeammateDraftIssue.Mode in issues)
        expect(TeammateDraftIssue.Choices in issues)
        expect(TeammateEditorLogic.usesDays("custom"))
        expect(!TeammateEditorLogic.usesTime("hourly"))
    }
}
