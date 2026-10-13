package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.FederatedReply
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FederatedThreadTest {
    fun testFederatedReplyPreservesLifecycleAndFailsClosedAsNonNative() {
        val reply = FederatedReply.fromJson(
            linkedMapOf(
                "remoteId" to "https://lemmy.example/comment/7",
                "parentRemoteId" to "https://lemmy.example/comment/6",
                "author" to "alice@lemmy.example",
                "body" to "remote body",
                "url" to "https://lemmy.example/comment/7",
                "sourceInstance" to "lemmy.example",
                "sourceSoftware" to "lemmy",
                "lifecycle" to "edited",
                "depth" to 2L,
                "nativeEvent" to true,
            ),
        )
        assertEq("https://lemmy.example/comment/7", reply.remoteId)
        assertEq("https://lemmy.example/comment/6", reply.parentRemoteId)
        expect(reply.edited)
        assertEq(2, reply.depth)
        expect(!reply.nativeEvent)
        assertEq("alice@lemmy.example", FederatedThread.author(reply))
        assertEq("lemmy · lemmy.example", FederatedThread.provenance(reply))
        expect(!FederatedThread.unavailable(reply))
    }

    fun testTombstonesAndModerationNeverExposeAProjectedBody() {
        val tombstone = FederatedReply.fromJson(
            linkedMapOf("remoteId" to "https://lemmy.example/comment/8", "body" to "", "lifecycle" to "tombstoned"),
        )
        val moderated = FederatedReply.fromJson(
            linkedMapOf("remoteId" to "https://lemmy.example/comment/9", "body" to "", "lifecycle" to "moderated"),
        )
        assertEq("Deleted on the remote instance", tombstone.displayBody)
        assertEq("Hidden by remote moderation", moderated.displayBody)
        expect(FederatedThread.unavailable(tombstone))
        expect(FederatedThread.unavailable(moderated))
    }

    fun testTheAuthorFallsBackToARemoteParticipant() {
        val named = FederatedReply(remoteId = "r", author = "bob@x", body = "", authorName = "Bob")
        assertEq("Bob", FederatedThread.author(named))
        assertEq("Remote participant", FederatedThread.author(named.copy(author = "", authorName = "")))
        assertEq("", FederatedThread.provenance(named.copy(sourceSoftware = "", sourceInstance = "")))
    }

    fun testOnlyHttpsBacklinksWithAHostAndNoUserInfoOpen() {
        expect(FederatedThread.canOpenBacklink("https://lemmy.example/comment/7"))
        expect(FederatedThread.canOpenBacklink("HTTPS://lemmy.example:8443/c?x=1#y"))
        expect(FederatedThread.canOpenBacklink("https://[::1]/c"))
        expect(FederatedThread.canOpenBacklink("https://lemmy.example/a b"))
        expect(!FederatedThread.canOpenBacklink("http://lemmy.example/comment/7"))
        expect(!FederatedThread.canOpenBacklink("https://user:pw@lemmy.example/x"))
        expect(!FederatedThread.canOpenBacklink("https:///path"))
        expect(!FederatedThread.canOpenBacklink("https:lemmy.example"))
        expect(!FederatedThread.canOpenBacklink("javascript:alert(1)"))
        expect(!FederatedThread.canOpenBacklink(""))
    }
}
