import FMGit
import FMNet
import Foundation
import XCTest
@testable import FMCode

private struct ProbeError: Error {}

private func probeRequest() throws -> GitHttpRequest {
    try GitHttpRequest(
        method: "GET",
        url: URL(string: "https://forkmesh.test/repo.git/info/refs")!,
        headers: ["accept": "*/*"],
        maxResponseBytes: 1 << 20,
        expectedContentType: "application/x-git-upload-pack-advertisement",
        deadline: nil
    )
}

private final class ProbeBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: Result<T, Error>?
    func set(_ value: Result<T, Error>) { lock.lock(); stored = value; lock.unlock() }
    var value: Result<T, Error>? { lock.lock(); defer { lock.unlock() }; return stored }
}

private func pollSleeping<T>(_ task: Task<T, Error>, _ check: () -> Bool) async throws -> T {
    let box = ProbeBox<T>()
    let waiter = Task { do { box.set(.success(try await task.value)) } catch { box.set(.failure(error)) } }
    while true {
        if let result = box.value { return try result.get() }
        if check() { task.cancel(); waiter.cancel(); throw ProbeError() }
        try? await Task.sleep(nanoseconds: 20_000_000)
    }
}

private func pollBlocking<T>(_ task: Task<T, Error>, _ check: () -> Bool) async throws -> T {
    let box = ProbeBox<T>()
    let waiter = Task { do { box.set(.success(try await task.value)) } catch { box.set(.failure(error)) } }
    while true {
        if let result = box.value { return try result.get() }
        if check() { task.cancel(); waiter.cancel(); throw ProbeError() }
        usleep(20_000)
        await Task.yield()
    }
}

final class DiagCancelProbes: XCTestCase {
    private func cancelAfter<T>(_ ms: UInt64, _ body: @escaping @Sendable () async throws -> T) async {
        let task = Task { try await body() }
        try? await Task.sleep(nanoseconds: ms * 1_000_000)
        task.cancel()
        _ = await task.result
    }

    func testP1SleepLoopCancelled() async {
        await cancelAfter(100) { () async throws -> Int in
            while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) }
            throw ProbeError()
        }
    }

    func testP2StreamIterationCancelled() async {
        let stream = AsyncThrowingStream<Data, Error> { continuation in
            continuation.yield(Data([1]))
            DiagKeep.keep(continuation)
        }
        await cancelAfter(100) { () async throws -> Int in
            var count = 0
            for try await _ in stream { count += 1 }
            return count
        }
    }

    func testP3AwaitingInnerValueCancelled() async {
        let inner = Task { () async throws -> Int in
            while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) }
            throw ProbeError()
        }
        Task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            inner.cancel()
        }
        await cancelAfter(100) { () async throws -> Int in
            let value = try await inner.value
            return value
        }
        inner.cancel()
        _ = await inner.result
    }

    func testP4GenericSleepingPollCancelled() async {
        await cancelAfter(100) { () async throws -> Int in
            let inner = Task { () async throws -> Int in
                while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) }
                throw ProbeError()
            }
            return try await pollSleeping(inner) { Task.isCancelled }
        }
    }

    func testP5GenericBlockingPollCancelled() async {
        await cancelAfter(100) { () async throws -> Int in
            let inner = Task { () async throws -> Int in
                while !Task.isCancelled { try? await Task.sleep(nanoseconds: 20_000_000) }
                throw ProbeError()
            }
            return try await pollBlocking(inner) { Task.isCancelled }
        }
    }

    func testP6TransportHeadOuterCancel() async {
        let transport = GitNetHttpTransport(http: LocalEngineSilentHTTPClient())
        await cancelAfter(100) { try await transport.send(try probeRequest(), cancellation: nil, limits: nil) }
    }

    func testP7TransportBodyOuterCancel() async {
        let transport = GitNetHttpTransport(
            http: LocalEngineStallingHTTPClient(contentType: "application/x-git-upload-pack-advertisement")
        )
        await cancelAfter(200) { try await transport.send(try probeRequest(), cancellation: nil, limits: nil) }
    }

    func testP8TransportHeadTokenCancel() async {
        let transport = GitNetHttpTransport(http: LocalEngineSilentHTTPClient())
        let token = GitCancellationToken()
        let task = Task { try await transport.send(try probeRequest(), cancellation: token, limits: nil) }
        try? await Task.sleep(nanoseconds: 100_000_000)
        token.cancel()
        _ = await task.result
    }

    func testP9SilentClientSendCancelled() async {
        let client = LocalEngineSilentHTTPClient()
        await cancelAfter(100) {
            try await client.send(HTTPRequest(method: "GET", url: URL(string: "https://forkmesh.test/")!))
        }
    }
}

enum DiagKeep {
    nonisolated(unsafe) static var kept: [Any] = []
    static let lock = NSLock()
    static func keep(_ value: Any) { lock.lock(); kept.append(value); lock.unlock() }
}
