package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalAgentRepository
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.market.FakeChannel
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DesktopReposTest {
    private val reply = mapOf(
        "desktop" to "studio",
        "repos" to listOf(
            mapOf("repo" to "zed/zeta", "branch" to "main", "checkout" to true, "private" to true, "published" to true),
            mapOf("owner" to "alice", "name" to "tool", "description" to "Tools", "branch" to "dev", "checkout" to true,
                "published" to false, "agents" to mapOf("running" to 2, "total" to 3)),
            mapOf("owner" to "", "name" to "", "repo" to "bob/beta", "checkout" to false),
            mapOf("owner" to "ALICE", "name" to "Tool"),
            mapOf("owner" to "nobody"),
            mapOf("repo" to "broken"),
            mapOf("owner" to "carol", "name" to "gamma", "agents" to mapOf("running" to 1)),
        ),
    )

    fun testParseSortAndDedupe() {
        val channel = FakeChannel()
        channel.replies["repos.list"] = reply
        val model = DesktopReposModel(channel, "old name")
        expect(model.available)
        expect(model.load())
        assertEq(listOf("alice/tool", "carol/gamma", "bob/beta", "zed/zeta"), model.repos.map { it.slug })
        assertEq("studio", model.desktopName)
        assertEq("Desktop · studio", model.title)
        assertEq(null, model.problem)
        expect(model.loaded && !model.loading && model.visible)
        val alice = model.repos.first()
        assertEq(2, alice.runningAgents)
        assertEq(3, alice.totalAgents)
        assertEq("dev · 2 running · Public · Not published", alice.detail)
        assertEq("No checkout · Public · Not published", model.repos[2].detail)
        assertEq("main · Private", model.repos[3].detail)
        assertEq(listOf("repos.list"), channel.requests.map { it.first })
    }

    fun testRowsBecomeRepositories() {
        val row = DesktopRepo("alice", "tool", description = "d", isPrivate = true)
        assertEq(Repository("alice", "tool", description = "d", mirrors = 0, isPrivate = true, liveHost = true), row.repository)
        expect(row.matches(""))
        expect(row.matches("alice/t"))
        expect(!row.matches("zzz"))
        expect(DesktopRepo("a", "b", description = "Build Tools").matches("tools"))
    }

    fun testSearchFiltersRows() {
        val channel = FakeChannel()
        channel.replies["repos.list"] = reply
        val model = DesktopReposModel(channel)
        model.load()
        assertEq(listOf("alice/tool"), model.rows("tool").map { it.slug })
        assertEq(4, model.rows("").size)
    }

    fun testFallbackToAgentSnapshotOnOlderDesktops() {
        val channel = FakeChannel()
        channel.failures["repos.list"] = LocalControlException("unknown_method")
        val model = DesktopReposModel(channel, "studio") { listOf(LocalAgentRepository("bob", "beta"), LocalAgentRepository("Bob", "Beta"), LocalAgentRepository("al", "a")) }
        expect(!model.load())
        assertEq(listOf("al/a", "bob/beta"), model.repos.map { it.slug })
        assertEq(null, model.problem)
        assertEq("Desktop · studio", model.title)
    }

    fun testProblemsWithoutFallback() {
        val cases = mapOf(
            "not_paired" to "Pair this phone with your desktop to see its repositories.",
            "not_owner" to "The desktop is signed in to another account.",
            "unsupported" to "Update the desktop to list its repositories here.",
            "unknown_method" to "Update the desktop to list its repositories here.",
            "timeout" to "The desktop is offline. Its repositories show here when it is back.",
        )
        for ((code, sentence) in cases) {
            val channel = FakeChannel()
            channel.failures["repos.list"] = LocalControlException(code)
            val model = DesktopReposModel(channel)
            model.load()
            assertEq(sentence, model.problem, code)
            expect(model.repos.isEmpty())
            expect(model.visible)
        }
        assertEq("The desktop is offline. Its repositories show here when it is back.", DesktopReposModel.explain(IllegalStateException("x")))
    }

    fun testProblemClearsOnSuccess() {
        val channel = FakeChannel()
        channel.failures["repos.list"] = LocalControlException("not_connected")
        val model = DesktopReposModel(channel)
        model.load()
        expect(model.problem != null)
        channel.failures.clear()
        channel.replies["repos.list"] = reply
        model.load()
        assertEq(null, model.problem)
        assertEq(4, model.repos.size)
    }

    fun testNoChannelIsHidden() {
        val model = DesktopReposModel(null)
        expect(!model.available)
        expect(!model.load())
        expect(!model.visible)
        assertEq("Desktop", model.title)
    }

    fun testChangedEventTriggersReload() {
        val channel = FakeChannel()
        val model = DesktopReposModel(channel)
        var changed = 0
        model.start { changed++ }
        model.start { changed += 100 }
        assertEq(1, channel.taps.size)
        channel.taps.single()(ControlEvent("repos.changed", emptyMap()))
        channel.taps.single()(ControlEvent("repos.other", emptyMap()))
        channel.taps.single()(ControlEvent("agents.changed", emptyMap()))
        assertEq(1, changed)
        model.stop()
        assertEq(0, channel.taps.size)
    }

    fun testTotalCountsDesktopOnlyRows() {
        val catalog = listOf(Repository("alice", "tool"), Repository("bob", "beta"))
        val desktop = listOf(DesktopRepo("Alice", "Tool"), DesktopRepo("carol", "gamma"))
        assertEq(3, DesktopReposModel.total(catalog, desktop))
        assertEq(2, DesktopReposModel.total(emptyList(), desktop))
    }
}
