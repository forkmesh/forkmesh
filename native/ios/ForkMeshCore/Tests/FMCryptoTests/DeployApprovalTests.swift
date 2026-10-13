import FMBase
import XCTest
@testable import FMCrypto

final class DeployApprovalTests: XCTestCase {
    private let seed: [UInt8] = Array(0..<32)
    private let publicKey = "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg"

    func testThePullMessageIsNineLinesWithTheOwnerLowercased() {
        let text = DeployApproval.message(
            kind: "pull", owner: "ForkMesh", repo: "forkmesh", target: "42",
            base: String(repeating: "a", count: 40), head: String(repeating: "b", count: 40),
            requestId: "mobile_0123456789abcdef0123456789abcdef0123", ts: 1_791_500_000_000)
        XCTAssertEqual(text, [
            "forkmesh-deploy-approval-v1", "pull", "forkmesh", "forkmesh", "42",
            String(repeating: "a", count: 40), String(repeating: "b", count: 40),
            "mobile_0123456789abcdef0123456789abcdef0123", "1791500000000",
        ].joined(separator: "\n"))
        XCTAssertFalse(text.hasSuffix("\n"))
    }

    func testThePullGoldenVector() throws {
        let identity = try Identity.fromSeed(seed)
        let approval = try XCTUnwrap(DeployApproval.sign(
            identity, kind: "pull", owner: "ForkMesh", repo: "forkmesh", target: "42",
            base: String(repeating: "a", count: 40), head: String(repeating: "b", count: 40),
            requestId: "mobile_0123456789abcdef0123456789abcdef0123", ts: 1_791_500_000_000))
        XCTAssertEqual(approval.keys, ["publicKey", "ts", "signature"])
        XCTAssertEqual(approval.str("publicKey"), publicKey)
        XCTAssertEqual(approval["ts"], .int(1_791_500_000_000))
        XCTAssertEqual(
            approval.str("signature"),
            "74RN64qcUus-AwkE42AcAMoJKR9EcXQ9Dp32W5yp9tFubzSj8fQK5NeQWN5t-gull9AnAD82G1ef1cR77_DwBw")
    }

    func testTheBranchGoldenVector() throws {
        let identity = try Identity.fromSeed(seed)
        let approval = try XCTUnwrap(DeployApproval.sign(
            identity, kind: "branch", owner: "forkmesh", repo: "forkmesh", target: "sess-1",
            requestId: "mobile_req", ts: 1_791_500_000_000))
        XCTAssertEqual(approval.str("publicKey"), publicKey)
        XCTAssertEqual(
            approval.str("signature"),
            "pUSma40Gi6pAd31NUblKmuZqxZ3ZNlOMQKWV7WchNhMPrxKdudlpzRA_02zGnF3PgCrcMZ4d3qLgv2Rj5u7xAg")
    }

    func testTheSignatureVerifiesOverTheMessage() throws {
        let identity = try Identity.fromSeed(seed)
        let approval = try XCTUnwrap(DeployApproval.sign(
            identity, kind: "branch", owner: "Alice", repo: "site", target: "s1", requestId: "r1", ts: 5))
        let text = DeployApproval.message(
            kind: "branch", owner: "alice", repo: "site", target: "s1", base: "", head: "", requestId: "r1", ts: 5)
        let signature = Array(try B64.decodeUnpadded(approval.str("signature")))
        XCTAssertTrue(Ed25519.verify(publicKey: identity.publicKeyBytes, message: ascii(text), signature: signature))
    }
}
