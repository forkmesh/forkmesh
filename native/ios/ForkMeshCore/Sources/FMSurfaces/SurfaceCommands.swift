import FMApi
import FMBase
import Foundation

public enum SurfaceCommand: Codable, Hashable, Sendable {
    case open(Route)
    case stopAgent(runId: String)
    case approve(approvalId: String)
    case startTimer(seconds: Int, label: String)

    public var survivesHandoff: Bool {
        if case .open = self { return true }
        return false
    }
}

public struct PendingSurfaceCommand: Codable, Hashable, Sendable {
    public var command: SurfaceCommand
    public var atMs: Int64

    public init(command: SurfaceCommand, atMs: Int64) {
        self.command = command
        self.atMs = atMs
    }
}

public final class PendingSurfaceCommandStore: @unchecked Sendable {
    public static let fileName = "pending-commands.json"
    public static let maxCommands = 8
    public static let maxAgeMs: Int64 = 10 * 60 * 1_000
    static let maxBytes = 32 * 1_024

    public let items: SharedItemStore
    private let clock: Clock
    private let lock = NSLock()

    public init(items: SharedItemStore, clock: Clock = SystemClock()) {
        self.items = items
        self.clock = clock
    }

    public convenience init(directory: URL, clock: Clock = SystemClock()) {
        self.init(items: DirectorySharedItemStore(directory: directory), clock: clock)
    }

    public static func shared() -> PendingSurfaceCommandStore? {
        SurfaceContainer.sharedItems().map { PendingSurfaceCommandStore(items: $0) }
    }

    private func load() -> [PendingSurfaceCommand] {
        SurfaceFiles.decode(
            [PendingSurfaceCommand].self, from: items.data(PendingSurfaceCommandStore.fileName),
            maxBytes: PendingSurfaceCommandStore.maxBytes) ?? []
    }

    @discardableResult
    public func enqueue(_ command: SurfaceCommand) -> Bool {
        guard command.survivesHandoff else { return false }
        lock.lock()
        defer { lock.unlock() }
        let now = clock.nowMillis
        var list = load().filter { now - $0.atMs < PendingSurfaceCommandStore.maxAgeMs && $0.command != command }
        list.append(PendingSurfaceCommand(command: command, atMs: now))
        list = Array(list.suffix(PendingSurfaceCommandStore.maxCommands))
        guard let data = SurfaceFiles.encode(list, maxBytes: PendingSurfaceCommandStore.maxBytes) else { return false }
        return items.set(data, PendingSurfaceCommandStore.fileName)
    }

    public func drain() -> [SurfaceCommand] {
        lock.lock()
        defer { lock.unlock() }
        let now = clock.nowMillis
        let list = load()
        if list.isEmpty { return [] }
        items.remove(PendingSurfaceCommandStore.fileName)
        return list.filter { now - $0.atMs < PendingSurfaceCommandStore.maxAgeMs && now >= $0.atMs - 60_000 }
            .map(\.command)
    }
}

public struct SurfaceCommandError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { message }

    public static let appNotReady = SurfaceCommandError("Open ForkMesh and try again.")
}

@MainActor
public final class SurfaceCommandCenter {
    public static let shared = SurfaceCommandCenter()

    public typealias Handler = @MainActor (SurfaceCommand) async throws -> Void

    public var handler: Handler?
    public var pendingStore: () -> PendingSurfaceCommandStore? = { PendingSurfaceCommandStore.shared() }
    public var waitStepNanoseconds: UInt64 = 250_000_000

    public init() {}

    public func perform(_ command: SurfaceCommand, waitSeconds: Double = 0) async throws {
        if let handler {
            try await handler(command)
            return
        }
        let steps = Int((waitSeconds * 1_000_000_000) / Double(max(1, waitStepNanoseconds)))
        if steps > 0 {
            for _ in 0..<steps {
                try? await Task.sleep(nanoseconds: waitStepNanoseconds)
                if let handler {
                    try await handler(command)
                    return
                }
            }
        }
        if command.survivesHandoff, pendingStore()?.enqueue(command) == true { return }
        throw SurfaceCommandError.appNotReady
    }

    public func drainPending() -> [SurfaceCommand] {
        pendingStore()?.drain() ?? []
    }
}
