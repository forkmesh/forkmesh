import FMApi
import FMBase
import Foundation

public final class NewPullsMarker: @unchecked Sendable {
    public static let key = "shell/pulls-seen"
    public static let limit = 40
    public static let calmPollEvery: TimeInterval = 20
    public static let warmPollEvery: TimeInterval = 300

    private let store: KeyValueStore

    public init(store: KeyValueStore) {
        self.store = store
    }

    public static func pollEvery(heat: DeviceHeat, lowPower: Bool) -> TimeInterval? {
        switch heat {
        case .critical: return nil
        case .serious: return warmPollEvery
        case .nominal, .fair: return lowPower ? warmPollEvery : calmPollEvery
        }
    }

    public static func top(_ pulls: [PublishedPull]) -> Int {
        pulls.map(\.number).max() ?? 0
    }

    public func seen(_ slug: String) -> Int? {
        entries[slug.lowercased()]
    }

    public func newCount(_ slug: String, pulls: [PublishedPull]) -> Int {
        guard let seen = seen(slug) else {
            record(slug, NewPullsMarker.top(pulls))
            return 0
        }
        return pulls.filter { $0.number > seen && PullStatusFilter.open.matches($0) }.count
    }

    public func markSeen(_ slug: String, pulls: [PublishedPull]) {
        record(slug, max(NewPullsMarker.top(pulls), seen(slug) ?? 0))
    }

    private var lines: [String] {
        store.stringList(forKey: NewPullsMarker.key) ?? []
    }

    private var entries: [String: Int] {
        var out: [String: Int] = [:]
        for line in lines {
            guard let (slug, number) = NewPullsMarker.parse(line), out[slug] == nil else { continue }
            out[slug] = number
        }
        return out
    }

    private func record(_ slug: String, _ number: Int) {
        let key: String = slug.lowercased()
        guard !key.isEmpty, seen(key) != number else { return }
        let rest: [String] = lines.filter { NewPullsMarker.parse($0)?.slug != key }
        store.set(Array((["\(number) \(key)"] + rest).prefix(NewPullsMarker.limit)), forKey: NewPullsMarker.key)
    }

    private static func parse(_ line: String) -> (slug: String, number: Int)? {
        let parts: [Substring] = line.split(separator: " ", maxSplits: 1)
        guard parts.count == 2, let number = Int(parts[0]), !parts[1].isEmpty else { return nil }
        return (String(parts[1]), number)
    }
}
