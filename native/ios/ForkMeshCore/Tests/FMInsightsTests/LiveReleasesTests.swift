import FMBase
import FMChat
import FMControl
import FMCrypto
import Foundation
import XCTest

@testable import FMInsights

let realManifestBase64 = [
    "ewogICJzY2hlbWEiOiAiZm9ya21lc2gtcmVsZWFzZS12MiIsCiAgInJlcG8iOiAiZm9ya21lc2gvZm9ya21lc2giLAogICJ0YWci",
    "OiAidjEuMi44IiwKICAidGFnX2NvbW1pdCI6ICIwMTE4ZDk5OTZmYjUyODdlMDJlMDhkZDlmN2JmNWU4MzgyMDk5ZWFjIiwKICAi",
    "YnVpbGRfY29tbWl0IjogIjAxMThkOTk5NmZiNTI4N2UwMmUwOGRkOWY3YmY1ZTgzODIwOTllYWMiLAogICJjaGFubmVsIjogImNh",
    "bmRpZGF0ZS0xLjIuOCIsCiAgImNyZWF0ZWRfYXQiOiAxNzkxODUzNzMxMDAwLAogICJjaGVja3N1bXNfc2hhMjU2IjogIjk5ZWUx",
    "YjI1ZTZlMTg0ZDU1Yjk4ODc2MGRmZGNhN2YwNDdkZDY1MjhhZGE0ODg0NDMwYjJmODY1NWQ4N2EwODAiLAogICJhc3NldHMiOiBb",
    "eyJuYW1lIjoiZm9ya21lc2gtYW5kcm9pZC11bml2ZXJzYWwuYXBrIiwiYmxvYl9zaGEyNTYiOiJjYWRlNDMwOWZiYjg4YzhlNDEw",
    "ZmMxZDk1YTdlMjJhY2JjNzVlMjY3MDIyYzEyNTlkNGRhZDZlZjJhYjI2Y2U0Iiwic2l6ZSI6MTY3OTc4NzgsIm9zIjoiYW5kcm9p",
    "ZCIsImFyY2giOiJ1bml2ZXJzYWwifSx7Im5hbWUiOiJmb3JrbWVzaC1saW51eC14ODZfNjQiLCJibG9iX3NoYTI1NiI6ImIzZTdi",
    "ZDJmNTA4YzNhNmM1ZWExNDIxMWU3Y2I4NWZiZjM3ODdlYWFiYmRiYTg3NTg1YjQ5MjdiY2ViMGM2YmQiLCJzaXplIjoxMzI4Njkw",
    "NjQsIm9zIjoibGludXgiLCJhcmNoIjoieDg2XzY0In0seyJuYW1lIjoiZm9ya21lc2gtbGludXgteDg2XzY0LkFwcEltYWdlIiwi",
    "YmxvYl9zaGEyNTYiOiJhNzkzMGIwYjA4YTE0YzUwNTVlYjVjNGExNTljZjZiMDNjODE1MWUxMzNkMWQ5ZDVkOWNmMWU1MDk4Mzdj",
    "NGQ3Iiwic2l6ZSI6MTEwMDgyNTUyLCJvcyI6ImxpbnV4IiwiYXJjaCI6Ing4Nl82NCJ9LHsibmFtZSI6ImZvcmttZXNoLWxpbnV4",
    "LXg4Nl82NC5BcHBJbWFnZS56c3luYyIsImJsb2Jfc2hhMjU2IjoiZmFhMmYyODQ5ZGYwZDU5ZjJhMzA4YjIyZGU5YTkxYjkzNTZj",
    "MmM4NDBjYTJmYjFiY2NjYzNmYzNlMWE0NTVlZiIsInNpemUiOjE4ODM2NCwib3MiOiJsaW51eCIsImFyY2giOiJ4ODZfNjQifSx7",
    "Im5hbWUiOiJmb3JrbWVzaC1tYWNvcy1hcm02NC5kbWciLCJibG9iX3NoYTI1NiI6ImRjMjM5MTUzMjE2ZGRjYTc3NzY1NzA1NDMx",
    "NzNmNjMwNWE5YzY1ODliNWNmZDJjMmUzZWFiZDYwYmI1NTBiNTkiLCJzaXplIjoyMTk3MTAzMzEsIm9zIjoibWFjb3MiLCJhcmNo",
    "IjoiYXJtNjQifSx7Im5hbWUiOiJmb3JrbWVzaC1tYWNvcy14ODZfNjQuZG1nIiwiYmxvYl9zaGEyNTYiOiJkYzIzOTE1MzIxNmRk",
    "Y2E3Nzc2NTcwNTQzMTczZjYzMDVhOWM2NTg5YjVjZmQyYzJlM2VhYmQ2MGJiNTUwYjU5Iiwic2l6ZSI6MjE5NzEwMzMxLCJvcyI6",
    "Im1hY29zIiwiYXJjaCI6Ing4Nl82NCJ9LHsibmFtZSI6ImZvcmttZXNoLW1pcnJvci1ub2RlLWxpbnV4LXg4Nl82NCIsImJsb2Jf",
    "c2hhMjU2IjoiZmE0ZDRjNzZkNDE2YjlkYzI1M2QwMDVlZjE4NWJhMWJmNTUxOTZiMWIwYTM1NTBmZTk4Mzg1MjEzYzc5N2IyMiIs",
    "InNpemUiOjEzNTE1NTA0LCJvcyI6ImxpbnV4IiwiYXJjaCI6Ing4Nl82NCJ9LHsibmFtZSI6ImZvcmttZXNoLXdpbmRvd3MteDg2",
    "XzY0LXNldHVwLmV4ZSIsImJsb2Jfc2hhMjU2IjoiNmZhY2YzMTk4MGJjYjEzOTAyNTYxODIyYzhkODEwYWE1ZjFhYmVhM2QwMGJl",
    "NzAwZTVmNDYwYjQ1YzI1MjMxZiIsInNpemUiOjEwMzYzMDcyOCwib3MiOiJ3aW5kb3dzIiwiYXJjaCI6Ing4Nl82NCJ9XQp9Cg==",
].joined()

