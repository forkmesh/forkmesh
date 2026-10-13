package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs

class FakeRemoteChannel : ControlCalling {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, String>()
    private val taps = LinkedHashMap<Any, (ControlEvent) -> Unit>()

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        failures[method]?.let { throw LocalControlException(it) }
        return replies[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps[token] = tap
        return token
    }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }

    val tapCount: Int get() = taps.size

    fun emit(name: String, data: Map<String, Any?> = emptyMap()) {
        for (tap in taps.values.toList()) tap(ControlEvent(name, data))
    }

    fun last(method: String): Map<String, Any?>? = calls.lastOrNull { it.first == method }?.second

    fun count(method: String): Int = calls.count { it.first == method }
}

private fun near(expected: Double, actual: Double, message: String = "") =
    expect(abs(expected - actual) < 0.001, "$message expected $expected but was $actual")

class DesktopRemoteTest {
    private val summaryJson: Map<String, Any?> = mapOf(
        "costs" to mapOf(
            "hosts" to 3L, "vultr" to 2L, "monthlyCost" to 15.0,
            "byRegion" to listOf(mapOf("region" to "ewr", "count" to 2L, "monthlyCost" to 15.0)),
            "byPlan" to listOf(mapOf("plan" to "vc2-1c-2gb", "count" to 1L, "monthlyCost" to 10.0)),
        ),
        "fleet" to mapOf(
            "available" to true, "enabled" to true, "desired" to 3L, "status" to "Target: 3", "nextCheckInMs" to 125_000L,
            "reconciling" to false,
        ),
        "provision" to mapOf(
            "active" to true, "node" to "mirror7", "stage" to 4L, "stageCount" to 6L, "state" to "active",
            "detail" to "Waiting for SSH", "resumable" to false,
            "stages" to listOf(
                mapOf("number" to 1L, "name" to "Credentials", "status" to "done"),
                mapOf("number" to 2L, "name" to "Plan + image", "status" to "done"),
                mapOf("number" to 3L, "name" to "Create server", "status" to "done"),
                mapOf("number" to 4L, "name" to "Boot + connect", "status" to "active", "detail" to "probe 3"),
                mapOf("number" to 5L, "name" to "Install", "status" to "pending"),
                mapOf("number" to 6L, "name" to "Live traffic", "status" to "pending"),
            ),
        ),
        "gateways" to listOf(
            mapOf("host" to "mirror2", "downSinceMs" to 1000L, "downForMs" to 7_200_000L, "retired" to false),
            mapOf("host" to "mirror1", "downSinceMs" to 10L, "downForMs" to 180_000_000L, "retired" to true),
        ),
        "mirrorClient" to mapOf("status" to "Running", "health" to "Healthy"),
        "mirrorPush" to mapOf("pushing" to 1L, "pending" to 0L),
        "canCreate" to true, "busy" to true, "deploying" to 0L, "admin" to true,
        "nodeDelete" to mapOf("inProgress" to false, "target" to ""),
        "running" to listOf("agentClis:mirror3"),
    )

    fun testSummaryParses() {
        val channel = FakeRemoteChannel()
        channel.replies["fleet.summary"] = summaryJson
        val store = FleetStore(channel, HostsExecutor.Inline)
        store.refresh()
        val summary = store.summary!!
        assertEq("2 Vultr servers · $15.00/month", summary.costs.line)
        near(180.0, summary.costs.yearlyCost)
        assertEq("Keeps 3 healthy mirrors · next check in 2m 5s", summary.fleet.line)
        assertEq("Deploying mirror7: Boot + connect", summary.provision.headline)
        near(0.5, summary.provision.progress)
        expect(summary.provision.isVisible)
        assertEq(1, summary.gatewaysDown)
        assertEq("Down 2d 2h · retired from the automatic push", summary.gateways[1].line)
        assertEq("Running", summary.mirrorClientStatus)
        expect(store.isRunning("agentClis", "mirror3"))
    }

