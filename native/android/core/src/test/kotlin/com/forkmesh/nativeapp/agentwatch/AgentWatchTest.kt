package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.agents.AgentAssistantMessage
import com.forkmesh.nativeapp.agents.AgentDone
import com.forkmesh.nativeapp.agents.AgentStopReason
import com.forkmesh.nativeapp.agents.AgentToolCall
import com.forkmesh.nativeapp.agents.AgentToolCallEvent
import com.forkmesh.nativeapp.agents.AgentToolResult
import com.forkmesh.nativeapp.agents.AgentToolResultEvent
import com.forkmesh.nativeapp.agents.AgentUserMessage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.math.floor

internal val sampleTranscript = """
==> Started from a prompt (CC).
==> Preparing an isolated worktree for branch 1-fix${'…'}

> fix the flaky test
please

==> Running Claude Code (stream-json transcript) on branch 1-fix
[Bash git status]

Looking at the failing test first.
It lives in the sync suite.

[Read /src/sync.dart]
[Grep kSlotSeconds]
[Bash python3 - <<'EOF'
p = 'x'
print(p)
EOF]
[Edit /src/sync.dart]
[Bash dart test test/sync_test.dart]
[TodoWrite {"todos":[]}]
[mcp__forkmesh__search_issues {"query":"flaky"}]
[Bash {"command":"sleep 30","run_in_background":true}]

Fixed and committed.

""".trimStart('\n')

internal class RecordingOutput : AgentWatchOutput {
    val played = ArrayList<List<Note>>()
    val felt = ArrayList<List<Pulse>>()
    var stops = 0

    override fun play(notes: List<Note>, now: Double) {
        played += notes
    }

    override fun feel(pulses: List<Pulse>) {
        felt += pulses
    }

    override fun stop() {
        stops += 1
    }
}

private fun near(want: Double, got: Double, tolerance: Double, label: String = "") {
    if (abs(want - got) > tolerance) throw AssertionError("$label expected <$want> but was <$got>")
}

class AgentWatchBeatsTest {
    fun testReadsEachCallPromptStatusLineAndParagraphOnce() {
        val beats = beatsFromTranscript(sampleTranscript)
        assertEq(
            listOf(
                AgentMotion.start, AgentMotion.system, AgentMotion.prompt, AgentMotion.system, AgentMotion.git,
                AgentMotion.speak, AgentMotion.read, AgentMotion.search, AgentMotion.run, AgentMotion.write,
                AgentMotion.test, AgentMotion.todo, AgentMotion.mcp, AgentMotion.spawn, AgentMotion.speak,
            ),
            beats.map { it.motion },
        )
        assertEq("Bash git status", beats[4].text)
        assertEq("fix the flaky test", beats[2].text)
    }

    fun testEarlyReadIsAPrefixOfALaterOne() {
        val all = beatsFromTranscript(sampleTranscript)
        var cut = 0
        while (cut < sampleTranscript.length) {
            val early = beatsFromTranscript(sampleTranscript.substring(0, cut))
            assertEq(all.take(early.size), early, "cut at $cut")
            cut += 7
        }
    }

    fun testALineStillBeingWrittenWaits() {
        expect(beatsFromTranscript("[Bash git sta").isEmpty())
    }

    fun testMotionForToolMatchesTheDesktopOrder() {
        assertEq(AgentMotion.todo, motionForTool("TodoWrite"))
        assertEq(AgentMotion.create, motionForTool("Write"))
        assertEq(AgentMotion.write, motionForTool("Edit"))
        assertEq(AgentMotion.scan, motionForTool("Glob"))
        assertEq(AgentMotion.web, motionForTool("WebFetch"))
        assertEq(AgentMotion.wait, motionForTool("BashOutput"))
        assertEq(AgentMotion.delegate, motionForTool("Agent"))
        assertEq(AgentMotion.skill, motionForTool("Skill"))
        assertEq(AgentMotion.ask, motionForTool("AskUserQuestion"))
        assertEq(AgentMotion.git, motionForTool("git_commit"))
        assertEq(AgentMotion.write, motionForTool("replace_text"))
        assertEq(AgentMotion.scan, motionForTool("list_directory"))
    }

