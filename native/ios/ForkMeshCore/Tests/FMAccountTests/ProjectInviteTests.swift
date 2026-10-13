import Foundation
import XCTest
@testable import FMAccount

final class ProjectInviteTests: XCTestCase {
    private let invitation = ProjectInvitation(projectName: "Garden & Co", inviter: "alice", origin: "https://app.forkmesh.com")

    func testTheLinkIsTheInvitersReferralLink() {
        XCTAssertEqual(ProjectInvite.invitationLink(invitation), "https://app.forkmesh.com/r/alice")
        var anonymous = invitation
        anonymous.inviter = ""
        XCTAssertEqual(ProjectInvite.invitationLink(anonymous), "https://app.forkmesh.com/signup")
        var offline = invitation
        offline.origin = ""
        XCTAssertEqual(ProjectInvite.invitationLink(offline), "")
        var local = invitation
        local.origin = "http://127.0.0.1:8787/"
        XCTAssertEqual(ProjectInvite.invitationLink(local), "http://127.0.0.1:8787/r/alice")
        XCTAssertEqual(ProjectInvite.host(local), "127.0.0.1:8787")
    }

    func testTheMessagesNameTheProjectAndInstance() {
        XCTAssertTrue(ProjectInvite.directMessageText(invitation).contains("“Garden & Co” on app.forkmesh.com"))
        XCTAssertEqual(ProjectInvite.emailSubject(invitation), "alice invited you to “Garden & Co” on ForkMesh")
        XCTAssertTrue(ProjectInvite.emailBody(invitation).contains("https://app.forkmesh.com/r/alice"))
        var offline = invitation
        offline.origin = ""
        XCTAssertFalse(ProjectInvite.emailBody(offline).contains("Accept the invitation"))
    }

    func testRecipientsAreSplitAndChecked() {
        let parsed = ProjectInvite.emailRecipients(" bob@example.com, carol@example.org;bob@example.com ")
        XCTAssertEqual(parsed.addresses, ["bob@example.com", "carol@example.org"])
        XCTAssertEqual(parsed.invalid, "")
        XCTAssertEqual(ProjectInvite.emailRecipients("bob@example.com, nope").invalid, "nope")
        XCTAssertEqual(ProjectInvite.emailRecipients(""), ProjectInvite.Recipients(addresses: [], invalid: ""))
        XCTAssertTrue(ProjectInvite.looksLikeEmail("bob@example.com"))
        XCTAssertFalse(ProjectInvite.looksLikeEmail("@bob"))
        XCTAssertFalse(ProjectInvite.looksLikeEmail("bob"))
    }

    func testTheMailtoLinkSurvivesDecoding() throws {
        let raw = ProjectInvite.mailtoUrl(["bob+x@example.com"], invitation)
        XCTAssertTrue(raw.hasPrefix("mailto:bob+x@example.com?subject="))
        XCTAssertFalse(raw.contains(" "))
        let query = try XCTUnwrap(raw.split(separator: "?", maxSplits: 1).last)
        var fields: [String: String] = [:]
        for pair in query.split(separator: "&") {
            let parts = pair.split(separator: "=", maxSplits: 1)
            fields[String(parts[0])] = String(parts[1]).removingPercentEncoding
        }
        XCTAssertEqual(fields["subject"], ProjectInvite.emailSubject(invitation))
        let body = try XCTUnwrap(fields["body"])
        XCTAssertTrue(body.contains("\r\n"))
        XCTAssertEqual(body.replacingOccurrences(of: "\r\n", with: "\n"), ProjectInvite.emailBody(invitation))
        XCTAssertTrue(ProjectInvite.mailtoUrl([], invitation).hasPrefix("mailto:?subject="))
    }
}
