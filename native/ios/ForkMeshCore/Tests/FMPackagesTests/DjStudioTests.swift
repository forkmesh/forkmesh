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

    func testWaveformIsStablePerVideo() {
        let first = DjStudio.waveform("dQw4w9WgXcQ", count: 200)
        XCTAssertEqual(first.count, 200)
        XCTAssertEqual(first, DjStudio.waveform("dQw4w9WgXcQ", count: 200))
        XCTAssertNotEqual(first, DjStudio.waveform("9bZkp7q19f0", count: 200))
        XCTAssertTrue(first.allSatisfy { (0...1).contains($0.level) && (0..<1).contains($0.hue) })
        XCTAssertTrue(DjStudio.waveform("", count: 200).isEmpty)
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
        let data = Data(#"{"a":{"videoId":"dQw4w9WgXcQ","cue":-4,"level":7,"rate":9,"bpm":"x","loop":true},"b":{"videoId":"nope"},"crossfader":3,"samplerLevel":-1,"sections":[{"deck":"b","start":12,"duration":90},null,null,null,{"deck":"a","start":1,"duration":2}],"zoomed":true}"#.utf8)
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
        XCTAssertTrue(saved.zoomed)
        XCTAssertEqual(DjSaved.decode(saved.encoded()), saved)
        XCTAssertEqual(DjSaved.decode(Data("nonsense".utf8)), DjSaved())
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