    fun testProvisionStageMachine() {
        val paused = ProvisionStatus(node = "mirror8", resumable = true, state = "paused")
        assertEq("mirror8 is paused", paused.headline)
        expect(paused.isVisible)
        val failed = ProvisionStatus(node = "mirror8", state = "failed")
        assertEq("mirror8 failed", failed.headline)
        expect(failed.isVisible)
        val done = ProvisionStatus(
            node = "mirror8",
            state = "done",
            stages = (1..6).map { ProvisionStage(it, "s$it", ProvisionStageState.Done, "") },
        )
        assertEq("mirror8 is live", done.headline)
        near(1.0, done.progress)
        expect(!done.isVisible)
        assertEq("Deploying New mirror", ProvisionStatus(active = true).headline)
        near(0.0, ProvisionStatus(stageCount = 0).progress)
        assertEq(ProvisionStageState.Pending, ProvisionStageState.of("mystery"))
    }

    fun testReconcileCountdown() {
        val target = FleetTarget(available = true, enabled = true, desired = 2, nextCheckInMs = 60_000)
        assertEq(60_000L, target.countdown(0))
        assertEq(15_000L, target.countdown(45_000))
        assertEq(0L, target.countdown(90_000))
        assertEq(-1L, FleetTarget(nextCheckInMs = -1).countdown(10))
        assertEq("Keeps 1 healthy mirror · checking now", FleetTarget(available = true, enabled = true, desired = 1, reconciling = true).line)
        assertEq("The desktop does not manage the fleet size.", FleetTarget(available = true).line)
        var clock = 1_000L
        val channel = FakeRemoteChannel()
        channel.replies["fleet.summary"] = summaryJson
        val store = FleetStore(channel, HostsExecutor.Inline) { clock }
        store.refresh()
        clock += 25_000
        assertEq(100_000L, store.targetCountdown())
    }

    fun testUnsupportedDesktopIsRecognised() {
        val channel = FakeRemoteChannel()
        channel.failures["fleet.summary"] = "unsupported"
        val store = FleetStore(channel, HostsExecutor.Inline)
        store.refresh()
        expect(store.unsupported)
        assertEq("This desktop's ForkMesh is too old for that. Update it.", store.error)
    }

    fun testPrepareAndRunCarryTheTokenAndTypedPhrase() {
        val channel = FakeRemoteChannel()
        channel.replies["fleet.prepare"] = mapOf(
            "token" to "tok", "action" to "deleteNode", "subject" to "mirror4", "summary" to "Delete",
            "typeToConfirm" to "DELETE mirror4",
        )
        channel.replies["fleet.run"] = mapOf("started" to true, "action" to "deleteNode", "subject" to "mirror4", "message" to "Deleting")
        val store = FleetStore(channel, HostsExecutor.Inline)
        val confirmation = store.prepare(FleetAction.DeleteNode("mirror4"))
        assertEq("mirror4", channel.last("fleet.prepare").str("host"))
        expect(confirmation.needsTypedConfirmation)
        expect(confirmation.needsDeviceAuthentication)
        val error = assertFails<LocalControlException> { store.run(confirmation, "delete mirror4") }
        assertEq("name_mismatch", error.code)
        assertEq(0, channel.count("fleet.run"))
        assertEq("Deleting", store.run(confirmation, " DELETE mirror4 "))
        val sent = channel.last("fleet.run")!!
        assertEq("tok", sent.str("token"))
        assertEq("deleteNode", sent.str("action"))
        assertEq("mirror4", sent.str("host"))
        assertEq("DELETE mirror4", sent.str("confirmText"))
        expect("deleteNode:mirror4" in store.running)
    }

    fun testPrepareWithoutATokenIsABadAnswer() {
        val channel = FakeRemoteChannel()
        channel.replies["fleet.prepare"] = mapOf("action" to "reconcile")
        val store = FleetStore(channel, HostsExecutor.Inline)
        val error = assertFails<LocalControlException> { store.prepare(FleetAction.Reconcile) }
        assertEq("bad_answer", error.code)
    }

