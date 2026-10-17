import FMBase
import Foundation
import XCTest

@testable import FMPackages

final class DjPartyTests: XCTestCase {
    func testThirtyDistinctReactionsCoverEveryStyle() {
        XCTAssertEqual(DjReaction.all.count, 30)
        XCTAssertEqual(Set(DjReaction.all.map(\.id)).count, 30)
        XCTAssertEqual(Set(DjReaction.all.map(\.emoji)).count, 30)
        XCTAssertEqual(Set(DjReaction.all.map(\.style)), Set(DjReactionStyle.allCases))
        for reaction in DjReaction.all {
            XCTAssertNotNil(reaction.id.range(of: "^[a-z0-9-]{1,24}$", options: .regularExpression), reaction.id)
            XCTAssertGreaterThan(reaction.seconds, 1)
        }
        XCTAssertEqual(DjReaction.find("fire")?.emoji, "🔥")
        XCTAssertNil(DjReaction.find("nope"))
    }

    func testJamParsesPresenceReactionsAndStatus() throws {
        let object: JSONObject = [
            "jam": [
                "id": "j1", "me": "bob", "myStatus": "joined", "now": 50_000,
                "members": [["name": "alice", "status": "owner"], ["name": "bob", "status": "joined"], ["name": "cara", "status": "joined"],
                            ["name": "dan", "status": "joined"], ["name": "eve", "status": "invited"]],
                "presence": [
                    ["name": "alice", "state": "studio", "playing": true, "energy": 0.8, "motion": [[0.5, -0.5], [9, 1], ["x", 1], [1]], "ago": 400],
                    ["name": "bob", "state": "listening", "playing": true, "ago": 1_000],
                    ["name": "cara", "state": "studio", "ago": 60_000],
                    ["state": "studio"],
                ],
                "reactions": [["id": "r1", "name": "alice", "kind": "hearts", "ago": 300], ["id": "r2", "kind": "made-up"], ["kind": "fire"]],
            ],
        ]
        let jam = try XCTUnwrap(DjJam.parse(object))
        XCTAssertEqual(jam.presence.map(\.name), ["alice", "bob", "cara"])
        let alice = try XCTUnwrap(jam.presence(of: "alice"))
        XCTAssertTrue(alice.dancing)
        XCTAssertEqual(alice.motion, [DjMotionSample(x: 0.5, y: -0.5), DjMotionSample(x: 2, y: 1)])
        XCTAssertEqual(jam.reactions, [DjJamReaction(id: "r1", name: "alice", kind: "hearts", ago: 300)])
        XCTAssertEqual(jam.members.map(jam.status(of:)), [.studio, .listening, .away, .away, .invited])
        XCTAssertTrue(DjPersonStatus.studio.live)
        XCTAssertFalse(DjPersonStatus.away.live)
        var later = jam
        later.now = 99
        later.presence = []
        later.reactions = []
        XCTAssertEqual(later.shared, jam.shared)
        later.queue = [DjQueueItem(id: "q", videoId: "dQw4w9WgXcQ", title: "", author: "")]
        XCTAssertNotEqual(later.shared, jam.shared)
    }

    func testMotionIsDownsampledMeasuredAndReplayed() {
        let still = Array(repeating: DjMotionSample.still, count: 40)
        XCTAssertEqual(DjMotion.energy(still), 0)
        XCTAssertEqual(DjMotion.energy([]), 0)
        let shaking = (0..<40).map { DjMotionSample(x: $0 % 2 == 0 ? 0.9 : -0.9, y: 0.2) }
        XCTAssertGreaterThan(DjMotion.energy(shaking), DjMotion.dancing)
        let shared = DjMotion.downsample(shaking)
        XCTAssertEqual(shared.count, DjMotion.shared)
        XCTAssertTrue(shared.allSatisfy { abs($0.x) == 0.9 })
        XCTAssertEqual(DjMotion.downsample([DjMotionSample(x: 5, y: -5)]), [DjMotionSample(x: 2, y: -2)])
        let path = [DjMotionSample(x: 0, y: 0), DjMotionSample(x: 1, y: -1)]
        XCTAssertEqual(DjMotion.replay(path, progress: 0.5).x, 0.5, accuracy: 1e-9)
        XCTAssertEqual(DjMotion.replay(path, progress: 1.5).y, -0.5, accuracy: 1e-9)
        XCTAssertEqual(DjMotion.replay([], progress: 0.3), .still)
        let body = DjJamPresence.body(state: .studio, playing: true, motion: shaking)
        XCTAssertEqual(body["state"]?.stringValue, "studio")
        XCTAssertEqual(body["motion"]?.arrayValue?.count, DjMotion.shared)
        XCTAssertGreaterThan(body["energy"]?.doubleValue ?? 0, 0)
    }