    fun testAShellCommandIsJudgedByWhatItRuns() {
        assertEq(AgentMotion.build, motionForCommand("ninja -C build"))
        assertEq(AgentMotion.install, motionForCommand("npm install"))
        assertEq(AgentMotion.git, motionForCommand("cd x && git log"))
        assertEq(AgentMotion.test, motionForCommand("ctest -R sync"))
        assertEq(AgentMotion.run, motionForCommand("ls -la"))
        assertEq(AgentMotion.spawn, motionForCommand("make", background = true))
    }

    fun testNewBeats() {
        val a = WatchBeat(AgentMotion.read, "a")
        val b = WatchBeat(AgentMotion.run, "b")
        val c = WatchBeat(AgentMotion.write, "c")
        val d = WatchBeat(AgentMotion.git, "d")
        assertEq(listOf(c, d), newBeats(listOf(a, b), listOf(a, b, c, d)))
        assertEq(emptyList<WatchBeat>(), newBeats(listOf(a, b), listOf(a, b)))
        assertEq(listOf(c, d), newBeats(listOf(a, b), listOf(b, c, d)))
        assertEq(listOf(c, d), newBeats(listOf(a), listOf(b, c, d), fallback = 2))
    }

    fun testPlainProseTakesTheMarkdownOff() {
        assertEq("Title one two quoted", plainProse("# Title\n- **one** `two`\n```\ncode\n```\n> quoted"))
    }

    fun testMotionTableIsComplete() {
        assertEq(51, AgentMotion.entries.size)
        assertEq("logInfo", AgentMotion.logInfo.name)
        assertEq("Command", AgentMotion.run.label)
        assertEq(AgentMotion.run, AgentMotion.withLabel("Command"))
        expect(AgentMotion.logSync.isLog && AgentMotion.logSync.isCue)
        expect(AgentMotion.network.isFaint && !AgentMotion.network.isBookkeeping)
        for (motion in AgentMotion.entries) {
            expect(motion.label.isNotEmpty() && motion.description.isNotEmpty() && motion.sound.isNotEmpty())
        }
    }

    fun testOnDeviceEventsBecomeBeats() {
        val beats = beatsFromAgentEvents(
            listOf(
                AgentUserMessage("fix it"),
                AgentToolCallEvent(AgentToolCall("1", "run_command", mapOf("command" to "dart test"))),
                AgentToolResultEvent(AgentToolResult("1", "run_command", "Permission denied", isError = true)),
                AgentAssistantMessage("Done **now**."),
                AgentDone(AgentStopReason.TurnLimit, 1, 0, 0),
            ),
        )
        assertEq(
            listOf(AgentMotion.prompt, AgentMotion.test, AgentMotion.denied, AgentMotion.speak, AgentMotion.finish),
            beats.map { it.motion },
        )
        assertEq("run_command dart test", beats[1].text)
        assertEq("Done now.", beats[3].said)
        assertEq("turnLimit after 1 turn", beats[4].text)
    }
}

class AgentVoiceTest {
    fun testIsTheSameAgentEveryTime() {
        assertEq(Voice.forAgent(281, "forkmesh"), Voice.forAgent(281, "Forkmesh "))
    }

    fun testTellsAFleetApart() {
        val voices = (1..40).map { Voice.forAgent(it, "agent") }
        expect(voices.map { it.instrument }.toSet().size > 3)
        expect(voices.map { it.pan }.toSet().size > 2)
        assertEq(setOf(0.0, 0.5), voices.map { it.slotOffset }.toSet())
    }

    fun testWalksThePentatonicScaleFromD3() {
        val voice = Voice(instrument = Instrument.marimba)
        near(146.832, voice.frequency(0), 1e-9)
        near(1.259921, voice.frequency(2) / voice.frequency(0), 1e-5)
        near(2.0, voice.frequency(5) / voice.frequency(0), 1e-9)
        near(0.5, voice.frequency(-5) / voice.frequency(0), 1e-9)
    }

