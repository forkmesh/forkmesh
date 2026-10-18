package com.forkmesh.nativeapp.teammates

import com.forkmesh.nativeapp.agents.ops.FakeControlChannel
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq

class ShortcutsTest {
    private val catalogJson: Map<String, Any?> = mapOf(
        "repo" to "f/r",
        "repos" to listOf("f/R", "f/other"),
        "entries" to listOf(
            mapOf("file" to "a.md", "scope" to "shortcut", "name" to "A", "uses" to 1L, "runnable" to true),
            mapOf("file" to "deploy.md", "scope" to "shortcut", "name" to "Deploy", "uses" to 3L, "runnable" to true),
            mapOf("file" to "b.skill", "scope" to "repository", "name" to "B", "uses" to 4L),
            mapOf("file" to "c.skill", "scope" to "app", "name" to "C", "uses" to 0L),
        ),
    )

    fun testRefreshSplitsShortcutsAndSkills() {
        val channel = FakeControlChannel()
        channel.reply("insights.shortcuts", catalogJson)
        val store = ShortcutsStore(channel)
        store.refresh("f/r")
        val catalog = store.catalog!!
        assertEq(listOf("deploy.md", "a.md"), catalog.shortcuts.map { it.file })
        assertEq(listOf("b.skill", "c.skill"), catalog.skills.map { it.file })
        assertEq("f/r", channel.params("insights.shortcuts").str("repo"))
        assertEq("f/R", catalog.pick("f/r"))
        assertEq("f/R", catalog.pick("x/y"))
        assertEq("", store.error)
    }

    fun testRunGoesThroughPrepareAndRun() {
        val channel = FakeControlChannel()
        channel.reply("insights.prepare", mapOf("token" to "t", "action" to "runShortcut", "subject" to "f/r|shortcut|deploy.md"))
        channel.reply("insights.run", mapOf("started" to true, "message" to "Started agent #4 for Deploy."))
        val store = ShortcutsStore(channel)
        val entry = ShortcutEntry.from(mapOf("file" to "deploy.md", "scope" to "shortcut", "name" to "Deploy", "runnable" to true))
        val confirmation = store.prepare("f/r", entry)
        assertEq("runShortcut", channel.params("insights.prepare").str("action"))
        assertEq("deploy.md", channel.params("insights.prepare").str("file"))
        assertEq("Started agent #4 for Deploy.", store.run(confirmation))
        assertEq("t", channel.params("insights.run").str("token"))
    }

    fun testOneTapRunPreparesAndRunsWithoutADialog() {
        val channel = FakeControlChannel()
        channel.reply("insights.prepare", mapOf("token" to "t", "action" to "runShortcut", "subject" to "f/r|shortcut|ship.sh"))
        channel.reply("insights.run", mapOf("started" to true, "message" to "Running Ship on the desktop."))
        channel.reply("insights.shortcuts", catalogJson)
        val store = ShortcutsStore(channel)
        val entry = ShortcutEntry.from(mapOf("file" to "ship.sh", "scope" to "shortcut", "name" to "Ship", "kind" to "script", "runnable" to true))
        assertEq("Running Ship on the desktop.", store.runNow("f/r", entry))
        assertEq("runShortcut", channel.params("insights.prepare").str("action"))
        assertEq("t", channel.params("insights.run").str("token"))
        assertEq(listOf("insights.prepare", "insights.run", "insights.shortcuts"), channel.methods().take(3))
    }

    fun testQueueAndUnqueueUseTheirOwnActions() {
        val channel = FakeControlChannel()
        channel.reply("insights.prepare", mapOf("token" to "q", "action" to "queueShortcut", "subject" to "f/r|shortcut|ship.sh"))
        channel.reply("insights.prepare", mapOf("token" to "u", "action" to "unqueueShortcut", "subject" to "f/r|shortcut|ship.sh"))
        channel.reply("insights.run", mapOf("queued" to 1L, "message" to "Queued Ship: number 1 in line on the desktop."))
        val store = ShortcutsStore(channel)
        val entry = ShortcutEntry.from(mapOf("file" to "ship.sh", "scope" to "shortcut", "name" to "Ship", "kind" to "script", "runnable" to true))
        assertEq(true, entry.queueable)
        assertEq("Queued Ship: number 1 in line on the desktop.", store.queue("f/r", entry))
        assertEq("queueShortcut", channel.params("insights.prepare").str("action"))
        assertEq("ship.sh", channel.params("insights.prepare").str("file"))
        store.unqueue("f/r", entry)
        assertEq("unqueueShortcut", channel.params("insights.prepare").str("action"))
        assertEq("u", channel.params("insights.run").str("token"))
    }

