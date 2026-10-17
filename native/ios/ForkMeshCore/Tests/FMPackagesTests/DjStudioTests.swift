import Foundation
import XCTest

@testable import FMPackages

final class DjStudioTests: XCTestCase {
    func testVideoIdAcceptsCommonYouTubeForms() {
        let id = "dQw4w9WgXcQ"
        let forms = [
            id, "<\(id)>", "https://www.youtube.com/watch?v=\(id)&t=42s", "youtube.com/watch?feature=share&v=\(id)",
            "https://youtu.be/\(id)?si=abc", "youtu.be/\(id)", "https://m.youtube.com/shorts/\(id)",
            "https://music.youtube.com/watch?v=\(id)&list=RD", "https://www.youtube-nocookie.com/embed/\(id)?start=3",
            "https://www.youtube.com/live/\(id)", "https://www.youtube.com/attribution_link?u=/watch%3Fv%3D\(id)%26feature%3Dshare",
            "https://www.youtube.com/watch#?v=\(id)", "Listen: https://youtu.be/\(id) now",
        ]
        for form in forms { XCTAssertEqual(DjStudio.videoId(form), id, form) }
        for bad in ["", "hello", "https://example.com/watch?v=\(id)", "https://youtu.be/short", "dQw4w9WgXc!"] {
            XCTAssertEqual(DjStudio.videoId(bad), "", bad)
        }
    }

