package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.ControlTransport
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class FakeHostsChannel : HostsChannel {
    override var reach: DesktopReach = DesktopReach.Connected(ControlTransport.Relay, "desk")
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val answers = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, String>()
    private val taps = LinkedHashMap<Any, (ControlEvent) -> Unit>()
    val reachListeners = ArrayList<() -> Unit>()

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        failures[method]?.let { throw LocalControlException(it) }
        return answers[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps[token] = tap
        return token
    }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }

    override fun addReachListener(listener: () -> Unit) {
        reachListeners.add(listener)
    }

    override fun removeReachListener(listener: () -> Unit) {
        reachListeners.remove(listener)
    }

    val tapCount: Int get() = taps.size

    fun emit(name: String, data: Map<String, Any?> = emptyMap()) {
        for (tap in taps.values.toList()) tap(ControlEvent(name, data))
    }

    fun calls(method: String): List<Map<String, Any?>> = calls.filter { it.first == method }.map { it.second }
}

private val mirrorRow: Map<String, Any?> = mapOf(
    "name" to "mirror35", "address" to "203.0.113.5", "user" to "root", "status" to "installed", "provider" to "Vultr",
    "region" to "ewr", "plan" to "vc2-1c-1gb", "monthlyCost" to 5L, "health" to "online",
    "healthLabel" to "● Online · Mirror node", "version" to "0.8.6", "online" to true,
    "actions" to listOf("update", "restart", "stop", "start", "destroy"),
)

private val listAnswer: Map<String, Any?> = mapOf(
    "hosts" to listOf(
        mirrorRow,
        mapOf(
            "name" to "box", "address" to "10.0.0.2", "user" to "pi", "status" to "installed", "health" to "offline",
            "healthLabel" to "● Offline · unreachable", "actions" to listOf("update", "teleport"),
        ),
    ),
    "online" to 1L, "attention" to 1L, "canCreate" to true, "busy" to false, "desktop" to "desk",
)

class HostsTest {
    fun testRowsAndSummaryReadTheDesktopsList() {
        val snapshot = HostsSnapshot.fromJson(listAnswer)
        assertEq(listOf("box", "mirror35"), snapshot.sorted.map { it.name })
        val mirror = snapshot.hosts[0]
        expect(mirror.isVultr)
        assertEq("vc2-1c-1gb · ewr · $5/mo", mirror.planLine)
        assertEq("Online · Mirror node", mirror.healthLine)
        assertEq(listOf(HostAction.Update, HostAction.Restart, HostAction.Stop, HostAction.Start, HostAction.Destroy), mirror.actions)
        assertEq(listOf(HostAction.Update), snapshot.hosts[1].actions)
        assertEq("2 saved · 1 online · 1 needs attention", snapshot.summary)
        assertEq("All 1 host online", HostsSnapshot.fromJson(mapOf("hosts" to listOf(mirrorRow), "online" to 1L)).summary)
        assertEq("No hosts yet", HostsSnapshot().summary)
        assertEq(HostHealth.Unknown, HostHealth.fromWire("weird"))
        assertEq("$3.50/mo", HostFormat.monthly(3.5))
        assertEq("2 GB RAM", HostFormat.memory(2048))
        assertEq("1500 MB RAM", HostFormat.memory(1500))
        assertEq("Chicago (ord)", HostFormat.region("ord"))
        assertEq("xyz", HostFormat.region("xyz"))
    }

    fun testDestroyNeedsTheNameTypedBackExactly() {
        val confirmation = HostConfirmation.fromJson(
            mapOf("token" to "t", "action" to "destroy", "host" to "mirror35", "summary" to "gone", "typeToConfirm" to "mirror35"),
        )!!
        expect(confirmation.needsTypedName)
        expect(!confirmation.accepts(""))
        expect(!confirmation.accepts("MIRROR35"))
        expect(confirmation.accepts(" mirror35 "))
        assertEq(60_000, confirmation.expiresInMs)
        val restart = HostConfirmation.fromJson(mapOf("token" to "t", "action" to "restart", "host" to "mirror35"))!!
        expect(restart.accepts(""))
        assertEq(null, HostConfirmation.fromJson(mapOf("token" to "", "action" to "restart")))
        assertEq(null, HostConfirmation.fromJson(mapOf("token" to "t", "action" to "format-disk")))
        expect(HostAction.Destroy.needsTypedName && !HostAction.Update.needsTypedName)
        expect(HostAction.Stop.isDestructive && !HostAction.Restart.isDestructive)
    }