    fun testActionBellsRecordOutcomesAndChangedRefreshes() {
        val channel = FakeRemoteChannel()
        channel.replies["fleet.summary"] = summaryJson
        var clock = 1000L
        val store = FleetStore(channel, HostsExecutor.Inline) { clock }
        channel.emit("fleet.action", mapOf("action" to "agentClis", "subject" to "mirror3", "ok" to true, "message" to "Installed"))
        assertEq("Installed", store.outcomes.first().message)
        assertEq(1000L, store.outcomes.first().atMs)
        assertEq(1, store.outcomesFor("mirror3").size)
        clock = 2000
        channel.emit("hosts.changed")
        assertEq(0, channel.count("fleet.summary"))
        channel.emit("fleet.changed")
        assertEq(1, channel.count("fleet.summary"))
        store.stop()
        assertEq(0, channel.tapCount)
    }

    fun testActionParams() {
        assertEq(4L, FleetAction.Target(true, 4).params["desired"])
        assertEq("fleetTarget", FleetAction.Target(true, 4).params["action"])
        assertEq("reinstall", FleetAction.DeployAll(FleetDeployMode.Reinstall).params.str("mode"))
        assertEq(false, FleetAction.Actions("m1", false).params["enabled"])
        assertEq("m9", FleetAction.Add(" m9 ", "1.2.3.4", "root").params.str("host"))
        assertEq("mirror4", FleetAction.DeleteNode("mirror4").params.str("host"))
        assertEq("123456", FleetAction.LinkCode(" 123456 ").params.str("code"))
        assertEq(listOf("action"), FleetAction.Reconcile.params.keys.toList())
        expect(FleetAction.DeployAll(FleetDeployMode.Reinstall).isDestructive)
        expect(!FleetAction.DeployAll(FleetDeployMode.Install).isDestructive)
        expect(FleetInput.validLinkCode("012345"))
        expect(!FleetInput.validLinkCode("12a456"))
        expect(FleetInput.validHostName("mirror-9"))
        expect(!FleetInput.validHostName("-bad"))
        expect(FleetInput.validAddress("2001:db8::1"))
        expect(!FleetInput.validAddress("1.2.3.4; reboot"))
        expect(FleetInput.validUser("root"))
        expect(!FleetInput.validUser("Root"))
    }

    fun testDiskUsageTree() {
        val usage = DiskUsage.fromJson(
            mapOf(
                "host" to "m1", "path" to "/var/lib",
                "entries" to listOf(
                    mapOf("name" to ".", "path" to "/var/lib", "bytes" to 1000.0, "total" to true),
                    mapOf("name" to "apt", "path" to "/var/lib/apt", "bytes" to 50.0),
                    mapOf("name" to "forkmesh", "path" to "/var/lib/forkmesh", "bytes" to 750.0),
                ),
            ),
        )
        near(1000.0, usage.total)
        assertEq(2, usage.children.size)
        assertEq(listOf("forkmesh", "apt"), usage.sortedChildren.map { it.name })
        near(0.75, usage.share(usage.sortedChildren[0]))
        assertEq("/var", usage.parentPath)
        assertEq("/", DiskUsage("m1", "/var", emptyList()).parentPath)
        assertEq(null, DiskUsage.fromJson(mapOf("path" to "/")).parentPath)
        val noTotal = DiskUsage("m1", "/x", listOf(DiskUsageEntry("a", "/x/a", 10.0, false), DiskUsageEntry("b", "/x/b", 30.0, false)))
        near(40.0, noTotal.total)
        near(0.0, DiskUsage("m1", "/x", emptyList()).let { it.share(DiskUsageEntry("a", "/x/a", 5.0, false)) })
    }

    fun testFormatting() {
        assertEq("512 B", RemoteFormat.bytes(512.0))
        assertEq("1.5 KB", RemoteFormat.bytes(1536.0))
        assertEq("150 MB", RemoteFormat.bytes(150.0 * 1024 * 1024))
        assertEq("—", RemoteFormat.bytes(-1.0))
        assertEq("59s", RemoteFormat.duration(59_000))
        assertEq("1h 0m", RemoteFormat.duration(3_600_000))
        assertEq("$5.00", RemoteFormat.money(5.0))
        assertEq("42%", RemoteFormat.percent(42.4))
        assertEq("The desktop already has a host with that name.", RemoteErrorText.describe("host_exists"))
        assertEq("The desktop did not answer in time.", RemoteErrorText.describe("timeout"))
    }

