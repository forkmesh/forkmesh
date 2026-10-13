// A remote operation is `async throws` plus a progress callback: it returns
// its value or throws GitException. `runGitOperation` maps failures: a
// GitException passes through unchanged, anything else becomes
// `operationInterrupted` with a redacted "unexpected failure: <type>" message,
// and a `completed` progress sample follows a success. GitProgressSink caps
// the number of events.

import Foundation

/// Phases reported to a `GitProgressListener`.
public enum GitProgressPhase: String, CaseIterable, Sendable {
    case connecting
    case listingRefs
    case negotiating
    case receivingPack
    case indexingPack
    case updatingRefs
    case checkingOut
    case preparingPack
    case sendingPack
    case awaitingReport
    case completed
}

/// A bounded progress sample containing typed counters and redacted labels.
public struct GitProgress: Sendable, CustomStringConvertible {
    public let phase: GitProgressPhase
    public let bytes: Int
    public let objects: Int
    public let refs: Int
    public let total: Int

    /// Redacted remote host (never a path, query, or credential).
    public let remoteHost: String?

    public init(
        _ phase: GitProgressPhase, bytes: Int = 0, objects: Int = 0, refs: Int = 0, total: Int = 0,
        remoteHost: String? = nil
    ) {
        self.phase = phase
        self.bytes = bytes
        self.objects = objects
        self.refs = refs
        self.total = total
        self.remoteHost = remoteHost
    }

    public init(
        phase: GitProgressPhase, bytes: Int = 0, objects: Int = 0, refs: Int = 0, total: Int = 0,
        remoteHost: String? = nil
    ) {
        self.init(phase, bytes: bytes, objects: objects, refs: refs, total: total, remoteHost: remoteHost)
    }

    public var description: String {
        "GitProgress(\(phase.rawValue), bytes: \(bytes), objects: \(objects), refs: \(refs))"
    }
}

public typealias GitProgressListener = @Sendable (GitProgress) -> Void

/// Bounded progress emitter handed to operation bodies.
public final class GitProgressSink: @unchecked Sendable {
    public static let maxEvents = 100_000

    private let listener: GitProgressListener?
    private let lock = NSLock()
    private var emitted = 0
    private var closed = false

    public init(_ listener: GitProgressListener?) { self.listener = listener }

    public func add(_ progress: GitProgress) {
        lock.lock()
        if closed || emitted >= Self.maxEvents {
            lock.unlock()
            return
        }
        emitted += 1
        lock.unlock()
        listener?(progress)
    }

    /// `controller.close()`: later samples are dropped.
    public func close() {
        lock.lock()
        closed = true
        lock.unlock()
    }
}

/// Runs `body` with a token (the caller's or a fresh one) and a bounded sink,
/// and maps its outcome as described at the top of this file.
public func runGitOperation<T>(
    cancellation: GitCancellationToken?,
    progress: GitProgressListener?,
    _ body: (GitCancellationToken, GitProgressSink) async throws -> T
) async throws -> T {
    let token = cancellation ?? GitCancellationToken()
    let sink = GitProgressSink(progress)
    defer { sink.close() }
    do {
        let value = try await body(token, sink)
        sink.add(GitProgress(.completed))
        return value
    } catch let error as GitException {
        throw error
    } catch is CancellationError {
        // Swift task cancellation is the same request as cancelling the token.
        throw GitException(.cancelled, "operation was cancelled")
    } catch {
        throw GitException(.operationInterrupted, "unexpected failure: \(dartRuntimeTypeName(error))")
    }
}

/// The type name an `unexpected failure` message reports for `error`.
public func dartRuntimeTypeName(_ error: Error) -> String {
    switch error {
    case is GitArgumentError: return "ArgumentError"
    case is GitStateError: return "StateError"
    case is GitFormatError: return "FormatException"
    case is GitFileSystemError, is EngineFileSystemError: return "FileSystemException"
    default: return String(describing: type(of: error))
    }
}