    fun testTheAddHostFormOffersTheDesktopsOwnChoices() {
        val options = HostCreateOptions.fromJson(
            mapOf(
                "vultr" to true, "defaultPlan" to "vhp-1c-2gb", "defaultRegion" to "ewr", "suggestedName" to "mirror39",
                "plans" to listOf(
                    mapOf("id" to "vhp-1c-2gb", "monthlyCost" to 5L, "ramMb" to 2048L, "diskGb" to 50L, "vcpus" to 1L, "regions" to listOf("ewr", "atl")),
                    mapOf("id" to "vc2-2c-4gb", "monthlyCost" to 20L, "ramMb" to 4096L, "regions" to listOf("ord")),
                ),
            ),
        )
        var form = AddHostForm(options)
        assertEq("vhp-1c-2gb", form.planId)
        assertEq("ewr", form.region)
        assertEq(null, form.problem)
        assertEq("mirror39", form.effectiveName)
        assertEq("", form.requestedPlan)
        assertEq("", form.requestedRegion)
        assertEq("$5/mo · 1 vCPU · 2 GB RAM · 50 GB disk", form.plan?.label)

        form = form.copy(region = "atl")
        assertEq("atl", form.requestedRegion)
        form = form.choosePlan("vc2-2c-4gb")
        assertEq("ord", form.region)
        assertEq("vc2-2c-4gb", form.requestedPlan)

        form = form.copy(name = "bad name")
        expect(form.problem != null)
        form = form.copy(name = "mirror-40")
        assertEq(null, form.problem)
        assertEq("mirror-40", form.effectiveName)
        expect(!AddHostForm.validName("-x"))
        expect(!AddHostForm.validName("é1"))
        expect(!AddHostForm.validName("a".repeat(64)))

        expect(AddHostForm(HostCreateOptions.fromJson(mapOf("vultr" to false))).problem != null)
        expect(AddHostForm(HostCreateOptions(vultr = true)).problem != null)
    }

    fun testTheLogBufferPausesSearchesCopiesAndStaysBounded() {
        val buffer = HostLogBuffer(limit = 100)
        buffer.appendBacklog(listOf("[mirror] started", "plain line"))
        assertEq(listOf("mirror", ""), buffer.lines.map { it.source })
        assertEq(listOf("started", "plain line"), buffer.lines.map { it.text })

        buffer.append(
            mapOf(
                "seq" to 1L, "dropped" to 3L,
                "lines" to listOf(
                    mapOf("t" to 1L, "src" to "ssh", "text" to "Accepted publickey"),
                    mapOf("t" to 2L, "src" to "system", "text" to "kernel: eth0 up"),
                ),
            ),
        )
        assertEq(3, buffer.dropped)
        assertEq(listOf("Accepted publickey"), buffer.filtered("PUBLICKEY").map { it.text })
        assertEq(1, buffer.filtered("ssh").size)
        assertEq(4, buffer.filtered("").size)
        assertEq("[system] kernel: eth0 up", buffer.copyText("kernel"))

        buffer.setPaused(true)
        buffer.append(mapOf("lines" to listOf(mapOf("src" to "node", "text" to "held"))))
        assertEq(4, buffer.lines.size)
        assertEq(1, buffer.held.size)
        buffer.setPaused(false)
        assertEq("held", buffer.lines.last().text)
        assertEq(0, buffer.heldCount)

        for (i in 0 until 250) buffer.append(mapOf("lines" to listOf(mapOf("src" to "node", "text" to "l$i"))))
        assertEq(100, buffer.lines.size)
        assertEq("l249", buffer.lines.last().text)
        assertEq(3 + 155, buffer.dropped)
        val ids = buffer.lines.map { it.id }
        assertEq(ids.size, ids.toSet().size)
    }

    fun testAPausedBufferKeepsOnlyTheNewestHeldLines() {
        val buffer = HostLogBuffer(limit = 10)
        assertEq(100, buffer.limit)
        buffer.setPaused(true)
        for (i in 0 until 130) buffer.append(mapOf("lines" to listOf(mapOf("text" to "h$i"))))
        assertEq(100, buffer.heldCount)
        assertEq(30, buffer.dropped)
        assertEq(0, buffer.lines.size)
        buffer.setPaused(false)
        assertEq("h30", buffer.lines.first().text)
        buffer.clear()
        assertEq(0, buffer.lines.size)
    }
}

class HostsStoreTest {
    fun testListRefreshesOnTheDesktopsBell() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.list"] = listAnswer
        val store = HostsStore(channel, HostsExecutor.Inline)
        var notified = 0
        store.addListener { notified++ }
        store.refresh()
        assertEq(2, store.snapshot?.hosts?.size)
        assertEq("0.8.6", store.host("MIRROR35")?.version)
        expect(!store.loading)
        expect(notified >= 2)
        channel.emit("hosts.changed")
        assertEq(2, channel.calls("hosts.list").size)

