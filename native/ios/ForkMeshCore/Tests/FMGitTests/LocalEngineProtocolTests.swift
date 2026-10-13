// The network protocol (Kotlin: GitNetworkProtocolTest.kt): the protocol pieces the engine's
// fetch and push drive, checked with the same fake-server bodies.

import Foundation
import XCTest
@testable import FMGit

final class LocalEngineProtocolTests: XCTestCase {
    private func dataLines(_ packets: [GitPktLine]) -> [String] {
        packets.filter { $0.kind == .data }.map { leText($0.payload) }
    }

    // MARK: protocol v2 requests

    func testLsRefsRequestCarriesCapabilitiesPrefixesPeelSymrefs() throws {
        let request = try GitProtocolV2.lsRefsRequest(refPrefixes: ["HEAD", "refs/heads/"])

        XCTAssertEqual(dataLines(request), [
            "command=ls-refs\n",
            "agent=\(GitProtocolV2.agent)\n",
            "peel\n",
            "symrefs\n",
            "ref-prefix HEAD\n",
            "ref-prefix refs/heads/\n",
        ])
        XCTAssertEqual(request[2].kind, .delimiter)
        XCTAssertEqual(request.last?.kind, .flush)
    }

    func testFetchRequestNeverAsksForThinPacksAndCanSendDone() throws {
        let lines = dataLines(try GitProtocolV2.fetchRequest(wants: [leFixtureId(1)], haves: [leFixtureId(2)], done: true))

        XCTAssertTrue(lines.contains("ofs-delta\n"))
        XCTAssertTrue(lines.contains("no-progress\n"))
        XCTAssertTrue(lines.contains("want \(leFixtureId(1).hex)\n"))
        XCTAssertTrue(lines.contains("have \(leFixtureId(2).hex)\n"))
        XCTAssertTrue(lines.contains("done\n"))
        XCTAssertFalse(lines.contains { $0.contains("thin-pack") })
    }

    func testFetchRequestRequiresAtLeastOneWant() {
        leAssertGitError { _ = try GitProtocolV2.fetchRequest(wants: [], done: true) }
    }

    // MARK: fetch response parsing

    func testParsesAnAcknowledgmentsOnlyResponse() throws {
        let response = try GitFetchResponse.parse(
            try GitPktLine.decodeAll(leAcknowledgmentsOnlyBody(["ACK \(leFixtureId(3).hex)"]))
        )

        XCTAssertFalse(response.hasPack)
        XCTAssertEqual(response.acknowledged, [leFixtureId(3)])
        XCTAssertFalse(response.nak)
    }

    func testParsesAcknowledgmentsFollowedByAPackfileSection() throws {
        let pack = try GitPackWriter.write([try GitPackWriteObject(.blob, leUtf8("x\n"))]).packBytes
        let response = try GitFetchResponse.parse(try GitPktLine.decodeAll(
            lePackfileBody(pack, acknowledgments: ["ACK \(leFixtureId(4).hex)", "ready"])
        ))

        XCTAssertTrue(response.ready)
        XCTAssertEqual(response.acknowledged, [leFixtureId(4)])
        XCTAssertEqual(response.packBytes, pack)
    }

    func testRejectsAResponseMixingAckAndNak() {
        leAssertGitError {
            _ = try GitFetchResponse.parse(
                try GitPktLine.decodeAll(leAcknowledgmentsOnlyBody(["ACK \(leFixtureId(4).hex)", "NAK"]))
            )
        }
    }

    func testTurnsASidebandErrorChannelIntoRemoteRejected() {
        leAssertGitError(.remoteRejected) {
            _ = try GitFetchResponse.parse([
                leDataLine("packfile\n"),
                .data([3] + leUtf8("server exploded")),
                .flush(),
            ])
        }
    }

    func testParsesShallowInfoBeforeThePackfile() throws {
        let response = try GitFetchResponse.parse([
            leDataLine("shallow-info\n"),
            leDataLine("shallow \(leFixtureId(1).hex)\n"),
            leDataLine("unshallow \(leFixtureId(2).hex)"),
            .delimiter(),
            leDataLine("packfile\n"),
            .data([1] + leUtf8("PACK")),
            .flush(),
        ])

        XCTAssertEqual(response.shallow, [leFixtureId(1)])
        XCTAssertEqual(response.unshallow, [leFixtureId(2)])
        XCTAssertEqual(response.packBytes, leUtf8("PACK"))
    }