    fun testTurnsClimbAndTokensFillOut() {
        val voice = Voice()
        voice.applyUsage(13, 400_000)
        assertEq(2, voice.drift)
        near(1.0, voice.bloom, 1e-9)
        voice.applyUsage(600, 0)
        assertEq(5, voice.drift)
        assertEq(0.0, voice.bloom)
    }

    fun testNotesLandOnTheSharedGrid() {
        for (offset in listOf(0.0, 0.5)) {
            val voice = Voice(slotOffset = offset)
            val melody = Melody()
            var now = 0.07
            repeat(6) {
                val note = noteForBeat(AgentMotion.read, voice, melody, now)
                val slots = note.start / kSlotSeconds - offset
                near(floor(slots + 0.5), slots, 1e-9)
                expect(note.start >= now)
                now += 0.31
            }
        }
    }

    fun testABurstBecomesGraceNotes() {
        val voice = Voice()
        val melody = Melody()
        val notes = (0 until 30).map { noteForBeat(AgentMotion.run, voice, melody, 1.0) }
        expect(notes.any { it.seconds == 0.1 })
        expect(melody.count < 30)
    }

    fun testRootsAndTheFailure() {
        val voice = Voice()
        for (motion in listOf(AgentMotion.start, AgentMotion.prompt, AgentMotion.finish)) {
            near(voice.frequency(0), noteForBeat(motion, voice, Melody(), 0.0).frequency, 1e-9)
        }
        near(voice.frequency(-2), noteForBeat(AgentMotion.fail, voice, Melody(), 0.0).frequency, 1e-9)
    }

    fun testCadences() {
        val voice = Voice()
        val done = doneCadence(voice, true, 1.0)
        assertEq(7, done.size)
        near(voice.frequency(0), done.last().frequency, 1e-9)
        assertEq(1.0, done.first().start)
        assertEq(6, doneCadence(voice, false, 0.0).size)
    }

    fun testEveryKindSoundsAndStaysInRange() {
        for (motion in AgentMotion.entries) {
            val voice = Voice.forAgent(motion.ordinal, motion.name)
            val note = noteForBeat(motion, voice, Melody(), 0.5, 400, 12345)
            expect(!note.isRest, motion.name)
            val pcm = renderNotes(listOf(note), 0.5, echo = false)
            assertEq(0, pcm.size % 2)
            val peak = pcm.maxOf { abs(it.toInt()) }
            expect(peak > 30, "${motion.name} is silent")
            expect(peak < 30001, "${motion.name} clips")
        }
    }

    fun testALaterNoteStartsLaterAndEchoes() {
        val note = Note(start = 1.3, frequency = 440.0, seconds = 0.1, gain = 0.3, timbre = Timbre.ping)
        val pcm = renderNotes(listOf(note), 1.0)
        fun at(seconds: Double) = floor(seconds * kWatchSampleRate + 0.5).toInt() * 2
        val lead = at(0.3)
        expect((0 until lead - 4).all { pcm[it].toInt() == 0 })
        expect((lead until lead + 2000).any { pcm[it].toInt() != 0 })
        val gone = at(0.3 + 0.35)
        expect((gone until gone + 2000).all { abs(pcm[it].toInt()) < 20 })
        val echoAt = at(0.3 + 0.45 + 0.02)
        expect((echoAt until echoAt + 4000).any { abs(pcm[it].toInt()) > 20 })
    }

    fun testEveryKindCanBeFeltBriefly() {
        for (motion in AgentMotion.entries) {
            val pulses = feelFor(motion, Voice.forAgent(3, "x"))
            expect(pulses.isNotEmpty(), motion.name)
            for (pulse in pulses) {
                expect(pulse.intensity in 0.0..1.0 && pulse.sharpness in 0.0..1.0)
                expect(pulse.atMs + pulse.ms <= 500)
            }
        }
    }
}

