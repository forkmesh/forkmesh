// Every engine operation runs on a detached task (never the main actor) so the
// UI never blocks on hashing, zlib, or filesystem walks. Operations are
// serialized per runner, and `cancel()` trips the operation's
// `GitCancellationToken` (which the engine checks between steps) and cancels
// the task (which aborts an in-flight HTTP exchange). A cancelled run fails
// with `GitException(.cancelled, ...)`.

import FMGit
import Foundation

/// Serializes and runs engine work off the main actor.
public final class GitOperationRunner: @unchecked Sendable {
    /// Receives `(operationName, elapsed, failed)` for every run so callers can
    /// surface engine timings (performance monitor, tests).
    public typealias DurationHook = @Sendable (_ name: String, _ elapsed: Duration, _ failed: Bool) -> Void

    private let onDuration: DurationHook?
    private let lock = NSLock()
    private var active = false
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private var disposed = false
    private var cancelRequested = false
    private var activeToken: GitCancellationToken?
    private var activeTask: Task<Void, Never>?

    public init(onDuration: DurationHook? = nil) {
        self.onDuration = onDuration
    }

    /// True while an operation is executing.
    public var isBusy: Bool { locked { active } }

    /// Execute `body` off the main actor and return its result.
    ///
    /// Errors thrown by `body` surface as `GitException` when they came from
    /// the engine, as `GitWorkerException` otherwise.
    public func run<T: Sendable>(
        _ name: String,
        _ body: @escaping @Sendable (GitCancellationToken) async throws -> T
    ) async throws -> T {
        if locked({ disposed }) {
            throw GitException(.cancelled, "git worker has been disposed")
        }
        // Serialize operations per runner: the engine's lock files are per
        // repository and overlapping local operations would only race them.
        await acquire()
        defer { release() }
        let token = GitCancellationToken()
        let refused: Bool = locked {
            if disposed { return true }
            cancelRequested = false
            activeToken = token
            return false
        }
        if refused {
            throw GitException(.cancelled, "git worker has been disposed")
        }
        let started = ContinuousClock.now
        var failed = false
        defer {
            let elapsed = ContinuousClock.now - started
            onDuration?(name, elapsed, failed)
        }
        do {
            return try await execute(token, body)
        } catch {
            failed = true
            throw error
        }
    }

    private func execute<T: Sendable>(
        _ token: GitCancellationToken,
        _ body: @escaping @Sendable (GitCancellationToken) async throws -> T
    ) async throws -> T {
        let box = ResultBox<T>()
        let task = Task.detached(priority: .userInitiated) {
            do {
                box.set(.success(try await body(token)))
            } catch {
                box.set(.failure(error))
            }
        }
        let cancelledEarly: Bool = locked {
            activeTask = task
            return cancelRequested
        }
        if cancelledEarly {
            // cancel() raced the start; honour it now.
            token.cancel()
            task.cancel()
        }
        await withTaskCancellationHandler {
            await task.value
        } onCancel: {
            token.cancel()
            task.cancel()
        }
        let wasCancelled: Bool = locked {
            activeTask = nil
            activeToken = nil
            return cancelRequested
        }
        switch box.get() {
        case .success(let value):
            return value
        case .failure(let error):
            if wasCancelled || Task.isCancelled {
                throw GitException(.cancelled, "operation was cancelled")
            }
            throw Self.normalize(error)
        case nil:
            throw GitException(.cancelled, "operation was cancelled")
        }
    }

    /// Engine and worker failures pass through, anything else becomes a
    /// worker failure carrying its text.
    static func normalize(_ error: Error) -> Error {
        if error is GitException || error is GitWorkerException || error is GitWorkspaceException { return error }
        if error is CancellationError { return GitException(.cancelled, "operation was cancelled") }
        return GitWorkerException(String(describing: error))
    }

    /// Stop the running operation, if any. Its `run` fails with a
    /// `cancelled` `GitException`.
    public func cancel() {
        let (token, task): (GitCancellationToken?, Task<Void, Never>?) = locked {
            guard active else { return (nil, nil) }
            cancelRequested = true
            return (activeToken, activeTask)
        }
        token?.cancel()
        task?.cancel()
    }

    /// Cancel any running work and refuse further runs.
    public func dispose() {
        locked { disposed = true }
        cancel()
    }

    // MARK: - Queue

    private func acquire() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            let ready: Bool = locked {
                if !active {
                    active = true
                    return true
                }
                waiters.append(continuation)
                return false
            }
            if ready { continuation.resume() }
        }
    }

    private func release() {
        let next: CheckedContinuation<Void, Never>? = locked {
            if waiters.isEmpty {
                active = false
                return nil
            }
            // `active` stays true: ownership passes straight to the next waiter.
            return waiters.removeFirst()
        }
        next?.resume()
    }

    private func locked<R>(_ body: () -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }
}

/// The worker task's result, handed back without requiring `T` to cross an
/// actor boundary twice.
private final class ResultBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var result: Result<T, Error>?

    func set(_ value: Result<T, Error>) {
        lock.lock()
        result = value
        lock.unlock()
    }

    func get() -> Result<T, Error>? {
        lock.lock()
        defer { lock.unlock() }
        return result
    }
}
