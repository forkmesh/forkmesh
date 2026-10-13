// The time source is a value a service is built with, so a test can fix it.

import Foundation

public protocol Clock: Sendable {
    var now: Date { get }
}

extension Clock {
    /// Milliseconds since the Unix epoch.
    public var nowMillis: Int64 { now.epochMillis }

    /// Whole seconds, as `millisecondsSinceEpoch ~/ 1000`.
    public var nowSeconds: Int64 { nowMillis / 1_000 }
}

public struct SystemClock: Clock {
    public init() {}
    public var now: Date { Date() }
}

/// A clock tests set and advance by hand.
public final class FixedClock: Clock, @unchecked Sendable {
    private let lock = NSLock()
    private var current: Date

    public init(_ now: Date = Date(timeIntervalSince1970: 0)) { current = now }

    public convenience init(epochMillis: Int64) { self.init(Date(epochMillis: epochMillis)) }

    public var now: Date {
        get { lock.lock(); defer { lock.unlock() }; return current }
        set { lock.lock(); current = newValue; lock.unlock() }
    }

    public func advance(by seconds: TimeInterval) {
        lock.lock()
        current = current.addingTimeInterval(seconds)
        lock.unlock()
    }
}

extension Date {
    /// `DateTime.millisecondsSinceEpoch`. Rounded to nearest: a Date counts
    /// seconds from 2001 as a double, so a whole millisecond can arrive as
    /// 4.9999 and must not floor to 4.
    public var epochMillis: Int64 { Int64((timeIntervalSince1970 * 1_000).rounded()) }

    /// `DateTime.fromMillisecondsSinceEpoch`.
    public init(epochMillis: Int64) { self.init(timeIntervalSince1970: Double(epochMillis) / 1_000) }
}
