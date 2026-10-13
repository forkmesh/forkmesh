package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitIndexV2Test {
    fun testWritesAndReadsAChecksummedVersion2Index() {
        val bytes = GitIndexV2(
            listOf(
                GitIndexEntry(path = utf8("bin/run"), objectId = fixtureId(1), mode = GitIndexMode.executable, size = 12),
                GitIndexEntry(path = utf8("hello.txt"), objectId = fixtureId(2), mode = GitIndexMode.regular, size = 6),
            ),
        ).encode()

        val index = GitIndexV2.parse(bytes, GitResourceLimits())

        assertEq(listOf("bin/run", "hello.txt"), index.entries.map { utf8Text(it.path) })
        assertEq(GitIndexMode.executable, index.entries.first().mode)
        assertEq(fixtureId(2), index.entries.last().objectId)
    }

    fun testRejectsAnIndexWithAChangedChecksumByte() {
        val bytes = GitIndexV2(
            listOf(GitIndexEntry(path = utf8("hello.txt"), objectId = fixtureId(2), mode = GitIndexMode.regular)),
        ).encode()
        bytes[bytes.size - 1] = (bytes[bytes.size - 1].toInt() xor 0x01).toByte()

        assertFails<GitException> { GitIndexV2.parse(bytes, GitResourceLimits()) }
    }

    fun testRejectsATraversalPathAndNonZeroStageEntry() {
        assertFails<GitException> {
            GitIndexEntry(path = utf8("../outside"), objectId = fixtureId(3), mode = GitIndexMode.regular)
        }
        assertFails<GitException> {
            GitIndexEntry(path = utf8("conflict.txt"), objectId = fixtureId(3), mode = GitIndexMode.regular, stage = 1)
        }
    }
}
