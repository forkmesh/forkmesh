import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation
import XCTest

@testable import FMControl

final class DesktopPromptImagesTests: XCTestCase {
    private actor Pauses {
        var seconds: [Double] = []
        func add(_ value: Double) { seconds.append(value) }
    }

    @MainActor private func relayHub(_ desktop: FakeRelayDesktop) throws -> DesktopControlHub {
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        return DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
    }

    @MainActor func testPhotosUploadInChunksAndArePacedThroughTheRelay() async throws {
        let desktop = try FakeRelayDesktop()
        var staged: [String: Data] = [:]
        var begun: [JSONObject] = []
        desktop.handler = { method, params in
            switch method {
            case "agents.image.begin":
                begun.append(params)
                let id = String(format: "%032x", begun.count)
                staged[id] = Data()
                return ["image": .string(id)]
            case "agents.image.chunk":
                let id = params.str("image")
                let data = Data(base64Encoded: params.str("data")) ?? Data()
                XCTAssertLessThanOrEqual(data.count, DesktopPromptImages.chunkBytes)
                staged[id, default: Data()].append(data)
                return ["received": .int(Int64(staged[id]?.count ?? 0))]
            default:
                return nil
            }
        }
        let hub = try relayHub(desktop)
        let link = try await hub.link()
        let big = Data([0xFF, 0xD8, 0xFF] + [UInt8](repeating: 7, count: DesktopPromptImages.chunkBytes + 10))
        let small = Data([0x89, 0x50, 0x4E, 0x47, 1, 2, 3])
        let photos = [
            PromptAttachment(id: "a", name: "a.jpg", bytes: big, mediaType: "image/jpeg"),
            PromptAttachment(id: "b", name: "b.png", bytes: small, mediaType: "image/png"),
        ]
        let pauses = Pauses()
        let ids = try await DesktopPromptImages.upload(photos, over: link) { await pauses.add($0) }

        XCTAssertEqual(ids, [.string(String(format: "%032x", 1)), .string(String(format: "%032x", 2))])
        XCTAssertEqual(staged[String(format: "%032x", 1)], big)
        XCTAssertEqual(staged[String(format: "%032x", 2)], small)
        XCTAssertEqual(begun.map { $0.str("mediaType") }, ["image/jpeg", "image/png"])
        XCTAssertEqual(begun.first?.int("size"), big.count)
        XCTAssertEqual(begun.first?.str("sha256"), Sha256.hex([UInt8](big)))
        let paused = await pauses.seconds
        XCTAssertEqual(paused.count, 3)
        XCTAssertGreaterThan(paused.first ?? 0, 2)
    }

    @MainActor func testADesktopFollowUpCarriesItsPhotos() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.handler = { method, _ in
            switch method {
            case "agents.list":
                return [
                    "sessions": [[
                        "id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "running",
                        "provider": "claude-code", "prompt": "Fix", "stoppable": true,
                    ]],
                ]
            case "agents.image.begin": return ["image": .string(String(repeating: "c", count: 32))]
            case "agents.image.chunk": return ["received": 4]
            default: return nil
            }
        }
        let center = AgentControlCenter(hub: try relayHub(desktop), cloud: nil)
        await center.refresh()
        let row = try XCTUnwrap(center.rows.first)
        let photo = PromptAttachment(id: "p", name: "p.png", bytes: Data([0x89, 0x50, 0x4E, 0x47]), mediaType: "image/png")
        try await center.prompt(row, "  ", images: [photo])

        let prompt = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" })
        XCTAssertEqual(prompt.obj("params").str("text"), "See the attached photos.")
        XCTAssertEqual(prompt.obj("params")["images"], .array([.string(String(repeating: "c", count: 32))]))
        let order = desktop.requests.map { $0.str("method") }.filter { $0.hasPrefix("agents.image") || $0 == "agents.prompt" }
        XCTAssertEqual(order, ["agents.image.begin", "agents.image.chunk", "agents.prompt"])
    }

    @MainActor func testAFollowUpWithoutPhotosSendsNoImageList() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.handler = { method, _ in
            guard method == "agents.list" else { return nil }
            return ["sessions": [["id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "running"]]]
        }
        let center = AgentControlCenter(hub: try relayHub(desktop), cloud: nil)
        await center.refresh()
        try await center.prompt(try XCTUnwrap(center.rows.first), "go on")
        let prompt = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" })
        XCTAssertNil(prompt.obj("params")["images"])
        XCTAssertFalse(desktop.requests.contains { $0.str("method").hasPrefix("agents.image") })
    }

    @MainActor func testANodeFollowUpCarriesItsPhotosToTheRelay() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [
            AgentLaunchSession(
                sessionId: "s1", owner: "alice", repo: "site", provider: "codex", status: "running",
                targetNode: "mirror35", title: "Docs", updatedAtMs: 1)
        ]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let photo = PromptAttachment(id: "p", name: "p.png", bytes: Data([0x89, 0x50, 0x4E, 0x47]), mediaType: "image/png")
        try await center.prompt(try XCTUnwrap(center.rows.first), "look", images: [photo])
        XCTAssertEqual(cloudApi.steered.first?.1, "look")
        XCTAssertEqual(cloudApi.steeredImages, [[photo]])
    }
}