    fun testQueuedPositionAndBusyRepoComeFromTheCatalog() {
        val channel = FakeControlChannel()
        channel.reply(
            "insights.shortcuts",
            mapOf(
                "repo" to "f/r",
                "repos" to listOf("f/r"),
                "entries" to listOf(
                    mapOf("file" to "build.sh", "scope" to "shortcut", "name" to "Build", "kind" to "script", "runnable" to true, "running" to true),
                    mapOf("file" to "ship.sh", "scope" to "shortcut", "name" to "Ship", "kind" to "script", "runnable" to true, "queued" to 2L),
                    mapOf("file" to "fix.md", "scope" to "shortcut", "name" to "Fix", "kind" to "prompt", "runnable" to true),
                ),
            ),
        )
        val store = ShortcutsStore(channel)
        store.refresh("f/r")
        val entries = store.catalog!!.entries
        assertEq(true, store.repoBusy("f/r"))
        assertEq(2, entries.first { it.file == "ship.sh" }.queued)
        assertEq(false, entries.first { it.file == "fix.md" }.queueable)
    }

    fun testChangedEventRefreshesTheShownRepo() {
        val channel = FakeControlChannel()
        channel.reply("insights.shortcuts", catalogJson)
        val store = ShortcutsStore(channel)
        channel.emit("insights.changed")
        assertEq(0, channel.methods().count { it == "insights.shortcuts" })
        store.refresh("f/r")
        channel.emit("insights.changed")
        assertEq(2, channel.methods().count { it == "insights.shortcuts" })
        store.stop()
        assertEq(0, channel.tapCount)
    }

    fun testOutputEventsStreamIntoTheRun() {
        val channel = FakeControlChannel()
        channel.reply(
            "insights.shortcutRun",
            mapOf("run" to mapOf("id" to 2L, "repo" to "f/r", "file" to "ship.sh", "name" to "Ship", "running" to true, "output" to "\$ bash ship.sh\nstart\n")),
        )
        val store = ShortcutsStore(channel)
        channel.emit("insights.shortcutOutput", mapOf("id" to 2L, "repo" to "f/r", "file" to "ship.sh", "name" to "Ship", "running" to true, "chunk" to "start\n"))
        assertEq("\$ bash ship.sh\nstart\n", store.lastRun?.output)
        channel.emit("insights.shortcutOutput", mapOf("id" to 2L, "repo" to "f/r", "file" to "ship.sh", "running" to true, "chunk" to "building\n"))
        channel.emit("insights.shortcutOutput", mapOf("id" to 2L, "repo" to "f/r", "file" to "ship.sh", "running" to false, "ok" to true, "message" to "done (exit 0)", "chunk" to "done (exit 0)\n"))
        val run = store.lastRun!!
        assertEq("\$ bash ship.sh\nstart\nbuilding\ndone (exit 0)\n", run.output)
        assertEq(false, run.running)
        assertEq(true, run.ok)
        assertEq("done (exit 0)", run.message)
        assertEq(true, run.matches("F/R", "ship.sh"))
        assertEq(1, channel.methods().count { it == "insights.shortcutRun" })
    }

    fun testTwoRunsAtOnceKeepTheirOwnOutput() {
        val channel = FakeControlChannel()
        val store = ShortcutsStore(channel)
        channel.emit("insights.shortcutOutput", mapOf("id" to 3L, "repo" to "f/r", "file" to "adhoc.sh", "running" to true, "chunk" to "ad hoc\n"))
        channel.emit("insights.shortcutOutput", mapOf("id" to 4L, "repo" to "f/r", "file" to "store.sh", "running" to true, "chunk" to "store\n"))
        channel.emit("insights.shortcutOutput", mapOf("id" to 3L, "repo" to "f/r", "file" to "adhoc.sh", "running" to false, "ok" to true, "message" to "done (exit 0)", "chunk" to "done (exit 0)\n"))
        assertEq("ad hoc\ndone (exit 0)\n", store.runs[3]?.output)
        assertEq("store\n", store.runs[4]?.output)
        assertEq(4, store.lastRun?.id)
        assertEq(false, store.isRunning("f/r", "adhoc.sh"))
        assertEq(true, store.isRunning("F/R", "store.sh"))
        assertEq(listOf(3, 4), channel.requests.filter { it.first == "insights.shortcutRun" }.map { it.second["id"] })
        channel.reply(
            "insights.shortcutRun",
            mapOf(
                "run" to mapOf("id" to 4L, "repo" to "f/r", "file" to "store.sh", "running" to true, "output" to "store\nupload\n"),
                "runs" to listOf(mapOf("id" to 4L, "repo" to "f/r", "file" to "store.sh", "running" to true)),
            ),
        )
        store.loadRun()
        assertEq(setOf(4), store.runs.keys)
        assertEq("store\nupload\n", store.lastRun?.output)
    }

