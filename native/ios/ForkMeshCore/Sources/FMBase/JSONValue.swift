// Decoded JSON as a value type, where native/android core/json/Json.kt uses
// `Any?`.

import Foundation

/// A decoded JSON value. Integral numbers that fit are `.int`,
/// everything else numeric is `.double`; the two encode differently ("1" and
/// "1.0"), and signed payloads depend on that.
public enum JSONValue: Equatable, Hashable, Sendable {
    case null
    case bool(Bool)
    case int(Int64)
    case double(Double)
    case string(String)
    case array([JSONValue])
    case object(JSONObject)
}

/// A JSON object whose keys keep their insertion order. Setting an existing
/// key keeps its position; equality
/// ignores order, as it does for a Kotlin `LinkedHashMap`.
public struct JSONObject: Sendable, ExpressibleByDictionaryLiteral {
    public private(set) var keys: [String]
    private var storage: [String: JSONValue]

    public init() {
        keys = []
        storage = [:]
    }

    public init(dictionaryLiteral elements: (String, JSONValue)...) {
        self.init(elements)
    }

    /// Later duplicates overwrite the value and keep the first position, as
    /// repeated `map[key] = value` would.
    public init<S: Sequence>(_ pairs: S) where S.Element == (String, JSONValue) {
        self.init()
        for (key, value) in pairs { self[key] = value }
    }

    public var count: Int { keys.count }
    public var isEmpty: Bool { keys.isEmpty }

    /// The values, in key order.
    public var values: [JSONValue] { keys.map { storage[$0] ?? .null } }

    /// Assigning nil removes the key; assign `.null` to store a JSON null.
    public subscript(key: String) -> JSONValue? {
        get { storage[key] }
        set {
            guard let newValue else {
                removeValue(forKey: key)
                return
            }
            if storage.updateValue(newValue, forKey: key) == nil { keys.append(key) }
        }
    }

    public func contains(_ key: String) -> Bool { storage[key] != nil }

    @discardableResult
    public mutating func removeValue(forKey key: String) -> JSONValue? {
        guard let old = storage.removeValue(forKey: key) else { return nil }
        if let index = keys.firstIndex(of: key) { keys.remove(at: index) }
        return old
    }

    public mutating func removeAll() {
        keys.removeAll()
        storage.removeAll()
    }

    /// `map.addAll(other)`: existing keys keep their place, new ones append.
    public mutating func merge(_ other: JSONObject) {
        for (key, value) in other { self[key] = value }
    }
}

extension JSONObject: Equatable, Hashable {
    public static func == (lhs: JSONObject, rhs: JSONObject) -> Bool { lhs.storage == rhs.storage }
    public func hash(into hasher: inout Hasher) { hasher.combine(storage) }
}

extension JSONObject: Sequence {
    public typealias Element = (key: String, value: JSONValue)

    public func makeIterator() -> AnyIterator<Element> {
        var index = 0
        let keys = self.keys
        let storage = self.storage
        return AnyIterator {
            guard index < keys.count else { return nil }
            let key = keys[index]
            index += 1
            return (key: key, value: storage[key] ?? .null)
        }
    }
}

// MARK: - Literals

extension JSONValue: ExpressibleByNilLiteral, ExpressibleByBooleanLiteral, ExpressibleByIntegerLiteral,
    ExpressibleByFloatLiteral, ExpressibleByStringLiteral, ExpressibleByArrayLiteral,
    ExpressibleByDictionaryLiteral {
    public init(nilLiteral: ()) { self = .null }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
    public init(integerLiteral value: Int64) { self = .int(value) }
    public init(floatLiteral value: Double) { self = .double(value) }
    public init(stringLiteral value: String) { self = .string(value) }
    public init(arrayLiteral elements: JSONValue...) { self = .array(elements) }
    public init(dictionaryLiteral elements: (String, JSONValue)...) { self = .object(JSONObject(elements)) }
}

// MARK: - Wrapping Swift values

extension JSONValue {
    public init(_ value: String) { self = .string(value) }
    public init(_ value: Bool) { self = .bool(value) }
    public init(_ value: Int) { self = .int(Int64(value)) }
    public init(_ value: Int64) { self = .int(value) }
    public init(_ value: Double) { self = .double(value) }
    public init(_ value: [JSONValue]) { self = .array(value) }
    public init(_ value: JSONObject) { self = .object(value) }
    public init(_ value: [String]) { self = .array(value.map(JSONValue.string)) }

    /// nil becomes a JSON null.
    public init(_ value: String?) { self = value.map(JSONValue.string) ?? .null }
    public init(_ value: Int?) { self = value.map { .int(Int64($0)) } ?? .null }
    public init(_ value: Bool?) { self = value.map(JSONValue.bool) ?? .null }
    public init(_ value: Double?) { self = value.map(JSONValue.double) ?? .null }
}
