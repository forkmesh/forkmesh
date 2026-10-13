import FMBase
import FMNet
import XCTest
@testable import FMLocalAI

@MainActor
final class LocalModelManagerTests: XCTestCase {
    private var temp: TempDirectory!
    private let weights = Data((0..<20).map { UInt8($0) })
    private let config = Data(#"{"model_type":"gemma4"}"#.utf8)
    private let template = Data("{{ messages }}".utf8)

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        temp.cleanUp()
    }

    private func makeManager(
        engine: FakeLocalChatEngine = FakeLocalChatEngine(),
        prefs: KeyValueStore = InMemoryKeyValueStore(),
        files: FakeFileDownloader? = nil,
        device: DeviceProfile = bigDevice,
        catalog: [LocalAIModel] = [tinyModel]
    ) -> (LocalModelManager, FakeFileDownloader) {
        let http = FakeHTTPClient()
        for model in catalog {
            http.serveListing(model, tinyListing(weights: weights, config: config, template: template))
        }
        let fake = files ?? FakeFileDownloader()
        if files == nil {
            fake.set("model.safetensors", .serve(weights))
            fake.set("config.json", .serve(config))
            fake.set("chat_template.jinja", .serve(template))
        }
        let store = LocalModelStore(root: temp.url)
        let downloader = HuggingFaceSnapshotDownloader(http: http, files: fake, store: store, progressGranularity: 1)
        let manager = LocalModelManager(engine: engine, store: store, downloader: downloader, prefs: prefs, device: device, catalog: catalog)
        return (manager, fake)
    }

    func testPreferencesPersistUnderLocalAIKeys() {
        let prefs = InMemoryKeyValueStore()
        let (manager, _) = makeManager(prefs: prefs, catalog: LocalModelCatalog.all)
        XCTAssertFalse(manager.isEnabled, "off until the user turns it on")
        XCTAssertEqual(manager.selectedModelId, "gemma-4-e4b-it-4bit", "12 GB device: the bigger model is recommended")

        manager.setEnabled(true)
        manager.select(LocalModelCatalog.gemma4E2B)
        XCTAssertEqual(prefs.bool(forKey: "localai/enabled"), true)
        XCTAssertEqual(prefs.string(forKey: "localai/model"), "gemma-4-e2b-it-4bit")

        let (reloaded, _) = makeManager(prefs: prefs, catalog: LocalModelCatalog.all)
        XCTAssertTrue(reloaded.isEnabled)
        XCTAssertEqual(reloaded.selectedModel, LocalModelCatalog.gemma4E2B)

        let unknown = InMemoryKeyValueStore()
        unknown.set("retired-model", forKey: "localai/model")
        let (fallback, _) = makeManager(prefs: unknown, device: DeviceProfile(physicalMemory: 8_000_000_000), catalog: LocalModelCatalog.all)
        XCTAssertEqual(fallback.selectedModelId, "gemma-4-e2b-it-4bit")
    }

    func testDownloadLoadUnloadDelete() async throws {
        let engine = FakeLocalChatEngine()
        let (manager, _) = makeManager(engine: engine)
        XCTAssertEqual(manager.state(for: tinyModel), .notDownloaded)
        XCTAssertTrue(manager.canDownload(tinyModel))

        manager.download(tinyModel)
        XCTAssertTrue(manager.state(for: tinyModel).isDownloading)
        await manager.waitForDownload(tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .downloaded)
        XCTAssertGreaterThan(manager.diskUsage[tinyModel.id] ?? 0, 0)
        XCTAssertNil(manager.downloadProgress[tinyModel.id])
        XCTAssertFalse(manager.canDownload(tinyModel))

        await manager.load(tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .ready)
        XCTAssertEqual(manager.loadedModelId, tinyModel.id)
        XCTAssertEqual(engine.loadedDirectories, [temp.url.appendingPathComponent("test-org/tiny-it-4bit", isDirectory: true)])

        // Loading the loaded model again is a no-op.
        await manager.load(tinyModel)
        XCTAssertEqual(engine.loadedDirectories.count, 1)

        await manager.unload()
        XCTAssertEqual(manager.state(for: tinyModel), .downloaded)
        XCTAssertNil(manager.loadedModelId)

        await manager.load(tinyModel)
        await manager.delete(tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .notDownloaded)
        XCTAssertNil(manager.loadedModelId)
        XCTAssertEqual(manager.diskUsage[tinyModel.id], 0)
        XCTAssertGreaterThanOrEqual(engine.unloadCount, 2)
    }