    fun testRowStatusShowsTheLiveLineThenTheDesktopsLastOutput() {
        val channel = FakeControlChannel()
        val store = ShortcutsStore(channel)
        val idle = ShortcutEntry.from(mapOf("file" to "ship.sh", "scope" to "shortcut", "kind" to "script", "runnable" to true))
        assertEq(false, store.status("f/r", idle).ran)
        val finished = ShortcutEntry.from(
            mapOf(
                "file" to "ship.sh", "scope" to "shortcut", "kind" to "script", "runnable" to true,
                "lastOutput" to "artefact   ForkMesh.ipa", "lastOk" to true, "finished" to 2L,
            ),
        )
        assertEq(true, finished.isScript)
        assertEq(ShortcutRowStatus(false, true, "artefact   ForkMesh.ipa"), store.status("f/r", finished))
        channel.emit("insights.shortcutOutput", mapOf("id" to 5L, "repo" to "f/r", "file" to "ship.sh", "running" to true, "chunk" to "  > Archive\n"))
        assertEq(ShortcutRowStatus(true, false, "> Archive"), store.status("F/R", finished))
        assertEq("artefact   ForkMesh.ipa", store.status("f/other", finished).line)
        channel.emit("insights.shortcutOutput", mapOf("id" to 5L, "repo" to "f/r", "file" to "ship.sh", "running" to false, "ok" to false, "message" to "failed (exit 1)", "chunk" to "failed (exit 1)\n"))
        assertEq("artefact   ForkMesh.ipa", store.status("f/r", finished).line)
    }

    fun testLastLineSkipsBlanksProgressFramesAndColour() {
        assertEq("two", ShortcutRun.lastLine("one\ntwo\n\n  \n"))
        assertEq("done", ShortcutRun.lastLine("10%\r55%\r\u001B[32mdone\u001B[0m\n"))
        assertEq("abc", ShortcutRun.lastLine("abcdef", maxChars = 3))
        assertEq("", ShortcutRun.lastLine(""))
    }

    fun testRunOutputKeepsWholeLinesOfTheTail() {
        assertEq("a\n", ShortcutRun.tail("a\n"))
        assertEq("last\n", ShortcutRun.tail("x".repeat(ShortcutRun.MAX_OUTPUT) + "\nlast\n"))
    }

    fun testProgressEstimatesFromTheLastRun() {
        assertEq("5s", ShortcutProgress.duration(5_400))
        assertEq("1m 05s", ShortcutProgress.duration(65_000))
        assertEq("1h 05m", ShortcutProgress.duration(3_900_000))
        val midway = ShortcutProgress.of(65_000, 90_000)
        assertEq("1m 05s of about 1m 30s · 25s left", midway.text)
        assertEq(true, kotlin.math.abs(midway.fraction!! - 65f / 90f) < 0.001f)
        val late = ShortcutProgress.of(100_000, 90_000)
        assertEq("1m 40s · 10s longer than the last run", late.text)
        assertEq(0.99f, late.fraction)
        val first = ShortcutProgress.of(12_000, -1)
        assertEq(null, first.fraction)
        assertEq("Running for 12s · no earlier run to estimate from", first.text)
    }

    fun testRowStatusCarriesTheLastDurationAndALiveEstimate() {
        val store = ShortcutsStore(FakeControlChannel())
        val idle = ShortcutEntry.from(
            mapOf("file" to "ship.sh", "scope" to "shortcut", "kind" to "script", "finished" to 1L, "lastOk" to true, "lastMs" to 90_000L),
        )
        assertEq("took 1m 30s", store.status("f/r", idle).tookText)
        assertEq(null, store.status("f/r", idle).progress(1_000_000))
        assertEq(-1L, ShortcutEntry.from(mapOf("file" to "old.sh", "scope" to "shortcut")).lastMs)

        val running = ShortcutEntry.from(
            mapOf(
                "file" to "ship.sh", "scope" to "shortcut", "kind" to "script", "running" to true, "lastMs" to 90_000L,
                "elapsedMs" to 30_000L, "expectedMs" to 90_000L,
            ),
            nowMs = 1_000_000,
        )
        assertEq(970_000L, running.startedAtMs)
        val status = store.status("f/r", running)
        assertEq(null, status.tookText)
        assertEq("1m 05s of about 1m 30s · 25s left", status.progress(1_035_000)?.text)

        var run = ShortcutRun.from(
            mapOf("id" to 7L, "repo" to "f/r", "file" to "ship.sh", "running" to true, "startedAtMs" to 8_000L, "expectedMs" to 4_000L),
            nowMs = 10_000,
        )
        assertEq(0.75f, run.progress(11_000)?.fraction)
        run = run.absorb(mapOf("id" to 7L, "running" to true, "startedAtMs" to 3_000L, "expectedMs" to 4_000L, "chunk" to "x\n"), nowMs = 12_000)
        assertEq(8_000L, run.startedAtMs)
        run = run.absorb(mapOf("id" to 7L, "running" to false, "ok" to true, "message" to "done (exit 0)"), nowMs = 13_000)
        assertEq(null, run.progress(13_000))
    }

