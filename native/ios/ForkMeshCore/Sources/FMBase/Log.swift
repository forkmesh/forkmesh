// A one-line logging facade, with a sink the app target (os_log) or a test
// can replace.

import Foundation

public enum LogLevel: Int, Comparable, Sendable {
    case debug, info, warning, error

    public static func < (lhs: LogLevel, rhs: LogLevel) -> Bool { lhs.rawValue < rhs.rawValue }

    public var label: String {
        switch self {
        case .debug: return "debug"
        case .info: return "info"
        case .warning: return "warn"
        case .error: return "error"
        }
    }
}

public enum Log {
    public typealias Sink = @Sendable (_ level: LogLevel, _ tag: String, _ message: String) -> Void

    private static let lock = NSLock()
    private static var currentSink: Sink = Log.standardError
    private static var currentMinimum: LogLevel = .debug

    /// Where lines go. Defaults to standard error.
    public static var sink: Sink {
        get { lock.lock(); defer { lock.unlock() }; return currentSink }
        set { lock.lock(); currentSink = newValue; lock.unlock() }
    }

    /// Lines below this level are dropped before the message is built.
    public static var minimumLevel: LogLevel {
        get { lock.lock(); defer { lock.unlock() }; return currentMinimum }
        set { lock.lock(); currentMinimum = newValue; lock.unlock() }
    }

    public static func debug(_ tag: String, _ message: @autoclosure () -> String) { write(.debug, tag, message) }
    public static func info(_ tag: String, _ message: @autoclosure () -> String) { write(.info, tag, message) }
    public static func warning(_ tag: String, _ message: @autoclosure () -> String) { write(.warning, tag, message) }
    public static func error(_ tag: String, _ message: @autoclosure () -> String) { write(.error, tag, message) }

    private static func write(_ level: LogLevel, _ tag: String, _ message: () -> String) {
        lock.lock()
        let sink = currentSink
        let minimum = currentMinimum
        lock.unlock()
        if level < minimum { return }
        sink(level, tag, message())
    }

    public static let standardError: Sink = { level, tag, message in
        FileHandle.standardError.write(Data("[\(level.label)] \(tag): \(message)\n".utf8))
    }
}
