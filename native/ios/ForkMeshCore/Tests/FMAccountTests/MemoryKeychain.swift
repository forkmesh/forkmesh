import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class MemoryKeychain: ThrowingSecureBackend, @unchecked Sendable {
    struct Locked: Error, CustomStringConvertible {
        var description: String { "errSecInteractionNotAllowed" }
    }

    private let lock = NSLock()
    private var values: [String: String]
    var failing = false

    init(_ seed: [String: String] = [:]) { values = seed }

    var snapshot: [String: String] {
        lock.lock()
        defer { lock.unlock() }
        return values
    }

    func read(_ key: String) throws -> String? {
        if failing { throw Locked() }
        return snapshot[key]
    }

    func write(_ key: String, _ value: String) throws {
        if failing { throw Locked() }
        lock.lock()
        values[key] = value
        lock.unlock()
    }

    func delete(_ key: String) throws {
        if failing { throw Locked() }
        lock.lock()
        values.removeValue(forKey: key)
        lock.unlock()
    }

    func readAll() throws -> [String: String] {
        if failing { throw Locked() }
        return snapshot
    }
}

