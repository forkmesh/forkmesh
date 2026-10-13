import FMGit
import Foundation
import XCTest
@testable import FMCode

final class LocalGitSmartServer: GitHttpTransport, @unchecked Sendable {
    let git: LocalTestGit
    private let lock = NSLock()
    private var repositories: [String: String] = [:]
    private var redirects: [String: String] = [:]
    private var unavailable: [String: Int] = [:]
    private var downHosts: Set<String> = []
    private var recorded: [GitHttpRequest] = []

    init(git: LocalTestGit) {
        self.git = git
    }

    func serve(_ base: String, _ bareRepository: String) {
        lock.lock()
        repositories[base] = bareRepository
        lock.unlock()
    }

    func redirect(_ base: String, to target: String) {
        lock.lock()
        redirects[base] = target
        lock.unlock()
    }

    func answer(_ base: String, status: Int) {
        lock.lock()
        unavailable[base] = status
        lock.unlock()
    }

    func takeDown(_ host: String) {
        lock.lock()
        downHosts.insert(host)
        lock.unlock()
    }

    var requests: [GitHttpRequest] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    var fetchBodies: [String] {
        requests.filter { $0.method == "POST" }.map { String(decoding: $0.body, as: UTF8.self) }
            .filter { $0.contains("command=fetch") }
    }

    func requested(host: String) -> [GitHttpRequest] {
        requests.filter { $0.url.host == host }
    }

    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        try cancellation?.throwIfCancelled()
        let (routes, moves, statuses, down) = record(request)
        if down.contains(request.url.host ?? "") {
            throw GitException(.networkUnavailable, "connection failed: host is down")
        }
        let text = request.url.absoluteString
        let bases = Set(routes.keys).union(moves.keys).union(statuses.keys)
        guard let base = bases.filter({ text.hasPrefix($0 + "/") }).max(by: { $0.count < $1.count }) else {
            throw classifyHttpFailure(request, statusCode: 404)
        }
        let suffix = String(text.dropFirst(base.count))
        if let status = statuses[base] {
            throw classifyHttpFailure(request, statusCode: status)
        }
        if let target = moves[base] {
            return GitHttpResponse(statusCode: 307, headers: ["location": target + suffix], body: [])
        }
        guard let directory = routes[base] else { throw classifyHttpFailure(request, statusCode: 404) }
        let response = try respond(request, suffix: suffix, directory: directory)
        if response.body.count > request.maxResponseBytes {
            throw GitException(.resourceLimitExceeded, "response body exceeds the configured limit")
        }
        return response
    }

    func close() async {}

    private func record(
        _ request: GitHttpRequest
    ) -> ([String: String], [String: String], [String: Int], Set<String>) {
        lock.lock()
        defer { lock.unlock() }
        recorded.append(request)
        return (repositories, redirects, unavailable, downHosts)
    }

    private func respond(_ request: GitHttpRequest, suffix: String, directory: String) throws -> GitHttpResponse {
        switch (request.method, suffix) {
        case ("GET", "/info/refs?service=git-upload-pack"):
            let body = try run(["upload-pack", "--stateless-rpc", "--http-backend-info-refs", directory], protocolV2: true)
            return ok("application/x-git-upload-pack-advertisement", body)
        case ("POST", "/git-upload-pack"):
            let body = try run(["upload-pack", "--stateless-rpc", directory], input: request.body, protocolV2: true)
            return ok("application/x-git-upload-pack-result", body)
        case ("GET", "/info/refs?service=git-receive-pack"):
            let banner = Array("001f# service=git-receive-pack\n0000".utf8)
            let body = try run(["receive-pack", "--stateless-rpc", "--http-backend-info-refs", directory])
            return ok("application/x-git-receive-pack-advertisement", banner + body)
        case ("POST", "/git-receive-pack"):
            let body = try run(["receive-pack", "--stateless-rpc", directory], input: request.body)
            return ok("application/x-git-receive-pack-result", body)
        default:
            throw classifyHttpFailure(request, statusCode: 404)
        }
    }

    private func ok(_ contentType: String, _ body: [UInt8]) -> GitHttpResponse {
        GitHttpResponse(statusCode: 200, headers: ["content-type": contentType], body: body)
    }

    private func run(_ arguments: [String], input: [UInt8] = [], protocolV2: Bool = false) throws -> [UInt8] {
        #if os(iOS) || os(tvOS) || os(watchOS)
        throw XCTSkip("no subprocesses on this platform")
        #else
        let process = Process()
        process.executableURL = URL(fileURLWithPath: git.path)
        process.arguments = arguments
        let home = NSTemporaryDirectory() + "forkmesh-git-home-\(ProcessInfo.processInfo.processIdentifier)"
        try? FileManager.default.createDirectory(atPath: home, withIntermediateDirectories: true)
        var environment = [
            "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
            "HOME": home,
            "XDG_CONFIG_HOME": home,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "LC_ALL": "C",
        ]
        if protocolV2 { environment["GIT_PROTOCOL"] = "version=2" }
        process.environment = environment
        let stdin = Pipe()
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardInput = stdin
        process.standardOutput = stdout
        process.standardError = stderr
        let exited = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in exited.signal() }
        try process.run()
        let group = DispatchGroup()
        let writer = stdin.fileHandleForWriting
        let payload = Data(input)
        DispatchQueue.global().async(group: group) {
            if !payload.isEmpty { writer.write(payload) }
            try? writer.close()
        }
        let errorBox = LocalDataBox()
        DispatchQueue.global().async(group: group) {
            errorBox.set(stderr.fileHandleForReading.readDataToEndOfFile())
        }
        let output = stdout.fileHandleForReading.readDataToEndOfFile()
        group.wait()
        exited.wait()
        if process.terminationStatus != 0 {
            throw LocalGitServerFailure(
                description: "git \(arguments.first ?? "") failed: " + String(decoding: errorBox.get(), as: UTF8.self))
        }
        return [UInt8](output)
        #endif
    }
}

