import FMBase
import FMNet
import XCTest
@testable import FMLocalAI

@MainActor
final class OnDeviceChatAccessTests: XCTestCase {
    private let gemma = LocalModelCatalog.gemma4E2B

    private func resolve(
        switchOn: Bool = true,
        engine: LocalEngineAvailability = .available,
        fit: LocalModelFit = .recommended,
        state: LocalModelState = .notDownloaded,
        isDownloaded: Bool = false,
        resumableBytes: Int64 = 0,
        totalBytes: Int64? = nil
    ) -> OnDeviceChatAccess {
        OnDeviceChatAccess.resolve(
            model: gemma,
            switchOn: switchOn,
            engine: engine,
            fit: fit,
            state: state,
            isDownloaded: isDownloaded,
            resumableBytes: resumableBytes,
            totalBytes: totalBytes
        )
    }

    func testUsableOpensTheChat() async {
        let access = resolve(state: .downloaded, isDownloaded: true)
        XCTAssertEqual(access, OnDeviceChatAccess(title: "Chat with Gemma 4 E2B", isEnabled: true, reason: nil, action: nil))
        XCTAssertTrue(resolve(fit: .tight(note: "Close other apps."), state: .ready, isDownloaded: true).isEnabled)
    }

    func testAFailedLoadStaysOpenWithItsReason() async {
        let access = resolve(state: .failed("Out of memory."), isDownloaded: true)
        XCTAssertTrue(access.isEnabled)
        XCTAssertEqual(access.reason, "Out of memory.")
        XCTAssertNil(access.action)
    }

    func testUnsupportedRuntimeHasNoAction() async {
        let access = resolve(switchOn: false, engine: .unavailable(reason: "No GPU here."), state: .downloaded, isDownloaded: true)
        XCTAssertFalse(access.isEnabled)
        XCTAssertEqual(access.reason, "Not supported on this device. No GPU here.")
        XCTAssertNil(access.action)
        XCTAssertEqual(access.title, "Chat with Gemma 4 E2B")
    }

    func testTooLittleMemoryHasNoAction() async {
        let access = resolve(switchOn: false, fit: .tooLarge(reason: "Needs 6 GB."))
        XCTAssertFalse(access.isEnabled)
        XCTAssertEqual(access.reason, "Needs 6 GB.")
        XCTAssertNil(access.action)
    }

    func testSwitchOffOffersTurnOn() async {
        let downloaded = resolve(switchOn: false, state: .downloaded, isDownloaded: true)
        XCTAssertFalse(downloaded.isEnabled)
        XCTAssertEqual(downloaded.reason, "On-device AI is off.")
        XCTAssertEqual(downloaded.action, .turnOn)
        XCTAssertEqual(downloaded.action?.label, "Turn on")

        let missing = resolve(switchOn: false)
        XCTAssertEqual(missing.reason, "On-device AI is off, and Gemma 4 E2B is not downloaded yet.")
        XCTAssertEqual(missing.action, .turnOn)
    }

    func testNotDownloadedOffersTheDownloadWithItsSize() async {
        let access = resolve()
        XCTAssertFalse(access.isEnabled)
        XCTAssertEqual(access.reason, "Gemma 4 E2B is not downloaded yet (3.6 GB).")
        XCTAssertEqual(access.action, .download(bytes: gemma.approxDownloadBytes))
        XCTAssertEqual(access.action?.label, "Download · 3.6 GB")
        XCTAssertNil(access.progress)
    }

    func testAPartialDownloadOffersResume() async {
        let access = resolve(resumableBytes: 1_200_000_000)
        XCTAssertEqual(access.reason, "Gemma 4 E2B is partly downloaded: 1.2 GB of 3.6 GB.")
        XCTAssertEqual(access.action, .resumeDownload(doneBytes: 1_200_000_000, totalBytes: gemma.approxDownloadBytes))
        XCTAssertEqual(access.action?.label, "Resume download")
    }

    func testDownloadingShowsProgressAndCancel() async {
        let access = resolve(state: .downloading(progress: 0.425, bytes: 1_500_000_000), totalBytes: 3_600_000_000)
        XCTAssertFalse(access.isEnabled)
        XCTAssertEqual(access.reason, "Downloading Gemma 4 E2B: 42% · 1.5 GB of 3.6 GB.")
        XCTAssertEqual(access.action, .cancelDownload)
        XCTAssertEqual(access.progress, 0.425)

        XCTAssertEqual(resolve(state: .downloading(progress: 3, bytes: 0)).progress, 1)
    }

    func testAFailedDownloadOffersItAgain() async {
        let fresh = resolve(state: .failed("The network went away."))
        XCTAssertEqual(fresh.reason, "The network went away.")
        XCTAssertEqual(fresh.action, .download(bytes: gemma.approxDownloadBytes))

        let partial = resolve(state: .failed("The network went away."), resumableBytes: 10)
        XCTAssertEqual(partial.action, .resumeDownload(doneBytes: 10, totalBytes: gemma.approxDownloadBytes))
    }

    func testAnUnsetModelDefaultsToGemma() async {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let store = LocalModelStore(root: temp.url)
        let downloader = HuggingFaceSnapshotDownloader(http: FakeHTTPClient(), files: FakeFileDownloader(), store: store)
        func manager(_ device: DeviceProfile) -> LocalModelManager {
            LocalModelManager(engine: FakeLocalChatEngine(), store: store, downloader: downloader, prefs: InMemoryKeyValueStore(), device: device)
        }
        XCTAssertEqual(manager(DeviceProfile(physicalMemory: 1_000_000)).selectedModel, LocalModelCatalog.gemma4E2B)
        XCTAssertEqual(manager(DeviceProfile(physicalMemory: 6_000_000_000)).selectedModel, LocalModelCatalog.gemma4E2B)
        XCTAssertEqual(manager(DeviceProfile(physicalMemory: 12_000_000_000)).selectedModel, LocalModelCatalog.gemma4E4B)
        XCTAssertEqual(manager(DeviceProfile(physicalMemory: 6_000_000_000)).chatAccess.title, "Chat with Gemma 4 E2B")
    }

    func testTheManagerWalksFromOffToUsable() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let store = LocalModelStore(root: temp.url)
        let downloader = HuggingFaceSnapshotDownloader(http: FakeHTTPClient(), files: FakeFileDownloader(), store: store)
        let manager = LocalModelManager(
            engine: FakeLocalChatEngine(), store: store, downloader: downloader, prefs: InMemoryKeyValueStore(), device: bigDevice, catalog: [tinyModel]
        )
        XCTAssertEqual(manager.chatAccess.action, .turnOn)
        manager.performChatAccess(.turnOn)
        XCTAssertTrue(manager.isEnabled)
        XCTAssertEqual(manager.chatAccess.action, .download(bytes: tinyModel.approxDownloadBytes))
        XCTAssertFalse(manager.chatAccess.isEnabled)

        let directory = store.directory(for: tinyModel)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data("{}".utf8).write(to: directory.appendingPathComponent("config.json"))
        try store.writeManifest(
            SnapshotManifest(repo: tinyModel.repo, revision: tinyModel.revision, files: [HubFile(path: "config.json", size: 2)], completedAtMs: 0),
            for: tinyModel
        )
        manager.refresh()
        XCTAssertEqual(manager.chatAccess, OnDeviceChatAccess(title: "Chat with Tiny", isEnabled: true, reason: nil, action: nil))
        XCTAssertEqual(manager.chatAccess.isEnabled, manager.isUsable)
    }
}