    fun testRootsGroupWorktreesUnderTheirRepository() {
        val channel = FakeRemoteChannel()
        channel.replies["files.browse.roots"] = mapOf(
            "roots" to listOf(
                mapOf("id" to "f/forkmesh", "label" to "f/forkmesh", "kind" to "repo", "branch" to "main", "repo" to "f/forkmesh"),
                mapOf("id" to "f/forkmesh#b", "label" to "zeta", "kind" to "worktree", "branch" to "zeta", "repo" to "f/forkmesh"),
                mapOf("id" to "f/forkmesh#a", "label" to "alpha", "kind" to "worktree", "branch" to "alpha", "repo" to "f/forkmesh"),
                mapOf("id" to "f/other#x", "label" to "x", "kind" to "worktree", "repo" to "f/other"),
            ),
        )
        val store = FilesBrowserStore(channel)
        store.loadRoots()
        val groups = store.groups
        assertEq(listOf("f/forkmesh", "f/other"), groups.map { it.repo.id })
        assertEq(listOf("alpha", "zeta"), groups[0].worktrees.map { it.label })
        assertEq("f/other", groups[1].repo.label)
    }

    fun testListCachesAndReadDecodes() {
        val channel = FakeRemoteChannel()
        channel.replies["files.browse.list"] = mapOf(
            "root" to "f/r", "path" to "src", "total" to 2L, "truncated" to false,
            "entries" to listOf(
                mapOf("name" to "lib", "kind" to "dir"),
                mapOf("name" to ".env", "kind" to "file", "size" to 20.0, "sensitive" to true),
                mapOf("name" to "out", "kind" to "symlink", "escapes" to true),
            ),
        )
        val store = FilesBrowserStore(channel)
        val listing = store.list("f/r", "src")
        assertEq("src", channel.last("files.browse.list").str("path"))
        assertEq(listing, store.cached("f/r", "src"))
        expect(listing.entries[0].kind.isFolder)
        expect(!listing.entries[1].canOpen)
        expect(!listing.entries[2].canOpen)
        assertEq("20 B", listing.entries[1].sizeLabel)
        channel.replies["files.browse.read"] = mapOf(
            "size" to 10.0, "offset" to 0.0, "length" to 4.0, "eof" to false, "binary" to true, "base64" to "AAEC",
        )
        val slice = store.read("f/r", "a.bin")
        assertEq(listOf<Byte>(0, 1, 2), slice.data!!.toList())
        near(4.0, slice.nextOffset)
    }

    fun testStatusAndGitActions() {
        val channel = FakeRemoteChannel()
        channel.replies["files.browse.status"] = mapOf(
            "root" to "f/r", "branch" to "main", "upstream" to "origin/main", "ahead" to 0L, "behind" to 3L, "total" to 2L,
            "staged" to 1L, "unstaged" to 1L, "untracked" to 0L, "conflicted" to 0L,
            "files" to listOf(
                mapOf("path" to "a", "index" to "M", "worktree" to " ", "kind" to "modified"),
                mapOf("path" to "b", "index" to " ", "worktree" to "M", "kind" to "modified"),
            ),
        )
        val store = FilesBrowserStore(channel)
        channel.emit("files.browse.changed", mapOf("root" to "f/r"))
        expect("f/r" in store.changedRoots)
        val status = store.status("f/r")
        expect("f/r" !in store.changedRoots)
        assertEq("3 behind origin/main", status.syncLine)
        assertEq("1 staged · 1 modified", status.changesLine)
        expect(status.canFastForward)
        expect(status.files[0].isStaged)
        expect(!status.files[1].isStaged)
        assertEq(status, store.cachedStatus("f/r"))
        assertEq("No upstream", GitWorkingStatus().syncLine)
        assertEq("No changes", GitWorkingStatus().changesLine)
        assertEq("2 ahead, 1 behind o/m", GitWorkingStatus(upstream = "o/m", ahead = 2, behind = 1).syncLine)
        channel.replies["files.browse.prepare"] = mapOf("token" to "t1", "action" to "pull", "subject" to "f/r", "summary" to "Pull")
        channel.replies["files.browse.run"] = mapOf("started" to true, "message" to "Pulling")
        val confirmation = store.prepare(GitRemoteAction.Pull, "f/r")
        expect(!confirmation.needsTypedConfirmation)
        assertEq("Pulling", store.run(confirmation))
        assertEq("f/r", channel.last("files.browse.run").str("root"))
        assertEq("t1", channel.last("files.browse.run").str("token"))
        expect("confirmText" !in channel.last("files.browse.run")!!)
    }

