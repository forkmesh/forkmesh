// A Hugging Face snapshot download without the Hub SDK: one API call lists
// the files at a revision, then each file matching the patterns streams to
// disk from its `resolve` URL. First-party code over FMNet, so the only
// third-party code in the app stays MLX and its tokenizer.

import FMBase
import FMNet
import Foundation

/// One file in a repository at a revision.
public struct HubFile: Sendable, Equatable, Hashable {
    /// Path in the repository ("model.safetensors").
    public var path: String
    /// Size in bytes, when the API reported it.
    public var size: Int64?

    public init(path: String, size: Int64?) {
        self.path = path
        self.size = size
    }
}

public enum HuggingFaceHub {
    public static let defaultEndpoint = URL(string: "https://huggingface.co")!

    /// `GET /api/models/<repo>/revision/<rev>?blobs=true`: `siblings` lists
    /// every file, and `blobs=true` adds sizes (`size`, and `lfs.size` for
    /// LFS files).
    public static func listingURL(repo: String, revision: String, endpoint: URL = defaultEndpoint) -> URL {
        var components = URLComponents(url: endpoint, resolvingAgainstBaseURL: false)!
        components.percentEncodedPath = "/api/models/\(repo)/revision/\(escape(revision))"
        components.queryItems = [URLQueryItem(name: "blobs", value: "true")]
        return components.url!
    }

    /// `/<repo>/resolve/<rev>/<path>`; answers with a redirect to the CDN.
    public static func fileURL(repo: String, revision: String, path: String, endpoint: URL = defaultEndpoint) -> URL {
        var components = URLComponents(url: endpoint, resolvingAgainstBaseURL: false)!
        let encodedPath = path.split(separator: "/", omittingEmptySubsequences: false).map { escape(String($0)) }.joined(separator: "/")
        components.percentEncodedPath = "/\(repo)/resolve/\(escape(revision))/\(encodedPath)"
        return components.url!
    }

    private static func escape(_ segment: String) -> String {
        var allowed = CharacterSet.urlPathAllowed
        allowed.remove(charactersIn: "/?#")
        return segment.addingPercentEncoding(withAllowedCharacters: allowed) ?? segment
    }

    /// The `siblings` of a listing, with sizes where present.
    public static func parseListing(_ value: JSONValue) throws -> [HubFile] {
        guard let object = value.objectValue else { throw LocalModelError.badListing("not an object") }
        guard let siblings = object["siblings"]?.arrayValue else { throw LocalModelError.badListing("no siblings") }
        return siblings.compactMap { sibling -> HubFile? in
            guard let entry = sibling.objectValue, let path = entry.optStr("rfilename"), !path.isEmpty else { return nil }
            let size = entry["size"]?.int64Value ?? entry.obj("lfs")["size"]?.int64Value
            return HubFile(path: path, size: size)
        }
    }

    /// Files a snapshot takes: top-level paths matching any pattern.
    /// Subdirectories are skipped (MLX repositories keep everything at the
    /// top; a stray folder is never what the loader wants).
    public static func select(_ files: [HubFile], patterns: [String]) -> [HubFile] {
        files.filter { file in
            !file.path.contains("/") && patterns.contains { Glob.matches($0, file.path) }
        }
    }
}

/// fnmatch-style patterns: `*` any run, `?` one character, `[abc]` a set.
public enum Glob {
    public static func matches(_ pattern: String, _ text: String) -> Bool {
        match(Array(pattern), 0, Array(text), 0)
    }

    private static func match(_ p: [Character], _ pi: Int, _ t: [Character], _ ti: Int) -> Bool {
        var pi = pi
        var ti = ti
        var starP = -1
        var starT = -1
        while ti < t.count {
            if pi < p.count, p[pi] == "*" {
                starP = pi
                starT = ti
                pi += 1
                continue
            }
            if pi < p.count, let step = single(p, pi, t[ti]), step.matched {
                pi += step.width
                ti += 1
                continue
            }
            if starP >= 0 {
                pi = starP + 1
                starT += 1
                ti = starT
                continue
            }
            return false
        }
        while pi < p.count, p[pi] == "*" { pi += 1 }
        return pi == p.count
    }

    /// Whether the pattern element at `pi` matches `c`, and how many pattern
    /// characters it spans. nil for `*`.
    private static func single(_ p: [Character], _ pi: Int, _ c: Character) -> (width: Int, matched: Bool)? {
        let head = p[pi]
        if head == "?" { return (width: 1, matched: true) }
        if head == "[", let close = p[(pi + 1)...].firstIndex(of: "]"), close > pi + 1 {
            let set = p[(pi + 1)..<close]
            return (width: close - pi + 1, matched: set.contains(c))
        }
        return (width: 1, matched: head == c)
    }
}

/// Progress of a whole snapshot.
public struct SnapshotProgress: Sendable, Equatable {
    public var completedBytes: Int64
    public var totalBytes: Int64
    public var currentFile: String?

    public init(completedBytes: Int64, totalBytes: Int64, currentFile: String?) {
        self.completedBytes = completedBytes
        self.totalBytes = totalBytes
        self.currentFile = currentFile
    }

    public var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return min(max(Double(completedBytes) / Double(totalBytes), 0), 1)
    }
}

/// Downloads a catalog model into its `LocalModelStore` directory.
///
/// Resumable (each file continues from its partial copy), skip-complete (a
/// file already in place at its listed size is not fetched again), atomic (a
/// file moves into place only once complete, and the manifest is written
/// last), cancellable (cancel the task; partial files stay for next time).
public final class HuggingFaceSnapshotDownloader: @unchecked Sendable {
    public let http: HTTPClient
    public let files: FileDownloading
    public let store: LocalModelStore
    public let endpoint: URL
    /// Progress is reported at most this often (in bytes), so a multi-gigabyte
    /// download does not flood the main actor with 100 000 updates.
    public let progressGranularity: Int64
    private let clock: Clock

