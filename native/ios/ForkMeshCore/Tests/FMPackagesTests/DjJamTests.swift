import FMBase
import Foundation
import XCTest

@testable import FMPackages

final class DjJamTests: XCTestCase {
    func testJamParsesMembersQueueAndDecks() throws {
        let object: JSONObject = [
            "ok": true,
            "jam": [
                "id": "j1", "title": "Friday", "owner": "alice", "me": "bob", "myStatus": "joined", "now": 10_000,
                "members": [["name": "alice", "status": "owner"], ["name": "bob", "status": "joined"], ["name": "cara", "status": "invited"], ["status": "x"]],
                "queue": [
                    ["id": "q1", "videoId": "dQw4w9WgXcQ", "title": "Song", "author": "Band", "addedBy": "bob"],
                    ["id": "q2", "videoId": "bad"],
                ],
                "decks": [
                    "a": ["videoId": "9bZkp7q19f0", "title": "Other", "playing": true, "position": 30, "rate": 1.1, "at": 8_000, "by": "alice"],
                    "b": ["videoId": "nope", "rate": 9],
                ],
            ],
        ]
        let jam = try XCTUnwrap(DjJam.parse(object))
        XCTAssertEqual(jam.id, "j1")
        XCTAssertFalse(jam.isOwner)
        XCTAssertTrue(jam.joined)
        XCTAssertEqual(jam.members.map(\.name), ["alice", "bob", "cara"])
        XCTAssertEqual(jam.listeners.map(\.name), ["alice", "bob"])
        XCTAssertEqual(jam.queue, [DjQueueItem(id: "q1", videoId: "dQw4w9WgXcQ", title: "Song", author: "Band", addedBy: "bob")])
        XCTAssertEqual(jam[.a].videoId, "9bZkp7q19f0")
        XCTAssertEqual(jam[.a].expectedPosition(serverNow: jam.now), 32.2, accuracy: 1e-9)
        XCTAssertEqual(jam[.b].videoId, "")
        XCTAssertEqual(jam[.b].rate, 1 + DjStudio.pitchRange)
        XCTAssertEqual(jam[.b].expectedPosition(serverNow: 99_000), 0)
        XCTAssertNil(DjJam.parse(["ok": true]))
    }

    func testJamListAndSeekThreshold() {
        let list = DjJamSummary.list(["jams": [["id": "j1", "title": "T", "owner": "alice", "myStatus": "invited", "members": 3], ["title": "no id"]]])
        XCTAssertEqual(list, [DjJamSummary(id: "j1", title: "T", owner: "alice", myStatus: "invited", members: 3)])
        XCTAssertTrue(list[0].invited)
        XCTAssertFalse(DjJam.needsSeek(local: 10, remote: 11, playing: true))
        XCTAssertTrue(DjJam.needsSeek(local: 10, remote: 12, playing: true))
        XCTAssertTrue(DjJam.needsSeek(local: 10, remote: 10.8, playing: false))
        XCTAssertFalse(DjJam.needsSeek(local: .nan, remote: 10, playing: false))
    }

    func testDeckBodyRoundsPosition() {
        let body = DjJamDeck(videoId: "dQw4w9WgXcQ", title: "Song", playing: true, position: 12.34567, rate: 1.02).body
        XCTAssertEqual(body["position"]?.doubleValue, 12.346)
        XCTAssertEqual(body["playing"]?.boolValue, true)
        XCTAssertEqual(body["videoId"]?.stringValue, "dQw4w9WgXcQ")
    }

    func testQueueItemsDecodeSafely() throws {
        let items = try JSONDecoder().decode([DjQueueEntry].self, from: Data(#"[{"id":"a","videoId":"dQw4w9WgXcQ","title":"Song"},{"videoId":"bad"}]"#.utf8))
        XCTAssertEqual(items.compactMap(\.item).map(\.videoId), ["dQw4w9WgXcQ"])
    }

    func testWaveformFollowsLoudnessAndBrightness() {
        let rate = 22_050.0
        var quiet = [Float](repeating: 0, count: 22_050)
        var bass = [Float](repeating: 0, count: 22_050)
        var treble = [Float](repeating: 0, count: 22_050)
        for index in 0..<22_050 {
            let time = Double(index) / rate
            quiet[index] = Float(0.05 * sin(2 * Double.pi * 60 * time))
            bass[index] = Float(0.8 * sin(2 * Double.pi * 60 * time))
            treble[index] = Float(0.8 * sin(2 * Double.pi * 7000 * time))
        }
        var accumulator = DjWaveAccumulator(sampleRate: rate, totalSamples: 66_150, bars: 3)
        accumulator.add(quiet)
        accumulator.add(bass)
        accumulator.add(treble)
        let bars = accumulator.finish(bars: 3)
        XCTAssertEqual(bars.count, 3)
        XCTAssertLessThan(bars[0].level, 0.2)
        XCTAssertGreaterThan(bars[1].level, 0.8)
        XCTAssertLessThan(bars[1].hue, 0.15)
        XCTAssertGreaterThan(bars[2].hue, 0.45)
        XCTAssertEqual(DjWaveCache.decode(DjWaveCache.encode(bars)).count, 3)
        XCTAssertEqual(DjWaveCache.decode(DjWaveCache.encode(bars))[1].level, bars[1].level, accuracy: 0.01)
        var silent = DjWaveAccumulator(sampleRate: rate, totalSamples: 100)
        silent.add([Float](repeating: 0, count: 100))
        XCTAssertTrue(silent.finish().isEmpty)
        XCTAssertTrue(DjWaveCache.decode(Data([1])).isEmpty)
    }
}