struct LocalGitServerFailure: Error, CustomStringConvertible {
    let description: String
}

final class LocalDataBox: @unchecked Sendable {
    private let lock = NSLock()
    private var data = Data()

    func set(_ value: Data) {
        lock.lock()
        data = value
        lock.unlock()
    }

    func get() -> Data {
        lock.lock()
        defer { lock.unlock() }
        return data
    }
}

struct LocalGitOrigin {
    let git: LocalTestGit
    let work: String
    let bare: String

    static func make(_ test: XCTestCase, commits: Int) throws -> LocalGitOrigin {
        let git = try LocalTestGit.require()
        let root = try localTempDirectory(test, "fm-origin")
        let work = root.appendingPathComponent("work").path
        let bare = root.appendingPathComponent("origin.git").path
        try check(git.run(["init", "-q", "-b", "main", work]))
        let origin = LocalGitOrigin(git: git, work: work, bare: bare)
        for index in 1...max(commits, 1) {
            try origin.commit("step.txt", "step \(index)\n", "Step \(index)")
        }
        try check(git.run(["init", "-q", "--bare", "-b", "main", bare]))
        try check(git.run(["-C", bare, "config", "receive.denyNonFastForwards", "true"]))
        try check(git.run(["-C", work, "push", "-q", bare, "main"]))
        return origin
    }

    @discardableResult
    func commit(_ file: String, _ text: String, _ message: String) throws -> String {
        try localWrite(URL(fileURLWithPath: work).appendingPathComponent(file), text)
        try Self.check(git.run(["-C", work, "add", file]))
        try Self.check(git.run(["-C", work, "commit", "-q", "-m", message]))
        return try revParse("HEAD", in: work)
    }

    func branch(_ name: String, at revision: String) throws {
        try Self.check(git.run(["-C", work, "branch", "-f", name, revision]))
    }

    func publish(_ refs: String...) throws {
        try Self.check(git.run(["-C", work, "push", "-q", "-f", bare] + refs))
    }

    func tip(_ ref: String) throws -> String {
        try revParse(ref, in: bare)
    }

    func revParse(_ revision: String, in directory: String) throws -> String {
        let result = try git.run(["-C", directory, "rev-parse", revision])
        try Self.check(result)
        return result.out.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func check(_ result: (status: Int32, out: String, err: String)) throws {
        if result.status != 0 { throw LocalGitServerFailure(description: result.err) }
    }
}