    public init(
        http: HTTPClient,
        files: FileDownloading? = nil,
        store: LocalModelStore,
        endpoint: URL = HuggingFaceHub.defaultEndpoint,
        progressGranularity: Int64 = 4 * 1024 * 1024,
        clock: Clock = SystemClock()
    ) {
        self.http = http
        self.files = files ?? HTTPFileDownloader(http: http)
        self.store = store
        self.endpoint = endpoint
        self.progressGranularity = progressGranularity
        self.clock = clock
    }

    /// The files the model's patterns select at its revision.
    public func listFiles(for model: LocalAIModel) async throws -> [HubFile] {
        let url = HuggingFaceHub.listingURL(repo: model.repo, revision: model.revision, endpoint: endpoint)
        let response = try await http.send(HTTPRequest(method: "GET", url: url, timeout: 30))
        guard response.ok else { throw LocalModelError.http(status: response.status, url: url.absoluteString) }
        let value: JSONValue
        do {
            value = try response.json()
        } catch {
            throw LocalModelError.badListing("not JSON")
        }
        let selected = HuggingFaceHub.select(try HuggingFaceHub.parseListing(value), patterns: model.filePatterns)
        guard !selected.isEmpty else { throw LocalModelError.emptySnapshot(model.repo) }
        return selected
    }

    /// Downloads what is missing and returns the model directory.
    @discardableResult
    public func download(
        _ model: LocalAIModel,
        progress: @escaping @Sendable (SnapshotProgress) -> Void = { _ in }
    ) async throws -> URL {
        let directory = store.directory(for: model)
        if store.isComplete(model) {
            let total = store.readManifest(for: model)?.totalBytes ?? 0
            progress(SnapshotProgress(completedBytes: total, totalBytes: total, currentFile: nil))
            return directory
        }
        let listed = try await listFiles(for: model)
        try Task.checkCancellation()
        do {
            try store.ensureDirectories(for: model)
        } catch {
            throw LocalModelError.io(error.localizedDescription)
        }
        // A manifest from another revision must not survive a new download.
        store.removeManifest(for: model)

        let total = listed.reduce(Int64(0)) { $0 + ($1.size ?? 0) }
        var done: Int64 = 0
        var pending: [HubFile] = []
        for file in listed {
            let final = directory.appendingPathComponent(file.path)
            if let size = store.fileSize(final), let expected = file.size, size == expected {
                done += size
            } else {
                pending.append(file)
            }
        }

        let partialDirectory = store.partialDirectory(for: model)
        let held = pending.reduce(Int64(0)) { sum, file in
            sum + (store.fileSize(partialDirectory.appendingPathComponent(file.path)) ?? 0)
        }
        let stillNeeded = max(total - done - held, 0)
        if let available = store.availableCapacity(), stillNeeded > 0, available < stillNeeded {
            throw LocalModelError.insufficientStorage(needed: stillNeeded, available: available)
        }

        progress(SnapshotProgress(completedBytes: done, totalBytes: total, currentFile: pending.first?.path))
        let reporter = ProgressThrottle(granularity: progressGranularity)
        for file in pending {
            try Task.checkCancellation()
            let partial = partialDirectory.appendingPathComponent(file.path)
            let url = HuggingFaceHub.fileURL(repo: model.repo, revision: model.revision, path: file.path, endpoint: endpoint)
            let base = done
            let name = file.path
            let size = try await files.download(url, to: partial, expectedBytes: file.size) { bytes in
                if reporter.shouldReport(bytes) {
                    progress(SnapshotProgress(completedBytes: base + bytes, totalBytes: total, currentFile: name))
                }
            }
            if let expected = file.size, size != expected {
                // A wrong size is not resumable; drop it so the retry starts clean.
                try? FileManager.default.removeItem(at: partial)
                throw LocalModelError.sizeMismatch(file: file.path, expected: expected, actual: size)
            }
            do {
                try store.promote(partial, to: directory.appendingPathComponent(file.path))
            } catch {
                throw LocalModelError.io(error.localizedDescription)
            }
            done += size
            reporter.reset()
            progress(SnapshotProgress(completedBytes: done, totalBytes: total, currentFile: name))
        }

        let manifest = SnapshotManifest(
            repo: model.repo,
            revision: model.revision,
            files: listed.map { file in
                HubFile(path: file.path, size: file.size ?? store.fileSize(directory.appendingPathComponent(file.path)))
            },
            completedAtMs: clock.nowMillis
        )
        do {
            try store.writeManifest(manifest, for: model)
        } catch {
            throw LocalModelError.io(error.localizedDescription)
        }
        store.removePartialDirectory(for: model)
        progress(SnapshotProgress(completedBytes: total, totalBytes: total, currentFile: nil))
        return directory
    }
}

/// Lets a progress value through once it has moved `granularity` bytes.
final class ProgressThrottle: @unchecked Sendable {
    private let lock = NSLock()
    private let granularity: Int64
    private var last: Int64 = -1

    init(granularity: Int64) { self.granularity = max(granularity, 1) }

    func shouldReport(_ bytes: Int64) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if last < 0 || bytes - last >= granularity || bytes < last {
            last = bytes
            return true
        }
        return false
    }

    func reset() {
        lock.lock()
        last = -1
        lock.unlock()
    }
}
