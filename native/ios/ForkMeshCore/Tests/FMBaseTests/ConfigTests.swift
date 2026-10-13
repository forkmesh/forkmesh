import XCTest
@testable import FMBase

final class ConfigTests: XCTestCase {
    private let relayPath = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"

    func testDefaultsAreProd() {
        let env = AppEnv.prod
        XCTAssertEqual(env.environment, .prod)
        XCTAssertFalse(env.isDev)
        XCTAssertEqual(env.defaultServerUrl, "wss://app.forkmesh.com\(relayPath)")
        XCTAssertEqual(env.worldSiteUrl, "https://world.forkmesh.com/")
        XCTAssertEqual(AppEnv(infoDictionary: nil), env)
        XCTAssertEqual(AppEnv(infoDictionary: [:]), env)
    }

    func testProdJsonValuesResolveToTheSameRelay() {
        let env = AppEnv(infoDictionary: [
            "FORKMESH_ENV": "prod", "FORKMESH_API_HOST": "app.forkmesh.com",
            "FORKMESH_WORLD_URL": "https://world.forkmesh.com/",
        ])
        XCTAssertEqual(env.defaultServerUrl, AppEnv.prodServerUrl)
        XCTAssertEqual(env.describe(env.defaultServerUrl), "prod · app.forkmesh.com")
    }

    func testOnlyAnExplicitDevMarkerIsDev() {
        for marker in ["dev", "DEV", "development", "Local"] {
            XCTAssertTrue(AppEnv(rawEnvironment: marker).isDev, marker)
        }
        for marker in ["", "prod", "production", "devv", "staging"] {
            XCTAssertFalse(AppEnv(rawEnvironment: marker).isDev, marker)
        }
        XCTAssertEqual(AppEnv(rawEnvironment: "dev").defaultServerUrl, "ws://localhost:8787\(relayPath)")
    }

    func testOverridesWinInOrder() {
        XCTAssertEqual(
            AppEnv(rawEnvironment: "dev", apiHostOverride: "staging.forkmesh.com").defaultServerUrl,
            "wss://staging.forkmesh.com\(relayPath)"
        )
        XCTAssertEqual(
            AppEnv(apiHostOverride: "h", serverUrlOverride: "ws://x/ws").defaultServerUrl,
            "ws://x/ws"
        )
    }

    func testUnsubstitutedBuildSettingsKeepDefaults() {
        let env = AppEnv(infoDictionary: [
            "FORKMESH_ENV": "$(FORKMESH_ENV)", "FORKMESH_API_HOST": " ", "FORKMESH_WORLD_URL": 5,
        ])
        XCTAssertEqual(env, AppEnv.prod)
    }

    func testRelayUrlForHost() {
        XCTAssertEqual(AppEnv.relayUrlForHost(" localhost:8787 "), "ws://localhost:8787\(relayPath)")
        XCTAssertEqual(AppEnv.relayUrlForHost("127.0.0.1"), "ws://127.0.0.1\(relayPath)")
        XCTAssertEqual(AppEnv.relayUrlForHost("10.0.2.2:8787"), "ws://10.0.2.2:8787\(relayPath)")
        XCTAssertEqual(AppEnv.relayUrlForHost("staging.forkmesh.com"), "wss://staging.forkmesh.com\(relayPath)")
    }

    func testLoopbackDetection() {
        XCTAssertTrue(AppEnv.isLoopbackHost(" LOCALHOST "))
        XCTAssertTrue(AppEnv.isLoopbackHost("::1"))
        XCTAssertFalse(AppEnv.isLoopbackHost("app.forkmesh.com"))
        XCTAssertTrue(AppEnv.isLoopbackUrl(" ws://localhost:8787/x "))
        XCTAssertTrue(AppEnv.isLoopbackUrl("http://127.0.0.1/"))
        XCTAssertTrue(AppEnv.isLoopbackUrl("ws://[::1]:8787/x"))
        XCTAssertFalse(AppEnv.isLoopbackUrl("wss://app.forkmesh.com/x"))
        XCTAssertFalse(AppEnv.isLoopbackUrl("localhost"))
        XCTAssertFalse(AppEnv.isLoopbackUrl(""))
    }

    func testDescribe() {
        XCTAssertEqual(AppEnv.prod.describe("wss://App.ForkMesh.com/ws"), "prod · app.forkmesh.com")
        XCTAssertEqual(AppEnv(rawEnvironment: "local").describe("ws://localhost:8787/ws"), "dev · localhost")
        XCTAssertEqual(AppEnv.prod.describe(""), "prod")
    }

    func testAppBuildFromInfoDictionary() {
        let build = AppBuild(infoDictionary: ["CFBundleShortVersionString": "1.2.2", "CFBundleVersion": "13"])
        XCTAssertEqual(build.version, "1.2.2+13")
        XCTAssertEqual(AppBuild(infoDictionary: ["CFBundleShortVersionString": "1.2.2"]).version, "1.2.2")
        XCTAssertEqual(AppBuild(infoDictionary: nil).version, "0.0.0")
        #if os(Linux)
        XCTAssertEqual(build.platform, "linux")
        #endif
        XCTAssertFalse(build.osVersion.isEmpty)
        let config = AppConfig(infoDictionary: ["CFBundleShortVersionString": "2.0", "FORKMESH_ENV": "dev"])
        XCTAssertTrue(config.env.isDev)
        XCTAssertEqual(config.build.version, "2.0")
    }
}
