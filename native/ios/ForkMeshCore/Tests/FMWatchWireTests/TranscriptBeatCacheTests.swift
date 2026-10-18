import FMAgentWatch
import Foundation
import XCTest

@testable import FMWatchWire

@MainActor
final class TranscriptBeatCacheTests: XCTestCase {
    private final class Seen: @unchecked Sendable {
        private let lock = NSLock()
        private var threads: [Bool] = []

        func add(_ onMain: Bool) {
            lock.lock()
            threads.append(onMain)
            lock.unlock()
        }

        var all: [Bool] {
            lock.lock()
            defer { lock.unlock() }
            return threads
        }
    }

    func testTranscriptsAreParsedOffTheMainThreadOncePerLength() async {
        let seen = Seen()
        let cache = TranscriptBeatCache { (text: String) -> [WireBeat] in
            seen.add(Thread.isMainThread)
            return TranscriptBeatCache.wireBeats(text)
        }
        let transcript = "> fix the list\n\nReading the code.\n\n[Read AgentsScreen.swift]\n"
        let first = await cache.beats("desktop:repo:7", transcript: transcript)
        XCTAssertEqual(first, TranscriptBeatCache.wireBeats(transcript))
        XCTAssertEqual(seen.all, [false], "a whole transcript is parsed on a background thread")

        _ = await cache.beats("desktop:repo:7", transcript: transcript)
        XCTAssertEqual(cache.parses, 1, "a transcript that has not grown is not parsed again")
        XCTAssertEqual(cache.cached("desktop:repo:7"), first)

        let grown = transcript + "Fixed it.\n\n"
        let second = await cache.beats("desktop:repo:7", transcript: grown)
        XCTAssertEqual(cache.parses, 2)
        XCTAssertEqual(second, TranscriptBeatCache.wireBeats(grown))
        XCTAssertEqual(seen.all, [false, false])

        cache.removeAll()
        XCTAssertNil(cache.cached("desktop:repo:7"))
    }
}