    fun testStepsDecodeAndLinkToShortcutsInTheSameScope() {
        val catalog = ShortcutCatalog.from(
            mapOf(
                "repo" to "f/r",
                "entries" to listOf(
                    mapOf(
                        "file" to "deploy.sh", "scope" to "shortcut", "name" to "Deploy new version", "kind" to "script",
                        "steps" to listOf(
                            mapOf("name" to "Cut the new version"),
                            mapOf("name" to " Deploy Android build ", "shortcut" to "deploy-build-android.sh"),
                            mapOf("name" to "Deploy Linux build", "shortcut" to "deploy-build-linux"),
                            mapOf("name" to "Deploy iOS build", "shortcut" to "deploy-build-ios.sh"),
                            mapOf("name" to "Run itself", "shortcut" to "deploy.sh"),
                        ),
                    ),
                    mapOf("file" to "deploy-build-android.sh", "scope" to "shortcut", "name" to "Android", "kind" to "script"),
                    mapOf("file" to "deploy-build-linux.sh", "scope" to "shortcut", "name" to "Linux", "kind" to "script"),
                    mapOf("file" to "deploy-build-ios.sh", "scope" to "repository", "name" to "iOS skill"),
                ),
            ),
        )
        val deploy = catalog.entries.first { it.file == "deploy.sh" }
        assertEq(5, deploy.steps.size)
        assertEq("5 steps", deploy.stepsText)
        assertEq(ShortcutStep("Cut the new version"), deploy.steps[0])
        assertEq(ShortcutStep("Deploy Android build", "deploy-build-android.sh"), deploy.steps[1])
        assertEq(false, deploy.steps[0].linked)
        assertEq(null, catalog.target(deploy, deploy.steps[0]))
        assertEq("deploy-build-android.sh", catalog.target(deploy, deploy.steps[1])?.file)
        assertEq("deploy-build-linux.sh", catalog.target(deploy, deploy.steps[2])?.file)
        assertEq(null, catalog.target(deploy, deploy.steps[3]))
        assertEq(null, catalog.target(deploy, deploy.steps[4]))
        assertEq("Step 2 of 7, Deploy Android build, opens shortcut", deploy.steps[1].spoken(1, 7, true))
        assertEq("Step 1 of 7, Cut the new version", deploy.steps[0].spoken(0, 7, false))
        assertEq("", catalog.entries.first { it.file == "deploy-build-android.sh" }.stepsText)
    }

    fun testStepsTolerateAbsenceAndGarbage() {
        assertEq(emptyList<ShortcutStep>(), ShortcutEntry.from(mapOf("file" to "a.sh", "scope" to "shortcut")).steps)
        assertEq(emptyList<ShortcutStep>(), ShortcutEntry.from(mapOf("file" to "a.sh", "steps" to "Plan, build")).steps)
        assertEq(emptyList<ShortcutStep>(), ShortcutEntry.from(mapOf("file" to "a.sh", "steps" to mapOf("name" to "x"))).steps)
        val messy = ShortcutEntry.from(
            mapOf(
                "file" to "a.sh",
                "steps" to listOf(
                    "Plan",
                    42L,
                    null,
                    mapOf("name" to "   "),
                    mapOf("name" to 7L),
                    mapOf("shortcut" to "b.sh"),
                    mapOf("name" to "Build", "shortcut" to listOf("b.sh")),
                    mapOf("name" to "Check", "shortcut" to "  "),
                ),
            ),
        )
        assertEq(listOf(ShortcutStep("b.sh", "b.sh"), ShortcutStep("Build"), ShortcutStep("Check")), messy.steps)
        assertEq("1 step", ShortcutEntry.from(mapOf("steps" to listOf(mapOf("name" to "Only")))).stepsText)
        val many = ShortcutEntry.from(mapOf("steps" to (1..40).map { mapOf("name" to "Step $it") }))
        assertEq(ShortcutStep.MAX, many.steps.size)
        assertEq("Step 32", many.steps.last().name)
    }
}
