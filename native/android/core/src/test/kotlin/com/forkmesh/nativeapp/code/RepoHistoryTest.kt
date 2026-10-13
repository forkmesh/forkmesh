package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.testing.assertEq

class RepoHistoryTest {
    private val main = "a".repeat(40)
    private val feature = "b".repeat(40)
    private val branches = listOf(RepoBranch("main", main, isDefault = true), RepoBranch("feat", feature))

    fun testReadsBothAnswerShapes() {
        val list = RepoHistoryRead.fromJson(listOf(mapOf("hash" to feature.uppercase()), "junk"))
        assertEq(1, list.commits.size)
        assertEq(feature, list.tip)
        val obj = RepoHistoryRead.fromJson(mapOf("commit" to main, "commits" to listOf(mapOf("hash" to feature))))
        assertEq(main, obj.tip)
        assertEq(null, RepoHistoryRead.fromJson(null).tip)
    }

    fun testDetectsWhetherTheServerHonouredTheRef() {
        fun read(tip: String) = RepoHistoryRead(listOf(mapOf("hash" to tip)))
        assertEq(RepoHistoryRefSupport.Honored, RepoHistoryRefSupport.detect("feat", read(feature), branches, "main"))
        assertEq(RepoHistoryRefSupport.Ignored, RepoHistoryRefSupport.detect("feat", read(main), branches, "main"))
        assertEq(RepoHistoryRefSupport.Unknown, RepoHistoryRefSupport.detect("gone", read(main), branches, "main"))
        assertEq(RepoHistoryRefSupport.Unknown, RepoHistoryRefSupport.detect("feat", RepoHistoryRead(emptyList()), branches, "main"))
        assertEq(RepoHistoryRefSupport.Honored, RepoHistoryRefSupport.detect(feature.substring(0, 9), read(feature), emptyList(), ""))
        assertEq(RepoHistoryRefSupport.Unknown, RepoHistoryRefSupport.detect("x", read("c".repeat(40)), emptyList(), ""))
        assertEq(false, RepoHistoryRefSupport.sameCommit("abc", "abc"))
        assertEq(true, RepoHistoryRefSupport.isCommitId("DEADBEEF"))
        assertEq(false, RepoHistoryRefSupport.isCommitId("feature-x"))
    }

    fun testNotice() {
        assertEq(
            "This server lists only the default branch's history, so these are the commits of main, not feat.",
            RepoHistoryText.notice("feat", RepoHistoryRefSupport.Ignored, "main"),
        )
        assertEq(null, RepoHistoryText.notice("feat", RepoHistoryRefSupport.Honored, "main"))
        assertEq("History of feat", RepoHistoryText.title("feat"))
    }
}