let realSignatureBase64 = "Fph8gpCRquRwaaIxvZIsrnOUo9CLzjfzaADYZgnWovky2mfO9dbQtQkNUm7hGk5VOfDi1+xTu7HXElD0LLgKCg=="

func realReleaseBlobs() -> JSONObject {
    [
        ".forkmesh/releases/latest/release.json": ["content": .string(realManifestBase64), "encoding": "base64"],
        ".forkmesh/releases/latest/release.json.sig": ["content": .string(realSignatureBase64), "encoding": "base64"],
        "desktop/CMakeLists.txt": ["content": "project(ForkMesh VERSION 1.2.8 LANGUAGES CXX)"],
    ]
}

@MainActor
final class BlobTransport: InsightsRequesting {
    var answers: [JSONObject]
    var paths: [String] = []

    init(_ answers: [JSONObject]) {
        self.answers = answers
    }

    func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        paths.append(path)
        let blobs = paths.count <= answers.count ? answers[paths.count - 1] : JSONObject()
        return ["blobs": .object(blobs)]
    }
}

final class LiveReleasesTests: XCTestCase {
    let seed = (0..<32).map { UInt8(truncatingIfNeeded: $0 * 7 + 1) }
    let sha = String(repeating: "a", count: 64)

    var key: [UInt8] { (try? Ed25519.publicKey(seed: seed)) ?? [] }

    func manifest(_ tag: String, _ assets: [(String, Int)]) -> String {
        JSON.encode(.object([
            "schema": "forkmesh-release-v2", "repo": "forkmesh/forkmesh", "tag": .string(tag),
            "assets": .array(assets.map { name, build in
                .object(["name": .string(name), "blob_sha256": .string(sha), "size": 10, "build": .int(Int64(build))])
            }),
        ]))
    }

    func sign(_ blobs: inout JSONObject, _ channel: String, _ text: String, signer: [UInt8]? = nil) throws {
        let paths = LiveReleases.manifestPaths(channel)
        let signature = try Ed25519.sign(seed: signer ?? seed, message: Array(text.utf8))
        blobs[paths[0]] = ["content": .string(text)]
        blobs[paths[1]] = ["content": .string(B64.encode(signature)), "encoding": "base64"]
    }

    func testTheRealManifestVerifiesAgainstTheHomepageKey() throws {
        let release = try XCTUnwrap(LiveReleases.verified(realReleaseBlobs(), channel: "latest"))
        XCTAssertEqual(release.tag, "v1.2.8")
        XCTAssertNotNil(release.asset("forkmesh-android-universal.apk"))
        var tampered = realReleaseBlobs()
        tampered[".forkmesh/releases/latest/release.json"] = ["content": .string(manifest("v9.9.9", []))]
        XCTAssertNil(LiveReleases.verified(tampered, channel: "latest"))
    }