    fun testRemotePaths() {
        assertEq("a", RemotePath.join("", "a"))
        assertEq("a/b", RemotePath.join("a", "b"))
        assertEq("a", RemotePath.parent("a/b"))
        assertEq("", RemotePath.parent("a"))
        assertEq(null, RemotePath.parent(""))
        assertEq(listOf("a", "a/b"), RemotePath.crumbs("a/b").map { it.second })
        expect(RemotePath.isImage("x/Logo.PNG"))
        expect(!RemotePath.isImage("x/main.cpp"))
        assertEq("", RemotePath.fileExtension("Makefile"))
    }

    fun testHealthParses() {
        val channel = FakeRemoteChannel()
        channel.replies["diag.health"] = mapOf(
            "desktop" to "box", "level" to "busy", "cpuPercent" to 42.0, "cpuCount" to 8L, "load1" to 3.5,
            "memory" to mapOf("total" to 1000.0, "available" to 250.0, "appResident" to 100.0),
            "pressure" to mapOf("valid" to true, "memoryFull" to 0.5),
            "disk" to mapOf("total" to 2000.0, "free" to 500.0),
            "agents" to mapOf("running" to 2L, "queued" to 1L, "total" to 5L),
            "stalls" to 3L,
        )
        val store = DiagnosticsStore(channel, HostsExecutor.Inline)
        store.refreshHealth()
        val health = store.health!!
        assertEq(DesktopHealthLevel.Busy, health.level)
        near(0.75, health.memoryUsedShare)
        near(0.75, health.diskUsedShare)
        assertEq("2 running · 1 queued", health.agentsLine)
        assertEq("3.5 on 8 CPUs", health.loadLine)
        assertEq(3, health.stalls)
        assertEq(2, DesktopHealth.fromJson(mapOf("stalls" to listOf(1L, 2L))).stalls)
        assertEq("—", DesktopHealth().memoryLine)
        assertEq(null, DesktopHealth().memoryUsedShareOrNull)
        val onBattery = DesktopHealth.fromJson(mapOf("power" to mapOf("onBattery" to true, "percent" to 61L)))
        assertEq(true, onBattery.onBattery)
        near(0.61, onBattery.batteryShare)
        assertEq("61% left", onBattery.batteryLine)
        assertEq(false, DesktopHealth().onBattery)
        assertEq("On battery", DesktopHealth().batteryLine)
    }

    fun testLogTailPagesBackwards() {
        val channel = FakeRemoteChannel()
        channel.replies["diag.log"] = mapOf(
            "lines" to listOf(
                mapOf("offset" to 100L, "t" to 1L, "severity" to "info", "text" to "2026-09-24 10:00:00  hello"),
                mapOf("offset" to 200L, "t" to 1L, "severity" to "error", "text" to "2026-09-24 10:00:01  boom failed"),
            ),
            "start" to 100L, "end" to 300L, "more" to true,
        )
        val store = DiagnosticsStore(channel, HostsExecutor.Inline)
        val tail = DesktopLogTail(store, pageSize = 2)
        tail.severity = LogSeverity.Error
        tail.query = "boom"
        tail.loadLatest()
        assertEq(2, tail.lines.size)
        assertEq("boom failed", tail.lines[1].body)
        assertEq(1, tail.errorCount)
        assertEq("error", channel.last("diag.log").str("severity"))
        assertEq("boom", channel.last("diag.log").str("query"))
        expect("before" !in channel.last("diag.log")!!)
        channel.replies["diag.log"] = mapOf(
            "lines" to listOf(mapOf("offset" to 10L, "t" to 1L, "severity" to "info", "text" to "older")),
            "start" to 10L, "end" to 100L, "more" to false,
        )
        tail.loadOlder()
        assertEq(100L, channel.last("diag.log")!!["before"])
        assertEq(listOf(10L, 100L, 200L), tail.lines.map { it.offset })
        expect(!tail.more)
        tail.loadOlder()
        assertEq(2, channel.count("diag.log"))
    }

