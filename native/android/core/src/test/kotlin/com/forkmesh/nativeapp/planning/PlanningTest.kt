package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.model.RepoBlob
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneId

class FakePlanningApi : ApiService(TestSettings()) {
    val calls = mutableListOf<Triple<String, String, Map<String, Any?>?>>()
    val answers = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()
    val trees = HashMap<String, RepoTree>()
    val blobs = HashMap<String, String>()
    var issueList: List<Issue> = emptyList()
    var cleared = 0

    override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        calls.add(Triple(method, path, body))
        val key = "$method ${path.substringBefore('?')}"
        failures[key]?.let { throw it }
        return answers[key] ?: mapOf("ok" to true)
    }

    override fun tree(owner: String, name: String, path: String, ref: String): RepoTree =
        trees[path] ?: throw OrgApiException("not_found", "not found")

    override fun blob(owner: String, name: String, path: String, ref: String): RepoBlob =
        RepoBlob(path, blobs[path] ?: throw OrgApiException("not_found", "not found"))

    override fun publishedIssues(owner: String, name: String): List<Issue> = issueList

    override fun clearRepoCache(owner: String, name: String) {
        cleared++
    }
}

class PlanningTest {
    private val utc = ZoneId.of("UTC")
    private val day = 86_400_000L

    private fun ms(text: String) = Instant.parse(text).toEpochMilli()

    fun testRecurrenceExpandsWithinRangeAndUntil() {
        val weekly = CalendarEvent(
            id = "a".repeat(32), startAt = ms("2026-01-05T09:00:00Z"), endAt = ms("2026-01-05T10:00:00Z"),
            timezone = "UTC", title = "Standup", recurrence = CalendarRecurrence.WEEKLY, recurrenceUntil = ms("2026-03-01T00:00:00Z"),
        )
        val feb = CalendarMath.occurrences(weekly, ms("2026-02-01T00:00:00Z"), ms("2026-03-31T00:00:00Z"), utc)
        assertEq(listOf("2026-02-02", "2026-02-09", "2026-02-16", "2026-02-23"), feb.map { CalendarMath.dayKey(it.startAt, utc) })
        val monthly = CalendarEvent(
            id = "m", startAt = ms("2020-01-31T12:00:00Z"), endAt = ms("2020-01-31T13:00:00Z"), timezone = "UTC",
            title = "Rent", recurrence = CalendarRecurrence.MONTHLY,
        )
        val later = CalendarMath.occurrences(monthly, ms("2030-02-01T00:00:00Z"), ms("2030-04-01T00:00:00Z"), utc)
        assertEq(listOf("2030-02-28", "2030-03-31"), later.map { CalendarMath.dayKey(it.startAt, utc) })
        val single = CalendarEvent(id = "s", startAt = ms("2026-01-05T23:00:00Z"), endAt = ms("2026-01-06T01:00:00Z"), title = "Late")
        assertEq(1, CalendarMath.occurrences(single, ms("2026-01-06T00:00:00Z"), ms("2026-01-07T00:00:00Z"), utc).size)
        expect(CalendarMath.occurrences(single, ms("2026-01-06T01:00:00Z"), ms("2026-01-07T00:00:00Z"), utc).isEmpty())
        val daily = CalendarEvent(id = "d", startAt = 0, endAt = 60_000, title = "Tick", recurrence = CalendarRecurrence.DAILY)
        assertEq(CalendarMath.MAX_OCCURRENCES_PER_EVENT, CalendarMath.occurrences(daily, 0, 5_000 * day, utc).size)
    }

