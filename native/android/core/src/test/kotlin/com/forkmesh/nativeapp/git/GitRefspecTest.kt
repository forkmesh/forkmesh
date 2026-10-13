package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class GitRefspecTest {
    fun testParsesAndMapsTheStandardWildcardFetchRefspec() {
        val refspec = GitFetchRefspec.parse("+refs/heads/*:refs/remotes/origin/*")

        expect(refspec.isWildcard)
        expect(refspec.force)
        assertEq("refs/remotes/origin/main", refspec.destinationFor("refs/heads/main"))
        assertEq("refs/remotes/origin/feature/x", refspec.destinationFor("refs/heads/feature/x"))
        assertEq(null, refspec.destinationFor("refs/tags/v1"))
        assertEq(refspec.value, GitFetchRefspec.standard("origin").value)
    }

    fun testParsesASingleBranchFetchRefspec() {
        val refspec = GitFetchRefspec.parse("refs/heads/main:refs/remotes/origin/main")

        expect(!refspec.isWildcard)
        assertEq("refs/remotes/origin/main", refspec.destinationFor("refs/heads/main"))
        assertEq(null, refspec.destinationFor("refs/heads/dev"))
    }

    fun testRejectsRefspecsThatCouldWriteOutsideRefsRemotes() {
        for (value in listOf(
            "refs/heads/*:refs/heads/*",
            "refs/heads/*:refs/tags/*",
            "+refs/*:refs/remotes/origin/*",
            "refs/heads/main:refs/remotes/origin/*",
            "refs/heads/*:refs/remotes/*",
            "refs/heads/a:refs/remotes/origin/a:extra",
        )) {
            val thrown = runCatching { GitFetchRefspec.parse(value) }.exceptionOrNull()
            expect(thrown is GitException, "expected GitException for $value but got $thrown")
        }
    }

    fun testPushRefspecMapsLocalToRemoteBranchAndDefaultsTheName() {
        val refspec = GitPushRefspec.parse("main")
        assertEq("refs/heads/main", refspec.local.value)
        assertEq("refs/heads/main", refspec.remote.value)

        val renamed = GitPushRefspec.parse("refs/heads/dev:refs/heads/staging")
        assertEq("dev", renamed.localBranch)
        assertEq("staging", renamed.remoteBranch)
    }

    fun testPushRefspecRejectsWildcardsForceAndDeletion() {
        for (value in listOf("refs/heads/*:refs/heads/*", "+main", ":main", "main:")) {
            val thrown = runCatching { GitPushRefspec.parse(value) }.exceptionOrNull()
            expect(thrown is GitException, "expected GitException for $value but got $thrown")
        }
    }

    fun testRemoteNamesAreValidated() {
        GitRemoteName.validate("origin")
        assertGitError(GitErrorCode.invalidRef) { GitRemoteName.validate("../evil") }
        assertGitError(GitErrorCode.invalidRef) { GitRemoteName.validate("") }
    }
}
