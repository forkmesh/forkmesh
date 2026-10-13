// The codec-free prompt attachment cases, the
// model catalog, and image blocks through the providers and the agent loop.

import FMApi
import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMAgents

final class PromptAttachmentsTests: XCTestCase {
    private func segment(_ marker: UInt8, _ payload: [UInt8]) -> [UInt8] {
        let length = payload.count + 2
        return [0xFF, marker, UInt8(length >> 8), UInt8(length & 0xFF)] + payload
    }

    func testStripRemovesExifXmpAndCommentsButKeepsJfifAndImage() {
        let jfif = segment(0xE0, Array("JFIF".utf8) + [0, 1, 1, 0, 0, 1, 0, 1, 0, 0])
        let exif = segment(0xE1, Array("Exif".utf8) + [0, 0, 9, 9, 9])
        let icc = segment(0xE2, Array("ICC_PROFILE".utf8))
        let comment = segment(0xFE, Array("secret place".utf8))
        let quant = segment(0xDB, [0, 1, 2, 3])
        let scan: [UInt8] = [0xFF, 0xDA, 0, 4, 7, 7, 0x12, 0x34, 0xFF, 0xD9]
        // Typed step by step: one long `+` chain times out the type checker on a busy machine.
        let soi: [UInt8] = [0xFF, 0xD8]
        var bytes: [UInt8] = soi
        for part in [jfif, exif, icc, comment, quant, scan] { bytes += part }
        var expected: [UInt8] = soi
        for part in [jfif, quant, scan] { expected += part }
        let out = [UInt8](stripJpegMetadata(Data(bytes)))
        XCTAssertEqual(out, expected)
    }

    func testStripLeavesNonJpegAndBrokenInputAlone() {
        let png = Data([0x89, 0x50, 0x4E, 0x47, 0, 0])
        XCTAssertEqual(stripJpegMetadata(png), png)
        let truncated = Data([0xFF, 0xD8, 0xFF, 0xE1, 0x40, 0x00, 1, 2])
        XCTAssertEqual(stripJpegMetadata(truncated), truncated)
    }

    func testTargetSizeFitsTheLongestSideAndNeverUpscales() {
        XCTAssertTrue(PromptPhotoRules.targetSize(width: 4032, height: 3024) == (2048, 1536))
        XCTAssertTrue(PromptPhotoRules.targetSize(width: 1000, height: 3000) == (683, 2048))
        XCTAssertTrue(PromptPhotoRules.targetSize(width: 800, height: 600) == (800, 600))
    }

    func testNamesAndPaths() {
        XCTAssertEqual(PromptPhotoRules.attachmentName("IMG_0001.HEIC"), "IMG_0001.jpg")
        XCTAssertEqual(PromptPhotoRules.attachmentName("  "), "photo.jpg")
        let item = PromptAttachment(id: "ab/../c", name: "x.jpg", bytes: Data())
        XCTAssertEqual(attachmentRelativePath(item), ".forkmesh/attachments/abc.jpg")
        XCTAssertEqual(
            promptWithAttachmentPaths("Look", ["a.jpg"]),
            "Look\n\nAttached images (open them with your file-reading tool):\n- a.jpg")
        XCTAssertNotEqual(PromptPhotoRules.newId(), PromptPhotoRules.newId())
    }

    func testFinishRefusesOversizedOutput() {
        let big = Data([0xFF, 0xD8, 0xFF, 0xDA] + [UInt8](repeating: 0, count: PromptAttachmentLimits.maxBytes + 1))
        XCTAssertThrowsError(try PromptPhotoRules.finish(jpeg: big, pickedName: "a", width: 1, height: 1))
    }

