import FMApi
import FMBase
import FMGit
import XCTest
@testable import FMCode

@MainActor
final class CloneEndpointsTests: XCTestCase {
    private let relay = ForkMeshRemoteCredentials(
        origin: "wss://app.forkmesh.com/ws", owner: "forkmesh", repo: "forkmesh",
        seed: Array(repeating: 7, count: 32), pubkeyB64url: "pub")

    private func row(
        _ node: String, latency: Int = 0, healthy: Bool = true, behind: Bool = false, abuse: Bool = false,
        url: String? = nil, endpoint: String = "", cloneAvailable: Bool = true, online: Bool = true
    ) -> RepoMirror {
        RepoMirror(json: [
            "node": .string(node), "repo": "forkmesh", "status": online ? "online" : "offline",
            "lastSync": 1_790_000_000_000, "cloneAvailable": .bool(cloneAvailable), "integrity": "ok",
            "mirrorUrl": .string(url ?? "https://\(node).forkmesh.com/forkmesh/forkmesh.git"),
            "endpoint": .string(endpoint), "endpointHealthy": .bool(healthy), "endpointFresh": .bool(healthy),
            "latencyMs": .int(Int64(latency)), "behind": .bool(behind), "abuseBlocked": .bool(abuse),
        ])
    }

    func testHealthyMirrorsComeFirstByLatencyThenTheRestThenTheRelay() {
        let plan = CloneEndpoints.plan(owner: "forkmesh", name: "forkmesh", mirrors: [
            row("mirror38", latency: 120),
            row("mirror37", latency: 30, healthy: false),
            row("mirror36", latency: 0),
            row("mirror35", latency: 45),
            row("mirror34", latency: 10, behind: true),
        ], relay: relay)

        XCTAssertEqual(plan.map(\.host), [
            "mirror35.forkmesh.com", "mirror38.forkmesh.com", "mirror36.forkmesh.com", "mirror34.forkmesh.com",
            "mirror37.forkmesh.com", "app.forkmesh.com",
        ])
        XCTAssertEqual(plan.first?.kind, .mirror(node: "mirror35", healthy: true))
        XCTAssertEqual(plan.first?.url, "https://mirror35.forkmesh.com/forkmesh/forkmesh.git")
        XCTAssertEqual(plan.last?.kind, .relay)
        XCTAssertEqual(plan.last?.url, "https://app.forkmesh.com/forkmesh/forkmesh")
        XCTAssertEqual(plan.last?.label, "the relay (app.forkmesh.com)")
        XCTAssertEqual(plan.first?.label, "mirror35.forkmesh.com")
    }

    func testUnusableRowsAreDroppedAndAnEndpointWithoutAURLIsCompleted() {
        let plan = CloneEndpoints.plan(owner: "forkmesh", name: "forkmesh", mirrors: [
            row("blocked", abuse: true),
            row("plain", url: "http://plain.forkmesh.com/forkmesh/forkmesh.git"),
            row("creds", url: "https://user:pw@creds.forkmesh.com/forkmesh/forkmesh.git"),
            row("gone", healthy: false, cloneAvailable: false, online: false),
            row("bare", url: "", endpoint: "https://bare.forkmesh.com/"),
            row("dup", url: "https://BARE.forkmesh.com/forkmesh/forkmesh.git"),
            row("nothing", url: ""),
        ], relay: relay)

        XCTAssertEqual(plan.map(\.url), [
            "https://bare.forkmesh.com/forkmesh/forkmesh.git",
            "https://app.forkmesh.com/forkmesh/forkmesh",
        ])
    }

    func testNoRelayOriginMeansMirrorsOnly() {
        let offline = ForkMeshRemoteCredentials(origin: "", owner: "forkmesh", repo: "forkmesh")
        XCTAssertEqual(CloneEndpoints.plan(owner: "forkmesh", name: "forkmesh", mirrors: [], relay: offline), [])
        XCTAssertEqual(
            CloneEndpoints.plan(owner: "forkmesh", name: "forkmesh", mirrors: [row("mirror35")], relay: offline).count, 1)
    }

    func testAMirrorEndpointIsAnonymousAndKeepsTheRepositoryIdentity() {
        let plan = CloneEndpoints.plan(owner: "forkmesh", name: "forkmesh", mirrors: [row("mirror35")], relay: relay)
        XCTAssertTrue(relay.canSign)

        let mirror = plan[0].credentials(relay)
        XCTAssertFalse(mirror.canSign)
        XCTAssertTrue(mirror.seed.isEmpty)
        XCTAssertEqual(mirror.remoteUrl, "https://mirror35.forkmesh.com/forkmesh/forkmesh.git")
        XCTAssertEqual(mirror.relayUrl, "https://app.forkmesh.com/forkmesh/forkmesh")
        XCTAssertEqual(mirror.host, "mirror35.forkmesh.com")
        XCTAssertEqual(mirror.owner, "forkmesh")
        XCTAssertEqual(mirror.repo, "forkmesh")
        XCTAssertEqual(mirror.anonymous, mirror)

        let viaRelay = plan[1].credentials(relay)
        XCTAssertTrue(viaRelay.canSign)
        XCTAssertEqual(viaRelay.remoteUrl, "https://app.forkmesh.com/forkmesh/forkmesh")
        XCTAssertEqual(viaRelay.host, "app.forkmesh.com")
    }

