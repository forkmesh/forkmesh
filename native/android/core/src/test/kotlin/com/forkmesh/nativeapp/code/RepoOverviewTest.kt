package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RepoOverviewTest {
    private class FakeClient : RepoOverviewClient {
        val calls = ArrayList<String>()
        val failing = HashSet<String>()

        private fun <T> answer(kind: String, owner: String, name: String, value: List<T>): List<T> {
            calls += "$kind $owner/$name"
            if (kind in failing) throw IllegalStateException("$kind failed")
            return value
        }

        override fun issues(owner: String, name: String): List<Issue> = answer(
            "issues", owner, name,
            listOf(Issue(1, "a"), Issue(2, "b", status = "closed"), Issue(3, "c", status = "in_progress")),
        )

        override fun pulls(owner: String, name: String): List<PublishedPull> = answer(
            "pulls", owner, name,
            listOf(PublishedPull(1, "a"), PublishedPull(2, "b", status = "merged"), PublishedPull(3, "c", status = "closed")),
        )

        override fun branches(owner: String, name: String): List<RepoBranch> =
            answer("branches", owner, name, listOf(RepoBranch("main", isDefault = true), RepoBranch("dev"), RepoBranch("fix")))

        override fun releases(owner: String, name: String): List<RepoRelease> =
            answer("releases", owner, name, listOf(RepoRelease("v1"), RepoRelease("v2")))

        override fun commits(owner: String, name: String): List<Map<String, Any?>> =
            answer("commits", owner, name, (1..42).map { mapOf("hash" to "h$it") })
    }

    fun testLoadCountsOpenWorkAndLists() {
        val client = FakeClient()
        val model = RepoOverviewModel(Repository("alice", "tool", mirrors = 3), client)
        assertEq(mapOf(RepoHubSection.Mirrors to 3), model.counts)
        val counts = model.load()
        assertEq(2, counts[RepoHubSection.Issues])
        assertEq(1, counts[RepoHubSection.Pulls])
        assertEq(42, counts[RepoHubSection.Commits])
        assertEq(3, counts[RepoHubSection.Branches])
        assertEq(2, counts[RepoHubSection.Releases])
        assertEq(3, counts[RepoHubSection.Mirrors])
        expect(client.calls.all { it.endsWith(" alice/tool") })
        assertEq(5, client.calls.size)
    }

    fun testFailedReadLeavesTileWithoutCount() {
        val client = FakeClient()
        client.failing += setOf("pulls", "releases")
        val model = RepoOverviewModel(Repository("alice", "tool", mirrors = 0), client)
        model.load()
        val tiles = model.tiles().associateBy { it.section }
        assertEq(null, tiles.getValue(RepoHubSection.Pulls).count)
        assertEq(null, tiles.getValue(RepoHubSection.Releases).count)
        assertEq(null, tiles.getValue(RepoHubSection.Mirrors).count)
        assertEq(2, tiles.getValue(RepoHubSection.Issues).count)
        assertEq(null, tiles.getValue(RepoHubSection.Code).count)
        assertEq("Issues, 2", tiles.getValue(RepoHubSection.Issues).accessibilityLabel)
        assertEq("Code", tiles.getValue(RepoHubSection.Code).accessibilityLabel)
    }

    fun testNoClientLoadsNothing() {
        val model = RepoOverviewModel(Repository("alice", "tool"), null)
        assertEq(mapOf(RepoHubSection.Mirrors to 1), model.load())
        assertEq(null, model.count(RepoHubSection.Issues))
    }

    fun testTilesCoverEveryTabAndTool() {
        val tiles = RepoOverviewModel(Repository("a", "b"), null).tiles()
        assertEq(RepoHubSection.entries.toList(), tiles.map { it.section })
        for (tab in RepoDetailTab.entries) {
            expect(tiles.any { it.section.tab == tab }, "no tile for $tab")
            assertEq(tab, RepoHubSection.of(tab).tab)
        }
        val titles = tiles.map { it.title }
        for (tool in listOf("Security", "Webhooks", "Issue board", "Backlog", "Merge queue", "Code map", "Insights", "Settings", "Branches", "Download ZIP", "Projects", "Milestones")) {
            expect(tool in titles, "missing $tool")
        }
        expect("Shortcuts" !in titles, "Shortcuts lives in the navigator")
        assertEq(titles.size, titles.toSet().size)
        assertEq(26, tiles.size)
        for (tool in listOf("Ember", "Strata", "Synapse")) expect(tool in titles, "missing $tool")
        val gated = com.forkmesh.nativeapp.market.PackageSurfaces.of { it != "strata" }.repoSections
        expect(RepoHubSection.Strata in gated && RepoHubSection.Ember !in gated && RepoHubSection.Synapse !in gated, "Strata hides when its package is removed")
    }

    fun testCountText() {
        assertEq("999+", RepoOverviewTile(RepoHubSection.Commits, 1200).countText)
        assertEq("0", RepoOverviewTile(RepoHubSection.Issues, 0).countText)
        assertEq(null, RepoOverviewTile(RepoHubSection.Code, null).countText)
    }

    fun testAdoptTakesTheCatalogRecord() {
        val model = RepoOverviewModel(Repository("Alice", "Tool"), null)
        expect(!model.adopt(listOf(Repository("bob", "tool"))))
        expect(model.adopt(listOf(Repository("alice", "tool", description = "d", mirrors = 4, defaultBranch = "trunk"))))
        assertEq("trunk", model.repo.defaultBranch)
        assertEq(4, model.counts[RepoHubSection.Mirrors])
        model.adopt(listOf(Repository("alice", "tool", mirrors = 0)))
        assertEq(null, model.counts[RepoHubSection.Mirrors])
    }

    fun testCurrentRepoMemoryPersists() {
        val prefs = MemoryPrefStore()
        val memory = CurrentRepoMemory(prefs)
        assertEq(null, memory.ref)
        expect(!memory.remember("", "x"))
        expect(memory.remember("alice", "tool"))
        expect(!memory.remember("alice", "tool"))
        assertEq("alice/tool", prefs.getString(CurrentRepoMemory.KEY))
        assertEq(RepoRef("alice", "tool"), CurrentRepoMemory(prefs).ref)
        assertEq(null, CurrentRepoMemory(null).ref)
    }
}
