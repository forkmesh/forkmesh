// Preferences storage behind a protocol the app target implements over
// UserDefaults (native/android: platform/Prefs.kt). Keys keep the names the
// retired Flutter app wrote ("server/url", "auth/session", ...).

import Foundation

/// Getters return nil when the key was never set or holds another type.
/// Reads and writes are synchronous, as UserDefaults is; implementations are
/// safe to call from any thread.
public protocol KeyValueStore: AnyObject, Sendable {
    func string(forKey key: String) -> String?
    func bool(forKey key: String) -> Bool?
    func int(forKey key: String) -> Int?
    func double(forKey key: String) -> Double?
    func stringList(forKey key: String) -> [String]?

    func set(_ value: String, forKey key: String)
    func set(_ value: Bool, forKey key: String)
    func set(_ value: Int, forKey key: String)
    func set(_ value: Double, forKey key: String)
    func set(_ value: [String], forKey key: String)

    func remove(_ key: String)
    func contains(_ key: String) -> Bool
    var keys: Set<String> { get }
    func clear()
}

extension KeyValueStore {
    public func string(forKey key: String, or fallback: String) -> String { string(forKey: key) ?? fallback }
    public func bool(forKey key: String, or fallback: Bool) -> Bool { bool(forKey: key) ?? fallback }
    public func int(forKey key: String, or fallback: Int) -> Int { int(forKey: key) ?? fallback }
    public func double(forKey key: String, or fallback: Double) -> Double { double(forKey: key) ?? fallback }

    /// nil removes the key, as the Kotlin `putString(key, null)` does.
    public func set(_ value: String?, forKey key: String) {
        if let value { set(value, forKey: key) } else { remove(key) }
    }
}

/// `SharedPreferences.setMockInitialValues`, for tests.
public final class InMemoryKeyValueStore: KeyValueStore, @unchecked Sendable {
    private let lock = NSLock()
    private var values: JSONObject

    public init(_ seed: JSONObject = [:]) { values = seed }

    /// Everything stored, in the order it was first set.
    public var snapshot: JSONObject {
        lock.lock()
        defer { lock.unlock() }
        return values
    }

    private func read(_ key: String) -> JSONValue? {
        lock.lock()
        defer { lock.unlock() }
        return values[key]
    }

    private func write(_ key: String, _ value: JSONValue?) {
        lock.lock()
        values[key] = value
        lock.unlock()
    }

    public func string(forKey key: String) -> String? { read(key)?.stringValue }
    public func bool(forKey key: String) -> Bool? { read(key)?.boolValue }

    public func int(forKey key: String) -> Int? {
        if case .int(let value)? = read(key) { return Int(truncatingIfNeeded: value) }
        return nil
    }

    /// A whole double round-trips through a JSON file as "1.0", so the types
    /// stay apart; an int is still readable as a double, as in UserDefaults.
    public func double(forKey key: String) -> Double? { read(key)?.doubleValue }

    public func stringList(forKey key: String) -> [String]? {
        guard let items = read(key)?.arrayValue else { return nil }
        return items.compactMap(\.stringValue)
    }

    public func set(_ value: String, forKey key: String) { write(key, .string(value)) }
    public func set(_ value: Bool, forKey key: String) { write(key, .bool(value)) }
    public func set(_ value: Int, forKey key: String) { write(key, .int(Int64(value))) }
    public func set(_ value: Double, forKey key: String) { write(key, .double(value)) }
    public func set(_ value: [String], forKey key: String) { write(key, .array(value.map(JSONValue.string))) }

    public func remove(_ key: String) { write(key, nil) }
    public func contains(_ key: String) -> Bool { read(key) != nil }
    public var keys: Set<String> { Set(snapshot.keys) }

    public func clear() {
        lock.lock()
        values.removeAll()
        lock.unlock()
    }
}

/// A `KeyValueStore` kept as one JSON file, for Linux tools and tests that
/// need persistence. Every change rewrites the file atomically; an unreadable
/// file starts empty instead of failing the launch.
public final class FileKeyValueStore: KeyValueStore, @unchecked Sendable {
    public let url: URL
    private let memory: InMemoryKeyValueStore
    private let writeLock = NSLock()

    public init(url: URL) {
        self.url = url
        memory = InMemoryKeyValueStore((try? Data(contentsOf: url)).map { JSON.parseObject($0) } ?? JSONObject())
    }

    /// The snapshot is read inside the lock, so whichever thread writes last
    /// writes the newest state.
    private func persist() {
        writeLock.lock()
        defer { writeLock.unlock() }
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true
            )
            try Data(JSON.encodePretty(.object(memory.snapshot)).utf8).write(to: url, options: .atomic)
        } catch {
            Log.warning("FileKeyValueStore", "could not write \(url.path): \(error)")
        }
    }

    public func string(forKey key: String) -> String? { memory.string(forKey: key) }
    public func bool(forKey key: String) -> Bool? { memory.bool(forKey: key) }
    public func int(forKey key: String) -> Int? { memory.int(forKey: key) }
    public func double(forKey key: String) -> Double? { memory.double(forKey: key) }
    public func stringList(forKey key: String) -> [String]? { memory.stringList(forKey: key) }
    public func set(_ value: String, forKey key: String) { memory.set(value, forKey: key); persist() }
    public func set(_ value: Bool, forKey key: String) { memory.set(value, forKey: key); persist() }
    public func set(_ value: Int, forKey key: String) { memory.set(value, forKey: key); persist() }
    public func set(_ value: Double, forKey key: String) { memory.set(value, forKey: key); persist() }
    public func set(_ value: [String], forKey key: String) { memory.set(value, forKey: key); persist() }
    public func remove(_ key: String) { memory.remove(key); persist() }
    public func contains(_ key: String) -> Bool { memory.contains(key) }
    public var keys: Set<String> { memory.keys }
    public func clear() { memory.clear(); persist() }
}
