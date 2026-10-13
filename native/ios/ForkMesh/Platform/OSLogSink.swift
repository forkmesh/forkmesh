// `debugPrint` for the native app: FMBase's `Log` routed to the unified
// logging system, so Console.app and `log stream` show the core's lines under
// the app's subsystem, one category per tag.

import FMBase
import Foundation
import os

enum OSLogSink {
    static let subsystem: String = Bundle.main.bundleIdentifier ?? "com.forkmesh.forkmesh"

    private static let lock = NSLock()
    private static var loggers: [String: Logger] = [:]

    private static func logger(_ tag: String) -> Logger {
        lock.lock()
        defer { lock.unlock() }
        if let existing = loggers[tag] {
            return existing
        }
        let created: Logger = Logger(subsystem: subsystem, category: tag)
        loggers[tag] = created
        return created
    }

    /// Install as `Log.sink`. Messages are public: the core never logs a
    /// secret (tokens are described, not printed), and a private message
    /// would read "<private>" in every field report.
    static func install(minimumLevel: LogLevel = .info) {
        Log.minimumLevel = minimumLevel
        Log.sink = { (level: LogLevel, tag: String, message: String) in
            let log: Logger = OSLogSink.logger(tag)
            switch level {
            case .debug:
                log.debug("\(message, privacy: .public)")
            case .info:
                log.info("\(message, privacy: .public)")
            case .warning:
                log.warning("\(message, privacy: .public)")
            case .error:
                log.error("\(message, privacy: .public)")
            }
        }
    }
}
