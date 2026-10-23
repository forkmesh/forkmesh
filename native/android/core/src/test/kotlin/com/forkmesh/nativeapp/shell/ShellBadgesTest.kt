package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.tasks.TaskBoard
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellBadgesTest {
    private fun pull(number: Int, status: String = "open") = PublishedPull(number, "PR $number", status)

    fun testFirstSightRecordsABaselineInsteadOfFlaggingEveryOpenPull() {
        val marker = NewPullsMarker(MemoryPrefStore())
        assertEq(0, marker.newCount("Acme/Repo", listOf(pull(1), pull(2), pull(3, "merged"))))
        assertEq(3, marker.seen("acme/repo"))
    }

    fun testOnlyOpenPullsAboveTheMarkCountAsNew() {
        val marker = NewPullsMarker(MemoryPrefStore())
        marker.newCount("acme/repo", listOf(pull(1)))
        val later = listOf(pull(1), pull(2), pull(3, "closed"), pull(4), pull(5, "merged"))
        assertEq(2, marker.newCount("acme/repo", later))
    }

    fun testAnEmptyRepositoryCountsItsFirstPull() {
        val marker = NewPullsMarker(MemoryPrefStore())
        assertEq(0, marker.newCount("acme/empty", emptyList()))
        assertEq(1, marker.newCount("acme/empty", listOf(pull(1))))
    }

    fun testOpeningThePullRequestsPageClearsTheBadge() {
        val marker = NewPullsMarker(MemoryPrefStore())
        marker.newCount("acme/repo", listOf(pull(1)))
        val pulls = listOf(pull(1), pull(2), pull(3))
        assertEq(2, marker.newCount("acme/repo", pulls))
        marker.markSeen("ACME/repo", pulls)
        assertEq(0, marker.newCount("acme/repo", pulls))
    }

    fun testMarkingSeenNeverMovesTheMarkBackwards() {
        val marker = NewPullsMarker(MemoryPrefStore())
        marker.markSeen("acme/repo", listOf(pull(9)))
        marker.markSeen("acme/repo", listOf(pull(2)))
        assertEq(9, marker.seen("acme/repo"))
    }

    fun testMarksPersistPerRepositoryAndStayBounded() {
        val store = MemoryPrefStore()
        val marker = NewPullsMarker(store)
        for (index in 0 until NewPullsMarker.LIMIT + 5) marker.markSeen("acme/repo$index", listOf(pull(index + 1)))
        val reread = NewPullsMarker(store)
        assertEq(NewPullsMarker.LIMIT + 5, reread.seen("acme/repo${NewPullsMarker.LIMIT + 4}"))
        assertEq(null, reread.seen("acme/repo0"))
        assertEq(NewPullsMarker.LIMIT, store.getString(NewPullsMarker.KEY)!!.split('\n').size)
        assertEq("shell/pulls-seen", NewPullsMarker.KEY)
    }

    fun testAMarkerWithoutAStoreStillRemembersWithinTheSession() {
        val marker = NewPullsMarker(null)
        marker.markSeen("acme/repo", listOf(pull(4)))
        assertEq(4, marker.seen("acme/repo"))
    }

    fun testPollingSlowsWhenThePhoneIsWarmAndStopsWhenCritical() {
        assertEq(NewPullsMarker.CALM_POLL_MS, NewPullsMarker.pollEvery(DeviceHeat.Calm, false))
        assertEq(NewPullsMarker.WARM_POLL_MS, NewPullsMarker.pollEvery(DeviceHeat.Calm, true))
        assertEq(NewPullsMarker.WARM_POLL_MS, NewPullsMarker.pollEvery(DeviceHeat.Warm, false))
        assertEq(null, NewPullsMarker.pollEvery(DeviceHeat.Critical, false))
        assertEq(20_000L, NewPullsMarker.CALM_POLL_MS)
        assertEq(300_000L, NewPullsMarker.WARM_POLL_MS)
    }

    fun testAPullIsOpenUnlessClosedOrMerged() {
        assertEq(2, NewPullsMarker.openCount(listOf(pull(1), pull(2, "draft"), pull(3, "merged"), pull(4, "closed"))))
    }

    fun testTheShellListIsOnlyShownForTheCurrentRepository() {
        val repo = RepoRef("Acme", "Repo")
        val empty = ShellPulls()
        expect(!empty.holds(repo))
        val first = empty.adopted(repo, listOf(pull(1), pull(2)), fresh = 1)
        assertEq(1, first.revision)
        assertEq(1, first.newBadge(RepoRef("acme", "repo")))
        assertEq(2, first.openCount(repo))
        assertEq(0, first.newBadge(RepoRef("acme", "other")))
        assertEq(null, first.openCount(null))
        val same = first.adopted(repo, listOf(pull(1), pull(2)), fresh = 0)
        assertEq(1, same.revision)
        assertEq(0, same.newBadge(repo))
        val changed = same.adopted(repo, listOf(pull(1), pull(2), pull(3)), fresh = 1)
        assertEq(2, changed.revision)
    }

    fun testBugReportsShowTheNewCountOnly() {
        val none = ShellBadgeCounts()
        assertEq(null, none.bugReports)
        val counts = none.adoptBugReports(mapOf("new" to 7, "triaged" to 2, "closed" to 4))
        assertEq(7, counts.bugReports)
        assertEq(counts, counts.adoptBugReports(mapOf("triaged" to 1)))
        assertEq(0, counts.adoptBugReports(mapOf("new" to 0)).bugReports)
    }

    fun testAssignedTasksCountOpenTasksForTheSignedInUser() {
        val tasks = listOf(
            mapOf("id" to "1", "status" to "open", "assigneeKind" to "user", "assignee" to "Kai"),
            mapOf("id" to "2", "status" to "done", "assigneeKind" to "user", "assignee" to "kai"),
            mapOf("id" to "3", "status" to "active", "assigneeKind" to "user", "assignee" to "kai"),
            mapOf("id" to "4", "status" to "open", "assigneeKind" to "user", "assignee" to "lee"),
            mapOf("id" to "5", "status" to "open", "assigneeKind" to "agent", "assignee" to "bot"),
        )
        val board = TaskBoard(tasks, actor = "kai", canManage = false, members = emptyList(), departments = emptyList(), nextOffset = null)
        assertEq(2, ShellBadgeCounts().adoptTasks(board).assignedTasks)
    }

    fun testRepoCountsBelongToOneRepositoryAtATime() {
        val repo = RepoRef("acme", "repo")
        val first = ShellRepoCounts().adopt(repo, mapOf(RepoHubSection.Pulls to 3, RepoHubSection.Issues to 5))
        assertEq(3, first.of(RepoRef("Acme", "Repo"), RepoHubSection.Pulls))
        assertEq(null, first.of(RepoRef("acme", "other"), RepoHubSection.Pulls))
        assertEq(null, first.of(null, RepoHubSection.Pulls))
        val merged = first.adopt(repo, mapOf(RepoHubSection.Pulls to 4, RepoHubSection.Actions to 1))
        assertEq(4, merged.of(repo, RepoHubSection.Pulls))
        assertEq(5, merged.of(repo, RepoHubSection.Issues))
        assertEq(1, merged.of(repo, RepoHubSection.Actions))
        val other = merged.adopt(RepoRef("acme", "other"), mapOf(RepoHubSection.Pulls to 9))
        assertEq(null, other.of(repo, RepoHubSection.Issues))
        expect(other.knows(RepoRef("ACME", "other")))
    }

    fun testSavedRepoCountsComeBackForTheirRepositoryOnly() {
        val repo = RepoRef("Acme", "Repo")
        val saved = ShellRepoCounts.encode(mapOf(RepoHubSection.Pulls to 3, RepoHubSection.Issues to 12))
        assertEq("Issues:12,Pulls:3", saved)
        assertEq("shell/repoCounts/acme/repo", ShellRepoCounts.key(repo))
        val restored = ShellRepoCounts().restored(repo, saved)
        assertEq(12, restored.of(RepoRef("acme", "repo"), RepoHubSection.Issues))
        assertEq(mapOf(RepoHubSection.Pulls to 3), ShellRepoCounts.decode("Pulls:3,Bogus:1,Issues:x,"))
        val live = ShellRepoCounts().adopt(repo, mapOf(RepoHubSection.Pulls to 7))
        assertEq(7, live.restored(repo, saved).of(repo, RepoHubSection.Pulls))
        assertEq(ShellRepoCounts(), ShellRepoCounts().restored(repo, ""))
    }
}
