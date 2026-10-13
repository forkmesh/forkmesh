import FMBase
import FMNet
import Foundation
@testable import FMLocalAI

/// A fresh directory under the system temp dir, removed by `cleanUp`.
final class TempDirectory {
    let url: URL

    init() {
        url = FileManager.default.temporaryDirectory
            .appendingPathComponent("fmlocalai-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    }

    func cleanUp() {
        try? FileManager.default.removeItem(at: url)
    }
}

/// A small catalog model whose repository the fakes serve.
let tinyModel = LocalAIModel(
    id: "tiny-it-4bit",
    repo: "test-org/tiny-it-4bit",
    revision: "abc123",
    displayName: "Tiny",
    summary: "A test model.",
    approxDownloadBytes: 30,
    approxMemoryBytes: 1_000,
    minimumPhysicalMemory: 1_000_000,
    recommendedPhysicalMemory: 2_000_000,
    contextLength: 4096
)

let bigDevice = DeviceProfile(physicalMemory: 12_000_000_000)

/// The listing the Hugging Face API returns for `tinyModel`.
func tinyListing(weights: Data, config: Data, template: Data) -> JSONValue {
    [
        "id": "test-org/tiny-it-4bit",
        "sha": "abc123",
        "siblings": [
            ["rfilename": ".gitattributes", "size": 10],
            ["rfilename": "README.md", "size": 5],
            ["rfilename": "config.json", "size": .int(Int64(config.count))],
            ["rfilename": "chat_template.jinja", "size": .int(Int64(template.count))],
            [
                "rfilename": "model.safetensors",
                "size": .int(Int64(weights.count)),
                "lfs": ["sha256": "00", "size": .int(Int64(weights.count))],
            ],
            ["rfilename": "onnx/model.json", "size": 3],
        ],
    ]
}

extension FakeHTTPClient {
    /// Answers the listing for `model` at its revision.
    func serveListing(_ model: LocalAIModel, _ listing: JSONValue) {
        on("GET", path: "/api/models/\(model.repo)/revision/\(model.revision)", json: listing)
    }
}

func readFile(_ url: URL) -> Data? {
    try? Data(contentsOf: url)
}
