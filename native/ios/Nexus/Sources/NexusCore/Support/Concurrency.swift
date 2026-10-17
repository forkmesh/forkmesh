import Foundation

public final class Locked<Value>: @unchecked Sendable {
    private let lock = NSLock()
    private var value: Value

    public init(_ value: Value) {
        self.value = value
    }

    @discardableResult
    public func withLock<Result>(_ body: (inout Value) throws -> Result) rethrows -> Result {
        lock.lock()
        defer { lock.unlock() }
        return try body(&value)
    }

    public var current: Value {
        withLock { $0 }
    }
}

public final class OnceFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var fired = false

    public init() {}

    public func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if fired { return false }
        fired = true
        return true
    }
}

public enum BoundedConcurrency {
    public static func run<Item: Sendable, Output: Sendable>(
        _ items: [Item],
        maximumConcurrency: Int,
        operation: @escaping @Sendable (Item) async -> Output,
        onOutput: (Output) async -> Void
    ) async {
        let width = max(1, maximumConcurrency)
        await withTaskGroup(of: Output.self) { group in
            var iterator = items.makeIterator()
            var running = 0
            while running < width, let item = iterator.next() {
                group.addTask { await operation(item) }
                running += 1
            }
            while let output = await group.next() {
                await onOutput(output)
                if Task.isCancelled {
                    group.cancelAll()
                    continue
                }
                if let item = iterator.next() {
                    group.addTask { await operation(item) }
                }
            }
        }
    }
}

public func withTimeout<Value: Sendable>(
    _ timeout: Duration,
    operation: @escaping @Sendable () async throws -> Value
) async throws -> Value {
    try await withThrowingTaskGroup(of: Value.self) { group in
        group.addTask { try await operation() }
        group.addTask {
            try await Task.sleep(for: timeout)
            throw ConnectionFailure.timedOut
        }
        defer { group.cancelAll() }
        guard let value = try await group.next() else { throw ConnectionFailure.cancelled }
        return value
    }
}
