package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.localrepos.gitBlobId
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset
import java.time.ZonedDateTime

class QuickEditTest {
    private val at = ZonedDateTime.of(2026, 9, 19, 14, 30, 0, 0, ZoneOffset.UTC).toInstant().toEpochMilli()

    fun testLetsOrdinaryTextThrough() {
        assertEq(null, QuickEdit.blockerFor("hi\n".toByteArray()))
    }

    fun testRefusesBinaryNonUtf8AndOversizedContent() {
        assertEq(QuickEditBlocker.Binary, QuickEdit.blockerFor(byteArrayOf(0x00, 0x01)))
        assertEq(QuickEditBlocker.NotUtf8, QuickEdit.blockerFor(byteArrayOf(0x68, 0xff.toByte())))
        assertEq(QuickEditBlocker.TooLarge, QuickEdit.blockerFor(ByteArray(2048), maxBytes = 1024))
    }

    fun testSizeIsJudgedBeforeContent() {
        assertEq(QuickEditBlocker.TooLarge, QuickEdit.blockerFor(ByteArray(3), maxBytes = 2))
    }

    fun testRefusesWhenTheServedBlobIdDisagrees() {
        val bytes = "hello\n".toByteArray()
        assertEq(QuickEditBlocker.BytesDisagree, QuickEdit.blockerFor(bytes, servedBlobSha = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"))
        assertEq(null, QuickEdit.blockerFor(bytes, servedBlobSha = gitBlobId(bytes).hex))
        assertEq(null, QuickEdit.blockerFor(bytes, servedBlobSha = ""))
    }

    fun testHeadFollowsTheCrossNodeConvention() {
        assertEq("kaif:mobile-edit/readme-md-0919-1430", QuickEdit.head("kaif", "docs/README.md", at))
        assertEq("mobile-edit/readme-md-0919-1430", QuickEdit.head("", "README.md", at))
        assertEq("kaif:mobile-edit/env-0919-1430", QuickEdit.head("kaif", ".env", at))
        assertEq("kaif:mobile-edit/file-0919-1430", QuickEdit.head("kaif", "---", at))
        expect(QuickEdit.head("k", "a.txt", at) != QuickEdit.head("k", "a.txt", at + 60_000))
    }

    fun testCopyAndFailures() {
        assertEq("Update README.md", QuickEdit.suggestTitle("docs/README.md"))
        assertEq("main", QuickEdit.base("", "main"))
        assertEq("dev", QuickEdit.base("dev", "main"))
        expect(QuickEdit.submitFailure("HTTP 429 inbox_full").startsWith("This repository is not accepting"))
        expect(QuickEdit.submitFailure("author_quota").startsWith("You have too many"))
        assertEq("This change is too large to submit from mobile.", QuickEdit.submitFailure("pull_too_large"))
        assertEq("Could not submit the change: boom", QuickEdit.submitFailure("boom"))
        assertEq(QuickEditBlocker.TooLarge, QuickEdit.loadFailureBlocker(Exception("a.bin is too large to edit on this device.")))
        assertEq(null, QuickEdit.loadFailureBlocker(Exception("HTTP 500")))
        assertEq("Files over 512 KB are edited from a clone, not in the quick editor.", QuickEdit.blockerMessage(QuickEditBlocker.TooLarge))
    }

    fun testPatchCarriesRealBlobIdsAndNoMode() {
        val result = QuickEdit.patch("a.txt", "hello\n".toByteArray(), "goodbye\n")
        expect(result.patch.contains("index ce013625030ba8dba906f756967f9e9ca394464a.."))
        expect(!result.patch.contains("100644"))
        assertEq(listOf("a.txt"), result.changedPaths)
        expect(QuickEdit.patch("a.txt", "same\n".toByteArray(), "same\n").isEmpty)
    }
}
