package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class NotificationDeepLinkTest {
    fun testIssueLinkOpensTheIssueTabWithItsNumber() {
        val link = NotificationDeepLink.parse("/forkmesh/forkmesh/issues/692")!!
        assertEq("forkmesh", link.repo.owner)
        assertEq("forkmesh", link.repo.name)
        assertEq(RepoDetailTab.Issues, link.initialTab)
        assertEq(692, link.number)
    }

    fun testCommitLinkKeepsTheReference() {
        val link = NotificationDeepLink.parse("/a/b/commit/abc123?x=1#frag")!!
        assertEq(RepoDetailTab.Commits, link.initialTab)
        assertEq("abc123", link.reference)
    }

    fun testCodeIsTheDefaultAndUnknownSectionsFallBackToIt() {
        assertEq(RepoDetailTab.Code, NotificationDeepLink.parse("/a/b")!!.initialTab)
        assertEq(RepoDetailTab.Code, NotificationDeepLink.parse("/a/b/blob/main/README.md")!!.initialTab)
        assertEq(RepoDetailTab.Code, NotificationDeepLink.parse("/a/b/settings")!!.initialTab)
        assertEq(0, NotificationDeepLink.parse("/a/b/pulls/x")!!.number)
    }

    fun testRejectsExternalApiAndUnsafeLinks() {
        expect(NotificationDeepLink.parse("https://forkmesh.com/a/b") == null, "absolute URL")
        expect(NotificationDeepLink.parse("//forkmesh.com/a/b") == null, "authority")
        expect(NotificationDeepLink.parse("/api/accounts") == null, "api path")
        expect(NotificationDeepLink.parse("/dashboard/settings") == null, "dashboard path")
        expect(NotificationDeepLink.parse("/a") == null, "too short")
        expect(NotificationDeepLink.parse("/a b/c") == null, "unsafe owner")
        expect(NotificationDeepLink.parse("   ") == null, "blank")
    }

    fun testACodePushLinkNamesTheBranch() {
        val link = NotificationDeepLink.parse("/forkmesh/forkmesh/commits/feature/mobile-direct-send")!!
        assertEq(RepoDetailTab.Commits, link.initialTab)
        assertEq("feature/mobile-direct-send", link.branch)
        assertEq("", link.reference)
        expect(link.isBranchCommits)
    }

    fun testAHashUnderCommitsIsStillACommit() {
        val link = NotificationDeepLink.parse("/a/b/commits/0123abcd")!!
        assertEq("0123abcd", link.reference)
        assertEq("", link.branch)
        expect(!link.isBranchCommits)
    }

    fun testBranchFromTreeOrTheQuery() {
        assertEq("dev", NotificationDeepLink.parse("/a/b/tree/dev")!!.branch)
        assertEq("topic/x", NotificationDeepLink.parse("/a/b?branch=topic%2Fx")!!.branch)
        assertEq("rel", NotificationDeepLink.parse("/a/b/commits?ref=rel")!!.branch)
        assertEq("main", NotificationDeepLink.parse("/a/b/tree/dev?branch=main")!!.branch)
        assertEq("", NotificationDeepLink.parse("/a/b/commit/abc")!!.branch)
    }

    fun testAnAgentLinkCarriesTheSession() {
        val link = NotificationDeepLink.parse("/a/b/agents/42")!!
        assertEq(RepoDetailTab.Agents, link.initialTab)
        assertEq(42, link.session)
        assertEq(0, NotificationDeepLink.parse("/a/b/agents")!!.session)
    }

    fun testARunLinkCarriesTheRun() {
        val link = NotificationDeepLink.parse("/a/b/actions/runs/41")!!
        assertEq(RepoDetailTab.Actions, link.initialTab)
        assertEq(41, link.run)
        assertEq(0, NotificationDeepLink.parse("/a/b/actions")!!.run)
        assertEq(0, NotificationDeepLink.parse("/a/b/actions/runs/x")!!.run)
        assertEq(0, NotificationDeepLink.parse("/a/b/actions/runs/-3")!!.run)
    }
}
