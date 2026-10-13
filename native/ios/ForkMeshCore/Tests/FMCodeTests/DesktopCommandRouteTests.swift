import FMAccount
import FMApi
import FMBase
import FMCrypto
import XCTest
@testable import FMCode

@MainActor
private final class FakeRouteProbe: DesktopCommandRouteProbing {
    var answers: [Bool?] = []
    var calls = 0

    func desktopCommandRouteAnswered(_ owner: String, _ name: String) async -> Bool? {
        calls += 1
        return answers.isEmpty ? nil : answers.removeFirst()
    }
}

final class DesktopCommandRouteTests: XCTestCase {
    private let now: Int64 = 1_800_000_000_000

    @MainActor private func signer(_ session: AuthSession?, _ identity: DeviceIdentity?) -> RepoSigner {
        RepoSigner(session: { session }, identity: identity, clock: FixedClock(epochMillis: now))
    }

    func testTheCanonicalStringIsTheServersAndEverySignerSharesIt() {
        let expected = "forkmesh-desktop-command-v1\nalice\nrepo\npull.merge\npull:7\n1800000000000"
        XCTAssertEqual(
            DesktopCommandContract.canonical(owner: "alice", name: "repo", command: "pull.merge", target: "pull:7", ts: "1800000000000"),
            expected)
        XCTAssertEqual(
            DesktopCommand.canonical(owner: "alice", name: "repo", command: "pull.merge", target: "pull:7", ts: "1800000000000"),
            expected)
        XCTAssertEqual(
            RepoOwnerSigning.desktopCommandCanonical(
                owner: "alice", name: "repo", command: "pull.merge", target: "pull:7", ts: "1800000000000"),
            expected)
    }

    func testTheSignatureMatchesTheServerTestVector() throws {
        let identity = try repoTestIdentity()
        XCTAssertEqual(identity.publicKeyB64url, "6kpsY-KcUgq-9VB7Ey7F-ZVHdq6-vnuSQh7qaRRG0iw")
        let canonical = DesktopCommandContract.canonical(
            owner: "alice", name: "repo", command: "pull.merge", target: "pull:7", ts: "1800000000000")
        XCTAssertEqual(
            identity.sign(Array(canonical.utf8)),
            "a13FYSRIB4s_7l5xjj4WNISL-txH4EABZbYZa6Un2P6SOaWTUn_XL5GGu6-PAOJbkI9OQOrkPIJFX1KfPaf9CQ")
    }

    func testTargetsFollowTheServerAllowList() {
        XCTAssertEqual(DesktopCommandContract.commands.count, 11)
        XCTAssertTrue(DesktopCommandContract.isValid(command: "pull.merge", target: "pull:7"))
        XCTAssertTrue(DesktopCommandContract.isValid(command: "pull.delete", target: "pull:999999999"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "pull.delete", target: "pull:1234567890"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "pull.merge", target: "pull:0"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "pull.merge", target: "pull:7\n"))
        XCTAssertTrue(DesktopCommandContract.isValid(command: "action.rerun", target: "run:12"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "action.rerun", target: "run:-1"))
        XCTAssertTrue(DesktopCommandContract.isValid(command: "release.publish", target: "release:v1.2.0+b1"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "release.publish", target: "release:../x"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "release.publish", target: "pull:7"))
        XCTAssertFalse(DesktopCommandContract.isValid(command: "agent.stop", target: "pull:7"))
        XCTAssertEqual(DesktopCommandContract.pullTarget(3), "pull:3")
        XCTAssertEqual(DesktopCommandContract.runTarget(12), "run:12")
        XCTAssertEqual(DesktopCommandContract.releaseTarget("v2"), "release:v2")
        for command in PullShipCommands.all(collabFixturePull).map(\.command) {
            XCTAssertTrue(
                DesktopCommandContract.isValid(command: command, target: PullDetail.commandTarget(collabFixturePull)), command)
        }
        for action in RepoReleaseAction.all {
            XCTAssertTrue(DesktopCommandContract.isValid(command: action.command, target: "release:v1"), action.command)
        }
    }

