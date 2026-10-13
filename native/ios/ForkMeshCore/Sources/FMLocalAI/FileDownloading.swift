// Streaming a multi-gigabyte file to disk, resumably.
//
// FMNet's `HTTPClient.stream` already streams (its timeout covers only the
// response head, and nothing buffers the body), so the production downloader
// is built on it rather than on a second URLSession wrapper: the same client,
// the same cancellation, and FakeHTTPClient in tests.

import FMBase
import FMNet
import Foundation

public protocol FileDownloading: Sendable {
    /// Downloads `url` into `partialFile`, continuing from the bytes already
    /// in it when the server honours a Range request, and returns the file's
    /// final size. `progress` gets the bytes on disk so far. Cancelling the
    /// task stops the transfer and keeps what arrived, for the next attempt.
    func download(
        _ url: URL,
        to partialFile: URL,
        expectedBytes: Int64?,
        progress: @escaping @Sendable (Int64) -> Void
    ) async throws -> Int64
}

public enum LocalModelError: Error, Equatable, Sendable, CustomStringConvertible {
    /// The Hugging Face API or a file answered with this status.
    case http(status: Int, url: String)
    case badListing(String)
    /// No file of the snapshot matched the patterns.
    case emptySnapshot(String)
    case sizeMismatch(file: String, expected: Int64, actual: Int64)
    case insufficientStorage(needed: Int64, available: Int64)
    case notDownloaded(String)
    case engineUnavailable(String)
    /// The "On-device AI" switch is off.
    case disabled
    case io(String)

    public var description: String {
        switch self {
        case .http(let status, let url):
            if status == 401 || status == 403 { return "The model host refused the download (\(status))." }
            if status == 404 { return "The model host does not have this file any more (\(url))." }
            return "The model host answered \(status) for \(url)."
        case .badListing(let detail): return "The model host sent a file list this app cannot read (\(detail))."
        case .emptySnapshot(let repo): return "\(repo) has no files this app can load."
        case .sizeMismatch(let file, let expected, let actual):
            return "\(file) arrived with \(actual) bytes instead of \(expected). Try the download again."
        case .insufficientStorage(let needed, let available):
            return "Not enough free storage: the download needs \(LocalAIFormat.bytes(needed)) more and \(LocalAIFormat.bytes(available)) is free."
        case .notDownloaded(let name): return "\(name) is not downloaded yet."
        case .engineUnavailable(let reason): return reason
        case .disabled: return "On-device AI is off. Turn it on in Settings, On-device AI."
        case .io(let detail): return "Could not write the model to storage (\(detail))."
        }
    }
}

/// `FileDownloading` over any `HTTPClient` (production: `URLSessionHTTPClient`).
public struct HTTPFileDownloader: FileDownloading {
    public let http: HTTPClient
    /// How long to wait for the response head; the body has no deadline.
    public let headTimeout: TimeInterval

    public init(http: HTTPClient, headTimeout: TimeInterval = 60) {
        self.http = http
        self.headTimeout = headTimeout
    }

    public func download(
        _ url: URL,
        to partialFile: URL,
        expectedBytes: Int64?,
        progress: @escaping @Sendable (Int64) -> Void
    ) async throws -> Int64 {
        try await attempt(url, to: partialFile, expectedBytes: expectedBytes, progress: progress, allowRestart: true)
    }

    private func attempt(
        _ url: URL,
        to partialFile: URL,
        expectedBytes: Int64?,
        progress: @escaping @Sendable (Int64) -> Void,
        allowRestart: Bool
    ) async throws -> Int64 {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: partialFile.deletingLastPathComponent(), withIntermediateDirectories: true)
        var offset = HTTPFileDownloader.size(of: partialFile)
        if let expected = expectedBytes, offset == expected, expected > 0 {
            progress(offset)
            return offset
        }
        if let expected = expectedBytes, offset > expected {
            try? fileManager.removeItem(at: partialFile)
            offset = 0
        }

        var headers: HTTPHeaders = [:]
        if offset > 0 { headers["Range"] = "bytes=\(offset)-" }
        let request = HTTPRequest(
            method: "GET", url: url, headers: headers, timeout: headTimeout,
            followRedirects: true, maxResponseBytes: Int.max
        )
        let response = try await http.stream(request)

