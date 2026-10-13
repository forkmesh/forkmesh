// The engine half of the agent watch; the screen's tests need SwiftUI and live with the app.

import XCTest

@testable import FMAgentWatch

/// A desktop transcript.txt in the shape AgentStore writes it.
let sampleTranscript = """
    ==> Started from a prompt (CC).
    ==> Preparing an isolated worktree for branch 1-fix\u{2026}

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
    [Bash flutter test test/sync_test.dart]
    [TodoWrite {"todos":[]}]
    [mcp__forkmesh__search_issues {"query":"flaky"}]
    [Bash {"command":"sleep 30","run_in_background":true}]

    Fixed and committed.

    """

@MainActor
final class RecordingOutput: AgentWatchOutput {
    var played: [[Note]] = []
    var felt: [[Pulse]] = []
    var stops = 0

    func play(_ notes: [Note], now: Double) { played.append(notes) }
    func feel(_ pulses: [Pulse]) { felt.append(pulses) }
    func stop() { stops += 1 }
}

final class AgentWatchBeatsTests: XCTestCase {
    func testReadsEachCallPromptStatusLineAndParagraphOnce() {
        let beats = beatsFromTranscript(sampleTranscript)
        XCTAssertEqual(beats.map(\.motion), [
            .start, .system, .prompt, .system, .git, .speak, .read, .search,
            .run,  // the multi-line heredoc is one call
            .write, .test, .todo, .mcp, .spawn, .speak,
        ])
        XCTAssertEqual(beats[4].text, "Bash git status")
        XCTAssertEqual(beats[2].text, "fix the flaky test")
    }

    func testEarlyReadIsAPrefixOfALaterOne() {
        let all = beatsFromTranscript(sampleTranscript)
        let units = Array(sampleTranscript.utf16)
        var cut = 0
        while cut < units.count {
            let early = beatsFromTranscript(String(decoding: units[0..<cut], as: UTF16.self))
            XCTAssertEqual(Array(all.prefix(early.count)), early, "cut at \(cut)")
            cut += 7
        }
    }

    func testALineStillBeingWrittenWaits() {
        XCTAssertTrue(beatsFromTranscript("[Bash git sta").isEmpty)
    }

    func testMotionForToolMatchesTheDesktopOrder() {
        XCTAssertEqual(motionForTool("TodoWrite"), .todo)
        XCTAssertEqual(motionForTool("Write"), .create)
        XCTAssertEqual(motionForTool("Edit"), .write)
        XCTAssertEqual(motionForTool("Glob"), .scan)
        XCTAssertEqual(motionForTool("WebFetch"), .web)
        XCTAssertEqual(motionForTool("BashOutput"), .wait)
        XCTAssertEqual(motionForTool("Agent"), .delegate)
        XCTAssertEqual(motionForTool("Skill"), .skill)
        XCTAssertEqual(motionForTool("AskUserQuestion"), .ask)
        XCTAssertEqual(motionForTool("git_commit"), .git)
        XCTAssertEqual(motionForTool("replace_text"), .write)
        XCTAssertEqual(motionForTool("list_directory"), .scan)
    }

    func testAShellCommandIsJudgedByWhatItRuns() {
        XCTAssertEqual(motionForCommand("ninja -C build"), .build)
        XCTAssertEqual(motionForCommand("npm install"), .install)
        XCTAssertEqual(motionForCommand("cd x && git log"), .git)
        XCTAssertEqual(motionForCommand("ctest -R sync"), .test)
        XCTAssertEqual(motionForCommand("ls -la"), .run)
        XCTAssertEqual(motionForCommand("make", background: true), .spawn)
    }

    func testNewBeats() {
        let a = WatchBeat(.read, "a")
        let b = WatchBeat(.run, "b")
        let c = WatchBeat(.write, "c")
        let d = WatchBeat(.git, "d")
        XCTAssertEqual(newBeats([a, b], [a, b, c, d]), [c, d])
        XCTAssertEqual(newBeats([a, b], [a, b]), [])
        XCTAssertEqual(newBeats([a, b], [b, c, d]), [c, d])
        XCTAssertEqual(newBeats([a], [b, c, d], fallback: 2), [c, d])
    }

    func testPlainProseTakesTheMarkdownOff() {
        XCTAssertEqual(plainProse("# Title\n- **one** `two`\n```\ncode\n```\n> quoted"), "Title one two quoted")
    }