    @MainActor func testARerunRequestIsSignedForThisRepository() async throws {
        let identity = try repoTestIdentity()
        let client = FakeRepoClient()
        let text = await DesktopCommandRequests.rerun(
            client: client, signer: signer(repoTestSession(nodeName: "Alice", pubkey: identity.publicKeyB64url), identity),
            repo: RepoRef(owner: "Alice", name: "repo"), runId: 12, workflowPath: ".forkmesh/ci.yml")
        XCTAssertEqual(text, RepoDesktopCommands.rerunSuccess)
        XCTAssertEqual(client.desktopCommands.count, 1)
        let sent = client.desktopCommands[0]
        XCTAssertEqual(sent.command, "action.rerun")
        XCTAssertEqual(sent.target, "run:12")
        XCTAssertEqual(sent.ownerAccount, "alice")
        XCTAssertEqual(sent.ts, "1800000000000")
        XCTAssertEqual(JSON.encode(sent.payload), #"{"runId":12,"workflowPath":".forkmesh/ci.yml"}"#)
        let canonical = "forkmesh-desktop-command-v1\nAlice\nrepo\naction.rerun\nrun:12\n1800000000000"
        XCTAssertTrue(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(canonical.utf8),
            signature: Array(try B64.decodeUnpadded(sent.sig))))
        let otherRepo = "forkmesh-desktop-command-v1\nAlice\nother\naction.rerun\nrun:12\n1800000000000"
        XCTAssertFalse(Ed25519.verify(
            publicKey: identity.publicKeyBytes, message: Array(otherRepo.utf8),
            signature: Array(try B64.decodeUnpadded(sent.sig))))
    }

    @MainActor func testUnsupportedRequestsAndStrangersNeverReachTheServer() async throws {
        let identity = try repoTestIdentity()
        let client = FakeRepoClient()
        let owner = signer(repoTestSession(nodeName: "alice", pubkey: identity.publicKeyB64url), identity)
        let unknown = await DesktopCommandRequests.request(
            client: client, signer: owner, owner: "alice", name: "repo", command: "agent.stop", target: "pull:1",
            payload: [:], successMessage: "ok")
        XCTAssertEqual(unknown, DesktopCommandContract.unsupportedMessage)
        let badTarget = await DesktopCommandRequests.request(
            client: client, signer: owner, owner: "alice", name: "repo", command: "pull.merge", target: "pull:x",
            payload: [:], successMessage: "ok")
        XCTAssertEqual(badTarget, DesktopCommandContract.unsupportedMessage)
        let stranger = await DesktopCommandRequests.request(
            client: client, signer: signer(repoTestSession(nodeName: "bob", pubkey: identity.publicKeyB64url), identity),
            owner: "alice", name: "repo", command: "pull.merge", target: "pull:1", payload: [:], successMessage: "ok")
        XCTAssertEqual(stranger, "Exception: Only the repo owner can request desktop control for this repo.")
        XCTAssertTrue(client.desktopCommands.isEmpty)
        let sent = await DesktopCommandRequests.request(
            client: client, signer: owner, owner: "alice", name: "repo", command: "release.publish",
            target: "release:v1", payload: ["tag": "v1"], successMessage: "queued")
        XCTAssertEqual(sent, "queued")
        client.desktopResult = { DesktopCommandResult(ok: false) }
        let refused = await DesktopCommandRequests.request(
            client: client, signer: owner, owner: "alice", name: "repo", command: "release.publish",
            target: "release:v1", payload: [:], successMessage: "queued")
        XCTAssertEqual(refused, RepoDesktopCommands.defaultFailure)
    }

    @MainActor func testThePullShipControlsRefuseAnUnsupportedCommand() async throws {
        let identity = try repoTestIdentity()
        let text = await requestDesktopCommand(
            api: FakeCollabAPI(), repo: Repository(owner: "alice", name: "repo", defaultBranch: "main"),
            command: "pull.rebase", target: "pull:1", payload: [:], successMessage: "ok",
            account: CollabAccount(nodeName: "alice", pubkey: identity.publicKeyB64url), identity: identity,
            clock: FixedClock(epochMillis: now))
        XCTAssertEqual(text, DesktopCommandContract.unsupportedMessage)
    }

    @MainActor func testAvailabilityIsProbedOnceAndRecheckedOnlyAfterAWhile() async {
        let clock = FixedClock(epochMillis: now)
        let availability = DesktopCommandAvailability(clock: clock)
        let probe = FakeRouteProbe()

        probe.answers = [nil]
        var shown = await availability.refresh(owner: "alice", name: "repo", probe: probe)
        XCTAssertFalse(shown)
        probe.answers = [false]
        shown = await availability.refresh(owner: "alice", name: "repo", probe: probe)
        XCTAssertFalse(shown)
        XCTAssertEqual(probe.calls, 2, "an unanswered probe is not remembered")

        probe.answers = [true]
        shown = await availability.refresh(owner: "alice", name: "repo", probe: probe)
        XCTAssertFalse(shown)
        XCTAssertEqual(probe.calls, 2, "a missing route is not probed again at once")

        clock.now = Date(epochMillis: now + DesktopCommandAvailability.recheckMs + 1)
        shown = await availability.refresh(owner: "alice", name: "repo", probe: probe)
        XCTAssertTrue(shown)
        XCTAssertTrue(availability.available)
        XCTAssertEqual(probe.calls, 3)

        shown = await availability.refresh(owner: "bob", name: "other", probe: probe)
        XCTAssertTrue(shown)
        XCTAssertEqual(probe.calls, 3, "once the server answers, the buttons stay without more probes")

        availability.reset()
        XCTAssertFalse(availability.available)
    }
}
