package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitRemoteUrlTest {
    fun testAcceptsOnlyCredentialFreeHttpsGitRemoteUrls() {
        val remote = GitRemoteUrl.parse("https://example.test/group/repo.git")

        assertEq("https", remote.uri.scheme)
        assertEq("https://example.test", remote.redactedOrigin)
    }

    fun testRejectsNonHttpsSchemesUrlCredentialsAndFragments() {
        for (value in listOf(
            "http://example.test/repo.git",
            "file:///tmp/repo.git",
            "ssh://example.test/repo.git",
            "https://user:secret@example.test/repo.git",
            "https://example.test/repo.git#fragment",
        )) {
            assertFails<GitException> { GitRemoteUrl.parse(value) }
        }
    }
}
