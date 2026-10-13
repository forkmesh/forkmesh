// The real vault Worker module over the same stdio bridge the browser and desktop clients are
// tested with (marketplace/packages/vault/tests/vault_stdio_server.py). Tests that use it
// skip when python3 or the script is missing.
import FMBase
import Foundation
import XCTest
@testable import FMVault

final class VaultBridge: @unchecked Sendable {
    private let process = Process()
    private let input = Pipe()
    private let output = Pipe()
    private let lock = NSLock()
    private var buffer = Data()
    private(set) var calls: [(String, String)] = []

    static var script: URL {
        VaultInteropTests.fixturesDirectory.deletingLastPathComponent().appendingPathComponent("vault_stdio_server.py")
    }

    static func start() throws -> VaultBridge {
        guard FileManager.default.fileExists(atPath: script.path) else {
            throw XCTSkip("the vault stdio bridge is not reachable from here")
        }
        let bridge = VaultBridge()
        bridge.process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        bridge.process.arguments = ["python3", script.path]
        bridge.process.standardInput = bridge.input
        bridge.process.standardOutput = bridge.output
        do {
            try bridge.process.run()
        } catch {
            throw XCTSkip("python3 is missing: \(error)")
        }
        return bridge
    }

    func stop() {
        try? input.fileHandleForWriting.close()
        if process.isRunning { process.terminate() }
        process.waitUntilExit()
    }

    /// One JSON line out, one back.
    func send(_ message: JSONObject) throws -> JSONObject {
        lock.lock()
        defer { lock.unlock() }
        input.fileHandleForWriting.write(Data((JSON.encode(message) + "\n").utf8))
        while true {
            if let newline = buffer.firstIndex(of: 0x0a) {
                let line = buffer[buffer.startIndex..<newline]
                buffer = Data(buffer[(newline + 1)...])
                return JSON.parseObject(Data(line))
            }
            let chunk = output.fileHandleForReading.availableData
            if chunk.isEmpty { throw VaultError("bridge_closed") }
            buffer.append(chunk)
        }
    }

    func control(_ name: String, _ extra: JSONObject = JSONObject()) throws -> JSONObject {
        var message: JSONObject = ["control": .string(name)]
        message.merge(extra)
        return try send(message).obj("data")
    }

    private func record(_ method: String, _ path: String) {
        lock.lock()
        calls.append((method, path))
        lock.unlock()
    }

    /// The request function a VaultClient is given, acting as `actor`.
    func request(as actor: @escaping @Sendable () -> String) -> VaultRequest {
        { [self] method, path, body in
            record(method, path)
            let reply = try send([
                "actor": .string(actor()), "method": .string(method), "path": .string(path),
                "body": body.map(JSONValue.object) ?? .null,
            ])
            let data = reply.obj("data")
            let status = reply.int("status")
            if status >= 400 || data["ok"] == .bool(false) {
                throw VaultError(data["error"].map { $0.string(or: "http_\(status)") } ?? "http_\(status)")
            }
            return data
        }
    }

    @MainActor
    func client(for actor: String) -> VaultClient {
        let client = VaultClient(request: request(as: { actor }))
        client.newMemoryKiB = VaultKdf.floorMemoryKiB
        client.newPasses = VaultKdf.floorPasses
        return client
    }
}

func refused(_ code: String, file: StaticString = #filePath, line: UInt = #line, _ action: () async throws -> Void) async {
    do {
        try await action()
        XCTFail("accepted; expected \(code)", file: file, line: line)
    } catch let error as VaultError {
        XCTAssertEqual(error.code, code, file: file, line: line)
    } catch {
        XCTFail("threw \(error); expected \(code)", file: file, line: line)
    }
}