    func testMotionTableIsComplete() {
        XCTAssertEqual(AgentMotion.allCases.count, 51)
        XCTAssertEqual(AgentMotion.logInfo.name, "logInfo")
        XCTAssertEqual(AgentMotion.run.label, "Command")
        XCTAssertEqual(AgentMotion.withLabel("Command"), .run)
        XCTAssertTrue(AgentMotion.logSync.isLog && AgentMotion.logSync.isCue)
        XCTAssertTrue(AgentMotion.network.isFaint && !AgentMotion.network.isBookkeeping)
        for motion in AgentMotion.allCases {
            XCTAssertFalse(motion.label.isEmpty)
            XCTAssertFalse(motion.description.isEmpty)
            XCTAssertFalse(motion.sound.isEmpty)
        }
    }
}

final class AgentVoiceTests: XCTestCase {
    func testIsTheSameAgentEveryTime() {
        let one = Voice.forAgent(281, "forkmesh")
        let two = Voice.forAgent(281, "Forkmesh ")
        XCTAssertEqual(two, one)
    }

    func testTellsAFleetApart() {
        let voices = (1...40).map { Voice.forAgent($0, "agent") }
        XCTAssertGreaterThan(Set(voices.map(\.instrument)).count, 3)
        XCTAssertGreaterThan(Set(voices.map(\.pan)).count, 2)
        XCTAssertEqual(Set(voices.map(\.slotOffset)), [0.0, 0.5])
    }

    func testWalksThePentatonicScaleFromD3() {
        let voice = Voice(instrument: .marimba)
        XCTAssertEqual(voice.frequency(0), 146.832, accuracy: 1e-9)
        XCTAssertEqual(voice.frequency(2) / voice.frequency(0), 1.259921, accuracy: 1e-5)
        XCTAssertEqual(voice.frequency(5) / voice.frequency(0), 2, accuracy: 1e-9)
        XCTAssertEqual(voice.frequency(-5) / voice.frequency(0), 0.5, accuracy: 1e-9)
    }

    func testTurnsClimbAndTokensFillOut() {
        var voice = Voice()
        voice.applyUsage(turns: 13, tokens: 400_000)
        XCTAssertEqual(voice.drift, 2)
        XCTAssertEqual(voice.bloom, 1, accuracy: 1e-9)
        voice.applyUsage(turns: 600, tokens: 0)
        XCTAssertEqual(voice.drift, 5)
        XCTAssertEqual(voice.bloom, 0)
    }

    func testNotesLandOnTheSharedGrid() {
        for offset in [0.0, 0.5] {
            let voice = Voice(slotOffset: offset)
            var melody = Melody()
            var now = 0.07
            for _ in 0..<6 {
                let note = noteForBeat(.read, voice, &melody, now)
                let slots = note.start / kSlotSeconds - offset
                XCTAssertEqual(slots, slots.rounded(), accuracy: 1e-9)
                XCTAssertGreaterThanOrEqual(note.start, now)
                now += 0.31
            }
        }
    }

    func testABurstBecomesGraceNotes() {
        let voice = Voice()
        var melody = Melody()
        let notes = (0..<30).map { _ in noteForBeat(.run, voice, &melody, 1.0) }
        XCTAssertGreaterThan(notes.filter { $0.seconds == 0.1 }.count, 0)
        XCTAssertLessThan(melody.count, 30)
    }

    func testRootsAndTheFailure() {
        let voice = Voice()
        for motion: AgentMotion in [.start, .prompt, .finish] {
            var melody = Melody()
            XCTAssertEqual(noteForBeat(motion, voice, &melody, 0).frequency, voice.frequency(0), accuracy: 1e-9)
        }
        var melody = Melody()
        XCTAssertEqual(noteForBeat(.fail, voice, &melody, 0).frequency, voice.frequency(-2), accuracy: 1e-9)
    }

    func testCadences() {
        let voice = Voice()
        let done = doneCadence(voice, true, 1.0)
        XCTAssertEqual(done.count, 7)
        XCTAssertEqual(done.last!.frequency, voice.frequency(0), accuracy: 1e-9)
        XCTAssertEqual(done.first!.start, 1.0)
        XCTAssertEqual(doneCadence(voice, false, 0).count, 6)
    }

    func testEveryKindSoundsAndStaysInRange() {
        for motion in AgentMotion.allCases {
            let voice = Voice.forAgent(motion.rawValue, motion.name)
            var melody = Melody()
            let note = noteForBeat(motion, voice, &melody, 0.5, bytes: 400, seed: 12345)
            XCTAssertFalse(note.isRest, motion.name)
            let pcm = renderNotes([note], from: 0.5, echo: false)
            XCTAssertEqual(pcm.count % 2, 0)
            let peak = pcm.map { abs(Int($0)) }.max() ?? 0
            XCTAssertGreaterThan(peak, 30, "\(motion.name) is silent")
            XCTAssertLessThan(peak, 30001, "\(motion.name) clips")
        }
    }

