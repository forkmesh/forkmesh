import XCTest
@testable import FMLocalAI

final class LocalAIModelTests: XCTestCase {
    func testCatalogPinsGemma4Revisions() {
        XCTAssertEqual(LocalModelCatalog.defaultModel, LocalModelCatalog.gemma4E2B)
        XCTAssertEqual(LocalModelCatalog.all.map(\.id), ["gemma-4-e2b-it-4bit", "gemma-4-e4b-it-4bit"])
        XCTAssertEqual(Set(LocalModelCatalog.all.map(\.id)).count, LocalModelCatalog.all.count)
        for model in LocalModelCatalog.all {
            XCTAssertTrue(model.repo.hasPrefix("mlx-community/gemma-4-"))
            XCTAssertEqual(model.revision.count, 40, "a commit, not a branch")
            XCTAssertEqual(model.filePatterns, ["*.safetensors", "*.json", "*.jinja"])
            XCTAssertEqual(model.contextLength, 131_072)
            XCTAssertLessThan(model.approxMemoryBytes, model.approxDownloadBytes)
            XCTAssertLessThan(model.minimumPhysicalMemory, model.recommendedPhysicalMemory)
        }
        XCTAssertEqual(LocalModelCatalog.model(id: "gemma-4-e4b-it-4bit"), LocalModelCatalog.gemma4E4B)
        XCTAssertNil(LocalModelCatalog.model(id: "nope"))
    }

    func testFitBySimulatorAndMemory() {
        let e2b = LocalModelCatalog.gemma4E2B
        let e4b = LocalModelCatalog.gemma4E4B

        let simulator = DeviceProfile(physicalMemory: 16_000_000_000, isSimulator: true)
        XCTAssertFalse(simulator.fit(for: e2b).canRun)
        XCTAssertTrue(simulator.fit(for: e2b).message?.contains("Simulator") ?? false)

        let fourGB = DeviceProfile(physicalMemory: 4_000_000_000)
        guard case .tooLarge(let reason) = fourGB.fit(for: e2b) else { return XCTFail("4 GB runs E2B?") }
        XCTAssertTrue(reason.contains("6 GB"), reason)
        XCTAssertTrue(reason.contains("4 GB"), reason)

        let sixGB = DeviceProfile(physicalMemory: 6_000_000_000)
        guard case .tight = sixGB.fit(for: e2b) else { return XCTFail("6 GB should be tight for E2B") }
        XCTAssertFalse(sixGB.fit(for: e4b).canRun)

        let eightGB = DeviceProfile(physicalMemory: 8_050_000_000)
        XCTAssertEqual(eightGB.fit(for: e2b), .recommended)
        guard case .tight = eightGB.fit(for: e4b) else { return XCTFail("8 GB should be tight for E4B") }

        let twelveGB = DeviceProfile(physicalMemory: 12_000_000_000)
        XCTAssertEqual(twelveGB.fit(for: e4b), .recommended)

        // Little free right now: a caveat even on a big phone.
        let busy = DeviceProfile(physicalMemory: 12_000_000_000, availableMemory: 1_500_000_000)
        guard case .tight(let note) = busy.fit(for: e2b) else { return XCTFail("low free memory should warn") }
        XCTAssertTrue(note.contains("Close other apps"), note)
    }

    func testRecommendedModelIsLargestComfortableOne() {
        XCTAssertEqual(LocalModelCatalog.recommendedModel(for: DeviceProfile(physicalMemory: 12_000_000_000)), LocalModelCatalog.gemma4E4B)
        XCTAssertEqual(LocalModelCatalog.recommendedModel(for: DeviceProfile(physicalMemory: 8_000_000_000)), LocalModelCatalog.gemma4E2B)
        XCTAssertEqual(LocalModelCatalog.recommendedModel(for: DeviceProfile(physicalMemory: 6_000_000_000)), LocalModelCatalog.gemma4E2B)
        XCTAssertNil(LocalModelCatalog.recommendedModel(for: DeviceProfile(physicalMemory: 4_000_000_000)))
        XCTAssertNil(LocalModelCatalog.recommendedModel(for: DeviceProfile(physicalMemory: 12_000_000_000, isSimulator: true)))
    }

    func testFormatting() {
        XCTAssertEqual(LocalAIFormat.bytes(3_583_086_498), "3.6 GB")
        XCTAssertEqual(LocalAIFormat.bytes(32_169_626), "32 MB")
        XCTAssertEqual(LocalAIFormat.bytes(17_336), "17 KB")
        XCTAssertEqual(LocalAIFormat.bytes(12), "12 B")
        XCTAssertEqual(LocalAIFormat.bytes(-5), "0 B")
        XCTAssertEqual(LocalAIFormat.memory(5_968_297_984), "6 GB")
        XCTAssertEqual(LocalAIFormat.tokensPerSecond(38.25), "38.2 tok/s")
        XCTAssertEqual(LocalAIFormat.tokensPerSecond(.nan), "– tok/s")
        XCTAssertEqual(LocalAIFormat.percent(0.456), "45%")
        XCTAssertEqual(LocalAIFormat.percent(2), "100%")
    }
}
