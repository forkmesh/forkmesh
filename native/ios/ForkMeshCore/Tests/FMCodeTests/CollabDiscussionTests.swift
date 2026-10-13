// The federated thread model vectors (the request shape is ApiService's, tested in FMApiTests),
// native/android
// FederatedThreadTest.kt and the discussion assertions of
// RepoCollaborationTest.kt.

import FMApi
import FMBase
import XCTest
@testable import FMCode

final class CollabDiscussionTests: XCTestCase {
    func testFederatedReplyPreservesLifecycleAndFailsClosedAsNonNative() {
        let reply = FederatedReply(json: [
            "remoteId": "https://lemmy.example/comment/7",
            "parentRemoteId": "https://lemmy.example/comment/6",
            "author": "alice@lemmy.example",
            "body": "remote body",
            "url": "https://lemmy.example/comment/7",
            "sourceInstance": "lemmy.example",
            "sourceSoftware": "lemmy",
            "lifecycle": "edited",
            "depth": 2,
            // A compromised or old server response cannot promote this projection.
            "nativeEvent": true,
        ])
        XCTAssertEqual(reply.remoteId, "https://lemmy.example/comment/7")
        XCTAssertEqual(reply.parentRemoteId, "https://lemmy.example/comment/6")
        XCTAssertTrue(reply.edited)
        XCTAssertEqual(reply.depth, 2)
        XCTAssertFalse(reply.nativeEvent)
        XCTAssertEqual(FederatedThread.author(reply), "alice@lemmy.example")
        XCTAssertEqual(FederatedThread.provenance(reply), "lemmy · lemmy.example")
        XCTAssertFalse(FederatedThread.unavailable(reply))
        XCTAssertEqual(FederatedThread.indent(reply), 28)
    }

    func testTombstonesAndModerationNeverExposeAProjectedBody() {
        let tombstone = FederatedReply(json: [
            "remoteId": "https://lemmy.example/comment/8", "body": "", "lifecycle": "tombstoned",
        ])
        let moderated = FederatedReply(json: [
            "remoteId": "https://lemmy.example/comment/9", "body": "", "lifecycle": "moderated",
        ])
        XCTAssertEqual(tombstone.displayBody, "Deleted on the remote instance")
        XCTAssertEqual(moderated.displayBody, "Hidden by remote moderation")
        XCTAssertTrue(FederatedThread.unavailable(tombstone))
        XCTAssertTrue(FederatedThread.unavailable(moderated))
    }

    func testTheAuthorFallsBackToARemoteParticipant() {
        var named = FederatedReply(remoteId: "r", author: "bob@x", body: "", authorName: "Bob")
        XCTAssertEqual(FederatedThread.author(named), "Bob")
        named.sourceSoftware = ""
        named.sourceInstance = ""
        XCTAssertEqual(FederatedThread.provenance(named), "")
        named.author = ""
        named.authorName = ""
        XCTAssertEqual(FederatedThread.author(named), "Remote participant")
    }

    func testOnlyHttpsBacklinksWithAHostAndNoUserInfoOpen() {
        XCTAssertTrue(FederatedThread.canOpenBacklink("https://lemmy.example/comment/7"))
        XCTAssertTrue(FederatedThread.canOpenBacklink("HTTPS://lemmy.example:8443/c?x=1#y"))
        XCTAssertTrue(FederatedThread.canOpenBacklink("https://[::1]/c"))
        XCTAssertTrue(FederatedThread.canOpenBacklink("https://lemmy.example/a b"))
        XCTAssertFalse(FederatedThread.canOpenBacklink("http://lemmy.example/comment/7"))
        XCTAssertFalse(FederatedThread.canOpenBacklink("https://user:pw@lemmy.example/x"))
        XCTAssertFalse(FederatedThread.canOpenBacklink("https:///path"))
        XCTAssertFalse(FederatedThread.canOpenBacklink("https:lemmy.example"))
        XCTAssertFalse(FederatedThread.canOpenBacklink("javascript:alert(1)"))
        XCTAssertFalse(FederatedThread.canOpenBacklink(""))
        XCTAssertEqual(
            FederatedThread.backlinkURL("https://lemmy.example/comment/7")?.absoluteString, "https://lemmy.example/comment/7")
        XCTAssertNotNil(FederatedThread.backlinkURL("https://lemmy.example/a b"))
        XCTAssertNil(FederatedThread.backlinkURL("http://lemmy.example/comment/7"))
    }

    func testNewDiscussionCategoriesAndDiscussionDetail() {
        XCTAssertEqual(discussionCategoryLabels.map(\.key), ["general", "ideas", "help", "show-and-tell"])
        XCTAssertEqual(discussionCategoryLabels.map(\.label), ["General", "Ideas", "Help", "Show & tell"])
        XCTAssertEqual(discussionCategoryLabels.first { $0.label == "Ideas" }?.key, "ideas")

        var discussion = RepoDiscussion(
            number: 3, title: "Mobile maintainer workflow", body: "How should maintainers review from phones?",
            author: "alice", category: "ideas",
            events: [
                DiscussionEvent(
                    type: "comment", body: "Keep desktop node as the canonical apply surface.", authorName: "Mona"),
            ])
        XCTAssertEqual(
            DiscussionDetail.chips(discussion, repoFullName: "owner/repo").map(\.label),
            ["discussion", "ideas", "alice", "owner/repo"])
        XCTAssertEqual(DiscussionCard.subtitle(discussion), "#3 · ideas · alice")
        XCTAssertEqual(DiscussionDetail.title(3), "Discussion #3")
        discussion.body = ""
        XCTAssertEqual(DiscussionDetail.body(discussion), "No description provided.")
        discussion.author = ""
        XCTAssertEqual(DiscussionCard.subtitle(discussion), "#3 · ideas")
        XCTAssertTrue(NewDiscussionComposer.ready(title: "t", body: "b"))
        XCTAssertFalse(NewDiscussionComposer.ready(title: "t", body: " \n"))
        XCTAssertEqual(NewDiscussionComposer.defaultCategory, "general")
    }
}
