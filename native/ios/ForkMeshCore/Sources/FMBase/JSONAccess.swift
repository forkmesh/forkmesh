// Lenient accessors for decoded JSON, after native/android core/json/JsonAccess.kt:
// `str` reads any value as text (missing or null as ""), `bool` is true only
// for a JSON true, `int` reads a number or a numeric string, truncated (else
// the fallback, 0 by default).

import Foundation

// MARK: - Strict views: nil unless the value has that JSON type

extension JSONValue {
    public var isNull: Bool { self == .null }

    public var stringValue: String? {
        if case .string(let value) = self { return value }
        return nil
    }

    public var boolValue: Bool? {
        if case .bool(let value) = self { return value }
        return nil
    }

    /// Any JSON number, as `(value as num?)?.toInt()`: doubles truncate.
    public var int64Value: Int64? {
        switch self {
        case .int(let value): return value
        case .double(let value): return JSONValue.truncate(value)
        default: return nil
        }
    }

    public var intValue: Int? { int64Value.map { Int(truncatingIfNeeded: $0) } }

    /// Any JSON number, as `(value as num?)?.toDouble()`.
    public var doubleValue: Double? {
        switch self {
        case .int(let value): return Double(value)
        case .double(let value): return value
        default: return nil
        }
    }

    public var arrayValue: [JSONValue]? {
        if case .array(let value) = self { return value }
        return nil
    }

    public var objectValue: JSONObject? {
        if case .object(let value) = self { return value }
        return nil
    }

    /// The member of an object; nil for a missing key or any other type.
    public subscript(key: String) -> JSONValue? {
        get { objectValue?[key] }
        set {
            guard case .object(var object) = self else { return }
            object[key] = newValue
            self = .object(object)
        }
    }

    /// The element of an array; nil when out of range or any other type.
    public subscript(index: Int) -> JSONValue? {
        guard case .array(let items) = self, items.indices.contains(index) else { return nil }
        return items[index]
    }

    /// Truncates toward zero, clamped to the Int64 range; NaN has no integer value.
    static func truncate(_ value: Double) -> Int64? {
        if value.isNaN { return nil }
        if value >= 0x1p63 { return Int64.max }
        if value <= -0x1p63 { return Int64.min }
        return Int64(value)
    }
}

// MARK: - Lenient views: any value as text or a number

extension JSONValue {
    /// The value as text, with null as "": whole doubles keep their ".0",
    /// objects and arrays print as JSON.
    public var text: String {
        switch self {
        case .null: return ""
        case .string(let value): return value
        case .bool(let value): return value ? "true" : "false"
        case .int(let value): return String(value)
        case .double(let value): return JSON.formatDouble(value)
        case .array, .object: return JSON.encode(self)
        }
    }

    /// The value as text, or `fallback` for null.
    public func string(or fallback: String = "") -> String { isNull ? fallback : text }

    /// A number, or a string holding one; anything else is the fallback.
    public func int64(or fallback: Int64 = 0) -> Int64 {
        switch self {
        case .int, .double:
            return int64Value ?? fallback
        case .string(let value):
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if let number = Int64(trimmed) { return number }
            if let number = Double(trimmed), let whole = JSONValue.truncate(number) { return whole }
            return fallback
        default:
            return fallback
        }
    }

    public func int(or fallback: Int = 0) -> Int { Int(truncatingIfNeeded: int64(or: Int64(fallback))) }

    public func double(or fallback: Double = 0) -> Double {
        switch self {
        case .int, .double:
            return doubleValue ?? fallback
        case .string(let value):
            return Double(value.trimmingCharacters(in: .whitespacesAndNewlines)) ?? fallback
        default:
            return fallback
        }
    }

    /// Only a real JSON true.
    public var isTrue: Bool { self == .bool(true) }

    public func bool(or fallback: Bool) -> Bool { boolValue ?? fallback }

    /// The object, or an empty one.
    public var objectOrEmpty: JSONObject { objectValue ?? JSONObject() }

    /// The array, or an empty one.
    public var arrayOrEmpty: [JSONValue] { arrayValue ?? [] }

    /// The object entries of an array.
    public var objects: [JSONObject] { arrayOrEmpty.compactMap(\.objectValue) }

    /// Every element of an array as text.
    public var strings: [String] { arrayOrEmpty.map(\.text) }
}

// MARK: - By key, on an object

extension JSONObject {
    /// The value as text; missing or null is "".
    public func str(_ key: String) -> String { self[key]?.text ?? "" }

    /// The value as text; missing or null is `fallback`.
    public func str(_ key: String, _ fallback: String) -> String { self[key]?.string(or: fallback) ?? fallback }

    /// The first non-null of several keys as text, else "".
    public func firstStr(_ keys: String...) -> String {
        for key in keys {
            if let value = self[key], !value.isNull { return value.text }
        }
        return ""
    }

    /// The string, or nil when the key is missing, null or not a string.
    public func optStr(_ key: String) -> String? { self[key]?.stringValue }

    public func int64(_ key: String, _ fallback: Int64 = 0) -> Int64 { self[key]?.int64(or: fallback) ?? fallback }

    public func int(_ key: String, _ fallback: Int = 0) -> Int { self[key]?.int(or: fallback) ?? fallback }

    public func double(_ key: String, _ fallback: Double = 0) -> Double { self[key]?.double(or: fallback) ?? fallback }

    /// Only a real JSON true.
    public func bool(_ key: String) -> Bool { self[key]?.isTrue ?? false }

    /// nil unless the value is an actual JSON boolean.
    public func optBool(_ key: String) -> Bool? { self[key]?.boolValue }

    public func obj(_ key: String) -> JSONObject { self[key]?.objectValue ?? JSONObject() }

    public func list(_ key: String) -> [JSONValue] { self[key]?.arrayValue ?? [] }

    /// The object entries of an array field.
    public func maps(_ key: String) -> [JSONObject] { self[key]?.objects ?? [] }

    /// A scalar array field as strings.
    public func strings(_ key: String) -> [String] { self[key]?.strings ?? [] }
}

// MARK: - By key, through a value (an empty result for anything but an object)

extension JSONValue {
    public func str(_ key: String) -> String { objectValue?.str(key) ?? "" }
    public func str(_ key: String, _ fallback: String) -> String { objectValue?.str(key, fallback) ?? fallback }
    public func int64(_ key: String, _ fallback: Int64 = 0) -> Int64 { objectValue?.int64(key, fallback) ?? fallback }
    public func int(_ key: String, _ fallback: Int = 0) -> Int { objectValue?.int(key, fallback) ?? fallback }
    public func double(_ key: String, _ fallback: Double = 0) -> Double { objectValue?.double(key, fallback) ?? fallback }
    public func bool(_ key: String) -> Bool { objectValue?.bool(key) ?? false }
    public func obj(_ key: String) -> JSONObject { objectValue?.obj(key) ?? JSONObject() }
    public func list(_ key: String) -> [JSONValue] { objectValue?.list(key) ?? [] }
    public func maps(_ key: String) -> [JSONObject] { objectValue?.maps(key) ?? [] }
    public func strings(_ key: String) -> [String] { objectValue?.strings(key) ?? [] }
}
