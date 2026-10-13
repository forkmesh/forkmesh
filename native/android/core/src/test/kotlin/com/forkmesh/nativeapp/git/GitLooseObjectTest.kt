package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitLooseObjectTest {
    fun testFramesHelloBlobWithGitCanonicalBytesAndObjectId() {
        val frame = GitObjectFrame.encode(GitObjectType.blob, utf8("hello\n"))

        assertEq("blob 6\u0000hello\n", utf8Text(frame))
        assertEq("ce013625030ba8dba906f756967f9e9ca394464a", GitObjectId.sha1Of(frame).hex)
    }

    fun testRejectsObjectBodyWhoseDeclaredLengthIsWrong() {
        assertFails<GitException> {
            GitObjectFrame.decode(utf8("blob 7\u0000hello\n"), GitResourceLimits())
        }
    }

    fun testWritesAndReadsAVerifiedLooseObject() = withTempDir("forkmesh-loose-object-") { root ->
        val store = LooseObjectStore(root, GitResourceLimits())

        val id = store.write(GitObjectType.blob, utf8("hello\n"))
        val frame = store.read(id)

        assertEq("ce013625030ba8dba906f756967f9e9ca394464a", id.hex)
        assertEq(GitObjectType.blob, frame.type)
        assertEq("hello\n", utf8Text(frame.body))
    }

    fun testRejectsALooseObjectStoredAtTheWrongObjectId() = withTempDir("forkmesh-object-mismatch-") { root ->
        val store = LooseObjectStore(root, GitResourceLimits())
        val id = store.write(GitObjectType.blob, utf8("hello\n"))
        val source = store.fileFor(id)
        val wrong = store.fileFor(GitObjectId.parseSha1("0000000000000000000000000000000000000001"))
        wrong.parentFile.mkdirs()
        source.copyTo(wrong)

        assertFails<GitException> {
            store.read(GitObjectId.parseSha1("0000000000000000000000000000000000000001"))
        }
    }

    fun testFailsClosedBeforeExposingAnUntrustedLooseSha1Object() = withTempDir("forkmesh-untrusted-loose-") { root ->
        val store = LooseObjectStore(root, GitResourceLimits())
        val id = store.write(GitObjectType.blob, utf8("hello\n"))

        assertGitError(GitErrorCode.hashScreeningUnavailable) {
            store.read(id, origin = GitSha1ObjectOrigin.untrustedTransport)
        }
    }

    fun testAdmitsAnUntrustedLooseObjectOnlyWithAClearDetector() = withTempDir("forkmesh-screened-loose-") { root ->
        val store = LooseObjectStore(root, GitResourceLimits(), detector = ClearDetector())
        val id = store.write(GitObjectType.blob, utf8("hello\n"))

        val frame = store.read(id, origin = GitSha1ObjectOrigin.untrustedTransport)

        assertEq("hello\n", utf8Text(frame.body))
    }
}