    func testModelCatalogDefaultsToOpus55() {
        XCTAssertEqual(AgentModelCatalog.defaultNodeModel.modelId, "claude-opus-5-5")
        XCTAssertEqual(AgentModelCatalog.nodeModels.first, AgentModelCatalog.defaultNodeModel)
        XCTAssertEqual(AgentModelCatalog.shortLabel("claude-opus-5-5"), "Opus 5.5")
        XCTAssertEqual(AgentModelCatalog.shortLabel("terra"), "GPT-5.6 Terra")
        XCTAssertEqual(AgentModelCatalog.shortLabel("mystery"), "mystery")
        XCTAssertEqual(AgentModelCatalog.choices(offered: ["claude-code": ["sonnet"]]).map(\.alias), ["sonnet"])
        XCTAssertEqual(AgentModelCatalog.choices(offered: ["x": ["y"]]).count, AgentModelCatalog.nodeModels.count)
        XCTAssertEqual(AgentProviderKind.anthropic.models.first?.id, "claude-opus-5-5")
        XCTAssertEqual(AnthropicProvider.defaultModel, "claude-opus-5-5")
        XCTAssertEqual(AgentModelCatalog.nodeModels.first { $0.provider == "codex" }?.launchProvider, "codex")
    }

    func testModelCatalogOffersGemmaOnTheNodesOllama() {
        let gemma = AgentModelCatalog.nodeModels.filter { $0.provider == "local-llama" }
        XCTAssertEqual(gemma.map(\.alias), ["gemma4:e4b", "gemma4:e2b", "gemma4:12b", "gemma4:26b", "gemma4:31b"])
        XCTAssertEqual(gemma.first?.launchProvider, "local-llama")
        XCTAssertEqual(AgentModelCatalog.defaultNodeModel.launchProvider, "auto")
        XCTAssertEqual(AgentModelCatalog.shortLabel("gemma4:12b"), "Gemma 4 12B")
        let offered = AgentModelCatalog.choices(offered: ["claude-code": ["opus"], "local-llama": ["gemma4:e4b"]])
        XCTAssertEqual(offered.map(\.alias), ["opus", "gemma4:e4b"])
        XCTAssertFalse(AgentModelCatalog.choices(offered: ["claude-code": ["opus"]]).contains { $0.provider == "local-llama" })
    }

    func testFollowUpChoicesOfferTheOtherRuntimeAfterTheSessionsOwn() {
        let codex = AgentModelCatalog.followUpChoices(provider: "codex")
        XCTAssertEqual(codex.prefix(4).map(\.alias), ["terra", "sol", "luna", "spark"])
        XCTAssertEqual(codex.dropFirst(4).first, AgentModelCatalog.defaultNodeModel)
        XCTAssertEqual(Set(codex.map(\.provider)), ["codex", "claude-code"])
        let claude = AgentModelCatalog.followUpChoices(provider: "claude-code")
        XCTAssertEqual(claude.first, AgentModelCatalog.defaultNodeModel)
        XCTAssertEqual(claude.last?.alias, "spark")
        XCTAssertEqual(claude.count, codex.count)
        XCTAssertTrue(AgentModelCatalog.followUpChoices(provider: "local-llama").isEmpty)
        XCTAssertEqual(AgentModelCatalog.runtimeLabel("codex"), "Codex")
        XCTAssertEqual(AgentModelCatalog.runtimeLabel("claude-code"), "Claude Code")
    }
}

@MainActor
final class PromptImageBlockTests: XCTestCase {
    private let image = PromptAttachment(id: "i", name: "p.jpg", bytes: Data([1, 2, 3]))

    func testAnthropicPutsImagesBeforeText() async throws {
        var history: [JSONValue] = []
        AnthropicProvider(apiKey: "sk-test").appendUser(&history, "What is this?", images: [image])
        let content = try XCTUnwrap(history.last?["content"]?.arrayValue)
        XCTAssertEqual(content.count, 2)
        XCTAssertEqual(content[0]["type"], "image")
        XCTAssertEqual(content[0]["source"]?["media_type"], "image/jpeg")
        XCTAssertEqual(content[0]["source"]?["data"], "AQID")
        XCTAssertEqual(content[1]["text"], "What is this?")
    }

    func testOpenAiUsesDataUrls() async throws {
        var history: [JSONValue] = []
        OpenAiProvider(apiKey: "sk-test").appendUser(&history, "Hi", images: [image])
        let content = try XCTUnwrap(history.last?["content"]?.arrayValue)
        XCTAssertEqual(content[1]["image_url"]?["url"], "data:image/jpeg;base64,AQID")
    }

    func testPlainTextStaysAString() async {
        var history: [JSONValue] = []
        AnthropicProvider(apiKey: "sk-test").appendUser(&history, "Hi", images: [])
        XCTAssertEqual(history.last?["content"], "Hi")
    }
}
