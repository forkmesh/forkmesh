package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.URI
import java.net.URLDecoder

class ProjectInviteTest {
    private val invitation = ProjectInvitation("Garden & Co", "alice", "https://app.forkmesh.com")

    fun testTheLinkIsTheInvitersReferralLink() {
        assertEq("https://app.forkmesh.com/r/alice", ProjectInvite.invitationLink(invitation))
        assertEq("https://app.forkmesh.com/signup", ProjectInvite.invitationLink(invitation.copy(inviter = "")))
        assertEq("", ProjectInvite.invitationLink(invitation.copy(origin = "")))
        assertEq("http://127.0.0.1:8787/r/alice", ProjectInvite.invitationLink(invitation.copy(origin = "http://127.0.0.1:8787/")))
    }

    fun testTheMessagesNameTheProjectAndInstance() {
        expect(ProjectInvite.directMessageText(invitation).contains("“Garden & Co” on app.forkmesh.com"))
        assertEq("alice invited you to “Garden & Co” on ForkMesh", ProjectInvite.emailSubject(invitation))
        assertEq("You’re invited to “Garden & Co” on ForkMesh", ProjectInvite.emailSubject(invitation.copy(inviter = "")))
        expect(ProjectInvite.emailBody(invitation).contains("https://app.forkmesh.com/r/alice"))
        expect(!ProjectInvite.emailBody(invitation.copy(origin = "")).contains("Accept the invitation"))
    }

    fun testRecipientsAreSplitAndChecked() {
        val parsed = ProjectInvite.emailRecipients(" bob@example.com, carol@example.org;bob@example.com ")
        assertEq(listOf("bob@example.com", "carol@example.org"), parsed.addresses)
        assertEq("", parsed.invalid)
        assertEq("nope", ProjectInvite.emailRecipients("bob@example.com, nope").invalid)
        assertEq(ProjectInvite.Recipients(emptyList(), ""), ProjectInvite.emailRecipients(""))
        expect(ProjectInvite.looksLikeEmail("bob@example.com"))
        expect(!ProjectInvite.looksLikeEmail("@bob"))
        expect(!ProjectInvite.looksLikeEmail("bob"))
    }

    fun testTheMailtoLinkSurvivesDecoding() {
        val url = ProjectInvite.mailtoUrl(listOf("bob+x@example.com"), invitation)
        expect(url.startsWith("mailto:bob+x@example.com?subject="))
        expect(!url.contains(' '))
        val query = URI(url).rawSchemeSpecificPart.substringAfter('?')
        val fields = query.split('&').associate { it.substringBefore('=') to it.substringAfter('=') }
        fun decode(raw: String) = URLDecoder.decode(raw.replace("+", "%2B"), "UTF-8")
        assertEq(ProjectInvite.emailSubject(invitation), decode(fields.getValue("subject")))
        val body = decode(fields.getValue("body"))
        expect(body.contains("\r\n"))
        assertEq(ProjectInvite.emailBody(invitation), body.replace("\r", ""))
        expect(ProjectInvite.mailtoUrl(emptyList(), invitation).startsWith("mailto:?subject="))
    }
}