    fun testGridsAndDayKeys() {
        val anchor = ms("2026-09-24T15:00:00Z")
        val grid = CalendarMath.monthGrid(anchor, 2, utc)
        assertEq(42, grid.size)
        assertEq("2026-08-31", CalendarMath.dayKey(grid[0], utc))
        assertEq("2026-09-20", CalendarMath.dayKey(CalendarMath.weekDays(anchor, 1, utc).first(), utc))
        assertEq(null, CalendarMath.parseDayKey("2026-02-30", utc))
        assertEq(ms("2026-09-24T00:00:00Z"), CalendarMath.parseDayKey("2026-09-24", utc))
        val range = CalendarMath.range(CalendarMode.WEEK, anchor, 2, utc)
        assertEq(7 * day, range.end - range.start)
        val berlin = ZoneId.of("Europe/Berlin")
        val dst = CalendarMath.range(CalendarMode.WEEK, ms("2026-03-25T12:00:00Z"), 2, berlin)
        assertEq(7 * day - 3_600_000, dst.end - dst.start)
        val agenda = CalendarMath.agenda(
            CalendarMath.occurrences(
                listOf(
                    CalendarEvent(id = "x", startAt = ms("2026-09-24T09:00:00Z"), endAt = ms("2026-09-24T10:00:00Z"), title = "B"),
                    CalendarEvent(id = "y", startAt = ms("2026-09-24T09:00:00Z"), endAt = ms("2026-09-24T10:00:00Z"), title = "A"),
                    CalendarEvent(id = "z", startAt = ms("2026-09-25T09:00:00Z"), endAt = ms("2026-09-25T09:00:00Z"), title = "C"),
                ),
                ms("2026-09-24T00:00:00Z"), ms("2026-09-30T00:00:00Z"), utc,
            ),
            utc,
        )
        assertEq(listOf("2026-09-24", "2026-09-25"), agenda.map { it.day })
        assertEq(listOf("A", "B"), agenda[0].items.map { it.event.title })
    }

    fun testDraftValidationAndBody() {
        var draft = CalendarEventDraft(startAt = 1_000, endAt = 2_000, timezone = "Europe/Paris", calendar = "acme")
        assertEq("Give the event a title.", draft.validationError)
        draft = draft.copy(title = " Review ", meetingUrl = "http://meet")
        assertEq("Meeting links must start with https://.", draft.validationError)
        draft = draft.copy(meetingUrl = "https://meet.example/x", reminders = listOf(7))
        assertEq("Choose reminders from the list.", draft.validationError)
        draft = draft.copy(reminders = listOf(10, 60, 10), attendees = listOf(" Bob ", ""))
        assertEq(null, draft.validationError)
        val body = draft.body
        assertEq("Review", body["title"])
        assertEq("organization", body["scope"])
        assertEq("acme", body["calendar"])
        assertEq(listOf(60L, 10L), body["reminders"])
        assertEq(listOf("bob"), body["attendees"])
        assertEq("personal", CalendarEventDraft(startAt = 0, endAt = 1, timezone = "").body["scope"])
        assertEq("2 hours before", CalendarEventDraft.reminderLabel(120))
        assertEq("1 hour before", CalendarEventDraft.reminderLabel(60))
    }