    func testAutomixStartsPreloadsAndMixesThroughTheQueue() {
        let empty = DjAutomixDeck(loaded: false)
        let cued = DjAutomixDeck(loaded: true, duration: 200)
        XCTAssertEqual(DjAutomix.step(a: empty, b: empty, favoured: .a, queued: false), .wait)
        XCTAssertEqual(DjAutomix.step(a: empty, b: empty, favoured: .a, queued: true), .preload(.a))
        XCTAssertEqual(DjAutomix.step(a: empty, b: cued, favoured: .a, queued: false), .start(.b))
        let early = DjAutomixDeck(loaded: true, playing: true, position: 20, duration: 200)
        XCTAssertEqual(DjAutomix.step(a: early, b: empty, favoured: .a, queued: true), .preload(.b))
        XCTAssertEqual(DjAutomix.step(a: early, b: empty, favoured: .a, queued: false), .wait)
        XCTAssertEqual(DjAutomix.step(a: early, b: cued, favoured: .a, queued: true), .wait)
        let ending = DjAutomixDeck(loaded: true, playing: true, position: 194, duration: 200)
        XCTAssertEqual(DjAutomix.step(a: ending, b: cued, favoured: .a, queued: false), .mix(from: .a, to: .b, seconds: 6))
        let fast = DjAutomixDeck(loaded: true, playing: true, position: 180, duration: 200, rate: 2.5)
        XCTAssertEqual(DjAutomix.step(a: fast, b: cued, favoured: .a, queued: false), .mix(from: .a, to: .b, seconds: 8))
        let spent = DjAutomixDeck(loaded: true, position: 199, duration: 200)
        XCTAssertFalse(spent.fresh)
        XCTAssertEqual(DjAutomix.step(a: spent, b: early, favoured: .b, queued: true), .preload(.a))
        XCTAssertEqual(DjAutomix.step(a: spent, b: empty, favoured: .a, queued: false), .wait)
        XCTAssertEqual(DjAutomix.step(a: early, b: ending, favoured: .b, queued: true), .wait)
    }

    func testAutomixReplaysPlayedSongsOnceTheQueueRunsOut() {
        let library = [
            DjPlayedSong(id: "aaaaaaaaaaa", title: "Newest", author: "", plays: 3, last: 500),
            DjPlayedSong(id: "bbbbbbbbbbb", title: "Newer", author: "", plays: 1, last: 400),
            DjPlayedSong(id: "ccccccccccc", title: "Older", author: "", plays: 2, last: 200),
            DjPlayedSong(id: "ddddddddddd", title: "Oldest", author: "", plays: 1, last: 100),
        ]
        var picked = Set<String>()
        for seed in 0..<64 as Range<UInt64> {
            let song = DjAutomix.next(library, loaded: ["aaaaaaaaaaa"], playing: ["aaaaaaaaaaa"], seed: seed)
            picked.insert(song?.id ?? "")
        }
        XCTAssertEqual(picked, ["ccccccccccc", "ddddddddddd"])
        XCTAssertEqual(DjAutomix.next(library, loaded: [], playing: [], seed: 1), DjAutomix.next(library, loaded: [], playing: [], seed: 1))
        let one = [library[0]]
        XCTAssertNil(DjAutomix.next(one, loaded: ["aaaaaaaaaaa"], playing: ["aaaaaaaaaaa"], seed: 3))
        XCTAssertEqual(DjAutomix.next(one, loaded: ["aaaaaaaaaaa"], playing: [], seed: 3)?.id, "aaaaaaaaaaa")
        XCTAssertNil(DjAutomix.next([], loaded: [], playing: [], seed: 3))
        let spent = DjAutomixDeck(loaded: true, position: 199, duration: 200)
        XCTAssertEqual(DjAutomix.step(a: spent, b: spent, favoured: .b, queued: true), .preload(.b))
        let early = DjAutomixDeck(loaded: true, playing: true, position: 20, duration: 200)
        XCTAssertEqual(DjAutomix.step(a: early, b: spent, favoured: .a, queued: true), .preload(.b))
        let failed = DjAutomixDeck(loaded: false)
        XCTAssertEqual(DjAutomix.step(a: early, b: failed, favoured: .a, queued: true), .preload(.b))
        let ending = DjAutomixDeck(loaded: true, playing: true, position: 191, duration: 200)
        let cued = DjAutomixDeck(loaded: true, duration: 180)
        XCTAssertEqual(DjAutomix.step(a: ending, b: cued, favoured: .a, queued: true), .mix(from: .a, to: .b, seconds: 8))
    }
}
