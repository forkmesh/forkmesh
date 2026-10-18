import FMAgentWatch
import Foundation

@MainActor
public final class TranscriptBeatCache {
    public typealias Parse = @Sendable (String) -> [WireBeat]

    private struct Entry {
        let length: Int
        let beats: [WireBeat]
    }

    private let parse: Parse
    private var entries: [String: Entry] = [:]
    private var generation = 0
    public private(set) var parses = 0

    public nonisolated static func wireBeats(_ transcript: String) -> [WireBeat] {
        beatsFromTranscript(transcript).map(WireBeat.init)
    }

    public init(parse: @escaping Parse = TranscriptBeatCache.wireBeats) {
        self.parse = parse
    }

    public func cached(_ key: String) -> [WireBeat]? {
        entries[key]?.beats
    }

    public func beats(_ key: String, transcript: String) async -> [WireBeat] {
        let length = transcript.utf8.count
        if let entry = entries[key], entry.length == length { return entry.beats }
        let parse = self.parse
        let started = generation
        parses += 1
        let beats = await Task.detached(priority: .utility) { parse(transcript) }.value
        if started == generation { entries[key] = Entry(length: length, beats: beats) }
        return beats
    }

    public func removeAll() {
        generation += 1
        entries.removeAll()
    }
}