    fun testCalendarModelLoadsMutatesAndRsvps() {
        val api = FakePlanningApi()
        val id = "b".repeat(32)
        val event = linkedMapOf<String, Any?>(
            "id" to id, "owner" to "carol", "calendar" to "personal", "scope" to "personal",
            "startAt" to ms("2026-09-24T12:00:00Z"), "endAt" to ms("2026-09-24T13:00:00Z"), "title" to "Lunch",
            "attendees" to listOf(mapOf("name" to "alice", "status" to "invited")),
        )
        api.answers["GET /api/calendar"] = mapOf(
            "ok" to true, "actor" to "Alice", "events" to listOf(event),
            "calendars" to listOf(
                mapOf("id" to "personal", "name" to "personal", "scope" to "personal"),
                mapOf("id" to "acme", "name" to "acme", "scope" to "organization", "role" to "admin"),
            ),
        )
        val model = CalendarModel(api, CalendarMode.DAY, zone = utc, firstWeekday = 2, clock = { ms("2026-09-24T10:30:00Z") })
        model.load()
        assertEq("alice", model.actor)
        assertEq(2, model.calendars.size)
        assertEq(listOf("Lunch"), model.occurrencesOnDay(model.today).map { it.event.title })
        assertEq(false, model.canEdit(model.events[0]))
        expect(api.calls[0].second.startsWith("/api/calendar?start="))
        expect(!model.needsLoad())
        val draft = model.newDraft()
        assertEq(ms("2026-09-24T11:00:00Z"), draft.startAt)
        assertEq(null, model.create(draft.copy(endAt = draft.startAt)))
        assertEq(1, api.calls.size)
        val created = LinkedHashMap(event).apply { put("id", "c".repeat(32)); put("owner", "alice"); put("title", "Mine") }
        api.answers["POST /api/calendar"] = mapOf("ok" to true, "event" to created)
        val made = model.create(draft.copy(title = "Mine"))
        assertEq("Mine", made?.title)
        expect(model.canEdit(made!!))
        expect(model.rsvp(id, "accepted"))
        assertEq("accepted", model.event(id)?.attendeeStatus("alice"))
        expect(!model.rsvp(id, "maybe"))
        expect(!model.delete("../x"))
        expect(model.delete(id))
        assertEq(null, model.event(id))
        api.failures["GET /api/calendar"] = OrgApiException("forbidden", "no")
        model.load()
        assertEq("You do not have access to that.", model.error)
        model.step(1)
        assertEq("2026-09-25", model.anchorKey)
        model.mode = CalendarMode.MONTH
        model.step(1)
        assertEq("2026-10-01", model.anchorKey)
    }

    fun testTimersSurviveRestartAndSettle() {
        var now = 1_000_000L
        val kv = MemoryPrefStore()
        var next = 0
        fun store() = PlanningTimerStore(kv, { now }, { "t${++next}" })
        val a = store()
        val watch = a.startStopwatch("  Deep work ")
        assertEq("Deep work", watch.label)
        now += 90_000
        a.pause(watch.id)
        now += 600_000
        assertEq(90_000L, a.timers[0].elapsedMs(now))
        expect(a.timers[0].isPaused)
        val b = store()
        assertEq(1, b.timers.size)
        b.resume(watch.id)
        now += 10_000
        assertEq("1:40", b.timers[0].display(now))
        val countdown = b.startCountdown("", 60)
        assertEq("Countdown", countdown.label)
        assertEq(listOf("timer-${countdown.id}"), b.notificationPlan().map { it.id })
        assertEq(countdown.id, b.activityTimer()?.id)
        now += 61_000
        val rung = b.settle()
        assertEq(listOf(countdown.id), rung.map { it.id })
        assertEq(now - 1_000, rung[0].finishedAtMs)
        assertEq("Done", b.timers.first { it.id == countdown.id }.display(now))
        assertEq(emptyList<PlanningTimer>(), b.settle())
        assertFails<PlanningTimerException> { b.startCountdown("x", 0) }
        assertFails<PlanningTimerException> { b.setAlarm("x", now - 1) }
        val alarm = b.setAlarm("Wake", now + 5_000)
        assertEq("0:05", alarm.display(now))
        b.removeFinished()
        assertEq(2, b.timers.size)
        assertEq(2, store().timers.size)
        assertEq("1:01", PlanningTimer.clock(61_500, false))
        assertEq("1:02", PlanningTimer.clock(61_500, true))
        assertEq("1:02:03", PlanningTimer.clock(3_723_000, false))
        val ping = TimerPings.body(rung[0])!!
        assertEq("countdown", ping["kind"])
        assertEq(null, TimerPings.body(watch))
        val api = FakePlanningApi()
        assertEq(1, TimerPings.post(rung, api, now))
        assertEq(0, TimerPings.post(rung, api, now + TimerPings.MAX_AGE_MS + 5_000))
    }

    fun testTimerStoreCapAndCorruptData() {
        val kv = MemoryPrefStore(mapOf(PlanningTimerStore.STORAGE_KEY to "not json"))
        val store = PlanningTimerStore(kv, { 10L })
        assertEq(0, store.timers.size)
        repeat(PlanningTimerStore.MAX_TIMERS) { store.startStopwatch("t$it") }
        assertFails<PlanningTimerException> { store.startStopwatch("one more") }
    }