        let append: Bool
        switch response.status {
        case 206:
            append = offset > 0
        case 200:
            append = false
        case 416 where offset > 0:
            // Nothing past what we hold: either it is complete or the partial
            // file belongs to something else. Start over once.
            if expectedBytes == nil || expectedBytes == offset {
                progress(offset)
                return offset
            }
            try? fileManager.removeItem(at: partialFile)
            guard allowRestart else { throw LocalModelError.http(status: 416, url: url.absoluteString) }
            return try await attempt(url, to: partialFile, expectedBytes: expectedBytes, progress: progress, allowRestart: false)
        default:
            throw LocalModelError.http(status: response.status, url: url.absoluteString)
        }

        if !append {
            offset = 0
            _ = fileManager.createFile(atPath: partialFile.path, contents: nil)
        } else if !fileManager.fileExists(atPath: partialFile.path) {
            _ = fileManager.createFile(atPath: partialFile.path, contents: nil)
        }

        let handle: FileHandle
        do {
            handle = try FileHandle(forWritingTo: partialFile)
        } catch {
            throw LocalModelError.io(error.localizedDescription)
        }
        defer { try? handle.close() }
        do {
            if append {
                _ = try handle.seekToEnd()
            } else {
                try handle.truncate(atOffset: 0)
            }
        } catch {
            throw LocalModelError.io(error.localizedDescription)
        }

        var written = offset
        progress(written)
        for try await chunk in response.body {
            try Task.checkCancellation()
            if chunk.isEmpty { continue }
            do {
                try handle.write(contentsOf: chunk)
            } catch {
                throw LocalModelError.io(error.localizedDescription)
            }
            written += Int64(chunk.count)
            progress(written)
        }
        try Task.checkCancellation()
        return written
    }

    static func size(of url: URL) -> Int64 {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path) else { return 0 }
        if let number = attributes[.size] as? NSNumber { return number.int64Value }
        return 0
    }
}

/// A `FileDownloading` for tests: serves bodies from memory, records calls,
/// and can fail, stop part-way or wait.
public final class FakeFileDownloader: FileDownloading, @unchecked Sendable {
    public struct Call: Sendable, Equatable {
        public let url: URL
        public let partialFile: URL
        public let startOffset: Int64
        public let expectedBytes: Int64?
    }

    public enum Behavior: Sendable {
        /// Writes the rest of `body` (from the partial file's size on).
        case serve(Data)
        /// Writes `body` up to `after` bytes, then throws `error`.
        case failAfter(Data, after: Int, error: LocalModelError)
        /// Writes `body` up to `after` bytes, then waits until cancelled.
        case hangAfter(Data, after: Int)
        case fail(LocalModelError)
    }

    private let lock = NSLock()
    private var behaviors: [String: Behavior] = [:]
    private var recorded: [Call] = []

    public init() {}

    /// Behaviour for URLs whose last path component is `fileName`.
    public func set(_ fileName: String, _ behavior: Behavior) {
        lock.lock()
        behaviors[fileName] = behavior
        lock.unlock()
    }

    public var calls: [Call] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    public func download(
        _ url: URL,
        to partialFile: URL,
        expectedBytes: Int64?,
        progress: @escaping @Sendable (Int64) -> Void
    ) async throws -> Int64 {
        try FileManager.default.createDirectory(
            at: partialFile.deletingLastPathComponent(), withIntermediateDirectories: true
        )
        let start = HTTPFileDownloader.size(of: partialFile)
        let behavior: Behavior? = lock.withLock {
            recorded.append(Call(url: url, partialFile: partialFile, startOffset: start, expectedBytes: expectedBytes))
            return behaviors[url.lastPathComponent]
        }

        switch behavior {
        case .none:
            throw LocalModelError.http(status: 404, url: url.absoluteString)
        case .fail(let error)?:
            throw error
        case .serve(let body)?:
            try write(body, from: Int(start), upTo: body.count, into: partialFile)
            progress(Int64(body.count))
            return Int64(body.count)
        case .failAfter(let body, let after, let error)?:
            try write(body, from: Int(start), upTo: after, into: partialFile)
            progress(Int64(max(after, Int(start))))
            throw error
        case .hangAfter(let body, let after)?:
            try write(body, from: Int(start), upTo: after, into: partialFile)
            progress(Int64(max(after, Int(start))))
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 2_000_000)
            }
            throw CancellationError()
        }
    }

    private func write(_ body: Data, from start: Int, upTo end: Int, into file: URL) throws {
        let existing = (try? Data(contentsOf: file)) ?? Data()
        let upper = min(max(end, start), body.count)
        let lower = min(start, upper)
        var combined = existing.prefix(lower)
        combined.append(body.subdata(in: lower..<upper))
        try combined.write(to: file)
    }
}
