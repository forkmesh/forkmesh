// The Swift engine against recorded golden outputs (AgentWatchGoldens.swift):
// the same beats, voices, notes, touches and PCM for the same input.

import XCTest

@testable import FMAgentWatch

final class AgentWatchGoldenTests: XCTestCase {
    func testTranscriptBeatsMatchDart() {
        let beats = beatsFromTranscript(Goldens.transcript)
        XCTAssertEqual(beats.count, Goldens.beats.count)
        for (beat, want) in zip(beats, Goldens.beats) {
            XCTAssertEqual(beat.motion.name, want.0, want.1)
            XCTAssertEqual(beat.text, want.1)
            XCTAssertEqual(beat.bytes, want.2, want.1)
            XCTAssertEqual(beat.said, want.3, want.1)
            XCTAssertEqual(beat.seed, want.4, want.1)
        }
    }

    func testToolsAndCommandsMatchDart() {
        for (tool, want) in Goldens.tools {
            XCTAssertEqual(motionForTool(tool).name, want, tool)
        }
        for (command, want) in Goldens.commands {
            XCTAssertEqual(motionForCommand(command).name, want, command)
        }
    }

    func testVoicesMatchDart() {
        for want in Goldens.voices {
            let voice = Voice.forAgent(want.0, want.1, salt: want.2)
            let label = "\(want.0) \(want.1) \(want.2)"
            XCTAssertEqual(voice.instrument.rawValue, want.3, label)
            XCTAssertEqual(voice.octave, want.4, label)
            XCTAssertEqual(voice.degreeOffset, want.5, label)
            XCTAssertEqual(voice.pan, want.6, label)
            XCTAssertEqual(voice.slotOffset, want.7, label)
            XCTAssertEqual(voice.detune, want.8, accuracy: 1e-15, label)
            XCTAssertEqual(voice.hue, want.9, accuracy: 1e-15, label)
            XCTAssertEqual(voice.description, want.10, label)
            for (k, frequency) in want.11.enumerated() {
                XCTAssertEqual(voice.frequency(k - 3), frequency, accuracy: frequency * 1e-13, label)
            }
        }
    }

    func testMelodyMatchesDart() {
        var voice = Voice.forAgent(Goldens.melodyVoice.id, Goldens.melodyVoice.title)
        voice.applyUsage(turns: Goldens.melodyVoice.turns, tokens: Goldens.melodyVoice.tokens)
        var melody = Melody()
        var notes: [Note] = []
        notes += announce(voice, &melody, 0.02)
        var now = 0.5
        for (i, motion) in AgentMotion.allCases.enumerated() {
            let bytes = (i * 137) % 9000
            let seed = i % 2 == 0 ? 0 : fnv1a("\(motion.name)|\(i)")
            notes.append(noteForBeat(motion, voice, &melody, now, bytes: bytes, seed: seed))
            now += i % 3 == 0 ? 0.31 : 0.07
        }
        for k in 0..<20 {
            notes.append(noteForBeat(.run, voice, &melody, now, bytes: k * 50, seed: k))
        }
        notes += doneCadence(voice, true, 30.0)
        notes += doneCadence(voice, false, 33.0)

        XCTAssertEqual(notes.count, Goldens.melodyNotes.count)
        for (i, (got, golden)) in zip(notes, Goldens.melodyNotes).enumerated() {
            let want = golden.note
            let label = "note \(i)"
            XCTAssertEqual(got.start, want.start, accuracy: 1e-12, label)
            XCTAssertEqual(got.frequency, want.frequency, accuracy: max(1e-12, want.frequency * 1e-13), label)
            XCTAssertEqual(got.seconds, want.seconds, accuracy: 1e-12, label)
            XCTAssertEqual(got.gain, want.gain, accuracy: 1e-12, label)
            XCTAssertEqual(got.pan, want.pan, accuracy: 1e-12, label)
            XCTAssertEqual(got.timbre, want.timbre, label)
            XCTAssertEqual(got.instrument, want.instrument, label)
            XCTAssertEqual(got.bloom, want.bloom, accuracy: 1e-12, label)
            XCTAssertEqual(got.seed, want.seed, label)
        }
        let end = Goldens.melodyEnd
        XCTAssertEqual(melody.degree, end.degree)
        XCTAssertEqual(melody.lastSlot, end.lastSlot, accuracy: 1e-9)
        XCTAssertEqual(melody.lastGrace, end.lastGrace, accuracy: 1e-9)
        XCTAssertEqual(melody.count, end.count)
        XCTAssertEqual(melody.graceCount, end.graceCount)
        XCTAssertEqual(melody.graceRun, end.graceRun)
    }

    func testFeelMatchesDart() {
        let voice = Voice.forAgent(3, "x")
        for (name, want) in Goldens.feel {
            let got = feelFor(motionNamed(name), voice)
            XCTAssertEqual(got.count, want.count, name)
            for (pulse, w) in zip(got, want) {
                XCTAssertEqual(pulse.atMs, w.0, name)
                XCTAssertEqual(pulse.ms, w.1, name)
                XCTAssertEqual(pulse.intensity, w.2, accuracy: 1e-12, name)
                XCTAssertEqual(pulse.sharpness, w.3, accuracy: 1e-12, name)
            }
        }
    }

    func testPcmMatchesDart() {
        for (i, want) in Goldens.pcm.enumerated() {
            let pcm = renderNotes(want.notes.map(\.note), from: want.from, echo: want.echo)
            XCTAssertEqual(pcm.count, want.count, "render \(i)")
            let sum = pcm.reduce(0) { $0 + abs(Int($1)) }
            // A last-bit libm difference may move a sample by one step.
            XCTAssertEqual(Double(sum), Double(want.sum), accuracy: Double(pcm.count), "render \(i)")
            var k = 0
            for sample in want.samples {
                XCTAssertEqual(Int(pcm[k]), sample, accuracy: 2, "render \(i) sample \(k)")
                k += 997
            }
        }
    }
}