    fun testProjectsMilestonesAndGantt() {
        val api = FakePlanningApi()
        api.trees[PlanningRecords.PROJECTS_PATH] = RepoTree(
            PlanningRecords.PROJECTS_PATH,
            listOf(
                RepoTreeEntry("1", ".forkmesh/projects/1", "dir"),
                RepoTreeEntry("2", ".forkmesh/projects/2", "dir"),
                RepoTreeEntry("3", ".forkmesh/projects/3", "dir"),
                RepoTreeEntry("notes", ".forkmesh/projects/notes", "dir"),
            ),
        )
        api.blobs[".forkmesh/projects/1/project-1.json"] =
            """{"number":1,"title":"Beta","startDate":2000,"endDate":4000,"issues":[1,2]}"""
        api.blobs[".forkmesh/projects/2/project-2.json"] =
            """{"number":2,"title":"Gone","events":[{"type":"delete"}]}"""
        api.blobs[".forkmesh/projects/3/project-3.json"] =
            """{"number":3,"title":"Alpha","startDate":1000,"endDate":3000,"milestone":"v1"}"""
        api.blobs[PlanningRecords.MILESTONES_PATH] =
            """[{"title":"v2","due":9000},{"title":"v1","due":5000},{"title":" ","due":1},{"title":"old","status":"closed"}]"""
        api.issueList = listOf(
            Issue(1, "one", status = "closed"),
            Issue(2, "two"),
            Issue(3, "three", milestone = "v1", status = "closed"),
            Issue(4, "four", milestone = "v1"),
        )
        val model = RepoPlanningModel(api, PlanningRepo.of("acme/app")!!)
        model.load(refresh = true)
        assertEq(1, api.cleared)
        assertEq(listOf(3, 1), model.shownProjects.map { it.number })
        assertEq(listOf("v1", "v2"), model.shownMilestones.map { it.title })
        model.showClosed = true
        assertEq(3, model.shownMilestones.size)
        assertEq("1 of 2 closed · 50%", model.progress(model.project(1)!!).label)
        assertEq(listOf(3, 4), model.linkedIssues(model.project(3)!!).map { it.number })
        assertEq("1 of 2 closed · 50%", model.progress(model.milestones.first { it.title == "v1" }).label)
        val span = model.timeline!!
        assertEq(PlanningSpan(1000, 4000), span)
        val bar = PlanningRecords.bar(model.project(1)!!, span)!!
        expect(kotlin.math.abs(bar.offset - 1.0 / 3) < 1e-9)
        expect(kotlin.math.abs(bar.width - 2.0 / 3) < 1e-9)
        assertEq(null, PlanningRecords.bar(RepoProject(9, "x"), span))
        assertEq(PlanningBar(0.0, 0.01), PlanningRecords.bar(RepoProject(9, "x", startMs = 1000, endMs = 1001), PlanningSpan(1000, 1_000_000)))
        assertEq("No issues", PlanningProgress(0, 0).label)
        assertEq(null, PlanningRepo.of("acme"))
        assertEq(null, PlanningRepo.of("a/b/c"))
    }