class AgentWatchControllerTest {
    fun testReplaysAFinishedRunAndClosesItWithACadence() {
        val output = RecordingOutput()
        val clock = ManualAgentWatchClock()
        val controller = AgentWatchController(
            listOf(AgentWatchTarget("a", 1, "fix", project = "forkmesh") {
                AgentWatchSnapshot("done", beatsFromTranscript(sampleTranscript))
            }),
            output,
            clock,
        )
        controller.start()
        clock.advance(10.0)
        controller.advance()
        val beats = beatsFromTranscript(sampleTranscript).size
        assertEq(beats + 2, output.played.size)
        assertEq(7, output.played.last().size)
        val agent = controller.agents[0]
        assertEq(AgentMotion.finish, agent.last?.motion)
        expect(controller.lastSounded[AgentMotion.test] != null)
        assertEq(1, controller.landCount[AgentMotion.test])
        assertEq("Fixed and committed.", agent.said)
        expect((controller.kindsOf(agent).firstOrNull()?.second ?: 0) > 1)
        expect(output.felt.isNotEmpty())
        for (notes in output.played) {
            val slots = notes[0].start / (kSlotSeconds / 4)
            near(floor(slots + 0.5), slots, 1e-6)
        }
        controller.dispose()
    }

    fun testFollowsALiveRun() {
        var text = sampleTranscript
        var status = "running"
        var reads = 0
        val output = RecordingOutput()
        val clock = ManualAgentWatchClock()
        val controller = AgentWatchController(
            listOf(AgentWatchTarget("a", 2, "fix") {
                reads += 1
                AgentWatchSnapshot(status, beatsFromTranscript(text))
            }),
            output,
            clock,
            debounce = 0.1,
        )
        controller.start()
        clock.advance(5.0)
        assertEq(1 + AgentWatchController.CATCH_UP, output.played.size)

        text += "[Bash ninja -C build]\n"
        controller.ring()
        clock.advance(0.1)
        clock.advance(2.0)
        assertEq(2 + AgentWatchController.CATCH_UP, output.played.size)
        assertEq(AgentMotion.build, controller.agents[0].last?.motion)

        status = "failed"
        controller.ring()
        clock.advance(0.1)
        clock.advance(2.0)
        assertEq(AgentMotion.fail, controller.agents[0].last?.motion)
        assertEq(WatchState.failed, controller.agents[0].state)
        assertEq(6, output.played.last().size)

        val before = reads
        controller.ring()
        clock.advance(1.0)
        assertEq(before, reads)
        controller.dispose()
    }

    fun testSeatsAnAgentThatStartsWhileWatching() {
        val output = RecordingOutput()
        val clock = ManualAgentWatchClock()
        val first = AgentWatchTarget("a", 1, "fix") { AgentWatchSnapshot("running", beatsFromTranscript(sampleTranscript)) }
        val controller = AgentWatchController(listOf(first), output, clock, debounce = 0.1)
        controller.start()
        clock.advance(5.0)
        val before = output.played.size

        assertEq(0, controller.add(listOf(first)))
        val second = AgentWatchTarget("b", 2, "docs") { AgentWatchSnapshot("running", beatsFromTranscript("[Read /README.md]\n")) }
        assertEq(1, controller.add(listOf(first, second)))
        clock.advance(5.0)
        assertEq(listOf("a", "b"), controller.agents.map { it.target.key })
        expect(controller.agents[1].loaded)
        assertEq(AgentMotion.read, controller.agents[1].last?.motion)
        assertEq(before + 2, output.played.size)
        controller.dispose()
        assertEq(0, controller.add(listOf(AgentWatchTarget("c", 3, "late") { AgentWatchSnapshot("running", emptyList()) })))
    }

    fun testSoundKindsAndFeelSwitchOffIndependently() {
        val output = RecordingOutput()
        val clock = ManualAgentWatchClock()
        val controller = AgentWatchController(
            listOf(AgentWatchTarget("a", 3, "fix") {
                AgentWatchSnapshot("done", listOf(WatchBeat(AgentMotion.read, "a"), WatchBeat(AgentMotion.run, "b")))
            }),
            output,
            clock,
            sound = false,
        )
        controller.setMuted(AgentMotion.run, true)
        controller.start()
        clock.advance(3.0)
        controller.advance()
        expect(output.played.isEmpty())
        assertEq(1, controller.landCount[AgentMotion.read])
        assertEq(3, output.felt.size)
        controller.sound = true
        assertEq(1, output.played.size)
        controller.feel = false
        controller.greet(controller.agents[0])
        assertEq(3, output.felt.size)
        controller.dispose()
        expect(output.stops > 0)
    }