    func testSharedVideoIdsTakeOnlyYouTubeShares() {
        let id = "dQw4w9WgXcQ"
        let other = "9bZkp7q19f0"
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: ["https://youtu.be/\(id)?si=abc"], text: "Never Gonna Give You Up"), [id])
        XCTAssertEqual(
            DjStudio.sharedVideoIds(urls: ["https://www.youtube.com/watch?v=\(id)", "https://youtu.be/\(other)", "https://youtu.be/\(id)"], text: ""),
            [id, other])
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: [], text: " https://youtu.be/\(id)\n"), [id])
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: ["https://youtu.be/\(id)", "https://example.com/page"], text: ""), [])
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: [], text: "Summarize https://youtu.be/\(id)"), [])
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: [], text: id), [])
        XCTAssertEqual(DjStudio.sharedVideoIds(urls: [], text: ""), [])
    }

    func testEqualPowerCrossfader() {
        let left = DjStudio.gains(crossfader: -1)
        XCTAssertEqual(left.a, 1, accuracy: 1e-9)
        XCTAssertEqual(left.b, 0)
        let right = DjStudio.gains(crossfader: 1)
        XCTAssertEqual(right.a, 0)
        XCTAssertEqual(right.b, 1, accuracy: 1e-9)
        let centre = DjStudio.gains(crossfader: 0)
        XCTAssertEqual(centre.a * centre.a + centre.b * centre.b, 1, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.gain(.b, crossfader: 5), 1, accuracy: 1e-9)
    }

    func testClockAndRates() {
        XCTAssertEqual(DjStudio.clock(48.9), "00:48")
        XCTAssertEqual(DjStudio.clock(3725), "1:02:05")
        XCTAssertEqual(DjStudio.clock(.nan), "00:00")
        XCTAssertEqual(DjStudio.clampRate(3), 1 + DjStudio.pitchRange)
        XCTAssertEqual(DjStudio.syncedRate(ownBpm: 120, otherBpm: 126, otherRate: 1), 1.05, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.syncedRate(ownBpm: 0, otherBpm: 126, otherRate: 1.02), 1.02, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.scratchSeconds(fromRadians: 2 * Double.pi), DjStudio.secondsPerTurn, accuracy: 1e-9)
    }

    func testScratchSpeedFollowsTheHand() {
        let forward = DjStudio.scratchSpeed(previous: 0, seconds: 0.1, elapsed: 0.1)
        XCTAssertEqual(forward, 0.6, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.scratchSpeed(previous: forward, seconds: 0.1, elapsed: 0.1), 0.84, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.scratchSpeed(previous: 0, seconds: 5, elapsed: 0.001), 4.8, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.scratchSpeed(previous: 1, seconds: 0.1, elapsed: 0), 1)
        XCTAssertEqual(DjStudio.scratchLevel(-5), 1)
        XCTAssertEqual(DjStudio.scratchLevel(5), 0.5)
        XCTAssertEqual(DjStudio.scratchLevel(0), 0)
        XCTAssertEqual(DjStudio.scratchLevel(.nan), 0)
    }

    func testOnlyADeliberateDragScratches() {
        XCTAssertFalse(DjStudio.isScratch(dx: 3, dy: 4))
        XCTAssertFalse(DjStudio.isScratch(dx: 8, dy: -8))
        XCTAssertTrue(DjStudio.isScratch(dx: 0, dy: DjStudio.scratchThreshold))
        XCTAssertTrue(DjStudio.isScratch(dx: -12, dy: 9))
        XCTAssertFalse(DjStudio.isScratch(dx: .nan, dy: 40))
    }

    func testSampleSectionRunsFromStartToStop() {
        XCTAssertEqual(DjStudio.sampleSection(.a, start: 10, end: 14.5), DjSection(deck: .a, start: 10, duration: 4.5))
        XCTAssertEqual(DjStudio.sampleSection(.b, start: 10, end: 10.6), DjSection(deck: .b, start: 10, duration: 1))
        XCTAssertEqual(DjStudio.sampleSection(.b, start: 0, end: 90)?.duration, 30)
        XCTAssertNil(DjStudio.sampleSection(.a, start: 10, end: 10.1))
        XCTAssertNil(DjStudio.sampleSection(.a, start: 10, end: 4))
        XCTAssertNil(DjStudio.sampleSection(.a, start: .nan, end: 4))
        XCTAssertEqual(DjLoadTarget(.b).side, .b)
        XCTAssertNil(DjLoadTarget.queue.side)
    }

    func testTrackedPositionSmoothsSmallDriftAndSnapsLargeJumps() {
        XCTAssertEqual(DjStudio.trackedPosition(predicted: 10, reported: 10.1, continuous: true), 10.035, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.trackedPosition(predicted: 10, reported: 12, continuous: true), 12)
        XCTAssertEqual(DjStudio.trackedPosition(predicted: 10, reported: 10.1, continuous: false), 10.1)
        XCTAssertEqual(DjStudio.trackedPosition(predicted: 10, reported: .nan, continuous: true), 10)
    }

    func testAmplitudeFollowsThePlayheadSmoothly() {
        let bars = [DjWaveBar(level: 0.2, hue: 0.1), DjWaveBar(level: 1, hue: 0.5), DjWaveBar(level: 0.4, hue: 0.9)]
        XCTAssertEqual(DjStudio.amplitude(bars, time: 50, duration: 300), bars[0])
        XCTAssertEqual(DjStudio.amplitude(bars, time: 150, duration: 300), bars[1])
        let between = DjStudio.amplitude(bars, time: 100, duration: 300)
        XCTAssertEqual(between.level, 0.6, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.amplitude(bars, time: 300, duration: 300).level, 0.4, accuracy: 1e-9)
        XCTAssertEqual(DjStudio.amplitude(bars, time: 301, duration: 300).level, 0)
        XCTAssertEqual(DjStudio.amplitude([], time: 5, duration: 10).level, 0)
        XCTAssertEqual(DjStudio.amplitude(bars, time: .nan, duration: 10).level, 0)
    }

    func testSpinRampsUpAndDownWithoutJumps() {
        var spin = DjSpin()
        XCTAssertEqual(spin.step(target: 10, speed: 1, at: 0), 10)
        spin.hold(10, at: 0)
        var now = 0.0
        var shown = 10.0
        var target = 10.0
        for frame in 1...30 {
            now += 1.0 / 60
            if frame > 6 { target += 1.0 / 60 }
            let next = spin.step(target: target, speed: frame > 6 ? 1 : DjSpin.waiting, at: now)
            XCTAssertGreaterThanOrEqual(next, shown)
            XCTAssertLessThan(next - shown, 0.03)
            shown = next
        }
        XCTAssertEqual(shown, target, accuracy: 0.03)
        for _ in 1...60 {
            now += 1.0 / 60
            let next = spin.step(target: target, speed: 0, at: now)
            XCTAssertLessThan(abs(next - shown), 0.03)
            shown = next
        }
        XCTAssertEqual(shown, target, accuracy: 0.01)
        spin.hold(target, at: now)
        XCTAssertEqual(spin.step(target: target, speed: 1, at: now + 5), target)
        XCTAssertGreaterThan(spin.step(target: target, speed: 1, at: now + 5.016), target)
        XCTAssertEqual(spin.step(target: 42, speed: 1, at: now + 5.03), 42)
        XCTAssertEqual(spin.step(target: 42, speed: 1, at: now + 5.03), 42)
        XCTAssertEqual(spin.step(target: .nan, speed: 1, at: now + 5.05), 42)
    }

    func testWaveBarsFollowTheSongLength() {
        XCTAssertEqual(DjWaveAccumulator.bars(seconds: 30), DjWaveAccumulator.barCount)
        XCTAssertEqual(DjWaveAccumulator.bars(seconds: 240), 4800)
        XCTAssertEqual(DjWaveAccumulator.bars(seconds: 7200), DjWaveAccumulator.mostBars)
        XCTAssertEqual(DjWaveAccumulator.bars(seconds: .nan), DjWaveAccumulator.barCount)
    }

    func testWaveRangesSplitTheStream() throws {
        let url = try XCTUnwrap(URL(string: "https://rr1.googlevideo.com/videoplayback?itag=18&clen=5000001&dur=212.4"))
        XCTAssertEqual(DjWaveRanges.declaredLength(url), 5_000_001)
        XCTAssertNil(DjWaveRanges.declaredLength(try XCTUnwrap(URL(string: "https://example.com/a.mp4"))))
        XCTAssertEqual(DjWaveRanges.total(contentRange: "bytes 0-2097151/5000001"), 5_000_001)
        XCTAssertNil(DjWaveRanges.total(contentRange: "bytes 0-10/*"))
        XCTAssertNil(DjWaveRanges.total(contentRange: nil))
        let spans = DjWaveRanges.spans(total: 5_000_001, from: 2_097_152)
        XCTAssertEqual(spans, [2_097_152...4_194_303, 4_194_304...5_000_000])
        XCTAssertEqual(spans.map(\.count).reduce(0, +), 5_000_001 - 2_097_152)
        XCTAssertTrue(DjWaveRanges.spans(total: 10, from: 10).isEmpty)
        XCTAssertEqual(DjWaveRanges.header(0...2_097_151), "bytes=0-2097151")
    }

    func testTapTempo() {
        var tempo = DjTapTempo()
        XCTAssertNil(tempo.tap(at: 10))
        XCTAssertNil(tempo.tap(at: 10.5))
        XCTAssertEqual(tempo.tap(at: 11), 120)
        XCTAssertEqual(tempo.tap(at: 11.5), 120)
        XCTAssertNil(tempo.tap(at: 20))
        XCTAssertEqual(tempo.taps, [20])
    }

    func testSearchParsing() throws {
        let instances = Data(#"[["a",{"api":false,"uri":"https://a.example"}],["b",{"api":true,"uri":"http://b.example"}],["c",{"api":true,"uri":"https://c.example/"}]]"#.utf8)
        XCTAssertEqual(DjStudio.searchInstances(instances), [URL(string: "https://c.example")!])
        XCTAssertEqual(DjStudio.searchInstances(Data("{}".utf8)), [])
        let url = try XCTUnwrap(DjStudio.searchURL(URL(string: "https://c.example")!, query: "deep house & mix"))
        XCTAssertEqual(url.absoluteString, "https://c.example/api/v1/search?q=deep%20house%20%26%20mix&type=video")
        let results = DjStudio.searchResults(Data(#"[{"videoId":"dQw4w9WgXcQ","title":"Song","author":"Band","lengthSeconds":213},{"videoId":"dQw4w9WgXcQ"},{"videoId":"bad"},{"url":"/watch?v=9bZkp7q19f0","title":""}]"#.utf8))
        XCTAssertEqual(results, [
            DjSearchResult(id: "dQw4w9WgXcQ", title: "Song", author: "Band", seconds: 213),
            DjSearchResult(id: "9bZkp7q19f0", title: "Untitled video", author: ""),
        ])
    }

    func testSavedStateIsSanitised() throws {
        let data = Data(#"{"a":{"videoId":"dQw4w9WgXcQ","cue":-4,"level":7,"rate":9,"bpm":"x","loop":true},"b":{"videoId":"nope"},"crossfader":3,"samplerLevel":-1,"sections":[{"deck":"b","start":12,"duration":90},null,null,null,{"deck":"a","start":1,"duration":2}]}"#.utf8)
        let saved = DjSaved.decode(data)
        XCTAssertEqual(saved.a.videoId, "dQw4w9WgXcQ")
        XCTAssertEqual(saved.a.cue, 0)
        XCTAssertEqual(saved.a.level, 1)
        XCTAssertEqual(saved.a.rate, 1 + DjStudio.pitchRange)
        XCTAssertEqual(saved.a.bpm, 0)
        XCTAssertTrue(saved.a.loop)
        XCTAssertEqual(saved.b.videoId, "")
        XCTAssertEqual(saved.crossfader, 1)
        XCTAssertEqual(saved.samplerLevel, 0)
        XCTAssertEqual(saved.sections.count, DjStudio.sectionCount)
        XCTAssertEqual(saved.sections[0], DjSection(deck: .b, start: 12, duration: 30))
        XCTAssertEqual(DjSaved.decode(saved.encoded()), saved)
        XCTAssertEqual(DjSaved.decode(Data("nonsense".utf8)), DjSaved())
    }

    func testSavedPlaybackResumesWhereItWas() {
        let old = DjSaved.decode(Data(#"{"a":{"videoId":"dQw4w9WgXcQ","cue":42}}"#.utf8))
        XCTAssertEqual(old.a.position, 42)
        XCTAssertFalse(old.playing)
        XCTAssertFalse(old.automix)
        let saved = DjSaved.decode(Data(#"{"a":{"videoId":"dQw4w9WgXcQ","cue":5,"position":-3,"playing":true},"b":{"position":80,"playing":true},"automix":true}"#.utf8))
        XCTAssertEqual(saved.a.position, 0)
        XCTAssertTrue(saved.a.playing)
        XCTAssertEqual(saved.b.position, 80)
        XCTAssertFalse(saved.b.playing)
        XCTAssertTrue(saved.playing)
        XCTAssertTrue(saved.automix)
        var playing = DjSaved()
        playing.b.videoId = "9bZkp7q19f0"
        playing.b.position = 61.5
        playing.b.playing = true
        XCTAssertEqual(DjSaved.decode(playing.encoded()), playing)
    }

    func testHistoryKeepsLatestFirstWithoutDuplicates() {
        var history = DjStudio.remember([], id: "dQw4w9WgXcQ", title: "Song", author: "Band")
        history = DjStudio.remember(history, id: "9bZkp7q19f0", title: "Other", author: "")
        history = DjStudio.remember(history, id: "dQw4w9WgXcQ", title: "", author: "")
        XCTAssertEqual(history, [
            DjHistoryItem(id: "dQw4w9WgXcQ", title: "Song", author: "Band"),
            DjHistoryItem(id: "9bZkp7q19f0", title: "Other", author: ""),
        ])
        XCTAssertEqual(DjStudio.remember(history, id: "bad", title: "x", author: ""), history)
        XCTAssertEqual(DjStudio.remember(history, id: "9bZkp7q19f0", title: "New", author: "", limit: 1), [DjHistoryItem(id: "9bZkp7q19f0", title: "New", author: "")])
        let saved = DjSaved.decode(Data(#"{"history":[{"id":"dQw4w9WgXcQ","title":"Song"},{"id":"nope"},{"id":"dQw4w9WgXcQ","title":"Again"},{"title":"x"}]}"#.utf8))
        XCTAssertEqual(saved.history, [DjHistoryItem(id: "dQw4w9WgXcQ", title: "Song", author: "")])
        let queued = DjSaved.decode(Data(#"{"queue":[{"id":"q1","videoId":"dQw4w9WgXcQ"},{"id":"q1","videoId":"9bZkp7q19f0"},{"id":"q2","videoId":"x"}]}"#.utf8))
        XCTAssertEqual(queued.queue.map(\.videoId), ["dQw4w9WgXcQ"])
        XCTAssertEqual(DjSaved.decode(queued.encoded()), queued)
        XCTAssertEqual(DjSaved.decode(saved.encoded()), saved)
    }

    func testDrumPadsAreAudibleAndBounded() {
        for pad in DjDrumPad.allCases {
            let samples = DjDrumSynth.samples(pad, sampleRate: 22_050)
            XCTAssertFalse(samples.isEmpty, pad.rawValue)
            XCTAssertTrue(samples.allSatisfy { $0.isFinite && abs($0) <= 1 }, pad.rawValue)
            XCTAssertGreaterThan(samples.map { abs($0) }.max() ?? 0, 0.05, pad.rawValue)
            XCTAssertLessThan(abs(samples.last ?? 1), 0.01, pad.rawValue)
        }
        XCTAssertEqual(DjDrumSynth.samples(.snare, sampleRate: 8000), DjDrumSynth.samples(.snare, sampleRate: 8000))
    }
}
