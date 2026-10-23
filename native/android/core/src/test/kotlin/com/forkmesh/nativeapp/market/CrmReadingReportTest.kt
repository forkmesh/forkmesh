package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CrmReadingReportTest {
    private fun email(vararg pairs: Pair<String, Any?>) = CrmActivity.fromJson(mapOf(*pairs))

    fun testTheEmailRowCarriesOpensClicksReadTimeAndScroll() {
        val sent = email("id" to "e1", "automated" to true, "opens" to 2, "clicks" to 1, "readSeconds" to 120, "scrollDepth" to 100)
        expect(sent.automated)
        assertEq("2 min+", sent.readLabel)
        assertEq("to the end", sent.scrollLabel)
        assertEq("2 opens · 1 click · read 2 min+ · scrolled to the end", sent.engagementLine)
    }

    fun testPartialReadsAndUntrackedEmailsStayQuiet() {
        assertEq("1 open · read 30s+ · scrolled 50%", email("opens" to 1, "readSeconds" to 30, "scrollDepth" to 50).engagementLine)
        val quiet = email("id" to "e2", "scrollDepth" to 400)
        assertEq("scrolled to the end", quiet.reachLabel)
        assertEq(100, quiet.scrollDepth)
        assertEq("", email("id" to "e3").engagementLine)
        assertEq(0, email("opens" to -4, "clicks" to 99_999_999_999L).opens)
        assertEq(Int.MAX_VALUE, email("clicks" to 99_999_999_999L).clicks)
    }

    fun testADeckReaderShowsSlideReachAndMailApp() {
        val deck = email(
            "opens" to 1, "readSeconds" to 240, "scrollDepth" to 64, "slideCount" to 14, "slideReached" to 9,
            "slideTitle" to "Market", "mailClient" to "iPhone · Apple Mail", "readingReported" to "yes",
        )
        assertEq("1 open · read 4 min+ · reached slide 9/14 · Market · iPhone · Apple Mail", deck.engagementLine)
        val whole = email("opens" to 1, "scrollDepth" to 100, "slideCount" to 14, "slideReached" to 20)
        assertEq(14, whole.slideReached)
        assertEq("read all 14 slides", whole.reachLabel)
    }

    fun testAMailAppThatCannotReportSaysSo() {
        val gmail = email("opens" to 2, "mailClient" to "Gmail", "readingReported" to "no")
        expect(gmail.silentClient)
        assertEq("2 opens · Gmail doesn't report read time", gmail.engagementLine)
        assertEq("", email("mailClient" to "Gmail", "readingReported" to "no").engagementLine)
    }
}