    func testALaterNoteStartsLaterAndEchoes() {
        let note = Note(start: 1.3, frequency: 440, seconds: 0.1, gain: 0.3, timbre: .ping)
        let pcm = renderNotes([note], from: 1.0)
        let lead = Int((0.3 * Double(kWatchSampleRate)).rounded()) * 2
        XCTAssertTrue(pcm[0..<(lead - 4)].allSatisfy { $0 == 0 })
        XCTAssertTrue(pcm[lead..<(lead + 2000)].contains { $0 != 0 })
        let gone = Int(((0.3 + 0.35) * Double(kWatchSampleRate)).rounded()) * 2
        XCTAssertTrue(pcm[gone..<(gone + 2000)].allSatisfy { abs(Int($0)) < 20 })
        let echoAt = Int(((0.3 + 0.45 + 0.02) * Double(kWatchSampleRate)).rounded()) * 2
        XCTAssertTrue(pcm[echoAt..<(echoAt + 4000)].contains { abs(Int($0)) > 20 })
    }

    func testEveryKindCanBeFeltBriefly() {
        for motion in AgentMotion.allCases {
            let pulses = feelFor(motion, Voice.forAgent(3, "x"))
            XCTAssertFalse(pulses.isEmpty, motion.name)
            for pulse in pulses {
                XCTAssertTrue((0...1).contains(pulse.intensity))
                XCTAssertTrue((0...1).contains(pulse.sharpness))
                XCTAssertLessThanOrEqual(pulse.atMs + pulse.ms, 500)
            }
        }
        XCTAssertEqual(WatchHapticKind.for(.fail), .failure)
        XCTAssertEqual(WatchHapticKind.for(.finish), .success)
        XCTAssertNil(WatchHapticKind.for(.frame))
    }
}

@MainActor
final class AgentWatchControllerTests: XCTestCase {
    func testReplaysAFinishedRunAndClosesItWithACadence() async {
        let output = RecordingOutput()
        let clock = ManualAgentWatchClock()
        let controller = AgentWatchController(
            targets: [
                AgentWatchTarget(key: "a", id: 1, title: "fix", project: "forkmesh") {
                    AgentWatchSnapshot(status: "done", beats: beatsFromTranscript(sampleTranscript))
                },
            ],
            output: output, clock: clock)
        await controller.start()
        clock.advance(by: 10)
        controller.advance()
        let beats = beatsFromTranscript(sampleTranscript).count
        XCTAssertEqual(output.played.count, beats + 2)
        XCTAssertEqual(output.played.last?.count, 7)
        let agent = controller.agents[0]
        XCTAssertEqual(agent.last?.motion, .finish)
        XCTAssertNotNil(controller.lastSounded[.test])
        XCTAssertEqual(controller.landCount[.test], 1)
        XCTAssertEqual(agent.said, "Fixed and committed.")
        XCTAssertGreaterThan(controller.kindsOf(agent).first?.1 ?? 0, 1)
        XCTAssertFalse(output.felt.isEmpty)
        for notes in output.played {
            let slots = notes[0].start / (kSlotSeconds / 4)
            XCTAssertEqual(slots, slots.rounded(), accuracy: 1e-6)
        }
        controller.dispose()
    }

    func testFollowsALiveRun() async {
        final class Run: @unchecked Sendable {
            var text = sampleTranscript
            var status = "running"
            var reads = 0
        }
        let run = Run()
        let output = RecordingOutput()
        let clock = ManualAgentWatchClock()
        let controller = AgentWatchController(
            targets: [
                AgentWatchTarget(key: "a", id: 2, title: "fix") {
                    run.reads += 1
                    return AgentWatchSnapshot(status: run.status, beats: beatsFromTranscript(run.text))
                },
            ],
            output: output, clock: clock, debounce: 0.1)
        await controller.start()
        clock.advance(by: 5)
        XCTAssertEqual(output.played.count, 1 + AgentWatchController.catchUp)

        run.text += "[Bash ninja -C build]\n"
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        clock.advance(by: 2)
        XCTAssertEqual(output.played.count, 2 + AgentWatchController.catchUp)
        XCTAssertEqual(controller.agents[0].last?.motion, .build)

        run.status = "failed"
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        clock.advance(by: 2)
        XCTAssertEqual(controller.agents[0].last?.motion, .fail)
        XCTAssertEqual(controller.agents[0].state, .failed)
        XCTAssertEqual(output.played.last?.count, 6)

        // Finished runs are not read again.
        let before = run.reads
        controller.ring()
        clock.advance(by: 1)
        await controller.settle()
        XCTAssertEqual(run.reads, before)
        controller.dispose()
    }

