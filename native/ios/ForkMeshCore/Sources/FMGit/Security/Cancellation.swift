import Foundation

/// Cooperative cancellation. A token is used (rather than Swift task
/// cancellation) because the engine polls it between objects and chunks and
/// callers cancel from a button, not by dropping a task. It is thread-safe
/// here since the engine runs off the caller's thread.
public final class GitCancellationToken: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false

    public init() {}

    public var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    public func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    public func throwIfCancelled() throws {
        if isCancelled { throw GitException(.cancelled, "operation was cancelled") }
    }
}