    func testFetchRequestCarriesShallowLinesAndDeepen() throws {
        let lines = dataLines(try GitProtocolV2.fetchRequest(
            wants: [leFixtureId(1)], done: true, shallow: [leFixtureId(3)], deepen: 25))

        XCTAssertTrue(lines.contains("shallow \(leFixtureId(3).hex)\n"))
        XCTAssertTrue(lines.contains("deepen 25\n"))
        leAssertGitError { _ = try GitProtocolV2.fetchRequest(wants: [leFixtureId(1)], done: true, deepen: 0) }
    }

    func testShallowInfoWithoutAPackIsRejected() {
        leAssertGitError {
            _ = try GitFetchResponse.parse([
                leDataLine("shallow-info\n"),
                leDataLine("shallow \(leFixtureId(1).hex)\n"),
                .flush(),
            ])
        }
    }

    // MARK: receive-pack advertisement

    func testParsesRefsPeeledEntriesAndCapabilityTokens() throws {
        let advertisement = try GitSmartHttp.parseReceivePackAdvertisement(leReceivePackAdvertisementBody([
            ("refs/heads/main", leFixtureId(1)),
            ("refs/tags/v1", leFixtureId(2)),
        ]))

        XCTAssertEqual(advertisement.refs.count, 2)
        XCTAssertEqual(advertisement.refs["refs/heads/main"], leFixtureId(1))
        XCTAssertTrue(advertisement.supports("report-status-v2"))
        XCTAssertTrue(advertisement.supports("delete-refs"))
        XCTAssertEqual(advertisement.valueOf("object-format"), "sha1")
    }

    func testParsesAnEmptyRepositoryAdvertisement() throws {
        let advertisement = try GitSmartHttp.parseReceivePackAdvertisement(leReceivePackAdvertisementBody([]))

        XCTAssertTrue(advertisement.refs.isEmpty)
        XCTAssertTrue(advertisement.supports("report-status"))
    }

    func testSelectsReportStatusV2AndSidebandNeverDeleteRefs() throws {
        let advertisement = try GitSmartHttp.parseReceivePackAdvertisement(
            leReceivePackAdvertisementBody([("refs/heads/main", leFixtureId(1))])
        )
        let capabilities = try GitReceivePack.selectCapabilities(advertisement)

        XCTAssertTrue(capabilities.contains("report-status-v2"))
        XCTAssertTrue(capabilities.contains("side-band-64k"))
        XCTAssertTrue(capabilities.contains("atomic"))
        XCTAssertFalse(capabilities.contains("delete-refs"))
    }

    func testRequiresReportStatusSupport() throws {
        let advertisement = try GitSmartHttp.parseReceivePackAdvertisement(leReceivePackAdvertisementBody(
            [("refs/heads/main", leFixtureId(1))], capabilities: "side-band-64k agent=git/test"
        ))

        leAssertGitError(.protocolUnsupported) { _ = try GitReceivePack.selectCapabilities(advertisement) }
    }

    // MARK: receive-pack report parsing

    func testParsesASidebandReportStatusV2ResponseWithOptions() throws {
        let status = try GitReceivePack.parseResponse(leSidebandReportBody([
            "unpack ok",
            "ok refs/heads/main",
            "option old-oid \(leFixtureId(1).hex)",
            "option new-oid \(leFixtureId(2).hex)",
            "ng refs/heads/dev non-fast-forward",
        ]), sideband: true)

        XCTAssertTrue(status.unpackOk)
        XCTAssertEqual(status.refs.count, 2)
        XCTAssertTrue(status.refs[0].accepted)
        XCTAssertEqual(status.refs[1].code, .nonFastForward)
    }

    func testReportsUnpackFailureWithARedactedMessage() throws {
        let status = try GitReceivePack.parseResponse(
            leSidebandReportBody(["unpack index-pack abnormal exit"]), sideband: true
        )

        XCTAssertFalse(status.unpackOk)
        XCTAssertEqual(status.unpackMessage, "index-pack abnormal exit")
    }

    // MARK: push update construction

    func testARefDeletionIsNotExpressible() {
        leAssertGitError(.invalidRef) {
            _ = try GitPushRefUpdate(refName: "refs/heads/main", expectedOld: leFixtureId(1), next: GitReceivePack.zeroId)
        }
        leAssertGitError { _ = try GitPushRefspec.parse(":refs/heads/main") }
        leAssertGitError { _ = try GitPushRefspec.parse("+refs/heads/main:refs/heads/main") }
    }
}