    func testDownloadFailureAndCancel() async throws {
        let files = FakeFileDownloader()
        files.set("config.json", .serve(config))
        files.set("chat_template.jinja", .serve(template))
        files.set("model.safetensors", .fail(.http(status: 403, url: "u")))
        let (manager, _) = makeManager(files: files)

        manager.download(tinyModel)
        await manager.waitForDownload(tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .failed("The model host refused the download (403)."))

        files.set("model.safetensors", .hangAfter(weights, after: 6))
        manager.download(tinyModel)
        let weightsPartial = manager.store.partialDirectory(for: tinyModel).appendingPathComponent("model.safetensors")
        while (manager.store.fileSize(weightsPartial) ?? 0) < 6 { try await Task.sleep(nanoseconds: 1_000_000) }
        manager.cancelDownload(tinyModel)
        await manager.waitForDownload(tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .notDownloaded, "a cancel is not a failure")
        XCTAssertEqual(manager.resumableBytes[tinyModel.id], 6)
    }

    func testRefusesWhatTheDeviceCannotRun() async {
        let (small, _) = makeManager(device: DeviceProfile(physicalMemory: 500_000))
        XCTAssertFalse(small.canDownload(tinyModel))
        small.download(tinyModel)
        guard case .failed(let reason) = small.state(for: tinyModel) else { return XCTFail("should refuse") }
        XCTAssertTrue(reason.contains("at least"), reason)

        let unavailable = FakeLocalChatEngine(availability: .unavailable(reason: "No GPU here."))
        let (simulator, _) = makeManager(engine: unavailable)
        XCTAssertEqual(simulator.fit(for: tinyModel), .unsupported(reason: "No GPU here."))
        XCTAssertNil(simulator.recommendedModel)
        simulator.setEnabled(true)
        do {
            try await simulator.ensureReady()
            XCTFail("unavailable engine")
        } catch {
            XCTAssertEqual(LocalModelManager.describe(error), "No GPU here.")
        }
    }

    func testEnsureReadyGatesOnSwitchDownloadAndLoad() async throws {
        let engine = FakeLocalChatEngine()
        let (manager, _) = makeManager(engine: engine)
        do {
            try await manager.ensureReady()
            XCTFail("switch is off")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .disabled)
        }
        manager.setEnabled(true)
        do {
            try await manager.ensureReady()
            XCTFail("not downloaded")
        } catch let error as LocalModelError {
            XCTAssertEqual(error, .notDownloaded("Tiny"))
        }
        let peerless = await manager.modelForPeers()
        XCTAssertNil(peerless)

        manager.download(tinyModel)
        await manager.waitForDownload(tinyModel)
        engine.failNextLoad(.loadFailed("bad weights"))
        do {
            try await manager.ensureReady()
            XCTFail("load fails")
        } catch {
            XCTAssertTrue(LocalModelManager.describe(error).contains("bad weights"))
        }
        guard case .failed = manager.state(for: tinyModel) else { return XCTFail("failed state") }

        let model = try await manager.ensureReady()
        XCTAssertEqual(model, tinyModel)
        XCTAssertEqual(manager.state(for: tinyModel), .ready)
        let forPeers = await manager.modelForPeers()
        XCTAssertEqual(forPeers, tinyModel)

        // Off unloads.
        manager.setEnabled(false)
        while manager.loadedModelId != nil { try await Task.sleep(nanoseconds: 1_000_000) }
        let loaded = await engine.loadedModelId()
        XCTAssertNil(loaded)
    }
}

extension LocalModelManagerTests {
    func testAFailedLoadIsStillDownloadedAndUsable() async throws {
        let engine = FakeLocalChatEngine()
        let (manager, _) = makeManager(engine: engine)
        manager.setEnabled(true)
        manager.download(tinyModel)
        await manager.waitForDownload(tinyModel)
        engine.failNextLoad(.loadFailed("x"))
        await manager.load(tinyModel)
        guard case .failed = manager.state(for: tinyModel) else { return XCTFail("failed") }
        XCTAssertTrue(manager.isDownloaded(tinyModel))
        XCTAssertTrue(manager.isUsable, "chat can retry the load")
        XCTAssertFalse(manager.canDownload(tinyModel))
    }
}