        channel.failures["hosts.list"] = "not_owner"
        store.refresh()
        assertEq("Only the account that owns this desktop can manage its hosts.", store.error)
        assertEq(2, store.snapshot?.hosts?.size)
        store.dispose()
        assertEq(0, channel.tapCount)
    }

    fun testARefreshAskedForDuringARefreshRunsOnceMoreAfterIt() {
        val channel = object : HostsChannel by FakeHostsChannel() {
            var count = 0
            lateinit var store: HostsStore
            override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
                count++
                if (count == 1) {
                    store.refresh()
                    store.refresh()
                }
                return listAnswer
            }
        }
        val store = HostsStore(channel, HostsExecutor.Inline)
        channel.store = store
        store.refresh()
        assertEq(2, channel.count)
        expect(!store.loading)
    }

    fun testEveryChangeIsPreparedThenRunWithItsToken() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = mapOf(
            "token" to "tok-1", "action" to "destroy", "host" to "mirror35", "summary" to "Permanently DELETE",
            "typeToConfirm" to "mirror35",
        )
        channel.answers["hosts.run"] = mapOf("started" to true, "host" to "mirror35", "message" to "Destroying the server.")
        val store = HostsStore(channel, HostsExecutor.Inline) { 42L }
        val confirmation = store.prepare(HostAction.Destroy, "mirror35")
        assertEq("destroy", channel.calls("hosts.prepare").first().str("action"))

        val mismatch = assertFails<LocalControlException> { store.run(confirmation, typedName = "mirror3") }
        assertEq("name_mismatch", mismatch.code)
        expect(channel.calls("hosts.run").isEmpty())

        assertEq("Destroying the server.", store.run(confirmation, typedName = "mirror35"))
        val run = channel.calls("hosts.run")[0]
        assertEq("tok-1", run.str("token"))
        assertEq("mirror35", run.str("confirmName"))
        expect(store.isRunning(HostAction.Destroy, "Mirror35"))
        expect(store.isRunningAny("MIRROR35"))
        assertEq(1, channel.calls("hosts.list").size)

        channel.emit("hosts.action", mapOf("action" to "destroy", "host" to "mirror35", "ok" to true, "message" to "Destroyed"))
        expect(!store.isRunning(HostAction.Destroy, "mirror35"))
        assertEq("Destroy server of mirror35 finished", store.outcomes.first().headline)
        assertEq(42L, store.outcomes.first().atMs)
        assertEq(1, store.outcomesFor("MIRROR35").size)

        assertFails<LocalControlException> {
            channel.answers["hosts.prepare"] = mapOf("token" to "", "action" to "destroy")
            store.prepare(HostAction.Destroy, "mirror35")
        }
    }

    fun testAnSshTerminalIsConfirmedThenOpenedOnTheDesktop() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = mapOf(
            "token" to "tok-3", "action" to "terminal", "host" to "mirror35", "summary" to "Open an SSH shell",
        )
        channel.answers["hosts.run"] = mapOf(
            "started" to true, "terminal" to "t-ssh", "provider" to "ssh", "host" to "mirror35", "label" to "SSH to mirror35",
        )
        val store = HostsStore(channel, HostsExecutor.Inline)
        val confirmation = store.prepare(HostAction.Terminal, "mirror35")
        expect(!confirmation.needsTypedName)

        val refused = assertFails<LocalControlException> { store.run(confirmation) }
        assertEq("unsupported_action", refused.code)
        expect(channel.calls("hosts.run").isEmpty())

        assertEq("t-ssh", store.openTerminal(confirmation, cols = 60, rows = 30))
        val run = channel.calls("hosts.run")[0]
        assertEq("terminal", run.str("action"))
        assertEq("tok-3", run.str("token"))
        assertEq(60, run["cols"])
        assertEq(30, run["rows"])
        expect(!store.isRunning(HostAction.Terminal, "mirror35"))

        channel.answers["hosts.run"] = mapOf("started" to true)
        assertEq("terminal_failed", assertFails<LocalControlException> { store.openTerminal(confirmation) }.code)

        channel.answers["terminal.list"] = mapOf(
            "terminals" to listOf(
                mapOf("terminal" to "a", "provider" to "ssh", "host" to "Mirror35"),
                mapOf("terminal" to "b", "provider" to "ssh", "host" to "box"),
                mapOf("terminal" to "c", "provider" to "claude-code"),
            ),
        )
        assertEq(listOf("a"), store.openTerminals("mirror35").map { it.terminal })
        assertEq(HostAction.InstallBinary, HostAction.fromWire("installBinary"))
    }

    fun testOutcomesKeepTheNewestTwenty() {
        val channel = FakeHostsChannel()
        var clock = 0L
        val store = HostsStore(channel, HostsExecutor.Inline) { ++clock }
        for (i in 0 until 25) channel.emit("hosts.action", mapOf("action" to "update", "host" to "h$i", "ok" to false))
        assertEq(20, store.outcomes.size)
        assertEq("h24", store.outcomes.first().host)
        assertEq("Update ForkMesh of h24 failed", store.outcomes.first().headline)
    }

    fun testANewHostSendsOnlyTheChoicesThatDifferFromTheDefaults() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.prepare"] = mapOf("token" to "tok-2", "action" to "install", "host" to "", "summary" to "bills monthly")
        channel.answers["hosts.run"] = mapOf("started" to true, "host" to "mirror39", "message" to "Creating the server.")
        val store = HostsStore(channel, HostsExecutor.Inline)
        val confirmation = store.prepare(HostAction.Install, "")
        store.run(confirmation, plan = "vc2-2c-4gb", region = "ord")
        val run = channel.calls("hosts.run")[0]
        assertEq("vc2-2c-4gb", run.str("plan"))
        assertEq("ord", run.str("region"))
        expect(!run.containsKey("confirmName"))
        expect(store.isRunning(HostAction.Install, "mirror39"))
        expect(store.isInstalling())
    }

    fun testDetailAndOptionsReadTheDesktopsAnswers() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.detail"] = mirrorRow + mapOf("recentLog" to listOf("a", "b"), "lastResult" to "ok")
        channel.answers["hosts.options"] = mapOf("vultr" to true, "suggestedName" to "mirror40")
        val store = HostsStore(channel, HostsExecutor.Inline)
        val detail = store.detail("mirror35")
        assertEq(listOf("a", "b"), detail.recentLog)
        assertEq("ok", detail.lastResult)
        assertEq("mirror35", channel.calls("hosts.detail").first().str("host"))
        assertEq("mirror40", store.createOptions().suggestedName)
    }

    fun testTheLiveLogAcknowledgesItsStreamAndIgnoresOthers() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.logs.start"] = mapOf("stream" to "st1", "backlog" to listOf("[node] hello"), "window" to 4L)
        val log = HostLogStream(channel, "mirror35", HostsExecutor.Inline)
        log.start()
        expect(log.live)
        assertEq("mirror35", channel.calls("hosts.logs.start").first().str("host"))
        assertEq(listOf("hello"), log.read { b -> b.lines.map { it.text } })
        assertEq(1, channel.reachListeners.size)

        channel.emit("hosts.log", mapOf("stream" to "other", "seq" to 1L, "lines" to listOf(mapOf("text" to "not mine"))))
        channel.emit("hosts.log", mapOf("stream" to "st1", "seq" to 1L, "lines" to listOf(mapOf("src" to "mirror", "text" to "sync ok"))))
        assertEq(listOf("hello", "sync ok"), log.read { b -> b.lines.map { it.text } })
        val ack = channel.calls("hosts.logs.ack")
        assertEq(1, ack.size)
        assertEq(1, ack.first().int("seq"))
        assertEq("st1", ack.first().str("stream"))

        log.setPaused(true)
        channel.emit("hosts.log", mapOf("stream" to "st1", "seq" to 2L, "lines" to listOf(mapOf("text" to "later"))))
        assertEq(2, channel.calls("hosts.logs.ack").size)
        assertEq(1, log.read { it.heldCount })
        log.setPaused(false)
        assertEq("later", log.read { it.lines.last().text })

        channel.emit("__closed", mapOf("reason" to "relay_reconnected"))
        expect(!log.live)
        expect(log.error.isNotEmpty())

        log.stop()
        assertEq(0, channel.tapCount)
        assertEq(0, channel.reachListeners.size)
        expect(channel.calls("hosts.logs.stop").isEmpty())
    }

    fun testTheLiveLogStopsItsStreamAndNoticesALostDesktop() {
        val channel = FakeHostsChannel()
        channel.answers["hosts.logs.start"] = mapOf("stream" to "st9")
        val log = HostLogStream(channel, "box", HostsExecutor.Inline)
        log.start()
        channel.reach = DesktopReach.Offline("gone")
        channel.reachListeners.toList().forEach { it() }
        expect(!log.live)
        assertEq("The desktop connection closed. Reopen the log to continue.", log.error)

        val again = HostLogStream(channel, "box", HostsExecutor.Inline)
        channel.reach = DesktopReach.Connected(ControlTransport.Lan, "desk")
        again.start()
        again.stop()
        assertEq("st9", channel.calls("hosts.logs.stop").first().str("stream"))

        channel.failures["hosts.logs.start"] = "timeout"
        val failing = HostLogStream(channel, "box", HostsExecutor.Inline)
        failing.start()
        expect(!failing.live)
        assertEq("The desktop did not answer in time.", failing.error)
        failing.stop()
    }
}