    func testAFinishedRunListedAgainIsReadAgain() async {
        final class Run: @unchecked Sendable {
            var text = sampleTranscript
            var status = "done"
            var reads = 0
        }
        let run = Run()
        let clock = ManualAgentWatchClock()
        let target = AgentWatchTarget(key: "a", id: 2, title: "fix") {
            run.reads += 1
            return AgentWatchSnapshot(status: run.status, beats: beatsFromTranscript(run.text))
        }
        let controller = AgentWatchController(
            targets: [target], output: RecordingOutput(), clock: clock, debounce: 0.1)
        await controller.start()
        clock.advance(by: 5)
        XCTAssertEqual(controller.agents[0].state, .done)
        XCTAssertEqual(run.reads, 1)

        run.status = "running"
        run.text += "[Bash ninja -C build]\n"
        XCTAssertEqual(controller.add([target]), 0)
        let revision = controller.revision
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        XCTAssertEqual(run.reads, 2)
        XCTAssertEqual(controller.agents[0].state, .running)
        XCTAssertGreaterThan(controller.revision, revision)

        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        XCTAssertEqual(run.reads, 3)

        run.status = "done"
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        XCTAssertEqual(controller.agents[0].state, .done)
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        XCTAssertEqual(run.reads, 4)
        controller.dispose()
    }

    func testSeatsAnAgentThatStartsWhileWatching() async {
        let output = RecordingOutput()
        let clock = ManualAgentWatchClock()
        let first = AgentWatchTarget(key: "a", id: 1, title: "fix") {
            AgentWatchSnapshot(status: "running", beats: beatsFromTranscript(sampleTranscript))
        }
        let controller = AgentWatchController(targets: [first], output: output, clock: clock, debounce: 0.1)
        await controller.start()
        clock.advance(by: 5)
        let before = output.played.count

        XCTAssertEqual(controller.add([first]), 0)
        let second = AgentWatchTarget(key: "b", id: 2, title: "docs") {
            AgentWatchSnapshot(status: "running", beats: beatsFromTranscript("[Read /README.md]\n"))
        }
        XCTAssertEqual(controller.add([first, second]), 1)
        await controller.settle()
        clock.advance(by: 5)
        XCTAssertEqual(controller.agents.map(\.target.key), ["a", "b"])
        XCTAssertTrue(controller.agents[1].loaded)
        XCTAssertEqual(controller.agents[1].last?.motion, .read)
        XCTAssertEqual(output.played.count, before + 2)
        controller.dispose()
        XCTAssertEqual(controller.add([AgentWatchTarget(key: "c", id: 3, title: "late") {
            AgentWatchSnapshot(status: "running", beats: [])
        }]), 0)
    }

    func testSoundKindsAndFeelSwitchOffIndependently() async {
        let output = RecordingOutput()
        let clock = ManualAgentWatchClock()
        let controller = AgentWatchController(
            targets: [
                AgentWatchTarget(key: "a", id: 3, title: "fix") {
                    AgentWatchSnapshot(status: "done", beats: [WatchBeat(.read, "a"), WatchBeat(.run, "b")])
                },
            ],
            output: output, clock: clock, sound: false)
        controller.setMuted(.run, true)
        await controller.start()
        clock.advance(by: 3)
        controller.advance()
        XCTAssertTrue(output.played.isEmpty)
        XCTAssertEqual(controller.landCount[.read], 1)
        XCTAssertEqual(output.felt.count, 3)  // the read, the run, the cadence

        controller.sound = true
        XCTAssertEqual(output.played.count, 1)  // its seat, announced
        controller.feel = false
        controller.greet(controller.agents[0])
        XCTAssertEqual(output.felt.count, 3)
        controller.dispose()
        XCTAssertGreaterThan(output.stops, 0)
    }

    func testSeatsAndStates() async {
        XCTAssertEqual(watchStateFor("waiting_for_input"), .waiting)
        XCTAssertEqual(watchStateFor(""), .starting)
        XCTAssertEqual(watchStateFor("merged"), .done)
        XCTAssertEqual(bandFor(.failed), .resting)
        let target = AgentWatchTarget(key: "k", id: 1, title: "t", subtitle: "owner/name") {
            AgentWatchSnapshot(status: "", beats: [])
        }
        XCTAssertEqual(target.projectName, "name")
        let controller = AgentWatchController(
            targets: [target, AgentWatchTarget(key: "k2", id: 1, title: "t", subtitle: "owner/name", load: target.load)],
            output: RecordingOutput(), clock: ManualAgentWatchClock())
        let a = controller.agents[0].voice
        let b = controller.agents[1].voice
        XCTAssertFalse(a.instrument == b.instrument && (a.octave == b.octave || a.pan == b.pan))
        controller.dispose()
    }
}
