package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitObjectModelsTest {
    fun testTreeWriterUsesGitRawByteDirectoryOrdering() {
        val tree = GitTree(
            listOf(
                GitTreeEntry.regular(utf8("z"), fixtureId(1)),
                GitTreeEntry.directory(utf8("a"), fixtureId(2)),
            ),
        )

        assertEq(listOf("a", "z"), tree.entries.map { utf8Text(it.name) })
        assertEq(2, GitTree.parse(tree.encode()).entries.size)
    }

    fun testTreeParserRejectsUnsortedOrGitlinkEntries() {
        val unsorted = utf8("100644 z\u0000") + fixtureId(1).bytes + utf8("100644 a\u0000") + fixtureId(2).bytes
        val gitlink = utf8("160000 module\u0000") + fixtureId(1).bytes

        assertFails<GitException> { GitTree.parse(unsorted) }
        assertFails<GitException> { GitTree.parse(gitlink) }
    }

    fun testCommitParserPreservesParentOrderAndMessageBytes() {
        val commit = GitCommit.parse(
            utf8(
                "tree ${fixtureId(1).hex}\n" +
                    "parent ${fixtureId(2).hex}\n" +
                    "parent ${fixtureId(3).hex}\n" +
                    "author Ada <ada@example.test> 0 +0000\n" +
                    "committer Ada <ada@example.test> 0 +0000\n" +
                    "\n" +
                    "message\n",
            ),
        )

        assertEq(fixtureId(1), commit.tree)
        assertEq(listOf(fixtureId(2), fixtureId(3)), commit.parents)
        assertEq("message\n", utf8Text(commit.message))
    }

    fun testAnnotatedTagParserRejectsAMalformedTargetObjectId() {
        assertFails<GitException> {
            GitAnnotatedTag.parse(utf8("object not-an-oid\ntype commit\ntag v1\n\nmessage\n"))
        }
    }
}
