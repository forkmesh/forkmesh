package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CovePatchTest {
    fun testGitBlobIds() {
        assertEq("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", CovePatch.gitBlobId(ByteArray(0)))
        assertEq("ce013625030ba8dba906f756967f9e9ca394464a", CovePatch.gitBlobId("hello\n".toByteArray()))
    }

    fun testNewFilePatch() {
        val after = "{\n}\n".toByteArray()
        val patch = CovePatch.replaceFile(".forkmesh/coves/x.cove", null, after)
        assertEq(
            "diff --git a/.forkmesh/coves/x.cove b/.forkmesh/coves/x.cove\n" +
                "new file mode 100644\n" +
                "index 0000000000000000000000000000000000000000..${CovePatch.gitBlobId(after)}\n" +
                "--- /dev/null\n+++ b/.forkmesh/coves/x.cove\n@@ -0,0 +1,2 @@\n+{\n+}\n",
            patch,
        )
    }

    fun testReplacementPatchHandlesMissingNewline() {
        val before = "a\nb".toByteArray()
        val after = "c\n".toByteArray()
        val patch = CovePatch.replaceFile("f", before, after)
        expect(patch.contains("index ${CovePatch.gitBlobId(before)}..${CovePatch.gitBlobId(after)}\n"))
        expect(patch.contains("@@ -1,2 +1 @@\n-a\n-b\n\\ No newline at end of file\n+c\n"))
        assertEq("", CovePatch.replaceFile("f", after, after))
    }

    fun testHeadLabel() {
        val now = 1_758_700_000_000L
        assertEq("alice:mobile-cove/0123456789ab-0924-074640", CovePatch.head("Alice", "0123456789abcdef", now))
        assertEq("mobile-cove/ab-0924-074640", CovePatch.head("", "ab", now))
    }
}