    fun testLogTailKeepsAtMostMaxLines() {
        val channel = FakeRemoteChannel()
        channel.replies["diag.log"] = mapOf(
            "lines" to (5L..6L).map { mapOf("offset" to it, "text" to "l$it") },
            "more" to true,
        )
        val store = DiagnosticsStore(channel, HostsExecutor.Inline)
        val tail = DesktopLogTail(store, pageSize = 2, maxLines = 3)
        tail.loadLatest()
        channel.replies["diag.log"] = mapOf("lines" to (3L..4L).map { mapOf("offset" to it, "text" to "l$it") }, "more" to true)
        tail.loadOlder()
        assertEq(listOf(3L, 4L, 5L), tail.lines.map { it.offset })
    }

    fun testProcessesAndStopAgent() {
        val channel = FakeRemoteChannel()
        channel.replies["diag.processes"] = mapOf(
            "memoryTotal" to 1000.0,
            "processes" to listOf(
                mapOf(
                    "pid" to 10L, "name" to "claude", "rssBytes" to 400.0, "percent" to 40.0,
                    "agent" to mapOf("session" to 7L, "label" to "Fix bug", "stoppable" to true),
                ),
                mapOf("pid" to 11L, "name" to "chrome", "rssBytes" to 100.0, "percent" to 10.0),
            ),
        )
        channel.replies["diag.prepare"] = mapOf("token" to "t", "action" to "stopAgent", "subject" to "7", "summary" to "Stop")
        channel.replies["diag.run"] = mapOf("started" to true, "message" to "Stop requested.")
        val store = DiagnosticsStore(channel, HostsExecutor.Inline)
        store.refreshProcesses()
        val list = store.processes!!
        assertEq(listOf(10), list.agentProcesses.map { it.pid })
        near(0.4, list.share(list.processes[0]))
        val confirmation = store.prepare(DiagnosticsAction.StopAgent(7))
        assertEq(7L, channel.last("diag.prepare")!!["session"])
        assertEq("Stop requested.", store.run(confirmation))
        assertEq(2, channel.count("diag.processes"))
        channel.emit("diag.changed")
        assertEq(3, channel.count("diag.processes"))
    }

    fun testStallsAndBackups() {
        val channel = FakeRemoteChannel()
        channel.replies["diag.stalls"] = mapOf(
            "canSend" to true,
            "stalls" to listOf(mapOf("index" to 1L, "when" to "2026-09-24 09:00:00", "summary" to "UI froze", "lines" to 30L)),
        )
        channel.replies["diag.stall"] = mapOf("index" to 1L, "text" to "frame", "truncated" to true)
        channel.replies["diag.backups"] = mapOf(
            "enabled" to true, "keep" to 24L, "running" to false, "result" to "failed", "count" to 2L,
            "totalBytes" to 3072.0, "lastBackupAtMs" to 1_700_000_000_000L, "lastBackupBytes" to 2048.0, "error" to "tar_missing",
        )
        val store = DiagnosticsStore(channel, HostsExecutor.Inline)
        store.refreshStalls()
        assertEq("UI froze", store.stalls!!.stalls.first().summary)
        expect(store.stalls!!.canSend)
        expect(store.stall(1).truncated)
        assertEq("sendStall", DiagnosticsAction.SendStall(1).params.str("action"))
        store.refreshBackups()
        val backups = store.backups!!
        assertEq(DesktopBackupResult.Failed, backups.result)
        expect(backups.hasBackup)
        assertEq("2.0 KB · 2 copies, 3.0 KB in all", backups.sizeLine)
        assertEq("The desktop has no tar tool to write backups with.", backups.errorText)
    }
}
