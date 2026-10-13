// Runs the real `git` for interoperability checks (as the Kotlin port's
// GitInteropSupport.kt does). Tests that
// use it skip when git is not installed; iOS has no Process at all.

import Foundation
import XCTest

struct GitBinary {
    let path: String

    static func locate() -> GitBinary? {
        #if os(iOS) || os(tvOS) || os(watchOS)
        return nil
        #else
        for candidate in ["/usr/bin/git", "/usr/local/bin/git", "/opt/homebrew/bin/git", "/bin/git"]
        where FileManager.default.isExecutableFile(atPath: candidate) {
            return GitBinary(path: candidate)
        }
        return nil
        #endif
    }

    @discardableResult
    func run(_ arguments: [String], input: [UInt8]? = nil, directory: String? = nil) throws -> String {
        String(decoding: try runBytes(arguments, input: input, directory: directory), as: UTF8.self)
    }

    func runBytes(_ arguments: [String], input: [UInt8]? = nil, directory: String? = nil) throws -> [UInt8] {
        #if os(iOS) || os(tvOS) || os(watchOS)
        throw XCTSkip("no subprocesses on this platform")
        #else
        let result = try runHermeticGit(path, arguments, input: input, directory: directory, fixedDates: true)
        if result.status != 0 {
            throw NSError(domain: "git", code: Int(result.status), userInfo: [
                NSLocalizedDescriptionKey: "git \(arguments.joined(separator: " ")): "
                    + String(decoding: result.errors, as: UTF8.self),
            ])
        }
        return [UInt8](result.output)
        #endif
    }
}

#if !(os(iOS) || os(tvOS) || os(watchOS))
private let hermeticGitHome: String = {
    let path = NSTemporaryDirectory() + "forkmesh-git-home-\(ProcessInfo.processInfo.processIdentifier)"
    try? FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
    return URL(fileURLWithPath: path).resolvingSymlinksInPath().path
}()

func runHermeticGit(
    _ path: String,
    _ arguments: [String],
    input: [UInt8]? = nil,
    directory: String? = nil,
    fixedDates: Bool = false
) throws -> (status: Int32, output: Data, errors: Data) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: path)
    process.arguments = arguments
    if let directory { process.currentDirectoryURL = URL(fileURLWithPath: directory) }
    var environment = [
        "PATH": "/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin",
        "HOME": hermeticGitHome,
        "XDG_CONFIG_HOME": hermeticGitHome,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_ATTR_NOSYSTEM": "1",
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_AUTHOR_NAME": "Interop", "GIT_AUTHOR_EMAIL": "interop@example.test",
        "GIT_COMMITTER_NAME": "Interop", "GIT_COMMITTER_EMAIL": "interop@example.test",
        "LC_ALL": "C",
    ]
    if fixedDates {
        environment["GIT_AUTHOR_DATE"] = "1700000000 +0000"
        environment["GIT_COMMITTER_DATE"] = "1700000000 +0000"
    }
    if let developer = ProcessInfo.processInfo.environment["DEVELOPER_DIR"] {
        environment["DEVELOPER_DIR"] = developer
    }
    process.environment = environment
    let output = Pipe()
    let errors = Pipe()
    let stdin = Pipe()
    process.standardOutput = output
    process.standardError = errors
    process.standardInput = stdin
    let exited = DispatchSemaphore(value: 0)
    process.terminationHandler = { _ in exited.signal() }
    try process.run()

    let drained = DispatchGroup()
    let lock = NSLock()
    var errorData = Data()
    DispatchQueue.global().async(group: drained) {
        let data = errors.fileHandleForReading.readDataToEndOfFile()
        lock.lock()
        errorData = data
        lock.unlock()
    }
    if let input { stdin.fileHandleForWriting.write(Data(input)) }
    try? stdin.fileHandleForWriting.close()
    let outputData = output.fileHandleForReading.readDataToEndOfFile()
    drained.wait()
    exited.wait()
    lock.lock()
    defer { lock.unlock() }
    return (process.terminationStatus, outputData, errorData)
}
#endif