    fun testTaskOperations() {
        val api = FakePlanningApi()
        api.answers["POST "] = mapOf("task" to mapOf("id" to "t1"))
        api.answers["GET /t1/history"] = mapOf("history" to listOf(mapOf("id" to "h", "kind" to "created", "createdAt" to 5L)))
        val ops = TaskOperations { method, path, body -> api.accountApiCall(method, path, body) }
        assertEq("t1", ops.quickAdd(" Ship it ")["id"])
        assertEq("unassigned", api.calls.last().third?.get("assigneeKind"))
        assertEq("Give the task a title.", assertFails<TaskOperationException> { ops.quickAdd("  ") }.message)
        val big = TaskAttachmentDraft("a.png", "image/png", ByteArray(TaskAttachmentDraft.MAX_BYTES + 1))
        assertEq("Each image must be under 256 KB.", assertFails<TaskOperationException> { ops.quickAdd("x", listOf(big)) }.message)
        assertEq("Attach PNG, JPEG, GIF or WebP images.", TaskAttachmentDraft.validate(listOf(TaskAttachmentDraft("a", "image/bmp", ByteArray(1)))))
        ops.quickAdd("pic", listOf(TaskAttachmentDraft("a.png", "image/png", byteArrayOf(1, 2, 3))))
        assertEq("AQID", (api.calls.last().third?.get("attachments") as List<*>).first().let { (it as Map<*, *>)["file"] })
        ops.loadHistory("t1")
        assertEq(listOf("created"), ops.history.map { it.kind })
        assertEq("Choose a repository as owner/name.", assertFails<TaskOperationException> { ops.assignToAgent("t1", "nope") }.message)
        ops.assignToAgent("t1", " acme/app ")
        assertEq(mapOf("assigneeKind" to "agent", "repository" to "acme/app"), api.calls.last().third)
        assertFails<TaskOperationException> { ops.setPriority("t1", 100) }
        ops.setPriority("t1", 5)
        assertEq(5L, api.calls.last().third?.get("priority"))
        assertFails<TaskOperationException> { ops.linkAgentSession("t1", " ") }
        val filed = mutableListOf<String>()
        ops.moveToIssue(mapOf("id" to "t9", "title" to "Bug", "details" to "It breaks", "completionNote" to "Seen twice"), "acme/app") { o, n, t, b ->
            filed.add("$o/$n|$t|$b")
        }
        assertEq("acme/app|Bug|It breaks\n\nSeen twice\n\n---\nMoved from organization task t9.", filed.single())
        assertEq("DELETE", api.calls.last().first)
        api.failures["DELETE /bad"] = OrgApiException("x", "y")
        assertEq(1 to 1, ops.deleteClosed(listOf(mapOf("id" to "ok", "status" to "done"), mapOf("id" to "bad", "status" to "done"), mapOf("id" to "o", "status" to "idle"))))
        assertEq("Agent: Waiting for you", TaskAgentInfo.of(mapOf("agent" to mapOf("status" to "waiting")))?.label)
        assertEq("Claude: Linked", TaskAgentInfo.of(mapOf("agentSessionId" to "s", "agent" to mapOf("provider" to "claude")))?.label)
        assertEq(null, TaskAgentInfo.of(emptyMap()))
        assertEq("[task:t1] Fix\n\nDo it", PlanningConversion.agentPrompt(mapOf("id" to "t1", "title" to "Fix", "details" to " Do it ")))
        assertEq(TaskListFilter.DONE, TaskListFilter.of("Done"))
        assertEq(TaskListFilter.MINE, TaskListFilter.of(null))
    }

    fun testNoteConversions() {
        assertEq("Heading", PlanningConversion.title("", "\n## Heading\nbody"))
        assertEq("Untitled note", PlanningConversion.title(" ", "###"))
        val details = PlanningConversion.details("Plan", "x".repeat(5_000), "https://n/1")
        assertEq(PlanningConversion.TASK_DETAILS_MAX, details.length)
        expect(details.endsWith("Converted from the note \"Plan\" (published at https://n/1)"))
        val api = FakePlanningApi()
        api.answers["POST "] = mapOf("task" to mapOf("id" to "n1"))
        assertEq("n1", NoteConversions.toTask("Plan", "body", null) { m, p, b -> api.accountApiCall(m, p, b) }["id"])
        assertEq("Choose a repository as owner/name.", NoteConversions.toIssue("P", "b", null, "bad") { _, _, _, _ -> })
        assertEq(null, NoteConversions.toIssue("P", "b", null, "a/b") { _, _, _, _ -> })
        assertEq("The issue was not filed.", NoteConversions.toIssue("P", "b", null, "a/b") { _, _, _, _ -> error("x") })
        assertEq("Sign in again to continue.", PlanningErrors.text(OrgApiException("unauthorized", ""), "f"))
        assertEq("Too many.", PlanningErrors.text(OrgApiException("rate", "too many"), "f"))
        assertEq("f", PlanningErrors.text(IllegalStateException(), "f"))
    }
}