    func testRunTriesEachSourceInOrderUntilOneWorks() async throws {
        let plan = CloneEndpoints.plan(
            owner: "forkmesh", name: "forkmesh", mirrors: [row("mirror35", latency: 1), row("mirror36", latency: 2)], relay: relay)
        var tried = [String]()
        var announced = [Int]()

        let result = try await CloneEndpoints.run(
            repository: "forkmesh/forkmesh", endpoints: plan, onAttempt: { _, index in announced.append(index) }
        ) { endpoint in
            tried.append(endpoint.host)
            if endpoint.host == "mirror35.forkmesh.com" {
                throw GitWorkspaceException("Could not reach ForkMesh.", code: .networkUnavailable)
            }
            return endpoint.host
        }

        XCTAssertEqual(result, "mirror36.forkmesh.com")
        XCTAssertEqual(tried, ["mirror35.forkmesh.com", "mirror36.forkmesh.com"])
        XCTAssertEqual(announced, [0, 1])
    }

    func testRunStopsAtCancellationAndSizeRefusals() async {
        let plan = CloneEndpoints.plan(
            owner: "forkmesh", name: "forkmesh", mirrors: [row("mirror35", latency: 1), row("mirror36", latency: 2)], relay: relay)
        for final in [
            GitWorkspaceException("Operation cancelled.", code: .cancelled),
            GitWorkspaceException("too large", code: .resourceLimitExceeded),
        ] {
            var tried = 0
            do {
                _ = try await CloneEndpoints.run(repository: "forkmesh/forkmesh", endpoints: plan) { _ -> Int in
                    tried += 1
                    throw final
                }
                XCTFail("expected a failure")
            } catch {
                XCTAssertEqual(error as? GitWorkspaceException, final)
            }
            XCTAssertEqual(tried, 1)
        }
        XCTAssertTrue(CloneEndpoints.isFinal(LocalRepoStoreException("A folder with that name already exists on this device.")))
        XCTAssertFalse(CloneEndpoints.isFinal(GitWorkspaceException("redirected", code: .remoteRejected)))
    }

    func testRunReportsEverySourceWhenAllFail() async {
        let plan = CloneEndpoints.plan(
            owner: "forkmesh", name: "forkmesh", mirrors: [row("mirror35", latency: 1)], relay: relay)
        do {
            _ = try await CloneEndpoints.run(repository: "forkmesh/forkmesh", endpoints: plan) { endpoint -> Int in
                throw GitWorkspaceException("failed at \(endpoint.host)", code: .networkUnavailable)
            }
            XCTFail("expected a failure")
        } catch let failure as CloneFailure {
            XCTAssertEqual(failure.attempts.map(\.endpoint), plan)
            XCTAssertEqual(failure.message, [
                "Could not clone forkmesh/forkmesh from any of its 2 sources.",
                "• mirror35.forkmesh.com: failed at mirror35.forkmesh.com",
                "• the relay (app.forkmesh.com): failed at app.forkmesh.com",
            ].joined(separator: "\n"))
        } catch {
            XCTFail("unexpected \(error)")
        }
    }

    func testWithNoMirrorsTheFailureSaysSo() {
        let relayOnly = CloneEndpoint(kind: .relay, url: "https://app.forkmesh.com/forkmesh/forkmesh")
        let attempt = CloneAttempt(endpoint: relayOnly, message: "Nothing on this relay is serving this repository right now.")
        XCTAssertEqual(
            CloneFailure(repository: "forkmesh/forkmesh", attempts: [attempt]).message.components(separatedBy: "\n").first,
            "No mirror is serving forkmesh/forkmesh right now.")
        XCTAssertEqual(
            CloneFailure(repository: "forkmesh/forkmesh", attempts: [attempt], lookupProblem: "HTTP 500")
                .message.components(separatedBy: "\n").first,
            "Could not list the mirrors of forkmesh/forkmesh (HTTP 500).")
        XCTAssertEqual(CloneFailure(repository: "a/b", attempts: []).message, "No mirror is serving a/b right now.")
    }

    func testProgressLabelsNameTheSource() {
        XCTAssertEqual(
            CloneToDeviceModel.progressLabel(GitTransferProgress(phase: "receivingPack", bytes: 3 * 1024 * 1024), source: "mirror35.forkmesh.com"),
            "Downloading 3.0 MB · mirror35.forkmesh.com")
        XCTAssertEqual(
            CloneToDeviceModel.progressLabel(GitTransferProgress(phase: "connecting"), source: "mirror35.forkmesh.com"),
            "Connecting to mirror35.forkmesh.com")
        let endpoint = CloneEndpoint(kind: .mirror(node: "mirror35", healthy: true), url: "https://mirror35.forkmesh.com/forkmesh/forkmesh.git")
        XCTAssertEqual(
            CloneToDeviceModel.attemptLabel(endpoint, index: 1, of: 5), "Connecting to mirror35.forkmesh.com (source 2 of 5)")
        XCTAssertEqual(CloneToDeviceModel.attemptLabel(endpoint, index: 0, of: 1), "Connecting to mirror35.forkmesh.com")
    }
}
