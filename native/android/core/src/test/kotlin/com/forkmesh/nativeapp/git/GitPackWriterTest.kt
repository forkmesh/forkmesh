package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class GitPackWriterTest {
    fun testWritesANonDeltaPackAndMatchingIndexThatForkMeshCanReopen() {
        val written = GitPackWriter.write(
            listOf(
                GitPackWriteObject(GitObjectType.blob, utf8("hello\n")),
                GitPackWriteObject(GitObjectType.blob, byteArrayOf(0, 1, 0xff.toByte())),
            ),
        )

        val pack = GitPackReader.parse(written.packBytes, GitResourceLimits())
        val index = GitPackIndexV2.parse(written.indexBytes, GitResourceLimits())

        assertEq(2, pack.objects.size)
        for (packObject in pack.objects) {
            expect(index.lookup(packObject.objectId) != null, "index lists ${packObject.objectId}")
        }
    }

    fun testRefusesToReturnAPackWhenCancellationWasRequested() {
        val cancellation = GitCancellationToken()
        cancellation.cancel()

        assertGitError(GitErrorCode.cancelled) {
            GitPackWriter.write(listOf(GitPackWriteObject(GitObjectType.blob, byteArrayOf(1))), cancellation = cancellation)
        }
    }
}
