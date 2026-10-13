import FMBase
import FMNet
import XCTest
@testable import FMLocalAI

final class HuggingFaceSnapshotTests: XCTestCase {
    private var temp: TempDirectory!
    private let weights = Data((0..<20).map { UInt8($0) })
    private let config = Data(#"{"model_type":"gemma4"}"#.utf8)
    private let template = Data("{{ messages }}".utf8)

    override func setUp() {
        temp = TempDirectory()
    }

    override func tearDown() {
        temp.cleanUp()
    }

    // MARK: - Pure parts

    func testGlob() {
        XCTAssertTrue(Glob.matches("*.safetensors", "model.safetensors"))
        XCTAssertTrue(Glob.matches("*.safetensors", "model-00001-of-00002.safetensors"))
        XCTAssertFalse(Glob.matches("*.safetensors", "model.safetensors.index.json"))
        XCTAssertTrue(Glob.matches("*.json", "model.safetensors.index.json"))
        XCTAssertTrue(Glob.matches("*.jinja", "chat_template.jinja"))
        XCTAssertFalse(Glob.matches("*.json", "README.md"))
        XCTAssertTrue(Glob.matches("tokenizer.*", "tokenizer.json"))
        XCTAssertTrue(Glob.matches("model?.bin", "model1.bin"))
        XCTAssertFalse(Glob.matches("model?.bin", "model12.bin"))
        XCTAssertTrue(Glob.matches("[ab]*", "b-file"))
        XCTAssertFalse(Glob.matches("[ab]*", "c-file"))
        XCTAssertTrue(Glob.matches("*", ""))
        XCTAssertFalse(Glob.matches("a", ""))
    }

    func testURLs() {
        XCTAssertEqual(
            HuggingFaceHub.listingURL(repo: "mlx-community/gemma-4-e2b-it-4bit", revision: "2387675").absoluteString,
            "https://huggingface.co/api/models/mlx-community/gemma-4-e2b-it-4bit/revision/2387675?blobs=true"
        )
        XCTAssertEqual(
            HuggingFaceHub.fileURL(repo: "mlx-community/gemma-4-e2b-it-4bit", revision: "2387675", path: "model.safetensors").absoluteString,
            "https://huggingface.co/mlx-community/gemma-4-e2b-it-4bit/resolve/2387675/model.safetensors"
        )
        XCTAssertEqual(
            HuggingFaceHub.fileURL(repo: "o/r", revision: "refs/pr/1", path: "a b.json").absoluteString,
            "https://huggingface.co/o/r/resolve/refs%2Fpr%2F1/a%20b.json"
        )
    }

    func testParseListingAndSelect() throws {
        let files = try HuggingFaceHub.parseListing(tinyListing(weights: weights, config: config, template: template))
        XCTAssertEqual(files.count, 6)
        XCTAssertEqual(files.first { $0.path == "model.safetensors" }?.size, 20)
        let selected = HuggingFaceHub.select(files, patterns: LocalAIModel.defaultFilePatterns)
        XCTAssertEqual(selected.map(\.path), ["config.json", "chat_template.jinja", "model.safetensors"])

        // Size only under lfs, or missing.
        let lfsOnly = try HuggingFaceHub.parseListing([
            "siblings": [["rfilename": "w.safetensors", "lfs": ["size": 99]], ["rfilename": "c.json"], ["size": 3]],
        ])
        XCTAssertEqual(lfsOnly, [HubFile(path: "w.safetensors", size: 99), HubFile(path: "c.json", size: nil)])
        XCTAssertThrowsError(try HuggingFaceHub.parseListing(["error": "Repository not found"]))
        XCTAssertThrowsError(try HuggingFaceHub.parseListing(.array([])))
    }

    func testManifestRoundTrip() {
        let manifest = SnapshotManifest(
            repo: "o/r", revision: "abc", files: [HubFile(path: "a.json", size: 3), HubFile(path: "b.json", size: nil)],
            completedAtMs: 1_700_000_000_000
        )
        XCTAssertEqual(SnapshotManifest(json: manifest.json), manifest)
        XCTAssertEqual(manifest.totalBytes, 3)
        XCTAssertNil(SnapshotManifest(json: ["files": .array([])]))
    }

    // MARK: - Snapshot download

    private func makeDownloader(_ http: FakeHTTPClient, _ files: FakeFileDownloader) -> (HuggingFaceSnapshotDownloader, LocalModelStore) {
        let store = LocalModelStore(root: temp.url)
        return (HuggingFaceSnapshotDownloader(http: http, files: files, store: store, progressGranularity: 1, clock: FixedClock(epochMillis: 42)), store)
    }

    private func serveAll(_ files: FakeFileDownloader) {
        files.set("model.safetensors", .serve(weights))
        files.set("config.json", .serve(config))
        files.set("chat_template.jinja", .serve(template))
    }

    func testDownloadsMatchingFilesAndWritesManifestLast() async throws {
        let http = FakeHTTPClient()
        http.serveListing(tinyModel, tinyListing(weights: weights, config: config, template: template))
        let files = FakeFileDownloader()
        serveAll(files)
        let (downloader, store) = makeDownloader(http, files)
        let seen = ProgressLog()

        let directory = try await downloader.download(tinyModel) { seen.add($0) }

        XCTAssertEqual(directory, temp.url.appendingPathComponent("test-org/tiny-it-4bit", isDirectory: true))
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.url.query, "blobs=true")
        XCTAssertEqual(Set(files.calls.map { $0.url.lastPathComponent }), ["config.json", "chat_template.jinja", "model.safetensors"])
        XCTAssertEqual(
            files.calls.first { $0.url.lastPathComponent == "model.safetensors" }?.url.absoluteString,
            "https://huggingface.co/test-org/tiny-it-4bit/resolve/abc123/model.safetensors"
        )
        XCTAssertEqual(files.calls.first { $0.url.lastPathComponent == "model.safetensors" }?.expectedBytes, 20)
        XCTAssertEqual(readFile(directory.appendingPathComponent("model.safetensors")), weights)
        XCTAssertEqual(readFile(directory.appendingPathComponent("config.json")), config)
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.partialDirectory(for: tinyModel).path))
        XCTAssertTrue(store.isComplete(tinyModel))
        let manifest = try XCTUnwrap(store.readManifest(for: tinyModel))
        XCTAssertEqual(manifest.revision, "abc123")
        XCTAssertEqual(manifest.completedAtMs, 42)
        XCTAssertEqual(manifest.totalBytes, Int64(weights.count + config.count + template.count))
        XCTAssertEqual(store.diskUsage(for: tinyModel), store.fileSize(store.manifestURL(for: tinyModel))! + manifest.totalBytes)

        let last = try XCTUnwrap(seen.values.last)
        XCTAssertEqual(last.fraction, 1)
        XCTAssertNil(last.currentFile)
        XCTAssertEqual(seen.values.map(\.completedBytes), seen.values.map(\.completedBytes).sorted())

        // Complete: a second download touches nothing.
        _ = try await downloader.download(tinyModel)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(files.calls.count, 3)

        try store.delete(tinyModel)
        XCTAssertFalse(store.isComplete(tinyModel))
        XCTAssertEqual(store.diskUsage(for: tinyModel), 0)
    }

    func testFailureKeepsPartialAndNextAttemptResumes() async throws {
        let http = FakeHTTPClient()
        http.serveListing(tinyModel, tinyListing(weights: weights, config: config, template: template))
        let files = FakeFileDownloader()
        serveAll(files)
        files.set("model.safetensors", .failAfter(weights, after: 8, error: .http(status: 503, url: "x")))
        let (downloader, store) = makeDownloader(http, files)

        do {
            try await downloader.download(tinyModel)
            XCTFail("expected the 503")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .http(status: 503, url: "x"))
        }
        XCTAssertFalse(store.isComplete(tinyModel))
        XCTAssertNil(store.readManifest(for: tinyModel))
        XCTAssertEqual(store.partialBytes(for: tinyModel), 8)

        files.set("model.safetensors", .serve(weights))
        let before = files.calls.count
        try await downloader.download(tinyModel)
        let retried = Array(files.calls[before...])
        // Files already in place are not fetched again; the weights resume at 8.
        XCTAssertEqual(retried.map { $0.url.lastPathComponent }, ["model.safetensors"])
        XCTAssertEqual(retried.first?.startOffset, 8)
        XCTAssertEqual(readFile(store.directory(for: tinyModel).appendingPathComponent("model.safetensors")), weights)
        XCTAssertTrue(store.isComplete(tinyModel))
    }

    func testCancellationKeepsPartialFile() async throws {
        let http = FakeHTTPClient()
        http.serveListing(tinyModel, tinyListing(weights: weights, config: config, template: template))
        let files = FakeFileDownloader()
        serveAll(files)
        files.set("model.safetensors", .hangAfter(weights, after: 5))
        let (downloader, store) = makeDownloader(http, files)

        let task = Task { try await downloader.download(tinyModel) }
        let weightsPartial = store.partialDirectory(for: tinyModel).appendingPathComponent("model.safetensors")
        while (store.fileSize(weightsPartial) ?? 0) < 5 { try await Task.sleep(nanoseconds: 1_000_000) }
        task.cancel()
        do {
            _ = try await task.value
            XCTFail("expected cancellation")
        } catch is CancellationError {}
        XCTAssertEqual(store.partialBytes(for: tinyModel), 5)
        XCTAssertFalse(store.isComplete(tinyModel))
    }

    func testSizeMismatchDropsTheFile() async throws {
        let http = FakeHTTPClient()
        http.serveListing(tinyModel, tinyListing(weights: weights, config: config, template: template))
        let files = FakeFileDownloader()
        serveAll(files)
        files.set("model.safetensors", .serve(weights.prefix(12)))
        let (downloader, store) = makeDownloader(http, files)

        do {
            try await downloader.download(tinyModel)
            XCTFail("expected a size mismatch")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .sizeMismatch(file: "model.safetensors", expected: 20, actual: 12))
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: store.partialDirectory(for: tinyModel).appendingPathComponent("model.safetensors").path))
    }

    func testListingErrors() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/models/\(tinyModel.repo)/revision/\(tinyModel.revision)", status: 404, json: ["error": "Revision Not Found"])
        let (downloader, _) = makeDownloader(http, FakeFileDownloader())
        do {
            try await downloader.download(tinyModel)
            XCTFail("expected 404")
        } catch let error as LocalModelError {
            guard case .http(404, _) = error else { return XCTFail("\(error)") }
        } catch {
            XCTFail("\(error)")
        }

        let empty = FakeHTTPClient()
        empty.serveListing(tinyModel, ["siblings": [["rfilename": "README.md"]]])
        let (emptyDownloader, _) = makeDownloader(empty, FakeFileDownloader())
        do {
            try await emptyDownloader.download(tinyModel)
            XCTFail("expected empty snapshot")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .emptySnapshot("test-org/tiny-it-4bit"))
        } catch {
            XCTFail("\(error)")
        }
    }

    func testStoreRejectsManifestOfAnotherRevision() throws {
        let store = LocalModelStore(root: temp.url)
        try FileManager.default.createDirectory(at: store.directory(for: tinyModel), withIntermediateDirectories: true)
        try Data("x".utf8).write(to: store.directory(for: tinyModel).appendingPathComponent("config.json"))
        try store.writeManifest(SnapshotManifest(repo: tinyModel.repo, revision: "old", files: [HubFile(path: "config.json", size: 1)], completedAtMs: 0), for: tinyModel)
        XCTAssertFalse(store.isComplete(tinyModel))
        try store.writeManifest(SnapshotManifest(repo: tinyModel.repo, revision: "abc123", files: [HubFile(path: "config.json", size: 2)], completedAtMs: 0), for: tinyModel)
        XCTAssertFalse(store.isComplete(tinyModel), "wrong size")
        try store.writeManifest(SnapshotManifest(repo: tinyModel.repo, revision: "abc123", files: [HubFile(path: "config.json", size: 1)], completedAtMs: 0), for: tinyModel)
        XCTAssertTrue(store.isComplete(tinyModel))
        XCTAssertNotNil(store.availableCapacity())
    }

    // MARK: - HTTPFileDownloader

    func testHTTPFileDownloaderFreshAndResumed() async throws {
        let body = Data((0..<100).map { UInt8($0) })
        let http = FakeHTTPClient { request in
            if let range = request.headers["Range"] {
                XCTAssertEqual(range, "bytes=40-")
                return HTTPResponse(status: 206, body: body.subdata(in: 40..<100))
            }
            return HTTPResponse(status: 200, body: body)
        }
        let downloader = HTTPFileDownloader(http: http)
        let partial = temp.url.appendingPathComponent("p/model.part")
        let url = URL(string: "https://example.test/model")!

        let fresh = try await downloader.download(url, to: partial, expectedBytes: 100) { _ in }
        XCTAssertEqual(fresh, 100)
        XCTAssertEqual(readFile(partial), body)
        XCTAssertNil(http.requests[0].headers["Range"])
        XCTAssertEqual(http.requests[0].maxResponseBytes, Int.max)

        try body.prefix(40).write(to: partial)
        let resumed = try await downloader.download(url, to: partial, expectedBytes: 100) { _ in }
        XCTAssertEqual(resumed, 100)
        XCTAssertEqual(readFile(partial), body)

        // Already complete: no request at all.
        let count = http.requests.count
        let again = try await downloader.download(url, to: partial, expectedBytes: 100) { _ in }
        XCTAssertEqual(again, 100)
        XCTAssertEqual(http.requests.count, count)
    }

    func testHTTPFileDownloaderRestartsWhenServerIgnoresRange() async throws {
        let body = Data((0..<50).map { UInt8($0) })
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, body: body) }
        let downloader = HTTPFileDownloader(http: http)
        let partial = temp.url.appendingPathComponent("model.part")
        try Data(repeating: 0xFF, count: 30).write(to: partial)

        let size = try await downloader.download(URL(string: "https://example.test/m")!, to: partial, expectedBytes: 50) { _ in }
        XCTAssertEqual(size, 50)
        XCTAssertEqual(readFile(partial), body)
        XCTAssertEqual(http.requests.first?.headers["Range"], "bytes=30-")
    }

    func testHTTPFileDownloader416AndErrors() async throws {
        let partial = temp.url.appendingPathComponent("model.part")
        try Data(repeating: 1, count: 10).write(to: partial)
        let done = FakeHTTPClient { _ in HTTPResponse(status: 416) }
        let size = try await HTTPFileDownloader(http: done).download(URL(string: "https://example.test/m")!, to: partial, expectedBytes: nil) { _ in }
        XCTAssertEqual(size, 10)

        let missing = FakeHTTPClient { _ in HTTPResponse(status: 404) }
        do {
            _ = try await HTTPFileDownloader(http: missing).download(URL(string: "https://example.test/m")!, to: partial, expectedBytes: 99) { _ in }
            XCTFail("expected 404")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .http(status: 404, url: "https://example.test/m"))
        }
    }
}

final class ProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [SnapshotProgress] = []

    func add(_ progress: SnapshotProgress) {
        lock.lock()
        stored.append(progress)
        lock.unlock()
    }

    var values: [SnapshotProgress] {
        lock.lock()
        defer { lock.unlock() }
        return stored
    }
}