    fun testALoadErrorIsShownOnTheAgent() {
        val controller = AgentWatchController(
            listOf(AgentWatchTarget("a", 4, "fix") { throw AgentWatchSealedException() }),
            RecordingOutput(),
            ManualAgentWatchClock(),
        )
        controller.start()
        val agent = controller.agents[0]
        expect(agent.loaded)
        assertEq("Sealed for another owner device.", agent.error)
        controller.dispose()
    }

    fun testSeatsAndStates() {
        assertEq(WatchState.waiting, watchStateFor("waiting_for_input"))
        assertEq(WatchState.starting, watchStateFor(""))
        assertEq(WatchState.done, watchStateFor("merged"))
        assertEq(WatchBand.resting, bandFor(WatchState.failed))
        val target = AgentWatchTarget("k", 1, "t", subtitle = "owner/name") { AgentWatchSnapshot("", emptyList()) }
        assertEq("name", target.projectName)
        val controller = AgentWatchController(
            listOf(target, AgentWatchTarget("k2", 1, "t", subtitle = "owner/name", load = target.load)),
            RecordingOutput(),
            ManualAgentWatchClock(),
        )
        val a = controller.agents[0].voice
        val b = controller.agents[1].voice
        expect(!(a.instrument == b.instrument && (a.octave == b.octave || a.pan == b.pan)))
        controller.dispose()
    }
}

class AgentWatchLayoutTest {
    private fun agent(id: Int, status: String): WatchedAgent {
        val target = AgentWatchTarget("$id", id, "t$id") { AgentWatchSnapshot("", emptyList()) }
        return WatchedAgent(target, Voice.forAgent(id, "p")).also { it.setState(watchStateFor(status)) }
    }

    fun testSeatsEachBandInItsOwnRowsFewerToARowOnAPhone() {
        val agents = (1..4).map { agent(it, "running") } + (5..14).map { agent(it, "done") }
        val phone = WatchFieldLayout(390.0, 1400.0, agents)
        assertEq(3, WatchFieldLayout.rowSizeFor(390.0))
        assertEq(setOf(WatchBand.working, WatchBand.resting), phone.rows.map { it.band }.toSet())
        assertEq(4, phone.rows.first().count)
        expect(phone.pointFor(agents[0]).y < phone.pointFor(agents.last()).y)
        val desktop = WatchFieldLayout(1600.0, 1000.0, agents)
        assertEq(3, desktop.rows.size)
        expect(WatchFieldLayout.heightFor(390.0, agents) > 800)
        expect(phone.agentAt(phone.pointFor(agents[0]), 0.0) === agents[0])
        expect(phone.agentAt(WatchPoint(-500.0, -500.0), 0.0) == null)
    }

    fun testTheFaceIsTheOneTheDesktopDrawsForTheModel() {
        assertEq("opus", watchFaceName("claude-code", "claude-opus-5-5"))
        assertEq("feather", watchFaceName("claude-code", ""))
        assertEq("sun", watchFaceName("codex", "gpt-5"))
        assertEq("globe-leaf", watchFaceName("", ""))
    }

    fun testCardLinesSayWhoItIs() {
        val target = AgentWatchTarget("a", 7, "#7 - Sound the agents", subtitle = "forkmesh/forkmesh") {
            AgentWatchSnapshot("done", emptyList())
        }
        val controller = AgentWatchController(listOf(target), SilentAgentWatchOutput(), ManualAgentWatchClock())
        val agent = controller.agents[0]
        agent.setState(WatchState.done)
        val lines = watchCardLines(controller, agent)
        assertEq("#7 forkmesh · Sound the agents", lines[0])
        expect(lines[1].startsWith("done · playing as "))
        assertEq("double tap to stop hearing it", lines.last())
        controller.dispose()
    }
}
