package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DesktopUpdateTest {
    private val idleStatus: Map<String, Any?> = mapOf(
        "run" to 1L, "running" to false, "version" to "v1.0.0", "startedAtMs" to 1000L,
        "lines" to emptyList<String>(), "seq" to 0L,
    )

    fun testStatusLoadsVersionAndBacklog() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus + mapOf(
            "running" to true, "kind" to "update", "percent" to 60L, "phase" to "compiling",
            "lines" to listOf("\$ cmake --build", "[1/9] Building"),
        )
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline)
        store.refresh()
        assertEq("v1.0.0", store.version)
        assertEq(listOf("\$ cmake --build", "[1/9] Building"), store.lines)
        assertEq(DesktopUpdateStage.Running, store.stage, "an update started on the desktop shows too")
        assertEq("60% · compiling", store.state.headline)
        expect(store.busy, "busy while running")
    }

    fun testRunStreamsProgressLines() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus
        channel.replies["update.prepare"] = mapOf(
            "token" to "tok", "action" to "updateRestart", "subject" to "desktop", "summary" to "Pull and restart",
        )
        channel.replies["update.run"] = mapOf("started" to true, "action" to "updateRestart", "message" to "Updating.")
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline)
        store.refresh()
        val confirmation = store.prepare()
        assertEq("updateRestart", channel.last("update.prepare").str("action"))
        assertEq("Pull and restart", confirmation.summary)
        assertEq("Updating.", store.run(confirmation))
        assertEq("tok", channel.last("update.run").str("token"))
        assertEq(DesktopUpdateStage.Running, store.stage)

        channel.emit(
            "update.progress",
            mapOf(
                "run" to 2L, "running" to true, "kind" to "update", "percent" to 20L, "phase" to "pulling the source",
                "activity" to "20% pulling the source", "lines" to listOf("==> Pulling", "Already up to date."),
                "skipped" to 0L,
            ),
        )
        assertEq(listOf("==> Pulling", "Already up to date."), store.lines)
        assertEq(20, store.state.percent)
        assertEq("20% pulling the source", store.state.headline)
        channel.emit(
            "update.progress",
            mapOf("run" to 2L, "running" to true, "percent" to 60L, "lines" to listOf("[40/41] Linking"), "skipped" to 12L),
        )
        assertEq(4, store.lines.size)
        assertEq("… 12 earlier lines not shown", store.lines[2])
        assertEq("[40/41] Linking", store.lines.last())
        channel.emit("fleet.progress", mapOf("lines" to listOf("other family")))
        assertEq(4, store.lines.size, "other families' events are ignored")
    }

    fun testRestartComputerUsesItsOwnConfirmedAction() {
        val channel = FakeRemoteChannel()
        channel.replies["update.prepare"] = mapOf(
            "token" to "reboot", "action" to "computerRestart", "subject" to "computer",
            "summary" to "Restart this entire computer.",
        )
        channel.replies["update.run"] = mapOf("started" to true, "message" to "The computer is restarting.")
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline)
        val confirmation = store.prepareComputerRestart()
        assertEq("computerRestart", channel.last("update.prepare").str("action"))
        assertEq("The computer is restarting.", store.restartComputer(confirmation))
        assertEq("computerRestart", channel.last("update.run").str("action"))
        assertEq(DesktopUpdateStage.Restarting, store.stage)
        assertEq(listOf("==> Restarting computer", "\$ loginctl reboot"), store.lines)
        assertEq("computerRestart", store.state.kind)
        channel.emit(
            "update.progress",
            mapOf(
                "run" to 2L, "running" to true, "kind" to "computerRestart",
                "lines" to listOf("==> Restarting computer", "\$ loginctl reboot"),
            ),
        )
        assertEq(DesktopUpdateStage.Restarting, store.stage)
    }

    fun testFailureStopsTracking() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline)
        store.refresh()
        channel.emit("update.progress", mapOf("run" to 2L, "running" to true, "percent" to 35L, "lines" to listOf("\$ cmake")))
        assertEq(DesktopUpdateStage.Running, store.stage)
        channel.emit(
            "update.progress",
            mapOf("run" to 2L, "running" to false, "error" to "Update failed: cmake exited 1", "lines" to listOf("!! Update failed")),
        )
        assertEq(DesktopUpdateStage.Failed, store.stage)
        assertEq("Update failed: cmake exited 1", store.state.error)
        channel.emit("__closed", mapOf("reason" to "gone"))
        assertEq(DesktopUpdateStage.Failed, store.stage, "a dropped link after a failure is not a restart")
        assertEq(1, channel.count("update.status"))
    }

    fun testRelaunchWaitsForTheNewDesktop() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus
        var polls = 0
        var clock = 0L
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline, now = { clock }) { ms ->
            clock += ms
            polls++
            if (polls == 1) channel.failures["update.status"] = "not_connected"
            if (polls == 3) {
                channel.failures.remove("update.status")
                channel.replies["update.status"] = mapOf(
                    "run" to 0L, "running" to false, "version" to "v1.1.0", "startedAtMs" to 99_000L,
                    "lines" to emptyList<String>(),
                )
            }
        }
        store.refresh()
        channel.emit("update.progress", mapOf("run" to 2L, "running" to true, "percent" to 80L, "lines" to listOf("==> Installing")))
        channel.emit(
            "update.progress",
            mapOf(
                "run" to 2L, "running" to true, "percent" to 100L, "status" to "Relaunching...", "relaunching" to true,
                "lines" to listOf("==> Relaunching..."),
            ),
        )
        assertEq(DesktopUpdateStage.Back, store.stage)
        assertEq("v1.0.0", store.previousVersion)
        assertEq("v1.1.0", store.version)
        assertEq(3, polls)
    }

    fun testSameDesktopStillBuildingKeepsWaiting() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus
        var polls = 0
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline, now = { 0L }) {
            polls++
            channel.replies["update.status"] = idleStatus + mapOf(
                "running" to (polls < 2),
                "error" to if (polls < 2) "" else "Update installed but the relaunch failed",
            )
        }
        store.refresh()
        channel.emit("update.progress", mapOf("run" to 2L, "running" to true, "percent" to 60L))
        channel.emit("__closed", mapOf("reason" to "relay"))
        assertEq(DesktopUpdateStage.Failed, store.stage)
        assertEq("Update installed but the relaunch failed", store.state.error)
        assertEq(2, polls)
    }

    fun testDesktopThatNeverReturnsGivesUp() {
        val channel = FakeRemoteChannel()
        channel.replies["update.status"] = idleStatus
        var clock = 0L
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline, now = { clock }) { ms ->
            clock += ms
            channel.failures["update.status"] = "not_connected"
        }
        store.refresh()
        channel.emit("update.progress", mapOf("run" to 2L, "running" to true, "relaunching" to true))
        assertEq(DesktopUpdateStage.Failed, store.stage)
        expect(store.error.contains("has not come back"), "gave up with a reason")
    }

    fun testOldDesktopIsUnsupported() {
        val channel = FakeRemoteChannel()
        channel.failures["update.status"] = "unsupported"
        val store = DesktopUpdateStore(channel, HostsExecutor.Inline)
        store.refresh()
        expect(store.unsupported, "unsupported")
        store.stop()
        assertEq(0, channel.tapCount)
    }
}
