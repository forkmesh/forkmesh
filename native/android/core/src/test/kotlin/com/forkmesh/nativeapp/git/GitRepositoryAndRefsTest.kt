package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitRepositoryAndRefsTest {
    fun testRejectsTraversalAndLockSuffixRefs() {
        for (value in listOf("refs/heads/../main", "refs/heads/main.lock", "refs/heads/a..b", "refs/tags/v1")) {
            assertFails<GitException> { GitRefName.branch(value) }
        }
    }

    fun testPackedRefsReadsAPeeledTag() {
        val refs = PackedRefs.parse(
            utf8("# pack-refs with: peeled\n${fixtureId(1).hex} refs/tags/v1\n^${fixtureId(2).hex}\n"),
            GitResourceLimits(),
        )

        assertEq(fixtureId(2), refs.lookup("refs/tags/v1")!!.peeled)
    }

    fun testStaleRefUpdateLeavesOriginalObjectIdUnchanged() = withTempDir("forkmesh-ref-") { root ->
        val refs = RefStore(root)
        val ref = GitRefName.branch("refs/heads/main")
        refs.update(ref, expectedOld = null, next = fixtureId(3))

        val result = refs.update(ref, expectedOld = fixtureId(1), next = fixtureId(2))

        assertEq(GitRefUpdateResult.nonFastForward, result)
        assertEq(fixtureId(3), refs.readDirect(ref))
    }

    fun testInitializesAndReopensASha1RepositoryWithSymbolicMainHead() = withTempDir("forkmesh-repository-") { root ->
        val created = GitRepository.init(GitRepositoryInit(root))
        val id = created.writeBlob(utf8("hello\n"))
        val reopened = GitRepository.open(GitRepositoryOpen(root))

        assertEq(GitSymbolicRef("refs/heads/main"), reopened.readHead())
        assertEq("hello\n", utf8Text(reopened.readBlob(id)))
    }
}
