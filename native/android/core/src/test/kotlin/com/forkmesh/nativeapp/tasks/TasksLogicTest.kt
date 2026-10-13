package com.forkmesh.nativeapp.tasks

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class TasksLogicTest {
    private val board = TaskBoard.fromJson(
        mapOf(
            "actor" to "Alice",
            "canManage" to true,
            "members" to listOf("bob", ""),
            "departments" to listOf("engineering"),
            "hasMore" to true,
            "nextOffset" to 50L,
            "tasks" to listOf(
                mapOf("id" to "1", "status" to "idle", "assigneeKind" to "user", "assignee" to "alice"),
                mapOf("id" to "2", "status" to "active", "assigneeKind" to "user", "assignee" to "bob"),
                mapOf("id" to "3", "status" to "done", "assigneeKind" to "user", "assignee" to "alice"),
            ),
        ),
    )

    fun testReminderStatusSaysWhetherTheAssigneeSawIt() {
        val now = 1_790_000_000_000L
        assertEq(
            "Not seen yet: @bob was pinged and pushed to 2 phones",
            TasksLogic.reminderStatus(mapOf("to" to "bob", "delivered" to true, "phones" to 2L), now),
        )
        assertEq("Not seen yet: the assignee was pinged", TasksLogic.reminderStatus(mapOf("delivered" to true), now))
        assertEq("Not delivered: the ping could not be filed", TasksLogic.reminderStatus(mapOf("delivered" to false), now))
        expect(
            TasksLogic.reminderStatus(mapOf("to" to "bob", "seenAt" to now - 60_000, "seenVia" to "ping"), now)
                .startsWith("Seen: @bob opened the ping "),
        )
    }

    fun testBoardSplitsAndPages() {
        assertEq("alice", board.actor)
        assertEq(listOf("1"), board.mineOpen.map { it["id"] })
        assertEq(listOf("1", "2"), board.open.map { it["id"] })
        assertEq(listOf("3"), board.done.map { it["id"] })
        assertEq(50L, board.nextOffset)
        assertEq(listOf("bob"), board.members)
        val merged = board.merged(TaskBoard.fromJson(mapOf("tasks" to listOf(mapOf("id" to "3"), mapOf("id" to "4")))))
        assertEq(listOf("1", "2", "3", "4"), merged.tasks.map { it["id"] })
        assertEq(null, merged.nextOffset)
    }

    private val team = TaskBoard.fromJson(
        mapOf(
            "actor" to "alice",
            "members" to listOf("wendy", "Carol", "alice", "bob", "wendy", " "),
            "tasks" to listOf(
                mapOf("id" to "a1", "status" to "idle", "assigneeKind" to "user", "assignee" to "alice"),
                mapOf("id" to "a2", "status" to "done", "assigneeKind" to "user", "assignee" to "alice"),
                mapOf("id" to "w1", "status" to "active", "assigneeKind" to "user", "assignee" to "wendy"),
                mapOf("id" to "w2", "status" to "idle", "assigneeKind" to "user", "assignee" to "Wendy"),
                mapOf("id" to "w3", "status" to "done", "assigneeKind" to "user", "assignee" to "wendy"),
                mapOf("id" to "b1", "status" to "idle", "assigneeKind" to "claude"),
                mapOf("id" to "b2", "status" to "idle", "assigneeKind" to "agent", "assignee" to "agent"),
                mapOf("id" to "u1", "status" to "idle", "assigneeKind" to "unassigned"),
                mapOf("id" to "u2", "status" to "done", "assigneeKind" to "user"),
            ),
        ),
    )

    fun testPeopleListTheActorFirstThenMembersAToZ() {
        assertEq(
            listOf("all", "user:alice", "user:bob", "user:carol", "user:wendy", "agent", "unassigned"),
            team.people.map { it.key },
        )
        assertEq(listOf("Everyone", "@alice (you)", "@bob", "@carol", "@wendy", "Bot", "Unassigned"), team.people.map { it.label })
        assertEq("wendy", team.people.first { it.key == "user:wendy" }.name)
        val solo = TaskBoard.fromJson(mapOf("actor" to "zed", "members" to listOf("amy")))
        assertEq(listOf("all", "user:zed", "user:amy", "agent", "unassigned"), solo.people.map { it.key })
    }

    fun testPersonOpenCountsSkipDoneTasks() {
        val counts = team.personOpenCounts()
        assertEq(6, counts["all"])
        assertEq(1, counts["user:alice"])
        assertEq(2, counts["user:wendy"])
        assertEq(2, counts["agent"])
        assertEq(1, counts["unassigned"])
        assertEq(null, counts["user:bob"])
    }

    fun testTabCountsFollowTheSelectedPersonButMineDoesNot() {
        assertEq(listOf(1, 6, 3), team.tabCounts(TaskPerson.EVERYONE))
        assertEq(listOf(1, 2, 1), team.tabCounts("user:wendy"))
        assertEq(listOf(1, 0, 0), team.tabCounts("user:bob"))
        assertEq(listOf(1, 2, 0), team.tabCounts("agent"))
        assertEq(listOf(1, 1, 1), team.tabCounts("unassigned"))
        assertEq(listOf("w1", "w2"), team.lists("user:wendy")[1].map { it["id"] })
        assertEq(listOf("a1"), team.lists("user:wendy")[0].map { it["id"] })
    }

    fun testFilterSelectionRules() {
        val start = TaskFilter()
        assertEq(TaskFilter(0, "all"), start)
        val wendy = start.selectPerson("user:wendy")
        assertEq(TaskFilter(1, "user:wendy"), wendy)
        assertEq(TaskFilter(1, "all"), wendy.selectPerson("user:wendy"))
        assertEq(TaskFilter(2, "user:wendy"), wendy.selectTab(2))
        assertEq(TaskFilter(2, "agent"), wendy.selectTab(2).selectPerson("agent"))
        assertEq(TaskFilter(0, "all"), wendy.selectTab(0))
        assertEq(TaskFilter(0, "all"), start.selectPerson("all"))
        assertEq(TaskFilter(1, "all"), TaskFilter(1, "user:gone").within(team))
        assertEq(wendy, wendy.within(team))
    }

    fun testLabels() {
        assertEq("under a minute", TasksLogic.elapsed(59_000))
        assertEq("45m", TasksLogic.elapsed(45 * 60_000L))
        assertEq("2h", TasksLogic.elapsed(120 * 60_000L))
        assertEq("2h 5m", TasksLogic.elapsed(125 * 60_000L))
        assertEq("Customer success", TasksLogic.departmentLabel("customer-success"))
        assertEq("Agent", TasksLogic.assigneeLabel(mapOf("assigneeKind" to "agent")))
        assertEq("@bob", TasksLogic.assigneeLabel(mapOf("assigneeKind" to "user", "assignee" to "bob")))
        assertEq("Unassigned", TasksLogic.assigneeLabel(mapOf("assigneeKind" to "user")))
        assertEq("Needs help", TasksLogic.checkinLabel("needs_help"))
        assertEq("Waiting on review", TasksLogic.checkinLabel("waiting_on_review"))
        assertEq("Idle", TasksLogic.statusLabel(emptyMap()))
        assertEq("No open tasks", TasksLogic.emptyTitle(1))
        assertEq("No open tasks for @wendy", TasksLogic.emptyTitle(1, TaskPerson("user:wendy", "@wendy", "wendy")))
        assertEq("No finished tasks for the bot yet", TasksLogic.emptyTitle(2, TaskPerson("agent", "Bot")))
        val row = TasksLogic.rowSubtitle(mapOf("assigneeKind" to "unassigned", "replyCount" to 1L))
        if (!row.startsWith("Unassigned · General · updated ") || !row.endsWith(" · 1 reply")) throw AssertionError(row)
    }

    fun testActionsFollowTheRoles() {
        val mine = mapOf("status" to "idle", "assigneeKind" to "user", "assignee" to "alice")
        assertEq(TasksLogic.Actions(start = true, stop = false, checkin = false, finish = true, reopen = false), TasksLogic.actions(mine, "alice", false))
        val running = mine + ("status" to "active")
        assertEq(TasksLogic.Actions(start = false, stop = true, checkin = true, finish = true, reopen = false), TasksLogic.actions(running, "alice", false))
        val done = mapOf("status" to "done", "assigneeKind" to "user", "assignee" to "bob", "qa" to mapOf("reviewer" to "carol"))
        assertEq(true, TasksLogic.actions(done, "carol", false).reopen)
        assertEq(false, TasksLogic.actions(done, "dan", false).reopen)
        assertEq(false, TasksLogic.actions(mine, "dan", false).finish)
        assertEq(true, TasksLogic.actions(mine, "dan", true).finish)
        assertEq(true, TasksLogic.actions(mine, "dan", false).remind)
        assertEq(true, TasksLogic.actions(running, "dan", false).remind)
        assertEq(false, TasksLogic.actions(done, "dan", false).remind)
        assertEq(false, TasksLogic.actions(mapOf("status" to "idle", "assigneeKind" to "agent", "assignee" to "agent"), "dan", false).remind)
        assertEq(false, TasksLogic.actions(mapOf("status" to "idle", "assigneeKind" to "user"), "dan", false).remind)
        assertEq(listOf("alice", "bob"), TasksLogic.assignees("alice", true, listOf("bob", "alice")))
        assertEq(listOf("alice"), TasksLogic.assignees("alice", false, listOf("bob")))
    }

    fun testErrorsAndRequests() {
        assertEq("Only the assignee can do that.", TasksLogic.errorText(OrgApiException("assignee_only", "x"), "f"))
        assertEq("server says", TasksLogic.errorText(OrgApiException("other", "server says"), "f"))
        assertEq("f", TasksLogic.errorText(IllegalStateException(), "f"))
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return mapOf("task" to mapOf("id" to "t"))
            }
        }
        val tasks = TasksApi(api)
        tasks.board(40)
        tasks.create(" Fix ", "", "general", "")
        tasks.post("t", "/complete", mapOf("completionNote" to "ok"))
        assertEq("GET /api/tasks?offset=40 ", calls[0])
        assertEq("POST /api/tasks {title=Fix, details=, department=general, assigneeKind=unassigned}", calls[1])
        assertEq("POST /api/tasks/t/complete {completionNote=ok}", calls[2])
    }
}