    @MainActor func testEveryPlatformShowsTheVersionTheHomepageOffers() async throws {
        var first = JSONObject()
        try sign(&first, "latest", manifest("v1.2.8", [
            ("forkmesh-android-universal.apk", 3), ("forkmesh-linux-x86_64.AppImage", 0), ("forkmesh-macos-arm64.dmg", 0),
        ]))
        first["desktop/CMakeLists.txt"] = ["content": "project(ForkMesh VERSION 1.2.9 LANGUAGES CXX)"]
        var second = JSONObject()
        try sign(&second, "candidate-1.2.9", manifest("v1.2.9", [("forkmesh-linux-x86_64.AppImage", 0)]))
        try sign(&second, "candidate-1.2.6", manifest("v1.2.6", [
            ("forkmesh-windows-x86_64-setup.exe", 0), ("forkmesh-macos-x86_64.dmg", 0),
        ]))
        try sign(&second, "candidate-1.2.7", manifest("v1.2.7", [("forkmesh-macos-x86_64.dmg", 0)]))
        let transport = BlobTransport([first, second])
        let summary = try await LiveReleases.load(transport, key: key)
        XCTAssertTrue(transport.paths[0].hasPrefix("/api/repo/forkmesh/forkmesh/blobs?path=.forkmesh%2Freleases%2Flatest%2Frelease.json&"))
        XCTAssertTrue(transport.paths[0].hasSuffix("&ref=main"))
        XCTAssertTrue(transport.paths[1].contains("candidate-1.2.9") && transport.paths[1].contains("candidate-1.2.2"))
        XCTAssertFalse(transport.paths[1].contains("candidate-1.2.1"))
        XCTAssertEqual(summary.latest, "v1.2.8")
        XCTAssertEqual(summary.newest, "v1.2.9")
        let tiles = LiveReleases.tiles(summary)
        XCTAssertEqual(tiles[LiveReleases.latestId]?.value, "v1.2.9")
        XCTAssertEqual(tiles[LiveReleases.latestId]?.note, "downloads are v1.2.8 until v1.2.9 builds are published")
        XCTAssertEqual(tiles["release-android"]?.value, "v1.2.8")
        XCTAssertEqual(tiles["release-android"]?.note, "APK · build 3")
        XCTAssertEqual(tiles["release-android"]?.status, .good)
        XCTAssertEqual(tiles["release-macos"]?.value, "v1.2.8")
        XCTAssertEqual(tiles["release-macos"]?.note, "DMG · Apple silicon v1.2.8, Intel v1.2.7 · behind v1.2.8")
        XCTAssertEqual(tiles["release-macos"]?.status, .warning)
        XCTAssertEqual(tiles["release-windows"]?.value, "v1.2.6")
        XCTAssertEqual(tiles["release-windows"]?.note, "Installer · behind v1.2.8")
        XCTAssertEqual(tiles["release-linux"]?.value, "v1.2.8")
    }

    @MainActor func testMissingBuildsAndBadSignaturesAreNotOffered() async throws {
        var first = JSONObject()
        try sign(&first, "latest", manifest("v1.0.0", [("forkmesh-linux-x86_64.AppImage", 0)]))
        first["desktop/CMakeLists.txt"] = ["content": "project(ForkMesh VERSION 1.0.1)"]
        var second = JSONObject()
        try sign(&second, "candidate-1.0.1", manifest("v1.0.1", [("forkmesh-linux-x86_64.AppImage", 0)]),
                 signer: [UInt8](repeating: 9, count: 32))
        let summary = try await LiveReleases.load(BlobTransport([first, second]), key: key)
        XCTAssertEqual(summary.newest, "v1.0.0")
        let tiles = LiveReleases.tiles(summary)
        XCTAssertEqual(tiles[LiveReleases.latestId]?.note, "1 verified download")
        XCTAssertEqual(tiles["release-android"]?.available, false)
        XCTAssertEqual(tiles["release-android"]?.note, "Not in v1.0.0")
        XCTAssertEqual(tiles["release-windows"]?.note, "Coming soon")
        do {
            _ = try await LiveReleases.load(BlobTransport([]), key: key)
            XCTFail("an unsigned manifest must not load")
        } catch let error as InsightsError {
            XCTAssertEqual(error.code, "release_unverified")
        }
    }

    func testIosShowsTheVersionLiveOnTheAppStore() {
        let tile = LiveReleases.appStore(object(
            #"{"snapshot":{"versions":[{"version":"1.2.9","status":{"state":"WAITING_FOR_REVIEW","label":"Waiting for review"}},{"version":"1.2.8","build":"41","status":{"state":"READY_FOR_SALE","label":"Ready for sale"}}]}}"#))
        XCTAssertEqual(tile.value, "v1.2.8")
        XCTAssertEqual(tile.note, "App Store · build 41 · v1.2.9 waiting for review")
        XCTAssertEqual(LiveReleases.appStore(["ok": true]).available, false)
    }
}

let desktopTilesJSON = """
{"sampledAt":77,"tiles":[
 {"id":"issues-open","value":"12","note":"open","status":"warning","trend":"down",
  "series":{"labels":["a","b","c"],"values":[1,2,3],"categorical":false}},
 {"id":"cpu","available":false,"note":"Sampling"},
 {"id":"from-a-newer-desktop","value":"1"},
 {"value":"no id"}]}
"""

final class DesktopTilesTests: XCTestCase {
    func testReplyKeepsEveryReadingUnderItsBoardId() {
        let reply = DesktopTiles.reply(object(desktopTilesJSON))
        XCTAssertEqual(reply.sampledAt, 77)
        XCTAssertEqual(reply.offered, ["desktop.issues-open", "desktop.cpu", "desktop.from-a-newer-desktop"])
        let issues = reply.tiles["desktop.issues-open"]
        XCTAssertEqual(issues?.value, "12")
        XCTAssertEqual(issues?.status, .warning)
        XCTAssertEqual(issues?.trend, .downIsGood)
        XCTAssertEqual(issues?.series.values, [1, 2, 3])
        XCTAssertEqual(reply.tiles["desktop.cpu"]?.available, false)
        XCTAssertEqual(reply.tiles["desktop.cpu"]?.note, "Sampling")
        XCTAssertGreaterThanOrEqual(DesktopTiles.ids.count, 100)
        XCTAssertTrue(DesktopTiles.ids.isSubset(of: Set(StatsCatalog.entries.map(\.id))))
        XCTAssertEqual(DesktopTiles.defaults.first { $0.id == "desktop.cpu" }?.width, 2)
    }

    @MainActor func testClientAsksTheDesktopForEveryTile() async throws {
        let channel = FakeDesktopChannel()
        channel.replies[DesktopStatsMethod.tiles] = object(desktopTilesJSON)
        let reply = try await DesktopStatsClient(channel: channel).tiles()
        XCTAssertEqual(channel.requests.first?.0, "stats.tiles")
        XCTAssertEqual(reply.tiles["desktop.issues-open"]?.value, "12")
    }

    @MainActor func testBoardShowsOnlyTheDesktopTilesTheDesktopOffers() async {
        let prefs = InMemoryKeyValueStore()
        let model = StatsBoardModel(
            transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: prefs),
            desktopTiles: { DesktopTiles.reply(object(desktopTilesJSON)) })
        XCTAssertFalse(model.visible.contains { DesktopTiles.isDesktopTile($0.id) })
        XCTAssertFalse(model.addable.contains { DesktopTiles.isDesktopTile($0.id) })
        await model.reload()
        XCTAssertEqual(model.visible.map(\.id).filter(DesktopTiles.isDesktopTile), ["desktop.issues-open", "desktop.cpu"])
        XCTAssertEqual(model.data("desktop.issues-open")?.value, "12")
        XCTAssertNil(model.data("desktop.from-a-newer-desktop"))
        model.remove("desktop.cpu")
        XCTAssertEqual(model.addable.map(\.id).filter(DesktopTiles.isDesktopTile), ["desktop.cpu"])
        let reopened = StatsBoardModel(
            transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: prefs),
            desktopTiles: { throw LocalControlException("timeout") })
        XCTAssertEqual(reopened.visible.map(\.id).filter(DesktopTiles.isDesktopTile), ["desktop.issues-open"])
        await reopened.reload()
        XCTAssertEqual(reopened.data("desktop.issues-open")?.note, "Your desktop took too long to answer. Pull to try again.")
        let old = StatsBoardModel(
            transport: FakeInsightsTransport(), layoutStore: StatsLayoutStore(store: prefs),
            desktopTiles: { throw LocalControlException("unsupported") })
        await old.reload()
        XCTAssertFalse(old.visible.contains { DesktopTiles.isDesktopTile($0.id) })
        XCTAssertEqual(old.desktopTilesNote, "Update ForkMesh on your desktop to see all of its stats here.")
    }
}
